// SPDX-License-Identifier: GPL-3.0-or-later
//! Install / uninstall of the graphics layer next to the game executable.
//!
//! An existing DLL with the proxy name (ReShade, DXVK, Special K, ...) is
//! renamed to `<stem>.next.dll` and recorded in `tessera.ini` as the next
//! layer. Uninstall removes our files and restores that name.

use std::fs;
use std::io;
use std::path::Path;
use std::sync::Mutex;

/// Every tessera.ini writer (UI thread and the settings loader) goes through
/// this, so a read-modify-write never drops another's change.
static INI_LOCK: Mutex<()> = Mutex::new(());

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
        return Err(io::Error::new(io::ErrorKind::NotFound, format!("layer not found: {}", payload.display())));
    }
    let target = dir.join(proxy);
    let mut chained = read_marker(dir).and_then(|m| m.chained);

    if read_marker(dir).is_none() && target.exists() {
        let next = next_name(proxy);
        if dir.join(&next).exists() {
            return Err(io::Error::new(io::ErrorKind::AlreadyExists, format!("{next} already exists; not overwriting it")));
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
    let _g = INI_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    set_key(dir, "next", Some(next))
}

/// Sets (or with None removes) one key in tessera.ini, keeping every other
/// line. Caller holds INI_LOCK.
fn set_key(dir: &Path, name: &str, value: Option<&str>) -> io::Result<()> {
    set_keys(dir, &[(name, value)])
}

fn set_keys(dir: &Path, keys: &[(&str, Option<&str>)]) -> io::Result<()> {
    let old = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    let mut out = String::from("[tessera]\n");
    for &(name, value) in keys {
        if let Some(v) = value {
            out.push_str(&format!("{name}={v}\n"));
        }
    }
    for line in old.lines().map(str::trim) {
        let key = line.split('=').next().unwrap_or("").trim().to_ascii_lowercase();
        if line.is_empty() || line.starts_with('[') || keys.iter().any(|(name, _)| key == *name) {
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
    fs::write(dir.join(INI), out)
}

/// P1: records the lowest frame limit another tool (RTSS, NVIDIA control
/// panel) applies to this game as `ext_fps_limit`. It takes precedence over
/// an equal or higher Tessera cap. Rewrites tessera.ini only on a change.
pub fn note_external_limit(dir: &Path, exe_name: &str) -> io::Result<Option<crate::limits::ExternalLimit>> {
    let found = crate::limits::external_fps_limit(exe_name);
    let _g = INI_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    // Uninstalled while the limits were read: leave no ini behind.
    if read_marker(dir).is_none() {
        return Ok(found);
    }
    let text = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    let cur = text
        .lines()
        .filter_map(|l| l.split_once('='))
        .find(|(k, _)| k.trim().eq_ignore_ascii_case("ext_fps_limit"))
        .and_then(|(_, v)| v.trim().parse::<u32>().ok())
        .unwrap_or(0);
    let want = found.map_or(0, |l| l.fps);
    if want != cur {
        set_key(dir, "ext_fps_limit", (want > 0).then(|| want.to_string()).as_deref())?;
    }
    Ok(found)
}

/// Per-game switches the layer reads from tessera.ini (src/dll/config.cpp).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Settings {
    pub process: bool,
    pub residency: bool,
    /// 0 = off.
    pub fps_limit: u32,
    /// Budget cap reported to the game, 0 = off.
    pub vram_budget_mb: u32,
    /// P2: windowed blit swapchains become flip model; off by default.
    pub flip_model: bool,
    /// R1: big render targets the game releases come back on its next create; off by default.
    pub rt_recycle: bool,
    /// U1: Unreal texture streaming values (streaming_profile) set in the engine; off by default.
    pub ue_streaming: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { process: true, residency: false, fps_limit: 0, vram_budget_mb: 0, flip_model: false, rt_recycle: false, ue_streaming: false }
    }
}

const SETTING_KEYS: [&str; 7] =
    ["process", "residency", "fps_limit", "vram_budget_mb", "flip_model", "rt_recycle", "ue_streaming"];

pub fn settings(dir: &Path) -> Settings {
    let mut s = Settings::default();
    let text = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    for line in text.lines() {
        let Some((k, v)) = line.split_once('=') else { continue };
        let v: u32 = v.trim().parse().unwrap_or(0);
        match k.trim().to_ascii_lowercase().as_str() {
            "process" => s.process = v != 0,
            "residency" => s.residency = v != 0,
            "fps_limit" => s.fps_limit = v,
            "vram_budget_mb" => s.vram_budget_mb = v,
            "flip_model" => s.flip_model = v != 0,
            "rt_recycle" => s.rt_recycle = v != 0,
            "ue_streaming" => s.ue_streaming = v != 0,
            _ => {}
        }
    }
    s
}

/// Rewrites our keys in tessera.ini and keeps every other line (`next=`, manual keys).
pub fn save_settings(dir: &Path, s: &Settings) -> io::Result<()> {
    let _g = INI_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let old = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    let mut out = String::from("[tessera]\n");
    for line in old.lines().map(str::trim) {
        let key = line.split('=').next().unwrap_or("").trim().to_ascii_lowercase();
        if line.is_empty() || line.starts_with('[') || SETTING_KEYS.contains(&key.as_str()) {
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
    out.push_str(&format!(
        "process={}\nresidency={}\nfps_limit={}\nvram_budget_mb={}\nflip_model={}\nrt_recycle={}\nue_streaming={}\n",
        s.process as u8,
        s.residency as u8,
        s.fps_limit,
        s.vram_budget_mb,
        s.flip_model as u8,
        s.rt_recycle as u8,
        s.ue_streaming as u8
    ));
    fs::write(dir.join(INI), out)
}

/// Settings edited independently of the main FPS and VRAM controls.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AdvancedSettings {
    pub max_frame_latency: u32,
    pub d3d11_call_diagnostics: bool,
    pub present_cpu_sets: bool,
    pub mip_residency: bool,
    pub mip_prediction: bool,
    pub mip_streaming: bool,
    pub texture_dedup: bool,
    pub stream_io: u32,
    pub texture_quality: u32,
    pub mip_feedback: bool,
}

pub fn advanced_settings(dir: &Path) -> AdvancedSettings {
    let mut out = AdvancedSettings::default();
    let text = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        let value = value.trim().parse::<u32>().unwrap_or(0);
        match key.trim().to_ascii_lowercase().as_str() {
            "max_frame_latency" => out.max_frame_latency = value.min(16),
            "d3d11_call_diagnostics" => out.d3d11_call_diagnostics = value != 0,
            "present_cpu_sets" => out.present_cpu_sets = value != 0,
            "mip_residency" => out.mip_residency = value != 0,
            "mip_prediction" => out.mip_prediction = value != 0,
            "mip_streaming" => out.mip_streaming = value != 0,
            "texture_dedup" => out.texture_dedup = value != 0,
            "stream_io" => out.stream_io = value.min(2),
            "texture_quality" => out.texture_quality = value.min(2),
            "mip_feedback" => out.mip_feedback = value != 0,
            _ => {}
        }
    }
    out
}

/// The executable lives under the library's install folder. Store the depth
/// instead of writing a Unicode path to a Win32 profile string.
pub fn stream_root_levels(exe_dir: &Path, install_root: &Path) -> Option<u32> {
    let exe_dir = fs::canonicalize(exe_dir).ok()?;
    let install_root = fs::canonicalize(install_root).ok()?;
    exe_dir.ancestors().take(13).position(|p| p == install_root).map(|n| n as u32)
}

pub fn save_advanced_setting(dir: &Path, key: &str, value: u32, stream_levels: Option<u32>) -> io::Result<()> {
    let max = match key {
        "max_frame_latency" => 16,
        "stream_io" | "texture_quality" => 2,
        "present_cpu_sets" | "mip_residency" | "mip_prediction" | "mip_streaming" |
        "texture_dedup" | "mip_feedback" | "d3d11_call_diagnostics" => 1,
        _ => return Err(io::Error::new(io::ErrorKind::InvalidInput, "unknown setting")),
    };
    if value > max {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "setting out of range"));
    }
    let _g = INI_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    if read_marker(dir).is_none() {
        return Err(io::Error::new(io::ErrorKind::NotFound, "the layer is not installed"));
    }
    let value_string = value.to_string();
    if key == "stream_io" && value != 0 {
        if let Some(levels) = stream_levels.filter(|&n| n <= 12) {
            return set_keys(dir, &[(key, Some(&value_string)), ("stream_root_up", Some(&levels.to_string()))]);
        }
    }
    set_key(dir, key, Some(&value_string))
}

fn is_ue_key(key: &str) -> bool {
    key == "ue_image" || key.starts_with("cvar.")
}

/// What `apply_ue_cvars` wrote: variables set and those left out (with why).
#[derive(Debug, Default, PartialEq, Eq)]
pub struct UeCvars {
    pub set: usize,
    pub skipped: Vec<String>,
}

/// U1 variables tessera.ini asks for: the streaming profile (ue_streaming) and
/// each test variant whose key is non-zero (uecvars::VARIANTS).
fn ue_wanted(dir: &Path, vram_mb: u32) -> Vec<(&'static str, f64)> {
    let s = settings(dir);
    let mut wanted = Vec::new();
    if s.ue_streaming {
        // The cap the user set for this game counts as its memory.
        let vram_mb = if s.vram_budget_mb > 0 { vram_mb.min(s.vram_budget_mb) } else { vram_mb };
        wanted = crate::uecvars::streaming_profile(vram_mb);
    }
    let text = fs::read_to_string(dir.join(INI)).unwrap_or_default();
    for (key, vars) in crate::uecvars::VARIANTS {
        let on = text.lines().filter_map(|l| l.split_once('=')).any(|(k, v)| {
            k.trim().eq_ignore_ascii_case(key) && v.trim().parse::<u32>().unwrap_or(0) != 0
        });
        if on {
            wanted.extend_from_slice(vars);
        }
    }
    wanted
}

/// U1: rewrites the `ue_image`/`cvar.*` lines of tessera.ini for `exe` (the
/// process the layer loads in) from the ue_streaming setting. Off: the lines
/// go. On: the executable is scanned again (~0.1 s), so an updated game gets
/// fresh slots, never stale ones.
pub fn apply_ue_cvars(dir: &Path, exe: &Path, vram_mb: u32) -> io::Result<UeCvars> {
    let mut lines = Vec::new();
    let mut report = UeCvars::default();
    let wanted = ue_wanted(dir, vram_mb);
    if !wanted.is_empty() {
        let names: Vec<&str> = wanted.iter().map(|w| w.0).collect();
        match crate::uecvars::find_cvars(exe, &names)? {
            Ok((id, slots)) => {
                let (l, skipped) = crate::uecvars::ini_lines(id, &wanted, &slots);
                report.set = l.len().saturating_sub(1);
                report.skipped = skipped;
                lines = l;
            }
            Err(e) => report.skipped = names.iter().map(|n| format!("{n} ({e:?})")).collect(),
        }
    }
    let _g = INI_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let Ok(old) = fs::read_to_string(dir.join(INI)) else {
        return Ok(report); // not installed: nothing to keep in step
    };
    // Turned off while this scan ran (saves run on their own threads): no lines.
    if ue_wanted(dir, vram_mb).is_empty() {
        lines.clear();
        report = UeCvars::default();
    }
    let mut out = String::new();
    for line in old.lines().map(str::trim) {
        let key = line.split('=').next().unwrap_or("").trim().to_ascii_lowercase();
        if line.is_empty() || is_ue_key(&key) {
            continue;
        }
        out.push_str(line);
        out.push('\n');
    }
    for l in &lines {
        out.push_str(l);
        out.push('\n');
    }
    if out != old {
        fs::write(dir.join(INI), out)?;
    }
    Ok(report)
}

/// Removes our layer and restores a chained DLL to its original name.
pub fn uninstall(dir: &Path) -> io::Result<InstallState> {
    let _g = INI_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let Some(m) = read_marker(dir) else {
        return Err(io::Error::new(io::ErrorKind::NotFound, "Tessera is not installed here"));
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

    #[test]
    fn settings_round_trip_keeps_other_lines() {
        let t = tempfile::tempdir().unwrap();
        assert_eq!(settings(t.path()), Settings::default());
        fs::write(t.path().join(INI), "[tessera]\nnext=d3d11.next.dll\nfps_limit=30\nmax_frame_latency=1\n").unwrap();
        assert_eq!(settings(t.path()).fps_limit, 30);
        let s = Settings { process: false, residency: true, fps_limit: 141, vram_budget_mb: 6144, flip_model: true, rt_recycle: true, ue_streaming: true };
        save_settings(t.path(), &s).unwrap();
        assert_eq!(settings(t.path()), s);
        let ini = fs::read_to_string(t.path().join(INI)).unwrap();
        assert!(ini.starts_with("[tessera]\nnext=d3d11.next.dll\nmax_frame_latency=1\n"), "{ini}");
    }
    #[test]
    fn advanced_options_preserve_unrelated_settings_and_stream_root() {
        let t = tempfile::tempdir().unwrap();
        let root = t.path().join("Jogo Á");
        let exe_dir = root.join("Binaries").join("Win64");
        fs::create_dir_all(&exe_dir).unwrap();
        fs::write(exe_dir.join(MARKER), "proxy=d3d11.dll\n").unwrap();
        fs::write(exe_dir.join(INI), "[tessera]\nnext=d3d11.next.dll\nfps_limit=60\ntexture_dedup=1\n").unwrap();
        let levels = stream_root_levels(&exe_dir, &root);
        assert_eq!(levels, Some(2));
        save_advanced_setting(&exe_dir, "stream_io", 1, levels).unwrap();
        save_advanced_setting(&exe_dir, "texture_quality", 2, None).unwrap();
        assert!(save_advanced_setting(&exe_dir, "texture_quality", 3, None).is_err());
        assert!(save_advanced_setting(&exe_dir, "next", 1, None).is_err());
        let advanced = advanced_settings(&exe_dir);
        assert_eq!(advanced.stream_io, 1);
        assert_eq!(advanced.texture_quality, 2);
        assert!(advanced.texture_dedup);
        save_settings(&exe_dir, &settings(&exe_dir)).unwrap();
        assert_eq!(advanced_settings(&exe_dir), advanced);
        let ini = fs::read_to_string(exe_dir.join(INI)).unwrap();
        assert!(ini.contains("stream_root_up=2\n") && ini.contains("next=d3d11.next.dll\n"));
        assert_eq!(settings(&exe_dir).fps_limit, 60);
    }

    #[test]
    fn ue_cvar_lines_follow_the_setting() {
        let t = tempfile::tempdir().unwrap();
        let exe = t.path().join("game.exe");
        fs::write(&exe, b"not a PE").unwrap();
        // Not installed: nothing written.
        assert_eq!(apply_ue_cvars(t.path(), &exe, 8192).unwrap().set, 0);
        assert!(!t.path().join(INI).exists());
        let stale = "[tessera]\nnext=x.dll\nue_streaming=1\nue_image=1 2\ncvar.r.Old=1 i p 10\n";
        fs::write(t.path().join(INI), stale).unwrap();
        // On, but nothing placed (not a PE): old slots go, other lines stay.
        let r = apply_ue_cvars(t.path(), &exe, 8192).unwrap();
        assert_eq!(r.set, 0);
        assert_eq!(r.skipped.len(), 2);
        assert_eq!(fs::read_to_string(t.path().join(INI)).unwrap(), "[tessera]\nnext=x.dll\nue_streaming=1\n");
        // save_settings keeps cvar lines; turning the setting off and applying drops them.
        fs::write(t.path().join(INI), stale).unwrap();
        save_settings(t.path(), &Settings::default()).unwrap();
        assert!(fs::read_to_string(t.path().join(INI)).unwrap().contains("cvar.r.Old"));
        apply_ue_cvars(t.path(), &exe, 8192).unwrap();
        assert!(!fs::read_to_string(t.path().join(INI)).unwrap().contains("ue_image"));
        // A test variant asks for its variables without ue_streaming; "0" does not.
        fs::write(t.path().join(INI), "[tessera]\nue_drs_off=1\nue_rt_grow=0\n").unwrap();
        let r = apply_ue_cvars(t.path(), &exe, 8192).unwrap();
        assert_eq!(r.skipped.len(), 2);
        assert!(r.skipped[0].starts_with("r.DynamicRes.OperationMode"));
    }
}
