// SPDX-License-Identifier: GPL-3.0-or-later
//! Graphics APIs configured in a Unity player's serialized BuildSettings.

use crate::pe::ApiSet;
use std::fs::File;
use std::io::Read;
use std::path::Path;

pub(crate) fn graphics_apis(exe: &Path) -> Option<ApiSet> {
    let mut data_dir = exe.file_stem()?.to_os_string();
    data_dir.push("_Data");
    let mut data = Vec::new();
    File::open(exe.with_file_name(data_dir).join("globalgamemanagers")).ok()?
        .take(16 * 1024 * 1024).read_to_end(&mut data).ok()?;
    let (build_settings, unity_version) = build_settings(&data)?;
    build_graphics_apis(build_settings, unity_version)
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
