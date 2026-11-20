// SPDX-License-Identifier: GPL-3.0-or-later
//! Scripted install/uninstall of the D3D11 layer, for tests without the UI:
//! `cargo run -p tessera-core --example layer -- install <game exe dir> <payload dll>`
//! `cargo run -p tessera-core --example layer -- uninstall <game exe dir>`
//! `cargo run -p tessera-core --example layer -- status <game exe dir>`
//! `cargo run -p tessera-core --example layer -- crashes <game exe>` (safe-mode counter)

use std::path::Path;
use tessera_core::{install, session};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let usage = || -> ! {
        eprintln!("usage: layer install <dir> <payload> | uninstall <dir> | status <dir> | crashes <exe>");
        std::process::exit(2)
    };
    let dir = Path::new(args.get(1).unwrap_or_else(|| usage()));
    let result = match args[0].as_str() {
        "install" => install::install(dir, Path::new(args.get(2).unwrap_or_else(|| usage())), install::PROXY_D3D11),
        "uninstall" => install::uninstall(dir),
        "status" => Ok(install::state(dir, install::PROXY_D3D11)),
        "crashes" => {
            println!("{}", session::crash_count(dir));
            return;
        }
        _ => usage(),
    };
    match result {
        Ok(s) => println!("{s:?}"),
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1)
        }
    }
}
