// SPDX-License-Identifier: GPL-3.0-or-later
//! Named shader entries in CRI CPK archives, including CRILAYLA compression.

const MAX_ENTRY: usize = 64 << 20;

fn int(bytes: &[u8], at: usize, size: usize) -> Option<usize> {
    bytes.get(at..at.checked_add(size)?)?.iter().try_fold(0usize, |v, &b| {
        v.checked_mul(256)?.checked_add(b as usize)
    })
}

fn le32(bytes: &[u8], at: usize) -> Option<usize> {
    Some(u32::from_le_bytes(bytes.get(at..at.checked_add(4)?)?.try_into().ok()?) as usize)
}

struct Column {
    name: usize,
    size: usize,
    constant: Option<usize>,
    position: Option<usize>,
}

struct Table<'a> {
    bytes: &'a [u8],
    rows: usize,
    width: usize,
    count: usize,
    strings: &'a [u8],
    columns: Vec<Column>,
}

impl<'a> Table<'a> {
    fn parse(packet: &'a [u8], at: usize, tag: &[u8; 4]) -> Option<Self> {
        let packet = packet.get(at..)?;
        if !packet.starts_with(tag) { return None; }
        let bytes = packet.get(16..)?;
        if !bytes.starts_with(b"@UTF") { return None; }
        let total = int(bytes, 4, 4)?.checked_add(8)?;
        let bytes = bytes.get(..total)?;
        let rows = int(bytes, 8, 4)?.checked_add(8)?;
        let strings_at = int(bytes, 12, 4)?.checked_add(8)?;
        let strings_end = int(bytes, 16, 4)?.checked_add(8)?;
        let strings = bytes.get(strings_at..strings_end)?;
        let count = int(bytes, 28, 4)?;
        let width = int(bytes, 26, 2)?;
        if width == 0 || rows.checked_add(count.checked_mul(width)?)? > strings_at { return None; }
        let (mut field, mut position) = (32, 0usize);
        let mut columns = Vec::new();
        for _ in 0..int(bytes, 24, 2)? {
            let flags = *bytes.get(field)?;
            let name = int(bytes, field + 1, 4)?;
            let size = match flags & 15 {
                0 | 1 => 1,
                2 | 3 => 2,
                4 | 5 | 8 | 10 => 4,
                6 | 7 | 9 | 11 => 8,
                _ => return None,
            };
            field += 5;
            let (constant, position_at) = match flags >> 4 {
                1 => (None, None),
                3 => {
                    let at = field;
                    field += size;
                    (Some(at), None)
                }
                5 => {
                    let at = position;
                    position += size;
                    (None, Some(at))
                }
                _ => return None,
            };
            if field > rows || position > width { return None; }
            columns.push(Column { name, size, constant, position: position_at });
        }
        Some(Self { bytes, rows, width, count, strings, columns })
    }

    fn string(&self, at: usize) -> Option<&'a [u8]> {
        let bytes = self.strings.get(at..)?;
        bytes.get(..memchr::memchr(0, bytes)?)
    }

    fn column(&self, name: &[u8]) -> Option<&Column> {
        self.columns.iter().find(|c| self.string(c.name) == Some(name))
    }

    fn value(&self, row: usize, column: &Column) -> Option<usize> {
        if row >= self.count { return None; }
        let at = column.constant.or_else(|| column.position.map(|p| self.rows + row * self.width + p));
        at.map_or(Some(0), |at| int(self.bytes, at, column.size))
    }
}

/// Returns the stored ranges and exact decoded sizes of named shader entries.
pub(crate) fn shaders(archive: &[u8]) -> Option<Vec<(&[u8], usize)>> {
    let header = Table::parse(archive, 0, b"CPK ")?;
    let toc_offset = header.value(0, header.column(b"TocOffset")?)?;
    let content = header.value(0, header.column(b"ContentOffset")?)?;
    let base = toc_offset.min(content);
    let toc = Table::parse(archive, toc_offset, b"TOC ")?;
    let (name, offset, stored, raw) = (toc.column(b"FileName")?, toc.column(b"FileOffset")?,
        toc.column(b"FileSize")?, toc.column(b"ExtractSize")?);
    let mut entries = Vec::new();
    for row in 0..toc.count {
        let Some(name) = toc.value(row, name).and_then(|i| toc.string(i)) else { continue };
        let name = name.to_ascii_lowercase();
        if !name.windows(6).any(|part| part == b"shader") &&
            ![b".vso", b".pso", b".cso", b".gso", b".hso", b".dso"].iter().any(|ext| name.ends_with(*ext)) {
            continue;
        }
        let size = toc.value(row, raw)?;
        if size > MAX_ENTRY { continue; }
        let start = base.checked_add(toc.value(row, offset)?)?;
        let bytes = archive.get(start..start.checked_add(toc.value(row, stored)?)?)?;
        entries.push((bytes, size));
    }
    Some(entries)
}

struct ReverseBits<'a> {
    input: &'a [u8],
    bits: u32,
    available: usize,
}

impl ReverseBits<'_> {
    fn take(&mut self, count: usize) -> Option<usize> {
        while self.available < count {
            let (&next, prefix) = self.input.split_last()?;
            self.input = prefix;
            self.bits = (self.bits << 8) | u32::from(next);
            self.available += 8;
        }
        self.available -= count;
        Some(((self.bits >> self.available) as usize) & ((1 << count) - 1))
    }
}

pub(crate) fn decompress(bytes: &[u8], expected: usize, out: &mut Vec<u8>) -> Option<()> {
    if !bytes.starts_with(b"CRILAYLA") || expected > MAX_ENTRY { return None; }
    let size = le32(bytes, 8)?.checked_add(256)?;
    if size != expected { return None; }
    let head = le32(bytes, 12)?.checked_add(16)?;
    let mut bits = ReverseBits { input: bytes.get(16..head)?, bits: 0, available: 0 };
    let initial = bytes.get(head..head.checked_add(256)?)?;
    out.clear();
    out.resize(size, 0);
    out[..256].copy_from_slice(initial);
    let mut end = size;
    while end > 256 {
        if bits.take(1)? == 0 {
            end -= 1;
            out[end] = bits.take(8)? as u8;
            continue;
        }
        let distance = bits.take(13)? + 3;
        let mut length = 3;
        for width in [2, 3, 5].into_iter().chain(std::iter::repeat(8)) {
            let extra = bits.take(width)?;
            length += extra;
            if length > end - 256 { return None; }
            if extra != (1 << width) - 1 { break; }
        }
        if end - 1 + distance >= size { return None; }
        for _ in 0..length {
            end -= 1;
            out[end] = out[end + distance];
        }
    }
    Some(())
}
