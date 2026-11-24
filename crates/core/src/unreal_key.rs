// SPDX-License-Identifier: GPL-3.0-or-later
//! Reconstructs possible archive keys from constants stored by game code.

use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use iced_x86::{Decoder, DecoderOptions, Instruction, Mnemonic, OpKind, Register};
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

struct Write {
    ip: u64,
    base: Register,
    offset: i64,
    bytes: [u8; 32],
    size: usize,
}

fn constant(bytes: &[u8]) -> Option<[u8; 32]> {
    let mut value = [0; 32];
    value.get_mut(..bytes.len())?.copy_from_slice(bytes);
    Some(value)
}

fn move_size(instruction: &Instruction) -> Option<usize> {
    use Mnemonic::*;
    match instruction.mnemonic() {
        Mov | Movaps | Movups | Movapd | Movupd | Movdqa | Movdqu | Vmovaps | Vmovups |
        Vmovapd | Vmovupd | Vmovdqa | Vmovdqu => {
            let size = instruction.memory_size().size();
            matches!(size, 4 | 8 | 16 | 32).then_some(size)
        }
        _ => None,
    }
}

fn assembled_key(writes: &VecDeque<Write>, newest: &Write, start: i64) -> Option<[u8; 32]> {
    let end = start.checked_add(32)?;
    if !(start..end).contains(&newest.offset) { return None; }
    let mut occupied = 0u32;
    let mut key = [0; 32];
    for write in std::iter::once(newest).chain(writes.iter().rev()) {
        if write.base != newest.base { continue; }
        for (index, byte) in write.bytes[..write.size].iter().enumerate() {
            let address = write.offset.checked_add(index as i64)?;
            if !(start..end).contains(&address) { continue; }
            let slot = (address - start) as usize;
            let bit = 1 << slot;
            if occupied & bit == 0 {
                key[slot] = *byte;
                occupied |= bit;
            }
        }
        if occupied == u32::MAX { return Some(key); }
    }
    None
}

fn scan_code(bitness: u32, address: u64, code: &[u8], load: impl Fn(u64, usize) -> Option<[u8; 32]>,
             mut candidate: impl FnMut([u8; 32]) -> bool) -> bool {
    let mut decoder = Decoder::with_ip(bitness, code, address, DecoderOptions::NONE);
    let mut registers = HashMap::new();
    let mut writes: VecDeque<Write> = VecDeque::new();
    while decoder.can_decode() {
        let ins = decoder.decode();
        let ip = ins.ip();
        if ins.op_count() != 2 { continue; }
        if ins.op0_kind() == OpKind::Register {
            let dst = ins.op0_register();
            registers.remove(&dst);
            let value = match ins.op1_kind() {
                OpKind::Immediate64 if ins.mnemonic() == Mnemonic::Mov =>
                    constant(&ins.immediate64().to_le_bytes()).map(|value| (8, value)),
                OpKind::Memory if ins.is_ip_rel_memory_operand() => move_size(&ins)
                    .and_then(|size| load(ins.ip_rel_memory_address(), size).map(|value| (size, value))),
                _ => None,
            };
            if let Some((size, value)) = value { registers.insert(dst, (ip, size, value)); }
            continue;
        }
        if ins.op0_kind() != OpKind::Memory || ins.memory_index() != Register::None
            || ins.is_ip_rel_memory_operand() || ins.memory_base() == Register::None
        {
            continue;
        }
        let Some(size) = move_size(&ins) else { continue };
        let bytes = match ins.op1_kind() {
            OpKind::Immediate32 if size == 4 => constant(&ins.immediate32().to_le_bytes()),
            OpKind::Register => registers.get(&ins.op1_register())
                .filter(|(loaded_at, width, _)| ip - loaded_at < 64 && *width == size)
                .map(|&(_, _, bytes)| bytes),
            _ => None,
        };
        let Some(bytes) = bytes else { continue };
        let offset = if bitness == 32 {
            i64::from(ins.memory_displacement32() as i32)
        } else {
            ins.memory_displacement64() as i64
        };
        let newest = Write { ip, base: ins.memory_base(), offset, bytes, size };
        while writes.front().is_some_and(|write| ip - write.ip > 160) { writes.pop_front(); }
        let starts = writes.iter().filter(|write| write.base == newest.base).map(|write| write.offset);
        if std::iter::once(newest.offset).chain(starts).filter_map(|start| assembled_key(&writes, &newest, start))
            .any(&mut candidate)
        {
            return true;
        }
        writes.push_back(newest);
    }
    false
}

fn scan_pe(exe: &Path, mut candidate: impl FnMut([u8; 32]) -> bool) -> Option<()> {
    let file = File::open(exe).ok()?;
    if file.metadata().ok()?.len() > i32::MAX as u64 { return None; }
    // SAFETY: read-only view of the on-disk game executable.
    let map = unsafe { memmap2::Mmap::map(&file) }.ok()?;
    let pe = object::File::parse(&*map).ok()?;
    let bitness = match pe.architecture() {
        object::Architecture::X86_64 => 64,
        object::Architecture::I386 => 32,
        _ => return None,
    };
    let sections: Vec<_> = pe.sections().filter_map(|s| Some((s.address(), s.data().ok()?, s.flags()))).collect();
    let load = |address: u64, size| sections.iter().find_map(|&(base, bytes, _)| {
        let start = usize::try_from(address.checked_sub(base)?).ok()?;
        constant(bytes.get(start..start.checked_add(size)?)?)
    });
    for &(address, code, flags) in &sections {
        if matches!(flags, SectionFlags::Coff { characteristics } if characteristics.0 & object::pe::IMAGE_SCN_MEM_EXECUTE.0 != 0)
            && scan_code(bitness, address, code, load, &mut candidate)
        {
            break;
        }
    }
    Some(())
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
    fn reconstructs_mixed_width_constant_writes() {
        let mut code = vec![0x48, 0xb8]; // mov rax, imm64
        let key = std::array::from_fn(|i| i as u8);
        code.extend_from_slice(&key[..8]);
        code.extend_from_slice(&[0x48, 0x89, 0x44, 0x24, 0x10]); // mov [rsp+16], rax
        for (i, bytes) in key[8..].chunks_exact(4).enumerate() {
            code.extend_from_slice(&[0xc7, 0x44, 0x24, 0x18 + 4 * i as u8]);
            code.extend_from_slice(bytes);
        }
        assert!(scan_code(64, 0x1000, &code, |_, _| None, |candidate| candidate == key));
    }

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
