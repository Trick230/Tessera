// SPDX-License-Identifier: GPL-3.0-or-later
//! S1: shader search over a game's install files. Finds DXBC
//! containers stored raw (exe, DLLs, loose files, uncompressed archives) and
//! inside zlib, zstd and LZ4-frame streams and Oodle blocks (with the game's
//! own codec DLL, when it ships one), then keeps the ones the D3D11 warm-up
//! can create. Read-only: never writes into the game folder.
//!
//! Recognised container layouts provide the compressed entry's exact bounds
//! and decoded size. Loose streams still need a recognisable header.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{self, Cursor, Read};
use std::path::{Path, PathBuf};

use rayon::prelude::*;

use crate::oodle::{self, Oodle};
use crate::unreal_pak::{self, PakKey};
use crate::unreal_shader::{self, Payload};
use crate::shaders::{Shader, FLAG_DXBC, FLAG_DXIL, STAGE_CS, STAGE_DS, STAGE_GS, STAGE_HS, STAGE_PS, STAGE_VS};

/// Largest decompressed stream searched; bigger ones are cut (their start is still searched).
const STREAM_CAP: u64 = 64 << 20;
/// Sekiro's main shader bundle expands to more than 320 MiB.
const DCX_CAP: usize = 512 << 20;
static LARGE_DCX: std::sync::Mutex<()> = std::sync::Mutex::new(());
/// Streams found inside decompressed streams are searched to this depth.
const MAX_DEPTH: u32 = 2;
const MIN_FILE: u64 = 32;
/// Files scanned at once (see scan_dir).
const SCAN_THREADS: usize = 4;
/// Search window: candidates of one window are found while it is in cache.
const CHUNK: usize = 8 << 20;

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct ScanStats {
    pub files: u64,
    pub bytes: u64,
    /// Containers found (before dedupe), by where they were.
    pub raw: u64,
    pub glsl: u64,
    pub spirv: u64,
    pub cpk_entries: u64,
    pub dcx_entries: u64,
    pub zip_entries: u64,
    pub apk_entries: u64,
    pub unity_entries: u64,
    pub unreadable_unity_entries: u64,
    pub zlib: u64,
    pub gzip: u64,
    pub zstd: u64,
    pub lz4: u64,
    pub oodle: u64,
    /// Unique containers the D3D11 warm-up can create.
    pub usable: u64,
    /// Unique containers it cannot (DXIL, SM 5.1+, libraries, mesh shaders).
    pub skipped: u64,
    pub dxil: u64,
    /// D3D11 containers whose checksum does not match their contents: the
    /// runtime rejects them as they are, so the game never creates them so.
    pub invalid: u64,
    pub unreal_packages: u64,
    pub unreadable_unreal_packages: u64,
    pub locked_unreal_paks: u64,
    pub unreal_libraries_unreadable: u64,
    pub unreal_shaders_expected: u64,
    pub unreal_shaders_decoded: u64,
    pub unreal_shaders_dxbc: u64,
    pub unreal_shaders_dxil: u64,
    pub unreal_shaders_spirv: u64,
}

impl ScanStats {
    fn add(&mut self, o: &ScanStats) {
        self.files += o.files;
        self.bytes += o.bytes;
        self.raw += o.raw;
        self.cpk_entries += o.cpk_entries;
        self.dcx_entries += o.dcx_entries;
        self.zip_entries += o.zip_entries;
        self.apk_entries += o.apk_entries;
        self.unity_entries += o.unity_entries;
        self.unreadable_unity_entries += o.unreadable_unity_entries;
        self.zlib += o.zlib;
        self.gzip += o.gzip;
        self.zstd += o.zstd;
        self.lz4 += o.lz4;
        self.oodle += o.oodle;
        self.invalid += o.invalid;
        self.unreal_packages += o.unreal_packages;
        self.unreadable_unreal_packages += o.unreadable_unreal_packages;
        self.locked_unreal_paks += o.locked_unreal_paks;
        self.unreal_libraries_unreadable += o.unreal_libraries_unreadable;
        self.unreal_shaders_expected += o.unreal_shaders_expected;
        self.unreal_shaders_decoded += o.unreal_shaders_decoded;
        self.unreal_shaders_dxbc += o.unreal_shaders_dxbc;
        self.unreal_shaders_dxil += o.unreal_shaders_dxil;
        self.unreal_shaders_spirv += o.unreal_shaders_spirv;
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
    #[inline(always)]
    fn round<const R: usize>(state: [u32; 4], words: &[u32; 16]) -> [u32; 4] {
        let [mut a, mut bb, mut c, mut d] = state;
        for i in 0..16 {
            let (f, g) = match R {
                0 => ((bb & c) | (!bb & d), i),
                1 => ((d & bb) | (!d & c), (5 * i + 1) % 16),
                2 => (bb ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (bb | !d), (7 * i) % 16),
            };
            let f = f.wrapping_add(a).wrapping_add(K[R * 16 + i]).wrapping_add(words[g]);
            a = d;
            d = c;
            c = bb;
            bb = bb.wrapping_add(f.rotate_left(S[R * 4 + i % 4]));
        }
        [a, bb, c, d]
    }
    fn block(st: &mut [u32; 4], b: &[u8; 64]) {
        let words = std::array::from_fn(|i| u32::from_le_bytes(b[i * 4..i * 4 + 4].try_into().unwrap()));
        let state = round::<0>(*st, &words);
        let state = round::<1>(state, &words);
        let state = round::<2>(state, &words);
        let state = round::<3>(state, &words);
        for (s, v) in st.iter_mut().zip(state) {
            *s = s.wrapping_add(v);
        }
    }
    let data = &code[20..];
    let bits = (data.len() as u32).wrapping_mul(8);
    let mut st = [0x6745_2301u32, 0xefcd_ab89, 0x98ba_dcfe, 0x1032_5476];
    let full = data.len() - data.len() % 64;
    for c in data[..full].as_chunks::<64>().0 {
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
        _ if dxil => u16::MAX, // discover libraries, mesh and ray tracing without warming them as D3D11
        _ => return None,
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
    skipped: HashMap<u64, bool>,
    glsl: HashSet<u128>,
    spirv: HashSet<u128>,
    stats: ScanStats,
    oodle: Option<Oodle>,
}

impl Found {
    fn unreal_payload(&mut self, kind: Payload, data: &[u8]) {
        if matches!(kind, Payload::Shader) {
            self.record_unreal_shader(data);
            return;
        }
        if matches!(kind, Payload::Library) || data.starts_with(&2u32.to_le_bytes()) {
            let before = self.stats.unreal_shaders_decoded;
            if let Some(expected) = unreal_shader::scan_file(data, self.oodle, |code| self.record_unreal_shader(code)) {
                self.stats.unreal_shaders_expected += expected;
                if self.stats.unreal_shaders_decoded - before < expected {
                    self.search_unreal_package(data);
                }
                return;
            }
            self.stats.unreal_libraries_unreadable += u64::from(matches!(kind, Payload::Library));
        }
        self.record_spirv(data);
        self.search_unreal_package(data);
    }

    fn record_unreal_shader(&mut self, data: &[u8]) {
        self.stats.unreal_shaders_decoded += 1;
        for start in memchr::memmem::find_iter(data, b"DXBC") {
            let Some(c) = container_at(&data[start..]) else { continue };
            let code = &data[start..start + c.len];
            let valid = self.record(&c, code) || self.shaders.contains_key(&c.hash) || !c.d3d11();
            if !valid { continue; }
            if c.flags & FLAG_DXIL != 0 {
                self.stats.unreal_shaders_dxil += 1;
            } else {
                self.stats.unreal_shaders_dxbc += 1;
            }
            return;
        }
        if let Some(hash) = unreal_spirv_hash(data) {
            self.spirv.insert(hash);
            self.stats.unreal_shaders_spirv += 1;
        }
    }

    // Legacy Unreal FShaderResource layout adapted from SCSKiller's
    // InlineShaders 'Z' reader (05205ef1f6c3914b1f1245d133ebc710e2d30b52):
    // the serialized length immediately before the zlib stream bounds it.
    fn search_unreal_package(&mut self, data: &[u8]) {
        self.search(data, MAX_DEPTH);
        let mut decoded = Vec::new();
        for at in memchr::memchr_iter(0x78, data) {
            if at < 4 || at + 2 > data.len() || !is_zlib_header(&data[at..]) { continue; }
            let size = u32_at(data, at - 4) as usize;
            let Some(stream) = (size >= 3 && size <= data.len() - at).then(|| &data[at..at + size]) else {
                continue;
            };
            decoded.clear();
            if inflate(stream, &mut decoded) == Some(Some(size)) {
                self.stats.zlib += self.search(&decoded, MAX_DEPTH);
            }
        }
    }

    fn merge(mut self, o: Found) -> Found {
        self.stats.add(&o.stats);
        for (k, v) in o.shaders {
            self.shaders.entry(k).or_insert(v);
        }
        self.skipped.extend(o.skipped);
        self.glsl.extend(o.glsl);
        self.spirv.extend(o.spirv);
        self
    }

    fn search_zip(&mut self, data: &[u8]) {
        let Ok(mut archive) = zip::ZipArchive::new(Cursor::new(data)) else { return };
        let mut out = Vec::new();
        for i in 0..archive.len() {
            let Ok(entry) = archive.by_index(i) else { continue };
            let size = entry.size();
            let spirv = is_spv_path(Path::new(entry.name()));
            let stored = entry.compression() == zip::CompressionMethod::Stored;
            if !entry.is_file() || (stored && !spirv) || !(MIN_FILE..=STREAM_CAP).contains(&size) {
                continue;
            }
            out.clear();
            if entry.take(size + 1).read_to_end(&mut out).is_ok() && out.len() as u64 == size {
                if spirv {
                    self.record_spirv(&out);
                }
                if !stored {
                    self.stats.zip_entries += 1;
                    self.search(&out, 1);
                }
            }
        }
    }

    fn search_cpk(&mut self, data: &[u8]) {
        let Some(entries) = crate::cri::shaders(data) else { return };
        let mut out = Vec::new();
        for (bytes, size) in entries {
            let decoded = if bytes.len() == size {
                bytes
            } else {
                if crate::cri::decompress(bytes, size, &mut out).is_none() { continue; }
                &out
            };
            self.stats.cpk_entries += 1;
            self.search(decoded, 1);
        }
    }

    fn search_apk(&mut self, data: &[u8]) -> bool {
        let Some(entries) = crate::artdink::entries(data) else { return false };
        let mut complete = !entries.is_empty();
        let mut out = Vec::new();
        for entry in entries {
            if !entry.compressed {
                self.stats.apk_entries += 1;
                self.search(entry.bytes, 1);
            } else if entry.unpacked as u64 <= STREAM_CAP {
                out.clear();
                let decoded = flate2::read::ZlibDecoder::new(entry.bytes)
                    .take(entry.unpacked as u64 + 1).read_to_end(&mut out);
                if decoded.is_ok() && out.len() == entry.unpacked {
                    self.stats.apk_entries += 1;
                    self.search(&out, 1);
                } else {
                    complete = false;
                }
            } else {
                complete = false;
            }
        }
        complete
    }

    fn search_dcx(&mut self, data: &[u8]) {
        let mut out = Vec::new();
        for at in memchr::memmem::find_iter(data, b"DCX\0") {
            let bytes = &data[at..];
            // DFLT streams are already recognised by the zlib search.
            let Some((payload, size)) = dcx_kraken(bytes) else { continue };
            if size as u64 > STREAM_CAP {
                // Keep at most one large decompression buffer across the scan's workers.
                let Ok(_guard) = LARGE_DCX.lock() else { continue };
                self.search_dcx_payload(payload, size, &mut Vec::new());
            } else {
                self.search_dcx_payload(payload, size, &mut out);
            }
        }
    }

    fn search_dcx_payload(&mut self, payload: &[u8], size: usize, out: &mut Vec<u8>) {
        out.clear();
        if self.oodle.is_some_and(|oodle| oodle.decode_exact(payload, size, out)) {
            self.stats.dcx_entries += 1;
            self.search(out, MAX_DEPTH);
        }
    }

    fn search_glsl(&mut self, data: &[u8]) {
        if memchr::memmem::find(data, b"glShaderSource").is_none() { return; }
        for at in memchr::memmem::find_iter(data, b"#version ") {
            let rest = &data[at..data.len().min(at.saturating_add(64 << 10))];
            let Some(end) = memchr::memchr(0, rest) else { continue };
            let source = &rest[..end];
            if !source.iter().all(|b| b.is_ascii_graphic() || b.is_ascii_whitespace()) ||
                memchr::memmem::find(source, b"void main").is_none() ||
                source.iter().rev().find(|b| !b.is_ascii_whitespace()) != Some(&b'}') {
                continue;
            }
            self.glsl.insert(xxhash_rust::xxh3::xxh3_128(source));
        }
    }

    fn record_spirv(&mut self, bytes: &[u8]) {
        if let Some(hash) = spirv_hash(bytes) {
            self.spirv.insert(hash);
        }
    }

    fn record(&mut self, c: &Container, code: &[u8]) -> bool {
        if !c.d3d11() {
            return self.skipped.insert(c.hash, c.flags & FLAG_DXIL != 0).is_none();
        }
        if self.shaders.contains_key(&c.hash) {
            return false;
        }
        let fresh = code[4..20].iter().all(|&b| b == 0) || checksum(code) == code[4..20];
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
        const OODLE: u8 = 4;
        const GZIP: u8 = 5;
        let gzip = memchr::memmem::Finder::new(b"\x1f\x8b\x08");
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
                cand.extend(gzip.find_iter(w).map(|i| (start + i, GZIP)));
                cand.extend(zstd.find_iter(w).map(|i| (start + i, 1)));
                cand.extend(lz4.find_iter(w).map(|i| (start + i, 2)));
                cand.extend(
                    memchr::memchr_iter(0x78, &w[..end - start])
                        .filter(|&i| i + 2 <= w.len() && is_zlib_header(&w[i..]))
                        .map(|i| (start + i, 0)),
                );
                if self.oodle.is_some() {
                    cand.extend(
                        memchr::memchr_iter(0x8C, &w[..end - start])
                            .filter(|&i| oodle::run_start(&b[start + i..]).is_some())
                            .map(|i| (start + i, OODLE)),
                    );
                }
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
                    0 => inflate_run(rest, &mut out),
                    OODLE => match self.oodle.map(|o| o.run(rest, &mut out, STREAM_CAP as usize)) {
                        Some(used) if used > 0 => Some(Some(used)),
                        _ => None,
                    },
                    1 => read_capped(ruzstd::decoding::StreamingDecoder::new(rest).ok(), &mut out).then_some(None),
                    GZIP => read_capped(Some(flate2::read::GzDecoder::new(rest)), &mut out).then_some(None),
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
                    OODLE => self.stats.oodle += n,
                    GZIP => self.stats.gzip += n,
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

fn dcx_kraken(bytes: &[u8]) -> Option<(&[u8], usize)> {
    if bytes.get(24..28)? != b"DCS\0" || bytes.get(36..44)? != b"DCP\0KRAK" ||
        bytes.get(68..76)? != b"DCA\0\0\0\0\x08" {
        return None;
    }
    let big = |at| Some(u32::from_be_bytes(bytes.get(at..at + 4)?.try_into().ok()?) as usize);
    let raw = big(28)?;
    let compressed = big(32)?;
    if !(MIN_FILE as usize..=DCX_CAP).contains(&raw) || compressed == 0 { return None; }
    Some((bytes.get(76..76usize.checked_add(compressed)?)?, raw))
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
            Ok(_) if out.len() as u64 >= STREAM_CAP => break, // whole output, across a run
            Ok(_) => {}
            Err(_) => break,
        }
    }
    (!out.is_empty()).then_some(None)
}

/// Inflates a run of zlib streams that follow each other (up to 15 bytes
/// apart, the padding of encrypted-size alignment), joined into one output:
/// archives like Unreal paks compress a file in 64 KiB blocks, one stream
/// each, so a container across a block edge is only whole once joined.
fn inflate_run(b: &[u8], out: &mut Vec<u8>) -> Option<Option<usize>> {
    let first = inflate(b, out)?;
    let Some(mut used) = first else { return Some(None) };
    'run: while (out.len() as u64) < STREAM_CAP {
        for gap in 0..16 {
            let q = used + gap;
            if q + 2 > b.len() {
                break 'run;
            }
            if !is_zlib_header(&b[q..]) {
                continue;
            }
            let keep = out.len();
            match inflate(&b[q..], out) {
                Some(Some(c)) => {
                    used = q + c;
                    continue 'run;
                }
                _ => {
                    out.truncate(keep); // not a whole stream: the run ends here
                    break 'run;
                }
            }
        }
        break;
    }
    Some(Some(used))
}

/// Reads a decoder to its end or STREAM_CAP; false when it gave nothing.
fn read_capped<R: Read>(r: Option<R>, out: &mut Vec<u8>) -> bool {
    let Some(r) = r else { return false };
    let _ = r.take(STREAM_CAP).read_to_end(out); // a corrupt tail still leaves what decoded
    !out.is_empty()
}

fn is_spv_path(path: &Path) -> bool {
    path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("spv"))
}

/// Recognises a complete shader module at a file or archive entry boundary.
/// Checks the binary structure without validating device-specific capabilities.
fn spirv_hash(bytes: &[u8]) -> Option<u128> {
    const MAGIC: u32 = 0x0723_0203;
    if bytes.len() < 20 || bytes.len() % 4 != 0 {
        return None;
    }
    let read = match u32_at(bytes, 0) {
        MAGIC => u32::from_le_bytes,
        magic if magic == MAGIC.swap_bytes() => u32::from_be_bytes,
        _ => return None,
    };
    let word = |offset| read(bytes[offset..offset + 4].try_into().unwrap());
    let version = word(4);
    let bound = word(12);
    if version & 0xffff_00ff != 0x0001_0000 || bound == 0 || word(16) != 0 {
        return None;
    }
    let (mut memory_model, mut entry_point, mut function_end) = (false, false, false);
    let mut offset = 20;
    while offset < bytes.len() {
        let instruction = word(offset);
        let words = (instruction >> 16) as usize;
        let end = offset + words * 4;
        if words == 0 || end > bytes.len() {
            return None;
        }
        match instruction as u16 {
            14 if words == 3 => memory_model = matches!(word(offset + 8), 1 | 3), // OpMemoryModel
            15 if words >= 4 => { // OpEntryPoint
                let id = word(offset + 8);
                entry_point |= matches!(word(offset + 4), 0..=5 | 5267..=5268 | 5313..=5318 | 5364..=5365)
                    && id > 0 && id < bound && bytes[offset + 12..end].contains(&0);
            }
            56 if words == 1 => function_end = true, // OpFunctionEnd
            _ => {}
        }
        offset = end;
    }
    if !(memory_model && entry_point && function_end) {
        return None;
    }
    if u32_at(bytes, 0) == MAGIC {
        return Some(xxhash_rust::xxh3::xxh3_128(bytes));
    }
    // Deduplicate equivalent modules encoded in the opposite byte order.
    let mut hash = xxhash_rust::xxh3::Xxh3::new();
    for offset in (0..bytes.len()).step_by(4) {
        hash.update(&word(offset).to_le_bytes());
    }
    Some(hash.digest128())
}

fn unreal_spirv_hash(bytes: &[u8]) -> Option<u128> {
    if let Some(hash) = spirv_hash(bytes) { return Some(hash); }
    for start in memchr::memmem::find_iter(bytes, b"\x03\x02\x23\x07") {
        if start < 4 { continue; }
        // Native Vulkan shader records can put a counted SPIR-V array after a header.
        let count = u32_at(bytes, start - 4) as usize;
        let code = &bytes[start..];
        for size in [count, count.saturating_mul(4)] {
            if let Some(hash) = code.get(..size).and_then(spirv_hash) {
                return Some(hash);
            }
        }
    }
    None
}

fn scan_file(path: &Path, oodle: Option<Oodle>, key: Option<&PakKey>,
             read_group: impl FnMut(&[u8], &mut Vec<u8>) -> Option<()>) -> Found {
    let mut f = Found { oodle, ..Found::default() };
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
    let extension = path.extension().and_then(|s| s.to_str()).unwrap_or_default();
    let pak = if extension.eq_ignore_ascii_case("pak") {
        unreal_pak::scan(&map, key, oodle, |kind, bytes| f.unreal_payload(kind, bytes))
    } else if extension.eq_ignore_ascii_case("utoc") {
        crate::unreal_io::scan(&map, path, key, oodle, read_group, |kind, bytes| f.unreal_payload(kind, bytes))
    } else if extension.eq_ignore_ascii_case("ushaderbytecode") {
        f.unreal_payload(Payload::Library, &map);
        return f;
    } else {
        unreal_pak::PakScan::default()
    };
    f.stats.unreal_packages += pak.packages;
    f.stats.unreadable_unreal_packages += pak.unreadable;
    f.stats.locked_unreal_paks += u64::from(pak.locked);
    f.stats.unreal_shaders_expected += pak.expected_shaders;
    f.stats.unreal_libraries_unreadable += pak.unreadable_libraries;
    let apk = map.starts_with(b"ENDILTLE") && f.search_apk(&map);
    let unity = if [b"UnityFS\0".as_slice(), b"UnityWeb\0", b"UnityRaw\0"].iter().any(|s| map.starts_with(s)) {
        crate::unity::scan_bundle(&map, |bytes| {
            f.record_spirv(bytes);
            f.search(bytes, 0);
        })
    } else {
        None
    };
    if let Some((entries, unreadable)) = unity {
        f.stats.unity_entries += entries;
        f.stats.unreadable_unity_entries += unreadable;
    }
    if (!pak.opened || pak.unreadable > 0) && !apk && !unity.is_some_and(|(_, unreadable)| unreadable == 0) {
        f.search(&map, 0);
    }
    if map.starts_with(b"PK\x03\x04") {
        f.search_zip(&map);
    } else if map.starts_with(b"CPK ") {
        f.search_cpk(&map);
    }
    if oodle.is_some() && (extension.eq_ignore_ascii_case("bdt") || extension.eq_ignore_ascii_case("dcx")) {
        f.search_dcx(&map);
    }
    if extension.eq_ignore_ascii_case("exe") || extension.eq_ignore_ascii_case("dll") {
        f.search_glsl(&map);
    }
    if is_spv_path(path) {
        f.record_spirv(&map);
    }
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
    scan_paths(dir, &files(dir), None, None)
}

fn scan_paths(dir: &Path, paths: &[PathBuf], key: Option<&PakKey>,
              detected: Option<&crate::unreal_key::DetectedKeys>) -> (Vec<Shader>, ScanStats) {
    let oodle = oodle::find(dir).and_then(|p| oodle::load(&p));
    // Each thread may hold a few STREAM_CAP buffers (a stream and the streams
    // inside it), so the thread count bounds the scan's memory.
    let threads = std::thread::available_parallelism().map_or(1, |n| n.get()).min(SCAN_THREADS);
    let catalog = std::sync::OnceLock::new();
    let archive_key = |p: &Path| detected.and_then(|keys| keys.for_archive(p)).or(key);
    let scan = || paths.par_iter().map(|p| {
        scan_file(p, oodle, archive_key(p), |id, out| {
            catalog.get_or_init(|| crate::unreal_io::Catalog::new(paths, archive_key))
                .read_group(id, p, oodle, out)
        })
    }).reduce(Found::default, Found::merge);
    let found = match rayon::ThreadPoolBuilder::new().num_threads(threads).build() {
        Ok(pool) => pool.install(scan),
        Err(_) => scan(),
    };
    let mut stats = found.stats;
    let mut shaders: Vec<Shader> = found.shaders.into_values().collect();
    shaders.sort_unstable_by_key(|s| s.hash);
    stats.usable = shaders.len() as u64;
    stats.skipped = found.skipped.len() as u64;
    stats.dxil = found.skipped.values().filter(|&&dxil| dxil).count() as u64;
    stats.glsl = found.glsl.len() as u64;
    stats.spirv = found.spirv.len() as u64;
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

/// Identifies the install by the names, sizes and timestamps of its files.
fn install_key(exe: &Path, root: &Path) -> String {
    use std::hash::{Hash, Hasher};
    let mut dirs = vec![root.to_path_buf()];
    let mut files = Vec::new();
    while let Some(dir) = dirs.pop() {
        let Ok(entries) = fs::read_dir(dir) else { continue };
        for entry in entries.flatten() {
            let Ok(kind) = entry.file_type() else { continue };
            let path = entry.path();
            if kind.is_dir() {
                dirs.push(path);
            } else if kind.is_file() && !volatile(&path) {
                // On Windows the directory entry already contains this metadata.
                if let Ok(meta) = entry.metadata() {
                    files.push((path, meta.len(), meta.modified().ok()));
                }
            }
        }
    }
    files.sort_unstable_by(|a, b| a.0.cmp(&b.0));
    let mut key = std::collections::hash_map::DefaultHasher::new();
    if let Some((path, _)) = unreal_key_file(exe) {
        path.hash(&mut key);
        fs::metadata(path).ok().and_then(|meta| meta.modified().ok()).hash(&mut key);
    }
    for (p, len, modified) in files {
        p.strip_prefix(root).unwrap_or(&p).hash(&mut key);
        len.hash(&mut key);
        modified.hash(&mut key);
    }
    format!("tessera-scan 10 {:016x}", key.finish())
}

fn unreal_key_reference(exe: &Path) -> Option<PathBuf> {
    Some(crate::warm::with_suffix(&scan_base(exe)?, ".pak-key-path"))
}

#[derive(Clone, Copy)]
pub enum PakKeySource {
    Environment,
    LinkedFile,
    ScsKiller,
}

fn unreal_key_file(exe: &Path) -> Option<(PathBuf, PakKeySource)> {
    if let Some(path) = std::env::var_os("TESSERA_UNREAL_PAK_KEY_FILE").filter(|p| !p.is_empty()) {
        return Some((path.into(), PakKeySource::Environment));
    }
    if let Some(reference) = unreal_key_reference(exe) {
        if let Ok(path) = fs::read_to_string(reference) {
            return Some((path.into(), PakKeySource::LinkedFile));
        }
    }
    if !exe.file_name()?.eq_ignore_ascii_case("ff7remake_.exe") {
        return None;
    }
    let path = PathBuf::from(std::env::var_os("LOCALAPPDATA")?)
        .join("SCSKiller/games/steam_1462040/aes.key");
    path.is_file().then_some((path, PakKeySource::ScsKiller))
}

pub fn unreal_key_status(exe: &Path) -> Option<(PakKeySource, bool)> {
    let (path, source) = unreal_key_file(exe)?;
    Some((source, PakKey::from_file(&path).is_some()))
}

fn key_path(base: &Path) -> PathBuf {
    let mut s = base.as_os_str().to_owned();
    s.push(".key");
    PathBuf::from(s)
}

/// What the last scan recorded in `<base>.key`: the install key on the
/// first line, then "invalid <n>" (containers whose checksum did not match).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScanRecord {
    pub key: String,
    pub invalid: Option<u64>,
    pub unsupported: Option<u64>,
    pub unreal_packages: Option<u64>,
    pub unreadable_unreal_packages: Option<u64>,
    pub locked_unreal_paks: Option<u64>,
    pub auto_key_archives: Option<u64>,
    pub glsl: Option<u64>,
    pub spirv: Option<u64>,
    pub dxil: Option<u64>,
    pub unreal_libraries_unreadable: Option<u64>,
    pub unreal_shaders_expected: Option<u64>,
    pub unreal_shaders_decoded: Option<u64>,
    pub unreal_shaders_dxbc: Option<u64>,
    pub unreal_shaders_dxil: Option<u64>,
    pub unreal_shaders_spirv: Option<u64>,
}

pub fn record_at(base: &Path) -> Option<ScanRecord> {
    let text = fs::read_to_string(key_path(base)).ok()?;
    let mut lines = text.lines();
    let key = lines.next()?.to_string();
    let value = |prefix| lines.clone().find_map(|l| l.strip_prefix(prefix)?.trim().parse().ok());
    Some(ScanRecord {
        key, invalid: value("invalid "), unsupported: value("unsupported "),
        unreal_packages: value("unreal_packages "), unreadable_unreal_packages: value("unreadable_unreal_packages "),
        locked_unreal_paks: value("locked_unreal_paks "),
        auto_key_archives: value("auto_key_archives "),
        glsl: value("glsl "), spirv: value("spirv "), dxil: value("dxil "),
        unreal_libraries_unreadable: value("unreal_libraries_unreadable "),
        unreal_shaders_expected: value("unreal_shaders_expected "),
        unreal_shaders_decoded: value("unreal_shaders_decoded "),
        unreal_shaders_dxbc: value("unreal_shaders_dxbc "),
        unreal_shaders_dxil: value("unreal_shaders_dxil "),
        unreal_shaders_spirv: value("unreal_shaders_spirv "),
    })
}

/// True when the scan store matches the install as it is now.
pub fn up_to_date(exe: &Path, root: &Path, require_spirv: bool) -> bool {
    scan_base(exe).and_then(|b| record_at(&b)).is_some_and(|r| {
        (!require_spirv || r.spirv.is_some()) && r.key == install_key(exe, root)
    })
}

/// The key of the install as it is now (walks the folder's file metadata).
pub fn current_key(exe: &Path, root: &Path) -> String {
    install_key(exe, root)
}

/// Scans the install folder of `exe` (`root`) and writes what the layer has
/// not stored yet to the scan store, which the warm-up compiles too.
pub fn scan_game(exe: &Path, root: &Path) -> io::Result<ScanStats> {
    let base = scan_base(exe).ok_or_else(|| io::Error::other("invalid game path"))?;
    let key = install_key(exe, root);
    let pak_key = unreal_key_file(exe).and_then(|(p, _)| PakKey::from_file(&p));
    let paths = files(root);
    let detected = crate::unreal_key::detect(exe, &paths);
    let (mut shaders, stats) = scan_paths(root, &paths, pak_key.as_ref(), Some(&detected));
    let stored: std::collections::HashSet<u64> = crate::shaders::read(exe).iter().map(|s| s.hash).collect();
    shaders.retain(|s| !stored.contains(&s.hash));
    crate::shaders::write_base(&base, &shaders)?;
    fs::write(key_path(&base), format!(
        "{key}\ninvalid {}\nunsupported {}\nunreal_packages {}\nunreadable_unreal_packages {}\nlocked_unreal_paks {}\nauto_key_archives {}\nglsl {}\nspirv {}\ndxil {}\ncpk_entries {}\ndcx_entries {}\nzip_entries {}\napk_entries {}\nunity_entries {}\nunreadable_unity_entries {}\nunreal_libraries_unreadable {}\nunreal_shaders_expected {}\nunreal_shaders_decoded {}\nunreal_shaders_dxbc {}\nunreal_shaders_dxil {}\nunreal_shaders_spirv {}\n",
        stats.invalid, stats.skipped, stats.unreal_packages, stats.unreadable_unreal_packages, stats.locked_unreal_paks,
        detected.count(), stats.glsl, stats.spirv, stats.dxil, stats.cpk_entries, stats.dcx_entries, stats.zip_entries, stats.apk_entries,
        stats.unity_entries, stats.unreadable_unity_entries,
        stats.unreal_libraries_unreadable, stats.unreal_shaders_expected, stats.unreal_shaders_decoded,
        stats.unreal_shaders_dxbc, stats.unreal_shaders_dxil, stats.unreal_shaders_spirv
    ))?;
    Ok(stats)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_deflated_dxbc_and_deduplicates_embedded_glsl() {
        use std::io::Write;

        let dir = tempfile::tempdir().unwrap();
        let dxbc = container(0x0000_0050, 0);
        let mut archive = zip::ZipWriter::new(Cursor::new(Vec::new()));
        archive.start_file("shader.bin", zip::write::SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)).unwrap();
        archive.write_all(&dxbc).unwrap();
        fs::write(dir.path().join("archive.dat"), archive.finish().unwrap().into_inner()).unwrap();
        let glsl = b"glShaderSource\0#version 330\nvoid main() { gl_Position = vec4(0); }\0";
        fs::write(dir.path().join("a.dll"), glsl).unwrap();
        fs::write(dir.path().join("b.dll"), glsl).unwrap();
        let (shaders, stats) = scan_dir(dir.path());
        assert_eq!(shaders.iter().map(|s| s.hash).collect::<Vec<_>>(), [hash_of(&dxbc)]);
        assert_eq!(stats.zip_entries, 1);
        assert_eq!(stats.glsl, 1);
    }

    #[test]
    fn extracts_dx_bytecode_across_unity_bundle_blocks() {
        fn lzma(bytes: &[u8], unity_fs: bool) -> Vec<u8> {
            let mut compressed = Vec::new();
            lzma_rs::lzma_compress(&mut Cursor::new(bytes), &mut compressed).unwrap();
            if unity_fs { compressed.drain(5..13); }
            compressed
        }
        fn string(out: &mut Vec<u8>, value: &[u8]) {
            out.extend_from_slice(value);
            out.push(0);
        }

        let dir = tempfile::tempdir().unwrap();
        let dxbc = signed(0x0000_0050);
        let mut dxil = signed(0x0000_0060);
        dxil[36..40].copy_from_slice(b"DXIL");
        let mut shaders = dxbc.clone();
        shaders.extend_from_slice(&dxil);
        let parts = [&shaders[..25], &shaders[25..]];
        let stored = [lzma(parts[0], true), lz4_flex::block::compress(parts[1])];
        let mut table = vec![0; 16];
        table.extend_from_slice(&2u32.to_be_bytes());
        for ((part, stored), compression) in parts.iter().zip(&stored).zip([1u16, 3]) {
            table.extend_from_slice(&(part.len() as u32).to_be_bytes());
            table.extend_from_slice(&(stored.len() as u32).to_be_bytes());
            table.extend_from_slice(&compression.to_be_bytes());
        }
        table.extend_from_slice(&1u32.to_be_bytes());
        table.extend_from_slice(&0u64.to_be_bytes());
        table.extend_from_slice(&(shaders.len() as u64).to_be_bytes());
        table.extend_from_slice(&4u32.to_be_bytes());
        string(&mut table, b"CAB-test");
        let table_size = table.len() as u32;
        let table = lz4_flex::block::compress(&table);
        let mut bundle = b"UnityFS\0\0\0\0\x08".to_vec();
        string(&mut bundle, b"5.x.x");
        string(&mut bundle, b"6000.0.1f1");
        let size_at = bundle.len();
        bundle.extend_from_slice(&0u64.to_be_bytes());
        bundle.extend_from_slice(&(table.len() as u32).to_be_bytes());
        bundle.extend_from_slice(&table_size.to_be_bytes());
        bundle.extend_from_slice(&0x2c2u32.to_be_bytes());
        bundle.resize(bundle.len().next_multiple_of(16), 0);
        for bytes in stored { bundle.extend_from_slice(&bytes); }
        bundle.extend_from_slice(&table);
        let bundle_size = bundle.len() as u64;
        bundle[size_at..size_at + 8].copy_from_slice(&bundle_size.to_be_bytes());
        fs::write(dir.path().join("modern.bundle"), bundle).unwrap();

        let mut assets = 1u32.to_be_bytes().to_vec();
        string(&mut assets, b"legacy.assets");
        let start = assets.len() as u32 + 8;
        assets.extend_from_slice(&start.to_be_bytes());
        assets.extend_from_slice(&(dxbc.len() as u32).to_be_bytes());
        assets.extend_from_slice(&dxbc);
        for (signature, web) in [(b"UnityRaw".as_slice(), false), (b"UnityWeb", true)] {
            let stored = if web { lzma(&assets, false) } else { assets.clone() };
            let mut bundle = Vec::new();
            string(&mut bundle, signature);
            bundle.extend_from_slice(&3u32.to_be_bytes());
            string(&mut bundle, b"4.x.x");
            string(&mut bundle, b"4.7.2f1");
            let header_size = bundle.len() as u32 + 32;
            for n in [0, header_size, 1, 1, stored.len() as u32, assets.len() as u32,
                      header_size + stored.len() as u32, start] {
                bundle.extend_from_slice(&n.to_be_bytes());
            }
            bundle.extend_from_slice(&stored);
            fs::write(dir.path().join(if web { "legacy.web" } else { "legacy.raw" }), bundle).unwrap();
        }

        let (shaders, stats) = scan_dir(dir.path());
        assert_eq!((stats.unity_entries, stats.unreadable_unity_entries), (3, 0));
        assert_eq!(stats.dxil, 1);
        assert_eq!(shaders.iter().map(|s| s.hash).collect::<Vec<_>>(), [hash_of(&dxbc)]);
    }

    #[test]
    fn counts_complete_spirv_modules_in_files_and_zip_entries() {
        use std::io::Write;

        let dir = tempfile::tempdir().unwrap();
        let module: [u32; 35] = [
            0x0723_0203, 0x0001_0000, 0, 5, 0,
            0x0002_0011, 1, // OpCapability Shader
            0x0003_000e, 0, 1, // OpMemoryModel Logical GLSL450
            0x0005_000f, 5, 3, 0x6e69_616d, 0, // OpEntryPoint GLCompute %3 "main"
            0x0006_0010, 3, 17, 1, 1, 1, // OpExecutionMode %3 LocalSize
            0x0002_0013, 1, // OpTypeVoid
            0x0003_0021, 2, 1, // OpTypeFunction
            0x0005_0036, 1, 3, 0, 2, // OpFunction
            0x0002_00f8, 4, // OpLabel
            0x0001_00fd, // OpReturn
            0x0001_0038, // OpFunctionEnd
        ];
        let bytes: Vec<_> = module.into_iter().flat_map(u32::to_le_bytes).collect();
        let swapped: Vec<_> = module.into_iter().flat_map(u32::to_be_bytes).collect();
        assert!(spirv_hash(&bytes).is_some());
        assert_eq!(spirv_hash(&bytes), spirv_hash(&swapped));
        assert!(spirv_hash(&bytes[..bytes.len() - 8]).is_none());
        fs::write(dir.path().join("loose.SPV"), &bytes).unwrap();
        fs::write(dir.path().join("partial.spv"), &bytes[..bytes.len() - 8]).unwrap();
        let mut archive = zip::ZipWriter::new(Cursor::new(Vec::new()));
        for (i, compression) in [zip::CompressionMethod::Deflated, zip::CompressionMethod::Stored].into_iter().enumerate() {
            archive.start_file(format!("shader/{i}.spv"), zip::write::SimpleFileOptions::default()
                .compression_method(compression)).unwrap();
            let mut distinct = module;
            // Give the module a different local size and use big-endian encoding.
            distinct[18] += 1 + i as u32;
            let distinct: Vec<_> = distinct.into_iter().flat_map(u32::to_be_bytes).collect();
            archive.write_all(&distinct).unwrap();
        }
        archive.start_file("shader/copy.spv", zip::write::SimpleFileOptions::default()).unwrap();
        archive.write_all(&swapped).unwrap();
        fs::write(dir.path().join("shaders.zip"), archive.finish().unwrap().into_inner()).unwrap();
        let (shaders, stats) = scan_dir(dir.path());
        assert!(shaders.is_empty());
        assert_eq!(stats.spirv, 3);
    }

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
    fn joins_a_container_split_across_zlib_streams() {
        use std::io::Write;
        let ps = signed(0x0000_0050);
        let (a, b) = ps.split_at(ps.len() / 2);
        let z = |d: &[u8]| {
            let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
            e.write_all(d).unwrap();
            e.finish().unwrap()
        };
        for pad in [0usize, 7] {
            let blob = [vec![9u8; 5], z(a), vec![0u8; pad], z(b)].concat();
            let t = tempfile::tempdir().unwrap();
            fs::write(t.path().join("c.pak"), &blob).unwrap();
            let (shaders, _) = scan_dir(t.path());
            assert_eq!(shaders.iter().map(|s| s.hash).collect::<Vec<_>>(), [hash_of(&ps)], "padding {pad}");
        }
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

    #[test]
    fn discovers_unsigned_dxbc_and_counts_dxil_libraries_in_gzip() {
        use std::io::Write;
        let shader = container(0x0000_0050, 0);
        let mut library = container(0x0006_0060, 9);
        library[36..40].copy_from_slice(b"DXIL");
        let mut gzip = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
        gzip.write_all(&[shader.clone(), library].concat()).unwrap();
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join("shaders.gz"), gzip.finish().unwrap()).unwrap();
        let (shaders, stats) = scan_dir(dir.path());
        assert_eq!(shaders.iter().map(|s| s.hash).collect::<Vec<_>>(), [hash_of(&shader)]);
        assert_eq!((stats.gzip, stats.skipped, stats.invalid), (2, 1, 0));
    }

    #[test]
    fn reads_legacy_inline_shaders_from_an_encrypted_pak() {
        use aes::cipher::{BlockEncrypt, KeyInit};
        use sha1::{Digest, Sha1};
        use std::io::Write;

        fn compress(bytes: &[u8]) -> Vec<u8> {
            let mut writer = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::fast());
            writer.write_all(bytes).unwrap();
            writer.finish().unwrap()
        }
        fn number(out: &mut Vec<u8>, n: u32) { out.extend_from_slice(&n.to_le_bytes()); }
        fn offset(out: &mut Vec<u8>, n: usize) { out.extend_from_slice(&(n as u64).to_le_bytes()); }
        fn path(out: &mut Vec<u8>, s: &str) {
            number(out, s.len() as u32 + 1);
            out.extend_from_slice(s.as_bytes());
            out.push(0);
        }
        let cipher = aes::Aes256::new((&[7u8; 32]).into());
        let encrypt = |bytes: &mut Vec<u8>| {
            bytes.resize(bytes.len().next_multiple_of(16), 0);
            for block in bytes.chunks_exact_mut(16) {
                cipher.encrypt_block(aes::cipher::Block::<aes::Aes256>::from_mut_slice(block));
            }
        };
        let shader = signed(0x0000_0050);
        let inline = compress(&shader);
        let mut package = Vec::new();
        number(&mut package, inline.len() as u32);
        package.extend_from_slice(&inline);
        let mut payload = compress(&package);
        let mut header = Vec::new();
        for value in [0, payload.len(), package.len()] { offset(&mut header, value); }
        number(&mut header, 1);
        header.extend_from_slice(&Sha1::digest(&package));
        number(&mut header, 1);
        offset(&mut header, 73);
        offset(&mut header, 73 + payload.len());
        header.push(1);
        number(&mut header, 65536);
        assert_eq!(header.len(), 73);
        encrypt(&mut payload);
        let mut pak = header.clone();
        pak.extend_from_slice(&payload);
        let mut index = Vec::new();
        path(&mut index, "../../../");
        number(&mut index, 1);
        path(&mut index, "End/Content/Material.uexp");
        index.extend_from_slice(&header);
        let hash = Sha1::digest(&index);
        encrypt(&mut index);
        let start = pak.len();
        pak.extend_from_slice(&index);
        pak.push(1);
        number(&mut pak, 0x5a6f12e1);
        number(&mut pak, 4);
        offset(&mut pak, start);
        offset(&mut pak, index.len());
        pak.extend_from_slice(&hash);

        let dir = tempfile::tempdir().unwrap();
        let package_path = dir.path().join("chunk.pak");
        fs::write(&package_path, pak).unwrap();
        let key_path = dir.path().join("test.key");
        fs::write(&key_path, "07".repeat(32)).unwrap();
        let key = PakKey::from_file(&key_path).unwrap();
        assert_eq!(scan_file(&package_path, None, None, |_, _| None).stats.locked_unreal_paks, 1);
        let f = scan_file(&package_path, None, Some(&key), |_, _| None);
        assert_eq!((f.stats.unreal_packages, f.stats.locked_unreal_paks, f.stats.unreadable_unreal_packages), (1, 0, 0));
        assert_eq!(f.shaders[&hash_of(&shader)].code, shader);
    }

    #[test]
    fn extracts_unreal_shader_libraries_and_resolves_shared_iostore_groups() {
        fn number(out: &mut Vec<u8>, n: u32) { out.extend_from_slice(&n.to_le_bytes()); }
        fn table(out: &mut Vec<u8>, size: u32, bytes: &[u8]) {
            number(out, size);
            out.extend_from_slice(bytes);
        }
        fn iostore(root: &Path, filename: &str, id: [u8; 12], data: &[u8]) {
            let mut toc = vec![0u8; 144];
            toc[..16].copy_from_slice(b"-==--==--==--==-");
            toc[16] = 5;
            for (at, value) in [(20, 144u32), (24, 1), (28, 1), (32, 12), (44, 4096), (52, 1)] {
                toc[at..at + 4].copy_from_slice(&value.to_le_bytes());
            }
            toc[88..96].copy_from_slice(&u64::MAX.to_le_bytes());
            toc.extend_from_slice(&id);
            toc.extend_from_slice(&[0; 5]);
            toc.extend_from_slice(&(data.len() as u64).to_be_bytes()[3..]);
            toc.extend_from_slice(&((data.len() as u64) << 40).to_le_bytes());
            number(&mut toc, data.len() as u32);
            fs::write(root.join(filename).with_extension("utoc"), toc).unwrap();
            fs::write(root.join(filename).with_extension("ucas"), data).unwrap();
        }
        fn zlib(data: &[u8]) -> Vec<u8> {
            use std::io::Write;
            let mut encoder = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
            encoder.write_all(data).unwrap();
            encoder.finish().unwrap()
        }

        let dir = tempfile::tempdir().unwrap();
        let dxbc = signed(0x0000_0050);
        let mut dxil = signed(0x0000_0060);
        dxil[36..40].copy_from_slice(b"DXIL");
        let mut group = dxbc.clone();
        group.extend_from_slice(&dxil);
        let compressed = zlib(&group);
        let mut group_id = [0; 12];
        group_id[11] = 9;
        let mut library_id = [0; 12];
        library_id[11] = 8;

        let mut library = 1u32.to_le_bytes().to_vec();
        table(&mut library, 0, &[]);
        table(&mut library, 2, &[0; 40]);
        table(&mut library, 1, &group_id);
        table(&mut library, 0, &[]);
        number(&mut library, 2);
        library.extend_from_slice(&0u64.to_le_bytes());
        library.extend_from_slice(&((dxbc.len() as u64) << 34).to_le_bytes());
        number(&mut library, 1);
        for n in [0, 2, group.len() as u32, compressed.len() as u32] { number(&mut library, n); }
        number(&mut library, 2);
        number(&mut library, 1);
        number(&mut library, 0);
        iostore(dir.path(), "library", library_id, &library);
        iostore(dir.path(), "group", group_id, &compressed);

        let mut standalone = 2u32.to_le_bytes().to_vec();
        table(&mut standalone, 0, &[]);
        table(&mut standalone, 1, &[0; 20]);
        table(&mut standalone, 0, &[]);
        number(&mut standalone, 1);
        standalone.extend_from_slice(&0u64.to_le_bytes());
        let compressed = lz4_flex::block::compress(&dxbc);
        number(&mut standalone, compressed.len() as u32);
        number(&mut standalone, dxbc.len() as u32);
        standalone.push(0);
        table(&mut standalone, 0, &[]);
        table(&mut standalone, 0, &[]);
        standalone.extend_from_slice(&compressed);
        fs::write(dir.path().join("ShaderArchive-PCD3D_SM5.ushaderbytecode"), standalone).unwrap();

        let (shaders, stats) = scan_dir(dir.path());
        assert_eq!((stats.unreal_shaders_expected, stats.unreal_shaders_decoded), (3, 3));
        assert_eq!((stats.unreal_shaders_dxbc, stats.unreal_shaders_dxil, stats.dxil), (2, 1, 1));
        assert_eq!(stats.unreal_libraries_unreadable, 0);
        assert_eq!(shaders.len(), 1);
        assert_eq!(shaders[0].code, dxbc);
    }
}
