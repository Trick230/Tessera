// SPDX-License-Identifier: GPL-3.0-or-later
//! Prepare pipelines through game engine precompilers.

use std::fs::{self, File};
use std::io;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

use crate::scan::{self, ShaderApi};
use crate::warm::{self, with_suffix};

#[derive(Clone, Copy)]
pub enum NativePrecompiler {
    SavageWind,
}

#[derive(Debug, PartialEq, Eq)]
pub struct NativeStats {
    pub shaders: u64,
    pub translations: u64,
    pub failed: u64,
    pub pipelines: Option<PipelineStats>,
}

impl std::fmt::Display for NativeStats {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} shaders, {} translations, {} translation failures", self.shaders, self.translations, self.failed)?;
        if let Some(stats) = &self.pipelines {
            write!(f, ". Pipeline list: {} of {} built, {} skipped", stats.prepared, stats.total, stats.failed)
        } else {
            write!(f, ". Pipeline list completion unconfirmed")
        }
    }
}

impl NativePrecompiler {
    pub fn detect(exe: &Path, api: ShaderApi) -> Option<Self> {
        if api != ShaderApi::Vulkan || !exe.file_name()?.eq_ignore_ascii_case("savagewind.exe")
            || !exe.parent()?.join("extracted/romfs").is_dir()
        {
            return None;
        }
        let file = File::open(exe).ok()?;
        let image = unsafe { memmap2::Mmap::map(&file) }.ok()?;
        memchr::memmem::find(&image, b"--precompile-shaders\0").map(|_| Self::SavageWind)
    }

    pub fn run(self, exe: &Path) -> io::Result<NativeStats> {
        let report = self.report_path(exe).ok_or_else(|| io::Error::other("invalid game path"))?;
        let _lock = scan::lock(exe)?;
        if warm::is_running(exe) {
            return Err(io::Error::other("close the game before preparing pipelines"));
        }
        let start = Instant::now();
        let result = match self {
            Self::SavageWind => run_savage_wind(exe),
        };
        let summary = match &result {
            Ok(stats) => stats.to_string(),
            Err(error) => format!("Preparation failed ({error})"),
        };
        let summary = format!("{summary} ({:.1} s).", start.elapsed().as_secs_f64());
        if let Err(error) = write_report(&report, &summary) {
            return Err(io::Error::other(format!("{summary} Could not save the result ({error}).")));
        }
        result
    }

    pub fn last_result(self, exe: &Path) -> Option<String> {
        fs::read_to_string(self.report_path(exe)?).ok()
    }

    fn report_path(self, exe: &Path) -> Option<PathBuf> {
        match self {
            Self::SavageWind => Some(with_suffix(&scan::scan_base_for(exe, ShaderApi::Vulkan)?, ".precompile.txt")),
        }
    }
}

fn write_report(path: &Path, summary: &str) -> io::Result<()> {
    let temp = with_suffix(path, ".tmp");
    fs::write(&temp, summary)?;
    fs::rename(temp, path)
}

fn run_savage_wind(exe: &Path) -> io::Result<NativeStats> {
    let exe = std::path::absolute(exe)?;
    let root = exe.parent().ok_or_else(|| io::Error::other("missing game directory"))?;
    let output = Command::new(&exe)
        .creation_flags(0x0800_0000)
        .current_dir(root)
        .arg("--precompile-shaders")
        .env("SAVAGEWIND_HIDDEN", "1")
        .env_remove("SAVAGEWIND_PRECOMPILE_NO_PIPELINES")
        .output()?;
    let output_text = String::from_utf8_lossy(&output.stderr);
    if !output.status.success() {
        let detail = output_text.lines().rev().find(|line| line.starts_with("[precompile] stopped:"));
        return Err(io::Error::other(detail.map_or_else(|| output.status.to_string(), str::to_owned)));
    }
    savage_wind_stats(&output_text)
}

fn savage_wind_stats(output: &str) -> io::Result<NativeStats> {
    let report = output.lines().rev()
        .find_map(|line| line.strip_prefix("[precompile] done: "))
        .and_then(|line| {
            let (shaders, rest) = line.split_once(" shaders, ")?;
            let (translations, rest) = rest.split_once(" translations (")?;
            let (_, rest) = rest.split_once(" cached, ")?;
            let (failed, _) = rest.split_once(" failed)")?;
            Some(NativeStats {
                shaders: shaders.parse().ok()?,
                translations: translations.parse().ok()?,
                failed: failed.parse().ok()?,
                pipelines: savage_wind_pipelines(output),
            })
        });
    report.ok_or_else(|| io::Error::other("the game precompiler did not report a result"))
}

fn savage_wind_pipelines(output: &str) -> Option<PipelineStats> {
    // The stock CLI can exit with the pipeline-list workers still running.
    // The progress meter can share a line with the pipeline log.
    let (_, last_warm) = output.rsplit_once("[pso] pre-warm")?;
    let line = last_warm.lines().next()?;
    let (_, rest) = line.strip_prefix(" done in ")?.split_once(" ms: ")?;
    let (built, rest) = rest.split_once(" pipelines built, ")?;
    let ("0", rest) = rest.split_once(" built first by draws, ")? else { return None };
    let (skipped, _) = rest.split_once(" skipped")?;
    let prepared: u64 = built.parse().ok()?;
    let failed: u64 = skipped.parse().ok()?;
    Some(PipelineStats { prepared, failed, total: prepared.checked_add(failed)? })
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct PipelineStats {
    pub prepared: u64,
    pub failed: u64,
    pub total: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_precompiler_requires_the_vulkan_install_and_command() {
        let dir = tempfile::tempdir().unwrap();
        let exe = dir.path().join("savagewind.exe");
        fs::create_dir_all(dir.path().join("extracted/romfs")).unwrap();
        fs::write(&exe, b"--precompile-shaders-unsupported\0").unwrap();
        assert!(NativePrecompiler::detect(&exe, ShaderApi::Vulkan).is_none());
        fs::write(&exe, b"--precompile-shaders\0").unwrap();
        assert!(NativePrecompiler::detect(&exe, ShaderApi::Vulkan).is_some());
        assert!(NativePrecompiler::detect(&exe, ShaderApi::D3d12).is_none());
    }

    #[test]
    fn native_precompiler_reports_translation_failures_and_requires_a_summary() {
        let output = "\r[precompile]  99%\n[precompile] done: 14 shaders, 20 translations (3 made, 15 cached, 2 failed) in 1.2 s\n";
        assert_eq!(savage_wind_stats(output).unwrap(), NativeStats {
            shaders: 14, translations: 20, failed: 2, pipelines: None,
        });
        assert!(savage_wind_stats("[precompile] 100%").is_err());
        let complete = "\r[precompile]  99%[pso] pre-warm done in 13 ms: 3 pipelines built, 0 built first by draws, 1 skipped";
        assert_eq!(savage_wind_pipelines(complete), Some(PipelineStats { prepared: 3, failed: 1, total: 4 }));
        assert!(savage_wind_pipelines(&format!("{complete}\n[pso] pre-warm: 8 pipelines on 2 threads")).is_none());
    }

    #[test]
    fn latest_precompile_report_replaces_the_previous_run() {
        let dir = tempfile::tempdir().unwrap();
        let report = dir.path().join("precompile.txt");
        write_report(&report, "Preparation failed.").unwrap();
        write_report(&report, "Pipeline list completion unconfirmed.").unwrap();
        assert_eq!(fs::read_to_string(report).unwrap(), "Pipeline list completion unconfirmed.");
    }
}
