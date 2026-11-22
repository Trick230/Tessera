// SPDX-License-Identifier: GPL-3.0-or-later
//! Reader for the session logs written by tessera.dll. Mirrors
//! `include/tessera/session_format.h`, which is the source of truth.

use std::path::{Path, PathBuf};

pub const MAGIC: &[u8; 8] = b"TSRSESS\0";
pub const VERSION: u32 = 1;
pub const HEADER_SIZE: usize = 568;
pub const RECORD_SIZE: usize = 40;
/// Unclean starts in a row after which the layer disables itself.
pub const SAFE_MODE_CRASHES: u32 = 2;

pub const REC_FRAME: u16 = 1;
pub const REC_TEXTURE2D: u16 = 2;
pub const REC_BUFFER: u16 = 3;
pub const REC_SRV: u16 = 4;
pub const REC_DEVICE: u16 = 5;
pub const REC_SWAPCHAIN: u16 = 6;
pub const REC_VRAM: u16 = 7;
pub const REC_SHADER: u16 = 8;
pub const REC_WORKSET: u16 = 9;
pub const REC_RESIDENCY: u16 = 10;
pub const REC_MIP_FEEDBACK: u16 = 12;
pub const REC_END: u16 = 0xFFFF;

/// Resource created by code outside the game's folder (overlay, SDK).
pub const FLAG_FOREIGN: u32 = 0x8000_0000;
/// Eviction priority raised by the layer (M2).
pub const FLAG_PRIORITY: u32 = 0x4000_0000;
/// The texture allocation uses the compressed byte count (M9).
pub const FLAG_RECOMPRESSED: u32 = 0x0400_0000;
/// Bit in `d` of SHADER records: not in the shader store when created (shader_store_format.h).
pub const SHADER_NEW: u32 = 0x4;

/// A hitch counts as caused by resource creation when the game created at
/// least this many resources or bytes during that frame.
const HITCH_CREATE_COUNT: u32 = 8;
const HITCH_CREATE_BYTES: u64 = 4 << 20;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Record {
    pub qpc: u64,
    pub kind: u16,
    pub format: u16,
    pub flags: u32,
    pub a: u32,
    pub b: u32,
    pub c: u32,
    pub d: u32,
    pub bytes: u64,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Header {
    pub pid: u32,
    pub qpc_frequency: u64,
    pub start_qpc: u64,
    /// UTC FILETIME (100 ns ticks since 1601).
    pub start_filetime: u64,
    pub exe_path: String,
}

/// Aggregates of one session, enough for the detail panel and the planner.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Summary {
    pub header: Header,
    /// The file ends with an END record (the game exited through DLL detach).
    pub clean: bool,
    /// Records lost to ring overrun, as reported by the END record.
    pub dropped: u32,
    pub frames: u64,
    /// From the first to the last record, in seconds.
    pub duration_s: f64,
    /// Frame time percentiles in milliseconds (p50, p99, p99.9) and the worst frame.
    pub frame_avg_ms: f64,
    pub frame_p50_ms: f64,
    pub frame_p99_ms: f64,
    pub frame_p999_ms: f64,
    pub frame_max_ms: f64,
    /// Frames slower than twice the median: the stutters a player notices.
    pub hitches: u64,
    /// D2 (basic): hitches in frames where the game created many resources
    /// (streaming or loading); the rest are other causes (shaders, I/O,
    /// game logic) that the layer does not record yet.
    pub hitches_creating: u64,
    /// D2: hitches in frames where the game created a shader (driver compile);
    /// counted before the other causes.
    pub hitches_shaders: u64,
    /// S6: shaders the game created, those not yet in the shader store (cold:
    /// compiled for the first time), and the time the creation calls took.
    pub shaders: u64,
    pub shaders_new: u64,
    pub shader_create_ms: f64,
    /// D2: hitches while the game used more VRAM than the OS budget (and
    /// not creating resources): paging between VRAM and system memory.
    pub hitches_over_budget: u64,
    /// M1 samples (about one a second): lowest real budget, highest usage
    /// and seconds spent over budget, in MB. Zero when the layer had no samples.
    pub vram_budget_mb: u32,
    pub vram_peak_mb: u32,
    pub vram_over_s: u32,
    /// Budget cap the layer reported to the game (0 = none).
    pub vram_cap_mb: u32,
    /// Frames the frame limiter waited for (P1), and its total wait in seconds.
    pub limited_frames: u64,
    pub limiter_wait_s: f64,
    /// Resources made by other modules (overlays, SDKs), left untouched.
    pub foreign: u64,
    /// Resources the layer raised to maximum eviction priority (M2).
    pub prioritized: u64,
    pub textures: u64,
    pub texture_bytes: u64,
    pub recompressed_textures: u64,
    pub mip_feedback_samples: u64,
    pub buffers: u64,
    pub buffer_bytes: u64,
    pub feature_level: u32,
    /// D3D11 tiled resources tier of the device (C4), 0 = none.
    pub tiled_tier: u32,
    pub swapchain: Option<(u32, u32)>,
    /// M11 texture working set; None when the layer had no samples.
    pub workset: Option<WorkSet>,
    /// M3-M5/M10 mip residency; None when it was off.
    pub residency: Option<Residency>,
}

/// M3-M5/M10: what mip residency did during the session.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Residency {
    /// Most managed textures alive at once, and most with the top mip evicted.
    pub managed_peak: u32,
    pub evicted_peak: u32,
    pub evictions: u64,
    pub restores: u64,
    /// Restores that came after the texture was already drawn (pop-in risk).
    pub late: u32,
    /// Most system memory holding evicted top mips, MB.
    pub ram_peak_mb: u32,
}

/// M11: how much of the game's sampled texture memory play really binds.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct WorkSet {
    /// Seconds sampled while the game presented frames.
    pub samples: u32,
    /// Most sampled-texture memory alive at once, MB.
    pub textures_mb: u32,
    /// Bound in a second: average and highest, MB.
    pub used_avg_mb: u32,
    pub used_peak_mb: u32,
    /// Highest bound over any 10 s, MB.
    pub used10_peak_mb: u32,
    /// Textures and views the meter could not track (tables full).
    pub untracked: u32,
}

impl Summary {
    /// Estimated VRAM allocated by the game through the hooked calls (no frees tracked yet).
    pub fn vram_bytes(&self) -> u64 {
        self.texture_bytes + self.buffer_bytes
    }
}

#[derive(Debug)]
pub enum Error {
    Io(std::io::Error),
    Format(&'static str),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Io(e) => write!(f, "{e}"),
            Error::Format(m) => write!(f, "invalid session: {m}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

pub fn parse_header(b: &[u8]) -> Result<(Header, usize, usize), Error> {
    if b.len() < 24 || &b[..8] != MAGIC {
        return Err(Error::Format("magic"));
    }
    if u32_at(b, 8) != VERSION {
        return Err(Error::Format("version"));
    }
    let header_size = u32_at(b, 12) as usize;
    let record_size = u32_at(b, 16) as usize;
    // Only the known prefix is read here; summarize() checks the full header_size.
    if header_size < HEADER_SIZE || record_size < RECORD_SIZE || b.len() < HEADER_SIZE {
        return Err(Error::Format("tamanhos"));
    }
    let wide: Vec<u16> = (0..260).map(|i| u16_at(b, 48 + i * 2)).take_while(|&c| c != 0).collect();
    let h = Header {
        pid: u32_at(b, 20),
        qpc_frequency: u64_at(b, 24),
        start_qpc: u64_at(b, 32),
        start_filetime: u64_at(b, 40),
        exe_path: String::from_utf16_lossy(&wide),
    };
    if h.qpc_frequency == 0 {
        return Err(Error::Format("frequency"));
    }
    Ok((h, header_size, record_size))
}

/// Whole records after the header; a trailing partial record (cut-short file) is ignored.
pub fn records(b: &[u8], header_size: usize, record_size: usize) -> impl Iterator<Item = Record> + '_ {
    b[header_size..].chunks_exact(record_size).map(|r| Record {
        qpc: u64_at(r, 0),
        kind: u16_at(r, 8),
        format: u16_at(r, 10),
        flags: u32_at(r, 12),
        a: u32_at(r, 16),
        b: u32_at(r, 20),
        c: u32_at(r, 24),
        d: u32_at(r, 28),
        bytes: u64_at(r, 32),
    })
}

/// Nearest-rank percentile of a sorted slice.
fn percentile(sorted: &[u32], p: f64) -> u32 {
    if sorted.is_empty() {
        return 0;
    }
    let rank = ((p / 100.0) * sorted.len() as f64).ceil() as usize;
    sorted[rank.clamp(1, sorted.len()) - 1]
}

pub fn summarize(b: &[u8]) -> Result<Summary, Error> {
    let (header, hs, rs) = parse_header(b)?;
    if b.len() < hs {
        return Err(Error::Format("tamanhos"));
    }
    let mut s = Summary { header, ..Default::default() };
    let mut res: Option<Residency> = None;
    let mut frame_us: Vec<u32> = Vec::with_capacity((b.len() - hs) / rs);
    // Frames during which the game created many resources, by index into frame_us.
    let mut creating: Vec<bool> = Vec::with_capacity(frame_us.capacity());
    let mut over: Vec<bool> = Vec::with_capacity(frame_us.capacity());
    let mut compiling: Vec<bool> = Vec::with_capacity(frame_us.capacity());
    let mut shader_now = false;
    let mut shader_us = 0u64;
    let mut over_now = false;
    let (mut made, mut made_bytes) = (0u32, 0u64);
    let mut limiter_us = 0u64;
    let mut ws = WorkSet::default();
    let mut ws_used_kb = 0u64;
    let (mut first, mut last) = (u64::MAX, 0u64);
    for r in records(b, hs, rs) {
        if r.kind != REC_END {
            first = first.min(r.qpc);
            last = last.max(r.qpc);
        }
        if matches!(r.kind, REC_TEXTURE2D | REC_BUFFER) {
            if r.flags & FLAG_FOREIGN != 0 {
                s.foreign += 1;
            } else {
                made += 1;
                made_bytes = made_bytes.saturating_add(r.bytes);
            }
            if r.flags & FLAG_PRIORITY != 0 {
                s.prioritized += 1;
            }
        }
        match r.kind {
            REC_FRAME => {
                s.frames += 1;
                if r.a != 0 && r.a != u32::MAX {
                    frame_us.push(r.a);
                    creating.push(made >= HITCH_CREATE_COUNT || made_bytes >= HITCH_CREATE_BYTES);
                    over.push(over_now);
                    compiling.push(shader_now);
                }
                shader_now = false;
                if r.b != 0 {
                    s.limited_frames += 1;
                    limiter_us += r.b as u64;
                }
                (made, made_bytes) = (0, 0);
            }
            REC_TEXTURE2D => {
                s.textures += 1;
                s.texture_bytes = s.texture_bytes.saturating_add(r.bytes);
                s.recompressed_textures += u64::from(r.flags & FLAG_RECOMPRESSED != 0);
            }
            REC_MIP_FEEDBACK => s.mip_feedback_samples += 1,
            REC_BUFFER => {
                s.buffers += 1;
                s.buffer_bytes = s.buffer_bytes.saturating_add(r.bytes);
            }
            REC_VRAM if r.a > 0 => {
                s.vram_budget_mb = if s.vram_budget_mb == 0 { r.a } else { s.vram_budget_mb.min(r.a) };
                s.vram_peak_mb = s.vram_peak_mb.max(r.b);
                s.vram_cap_mb = r.d;
                over_now = r.b > r.a;
                s.vram_over_s += over_now as u32;
            }
            REC_SHADER if r.flags & FLAG_FOREIGN == 0 => {
                s.shaders += 1;
                s.shaders_new += (r.d & SHADER_NEW != 0) as u64;
                shader_us += r.c as u64;
                shader_now = true;
            }
            REC_DEVICE => {
                s.feature_level = r.a;
                s.tiled_tier = r.b;
            }
            // Seconds with no frames (loading, menus paused) say nothing about play.
            REC_WORKSET if r.flags > 0 => {
                ws.samples += 1;
                ws.textures_mb = ws.textures_mb.max((r.bytes >> 20) as u32);
                ws.used_peak_mb = ws.used_peak_mb.max(r.a >> 10);
                ws.used10_peak_mb = ws.used10_peak_mb.max(r.b >> 10);
                ws.untracked = ws.untracked.max(r.d);
                ws_used_kb += r.a as u64;
            }
            REC_SWAPCHAIN => s.swapchain = Some((r.a, r.b)),
            REC_RESIDENCY => {
                let m = res.get_or_insert_with(Residency::default);
                m.managed_peak = m.managed_peak.max(r.a);
                m.evicted_peak = m.evicted_peak.max(r.b);
                m.restores += r.c as u64;
                m.evictions += r.d as u64;
                m.late = m.late.max(r.flags);
                m.ram_peak_mb = m.ram_peak_mb.max((r.bytes >> 20) as u32);
            }
            REC_END => {
                s.clean = true;
                s.dropped = r.a;
            }
            _ => {}
        }
    }
    if last > first {
        s.duration_s = (last - first) as f64 / s.header.qpc_frequency as f64;
    }
    s.limiter_wait_s = limiter_us as f64 / 1e6;
    s.residency = res;
    if ws.samples > 0 {
        ws.used_avg_mb = (ws_used_kb / ws.samples as u64 >> 10) as u32;
        s.workset = Some(ws);
    }
    s.shader_create_ms = shader_us as f64 / 1e3;
    if !frame_us.is_empty() {
        let sum: u64 = frame_us.iter().map(|&v| v as u64).sum();
        s.frame_avg_ms = sum as f64 / frame_us.len() as f64 / 1000.0;
        let mut sorted = frame_us.clone();
        sorted.sort_unstable();
        let ms = |v: u32| v as f64 / 1000.0;
        let p50 = percentile(&sorted, 50.0);
        s.frame_p50_ms = ms(p50);
        s.frame_p99_ms = ms(percentile(&sorted, 99.0));
        s.frame_p999_ms = ms(percentile(&sorted, 99.9));
        s.frame_max_ms = ms(*sorted.last().unwrap());
        let limit = p50.saturating_mul(2);
        for (((&v, &c), &o), &sh) in frame_us.iter().zip(&creating).zip(&over).zip(&compiling) {
            if v > limit {
                s.hitches += 1;
                s.hitches_shaders += sh as u64;
                s.hitches_creating += (!sh && c) as u64;
                s.hitches_over_budget += (!sh && !c && o) as u64;
            }
        }
    }
    Ok(s)
}

/// M11 → M1: a VRAM cap, in whole GB as MB, that still fits everything play
/// used: the peak usage less the sampled textures nothing bound for 10 s,
/// plus a margin. None without a full minute of samples, when the meter
/// missed textures, or when the cap would not be below the real budget.
pub fn suggested_vram_cap_mb(s: &Summary) -> Option<u32> {
    const MARGIN_MB: u32 = 512;
    let w = s.workset.as_ref()?;
    if w.samples < 60 || w.untracked > 0 || s.vram_budget_mb == 0 || s.vram_peak_mb == 0 {
        return None;
    }
    let idle = w.textures_mb.saturating_sub(w.used10_peak_mb);
    let need = s.vram_peak_mb.saturating_sub(idle) + MARGIN_MB;
    let cap = need.div_ceil(1024).max(3) * 1024;
    (cap < s.vram_budget_mb / 1024 * 1024).then_some(cap)
}

pub fn summarize_file(path: &Path) -> Result<Summary, Error> {
    summarize(&std::fs::read(path)?)
}

pub fn sessions_dir() -> PathBuf {
    crate::paths::data_dir().join("sessions")
}

fn exe_file_name(exe: &Path) -> Option<String> {
    exe.file_name().map(|n| n.to_string_lossy().into_owned())
}

fn norm_path(p: &str) -> String {
    let p = p.strip_prefix(r"\\?\").unwrap_or(p);
    p.replace('/', "\\").to_lowercase()
}

/// Header of a session file, reading only its fixed prefix.
pub fn read_header(path: &Path) -> Option<Header> {
    use std::io::Read;
    let mut b = [0u8; HEADER_SIZE];
    std::fs::File::open(path).ok()?.read_exact(&mut b).ok()?;
    parse_header(&b).ok().map(|(h, _, _)| h)
}

/// Newest valid `.tsess` the layer wrote for this executable. Files are named
/// `<exe name>_<time>_<pid>.tsess`; the full path in the header tells apart
/// games that share an exe name, and empty or corrupt files are skipped.
pub fn latest_in(dir: &Path, exe: &Path) -> Option<PathBuf> {
    let prefix = format!("{}_", exe_file_name(exe)?.to_lowercase());
    let want = norm_path(&exe.to_string_lossy());
    let mut files: Vec<_> = std::fs::read_dir(dir)
        .ok()?
        .filter_map(Result::ok)
        .filter_map(|e| {
            let n = e.file_name().to_string_lossy().to_lowercase();
            (n.starts_with(&prefix) && n.ends_with(".tsess"))
                .then(|| (e.metadata().and_then(|m| m.modified()).ok(), n, e.path()))
        })
        .collect();
    // Newest first by mtime; the name (local start time) breaks ties.
    files.sort_unstable_by(|a, b| (&b.0, &b.1).cmp(&(&a.0, &a.1)));
    files
        .into_iter()
        .map(|(_, _, p)| p)
        .find(|p| read_header(p).is_some_and(|h| norm_path(&h.exe_path) == want))
}

pub fn latest(exe: &Path) -> Option<PathBuf> {
    latest_in(&sessions_dir(), exe)
}

/// FNV-1a 64 over the exe path as UTF-16 code units, with a leading `\\?\`
/// dropped, '/' read as '\' and ASCII A-Z lowercased. Must match path_key()
/// in src/dll/session.cpp.
fn safe_mode_key(exe: &std::ffi::OsStr) -> u64 {
    use std::os::windows::ffi::OsStrExt;
    let wide: Vec<u16> = exe.encode_wide().collect();
    let prefix: Vec<u16> = r"\\?\".encode_utf16().collect();
    let p = wide.strip_prefix(&prefix[..]).unwrap_or(&wide);
    p.iter().fold(0xcbf2_9ce4_8422_2325, |h, &c| {
        let c = match c {
            0x2f => 0x5c,
            0x41..=0x5a => c + 32,
            _ => c,
        };
        (h ^ u64::from(c)).wrapping_mul(0x100_0000_01b3)
    })
}

/// `<exe name>-<path key>`: the layer's per-game file stem, keyed by the full
/// path so games that share an exe name keep separate files.
pub fn exe_key_name(exe: &Path) -> Option<String> {
    Some(format!("{}-{:016x}", exe_file_name(exe)?, safe_mode_key(exe.as_os_str())))
}

fn safe_mode_file(exe: &Path) -> Option<PathBuf> {
    Some(crate::paths::data_dir().join("safemode").join(format!("{}.count", exe_key_name(exe)?)))
}

/// Unclean starts in a row recorded by the layer for this executable.
pub fn crash_count(exe: &Path) -> u32 {
    safe_mode_file(exe)
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|t| t.trim().parse().ok())
        .unwrap_or(0)
}

pub fn safe_mode_active(exe: &Path) -> bool {
    crash_count(exe) >= SAFE_MODE_CRASHES
}

/// Re-enables the layer after safe mode tripped. Only touches Tessera's own data dir.
pub fn reset_safe_mode(exe: &Path) -> std::io::Result<()> {
    match safe_mode_file(exe) {
        Some(p) => match std::fs::remove_file(p) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
            _ => Ok(()),
        },
        None => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn header(freq: u64) -> Vec<u8> {
        header_for(freq, "C:\\g\\game.exe")
    }

    fn header_for(freq: u64, exe: &str) -> Vec<u8> {
        let mut b = vec![0u8; HEADER_SIZE];
        b[..8].copy_from_slice(MAGIC);
        b[8..12].copy_from_slice(&VERSION.to_le_bytes());
        b[12..16].copy_from_slice(&(HEADER_SIZE as u32).to_le_bytes());
        b[16..20].copy_from_slice(&(RECORD_SIZE as u32).to_le_bytes());
        b[20..24].copy_from_slice(&1234u32.to_le_bytes());
        b[24..32].copy_from_slice(&freq.to_le_bytes());
        for (i, c) in exe.encode_utf16().enumerate() {
            b[48 + i * 2..50 + i * 2].copy_from_slice(&c.to_le_bytes());
        }
        b
    }

    fn rec(b: &mut Vec<u8>, qpc: u64, kind: u16, a: u32, bb: u32, bytes: u64) {
        rec_flags(b, qpc, kind, 0, a, bb, bytes)
    }

    fn rec_flags(b: &mut Vec<u8>, qpc: u64, kind: u16, flags: u32, a: u32, bb: u32, bytes: u64) {
        rec_full(b, qpc, kind, flags, [a, bb, 0, 0], bytes)
    }

    fn rec_full(b: &mut Vec<u8>, qpc: u64, kind: u16, flags: u32, abcd: [u32; 4], bytes: u64) {
        b.extend_from_slice(&qpc.to_le_bytes());
        b.extend_from_slice(&kind.to_le_bytes());
        b.extend_from_slice(&0u16.to_le_bytes());
        b.extend_from_slice(&flags.to_le_bytes());
        for v in abcd {
            b.extend_from_slice(&v.to_le_bytes());
        }
        b.extend_from_slice(&bytes.to_le_bytes());
    }


    #[test]
    fn summarizes_mip_residency() {
        let mut b = header(1000);
        assert_eq!(summarize(&b).unwrap().residency, None, "off: no records");
        rec_full(&mut b, 1, REC_RESIDENCY, 0, [10, 4, 1, 5], 30 << 20);
        rec_full(&mut b, 2, REC_RESIDENCY, 2, [12, 2, 3, 0], 10 << 20);
        let r = summarize(&b).unwrap().residency.unwrap();
        assert_eq!(
            r,
            Residency { managed_peak: 12, evicted_peak: 4, evictions: 5, restores: 4, late: 2, ram_peak_mb: 30 }
        );
    }

    #[test]
    fn suggests_a_vram_cap_from_the_working_set() {
        let w = WorkSet { samples: 600, textures_mb: 5000, used_avg_mb: 900, used_peak_mb: 1500, used10_peak_mb: 2000, untracked: 0 };
        let mut s = Summary { vram_budget_mb: 7400, vram_peak_mb: 7000, workset: Some(w), ..Default::default() };
        // 7000 - (5000 - 2000) + 512 = 4512 → 5 GB.
        assert_eq!(suggested_vram_cap_mb(&s), Some(5 << 10));
        s.workset.as_mut().unwrap().used10_peak_mb = 4900;
        assert_eq!(suggested_vram_cap_mb(&s), None, "no lower than the budget's whole GB");
        s.workset.as_mut().unwrap().used10_peak_mb = 100;
        s.vram_peak_mb = 2000;
        assert_eq!(suggested_vram_cap_mb(&s), Some(3 << 10), "never below 3 GB");
        s.workset.as_mut().unwrap().untracked = 1;
        assert_eq!(suggested_vram_cap_mb(&s), None, "a meter that missed textures undercounts");
        s.workset.as_mut().unwrap().untracked = 0;
        s.workset.as_mut().unwrap().samples = 30;
        assert_eq!(suggested_vram_cap_mb(&s), None, "under a minute of play");
    }
    #[test]
    fn summarizes_the_working_set() {
        let mut b = header(1_000_000);
        rec(&mut b, 0, REC_DEVICE, 0xb000, 2, 0);
        // A loading second (no frames) is left out.
        rec_full(&mut b, 1, REC_WORKSET, 0, [9 << 20, 9 << 20, 5, 0], 900 << 20);
        rec_full(&mut b, 2, REC_WORKSET, 60, [100 << 10, 300 << 10, 400, 0], 1000 << 20);
        rec_full(&mut b, 3, REC_WORKSET, 60, [200 << 10, 350 << 10, 420, 3], 1200 << 20);
        let s = summarize(&b).unwrap();
        assert_eq!(s.tiled_tier, 2);
        let w = s.workset.unwrap();
        assert_eq!(
            w,
            WorkSet { samples: 2, textures_mb: 1200, used_avg_mb: 150, used_peak_mb: 200, used10_peak_mb: 350, untracked: 3 }
        );
        assert!(summarize(&header(1_000_000)).unwrap().workset.is_none());
    }

    #[test]
    fn summarizes_frames_and_resources() {
        let mut b = header(1_000_000);
        rec(&mut b, 0, REC_DEVICE, 0xb000, 0, 0);
        rec(&mut b, 10, REC_SWAPCHAIN, 1920, 1080, 0);
        rec(&mut b, 20, REC_TEXTURE2D, 256, 256, 1000);
        rec(&mut b, 30, REC_BUFFER, 64, 0, 64);
        rec(&mut b, 40, REC_FRAME, 0, 0, 0); // first frame: no time
        for i in 0..99u64 {
            rec(&mut b, 50 + i, REC_FRAME, 10_000, 0, 0);
        }
        rec(&mut b, 2_000_000, REC_FRAME, 50_000, 0, 0);
        rec(&mut b, 2_000_010, REC_END, 3, 0, 105);
        let s = summarize(&b).unwrap();
        assert_eq!(s.header.exe_path, "C:\\g\\game.exe");
        assert_eq!(s.header.pid, 1234);
        assert!(s.clean);
        assert_eq!(s.dropped, 3);
        assert_eq!(s.frames, 101);
        assert_eq!((s.textures, s.texture_bytes, s.buffers, s.buffer_bytes), (1, 1000, 1, 64));
        assert_eq!(s.vram_bytes(), 1064);
        assert_eq!(s.feature_level, 0xb000);
        assert_eq!(s.swapchain, Some((1920, 1080)));
        assert_eq!(s.frame_p50_ms, 10.0);
        assert_eq!(s.frame_p99_ms, 10.0);
        assert_eq!(s.frame_p999_ms, 50.0);
        assert_eq!(s.frame_max_ms, 50.0);
        assert_eq!(s.hitches, 1);
        assert!((s.duration_s - 2.0).abs() < 1e-9);
    }

    #[test]
    fn attributes_hitches_and_counts_layer_actions() {
        let mut b = header(1_000_000);
        rec(&mut b, 0, REC_FRAME, 0, 0, 0);
        for i in 0..98u64 {
            rec(&mut b, 1 + i, REC_FRAME, 10_000, 2_000, 0); // limited: waited 2 ms
        }
        // Hitch 1: the game streams 10 textures in that frame.
        for i in 0..10u64 {
            rec_flags(&mut b, 200 + i, REC_TEXTURE2D, FLAG_PRIORITY, 64, 64, 100);
        }
        rec(&mut b, 300, REC_FRAME, 40_000, 0, 0);
        // Hitch 2: only foreign creations (an overlay), so "other cause".
        for i in 0..10u64 {
            rec_flags(&mut b, 400 + i, REC_BUFFER, FLAG_FOREIGN, 64, 0, 64);
        }
        rec(&mut b, 500, REC_FRAME, 40_000, 0, 0);
        let s = summarize(&b).unwrap();
        assert_eq!((s.hitches, s.hitches_creating), (2, 1));
        assert_eq!((s.foreign, s.prioritized), (10, 10));
        assert_eq!(s.limited_frames, 98);
        assert!((s.limiter_wait_s - 0.196).abs() < 1e-9);
    }

    #[test]
    fn hitches_over_vram_budget() {
        let mut b = header(1_000_000);
        rec(&mut b, 0, REC_VRAM, 7000, 5000, 0);
        for i in 0..99u64 {
            rec(&mut b, 1 + i, REC_FRAME, 10_000, 0, 0);
        }
        rec(&mut b, 200, REC_VRAM, 6800, 7200, 0); // over budget from here
        rec(&mut b, 300, REC_FRAME, 40_000, 0, 0);
        let s = summarize(&b).unwrap();
        assert_eq!((s.hitches, s.hitches_over_budget, s.hitches_creating), (1, 1, 0));
        assert_eq!((s.vram_budget_mb, s.vram_peak_mb, s.vram_over_s), (6800, 7200, 1));
    }

    #[test]
    fn hitches_while_compiling_shaders() {
        let mut b = header(1_000_000);
        for i in 0..99u64 {
            rec(&mut b, 1 + i, REC_FRAME, 10_000, 0, 0);
        }
        // A cold game shader, a known one, and an overlay's (ignored).
        let mut shader = |qpc: u64, flags: u32, d: u32| {
            rec_flags(&mut b, qpc, REC_SHADER, flags, 4, 900, 0xABCD);
            let n = b.len();
            b[n - 16..n - 12].copy_from_slice(&2000u32.to_le_bytes()); // c = 2 ms
            b[n - 12..n - 8].copy_from_slice(&d.to_le_bytes());
        };
        shader(150, 0, SHADER_NEW);
        shader(151, 0, 0);
        shader(152, FLAG_FOREIGN, SHADER_NEW);
        rec(&mut b, 200, REC_FRAME, 40_000, 0, 0);
        let s = summarize(&b).unwrap();
        assert_eq!((s.hitches, s.hitches_shaders, s.hitches_creating), (1, 1, 0));
        assert_eq!((s.shaders, s.shaders_new), (2, 1));
        assert_eq!(s.shader_create_ms, 4.0);
    }

    #[test]
    fn cut_short_file_is_valid_up_to_last_whole_record() {
        let mut b = header(10_000_000);
        rec(&mut b, 0, REC_FRAME, 0, 0, 0);
        rec(&mut b, 1, REC_FRAME, 16_000, 0, 0);
        b.extend_from_slice(&[0xAB; 17]);
        let s = summarize(&b).unwrap();
        assert!(!s.clean);
        assert_eq!(s.frames, 2);
        assert_eq!(s.frame_p50_ms, 16.0);
    }

    #[test]
    fn rejects_bad_headers() {
        assert!(summarize(b"nope").is_err());
        let mut b = header(1);
        b[8] = 9;
        assert!(summarize(&b).is_err());
        assert!(summarize(&header(0)).is_err());
    }

    #[test]
    fn latest_picks_newest_for_exe_only() {
        let d = tempfile::tempdir().unwrap();
        let w = |n: &str, exe: &str| std::fs::write(d.path().join(n), header_for(1, exe)).unwrap();
        let pause = || std::thread::sleep(std::time::Duration::from_millis(20));
        w("game.exe_20261120-010000_1.tsess", "C:\\x\\game.exe");
        pause();
        w("GAME.EXE_20261120-020000_2.tsess", "c:/X/GAME.EXE");
        pause();
        w("game.exe_20261120-030000_3.tsess", "D:\\other\\game.exe"); // same name, other game
        pause();
        std::fs::write(d.path().join("game.exe_20261120-040000_4.tsess"), b"").unwrap(); // cut before header
        w("other.exe_20261120-050000_5.tsess", "C:\\x\\other.exe");
        w("game.exe.log", "C:\\x\\game.exe");
        let p = latest_in(d.path(), Path::new("C:\\x\\Game.exe")).unwrap();
        assert_eq!(p.file_name().unwrap(), "GAME.EXE_20261120-020000_2.tsess");
        let p = latest_in(d.path(), Path::new("D:\\other\\game.exe")).unwrap();
        assert_eq!(p.file_name().unwrap(), "game.exe_20261120-030000_3.tsess");
        assert!(latest_in(d.path(), Path::new("none.exe")).is_none());
    }

    #[test]
    fn safe_mode_key_normalizes_and_separates() {
        let k = safe_mode_key(std::ffi::OsStr::new(r"C:\Games\A\game.exe"));
        assert_eq!(k, safe_mode_key(std::ffi::OsStr::new("c:/games/a/GAME.EXE")));
        assert_eq!(k, safe_mode_key(std::ffi::OsStr::new(r"\\?\C:\Games\A\game.exe")));
        assert_ne!(k, safe_mode_key(std::ffi::OsStr::new(r"C:\Games\B\game.exe")));
        assert_eq!(safe_mode_key(std::ffi::OsStr::new("")), 0xcbf2_9ce4_8422_2325);
    }
}
