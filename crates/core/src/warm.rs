// SPDX-License-Identifier: GPL-3.0-or-later
//! Shader warm-up (S5): runs `tessera_warm.exe` on a game's shaders before
//! the game starts, so the driver's disk cache already holds them. Two
//! stores feed it: the layer's (shaders the game created, S6) and the scan
//! store (bytecode found in the install files, S1). The exe is copied under
//! the game's exe name (drivers key their cache by it). Never runs while the
//! game is running.

use std::fs;
use std::io;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const WARM_EXE: &str = "tessera_warm.exe";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
/// Crashes or device removals tolerated in one warm-up; each relaunch skips
/// the shader that was being compiled.
const MAX_RELAUNCHES: u32 = 8;

/// Settled shaders in the warm-up logs. The logs restart on a new driver;
/// until the next warm-up they describe the driver that wrote them.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct WarmStats {
    /// Compiled (used in a draw, or created when nothing can draw it).
    pub ok: u64,
    pub failed: u64,
    /// Not D3D11 shaders (DXIL).
    pub skipped: u64,
}

fn with_suffix(base: &Path, suffix: &str) -> PathBuf {
    let mut s = base.as_os_str().to_owned();
    s.push(suffix);
    PathBuf::from(s)
}

/// `<data>\shaders\<exe>-<key>`: the layer store's base path (.tshd/.tshi).
pub fn store_base(exe: &Path) -> Option<PathBuf> {
    let stem = crate::session::exe_key_name(exe)?;
    Some(crate::paths::data_dir().join("shaders").join(stem))
}

/// Both stores' base paths: the layer's, then the scan's.
pub fn bases(exe: &Path) -> Vec<PathBuf> {
    [store_base(exe), crate::scan::scan_base(exe)].into_iter().flatten().collect()
}

/// Shaders in both stores, each hash once (a later session may store a
/// shader the scan found first).
pub fn stored(exe: &Path) -> crate::shaders::StoreStats {
    let mut t = crate::shaders::StoreStats::default();
    let mut seen = std::collections::HashSet::new();
    for b in bases(exe) {
        let (store, index) = (with_suffix(&b, ".tshd"), with_suffix(&b, ".tshi"));
        let s = crate::shaders::stats_at(&store, &index);
        let hashes = crate::shaders::hashes_at(&store, &index);
        let fresh = hashes.iter().filter(|&&h| seen.insert(h)).count() as u64;
        t.shaders += fresh;
        // Bytes are not per hash in the index: scale the store's by the share kept.
        t.bytes += if hashes.is_empty() { 0 } else { s.bytes * fresh / hashes.len() as u64 };
    }
    t
}

#[derive(Clone, Copy)]
enum Outcome {
    Ok,
    Failed,
    Skipped,
}

/// Adds a log's settled hashes to `seen` (first outcome wins). A pending
/// "try" (the run died on it) counts as failed, as the warm exe does on its
/// next launch.
fn settle(text: &str, seen: &mut std::collections::HashMap<String, Outcome>) {
    let mut lines = text.lines();
    if !lines.next().is_some_and(|h| h.starts_with("tessera-warm 1 ")) {
        return;
    }
    let mut pending = None;
    for l in lines {
        let Some((word, h)) = l.split_once(' ') else { continue };
        let o = match word {
            "try" => {
                pending = Some(h);
                continue;
            }
            "ok" | "made" => Outcome::Ok,
            "fail" => Outcome::Failed,
            "skip" => Outcome::Skipped,
            _ => continue,
        };
        pending = None;
        seen.entry(h.to_string()).or_insert(o);
    }
    if let Some(h) = pending {
        seen.entry(h.to_string()).or_insert(Outcome::Failed);
    }
}

fn count(seen: &std::collections::HashMap<String, Outcome>) -> WarmStats {
    let mut st = WarmStats::default();
    for o in seen.values() {
        match o {
            Outcome::Ok => st.ok += 1,
            Outcome::Failed => st.failed += 1,
            Outcome::Skipped => st.skipped += 1,
        }
    }
    st
}

/// Parses one warm-up log; each hash counts once.
pub fn parse_log(text: &str) -> WarmStats {
    let mut seen = std::collections::HashMap::new();
    settle(text, &mut seen);
    count(&seen)
}

/// Both stores' logs, each hash once.
pub fn stats(exe: &Path) -> WarmStats {
    let mut seen = std::collections::HashMap::new();
    for b in bases(exe) {
        if let Ok(text) = fs::read_to_string(with_suffix(&b, ".twarm")) {
            settle(&text, &mut seen);
        }
    }
    count(&seen)
}

/// True when a process with this exe file name is running.
pub fn is_running(exe: &Path) -> bool {
    let Some(name) = exe.file_name().and_then(|n| n.to_str()) else { return false };
    let out = Command::new("tasklist")
        .args(["/FI", &format!("IMAGENAME eq {name}"), "/NH", "/FO", "CSV"])
        .creation_flags(CREATE_NO_WINDOW)
        .output();
    let needle = format!("\"{}\"", name.to_ascii_lowercase());
    out.is_ok_and(|o| String::from_utf8_lossy(&o.stdout).to_ascii_lowercase().contains(&needle))
}

/// Warms every store of `exe` with `warm_exe`; returns the warm exe's summary
/// lines. Blocks until done: call it off the UI thread.
pub fn run(exe: &Path, warm_exe: &Path) -> io::Result<String> {
    let other = |m: &str| io::Error::other(m.to_string());
    let base = store_base(exe).ok_or_else(|| other("caminho do jogo inválido"))?;
    let todo: Vec<PathBuf> = bases(exe).into_iter().filter(|b| with_suffix(b, ".tshi").is_file()).collect();
    if todo.is_empty() || stored(exe).shaders == 0 {
        return Err(other("ainda não há shaders: procura nos ficheiros ou joga uma vez com a camada"));
    }
    if is_running(exe) {
        return Err(other("fecha o jogo antes de aquecer os shaders"));
    }
    let name = exe.file_name().ok_or_else(|| other("caminho do jogo inválido"))?;
    let dir = crate::paths::data_dir().join("warm").join(base.file_name().unwrap_or_default());
    fs::create_dir_all(&dir)?;
    let copy = dir.join(name);
    fs::copy(warm_exe, &copy)?;
    let mut lines = Vec::new();
    for b in todo {
        lines.push(run_one(&copy, &b)?);
    }
    Ok(lines.join("\n"))
}

fn run_one(copy: &Path, base: &Path) -> io::Result<String> {
    for _ in 0..=MAX_RELAUNCHES {
        let out = Command::new(copy).arg(base).creation_flags(CREATE_NO_WINDOW).output()?;
        match out.status.code() {
            Some(0) => return Ok(String::from_utf8_lossy(&out.stdout).trim().to_string()),
            Some(1) => {
                let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
                return Err(io::Error::other(if err.is_empty() { "o aquecimento falhou".to_string() } else { err }));
            }
            // Device removed (2) or a crash in the driver: the log marks the
            // pending shader failed and the relaunch goes on past it.
            _ => continue,
        }
    }
    Err(io::Error::other("o driver falhou demasiadas vezes; tenta outra vez"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_each_hash_once_and_a_pending_try_as_failed() {
        let log = "tessera-warm 1 driver 0000000000000001\ntry a\nok a\ntry b\nfail b\ntry d\nmade d\nskip e\ntry c\n";
        assert_eq!(parse_log(log), WarmStats { ok: 2, failed: 2, skipped: 1 });
        assert_eq!(parse_log("something else\nok a\n"), WarmStats::default());
    }
}
