// SPDX-License-Identifier: GPL-3.0-or-later
//! Finds standard Unreal AES keys in installed PE executables. The
//! instruction patterns follow SCSKiller's UnrealKeys (GPL-3.0-or-later).

use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use object::{Object, ObjectSection, SectionFlags};
use sha1::{Digest, Sha1};

use crate::unreal_pak::{self, PakKey};

type Keys = HashMap<PathBuf, usize>;
const INDEX_LIMIT: u64 = 64 << 20;
const CANDIDATE_LIMIT: usize = 1 << 16;

pub(crate) struct DetectedKeys {
    keys: Vec<PakKey>,
    archives: Keys,
}

impl DetectedKeys {
    pub(crate) fn count(&self) -> usize {
        self.archives.len()
    }

    pub(crate) fn for_archive(&self, path: &Path) -> Option<&PakKey> {
        self.keys.get(*self.archives.get(path)?)
    }
}

fn read_at(file: &mut File, offset: u64, bytes: &mut [u8]) -> Option<()> {
    file.seek(SeekFrom::Start(offset)).ok()?;
    file.read_exact(bytes).ok()
}

fn u32_at(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at.checked_add(4)?)?.try_into().ok()?))
}

fn u64_at(bytes: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(bytes.get(at..at.checked_add(8)?)?.try_into().ok()?))
}

struct Probe {
    path: PathBuf,
    offset: u64,
    size: u64,
    head: [u8; 16],
    pak_hash: Option<[u8; 20]>,
}

impl Probe {
    fn read(path: &Path) -> Option<Self> {
        let extension = path.extension()?.to_str()?;
        if !extension.eq_ignore_ascii_case("pak") && !extension.eq_ignore_ascii_case("utoc") {
            return None;
        }
        let mut file = File::open(path).ok()?;
        let len = file.metadata().ok()?.len();
        let (offset, size, pak_hash) = match extension.to_ascii_lowercase().as_str() {
            "pak" => {
                let mut tail = vec![0; len.min(512) as usize];
                let base = len - tail.len() as u64;
                read_at(&mut file, base, &mut tail)?;
                let at = tail.windows(5).rposition(|w| w == b"\x01\xe1\x12\x6f\x5a")?;
                let version = u32_at(&tail, at + 5)?;
                if !(4..=14).contains(&version) { return None; }
                let start = u64_at(&tail, at + 9)?;
                let size = u64_at(&tail, at + 17)?;
                if start.checked_add(size)? > base + at as u64 { return None; }
                (start, size, Some(tail.get(at + 25..at + 45)?.try_into().ok()?))
            }
            "utoc" => {
                let (offset, size) = io_directory(&mut file)?;
                (offset, size, None)
            }
            _ => return None,
        };
        if !(16..=INDEX_LIMIT).contains(&size) || size % 16 != 0 || offset.checked_add(size)? > len {
            return None;
        }
        let mut head = [0; 16];
        read_at(&mut file, offset, &mut head)?;
        Some(Self { path: path.to_owned(), offset, size, head, pak_hash })
    }

    fn opens(&self, key: &PakKey) -> Option<()> {
        let mut head = self.head;
        key.decode(&mut head)?;
        let length = (u32_at(&head, 0)? as i32).unsigned_abs();
        let plausible_mount = (4..=4096).contains(&length)
            && (head[4..].starts_with(b"../") || head[4..].starts_with(b".\0.\0/\0"));
        if !plausible_mount { return None; }
        let mut bytes = vec![0; self.size as usize];
        read_at(&mut File::open(&self.path).ok()?, self.offset, &mut bytes)?;
        let stored_hash = self.pak_hash.as_ref().map(|_| Sha1::digest(&bytes));
        key.decode(&mut bytes)?;
        let mut data = bytes.as_slice();
        let mount = unreal_pak::path(&mut data)?;
        if !mount.starts_with("../") || !mount.ends_with('/') { return None; }
        if let Some(hash) = self.pak_hash {
            let count = u32_at(data, 0)? as usize;
            return (count <= data.len() && (Sha1::digest(&bytes)[..] == hash ||
                stored_hash.is_some_and(|stored| stored[..] == hash))).then_some(());
        }
        validate_io_directory(data)
    }
}

fn io_directory(file: &mut File) -> Option<(u64, u64)> {
    let mut header = [0; 144];
    read_at(file, 0, &mut header)?;
    let (version, flags) = (header[16], header[80]);
    if &header[..16] != b"-==--==--==--==-" || !(2..=8).contains(&version) || flags & 0x0a != 0x0a {
        return None;
    }
    let field = |at| u32_at(&header, at).map(u64::from);
    let blocks = field(28)?;
    if field(20)? != 144 || field(32)? != 12 { return None; }
    let mut offset = field(20)? + field(24)? * 22;
    if version >= 4 { offset += field(84)? * 4; }
    if version >= 5 { offset += field(96)? * 4; }
    offset = offset.checked_add(blocks.checked_mul(field(32)?)?)?
        .checked_add(field(36)?.checked_mul(field(40)?)?)?;
    if flags & 4 != 0 {
        let mut signature = [0; 4];
        read_at(file, offset, &mut signature)?;
        offset = offset.checked_add(4 + u64::from(u32_at(&signature, 0)?) * 2 + blocks * 20)?;
    }
    Some((offset, field(48)?))
}

fn validate_io_directory(mut data: &[u8]) -> Option<()> {
    // Serialized directory entries, file entries and FString table.
    for entry_size in [16, 12] {
        let count = u32_at(data, 0)? as usize;
        data = data.get(4usize.checked_add(count.checked_mul(entry_size)?)?..)?;
    }
    let count = u32_at(data, 0)? as usize;
    data = data.get(4..)?;
    if count > data.len() / 4 { return None; }
    for _ in 0..count {
        if u32_at(data, 0)? == 0 {
            data = data.get(4..)?;
        } else {
            unreal_pak::path(&mut data)?;
        }
    }
    (data.len() < 16).then_some(())
}

#[derive(Clone, Copy)]
struct Store {
    at: usize,
    base: u8,
    offset: i32,
    width: usize,
    value: [u8; 32],
}

/// ModRM for a simple base register and optional displacement.
fn memory(code: &[u8], extra_base: u8) -> Option<(u8, i32, usize)> {
    let mode = code[0] >> 6;
    let base = code[0] & 7;
    if mode == 3 || base == 5 && mode == 0 { return None; }
    let mut used = 1;
    if base == 4 {
        if code[1] & 0x3f != 0x24 { return None; }
        used += 1;
    }
    let (displacement, size) = match mode {
        0 => (0, 0),
        1 => (i32::from(code[used] as i8), 1),
        2 => (u32_at(code, used)? as i32, 4),
        _ => return None,
    };
    Some((base | extra_base, displacement, used + size))
}

fn make_store(at: usize, operand: &[u8], base_bit: u8, value: impl FnOnce(usize) -> Option<[u8; 32]>, width: usize) -> Option<Store> {
    let (base, offset, length) = memory(operand, base_bit)?;
    Some(Store { at, base, offset, width, value: value(length)? })
}

fn expanded(bytes: &[u8]) -> Option<[u8; 32]> {
    let mut value = [0; 32];
    value.get_mut(..bytes.len())?.copy_from_slice(bytes);
    Some(value)
}

fn store_candidates<'a>(recent: &'a VecDeque<Store>, store: Store) -> impl Iterator<Item = [u8; 32]> + 'a {
    (0..32 / store.width).filter_map(move |i| {
        let start = store.offset.checked_sub((i * store.width) as i32)?;
        let mut key = [0; 32];
        for (part, chunk) in key.chunks_exact_mut(store.width).enumerate() {
            let offset = start.checked_add((part * store.width) as i32)?;
            let item = recent.iter().rev().find(|s| s.base == store.base && s.offset == offset && s.width == store.width)?;
            chunk.copy_from_slice(&item.value[..store.width]);
        }
        Some(key)
    })
}

fn scan_pe(exe: &Path, mut candidate: impl FnMut([u8; 32]) -> bool) -> Option<()> {
    let file = File::open(exe).ok()?;
    if file.metadata().ok()?.len() > i32::MAX as u64 { return None; }
    // SAFETY: read-only view of the on-disk game executable.
    let map = unsafe { memmap2::Mmap::map(&file) }.ok()?;
    let pe = object::File::parse(&*map).ok()?;
    let is_64 = pe.architecture() == object::Architecture::X86_64;
    if !matches!(pe.architecture(), object::Architecture::X86_64 | object::Architecture::I386) { return None; }
    let sections: Vec<_> = pe.sections().filter_map(|s| Some((s.address(), s.data().ok()?, s.flags()))).collect();
    let load = |address: u64, width| sections.iter().find_map(|&(base, bytes, _)| {
        let at = usize::try_from(address.checked_sub(base)?).ok()?;
        expanded(bytes.get(at..at.checked_add(width)?)?)
    });
    for &(address, code, flags) in &sections {
        if !matches!(flags, SectionFlags::Coff { characteristics } if characteristics.0 & object::pe::IMAGE_SCN_MEM_EXECUTE.0 != 0) {
            continue;
        }
        let mut recent = VecDeque::<Store>::new();
        let mut gprs = [None; 16];
        let mut vectors = [None; 16];
        for (at, b) in code.windows(16).enumerate() {
            let mut stores = [None; 2];
            match b[0] {
                0xc7 if (b[1] >> 3) & 7 == 0 => {
                    let store = |extra| make_store(at, &b[1..], extra, |n| expanded(&b[1+n..5+n]), 4);
                    stores[0] = store(0);
                    if is_64 && at > 0 && code[at - 1] == 0x41 { stores[1] = store(8); }
                }
                rex @ (0x48 | 0x49) if is_64 && (0xb8..=0xbf).contains(&b[1]) => {
                    gprs[usize::from((b[1] - 0xb8) | (rex & 1) * 8)] = Some((at, expanded(&b[2..10])?));
                }
                rex @ (0x48 | 0x49 | 0x4c | 0x4d) if is_64 && b[1] == 0x89 => {
                    let register = usize::from(((b[2] >> 3) & 7) | ((rex & 4) << 1));
                    if let Some((_, value)) = gprs[register].filter(|(p, _)| at - p < 48) {
                        stores[0] = make_store(at, &b[2..], (rex & 1) * 8, |_| Some(value), 8);
                    }
                }
                0x0f | 0x40..=0x4f | 0xc4 | 0xc5 if is_64 => {
                    if let Some((op, modrm, offset, r, base, width)) = vector_instruction(b) {
                        let register = usize::from(((modrm >> 3) & 7) | r);
                        if matches!(op, 0x10 | 0x28 | 0x6f) && modrm & 0xc7 == 5 {
                            let next = address + (at + offset + 5) as u64;
                            let displacement = i64::from(u32_at(b, offset + 1)? as i32);
                            vectors[register] = next.checked_add_signed(displacement)
                                .and_then(|src| load(src, width)).map(|value| (at, width, value));
                        } else if matches!(op, 0x11 | 0x29 | 0x7f) {
                            if let Some((_, _, value)) = vectors[register].filter(|(p, w, _)| at - p < 64 && *w == width) {
                                stores[0] = make_store(at, &b[offset..], base, |_| Some(value), width);
                            }
                        }
                    }
                }
                _ => {}
            }
            for store in stores.into_iter().flatten() {
                while recent.front().is_some_and(|s| at - s.at > 160) { recent.pop_front(); }
                recent.push_back(store);
                if store_candidates(&recent, store).any(&mut candidate) { return Some(()); }
            }
        }
    }
    Some(())
}

/// Opcode, ModRM, its offset, register/base extensions and vector width.
fn vector_instruction(b: &[u8]) -> Option<(u8, u8, usize, u8, u8, usize)> {
    let (at, r, base, width) = match b[0] {
        0x0f => (2, 0, 0, 16),
        rex @ 0x40..=0x4f if b[1] == 0x0f => (3, (rex & 4) << 1, (rex & 1) * 8, 16),
        0xc4 | 0xc5 => {
            let three = b[0] == 0xc4;
            if three && b[1] & 0x1f != 1 { return None; }
            let prefix = b[if three { 2 } else { 1 }];
            if prefix & 0x78 != 0x78 { return None; }
            (if three { 4 } else { 3 }, (!b[1] >> 4) & 8,
                if three { (!b[1] >> 2) & 8 } else { 0 }, if prefix & 4 == 0 { 16 } else { 32 })
        }
        _ => return None,
    };
    Some((b[at - 1], b[at], at, r, base, width))
}

pub(crate) fn detect(exe: &Path, paths: &[PathBuf]) -> DetectedKeys {
    let mut detected = DetectedKeys { keys: Vec::new(), archives: HashMap::new() };
    let mut probes: Vec<_> = paths.iter().filter_map(|path| Probe::read(path)).collect();
    if probes.is_empty() { return detected; }
    let mut tried = HashSet::new();
    scan_pe(exe, |candidate| {
        let mut unique = [false; 256];
        for byte in candidate { unique[usize::from(byte)] = true; }
        if unique.iter().filter(|&&seen| seen).count() < 16 || !tried.insert(candidate) { return false; }
        let key = PakKey::from_bytes(&candidate);
        let index = detected.keys.len();
        let before = probes.len();
        probes.retain(|probe| {
            if probe.opens(&key).is_none() { return true; }
            detected.archives.insert(probe.path.clone(), index);
            false
        });
        if probes.len() < before { detected.keys.push(key); }
        probes.is_empty() || tried.len() >= CANDIDATE_LIMIT
    });
    detected
}

#[cfg(test)]
mod tests {
    use super::*;
    use aes::cipher::{BlockEncrypt, KeyInit};

    #[test]
    fn validates_legacy_and_guid_paks_and_iostore_with_their_own_keys() {
        let dir = tempfile::tempdir().unwrap();
        let mount = b"../../../Content/\0";
        let mut index = (mount.len() as u32).to_le_bytes().to_vec();
        index.extend_from_slice(mount);
        index.extend_from_slice(&[0; 12]); // empty arrays in the IoStore directory
        index.resize(index.len().next_multiple_of(16), 0);
        let encrypt = |key: &[u8; 32]| {
            let aes = aes::Aes256::new(key.into());
            let mut bytes = index.clone();
            for block in bytes.chunks_exact_mut(16) {
                aes.encrypt_block(aes::cipher::Block::<aes::Aes256>::from_mut_slice(block));
            }
            bytes
        };
        let pak_bytes = std::array::from_fn(|i| i as u8);
        let io_bytes = std::array::from_fn(|i| 2 * i as u8);
        let pak_key = PakKey::from_bytes(&pak_bytes);
        let io_key = PakKey::from_bytes(&io_bytes);
        for version in [4u32, 11] {
            let mut pak = encrypt(&pak_bytes);
            if version >= 7 { pak.extend_from_slice(&[1; 16]); }
            pak.push(1);
            pak.extend_from_slice(&0x5a6f12e1u32.to_le_bytes());
            pak.extend_from_slice(&version.to_le_bytes());
            pak.extend_from_slice(&0u64.to_le_bytes());
            pak.extend_from_slice(&(index.len() as u64).to_le_bytes());
            pak.extend_from_slice(&Sha1::digest(&index));
            if version >= 8 { pak.extend_from_slice(&[0; 5 * 32]); }
            let path = dir.path().join(format!("test-{version}.pak"));
            std::fs::write(&path, pak).unwrap();
            let probe = Probe::read(&path).unwrap();
            assert!(probe.opens(&pak_key).is_some());
            assert!(probe.opens(&io_key).is_none());
        }

        let mut toc = vec![0; 144];
        toc[..16].copy_from_slice(b"-==--==--==--==-");
        toc[16] = 5;
        toc[20..24].copy_from_slice(&144u32.to_le_bytes());
        toc[32..36].copy_from_slice(&12u32.to_le_bytes());
        toc[48..52].copy_from_slice(&(index.len() as u32).to_le_bytes());
        toc[80] = 0x0a;
        toc.extend_from_slice(&encrypt(&io_bytes));
        let path = dir.path().join("test.utoc");
        std::fs::write(&path, toc).unwrap();
        let probe = Probe::read(&path).unwrap();
        assert!(probe.opens(&io_key).is_some());
        assert!(probe.opens(&pak_key).is_none());
    }
}
