// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only Unreal PAK input for the shader scan. The legacy package
//! selection follows SCSKiller's UnrealReader/InlineShaders (GPL-3.0-or-later).

use std::io::Read;
use std::path::Path;

use aes::cipher::{BlockDecrypt, KeyInit};
use sha1::{Digest, Sha1};

use crate::oodle::Oodle;
use crate::unreal_shader::{is_library, Payload};

const MAX_INDEX: usize = 64 << 20;
const REUSABLE_PACKAGE: usize = 64 << 20;
const MAX_PACKAGE: usize = 256 << 20;
static LARGE_PACKAGE: std::sync::Mutex<()> = std::sync::Mutex::new(());
const FOOTER: usize = 45;
const MAGIC: u32 = 0x5a6f12e1;

pub(crate) struct PakKey(aes::Aes256);

impl PakKey {
    pub(crate) fn from_file(path: &Path) -> Option<Self> {
        let mut text = String::new();
        std::fs::File::open(path).ok()?.take(4096).read_to_string(&mut text).ok()?;
        let value = text.lines().next()?.trim().as_bytes();
        let value = value.strip_prefix(b"0x").or_else(|| value.strip_prefix(b"0X")).unwrap_or(value);
        if value.len() != 64 { return None; }
        let mut key = [0u8; 32];
        for (byte, hex) in key.iter_mut().zip(value.chunks_exact(2)) {
            *byte = u8::from_str_radix(std::str::from_utf8(hex).ok()?, 16).ok()?;
        }
        Some(Self::from_bytes(&key))
    }

    pub(crate) fn from_bytes(key: &[u8; 32]) -> Self {
        Self(aes::Aes256::new(key.into()))
    }

    pub(crate) fn decode(&self, bytes: &mut [u8]) -> Option<()> {
        if bytes.len() % 16 != 0 { return None; }
        for block in bytes.chunks_exact_mut(16) {
            self.0.decrypt_block(aes::cipher::Block::<aes::Aes256>::from_mut_slice(block));
        }
        Some(())
    }
}

pub(crate) fn take<'a>(data: &mut &'a [u8], size: usize) -> Option<&'a [u8]> {
    let (value, remaining) = data.split_at_checked(size)?;
    *data = remaining;
    Some(value)
}

pub(crate) fn number(data: &mut &[u8]) -> Option<u32> {
    Some(u32::from_le_bytes(take(data, 4)?.try_into().ok()?))
}

pub(crate) fn offset(data: &mut &[u8]) -> Option<usize> {
    usize::try_from(u64::from_le_bytes(take(data, 8)?.try_into().ok()?)).ok()
}

pub(crate) fn path(data: &mut &[u8]) -> Option<String> {
    let len = number(data)? as i32;
    let chars = usize::try_from(len.unsigned_abs()).ok()?;
    if chars == 0 { return Some(String::new()); }
    if chars > 32768 { return None; }
    if len < 0 {
        let bytes = take(data, chars * 2)?;
        let text: Vec<_> = bytes.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect();
        String::from_utf16(text.strip_suffix(&[0])?).ok()
    } else {
        Some(std::str::from_utf8(take(data, chars)?.strip_suffix(&[0])?).ok()?.to_owned())
    }
}

#[derive(Clone, Copy)]
struct Entry<'a> {
    start: usize,
    compressed: usize,
    size: usize,
    method: u32,
    block_size: usize,
    encrypted: bool,
    blocks: &'a [u8],
    block_base: usize,
    deleted: bool,
    whole_stream: bool,
    payload: Payload,
}

fn entry<'a>(data: &mut &'a [u8], footer: &Footer<'_>, location: Option<usize>) -> Option<Entry<'a>> {
    let original = data.len();
    let start = location.unwrap_or(offset(data)?);
    let compressed = offset(data)?;
    let size = offset(data)?;
    let method = if footer.version == 8 && footer.methods.len() == 128 {
        u32::from(take(data, 1)?[0])
    } else {
        number(data)?
    };
    if footer.version == 1 { take(data, 8)?; } // Timestamp.
    take(data, 20)?; // Data hash.
    let whole_stream = footer.version < 3;
    let blocks = if method == 0 || whole_stream { &[] } else {
        let count = number(data)? as usize;
        take(data, count.checked_mul(16)?)?
    };
    let (flags, block_size) = if whole_stream { (0, 0) } else {
        (take(data, 1)?[0], number(data)? as usize)
    };
    let block_base = if footer.version >= 5 { start } else { 0 };
    let start = if method == 0 || whole_stream {
        start.checked_add(original - data.len())?
    } else { start };
    Some(Entry { start, compressed, size, method, block_size, encrypted: flags & 1 != 0,
                 blocks, block_base, deleted: flags & 2 != 0, whole_stream, payload: Payload::Package })
}

fn stored<'a>(pak: &'a [u8], start: usize, len: usize, encrypted: bool, key: Option<&PakKey>,
              scratch: &'a mut Vec<u8>) -> Option<&'a [u8]> {
    let padded = if encrypted { len.checked_add(15)? & !15 } else { len };
    let bytes = pak.get(start..start.checked_add(padded)?)?;
    if !encrypted { return Some(bytes); }
    scratch.clear();
    scratch.extend_from_slice(bytes);
    key?.decode(scratch)?;
    scratch.get(..len)
}

pub(crate) fn decode_block(method: &[u8], bytes: &[u8], raw: usize, oodle: Option<Oodle>,
                           out: &mut Vec<u8>) -> Option<()> {
    let begin = out.len();
    if method.eq_ignore_ascii_case(b"None") {
        if bytes.len() != raw { return None; }
        out.extend_from_slice(bytes);
    } else if method.eq_ignore_ascii_case(b"Zlib") {
        flate2::read::ZlibDecoder::new(bytes).take(raw as u64 + 1).read_to_end(out).ok()?;
    } else if method.eq_ignore_ascii_case(b"Gzip") {
        flate2::read::GzDecoder::new(bytes).take(raw as u64 + 1).read_to_end(out).ok()?;
    } else if method.eq_ignore_ascii_case(b"Oodle") {
        if !oodle?.decode_exact(bytes, raw, out) { return None; }
    } else if method.eq_ignore_ascii_case(b"LZ4") {
        out.resize(begin.checked_add(raw)?, 0);
        if lz4_flex::block::decompress_into(bytes, &mut out[begin..]).ok()? != raw {
            return None;
        }
    } else if method.eq_ignore_ascii_case(b"Zstd") {
        ruzstd::decoding::StreamingDecoder::new(bytes).ok()?
            .take(raw as u64 + 1).read_to_end(out).ok()?;
    } else {
        return None;
    }
    (out.len() - begin == raw).then_some(())
}

fn package(pak: &[u8], e: Entry<'_>, method: &[u8], key: Option<&PakKey>, oodle: Option<Oodle>,
           out: &mut Vec<u8>, scratch: &mut Vec<u8>) -> Option<()> {
    out.clear();
    if e.size == 0 || e.size > MAX_PACKAGE || e.compressed > MAX_PACKAGE { return None; }
    if e.method == 0 {
        if e.compressed != e.size { return None; }
        out.extend_from_slice(stored(pak, e.start, e.size, e.encrypted, key, scratch)?);
        return Some(());
    }
    if e.whole_stream {
        let bytes = stored(pak, e.start, e.compressed, false, None, scratch)?;
        return decode_block(method, bytes, e.size, oodle, out);
    }
    if e.block_size == 0 || e.blocks.len() / 16 != e.size.div_ceil(e.block_size) { return None; }
    for mut block in e.blocks.chunks_exact(16) {
        let start = offset(&mut block)?;
        let len = offset(&mut block)?.checked_sub(start)?;
        if len > REUSABLE_PACKAGE { return None; }
        let bytes = stored(pak, e.block_base.checked_add(start)?, len, e.encrypted, key, scratch)?;
        let raw = e.block_size.min(e.size - out.len());
        decode_block(method, bytes, raw, oodle, out)?;
    }
    Some(())
}

#[derive(Default)]
pub(crate) struct PakScan {
    pub opened: bool,
    pub locked: bool,
    pub packages: u64,
    pub unreadable: u64,
    pub expected_shaders: u64,
    pub unreadable_libraries: u64,
}

struct Footer<'a> {
    version: u32,
    encrypted: bool,
    frozen: bool,
    index: Index,
    end: usize,
    methods: &'a [u8],
}

struct Index {
    start: usize,
    size: usize,
    hash: [u8; 20],
}

impl Index {
    fn parse(data: &mut &[u8]) -> Option<Self> {
        let start = offset(data)?;
        let size = offset(data)?;
        let hash = take(data, 20)?.try_into().ok()?;
        (size <= MAX_INDEX).then_some(Self { start, size, hash })
    }

    fn matches(&self, bytes: &[u8]) -> bool {
        Sha1::digest(bytes)[..] == self.hash
    }

    fn read<'a>(&self, pak: &'a [u8], encrypted: bool, key: Option<&PakKey>,
                scratch: &'a mut Vec<u8>) -> Option<&'a [u8]> {
        stored(pak, self.start, self.size, encrypted, key, scratch)
    }
}

impl<'a> Footer<'a> {
    fn parse(pak: &'a [u8]) -> Option<Self> {
        if let Some(end) = pak.len().checked_sub(FOOTER - 1) {
            let mut bytes = &pak[end..];
            let magic = number(&mut bytes)?;
            let version = number(&mut bytes)?;
            if magic == MAGIC && (1..=3).contains(&version) {
                let index = Index::parse(&mut bytes)?;
                if index.start.checked_add(index.size)? <= end {
                    return Some(Self { version, encrypted: false, frozen: false, index, end, methods: &[] });
                }
            }
        }
        for suffix in [0, 128, 160, 161, 164] {
            let end = pak.len().checked_sub(FOOTER + suffix)?;
            let mut bytes = &pak[end..];
            let flag = take(&mut bytes, 1)?[0];
            if flag > 1 || number(&mut bytes)? != MAGIC { continue; }
            let version = number(&mut bytes)?;
            let methods = match (version, suffix) {
                (4..=7, 0) => &[][..],
                (8, 128 | 160) | (10..=14, 160) | (14, 164) => &bytes[36..36 + suffix.min(160)],
                (9, 161) if bytes[36] <= 1 => &bytes[37..],
                _ => continue,
            };
            let frozen = version == 9 && bytes[36] == 1;
            let end = end.checked_sub(if version >= 7 { 16 } else { 0 })?;
            let index = Index::parse(&mut bytes)?;
            if index.start.checked_add(index.size)? > end { continue; }
            return Some(Self { version, encrypted: flag == 1, frozen, index, end, methods });
        }
        None
    }

    fn method(&self, method: u32) -> Option<&[u8]> {
        if method == 0 { return Some(b"None"); }
        if self.methods.is_empty() {
            return match method & 7 {
                1 => Some(b"Zlib"),
                2 => Some(b"Gzip"),
                4 => Some(b"Oodle"),
                _ => None,
            };
        }
        let start = (method as usize - 1).checked_mul(32)?;
        self.methods.get(start..start.checked_add(32)?)?.split(|&b| b == 0).next()
    }
}

fn frozen_entries<'a>(index: &[u8], pak: &'a [u8], footer: &Footer<'_>,
                      entries: &mut Vec<Entry<'a>>) -> Option<u64> {
    // The frozen mount point occupies a 16-byte FString. The following array
    // has a relative pointer, count and capacity, followed elsewhere by the
    // 8-byte-aligned FPakEntry records. Read the complete entry data from the
    // header next to each payload, as with encoded modern entries.
    const ARRAY: usize = 16;
    const STRIDE: usize = 80;
    let mut header = index.get(ARRAY..)?;
    let pointer = u64::from_le_bytes(take(&mut header, 8)?.try_into().ok()?);
    let count = number(&mut header)? as usize;
    if number(&mut header)? as usize != count { return None; }
    if count == 0 { return Some(0); }
    if pointer & 1 == 0 { return None; }
    let relative = usize::try_from((pointer as i64) >> 1).ok()?;
    let begin = ARRAY.checked_add(relative)?;
    let size = count.checked_mul(STRIDE)?;
    let table = index.get(begin..begin.checked_add(size)?)?;
    let mut unreadable = 0;
    for record in table.chunks_exact(STRIDE) {
        if record[72] & 2 != 0 { continue; } // Deleted.
        let item = offset(&mut &record[..8]).and_then(|start| {
            entry(&mut pak.get(start..footer.index.start)?, footer, Some(start))
        });
        if let Some(item) = item { entries.push(item); } else { unreadable += 1; }
    }
    Some(unreadable)
}

fn shader_path(name: &str) -> bool {
    let name = name.to_ascii_lowercase();
    [".uexp", ".uasset", ".umap", ".ushaderbytecode", ".spv"].iter().any(|ext| name.ends_with(ext))
        || (name.ends_with(".bin") && name.contains("globalshadercache"))
}

fn payload(name: &str) -> Payload {
    if is_library(name) {
        Payload::Library
    } else {
        Payload::Package
    }
}

fn legacy_entries<'a>(mut data: &'a [u8], footer: &Footer<'_>, count: usize,
                      entries: &mut Vec<Entry<'a>>) -> Option<usize> {
    let original = data.len();
    for _ in 0..count {
        let name = path(&mut data)?;
        let mut entry = entry(&mut data, footer, None)?;
        if !entry.deleted && shader_path(&name) {
            entry.payload = payload(&name);
            entries.push(entry);
        }
    }
    Some(original - data.len())
}

fn index_reference(data: &mut &[u8]) -> Option<Option<Index>> {
    if number(data)? == 0 { Some(None) } else { Some(Some(Index::parse(data)?)) }
}

fn directory_locations(mut data: &[u8], mut visit: impl FnMut(i32, Payload)) -> Option<()> {
    let dirs = number(&mut data)? as usize;
    if dirs == 0x50464451 {
        let dirs = number(&mut data)? as usize;
        let files = number(&mut data)? as usize;
        let restart = number(&mut data)? as usize;
        let dir_bytes = number(&mut data)? as usize;
        let file_bytes = number(&mut data)? as usize;
        let hashes = number(&mut data)? as usize;
        if restart == 0 { return None; }
        take(&mut data, 4)?;
        take(&mut data, hashes.checked_mul(12)?)?;
        take(&mut data, (dirs.div_ceil(restart) + 1).checked_mul(4)?)?;
        take(&mut data, (dirs + 1).checked_mul(4)?)?;
        let mut offsets = take(&mut data, (files + 1).checked_mul(4)?)?;
        let mut locations = take(&mut data, files.checked_mul(4)?)?;
        take(&mut data, dir_bytes)?;
        let names = take(&mut data, file_bytes)?;
        let mut start = number(&mut offsets)? as usize;
        for _ in 0..files {
            let end = number(&mut offsets)? as usize;
            let name = std::str::from_utf8(names.get(start..end)?).ok()?;
            let location = number(&mut locations)? as i32;
            if shader_path(name) { visit(location, payload(name)); }
            start = end;
        }
        return Some(());
    }
    for _ in 0..dirs {
        path(&mut data)?;
        let count = number(&mut data)?;
        for _ in 0..count {
            let name = path(&mut data)?;
            let location = number(&mut data)? as i32;
            if shader_path(&name) { visit(location, payload(&name)); }
        }
    }
    Some(())
}

fn modern_entries<'a>(mut data: &'a [u8], pak: &'a [u8], footer: &Footer<'_>, key: Option<&PakKey>,
                      entries: &mut Vec<Entry<'a>>) -> Option<u64> {
    take(&mut data, 8)?; // Path hash seed.
    let path_index = index_reference(&mut data)?;
    let directory = index_reference(&mut data)?;
    let encoded_size = number(&mut data)? as usize;
    let encoded = take(&mut data, encoded_size)?;
    let count = number(&mut data)? as usize;
    if count > data.len() / 53 { return None; }
    let mut full = Vec::new();
    for _ in 0..count {
        full.push(entry(&mut data, footer, None)?);
    }

    let mut unreadable = 0;
    let mut visit = |location: i32, payload: Payload| {
        if location == i32::MIN { return; }
        let item = if location >= 0 {
            (|| {
                let mut encoded = encoded.get(location as usize..)?;
                let flags = number(&mut encoded)?;
                if flags & 63 == 63 { number(&mut encoded)?; }
                let start = if flags >> 31 == 1 { number(&mut encoded)? as usize } else { offset(&mut encoded)? };
                // The complete entry is also serialized at the start of the payload.
                entry(&mut pak.get(start..footer.index.start)?, footer, Some(start))
            })()
        } else {
            full.get((-location - 1) as usize).copied()
        };
        match item {
            Some(mut entry) if !entry.deleted => {
                entry.payload = payload;
                entries.push(entry);
            }
            Some(_) => {}
            None => unreadable += 1,
        }
    };
    let mut scratch = Vec::new();
    if let Some(directory) = directory {
        let index = directory.read(&pak[..footer.end], footer.encrypted, key, &mut scratch)?;
        if !directory.matches(index) && !directory.matches(pak.get(directory.start..directory.start + directory.size)?) {
            return None;
        }
        directory_locations(index, visit)?;
    } else {
        let path_index = path_index?;
        let mut index = path_index.read(&pak[..footer.end], footer.encrypted, key, &mut scratch)?;
        if !path_index.matches(index) && !path_index.matches(pak.get(path_index.start..path_index.start + path_index.size)?) {
            return None;
        }
        let count = number(&mut index)?;
        for _ in 0..count {
            take(&mut index, 8)?;
            visit(number(&mut index)? as i32, Payload::Package);
        }
    }
    Some(unreadable)
}

pub(crate) fn scan(pak: &[u8], key: Option<&PakKey>, oodle: Option<Oodle>,
                   mut found: impl FnMut(Payload, &[u8])) -> PakScan {
    let mut result = PakScan::default();
    let Some(footer) = Footer::parse(pak) else { return result };
    result.locked = footer.encrypted;
    let mut index_scratch = Vec::new();
    let Some(index) = footer.index.read(pak, footer.encrypted, key, &mut index_scratch) else { return result };
    let start = footer.index.start;
    let valid_hash = footer.index.matches(index) || footer.index.matches(&pak[start..start + footer.index.size]);
    let mut packages = Vec::new();
    if footer.frozen {
        if !valid_hash { return result; }
        result.locked = false;
        let Some(unreadable) = frozen_entries(index, pak, &footer, &mut packages) else { return result };
        result.unreadable += unreadable;
    } else {
        let mut data = index;
        if path(&mut data).is_none() { return result; }
        let Some(count) = number(&mut data) else { return result };
        if footer.version >= 10 {
            if !valid_hash { return result; }
            result.locked = false;
            let Some(unreadable) = modern_entries(data, pak, &footer, key, &mut packages) else { return result };
            result.unreadable += unreadable;
        } else {
            let Some(consumed) = legacy_entries(data, &footer, count as usize, &mut packages) else { return result };
            if !valid_hash && !footer.index.matches(&index[..index.len() - data.len() + consumed]) { return result; }
        }
    }
    result.opened = true;
    result.locked = false;
    let (mut out, mut scratch) = (Vec::new(), Vec::new());
    for e in packages {
        let large = e.size > REUSABLE_PACKAGE;
        // Large packages are rare and should not expand every scan worker's buffer.
        let _guard = large.then(|| LARGE_PACKAGE.lock().unwrap());
        let mut large_out = Vec::new();
        let output = if large { &mut large_out } else { &mut out };
        if footer.method(e.method).and_then(|method| package(&pak[..start], e, method, key, oodle, output, &mut scratch)).is_some() {
            result.packages += 1;
            found(e.payload, output);
        } else {
            result.unreadable += 1;
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_packages_above_the_reusable_buffer_limit() {
        let size = REUSABLE_PACKAGE + 1;
        let mut pak = vec![0; size];
        pak[size - 1] = 42;
        let entry = Entry { start: 0, compressed: size, size, method: 0, block_size: 0,
                            encrypted: false, blocks: &[], block_base: 0, deleted: false,
                            whole_stream: false, payload: Payload::Package };
        let mut out = Vec::new();
        assert!(package(&pak, entry, b"None", None, None, &mut out, &mut Vec::new()).is_some());
        assert_eq!(out.len(), size);
        assert_eq!(out.last(), Some(&42));
    }

    #[test]
    fn reads_pre_block_and_frozen_pak_indexes() {
        use std::io::Write;

        let payload = b"shader payload";
        let mut zlib = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::fast());
        zlib.write_all(payload).unwrap();
        let compressed = zlib.finish().unwrap();
        for version in [1u32, 2, 9] {
            let frozen = version == 9;
            let stored = if frozen { payload.as_slice() } else { &compressed };
            let mut header = vec![0; 8]; // Payload offset.
            header.extend_from_slice(&(stored.len() as u64).to_le_bytes());
            header.extend_from_slice(&(payload.len() as u64).to_le_bytes());
            header.extend_from_slice(&u32::from(!frozen).to_le_bytes());
            if version == 1 { header.extend_from_slice(&[0; 8]); }
            header.extend_from_slice(&Sha1::digest(payload));
            if frozen { header.extend_from_slice(&[0; 5]); }
            let mut pak = header.clone();
            pak.extend_from_slice(stored);
            let index_start = pak.len() as u64;
            let mut index = if frozen {
                let mut index = vec![0; 112];
                index[16..24].copy_from_slice(&33u64.to_le_bytes()); // Relative 16, frozen flag.
                index[24..28].copy_from_slice(&1u32.to_le_bytes());
                index[28..32].copy_from_slice(&1u32.to_le_bytes());
                index
            } else {
                let mut index = vec![0; 4]; // Empty mount point.
                index.extend_from_slice(&1u32.to_le_bytes());
                index.extend_from_slice(&10u32.to_le_bytes());
                index.extend_from_slice(b"test.uexp\0");
                index.extend_from_slice(&header);
                index
            };
            if frozen { index[32 + 8..32 + 16].copy_from_slice(&(stored.len() as u64).to_le_bytes()); }
            pak.extend_from_slice(&index);
            if frozen { pak.extend_from_slice(&[0; 17]); }
            pak.extend_from_slice(&MAGIC.to_le_bytes());
            pak.extend_from_slice(&version.to_le_bytes());
            pak.extend_from_slice(&index_start.to_le_bytes());
            pak.extend_from_slice(&(index.len() as u64).to_le_bytes());
            pak.extend_from_slice(&Sha1::digest(&index));
            if frozen {
                pak.push(1);
                pak.extend_from_slice(&[0; 160]);
            }
            let result = scan(&pak, None, None, |_, code| assert_eq!(code, payload));
            assert_eq!((result.opened, result.packages, result.unreadable), (true, 1, 0), "v{version}");
        }
    }

    #[test]
    fn reads_modern_encoded_entry_with_relative_zlib_blocks() {
        use std::io::Write;

        fn field(data: &mut Vec<u8>, n: u32) { data.extend_from_slice(&n.to_le_bytes()); }
        fn wide(data: &mut Vec<u8>, n: usize) { data.extend_from_slice(&(n as u64).to_le_bytes()); }
        fn string(data: &mut Vec<u8>, value: &[u8]) {
            field(data, value.len() as u32 + 1);
            data.extend_from_slice(value);
            data.push(0);
        }

        let payload = b"shader library";
        let mut zlib = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
        zlib.write_all(payload).unwrap();
        let compressed = zlib.finish().unwrap();
        let mut pak = vec![0u8; 22];
        let start = pak.len();
        wide(&mut pak, 0);
        wide(&mut pak, compressed.len());
        wide(&mut pak, payload.len());
        field(&mut pak, 2);
        pak.extend_from_slice(&Sha1::digest(payload));
        field(&mut pak, 1);
        wide(&mut pak, 73);
        wide(&mut pak, 73 + compressed.len());
        pak.push(0);
        field(&mut pak, 65536);
        pak.extend_from_slice(&compressed);

        let mut directory = Vec::new();
        field(&mut directory, 1);
        string(&mut directory, b"/");
        field(&mut directory, 1);
        string(&mut directory, b"ShaderArchive-PCD3D_SM6.ushaderbytecode");
        field(&mut directory, 0);

        let mut index = Vec::new();
        string(&mut index, b"../../../");
        field(&mut index, 1);
        wide(&mut index, 0);
        field(&mut index, 0);
        field(&mut index, 1);
        let directory_offset = index.len();
        wide(&mut index, 0);
        wide(&mut index, directory.len());
        index.extend_from_slice(&Sha1::digest(&directory));
        field(&mut index, 16);
        field(&mut index, 0xe1000060);
        field(&mut index, start as u32);
        field(&mut index, payload.len() as u32);
        field(&mut index, compressed.len() as u32);
        field(&mut index, 0);

        let index_start = pak.len();
        let directory_start = index_start + index.len();
        index[directory_offset..directory_offset + 8]
            .copy_from_slice(&(directory_start as u64).to_le_bytes());
        pak.extend_from_slice(&index);
        pak.extend_from_slice(&directory);
        pak.extend_from_slice(&[0; 17]);
        field(&mut pak, MAGIC);
        field(&mut pak, 11);
        wide(&mut pak, index_start);
        wide(&mut pak, index.len());
        pak.extend_from_slice(&Sha1::digest(&index));
        let methods = pak.len();
        pak.resize(methods + 160, 0);
        pak[methods + 32..methods + 36].copy_from_slice(b"Zlib");

        let result = scan(&pak, None, None, |kind, bytes| {
            assert!(matches!(kind, Payload::Library));
            assert_eq!(bytes, payload);
        });
        assert_eq!((result.opened, result.packages, result.unreadable), (true, 1, 0));
    }
}
