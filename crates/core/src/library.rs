// SPDX-License-Identifier: GPL-3.0-or-later
//! Game discovery: Steam libraries, Epic manifests and user-added folders.

use std::collections::{HashMap, HashSet};
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
            Store::Epic => "Epic Games",
            Store::Manual => "Folder",
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
    /// Steam app id, for Steam games.
    pub steam_appid: Option<u32>,
}

/// All games from every source, deduplicated by install folder.
pub fn discover(manual: &[PathBuf]) -> Vec<GameEntry> {
    let mut out = steam_games();
    out.extend(epic_games());
    for root in manual {
        if root.is_dir() {
            let name = root.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            out.push(GameEntry { name, root: root.clone(), store: Store::Manual, exe_hint: None, steam_appid: None });
        }
    }
    let mut seen = std::collections::HashSet::new();
    out.retain(|g| seen.insert(install_key(&g.root)));
    out.sort_by_key(|g| g.name.to_lowercase());
    out
}

pub fn install_key(root: &Path) -> String {
    root.canonicalize().unwrap_or_else(|_| root.to_path_buf())
        .to_string_lossy().to_ascii_lowercase()
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
    // Steam's own type for each app drops software and tools (Wallpaper
    // Engine, Borderless Gaming...). Apps it does not know stay listed.
    let wanted: HashSet<u32> = out.iter().map(|(id, _)| *id).collect();
    let types = steam_app_types(&root.join("appcache").join("appinfo.vdf"), &wanted);
    out.into_iter()
        .filter(|(id, _)| types.get(id).map_or(true, |t| !is_not_a_game(t)))
        .map(|(_, g)| g)
        .collect()
}

/// Art Steam keeps for an installed app in appcache/librarycache.
#[derive(Debug, Default, Clone)]
pub struct SteamArt {
    /// 32x32 icon.
    pub icon: Option<PathBuf>,
    /// Wide key art shown at the top of the Steam library page.
    pub hero: Option<PathBuf>,
}

/// Steam's cached art for an app. Handles both the per-app folder layout
/// (`<id>\library_hero.jpg`, `<id>\<sha1>.jpg` icon, maybe one level deeper)
/// and the older flat one (`<id>_library_hero.jpg`, `<id>_icon.jpg`).
pub fn steam_art(appid: u32) -> SteamArt {
    static ROOT: std::sync::OnceLock<Option<PathBuf>> = std::sync::OnceLock::new();
    let mut art = SteamArt::default();
    let Some(cache) = ROOT.get_or_init(steam_root).as_ref().map(|r| r.join("appcache").join("librarycache")) else {
        return art;
    };
    let dir = cache.join(appid.to_string());
    let mut dirs = vec![dir.clone()];
    if let Ok(rd) = std::fs::read_dir(&dir) {
        dirs.extend(rd.flatten().map(|e| e.path()).filter(|p| p.is_dir()));
    }
    for d in &dirs {
        let Ok(rd) = std::fs::read_dir(d) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_ascii_lowercase();
            if n == "library_hero.jpg" {
                art.hero.get_or_insert(e.path());
            } else if n.len() == 44 && n.ends_with(".jpg") && n[..40].bytes().all(|b| b.is_ascii_hexdigit()) {
                art.icon.get_or_insert(e.path());
            }
        }
    }
    let flat = |suffix: &str| Some(cache.join(format!("{appid}_{suffix}"))).filter(|p| p.is_file());
    art.hero = art.hero.or_else(|| flat("library_hero.jpg"));
    art.icon = art.icon.or_else(|| flat("icon.jpg"));
    art
}

/// Steam app types that are not games.
fn is_not_a_game(kind: &str) -> bool {
    ["application", "tool", "config", "music", "video", "dlc"].iter().any(|k| kind.eq_ignore_ascii_case(k))
}

/// Steam's app type ("Game", "Application", "Tool"...) for the given app ids,
/// read from appcache/appinfo.vdf. Empty when the file is missing or unknown.
fn steam_app_types(path: &Path, wanted: &HashSet<u32>) -> HashMap<u32, String> {
    let mut out = HashMap::new();
    if let Ok(data) = std::fs::read(path) {
        let _ = parse_appinfo(&data, wanted, &mut out);
    }
    out
}

/// appinfo.vdf, versions 28 and 29: header, then entries (app id, size,
/// 60 bytes of metadata, binary KeyValues) until app id 0. Version 29 keys
/// are indexes into a string table whose offset is in the header.
fn parse_appinfo(d: &[u8], wanted: &HashSet<u32>, out: &mut HashMap<u32, String>) -> Option<()> {
    let u32_at = |o: usize| d.get(o..o.checked_add(4)?).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    let (v29, mut pos) = match u32_at(0)? {
        0x0756_4428 => (false, 8),
        0x0756_4429 => (true, 16),
        _ => return None,
    };
    let mut strings: Vec<&[u8]> = Vec::new();
    if v29 {
        let off = usize::try_from(u64::from_le_bytes(d.get(8..16)?.try_into().ok()?)).ok()?;
        let count = u32_at(off)? as usize;
        let mut p = off + 4;
        for _ in 0..count {
            let len = d.get(p..)?.iter().position(|&b| b == 0)?;
            strings.push(&d[p..p + len]);
            p += len + 1;
        }
    }
    loop {
        let appid = u32_at(pos)?;
        if appid == 0 {
            return Some(());
        }
        let start = pos + 8;
        let end = start.checked_add(u32_at(pos + 4)? as usize)?;
        if end > d.len() {
            return None;
        }
        if wanted.contains(&appid) {
            if let Some(t) = d.get(start + 60..end).and_then(|kv| kv_common_type(kv, v29, &strings)) {
                out.insert(appid, t);
            }
        }
        pos = end;
    }
}

/// The string at appinfo/common/type in one binary KeyValues blob.
fn kv_common_type(kv: &[u8], v29: bool, strings: &[&[u8]]) -> Option<String> {
    let cstr = |p: usize| -> Option<(&[u8], usize)> {
        let len = kv.get(p..)?.iter().position(|&b| b == 0)?;
        Some((&kv[p..p + len], p + len + 1))
    };
    let mut p = 0;
    let mut depth = 0usize;
    let mut in_common = false;
    loop {
        let t = *kv.get(p)?;
        p += 1;
        if t == 0x08 || t == 0x0B {
            depth = depth.checked_sub(1)?;
            in_common = false;
            continue;
        }
        let key = if v29 {
            let i = u32::from_le_bytes(kv.get(p..p + 4)?.try_into().ok()?) as usize;
            p += 4;
            *strings.get(i)?
        } else {
            let (k, next) = cstr(p)?;
            p = next;
            k
        };
        match t {
            0x00 => {
                depth += 1;
                in_common = depth == 2 && key.eq_ignore_ascii_case(b"common");
            }
            0x01 => {
                let (v, next) = cstr(p)?;
                p = next;
                if in_common && depth == 2 && key.eq_ignore_ascii_case(b"type") {
                    return Some(String::from_utf8_lossy(v).into_owned());
                }
            }
            0x02 | 0x03 | 0x04 | 0x06 => p += 4,
            0x07 | 0x0A => p += 8,
            _ => return None,
        }
    }
}

fn parse_acf(acf: &str, steamapps: &Path) -> Option<(u32, GameEntry)> {
    let appid = vdf_values(acf, "appid").into_iter().next()?;
    if STEAM_SKIP_APPIDS.contains(&appid.as_str()) {
        return None;
    }
    let id: u32 = appid.parse().ok()?;
    let name = vdf_values(acf, "name").into_iter().next()?;
    let dir = vdf_values(acf, "installdir").into_iter().next()?;
    let root = steamapps.join("common").join(dir);
    if !root.is_dir() || name.contains("Redistributable") || name.starts_with("Proton") {
        return None;
    }
    Some((id, GameEntry { name, root, store: Store::Steam, exe_hint: None, steam_appid: Some(id) }))
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
    use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_READ, KEY_WOW64_32KEY};
    use winreg::RegKey;

    let base = std::env::var_os("PROGRAMDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("C:\\ProgramData"));
    let launcher = RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(r"SOFTWARE\Epic Games\EpicGamesLauncher", KEY_READ | KEY_WOW64_32KEY)
        .and_then(|key| key.get_value::<String, _>("AppDataPath"))
        .ok().map(PathBuf::from).filter(|path| path.is_absolute()).map(|path| path.join("Manifests"));
    let eos = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey(r"Software\Epic Games\EOS")
        .and_then(|key| key.get_value::<String, _>("ModSdkMetadataDir"))
        .ok().map(PathBuf::from).filter(|path| path.is_absolute());
    let preview = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey(r"Software\Epic Games\EpicGamesLauncher")
        .and_then(|key| key.get_value::<String, _>("AppDataPath"))
        .ok().map(PathBuf::from).filter(|path| path.is_absolute());
    let preview_default = std::env::var_os("LOCALAPPDATA").map(PathBuf::from)
        .map(|path| path.join("com.epicgames.eglv2"));
    let default = base.join(r"Epic\EpicGamesLauncher\Data\Manifests");
    let mut seen = HashSet::new();
    let mut games: Vec<_> = [launcher, eos, Some(default)].into_iter().flatten()
        .filter(|dir| seen.insert(install_key(dir)))
        .flat_map(|dir| read_epic_manifests(&dir))
        .collect();
    seen.clear();
    games.extend([preview, preview_default].into_iter().flatten()
        .filter(|dir| seen.insert(install_key(dir)))
        .filter_map(|dir| std::fs::read_to_string(dir.join("installations.jsonl")).ok())
        .flat_map(|text| parse_epic_preview(&text)));
    games
}

fn read_epic_manifests(dir: &Path) -> Vec<GameEntry> {
    let Ok(rd) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut out = Vec::new();
    for f in rd.flatten() {
        let path = f.path();
        if !path.extension().is_some_and(|e| e.eq_ignore_ascii_case("item")) {
            continue;
        }
        if let Ok(text) = std::fs::read_to_string(path) {
            out.extend(parse_epic_manifest(&text));
        }
    }
    out
}

fn parse_epic_manifest(text: &str) -> Option<GameEntry> {
    let v: serde_json::Value = serde_json::from_str(text.trim_start_matches('\u{feff}')).ok()?;
    let is_game = v["AppCategories"].as_array().is_none_or(|categories| {
        categories.iter().filter_map(|c| c.as_str()).any(|c| {
            c.eq_ignore_ascii_case("games") || c.to_ascii_lowercase().starts_with("games/")
        })
    });
    if !is_game || v["bIsIncompleteInstall"].as_bool() == Some(true) ||
        v["bIsExecutable"].as_bool() == Some(false) {
        return None;
    }
    let root = PathBuf::from(v["InstallLocation"].as_str()?);
    if !root.is_absolute() || !root.is_dir() {
        return None;
    }
    let name = v["DisplayName"].as_str().map(str::trim).filter(|s| !s.is_empty())
        .map(str::to_owned).or_else(|| root.file_name().map(|s| s.to_string_lossy().into_owned()))?;
    let exe_hint = v["LaunchExecutable"].as_str().and_then(|exe| epic_exe_hint(&root, exe));
    Some(GameEntry { name, root, store: Store::Epic, exe_hint, steam_appid: None })
}

fn epic_exe_hint(root: &Path, exe: &str) -> Option<PathBuf> {
    let exe = Path::new(exe);
    let relative = if exe.is_absolute() { exe.strip_prefix(root).ok()? } else { exe };
    if !relative.extension().is_some_and(|e| e.eq_ignore_ascii_case("exe")) ||
        !relative.components().all(|c| matches!(c, std::path::Component::Normal(_) | std::path::Component::CurDir)) {
        return None;
    }
    Some(relative.to_path_buf())
}

fn parse_epic_preview(text: &str) -> Vec<GameEntry> {
    let mut latest = HashMap::new();
    for line in text.lines() {
        let Ok(record) = serde_json::from_str::<serde_json::Value>(line.trim_start_matches('\u{feff}')) else { continue };
        if let Some(id) = record["installationId"].as_str().filter(|id| !id.is_empty()) {
            latest.insert(id.to_owned(), record);
        }
    }
    latest.into_values().filter_map(|record| {
        if record["complete"].as_bool() != Some(true) ||
            !record["platform"].as_str().is_some_and(|p| p.eq_ignore_ascii_case("Windows")) {
            return None;
        }
        let root = PathBuf::from(record["dir"].as_str()?);
        let exe = epic_exe_hint(&root, record["launchExecutable"].as_str()?)?;
        if !root.is_absolute() || !root.join(&exe).is_file() {
            return None;
        }
        let name = root.file_name()?.to_string_lossy().into_owned();
        Some(GameEntry { name, root, store: Store::Epic, exe_hint: Some(exe), steam_appid: None })
    }).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn epic_manifest_reader_keeps_installed_games() {
        let dir = tempfile::tempdir().unwrap();
        let manifests = dir.path().join("Manifests");
        std::fs::create_dir(&manifests).unwrap();
        let root = dir.path().join("Example Game");
        std::fs::create_dir(&root).unwrap();
        let manifest = serde_json::json!({
            "DisplayName": "Example Game",
            "InstallLocation": root,
            "LaunchExecutable": root.join("Bin").join("Example.exe"),
            "AppCategories": ["games/example"],
            "bIsExecutable": true,
        });
        std::fs::write(manifests.join("installed.ITEM"), format!("\u{feff}{manifest}")).unwrap();
        let mut incomplete = manifest.clone();
        incomplete["bIsIncompleteInstall"] = true.into();
        std::fs::write(manifests.join("incomplete.item"), incomplete.to_string()).unwrap();
        let mut addon = manifest.clone();
        addon["AppCategories"] = serde_json::json!(["addons"]);
        std::fs::write(manifests.join("addon.item"), addon.to_string()).unwrap();
        let mut missing = manifest;
        missing["InstallLocation"] = serde_json::json!(root.join("missing"));
        std::fs::write(manifests.join("missing.item"), missing.to_string()).unwrap();
        std::fs::write(manifests.join("broken.item"), "{").unwrap();

        let games = read_epic_manifests(&manifests);
        assert_eq!(games.len(), 1);
        assert_eq!(games[0].name, "Example Game");
        assert_eq!(games[0].root, root);
        assert_eq!(games[0].store, Store::Epic);
        assert_eq!(games[0].exe_hint, Some(PathBuf::from("Bin").join("Example.exe")));
    }

    #[test]
    fn epic_preview_uses_the_latest_record_for_each_installation() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("Preview Game");
        std::fs::create_dir(&root).unwrap();
        std::fs::write(root.join("Game.exe"), []).unwrap();
        let mut record = serde_json::json!({
            "installationId": "old", "complete": true, "platform": "Windows",
            "dir": root, "launchExecutable": "Game.exe",
        });
        let mut log = format!("{{\"version\":1,\"entries\":0}}\n{record}\n");
        record["complete"] = false.into();
        log.push_str(&format!("{record}\n"));
        record["installationId"] = "current".into();
        log.push_str(&format!("{record}\n"));
        record["complete"] = true.into();
        log.push_str(&format!("{record}\n{{"));
        let games = parse_epic_preview(&log);
        assert_eq!(games.len(), 1);
        assert_eq!(games[0].name, "Preview Game");
        assert_eq!(games[0].root, root);
        assert_eq!(games[0].store, Store::Epic);
        assert_eq!(games[0].exe_hint.as_deref(), Some(Path::new("Game.exe")));
    }

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
        let (id, g) = parse_acf(acf, t.path()).unwrap();
        assert_eq!(id, 1462040);
        assert_eq!(g.name, "FINAL FANTASY VII REMAKE INTERGRADE");
        assert!(g.root.ends_with("common/FF7R"));
        let redist = acf.replace("1462040", "228980");
        assert!(parse_acf(&redist, t.path()).is_none());
    }

    /// A version 29 appinfo.vdf with two apps: a game and an application.
    fn appinfo_v29() -> Vec<u8> {
        let strings = ["appinfo", "common", "type", "name"];
        let kv = |kind: &str| {
            let mut b = vec![0x00];
            b.extend(0u32.to_le_bytes()); // appinfo
            b.push(0x02);
            b.extend(3u32.to_le_bytes()); // an int before common
            b.extend(7u32.to_le_bytes());
            b.push(0x00);
            b.extend(1u32.to_le_bytes()); // common
            b.push(0x01);
            b.extend(3u32.to_le_bytes());
            b.extend(b"Some name\0");
            b.push(0x01);
            b.extend(2u32.to_le_bytes());
            b.extend(kind.as_bytes());
            b.push(0);
            b.extend([0x08, 0x08, 0x08]);
            b
        };
        let mut d = Vec::new();
        d.extend(0x0756_4429u32.to_le_bytes());
        d.extend(1u32.to_le_bytes());
        d.extend(0u64.to_le_bytes()); // string table offset, patched below
        for (id, kind) in [(10u32, "Game"), (20, "Application")] {
            let body = kv(kind);
            d.extend(id.to_le_bytes());
            d.extend(((60 + body.len()) as u32).to_le_bytes());
            d.extend([0u8; 60]);
            d.extend(body);
        }
        d.extend(0u32.to_le_bytes());
        let off = d.len() as u64;
        d[8..16].copy_from_slice(&off.to_le_bytes());
        d.extend((strings.len() as u32).to_le_bytes());
        for s in strings {
            d.extend(s.as_bytes());
            d.push(0);
        }
        d
    }

    #[test]
    fn appinfo_gives_steam_app_types() {
        let wanted: HashSet<u32> = [10, 20].into();
        let mut out = HashMap::new();
        assert!(parse_appinfo(&appinfo_v29(), &wanted, &mut out).is_some());
        assert_eq!(out.get(&10).map(String::as_str), Some("Game"));
        assert_eq!(out.get(&20).map(String::as_str), Some("Application"));
        assert!(!is_not_a_game("Game"));
        assert!(is_not_a_game("application"));
        assert!(parse_appinfo(b"junk", &wanted, &mut HashMap::new()).is_none());
    }
}
