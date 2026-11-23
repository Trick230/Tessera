// SPDX-License-Identifier: GPL-3.0-or-later
//! Engine detection from the install layout.

use std::fs::File;
use std::io::Read;
use std::path::{Component, Path};

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

/// Unity players record their initialized renderer in the per-product Player.log.
pub(crate) fn unity_last_api(exe: &Path) -> Option<&'static str> {
    let local = std::env::var_os("LOCALAPPDATA")?;
    let local_low = Path::new(&local).parent()?.join("LocalLow");
    unity_last_api_at(exe, &local_low)
}

fn unity_last_api_at(exe: &Path, local_low: &Path) -> Option<&'static str> {
    let mut data_dir = exe.file_stem()?.to_os_string();
    data_dir.push("_Data");
    let mut identity = String::new();
    File::open(exe.with_file_name(data_dir).join("app.info")).ok()?
        .take(4096).read_to_string(&mut identity).ok()?;
    let mut lines = identity.trim_start_matches('\u{feff}').lines();
    let mut log_dir = local_low.to_path_buf();
    for name in [lines.next()?.trim(), lines.next()?.trim()] {
        let mut components = Path::new(name).components();
        if !matches!(components.next(), Some(Component::Normal(part)) if part == name)
            || components.next().is_some()
        {
            return None;
        }
        log_dir.push(name);
    }
    let log = File::open(log_dir.join("Player.log"))
        .or_else(|_| File::open(log_dir.join("output_log.txt"))).ok()?;
    let mut startup = Vec::new();
    log.take(256 * 1024).read_to_end(&mut startup).ok()?;
    unity_log_api(&String::from_utf8_lossy(&startup))
}

fn unity_log_api(log: &str) -> Option<&'static str> {
    let mut lines = log.lines().map(str::trim).peekable();
    while let Some(line) = lines.next() {
        if line == "Direct3D:" {
            let version = lines.peek().and_then(|line| line.strip_prefix("Version:"))
                .and_then(|value| value.trim_start().strip_prefix("Direct3D "));
            match version.and_then(|value| value.split(['.', ' ']).next()) {
                Some("12") => return Some("D3D12"),
                Some("11") => return Some("D3D11"),
                Some("9") => return Some("D3D9"),
                _ => {}
            }
        }
        if line.starts_with("Vulkan renderer=[") {
            return Some("Vulkan");
        }
        if line.starts_with("OPENGL LOG: Creating OpenGL ") && !line.starts_with("OPENGL LOG: Creating OpenGL ES ") {
            return Some("OpenGL");
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unity_reads_renderer_from_the_logs_for_its_product() {
        let temp = tempfile::tempdir().unwrap();
        let exe = temp.path().join("Game.exe");
        let data = temp.path().join("Game_Data");
        let local_low = temp.path().join("LocalLow");
        let product_logs = local_low.join("Company").join("Game Title");
        std::fs::create_dir(&data).unwrap();
        std::fs::create_dir_all(&product_logs).unwrap();
        std::fs::write(data.join("app.info"), "\u{feff}Company\r\nGame Title\r\n").unwrap();
        let player_log = product_logs.join("Player.log");
        std::fs::write(&player_log, "[Vulkan init] extensions: count=15\nDirect3D:\n    Version: Direct3D 11.0 [level 11.1]\n").unwrap();
        assert_eq!(unity_last_api_at(&exe, &local_low), Some("D3D11"));

        std::fs::write(&player_log, "Forcing GfxDevice: Vulkan\n[Vulkan init] Physical Device 1\n").unwrap();
        assert_eq!(unity_last_api_at(&exe, &local_low), None);

        std::fs::write(&player_log, "Vulkan renderer=[Example GPU] id=[1]\n").unwrap();
        assert_eq!(unity_last_api_at(&exe, &local_low), Some("Vulkan"));

        std::fs::write(data.join("app.info"), "Other Company\nGame Title\n").unwrap();
        assert_eq!(unity_last_api_at(&exe, &local_low), None);
    }
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
