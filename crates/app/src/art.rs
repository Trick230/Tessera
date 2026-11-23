// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;
use std::path::{Path, PathBuf};

use image::{DynamicImage, ImageFormat, ImageReader, Limits, imageops::FilterType};
use slint::{Rgba8Pixel, SharedPixelBuffer};
use tessera_core::{GameInfo, library};

pub type Pixels = SharedPixelBuffer<Rgba8Pixel>;

fn decode(reader: impl std::io::BufRead + std::io::Seek, format: Option<ImageFormat>) -> Option<DynamicImage> {
    let mut reader = ImageReader::new(reader);
    if let Some(format) = format {
        reader.set_format(format);
    } else {
        reader = reader.with_guessed_format().ok()?;
    }
    let mut limits = Limits::default();
    limits.max_image_width = Some(4096);
    limits.max_image_height = Some(4096);
    reader.limits(limits);
    reader.decode().ok()
}

fn art_paths<'a>(game: &'a GameInfo, names: &'a [&'a str]) -> impl Iterator<Item = PathBuf> + 'a {
    let root = &game.entry.root;
    let exe_dir = game.exe_dir().unwrap_or(root);
    let dirs = [root.join("tessera-art"), root.to_path_buf(), exe_dir.to_path_buf(),
        root.join("res"), exe_dir.join("res"), root.join("assets"), root.join("art")];
    names.iter().flat_map(move |name| {
        dirs.clone().into_iter().flat_map(move |dir| {
            ["png", "jpg", "jpeg", "webp", "ico"].into_iter().map(move |ext| dir.join(format!("{name}.{ext}")))
        })
    })
}

fn load_file(path: &Path) -> Option<DynamicImage> {
    let file = std::fs::File::open(path).ok()?;
    if file.metadata().ok()?.len() > 16 << 20 {
        return None;
    }
    decode(std::io::BufReader::new(file), None)
}

fn pixels(image: DynamicImage, width: u32, height: u32) -> Pixels {
    let image = if image.width() > width || image.height() > height {
        image.resize(width, height, FilterType::Triangle)
    } else {
        image
    }.into_rgba8();
    Pixels::clone_from_slice(image.as_raw(), image.width(), image.height())
}

pub fn icon(game: &GameInfo) -> Option<Pixels> {
    let launch = game.entry.exe_hint.as_ref().map(|exe| game.entry.root.join(exe));
    let executables = || launch.as_deref().into_iter()
        .chain(game.exe.as_deref().filter(|exe| Some(*exe) != launch.as_deref()));
    let image = game.entry.steam_appid.and_then(library::steam_icon)
        .and_then(|path| load_file(&path))
        .or_else(|| executables().find_map(|exe| load_file(&exe.with_extension("ico"))))
        .or_else(|| art_paths(game, &["icon", "game-icon", "app-icon"]).find_map(|path| load_file(&path)))
        .or_else(|| executables().find_map(|exe| {
            let ico = tessera_core::pe::executable_icon(exe)?;
            decode(Cursor::new(ico), Some(ImageFormat::Ico))
        }))?;

    Some(pixels(image, 64, 64))
}
