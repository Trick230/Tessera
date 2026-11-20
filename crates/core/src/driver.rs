// SPDX-License-Identifier: GPL-3.0-or-later
//! S7: driver shader cache check. Read-only: reports the GPU driver, the size
//! of its shader cache folders and (NVIDIA) the cache size limit, and notices
//! driver updates, after which every game recompiles its shaders.

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

    pub fn cache_limit() -> Option<NvCacheLimit> {
        let name: Vec<u16> = "nvapi64.dll\0".encode_utf16().collect();
        // SAFETY: plain Win32 / NVAPI C calls with buffers sized per the
        // NVAPI headers; the struct version lets the driver reject a mismatch.
        unsafe {
            let module = LoadLibraryW(name.as_ptr());
            if module.is_null() {
                return None;
            }
            let r = query(module);
            FreeLibrary(module);
            r
        }
    }

    unsafe fn query(module: *mut c_void) -> Option<NvCacheLimit> {
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
                let read = |id: u32| -> Result<Option<u32>, ()> {
                    let mut s = Box::new(Setting([0; SETTING_SIZE]));
                    let version = SETTING_SIZE as u32 | (1 << 16);
                    s.0[..4].copy_from_slice(&version.to_le_bytes());
                    match setting(session, profile, id, &mut *s) {
                        0 => Ok(Some(u32::from_le_bytes(
                            s.0[CURRENT_VALUE_OFFSET..CURRENT_VALUE_OFFSET + 4].try_into().unwrap(),
                        ))),
                        NVAPI_SETTING_NOT_FOUND => Ok(None),
                        _ => Err(()),
                    }
                };
                if read(SHADER_CACHE).ok()? == Some(0) {
                    return Some(NvCacheLimit::Disabled);
                }
                Some(match read(SHADER_CACHE_MAX_SIZE).ok()? {
                    None => NvCacheLimit::Default,
                    Some(0xFFFF_FFFF) => NvCacheLimit::Unlimited,
                    Some(0) => NvCacheLimit::Disabled,
                    Some(mb) => NvCacheLimit::Megabytes(mb),
                })
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
}
