// SPDX-License-Identifier: GPL-3.0-or-later
//! Per-game analysis: main executable, graphics API, engine, anti-cheat,
//! executable hash and layer install state. Read-only.

use crate::engine::{self, Engine};
use crate::install::{self, InstallState};
use crate::library::{GameEntry, Store};
use crate::pe::{self, ApiSet, ExeScan};
use std::path::{Path, PathBuf};
use walkdir::WalkDir;

#[derive(Debug, Clone)]
pub struct GameInfo {
    pub entry: GameEntry,
    pub exe: Option<PathBuf>,
    pub scan: ExeScan,
    pub engine: Engine,
    unity_last_api: Option<&'static str>,
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
        if self.engine == Engine::Unity {
            return self.unity_last_api
                .map_or_else(|| "Unknown API".into(), |name| format!("{name} (last run)"));
        }
        let all = self.scan.all();
        if all.is_empty() {
            return "Unknown API".into();
        }
        let names = all.names();
        if self.entry.store == Store::Recomp && names.len() > 1 {
            return format!("{} (detected)", names.join(", "));
        }
        // Unreal ships D3D12 and D3D11 RHIs; D3D11 is picked with -dx11.
        if matches!(self.engine, Engine::Unreal { .. }) && all.has(ApiSet::D3D12) && all.has(ApiSet::D3D11) {
            return "D3D12, D3D11 (-dx11)".into();
        }
        names.join(", ")
    }

    /// Phase 0 layer only supports D3D11, and never with anti-cheat.
    pub fn can_install(&self) -> bool {
        self.anticheat.is_empty() && self.exe.is_some() && self.scan.architecture.is_some() && self.scan.all().has(ApiSet::D3D11)
    }
}

pub fn analyze(entry: GameEntry) -> GameInfo {
    let exe = find_main_exe(&entry.root, entry.exe_hint.as_deref());
    let engine = engine::detect(&entry.root);
    let (mut scan, exe_hash) = match &exe {
        Some(p) => (pe::scan_exe(p).unwrap_or_default(), hash_file(p).ok()),
        None => (ExeScan::default(), None),
    };
    // Unity launchers can leave all graphics API references in the player DLL.
    if let (Engine::Unity, Some(p), Some(architecture)) = (engine, exe.as_deref(), scan.architecture) {
        if let Some(player) = pe::scan_exe(&p.with_file_name("UnityPlayer.dll")).ok()
            .filter(|player| player.architecture == Some(architecture))
        {
            scan.referenced.0 |= player.all().0;
        }
    }
    // FFVIII Remastered's bootstrap loads its locale-specific OpenGL renderer.
    if let Some(p) = exe.as_deref().filter(|p| p.file_name().is_some_and(|n| n.eq_ignore_ascii_case("FFVIII.exe"))) {
        if ["FFVIII_EFIGS.dll", "FFVIII_JP.dll"].iter().any(|name| {
            pe::scan_exe(&p.with_file_name(name)).is_ok_and(|renderer| renderer.all().has(ApiSet::OPENGL))
        }) {
            scan.referenced.0 |= ApiSet::OPENGL;
        }
    }
    let install = exe
        .as_deref()
        .and_then(Path::parent)
        .map(|d| install::state(d, install::PROXY_D3D11))
        .unwrap_or(InstallState::NotInstalled);
    GameInfo {
        engine,
        unity_last_api: if engine == Engine::Unity { exe.as_deref().and_then(engine::unity_last_api) } else { None },
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
    best.map(|(_, _, p)| p).or_else(|| {
        // A store manifest can name an executable outside the normal search.
        let hint = hint?;
        if !hint.components().all(|c| matches!(c, std::path::Component::Normal(_) | std::path::Component::CurDir)) ||
            !hint.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("exe")) {
            return None;
        }
        let exe = root.join(hint);
        exe.is_file().then_some(exe)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn touch(p: &Path, size: usize) {
        std::fs::create_dir_all(p.parent().unwrap()).unwrap();
        std::fs::write(p, vec![0u8; size]).unwrap();
    }

    fn minimal_pe64() -> Vec<u8> {
        let mut pe = vec![0; 512];
        pe[..2].copy_from_slice(b"MZ");
        pe[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        pe[0x80..0x84].copy_from_slice(b"PE\0\0");
        pe[0x84..0x86].copy_from_slice(&0x8664u16.to_le_bytes());
        pe[0x94..0x96].copy_from_slice(&240u16.to_le_bytes());
        pe[0x98..0x9a].copy_from_slice(&0x20bu16.to_le_bytes());
        pe
    }

    fn pe64_importing(dll: &str) -> Vec<u8> {
        let mut pe = minimal_pe64();
        // Add one .rdata section containing the import table.
        pe.resize(1024, 0);
        pe[0x86..0x88].copy_from_slice(&1u16.to_le_bytes());
        pe[0x104..0x108].copy_from_slice(&16u32.to_le_bytes());
        pe[0x110..0x114].copy_from_slice(&0x1000u32.to_le_bytes());
        pe[0x114..0x118].copy_from_slice(&40u32.to_le_bytes());
        pe[0x188..0x190].copy_from_slice(b".rdata\0\0");
        pe[0x190..0x194].copy_from_slice(&0x200u32.to_le_bytes());
        pe[0x194..0x198].copy_from_slice(&0x1000u32.to_le_bytes());
        pe[0x198..0x19c].copy_from_slice(&0x200u32.to_le_bytes());
        pe[0x19c..0x1a0].copy_from_slice(&0x200u32.to_le_bytes());
        pe[0x20c..0x210].copy_from_slice(&0x1030u32.to_le_bytes());
        pe[0x230..0x230 + dll.len()].copy_from_slice(dll.as_bytes());
        pe
    }

    #[test]
    fn d3d9_import_without_device_creation_does_not_identify_a_renderer() {
        let t = tempfile::tempdir().unwrap();
        let exe = t.path().join("Game.exe");
        let entry = GameEntry {
            name: "Game".into(),
            root: t.path().into(),
            store: Store::Steam,
            exe_hint: None,
            steam_appid: None,
        };
        let mut executable = pe64_importing("d3d9.dll");
        executable.extend_from_slice(b"D3DPERF_BeginEvent\0D3D11CreateDevice\0");
        std::fs::write(&exe, &executable).unwrap();
        assert_eq!(analyze(entry.clone()).api_label(), "D3D11");

        executable.extend_from_slice(b"Direct3DCreate9Ex\0");
        std::fs::write(&exe, executable).unwrap();
        assert_eq!(analyze(entry).api_label(), "D3D11, D3D9");
    }

    #[test]
    fn d3d12_import_takes_priority_over_unlinked_d3d9_probes() {
        let mut executable = pe64_importing("d3d12.dll");
        executable[0x230..0x23a].fill(0);
        executable[0x250..0x25a].copy_from_slice(b"d3d12.dll\0");
        executable[0x20c..0x210].copy_from_slice(&0x1050u32.to_le_bytes());
        executable[0x200..0x204].copy_from_slice(&0x1080u32.to_le_bytes());
        executable[0x280..0x288].copy_from_slice(&0x10b0u64.to_le_bytes());
        executable[0x2b2..0x2c3].copy_from_slice(b"D3D12CreateDevice");
        executable[0x220..0x224].copy_from_slice(&0x1060u32.to_le_bytes());
        executable[0x260..0x269].copy_from_slice(b"d3d9.dll\0");
        executable[0x214..0x218].copy_from_slice(&0x1070u32.to_le_bytes());
        executable[0x270..0x278].copy_from_slice(&0x1090u64.to_le_bytes());
        executable[0x292..0x2a2].copy_from_slice(b"D3DPERF_EndEvent");
        executable.extend_from_slice(b"Direct3DCreate9\0Direct3DCreate9Ex\0");
        assert_eq!(pe::scan_bytes(&executable).all(), ApiSet(ApiSet::D3D12));

        executable[0x292..0x2a2].copy_from_slice(b"Direct3DCreate9\0");
        assert_eq!(pe::scan_bytes(&executable).all(), ApiSet(ApiSet::D3D12 | ApiSet::D3D9));
    }

    #[test]
    fn unity_detects_graphics_api_in_player_dll() {
        let t = tempfile::tempdir().unwrap();
        let root = t.path();
        let mut pe = minimal_pe64();
        std::fs::write(root.join("Game.exe"), &pe).unwrap();
        pe.extend_from_slice(b"D3D11CreateDevice\0D3D12CreateDevice\0vkCreateGraphicsPipelines\0");
        std::fs::write(root.join("UnityPlayer.dll"), &pe).unwrap();

        let game = analyze(GameEntry {
            name: "Game".into(),
            root: root.to_path_buf(),
            store: Store::Manual,
            exe_hint: None,
            steam_appid: None,
        });
        assert_eq!(game.engine, Engine::Unity);
        assert_eq!(game.api_label(), "Unknown API");
        assert!(game.scan.all().has(ApiSet::D3D11));
        assert!(game.can_install());
    }

    #[test]
    fn recomp_detects_linked_renderer_without_guessing_from_dll_names() {
        let t = tempfile::tempdir().unwrap();
        let root = t.path();
        let entry = GameEntry {
            name: "Unleashed Recompiled".into(),
            root: root.into(),
            store: Store::Recomp,
            exe_hint: Some("UnleashedRecomp.exe".into()),
            steam_appid: None,
        };
        let mut executable = minimal_pe64();
        executable.extend_from_slice(b"d3d11.dll\0opengl32.dll\0vkGetInstanceProcAddr\0");
        let exe = root.join("UnleashedRecomp.exe");
        std::fs::write(&exe, &executable).unwrap();
        let mut renderer = minimal_pe64();
        renderer.extend_from_slice(b"D3D12CreateDevice\0vkCreateGraphicsPipelines\0");
        std::fs::write(root.join("Renderer.dll"), renderer).unwrap();
        assert_eq!(analyze(entry.clone()).api_label(), "Unknown API");

        std::fs::write(&exe, pe64_importing("Renderer.dll")).unwrap();
        assert_eq!(analyze(entry).api_label(), "D3D12, Vulkan (detected)");
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

    #[test]
    fn manifest_can_name_an_executable_under_engine() {
        let t = tempfile::tempdir().unwrap();
        let hint = Path::new("Engine/Binaries/Win64/Game.exe");
        let exe = t.path().join(hint);
        touch(&exe, 1000);
        assert_eq!(find_main_exe(t.path(), Some(hint)), Some(exe));
    }
}
