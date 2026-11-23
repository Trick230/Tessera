// SPDX-License-Identifier: GPL-3.0-or-later
//! Serialized shader libraries and the shader groups referenced by IoStore.

use crate::oodle::Oodle;
use crate::unreal_pak::{decode_block, number, offset, take};

const MAX_CODE: usize = 64 << 20;

#[derive(Clone, Copy)]
pub(crate) enum Payload {
    Package,
    Library,
    Shader,
}

pub(crate) fn is_library(name: &str) -> bool {
    name.rsplit_once('.').is_some_and(|(_, ext)| ext.eq_ignore_ascii_case("ushaderbytecode"))
}

fn array<'a>(data: &mut &'a [u8], width: usize) -> Option<&'a [u8]> {
    let count = number(data)? as usize;
    take(data, count.checked_mul(width)?)
}

fn decompress<'a>(code: &'a [u8], raw: usize, oodle: Option<Oodle>, out: &'a mut Vec<u8>) -> Option<&'a [u8]> {
    if raw == 0 || raw > MAX_CODE || code.is_empty() { return None; }
    if code.len() == raw { return Some(code); }
    for method in [b"Zlib".as_slice(), b"Zstd", b"Oodle", b"LZ4"] {
        out.clear();
        if decode_block(method, code, raw, oodle, out).is_some() {
            return Some(out);
        }
    }
    None
}

pub(crate) fn scan_file(mut data: &[u8], oodle: Option<Oodle>,
                        mut shader: impl FnMut(&[u8])) -> Option<u64> {
    let (entries, stride, hash_size) = match number(&mut data)? {
        // Early UE4 libraries serialized a map from SHA-1 to code entry.
        1 => (array(&mut data, 37)?, 37, 20),
        2 => (serialized_entries(&mut data)?, 17, 0),
        _ => return None,
    };
    let mut out = Vec::new();
    for entry in entries.chunks_exact(stride) {
        let code = (|| {
            let mut entry = &entry[hash_size..];
            let start = offset(&mut entry)?;
            let size = number(&mut entry)? as usize;
            let raw = number(&mut entry)? as usize;
            Some((data.get(start..start.checked_add(size)?)?, raw))
        })();
        if let Some(code) = code.and_then(|(code, raw)| decompress(code, raw, oodle, &mut out)) {
            shader(code);
        }
    }
    Some((entries.len() / stride) as u64)
}

fn serialized_entries<'a>(data: &mut &'a [u8]) -> Option<&'a [u8]> {
    let map_hashes = array(data, 20)?;
    let shader_hashes = array(data, 20)?;
    let maps = array(data, 16)?;
    let entries = array(data, 17)?;
    array(data, 16)?; // Preload ranges.
    array(data, 4)?; // Shader-map indices.
    (map_hashes.len() / 20 == maps.len() / 16 && shader_hashes.len() / 20 == entries.len() / 17)
        .then_some(entries)
}

pub(crate) struct Groups<'a> {
    ids: &'a [u8],
    shaders: &'a [u8],
    groups: &'a [u8],
    indices: &'a [u8],
}

impl<'a> Groups<'a> {
    pub(crate) fn parse(mut data: &'a [u8]) -> Option<Self> {
        if number(&mut data)? != 1 { return None; }
        let map_hashes = array(&mut data, 20)?;
        let shader_hashes = array(&mut data, 20)?;
        let ids = array(&mut data, 12)?;
        let maps = array(&mut data, 8)?;
        let shaders = array(&mut data, 8)?;
        let groups = array(&mut data, 16)?;
        let indices = array(&mut data, 4)?;
        if !data.is_empty() || map_hashes.len() / 20 != maps.len() / 8 ||
            shader_hashes.len() / 20 != shaders.len() / 8 || ids.len() / 12 != groups.len() / 16 {
            return None;
        }
        Some(Self { ids, shaders, groups, indices })
    }

    pub(crate) fn expected(&self) -> u64 { (self.shaders.len() / 8) as u64 }

    pub(crate) fn ids(&self) -> impl Iterator<Item = &[u8]> {
        self.ids.chunks_exact(12)
    }

    pub(crate) fn scan_group(&self, group: usize, packed: &[u8], oodle: Option<Oodle>,
                             out: &mut Vec<u8>, mut shader: impl FnMut(&[u8])) -> Option<()> {
        let mut entry = self.groups.get(group * 16..(group + 1) * 16)?;
        let first = number(&mut entry)? as usize;
        let count = number(&mut entry)? as usize;
        let raw = number(&mut entry)? as usize;
        if number(&mut entry)? as usize != packed.len() { return None; }
        let indices = self.indices.get(first * 4..first.checked_add(count)?.checked_mul(4)?)?;
        let mut offsets = Vec::new();
        for mut index in indices.chunks_exact(4) {
            let index = number(&mut index)? as usize;
            let entry = self.shaders.get(index * 8..(index + 1) * 8)?;
            let descriptor = u64::from_le_bytes(entry.try_into().ok()?);
            let offset = (descriptor >> 34) as usize;
            if (descriptor >> 4) & 0x3fff_ffff != group as u64 || offset >= raw { return None; }
            offsets.push(offset);
        }
        offsets.sort_unstable();
        if offsets.windows(2).any(|offsets| offsets[0] == offsets[1]) { return None; }
        offsets.push(raw);
        let code = decompress(packed, raw, oodle, out)?;
        for span in offsets.windows(2) {
            shader(&code[span[0]..span[1]]);
        }
        Some(())
    }
}
