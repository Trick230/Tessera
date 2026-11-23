// SPDX-License-Identifier: GPL-3.0-or-later
//! Anti-cheat detection by known files and folders. A game with anti-cheat
//! never gets the DLL; the app only reads its files.

use std::path::Path;
use walkdir::WalkDir;

/// (lowercase substring of a file or folder name, display name)
const MARKERS: &[(&str, &str)] = &[
    ("easyanticheat", "Easy Anti-Cheat"),
    ("start_protected_game", "Easy Anti-Cheat"),
    ("battleye", "BattlEye"),
    ("beservice", "BattlEye"),
    ("eaanticheat", "EA Javelin"),
    ("gameguard", "nProtect GameGuard"),
    ("npgamemon", "nProtect GameGuard"),
    ("xigncode", "XIGNCODE3"),
    ("x3.xem", "XIGNCODE3"),
    ("mhyprot", "mhyprot"),
    ("anticheatexpert", "Tencent ACE"),
    ("sguard", "Tencent ACE"),
    ("equ8", "EQU8"),
    ("pnkbstr", "PunkBuster"),
    ("punkbuster", "PunkBuster"),
    ("blackcipher", "BlackCipher"),
    ("hshield", "AhnLab HackShield"),
    ("faceit", "FACEIT"),
    ("vgk.sys", "Riot Vanguard"),
];

pub fn detect(root: &Path) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    let walker = WalkDir::new(root)
        .max_depth(4)
        .into_iter()
        .filter_entry(|e| !crate::paths::is_bulk_dir(e));
    for entry in walker.flatten() {
        let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
        for (marker, label) in MARKERS {
            let matched = if *marker == "sguard" {
                // ACE's SGuard name starts a component, unlike words ending in "sGuardian".
                name.split(|c: char| !c.is_ascii_alphanumeric()).any(|part| part.starts_with(marker))
            } else {
                name.contains(marker)
            };
            if matched && !found.iter().any(|f| f == label) {
                found.push((*label).to_string());
            }
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_eac_folder_and_nothing_in_clean_game() {
        let t = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(t.path().join("Game/Binaries/Win64")).unwrap();
        assert!(detect(t.path()).is_empty());
        std::fs::create_dir_all(t.path().join("EasyAntiCheat")).unwrap();
        assert_eq!(detect(t.path()), vec!["Easy Anti-Cheat"]);
    }

    #[test]
    fn distinguishes_sguard_components_from_game_assets() {
        let t = tempfile::tempdir().unwrap();
        std::fs::write(t.path().join("Obj_TravelersGuardianDeity_A.sbfres"), []).unwrap();
        assert!(detect(t.path()).is_empty());
        std::fs::write(t.path().join("SGuard64.exe"), []).unwrap();
        assert_eq!(detect(t.path()), vec!["Tencent ACE"]);
    }
}
