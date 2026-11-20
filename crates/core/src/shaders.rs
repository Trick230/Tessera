// SPDX-License-Identifier: GPL-3.0-or-later
//! Reader for the per-game shader store the layer writes (S6). Mirrors
//! include/tessera/shader_store_format.h: `<exe>-<path key>.tshd` holds the
//! bytecode, `.tshi` one (hash, end offset) per entry. A cut-short tail is
//! ignored, like the layer does.

use std::fs;
use std::path::{Path, PathBuf};

pub const STORE_MAGIC: &[u8; 8] = b"TSRSHDR\0";
pub const INDEX_MAGIC: &[u8; 8] = b"TSRSIDX\0";
pub const VERSION: u32 = 1;
const HEADER: usize = 24;
const ENTRY: usize = 16;
const INDEX_ENTRY: usize = 16;

pub const STAGE_VS: u16 = 0;
pub const STAGE_HS: u16 = 1;
pub const STAGE_DS: u16 = 2;
pub const STAGE_GS: u16 = 3;
pub const STAGE_PS: u16 = 4;
pub const STAGE_CS: u16 = 5;
pub const STAGE_GS_SO: u16 = 6;

pub const FLAG_DXBC: u16 = 0x1;
pub const FLAG_DXIL: u16 = 0x2;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Shader {
    pub hash: u64,
    pub stage: u16,
    pub flags: u16,
    pub code: Vec<u8>,
}

/// Count and bytecode bytes of the stored shaders.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct StoreStats {
    pub shaders: u64,
    pub bytes: u64,
}

pub fn store_paths(exe: &Path) -> Option<(PathBuf, PathBuf)> {
    let stem = crate::session::exe_key_name(exe)?;
    let dir = crate::paths::data_dir().join("shaders");
    Some((dir.join(format!("{stem}.tshd")), dir.join(format!("{stem}.tshi"))))
}

fn header_ok(b: &[u8], magic: &[u8; 8], entry: usize) -> bool {
    let u32_at = |o: usize| u32::from_le_bytes(b[o..o + 4].try_into().unwrap());
    b.len() >= HEADER
        && &b[..8] == magic
        && u32_at(8) == VERSION
        && u32_at(12) as usize == HEADER
        && u32_at(16) as usize == entry
}

/// Valid (hash, start, end) index entries: increasing ends within the store.
fn index_entries(index: &[u8], store_len: u64) -> Vec<(u64, u64, u64)> {
    let mut out = Vec::new();
    if !header_ok(index, INDEX_MAGIC, INDEX_ENTRY) {
        return out;
    }
    let mut start = HEADER as u64;
    for e in index[HEADER..].chunks_exact(INDEX_ENTRY) {
        let hash = u64::from_le_bytes(e[..8].try_into().unwrap());
        let end = u64::from_le_bytes(e[8..].try_into().unwrap());
        if end <= start || end > store_len {
            break;
        }
        out.push((hash, start, end));
        start = end;
    }
    out
}

/// Hashes in the index (cheap: 16 bytes per shader).
pub fn hashes_at(store: &Path, index: &Path) -> Vec<u64> {
    let (Ok(meta), Ok(idx)) = (fs::metadata(store), fs::read(index)) else { return Vec::new() };
    index_entries(&idx, meta.len()).iter().map(|e| e.0).collect()
}

/// Reads the index only (cheap: 16 bytes per shader).
pub fn stats_at(store: &Path, index: &Path) -> StoreStats {
    let (Ok(meta), Ok(idx)) = (fs::metadata(store), fs::read(index)) else { return StoreStats::default() };
    let e = index_entries(&idx, meta.len());
    StoreStats { shaders: e.len() as u64, bytes: e.last().map_or(0, |l| l.2 - HEADER as u64) }
}

pub fn stats(exe: &Path) -> StoreStats {
    store_paths(exe).map(|(s, i)| stats_at(&s, &i)).unwrap_or_default()
}

/// Every stored shader whose entry is whole and matches its index hash.
pub fn read_at(store: &Path, index: &Path) -> Vec<Shader> {
    let (Ok(b), Ok(idx)) = (fs::read(store), fs::read(index)) else { return Vec::new() };
    if !header_ok(&b, STORE_MAGIC, ENTRY) {
        return Vec::new();
    }
    let mut out = Vec::new();
    for (hash, start, end) in index_entries(&idx, b.len() as u64) {
        let e = &b[start as usize..end as usize];
        if e.len() < ENTRY {
            break;
        }
        let h = u64::from_le_bytes(e[..8].try_into().unwrap());
        let size = u32::from_le_bytes(e[8..12].try_into().unwrap()) as usize;
        if h != hash || ENTRY + size > e.len() {
            break;
        }
        out.push(Shader {
            hash,
            stage: u16::from_le_bytes([e[12], e[13]]),
            flags: u16::from_le_bytes([e[14], e[15]]),
            code: e[ENTRY..ENTRY + size].to_vec(),
        });
    }
    out
}

pub fn read(exe: &Path) -> Vec<Shader> {
    store_paths(exe).map(|(s, i)| read_at(&s, &i)).unwrap_or_default()
}

fn header(magic: &[u8; 8], entry: usize) -> Vec<u8> {
    let mut b = magic.to_vec();
    for v in [VERSION, HEADER as u32, entry as u32, 0] {
        b.extend_from_slice(&v.to_le_bytes());
    }
    b
}

/// Store and index bytes in the layer's format, entries padded to 8.
pub fn encode(shaders: &[Shader]) -> (Vec<u8>, Vec<u8>) {
    let (mut s, mut i) = (header(STORE_MAGIC, ENTRY), header(INDEX_MAGIC, INDEX_ENTRY));
    for sh in shaders {
        s.extend_from_slice(&sh.hash.to_le_bytes());
        s.extend_from_slice(&(sh.code.len() as u32).to_le_bytes());
        s.extend_from_slice(&sh.stage.to_le_bytes());
        s.extend_from_slice(&sh.flags.to_le_bytes());
        s.extend_from_slice(&sh.code);
        s.resize((s.len() + 7) & !7, 0);
        i.extend_from_slice(&sh.hash.to_le_bytes());
        i.extend_from_slice(&(s.len() as u64).to_le_bytes());
    }
    (s, i)
}

/// Writes `<base>.tshd` and `<base>.tshi` whole (temp file, then rename):
/// for stores the app owns, never the layer's live one.
pub fn write_base(base: &Path, shaders: &[Shader]) -> std::io::Result<()> {
    let (s, i) = encode(shaders);
    if let Some(dir) = base.parent() {
        fs::create_dir_all(dir)?;
    }
    for (ext, bytes) in [("tshd", &s), ("tshi", &i)] {
        let mut p = base.as_os_str().to_owned();
        p.push(".");
        p.push(ext);
        let path = PathBuf::from(p);
        let tmp = path.with_extension(format!("{ext}.tmp"));
        fs::write(&tmp, bytes)?;
        fs::rename(&tmp, &path)?;
    }
    Ok(())
}

pub fn read_base(base: &Path) -> Vec<Shader> {
    let with = |ext: &str| {
        let mut p = base.as_os_str().to_owned();
        p.push(ext);
        PathBuf::from(p)
    };
    read_at(&with(".tshd"), &with(".tshi"))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn header(magic: &[u8; 8], entry: u32) -> Vec<u8> {
        let mut b = magic.to_vec();
        for v in [VERSION, HEADER as u32, entry, 0] {
            b.extend_from_slice(&v.to_le_bytes());
        }
        b
    }

    /// Store and index bytes for the given (hash, stage, code).
    pub(crate) fn build(shaders: &[(u64, u16, &[u8])]) -> (Vec<u8>, Vec<u8>) {
        let (mut s, mut i) = (header(STORE_MAGIC, ENTRY as u32), header(INDEX_MAGIC, INDEX_ENTRY as u32));
        for &(hash, stage, code) in shaders {
            s.extend_from_slice(&hash.to_le_bytes());
            s.extend_from_slice(&(code.len() as u32).to_le_bytes());
            s.extend_from_slice(&stage.to_le_bytes());
            s.extend_from_slice(&FLAG_DXBC.to_le_bytes());
            s.extend_from_slice(code);
            s.resize((s.len() + 7) & !7, 0);
            i.extend_from_slice(&hash.to_le_bytes());
            i.extend_from_slice(&(s.len() as u64).to_le_bytes());
        }
        (s, i)
    }

    #[test]
    fn reads_whole_entries_and_ignores_a_cut_tail() {
        let t = tempfile::tempdir().unwrap();
        let (mut s, i) = build(&[(1, STAGE_VS, b"abc"), (2, STAGE_PS, b"0123456789")]);
        s.truncate(s.len() - 4); // the second entry was cut short by a crash
        let (sp, ip) = (t.path().join("g.tshd"), t.path().join("g.tshi"));
        fs::write(&sp, &s).unwrap();
        fs::write(&ip, &i).unwrap();
        let all = read_at(&sp, &ip);
        assert_eq!(all.len(), 1);
        assert_eq!((all[0].hash, all[0].stage, all[0].code.as_slice()), (1, STAGE_VS, &b"abc"[..]));
        assert_eq!(stats_at(&sp, &ip), StoreStats { shaders: 1, bytes: 24 });
    }

    #[test]
    fn rejects_foreign_files() {
        let t = tempfile::tempdir().unwrap();
        let (sp, ip) = (t.path().join("g.tshd"), t.path().join("g.tshi"));
        fs::write(&sp, b"nope").unwrap();
        fs::write(&ip, b"nope").unwrap();
        assert!(read_at(&sp, &ip).is_empty());
        assert_eq!(stats_at(&sp, &ip), StoreStats::default());
    }
}
