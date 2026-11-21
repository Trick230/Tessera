// SPDX-License-Identifier: GPL-3.0-or-later
//! Finds Unreal Engine console variables in a game executable, offline, so
//! the layer can set them without calling into the engine.
//!
//! Both kinds of static console variable register from a small thunk (MSVC
//! x64, UE4 and UE5):
//!
//! ```text
//! mov  rax,[rcx]                       ; IConsoleManager vtable
//! ...  default value / reference       ; r8d, r8b, xmm2, or lea r8,[rip+GVar]
//! lea  rdx,[rip+L"r.Name"]
//! call [rax+Register...]
//! mov  [rip+Target],rax
//! ; TAutoConsoleVariable<T> only:
//! call [rdx+AsVariableInt/Float/Bool] ; on the returned variable
//! mov  [rip+Ref],rax                   ; Ref == Target + 8
//! ```
//!
//! - `TAutoConsoleVariable<T>`: `Ref` points at `T ShadowedValue[2]` (game
//!   thread, render thread), which every `GetValueOn*Thread` reads.
//! - `FAutoConsoleVariableRef`: the engine code reads `GVar` itself.
//!
//! The value type comes from the default-value register (TAutoConsoleVariable)
//! or from how the code accesses `GVar` (FAutoConsoleVariableRef). Every
//! `Register...` overload has its own vtable slot, so all registrations through
//! one slot must agree on the type; a slot without a clear majority is dropped.

use memchr::memmem;
use object::read::pe::PeFile64;
use object::LittleEndian as LE;
use std::collections::HashMap;
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CvarKind {
    Int,
    Float,
    Bool,
}

impl CvarKind {
    pub fn tag(self) -> char {
        match self {
            CvarKind::Int => 'i',
            CvarKind::Float => 'f',
            CvarKind::Bool => 'b',
        }
    }
    fn index(self) -> usize {
        self as usize
    }
    const ALL: [CvarKind; 3] = [CvarKind::Int, CvarKind::Float, CvarKind::Bool];
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CvarForm {
    /// `rva` holds a pointer to `T[2]` (TAutoConsoleVariable::Ref).
    Indirect,
    /// The value itself is at `rva` (the FAutoConsoleVariableRef target).
    Direct,
}

impl CvarForm {
    pub fn tag(self) -> char {
        match self {
            CvarForm::Indirect => 'p',
            CvarForm::Direct => 'd',
        }
    }
}

/// Where the layer finds a variable's value, as an RVA in the executable's
/// writable data.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CvarSlot {
    pub kind: CvarKind,
    pub form: CvarForm,
    pub rva: u32,
}

/// Identity of the scanned image; the layer refuses slots of another build.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ImageId {
    pub timestamp: u32,
    pub size_of_image: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CvarError {
    NotPe64,
    /// The name is not in the executable (another engine version or game).
    NoName,
    /// The name is there but no static registration matches the pattern
    /// (string variable, console command, other compiler output).
    NoRegistration,
    /// The registration was found but its type could not be confirmed.
    UnknownType,
    /// Several registrations disagree.
    Ambiguous,
}

struct Section {
    va: u32,
    vsize: u32,
    raw: u32,
    raw_size: u32,
    exec: bool,
    write: bool,
}

struct Image {
    sections: Vec<Section>,
}

impl Image {
    fn section_of_rva(&self, rva: u32) -> Option<&Section> {
        self.sections.iter().find(|s| rva >= s.va && (rva as u64) < s.va as u64 + s.vsize.max(s.raw_size) as u64)
    }
    fn is_writable(&self, rva: u32) -> bool {
        self.section_of_rva(rva).is_some_and(|s| s.write && !s.exec)
    }
    fn is_rdata(&self, rva: u32) -> bool {
        self.section_of_rva(rva).is_some_and(|s| !s.write && !s.exec)
    }
}

fn parse(data: &[u8]) -> Option<(Image, ImageId)> {
    let pe = PeFile64::parse(data).ok()?;
    let nt = pe.nt_headers();
    let id = ImageId {
        timestamp: nt.file_header.time_date_stamp.get(LE),
        size_of_image: nt.optional_header.size_of_image.get(LE),
    };
    let sections = pe
        .section_table()
        .iter()
        .map(|s| {
            let c = s.characteristics.get(LE).0;
            Section {
                va: s.virtual_address.get(LE),
                vsize: s.virtual_size.get(LE),
                raw: s.pointer_to_raw_data.get(LE),
                raw_size: s.size_of_raw_data.get(LE),
                exec: c & object::pe::IMAGE_SCN_MEM_EXECUTE.0 != 0,
                write: c & object::pe::IMAGE_SCN_MEM_WRITE.0 != 0,
            }
        })
        .collect();
    Some((Image { sections }, id))
}

fn utf16z(name: &str) -> Vec<u8> {
    name.encode_utf16().chain([0]).flat_map(|c| c.to_le_bytes()).collect()
}

fn rel32(code: &[u8], at: usize) -> i32 {
    i32::from_le_bytes([code[at], code[at + 1], code[at + 2], code[at + 3]])
}

/// RVA of a rip-relative operand whose disp32 is at `at` and whose
/// instruction ends `tail` bytes after the disp32.
fn rip_target(code_rva: u32, code: &[u8], at: usize, tail: usize) -> u32 {
    (code_rva as i64 + (at + 4 + tail) as i64 + rel32(code, at) as i64) as u32
}

/// `call [reg+disp]` (FF /2, disp8 or disp32, no SIB): (length, disp).
fn vcall(code: &[u8], at: usize) -> Option<(usize, u32)> {
    if at + 6 > code.len() || code[at] != 0xFF {
        return None;
    }
    let m = code[at + 1];
    if m & 0x38 != 0x10 || m & 7 == 4 {
        return None;
    }
    match m >> 6 {
        1 => Some((3, code[at + 2] as u32)),
        2 => Some((6, rel32(code, at + 2) as u32)),
        _ => None,
    }
}

/// Next `mov [rip+X],rax` (48 89 05) in `code[from..to]`: (offset after it, X).
fn next_store_rax(code: &[u8], code_rva: u32, from: usize, to: usize) -> Option<(usize, u32)> {
    let to = to.min(code.len().saturating_sub(7));
    (from..to)
        .find(|&i| code[i] == 0x48 && code[i + 1] == 0x89 && code[i + 2] == 0x05)
        .map(|i| (i + 7, rip_target(code_rva, code, i + 3, 0)))
}

/// One decoded registration.
#[derive(Clone, Copy)]
struct Reg {
    disp: u32,
    form: CvarForm,
    rva: u32,
    /// Type from the default-value register (TAutoConsoleVariable only).
    hint: Option<CvarKind>,
    /// Indirect: the default is passed as zero (`xor r8d,r8d`), valid for any type.
    zero: bool,
}

/// Decodes the registration that follows `lea rdx,[name]` at `lea`.
fn decode(img: &Image, code: &[u8], code_rva: u32, lea: usize) -> Option<Reg> {
    // The thunk starts with `sub rsp,imm8` (48 83 EC) a few instructions back.
    let lo = lea.saturating_sub(64);
    let start = (lo..lea).rev().find(|&i| code[i] == 0x48 && code[i + 1] == 0x83 && code[i + 2] == 0xEC)?;
    let pre = &code[start..lea];
    let after = lea + 7;
    let call = (after..after + 12).find(|&i| vcall(code, i).is_some())?;
    let (clen, disp) = vcall(code, call)?;
    let (t_end, target) = next_store_rax(code, code_rva, call + clen, call + clen + 24)?;
    if !img.is_writable(target) {
        return None;
    }
    // TAutoConsoleVariable: AsVariable*() on the result, stored at Target + 8.
    if let Some(c2) = (t_end..t_end + 16).find(|&i| vcall(code, i).is_some()) {
        let (c2len, _) = vcall(code, c2)?;
        if let Some((_, rf)) = next_store_rax(code, code_rva, c2 + c2len, c2 + c2len + 16) {
            if rf == target.wrapping_add(8) {
                let has = |pat: &[u8]| memmem::find(pre, pat).is_some();
                // `xor r8d,r8d` is no hint: MSVC also passes `false` to the bool overload that way.
                let int = has(&[0x41, 0xB8]);
                let zero = has(&[0x45, 0x33, 0xC0]) || has(&[0x45, 0x31, 0xC0]);
                let float = has(&[0xF3, 0x0F, 0x10, 0x15]) || has(&[0x0F, 0x57, 0xD2]) || has(&[0x0F, 0x28, 0x15]);
                let boolean = has(&[0x41, 0xB0]) || has(&[0x45, 0x32, 0xC0]);
                let hint = match (int, float, boolean) {
                    (true, false, false) => Some(CvarKind::Int),
                    (false, true, false) => Some(CvarKind::Float),
                    (false, false, true) => Some(CvarKind::Bool),
                    _ => None,
                };
                return Some(Reg { disp, form: CvarForm::Indirect, rva: rf, hint, zero });
            }
        }
    }
    // FAutoConsoleVariableRef: lea r8,[rip+GVar] (4C 8D 05) with GVar in writable data.
    let mut gvar = None;
    for i in memmem::find_iter(pre, &[0x4C, 0x8D, 0x05]) {
        if i + 7 <= pre.len() {
            let v = rip_target(code_rva, code, start + i + 3, 0);
            if img.is_writable(v) {
                gvar = Some(v);
            }
        }
    }
    gvar.map(|v| Reg { disp, form: CvarForm::Direct, rva: v, hint: None, zero: false })
}

/// Votes per variable from rip-relative accesses: [int, float, bool].
fn access_votes(img: &Image, data: &[u8], vars: &HashMap<u32, [u32; 3]>) -> HashMap<u32, [u32; 3]> {
    let mut votes = vars.clone();
    for s in img.sections.iter().filter(|s| s.exec) {
        let start = (s.raw as usize).min(data.len());
        let end = start.saturating_add(s.raw_size as usize).min(data.len());
        let code = &data[start..end];
        for i in 4..code.len().saturating_sub(9) {
            let m = code[i];
            if m & 0xC7 != 0x05 {
                continue;
            }
            let (p1, p2, p3) = (code[i - 1], code[i - 2], code[i - 3]);
            let rex_w = p2 & 0xF8 == 0x48;
            let sse = |op: u8| p2 == 0x0F && p1 == op && (p3 == 0xF3 || (p3 & 0xF0 == 0x40 && code[i - 4] == 0xF3));
            // (kind, immediate bytes after the disp32)
            let hit = if (sse(0x10) || sse(0x11) || sse(0x58) || sse(0x59) || sse(0x5C) || sse(0x5E) || sse(0x5D) || sse(0x5F))
                || (p2 == 0x0F && (p1 == 0x2F || p1 == 0x2E) && p3 != 0x66)
            {
                Some((CvarKind::Float, 0))
            } else if p2 == 0x0F && p1 == 0xB6 {
                Some((CvarKind::Bool, 0))
            } else if p1 == 0x80 && m == 0x3D {
                Some((CvarKind::Bool, 1))
            } else if p1 == 0xC6 && m == 0x05 {
                Some((CvarKind::Bool, 1))
            } else if matches!(p1, 0x8A | 0x88 | 0x38 | 0x3A) && p2 != 0x0F {
                Some((CvarKind::Bool, 0))
            } else if p1 == 0x83 && m == 0x3D && !rex_w {
                Some((CvarKind::Int, 1))
            } else if (p1 == 0x81 && m == 0x3D || p1 == 0xC7 && m == 0x05) && !rex_w {
                Some((CvarKind::Int, 4))
            } else if matches!(p1, 0x8B | 0x89 | 0x3B | 0x39 | 0x03 | 0x2B) && !rex_w && p2 != 0x0F {
                Some((CvarKind::Int, 0))
            } else {
                None
            };
            if let Some((k, imm)) = hit {
                let t = rip_target(s.va, code, i + 1, imm);
                if let Some(v) = votes.get_mut(&t) {
                    v[k.index()] += 1;
                }
            }
        }
    }
    votes
}

/// Clear winner: at least `min` votes and 4x the runner-up.
fn winner(v: &[u32; 3], min: u32) -> Option<CvarKind> {
    let mut idx: Vec<usize> = (0..3).collect();
    idx.sort_by_key(|&i| std::cmp::Reverse(v[i]));
    (v[idx[0]] >= min && v[idx[0]] >= 4 * v[idx[1]]).then(|| CvarKind::ALL[idx[0]])
}

pub type ScanResult = Result<(ImageId, Vec<Result<CvarSlot, CvarError>>), CvarError>;

pub fn find_cvars_in(data: &[u8], names: &[&str]) -> ScanResult {
    let (img, id) = parse(data).ok_or(CvarError::NotPe64)?;
    // Requested name strings (exact, NUL-terminated, NUL before).
    let mut name_at: HashMap<u32, usize> = HashMap::new();
    let mut named = vec![false; names.len()];
    for (ni, name) in names.iter().enumerate() {
        let pat = utf16z(name);
        for s in img.sections.iter().filter(|s| !s.exec && !s.write) {
            let start = (s.raw as usize).min(data.len());
            let end = start.saturating_add(s.raw_size as usize).min(data.len());
            let raw = &data[start..end];
            for off in memmem::find_iter(raw, &pat) {
                if off % 2 != 0 || (off >= 2 && (raw[off - 2] != 0 || raw[off - 1] != 0)) {
                    continue;
                }
                named[ni] = true;
                name_at.insert(s.va.wrapping_add(off as u32), ni);
            }
        }
    }
    // Every registration in the image: the type check needs all of them.
    let mut regs: Vec<(Option<usize>, Reg)> = Vec::new();
    for s in img.sections.iter().filter(|s| s.exec) {
        let start = (s.raw as usize).min(data.len());
        let end = start.saturating_add(s.raw_size as usize).min(data.len());
        let code = &data[start..end];
        for lea in memmem::find_iter(code, &[0x48, 0x8D, 0x15]) {
            if lea + 7 > code.len() {
                break;
            }
            let t = rip_target(s.va, code, lea + 3, 0);
            if !img.is_rdata(t) {
                continue;
            }
            if let Some(r) = decode(&img, code, s.va, lea) {
                regs.push((name_at.get(&t).copied(), r));
            }
        }
    }
    let gvars: HashMap<u32, [u32; 3]> =
        regs.iter().filter(|(_, r)| r.form == CvarForm::Direct).map(|(_, r)| (r.rva, [0; 3])).collect();
    let votes = access_votes(&img, data, &gvars);
    let kind_of = |r: &Reg| match r.form {
        CvarForm::Indirect => r.hint,
        CvarForm::Direct => winner(&votes[&r.rva], 1),
    };
    // Per (form, slot) type consensus.
    let mut groups: HashMap<(CvarForm, u32), [u32; 3]> = HashMap::new();
    for (_, r) in &regs {
        if let Some(k) = kind_of(r) {
            groups.entry((r.form, r.disp)).or_default()[k.index()] += 1;
        }
    }
    let mut hits: Vec<Vec<Result<CvarSlot, CvarError>>> = vec![Vec::new(); names.len()];
    for (ni, r) in &regs {
        let Some(ni) = *ni else { continue };
        let min = if r.form == CvarForm::Direct { 3 } else { 1 };
        let group = groups.get(&(r.form, r.disp)).and_then(|g| winner(g, min));
        let slot = match (group, kind_of(r)) {
            (Some(g), Some(k)) if g == k => Ok(CvarSlot { kind: g, form: r.form, rva: r.rva }),
            // Direct: the slot's type wins over a variable with few accesses.
            (Some(g), None) if r.form == CvarForm::Direct => Ok(CvarSlot { kind: g, form: r.form, rva: r.rva }),
            // Indirect with a zero default: the slot's type, when at least 3 others agree.
            (Some(g), None) if r.zero && groups[&(r.form, r.disp)][g.index()] >= 3 => {
                Ok(CvarSlot { kind: g, form: r.form, rva: r.rva })
            }
            _ => Err(CvarError::UnknownType),
        };
        hits[ni].push(slot);
    }
    let out = hits
        .into_iter()
        .zip(named)
        .map(|(mut h, named)| {
            h.dedup();
            match h.as_slice() {
                [one] => one.clone(),
                [] if named => Err(CvarError::NoRegistration),
                [] => Err(CvarError::NoName),
                _ => Err(CvarError::Ambiguous),
            }
        })
        .collect();
    Ok((id, out))
}

pub fn find_cvars(exe: &Path, names: &[&str]) -> std::io::Result<ScanResult> {
    let file = std::fs::File::open(exe)?;
    // SAFETY: read-only mapping of a file we do not modify.
    let map = unsafe { memmap2::Mmap::map(&file)? };
    Ok(find_cvars_in(&map, names))
}

/// U1 streaming profile: the values players use for UE4 games on 6-8 GB cards
/// (FF7R community fix); a 10 GB card or more (9 GB of budget) gets the larger pool. Below 6 GB, or
/// with the size unknown, the pool is left to the game. `vram_mb`: the DXGI
/// budget the game had, or the card's size.
pub fn streaming_profile(vram_mb: u32) -> Vec<(&'static str, f64)> {
    let mut v = vec![("r.Streaming.MaxNumTexturesToStreamPerFrame", 3.0)];
    if vram_mb >= 6144 {
        v.push(("r.Streaming.PoolSize", if vram_mb >= 9216 { 5700.0 } else { 2850.0 }));
    }
    v
}

/// U1 test variants, each switched on by its own tessera.ini key (`<key>=1`),
/// off by default and independent of ue_streaming. Kept for A/B runs until
/// measurements say which ones help.
pub const VARIANTS: &[(&str, &[(&str, f64)])] = &[
    // Dynamic resolution off, full resolution (UE4 DRS is not meant for PC).
    ("ue_drs_off", &[("r.DynamicRes.OperationMode", 0.0), ("r.DynamicRes.MinScreenPercentage", 100.0)]),
    // Scene render targets only grow: no reallocation when the render size changes.
    ("ue_rt_grow", &[("r.SceneRenderTargetResizeMethod", 2.0)]),
    // Texture pool kept inside VRAM, more temporary memory for mip changes.
    ("ue_pool_vram", &[("r.Streaming.LimitPoolSizeToVRAM", 1.0), ("r.Streaming.MaxTempMemoryAllowed", 100.0)]),
    // Shader objects created when a package loads, not on first draw.
    ("ue_shaders_on_load", &[("r.CreateShadersOnLoad", 1.0)]),
    // Less game-thread time per frame for level streaming work (ms; UE4 defaults 5, 5, 1).
    (
        "ue_load_limits",
        &[
            ("s.AsyncLoadingTimeLimit", 2.0),
            ("s.LevelStreamingActorsUpdateTimeLimit", 2.0),
            ("s.UnregisterComponentsTimeLimit", 0.5),
        ],
    ),
];

/// tessera.ini lines (src/dll/uecvars.cpp) for `wanted` on the scanned image,
/// plus the variables left out and why.
pub fn ini_lines(id: ImageId, wanted: &[(&str, f64)], slots: &[Result<CvarSlot, CvarError>]) -> (Vec<String>, Vec<String>) {
    let mut lines = Vec::new();
    let mut skipped = Vec::new();
    for (&(name, value), slot) in wanted.iter().zip(slots) {
        match slot {
            Ok(s) => {
                let v = match s.kind {
                    CvarKind::Int => format!("{}", value.round() as i64),
                    CvarKind::Float => format!("{}", value as f32),
                    CvarKind::Bool => format!("{}", (value != 0.0) as u8),
                };
                lines.push(format!("cvar.{name}={v} {} {} {:x}", s.kind.tag(), s.form.tag(), s.rva));
            }
            Err(e) => skipped.push(format!("{name} ({e:?})")),
        }
    }
    if !lines.is_empty() {
        lines.insert(0, format!("ue_image={:x} {:x}", id.timestamp, id.size_of_image));
    }
    (lines, skipped)
}

#[cfg(test)]
mod tests {
    use super::*;

    const CODE_RVA: i64 = 0x1000;

    fn put(code: &mut [u8], disp_at: usize, tail: usize, target: u32) {
        let rel = target as i64 - (CODE_RVA + (disp_at + 4 + tail) as i64);
        code[disp_at..disp_at + 4].copy_from_slice(&(rel as i32).to_le_bytes());
    }

    /// FF7R's r.Streaming.PoolSize registration (UE 4.18), rebased.
    fn auto_int(code: &mut Vec<u8>, name: u32, target: u32) {
        let at = code.len();
        code.extend_from_slice(&[
            0x48, 0x83, 0xEC, 0x38, // sub rsp,38h
            0x48, 0x8B, 0x01, // mov rax,[rcx]
            0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov r8d,-1
            0x48, 0x8D, 0x15, 0, 0, 0, 0, // lea rdx,[name]
            0xFF, 0x50, 0x10, // call [rax+10h]
            0x48, 0x89, 0x05, 0, 0, 0, 0, // mov [target],rax
            0x48, 0x8B, 0xC8, 0x48, 0x8B, 0x10, // mov rcx,rax; mov rdx,[rax]
            0xFF, 0x52, 0x38, // call [rdx+38h]
            0x48, 0x89, 0x05, 0, 0, 0, 0, // mov [ref],rax
            0xC3,
        ]);
        put(code, at + 16, 0, name);
        put(code, at + 26, 0, target);
        put(code, at + 42, 0, target + 8);
    }

    /// FF7R's s.AsyncLoadingTimeLimit registration (float FAutoConsoleVariableRef).
    fn ref_var(code: &mut Vec<u8>, name: u32, gvar: u32, target: u32) {
        let at = code.len();
        code.extend_from_slice(&[
            0x48, 0x83, 0xEC, 0x38, // sub rsp,38h
            0x48, 0x8B, 0x01, // mov rax,[rcx]
            0x4C, 0x8D, 0x05, 0, 0, 0, 0, // lea r8,[gvar]
            0xC7, 0x44, 0x24, 0x20, 0, 0, 0, 0, // mov dword [rsp+20h],0
            0x48, 0x8D, 0x15, 0, 0, 0, 0, // lea rdx,[name]
            0xFF, 0x50, 0x20, // call [rax+20h]
            0x48, 0x89, 0x05, 0, 0, 0, 0, // mov [target],rax
            0xC3,
        ]);
        put(code, at + 10, 0, gvar);
        put(code, at + 25, 0, name);
        put(code, at + 35, 0, target);
    }

    /// movss xmm0,[gvar]
    fn read_float(code: &mut Vec<u8>, gvar: u32) {
        let at = code.len();
        code.extend_from_slice(&[0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0]);
        put(code, at + 4, 0, gvar);
    }

    fn names(list: &[&str]) -> (Vec<u8>, Vec<u32>) {
        let mut rdata = vec![0u8; 2];
        let mut at = Vec::new();
        for n in list {
            at.push(0x2000 + rdata.len() as u32);
            rdata.extend(utf16z(n));
        }
        (rdata, at)
    }

    fn image() -> Vec<u8> {
        let (rdata, at) = names(&["r.Streaming.PoolSize", "s.A", "s.B", "s.C"]);
        let mut code = Vec::new();
        auto_int(&mut code, at[0], 0x3008);
        for i in 0..3 {
            ref_var(&mut code, at[1 + i], 0x3100 + 4 * i as u32, 0x3180 + 16 * i as u32);
            read_float(&mut code, 0x3100 + 4 * i as u32);
        }
        pe_with(&code, &rdata)
    }

    /// Minimal PE32+: .text at 0x1000, .rdata at 0x2000, .data at 0x3000, file alignment 0x200.
    fn pe_with(code: &[u8], rdata: &[u8]) -> Vec<u8> {
        let mut f = vec![0u8; 0x800];
        f[0..2].copy_from_slice(b"MZ");
        f[0x3C..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        let nt = 0x80;
        f[nt..nt + 4].copy_from_slice(b"PE\0\0");
        let fh = nt + 4;
        f[fh..fh + 2].copy_from_slice(&0x8664u16.to_le_bytes());
        f[fh + 2..fh + 4].copy_from_slice(&3u16.to_le_bytes());
        f[fh + 4..fh + 8].copy_from_slice(&0x1234_5678u32.to_le_bytes());
        f[fh + 16..fh + 18].copy_from_slice(&240u16.to_le_bytes());
        f[fh + 18..fh + 20].copy_from_slice(&0x22u16.to_le_bytes());
        let oh = fh + 20;
        f[oh..oh + 2].copy_from_slice(&0x20Bu16.to_le_bytes());
        f[oh + 24..oh + 32].copy_from_slice(&0x1_4000_0000u64.to_le_bytes());
        f[oh + 32..oh + 36].copy_from_slice(&0x1000u32.to_le_bytes());
        f[oh + 36..oh + 40].copy_from_slice(&0x200u32.to_le_bytes());
        f[oh + 40..oh + 42].copy_from_slice(&6u16.to_le_bytes());
        f[oh + 56..oh + 60].copy_from_slice(&0x4000u32.to_le_bytes());
        f[oh + 60..oh + 64].copy_from_slice(&0x200u32.to_le_bytes());
        f[oh + 68..oh + 70].copy_from_slice(&2u16.to_le_bytes());
        f[oh + 108..oh + 112].copy_from_slice(&16u32.to_le_bytes());
        let sh = oh + 240;
        let secs: [(&[u8; 8], u32, u32, u32); 3] = [
            (b".text\0\0\0", 0x1000, 0x200, 0x6000_0020),
            (b".rdata\0\0", 0x2000, 0x400, 0x4000_0040),
            (b".data\0\0\0", 0x3000, 0x600, 0xC000_0040),
        ];
        for (i, (n, va, raw, ch)) in secs.iter().enumerate() {
            let h = sh + i * 40;
            f[h..h + 8].copy_from_slice(*n);
            f[h + 8..h + 12].copy_from_slice(&0x200u32.to_le_bytes());
            f[h + 12..h + 16].copy_from_slice(&va.to_le_bytes());
            f[h + 16..h + 20].copy_from_slice(&0x200u32.to_le_bytes());
            f[h + 20..h + 24].copy_from_slice(&raw.to_le_bytes());
            f[h + 36..h + 40].copy_from_slice(&ch.to_le_bytes());
        }
        f[0x200..0x200 + code.len()].copy_from_slice(code);
        f[0x400..0x400 + rdata.len()].copy_from_slice(rdata);
        f
    }

    #[test]
    fn finds_auto_and_ref_variables() {
        let (id, r) = find_cvars_in(&image(), &["r.Streaming.PoolSize", "s.B", "r.Missing"]).unwrap();
        assert_eq!(id.timestamp, 0x1234_5678);
        assert_eq!(r[0], Ok(CvarSlot { kind: CvarKind::Int, form: CvarForm::Indirect, rva: 0x3010 }));
        assert_eq!(r[1], Ok(CvarSlot { kind: CvarKind::Float, form: CvarForm::Direct, rva: 0x3104 }));
        assert_eq!(r[2], Err(CvarError::NoName));
    }

    #[test]
    fn prefix_of_longer_name_does_not_match() {
        let (_, r) = find_cvars_in(&image(), &["r.Streaming.Pool"]).unwrap();
        assert_eq!(r[0], Err(CvarError::NoName));
    }

    #[test]
    fn ref_must_follow_target() {
        let mut img = image();
        img[0x200 + 42] = img[0x200 + 42].wrapping_add(8); // Ref now Target + 16
        let (_, r) = find_cvars_in(&img, &["r.Streaming.PoolSize"]).unwrap();
        assert_eq!(r[0], Err(CvarError::NoRegistration));
    }

    #[test]
    fn ref_slot_without_consensus_is_unknown() {
        // One float access only for s.A: s.B and s.C have no reads, the slot has 1 vote < 3.
        let (rdata, at) = names(&["s.A", "s.B", "s.C"]);
        let mut code = Vec::new();
        for i in 0..3 {
            ref_var(&mut code, at[i], 0x3100 + 4 * i as u32, 0x3180 + 16 * i as u32);
        }
        read_float(&mut code, 0x3100);
        let (_, r) = find_cvars_in(&pe_with(&code, &rdata), &["s.A", "s.B"]).unwrap();
        assert_eq!(r[0], Err(CvarError::UnknownType));
        assert_eq!(r[1], Err(CvarError::UnknownType));
    }

    #[test]
    fn ini_lines_format_by_kind_and_skip_unplaced() {
        let id = ImageId { timestamp: 0x698ba49c, size_of_image: 0x5efc000 };
        let wanted = [("r.A", 2850.0), ("r.B", 0.5), ("r.C", 1.0), ("r.D", 3.0)];
        let slots = [
            Ok(CvarSlot { kind: CvarKind::Int, form: CvarForm::Indirect, rva: 0x59585c0 }),
            Ok(CvarSlot { kind: CvarKind::Float, form: CvarForm::Direct, rva: 0x53a57b4 }),
            Ok(CvarSlot { kind: CvarKind::Bool, form: CvarForm::Direct, rva: 0x10 }),
            Err(CvarError::NoName),
        ];
        let (lines, skipped) = ini_lines(id, &wanted, &slots);
        assert_eq!(
            lines,
            [
                "ue_image=698ba49c 5efc000",
                "cvar.r.A=2850 i p 59585c0",
                "cvar.r.B=0.5 f d 53a57b4",
                "cvar.r.C=1 b d 10"
            ]
        );
        assert_eq!(skipped, ["r.D (NoName)"]);
        assert!(ini_lines(id, &wanted[3..], &slots[3..]).0.is_empty());
        assert_eq!(streaming_profile(8192).len(), 2);
        assert_eq!(streaming_profile(0).len(), 1);
    }

    /// Real executable, when installed (TESSERA_UE_EXE=<path>).
    #[test]
    fn real_exe_when_given() {
        let Ok(p) = std::env::var("TESSERA_UE_EXE") else { return };
        let names = [
            "r.Streaming.PoolSize",
            "r.Streaming.MaxNumTexturesToStreamPerFrame",
            "r.Streaming.Boost",
            "s.AsyncLoadingTimeLimit",
            "s.LevelStreamingActorsUpdateTimeLimit",
            "r.CreateShadersOnLoad",
            "r.Streaming.LimitPoolSizeToVRAM",
            "r.Streaming.MaxTempMemoryAllowed",
            "s.UnregisterComponentsTimeLimit",
            "r.DynamicRes.OperationMode",
            "r.DynamicRes.MinScreenPercentage",
            "r.DynamicRes.MaxScreenPercentage",
            "r.DynamicRes.FrameTimeBudget",
            "r.DynamicRes.MinResolutionChangePeriod",
            "r.ScreenPercentage",
            "r.SceneRenderTargetResizeMethod",
        ];
        let t = std::time::Instant::now();
        let (id, r) = find_cvars(Path::new(&p), &names).unwrap().unwrap();
        eprintln!("{p}: {id:x?} in {:?}", t.elapsed());
        for (n, s) in names.iter().zip(&r) {
            eprintln!("  {n}: {s:x?}");
        }
        assert!(r[0].is_ok());
    }

    /// Share of console names whose registration decodes (TESSERA_UE_EXE=<path>).
    #[test]
    fn coverage_when_given() {
        let Ok(p) = std::env::var("TESSERA_UE_EXE") else { return };
        let data = std::fs::read(&p).unwrap();
        let mut set = std::collections::BTreeSet::new();
        let w: Vec<u16> = data.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
        let mut i = 0;
        while i < w.len() {
            let j = (i..w.len()).find(|&k| w[k] == 0).unwrap_or(w.len());
            if j - i > 3 && j - i < 80 && i > 0 && w[i - 1] == 0 {
                if let Ok(s) = String::from_utf16(&w[i..j]) {
                    let pre = ["r.", "s.", "t.", "g.", "fx.", "D3D11."];
                    if pre.iter().any(|x| s.starts_with(x))
                        && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '.' || c == '_')
                    {
                        set.insert(s);
                    }
                }
            }
            i = j + 1;
        }
        let v: Vec<&str> = set.iter().map(String::as_str).collect();
        let (_, r) = find_cvars_in(&data, &v).unwrap();
        let mut count: HashMap<String, u32> = HashMap::new();
        for (n, s) in v.iter().zip(&r) {
            let key = match s {
                Ok(c) => format!("{}{}", c.kind.tag(), c.form.tag()),
                Err(e) => format!("{e:?}"),
            };
            if matches!(s, Err(CvarError::UnknownType)) || (s.is_err() && (n.contains("Stream") || n.contains("Async"))) {
                eprintln!("  {key}: {n}");
            }
            *count.entry(key).or_default() += 1;
        }
        eprintln!("{p}: {} names {count:?}", v.len());
    }
}
