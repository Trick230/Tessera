// SPDX-License-Identifier: GPL-3.0-or-later
//! S1 bytecode scan without the UI (read-only on the game folder):
//! `cargo run --release -p tessera-core --example scan -- <game exe> <install root>`
//! Writes the scan store under the data dir (TESSERA_DATA_DIR to redirect).

use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() != 2 {
        eprintln!("usage: scan <game exe> <install root>");
        std::process::exit(2);
    }
    let t0 = std::time::Instant::now();
    match tessera_core::scan::scan_game(Path::new(&args[0]), Path::new(&args[1])) {
        Ok(s) => println!("{s:?}\n{:.1} s", t0.elapsed().as_secs_f64()),
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1)
        }
    }
}
