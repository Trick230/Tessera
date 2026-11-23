// SPDX-License-Identifier: GPL-3.0-or-later
//! Shader warm-up (S5): runs `tessera_warm.exe` on a game's shaders before
//! the game starts, so the driver's disk cache already holds them. Two
//! stores feed it: the layer's (shaders the game created, S6) and the scan
//! store (bytecode found in the install files, S1). The exe is copied under
//! the game's exe name (drivers key their cache by it). Never runs while the
//! game is running.

use std::collections::HashMap;
use std::fs;
use std::io;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const WARM_EXE: &str = "tessera_warm.exe";
pub const WARM_EXE_X86: &str = "tessera_warm_x86.exe";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
/// Crashes or device removals tolerated in one warm-up; each relaunch skips
/// the shader that was being compiled.
const MAX_RELAUNCHES: u32 = 8;
/// Seconds without progress in the warm-up log before a stall is reported.
/// Only reported: S5 never kills a warm-up mid-way (the driver may be
/// writing its cache), so the user decides.
const STALL_S: u64 = 60;

/// Settled shaders in the warm-up logs. The logs restart on a new driver;
/// until the next warm-up they describe the driver that wrote them.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct WarmStats {
    /// Created and exercised in a draw or dispatch.
    pub ok: u64,
    pub created_only: u64,
    pub failed: u64,
    /// Not D3D11 shaders (DXIL).
    pub skipped: u64,
}

pub(crate) fn with_suffix(base: &Path, suffix: &str) -> PathBuf {
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

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Outcome {
    Skipped,
    Failed,
    Made,
    Ok,
}

pub(crate) fn merge_outcome(seen: &mut HashMap<u64, Outcome>, hash: u64, outcome: Outcome) {
    seen.entry(hash).and_modify(|previous| *previous = (*previous).max(outcome)).or_insert(outcome);
}

/// Adds a log's latest outcomes to `seen` and returns
/// the driver version in its header (None for a foreign file). A pending
/// "try" (the run died on it) counts as failed, as the warm exe does on its
/// next launch.
pub(crate) fn settle(text: &str, seen: &mut HashMap<u64, Outcome>) -> Option<u64> {
    let mut lines = text.lines();
    let driver = lines.next()?.strip_prefix("tessera-warm 1 driver ")?;
    let driver = u64::from_str_radix(driver.trim(), 16).ok()?;
    let mut pending = None;
    let mut latest = HashMap::new();
    for l in lines {
        let Some((word, h)) = l.split_once(' ') else { continue };
        let Ok(h) = u64::from_str_radix(h.trim(), 16) else { continue };
        let o = match word {
            "try" => {
                pending = Some(h);
                continue;
            }
            "ok" => Outcome::Ok,
            "made" => Outcome::Made,
            "fail" => Outcome::Failed,
            "skip" => Outcome::Skipped,
            _ => continue,
        };
        if pending == Some(h) {
            pending = None;
        }
        latest.insert(h, o);
    }
    if let Some(h) = pending {
        latest.insert(h, Outcome::Failed);
    }
    for (hash, outcome) in latest {
        merge_outcome(seen, hash, outcome);
    }
    Some(driver)
}

fn count(seen: &HashMap<u64, Outcome>) -> WarmStats {
    let mut st = WarmStats::default();
    for o in seen.values() {
        match o {
            Outcome::Ok => st.ok += 1,
            Outcome::Made => st.created_only += 1,
            Outcome::Failed => st.failed += 1,
            Outcome::Skipped => st.skipped += 1,
        }
    }
    st
}

/// Parses one warm-up log; each hash counts once.
pub fn parse_log(text: &str) -> WarmStats {
    let mut seen = HashMap::new();
    settle(text, &mut seen);
    count(&seen)
}

/// Both stores' logs, each hash once.
pub fn stats(exe: &Path) -> WarmStats {
    let mut seen = HashMap::new();
    let drivers: Vec<_> = crate::driver::gpus().iter()
        .filter_map(|g| crate::driver::umd_version(&g.driver_version)).collect();
    for b in bases(exe) {
        if let Ok(text) = fs::read_to_string(with_suffix(&b, ".twarm")) {
            let mut log = HashMap::new();
            if settle(&text, &mut log).is_some_and(|driver| drivers.contains(&driver)) {
                for (hash, outcome) in log {
                    merge_outcome(&mut seen, hash, outcome);
                }
            }
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
/// lines. Blocks until done: call it off the UI thread. `on_stall` gets the
/// seconds the log has not moved, from STALL_S on, every 10 s.
pub fn run(exe: &Path, warm_exe: &Path, on_stall: &dyn Fn(u64)) -> io::Result<String> {
    let other = |m: &str| io::Error::other(m.to_string());
    let base = store_base(exe).ok_or_else(|| other("invalid game path"))?;
    let todo: Vec<PathBuf> = bases(exe).into_iter().filter(|b| with_suffix(b, ".tshi").is_file()).collect();
    if todo.is_empty() || stored(exe).shaders == 0 {
        return Err(other("no shaders yet: search the files or play once with the layer"));
    }
    if is_running(exe) {
        return Err(other("close the game before warming shaders"));
    }
    let name = exe.file_name().ok_or_else(|| other("invalid game path"))?;
    let dir = crate::paths::data_dir().join("warm").join(base.file_name().unwrap_or_default());
    fs::create_dir_all(&dir)?;
    let copy = dir.join(name);
    fs::copy(warm_exe, &copy)?;
    let mut lines = Vec::new();
    for b in &todo {
        let peer = bases(exe).into_iter().find(|other| other != b);
        lines.push(run_one(&copy, b, peer.as_deref(), on_stall)?);
    }
    Ok(lines.join("\n"))
}

/// Runs one warm-up to its end, reporting (never acting on) a log that stops
/// growing: the warm exe flushes it before each risky driver call.
fn wait_reporting(mut cmd: Command, log: &Path, stall_s: u64, on_stall: &dyn Fn(u64)) -> io::Result<std::process::Output> {
    use std::io::Read;
    use std::process::Stdio;
    use std::time::{Duration, Instant};
    let mut child = cmd
        .creation_flags(CREATE_NO_WINDOW)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    // Drained on their own threads so a full pipe never blocks the child.
    fn drain<R: Read + Send + 'static>(p: Option<R>) -> std::thread::JoinHandle<Vec<u8>> {
        std::thread::spawn(move || {
            let mut v = Vec::new();
            if let Some(mut p) = p {
                let _ = p.read_to_end(&mut v);
            }
            v
        })
    }
    let out = drain(child.stdout.take());
    let err = drain(child.stderr.take());
    let size = || fs::metadata(log).map(|m| m.len()).unwrap_or(0);
    let (mut last, mut moved, mut next) = (size(), Instant::now(), stall_s);
    let status = loop {
        if let Some(st) = child.try_wait()? {
            break st;
        }
        std::thread::sleep(Duration::from_millis(500));
        let now = size();
        if now != last {
            (last, moved, next) = (now, Instant::now(), stall_s);
        }
        let idle = moved.elapsed().as_secs();
        if idle >= next {
            next = idle + 10;
            on_stall(idle);
        }
    };
    let (stdout, stderr) = (out.join().unwrap_or_default(), err.join().unwrap_or_default());
    Ok(std::process::Output { status, stdout, stderr })
}

fn run_one(copy: &Path, base: &Path, peer: Option<&Path>, on_stall: &dyn Fn(u64)) -> io::Result<String> {
    for attempt in 0..=MAX_RELAUNCHES {
        let mut cmd = Command::new(copy);
        cmd.arg(base).arg(peer.unwrap_or_else(|| Path::new("")));
        if attempt == 0 {
            cmd.arg("--retry-failed");
        }
        let out = wait_reporting(cmd, &with_suffix(base, ".twarm"), STALL_S, on_stall)?;
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
    fn reports_a_stalled_warm_up_and_lets_it_finish() {
        let mut cmd = Command::new("ping");
        cmd.args(["-n", "4", "127.0.0.1"]); // about 3 s, no log written
        let told = std::cell::Cell::new(0u32);
        let out = wait_reporting(cmd, Path::new("no such log"), 1, &|_| told.set(told.get() + 1)).unwrap();
        assert!(out.status.success(), "never killed");
        assert!(!out.stdout.is_empty(), "output still collected");
        assert_eq!(told.get(), 1, "told once at the limit, then every 10 s");
    }

    #[test]
    fn counts_each_hash_once_and_a_pending_try_as_failed() {
        let log = "tessera-warm 1 driver 0000000000000001\ntry a\nok a\ntry b\nfail b\ntry d\nmade d\nskip e\ntry c\n";
        assert_eq!(parse_log(log), WarmStats { ok: 1, created_only: 1, failed: 2, skipped: 1 });
        let retried = "tessera-warm 1 driver 0000000000000001\nfail a\ntry a\nok a\n";
        assert_eq!(parse_log(retried), WarmStats { ok: 1, ..WarmStats::default() });
        assert_eq!(parse_log("something else\nok a\n"), WarmStats::default());
    }
}
