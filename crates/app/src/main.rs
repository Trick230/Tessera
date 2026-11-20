// SPDX-License-Identifier: GPL-3.0-or-later
//! tessera.exe — preparation app. Runs before the game and closes; never
//! touches a running game process.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use slint::{ComponentHandle, ModelRc, SharedString, VecModel};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use tessera_core::install::{self, InstallState};
use tessera_core::shader_check::ShaderCheck;
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
        InstallState::NotInstalled => "Not installed".into(),
        InstallState::Installed { chained: Some(n), .. } => format!("Installed (chains {n})"),
        InstallState::Installed { .. } => "Installed".into(),
        InstallState::Foreign { proxy } => format!("Not installed ({proxy} from another tool will be chained)"),
    }
}

fn row(g: &GameInfo) -> GameRow {
    let s = |v: String| SharedString::from(v);
    GameRow {
        name: s(g.entry.name.clone()),
        store: s(g.entry.store.label().into()),
        engine: s(g.engine.label().into()),
        api: s(g.api_label()),
        exe: s(g.exe.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| "not found".into())),
        root: s(g.entry.root.display().to_string()),
        hash: s(g.exe_hash.map(|h| format!("{h:032x}")).unwrap_or_else(|| "-".into())),
        anticheat: s(g.anticheat.join(", ")),
        status: s(status_label(&g.install)),
        installed: g.install.is_installed(),
        blocked: !g.anticheat.is_empty(),
        can_install: g.can_install(),
        d3d11: can_prepare(g),
        icon: steam_image(g, false),
        initial: s(g.entry.name.chars().find(|c| c.is_alphanumeric()).map(|c| c.to_uppercase().collect()).unwrap_or_default()),
    }
}

/// Steam's icon (`hero == false`) or key art for a game; empty when there is none.
/// Decoded images are kept, so filtering the list does not decode them again.
/// Whether Prepare shaders applies. S1/S5 only read the install and run
/// outside the game: anti-cheat games too. A game that also has D3D12 gains
/// nothing unless it runs on D3D11: offered once the layer has stored
/// shaders it created.
fn can_prepare(g: &GameInfo) -> bool {
    g.exe.as_ref().is_some_and(|e| {
        let all = g.scan.all();
        all.has(tessera_core::pe::ApiSet::D3D11)
            && (!all.has(tessera_core::pe::ApiSet::D3D12) || tessera_core::shaders::stats(e).shaders > 0)
    })
}

fn steam_image(g: &GameInfo, hero: bool) -> slint::Image {
    thread_local! {
        static CACHE: std::cell::RefCell<std::collections::HashMap<(u32, bool), slint::Image>> = Default::default();
    }
    let Some(id) = g.entry.steam_appid else { return slint::Image::default() };
    CACHE.with_borrow_mut(|c| {
        c.entry((id, hero))
            .or_insert_with(|| {
                let art = tessera_core::library::steam_art(id);
                let path = if hero { art.hero } else { art.icon };
                path.and_then(|p| slint::Image::load_from_path(&p).ok()).unwrap_or_default()
            })
            .clone()
    })
}

fn num(v: f64, decimals: usize) -> String {
    format!("{v:.decimals$}")
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
        format!("{}-{}-{} {}:{}", &stamp[0..4], &stamp[4..6], &stamp[6..8], &stamp[9..11], &stamp[11..13])
    } else {
        name
    }
}

fn gb(mb: u32) -> String {
    format!("{} GB", num(mb as f64 / 1024.0, 1))
}

/// The coverage meter (S6): shaders the last session created and how many
/// were already known, shaders stored (layer + install scan), and how the
/// last warm-up went.
fn shader_text(exe: &Path, sum: Option<&session::Summary>) -> String {
    let mut parts = Vec::new();
    if let Some(sum) = sum.filter(|s| s.shaders > 0) {
        parts.push(format!(
            "{} created in {}, {}% already known",
            sum.shaders,
            duration(sum.shader_create_ms / 1000.0),
            (sum.shaders - sum.shaders_new) * 100 / sum.shaders
        ));
    }
    let store = tessera_core::warm::stored(exe);
    if store.shaders > 0 {
        parts.push(format!("{} stored ({})", store.shaders, size(store.bytes)));
    }
    let w = tessera_core::warm::stats(exe);
    if w.ok + w.failed > 0 {
        let mut t = format!("{} warmed", w.ok);
        if w.failed > 0 {
            t += &format!(", {} failed", w.failed);
        }
        parts.push(t);
    }
    parts.join("; ")
}

/// Fills the panel for the last session. Also returns the real VRAM budget
/// seen in it (MB, 0 if unknown) for the budget cap choices, and the shaders
/// it created and how many of those were new (not stored yet).
fn session_info(exe: &Path) -> (SessionInfo, u32, Option<(u64, u64)>) {
    let s = |v: String| SharedString::from(v);
    let mut info = SessionInfo {
        safe_mode: session::safe_mode_active(exe),
        shaders: s(shader_text(exe, None)),
        ..Default::default()
    };
    let Some(path) = session::latest(exe) else { return (info, 0, None) };
    info.present = true;
    let sum = match session::summarize_file(&path) {
        Ok(v) => v,
        Err(e) => {
            info.when = s(session_when(&path));
            info.note = s(format!("Could not read {}: {e}", path.display()));
            return (info, 0, None);
        }
    };
    info.clean = sum.clean;
    info.when = s(format!(
        "{}, {}",
        session_when(&path),
        duration(sum.duration_s)
    ));

    // Tiles.
    if sum.frame_avg_ms > 0.0 {
        info.fps = s(num(1000.0 / sum.frame_avg_ms, 0));
        info.fps_sub = s(format!("{} ms per frame", num(sum.frame_avg_ms, 1)));
    } else {
        info.fps = "-".into();
        info.fps_sub = "no frames".into();
    }
    info.p99 = s(format!("{} ms", num(sum.frame_p99_ms, 1)));
    info.p99_sub = s(format!("worst frame {} ms", num(sum.frame_max_ms, 0)));
    // A p99 within 1.5x the median reads as smooth.
    info.p99_good = sum.frame_p50_ms > 0.0 && sum.frame_p99_ms <= sum.frame_p50_ms * 1.5;
    let minutes = (sum.duration_s / 60.0).max(1.0 / 60.0);
    let per_min = sum.hitches as f64 / minutes;
    info.stutters = s(sum.hitches.to_string());
    info.stutters_sub = s(if sum.hitches == 0 {
        "none above twice the median".into()
    } else {
        format!("{} per minute", num(per_min, 1))
    });
    info.stutters_good = per_min < 1.0;
    if sum.vram_budget_mb > 0 {
        info.vram = s(gb(sum.vram_peak_mb));
        info.vram_sub = s(format!("peak, of {} available", gb(sum.vram_budget_mb)));
        info.vram_good = sum.vram_over_s == 0;
    } else {
        info.vram = s(size(sum.vram_bytes()));
        info.vram_sub = "created by the game (estimate)".into();
        info.vram_good = true;
    }

    // Plain-language reading.
    let other = sum.hitches - sum.hitches_shaders - sum.hitches_creating - sum.hitches_over_budget;
    let mut causes = Vec::new();
    if sum.hitches_shaders > 0 {
        causes.push(format!("{} while compiling shaders", sum.hitches_shaders));
    }
    if sum.hitches_creating > 0 {
        causes.push(format!("{} while creating textures or buffers", sum.hitches_creating));
    }
    if sum.hitches_over_budget > 0 {
        causes.push(format!("{} with VRAM over budget", sum.hitches_over_budget));
    }
    if other > 0 {
        causes.push(format!("{other} with no clear cause (CPU, disk or game logic)"));
    }
    if !causes.is_empty() {
        info.causes = s(format!("Stutters: {}.", causes.join(", ")));
    }
    let mut advice = Vec::new();
    if sum.vram_over_s > 0 {
        advice.push(format!(
            "The game spent {} over the available VRAM, swapping memory with system RAM. Lower texture quality, close other apps or set a VRAM cap.",
            duration(sum.vram_over_s as f64)
        ));
    }
    if sum.hitches > 0 && sum.hitches_creating * 2 > sum.hitches {
        advice.push("Most stutters line up with resource creation (streaming or loading).".to_string());
    }
    if sum.hitches > 0 && sum.hitches_shaders * 2 > sum.hitches {
        advice.push(
            "Most stutters line up with shader compilation. Tessera kept the shaders; Prepare shaders warms them before the next run."
                .to_string(),
        );
    }
    if sum.limited_frames > 0 {
        advice.push(format!(
            "FPS limit held {} frames (waited {} in total).",
            sum.limited_frames,
            duration(sum.limiter_wait_s)
        ));
    }
    if !sum.clean {
        advice.push("The session did not end cleanly (crash or forced close). If it happens again, safe mode turns the layer off.".to_string());
    }
    info.advice = s(advice.join("\n"));

    // Details.
    info.frames = s(format!("{} in {}", sum.frames, duration(sum.duration_s)));
    info.frametime = s(format!(
        "median {} ms, p99 {} ms, p99.9 {} ms, worst {} ms",
        num(sum.frame_p50_ms, 1),
        num(sum.frame_p99_ms, 1),
        num(sum.frame_p999_ms, 1),
        num(sum.frame_max_ms, 1)
    ));
    info.created = s(format!(
        "{} textures ({}), {} buffers ({}), {} with memory priority",
        sum.textures,
        size(sum.texture_bytes),
        sum.buffers,
        size(sum.buffer_bytes),
        sum.prioritized
    ));
    info.shaders = s(shader_text(exe, Some(&sum)));
    let fl = sum.feature_level;
    info.display = s(match sum.swapchain {
        Some((w, h)) => format!("{w}×{h}, D3D feature level {}_{}", fl >> 12, (fl >> 8) & 0xF),
        None => "no swapchain recorded".into(),
    });
    let mut notes = Vec::new();
    if sum.dropped > 0 {
        notes.push(format!("{} records lost (buffer full)", sum.dropped));
    }
    if sum.foreign > 0 {
        notes.push(format!("{} overlay or SDK resources left untouched", sum.foreign));
    }
    if sum.vram_cap_mb > 0 {
        notes.push(format!("VRAM cap of {} reported to the game", gb(sum.vram_cap_mb)));
    }
    info.note = s(notes.join("; "));
    let run = (sum.shaders > 0).then_some((sum.shaders, sum.shaders_new));
    (info, sum.vram_budget_mb, run)
}

/// The Shaders tab: what the driver already compiled, how much of what the
/// game uses is covered, what is damaged and what is out of date.
fn shader_state(c: &ShaderCheck, run: Option<(u64, u64)>, can_prepare: bool) -> ShaderState {
    const NEUTRAL: i32 = 0;
    const OK: i32 = 1;
    const WARN: i32 = 2;
    const BAD: i32 = 3;
    let line = |label: &str, value: String, detail: Vec<String>, tone: i32| ShaderRow {
        label: label.into(),
        value: value.into(),
        detail: detail.join(" ").into(),
        // Nothing stored: nothing to call good yet.
        tone: if c.stored == 0 && tone == OK { NEUTRAL } else { tone },
    };
    let mut rows = Vec::new();

    // Compiled: the warm-up logs written with the installed driver.
    let mut d = Vec::new();
    let warmable = c.stored - c.skipped;
    let (value, tone) = if c.stored == 0 {
        d.push(if can_prepare {
            "No shaders stored yet. Prepare shaders searches the game files; playing with the layer on stores the rest."
        } else {
            "No shaders stored yet. The warm-up is for D3D11: play once on D3D11 with the layer on, then Prepare shaders compiles what it stored."
        }.to_string());
        ("Nothing stored".to_string(), NEUTRAL)
    } else if c.all_compiled() {
        d.push(format!("The installed driver already compiled all {} stored shaders the warm-up can take.", c.compiled));
        (format!("All {}", c.compiled), OK)
    } else {
        d.push(format!(
            "{} of {} stored shaders are compiled with the installed driver; {} would compile while playing. Prepare shaders compiles them first.",
            c.compiled, warmable, c.pending
        ));
        let v = if c.compiled == 0 { "Not yet".to_string() } else { format!("{} of {}", c.compiled, warmable) };
        (v, WARN)
    };
    if c.skipped > 0 {
        d.push(format!("{} are D3D12 shaders (DXIL) the D3D11 warm-up cannot take.", c.skipped));
    }
    rows.push(line("Compiled", value, d, tone));

    // Coverage: how many of the shaders the last run created were already stored.
    let (value, tone, d) = match run {
        Some((n, new)) => {
            let mut d = vec![format!("The last run created {n} shaders; {} were already stored.", n - new)];
            if new > 0 {
                d.push(format!("The other {new} are stored now and compile in the next preparation."));
            }
            (format!("{}%", (n - new) * 100 / n), if new == 0 { OK } else { WARN }, d)
        }
        None => (
            "Unknown".to_string(),
            NEUTRAL,
            vec!["Play once with the layer on: the run records every shader the game creates.".to_string()],
        ),
    };
    rows.push(line("Coverage", value, d, tone));

    // Damaged: bad store entries, driver refusals, bad containers in the install.
    let mut d = Vec::new();
    if c.damaged > 0 {
        d.push(format!("{} stored entries are cut short or fail their checksum; they are left out.", c.damaged));
    }
    if c.failed > 0 {
        d.push(format!("The driver refused {} in the warm-up.", c.failed));
    }
    let bad_files = c.bad_in_install.unwrap_or(0);
    if bad_files > 0 {
        d.push(format!("{bad_files} shaders in the game files fail their checksum; the search leaves them out."));
    }
    let tone = if c.damaged > 0 { BAD } else if d.is_empty() { OK } else { WARN };
    let value = if d.is_empty() { "None".to_string() } else { (c.damaged + c.failed + bad_files).to_string() };
    if d.is_empty() {
        d.push(if c.stored == 0 { "Nothing to check yet." } else { "Every stored shader is whole and matches its checksum." }.to_string());
    }
    rows.push(line("Damaged", value, d, tone));

    // Out of date: game updated since the search, driver updated since the warm-up.
    let mut d = Vec::new();
    if c.scan_outdated == Some(true) {
        d.push("The game changed since its files were searched; Prepare shaders searches them again.".to_string());
    }
    if c.old_driver > 0 {
        d.push(format!("{} were compiled with an older driver and must be compiled again.", c.old_driver));
    }
    let tone = if d.is_empty() { OK } else { WARN };
    let value = if d.is_empty() {
        "None"
    } else if c.old_driver > 0 {
        "Driver updated"
    } else {
        "Game updated"
    };
    if tone == OK {
        d.push(match (c.stored, c.scan_outdated) {
            (0, _) => "Nothing to compare yet.",
            (_, Some(_)) => "The search matches the game as installed, and the warm-up the installed driver.",
            (_, None) => "The warm-up matches the installed driver. The game files were not searched yet.",
        }.to_string());
    }
    if c.dropped > 0 {
        d.push(format!("{} warm-up entries refer to shaders no longer stored (ignored).", c.dropped));
    }
    rows.push(line("Out of date", value.to_string(), d, tone));

    ShaderState { present: true, rows: ModelRc::new(VecModel::from(rows)) }
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
            "Windows gives this game {}. A lower cap makes it free memory sooner.",
            gb(budget_mb)
        )
    } else {
        "Makes the game free memory sooner. Play once to see how much it gets.".to_string()
    };
    let gs = GameSettings {
        present: true,
        process: cur.process,
        residency: cur.residency,
        flip_model: cur.flip_model,
        fps_options: labels(&fps, "Off", &|x| format!("{x} fps")),
        fps_index: idx(&fps, cur.fps_limit),
        vram_options: labels(&vram, "Off", &|x| {
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
    ui.set_hero(game.map(|g| steam_image(g, true)).unwrap_or_default());
    *SETTINGS.lock().unwrap() = None;
    // A newer load supersedes any still running, even for the same view index.
    let gen = SESSION_GEN.fetch_add(1, Ordering::Relaxed) + 1;
    ui.set_session(SessionInfo::default());
    ui.set_settings(GameSettings::default());
    ui.set_shader_state(ShaderState::default());
    let Some(exe) = exe else { return };
    let root = game.map(|g| g.entry.root.clone());
    let prepare = game.is_some_and(can_prepare);
    let weak = ui.as_weak();
    std::thread::spawn(move || {
        let (info, budget, run) = session_info(&exe);
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
        // Reads whole stores (checksums): after the session, which shows first.
        let drivers: Vec<u64> = tessera_core::driver::gpus()
            .iter()
            .filter_map(|g| tessera_core::driver::umd_version(&g.driver_version))
            .collect();
        let check = tessera_core::shader_check::check(&exe, root.as_deref(), &drivers);
        let _ = weak.upgrade_in_event_loop(move |ui| {
            if SESSION_GEN.load(Ordering::Relaxed) == gen {
                ui.set_shader_state(shader_state(&check, run, prepare));
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
    ui.set_status_text("Looking for games…".into());
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
                    // TESSERA_TAB=<0..3>: open that tab (3 = Shaders) (scripted screenshots).
                    if let Some(t) = std::env::var("TESSERA_TAB").ok().and_then(|v| v.parse().ok()) {
                        ui.set_tab(t);
                    }
                }
            }
            ui.set_busy(false);
            ui.set_status_text(format!("{n} games found in {ms} ms").into());
        });
    });
}

fn app_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default()
}

fn payload_path() -> PathBuf {
    app_dir().join(PAYLOAD_D3D11)
}

/// S1 + S5: finds shader bytecode in the install files (again only when the
/// install changed), then compiles everything stored in the driver, off the
/// UI thread.
fn warm_action(ui: &MainWindow, state: &Shared, view_idx: i32) {
    let Some((exe, root)) = ({
        let st = state.lock().unwrap();
        st.view.get(view_idx as usize).and_then(|&gi| {
            let g = &st.games[gi];
            g.exe.clone().map(|e| (e, g.entry.root.clone()))
        })
    }) else {
        return;
    };
    ui.set_busy(true);
    ui.set_status_text("Preparing shaders…".into());
    let (weak, state) = (ui.as_weak(), state.clone());
    std::thread::spawn(move || {
        let t0 = std::time::Instant::now();
        let status = |m: &'static str| {
            let _ = weak.upgrade_in_event_loop(move |ui| ui.set_status_text(m.into()));
        };
        let mut found = String::new();
        let running = tessera_core::warm::is_running(&exe);
        if !running && !tessera_core::scan::up_to_date(&exe, &root) {
            status("Searching the game files for shaders (read only)…");
            match scan_isolated(&exe, &root) {
                Ok(n) => found = format!("{n} found in the files, "),
                Err(e) => found = format!("search failed ({e}), "),
            }
        }
        status("Warming shaders (outside the game)…");
        let result = tessera_core::warm::run(&exe, &app_dir().join(tessera_core::warm::WARM_EXE));
        let msg = match result {
            Ok(_) => {
                let w = tessera_core::warm::stats(&exe);
                format!(
                    "Shaders: {found}{} ready, {} failed ({:.1} s)",
                    w.ok,
                    w.failed,
                    t0.elapsed().as_secs_f64()
                )
            }
            Err(e) => format!("Shaders: {found}{e}"),
        };
        let _ = weak.upgrade_in_event_loop(move |ui| {
            ui.set_busy(false);
            ui.set_status_text(msg.into());
            // The selection may have moved while this ran: reload only this game.
            let mut st = state.lock().unwrap();
            let sel = ui.get_selected();
            let same = st.view.get(sel as usize).is_some_and(|&gi| st.games[gi].exe.as_deref() == Some(&exe));
            if same {
                load_session(&ui, &mut st, sel, true);
            }
        });
    });
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
        Ok(_) if do_install => format!("Layer installed in {}", dir.display()),
        Ok(_) => format!("Layer removed from {}", dir.display()),
        Err(e) => format!("Error: {e}"),
    };
    st.games[gi].install = install::state(&dir, install::PROXY_D3D11);
    apply_view(ui, &mut st);
    load_session(ui, &mut st, view_idx, true); // the settings card follows the install state
    ui.set_status_text(msg.into());
}

/// Runs the install scan (S1) in a child `tessera.exe --scan`, so a crash in
/// the game's own Oodle DLL cannot take the app down. The scan writes its
/// store whole (temp file, then rename), so a crash leaves the old one.
fn scan_isolated(exe: &Path, root: &Path) -> Result<u64, String> {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    let me = std::env::current_exe().map_err(|e| e.to_string())?;
    let out = std::process::Command::new(me)
        .arg("--scan")
        .arg(exe)
        .arg(root)
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| e.to_string())?;
    match out.status.code() {
        Some(0) => String::from_utf8_lossy(&out.stdout).trim().parse().map_err(|_| "no result".to_string()),
        Some(1) => Err(String::from_utf8_lossy(&out.stderr).trim().to_string()),
        Some(c) => Err(format!("the search crashed, code {:#x}", c as u32)),
        None => Err("the search was stopped".into()),
    }
}

/// `tessera.exe --scan <game exe> <install folder>`: the child side of
/// scan_isolated. Prints the usable shader count.
fn scan_child(args: &[String]) -> i32 {
    match tessera_core::scan::scan_game(Path::new(&args[0]), Path::new(&args[1])) {
        Ok(st) => {
            println!("{}", st.usable);
            0
        }
        Err(e) => {
            eprintln!("{e}");
            1
        }
    }
}

fn main() -> Result<(), slint::PlatformError> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() == 3 && args[0] == "--scan" {
        std::process::exit(scan_child(&args[1..]));
    }
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
            let Some(dir) = rfd::FileDialog::new().set_title("Game folder").pick_folder() else { return };
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
                Ok(()) => "Safe mode off: the layer loads again next time the game starts".to_string(),
                Err(e) => format!("Error: {e}"),
            };
            ui.set_status_text(msg.into());
            load_session(&ui, &mut st, i, true);
        });
    }
    {
        let (w, s) = (ui.as_weak(), state.clone());
        ui.on_warm_shaders(move |i| warm_action(&w.unwrap(), &s, i));
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
                Ok(()) => "Saved. Applies the next time the game starts".to_string(),
                Err(e) => format!("Could not save tessera.ini: {e}"),
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
        // TESSERA_SNAPSHOT_H=<px>: window height for the snapshot (default 1400).
        let h = std::env::var("TESSERA_SNAPSHOT_H").ok().and_then(|v| v.parse().ok()).unwrap_or(1400.0);
        ui.window().set_size(slint::LogicalSize::new(1300.0, h));
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
