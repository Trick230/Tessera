// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::io::Read;
use std::os::windows::fs::MetadataExt;
use std::path::Path;

/// Checks the TIO1 header and executable identity (src/dll/stream_io.cpp).
/// The layer validates the individual asset files when opening the trace.
pub fn has_trace(exe: &Path) -> bool {
    let Some(key) = crate::session::exe_key_name(exe) else {
        return false;
    };
    let Ok(meta) = fs::metadata(exe) else {
        return false;
    };
    let path = crate::paths::data_dir()
        .join("streams")
        .join(format!("{key}.tiot"));
    let Ok(mut file) = fs::File::open(path) else {
        return false;
    };
    let mut header = [0u8; 32];
    if !file.metadata().is_ok_and(|m| m.len() == 166944) || file.read_exact(&mut header).is_err() {
        return false;
    }
    let u32_at = |offset| u32::from_le_bytes(header[offset..offset + 4].try_into().unwrap());
    let u64_at = |offset| u64::from_le_bytes(header[offset..offset + 8].try_into().unwrap());
    &header[..4] == b"TIO1"
        && (1..=128).contains(&u32_at(4))
        && (1..=4096).contains(&u32_at(8))
        && u64_at(16) == meta.len()
        && u64_at(24) == meta.last_write_time()
}
