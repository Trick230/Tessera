// SPDX-License-Identifier: GPL-3.0-or-later
//! tessera.exe — preparation app. Runs before the game and closes; never
//! touches a running game process.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use tessera_core::install::{self, InstallState};
use tessera_core::{paths, session, GameInfo};

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
    /// Executable whose last session the detail panel shows (or is loading).
    shown_exe: Option<PathBuf>,
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

/// Decimal comma, as a Portuguese reader expects.
fn num(v: f64, decimals: usize) -> String {
    format!("{v:.decimals$}").replace('.', ",")
}

fn size(bytes: u64) -> String {
    let b = bytes as f64;
    if b >= 1024.0 * 1024.0 * 1024.0 {
        format!("{} GB", num(b / (1024.0 * 1024.0 * 1024.0), 2))
    } else {
        format!("{} MB", num(b / (1024.0 * 1024.0), 1))
    }
}

fn duration(s: f64) -> String {
    if s < 10.0 {
        return format!("{} s", num(s, 1));
    }
    let s = s.round() as u64;
    match (s / 3600, s / 60 % 60, s % 60) {
        (0, 0, sec) => format!("{sec} s"),
        (0, m, sec) => format!("{m} min {sec} s"),
        (h, m, _) => format!("{h} h {m} min"),
    }
}

/// `<exe>_YYYYMMDD-HHMMSS_<pid>.tsess`: the layer names files by local start time.
fn session_when(path: &Path) -> String {
    let name = path.file_stem().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let mut parts = name.rsplit('_');
    let (_pid, stamp) = (parts.next(), parts.next().unwrap_or(""));
    let d = stamp.as_bytes();
    if stamp.is_ascii() && d.len() == 15 && d[8] == b'-' {
        format!("{}/{}/{} {}:{}", &stamp[6..8], &stamp[4..6], &stamp[0..4], &stamp[9..11], &stamp[11..13])
    } else {
        name
    }
}

fn session_info(exe: &Path) -> SessionInfo {
    let s = |v: String| SharedString::from(v);
    let mut info = SessionInfo { safe_mode: session::safe_mode_active(exe), ..Default::default() };
    let Some(path) = session::latest(exe) else { return info };
    let sum = match session::summarize_file(&path) {
        Ok(v) => v,
        Err(e) => {
            info.present = true;
            info.when = s(session_when(&path));
            info.note = s(format!("Não foi possível ler {}: {e}", path.display()));
            return info;
        }
    };
    info.present = true;
    info.when = s(format!(
        "{} · {} · {}",
        session_when(&path),
        duration(sum.duration_s),
        if sum.clean { "terminou bem" } else { "interrompida (crash ou fecho forçado)" }
    ));
    info.frames = s(if sum.frame_avg_ms > 0.0 {
        format!("{} · média {} ms ({} fps)", sum.frames, num(sum.frame_avg_ms, 1), num(1000.0 / sum.frame_avg_ms, 0))
    } else {
        format!("{}", sum.frames)
    });
    info.frametime = s(format!(
        "p50 {} ms · p99 {} ms · p99,9 {} ms · pior {} ms",
        num(sum.frame_p50_ms, 1),
        num(sum.frame_p99_ms, 1),
        num(sum.frame_p999_ms, 1),
        num(sum.frame_max_ms, 1)
    ));
    info.hitches = s(if sum.hitches > 0 {
        format!(
            "{} frames acima do dobro da mediana ({} a criar recursos, {} outras causas)",
            sum.hitches,
            sum.hitches_creating,
            sum.hitches - sum.hitches_creating
        )
    } else {
        "nenhum".to_string()
    });
    info.vram = s(format!(
        "≈ {} criados ({} texturas, {}; {} buffers, {})",
        size(sum.vram_bytes()),
        sum.textures,
        size(sum.texture_bytes),
        sum.buffers,
        size(sum.buffer_bytes)
    ));
    let fl = sum.feature_level;
    info.display = s(match sum.swapchain {
        Some((w, h)) => format!("{w}×{h} · D3D feature level {}_{}", fl >> 12, (fl >> 8) & 0xF),
        None => "sem swapchain registada".into(),
    });
    if sum.dropped > 0 {
        info.note = s(format!("{} registos perdidos (buffer cheio)", sum.dropped));
    }
    info
}

/// Loads the selected game's last session off the UI thread; a 1 h log is tens of MB.
/// Skipped when the selected game did not change, unless `force`.
fn load_session(ui: &MainWindow, st: &mut State, view_idx: i32, force: bool) {
    let exe = st.view.get(view_idx as usize).and_then(|&gi| st.games[gi].exe.clone());
    if !force && exe == st.shown_exe {
        return;
    }
    st.shown_exe = exe.clone();
    // A newer load supersedes any still running, even for the same view index.
    let gen = SESSION_GEN.fetch_add(1, Ordering::Relaxed) + 1;
    ui.set_session(SessionInfo::default());
    let Some(exe) = exe else { return };
    let weak = ui.as_weak();
    std::thread::spawn(move || {
        let info = session_info(&exe);
        let _ = weak.upgrade_in_event_loop(move |ui| {
            if SESSION_GEN.load(Ordering::Relaxed) == gen {
                ui.set_session(info);
            }
        });
    });
}

static SESSION_GEN: AtomicU64 = AtomicU64::new(0);

fn apply_view(ui: &MainWindow, st: &mut State) {
    let f = st.filter.to_lowercase();
    st.view = (0..st.games.len()).filter(|&i| f.is_empty() || st.games[i].entry.name.to_lowercase().contains(&f)).collect();
    let rows: Vec<GameRow> = st.view.iter().map(|&i| row(&st.games[i])).collect();
    let sel = ui.get_selected();
    ui.set_games(ModelRc::new(VecModel::from(rows)));
    if sel >= st.view.len() as i32 {
        ui.set_selected(-1);
    }
    load_session(ui, st, ui.get_selected(), false);
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
            st.shown_exe = None; // a session may have been written since the last load
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
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_select(move |i| load_session(&w.unwrap(), &mut s.lock().unwrap(), i, false));
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_reset_safe_mode(move |i| {
            let ui = w.unwrap();
            let mut st = s.lock().unwrap();
            let Some(exe) = st.view.get(i as usize).and_then(|&gi| st.games[gi].exe.clone()) else { return };
            let msg = match session::reset_safe_mode(&exe) {
                Ok(()) => "Modo seguro desligado: a camada volta a carregar no próximo arranque".to_string(),
                Err(e) => format!("Erro: {e}"),
            };
            ui.set_status_text(msg.into());
            load_session(&ui, &mut st, i, true);
        });
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
    {
        // S7: read-only driver shader cache check, off the UI thread.
        let weak = ui.as_weak();
        std::thread::spawn(move || {
            let notice = tessera_core::driver::check().advice().join("\n");
            let _ = weak.upgrade_in_event_loop(move |ui| ui.set_driver_notice(notice.into()));
        });
    }
    ui.run()
}
