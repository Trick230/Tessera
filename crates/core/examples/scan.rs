// SPDX-License-Identifier: GPL-3.0-or-later
//! Prints what the app would show: `cargo run -p tessera-core --example scan --release`.

fn main() {
    let t = std::time::Instant::now();
    let games = tessera_core::analyze_all(tessera_core::discover(&[]));
    for g in &games {
        println!(
            "{:<45} {:<6} {:<18} {:<24} AC={:?} exe={}",
            g.entry.name,
            g.entry.store.label(),
            g.engine.label(),
            g.api_label(),
            g.anticheat,
            g.exe.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| "-".into())
        );
    }
    println!("{} jogos em {:?}", games.len(), t.elapsed());
}
