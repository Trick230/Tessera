// SPDX-License-Identifier: GPL-3.0-or-later
//! Little-endian Artdink APKs used by Accel World VS. Sword Art Online.
//! PACKTOC/PACKFSLS/PACKFSHD layout documented by QuickBMS dragon_ball_z_boz.bms.

fn u32_at(data: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_le_bytes(data.get(at..at.checked_add(4)?)?.try_into().ok()?) as usize)
}

fn u64_at(data: &[u8], at: usize) -> Option<usize> {
    usize::try_from(u64::from_le_bytes(data.get(at..at.checked_add(8)?)?.try_into().ok()?)).ok()
}

fn tagged<'a>(data: &'a [u8], at: usize, tag: &[u8; 8]) -> Option<(&'a [u8], usize)> {
    let rest = data.get(at..)?;
    if !rest.starts_with(tag) { return None; }
    let start = at.checked_add(16)?;
    let end = start.checked_add(u64_at(rest, 8)?)?;
    Some((data.get(start..end)?, end))
}

pub(crate) struct Entry<'a> {
    pub bytes: &'a [u8],
    pub unpacked: usize,
    pub compressed: bool,
}

fn entry<'a>(data: &'a [u8], record: &[u8], at: usize) -> Option<Option<Entry<'a>>> {
    let start = u64_at(record, at)?;
    let unpacked = u64_at(record, at + 8)?;
    let packed = u64_at(record, at + 16)?;
    if unpacked == 0 { return Some(None); }
    let compressed = packed != 0;
    let bytes = data.get(start..start.checked_add(if compressed { packed } else { unpacked })?)?;
    Some(Some(Entry { bytes, unpacked, compressed }))
}

pub(crate) fn entries(data: &[u8]) -> Option<Vec<Entry<'_>>> {
    if !data.starts_with(b"ENDILTLE") { return None; }
    let (_, front) = tagged(data, 16, b"PACKHEDR")?;
    let (toc, next) = tagged(data, front, b"PACKTOC ")?;
    let (packs, _) = tagged(data, next, b"PACKFSLS")?;
    if u32_at(toc, 0)? != 40 { return None; }
    let count = u32_at(toc, 4)?;
    let folders = u32_at(toc, 8)?.min(count);
    let mut found = Vec::new();
    let table = toc.get(16..16usize.checked_add(count.checked_mul(40)?)?)?;
    for record in table.chunks_exact(40).skip(folders) {
        found.extend(entry(data, record, 16)?);
    }
    let pack_count = u32_at(packs, 0)?;
    let table = packs.get(16..16usize.checked_add(pack_count.checked_mul(40)?)?)?;
    for record in table.chunks_exact(40) {
        let start = u64_at(record, 8)?;
        let size = u64_at(record, 16)?;
        let pack = data.get(start..start.checked_add(size)?)?;
        if !pack.starts_with(b"ENDILTLE") { return None; }
        let (header, _) = tagged(pack, 16, b"PACKFSHD")?;
        if u32_at(header, 4)? != 32 { return None; }
        let count = u32_at(header, 8)?;
        let table = header.get(32..32usize.checked_add(count.checked_mul(32)?)?)?;
        for record in table.chunks_exact(32) {
            found.extend(entry(pack, record, 8)?);
        }
    }
    Some(found)
}
