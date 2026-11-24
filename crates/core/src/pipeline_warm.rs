// SPDX-License-Identifier: GPL-3.0-or-later
//! Replay pipeline recordings through the optional Windows SCSKiller backend.

use std::fs::{self, File};
use std::io::{self, BufReader, Read, Seek, SeekFrom};
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::scan::{self, ShaderApi};
use crate::warm::{self, with_suffix};

fn link_path(exe: &Path, api: ShaderApi) -> Option<PathBuf> {
    match api {
        ShaderApi::D3d12 | ShaderApi::Vulkan => Some(with_suffix(&scan::scan_base_for(exe, api)?, ".capture")),
        _ => None,
    }
}

pub fn recording(exe: &Path, api: ShaderApi) -> Option<PathBuf> {
    let path = PathBuf::from(fs::read_to_string(link_path(exe, api)?).ok()?);
    path.is_file().then_some(path)
}

pub fn link(exe: &Path, api: ShaderApi, capture: &Path) -> io::Result<()> {
    let path = link_path(exe, api).ok_or_else(|| io::Error::other("invalid pipeline API or game path"))?;
    let capture = fs::canonicalize(capture)?;
    let _lock = scan::lock(exe)?;
    let temp = with_suffix(&path, ".tmp");
    fs::write(&temp, capture.to_str().ok_or_else(|| io::Error::other("invalid recording path"))?)?;
    fs::rename(temp, path)
}

// The compact recorder format is a versioned Brotli stream over the raw records.
fn d3d12_recording(capture: &Path, dest: &Path) -> io::Result<()> {
    let mut file = File::open(capture)?;
    let mut header = [0; 17];
    file.read_exact(&mut header)?;
    let mut out = File::create(dest)?;
    if &header[..8] != b"\0SCSKREC" {
        file.seek(SeekFrom::Start(0))?;
        io::copy(&mut file, &mut out)?;
    } else {
        if header[8] != 1 {
            return Err(io::Error::other("unsupported compact pipeline recording version"));
        }
        let length = u64::from_le_bytes(header[9..].try_into().unwrap());
        let decoded = brotli_decompressor::Decompressor::new(BufReader::new(file), 64 << 10);
        if io::copy(&mut decoded.take(length.saturating_add(1)), &mut out)? != length {
            return Err(io::Error::other("pipeline recording length does not match its header"));
        }
    }
    drop(out);
    let file = File::open(dest)?;
    let length = file.metadata()?.len();
    let mut reader = BufReader::new(file);
    let mut offset = 0;
    while offset < length {
        let mut record = [0; 5];
        reader.read_exact(&mut record)?;
        if matches!(record[0], b'1'..=b'3' | b'D') {
            return Err(io::Error::other("the recording contains D3D11 items"));
        }
        let size = u32::from_le_bytes(record[1..].try_into().unwrap());
        offset += 5 + u64::from(size);
        if offset > length {
            return Err(io::Error::other("truncated D3D12 pipeline recording"));
        }
        reader.seek_relative(i64::from(size))?;
    }
    Ok(())
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct ReplayStats {
    pub prepared: u64,
    pub failed: u64,
    pub total: u64,
}

fn replay_stats(stdout: &[u8]) -> io::Result<ReplayStats> {
    let end = stdout.rsplit(|&byte| byte == b'\n')
        .filter_map(|line| serde_json::from_slice::<serde_json::Value>(line).ok())
        .find(|record| record["event"] == "done")
        .ok_or_else(|| io::Error::other("pipeline replay returned no result"))?;
    let total = end["total"].as_u64().ok_or_else(|| io::Error::other("missing pipeline total"))?;
    let done = end["done"].as_u64().ok_or_else(|| io::Error::other("missing pipeline count"))?;
    if end["stopped"] == true || done != total {
        return Err(io::Error::other("pipeline replay stopped before completion"));
    }
    let count = |key: &str| end[key].as_u64().unwrap_or_default();
    let crashed = end["crashed"].as_array().map_or(0, |items| items.len() as u64);
    let failed = count("failed").saturating_add(count("driverSkipped"))
        .saturating_add(count("unsupported")).saturating_add(count("invalid"))
        .saturating_add(count("passedOver")).saturating_add(crashed).min(total);
    Ok(ReplayStats { prepared: total - failed, failed, total })
}

pub fn run(exe: &Path, api: ShaderApi, capture: &Path) -> io::Result<ReplayStats> {
    if !matches!(api, ShaderApi::D3d12 | ShaderApi::Vulkan) {
        return Err(io::Error::other("unsupported pipeline API"));
    }
    let backend = std::env::var_os("LOCALAPPDATA").map(PathBuf::from)
        .map(|dir| dir.join("SCSKiller.App/current/native/scskiller_warm.exe"))
        .filter(|path| path.is_file())
        .ok_or_else(|| io::Error::other("the Windows pipeline replay backend is unavailable"))?;
    if warm::is_running(exe) {
        return Err(io::Error::other("close the game before replaying pipelines"));
    }
    let _lock = scan::lock(exe)?;
    let base = scan::scan_base_for(exe, api).ok_or_else(|| io::Error::other("invalid game path"))?;
    let work = crate::paths::data_dir().join("pipeline").join(base.file_name().unwrap());
    fs::create_dir_all(&work)?;
    let mut command = Command::new(backend);
    command.creation_flags(0x0800_0000).current_dir(&work).env("SCSKILLER_DATA_DIR", &work)
        .arg(&work).arg(exe.file_name().ok_or_else(|| io::Error::other("invalid game path"))?)
        .args(["--threads", "4", "--memory-mb", "2048"]);
    if api == ShaderApi::D3d12 {
        d3d12_recording(capture, &work.join("scskiller.db"))?;
    } else {
        fs::copy(capture, work.join("scskiller.foz"))?;
        command.arg("--vulkan");
    }
    let output = command.output()?;
    if !output.status.success() {
        let error = String::from_utf8_lossy(&output.stderr);
        return Err(io::Error::other(format!(
            "pipeline replay failed ({}): {}", output.status, error.lines().last().unwrap_or_default()
        )));
    }
    replay_stats(&output.stdout)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_only_pipelines_replayed_successfully() {
        let result = replay_stats(br#"{"event":"done","done":8,"total":8,"failed":1,"driverSkipped":1,"invalid":1,"unsupported":1,"crashed":["abc"]}"#).unwrap();
        assert_eq!(result, ReplayStats { prepared: 3, failed: 5, total: 8 });
        assert!(replay_stats(br#"{"event":"done","done":7,"total":8,"stopped":true}"#).is_err());
    }

    #[test]
    fn rejects_mixed_d3d11_recordings_and_truncated_records() {
        let dir = tempfile::tempdir().unwrap();
        let capture = dir.path().join("input.db");
        let staged = dir.path().join("staged.db");
        fs::write(&capture, [b'G', 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, b'1', 0, 0, 0, 0]).unwrap();
        assert!(d3d12_recording(&capture, &staged).unwrap_err().to_string().contains("D3D11"));
        fs::write(&capture, [b'G', 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]).unwrap();
        assert!(d3d12_recording(&capture, &staged).unwrap_err().to_string().contains("truncated"));
    }
}
