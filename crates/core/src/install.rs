// SPDX-License-Identifier: GPL-3.0-or-later
//! Install / uninstall of the graphics layer next to the game executable.
//!
//! An existing DLL with the proxy name (ReShade, DXVK, Special K, ...) is
//! renamed to `<stem>.next.dll` and recorded in `tessera.ini` as the next
//! layer. Uninstall removes our files and restores that name.

use std::fs;
use std::io;
use std::path::Path;

pub const PROXY_D3D11: &str = "d3d11.dll";
pub const INI: &str = "tessera.ini";
pub const MARKER: &str = "tessera.install";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InstallState {
    NotInstalled,
    /// Our layer is installed; `chained` is the renamed DLL we load next.
    Installed { proxy: String, chained: Option<String> },
    /// A DLL with the proxy name exists and is not ours; install chains it.
    Foreign { proxy: String },
}

impl InstallState {
    pub fn is_installed(&self) -> bool {
        matches!(self, InstallState::Installed { .. })
    }
}

fn next_name(proxy: &str) -> String {
    let stem = proxy.strip_suffix(".dll").unwrap_or(proxy);
    format!("{stem}.next.dll")
}

struct Marker {
    proxy: String,
    chained: Option<String>,
}

fn read_marker(dir: &Path) -> Option<Marker> {
    let text = fs::read_to_string(dir.join(MARKER)).ok()?;
    let mut proxy = None;
    let mut chained = None;
    for line in text.lines() {
        match line.split_once('=') {
            Some(("proxy", v)) => proxy = Some(v.trim().to_string()),
            Some(("chained", v)) if !v.trim().is_empty() => chained = Some(v.trim().to_string()),
            _ => {}
        }
    }
    Some(Marker { proxy: proxy?, chained })
}

pub fn state(dir: &Path, proxy: &str) -> InstallState {
    if let Some(m) = read_marker(dir) {
        return InstallState::Installed { proxy: m.proxy, chained: m.chained };
    }
    if dir.join(proxy).exists() {
        InstallState::Foreign { proxy: proxy.to_string() }
    } else {
        InstallState::NotInstalled
    }
}

/// Copies `payload` as `dir/proxy`, chaining any foreign DLL already there.
/// Reinstalling over our own layer only replaces the DLL.
pub fn install(dir: &Path, payload: &Path, proxy: &str) -> io::Result<InstallState> {
    if !payload.is_file() {
        return Err(io::Error::new(io::ErrorKind::NotFound, format!("camada não encontrada: {}", payload.display())));
    }
    let target = dir.join(proxy);
    let mut chained = read_marker(dir).and_then(|m| m.chained);

    if read_marker(dir).is_none() && target.exists() {
        let next = next_name(proxy);
        if dir.join(&next).exists() {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("{next} já existe; não sobrescrevo")));
        }
        fs::rename(&target, dir.join(&next))?;
        chained = Some(next);
    }

    // Copy to a temp name first so a failed copy never leaves a broken proxy.
    let tmp = dir.join(format!("{proxy}.tessera-tmp"));
    if let Err(e) = fs::copy(payload, &tmp).and_then(|_| fs::rename(&tmp, &target)) {
        let _ = fs::remove_file(&tmp);
        return Err(e);
    }

    let next = chained.clone().unwrap_or_default();
    write_ini(dir, &next)?;
    fs::write(dir.join(MARKER), format!("proxy={proxy}\nchained={next}\n"))?;
    Ok(InstallState::Installed { proxy: proxy.to_string(), chained })
}

/// Writes `next=` into tessera.ini and keeps the other settings (the
/// per-optimization switches) across reinstalls.
fn write_ini(dir: &Path, next: &str) -> io::Result<()> {
    let old = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    let mut out = format!("[tessera]\nnext={next}\n");
    for line in old.lines().map(str::trim) {
        let key = line.split('=').next().unwrap_or("").trim().to_ascii_lowercase();
        if line.is_empty() || line.starts_with('[') || key == "next" {
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
    fs::write(dir.join(INI), out)
}

/// Removes our layer and restores a chained DLL to its original name.
pub fn uninstall(dir: &Path) -> io::Result<InstallState> {
    let Some(m) = read_marker(dir) else {
        return Err(io::Error::new(io::ErrorKind::NotFound, "o Tessera não está instalado aqui"));
    };
    let target = dir.join(&m.proxy);
    if target.exists() {
        fs::remove_file(&target)?;
    }
    if let Some(next) = &m.chained {
        let from = dir.join(next);
        if from.exists() {
            fs::rename(from, &target)?;
        }
    }
    let _ = fs::remove_file(dir.join(INI));
    fs::remove_file(dir.join(MARKER))?;
    Ok(state(dir, &m.proxy))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn install_and_uninstall_clean_folder() {
        let t = tempfile::tempdir().unwrap();
        let payload = t.path().join("payload.dll");
        fs::write(&payload, b"ours").unwrap();
        let game = t.path().join("game");
        fs::create_dir(&game).unwrap();

        let s = install(&game, &payload, PROXY_D3D11).unwrap();
        assert_eq!(s, InstallState::Installed { proxy: "d3d11.dll".into(), chained: None });
        assert_eq!(fs::read(game.join("d3d11.dll")).unwrap(), b"ours");

        assert_eq!(uninstall(&game).unwrap(), InstallState::NotInstalled);
        assert_eq!(fs::read_dir(&game).unwrap().count(), 0);
    }

    #[test]
    fn foreign_dll_is_chained_and_restored() {
        let t = tempfile::tempdir().unwrap();
        let payload = t.path().join("payload.dll");
        fs::write(&payload, b"ours").unwrap();
        let game = t.path().join("game");
        fs::create_dir(&game).unwrap();
        fs::write(game.join("d3d11.dll"), b"reshade").unwrap();
        assert!(matches!(state(&game, PROXY_D3D11), InstallState::Foreign { .. }));

        install(&game, &payload, PROXY_D3D11).unwrap();
        assert_eq!(fs::read(game.join("d3d11.next.dll")).unwrap(), b"reshade");
        assert!(fs::read_to_string(game.join(INI)).unwrap().contains("next=d3d11.next.dll"));

        // Reinstall must not chain our own DLL, and keeps the user's settings.
        let ini = fs::read_to_string(game.join(INI)).unwrap();
        fs::write(game.join(INI), format!("{ini}fps_limit=141\n")).unwrap();
        install(&game, &payload, PROXY_D3D11).unwrap();
        assert_eq!(fs::read(game.join("d3d11.next.dll")).unwrap(), b"reshade");
        let ini = fs::read_to_string(game.join(INI)).unwrap();
        assert_eq!(ini, "[tessera]\nnext=d3d11.next.dll\nfps_limit=141\n");

        uninstall(&game).unwrap();
        assert_eq!(fs::read(game.join("d3d11.dll")).unwrap(), b"reshade");
        assert!(!game.join("d3d11.next.dll").exists());
        assert!(!game.join(INI).exists());
    }
}
