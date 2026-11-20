// SPDX-License-Identifier: GPL-3.0-or-later
//! Shared path helpers.

use std::path::PathBuf;
use walkdir::DirEntry;

/// Folders that only hold bulk content; skipped when looking for executables
/// and markers so a scan of a 100 GB install stays in the millisecond range.
pub fn is_bulk_dir(e: &DirEntry) -> bool {
    if !e.file_type().is_dir() || e.depth() == 0 {
        return false;
    }
    let n = e.file_name().to_string_lossy().to_ascii_lowercase();
    matches!(
        n.as_str(),
        "content" | "paks" | "movies" | "movie" | "sound" | "audio" | "streamingassets" | "__installer"
    )
}

/// `%LOCALAPPDATA%\Tessera`
pub fn data_dir() -> PathBuf {
    let base = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
    base.join("Tessera")
}
