// SPDX-License-Identifier: GPL-3.0-or-later
//! Prints the summary of a session log:
//! `cargo run -p tessera-core --example session -- <file.tsess>`

fn main() {
    let mut args = std::env::args().skip(1);
    let Some(path) = args.next() else {
        eprintln!("usage: session <file.tsess> [--mips | --mip-misses | --stream-io]");
        std::process::exit(2)
    };
    let path = std::path::Path::new(&path);
    let detail = match args.next().as_deref() {
        Some("--mips") => Some(print_quality(path)),
        Some("--mip-misses") => Some(print_mip_misses(path)),
        Some("--stream-io") => Some(print_stream_io(path)),
        _ => None,
    };
    if let Some(result) = detail {
        if let Err(error) = result {
            eprintln!("{error}");
            std::process::exit(1);
        }
        return;
    }
    match tessera_core::session::summarize_file(path) {
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

fn print_mip_misses(path: &std::path::Path) -> Result<(), tessera_core::session::Error> {
    use tessera_core::session::{self, REC_MIP_SHADER_MISS};
    let data = std::fs::read(path)?;
    let (_, header, record) = session::parse_header(&data)?;
    println!("qpc,shader_hash,reason");
    for r in session::records(&data, header, record).filter(|r| r.kind == REC_MIP_SHADER_MISS) {
        let reason = match r.a {
            1 => "no_pixel_shader",
            2 => "untracked_shader",
            3 => "class_linkage",
            4 => "unsupported_bytecode",
            5 => "cache_miss",
            6 => "variant_install_failed",
            _ => "unknown",
        };
        println!("{},{:016x},{reason}", r.qpc, r.bytes);
    }
    Ok(())
}

fn print_stream_io(path: &std::path::Path) -> Result<(), tessera_core::session::Error> {
    use tessera_core::session::{self, REC_STREAM_IO};
    let data = std::fs::read(path)?;
    let (_, header, record) = session::parse_header(&data)?;
    println!("qpc,event,file_key,offset,bytes");
    for r in session::records(&data, header, record).filter(|r| r.kind == REC_STREAM_IO) {
        let event = match r.format {
            1 => "demand",
            2 => "startup_prefetch",
            3 => "material_prefetch",
            _ => "unknown",
        };
        println!("{},{event},{:08x}{:08x},{},{}", r.qpc, r.c, r.b, r.bytes, r.a);
    }
    Ok(())
}

fn print_quality(path: &std::path::Path) -> Result<(), tessera_core::session::Error> {
    use tessera_core::session::{self, FLAG_RECOMPRESSED, REC_MIP_FEEDBACK, REC_TEXTURE2D};
    let data = std::fs::read(path)?;
    let (_, header, record) = session::parse_header(&data)?;
    let mut mips = std::collections::BTreeMap::<u64, (u32, u64)>::new();
    let mut compressed = 0u64;
    let mut bytes = 0u64;
    for r in session::records(&data, header, record) {
        if r.kind == REC_MIP_FEEDBACK && r.bytes != 0 {
            let (mip, count) = mips.entry(r.bytes).or_insert((r.b, 0));
            *mip = (*mip).min(r.b);
            *count += 1;
        }
        if r.kind == REC_TEXTURE2D && r.flags & FLAG_RECOMPRESSED != 0 {
            compressed += 1;
            bytes = bytes.saturating_add(r.bytes);
        }
    }
    println!("{compressed} recompressed texture allocations, {bytes} BC bytes");
    for (texture, (mip, count)) in mips {
        println!("{texture:016x}  mip {mip}  ({count} samples)");
    }
    Ok(())
}
