# Tessera guide

The full reference behind the [README](../README.md): building, every `tessera.ini` key, the frame
limiter, the command line, pipeline recording, environment variables and the tests. Design, catalogue
and status are in [PLAN.md](PLAN.md).

- [Building](#building)
- [Per-game settings](#per-game-settings)
- [Frame limiter](#frame-limiter)
- [Streaming and texture previews](#streaming-and-texture-previews)
- [Command line](#command-line)
- [Shader discovery](#shader-discovery)
- [Pipeline recording](#pipeline-recording)
- [Environment variables](#environment-variables)
- [Testing](#testing)

## Building

Everything the build needs lives in `.toolchain/<host>/` inside the repository folder (not committed),
so the folder builds on another PC with nothing installed: pinned LLVM (clang-cl, lld-link, llvm-ml),
CMake, Ninja, Rust (`RUSTUP_HOME`/`CARGO_HOME` in the folder) and the MSVC CRT and Windows SDK fetched
by [xwin](https://github.com/Jake-Shadle/xwin). Each host gets its own toolchain and build folders
(`build/<host>/`), so one copy of the repository can move between machines; a build tree configured in
another folder is reconfigured on the next build. C++ Release builds use ThinLTO (CPU texture encoder:
about 23% faster than MSVC `/O2`). No Visual Studio or system SDK is used.

```powershell
powershell -File tools/toolchain.ps1 -AcceptLicense   # Windows x64/ARM64 host, ~2.7 GB
powershell -File build.ps1                            # app, D3D11/DXGI/Vulkan layers, warm-up, D3D12 recorder (x64 and x86)
powershell -File build.ps1 -Test                      # also Rust unit tests and the x64/x86 smoke tests
powershell -File build.ps1 -Quality                   # adds tessera_texcomp.exe (BC7/BC6H on the GPU, CPU fallback, FLIP)
powershell -File build.ps1 -Vulkan                    # adds the Vulkan recording layer and tessera_vulkan_replay.exe
```

The app ends up in `build\windows-<arch>\cargo\release\tessera.exe`.

```sh
sh tools/toolchain.sh --accept-license   # Linux x64/ARM64 host
sh build.sh                              # Windows binaries in build/linux-<arch>/cargo/x86_64-pc-windows-msvc/release/
```

`-AcceptLicense` / `--accept-license` accepts the Microsoft license of the CRT and SDK files (they stay
local and are never redistributed). Linux only builds: the tests need Windows. The Rust build scripts
there also need the system C compiler (gcc or clang). To call `cargo` or `cmake` by hand, dot-source
`tools/env.ps1` first; `tools/paths.ps1` gives the build folders.

`tessera.exe` needs the Visual C++ Redistributable (x64): it links the C++ runtime dynamically because
the prebuilt Skia library it uses does.

## Per-game settings

The app writes `tessera.ini` next to the game executable when it installs the layer, and most options
have a switch in the app. Everything below is **off by default**.

| Key | What it does |
|---|---|
| `residency=1` | M2 residency priorities. Never enable on NVIDIA driver 621.14 (hangs or crashes the GPU). |
| `mip_residency=1` | Tiled mip residency. Needs tiled resources tier 2. |
| `rt_recycle=1` | Keeps released large render targets and hands them back on an identical create. |
| `flip_model=1` | Flip presentation for blit-model swap chains in borderless windows. |
| `present_cpu_sets=1` | Puts the presenting thread on performance cores (hybrid CPUs). |
| `texture_dedup=1` | Shares byte-identical immutable textures on the same device. The game gets the same object for each copy, so a game that stores its own data on a texture object (private data) or expects distinct pointers can misbehave. |
| `stream_io=1` / `2` | Learn file reads / replay them as low-priority prefetch on the next launch. `stream_root` overrides the asset folder. |
| `mip_prediction=1` | Learns which textures are bound together; with `stream_io`, prefetches matching file ranges. |
| `mip_streaming=1` | With `mip_residency=1`: split uploads, smallest mips first, throttled by a GPU fence. |
| `mip_feedback=1` | Samples one instrumented draw per 60 frames to record the mip the shader asked for. |
| `texture_quality=1` / `2` | Capture candidates for recompression / apply FLIP-approved replacements. |
| `d3d11_prefer_11_1=1` | Requests D3D11 feature level 11_1 (some GPUs only report tiled tier 2 there), with the game's own request as fallback. |
| `d3d11_call_diagnostics=1` | Records game calls to `Map`, `GetData`, `Flush[1]` and `UpdateSubresource[1]` taking at least 1 ms. |
| `ue_streaming=1` | Unreal texture streaming profile: pool from the VRAM budget, 3 textures per frame. |
| `ue_drs_off`, `ue_rt_grow`, `ue_pool_vram`, `ue_shaders_on_load`, `ue_load_limits` | Unreal test variants under measurement. |

Always on unless set to 0: `process` (timer resolution, EcoQoS off) and `workset` (working-set meter).
Other values: `fps_limit`, `limiter_mode`, `max_frame_latency`, `vram_budget_mb`, `mip_ram_mb`, `next`
(the chained DLL). The app turns the `ue_*` keys into `cvar.*` lines: one value slot per variable, found
offline in the executable and checked by the layer before each write.

## Frame limiter

`fps_limit` caps the frame rate on every API the layer covers.

- `limiter_mode=1` (default) holds the start of each frame for the time its work is predicted not to need
  (p90 of the last 32 frames plus an adaptive margin), then aligns the present to the cadence, so input is
  sampled shortly before the frame is shown. A late frame (stutter) is presented at once and the cadence
  restarts from it, without a burst of short frames.
- `limiter_mode=0` waits just before the present: steady, up to one interval of added latency.
- `limiter_mode=2` waits only at the frame start: lowest latency, uneven cadence.

When the limiter is on it is in charge: it never yields to Reflex, NVAPI or Vulkan low-latency modes,
frame generation (Smooth Motion, XeSS-FG, FidelityFX), RTSS or the NVIDIA limiter.

Which DLL the app installs:

| Game uses | Layer | Covers |
|---|---|---|
| Direct3D 11 | `d3d11.dll` (`tessera.dll`) | every module |
| Direct3D 12 | `dxgi.dll` (`tessera_dxgi.dll`), hooks swap chains from DXGI factories | limiter, process settings, session recording |
| Vulkan only | `vulkan-1.dll` (`tessera_vulkan.dll`), wraps `vkQueuePresentKHR` (export and `vkGet*ProcAddr`) | limiter, process settings, session recording |

On D3D12 and Vulkan the memory, residency, presentation and texture modules stay idle and the app hides
their options.

Benches: `d3d11_smoke --limiter-bench <fps> <csv> [d3d12]` measures the three modes on a D3D11 (or D3D12
flip) swap chain with a noisy synthetic load; `tessera_vulkan_present <fps> <csv>` (built with `-Vulkan`)
does the same on Vulkan, with the layer copied next to it as `vulkan-1.dll`.

## Streaming and texture previews

These are D3D11 previews, off by default, in the app's Textures tab (performance cores in Settings).

- **P4** `present_cpu_sets=1` selects performance cores for the presenting thread on a hybrid CPU, unless
  the game already set CPU sets.
- **M8** `texture_dedup=1` verifies every pixel or compressed block (row padding excluded) before sharing.
  It covers single-slice BC and 8-bit RGBA/BGRA textures up to 2 MiB. The session counts eligible textures,
  avoided creates and bytes avoided.
- **I1-I3** `stream_io=1` records file ranges in a learning session; `stream_io=2` replays the trace as
  bounded startup prefetch at low I/O priority. With `mip_prediction=1` in both sessions, reads are tied to
  sampled materials and matching ranges are prefetched during play, starting with a limited share of the
  budget. Replay skips ranges covered by a recent read and favours ranges followed by real demand in earlier
  replays. The trace is keyed by executable path and version; playback checks each asset's size and write
  time. The app sets the install folder; `stream_root` overrides it.
- **M7** `mip_prediction=1` learns which textures are bound together. With `mip_residency=1` on tier 2 it
  restores likely partners in advance when VRAM has room.
- **I4-I5** `mip_streaming=1` with `mip_residency=1` uploads the smallest mips first, up to 2 MiB of
  speculative copies and one split upload or predicted restore per Present. A D3D11 fence tracks copies
  still in flight; an 8 MiB window throttles them. First use still queues the rest before the draw.
- **M6** `mip_feedback=1` samples up to one compatible immediate-context draw per 60 Presents. Preparation
  instruments the first supported sample per statically indexed 2D texture slot in recorded `ps_5_0`
  shaders (up to eight slots). The layer skips draws with UAVs or an occupied output slot 7, restores the
  shader and bindings afterwards and reads the result without waiting.
- **M9** `texture_quality=1` records up to 32 eligible immutable textures within 32 MiB. With the game
  closed, **Prepare quality** converts opaque RGBA8 or nonnegative RGBA16F to BC7 or BC6H and checks every
  mip with FLIP and channel bounds. `texture_quality=2` applies a replacement only when the input pixels
  match the capture; texture and view descriptions keep the original format, and copies from the resource
  return the original pixels. Needs `build.ps1 -Quality`.

## Command line

- `tessera.exe --scan <game-exe> <install-folder>` writes a `*.scan.zip` shader export under
  `%LOCALAPPDATA%\Tessera\shaders`: DXBC under `d3d11/`, DXIL and SM 5.1 DXBC under `d3d12/`, SPIR-V under
  `vulkan/`, GLSL under `opengl/`.
- `tessera.exe --prepare-quality "<game-exe>"` runs the offline recompression check.
- `tessera.exe --prepare-d3d12-capture <game-exe> <capture>` imports a D3D12 pipeline recording.
- `cargo run -p tessera-core --example session -- <file.tsess> [--mips]` prints a session.
- `cargo run -p tessera-core --example layer -- calls <session.tsess>` exports slow D3D11 context calls and
  creates as CSV, to match against a PresentMon capture taken with `--v1_metrics --qpc_time`.
- `cargo run -p tessera-core --example layer -- ue <dir> <exe>` rewrites the Unreal cvar lines of a `tessera.ini`.
- `tools/global_test.ps1` runs interleaved A/B launches through Steam with PresentMon (elevated);
  `tools/measure_overhead.ps1` measures the layer's cost.

## Shader discovery

- Unreal: PAK (legacy and modern indexes), IoStore (including encrypted blocks), shader libraries up to
  5.8. The AES key is detected from the executable. If an Oodle-compressed build ships without a decoder,
  set `TESSERA_OODLE_DLL` to a trusted, compatible 64-bit Oodle DLL.
- Unity: asset bundles, WebGL data archives (gzip, Brotli), APK assets, Vulkan modules (SMOL-V).
- Recompiled games: shaders embedded in the executable and disk caches, such as Unleashed Recompiled's
  Zstandard/SMOL-V libraries, Savage Wind's `SPZ1` files and Skate 3's per-user `nrhi_shaders` cache.
  For Unleashed Recompiled 1.0.3 (Windows x64),
  `cargo run -p tessera-core --example unleashed_inventory -- <game-exe>` lists the embedded pipelines and
  decodes their DXIL and SPIR-V without launching the game.

## Pipeline recording

### D3D12

`tessera_d3d12_capture.dll` records compute, graphics (pipeline state streams, including mesh, view
instancing and stream output) and ray tracing pipelines (`CreateStateObject`, `AddToStateObject`), for
devices created through `D3D12CreateDevice` or an Agility SDK factory from `D3D12GetInterface`.

In the app: **Shaders > Record pipelines** launches the game with the recorder temporarily placed next to
the executable; **Import pipeline recording** merges captures; **Prepare shaders** replays them offline to
warm the driver cache. By hand: copy the DLL next to the game as `d3d12.dll`, set `TESSERA_D3D12_CAPTURE`
to a new file, play, then run `--prepare-d3d12-capture`. Legacy graphics stream-output declarations are not
captured. This is a recording tool, not the D3D12 optimization layer (phase 6).

### Vulkan

Build with `-Vulkan`. For the game process, set `VK_LAYER_PATH` to the folder with the layer DLL and
manifest and add `VK_LAYER_TESSERA_telemetry` to `VK_INSTANCE_LAYERS`. The app does this for **Record
pipelines** and **Play and verify cache**; the variables it reads are listed below.

`tessera_vulkan_replay <recording> <new-cache>` rebuilds the pipelines on the same driver and adapter.
Preparation saves checkpoints scoped to the recording, game, driver and configuration, and resumes from
them; only a complete replay becomes the prepared cache. Dynamic rendering, graphics pipeline libraries
and extension-specific state are not captured yet. Cache hits prove reuse, not a stutter reduction; that
still needs frame-time measurements.

## Environment variables

| Variable | Read by | Effect |
|---|---|---|
| `TESSERA_DATA_DIR` | app, layer | Data folder instead of `%LOCALAPPDATA%\Tessera` (paths under 260 characters). |
| `TESSERA_OODLE_DLL` | app | Oodle DLL for Unreal builds that ship without a decoder. |
| `TESSERA_UNREAL_PAK_KEY_FILE` | app | AES key file for encrypted Unreal archives, instead of the key found in the executable. |
| `TESSERA_D3D12_CAPTURE` | D3D12 recorder | Output file of a D3D12 pipeline recording. |
| `TESSERA_VULKAN_TELEMETRY` | Vulkan layer | CSV with pipeline creation times, application-cache feedback and present intervals (QPC). |
| `TESSERA_VULKAN_PIPELINE_CAPTURE` | Vulkan layer | Records compute and graphics pipelines (SPIR-V, layouts, push and specialization constants, core state, render passes, Vulkan 1.2 binding flags, immutable samplers). |
| `TESSERA_VULKAN_COMPUTE_CAPTURE` | Vulkan layer | Same, compute pipelines only. |
| `TESSERA_VULKAN_CACHE_OUTPUT` | Vulkan layer | Saves a pipeline-cache snapshot at the first present, or at `TESSERA_VULKAN_CACHE_FRAME`; device teardown saves it if the checkpoint was missed. |
| `TESSERA_VULKAN_CACHE_INPUT` | Vulkan layer | Seeds that cache on a later launch, only for calls where the game passes no cache of its own. |
| `TESSERA_NO_STORES` | app | Lists only the added game folders, not Steam or Epic games. |
| `TESSERA_SELECT` | app | Preselects the first game whose name contains this text. |
| `TESSERA_TAB` | app | With `TESSERA_SELECT`: opens tab 0-4 (4 = Textures). |
| `TESSERA_SNAPSHOT` | app | Renders the window to this BMP after 6 s and quits (works with the screen locked). |
| `TESSERA_SNAPSHOT_H` | app | Window height for the snapshot (default 1400). |

The last four are for scripted screenshots: the README hero is a real limiter session of `d3d11_smoke`
in a sample library made of synthetic game folders.

## Testing

`build.ps1 -Test` runs the Rust unit tests (`cargo test`) and the x64/x86 D3D11 smoke tests. Focused
scripts (they do not build; run `build.ps1` first):

| Script | Covers |
|---|---|
| `tests/run_smoke.ps1` | D3D11, DXGI and Vulkan layers (x64 and x86): hooks, budget, residency, limiter, warm-up |
| `tests/run_phase4.ps1` | Prediction, dedup and I/O prefetch |
| `tests/run_quality.ps1` | Mip feedback and BC7/BC6H recompression (needs `-Quality`) |
| `tests/run_vulkan_replay.ps1` | Vulkan recording and replay (needs `-Vulkan`) |

CI builds and tests every push on Windows Server 2022 and cross-builds on Ubuntu 24.04, with the toolchain
and build trees cached. Its GPU is the Microsoft Basic Render Driver (no dedicated VRAM), so the smoke test
skips the VRAM checks there and the Vulkan parts are not built.
