// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;
use std::sync::OnceLock;

use tessera_core::{driver, install, quality, streams};

use crate::AdvancedOptions;

#[derive(Clone, Copy)]
pub struct Facts {
    pub tier: i32, // -1 until a D3D11 device has been recorded
    pub priority_blocked: bool,
    pub hybrid: bool,
    pub trace: bool,
    pub compressor: bool,
    pub quality: quality::Status,
    pub root_levels: Option<u32>,
}

impl Facts {
    pub fn read(
        exe: &Path,
        dir: &Path,
        root: Option<&Path>,
        tier: i32,
        gpus: &[driver::Gpu],
    ) -> Self {
        static HYBRID: OnceLock<bool> = OnceLock::new();
        Self {
            tier,
            priority_blocked: gpus.iter().any(|gpu| {
                gpu.vendor == driver::Vendor::Nvidia
                    && matches!(gpu.driver_version.as_str(), "32.0.16.2114" | "621.14")
            }),
            hybrid: *HYBRID.get_or_init(has_hybrid_cpu),
            trace: streams::has_trace(exe),
            compressor: crate::app_dir().join(quality::TEXCOMP_EXE).is_file(),
            quality: quality::status(exe),
            root_levels: root.and_then(|p| install::stream_root_levels(dir, p)),
        }
    }

    fn tiled(self) -> bool {
        self.tier < 0 || self.tier >= 2
    }

    pub fn permits(self, key: &str, value: u32, current: install::AdvancedSettings) -> bool {
        if value == 0 {
            return true;
        }
        match key {
            "present_cpu_sets" => self.hybrid,
            "mip_residency" => self.tiled(),
            "mip_streaming" => self.tiled() && current.mip_residency,
            "stream_io" if value == 2 => self.trace,
            "texture_quality" if value == 2 => self.quality.approved != 0,
            "mip_feedback" => self.quality.shaders != 0,
            _ => true,
        }
    }

    pub fn view(self, current: install::AdvancedSettings) -> AdvancedOptions {
        let status = self.quality;
        AdvancedOptions {
            max_frame_latency: current.max_frame_latency as i32,
            present_cpu_sets: current.present_cpu_sets,
            hybrid: self.hybrid,
            mip_residency: current.mip_residency,
            mip_prediction: current.mip_prediction,
            mip_streaming: current.mip_streaming,
            tiled: self.tiled(),
            tiled_hint: match self.tier {
                -1 => "Needs tiled resources Tier 2. Play once to check support.".to_owned(),
                0 => "The last run reported no tiled resources support.".to_owned(),
                tier if tier >= 2 => {
                    format!("Tier {tier} detected. Moves idle top mips to RAM under VRAM pressure.")
                }
                tier => format!("Tier {tier} detected. Moving idle mips needs Tier 2."),
            }
            .into(),
            texture_dedup: current.texture_dedup,
            stream_io: current.stream_io as i32,
            trace: self.trace,
            texture_quality: current.texture_quality as i32,
            quality_ready: status.approved > 0,
            quality_status: format!(
                "{} captured, {} approved.",
                status.captured, status.approved
            )
            .into(),
            compressor: self.compressor,
            mip_feedback: current.mip_feedback,
            feedback_ready: status.shaders != 0,
            prepared_shaders: status.shaders.min(i32::MAX as u32) as i32,
        }
    }
}

fn has_hybrid_cpu() -> bool {
    use std::{mem::size_of, ptr::null_mut};
    use windows_sys::Win32::System::SystemInformation::{
        CpuSetInformation, GetSystemCpuSetInformation, SYSTEM_CPU_SET_INFORMATION,
    };
    let mut size = 0u32;
    // Null output asks Windows for the size. The storage is aligned and each
    // returned record includes its own size.
    unsafe { GetSystemCpuSetInformation(null_mut(), 0, &mut size, null_mut(), 0) };
    if size == 0 || size > 1024 * 1024 {
        return false;
    }
    let mut buffer = vec![0u64; (size as usize).div_ceil(size_of::<u64>())];
    let ptr = buffer.as_mut_ptr().cast::<u8>();
    let mut used = 0;
    if unsafe { GetSystemCpuSetInformation(ptr.cast(), size, &mut used, null_mut(), 0) } == 0
        || used > size
    {
        return false;
    }
    let used = used as usize;
    let mut offset = 0;
    let mut first = None;
    while used - offset >= size_of::<SYSTEM_CPU_SET_INFORMATION>() {
        let set = unsafe {
            ptr.add(offset)
                .cast::<SYSTEM_CPU_SET_INFORMATION>()
                .read_unaligned()
        };
        let length = set.Size as usize;
        if length < size_of::<SYSTEM_CPU_SET_INFORMATION>() || length > used - offset {
            break;
        }
        if set.Type == CpuSetInformation {
            let class = unsafe { set.Anonymous.CpuSet.EfficiencyClass };
            if let Some(initial) = first {
                if initial != class {
                    return true;
                }
            } else {
                first = Some(class);
            }
        }
        offset += length;
    }
    false
}
