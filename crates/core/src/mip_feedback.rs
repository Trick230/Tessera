// SPDX-License-Identifier: GPL-3.0-or-later
//! Offline instrumentation of a subset of D3D11 pixel shaders.
//! Feedback is a raw UAV at u7 with 128 u32s initialized to UINT_MAX. A slot
//! records the smallest integer mip requested by a supported sample at tN.

const PS_5_0: u32 = 0x50;
const SAMPLE: u32 = 69;
const SAMPLE_L: u32 = 72;
const DECL_RESOURCE: u32 = 88;
const DECL_TEMPS: u32 = 104;
const DECL_GLOBAL_FLAGS: u32 = 106;
const DECL_UAV_RAW: u32 = 157;
const LOD: u32 = 108;
const MAX: u32 = 52;
const FTOU: u32 = 28;
const ATOMIC_UMIN: u32 = 177;
const FEEDBACK_SLOT: u32 = 7;
const IMMEDIATE: u32 = 0x00004001;
const MAX_TEXTURES: usize = 8;
const CACHE_LIMIT: usize = 128 * 1024 * 1024;

fn word(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn words(part: &[u8]) -> Vec<u32> {
    part.chunks_exact(4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .collect()
}

fn push(buf: &mut Vec<u8>, value: u32) {
    buf.extend_from_slice(&value.to_le_bytes());
}

fn instruction(op: u32, len: u32) -> u32 {
    op | (len << 24)
}

// One fixed register index, without extensions or relative addressing.
fn register(operand: &[u32]) -> Option<(u32, u32)> {
    let token = *operand.first()?;
    if token >> 31 != 0 || (token >> 20) & 3 != 1 || (token >> 22) & 7 != 0 {
        return None;
    }
    Some(((token >> 12) & 0xff, *operand.get(1)?))
}

fn sample_operands(inst: &[u32], explicit_lod: bool) -> Option<(&[u32], u32)> {
    let mut first = 1;
    let mut token = inst[0];
    if token & 0x00fff800 != 0 {
        return None;
    }
    while token >> 31 != 0 {
        token = *inst.get(first)?;
        // Resource dimension/return type extensions are harmless. Reject
        // immediate offsets and any extensions we don't understand.
        if !matches!(token & 63, 2 | 3) {
            return None;
        }
        first += 1;
    }
    let operands = inst.get(first..)?;
    let operand_words = if explicit_lod { 10 } else { 8 };
    if operands.len() != operand_words
        || !matches!(register(operands)?, (0 | 2, _))
        || !matches!(register(&operands[2..])?, (0 | 1, _))
        || !matches!(register(&operands[6..])?, (6, 0..=15))
    {
        return None;
    }
    if explicit_lod
        && operands[8] != IMMEDIATE
        && !matches!(register(&operands[8..]), Some((0 | 1, _)))
    {
        return None;
    }
    match register(&operands[4..])? {
        (7, slot @ 0..=127) => Some((operands, slot)),
        _ => None,
    }
}

fn emit_feedback(out: &mut Vec<u32>, operands: &[u32], slot: u32, temp: u32) {
    const DEST_X: u32 = 0x00100012;
    const SOURCE_X: u32 = 0x0010000a;
    const RESOURCE_Y: u32 = 0x0010701a;
    const UAV: u32 = 0x0011e000;
    let level = if operands.len() == 10 {
        [operands[8], operands[9]]
    } else {
        // lod's .y is the unclamped mip. The sample can overwrite its inputs.
        out.extend_from_slice(&[
            instruction(LOD, 9),
            DEST_X,
            temp,
            operands[2],
            operands[3],
            RESOURCE_Y,
            slot,
            operands[6],
            operands[7],
        ]);
        [SOURCE_X, temp]
    };
    out.extend_from_slice(&[
        instruction(MAX, 7),
        DEST_X,
        temp,
        level[0],
        level[1],
        IMMEDIATE,
        0,
        instruction(FTOU, 5),
        DEST_X,
        temp,
        SOURCE_X,
        temp,
        instruction(ATOMIC_UMIN, 7),
        UAV,
        FEEDBACK_SLOT,
        IMMEDIATE,
        slot * 4,
        SOURCE_X,
        temp,
    ]);
}

fn instrument_program(part: &[u8]) -> Option<(Vec<u8>, Vec<u32>)> {
    if part.len() % 4 != 0 {
        return None;
    }
    let code = words(part);
    if code.first() != Some(&PS_5_0) || *code.get(1)? as usize != code.len() {
        return None;
    }
    let mut cursor = 2;
    let mut declarations = Vec::new();
    let mut body = Vec::new();
    let mut slots = Vec::new();
    let mut resources = [false; 128];
    let mut temp = None;
    while cursor < code.len() {
        let token = code[cursor];
        let op = token & 2047;
        let len = if op == 53 {
            *code.get(cursor + 1)? as usize
        } else {
            ((token >> 24) & 127) as usize
        };
        if len == 0 || (op == 53 && len < 2) {
            return None;
        }
        let inst = code.get(cursor..cursor.checked_add(len)?)?;
        cursor += len;
        if matches!(op, 144..=146 | 156..=158)
            || (op == DECL_GLOBAL_FLAGS && token & (1 << 13) != 0)
        {
            return None; // linkable classes, existing UAVs or forced early depth
        }
        if op == DECL_TEMPS {
            if len != 2 || temp.is_some() || inst[1] >= 4096 {
                return None;
            }
            temp = Some(inst[1]);
            continue;
        }
        if op == DECL_RESOURCE && len == 4 && ((token >> 11) & 31) == 3 {
            if let Some((7, slot @ 0..=127)) = register(&inst[1..]) {
                resources[slot as usize] = true;
            }
        }
        if matches!(op, 101..=103) && register(&inst[1..]) == Some((2, FEEDBACK_SLOT)) {
            return None;
        }
        if matches!(op, 88..=106 | 143..=162) {
            declarations.extend_from_slice(inst);
        } else {
            if matches!(op, SAMPLE | SAMPLE_L) && slots.len() < MAX_TEXTURES {
                if let Some((operands, slot)) = sample_operands(inst, op == SAMPLE_L) {
                    if resources[slot as usize] && !slots.contains(&slot) {
                        emit_feedback(&mut body, operands, slot, temp.unwrap_or(0));
                        slots.push(slot);
                    }
                }
            }
            body.extend_from_slice(inst);
        }
    }
    if slots.is_empty() {
        return None;
    }
    let mut out = Vec::new();
    push(&mut out, PS_5_0);
    push(
        &mut out,
        (2 + declarations.len() + 5 + body.len()).try_into().ok()?,
    );
    for value in declarations {
        push(&mut out, value);
    }
    for value in [
        instruction(DECL_UAV_RAW, 3),
        0x0011e000,
        FEEDBACK_SLOT,
        instruction(DECL_TEMPS, 2),
        temp.unwrap_or(0) + 1,
    ] {
        push(&mut out, value);
    }
    for value in body {
        push(&mut out, value);
    }
    Some((out, slots))
}

/// Preserves signatures, adds the feedback UAV, and recomputes the DXBC
/// checksum. Reflection and statistics describing the old program are omitted.
pub fn instrument(code: &[u8]) -> Option<(Vec<u8>, Vec<u32>)> {
    if code.len() < 32
        || code.len() > 1024 * 1024
        || code.get(..4)? != b"DXBC"
        || word(code, 20)? != 1
        || word(code, 24)? as usize != code.len()
        || code[4..20] != crate::scan::checksum(code)
    {
        return None;
    }
    let count = word(code, 28)? as usize;
    if count == 0 || count > 32 || 32 + count * 4 > code.len() {
        return None;
    }
    let mut signatures = Vec::new();
    let mut program = None;
    for i in 0..count {
        let offset = word(code, 32 + i * 4)? as usize;
        if offset < 32 + count * 4 || offset % 4 != 0 {
            return None;
        }
        let tag = code.get(offset..offset.checked_add(4)?)?;
        let size = word(code, offset.checked_add(4)?)? as usize;
        let start = offset.checked_add(8)?;
        let part = code.get(start..start.checked_add(size)?)?;
        match tag {
            b"ISGN" | b"ISG1" | b"OSGN" | b"OSG1" | b"OSG5" => signatures.push((tag, part)),
            b"SHDR" | b"SHEX" if program.is_none() => program = Some(instrument_program(part)?),
            b"SHDR" | b"SHEX" | b"DXIL" => return None,
            _ => {}
        }
    }
    let (modified, slots) = program?;
    signatures.push((b"SHEX", &modified));
    let mut out = vec![0; 32 + signatures.len() * 4];
    out[..4].copy_from_slice(b"DXBC");
    out[20..24].copy_from_slice(&1u32.to_le_bytes());
    out[28..32].copy_from_slice(&(signatures.len() as u32).to_le_bytes());
    for (i, (tag, part)) in signatures.into_iter().enumerate() {
        let offset = (out.len() as u32).to_le_bytes();
        out[32 + i * 4..36 + i * 4].copy_from_slice(&offset);
        out.extend_from_slice(tag);
        push(&mut out, part.len() as u32);
        out.extend_from_slice(part);
        while out.len() % 4 != 0 {
            out.push(0);
        }
    }
    let total = (out.len() as u32).to_le_bytes();
    out[24..28].copy_from_slice(&total);
    let checksum = crate::scan::checksum(&out);
    out[4..20].copy_from_slice(&checksum);
    Some((out, slots))
}

/// Prepare the DLL's optional cache from shaders recorded for this game.
/// Keep the original checksum as the lookup key. The driver still validates
/// each instrumented variant before the DLL can use it.
pub fn prepare(exe: &std::path::Path) -> std::io::Result<usize> {
    use std::collections::BTreeMap;
    use std::fs::{self, OpenOptions};
    use std::io::{self, Write};

    let key = crate::session::exe_key_name(exe)
        .ok_or_else(|| io::Error::other("invalid executable path"))?;
    let root = crate::paths::data_dir().join("feedback");
    fs::create_dir_all(&root)?;
    let mut cache = BTreeMap::new();
    let mut bytes = 16;
    for shader in crate::shaders::read(exe) {
        if shader.stage != crate::shaders::STAGE_PS {
            continue;
        }
        if let Some((code, slots)) = instrument(&shader.code) {
            if code.len() > 1024 * 1024 || bytes + 32 + code.len() > CACHE_LIMIT {
                break;
            }
            let digest: [u8; 16] = shader.code[4..20].try_into().unwrap();
            if let std::collections::btree_map::Entry::Vacant(entry) = cache.entry(digest) {
                bytes += 32 + code.len();
                entry.insert((code, slots));
            }
            if cache.len() == 4096 {
                break;
            }
        }
    }
    let destination = root.join(format!("{key}.tmip"));
    let temp = destination.with_extension(format!("{}.tmp", std::process::id()));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temp)?;
    let result = (|| {
        file.write_all(b"TSRMIP1\0")?;
        file.write_all(&(cache.len() as u32).to_le_bytes())?;
        file.write_all(&0u32.to_le_bytes())?;
        for (key, (code, slots)) in &cache {
            let mut indices = [0u8; MAX_TEXTURES];
            for (dst, &slot) in indices.iter_mut().zip(slots) {
                *dst = slot as u8;
            }
            file.write_all(key)?;
            file.write_all(&(code.len() as u32).to_le_bytes())?;
            file.write_all(&(slots.len() as u32).to_le_bytes())?;
            file.write_all(&indices)?;
            file.write_all(code)?;
        }
        drop(file);
        fs::rename(&temp, destination)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result.map(|_| cache.len())
}
