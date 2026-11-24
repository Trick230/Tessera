// SPDX-License-Identifier: GPL-3.0-or-later
//! Launches an opted-in game with its pipeline recorder or prepared cache.

use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use crate::scan::{self, ShaderApi};
use crate::{anticheat, recording, warm};

const VULKAN_LAYER: &str = "VK_LAYER_TESSERA_telemetry";
const D3D12_PROXY: &str = "tessera_d3d12_capture.dll";

fn relative_to_game(exe: &Path, root: &Path) -> io::Result<PathBuf> {
    let absolute = std::path::absolute(exe)?;
    let actual = fs::canonicalize(exe)?;
    let root = fs::canonicalize(root)?;
    if !actual.is_file() || !actual.starts_with(&root) {
        return Err(io::Error::other("the game executable must be inside the selected game folder"));
    }
    if !anticheat::detect(&root).is_empty() {
        return Err(io::Error::other("pipeline recording and cache verification are unavailable for games with anti-cheat"));
    }
    if warm::is_running(exe) {
        return Err(io::Error::other("close the game before launching a pipeline session"));
    }
    Ok(absolute)
}

fn event_path(dir: &Path, label: &str, extension: &str) -> PathBuf {
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos();
    dir.join(format!("{label}-{}-{now}.{extension}", std::process::id()))
}

fn vulkan_layer(command: &mut Command, helper_dir: &Path) -> io::Result<()> {
    if !helper_dir.join("TesseraTelemetry.json").is_file()
        || !helper_dir.join("tessera_vulkan_telemetry.dll").is_file()
    {
        return Err(io::Error::other("the Vulkan recording layer is not installed"));
    }
    let existing = std::env::var_os("VK_LAYER_PATH").unwrap_or_default();
    let paths = std::env::join_paths(std::iter::once(helper_dir.to_path_buf()).chain(std::env::split_paths(&existing)))
        .map_err(io::Error::other)?;
    let existing = std::env::var("VK_INSTANCE_LAYERS").unwrap_or_default();
    let mut layers: Vec<_> = existing.split(';').filter(|name| !name.is_empty()).collect();
    if !layers.contains(&VULKAN_LAYER) { layers.push(VULKAN_LAYER); }
    command.env("VK_LAYER_PATH", paths).env("VK_INSTANCE_LAYERS", layers.join(";"))
        .env_remove("TESSERA_VULKAN_CACHE_INPUT").env_remove("TESSERA_VULKAN_CACHE_OUTPUT")
        .env_remove("TESSERA_VULKAN_TELEMETRY").env_remove("TESSERA_VULKAN_COMPUTE_CAPTURE");
    Ok(())
}

struct StagedProxy {
    path: PathBuf,
    payload: Vec<u8>,
    owned: bool,
}

impl StagedProxy {
    fn enter(path: PathBuf, helper_dir: &Path) -> io::Result<Self> {
        let payload = fs::read(helper_dir.join(D3D12_PROXY))?;
        if let Ok(existing) = fs::read(&path) {
            if existing == payload { return Ok(Self { path, payload, owned: false }); }
            return Err(io::Error::other("the game already has a different d3d12.dll"));
        }
        let mut file = OpenOptions::new().write(true).create_new(true).open(&path)?;
        if let Err(error) = file.write_all(&payload) {
            drop(file);
            let _ = fs::remove_file(&path);
            return Err(error);
        }
        Ok(Self { path, payload, owned: true })
    }

    fn finish(&mut self) -> io::Result<()> {
        if !self.owned { return Ok(()); }
        self.owned = false;
        if fs::read(&self.path).is_ok_and(|bytes| bytes == self.payload) {
            fs::remove_file(&self.path)?;
        }
        Ok(())
    }
}

impl Drop for StagedProxy {
    fn drop(&mut self) { let _ = self.finish(); }
}

pub fn record(exe: &Path, root: &Path, api: ShaderApi, helper_dir: &Path) -> io::Result<u64> {
    let dir = recording::directory(exe, api).ok_or_else(|| io::Error::other("unsupported pipeline recording API"))?;
    let dir = std::path::absolute(dir)?;
    let trace = {
        let _lock = scan::lock(exe)?;
        let absolute = relative_to_game(exe, root)?;
        let game_dir = absolute.parent().ok_or_else(|| io::Error::other("missing game directory"))?;
        fs::create_dir_all(&dir)?;
        let mut game = Command::new(&absolute);
        game.current_dir(game_dir);
        let trace = event_path(&dir, "recording", if api == ShaderApi::D3d12 { "td12" } else { "tvc" });
        let mut proxy = None;
        if api == ShaderApi::D3d12 {
            proxy = Some(StagedProxy::enter(game_dir.join("d3d12.dll"), helper_dir)?);
            game.env("TESSERA_D3D12_CAPTURE", &trace);
        } else {
            vulkan_layer(&mut game, helper_dir)?;
            game.env("TESSERA_VULKAN_COMPUTE_CAPTURE", &trace);
        }
        let exit = game.status()?;
        if let Some(mut proxy) = proxy { proxy.finish()?; }
        if !trace.is_file() {
            return Err(io::Error::other(format!("the game did not produce a pipeline recording ({exit})")));
        }
        trace
    };
    recording::import(exe, api, &trace)
}

pub fn play_vulkan(exe: &Path, root: &Path, helper_dir: &Path) -> io::Result<String> {
    let dir = recording::directory(exe, ShaderApi::Vulkan).ok_or_else(|| io::Error::other("invalid game path"))?;
    let dir = std::path::absolute(dir)?;
    let _lock = scan::lock(exe)?;
    let report = recording::prepared_vulkan(exe, &dir)
        .ok_or_else(|| io::Error::other("prepare the Vulkan recording for the current driver first"))?;
    let absolute = relative_to_game(exe, root)?;
    let output = event_path(&dir, "session", "csv");
    let mut game = Command::new(&absolute);
    vulkan_layer(&mut game, helper_dir)?;
    let exit = game.current_dir(absolute.parent().unwrap())
        .env("TESSERA_VULKAN_CACHE_INPUT", dir.join("pipeline-cache.bin"))
        .env("TESSERA_VULKAN_TELEMETRY", &output).status()?;
    if !exit.success() {
        return Err(io::Error::other(format!("game exited before successful cache verification ({exit})")));
    }
    let summary = summarize(&output)?;
    recording::save_verification(&dir, report, &summary)?;
    Ok(summary)
}

fn summarize(path: &Path) -> io::Result<String> {
    let lines = BufReader::new(File::open(path)?).lines();
    let mut count = 0u64;
    let mut known = 0u64;
    let mut hit = 0u64;
    for line in lines.skip(1) {
        let line = line?;
        let fields: Vec<_> = line.split(',').collect();
        if fields.len() < 8 || fields[7] != "layer" || !matches!(fields[0], "graphics" | "compute") {
            continue;
        }
        let parse = |index: usize| fields[index].parse::<u64>().ok();
        let (Some(pipelines), Some(valid), Some(hits)) = (parse(3), parse(4), parse(5)) else { continue };
        if hits > valid || valid > pipelines { continue; }
        count = count.saturating_add(pipelines);
        known = known.saturating_add(valid);
        hit = hit.saturating_add(hits);
    }
    if known == 0 { return Ok(format!("No valid Vulkan cache feedback for {count} pipeline creations using Tessera's cache.")); }
    Ok(format!("Vulkan cache hits: {hit} of {known} pipeline creations with valid feedback ({count} used Tessera's cache)."))
}
