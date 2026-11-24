// SPDX-License-Identifier: GPL-3.0-or-later
//! Inspect the Unleashed executable without starting the game.

use std::path::PathBuf;

fn main() -> std::io::Result<()> {
    let Some(exe) = std::env::args_os().nth(1) else {
        return Err(std::io::Error::other("usage: unleashed_inventory <game exe>"));
    };
    let inventory = tessera_core::unleashed::inventory(&PathBuf::from(exe))?;
    println!(
        "{} embedded PSOs, {} indexed shaders, {} referenced shaders decoded for DXIL and SPIR-V, {} vertex declarations resolved",
        inventory.pipelines.len(), inventory.indexed_shaders, inventory.referenced_shaders.len(),
        inventory.vertex_declarations.len()
    );
    Ok(())
}
