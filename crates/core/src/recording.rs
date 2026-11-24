// SPDX-License-Identifier: GPL-3.0-or-later
//! Application-managed, API-specific pipeline recordings and replay results.

use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufWriter, Write};
use std::os::windows::fs::OpenOptionsExt;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::{Value, json};
use xxhash_rust::xxh3::{Xxh3, xxh3_128};

use crate::scan::{self, ShaderApi};
use crate::{driver, warm};

const MAX_D3D12: usize = 512 << 20;
const MAX_VULKAN: usize = 256 << 20;
pub const VULKAN_REPLAY_EXE: &str = "tessera_vulkan_replay.exe";

struct Format {
    header: usize,
    record: usize,
    max_file: usize,
    d3d12: bool,
}

fn read_u32(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}

fn format(api: ShaderApi, bytes: &[u8]) -> io::Result<Format> {
    match api {
        ShaderApi::D3d12 if bytes.len() >= 16 && bytes.starts_with(b"TSRD12C\0")
            && read_u32(bytes, 8) == 1 && read_u32(bytes, 12) == 0 =>
            Ok(Format { header: 16, record: 32, max_file: MAX_D3D12, d3d12: true }),
        ShaderApi::Vulkan if bytes.len() >= 36
            && (bytes.starts_with(b"TSRVKC1\0") || bytes.starts_with(b"TSRVKC2\0")) =>
            Ok(Format { header: 36, record: 4, max_file: MAX_VULKAN, d3d12: false }),
        _ => Err(io::Error::other("the recording format does not match the selected API")),
    }
}

impl Format {
    fn records<'a>(&self, bytes: &'a [u8]) -> io::Result<Vec<&'a [u8]>> {
        if bytes.len() > self.max_file { return Err(io::Error::other("recording is too large")); }
        let mut bytes = &bytes[self.header..];
        let mut records = Vec::new();
        while !bytes.is_empty() {
            if bytes.len() < self.record { return Err(io::Error::other("truncated pipeline record")); }
            let payload = read_u32(bytes, 0) as usize;
            let dependent = if self.d3d12 { read_u32(bytes, 4) as usize } else { 0 };
            let maximum = if self.d3d12 {
                let kind = read_u32(bytes, 20);
                if kind > 4 || dependent > 1 << 20 { return Err(io::Error::other("unsupported D3D12 record")); }
                if kind == 0 { 16 << 20 } else { 64 << 20 }
            } else {
                32 << 20
            };
            let length = self.record + payload.min(maximum) + dependent;
            if payload == 0 || payload > maximum || length > bytes.len() || records.len() >= 500_000 {
                return Err(io::Error::other("invalid pipeline record size or count"));
            }
            let (record, rest) = bytes.split_at(length);
            records.push(record);
            bytes = rest;
        }
        Ok(records)
    }

    fn key<'a>(&self, bytes: &'a [u8]) -> (&'a [u8], &'a [u8]) {
        if self.d3d12 { (&bytes[..24], &bytes[32..]) } else { (&[], bytes) }
    }
}

pub(crate) fn directory(exe: &Path, api: ShaderApi) -> Option<PathBuf> {
    if !matches!(api, ShaderApi::D3d12 | ShaderApi::Vulkan) { return None; }
    let base = scan::scan_base_for(exe, api)?;
    Some(crate::paths::data_dir().join("recordings").join(base.file_name()?))
}

pub(crate) fn prepared_vulkan(exe: &Path, dir: &Path) -> Option<Value> {
    let active = active(dir)?;
    let report = json_file(&dir.join("prepared.json"))?;
    (report["capture"].as_str() == Some(&active.key) && report["environment"] == environment(exe)
        && cache_is_current(&report, &dir.join("pipeline-cache.bin"))).then_some(report)
}

pub(crate) fn save_verification(dir: &Path, mut report: Value, result: &str) -> io::Result<()> {
    report["verification"] = json!(result);
    write_json(&dir.join("prepared.json"), report)
}

fn identity(path: &Path) -> Option<Value> {
    let meta = fs::metadata(path).ok()?;
    let modified = meta.modified().ok()?.duration_since(UNIX_EPOCH).ok()?;
    Some(json!([meta.len(), modified.as_secs(), modified.subsec_nanos()]))
}

fn environment(exe: &Path) -> Value {
    let mut gpus: Vec<_> = driver::gpus().into_iter().map(|gpu| (gpu.name, gpu.driver_version)).collect();
    gpus.sort();
    let root = exe.parent().unwrap_or_else(|| Path::new(""));
    json!({
        "exe": identity(exe),
        "drivers": gpus,
        "config": identity(&root.join("config.toml")),
        "tessera": identity(&root.join("tessera.ini")),
    })
}

fn json_file(path: &Path) -> Option<Value> { serde_json::from_slice(&fs::read(path).ok()?).ok() }

fn write_json(path: &Path, value: Value) -> io::Result<()> {
    let temporary = warm::with_suffix(path, ".tmp");
    fs::write(&temporary, value.to_string())?;
    fs::rename(temporary, path)
}

struct Active {
    file: PathBuf,
    key: String,
    count: u64,
}

fn active(dir: &Path) -> Option<Active> {
    let manifest = json_file(&dir.join("active.json"))?;
    let key = manifest["key"].as_str()?;
    if key.len() != 32 || !key.bytes().all(|b| b.is_ascii_hexdigit()) { return None; }
    let file = dir.join(format!("{key}.capture"));
    file.is_file().then_some(Active { file, key: key.to_owned(), count: manifest["count"].as_u64()? })
}

pub struct Status {
    pub count: u64,
    pub last_result: Option<String>,
    pub verified: Option<String>,
    pub can_play: bool,
    pub changed: bool,
}

pub fn status(exe: &Path, api: ShaderApi) -> Option<Status> {
    let dir = directory(exe, api)?;
    let active = active(&dir)?;
    let report = json_file(&dir.join("prepared.json"));
    let current = report.as_ref().filter(|report| {
        report["capture"].as_str() == Some(&active.key) && report["environment"] == environment(exe)
    });
    Some(Status {
        count: active.count,
        last_result: current.and_then(|report| report["summary"].as_str()).map(str::to_owned),
        verified: current.and_then(|report| report["verification"].as_str()).map(str::to_owned),
        can_play: api == ShaderApi::Vulkan
            && current.is_some_and(|report| cache_is_current(report, &dir.join("pipeline-cache.bin"))),
        changed: report.is_some() && current.is_none(),
    })
}

fn cache_is_current(report: &Value, cache: &Path) -> bool {
    report["saved_cache"].as_array().is_some() && identity(cache).as_ref() == report.get("saved_cache")
}

struct ReadCapture {
    bytes: memmap2::Mmap,
    _file: File,
}

impl std::ops::Deref for ReadCapture {
    type Target = [u8];
    fn deref(&self) -> &[u8] { &self.bytes }
}

fn open_capture(path: &Path) -> io::Result<ReadCapture> {
    let file = OpenOptions::new().read(true).share_mode(1).open(path)?;
    if file.metadata()?.len() > MAX_D3D12 as u64 { return Err(io::Error::other("recording is too large")); }
    // Deny writes and deletion while the mapped recording is read.
    let bytes = unsafe { memmap2::Mmap::map(&file) }?;
    Ok(ReadCapture { bytes, _file: file })
}

pub fn import(exe: &Path, api: ShaderApi, path: &Path) -> io::Result<u64> {
    let dir = directory(exe, api).ok_or_else(|| io::Error::other("unsupported pipeline recording API"))?;
    let _lock = scan::lock(exe)?;
    let new = open_capture(path)?;
    let format = format(api, &new)?;
    let incoming = format.records(&new)?;
    if incoming.is_empty() { return Err(io::Error::other("the recording has no pipelines")); }
    let old = active(&dir).map(|active| open_capture(&active.file)).transpose()?;
    let header = &new[..format.header];
    let mut unique = HashMap::<u128, Vec<&[u8]>>::new();
    let mut records = Vec::new();
    let mut size = header.len();
    let existing = match old.as_ref() {
        Some(bytes) if bytes.starts_with(header) => format.records(bytes)?,
        _ => Vec::new(),
    };
    let mut hash = Xxh3::new();
    hash.update(header);
    for record in existing.into_iter().chain(incoming) {
        let key = format.key(record);
        let identity = xxh3_128(key.1) ^ xxh3_128(key.0).rotate_left(53);
        let entries = unique.entry(identity).or_default();
        if entries.iter().any(|entry| format.key(entry) == key) { continue; }
        size += record.len();
        if size > format.max_file || records.len() == 500_000 {
            return Err(io::Error::other("the merged recording is too large"));
        }
        entries.push(record);
        records.push(record);
        hash.update(record);
    }
    let key = format!("{:032x}", hash.digest128());
    let count = records.len() as u64;
    fs::create_dir_all(&dir)?;
    let destination = dir.join(format!("{key}.capture"));
    if !destination.is_file() {
        let temporary = dir.join("import.tmp");
        let mut out = BufWriter::new(File::create(&temporary)?);
        out.write_all(header)?;
        for record in records { out.write_all(record)?; }
        out.flush()?;
        drop(out);
        fs::rename(temporary, destination)?;
    }
    write_json(&dir.join("active.json"), json!({ "key": key, "count": count }))?;
    Ok(count)
}

pub fn prepare(exe: &Path, api: ShaderApi, helper_dir: &Path) -> io::Result<String> {
    let dir = directory(exe, api).ok_or_else(|| io::Error::other("unsupported pipeline recording API"))?;
    let _lock = scan::lock(exe)?;
    if !exe.is_file() || warm::is_running(exe) {
        return Err(io::Error::other("the game must be installed and closed to prepare its recording"));
    }
    let active = active(&dir).ok_or_else(|| io::Error::other("there is no imported pipeline recording"))?;
    let scope = environment(exe);
    let report_path = dir.join("prepared.json");
    let result = if api == ShaderApi::D3d12 {
        let base = scan::scan_base_for(exe, api).ok_or_else(|| io::Error::other("invalid game path"))?;
        warm::replay_d3d12_locked(exe, &helper_dir.join(warm::WARM_EXE), &active.file, &base)
    } else {
        let helper = helper_dir.join(VULKAN_REPLAY_EXE);
        if !helper.is_file() { return Err(io::Error::other("the Vulkan replay helper is not installed")); }
        let cache = dir.join("pipeline-cache.bin");
        if json_file(&report_path).is_some_and(|report| {
            report["capture"].as_str() == Some(&active.key) && report["environment"] == scope
                && cache_is_current(&report, &cache)
        }) {
            return Ok("The Vulkan cache for this recording is already prepared.".to_owned());
        }
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos();
        let temporary = dir.join(format!("replay-{}-{now}.bin", std::process::id()));
        let output = Command::new(helper).creation_flags(0x0800_0000)
            .arg(&active.file).arg(&temporary).output();
        output.and_then(|output| {
            if !output.status.success() {
                return Err(io::Error::other(String::from_utf8_lossy(&output.stderr).trim().to_owned()));
            }
            fs::rename(temporary, cache)?;
            Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
        })
    };
    let summary = match &result {
        Ok(summary) => summary.clone(),
        Err(error) => format!("Preparation failed ({error})"),
    };
    let saved_cache = (api == ShaderApi::Vulkan && result.is_ok())
        .then(|| identity(&dir.join("pipeline-cache.bin"))).flatten();
    write_json(&report_path, json!({
        "capture": active.key, "environment": scope, "summary": summary, "saved_cache": saved_cache,
    }))?;
    result
}
