// SPDX-License-Identifier: GPL-3.0-or-later
//! Prints the summary of a session log:
//! `cargo run -p tessera-core --example session -- <file.tsess>`

fn main() {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: session <file.tsess>");
        std::process::exit(2)
    };
    match tessera_core::session::summarize_file(std::path::Path::new(&path)) {
        Ok(s) => {
            println!("exe        {}", s.header.exe_path);
            println!("clean      {} (dropped {})", s.clean, s.dropped);
            println!("duration   {:.1} s, {} frames", s.duration_s, s.frames);
            println!(
                "frame ms   avg {:.2}  p50 {:.2}  p99 {:.2}  p99.9 {:.2}  max {:.2}  hitches {}",
                s.frame_avg_ms, s.frame_p50_ms, s.frame_p99_ms, s.frame_p999_ms, s.frame_max_ms, s.hitches
            );
            println!(
                "resources  {} textures {:.1} MB, {} buffers {:.1} MB",
                s.textures,
                s.texture_bytes as f64 / 1048576.0,
                s.buffers,
                s.buffer_bytes as f64 / 1048576.0
            );
            println!("hitches    {} creating resources, {} other", s.hitches_creating, s.hitches - s.hitches_creating);
            println!("layer      limited {} frames ({:.1} s waited), {} prioritized, {} foreign",
                s.limited_frames, s.limiter_wait_s, s.prioritized, s.foreign);
            println!("display    {:?}, feature level {:#x}", s.swapchain, s.feature_level);
            println!(
                "hitches    {} shaders, {} over the VRAM budget",
                s.hitches_shaders, s.hitches_over_budget
            );
            println!(
                "shaders    {} created, {} new, {:.1} ms creating",
                s.shaders, s.shaders_new, s.shader_create_ms
            );
            println!(
                "vram       budget {} MB, peak {} MB, {} s over, cap {} MB",
                s.vram_budget_mb, s.vram_peak_mb, s.vram_over_s, s.vram_cap_mb
            );
            match &s.workset {
                Some(w) => println!(
                    "workset    {} MB of textures, bound avg {} / peak {} MB a second, {} MB in 10 s, {} untracked",
                    w.textures_mb, w.used_avg_mb, w.used_peak_mb, w.used10_peak_mb, w.untracked
                ),
                None => println!("workset    no samples"),
            }
            match &s.residency {
                Some(r) => println!(
                    "residency  {} managed, {} evicted at most, {} evictions, {} restores, {} late, {} MB in RAM at most",
                    r.managed_peak, r.evicted_peak, r.evictions, r.restores, r.late, r.ram_peak_mb
                ),
                None => println!("residency  off"),
            }
        }
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1)
        }
    }
}
