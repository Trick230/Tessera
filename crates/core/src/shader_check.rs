// SPDX-License-Identifier: GPL-3.0-or-later
//! The state of a game's shaders, from the files Tessera keeps: how many the
//! driver has already compiled (warm-up logs written with the installed
//! driver), how many are still to compile, which stored ones are damaged and
//! which records are out of date (game or driver updated since). Reads the
//! whole stores to verify checksums, so callers run it off the UI thread.

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use crate::shaders::{FLAG_DXBC, FLAG_DXIL};
use crate::warm::{settle, with_suffix, Outcome};

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct ShaderCheck {
    /// Distinct intact shaders in both stores (layer and install scan).
    pub stored: u64,
    /// Stored and compiled by a warm-up with the installed driver.
    pub compiled: u64,
    /// The driver created these shaders without a compatible draw or dispatch.
    pub created_only: u64,
    /// Stored, refused by the driver in a warm-up with the installed driver.
    pub failed: u64,
    /// Stored but not D3D11 bytecode (DXIL): the D3D11 warm-up cannot take them.
    pub skipped: u64,
    /// Stored, not compiled with the installed driver yet.
    pub pending: u64,
    /// Part of `pending`: compiled before, but with another driver, whose
    /// cache entries the installed one does not use.
    pub old_driver: u64,
    /// Store entries that are cut short or whose bytes fail their checksum.
    pub damaged: u64,
    /// Containers in the install files that failed their checksum (last scan).
    pub bad_in_install: Option<u64>,
    pub unsupported_in_install: Option<u64>,
    /// Whether the install changed since the last scan (None: never scanned
    /// or install folder unknown).
    pub scan_outdated: Option<bool>,
    /// Hashes in the warm-up logs that no store holds any more.
    pub dropped: u64,
}

impl ShaderCheck {
    /// Every stored shader the warm-up can compile is compiled with this driver.
    pub fn all_compiled(&self) -> bool {
        self.compiled > 0 && self.pending == 0 && self.failed == 0 && self.created_only == 0
    }
}

/// True when a DXBC container's bytes match its checksum. A zero checksum is
/// an unsigned container: nothing to verify.
fn intact(code: &[u8]) -> bool {
    if code.len() < 20 || &code[..4] != b"DXBC" {
        return false;
    }
    code[4..20].iter().all(|&b| b == 0) || crate::scan::checksum(code) == code[4..20]
}

/// The check for one game. `root` is its install folder (for the scan's
/// freshness), `drivers` the installed drivers' UMD versions (any GPU may
/// have run the warm-up; empty means no current driver could be verified).
pub fn check(exe: &Path, root: Option<&Path>, drivers: &[u64]) -> ShaderCheck {
    let layer = crate::warm::store_base(exe);
    let scan = crate::scan::scan_base(exe);
    let key = match (&scan, root) {
        (Some(b), Some(r)) if crate::scan::record_at(b).is_some() => Some(crate::scan::current_key(r)),
        _ => None,
    };
    check_at(layer.as_deref(), scan.as_deref(), key.as_deref(), drivers)
}

/// `check` on explicit store bases and the install key as it is now.
pub fn check_at(layer: Option<&Path>, scan: Option<&Path>, install_key: Option<&str>, drivers: &[u64]) -> ShaderCheck {
    let mut c = ShaderCheck::default();
    let bases: Vec<&Path> = [layer, scan].into_iter().flatten().collect();

    let mut stored: HashMap<u64, u16> = HashMap::new();
    for b in &bases {
        let (store, index) = (with_suffix(b, ".tshd"), with_suffix(b, ".tshi"));
        let shaders = crate::shaders::read_at(&store, &index);
        c.damaged += crate::shaders::listed_at(&index).saturating_sub(shaders.len() as u64);
        for s in shaders {
            if s.flags & FLAG_DXBC != 0 && !intact(&s.code) {
                c.damaged += 1;
                continue;
            }
            stored.entry(s.hash).or_insert(s.flags);
        }
    }

    // Logs written with the installed driver describe its cache; older ones
    // only say what it lost.
    let (mut now, mut before) = (HashMap::new(), HashMap::new());
    for b in &bases {
        let Ok(text) = fs::read_to_string(with_suffix(b, ".twarm")) else { continue };
        let mut seen = HashMap::new();
        let Some(driver) = settle(&text, &mut seen) else { continue };
        let into = if drivers.contains(&driver) { &mut now } else { &mut before };
        for (h, o) in seen {
            crate::warm::merge_outcome(into, h, o);
        }
    }

    c.stored = stored.len() as u64;
    for (h, &flags) in &stored {
        match now.get(h) {
            Some(Outcome::Ok) => c.compiled += 1,
            Some(Outcome::Made) => c.created_only += 1,
            Some(Outcome::Failed) => c.failed += 1,
            Some(Outcome::Skipped) => c.skipped += 1,
            None if flags & FLAG_DXIL != 0 => c.skipped += 1,
            None => {
                c.pending += 1;
                c.old_driver += (!drivers.is_empty() && before.get(h) == Some(&Outcome::Ok)) as u64;
            }
        }
    }
    let logged: std::collections::HashSet<u64> = now.keys().chain(before.keys()).copied().collect();
    c.dropped = logged.iter().filter(|h| !stored.contains_key(h)).count() as u64;

    if let Some(r) = scan.and_then(crate::scan::record_at) {
        c.bad_in_install = r.invalid;
        c.unsupported_in_install = r.unsupported;
        c.scan_outdated = install_key.map(|k| k != r.key);
    }
    c
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::shaders::{encode, Shader, STAGE_PS};

    /// A signed DXBC container whose checksum's first 8 bytes are the hash.
    fn dxbc(seed: u8) -> Shader {
        let mut code = b"DXBC".to_vec();
        code.extend_from_slice(&[0; 16]);
        code.extend_from_slice(&[seed; 40]);
        let sum = crate::scan::checksum(&code);
        code[4..20].copy_from_slice(&sum);
        Shader { hash: u64::from_le_bytes(sum[..8].try_into().unwrap()), stage: STAGE_PS, flags: FLAG_DXBC, code }
    }

    fn write(base: &Path, shaders: &[Shader]) {
        let (s, i) = encode(shaders);
        fs::write(with_suffix(base, ".tshd"), s).unwrap();
        fs::write(with_suffix(base, ".tshi"), i).unwrap();
    }

    #[test]
    fn sorts_shaders_by_what_the_driver_has() {
        let t = tempfile::tempdir().unwrap();
        let (layer, scan) = (t.path().join("g"), t.path().join("g.scan"));
        let [a, b, c, d] = [1, 2, 3, 4].map(dxbc);
        let mut bad = dxbc(5);
        bad.code[30] ^= 1; // flipped on disk
        write(&layer, &[a.clone(), b.clone(), bad]);
        write(&scan, &[b.clone(), c.clone(), d.clone()]);
        let (now, old) = (0x20_0000_0010_0842u64, 0x20_0000_0010_0800u64);
        fs::write(
            with_suffix(&layer, ".twarm"),
            format!("tessera-warm 1 driver {now:016x}\nok {:x}\nfail {:x}\nok 99\n", a.hash, b.hash),
        )
        .unwrap();
        fs::write(with_suffix(&scan, ".twarm"), format!("tessera-warm 1 driver {old:016x}\nok {:x}\n", c.hash))
            .unwrap();
        fs::write(with_suffix(&scan, ".key"), "tessera-scan 2 files 1\ninvalid 7\n").unwrap();

        let r = check_at(Some(&layer), Some(&scan), Some("tessera-scan 2 files 2"), &[now]);
        assert_eq!((r.stored, r.compiled, r.failed, r.skipped), (4, 1, 1, 0));
        assert_eq!((r.pending, r.old_driver, r.damaged, r.dropped), (2, 1, 1, 1));
        assert_eq!((r.bad_in_install, r.scan_outdated), (Some(7), Some(true)));
        assert!(!r.all_compiled());

        let same = check_at(Some(&layer), Some(&scan), Some("tessera-scan 2 files 1"), &[now]);
        assert_eq!(same.scan_outdated, Some(false));

        fs::write(with_suffix(&scan, ".twarm"),
            format!("tessera-warm 1 driver {now:016x}\nfail {:x}\nok {:x}\nmade {:x}\n", b.hash, b.hash, c.hash),
        ).unwrap();
        let updated = check_at(Some(&layer), Some(&scan), None, &[now]);
        assert_eq!((updated.compiled, updated.created_only, updated.pending, updated.failed), (2, 1, 1, 0));
        assert_eq!(check_at(Some(&layer), Some(&scan), None, &[]).compiled, 0);
    }

    #[test]
    fn counts_cut_short_entries_as_damaged() {
        let t = tempfile::tempdir().unwrap();
        let base = t.path().join("g");
        let (mut s, i) = encode(&[dxbc(1), dxbc(2)]);
        s.truncate(s.len() - 8);
        fs::write(with_suffix(&base, ".tshd"), s).unwrap();
        fs::write(with_suffix(&base, ".tshi"), i).unwrap();
        let r = check_at(Some(&base), None, None, &[]);
        assert_eq!((r.stored, r.damaged, r.pending, r.scan_outdated), (1, 1, 1, None));
    }
}
