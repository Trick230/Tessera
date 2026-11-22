// SPDX-License-Identifier: GPL-3.0-or-later
//! Offline preparation of recorded shaders and immutable textures.

use std::io;
use std::os::windows::process::CommandExt;
use std::path::Path;
use std::process::Command;

pub const TEXCOMP_EXE: &str = "tessera_texcomp.exe";

pub fn prepare(exe: &Path, texcomp: &Path) -> io::Result<String> {
    if !exe.is_file() {
        return Err(io::Error::other("game executable not found"));
    }
    if crate::warm::is_running(exe) {
        return Err(io::Error::other(
            "close the game before preparing quality caches",
        ));
    }
    let key =
        crate::session::exe_key_name(exe).ok_or_else(|| io::Error::other("invalid game path"))?;
    let dir = crate::paths::data_dir().join("quality");
    let raw = dir.join(format!("{key}.raw"));
    let packed = dir.join(format!("{key}.packed"));
    let count = crate::mip_feedback::prepare(exe)?;
    let mut summary = format!("Prepared {count} pixel shaders for mip feedback.");
    if raw.is_dir() {
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        let output = Command::new(texcomp)
            .arg("--batch")
            .arg(raw)
            .arg(packed)
            .creation_flags(CREATE_NO_WINDOW)
            .output()?;
        if !output.status.success() {
            return Err(io::Error::other(format!(
                "Texture preparation failed ({}): {}{}",
                output.status,
                String::from_utf8_lossy(&output.stderr),
                String::from_utf8_lossy(&output.stdout)
            )));
        }
        summary.push('\n');
        summary.push_str(String::from_utf8_lossy(&output.stdout).trim());
    }
    Ok(summary)
}
