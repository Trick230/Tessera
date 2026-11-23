// SPDX-License-Identifier: GPL-3.0-or-later
//! Indexed reads from standard Unreal IoStore containers.

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use crate::oodle::Oodle;
use crate::unreal_pak::{decode_block, take, PakKey, PakScan};
use crate::unreal_shader::{Groups, Payload};

const HEADER_SIZE: usize = 144;
const MAX_CHUNK: usize = 256 << 20;

struct Toc<'a> {
    ids: &'a [u8],
    ranges: &'a [u8],
    blocks: &'a [u8],
    methods: &'a [u8],
    method_width: usize,
    block_size: usize,
    partition_count: u32,
    partition_size: u64,
    encrypted: bool,
    ue5: bool,
}

impl<'a> Toc<'a> {
    fn parse(mut data: &'a [u8]) -> Option<Self> {
        let header = take(&mut data, HEADER_SIZE)?;
        let version = header[16];
        let field = |at| u32::from_le_bytes(header[at..at + 4].try_into().unwrap()) as usize;
        if &header[..16] != b"-==--==--==--==-" || !(1..=8).contains(&version)
            || field(20) != HEADER_SIZE || field(32) != 12 || field(44) == 0 {
            return None;
        }
        let ids = take(&mut data, field(24).checked_mul(12)?)?;
        let ranges = take(&mut data, field(24).checked_mul(10)?)?;
        if version >= 4 {
            take(&mut data, field(84).checked_mul(4)?)?;
        }
        if version >= 5 {
            take(&mut data, field(96).checked_mul(4)?)?;
        }
        let blocks = take(&mut data, field(28).checked_mul(12)?)?;
        let method_width = field(40);
        let methods = take(&mut data, field(36).checked_mul(method_width)?)?;
        let (partition_count, partition_size) = if version < 3 {
            (1, u64::MAX)
        } else {
            (field(52) as u32, u64::from_le_bytes(header[88..96].try_into().ok()?))
        };
        if partition_size == 0 || (partition_count == 0 && !blocks.is_empty()) {
            return None;
        }
        Some(Self {
            ids, ranges, blocks, methods, method_width, block_size: field(44),
            partition_count, partition_size, encrypted: header[80] & 2 != 0,
            ue5: version >= 5 || (ids.chunks_exact(12).any(|id| id[11] == 6)
                && !ids.chunks_exact(12).any(|id| id[11] == 10)),
        })
    }

    fn method(&self, index: usize) -> Option<&[u8]> {
        if index == 0 { return Some(b"None"); }
        let start = (index - 1).checked_mul(self.method_width)?;
        let name = self.methods.get(start..start.checked_add(self.method_width)?)?;
        Some(name.split(|&b| b == 0).next()?)
    }

    fn shader_types(&self) -> (u8, u8) {
        if self.ue5 { (8, 9) } else { (11, 12) }
    }
}

fn uint40(bytes: &[u8]) -> usize {
    bytes.iter().fold(0, |value, &byte| (value << 8) | usize::from(byte))
}

struct Reader<'a> {
    toc: Toc<'a>,
    path: &'a Path,
    key: Option<&'a PakKey>,
    oodle: Option<Oodle>,
    files: HashMap<u32, File>,
    compressed: Vec<u8>,
    block: Vec<u8>,
    last_block: Option<usize>,
}

impl Reader<'_> {
    fn new<'a>(toc: Toc<'a>, path: &'a Path, key: Option<&'a PakKey>, oodle: Option<Oodle>) -> Reader<'a> {
        Reader {
            toc, path, key, oodle, files: HashMap::new(), compressed: Vec::new(), block: Vec::new(),
            last_block: None,
        }
    }

    fn partition_path(&self, index: u32) -> Option<PathBuf> {
        if index == 0 { return Some(self.path.with_extension("ucas")); }
        let mut name = self.path.file_stem()?.to_os_string();
        name.push(format!("_s{index}.ucas"));
        Some(self.path.with_file_name(name))
    }

    fn load_block(&mut self, index: usize) -> Option<()> {
        if self.last_block == Some(index) { return Some(()); }
        self.last_block = None;
        let at = index.checked_mul(12)?;
        let entry = self.toc.blocks.get(at..at.checked_add(12)?)?;
        let packed = u64::from_le_bytes(entry[..8].try_into().ok()?);
        let offset = packed & ((1 << 40) - 1);
        let size = (packed >> 40) as usize;
        let raw = u32::from_le_bytes(entry[8..12].try_into().ok()?) as usize & 0x00ff_ffff;
        let method = self.toc.method(entry[11] as usize)?;
        let partition = u32::try_from(offset / self.toc.partition_size).ok()?;
        if partition >= self.toc.partition_count || size == 0 || raw == 0 || raw > self.toc.block_size {
            return None;
        }
        if !self.files.contains_key(&partition) {
            self.files.insert(partition, File::open(self.partition_path(partition)?).ok()?);
        }
        let file = self.files.get_mut(&partition)?;
        let stored_size = if self.toc.encrypted { (size + 15) & !15 } else { size };
        let local_offset = offset % self.toc.partition_size;
        if local_offset.checked_add(stored_size as u64)? > self.toc.partition_size {
            return None;
        }
        self.compressed.resize(stored_size, 0);
        file.seek(SeekFrom::Start(local_offset)).ok()?;
        file.read_exact(&mut self.compressed).ok()?;
        if self.toc.encrypted {
            self.key?.decode(&mut self.compressed)?;
        }
        self.block.clear();
        decode_block(method, &self.compressed[..size], raw, self.oodle, &mut self.block)?;
        self.last_block = Some(index);
        Some(())
    }

    fn read_chunk(&mut self, index: usize, out: &mut Vec<u8>) -> Option<()> {
        out.clear();
        let at = index.checked_mul(10)?;
        let range = self.toc.ranges.get(at..at.checked_add(10)?)?;
        let mut offset = uint40(&range[..5]);
        let mut remaining = uint40(&range[5..]);
        if remaining == 0 || remaining > MAX_CHUNK { return None; }
        let block_size = self.toc.block_size;
        while remaining > 0 {
            self.load_block(offset / block_size)?;
            let start = offset % block_size;
            let size = (block_size - start).min(remaining);
            out.extend_from_slice(self.block.get(start..start + size)?);
            offset += size;
            remaining -= size;
        }
        Some(())
    }
}

pub(crate) struct Catalog<'a> {
    archives: Vec<(&'a Path, Option<&'a PakKey>, memmap2::Mmap)>,
    groups: HashMap<[u8; 12], Vec<(usize, usize)>>,
}

impl<'a> Catalog<'a> {
    pub(crate) fn new(paths: &'a [PathBuf], key: impl Fn(&Path) -> Option<&'a PakKey>) -> Self {
        let mut catalog = Self { archives: Vec::new(), groups: HashMap::new() };
        for path in paths {
            if !path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("utoc")) { continue; }
            let Ok(file) = File::open(path) else { continue };
            // SAFETY: Read-only mapping of the table, independent of the partition files.
            let Ok(map) = (unsafe { memmap2::Mmap::map(&file) }) else { continue };
            let Some(toc) = Toc::parse(&map) else { continue };
            let key = key(path);
            if toc.encrypted && key.is_none() { continue; }
            let index = catalog.archives.len();
            let group_type = toc.shader_types().1;
            for (chunk, id) in toc.ids.chunks_exact(12).enumerate() {
                if id[11] == group_type {
                    let id: [u8; 12] = id.try_into().unwrap();
                    catalog.groups.entry(id).or_default().push((index, chunk));
                }
            }
            catalog.archives.push((path, key, map));
        }
        catalog
    }

    pub(crate) fn read_group(&self, id: &[u8], from: &Path, oodle: Option<Oodle>,
                             out: &mut Vec<u8>) -> Option<()> {
        for &(archive, chunk) in self.groups.get(id)? {
            let (path, key, map) = &self.archives[archive];
            if *path == from { continue; }
            let Some(toc) = Toc::parse(map) else { continue };
            if Reader::new(toc, path, *key, oodle).read_chunk(chunk, out).is_some() {
                return Some(());
            }
        }
        None
    }
}

pub(crate) fn scan(toc: &[u8], path: &Path, key: Option<&PakKey>, oodle: Option<Oodle>,
                   mut read_external: impl FnMut(&[u8], &mut Vec<u8>) -> Option<()>,
                   mut found: impl FnMut(Payload, &[u8])) -> PakScan {
    let mut result = PakScan::default();
    let Some(toc) = Toc::parse(toc) else { return result };
    if toc.encrypted && key.is_none() {
        result.locked = true;
        return result;
    }
    result.opened = true;
    let ids = toc.ids;
    let ue5 = toc.ue5;
    let (library_type, group_type) = toc.shader_types();
    let mut reader = Reader::new(toc, path, key, oodle);
    let mut out = Vec::new();
    let groups_by_id: HashMap<_, _> = ids.chunks_exact(12).enumerate()
        .filter(|(_, id)| id[11] == group_type).map(|(index, id)| (id, index)).collect();
    let mut split_groups = HashSet::new();
    let (mut packed, mut decoded) = (Vec::new(), Vec::new());
    for (index, id) in ids.chunks_exact(12).enumerate() {
        if id[11] != library_type { continue; }
        let Some(library) = reader.read_chunk(index, &mut out).and_then(|()| Groups::parse(&out)) else {
            result.unreadable_libraries += 1;
            continue;
        };
        result.packages += 1;
        result.expected_shaders += library.expected();
        for (group, id) in library.ids().enumerate() {
            let mut emit = |code: &[u8]| found(Payload::Shader, code);
            if let Some(&chunk) = groups_by_id.get(id) {
                if reader.read_chunk(chunk, &mut packed).is_some() &&
                    library.scan_group(group, &packed, oodle, &mut decoded, &mut emit).is_some() {
                    split_groups.insert(chunk);
                    continue;
                }
            }
            if read_external(id, &mut packed).is_some() {
                library.scan_group(group, &packed, oodle, &mut decoded, &mut emit);
            }
        }
    }
    for (index, id) in ids.chunks_exact(12).enumerate() {
        if id[11] != group_type && !matches!((ue5, id[11]), (true, 1 | 7) | (false, 2)) {
            continue;
        }
        if split_groups.contains(&index) { continue; }
        if reader.read_chunk(index, &mut out).is_some() {
            result.packages += 1;
            found(Payload::Package, &out);
        } else {
            result.unreadable += 1;
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn reads_chunk_across_compressed_partitions() {
        use aes::cipher::{BlockEncrypt, KeyInit};

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("test.utoc");
        let mut encoder = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
        encoder.write_all(b"efgh").unwrap();
        let compressed = encoder.finish().unwrap();
        std::fs::write(path.with_extension("ucas"), b"abcd").unwrap();
        std::fs::write(dir.path().join("test_s1.ucas"), &compressed).unwrap();

        let mut toc = vec![0; HEADER_SIZE];
        toc[..16].copy_from_slice(b"-==--==--==--==-");
        toc[16] = 5;
        for (at, value) in [(20, 144u32), (24, 1), (28, 2), (32, 12), (36, 1), (40, 8),
                            (44, 4), (52, 2)] {
            toc[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        toc[88..96].copy_from_slice(&64u64.to_le_bytes());
        toc.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9]);
        toc.extend_from_slice(&[0, 0, 0, 0, 2, 0, 0, 0, 0, 5]);
        for (offset, size, method) in [(0, 4, 0), (64, compressed.len(), 1)] {
            toc.extend_from_slice(&(offset | ((size as u64) << 40)).to_le_bytes());
            toc.extend_from_slice(&(4u32 | (method << 24)).to_le_bytes());
        }
        toc.extend_from_slice(b"Zlib\0\0\0\0");
        let result = scan(&toc, &path, None, None, |_, _| None, |_, bytes| assert_eq!(bytes, b"cdefg"));
        assert_eq!((result.packages, result.unreadable), (1, 0));

        // Encryption pads each stored block independently, including blocks in
        // the second partition. A reader without the key reports the container as locked.
        toc[80] = 2;
        let locked = scan(&toc, &path, None, None, |_, _| None, |_, _| panic!("locked archive emitted bytes"));
        assert!(locked.locked);
        let key_file = dir.path().join("fixture.key");
        std::fs::write(&key_file, "07".repeat(32)).unwrap();
        let key = PakKey::from_file(&key_file).unwrap();
        let cipher = aes::Aes256::new((&[7u8; 32]).into());
        for (partition, bytes) in [(path.with_extension("ucas"), b"abcd".as_slice()),
                                   (dir.path().join("test_s1.ucas"), compressed.as_slice())] {
            let mut encrypted = bytes.to_vec();
            encrypted.resize(encrypted.len().next_multiple_of(16), 0);
            for block in encrypted.chunks_exact_mut(16) {
                cipher.encrypt_block(aes::cipher::Block::<aes::Aes256>::from_mut_slice(block));
            }
            std::fs::write(partition, encrypted).unwrap();
        }
        let result = scan(&toc, &path, Some(&key), None, |_, _| None,
                          |_, bytes| assert_eq!(bytes, b"cdefg"));
        assert_eq!((result.packages, result.unreadable, result.locked), (1, 0, false));
    }
}
