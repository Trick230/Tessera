# Tessera

**A game optimizer for Windows: less VRAM, no stutters, a flat frame time, no visible quality loss and no pop-in.**

Many current games want more video memory than the card has. When they run out, you get hitches, textures that load late, or settings you have to lower. Shader compilation, asset streaming while you move through the world, and uneven frame pacing cause stutters of their own.

Tessera tackles these problems in two steps. A preparation app does the heavy work **before** you play. A small graphics layer that the game loads itself manages memory and pacing **while** you play. It does not inject anything, it does not run in the background, and it touches nothing outside the game.

> **Status:** early development. The app and the Direct3D 11 layer work and are tested on real games. Memory, shader, pacing and Unreal Engine features exist but most are off by default until measurements show they help. Direct3D 12 comes later. See [Status](#status) and [Roadmap](#roadmap).

---

## What it does

| Problem | How Tessera addresses it |
|---|---|
| **VRAM pressure** | Brings memory use down toward what the game actually shows on screen. It works with the textures that are resident but unseen: residency priorities, tiled textures fed from a preallocated pool, and removing the coldest mip levels one at a time. |
| **Pop-in** | A removed mip is put back before the draw that needs it, within a fixed per-frame budget. The frame never waits on Tessera. |
| **Shader / PSO stutters** | Finds the game's shaders and pipeline caches on disk and compiles them ahead of time in a separate process, so the driver cache is warm before the first launch. |
| **Traversal stutters** | Learns which files and textures load together and prefetches them at low I/O priority. |
| **Frame pacing** | Precise frame limiter, flip model in borderless windows, frame latency control, and per-process scheduling tweaks. The D3D11 limiter yields to active Reflex and the Smooth Motion runtime. |
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

## Status

**Works today (Direct3D 11):**

- **App:** game discovery from Steam, Epic and folders you add; graphics API, engine and anti-cheat detection; one-click install and uninstall of the layer, chaining to any DLL already there; per-game settings; a session panel with frame time percentiles, stutters per minute, VRAM peak against the budget, and the likely causes.
- **Layer basics:** proxy DLL that forwards every export; session recording through a lock-free ring buffer (frame times, every texture, buffer and shader created, slow creates); safe mode after two crashes in a row.
- **Pacing:** precise frame limiter that yields to lower or equal RTSS and NVIDIA limits, active Reflex or NVAPI pacing, and the Smooth Motion runtime; frame latency control; flip model for borderless windows (off by default); process tweaks (timer resolution, EcoQoS off).
- **D3D11 call diagnostics:** opt in under Advanced to record game calls to `Map`, `GetData`, `Flush[1]` and `UpdateSubresource[1]` taking at least 1 ms. Session details show the counts, maximum times and overlap with slow CPU frames. Use `cargo run -p tessera-core --example layer -- calls <session.tsess>` to export slow context calls and resource creates as CSV. Match the process ID and QPC intervals with a PresentMon capture using `--v1_metrics --qpc_time` to investigate displayed frames.
- **Memory:** VRAM budget reporting and an optional cap, also through NVAPI and before the game creates its device; working-set meter; mip residency with tiled textures and pop-in-free restore (needs tiled resources tier 2, off by default); render target reuse (off by default).
- **Shaders:** scans the whole install for shader bytecode (including Oodle, zlib, zstd and LZ4 archives) and warms the driver cache in a separate process before you play; records new shaders during play for the next preparation; a coverage meter in the app.

The **Shaders** tab writes a `*.scan.zip` export under `%LOCALAPPDATA%\Tessera\shaders`
when you scan or prepare a game. The archive contains D3D11-compatible DXBC under
`d3d11/`, DXIL and Shader Model 5.1 DXBC under `d3d12/`, and SPIR-V under
`vulkan/`. GLSL sources go under `opengl/`. You can also run
`tessera.exe --scan <game-exe> <install-folder>`.
If an Oodle-compressed Unreal build ships without a separate decoder, set
`TESSERA_OODLE_DLL` to the path of a trusted, compatible 64-bit Oodle DLL.

The scan reads precompiled shaders embedded in recomp executables and disk
caches, including Unleashed Recompiled's Zstandard and SMOL-V libraries,
Savage Wind's `SPZ1` files, and Skate 3's per-user `nrhi_shaders` cache.

The x64 D3D12 capture DLL records compute and graphics pipelines created
through the corresponding `ID3D12Device` methods. It observes devices
created through `D3D12CreateDevice` or through an Agility factory obtained
via `D3D12GetInterface`. In an isolated copy of a game, place
`tessera_d3d12_capture.dll` next to its executable as `d3d12.dll` and set
`TESSERA_D3D12_CAPTURE` to a new output file for the game process. Use
`tessera.exe --prepare-d3d12-capture <game-exe> <capture>` once the game closes.
The recorded duration is the time spent in the driver's pipeline creation call.
The x64 helper also replays pipeline state streams, including supported
mesh, view instancing and stream-output subobjects, and self-contained
ray tracing pipelines created through `CreateStateObject`. Existing
collections, incremental state objects and legacy graphics stream-output
declarations are outside this capture path.

Build the Windows Vulkan layer with `-DTESSERA_BUILD_VULKAN_TELEMETRY=ON`
and the Vulkan SDK headers. For the game process, set `VK_LAYER_PATH` to
the directory containing the layer DLL and manifest, add
`VK_LAYER_TESSERA_telemetry` to `VK_INSTANCE_LAYERS`, and set
`TESSERA_VULKAN_TELEMETRY` to a new CSV path. The CSV records pipeline
creation times, Vulkan application-cache feedback, and times between
presentation calls. The QPC column aligns events with external input
markers. Displayed frame times and internal driver-cache hits require
separate telemetry.

`TESSERA_VULKAN_CACHE_OUTPUT` saves a driver-compatible pipeline-cache
snapshot at the first successful presentation by default.
`TESSERA_VULKAN_CACHE_FRAME` selects a later presentation for a gameplay
capture. Device teardown saves the snapshot if the checkpoint was missed.
`TESSERA_VULKAN_CACHE_INPUT` seeds the cache
on a later launch. Tessera supplies this cache only for calls where the
game provides no cache of its own. The snapshot contains opaque driver
data. Recreating pipeline descriptors offline requires a separate recording.

Set `TESSERA_VULKAN_COMPUTE_CAPTURE` to a new file to record core compute
pipelines with their SPIR-V, descriptor set layouts, push constants and
specialization constants. Vulkan 1.2 recordings can include descriptor
binding flags and core immutable samplers. Run
`tessera_vulkan_replay <recording> <new-cache>`
on the same driver and adapter, then pass that output to
`TESSERA_VULKAN_CACHE_INPUT`. Graphics pipelines and extension-specific
sampler and pipeline state have separate descriptor requirements.

Use **Import pipeline recording** in the library's **Shaders** tab for
D3D12 or Vulkan. The app merges compatible captures by pipeline content
and displays the last replay result for the current recording, executable
and driver. **Prepare shaders** replays the imported recording. Build
the optional Vulkan payload alongside the app with `build.ps1 -Vulkan`.

- **Unreal Engine:** sets console variables without calling into the engine. The app finds each variable in the executable offline; the layer checks the build and every slot before writing. Texture streaming profile, off by default.

**Being measured:** Unreal variants (dynamic resolution off, render targets that only grow, pool limits, shader creation on load, level streaming time limits) in Final Fantasy VII Remake.

**Next:** promote what the measurements confirm, prediction and I/O prefetch, then Direct3D 12.

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

The full design notes, the detailed status and what is left are in [`docs/PLAN.md`](docs/PLAN.md).

## Building

Requirements:

- Windows 10/11 x64.
- Visual Studio 2022 or its Build Tools, with the C++ workload, the Windows SDK and CMake.
- Rust stable with the MSVC toolchain. `rust-toolchain.toml` pins it.

```powershell
powershell -File build.ps1          # app + layer -> target\release\
powershell -File build.ps1 -Test    # also runs the unit tests and both D3D11 smoke tests
```

This produces `target\release\tessera.exe` with D3D11 layer and shader warm-up binaries for x64 and x86 games. The app picks the game's architecture and installs the matching layer as `d3d11.dll`. Unreal console variable overrides require an x64 game.

## Repository layout

```
crates/core      Rust: discovery, PE analysis, install/uninstall, session reader,
                 shader scan and warming, Unreal cvar scanner
crates/app       Rust + Slint: the preparation app
src/dll          C++20: the Direct3D 11 proxy layer
src/warm         C++20: tessera_warm.exe (out-of-process shader warming)
include/tessera  Binary formats shared by the layer and the app
tests            D3D11 smoke test (every layer module, session contents, safe mode)
tools            Export stub generator, overhead and A/B test scripts
docs             Design plan
```

## License

[GPL-3.0-or-later](LICENSE).
