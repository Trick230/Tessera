// SPDX-License-Identifier: GPL-3.0-or-later
//! Oodle block decoding for the S1 scan, through the game's own codec DLL
//! (`oo2core_*_win64.dll`, shipped by Unreal games that compress their paks
//! with Oodle). Tessera never ships or copies it; it is loaded read-only from
//! the install and only ever decodes, with Oodle's fuzz-safe checks on.

use std::path::{Path, PathBuf};

/// Raw sizes tried for a block, most common first. Unreal cuts a pak entry
/// into blocks of one fixed size (64 KiB by default; 256 KiB in some games),
/// and Oodle needs the exact raw size, which a block does not record.
const RAW_SIZES: [usize; 3] = [256 << 10, 64 << 10, 128 << 10];

type DecompressFn = unsafe extern "C" fn(
    comp: *const u8,
    comp_len: isize,
    raw: *mut u8,
    raw_len: isize,
    fuzz_safe: i32,
    check_crc: i32,
    verbosity: i32,
    dec_base: *mut u8,
    dec_len: isize,
    callback: *const u8,
    user: *const u8,
    scratch: *mut u8,
    scratch_len: isize,
    thread_phase: i32,
) -> isize;

/// A loaded decoder. Copy: the DLL stays loaded for the life of the process.
#[derive(Clone, Copy)]
pub struct Oodle {
    decompress: DecompressFn,
}

/// The first `oo2core_*_win64.dll` under `root` (Unreal keeps it in
/// Engine/Binaries/ThirdParty/Oodle/Win64).
pub fn find(root: &Path) -> Option<PathBuf> {
    walkdir::WalkDir::new(root).into_iter().filter_map(Result::ok).map(|e| e.into_path()).find(|p| {
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("").to_ascii_lowercase();
        name.starts_with("oo2core_") && name.ends_with("_win64.dll")
    })
}

#[cfg(windows)]
pub fn load(dll: &Path) -> Option<Oodle> {
    use std::ffi::c_void;
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn LoadLibraryExW(name: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    }
    // Dependencies only from the DLL's own folder and System32 (needs an absolute path).
    const SEARCH_DLL_LOAD_DIR_AND_SYSTEM32: u32 = 0x100 | 0x800;
    let dll = std::path::absolute(dll).ok()?;
    let name: Vec<u16> = dll.as_os_str().encode_wide().chain(Some(0)).collect();
    // SAFETY: plain Win32 calls; the export has the public OodleLZ_Decompress
    // signature, which has been stable across oo2core versions 5 to 9.
    unsafe {
        let module = LoadLibraryExW(name.as_ptr(), std::ptr::null_mut(), SEARCH_DLL_LOAD_DIR_AND_SYSTEM32);
        if module.is_null() {
            return None;
        }
        let f = GetProcAddress(module, b"OodleLZ_Decompress\0".as_ptr());
        (!f.is_null()).then(|| Oodle { decompress: std::mem::transmute::<*mut c_void, DecompressFn>(f) })
    }
}

#[cfg(not(windows))]
pub fn load(_dll: &Path) -> Option<Oodle> {
    None
}

/// Length of the Kraken/Mermaid/Selkie/Leviathan/Hydra block that may start at
/// `b[0]`, from its header: a new-stream block (0x8C), a known codec, and
/// one compressed quantum whose size fits in `b`.
pub fn block_len(b: &[u8]) -> Option<usize> {
    if b.len() < 6 || b[0] != 0x8C || !known_codec(b[1]) {
        return None;
    }
    let h = usize::from(b[2]) << 16 | usize::from(b[3]) << 8 | usize::from(b[4]);
    if h & 0x3FFFF == 0x3FFFF {
        return None; // special quanta (memset, uncompressed): not worth a guess
    }
    let crc = if b[1] & 0x80 != 0 { 3 } else { 0 };
    let len = 5 + crc + (h & 0x3FFFF) + 1;
    (len <= b.len()).then_some(len)
}

/// Kraken (6), Mermaid/Selkie (10) and Leviathan (12): the codecs Unreal
/// uses, with 256 KiB quanta and fuzz-safe decoding. Legacy ones (BitKnit 11
/// and older) are left out.
fn known_codec(c: u8) -> bool {
    matches!(c & 0x7F, 0x06 | 0x0A | 0x0C)
}

/// Whether a block starts at `b[0]`: compressed or stored, starting a new
/// stream (0x8C, 0xCC, as in Unreal) or continuing one (0x0C, 0x4C).
fn is_block_start(b: &[u8]) -> bool {
    b.len() >= 2 && b[0] & 0x3F == 0x0C && known_codec(b[1])
}

/// Archives that align compressed blocks to 16 bytes (encrypted Unreal paks,
/// IoStore) leave up to 15 bytes of padding between them.
const MAX_GAP: usize = 16;

/// Length of the block at `b[0]` when it can start a decodable run: the
/// archive cut its entry into full-size blocks, so another block (the next
/// one, or the shorter last one) follows it directly. A lone block is either
/// shorter than the block size (raw size unknown, not decodable) or a rare
/// entry of exactly one block; skipping both rejects almost every false 0x8C
/// before the decoder is ever called.
pub fn run_start(b: &[u8]) -> Option<usize> {
    let len = block_len(b)?;
    (0..MAX_GAP).any(|g| b.get(len + g..).is_some_and(is_block_start)).then_some(len)
}

impl Oodle {
    /// Decodes the block at the start of `b` (of `len` bytes, from
    /// block_len), appending it to `out`. False when no raw size fits.
    pub fn block(&self, b: &[u8], len: usize, out: &mut Vec<u8>) -> bool {
        for raw in RAW_SIZES {
            // Decoded into spare capacity: no zero fill, which would cost far
            // more than a failed try on the many false candidates.
            // Slack past `raw`, for older decoders that may overrun on bad input.
            out.reserve(raw + 64);
            // SAFETY: both buffers are valid for the lengths passed; fuzz-safe
            // decoding (1) bounds every read and write to them, and the length
            // is only extended over bytes the decoder wrote (all `raw` of them).
            let r = unsafe {
                (self.decompress)(
                    b.as_ptr(),
                    len as isize,
                    out.as_mut_ptr().add(out.len()),
                    raw as isize,
                    1,
                    0,
                    0,
                    std::ptr::null_mut(),
                    0,
                    std::ptr::null(),
                    std::ptr::null(),
                    std::ptr::null_mut(),
                    0,
                    3,
                )
            };
            if r == raw as isize {
                // SAFETY: the decoder reported writing exactly `raw` bytes.
                unsafe { out.set_len(out.len() + raw) };
                return true;
            }
        }
        false
    }

    /// Decodes a run of blocks that follow each other (one pak entry), joined
    /// so a container across a block edge is whole, up to `cap` bytes.
    /// Returns the input consumed; 0 when the first block does not decode.
    pub fn run(&self, b: &[u8], out: &mut Vec<u8>, cap: usize) -> usize {
        let Some(len) = block_len(b) else { return 0 };
        if !self.block(b, len, out) {
            return 0;
        }
        let mut used = len;
        'run: while out.len() < cap {
            for q in used..(used + MAX_GAP).min(b.len()) {
                if let Some(len) = block_len(&b[q..]) {
                    if self.block(&b[q..], len, out) {
                        used = q + len;
                        continue 'run;
                    }
                }
                if b[q] != 0 {
                    break 'run; // padding is zeros: anything else ends the run
                }
            }
            break;
        }
        used
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn block_header() {
        // Kraken, compressed quantum of 0x1234 + 1 bytes.
        let mut b = vec![0x8C, 0x06, 0x00, 0x12, 0x34];
        b.resize(5 + 0x1235, 0);
        assert_eq!(block_len(&b), Some(5 + 0x1235));
        assert_eq!(block_len(&b[..100]), None, "quantum past the end");
        b[1] = 0x86; // with a CRC: 3 more bytes
        b.resize(8 + 0x1235, 0);
        assert_eq!(block_len(&b), Some(8 + 0x1235));
        assert_eq!(run_start(&b), None, "no block after it");
        b.extend_from_slice(&[0xCC, 0x86]);
        assert_eq!(run_start(&b), Some(8 + 0x1235));
        b.truncate(8 + 0x1235);
        b.extend_from_slice(&[0, 0, 0, 0x8C, 0x0A]); // after alignment padding
        assert_eq!(run_start(&b), Some(8 + 0x1235));
        b.truncate(8 + 0x1235);
        b[1] = 0x07; // not a known codec
        assert_eq!(block_len(&b), None);
        b[1] = 0x06;
        b[0] = 0xCC; // not a new-stream block
        assert_eq!(block_len(&b), None);
    }
}
