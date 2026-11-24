// SPDX-License-Identifier: GPL-3.0-or-later
//! S7: driver shader cache check. Read-only: reports the GPU driver, the size
//! of its shader cache folders and (NVIDIA) the cache size limit, and notices
//! driver updates, after which every game recompiles its shaders.

use std::fs;
use std::io::{self, BufReader, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// AMD keeps at most this much shader cache; past it old entries are evicted.
pub const AMD_CACHE_LIMIT: u64 = 16 << 30;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Vendor {
    Nvidia,
    Amd,
    Intel,
    Other,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Gpu {
    pub name: String,
    pub vendor: Vendor,
    pub driver_version: String,
}

/// NVIDIA "Shader Cache Size" from the global driver profile.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NvCacheLimit {
    Unlimited,
    Disabled,
    /// Driver default (no explicit value set).
    Default,
    Megabytes(u32),
}

#[derive(Debug, Clone, PartialEq)]
pub struct Report {
    pub gpus: Vec<Gpu>,
    /// Shader cache folders that exist, with their size in bytes.
    pub caches: Vec<(PathBuf, u64)>,
    /// None when not NVIDIA or the driver could not be asked.
    pub nv_limit: Option<NvCacheLimit>,
    /// The primary driver version changed since the last check.
    pub driver_changed: bool,
}

impl Report {
    /// Advice for the user in Portuguese; empty when all is well.
    pub fn advice(&self) -> Vec<String> {
        let mut out = Vec::new();
        match self.nv_limit {
            Some(NvCacheLimit::Unlimited) | None => {}
            Some(NvCacheLimit::Disabled) => out.push(
                "The NVIDIA shader cache is off, so games recompile shaders on every start. \
                 NVIDIA Control Panel → Shader Cache Size → Unlimited."
                    .to_string(),
            ),
            Some(_) => out.push(
                "The NVIDIA shader cache has a size limit: with several big games, old shaders get \
                 evicted and compile again. NVIDIA Control Panel → Shader Cache Size → Unlimited."
                    .to_string(),
            ),
        }
        if self.gpus.iter().any(|g| g.vendor == Vendor::Amd) {
            let amd: u64 = self.caches.iter().filter(|(p, _)| is_amd_cache(p)).map(|(_, s)| s).sum();
            if amd > AMD_CACHE_LIMIT * 9 / 10 {
                out.push(format!(
                    "The AMD shader cache is close to its 16 GiB limit ({:.1} GiB); the oldest shaders \
                     will be dropped.",
                    amd as f64 / (1u64 << 30) as f64
                ));
            }
        }
        if self.driver_changed {
            out.push(
                "The graphics driver changed since last time: shader caches were reset and games \
                 will recompile shaders in their first sessions."
                    .to_string(),
            );
        }
        out
    }
}

fn is_amd_cache(p: &Path) -> bool {
    p.components().any(|c| c.as_os_str().eq_ignore_ascii_case("AMD"))
}

fn vendor_of(provider: &str, name: &str) -> Vendor {
    let s = format!("{provider} {name}").to_ascii_lowercase();
    if s.contains("nvidia") {
        Vendor::Nvidia
    } else if s.contains("amd") || s.contains("advanced micro devices") || s.contains("radeon") {
        Vendor::Amd
    } else if s.contains("intel") {
        Vendor::Intel
    } else {
        Vendor::Other
    }
}

/// Display adapters from the device class key, real GPUs first.
pub fn gpus() -> Vec<Gpu> {
    let hklm = winreg::RegKey::predef(winreg::enums::HKEY_LOCAL_MACHINE);
    let Ok(class) =
        hklm.open_subkey(r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}")
    else {
        return Vec::new();
    };
    let mut out: Vec<Gpu> = class
        .enum_keys()
        .flatten()
        .filter(|k| k.len() == 4 && k.bytes().all(|b| b.is_ascii_digit()))
        .filter_map(|k| class.open_subkey(&k).ok())
        .filter_map(|k| {
            let name: String = k.get_value("DriverDesc").ok()?;
            let driver_version: String = k.get_value("DriverVersion").ok()?;
            let provider: String = k.get_value("ProviderName").unwrap_or_default();
            Some(Gpu { vendor: vendor_of(&provider, &name), name, driver_version })
        })
        .collect();
    out.sort_by_key(|g| g.vendor == Vendor::Other);
    out.dedup();
    out
}

/// Dedicated memory of the largest display adapter in MB (0 when unknown),
/// from the driver's HardwareInformation.qwMemorySize.
pub fn vram_mb() -> u32 {
    let hklm = winreg::RegKey::predef(winreg::enums::HKEY_LOCAL_MACHINE);
    let Ok(class) =
        hklm.open_subkey(r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}")
    else {
        return 0;
    };
    class
        .enum_keys()
        .flatten()
        .filter(|k| k.len() == 4 && k.bytes().all(|b| b.is_ascii_digit()))
        .filter_map(|k| class.open_subkey(&k).ok())
        .filter_map(|k| k.get_raw_value("HardwareInformation.qwMemorySize").ok())
        .filter_map(|v| v.bytes.get(..8).map(|b| u64::from_le_bytes(b.try_into().unwrap())))
        .map(|b| (b >> 20).min(u32::MAX as u64) as u32)
        .max()
        .unwrap_or(0)
}

fn dir_size(p: &Path) -> u64 {
    walkdir::WalkDir::new(p)
        .into_iter()
        .flatten()
        .filter_map(|e| e.metadata().ok())
        .filter(|m| m.is_file())
        .map(|m| m.len())
        .sum()
}

/// Known shader cache folders under `%LOCALAPPDATA%` (and LocalLow for NVIDIA).
pub fn cache_dirs() -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Some(local) = std::env::var_os("LOCALAPPDATA").map(PathBuf::from) {
        for sub in [r"NVIDIA\DXCache", r"NVIDIA\GLCache", r"AMD\DxCache", r"AMD\DxcCache", r"AMD\VkCache", "D3DSCache"] {
            out.push(local.join(sub));
        }
        if let Some(parent) = local.parent() {
            out.push(parent.join(r"LocalLow\NVIDIA\PerDriverVersion\DXCache"));
        }
    }
    out.into_iter().filter(|p| p.is_dir()).collect()
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct NvidiaGameCache {
    pub files: u64,
    /// Used bytes in the matched files, read from their headers.
    pub bytes: u64,
    /// Driver cache entries in files whose record stream was fully parsed.
    pub entries: u64,
    pub counted_files: u64,
}

/// NVIDIA DXCache files grouped by the executable name in their headers. Built
/// once per library refresh: each file header is read once, not once per game.
#[derive(Debug, Default)]
pub struct NvidiaCacheIndex {
    files: std::collections::HashMap<Vec<u8>, Vec<NvidiaCacheFile>>,
}

#[derive(Debug)]
struct NvidiaCacheFile {
    path: PathBuf,
    bytes: u64,
    countable: bool,
}

/// Indexes the NVIDIA DXCache, or None without an NVIDIA GPU or cache folder.
pub fn nvidia_cache_index() -> Option<NvidiaCacheIndex> {
    if !gpus().iter().any(|g| g.vendor == Vendor::Nvidia) {
        return None;
    }
    nvidia_cache_index_at(&nvidia_cache_dir()?)
}

fn nvidia_cache_dir() -> Option<PathBuf> {
    Some(PathBuf::from(std::env::var_os("LOCALAPPDATA")?).join("NVIDIA/DXCache"))
}

fn nvidia_cache_file(entry: &fs::DirEntry) -> Option<(fs::File, Vec<u8>, u64)> {
    let path = entry.path();
    if !path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("nvph"))
        || !entry.file_type().ok()?.is_file()
    {
        return None;
    }
    let file = fs::File::open(path).ok()?;
    let len = file.metadata().ok()?.len();
    let mut header = Vec::with_capacity(0x111);
    (&file).take(0x111).read_to_end(&mut header).ok()?;
    Some((file, header, len))
}

fn nvidia_cache_index_at(dir: &Path) -> Option<NvidiaCacheIndex> {
    let mut index = NvidiaCacheIndex::default();
    for entry in fs::read_dir(dir).ok()?.flatten() {
        let Some((_, header, len)) = nvidia_cache_file(&entry) else { continue };
        let Some((name, bytes)) = nvidia_cache_header(&header, len) else { continue };
        index.files.entry(name.to_ascii_lowercase()).or_default()
            .push(NvidiaCacheFile { path: entry.path(), bytes, countable: header[0x10] == 1 });
    }
    Some(index)
}

impl NvidiaCacheIndex {
    /// Counts entries in supported files naming the executable; only these files' records are read.
    pub fn game(&self, exe: &Path) -> Option<NvidiaGameCache> {
        let name = exe.file_name()?.to_str()?.to_ascii_lowercase();
        let mut cache = NvidiaGameCache::default();
        for file in self.files.get(name.as_bytes()).into_iter().flatten() {
            cache.files += 1;
            cache.bytes += file.bytes;
            if !file.countable { continue }
            let Ok(mut handle) = fs::File::open(&file.path) else { continue };
            if handle.seek(SeekFrom::Start(0x111)).is_err() { continue }
            let mut reader = BufReader::with_capacity(64 * 1024, handle);
            if let Some(entries) = nvidia_cache_entries(&mut reader, file.bytes) {
                cache.entries += entries;
                cache.counted_files += 1;
            }
        }
        Some(cache)
    }
}

#[cfg(test)]
fn nvidia_game_cache_at(dir: &Path, exe_name: &str) -> Option<NvidiaGameCache> {
    nvidia_cache_index_at(dir)?.game(Path::new(exe_name))
}

/// Clears NVIDIA DXCache files whose header identifies this executable name.
pub(crate) fn clear_nvidia_game_cache(exe: &Path) -> io::Result<usize> {
    let Some(dir) = nvidia_cache_dir() else { return Ok(0) };
    let name = exe.file_name().and_then(|name| name.to_str())
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid executable name"))?;
    clear_nvidia_game_cache_at(&dir, name)
}

fn clear_nvidia_game_cache_at(dir: &Path, exe_name: &str) -> io::Result<usize> {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(0),
        Err(e) => return Err(e),
    };
    let mut removed = 0;
    for entry in entries {
        let entry = entry?;
        let Some((_file, header, len)) = nvidia_cache_file(&entry) else { continue };
        if nvidia_cache_header(&header, len).is_some_and(|(name, _)| name.eq_ignore_ascii_case(exe_name.as_bytes())) {
            let path = entry.path();
            match fs::remove_file(&path) {
                Ok(()) => removed += 1,
                Err(e) if e.kind() == io::ErrorKind::NotFound => (),
                Err(e) => return Err(io::Error::new(e.kind(), format!("{}: {e}", path.display()))),
            }
        }
    }
    Ok(removed)
}

fn nvidia_cache_header(header: &[u8], file_len: u64) -> Option<(&[u8], u64)> {
    if header.get(..4)? != b"nvph" {
        return None;
    }
    let used = u64::from_le_bytes(header.get(8..16)?.try_into().ok()?);
    let name_field = header.get(0x11..)?;
    let name = &name_field[..name_field.iter().position(|&b| b == 0)?];
    (used >= 0x12 && used <= file_len && !name.is_empty()).then_some((name, used))
}

/// Observed named NVPH layout on Windows. Each `phwr` record wraps a
/// `0x1234` v12/v13 object and a `0x5678` compressed payload. Count the
/// records, including driver variants, without decompressing their contents.
fn nvidia_cache_entries(reader: &mut BufReader<std::fs::File>, used: u64) -> Option<u64> {
    const RECORDS_START: u64 = 0x5d0;
    const PREFIX_LEN: u64 = 0x90;
    let mut remaining = used.checked_sub(RECORDS_START)?;
    reader.seek_relative(RECORDS_START as i64 - 0x111).ok()?;
    let mut count = 0;
    let mut prefix = [0; PREFIX_LEN as usize];
    let mut magic = [0; 4];
    while remaining != 0 {
        if remaining < PREFIX_LEN + 4 {
            return None;
        }
        reader.read_exact(&mut prefix).ok()?;
        let field = |offset| u64::from_le_bytes(prefix[offset..offset + 8].try_into().unwrap());
        let body_len = field(0x08);
        let metadata_len = field(0x70);
        let compressed_len = field(0x78);
        let payload_len = body_len.checked_sub(PREFIX_LEN - 0x20)?;
        if &prefix[..8] != b"phwr\0\0\0\0"
            || !matches!(field(0x20), 0x0c1234 | 0x0d1234)
            || field(0x28) != 0x40
            || field(0x30) != body_len - 0x40
            || field(0x60) != 0x5678
            || field(0x68) != 0x30
            || field(0x88) != 2
            || compressed_len < 4
            || payload_len.checked_sub(metadata_len)? != compressed_len
            || body_len > remaining - 0x20
        {
            return None;
        }
        reader.seek_relative(i64::try_from(metadata_len).ok()?).ok()?;
        reader.read_exact(&mut magic).ok()?;
        if magic != [0x28, 0xb5, 0x2f, 0xfd] {
            return None;
        }
        reader.seek_relative(i64::try_from(compressed_len - 4).ok()?).ok()?;
        remaining -= 0x20 + body_len;
        count += 1;
    }
    // The driver can append while the app is reading the cache.
    reader.seek(SeekFrom::Start(8)).ok()?;
    let mut current_used = [0; 8];
    reader.read_exact(&mut current_used).ok()?;
    (u64::from_le_bytes(current_used) == used).then_some(count)
}

#[cfg(windows)]
mod nvapi {
    //! The NVIDIA cache limit lives in the driver profile database, reachable
    //! only through NVAPI's DRS calls. Read-only; any failure returns None.
    use super::NvCacheLimit;
    use std::ffi::c_void;

    #[link(name = "kernel32")]
    extern "system" {
        fn LoadLibraryW(name: *const u16) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
        fn FreeLibrary(module: *mut c_void) -> i32;
    }

    // Function ids from the public NVAPI interface table.
    const INITIALIZE: u32 = 0x0150_E828;
    const UNLOAD: u32 = 0xD22B_DD7E;
    const DRS_CREATE_SESSION: u32 = 0x0694_D52E;
    const DRS_DESTROY_SESSION: u32 = 0xDAD9_CFF8;
    const DRS_LOAD_SETTINGS: u32 = 0x375D_BD6B;
    const DRS_GET_BASE_PROFILE: u32 = 0xDA84_66A0;
    const DRS_GET_SETTING: u32 = 0x73BF_8338;
    const DRS_FIND_APPLICATION_BY_NAME: u32 = 0xEEE5_66B2;

    /// FRL_FPS_ID: the control panel's "Max Frame Rate", fps, 0 = off.
    const FRAME_RATE_LIMIT: u32 = 0x1083_5002;
    /// NVDRS_APPLICATION_V1: version, isPredefined, appName, userFriendlyName,
    /// launcher (three 2048-u16 strings).
    const APPLICATION_SIZE: usize = 8 + 3 * 4096;

    /// PS_SHADERDISKCACHE_MAX_SIZE_ID (value in MB).
    const SHADER_CACHE_MAX_SIZE: u32 = 0x00AC_8497;
    /// PS_SHADERDISKCACHE_ID: 0 = off, 1 = on.
    const SHADER_CACHE: u32 = 0x0019_8FFF;
    const NVAPI_SETTING_NOT_FOUND: i32 = -160;

    /// NVDRS_SETTING_V1: version, name[2048] (u16), id, type, location,
    /// isCurrentPredefined, isPredefinedValid, predefined value (4100 bytes),
    /// current value (4100 bytes).
    const SETTING_SIZE: usize = 12320;
    const CURRENT_VALUE_OFFSET: usize = 4 + 4096 + 5 * 4 + 4100;

    #[repr(C, align(8))]
    struct Setting([u8; SETTING_SIZE]);

    type QueryInterface = unsafe extern "C" fn(u32) -> *mut c_void;
    type Fn0 = unsafe extern "C" fn() -> i32;
    type CreateSession = unsafe extern "C" fn(*mut *mut c_void) -> i32;
    type SessionFn = unsafe extern "C" fn(*mut c_void) -> i32;
    type GetBaseProfile = unsafe extern "C" fn(*mut c_void, *mut *mut c_void) -> i32;
    type GetSetting = unsafe extern "C" fn(*mut c_void, *mut c_void, u32, *mut Setting) -> i32;
    type FindApplication = unsafe extern "C" fn(*mut c_void, *const u16, *mut *mut c_void, *mut u8) -> i32;

    /// A loaded DRS session: settings read from a profile.
    struct Drs {
        session: *mut c_void,
        base: *mut c_void,
        setting: GetSetting,
        find: Option<FindApplication>,
    }

    impl Drs {
        /// Current value of a DWORD setting; Ok(None) when the profile does not set it.
        unsafe fn read(&self, profile: *mut c_void, id: u32) -> Result<Option<u32>, ()> {
            let mut s = Box::new(Setting([0; SETTING_SIZE]));
            let version = SETTING_SIZE as u32 | (1 << 16);
            s.0[..4].copy_from_slice(&version.to_le_bytes());
            match (self.setting)(self.session, profile, id, &mut *s) {
                0 => Ok(Some(u32::from_le_bytes(
                    s.0[CURRENT_VALUE_OFFSET..CURRENT_VALUE_OFFSET + 4].try_into().unwrap(),
                ))),
                NVAPI_SETTING_NOT_FOUND => Ok(None),
                _ => Err(()),
            }
        }

        /// The profile the driver applies to this executable, if any.
        unsafe fn app_profile(&self, exe: &str) -> Option<*mut c_void> {
            let find = self.find?;
            let mut name = [0u16; 2048];
            for (d, c) in name.iter_mut().take(2047).zip(exe.encode_utf16()) {
                *d = c;
            }
            let mut app = vec![0u8; APPLICATION_SIZE];
            let version = APPLICATION_SIZE as u32 | (1 << 16);
            app[..4].copy_from_slice(&version.to_le_bytes());
            let mut profile = std::ptr::null_mut();
            (find(self.session, name.as_ptr(), &mut profile, app.as_mut_ptr()) == 0 && !profile.is_null())
                .then_some(profile)
        }
    }

    pub fn cache_limit() -> Option<NvCacheLimit> {
        with_drs(|d| unsafe {
            if d.read(d.base, SHADER_CACHE).ok()? == Some(0) {
                return Some(NvCacheLimit::Disabled);
            }
            Some(match d.read(d.base, SHADER_CACHE_MAX_SIZE).ok()? {
                None => NvCacheLimit::Default,
                Some(0xFFFF_FFFF) => NvCacheLimit::Unlimited,
                Some(0) => NvCacheLimit::Disabled,
                Some(mb) => NvCacheLimit::Megabytes(mb),
            })
        })
    }

    /// The control panel's Max Frame Rate for this executable (its own
    /// profile first, then the global one); None when off or unreadable.
    pub fn frame_limit(exe: &str) -> Option<u32> {
        with_drs(|d| unsafe {
            let app = d.app_profile(exe).and_then(|p| d.read(p, FRAME_RATE_LIMIT).ok().flatten());
            let v = match app {
                Some(v) => v,
                None => d.read(d.base, FRAME_RATE_LIMIT).ok().flatten()?,
            };
            (v > 0).then_some(v)
        })
    }

    fn with_drs<T>(f: impl FnOnce(&Drs) -> Option<T>) -> Option<T> {
        let name: Vec<u16> = "nvapi64.dll\0".encode_utf16().collect();
        // SAFETY: plain Win32 / NVAPI C calls with buffers sized per the
        // NVAPI headers; the struct version lets the driver reject a mismatch.
        unsafe {
            let module = LoadLibraryW(name.as_ptr());
            if module.is_null() {
                return None;
            }
            let r = query(module, f);
            FreeLibrary(module);
            r
        }
    }

    unsafe fn query<T>(module: *mut c_void, f: impl FnOnce(&Drs) -> Option<T>) -> Option<T> {
        let qi = GetProcAddress(module, b"nvapi_QueryInterface\0".as_ptr());
        if qi.is_null() {
            return None;
        }
        let qi: QueryInterface = std::mem::transmute(qi);
        let get = |id: u32| {
            let p = qi(id);
            (!p.is_null()).then_some(p)
        };
        let init: Fn0 = std::mem::transmute(get(INITIALIZE)?);
        let unload: Option<Fn0> = get(UNLOAD).map(|p| std::mem::transmute(p));
        if init() != 0 {
            return None;
        }
        let r = (|| {
            let create: CreateSession = std::mem::transmute(get(DRS_CREATE_SESSION)?);
            let destroy: SessionFn = std::mem::transmute(get(DRS_DESTROY_SESSION)?);
            let load: SessionFn = std::mem::transmute(get(DRS_LOAD_SETTINGS)?);
            let base: GetBaseProfile = std::mem::transmute(get(DRS_GET_BASE_PROFILE)?);
            let setting: GetSetting = std::mem::transmute(get(DRS_GET_SETTING)?);
            let find: Option<FindApplication> = get(DRS_FIND_APPLICATION_BY_NAME).map(|p| std::mem::transmute(p));
            let mut session = std::ptr::null_mut();
            if create(&mut session) != 0 {
                return None;
            }
            let r = (|| {
                if load(session) != 0 {
                    return None;
                }
                let mut profile = std::ptr::null_mut();
                if base(session, &mut profile) != 0 {
                    return None;
                }
                f(&Drs { session, base: profile, setting, find })
            })();
            destroy(session);
            r
        })();
        if let Some(u) = unload {
            u();
        }
        r
    }
}

/// NVIDIA control panel Max Frame Rate for this executable, None when off.
pub fn nvidia_frame_limit(exe_name: &str) -> Option<u32> {
    if !gpus().iter().any(|g| g.vendor == Vendor::Nvidia) {
        return None;
    }
    nvapi::frame_limit(exe_name)
}

/// "32.0.15.6094" as the 64-bit UMD version D3D reports (16 bits a part),
/// the form the warm-up logs record.
pub fn umd_version(s: &str) -> Option<u64> {
    let parts: Vec<u16> = s.trim().split('.').map(|p| p.parse().ok()).collect::<Option<_>>()?;
    let [a, b, c, d] = parts[..] else { return None };
    Some((a as u64) << 48 | (b as u64) << 32 | (c as u64) << 16 | d as u64)
}

/// Remembers the primary driver version in `<data>\driver.txt` and says
/// whether it differs from the stored one (false on the first check).
pub fn note_driver(data_dir: &Path, version: &str) -> bool {
    let file = data_dir.join("driver.txt");
    let old = std::fs::read_to_string(&file).ok();
    let changed = old.as_deref().is_some_and(|o| o.trim() != version);
    if old.as_deref().map(str::trim) != Some(version) {
        let _ = std::fs::create_dir_all(data_dir);
        let _ = std::fs::write(&file, version);
    }
    changed
}

/// Full S7 check. Cheap enough for app start (folder sizes are a directory walk).
pub fn check() -> Report {
    let gpus = gpus();
    let caches = cache_dirs().into_iter().map(|p| {
        let s = dir_size(&p);
        (p, s)
    });
    let nv_limit = if gpus.iter().any(|g| g.vendor == Vendor::Nvidia) { nvapi::cache_limit() } else { None };
    let driver_changed =
        gpus.first().is_some_and(|g| note_driver(&crate::paths::data_dir(), &g.driver_version));
    Report { gpus, caches: caches.collect(), nv_limit, driver_changed }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vendor_detection() {
        assert_eq!(vendor_of("NVIDIA", "NVIDIA GeForce RTX 4060"), Vendor::Nvidia);
        assert_eq!(vendor_of("Advanced Micro Devices, Inc.", "AMD Radeon RX 7600"), Vendor::Amd);
        assert_eq!(vendor_of("Intel Corporation", "Intel(R) Arc(TM) A750"), Vendor::Intel);
        assert_eq!(vendor_of("Microsoft", "Microsoft Basic Display Adapter"), Vendor::Other);
    }

    #[test]
    fn umd_version_matches_d3d() {
        assert_eq!(umd_version("32.0.16.2114"), Some(0x0020_0000_0010_0842));
        assert_eq!(umd_version("32.0.16"), None);
        assert_eq!(umd_version("32.0.x.1"), None);
    }

    #[test]
    fn driver_change_is_noticed_once() {
        let dir = std::env::temp_dir().join(format!("tessera-driver-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        assert!(!note_driver(&dir, "32.0.15.6094")); // first check: nothing to compare
        assert!(!note_driver(&dir, "32.0.15.6094"));
        assert!(note_driver(&dir, "32.0.15.6614"));
        assert!(!note_driver(&dir, "32.0.15.6614"));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn advice_for_limited_nvidia_cache() {
        let r = Report {
            gpus: vec![],
            caches: vec![],
            nv_limit: Some(NvCacheLimit::Megabytes(4096)),
            driver_changed: false,
        };
        assert_eq!(r.advice().len(), 1);
        let ok = Report { nv_limit: Some(NvCacheLimit::Unlimited), ..r };
        assert!(ok.advice().is_empty());
    }

    #[test]
    fn nvidia_cache_matches_executable_names_and_reports_used_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let write_cache = |file, name: &[u8], used: u64| {
            let mut data = [0; 128];
            data[..4].copy_from_slice(b"nvph");
            data[8..16].copy_from_slice(&used.to_le_bytes());
            data[0x11..0x11 + name.len()].copy_from_slice(name);
            std::fs::write(dir.path().join(file), data).unwrap();
        };
        write_cache("a.nvph", b"FF7REMAKE_.EXE", 80);
        write_cache("b.nvph", b"ff7remake_.exe.bak", 100);
        assert_eq!(
            nvidia_game_cache_at(dir.path(), "ff7remake_.exe"),
            Some(NvidiaGameCache { files: 1, bytes: 80, ..NvidiaGameCache::default() }),
        );
        assert_eq!(nvidia_game_cache_at(dir.path(), "other.exe"), Some(NvidiaGameCache::default()));
        write_cache("c.nvph", b"other.exe", 129);
        assert_eq!(nvidia_game_cache_at(dir.path(), "other.exe"), Some(NvidiaGameCache::default()));
        assert_eq!(
            nvidia_game_cache_at(dir.path(), "ff7remake_.exe"),
            Some(NvidiaGameCache { files: 1, bytes: 80, ..NvidiaGameCache::default() }),
        );
        assert_eq!(clear_nvidia_game_cache_at(dir.path(), "ff7remake_.exe").unwrap(), 1);
        assert_eq!(nvidia_game_cache_at(dir.path(), "ff7remake_.exe"), Some(NvidiaGameCache::default()));
        assert!(dir.path().join("b.nvph").is_file());
        assert!(dir.path().join("c.nvph").is_file());
        assert_eq!(clear_nvidia_game_cache_at(dir.path(), "ff7remake_.exe").unwrap(), 0);
    }

    #[test]
    fn nvidia_cache_counts_only_complete_record_streams() {
        let dir = tempfile::tempdir().unwrap();
        let mut data = vec![0; 0x5d0];
        data[..4].copy_from_slice(b"nvph");
        data[0x10] = 1;
        data[0x11..0x19].copy_from_slice(b"game.exe");
        for (version, metadata_len) in [(0x0c1234u64, 1u64), (0x0d1234, 8)] {
            let body_len = 0x70 + metadata_len + 4;
            let mut record = vec![0; 0x20 + body_len as usize];
            record[..4].copy_from_slice(b"phwr");
            for (offset, value) in [
                (0x08, body_len), (0x20, version), (0x28, 0x40), (0x30, body_len - 0x40),
                (0x60, 0x5678), (0x68, 0x30), (0x70, metadata_len), (0x78, 4), (0x88, 2),
            ] {
                record[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
            }
            record[0x90 + metadata_len as usize..].copy_from_slice(&[0x28, 0xb5, 0x2f, 0xfd]);
            data.extend(record);
        }
        let used = data.len() as u64;
        data[8..16].copy_from_slice(&used.to_le_bytes());
        std::fs::write(dir.path().join("complete.nvph"), &data).unwrap();

        let truncated = used - 1;
        data[8..16].copy_from_slice(&truncated.to_le_bytes());
        std::fs::write(dir.path().join("truncated.nvph"), data).unwrap();
        assert_eq!(
            nvidia_game_cache_at(dir.path(), "GAME.EXE"),
            Some(NvidiaGameCache { files: 2, bytes: used + truncated, entries: 2, counted_files: 1 }),
        );
        assert_eq!(clear_nvidia_game_cache_at(dir.path(), "game.exe").unwrap(), 2);
        assert_eq!(nvidia_game_cache_at(dir.path(), "game.exe"), Some(NvidiaGameCache::default()));
    }
}
