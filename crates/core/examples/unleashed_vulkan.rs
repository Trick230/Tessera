// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs::OpenOptions;
use std::io::{self, BufWriter};
use std::path::Path;

fn main() -> io::Result<()> {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    let [exe, output, samples, filter, transparency] = args.as_slice() else {
        return Err(io::Error::other(
            "usage: unleashed_vulkan <game exe> <new inputs file> <1|2|4|8 samples> <bilinear|bicubic> <alpha-test|alpha-to-coverage>",
        ));
    };
    let samples = match samples.to_str() {
        Some("1") => 1, Some("2") => 2, Some("4") => 4, Some("8") => 8,
        _ => return Err(io::Error::other("invalid sample count")),
    };
    let bicubic = match filter.to_str() {
        Some("bilinear") => false, Some("bicubic") => true,
        _ => return Err(io::Error::other("invalid GI filter")),
    };
    let alpha_to_coverage = match transparency.to_str() {
        Some("alpha-test") => false, Some("alpha-to-coverage") => true,
        _ => return Err(io::Error::other("invalid transparency mode")),
    };
    let file = OpenOptions::new().write(true).create_new(true).open(output)?;
    tessera_core::unleashed::write_vulkan_inputs(Path::new(exe), BufWriter::new(file), samples, bicubic, alpha_to_coverage)
}
