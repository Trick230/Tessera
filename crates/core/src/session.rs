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
pub const REC_END: u16 = 0xFFFF;

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
    pub textures: u64,
    pub texture_bytes: u64,
    pub buffers: u64,
    pub buffer_bytes: u64,
    pub feature_level: u32,
    pub swapchain: Option<(u32, u32)>,
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
            Error::Format(m) => write!(f, "sessão inválida: {m}"),
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
        return Err(Error::Format("versão"));
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
        return Err(Error::Format("frequência"));
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
    let mut frame_us: Vec<u32> = Vec::with_capacity((b.len() - hs) / rs);
    let (mut first, mut last) = (u64::MAX, 0u64);
    for r in records(b, hs, rs) {
        if r.kind != REC_END {
            first = first.min(r.qpc);
            last = last.max(r.qpc);
        }
        match r.kind {
            REC_FRAME => {
                s.frames += 1;
                if r.a != 0 && r.a != u32::MAX {
                    frame_us.push(r.a);
                }
            }
            REC_TEXTURE2D => {
                s.textures += 1;
                s.texture_bytes = s.texture_bytes.saturating_add(r.bytes);
            }
            REC_BUFFER => {
                s.buffers += 1;
                s.buffer_bytes = s.buffer_bytes.saturating_add(r.bytes);
            }
            REC_DEVICE => s.feature_level = r.a,
            REC_SWAPCHAIN => s.swapchain = Some((r.a, r.b)),
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
    if !frame_us.is_empty() {
        let sum: u64 = frame_us.iter().map(|&v| v as u64).sum();
        s.frame_avg_ms = sum as f64 / frame_us.len() as f64 / 1000.0;
        frame_us.sort_unstable();
        let ms = |v: u32| v as f64 / 1000.0;
        let p50 = percentile(&frame_us, 50.0);
        s.frame_p50_ms = ms(p50);
        s.frame_p99_ms = ms(percentile(&frame_us, 99.0));
        s.frame_p999_ms = ms(percentile(&frame_us, 99.9));
        s.frame_max_ms = ms(*frame_us.last().unwrap());
        s.hitches = frame_us.iter().filter(|&&v| v > p50.saturating_mul(2)).count() as u64;
    }
    Ok(s)
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

fn safe_mode_file(exe: &Path) -> Option<PathBuf> {
    Some(crate::paths::data_dir().join("safemode").join(format!("{}.count", exe_file_name(exe)?)))
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
        b.extend_from_slice(&qpc.to_le_bytes());
        b.extend_from_slice(&kind.to_le_bytes());
        b.extend_from_slice(&0u16.to_le_bytes());
        b.extend_from_slice(&0u32.to_le_bytes());
        for v in [a, bb, 0, 0] {
            b.extend_from_slice(&v.to_le_bytes());
        }
        b.extend_from_slice(&bytes.to_le_bytes());
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
}
