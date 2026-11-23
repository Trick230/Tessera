// SPDX-License-Identifier: GPL-3.0-or-later
//! Native ports distributed outside the storefronts.

use std::path::{Path, PathBuf};
use walkdir::WalkDir;

use crate::library::{GameEntry, Store};
use crate::pe::ApiSet;

fn compact(name: &str) -> String {
    name.chars().filter(char::is_ascii_alphanumeric).map(|c| c.to_ascii_lowercase()).collect()
}

pub(crate) struct GraphicsBackends {
    pub supported: ApiSet,
    pub default_label: Option<&'static str>,
}

/// Documented Windows renderers for ports that can bundle unrelated API references.
pub(crate) fn graphics_backends(exe: &Path) -> Option<GraphicsBackends> {
    let stem = compact(exe.file_stem()?.to_str()?);
    let (supported, default_label) = match stem.as_str() {
        // hedge-dev/UnleashedRecomp: Video::CreateHostDevice.
        "unleashedrecomp" | "unleashedrecompiled" => (ApiSet::D3D12 | ApiSet::VULKAN, Some("D3D12 (default)")),
        // BanjoRecomp and Zelda64Recomp use RT64.
        "banjorecomp" | "banjorecompiled" | "zelda64recomp" | "zelda64recompiled" =>
            (ApiSet::D3D12 | ApiSet::VULKAN, None),
        // TwilitRealm/dusklight's Windows backends.
        "dusklight" => (ApiSet::D3D12 | ApiSet::D3D11 | ApiSet::VULKAN, None),
        // The Shipwright and 2ship2harkinian READMEs document the Windows backends.
        "shipofharkinian" | "soh" | "2ship2harkinian" | "2ship" => (ApiSet::D3D11 | ApiSet::OPENGL, None),
        _ => return None,
    };
    Some(GraphicsBackends { supported: ApiSet(supported), default_label })
}

pub(crate) fn collection_for(path: &Path) -> Option<&Path> {
    path.ancestors().find(|dir| dir.file_name().and_then(|name| name.to_str()).is_some_and(|name| {
        matches!(compact(name).as_str(), "recomps" | "recompgames" | "recompiledgames")
    }))
}

pub(crate) fn collection_games(path: &Path) -> Vec<GameEntry> {
    let Ok(entries) = std::fs::read_dir(path) else { return Vec::new() };
    entries.flatten().filter(|entry| entry.file_type().is_ok_and(|kind| kind.is_dir()))
        .filter_map(|entry| game(&entry.path())).collect()
}

fn title(stem: &str) -> Option<&'static str> {
    Some(match stem {
        "banjorecomp" | "banjorecompiled" => "Banjo: Recompiled",
        "dusklight" => "Dusklight: Twilight Princess",
        "edgeoftimerecomp" | "edgeoftimerecompiled" => "Spider-Man: Edge of Time Recompiled",
        "jetsetradiofuture" => "Jet Set Radio Future",
        "legodimensions" => "LEGO Dimensions",
        "projectdream" => "Sonic Dream Team (Project Dream)",
        "shipofharkinian" | "soh" => "Ship of Harkinian",
        "2ship2harkinian" | "2ship" => "2 Ship 2 Harkinian",
        "skate3" | "skate3recomp" => "Skate 3 Recompiled",
        "sotn" | "symphonyrecomp" => "Castlevania: Symphony of the Night Recompiled",
        "unleashedrecomp" | "unleashedrecompiled" => "Unleashed Recompiled",
        "zelda64recomp" | "zelda64recompiled" => "Zelda 64: Recompiled",
        _ => return None,
    })
}

pub(crate) fn game(root: &Path) -> Option<GameEntry> {
    let mut generic: Option<PathBuf> = None;
    let entries = WalkDir::new(root).min_depth(1).max_depth(2).sort_by_file_name()
        .into_iter().filter_entry(|entry| !crate::paths::is_bulk_dir(entry));
    for entry in entries.flatten() {
        let path = entry.path();
        if !entry.file_type().is_file() || !path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("exe")) {
            continue;
        }
        let Some(stem) = path.file_stem().and_then(|name| name.to_str()).map(compact) else { continue };
        let Ok(relative) = path.strip_prefix(root) else { continue };
        if let Some(name) = title(&stem) {
            return Some(recomp_entry(root, relative.to_path_buf(), name.to_owned()));
        }
        if generic.is_none() && (stem.ends_with("recomp") || stem.ends_with("recompiled")) {
            generic = Some(relative.to_path_buf());
        }
    }
    let name = root.file_name()?.to_string_lossy().into_owned();
    Some(recomp_entry(root, generic?, name))
}

fn recomp_entry(root: &Path, exe: PathBuf, name: String) -> GameEntry {
    GameEntry { name, root: root.to_path_buf(), store: Store::Recomp, exe_hint: Some(exe), steam_appid: None }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recognizes_nested_and_generic_recomps_in_a_collection() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("Recompiled Games");
        let sonic = root.join("Portable Sonic");
        let spider_man = root.join("Spider-Man").join("Windows");
        let other = root.join("Other Recomp");
        for folder in [&sonic, &spider_man, &other, &root.join("Tools")] {
            std::fs::create_dir_all(folder).unwrap();
        }
        std::fs::write(sonic.join("UnleashedRecomp.exe"), []).unwrap();
        std::fs::write(sonic.join("HedgeModManager.exe"), []).unwrap();
        std::fs::write(spider_man.join("EdgeOfTimeRecomp.exe"), []).unwrap();
        std::fs::write(other.join("ExampleRecompiled.exe"), []).unwrap();
        std::fs::write(root.join("Tools").join("helper.exe"), []).unwrap();

        assert_eq!(collection_for(&sonic), Some(root.as_path()));
        let games = collection_games(&root);
        assert_eq!(games.len(), 3);
        let game = games.iter().find(|game| game.root == sonic).unwrap();
        assert_eq!(game.name, "Unleashed Recompiled");
        assert_eq!(game.exe_hint.as_deref(), Some(Path::new("UnleashedRecomp.exe")));
        assert!(games.iter().any(|game| game.exe_hint.as_deref() ==
            Some(Path::new("Windows").join("EdgeOfTimeRecomp.exe").as_path())));
    }
}
