// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::PathBuf;
use std::process::ExitCode;

fn run() -> io::Result<bool> {
    let args: Vec<_> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    if let [mode, exe] = args.as_slice() {
        if mode.as_os_str() == "--game" {
            let count = tessera_core::mip_feedback::prepare(exe)?;
            println!("Prepared {count} captured pixel shaders for mip feedback.");
            return Ok(count > 0);
        }
    }
    let [input, output] = args.as_slice() else {
        return Err(io::Error::other(
            "usage: instrument <input.dxbc> <new-output.dxbc>",
        ));
    };
    if fs::metadata(input)?.len() > 1024 * 1024 {
        return Ok(false);
    }
    let original = fs::read(input)?;
    let Some((modified, slots)) = tessera_core::mip_feedback::instrument(&original) else {
        return Ok(false);
    };
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output)?
        .write_all(&modified)?;
    println!("Feedback at u7, texture slots: {slots:?}");
    Ok(true)
}

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => {
            eprintln!("Shader has no supported samples or uses unsupported bytecode.");
            ExitCode::from(2)
        }
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}
