// SPDX-License-Identifier: GPL-3.0-or-later
//! UnityFS bundle access and graphics APIs from serialized BuildSettings.

use crate::pe::ApiSet;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

mod shader;
pub(crate) mod smolv;

pub(crate) fn graphics_apis(exe: &Path) -> Option<ApiSet> {
    let mut data_dir = exe.file_stem()?.to_os_string();
    data_dir.push("_Data");
    let dir = exe.with_file_name(data_dir);
    let apis = |data: &[u8]| {
        let (settings, version) = build_settings(data)?;
        build_graphics_apis(settings, version)
    };
    let mut loose = Vec::new();
    File::open(dir.join("globalgamemanagers")).ok()
        .and_then(|file| file.take(MAX_METADATA as u64).read_to_end(&mut loose).ok())
        .and_then(|_| apis(&loose))
        .or_else(|| apis(&bundle_managers(File::open(dir.join("data.unity3d")).ok()?)?))
}

const MAX_METADATA: usize = 16 * 1024 * 1024;
const MAX_BLOCK: usize = 64 * 1024 * 1024;
const LEGACY_WEB: u32 = 0x1_0000;
const WEB_DATA: &[u8] = b"UnityWebData1.0\0";

pub(crate) fn web_entries(data: &[u8]) -> Option<Vec<&[u8]>> {
    let mut header = data.strip_prefix(WEB_DATA)?;
    let end = le_u32(&mut header)? as usize;
    if end > MAX_METADATA { return None; }
    let mut header = header.get(..end.checked_sub(WEB_DATA.len() + 4)?)?;
    let mut entries = Vec::new();
    while !header.is_empty() {
        let start = le_u32(&mut header)? as usize;
        let size = le_u32(&mut header)? as usize;
        let name_len = le_u32(&mut header)? as usize;
        take(&mut header, name_len)?;
        if start < end { return None; }
        entries.push(data.get(start..start.checked_add(size)?)?);
    }
    Some(entries)
}

fn be_u32(data: &mut &[u8]) -> Option<u32> {
    Some(u32::from_be_bytes(take(data, 4)?.try_into().ok()?))
}

fn be_u64(data: &mut &[u8]) -> Option<u64> {
    Some(u64::from_be_bytes(take(data, 8)?.try_into().ok()?))
}

fn c_string<'a>(data: &mut &'a [u8]) -> Option<&'a [u8]> {
    let len = memchr::memchr(0, data)?;
    Some(&take(data, len + 1)?[..len])
}

fn read_block(file: &mut impl Read, stored: usize, size: usize, flags: u32,
              limit: usize, compressed: &mut Vec<u8>, out: &mut Vec<u8>) -> Option<()> {
    if stored > limit || size > limit {
        return None;
    }
    match flags & 0x3f {
        0 if stored == size => {
            out.resize(size, 0);
            file.read_exact(out).ok()
        }
        1 => {
            compressed.resize(stored, 0);
            file.read_exact(compressed).ok()?;
            out.clear();
            let unpacked_size = if flags & LEGACY_WEB != 0 {
                lzma_rs::decompress::UnpackedSize::ReadHeaderButUseProvided(Some(size as u64))
            } else {
                lzma_rs::decompress::UnpackedSize::UseProvided(Some(size as u64))
            };
            let options = lzma_rs::decompress::Options {
                unpacked_size,
                memlimit: Some(MAX_BLOCK),
                ..Default::default()
            };
            lzma_rs::lzma_decompress_with_options(&mut compressed.as_slice(), out, &options).ok()?;
            (out.len() == size).then_some(())
        }
        2 | 3 => {
            compressed.resize(stored, 0);
            file.read_exact(compressed).ok()?;
            out.resize(size, 0);
            (lz4_flex::decompress_into(compressed, out).ok()? == size).then_some(())
        }
        _ => None,
    }
}

struct Block {
    stored_at: u64,
    stored: usize,
    raw_at: u64,
    raw: usize,
    flags: u32,
}

struct Node {
    offset: u64,
    size: u64,
    name: Box<[u8]>,
}

struct Bundle<R> {
    file: R,
    blocks: Vec<Block>,
    nodes: Vec<Node>,
    compressed: Vec<u8>,
    decoded: Vec<u8>,
    current: Option<usize>,
}

impl<R: Read + Seek> Bundle<R> {
    fn open(mut file: R) -> Option<Self> {
        let length = file.seek(SeekFrom::End(0)).ok()?;
        file.rewind().ok()?;
        let mut header = Vec::new();
        file.by_ref().take(512).read_to_end(&mut header).ok()?;
        let mut fields = header.as_slice();
        let signature = c_string(&mut fields)?;
        let version = be_u32(&mut fields)?;
        c_string(&mut fields)?;
        c_string(&mut fields)?;
        match signature {
            b"UnityFS" if (6..=9).contains(&version) => {}
            b"UnityWeb" | b"UnityRaw" if version == 6 => {}
            b"UnityWeb" | b"UnityRaw" if (1..=5).contains(&version) => {
                return Self::legacy(file, fields, length, version, signature == b"UnityWeb");
            }
            _ => return None,
        }
        let bundle_size = be_u64(&mut fields)?;
        let stored = be_u32(&mut fields)? as usize;
        let size = be_u32(&mut fields)? as usize;
        let flags = be_u32(&mut fields)?;
        if signature != b"UnityFS" { take(&mut fields, 1)?; }
        if flags & 0x40 == 0 || bundle_size > length { return None; }
        let header_end = (header.len() - fields.len()) as u64;
        let header_end = if version >= 7 { header_end.next_multiple_of(16) } else { header_end };
        let at_end = flags & 0x80 != 0;
        let table_start = if at_end { bundle_size.checked_sub(stored as u64)? } else { header_end };
        if table_start < header_end || table_start.checked_add(stored as u64)? > bundle_size {
            return None;
        }
        file.seek(SeekFrom::Start(table_start)).ok()?;
        let (mut compressed, mut table) = (Vec::new(), Vec::new());
        read_block(&mut file, stored, size, flags, MAX_METADATA, &mut compressed, &mut table)?;
        let mut directory = table.get(16..)?;
        let count = be_u32(&mut directory)? as usize;
        let descriptors = take(&mut directory, count.checked_mul(10)?)?;
        let mut blocks = Vec::new();
        let mut stored_at = if at_end { header_end } else { table_start + stored as u64 };
        if flags & 0x200 != 0 { stored_at = stored_at.next_multiple_of(16); }
        let data_end = if at_end { table_start } else { bundle_size };
        let mut raw_at = 0u64;
        for mut descriptor in descriptors.chunks_exact(10) {
            let raw = be_u32(&mut descriptor)? as usize;
            let stored = be_u32(&mut descriptor)? as usize;
            let flags = u16::from_be_bytes(descriptor.try_into().ok()?) as u32;
            let next = stored_at.checked_add(stored as u64)?;
            if next > data_end { return None; }
            blocks.push(Block { stored_at, stored, raw_at, raw, flags });
            stored_at = next;
            raw_at = raw_at.checked_add(raw as u64)?;
        }
        let mut nodes = Vec::new();
        for _ in 0..be_u32(&mut directory)? {
            let offset = be_u64(&mut directory)?;
            let size = be_u64(&mut directory)?;
            take(&mut directory, 4)?; // Node flags.
            let name = c_string(&mut directory)?.into();
            if offset.checked_add(size)? > raw_at { return None; }
            nodes.push(Node { offset, size, name });
        }
        Some(Self { file, blocks, nodes, compressed, decoded: Vec::new(), current: None })
    }

    fn legacy(file: R, mut fields: &[u8], length: u64, version: u32, web: bool) -> Option<Self> {
        if version >= 4 { take(&mut fields, 20)?; }
        take(&mut fields, 4)?; // Minimum streamed bytes.
        let stored_at = u64::from(be_u32(&mut fields)?);
        take(&mut fields, 4)?; // Levels needed before streaming.
        let levels = be_u32(&mut fields)? as usize;
        let mut last = take(&mut fields, levels.checked_mul(8)?)?.rchunks_exact(8).next()?;
        let stored = be_u32(&mut last)? as usize;
        let raw = be_u32(&mut last)? as usize;
        if stored_at.checked_add(stored as u64)? > length || (!web && stored != raw) { return None; }
        let flags = if web { LEGACY_WEB | 1 } else { 0 };
        let block = Block { stored_at, stored, raw_at: 0, raw, flags };
        let mut bundle = Self {
            file, blocks: vec![block], nodes: Vec::new(), compressed: Vec::new(),
            decoded: Vec::new(), current: None,
        };
        let mut table = Vec::new();
        bundle.read(0, raw.min(MAX_METADATA), &mut table)?;
        let mut directory = table.as_slice();
        for _ in 0..be_u32(&mut directory)? {
            let name = c_string(&mut directory)?.into();
            let offset = u64::from(be_u32(&mut directory)?);
            let size = u64::from(be_u32(&mut directory)?);
            if offset.checked_add(size)? > raw as u64 { return None; }
            bundle.nodes.push(Node { offset, size, name });
        }
        Some(bundle)
    }

    fn read(&mut self, start: u64, size: usize, out: &mut Vec<u8>) -> Option<()> {
        out.clear();
        let end = start.checked_add(size as u64)?;
        let first = self.blocks.partition_point(|block| block.raw_at + block.raw as u64 <= start);
        for index in first..self.blocks.len() {
            let block = &self.blocks[index];
            if block.raw_at >= end { break; }
            let from = start.saturating_sub(block.raw_at) as usize;
            let to = (end - block.raw_at).min(block.raw as u64) as usize;
            if block.flags & 0x3f == 0 && block.stored == block.raw {
                self.file.seek(SeekFrom::Start(block.stored_at + from as u64)).ok()?;
                let begin = out.len();
                out.resize(begin + to - from, 0);
                self.file.read_exact(&mut out[begin..]).ok()?;
                continue;
            }
            if self.current != Some(index) {
                self.current = None;
                self.file.seek(SeekFrom::Start(block.stored_at)).ok()?;
                read_block(&mut self.file, block.stored, block.raw, block.flags, MAX_BLOCK,
                           &mut self.compressed, &mut self.decoded)?;
                self.current = Some(index);
            }
            out.extend_from_slice(self.decoded.get(from..to)?);
        }
        (out.len() == size).then_some(())
    }
}

/// Read only the UnityFS directory and the blocks covering globalgamemanagers.
fn bundle_managers(file: impl Read + Seek) -> Option<Vec<u8>> {
    let mut bundle = Bundle::open(file)?;
    let node = bundle.nodes.iter().find(|node| node.name.as_ref() == b"globalgamemanagers")?;
    let mut output = Vec::new();
    bundle.read(node.offset, node.size.min(MAX_METADATA as u64) as usize, &mut output)?;
    Some(output)
}

pub(crate) fn scan_bundle(data: &[u8], decode_spirv: bool, mut visit: impl FnMut(&[u8], bool))
    -> Option<(u64, u64, u64, u64)>
{
    let mut bundle = Bundle::open(std::io::Cursor::new(data))?;
    bundle.nodes.sort_unstable_by_key(|node| node.offset);
    let mut out = Vec::new();
    let mut object = Vec::new();
    let mut decoded = Vec::new();
    let (mut read, mut unreadable) = (0, 0);
    let (mut shaders, mut unreadable_shaders) = (0, 0);
    const WINDOW: u64 = 16 << 20;
    const OVERLAP: u64 = 1 << 20;
    for index in 0..bundle.nodes.len() {
        let node = &bundle.nodes[index];
        let (start, length) = (node.offset, node.size);
        let end = start + length;
        if let Some((found, failed)) = scan_serialized(length, |offset, size, consume| {
            if size > MAX_BLOCK { return None; }
            bundle.read(start + offset, size, &mut object)?;
            consume(&object);
            Some(())
        }, decode_spirv, &mut decoded, &mut visit) {
            shaders += found;
            unreadable_shaders += failed;
        }
        let mut offset = start;
        let mut complete = true;
        while offset < end {
            let size = WINDOW.min(end - offset);
            if bundle.read(offset, size as usize, &mut out).is_some() {
                visit(&out, false);
            } else {
                complete = false;
            }
            if size == end - offset { break; }
            offset += WINDOW - OVERLAP;
        }
        read += u64::from(complete);
        unreadable += u64::from(!complete);
    }
    Some((read, unreadable, shaders, unreadable_shaders))
}

pub(crate) fn scan_serialized_slice(data: &[u8], decode_spirv: bool, mut visit: impl FnMut(&[u8], bool)) -> Option<(u64, u64)> {
    scan_serialized(data.len() as u64, |offset, size, consume| {
        let start = usize::try_from(offset).ok()?;
        consume(data.get(start..start.checked_add(size)?)?);
        Some(())
    }, decode_spirv, &mut Vec::new(), &mut visit)
}

fn scan_serialized(
    length: u64,
    mut read: impl FnMut(u64, usize, &mut dyn FnMut(&[u8])) -> Option<()>,
    decode_spirv: bool,
    decoded: &mut Vec<u8>,
    visit: &mut impl FnMut(&[u8], bool),
) -> Option<(u64, u64)> {
    let mut header = None;
    read(0, length.min(48) as usize,
         &mut |bytes| header = shader::SerializedFile::open(bytes, length))?;
    let header = header?;
    let mut index = None;
    read(header.header_size as u64, header.metadata_size,
         &mut |bytes| index = header.shaders(bytes))?;
    let (layout, objects) = index?;
    let found = objects.len() as u64;
    let mut failed = 0;
    let mut spirv = Vec::new();
    for object in objects {
        if object.offset.checked_add(object.size as u64).is_none_or(|end| end > length) {
            failed += 1;
            continue;
        }
        let mut complete = false;
        read(object.offset, object.size, &mut |bytes| {
            let mut program = |code: &[u8]| {
                visit(code, true);
                if !decode_spirv { return; }
                for start in memchr::memmem::find_iter(code, smolv::MAGIC) {
                    if smolv::decode(&code[start..], layout.legacy_smolv(), &mut spirv).is_some() {
                        visit(&spirv, true);
                    }
                }
            };
            complete = if object.has_shader_blob {
                layout.scan(bytes, decoded, &mut program).is_some()
            } else {
                // Compute and ray-tracing objects use inline program arrays.
                program(bytes);
                true
            };
        });
        failed += u64::from(!complete);
    }
    Some((found, failed))
}

fn take<'a>(data: &mut &'a [u8], len: usize) -> Option<&'a [u8]> {
    let (head, tail) = data.split_at_checked(len)?;
    *data = tail;
    Some(head)
}

fn le_u32(data: &mut &[u8]) -> Option<u32> {
    Some(u32::from_le_bytes(take(data, 4)?.try_into().ok()?))
}

fn build_settings(data: &[u8]) -> Option<(&[u8], &[u8])> {
    let format = u32::from_be_bytes(data.get(8..12)?.try_into().ok()?);
    if !(17..=22).contains(&format) || *data.get(16)? != 0 {
        return None;
    }
    let (header_len, metadata_len, data_offset) = if format == 22 {
        (48usize, u32::from_be_bytes(data.get(20..24)?.try_into().ok()?) as usize,
            usize::try_from(u64::from_be_bytes(data.get(32..40)?.try_into().ok()?)).ok()?)
    } else {
        (20, u32::from_be_bytes(data.get(..4)?.try_into().ok()?) as usize,
            u32::from_be_bytes(data.get(12..16)?.try_into().ok()?) as usize)
    };
    let metadata_end = header_len.checked_add(metadata_len)?;
    if metadata_end > data_offset {
        return None;
    }
    let metadata = data.get(header_len..metadata_end)?;
    let (unity_version, mut reader) = metadata.split_at_checked(memchr::memchr(0, metadata)?)?;
    if unity_version.is_empty() {
        return None;
    }
    take(&mut reader, 5)?; // Version terminator and build target.
    let has_type_tree = take(&mut reader, 1)?[0] != 0;
    let type_count = le_u32(&mut reader)? as usize;
    if type_count > reader.len() / 23 {
        return None;
    }
    let mut build_settings_type = None;
    for index in 0..type_count {
        let class = le_u32(&mut reader)?;
        take(&mut reader, if class == 114 { 35 } else { 19 })?;
        if class == 141 {
            build_settings_type = Some(index);
        }
        if has_type_tree {
            let nodes = le_u32(&mut reader)? as usize;
            let strings = le_u32(&mut reader)? as usize;
            let node_size = if format >= 19 { 32 } else { 24 };
            take(&mut reader, nodes.checked_mul(node_size)?.checked_add(strings)?)?;
            if format >= 21 {
                let dependencies = le_u32(&mut reader)? as usize;
                take(&mut reader, dependencies.checked_mul(4)?)?;
            }
        }
    }
    let build_settings_type = build_settings_type?;
    let object_count = le_u32(&mut reader)?;
    for _ in 0..object_count {
        let position = metadata.len() - reader.len();
        take(&mut reader, position.next_multiple_of(4) - position)?;
        take(&mut reader, 8)?; // Path ID.
        let offset = if format == 22 {
            usize::try_from(u64::from_le_bytes(take(&mut reader, 8)?.try_into().ok()?)).ok()?
        } else {
            le_u32(&mut reader)? as usize
        };
        let size = le_u32(&mut reader)? as usize;
        let object_type = le_u32(&mut reader)? as usize;
        if object_type == build_settings_type {
            let start = data_offset.checked_add(offset)?;
            return Some((data.get(start..start.checked_add(size)?)?, unity_version));
        }
    }
    None
}

fn build_graphics_apis(settings: &[u8], version: &[u8]) -> Option<ApiSet> {
    // In BuildSettings the API array follows m_Version, with m_AuthToken in some Unity versions.
    for offset in memchr::memmem::find_iter(settings, version) {
        let Some(prefix) = offset.checked_sub(4).filter(|start| start % 4 == 0) else { continue };
        if le_u32(&mut &settings[prefix..offset])? as usize != version.len() {
            continue;
        }
        let mut tail = settings.get((offset + version.len()).next_multiple_of(4)..)?;
        if let Some(apis) = read_api_array(tail) {
            return Some(apis);
        }
        let len = le_u32(&mut tail)? as usize;
        take(&mut tail, len.checked_add(3)? & !3)?;
        if let Some(apis) = read_api_array(tail) {
            return Some(apis);
        }
        if version.starts_with(b"2019.2.") {
            // Unity 2019.2 serializes runtime class hashes and script hashes before m_GraphicsAPIs.
            for entry_size in [20, 32] {
                let count = le_u32(&mut tail)? as usize;
                take(&mut tail, count.checked_mul(entry_size)?)?;
            }
            return read_api_array(tail);
        }
    }
    None
}

fn read_api_array(mut data: &[u8]) -> Option<ApiSet> {
    let count = le_u32(&mut data)? as usize;
    if count.checked_mul(4)? != data.len() {
        return None;
    }
    let mut apis = ApiSet::default();
    for _ in 0..count {
        apis.0 |= match le_u32(&mut data)? {
            0 | 17 => ApiSet::OPENGL,
            1 => ApiSet::D3D9,
            2 => ApiSet::D3D11,
            18 => ApiSet::D3D12,
            21 => ApiSet::VULKAN,
            _ => 0,
        };
    }
    Some(apis)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_graphics_apis_after_unity_2019_build_hashes() {
        let mut settings = b"\x0b\0\0\x002019.2.21f1\0".to_vec();
        settings.extend_from_slice(&4u32.to_le_bytes());
        settings.extend_from_slice(b"test");
        for entry_size in [20, 32] {
            settings.extend_from_slice(&1u32.to_le_bytes());
            settings.extend(std::iter::repeat_n(0x42, entry_size));
        }
        settings.extend_from_slice(&1u32.to_le_bytes());
        settings.extend_from_slice(&2u32.to_le_bytes());
        assert_eq!(build_graphics_apis(&settings, b"2019.2.21f1"), Some(ApiSet(ApiSet::D3D11)));
    }
}
