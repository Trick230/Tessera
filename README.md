# Tessera

**A game optimizer for Windows that makes 8 GB GPUs hold up in modern games: less VRAM, no stutters, no visible quality loss and no pop-in.**

Many current games want more video memory than a mid-range card has. When they run out, you get hitches, textures that load late, or settings you have to lower. Shader compilation, asset streaming while you move through the world, and uneven frame pacing cause stutters of their own.

Tessera tackles these problems in two steps. A preparation app does the heavy work **before** you play. A small graphics layer that the game loads itself manages memory and pacing **while** you play. It does not inject anything, it does not run in the background, and it touches nothing outside the game.

> **Status:** early development (Phase 0, foundations). The app and the Direct3D 11 layer work and are tested. The layer only measures for now; the optimizations described below come in later phases. See [Roadmap](#roadmap).

---

## What it does

| Problem | How Tessera addresses it |
|---|---|
| **VRAM pressure** | Brings memory use down toward what the game actually shows on screen. It works with the textures that are resident but unseen: residency priorities, tiled textures fed from a preallocated pool, and removing the coldest mip levels one at a time. |
| **Pop-in** | A removed mip is put back before the draw that needs it, within a fixed per-frame budget. The frame never waits on Tessera. |
| **Shader / PSO stutters** | Finds the game's shaders and pipeline caches on disk and compiles them ahead of time in a separate process, so the driver cache is warm before the first launch. |
| **Traversal stutters** | Learns which files and textures load together and prefetches them at low I/O priority. |
| **Frame pacing** | Precise frame limiter, flip model in borderless windows, frame latency control, and per-process scheduling tweaks. It backs off when Reflex or frame generation is active. |
| **Diagnosis** | Records frame times and resource creation in every session. The app shows the result: percentiles, stutters, a VRAM estimate and, later, the cause of each stutter. |

Quality is never traded silently. Any lossy step, such as recompressing a texture, is applied only if the [FLIP](https://github.com/NVlabs/flip) image metric confirms the difference is imperceptible.

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

- **Preparation app** (`tessera.exe`). It finds your games (Steam, Epic, or any folder you add) and detects the graphics API, engine and anti-cheat. It reads what the last session recorded and installs or removes the layer. It never touches a running game.
- **Graphics layer.** A proxy DLL placed in the game folder, which the game loads by itself. It intercepts calls only by patching vtables. There are no inline hooks in system DLLs and no remote injection. It has no network code and talks to the app only through files in `%LOCALAPPDATA%\Tessera`.
- **Helper tools** (planned). Shader warming, shader instrumentation, and texture compression with FLIP. Each one runs as a separate process, so a driver crash cannot take the app down.

## Principles

- **Prepare first.** Expensive work happens before the game starts, never inside it.
- **The frame never waits.** No mechanism blocks a frame. The most Tessera may cost is a small, bounded amount of work.
- **Only manage what it understands.** Tessera leaves alone resources it does not recognize and anything created by overlays or SDKs.
- **Get out of the way.** If the game, the driver or another tool already does something (Reflex, frame generation, its own streaming), Tessera steps back from that function.
- **Coexist.** If the game folder already has a DLL with the same name (ReShade, DXVK, Special K, OptiScaler), Tessera chains to it instead of replacing it. Uninstalling restores the original file.
- **Fail safely.** If a feature is unsupported or fails, only that feature turns off. After two crashes in a row with the layer, it disables itself on the next start (safe mode).

## Safety

- **Anti-cheat games are never modified.** If Tessera detects EasyAntiCheat, BattlEye or a similar system, it only reads the game's files and refuses to install the layer.
- **Only the game process is affected,** and only while the game runs: CPU sets, timer resolution, EcoQoS and a moderate priority. Nothing system-wide is changed.
- **Uninstalling is clean.** The layer is removed and any DLL it chained to gets its original name back.

## Current features (Phase 0)

- Game discovery from Steam and Epic, plus folders you add manually.
- Graphics API detection from the executable's imports, including games that switch API with a launch flag such as `-dx11`.
- Engine and anti-cheat detection.
- One-click install and uninstall of the Direct3D 11 layer, chaining to any DLL already there.
- Direct3D 11 proxy layer that forwards every export to the system DLL or to the next layer in the chain. It hooks device, factory and swap chain creation.
- Session recording: frame times plus every texture and buffer created. The layer writes it from a separate thread through a lock-free ring buffer.
- Safe mode with a crash counter. You can reset it from the app.
- A "Last session" panel showing frame time percentiles, stutter count, VRAM estimate and display mode.

## Roadmap

| Phase | Scope | Exit criterion |
|---|---|---|
| 0. Foundations | Proxy layer, chaining, safe mode, cache invalidation, anti-cheat detection, frame time logging (D3D11) | Games run the same; overhead under 1% |
| 1. Safe wins | VRAM budget reporting, residency priorities, frame limiter, flip model, frame latency, process tweaks | Better pacing, no regressions |
| 2. Shaders | Exhaustive shader discovery, engine PSO caches, out-of-process warming, self-correcting coverage | No compilation stutters from the second session |
| 3. D3D11 memory | Tiled textures, mip eviction under pressure, pop-in-free restore, working-set meter | VRAM going down, no late restores |
| 4. Prediction and I/O | Texture co-occurrence, deduplication, file prefetch, split uploads | Fewer traversal stutters, near-zero pop-in |
| 5. Fine quality | Shader-based mip feedback, FLIP-checked recompression | Extra savings with no FLIP-visible difference |
| 6. D3D12 | The same features for D3D12, PSO capture at startup | Lower VRAM and no PSO stutters in D3D12 games |
| 7. Expansion | System test, RTSS stats, AMD-specific shader paths | Works alongside popular mods and overlays |

The full design notes are in [`docs/PLAN.md`](docs/PLAN.md) (Portuguese).

## Building

Requirements:

- Windows 10/11 x64.
- Visual Studio 2022 or its Build Tools, with the C++ workload, the Windows SDK and CMake.
- Rust stable with the MSVC toolchain. `rust-toolchain.toml` pins it.

```powershell
powershell -File build.ps1          # app + layer -> target\release\
powershell -File build.ps1 -Test    # also runs the unit tests and the D3D11 smoke test
```

This produces `target\release\tessera.exe` and `target\release\tessera_d3d11.dll`. The app installs the layer into a game folder as `d3d11.dll`.

## Repository layout

```
crates/core      Rust: discovery, PE analysis, install/uninstall, session reader
crates/app       Rust + Slint: the preparation app
src/dll          C++20: the Direct3D 11 proxy layer
include/tessera  Binary formats shared by the layer and the app
tests            D3D11 smoke test (clean run, session contents, safe mode)
tools            Export stub generator
docs             Design plan
```

## License

[GPL-3.0-or-later](LICENSE).
