// SPDX-License-Identifier: GPL-3.0-or-later
//! Graphics API detection from a PE executable: static and delay-load
//! imports, plus DLL names referenced as strings (LoadLibrary paths).

use memchr::{memchr2, memmem};
use object::read::pe::{ImageNtHeaders, PeFile, PeFile32, PeFile64, ResourceDirectory, ResourceDirectoryTable};
use object::{LittleEndian as LE, ReadRef};
use std::collections::HashSet;
use std::fs::File;
use std::io;
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ApiSet(pub u8);

impl ApiSet {
    pub const D3D9: u8 = 1 << 0;
    pub const D3D11: u8 = 1 << 1;
    pub const D3D12: u8 = 1 << 2;
    pub const VULKAN: u8 = 1 << 3;
    pub const OPENGL: u8 = 1 << 4;
    pub const D3D8: u8 = 1 << 5;

    pub fn has(self, bit: u8) -> bool {
        self.0 & bit != 0
    }
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }
    pub fn names(self) -> Vec<&'static str> {
        DLLS.iter()
            .filter(|(_, bit, _)| self.has(*bit))
            .map(|(_, _, n)| *n)
            .collect()
    }
}

/// (dll name, bit, display name)
const DLLS: [(&str, u8, &str); 6] = [
    ("d3d12.dll", ApiSet::D3D12, "D3D12"),
    ("d3d11.dll", ApiSet::D3D11, "D3D11"),
    ("d3d9.dll", ApiSet::D3D9, "D3D9"),
    ("d3d8.dll", ApiSet::D3D8, "D3D8"),
    ("vulkan-1.dll", ApiSet::VULKAN, "Vulkan"),
    ("opengl32.dll", ApiSet::OPENGL, "OpenGL"),
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Architecture {
    X86,
    X64,
}

#[derive(Debug, Clone, Default)]
pub struct ExeScan {
    pub architecture: Option<Architecture>,
    /// APIs in the import or delay-load import tables.
    pub imported: ApiSet,
    /// APIs referenced through dynamic loading or game-local dependencies.
    pub referenced: ApiSet,
}

impl ExeScan {
    pub fn all(&self) -> ApiSet {
        ApiSet(self.imported.0 | self.referenced.0)
    }
}

pub fn scan_exe(path: &Path) -> io::Result<ExeScan> {
    Ok(scan_bytes(&map_binary(path)?))
}

fn map_binary(path: &Path) -> io::Result<memmap2::Mmap> {
    let file = File::open(path)?;
    // SAFETY: read-only mapping of a file we do not modify.
    unsafe { memmap2::Mmap::map(&file) }
}

/// Inspect the executable and its adjacent, architecture-matched PE dependencies.
pub(crate) fn scan_recomp(path: &Path) -> io::Result<ExeScan> {
    let (mut scan, mut pending) = scan_recomp_binary(&map_binary(path)?);
    let mut seen = HashSet::new();
    while let Some(name) = pending.pop() {
        let dll = Path::new(&name);
        if !dll.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("dll"))
            || dll.file_name() != Some(dll.as_os_str())
            || DLLS.iter().any(|(api, _, _)| name.eq_ignore_ascii_case(api))
            || !seen.insert(name.to_ascii_lowercase())
        {
            continue;
        }
        let Ok(data) = map_binary(&path.with_file_name(dll)) else { continue };
        let (dependency, imports) = scan_recomp_binary(&data);
        if dependency.architecture == scan.architecture {
            scan.referenced.0 |= dependency.all().0;
            pending.extend(imports);
        }
    }
    Ok(scan)
}

/// Returns the best image in the executable's first icon group as an ICO file.
pub fn executable_icon(path: &Path) -> Option<Vec<u8>> {
    let file = File::open(path).ok()?;
    // SAFETY: the file is mapped read-only and only parsed as data.
    let map = unsafe { memmap2::Mmap::map(&file).ok()? };
    if let Ok(pe) = PeFile64::parse(&map[..]) {
        return pe_icon(pe);
    }
    pe_icon(PeFile32::parse(&map[..]).ok()?)
}

fn resource_table<'a>(resources: ResourceDirectory<'a>, id: u16) -> Option<ResourceDirectoryTable<'a>> {
    resources.root().ok()?.entries.iter().find(|entry| entry.name_or_id().id() == Some(id))?
        .data(resources).ok()?.table()
}

fn pe_icon<'a, Pe: ImageNtHeaders>(pe: PeFile<'a, Pe>) -> Option<Vec<u8>> {
    const ICON: u16 = 3;
    const GROUP_ICON: u16 = 14;
    let sections = pe.section_table();
    let resources = pe.data_directories().resource_directory(pe.data(), &sections).ok()??;
    let data = |entry: &object::pe::ImageResourceDirectoryEntry| {
        let leaf = entry.data(resources).ok()?.table()?.entries.iter()
            .find_map(|language| language.data(resources).ok()?.data())?;
        let size = leaf.size.get(LE) as usize;
        sections.pe_data_at(pe.data(), leaf.offset_to_data.get(LE))?.get(..size)
    };
    let groups = resource_table(resources, GROUP_ICON)?;
    let group = groups.entries.iter().find_map(data)?;
    if group.get(..4)? != [0, 0, 1, 0] {
        return None;
    }
    let count = u16::from_le_bytes(group.get(4..6)?.try_into().ok()?) as usize;
    let dimensions = |value: u8| if value == 0 { 256 } else { usize::from(value) };
    let chosen = group.get(6..6 + count * 14)?.chunks_exact(14)
        .max_by_key(|entry| (dimensions(entry[0]) * dimensions(entry[1]), entry[6]))?;
    let id = u16::from_le_bytes([chosen[12], chosen[13]]);
    let icon = resource_table(resources, ICON)?.entries.iter()
        .find(|entry| entry.name_or_id().id() == Some(id)).and_then(data)?;
    if icon.len() > 1 << 20 {
        return None;
    }
    let mut ico = vec![0, 0, 1, 0, 1, 0];
    ico.extend_from_slice(&chosen[..8]);
    ico.extend_from_slice(&(icon.len() as u32).to_le_bytes());
    ico.extend_from_slice(&22_u32.to_le_bytes());
    ico.extend_from_slice(icon);
    Some(ico)
}

pub fn scan_bytes(data: &[u8]) -> ExeScan {
    let (mut scan, _) = scan_imports(data);
    for (dll, bit, _) in DLLS {
        if contains_ascii_ci(data, dll.as_bytes()) || contains_utf16_ci(data, dll.as_bytes()) {
            scan.referenced.0 |= bit;
        }
    }
    scan
}

fn scan_recomp_binary(data: &[u8]) -> (ExeScan, Vec<String>) {
    let (mut scan, imports) = scan_imports(data);
    if scan.architecture.is_none() {
        return (ExeScan::default(), Vec::new());
    }
    // A graphics entry point is stronger evidence than a DLL name in text.
    // Vulkan loaders can also be present in windowing libraries without a Vulkan renderer.
    for (symbol, bit) in [
        (b"D3D12CreateDevice\0".as_slice(), ApiSet::D3D12),
        (b"D3D11CreateDevice\0", ApiSet::D3D11),
        (b"D3D11CreateDeviceAndSwapChain\0", ApiSet::D3D11),
        (b"Direct3DCreate9\0", ApiSet::D3D9),
        (b"Direct3DCreate9Ex\0", ApiSet::D3D9),
        (b"Direct3DCreate8\0", ApiSet::D3D8),
        (b"vkCreateGraphicsPipelines\0", ApiSet::VULKAN),
        (b"glDrawElements\0", ApiSet::OPENGL),
    ] {
        if !scan.all().has(bit) && memmem::find(data, symbol).is_some() {
            scan.referenced.0 |= bit;
        }
    }
    (scan, imports)
}

fn scan_imports(data: &[u8]) -> (ExeScan, Vec<String>) {
    let mut scan = ExeScan::default();
    let imports = if let Ok(pe) = PeFile64::parse(data) {
        if pe.nt_headers().file_header().machine.get(LE) == object::pe::IMAGE_FILE_MACHINE_AMD64 {
            scan.architecture = Some(Architecture::X64);
        }
        imports(&pe)
    } else if let Ok(pe) = PeFile32::parse(data) {
        if pe.nt_headers().file_header().machine.get(LE) == object::pe::IMAGE_FILE_MACHINE_I386 {
            scan.architecture = Some(Architecture::X86);
        }
        imports(&pe)
    } else {
        Vec::new()
    };
    for name in &imports {
        for (dll, bit, _) in DLLS {
            if name.eq_ignore_ascii_case(dll) {
                scan.imported.0 |= bit;
            }
        }
    }
    (scan, imports)
}

fn imports<'d, Pe: ImageNtHeaders, R: ReadRef<'d>>(pe: &PeFile<'d, Pe, R>) -> Vec<String> {
    let mut out = Vec::new();
    if let Ok(Some(table)) = pe.import_table() {
        if let Ok(mut it) = table.descriptors() {
            while let Ok(Some(d)) = it.next() {
                if let Ok(n) = table.name(d.name.get(LE)) {
                    out.push(String::from_utf8_lossy(n).into_owned());
                }
            }
        }
    }
    if let Ok(Some(table)) = pe.delay_load_import_table() {
        if let Ok(mut it) = table.descriptors() {
            while let Ok(Some(d)) = it.next() {
                if let Ok(n) = table.name(d.dll_name_rva.get(LE)) {
                    out.push(String::from_utf8_lossy(n).into_owned());
                }
            }
        }
    }
    out
}

/// Case-insensitive search for a lowercase ASCII needle.
pub fn contains_ascii_ci(hay: &[u8], needle: &[u8]) -> bool {
    let (first, rest) = match needle.split_first() {
        Some(x) => x,
        None => return true,
    };
    let mut pos = 0;
    while let Some(off) = memchr2(*first, first.to_ascii_uppercase(), &hay[pos..]) {
        let start = pos + off;
        let tail = &hay[start + 1..];
        if tail.len() >= rest.len() && tail[..rest.len()].eq_ignore_ascii_case(rest) {
            return true;
        }
        pos = start + 1;
    }
    false
}

/// Case-insensitive search for a lowercase ASCII needle stored as UTF-16LE.
pub fn contains_utf16_ci(hay: &[u8], needle: &[u8]) -> bool {
    let (first, rest) = match needle.split_first() {
        Some(x) => x,
        None => return true,
    };
    let mut pos = 0;
    while let Some(off) = memchr2(*first, first.to_ascii_uppercase(), &hay[pos..]) {
        let start = pos + off;
        let tail = &hay[start..];
        if tail.len() >= needle.len() * 2
            && tail[1] == 0
            && rest.iter().enumerate().all(|(i, c)| {
                let lo = tail[2 + i * 2];
                tail[3 + i * 2] == 0 && lo.to_ascii_lowercase() == *c
            })
        {
            return true;
        }
        pos = start + 1;
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ascii_search_ignores_case() {
        assert!(contains_ascii_ci(b"xxD3D11.DLLyy", b"d3d11.dll"));
        assert!(!contains_ascii_ci(b"xxd3d11.dlyy", b"d3d11.dll"));
    }

    #[test]
    fn utf16_search_ignores_case() {
        let w: Vec<u8> = "..D3D12.dll.."
            .encode_utf16()
            .flat_map(|c| c.to_le_bytes())
            .collect();
        assert!(contains_utf16_ci(&w, b"d3d12.dll"));
        assert!(!contains_utf16_ci(&w, b"d3d11.dll"));
    }

    #[test]
    fn own_test_binary_parses_as_pe64() {
        let exe = std::env::current_exe().unwrap();
        let s = scan_exe(&exe).unwrap();
        assert_eq!(s.architecture, Some(Architecture::X64));
        assert!(s.imported.is_empty());
    }
}
