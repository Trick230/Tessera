// SPDX-License-Identifier: GPL-3.0-or-later
//! Compiled Shader objects in Unity serialized files.

use super::{MAX_BLOCK, MAX_METADATA, be_u32, be_u64, c_string, take};

pub(super) struct SerializedFile {
    format: u32,
    be: bool,
    pub(super) header_size: usize,
    pub(super) metadata_size: usize,
    data_offset: u64,
    file_size: u64,
}

#[derive(Clone, Copy)]
pub(super) struct ShaderLayout {
    version: (u32, u32),
    be: bool,
}

#[derive(Clone, Copy)]
struct Reader<'a> {
    data: &'a [u8],
    be: bool,
}

impl<'a> Reader<'a> {
    fn take(&mut self, size: usize) -> Option<&'a [u8]> {
        take(&mut self.data, size)
    }

    fn u32(&mut self) -> Option<u32> {
        let bytes = self.take(4)?.try_into().ok()?;
        Some(if self.be {
            u32::from_be_bytes(bytes)
        } else {
            u32::from_le_bytes(bytes)
        })
    }

    fn array(&mut self, width: usize) -> Option<&'a [u8]> {
        let count = self.u32()? as usize;
        self.take(count.checked_mul(width)?)
    }

    fn align(&mut self, original_len: usize) -> Option<()> {
        let pos = original_len - self.data.len();
        self.take(pos.next_multiple_of(4) - pos)?;
        Some(())
    }

    fn words(self) -> impl Iterator<Item = usize> + 'a {
        self.data.chunks_exact(4).map(move |word| {
            let bytes = word.try_into().unwrap();
            (if self.be {
                u32::from_be_bytes(bytes)
            } else {
                u32::from_le_bytes(bytes)
            }) as usize
        })
    }
}

impl SerializedFile {
    pub(super) fn open(mut header: &[u8], length: u64) -> Option<Self> {
        let mut metadata_size = be_u32(&mut header)? as usize;
        let mut file_size = u64::from(be_u32(&mut header)?);
        let format = be_u32(&mut header)?;
        let mut data_offset = u64::from(be_u32(&mut header)?);
        let endian = take(&mut header, 4)?;
        if !(14..=26).contains(&format) || endian[0] > 1 || endian[1..] != [0; 3] {
            return None;
        }
        let header_size = if format >= 22 {
            metadata_size = be_u32(&mut header)? as usize;
            file_size = be_u64(&mut header)?;
            data_offset = be_u64(&mut header)?;
            48
        } else {
            20
        };
        if metadata_size == 0
            || metadata_size > MAX_METADATA
            || file_size != length
            || (header_size + metadata_size) as u64 > data_offset
            || data_offset > file_size
        {
            return None;
        }
        Some(Self {
            format,
            be: endian[0] == 1,
            header_size,
            metadata_size,
            data_offset,
            file_size,
        })
    }

    pub(super) fn shaders(&self, metadata: &[u8]) -> Option<(ShaderLayout, Vec<(u64, usize)>)> {
        let mut reader = Reader {
            data: metadata,
            be: self.be,
        };
        let version = std::str::from_utf8(c_string(&mut reader.data)?).ok()?;
        let mut version = version.split('.');
        let version = (version.next()?.parse().ok()?, version.next()?.parse().ok()?);
        reader.take(4)?; // Build target.
        let has_tree = reader.take(1)?[0] != 0;
        let count = reader.u32()? as usize;
        if count > reader.data.len() / 20 {
            return None;
        }
        let mut classes = Vec::with_capacity(count);
        for _ in 0..count {
            let class = reader.u32()?;
            let type_flags = usize::from(self.format >= 16) + 2 * usize::from(self.format >= 17);
            let script_hash =
                (self.format < 16 && (class as i32) < 0) || (self.format >= 16 && class == 114);
            reader.take(type_flags + if script_hash { 32 } else { 16 })?;
            if has_tree {
                if self.format >= 23 {
                    reader.take(16)?; // Type tree hash.
                    reader.array(1)?; // Inline tree, if present.
                } else {
                    let nodes = reader.u32()? as usize;
                    let strings = reader.u32()? as usize;
                    let node_size = if self.format >= 19 { 32 } else { 24 };
                    reader.take(nodes.checked_mul(node_size)?.checked_add(strings)?)?;
                }
                if self.format >= 21 {
                    reader.array(4)?;
                }
            }
            classes.push(class);
        }

        let mut shaders = Vec::new();
        for _ in 0..reader.u32()? {
            reader.align(metadata.len())?;
            reader.take(8)?; // Path ID.
            let low = reader.u32()?;
            let offset = if self.format >= 22 {
                let high = reader.u32()?;
                let (low, high) = if self.be { (high, low) } else { (low, high) };
                (u64::from(high) << 32) | u64::from(low)
            } else {
                u64::from(low)
            };
            let size = reader.u32()? as usize;
            let type_id = reader.u32()? as usize;
            let class = if self.format < 16 {
                let bytes = reader.take(2)?.try_into().ok()?;
                u32::from(if self.be {
                    u16::from_be_bytes(bytes)
                } else {
                    u16::from_le_bytes(bytes)
                })
            } else {
                *classes.get(type_id)?
            };
            if self.format < 17 {
                reader.take(2)?;
            } // Script type index.
            if matches!(self.format, 15 | 16) {
                reader.take(1)?;
            } // Stripped.
            if class == 48 {
                let start = self.data_offset.checked_add(offset)?;
                if start.checked_add(size as u64)? > self.file_size {
                    return None;
                }
                shaders.push((start, size));
            }
        }
        shaders.sort_unstable_by_key(|&(start, _)| start);
        Some((
            ShaderLayout {
                version,
                be: self.be,
            },
            shaders,
        ))
    }
}

impl ShaderLayout {
    pub(super) fn legacy_smolv(self) -> bool {
        self.version < (2021, 1)
    }

    pub(super) fn scan(
        self,
        data: &[u8],
        output: &mut Vec<u8>,
        mut visit: impl FnMut(&[u8]),
    ) -> Option<()> {
        let mut reader = Reader { data, be: self.be };
        let name = reader.array(1)?;
        if std::str::from_utf8(name).is_err() {
            return None;
        }
        reader.align(data.len())?;
        if self.version < (5, 5) {
            visit(reader.array(1)?);
            if self.version < (5, 3) {
                return Some(());
            }
            reader.align(data.len())?;
            reader.array(1)?; // Path name.
            reader.align(data.len())?;
            let size = reader.u32()? as usize;
            let blob = reader.array(1)?;
            return decode(blob, size, output, &mut visit);
        }

        // The variable-size ShaderLab properties and passes precede the GPU
        // data. Identify the matching four platform tables, then validate their
        // blob ranges and the trailing object fields. This also works in
        // players with the type tree stripped from the serialized file.
        let tail = reader.data;
        for offset in (0..tail.len().saturating_sub(15)).step_by(4) {
            reader.data = &tail[offset..];
            if let Some((blob, chunks)) = self.shader_blob(reader) {
                let mut complete = true;
                for (offset, stored, raw) in chunks {
                    complete &=
                        decode(&blob[offset..offset + stored], raw, output, &mut visit).is_some();
                }
                return complete.then_some(());
            }
        }
        None
    }

    fn shader_blob<'a>(
        self,
        mut reader: Reader<'a>,
    ) -> Option<(&'a [u8], Vec<(usize, usize, usize)>)> {
        let platforms = Reader {
            data: reader.array(4)?,
            be: self.be,
        };
        let count = platforms.data.len() / 4;
        if count > 32 || platforms.words().any(|p| p > 63) {
            return None;
        }
        if count == 0 && reader.data.len() < 24 {
            return None;
        }

        let tables = if self.version < (2019, 3) {
            [reader.array(4)?, reader.array(4)?, reader.array(4)?]
        } else {
            let mut tables = [&[][..]; 3];
            for table in &mut tables {
                if reader.u32()? as usize != count {
                    return None;
                }
                let start = reader.data;
                for _ in 0..count {
                    reader.array(4)?;
                }
                *table = &start[..start.len() - reader.data.len()];
            }
            tables
        };
        let blob = reader.array(1)?;
        let padding = blob.len().next_multiple_of(4) - blob.len();
        reader.take(padding)?;
        let mut with_stages = reader;
        let has_stages = self.version >= (2021, 2)
            && with_stages
                .array(4)
                .is_some_and(|stages| stages.len() / 4 == count)
            && self.valid_trailer(with_stages).is_some();
        if !has_stages {
            self.valid_trailer(reader)?;
        }

        let mut tables = tables.map(|data| Reader { data, be: self.be });
        let mut chunks = Vec::new();
        if self.version < (2019, 3) {
            append_chunks(tables, count, blob.len(), &mut chunks)?;
        } else {
            for _ in 0..count {
                let [offsets, compressed, raw] = &mut tables;
                let data = [offsets.array(4)?, compressed.array(4)?, raw.array(4)?];
                append_chunks(
                    data.map(|data| Reader { data, be: self.be }),
                    data[0].len() / 4,
                    blob.len(),
                    &mut chunks,
                )?;
            }
        }
        chunks.sort_unstable();
        chunks.dedup();
        let mut end = 0;
        for &(offset, stored, _) in &chunks {
            if offset < end {
                return None;
            }
            end = offset + stored;
        }
        (end == blob.len()).then_some((blob, chunks))
    }

    fn valid_trailer(self, mut reader: Reader<'_>) -> Option<()> {
        reader.array(12)?; // Shader dependencies.
        if self.version >= (2018, 0) {
            let tail_len = reader.data.len();
            for _ in 0..reader.u32()? {
                reader.array(1)?; // Texture name.
                reader.align(tail_len)?;
                reader.take(12)?; // Texture pointer.
            }
        }
        let baked = reader.take(4)?[0];
        // Recent builds can append the asset GUID and its local file ID.
        let trailer = reader.data.len();
        (baked <= 1 && (matches!(trailer, 0 | 16) || (self.version.0 >= 6000 && trailer == 24)))
            .then_some(())
    }
}

fn append_chunks(
    tables: [Reader<'_>; 3],
    count: usize,
    blob_size: usize,
    chunks: &mut Vec<(usize, usize, usize)>,
) -> Option<()> {
    if tables.iter().any(|table| table.data.len() / 4 != count) {
        return None;
    }
    let [offsets, stored, raw] = tables.map(Reader::words);
    for ((offset, stored), raw) in offsets.zip(stored).zip(raw) {
        if stored > blob_size.checked_sub(offset)? || (stored == 0) != (raw == 0) {
            return None;
        }
        if stored != 0 {
            chunks.push((offset, stored, raw));
        }
    }
    Some(())
}

fn decode(
    blob: &[u8],
    size: usize,
    output: &mut Vec<u8>,
    visit: &mut impl FnMut(&[u8]),
) -> Option<()> {
    if blob.is_empty() && size == 0 {
        return Some(());
    }
    if size > MAX_BLOCK {
        return None;
    }
    output.resize(size, 0);
    if lz4_flex::decompress_into(blob, output).ok()? != size {
        return None;
    }
    visit(output);
    Some(())
}
