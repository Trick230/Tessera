// SPDX-License-Identifier: GPL-3.0-or-later
//! Offline inputs for the embedded PSO list in Unleashed Recompiled 1.0.3.

use std::collections::{BTreeMap, BTreeSet};
use std::fs::File;
use std::io::{self, Read};
use std::path::Path;

use sha1::{Digest, Sha1};

// This release stores native C++ arrays, with no versioned on-disk format.
// Check the entire image before applying file offsets for this x64 build.
const IMAGE_SIZE: u64 = 74_799_616;
const IMAGE_SHA1: [u8; 20] = [
    0xcc, 0xac, 0xb5, 0x06, 0x1c, 0x2b, 0xcd, 0x0f, 0x44, 0x0a,
    0x96, 0x8f, 0x51, 0x61, 0xee, 0x31, 0x69, 0x0b, 0x38, 0x95,
];
const PIPELINES: usize = 0x02b4_2530;
const PIPELINE_COUNT: usize = 217;
const PIPELINE_SIZE: usize = 109;
const SHADERS: usize = 0x0460_da60;
const SHADER_COUNT: usize = 1385;
const SHADER_SIZE: usize = 40;
const DXIL: usize = 0x02b7_dd40;
const SPIRV: usize = 0x02ec_ec70;
const VERTEX_ELEMENT_SIZE: usize = 12;
const VERTEX_DECLARATIONS: [usize; 24] = [
    0x32e0da0, 0x32e0e50, 0x32e0ea0, 0x32e0f10, 0x32e0fa0, 0x32e1030,
    0x32e1060, 0x32e1090, 0x32e10c0, 0x32e1150, 0x32e1180, 0x32e11e0,
    0x32e1250, 0x2b423d0, 0x32e1370, 0x32e13b0, 0x32e13e0, 0x32e1450,
    0x32e14d0, 0x32e1500, 0x32e1580, 0x32e15f0, 0x32e1670, 0x32e1700,
];

pub struct Pipeline {
    pub vertex_shader: u64,
    pub pixel_shader: Option<u64>,
    pub vertex_declaration: u64,
    /// Packed Unleashed state following the three hashes.
    pub state: [u8; PIPELINE_SIZE - 24],
}

pub struct Shader {
    pub dxil: Vec<u8>,
    pub spirv: Vec<u8>,
    pub specialization_mask: u32,
}

pub struct Inventory {
    pub pipelines: Vec<Pipeline>,
    pub indexed_shaders: usize,
    pub referenced_shaders: BTreeMap<u64, Shader>,
    /// Guest vertex elements, keyed by their normalized declaration hash.
    pub vertex_declarations: BTreeMap<u64, Vec<u8>>,
}

/// Input for the native Vulkan converter. Its packed state stays specific to
/// the identified executable; the converter emits ordinary Tessera recipes.
pub fn write_vulkan_inputs(exe: &Path, mut out: impl io::Write, samples: u8, bicubic: bool, transparency_aa: bool) -> io::Result<()> {
    if !matches!(samples, 1 | 2 | 4 | 8) {
        return Err(io::Error::other("expected 1, 2, 4 or 8 MSAA samples"));
    }
    let inventory = inventory(exe)?;
    out.write_all(b"TSRURV1\0")?;
    out.write_all(&[samples, u8::from(bicubic), u8::from(transparency_aa), 0])?;
    out.write_all(&(inventory.pipelines.len() as u32).to_le_bytes())?;
    for pipeline in &inventory.pipelines {
        let vertex = &inventory.referenced_shaders[&pipeline.vertex_shader];
        let pixel = pipeline.pixel_shader.map(|hash| &inventory.referenced_shaders[&hash]);
        let mask = vertex.specialization_mask | pixel.map_or(0, |shader| shader.specialization_mask);
        out.write_all(&pipeline.state)?;
        out.write_all(&mask.to_le_bytes())?;
        let declaration = &inventory.vertex_declarations[&pipeline.vertex_declaration];
        let fragment = pixel.map_or(&[][..], |shader| shader.spirv.as_slice());
        for bytes in [declaration.as_slice(), vertex.spirv.as_slice(), fragment] {
            out.write_all(&(bytes.len() as u32).to_le_bytes())?;
            out.write_all(bytes)?;
        }
    }
    out.flush()
}

fn u64_at(bytes: &[u8], pos: usize) -> u64 {
    u64::from_le_bytes(bytes[pos..pos + 8].try_into().unwrap())
}

fn u32_at(bytes: &[u8], pos: usize) -> u32 {
    u32::from_le_bytes(bytes[pos..pos + 4].try_into().unwrap())
}

fn decompress(frame: &[u8], size: usize) -> io::Result<Vec<u8>> {
    let decoder = ruzstd::decoding::StreamingDecoder::new(frame)
        .map_err(|error| io::Error::other(error.to_string()))?;
    let mut data = Vec::with_capacity(size);
    decoder.take(size as u64 + 1).read_to_end(&mut data)?;
    if data.len() != size {
        return Err(io::Error::other("unexpected Unleashed shader archive size"));
    }
    Ok(data)
}

fn bytecode<'a>(archive: &'a [u8], entry: &[u8], pos: usize) -> io::Result<&'a [u8]> {
    let offset = u32_at(entry, pos) as usize;
    let size = u32_at(entry, pos + 4) as usize;
    archive.get(offset..offset.saturating_add(size))
        .ok_or_else(|| io::Error::other("Unleashed shader range exceeds archive"))
}

fn vertex_declarations(image: &[u8]) -> io::Result<BTreeMap<u64, Vec<u8>>> {
    let mut declarations = BTreeMap::new();
    for offset in VERTEX_DECLARATIONS {
        let mut bytes = Vec::new();
        let mut terminated = false;
        for element in image[offset..].chunks_exact(VERTEX_ELEMENT_SIZE).take(64) {
            if element[..2] == [0, 255] || element[4..8] == [255; 4] {
                terminated = true;
                break;
            }
            bytes.extend_from_slice(&element[..11]);
            bytes.push(0);
        }
        if !terminated {
            return Err(io::Error::other("unterminated Unleashed vertex declaration"));
        }
        declarations.insert(xxhash_rust::xxh3::xxh3_64(&bytes), bytes);
    }
    Ok(declarations)
}

pub fn inventory(exe: &Path) -> io::Result<Inventory> {
    let file = File::open(exe)?;
    let unsupported = || io::Error::other("unsupported Unleashed executable (expected Windows x64 1.0.3)");
    if file.metadata()?.len() != IMAGE_SIZE {
        return Err(unsupported());
    }
    let image = unsafe { memmap2::Mmap::map(&file) }?;
    if Sha1::digest(&image)[..] != IMAGE_SHA1 {
        return Err(unsupported());
    }

    let vertex_declarations = vertex_declarations(&image)?;
    let mut needed = BTreeSet::new();
    let mut pipelines = Vec::with_capacity(PIPELINE_COUNT);
    for row in image[PIPELINES..PIPELINES + PIPELINE_COUNT * PIPELINE_SIZE].chunks_exact(PIPELINE_SIZE) {
        let vertex_shader = u64_at(row, 0);
        let pixel = u64_at(row, 8);
        let pixel_shader = (pixel != 0).then_some(pixel);
        let vertex_declaration = u64_at(row, 16);
        if !vertex_declarations.contains_key(&vertex_declaration) {
            return Err(io::Error::other("Unleashed PSO list references a missing vertex declaration"));
        }
        needed.insert(vertex_shader);
        needed.extend(pixel_shader);
        pipelines.push(Pipeline {
            vertex_shader,
            pixel_shader,
            vertex_declaration,
            state: row[24..].try_into().unwrap(),
        });
    }

    let dxil = decompress(&image[DXIL..SPIRV], 14_151_172)?;
    let spirv = decompress(&image[SPIRV..], 11_418_694)?;
    let mut referenced_shaders = BTreeMap::new();
    let rows = &image[SHADERS..SHADERS + SHADER_COUNT * SHADER_SIZE];
    for entry in rows.chunks_exact(SHADER_SIZE) {
        let hash = u64_at(entry, 0);
        if !needed.remove(&hash) {
            continue;
        }
        let dxil = bytecode(&dxil, entry, 8)?;
        if !crate::scan::container_at(dxil).is_some_and(|container| container.len == dxil.len()) {
            return Err(io::Error::other(format!("invalid Unleashed DXIL shader {hash:016x}")));
        }
        let packed = bytecode(&spirv, entry, 16)?;
        let mut spirv = Vec::new();
        if crate::unity::smolv::decode(packed, false, &mut spirv) != Some(packed.len()) {
            return Err(io::Error::other(format!("invalid Unleashed SPIR-V shader {hash:016x}")));
        }
        referenced_shaders.insert(hash, Shader { dxil: dxil.to_vec(), spirv, specialization_mask: u32_at(entry, 24) });
    }
    if !needed.is_empty() {
        return Err(io::Error::other("Unleashed PSO list references missing shaders"));
    }
    Ok(Inventory { pipelines, indexed_shaders: SHADER_COUNT, referenced_shaders, vertex_declarations })
}
