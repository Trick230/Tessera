// SPDX-License-Identifier: GPL-3.0-or-later
//! Engine detection from the install layout.

use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Engine {
    Unreal { io_store: bool },
    Unity,
    ReEngine,
    Unknown,
}

impl Engine {
    pub fn label(self) -> &'static str {
        match self {
            Engine::Unreal { io_store: true } => "Unreal (IoStore)",
            Engine::Unreal { io_store: false } => "Unreal",
            Engine::Unity => "Unity",
            Engine::ReEngine => "RE Engine",
            Engine::Unknown => "Unknown",
        }
    }
}

pub fn detect(root: &Path) -> Engine {
    if root.join("Engine").is_dir() {
        return Engine::Unreal { io_store: has_io_store(root) };
    }
    if root.join("UnityPlayer.dll").is_file() {
        return Engine::Unity;
    }
    if root.join("re_chunk_000.pak").is_file() {
        return Engine::ReEngine;
    }
    Engine::Unknown
}

/// `<Project>/Content/Paks/*.utoc` means UE 4.26+ IoStore containers.
fn has_io_store(root: &Path) -> bool {
    let Ok(dirs) = std::fs::read_dir(root) else { return false };
    for d in dirs.flatten() {
        let paks = d.path().join("Content").join("Paks");
        if let Ok(files) = std::fs::read_dir(&paks) {
            if files.flatten().any(|f| f.path().extension().is_some_and(|e| e.eq_ignore_ascii_case("utoc"))) {
                return true;
            }
        }
    }
    false
}
