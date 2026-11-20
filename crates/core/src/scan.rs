// SPDX-License-Identifier: GPL-3.0-or-later
//! S1: exhaustive bytecode search over a game's install files. Finds DXBC
//! containers stored raw (exe, DLLs, loose files, uncompressed archives) and
//! inside zlib, zstd and LZ4-frame streams, then keeps the ones the D3D11
//! warm-up can create. Read-only: never writes into the game folder.
//!
//! Not covered yet: Oodle (needs the game's own codec DLL), Brotli and raw
//! LZ4 blocks (no magic to find them by), encrypted archives (need the key).

use std::collections::HashMap;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use rayon::prelude::*;

use crate::shaders::{Shader, FLAG_DXBC, FLAG_DXIL, STAGE_CS, STAGE_DS, STAGE_GS, STAGE_HS, STAGE_PS, STAGE_VS};

/// Largest decompressed stream searched; bigger ones are cut (their start is still searched).
const STREAM_CAP: u64 = 64 << 20;
/// Streams found inside decompressed streams are searched to this depth.
const MAX_DEPTH: u32 = 2;
const MIN_FILE: u64 = 32;
/// Search window: candidates of one window are found while it is in cache.
const CHUNK: usize = 8 << 20;

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct ScanStats {
    pub files: u64,
    pub bytes: u64,
    /// Containers found (before dedupe), by where they were.
    pub raw: u64,
    pub zlib: u64,
    pub zstd: u64,
    pub lz4: u64,
    /// Unique containers the D3D11 warm-up can create.
    pub usable: u64,
    /// Unique containers it cannot (DXIL, SM 5.1+, libraries, mesh shaders).
    pub skipped: u64,
    /// D3D11 containers whose checksum does not match their contents: the
    /// runtime rejects them as they are, so the game never creates them so.
    pub invalid: u64,
}

impl ScanStats {
    fn add(&mut self, o: &ScanStats) {
        self.files += o.files;
        self.bytes += o.bytes;
        self.raw += o.raw;
        self.zlib += o.zlib;
        self.zstd += o.zstd;
        self.lz4 += o.lz4;
        self.invalid += o.invalid;
    }
}

/// A validated container at the start of `b`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Container {
    pub len: usize,
    pub hash: u64,
    pub stage: u16,
    pub flags: u16,
    /// Shader model major/minor; DXIL containers report their own.
    pub major: u8,
    pub minor: u8,
}

impl Container {
    /// What D3D11 can create: DXBC up to shader model 5.0.
    pub fn d3d11(&self) -> bool {
        self.flags & FLAG_DXIL == 0 && (self.major < 5 || (self.major == 5 && self.minor == 0))
    }
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

fn fnv(b: &[u8]) -> u64 {
    b.iter().fold(14695981039346656037u64, |h, &x| (h ^ x as u64).wrapping_mul(1099511628211))
}

/// Same hash as the layer (src/dll/shaders.cpp identify): the first 8 bytes
/// of the container checksum, FNV-1a when the container is unsigned.
pub fn hash_of(code: &[u8]) -> u64 {
    let h = u64::from_le_bytes(code[4..12].try_into().unwrap());
    let h = if h == 0 { fnv(code) } else { h };
    h.max(1)
}

/// The DXBC container checksum (bytes 4..20): MD5 rounds over bytes 20.. with
/// the container's own padding (bit count first in the last block, a second
/// count word at its end).
pub fn checksum(code: &[u8]) -> [u8; 16] {
    const S: [u32; 16] = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
    const K: [u32; 64] = [
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    ];
    fn block(st: &mut [u32; 4], b: &[u8]) {
        let x = |i: usize| u32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap());
        let [mut a, mut bb, mut c, mut d] = *st;
        for i in 0..64 {
            let (f, g) = match i / 16 {
                0 => ((bb & c) | (!bb & d), i),
                1 => ((d & bb) | (!d & c), (5 * i + 1) % 16),
                2 => (bb ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (bb | !d), (7 * i) % 16),
            };
            let f = f.wrapping_add(a).wrapping_add(K[i]).wrapping_add(x(g));
            a = d;
            d = c;
            c = bb;
            bb = bb.wrapping_add(f.rotate_left(S[(i / 16) * 4 + i % 4]));
        }
        for (s, v) in st.iter_mut().zip([a, bb, c, d]) {
            *s = s.wrapping_add(v);
        }
    }
    let data = &code[20..];
    let bits = (data.len() as u32).wrapping_mul(8);
    let mut st = [0x6745_2301u32, 0xefcd_ab89, 0x98ba_dcfe, 0x1032_5476];
    let full = data.len() - data.len() % 64;
    for c in data[..full].chunks_exact(64) {
        block(&mut st, c);
    }
    let left = &data[full..];
    let mut last = [0u8; 64];
    if left.len() >= 56 {
        last[..left.len()].copy_from_slice(left);
        last[left.len()] = 0x80;
        block(&mut st, &last);
        last = [0; 64];
    } else {
        last[4..4 + left.len()].copy_from_slice(left);
        last[4 + left.len()] = 0x80;
    }
    last[..4].copy_from_slice(&bits.to_le_bytes());
    last[60..].copy_from_slice(&((bits >> 2) | 1).to_le_bytes());
    block(&mut st, &last);
    let mut out = [0u8; 16];
    for (o, v) in out.chunks_exact_mut(4).zip(st) {
        o.copy_from_slice(&v.to_le_bytes());
    }
    out
}

/// Validates a DXBC container at the start of `b`: header, part table,
/// every part in bounds, and a shader program part (SHDR/SHEX or DXIL)
/// whose version token names a known stage.
pub fn container_at(b: &[u8]) -> Option<Container> {
    if b.len() < 32 || &b[..4] != b"DXBC" || u32_at(b, 20) != 1 {
        return None;
    }
    let len = u32_at(b, 24) as usize;
    let parts = u32_at(b, 28) as usize;
    if parts == 0 || parts > 64 || len > b.len() || len < 32 + 4 * parts {
        return None;
    }
    let (mut program, mut dxil) = (None, false);
    for i in 0..parts {
        let off = u32_at(b, 32 + 4 * i) as usize;
        if off < 32 + 4 * parts || off + 8 > len {
            return None;
        }
        let size = u32_at(b, off + 4) as usize;
        if off + 8 + size > len {
            return None;
        }
        let tag = &b[off..off + 4];
        let is_dxil = tag == b"DXIL";
        if (tag == b"SHDR" || tag == b"SHEX" || is_dxil) && size >= 8 && (program.is_none() || is_dxil) {
            program = Some(u32_at(b, off + 8));
            dxil |= is_dxil;
        }
    }
    let token = program?;
    // Version token: minor bits 0-3, major 4-7, program type 16-31.
    let stage = match token >> 16 {
        0 => STAGE_PS,
        1 => STAGE_VS,
        2 => STAGE_GS,
        3 => STAGE_HS,
        4 => STAGE_DS,
        5 => STAGE_CS,
        _ => return None, // libraries, mesh/amplification, ray tracing
    };
    let (major, minor) = (((token >> 4) & 0xF) as u8, (token & 0xF) as u8);
    if !(4..=6).contains(&major) {
        return None;
    }
    let code = &b[..len];
    let flags = FLAG_DXBC | if dxil { FLAG_DXIL } else { 0 };
    Some(Container { len, hash: hash_of(code), stage, flags, major, minor })
}

#[derive(Default)]
struct Found {
    shaders: HashMap<u64, Shader>,
    skipped: HashMap<u64, ()>,
    stats: ScanStats,
}

impl Found {
    fn merge(mut self, o: Found) -> Found {
        self.stats.add(&o.stats);
        for (k, v) in o.shaders {
            self.shaders.entry(k).or_insert(v);
        }
        self.skipped.extend(o.skipped);
        self
    }

    fn record(&mut self, c: &Container, code: &[u8]) -> bool {
        if !c.d3d11() {
            return self.skipped.insert(c.hash, ()).is_none();
        }
        if self.shaders.contains_key(&c.hash) {
            return false;
        }
        let fresh = checksum(code) == code[4..20];
        if !fresh {
            self.stats.invalid += 1;
        } else {
            self.shaders.insert(c.hash, Shader { hash: c.hash, stage: c.stage, flags: c.flags, code: code.to_vec() });
        }
        fresh
    }

    /// Every container in `b`, raw or in a compressed stream that starts in
    /// it, walked in CHUNK windows so a large file is read from disk once.
    /// Returns how many new unique containers were found.
    fn search(&mut self, b: &[u8], depth: u32) -> u64 {
        const DXBC: u8 = 3;
        let zstd = memchr::memmem::Finder::new(&[0x28, 0xB5, 0x2F, 0xFD]);
        let lz4 = memchr::memmem::Finder::new(&[0x04, 0x22, 0x4D, 0x18]);
        let dxbc = memchr::memmem::Finder::new(b"DXBC");
        let (mut found, mut skip_to) = (0u64, 0usize);
        let mut cand: Vec<(usize, u8)> = Vec::new();
        let mut out = Vec::new();
        let mut start = 0;
        while start < b.len() {
            let end = (start + CHUNK).min(b.len());
            let w = &b[start..(end + 3).min(b.len())]; // patterns may start up to the chunk's last byte
            cand.clear();
            cand.extend(dxbc.find_iter(w).map(|i| (start + i, DXBC)));
            if depth < MAX_DEPTH {
                cand.extend(zstd.find_iter(w).map(|i| (start + i, 1)));
                cand.extend(lz4.find_iter(w).map(|i| (start + i, 2)));
                cand.extend(
                    memchr::memchr_iter(0x78, &w[..end - start])
                        .filter(|&i| i + 2 <= w.len() && is_zlib_header(&w[i..]))
                        .map(|i| (start + i, 0)),
                );
            }
            cand.sort_unstable();
            for &(p, kind) in &cand {
                if p < skip_to || p >= end {
                    continue;
                }
                let rest = &b[p..];
                if kind == DXBC {
                    if let Some(c) = container_at(rest) {
                        if self.record(&c, &rest[..c.len]) {
                            found += 1;
                            if depth == 0 {
                                self.stats.raw += 1;
                            }
                        }
                        skip_to = p + c.len;
                    }
                    continue;
                }
                out.clear();
                let consumed = match kind {
                    0 => inflate(rest, &mut out),
                    1 => read_capped(ruzstd::decoding::StreamingDecoder::new(rest).ok(), &mut out).then_some(None),
                    _ => read_capped(Some(lz4_flex::frame::FrameDecoder::new(rest)), &mut out).then_some(None),
                };
                let Some(consumed) = consumed else { continue };
                if out.len() < 32 {
                    continue;
                }
                let inner = std::mem::take(&mut out);
                let n = self.search(&inner, depth + 1);
                out = inner;
                found += n;
                match kind {
                    0 => self.stats.zlib += n,
                    1 => self.stats.zstd += n,
                    _ => self.stats.lz4 += n,
                }
                // Only a stream that ended cleanly is skipped: a false header
                // may decode some garbage over the start of a real stream.
                if let Some(c) = consumed {
                    skip_to = p + c;
                }
            }
            start = end;
        }
        found
    }
}

/// zlib header: deflate, 32 KiB window or less, check bits, no preset dictionary.
fn is_zlib_header(b: &[u8]) -> bool {
    let (cmf, flg) = (b[0], b[1]);
    cmf & 0x0F == 8 && cmf >> 4 <= 7 && (u16::from(cmf) << 8 | u16::from(flg)) % 31 == 0 && flg & 0x20 == 0
}

/// Inflates up to STREAM_CAP. None when nothing decoded; otherwise
/// Some(input consumed) for a stream that ended cleanly, Some(None) for one
/// that broke off or hit the cap.
fn inflate(b: &[u8], out: &mut Vec<u8>) -> Option<Option<usize>> {
    let mut d = flate2::Decompress::new(true);
    let mut buf = [0u8; 64 << 10];
    loop {
        let (i0, o0) = (d.total_in() as usize, d.total_out());
        let r = d.decompress(&b[i0..], &mut buf, flate2::FlushDecompress::None);
        let produced = (d.total_out() - o0) as usize;
        out.extend_from_slice(&buf[..produced]);
        match r {
            Ok(flate2::Status::StreamEnd) => return Some(Some(d.total_in() as usize)),
            Ok(_) if produced == 0 && d.total_in() as usize == i0 => break, // input ended
            Ok(_) if d.total_out() >= STREAM_CAP => break,
            Ok(_) => {}
            Err(_) => break,
        }
    }
    (!out.is_empty()).then_some(None)
}

/// Reads a decoder to its end or STREAM_CAP; false when it gave nothing.
fn read_capped<R: Read>(r: Option<R>, out: &mut Vec<u8>) -> bool {
    let Some(r) = r else { return false };
    let _ = r.take(STREAM_CAP).read_to_end(out); // a corrupt tail still leaves what decoded
    !out.is_empty()
}

fn scan_file(path: &Path) -> Found {
    let mut f = Found::default();
    let Ok(file) = fs::File::open(path) else { return f };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    if len < MIN_FILE {
        return f;
    }
    // SAFETY: read-only map of a file we do not write; a game update while
    // scanning could change it, which at worst yields garbage we validate.
    let Ok(map) = (unsafe { memmap2::Mmap::map(&file) }) else { return f };
    f.stats.files = 1;
    f.stats.bytes = len;
    f.search(&map, 0);
    f
}

/// Every regular file under `dir`.
pub fn files(dir: &Path) -> Vec<PathBuf> {
    walkdir::WalkDir::new(dir)
        .into_iter()
        .filter_map(Result::ok)
        .filter(|e| e.file_type().is_file())
        .map(|e| e.into_path())
        .collect()
}

/// Scans `dir`; returns the D3D11-creatable shaders (sorted by hash) and stats.
pub fn scan_dir(dir: &Path) -> (Vec<Shader>, ScanStats) {
    let found = files(dir).par_iter().map(|p| scan_file(p)).reduce(Found::default, Found::merge);
    let mut stats = found.stats;
    let mut shaders: Vec<Shader> = found.shaders.into_values().collect();
    shaders.sort_unstable_by_key(|s| s.hash);
    stats.usable = shaders.len() as u64;
    stats.skipped = found.skipped.len() as u64;
    (shaders, stats)
}

/// `<data>\shaders\<exe>-<key>.scan`: the scan store's base path (.tshd/.tshi).
pub fn scan_base(exe: &Path) -> Option<PathBuf> {
    let mut s = crate::warm::store_base(exe)?.into_os_string();
    s.push(".scan");
    Some(PathBuf::from(s))
}

/// Files that change without the game changing: Tessera's own (settings,
/// the proxy DLL and the one it chains to) and what games write next to
/// their files (logs, configs, crash dumps).
fn volatile(p: &Path) -> bool {
    let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("").to_ascii_lowercase();
    let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    matches!(name.as_str(), "d3d11.dll" | "d3d12.dll" | "dxgi.dll")
        || name.ends_with(".next.dll")
        || matches!(ext.as_str(), "ini" | "log" | "txt" | "dmp" | "cfg" | "tmp")
}

/// Identifies an install's contents cheaply: file count, total size, newest
/// modification time. A game update changes it.
fn install_key(root: &Path) -> String {
    let (mut n, mut bytes, mut newest) = (0u64, 0u64, 0u64);
    for p in files(root) {
        if volatile(&p) {
            continue;
        }
        let Ok(m) = fs::metadata(&p) else { continue };
        n += 1;
        bytes += m.len();
        let t = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok());
        newest = newest.max(t.map_or(0, |d| d.as_secs()));
    }
    format!("tessera-scan 1 files {n} bytes {bytes} newest {newest}")
}

fn key_path(base: &Path) -> PathBuf {
    let mut s = base.as_os_str().to_owned();
    s.push(".key");
    PathBuf::from(s)
}

/// True when the scan store matches the install as it is now.
pub fn up_to_date(exe: &Path, root: &Path) -> bool {
    let Some(base) = scan_base(exe) else { return false };
    fs::read_to_string(key_path(&base)).is_ok_and(|k| k == install_key(root))
}

/// Scans the install folder of `exe` (`root`) and writes what the layer has
/// not stored yet to the scan store, which the warm-up compiles too.
pub fn scan_game(exe: &Path, root: &Path) -> io::Result<ScanStats> {
    let base = scan_base(exe).ok_or_else(|| io::Error::other("caminho do jogo inválido"))?;
    let key = install_key(root);
    let (mut shaders, stats) = scan_dir(root);
    let stored: std::collections::HashSet<u64> = crate::shaders::read(exe).iter().map(|s| s.hash).collect();
    shaders.retain(|s| !stored.contains(&s.hash));
    crate::shaders::write_base(&base, &shaders)?;
    fs::write(key_path(&base), key)?;
    Ok(stats)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal container: one SHEX part with the given version token.
    pub(crate) fn container(token: u32, checksum: u64) -> Vec<u8> {
        let mut b = b"DXBC".to_vec();
        b.extend_from_slice(&checksum.to_le_bytes());
        b.extend_from_slice(&[0; 8]);
        let total = 32 + 4 + 8 + 8;
        for v in [1u32, total, 1, 36] {
            b.extend_from_slice(&v.to_le_bytes());
        }
        b.extend_from_slice(b"SHEX");
        b.extend_from_slice(&8u32.to_le_bytes());
        b.extend_from_slice(&token.to_le_bytes());
        b.extend_from_slice(&2u32.to_le_bytes());
        b
    }

    /// The container with its real checksum, as a compiler writes it.
    fn signed(token: u32) -> Vec<u8> {
        let mut b = container(token, 0);
        let sum = checksum(&b);
        b[4..20].copy_from_slice(&sum);
        b
    }

    #[test]
    fn checksum_matches_the_compiler() {
        // Reference value from an independent implementation that matched
        // the checksums of 673 compiler-signed shaders.
        let ps = signed(0x0000_0050);
        assert_eq!(ps[4..20], [
            0xb7, 0xec, 0x72, 0xd5, 0x2a, 0x5d, 0x5a, 0x80, 0xcf, 0x35, 0x46, 0xaf, 0x03, 0x23, 0x99, 0xfa
        ]);
        let mut bad = ps.clone();
        bad[50] ^= 1;
        assert_ne!(checksum(&bad), bad[4..20]);
    }

    #[test]
    fn validates_containers_and_reads_the_stage() {
        let ps50 = container(0x0000_0050, 7);
        let c = container_at(&ps50).unwrap();
        assert_eq!((c.len, c.hash, c.stage, c.major, c.minor, c.d3d11()), (52, 7, STAGE_PS, 5, 0, true));
        let cs51 = container(0x0005_0051, 8);
        assert!(!container_at(&cs51).unwrap().d3d11());
        assert_eq!(container_at(&container(0x0006_0060, 9)), None); // library
        let mut cut = ps50.clone();
        cut.truncate(40);
        assert_eq!(container_at(&cut), None);
    }

    #[test]
    fn finds_raw_and_compressed_containers_once() {
        use std::io::Write;
        let (vs, ps) = (signed(0x0001_0050), signed(0x0000_0040));
        let bad = container(0x0001_0041, 13);
        let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
        z.write_all(&[vec![0u8; 100], ps.clone(), vs.clone()].concat()).unwrap();
        let blob = [b"junk".to_vec(), vs.clone(), vec![0x78; 9], z.finish().unwrap(), b"DXBCnope".to_vec(), bad].concat();
        let t = tempfile::tempdir().unwrap();
        fs::write(t.path().join("a.pak"), &blob).unwrap();
        let (shaders, stats) = scan_dir(t.path());
        let mut want = [(hash_of(&vs), STAGE_VS), (hash_of(&ps), STAGE_PS)];
        want.sort();
        assert_eq!(shaders.iter().map(|s| (s.hash, s.stage)).collect::<Vec<_>>(), want);
        assert!(shaders.iter().any(|s| s.code == vs));
        assert_eq!((stats.files, stats.raw, stats.zlib, stats.usable, stats.invalid), (1, 1, 1, 2, 1));
    }

    #[test]
    fn finds_containers_in_zstd_and_lz4_frames() {
        use std::io::Write;
        let (gs, cs) = (signed(0x0002_0050), signed(0x0005_0050));
        let zs = ruzstd::encoding::compress_to_vec(&gs[..], ruzstd::encoding::CompressionLevel::Fastest);
        let mut lz = lz4_flex::frame::FrameEncoder::new(Vec::new());
        lz.write_all(&cs).unwrap();
        let blob = [vec![1u8; 13], zs, vec![2u8; 7], lz.finish().unwrap()].concat();
        let t = tempfile::tempdir().unwrap();
        fs::write(t.path().join("b.bin"), &blob).unwrap();
        let (shaders, stats) = scan_dir(t.path());
        let mut want = [(hash_of(&gs), STAGE_GS), (hash_of(&cs), STAGE_CS)];
        want.sort();
        assert_eq!(shaders.iter().map(|s| (s.hash, s.stage)).collect::<Vec<_>>(), want);
        assert_eq!((stats.raw, stats.zstd, stats.lz4), (0, 1, 1));
    }
}
