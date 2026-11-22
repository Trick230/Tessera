// SPDX-License-Identifier: GPL-3.0-or-later
//! Read-only Unreal v4 PAK input for the inline shader scan. The package
//! selection follows SCSKiller's UnrealReader/InlineShaders (GPL-3.0-or-later).

use std::io::Read;
use std::path::Path;

use aes::cipher::{BlockDecrypt, KeyInit};
use sha1::{Digest, Sha1};

use crate::oodle::Oodle;

const MAX_INDEX: usize = 64 << 20;
const MAX_PACKAGE: usize = 64 << 20;
const FOOTER: usize = 45;

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
        Some(Self(aes::Aes256::new((&key).into())))
    }

    fn decode(&self, bytes: &mut [u8]) -> Option<()> {
        if bytes.len() % 16 != 0 { return None; }
        for block in bytes.chunks_exact_mut(16) {
            self.0.decrypt_block(aes::cipher::Block::<aes::Aes256>::from_mut_slice(block));
        }
        Some(())
    }
}

fn take<'a>(data: &mut &'a [u8], size: usize) -> Option<&'a [u8]> {
    let (value, remaining) = data.split_at_checked(size)?;
    *data = remaining;
    Some(value)
}

fn number(data: &mut &[u8]) -> Option<u32> {
    Some(u32::from_le_bytes(take(data, 4)?.try_into().ok()?))
}

fn offset(data: &mut &[u8]) -> Option<usize> {
    usize::try_from(u64::from_le_bytes(take(data, 8)?.try_into().ok()?)).ok()
}

fn path(data: &mut &[u8]) -> Option<String> {
    let len = number(data)? as i32;
    let chars = usize::try_from(len.unsigned_abs()).ok()?;
    if !(1..=32768).contains(&chars) { return None; }
    if len < 0 {
        let bytes = take(data, chars * 2)?;
        let text: Vec<_> = bytes.chunks_exact(2).map(|b| u16::from_le_bytes([b[0], b[1]])).collect();
        String::from_utf16(text.strip_suffix(&[0])?).ok()
    } else {
        Some(std::str::from_utf8(take(data, chars)?.strip_suffix(&[0])?).ok()?.to_owned())
    }
}

struct Entry<'a> {
    start: usize,
    compressed: usize,
    size: usize,
    method: u32,
    block_size: usize,
    encrypted: bool,
    blocks: &'a [u8],
}

fn entry<'a>(data: &mut &'a [u8]) -> Option<Entry<'a>> {
    let original = data.len();
    let start = offset(data)?;
    let compressed = offset(data)?;
    let size = offset(data)?;
    let method = number(data)?;
    take(data, 20)?; // data hash
    let blocks = if method == 0 { &[] } else {
        let count = number(data)? as usize;
        take(data, count.checked_mul(16)?)?
    };
    let encrypted = take(data, 1)?[0] & 1 != 0;
    let block_size = number(data)? as usize;
    // In v4 compressed-block offsets are absolute. Stored entries follow their header.
    let start = if method == 0 { start.checked_add(original - data.len())? } else { start };
    Some(Entry { start, compressed, size, method, block_size, encrypted, blocks })
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

fn package(pak: &[u8], e: Entry<'_>, key: Option<&PakKey>, oodle: Option<Oodle>,
           out: &mut Vec<u8>, scratch: &mut Vec<u8>) -> Option<()> {
    out.clear();
    if e.size == 0 || e.size > MAX_PACKAGE || e.compressed > MAX_PACKAGE { return None; }
    if e.method == 0 {
        if e.compressed != e.size { return None; }
        out.extend_from_slice(stored(pak, e.start, e.size, e.encrypted, key, scratch)?);
        return Some(());
    }
    if e.block_size == 0 || e.blocks.len() / 16 != e.size.div_ceil(e.block_size) { return None; }
    for mut block in e.blocks.chunks_exact(16) {
        let start = offset(&mut block)?;
        let len = offset(&mut block)?.checked_sub(start)?;
        if len > MAX_PACKAGE { return None; }
        let bytes = stored(pak, start, len, e.encrypted, key, scratch)?;
        let raw = e.block_size.min(e.size - out.len());
        let begin = out.len();
        if e.method & 1 != 0 {
            flate2::read::ZlibDecoder::new(bytes).take(raw as u64 + 1).read_to_end(out).ok()?;
        } else if e.method & 2 != 0 {
            flate2::read::GzDecoder::new(bytes).take(raw as u64 + 1).read_to_end(out).ok()?;
        } else if e.method & 4 != 0 {
            if !oodle?.decode_exact(bytes, raw, out) { return None; }
        } else {
            return None;
        }
        if out.len() - begin != raw { return None; }
    }
    Some(())
}

#[derive(Default)]
pub(crate) struct PakScan {
    pub opened: bool,
    pub locked: bool,
    pub packages: u64,
    pub unreadable: u64,
}

pub(crate) fn scan(pak: &[u8], key: Option<&PakKey>, oodle: Option<Oodle>,
                   mut found: impl FnMut(&[u8])) -> PakScan {
    let mut result = PakScan::default();
    let Some(mut footer) = pak.get(pak.len().saturating_sub(FOOTER)..).filter(|b| b.len() == FOOTER) else { return result };
    if footer[0] > 1 { return result; }
    let encrypted = footer[0] == 1;
    footer = &footer[1..];
    if number(&mut footer) != Some(0x5a6f12e1) || number(&mut footer) != Some(4) { return result; }
    result.locked = encrypted;
    let Some(start) = offset(&mut footer) else { return result };
    let Some(size) = offset(&mut footer).filter(|&n| n <= MAX_INDEX) else { return result };
    let Some(bytes) = start.checked_add(size).filter(|&end| end <= pak.len() - FOOTER)
        .and_then(|end| pak.get(start..end)) else { return result };
    let mut index = bytes.to_vec();
    if encrypted && key.and_then(|k| k.decode(&mut index)).is_none() { return result; }
    let mut data = index.as_slice();
    if path(&mut data).is_none() { return result; }
    let Some(count) = number(&mut data).filter(|&n| n as usize <= data.len() / 57) else { return result };
    let mut packages = Vec::new();
    for _ in 0..count {
        let Some(name) = path(&mut data) else { return result };
        let Some(e) = entry(&mut data) else { return result };
        let name = name.to_ascii_lowercase();
        if name.ends_with(".uexp") || name.ends_with(".uasset") || name.ends_with(".ushaderbytecode") ||
           (name.ends_with(".bin") && name.contains("globalshadercache")) {
            packages.push(e);
        }
    }
    let consumed = index.len() - data.len();
    if Sha1::digest(bytes)[..] != *footer && Sha1::digest(&index)[..] != *footer &&
       Sha1::digest(&index[..consumed])[..] != *footer { return result; }
    result.opened = true;
    result.locked = false;
    let (mut out, mut scratch) = (Vec::new(), Vec::new());
    for e in packages {
        if package(&pak[..start], e, key, oodle, &mut out, &mut scratch).is_some() {
            result.packages += 1;
            found(&out);
        } else {
            result.unreadable += 1;
        }
    }
    result
}
