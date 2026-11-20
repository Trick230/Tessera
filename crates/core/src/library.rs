// SPDX-License-Identifier: GPL-3.0-or-later
//! Game discovery: Steam libraries, Epic manifests and user-added folders.

use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Store {
    Steam,
    Epic,
    Manual,
}

impl Store {
    pub fn label(self) -> &'static str {
        match self {
            Store::Steam => "Steam",
            Store::Epic => "Epic",
            Store::Manual => "Pasta",
        }
    }
}

#[derive(Debug, Clone)]
pub struct GameEntry {
    pub name: String,
    pub root: PathBuf,
    pub store: Store,
    /// Executable named by the store manifest, relative to `root`.
    pub exe_hint: Option<PathBuf>,
}

/// All games from every source, deduplicated by install folder.
pub fn discover(manual: &[PathBuf]) -> Vec<GameEntry> {
    let mut out = steam_games();
    out.extend(epic_games());
    for root in manual {
        if root.is_dir() {
            let name = root.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            out.push(GameEntry { name, root: root.clone(), store: Store::Manual, exe_hint: None });
        }
    }
    let mut seen = std::collections::HashSet::new();
    out.retain(|g| seen.insert(g.root.to_string_lossy().to_ascii_lowercase()));
    out.sort_by_key(|g| g.name.to_lowercase());
    out
}

// ---------------------------------------------------------------- Steam

/// Steam tools that are not games.
const STEAM_SKIP_APPIDS: &[&str] = &["228980", "1070560", "1391110", "1628350", "1493710", "250820"];

fn steam_root() -> Option<PathBuf> {
    let hkcu = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER);
    let key = hkcu.open_subkey("Software\\Valve\\Steam").ok()?;
    let path: String = key.get_value("SteamPath").ok()?;
    Some(PathBuf::from(path.replace('/', "\\")))
}

pub fn steam_games() -> Vec<GameEntry> {
    let Some(root) = steam_root() else { return Vec::new() };
    let mut libs = vec![root.clone()];
    if let Ok(vdf) = std::fs::read_to_string(root.join("steamapps").join("libraryfolders.vdf")) {
        libs.extend(vdf_values(&vdf, "path").into_iter().map(PathBuf::from));
    }
    let mut seen = std::collections::HashSet::new();
    libs.retain(|l| seen.insert(l.to_string_lossy().to_ascii_lowercase()));

    let mut out = Vec::new();
    for lib in libs {
        let apps = lib.join("steamapps");
        let Ok(dir) = std::fs::read_dir(&apps) else { continue };
        for f in dir.flatten() {
            let n = f.file_name().to_string_lossy().into_owned();
            if !(n.starts_with("appmanifest_") && n.ends_with(".acf")) {
                continue;
            }
            let Ok(acf) = std::fs::read_to_string(f.path()) else { continue };
            if let Some(g) = parse_acf(&acf, &apps) {
                out.push(g);
            }
        }
    }
    out
}

fn parse_acf(acf: &str, steamapps: &Path) -> Option<GameEntry> {
    let appid = vdf_values(acf, "appid").into_iter().next()?;
    if STEAM_SKIP_APPIDS.contains(&appid.as_str()) {
        return None;
    }
    let name = vdf_values(acf, "name").into_iter().next()?;
    let dir = vdf_values(acf, "installdir").into_iter().next()?;
    let root = steamapps.join("common").join(dir);
    if !root.is_dir() || name.contains("Redistributable") || name.starts_with("Proton") {
        return None;
    }
    Some(GameEntry { name, root, store: Store::Steam, exe_hint: None })
}

/// Values of every `"key" "value"` pair with the given key (Valve KeyValues text).
pub fn vdf_values(text: &str, key: &str) -> Vec<String> {
    let mut out = Vec::new();
    for line in text.lines() {
        let mut parts = quoted(line.trim());
        if parts.next().is_some_and(|k| k.eq_ignore_ascii_case(key)) {
            if let Some(v) = parts.next() {
                out.push(v.replace("\\\\", "\\"));
            }
        }
    }
    out
}

/// Quoted tokens of a line, honouring `\"` escapes.
fn quoted(line: &str) -> impl Iterator<Item = String> + '_ {
    let mut chars = line.chars().peekable();
    std::iter::from_fn(move || {
        while chars.peek().is_some_and(|c| *c != '"') {
            chars.next();
        }
        chars.next()?;
        let mut s = String::new();
        while let Some(c) = chars.next() {
            match c {
                '"' => return Some(s),
                '\\' => {
                    if let Some(n) = chars.next() {
                        if n != '"' {
                            s.push('\\');
                        }
                        s.push(n);
                    }
                }
                _ => s.push(c),
            }
        }
        None
    })
}

// ---------------------------------------------------------------- Epic

pub fn epic_games() -> Vec<GameEntry> {
    let base = std::env::var_os("PROGRAMDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("C:\\ProgramData"));
    let dir = base.join("Epic\\EpicGamesLauncher\\Data\\Manifests");
    let Ok(rd) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut out = Vec::new();
    for f in rd.flatten() {
        if f.path().extension().is_none_or(|e| e != "item") {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(f.path()) else { continue };
        let Ok(v) = serde_json::from_str::<serde_json::Value>(&text) else { continue };
        let is_game = v["AppCategories"].as_array().is_none_or(|c| c.iter().any(|x| x == "games"));
        let (Some(name), Some(loc)) = (v["DisplayName"].as_str(), v["InstallLocation"].as_str()) else { continue };
        let root = PathBuf::from(loc);
        if !is_game || !root.is_dir() {
            continue;
        }
        let exe_hint = v["LaunchExecutable"].as_str().filter(|s| !s.is_empty()).map(PathBuf::from);
        out.push(GameEntry { name: name.to_string(), root, store: Store::Epic, exe_hint });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vdf_paths_are_unescaped() {
        let vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\Steam\"\n\t}\n}\n";
        assert_eq!(vdf_values(vdf, "path"), vec!["D:\\Steam"]);
    }

    #[test]
    fn acf_gives_name_and_folder() {
        let t = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(t.path().join("common/FF7R")).unwrap();
        let acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"1462040\"\n\t\"name\"\t\t\"FINAL FANTASY VII REMAKE INTERGRADE\"\n\t\"installdir\"\t\t\"FF7R\"\n}\n";
        let g = parse_acf(acf, t.path()).unwrap();
        assert_eq!(g.name, "FINAL FANTASY VII REMAKE INTERGRADE");
        assert!(g.root.ends_with("common/FF7R"));
        let redist = acf.replace("1462040", "228980");
        assert!(parse_acf(&redist, t.path()).is_none());
    }
}
