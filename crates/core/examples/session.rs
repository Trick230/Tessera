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
        }
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1)
        }
    }
}
