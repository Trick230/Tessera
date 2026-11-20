// SPDX-License-Identifier: GPL-3.0-or-later
//! Prints the S7 driver shader cache check:
//! `cargo run -p tessera-core --example driver`

fn main() {
    let r = tessera_core::driver::check();
    for g in &r.gpus {
        println!("gpu        {} ({:?}) driver {}", g.name, g.vendor, g.driver_version);
    }
    for (p, s) in &r.caches {
        println!("cache      {} {:.2} GiB", p.display(), *s as f64 / (1u64 << 30) as f64);
    }
    println!("nv limit   {:?}", r.nv_limit);
    println!("changed    {}", r.driver_changed);
    for a in r.advice() {
        println!("advice     {a}");
    }
}
