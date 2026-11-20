// SPDX-License-Identifier: GPL-3.0-or-later
//! Graphics API detection from a PE executable: static and delay-load
//! imports, plus DLL names referenced as strings (LoadLibrary paths).

use memchr::memchr2;
use object::read::pe::{ImageNtHeaders, PeFile, PeFile32, PeFile64};
use object::{LittleEndian as LE, ReadRef};
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
const DLLS: [(&str, u8, &str); 5] = [
    ("d3d12.dll", ApiSet::D3D12, "D3D12"),
    ("d3d11.dll", ApiSet::D3D11, "D3D11"),
    ("d3d9.dll", ApiSet::D3D9, "D3D9"),
    ("vulkan-1.dll", ApiSet::VULKAN, "Vulkan"),
    ("opengl32.dll", ApiSet::OPENGL, "OpenGL"),
];

#[derive(Debug, Clone, Default)]
pub struct ExeScan {
    pub is_64bit: bool,
    /// APIs in the import or delay-load import tables.
    pub imported: ApiSet,
    /// APIs whose DLL name appears as an ASCII or UTF-16 string.
    pub referenced: ApiSet,
}

impl ExeScan {
    pub fn all(&self) -> ApiSet {
        ApiSet(self.imported.0 | self.referenced.0)
    }
}

pub fn scan_exe(path: &Path) -> io::Result<ExeScan> {
    let file = File::open(path)?;
    // SAFETY: read-only mapping of a file we do not modify.
    let map = unsafe { memmap2::Mmap::map(&file)? };
    Ok(scan_bytes(&map))
}

pub fn scan_bytes(data: &[u8]) -> ExeScan {
    let mut scan = ExeScan::default();
    let imports = if let Ok(pe) = PeFile64::parse(data) {
        scan.is_64bit = true;
        imports(&pe)
    } else if let Ok(pe) = PeFile32::parse(data) {
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
    for (dll, bit, _) in DLLS {
        if contains_ascii_ci(data, dll.as_bytes()) || contains_utf16_ci(data, dll.as_bytes()) {
            scan.referenced.0 |= bit;
        }
    }
    scan
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
        assert!(s.is_64bit);
        assert!(s.imported.is_empty());
    }
}
