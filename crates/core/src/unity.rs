// SPDX-License-Identifier: GPL-3.0-or-later
//! Graphics APIs configured in a Unity player's serialized BuildSettings.

use crate::pe::ApiSet;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

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

fn read_block(file: &mut impl Read, stored: usize, size: usize, flags: u32) -> Option<Vec<u8>> {
    if stored > MAX_METADATA || size > MAX_METADATA {
        return None;
    }
    let mut data = vec![0; stored];
    file.read_exact(&mut data).ok()?;
    match flags & 0x3f {
        0 if stored == size => Some(data),
        2 | 3 => {
            let decoded = lz4_flex::decompress(&data, size).ok()?;
            (decoded.len() == size).then_some(decoded)
        }
        _ => None,
    }
}

/// Read only the UnityFS directory and the blocks covering globalgamemanagers.
fn bundle_managers(mut file: impl Read + Seek) -> Option<Vec<u8>> {
    let mut header = [0; 128];
    let len = file.read(&mut header).ok()?;
    let mut fields = header[..len].strip_prefix(b"UnityFS\0")?;
    let version = be_u32(&mut fields)?;
    if !(6..=8).contains(&version) {
        return None;
    }
    c_string(&mut fields)?;
    c_string(&mut fields)?;
    let bundle_size = be_u64(&mut fields)?;
    let stored = be_u32(&mut fields)? as usize;
    let size = be_u32(&mut fields)? as usize;
    let flags = be_u32(&mut fields)?;
    if flags & 0x40 == 0 {
        return None;
    }
    let header_end = (len - fields.len()) as u64;
    let header_end = if version >= 7 { header_end.next_multiple_of(16) } else { header_end };
    let at_end = flags & 0x80 != 0;
    let table_start = if at_end { bundle_size.checked_sub(stored as u64)? } else { header_end };
    file.seek(SeekFrom::Start(table_start)).ok()?;
    let table = read_block(&mut file, stored, size, flags)?;
    let mut directory = table.get(16..)?; // Data hash.
    let count = be_u32(&mut directory)? as usize;
    let blocks = take(&mut directory, count.checked_mul(10)?)?;
    let count = be_u32(&mut directory)?;
    let mut data_start = if at_end { header_end } else { table_start + stored as u64 };
    if flags & 0x200 != 0 {
        data_start = data_start.next_multiple_of(16);
    }
    for _ in 0..count {
        let offset = be_u64(&mut directory)?;
        let size = be_u64(&mut directory)?;
        take(&mut directory, 4)?; // Node flags.
        if c_string(&mut directory)? == b"globalgamemanagers" {
            let mut output = Vec::new();
            let end = offset.checked_add(size.min(MAX_METADATA as u64))?;
            let mut raw_start = 0u64;
            for mut block in blocks.chunks_exact(10) {
                let raw = be_u32(&mut block)?;
                let stored = be_u32(&mut block)?;
                let raw_end = raw_start + u64::from(raw);
                if raw_end > offset && raw_start < end {
                    file.seek(SeekFrom::Start(data_start)).ok()?;
                    let flags = u16::from_be_bytes(block.try_into().ok()?) as u32;
                    let data = read_block(&mut file, stored as usize, raw as usize, flags)?;
                    output.extend_from_slice(data.get((offset.max(raw_start) - raw_start) as usize..
                        (end.min(raw_end) - raw_start) as usize)?);
                }
                if raw_end >= end {
                    return Some(output);
                }
                raw_start = raw_end;
                data_start += u64::from(stored);
            }
            return None;
        }
    }
    None
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
