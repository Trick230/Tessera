// SPDX-License-Identifier: GPL-3.0-or-later
//! S5 shader warm-up without the UI:
//! `cargo run --release -p tessera-core --example warm -- <game exe> <tessera_warm.exe>`
//! Reads the stores under the data dir (TESSERA_DATA_DIR to redirect).

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() != 2 {
        eprintln!("usage: warm <game exe> <tessera_warm.exe>");
        std::process::exit(2);
    }
    let t0 = std::time::Instant::now();
    match tessera_core::warm::run(Path::new(&args[0]), Path::new(&args[1])) {
        Ok(s) => println!("{s}\n{:.1} s", t0.elapsed().as_secs_f64()),
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1)
        }
    }
}
