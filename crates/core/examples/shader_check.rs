// SPDX-License-Identifier: GPL-3.0-or-later
//! Shader state of one game without the UI:
//! `cargo run --release -p tessera-core --example shader_check -- <game exe> [install folder]`

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(exe) = args.first() else {
        eprintln!("usage: shader_check <game exe> [install folder]");
        std::process::exit(2);
    };
    let drivers: Vec<u64> =
        tessera_core::driver::gpus().iter().filter_map(|g| tessera_core::driver::umd_version(&g.driver_version)).collect();
    let t0 = std::time::Instant::now();
    let c = tessera_core::shader_check::check(Path::new(exe), args.get(1).map(Path::new), &drivers);
    println!("{c:#?}\ndrivers {drivers:x?}\n{:.0} ms", t0.elapsed().as_secs_f64() * 1e3);
}
