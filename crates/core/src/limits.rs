//! P1: frame limits set in other tools. Tessera's limiter runs just under the
//! lowest one (src/dll/pacing.cpp), so two limiters never fight and a lower
//! limit the user chose elsewhere stands. Read-only.

use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExternalLimit {
    pub fps: u32,
    pub source: &'static str,
}

/// The lowest frame limit RTSS or the NVIDIA control panel applies to `exe_name`.
pub fn external_fps_limit(exe_name: &str) -> Option<ExternalLimit> {
    // RTSS limits only while it runs; its profiles outlive it.
    let rtss = rtss_running()
        .then(rtss_dir)
        .flatten().and_then(|d| rtss_limit(&d, exe_name)).map(|fps| ExternalLimit { fps, source: "RTSS" });
    let nv = crate::driver::nvidia_frame_limit(exe_name).map(|fps| ExternalLimit { fps, source: "NVIDIA control panel" });
    [rtss, nv].into_iter().flatten().min_by_key(|l| l.fps)
}

fn rtss_running() -> bool {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    std::process::Command::new("tasklist")
        .args(["/FI", "IMAGENAME eq RTSS.exe", "/NH", "/FO", "CSV"])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .is_ok_and(|o| String::from_utf8_lossy(&o.stdout).to_ascii_lowercase().contains("\"rtss.exe\""))
}

fn rtss_dir() -> Option<PathBuf> {
    let hklm = winreg::RegKey::predef(winreg::enums::HKEY_LOCAL_MACHINE);
    for key in [r"SOFTWARE\WOW6432Node\Unwinder\RTSS", r"SOFTWARE\Unwinder\RTSS"] {
        if let Ok(dir) = hklm.open_subkey(key).and_then(|k| k.get_value::<String, _>("InstallDir")) {
            let dir = PathBuf::from(dir);
            if dir.join("Profiles").is_dir() {
                return Some(dir);
            }
        }
    }
    None
}

/// RTSS keeps a full copy of the settings per application (`<exe>.cfg`),
/// and `Global` for the rest.
fn rtss_limit(dir: &Path, exe_name: &str) -> Option<u32> {
    let profiles = dir.join("Profiles");
    // Profiles hold ANSI text (OSD strings, paths): read lossy, and fall
    // back to Global only when the application has no profile.
    let bytes = match std::fs::read(profiles.join(format!("{exe_name}.cfg"))) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => std::fs::read(profiles.join("Global")).ok()?,
        r => r.ok()?,
    };
    rtss_profile_limit(&String::from_utf8_lossy(&bytes))
}

/// `[Framerate] Limit` (fps, 0 = off) over `LimitDenominator`, rounded down.
pub fn rtss_profile_limit(text: &str) -> Option<u32> {
    let (mut limit, mut den, mut in_section) = (0u32, 1u32, false);
    for line in text.lines().map(str::trim) {
        if line.starts_with('[') {
            in_section = line.eq_ignore_ascii_case("[Framerate]");
            continue;
        }
        if !in_section {
            continue;
        }
        let Some((k, v)) = line.split_once('=') else { continue };
        let v: u32 = v.trim().parse().unwrap_or(0);
        match k.trim() {
            "Limit" => limit = v,
            "LimitDenominator" => den = v.max(1),
            _ => {}
        }
    }
    let fps = limit / den;
    (fps > 0).then_some(fps)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_rtss_framerate_limit() {
        let p = "[OSD]\nLimit=99\n[Framerate]\nLimit=60\nLimitDenominator=1\nLimitTime=0\n[Hooking]\n";
        assert_eq!(rtss_profile_limit(p), Some(60));
        // 59.94 stored as 5994/100: rounded down, still at or below the real limit.
        assert_eq!(rtss_profile_limit("[Framerate]\nLimit=5994\nLimitDenominator=100\n"), Some(59));
        assert_eq!(rtss_profile_limit("[Framerate]\nLimit=0\nLimitDenominator=1\n"), None);
        assert_eq!(rtss_profile_limit("[OSD]\nLimit=60\n"), None);
    }

    #[test]
    fn prefers_the_application_profile() {
        let t = tempfile::tempdir().unwrap();
        let p = t.path().join("Profiles");
        std::fs::create_dir(&p).unwrap();
        std::fs::write(p.join("Global"), "[Framerate]\nLimit=0\n").unwrap();
        std::fs::write(p.join("game.exe.cfg"), "[Framerate]\nLimit=72\n").unwrap();
        assert_eq!(rtss_limit(t.path(), "game.exe"), Some(72));
        assert_eq!(rtss_limit(t.path(), "other.exe"), None);
    }
}
