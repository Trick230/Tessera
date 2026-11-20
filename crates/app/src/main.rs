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

fn gb(mb: u32) -> String {
    format!("{} GB", num(mb as f64 / 1024.0, 1))
}

/// Fills the panel for the last session. Also returns the real VRAM budget
/// seen in it (MB, 0 if unknown) for the budget cap choices.
fn session_info(exe: &Path) -> (SessionInfo, u32) {
    let s = |v: String| SharedString::from(v);
    let mut info = SessionInfo { safe_mode: session::safe_mode_active(exe), ..Default::default() };
    let Some(path) = session::latest(exe) else { return (info, 0) };
    info.present = true;
    let sum = match session::summarize_file(&path) {
        Ok(v) => v,
        Err(e) => {
            info.when = s(session_when(&path));
            info.note = s(format!("Não foi possível ler {}: {e}", path.display()));
            return (info, 0);
        }
    };
    info.clean = sum.clean;
    info.when = s(format!(
        "{} · {} · {}",
        session_when(&path),
        duration(sum.duration_s),
        if sum.clean { "terminou bem" } else { "interrompida (crash ou fecho forçado)" }
    ));

    // Tiles.
    if sum.frame_avg_ms > 0.0 {
        info.fps = s(num(1000.0 / sum.frame_avg_ms, 0));
        info.fps_sub = s(format!("{} ms por frame", num(sum.frame_avg_ms, 1)));
    } else {
        info.fps = "-".into();
        info.fps_sub = "sem frames".into();
    }
    info.p99 = s(format!("{} ms", num(sum.frame_p99_ms, 1)));
    info.p99_sub = s(format!("1 % dos frames demorou mais · pior {} ms", num(sum.frame_max_ms, 0)));
    // A p99 within 1.5x the median reads as smooth.
    info.p99_good = sum.frame_p50_ms > 0.0 && sum.frame_p99_ms <= sum.frame_p50_ms * 1.5;
    let minutes = (sum.duration_s / 60.0).max(1.0 / 60.0);
    let per_min = sum.hitches as f64 / minutes;
    info.stutters = s(sum.hitches.to_string());
    info.stutters_sub = s(if sum.hitches == 0 {
        "nenhum frame acima do dobro da mediana".into()
    } else {
        format!("{} por minuto", num(per_min, 1))
    });
    info.stutters_good = per_min < 1.0;
    if sum.vram_budget_mb > 0 {
        info.vram = s(gb(sum.vram_peak_mb));
        info.vram_sub = s(format!("pico · o Windows deu {} ao jogo", gb(sum.vram_budget_mb)));
        info.vram_good = sum.vram_over_s == 0;
    } else {
        info.vram = s(size(sum.vram_bytes()));
        info.vram_sub = "criada pelo jogo (estimativa)".into();
        info.vram_good = true;
    }

    // Plain-language reading.
    let other = sum.hitches - sum.hitches_shaders - sum.hitches_creating - sum.hitches_over_budget;
    let mut causes = Vec::new();
    if sum.hitches_shaders > 0 {
        causes.push(format!("{} enquanto o jogo compilava shaders", sum.hitches_shaders));
    }
    if sum.hitches_creating > 0 {
        causes.push(format!("{} enquanto o jogo criava texturas ou buffers", sum.hitches_creating));
    }
    if sum.hitches_over_budget > 0 {
        causes.push(format!("{} com a VRAM acima do orçamento", sum.hitches_over_budget));
    }
    if other > 0 {
        causes.push(format!("{other} sem causa identificada (CPU, disco ou lógica do jogo)"));
    }
    if !causes.is_empty() {
        info.causes = s(format!("Stutters: {}.", causes.join(" · ")));
    }
    let mut advice = Vec::new();
    if sum.vram_over_s > 0 {
        advice.push(format!(
            "O jogo passou {} acima da VRAM disponível e a memória foi trocada com a RAM. Baixar a qualidade das texturas, fechar outras apps ou definir um teto de VRAM ajuda.",
            duration(sum.vram_over_s as f64)
        ));
    }
    if sum.hitches > 0 && sum.hitches_creating * 2 > sum.hitches {
        advice.push("A maior parte dos stutters coincide com a criação de recursos (streaming ou carregamentos).".to_string());
    }
    if sum.hitches > 0 && sum.hitches_shaders * 2 > sum.hitches {
        advice.push(
            "A maior parte dos stutters coincide com a compilação de shaders. O Tessera guardou-os; a próxima preparação vai aquecê-los antes do jogo."
                .to_string(),
        );
    }
    if sum.limited_frames > 0 {
        advice.push(format!(
            "Limitador ativo em {} frames (esperou {} no total).",
            sum.limited_frames,
            duration(sum.limiter_wait_s)
        ));
    }
    if !sum.clean {
        advice.push("A sessão não terminou bem; se se repetir, o modo seguro desliga a camada.".to_string());
    }
    info.advice = s(advice.join("\n"));

    // Details.
    info.frames = s(format!("{} em {}", sum.frames, duration(sum.duration_s)));
    info.frametime = s(format!(
        "mediana {} ms · p99 {} ms · p99,9 {} ms · pior {} ms",
        num(sum.frame_p50_ms, 1),
        num(sum.frame_p99_ms, 1),
        num(sum.frame_p999_ms, 1),
        num(sum.frame_max_ms, 1)
    ));
    info.created = s(format!(
        "{} texturas ({}) · {} buffers ({}) · {} com prioridade de memória",
        sum.textures,
        size(sum.texture_bytes),
        sum.buffers,
        size(sum.buffer_bytes),
        sum.prioritized
    ));
    if sum.shaders > 0 {
        // Coverage: shaders of this session that were already stored (what a
        // warm-up can compile before the game asks for them).
        let known = sum.shaders - sum.shaders_new;
        let store = tessera_core::shaders::stats(exe);
        info.shaders = s(format!(
            "{} criados em {} · {} % já conhecidos · {} guardados ({})",
            sum.shaders,
            duration(sum.shader_create_ms / 1000.0),
            known * 100 / sum.shaders,
            store.shaders,
            size(store.bytes)
        ));
    }
    let fl = sum.feature_level;
    info.display = s(match sum.swapchain {
        Some((w, h)) => format!("{w}×{h} · D3D feature level {}_{}", fl >> 12, (fl >> 8) & 0xF),
        None => "sem swapchain registada".into(),
    });
    let mut notes = Vec::new();
    if sum.dropped > 0 {
        notes.push(format!("{} registos perdidos (buffer cheio)", sum.dropped));
    }
    if sum.foreign > 0 {
        notes.push(format!("{} recursos de overlays ou SDKs deixados intactos", sum.foreign));
    }
    if sum.vram_cap_mb > 0 {
        notes.push(format!("teto de VRAM de {} reportado ao jogo", gb(sum.vram_cap_mb)));
    }
    info.note = s(notes.join(" · "));
    (info, sum.vram_budget_mb)
}

const FPS_CHOICES: [u32; 9] = [0, 30, 40, 60, 90, 120, 144, 165, 240];

/// Values behind the settings card's choices, for the game it shows.
struct SettingsLists {
    dir: PathBuf,
    fps: Vec<u32>,
    vram: Vec<u32>,
}

/// The settings card shown now (None while loading or not installed).
static SETTINGS: Mutex<Option<SettingsLists>> = Mutex::new(None);

/// Choice lists for the settings card; a value set by hand in tessera.ini is
/// kept as an extra entry so the card never rewrites it by accident.
fn game_settings(dir: &Path, budget_mb: u32) -> (GameSettings, SettingsLists) {
    let cur = install::settings(dir);
    let mut fps: Vec<u32> = FPS_CHOICES.to_vec();
    // Caps from 3 GB up to the real budget (or 12 GB when no session measured it).
    let top = if budget_mb > 0 { budget_mb / 1024 } else { 12 };
    let mut vram: Vec<u32> = std::iter::once(0).chain((3..=top.max(3)).map(|g| g * 1024)).collect();
    for (list, v) in [(&mut fps, cur.fps_limit), (&mut vram, cur.vram_budget_mb)] {
        if !list.contains(&v) {
            list.push(v);
            list.sort_unstable();
        }
    }
    let labels = |list: &[u32], off: &str, f: &dyn Fn(u32) -> String| -> ModelRc<SharedString> {
        let v: Vec<SharedString> = list.iter().map(|&x| if x == 0 { off.into() } else { f(x).into() }).collect();
        ModelRc::new(VecModel::from(v))
    };
    let idx = |list: &[u32], v: u32| list.iter().position(|&x| x == v).unwrap_or(0) as i32;
    let hint = if budget_mb > 0 {
        format!(
            "O Windows deu {} a este jogo. Um teto mais baixo faz o jogo poupar memória mais cedo e deixa margem para o navegador e o Discord.",
            gb(budget_mb)
        )
    } else {
        "Diz ao jogo que há menos VRAM, para gerir a memória mais cedo. Joga uma sessão para ver quanta o Windows lhe dá.".to_string()
    };
    let gs = GameSettings {
        present: true,
        process: cur.process,
        residency: cur.residency,
        flip_model: cur.flip_model,
        fps_options: labels(&fps, "Sem limite", &|x| format!("{x} fps")),
        fps_index: idx(&fps, cur.fps_limit),
        vram_options: labels(&vram, "Sem teto", &|x| {
            if x % 1024 == 0 { format!("{} GB", x / 1024) } else { format!("{x} MB") }
        }),
        vram_index: idx(&vram, cur.vram_budget_mb),
        vram_hint: hint.into(),
    };
    (gs, SettingsLists { dir: dir.to_path_buf(), fps, vram })
}

/// Loads the selected game's last session off the UI thread; a 1 h log is tens of MB.
/// Skipped when the selected game did not change, unless `force`.
fn load_session(ui: &MainWindow, st: &mut State, view_idx: i32, force: bool) {
    let game = st.view.get(view_idx as usize).map(|&gi| &st.games[gi]);
    let exe = game.and_then(|g| g.exe.clone());
    let dir = game.filter(|g| g.install.is_installed()).and_then(|g| g.exe_dir().map(Path::to_path_buf));
    if !force && exe == st.shown_exe {
        return;
    }
    st.shown_exe = exe.clone();
    *SETTINGS.lock().unwrap() = None;
    // A newer load supersedes any still running, even for the same view index.
    let gen = SESSION_GEN.fetch_add(1, Ordering::Relaxed) + 1;
    ui.set_session(SessionInfo::default());
    ui.set_settings(GameSettings::default());
    let Some(exe) = exe else { return };
    let weak = ui.as_weak();
    std::thread::spawn(move || {
        let (info, budget) = session_info(&exe);
        // GameSettings holds models (not Send); tessera.ini is tiny, read it on the UI thread.
        let _ = weak.upgrade_in_event_loop(move |ui| {
            if SESSION_GEN.load(Ordering::Relaxed) != gen {
                return;
            }
            ui.set_session(info);
            if let Some((gs, lists)) = dir.map(|d| game_settings(&d, budget)) {
                *SETTINGS.lock().unwrap() = Some(lists);
                ui.set_settings(gs);
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
            // TESSERA_SELECT=<name part>: preselect a game (scripted screenshots).
            if let (true, Ok(want)) = (ui.get_selected() < 0, std::env::var("TESSERA_SELECT")) {
                let want = want.to_lowercase();
                if let Some(i) = st.view.iter().position(|&gi| st.games[gi].entry.name.to_lowercase().contains(&want)) {
                    ui.set_selected(i as i32);
                    load_session(&ui, &mut st, i as i32, true);
                }
            }
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
    load_session(ui, &mut st, view_idx, true); // the settings card follows the install state
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
        let w = ui.as_weak();
        ui.on_settings_changed(move |_i, process, residency, flip_model, fps_i, vram_i| {
            let ui = w.unwrap();
            let slot = SETTINGS.lock().unwrap();
            let Some(l) = slot.as_ref() else { return };
            let pick = |list: &[u32], i: i32| list.get(i as usize).copied().unwrap_or(0);
            let new = install::Settings {
                process,
                residency,
                fps_limit: pick(&l.fps, fps_i),
                vram_budget_mb: pick(&l.vram, vram_i),
                flip_model,
            };
            let msg = match install::save_settings(&l.dir, &new) {
                Ok(()) => "Definições guardadas: aplicam-se no próximo arranque do jogo".to_string(),
                Err(e) => format!("Erro ao guardar tessera.ini: {e}"),
            };
            ui.set_status_text(msg.into());
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
    // TESSERA_SNAPSHOT=<file.bmp>: render the window to a file after a few
    // seconds and quit (scripted screenshots; works with the screen locked).
    let _snap_timer = std::env::var("TESSERA_SNAPSHOT").ok().map(|out| {
        ui.window().set_size(slint::LogicalSize::new(1180.0, 1400.0));
        let w = ui.as_weak();
        let t = slint::Timer::default();
        t.start(slint::TimerMode::SingleShot, std::time::Duration::from_secs(6), move || {
            if let Some(Ok(img)) = w.upgrade().map(|ui| ui.window().take_snapshot()) {
                let _ = std::fs::write(&out, bmp(img.width(), img.height(), img.as_bytes()));
            }
            let _ = slint::quit_event_loop();
        });
        t
    });
    ui.run()
}

/// Top-down 32-bit BMP from RGBA8 pixels.
fn bmp(w: u32, h: u32, rgba: &[u8]) -> Vec<u8> {
    let size = 54 + rgba.len() as u32;
    let mut b = Vec::with_capacity(size as usize);
    b.extend_from_slice(b"BM");
    for v in [size, 0, 54, 40, w, (h as i32).wrapping_neg() as u32] {
        b.extend_from_slice(&v.to_le_bytes());
    }
    b.extend_from_slice(&1u16.to_le_bytes());
    b.extend_from_slice(&32u16.to_le_bytes());
    for v in [0u32, rgba.len() as u32, 2835, 2835, 0, 0] {
        b.extend_from_slice(&v.to_le_bytes());
    }
    for p in rgba.chunks_exact(4) {
        b.extend_from_slice(&[p[2], p[1], p[0], p[3]]);
    }
    b
}
