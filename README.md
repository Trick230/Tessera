# Tessera

**Smoother games on Windows: fewer stutters, steadier frame times and less video memory, without lowering quality and without textures popping in.**

Stutters usually come from a few places: shaders compiled the first time something appears, the game loading data while you move through the world, running out of video memory, and uneven frame pacing. Tessera works on each of them.

It does the heavy work **before** you play, in a small app. While you play, only a light graphics layer runs; the app may stay open, but it never touches a running game (it sets everything up before the game starts). The game loads it by itself, like any other DLL in its folder. Nothing is injected, nothing stays in the background, and nothing outside the game is changed.

> **Early development.** It works on Direct3D 11 games and is tested on real ones (mainly Final Fantasy VII Remake). Most features are off by default until measurements show they help. Direct3D 12 support is in progress. See [Status](#status).

---

## What it does

| You notice | What Tessera does |
|---|---|
| **Hitches the first time you see an effect or area** | Finds the game's shaders on disk (or records them while you play) and compiles them ahead of time, so the driver's cache is ready before you launch the game. |
| **Hitches while moving through the world** | Learns which files the game reads together and loads them early, at low priority. |
| **Uneven frame times** | A precise frame limiter, lower input latency settings and per-game scheduling tweaks. It works in D3D11, D3D12 and Vulkan games and steps aside when Reflex, RTSS or the NVIDIA limiter is already doing the job. |
| **Running out of video memory** | Reports the real budget to the game and keeps memory close to what is actually on screen. Removed texture detail comes back *before* it is drawn, so there is no pop-in. |
| **Unreal Engine games** | Applies better engine settings (for example texture streaming) safely, without editing the game's files. |
| **"Why does it stutter?"** | Every session is recorded. The app shows frame time percentiles, stutters per minute, video memory used and the likely cause. |

Quality is never traded silently. A lossy step, such as recompressing a texture, is only used if the [FLIP](https://github.com/NVlabs/flip) image metric confirms the difference is invisible.

## Getting started

There are no prebuilt downloads yet; [build it](#building) first. Then:

1. **Open `tessera.exe`.** Your Steam and Epic games appear in the **Library**. Add other folders in **Settings**.
2. **Pick a game.** The app shows its graphics API, engine and anti-cheat.
3. **Shaders tab → Prepare shaders.** Tessera scans the game and warms the driver's shader cache. Run it again after a game or driver update; the app tells you when it is out of date.
4. **Install layer.** Optional, Direct3D 11 games only. Settings in the **Session**, **Textures** and **Settings** tabs apply the next time the game starts.
5. **Play as usual** from Steam, Epic or the game's own launcher, then close the game.
6. **Session tab.** Look at what happened: average FPS, stutters, frame times and video memory.

To undo everything, select **Uninstall**. The game folder goes back to how it was.

## Safety

- **Anti-cheat games are never modified.** If Tessera finds EasyAntiCheat, BattlEye or a similar system, it only reads the game's files and will not install the layer.
- **Only the game is affected, and only while it runs.** CPU scheduling, timer resolution and priority changes apply to the game's process alone. Nothing system-wide is changed.
- **It coexists.** If the game already has a DLL with the same name (ReShade, DXVK, Special K, OptiScaler), Tessera loads it next instead of replacing it, and restores it when you uninstall.
- **It fails safely.** If a feature is unsupported or fails, only that feature turns off. After two crashes in a row with the layer, it disables itself on the next launch.
- **No network code** in the layer. The app and the layer only exchange files in `%LOCALAPPDATA%\Tessera`.

## Status

| Area | State |
|---|---|
| App: library, detection, install/uninstall, session panel | Works |
| Shader discovery and warming (Unreal, Unity, recompiled games, D3D11/D3D12/Vulkan bytecode) | Works; first-session stutter test still to do |
| D3D12 and Vulkan pipeline recording and offline replay | Works as a tool; game benefit not yet measured |
| Frame limiter (D3D11, D3D12, Vulkan), process tweaks, frame latency (D3D11) | Works; measured on a synthetic bench, game test still to do |
| Unreal texture streaming profile | Measured on FF7 Remake: 47.8–51.5 → 59.8 FPS at a 60 cap. Off by default for now. |
| Other Unreal settings (dynamic resolution, render targets, pools) | Being measured |
| Render target reuse | Measured on FF7 Remake at 4K: hitches over 50 ms from 8.0 to 1.5 per minute. Off by default. |
| Texture memory saving (tiled textures, deduplication, recompression) | Built and tested synthetically; needs a game and GPU that qualify. Off by default. |
| File prefetch and texture prediction | Built; prefetch works in game, stutter reduction not yet shown. Off by default. |
| Direct3D 12 optimization layer | Not started (recording tools exist) |

Full details, findings and what is left: [`docs/PLAN.md`](docs/PLAN.md). Advanced options, `tessera.ini` keys and command-line tools: [`docs/ADVANCED.md`](docs/ADVANCED.md).

## Building

Requirements: Windows 10/11 x64; Visual Studio 2022 or its Build Tools with the C++ workload, Windows SDK and CMake; Rust stable with the MSVC toolchain (pinned by `rust-toolchain.toml`).

```powershell
powershell -File build.ps1          # app + layer -> target\release\
powershell -File build.ps1 -Test    # also runs the unit tests and the D3D11 smoke tests
```

This produces `target\release\tessera.exe` with the layer and shader warm-up for both 64-bit and 32-bit games. Optional parts (texture recompression, Vulkan tools) are in [`docs/ADVANCED.md`](docs/ADVANCED.md).

## How it works

```
 tessera.exe (preparation app)              tessera layer (inside the game)
 runs before the game, then closes          loaded by the game as d3d11.dll
 ------------------------------             -------------------------------
 detects games, API, engine, anti-cheat     forwards to the system DLL
 reads the previous session's logs   <----  writes session logs
 builds caches, warms shaders        ---->  reads caches (memory-mapped)
 installs / uninstalls the layer            hooks via vtable patching only
```

- **Preparation app** (`tessera.exe`, Rust + Slint): finds games, detects API, engine and anti-cheat, reads the last session, prepares caches, installs or removes the layer. It never touches a running game.
- **Graphics layer** (C++): a proxy DLL in the game folder. It intercepts calls only by patching vtables: no inline hooks in system DLLs, no remote injection.
- **Helper tools**: shader warm-up (`tessera_warm.exe`), pipeline recorders and replay for D3D12 and Vulkan, texture recompression with FLIP. Each runs as its own process, so a driver crash cannot take the app down.

## Repository layout

```
crates/core          Rust: discovery, detection, archive and shader scanning, install, sessions
crates/app           Rust + Slint: the preparation app
src/dll              C++20: the Direct3D 11 layer
src/warm             C++20: tessera_warm.exe (out-of-process shader warm-up)
src/d3d12_capture    C++20: D3D12 pipeline recorder and replay
src/vulkan_telemetry C++20: Vulkan layer for pipeline recording, cache telemetry and replay
src/texcomp          C++20: BC7/BC6H recompression checked with FLIP
include/tessera      Binary formats shared by the layer, the tools and the app
tests                D3D11 smoke tests, quality and replay tests
tools                Export stub generator, overhead and A/B test scripts
docs                 Design plan and advanced usage
```

## License

[GPL-3.0-or-later](LICENSE).
