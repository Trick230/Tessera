// SPDX-License-Identifier: GPL-3.0-or-later
//! Tessera preparation core: game discovery, executable analysis and
//! installation of the graphics layer. Never touches a running game.

pub mod anticheat;
pub mod driver;
pub mod engine;
pub mod game;
pub mod install;
pub mod library;
pub mod limits;
pub mod oodle;
pub mod pe;
pub mod paths;
pub mod scan;
pub mod session;
pub mod shader_check;
pub mod shaders;
pub mod warm;

pub use game::{analyze, analyze_all, GameInfo};
pub use library::{discover, GameEntry, Store};
