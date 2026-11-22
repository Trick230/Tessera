// SPDX-License-Identifier: GPL-3.0-or-later
//! Offline preparation of recorded shaders and immutable textures.

use std::fs;
use std::io::{self, Read};
use std::os::windows::process::CommandExt;
use std::path::Path;
use std::process::Command;

pub const TEXCOMP_EXE: &str = "tessera_texcomp.exe";

#[derive(Clone, Copy, Debug, Default)]
pub struct Status {
    pub captured: u32,
    pub approved: u32,
    pub shaders: u32,
}

fn texture_count(dir: &Path, extension: &str) -> u32 {
    fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| {
            e.path().extension().is_some_and(|x| x == extension)
                && e.file_type().is_ok_and(|t| t.is_file())
        })
        .count() as u32
}

pub fn status(exe: &Path) -> Status {
    let Some(key) = crate::session::exe_key_name(exe) else {
        return Status::default();
    };
    let data = crate::paths::data_dir();
    let dir = data.join("quality");
    let mut header = [0u8; 16];
    let shaders = fs::File::open(data.join("feedback").join(format!("{key}.tmip")))
        .and_then(|mut f| f.read_exact(&mut header))
        .ok()
        .filter(|_| &header[..8] == b"TSRMIP1\0")
        .map_or(0, |_| {
            u32::from_le_bytes(header[8..12].try_into().unwrap()).min(4096)
        });
    Status {
        captured: texture_count(&dir.join(format!("{key}.raw")), "txr"),
        approved: texture_count(&dir.join(format!("{key}.packed")), "tbc"),
        shaders,
    }
}

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
    if texture_count(&raw, "txr") != 0 {
        if !texcomp.is_file() {
            summary.push_str(" The texture compressor is unavailable.");
            return Ok(summary);
        }
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
