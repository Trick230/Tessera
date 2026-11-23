// SPDX-License-Identifier: GPL-3.0-or-later

use crate::AppTheme;
use std::{fs, io};
use tessera_core::paths::data_dir;
use winreg::{RegKey, enums::{HKEY_CURRENT_USER, KEY_SET_VALUE}};

const RUN_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Run";
const STARTUP_VALUE: &str = "Tessera";

pub fn load_theme() -> AppTheme {
    match fs::read_to_string(data_dir().join("theme.txt")).as_deref().map(str::trim) {
        Ok("light") => AppTheme::Light,
        Ok("dark") => AppTheme::Dark,
        _ => AppTheme::System,
    }
}

pub fn save_theme(theme: AppTheme) -> io::Result<()> {
    let dir = data_dir();
    let value = match theme {
        AppTheme::System => "system",
        AppTheme::Light => "light",
        AppTheme::Dark => "dark",
    };
    fs::create_dir_all(&dir)?;
    let temp = dir.join(format!("theme.{}.tmp", std::process::id()));
    fs::write(&temp, value)?;
    fs::rename(temp, dir.join("theme.txt"))
}

pub fn is_dark(theme: AppTheme) -> bool {
    match theme {
        AppTheme::System => RegKey::predef(HKEY_CURRENT_USER)
            .open_subkey(r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
            .and_then(|key| key.get_value::<u32, _>("AppsUseLightTheme"))
            .unwrap_or(0) == 0,
        AppTheme::Light => false,
        AppTheme::Dark => true,
    }
}

pub fn starts_with_windows() -> bool {
    RegKey::predef(HKEY_CURRENT_USER).open_subkey(RUN_KEY)
        .and_then(|key| key.get_value::<String, _>(STARTUP_VALUE))
        .is_ok_and(|value| !value.is_empty())
}

pub fn set_starts_with_windows(enabled: bool) -> io::Result<()> {
    let user = RegKey::predef(HKEY_CURRENT_USER);
    if enabled {
        let exe = std::env::current_exe()?;
        let mut command = std::ffi::OsString::from("\"");
        command.push(exe);
        command.push("\"");
        let (key, _) = user.create_subkey(RUN_KEY)?;
        key.set_value(STARTUP_VALUE, &command)
    } else {
        let result = user.open_subkey_with_flags(RUN_KEY, KEY_SET_VALUE)
            .and_then(|key| key.delete_value(STARTUP_VALUE));
        match result {
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
            other => other,
        }
    }
}
