// SPDX-License-Identifier: GPL-3.0-or-later
//! tessera.exe — preparation app. Runs before the game and closes; never
//! touches a running game process.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use tessera_core::install::{self, InstallState};
use tessera_core::{paths, GameInfo};

slint::include_modules!();

/// Built layer shipped next to tessera.exe; installed under the proxy name.
const PAYLOAD_D3D11: &str = "tessera_d3d11.dll";

#[derive(Default)]
struct State {
    games: Vec<GameInfo>,
    /// Indices into `games` currently shown (after the search filter).
    view: Vec<usize>,
    filter: String,
    folders: Vec<PathBuf>,
}

type Shared = Arc<Mutex<State>>;

fn folders_file() -> PathBuf {
    paths::data_dir().join("folders.txt")
}

fn load_folders() -> Vec<PathBuf> {
    std::fs::read_to_string(folders_file())
        .map(|t| t.lines().filter(|l| !l.trim().is_empty()).map(PathBuf::from).collect())
        .unwrap_or_default()
}

fn save_folders(folders: &[PathBuf]) {
    let _ = std::fs::create_dir_all(paths::data_dir());
    let text: String = folders.iter().map(|p| format!("{}\n", p.display())).collect();
    let _ = std::fs::write(folders_file(), text);
}

fn status_label(s: &InstallState) -> String {
    match s {
        InstallState::NotInstalled => "Não instalada".into(),
        InstallState::Installed { chained: Some(n), .. } => format!("Instalada (encadeia {n})"),
        InstallState::Installed { .. } => "Instalada".into(),
        InstallState::Foreign { proxy } => format!("Não instalada ({proxy} de outra ferramenta será encadeada)"),
    }
}

fn row(g: &GameInfo) -> GameRow {
    let s = |v: String| SharedString::from(v);
    GameRow {
        name: s(g.entry.name.clone()),
        store: s(g.entry.store.label().into()),
        engine: s(g.engine.label().into()),
        api: s(g.api_label()),
        exe: s(g.exe.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| "não encontrado".into())),
        root: s(g.entry.root.display().to_string()),
        hash: s(g.exe_hash.map(|h| format!("{h:032x}")).unwrap_or_else(|| "-".into())),
        anticheat: s(g.anticheat.join(", ")),
        status: s(status_label(&g.install)),
        installed: g.install.is_installed(),
        blocked: !g.anticheat.is_empty(),
        can_install: g.can_install(),
    }
}

fn apply_view(ui: &MainWindow, st: &mut State) {
    let f = st.filter.to_lowercase();
    st.view = (0..st.games.len()).filter(|&i| f.is_empty() || st.games[i].entry.name.to_lowercase().contains(&f)).collect();
    let rows: Vec<GameRow> = st.view.iter().map(|&i| row(&st.games[i])).collect();
    let sel = ui.get_selected();
    ui.set_games(ModelRc::new(VecModel::from(rows)));
    if sel >= st.view.len() as i32 {
        ui.set_selected(-1);
    }
}

fn refresh(ui: &MainWindow, state: &Shared) {
    ui.set_busy(true);
    ui.set_status_text("A procurar jogos…".into());
    let weak = ui.as_weak();
    let state = state.clone();
    let folders = state.lock().unwrap().folders.clone();
    std::thread::spawn(move || {
        let t = std::time::Instant::now();
        let games = tessera_core::analyze_all(tessera_core::discover(&folders));
        let ms = t.elapsed().as_millis();
        let _ = weak.upgrade_in_event_loop(move |ui| {
            let mut st = state.lock().unwrap();
            let n = games.len();
            st.games = games;
            apply_view(&ui, &mut st);
            ui.set_busy(false);
            ui.set_status_text(format!("{n} jogos analisados em {ms} ms").into());
        });
    });
}

fn payload_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_default()
        .join(PAYLOAD_D3D11)
}

/// Runs install/uninstall on the selected game and refreshes its row.
fn layer_action(ui: &MainWindow, state: &Shared, view_idx: i32, do_install: bool) {
    let mut st = state.lock().unwrap();
    let Some(&gi) = st.view.get(view_idx as usize) else { return };
    let g = &st.games[gi];
    let Some(dir) = g.exe_dir().map(Path::to_path_buf) else { return };
    if do_install && !g.can_install() {
        return;
    }
    let result = if do_install {
        install::install(&dir, &payload_path(), install::PROXY_D3D11)
    } else {
        install::uninstall(&dir)
    };
    let msg = match &result {
        Ok(_) if do_install => format!("Camada instalada em {}", dir.display()),
        Ok(_) => format!("Camada removida de {}", dir.display()),
        Err(e) => format!("Erro: {e}"),
    };
    st.games[gi].install = install::state(&dir, install::PROXY_D3D11);
    apply_view(ui, &mut st);
    ui.set_status_text(msg.into());
}

fn main() -> Result<(), slint::PlatformError> {
    let ui = MainWindow::new()?;
    let state: Shared = Arc::new(Mutex::new(State { folders: load_folders(), ..Default::default() }));

    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_refresh(move || refresh(&w.unwrap(), &s));
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_filter_changed(move |t| {
            let mut st = s.lock().unwrap();
            st.filter = t.to_string();
            apply_view(&w.unwrap(), &mut st);
        });
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_add_folder(move || {
            let Some(dir) = rfd::FileDialog::new().set_title("Pasta do jogo").pick_folder() else { return };
            {
                let mut st = s.lock().unwrap();
                if !st.folders.contains(&dir) {
                    st.folders.push(dir);
                    save_folders(&st.folders);
                }
            }
            refresh(&w.unwrap(), &s);
        });
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_install(move |i| layer_action(&w.unwrap(), &s, i, true));
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_uninstall(move |i| layer_action(&w.unwrap(), &s, i, false));
    }
    {
        let s = state.clone();
        ui.on_open_folder(move |i| {
            let st = s.lock().unwrap();
            if let Some(&gi) = st.view.get(i as usize) {
                let g = &st.games[gi];
                let dir = g.exe_dir().unwrap_or(&g.entry.root);
                let _ = std::process::Command::new("explorer").arg(dir).spawn();
            }
        });
    }

    refresh(&ui, &state);
    ui.run()
}
