# Tessera — advanced usage

Power-user and developer notes that do not belong in the [README](../README.md). Design and status are in
[`PLAN.md`](PLAN.md).

## Building the optional parts

```powershell
powershell -File build.ps1            # app + D3D11 layer, shader warm-up and D3D12 recorder (x64 and x86)
powershell -File build.ps1 -Test      # also Rust unit tests and the x64/x86 D3D11 smoke tests
powershell -File build.ps1 -Quality   # adds tessera_texcomp.exe (BC7/BC6H + FLIP)
powershell -File build.ps1 -Vulkan    # adds the Vulkan layer and tessera_vulkan_replay.exe (needs the Vulkan SDK headers)
```

Focused tests: `tests/run_smoke.ps1` (D3D11 layer), `tests/run_phase4.ps1` (prediction and I/O),
`tests/run_quality.ps1` (mip feedback and recompression), `tests/run_vulkan_replay.ps1`.

## Per-game settings (`tessera.ini`)

The app writes `tessera.ini` next to the game executable when it installs the layer. Most options also have a
switch in the app. Everything below is **off by default**.

| Key | What it does |
|---|---|
| `residency=1` | M2 residency priorities. Never enable on NVIDIA driver 621.14 (hangs or crashes FF7R). |
| `mip_residency=1` | Tiled mip residency. Needs tiled resources tier 2. |
| `rt_recycle=1` | Keeps released large render targets and hands them back on an identical create. |
| `flip_model=1` | Flip presentation for blit-model swap chains in borderless windows. |
| `present_cpu_sets=1` | Puts the presenting thread on performance cores (hybrid CPUs). |
| `texture_dedup=1` | Shares byte-identical immutable textures on the same device. The game gets the same object for each copy, so a game that stores its own data on a texture object (private data) or expects distinct pointers can misbehave; off by default. |
| `stream_io=1` / `2` | Learn file reads / replay them as low-priority prefetch on the next launch. `stream_root` overrides the asset folder. |
| `mip_prediction=1` | Learns which textures are bound together; with `stream_io`, prefetches matching file ranges. |
| `mip_streaming=1` | With `mip_residency=1`: split uploads, smallest mips first, throttled by a GPU fence. |
| `mip_feedback=1` | Samples one instrumented draw per 60 frames to record the mip the shader asked for. |
| `texture_quality=1` / `2` | Capture candidates for recompression / apply FLIP-approved replacements. |
| `d3d11_prefer_11_1=1` | Requests D3D11 feature level 11_1 (some GPUs only report tiled tier 2 there), with the game's own request as fallback. |
| `d3d11_call_diagnostics=1` | Records game calls to `Map`, `GetData`, `Flush[1]` and `UpdateSubresource[1]` taking at least 1 ms. |
| `ue_streaming=1` | Unreal texture streaming profile: pool from the VRAM budget, 3 textures per frame. |
| `ue_drs_off`, `ue_rt_grow`, `ue_pool_vram`, `ue_shaders_on_load`, `ue_load_limits` | Unreal test variants under measurement. |

Always on unless set to 0: `process` (timer resolution, EcoQoS off), `workset` (working-set meter). Other
values: `fps_limit`, `max_frame_latency`, `vram_budget_mb`, `mip_ram_mb`. The app turns the `ue_*` keys into
`cvar.*` lines (one value slot per variable, checked by the layer before each write).

## Command line

- `tessera.exe --scan <game-exe> <install-folder>` writes a `*.scan.zip` shader export under
  `%LOCALAPPDATA%\Tessera\shaders`: DXBC under `d3d11/`, DXIL and SM 5.1 DXBC under `d3d12/`, SPIR-V under
  `vulkan/`, GLSL under `opengl/`.
- `tessera.exe --prepare-quality "<game-exe>"` runs the offline recompression check.
- `tessera.exe --prepare-d3d12-capture <game-exe> <capture>` imports a D3D12 pipeline recording.
- `cargo run -p tessera-core --example session -- <file.tsess> [--mips]` prints a session.
- `cargo run -p tessera-core --example layer -- calls <session.tsess>` exports slow D3D11 context calls and
  creates as CSV. Match them to a PresentMon capture taken with `--v1_metrics --qpc_time`.
- `cargo run -p tessera-core --example layer -- ue <dir> <exe>` rewrites the Unreal cvar lines of a `tessera.ini`.
- `tools/global_test.ps1` runs interleaved A/B launches through Steam with PresentMon (elevated).

## Shader discovery notes

- Unreal: PAK (legacy and modern indexes), IoStore (including encrypted blocks), shader libraries up to 5.8.
  The AES key is detected from the executable. If an Oodle-compressed build ships without a decoder, set
  `TESSERA_OODLE_DLL` to a trusted, compatible 64-bit Oodle DLL.
- Unity: asset bundles, WebGL data archives (gzip, Brotli), APK assets, Vulkan modules (SMOL-V).
- Recompiled games: shaders embedded in the executable and disk caches, such as Unleashed Recompiled's
  Zstandard/SMOL-V libraries, Savage Wind's `SPZ1` files and Skate 3's per-user `nrhi_shaders` cache.
  For Unleashed Recompiled 1.0.3 (Windows x64),
  `cargo run -p tessera-core --example unleashed_inventory -- <game-exe>` lists the embedded pipelines and
  decodes their DXIL and SPIR-V without launching the game.

## D3D12 pipeline recording

`tessera_d3d12_capture.dll` records compute, graphics (pipeline state streams, including mesh, view
instancing and stream output) and ray tracing pipelines (`CreateStateObject`, `AddToStateObject`), for devices
created through `D3D12CreateDevice` or an Agility SDK factory from `D3D12GetInterface`.

In the app: **Shaders → Record pipelines** launches the game with the recorder temporarily placed next to the
executable; **Import pipeline recording** merges captures; **Prepare shaders** replays them offline to warm the
driver cache. Manually: copy the DLL next to the game as `d3d12.dll`, set `TESSERA_D3D12_CAPTURE` to a new
file, play, then run `--prepare-d3d12-capture`. Legacy graphics stream-output declarations are not captured.
This is a recording tool, not the D3D12 optimization layer (phase 6).

## Vulkan pipeline recording

Build with `-Vulkan`. For the game process, set `VK_LAYER_PATH` to the folder with the layer DLL and manifest
and add `VK_LAYER_TESSERA_telemetry` to `VK_INSTANCE_LAYERS`. The app does this when you use
**Record pipelines** or **Play and verify cache**.

| Variable | Effect |
|---|---|
| `TESSERA_VULKAN_TELEMETRY` | CSV with pipeline creation times, application-cache feedback and present intervals (QPC). |
| `TESSERA_VULKAN_PIPELINE_CAPTURE` | Records compute and graphics pipelines (SPIR-V, layouts, push and specialization constants, core state, render passes, Vulkan 1.2 binding flags and immutable samplers). |
| `TESSERA_VULKAN_CACHE_OUTPUT` | Saves a pipeline-cache snapshot at the first present, or at `TESSERA_VULKAN_CACHE_FRAME`; device teardown saves it if the checkpoint was missed. |
| `TESSERA_VULKAN_CACHE_INPUT` | Seeds that cache on a later launch, only for calls where the game passes no cache of its own. |

`tessera_vulkan_replay <recording> <new-cache>` rebuilds the pipelines on the same driver and adapter.
Preparation saves checkpoints scoped to the recording, game, driver and configuration, and resumes from them;
only a complete replay becomes the prepared cache. Dynamic rendering, graphics pipeline libraries and
extension-specific state are not captured yet. Cache hits prove reuse, not a stutter reduction; that still
needs frame-time measurements.
