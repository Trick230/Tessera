// SPDX-License-Identifier: GPL-3.0-or-later
//! Per-game analysis: main executable, graphics API, engine, anti-cheat,
//! executable hash and layer install state. Read-only.

use crate::engine::{self, Engine};
use crate::install::{self, InstallState};
use crate::library::GameEntry;
use crate::pe::{self, ApiSet, ExeScan};
use std::path::{Path, PathBuf};
use walkdir::WalkDir;

#[derive(Debug, Clone)]
pub struct GameInfo {
    pub entry: GameEntry,
    pub exe: Option<PathBuf>,
    pub scan: ExeScan,
    pub engine: Engine,
    pub anticheat: Vec<String>,
    /// xxh3-128 of the main executable; keys every cache of this game.
    pub exe_hash: Option<u128>,
    pub install: InstallState,
}

impl GameInfo {
    pub fn exe_dir(&self) -> Option<&Path> {
        self.exe.as_deref().and_then(Path::parent)
    }

    /// Short API description for the UI, e.g. "D3D12, D3D11 (-dx11)".
    pub fn api_label(&self) -> String {
        let all = self.scan.all();
        if all.is_empty() {
            return "Unknown API".into();
        }
        let names = all.names();
        // Unreal ships D3D12 and D3D11 RHIs; D3D11 is picked with -dx11.
        if matches!(self.engine, Engine::Unreal { .. }) && all.has(ApiSet::D3D12) && all.has(ApiSet::D3D11) {
            return "D3D12, D3D11 (-dx11)".into();
        }
        names.join(", ")
    }

    /// Phase 0 layer only supports D3D11, and never with anti-cheat.
    pub fn can_install(&self) -> bool {
        // The layer is x64 only.
        self.anticheat.is_empty() && self.exe.is_some() && self.scan.is_64bit && self.scan.all().has(ApiSet::D3D11)
    }
}

pub fn analyze(entry: GameEntry) -> GameInfo {
    let exe = find_main_exe(&entry.root, entry.exe_hint.as_deref());
    let (scan, exe_hash) = match &exe {
        Some(p) => (pe::scan_exe(p).unwrap_or_default(), hash_file(p).ok()),
        None => (ExeScan::default(), None),
    };
    let install = exe
        .as_deref()
        .and_then(Path::parent)
        .map(|d| install::state(d, install::PROXY_D3D11))
        .unwrap_or(InstallState::NotInstalled);
    GameInfo {
        engine: engine::detect(&entry.root),
        anticheat: crate::anticheat::detect(&entry.root),
        entry,
        exe,
        scan,
        exe_hash,
        install,
    }
}

/// Analyzes every game in parallel, one task per game.
pub fn analyze_all(entries: Vec<GameEntry>) -> Vec<GameInfo> {
    use rayon::prelude::*;
    entries.into_par_iter().map(analyze).collect()
}

pub fn hash_file(path: &Path) -> std::io::Result<u128> {
    let f = std::fs::File::open(path)?;
    // SAFETY: read-only mapping of a file we do not modify.
    let map = unsafe { memmap2::Mmap::map(&f)? };
    Ok(xxhash_rust::xxh3::xxh3_128(&map))
}

/// Executable names that are never the game.
const EXE_SKIP: &[&str] = &[
    "unins", "crash", "redist", "setup", "install", "prereq", "dxweb", "vcredist", "vc_redist", "dotnet",
    "easyanticheat", "start_protected_game", "beservice", "battleye", "cefprocess", "cefsharp", "quicksfv",
    "touchup", "cleanup", "reporter", "updater", "helper", "unitycrashhandler", "dxsetup", "launcherpatcher",
];

/// Picks the real game executable. Unreal games keep it in
/// `<Project>/Binaries/Win64/`; the root exe is usually a small bootstrap.
pub fn find_main_exe(root: &Path, hint: Option<&Path>) -> Option<PathBuf> {
    let mut best: Option<(u8, u64, PathBuf)> = None;
    let walker = WalkDir::new(root).max_depth(5).into_iter().filter_entry(|e| {
        if crate::paths::is_bulk_dir(e) {
            return false;
        }
        // Unreal engine-side tools (CrashReportClient, ...) live under Engine/.
        !(e.depth() == 1 && e.file_type().is_dir() && e.file_name().eq_ignore_ascii_case("Engine"))
    });
    for e in walker.flatten() {
        if !e.file_type().is_file() {
            continue;
        }
        let path = e.path();
        if !path.extension().is_some_and(|x| x.eq_ignore_ascii_case("exe")) {
            continue;
        }
        let name = e.file_name().to_string_lossy().to_ascii_lowercase();
        if EXE_SKIP.iter().any(|s| name.contains(s)) {
            continue;
        }
        let size = e.metadata().map(|m| m.len()).unwrap_or(0);
        let rel = path.strip_prefix(root).unwrap_or(path).to_string_lossy().to_ascii_lowercase();
        // Launchers are only picked when nothing else exists.
        let score = if name.contains("launcher") {
            0
        } else if rel.contains("binaries\\win64") && !name.contains("-win64-") {
            3
        } else if hint.is_some_and(|h| rel == h.to_string_lossy().to_ascii_lowercase().replace('/', "\\")) {
            2
        } else {
            1
        };
        if best.as_ref().is_none_or(|(s, z, _)| (score, size) > (*s, *z)) {
            best = Some((score, size, path.to_path_buf()));
        }
    }
    best.map(|(_, _, p)| p)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn touch(p: &Path, size: usize) {
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, vec![0u8; size]).unwrap();
    }

    #[test]
    fn unreal_binaries_exe_beats_root_bootstrap() {
        let t = tempfile::tempdir().unwrap();
        let r = t.path();
        touch(&r.join("ff7remake.exe"), 4000);
        touch(&r.join("End/Binaries/Win64/ff7remake_.exe"), 1000);
        touch(&r.join("Engine/Binaries/Win64/CrashReportClient.exe"), 9000);
        touch(&r.join("Engine/Extras/Redist/en-us/UE4PrereqSetup_x64.exe"), 9000);
        let exe = find_main_exe(r, None).unwrap();
        assert!(exe.ends_with("End/Binaries/Win64/ff7remake_.exe"));
    }

    #[test]
    fn largest_non_tool_exe_wins_elsewhere() {
        let t = tempfile::tempdir().unwrap();
        let r = t.path();
        touch(&r.join("game.exe"), 3000);
        touch(&r.join("unins000.exe"), 9000);
        touch(&r.join("UnityCrashHandler64.exe"), 9000);
        assert!(find_main_exe(r, None).unwrap().ends_with("game.exe"));
    }
}
