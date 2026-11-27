# Tessera — full plan

Design document. It sums up every decision and idea discussed so far.
Whenever something is measured and turns out different from what is written here, update this file.

---

## 1. Goal

Let GPUs with limited VRAM run games that are heavy on video memory, and remove stutter in general:

- **Memory:** bring VRAM use down close to the real *working set* (e.g. FF7 Remake in the sewers, from
  ~11 GB to a realistic 7–8 GB; 6 GB in the best case), with no perceptible quality loss.
- **Zero stutters** from: running out of VRAM, shader/PSO compilation and (as far as possible) traversal.
- **Zero pop-in** (guaranteed in D3D11; imperceptible in D3D12).
- Everything generic: **no manual per-game work**. At most, per-engine rules (UE, Unity, …).

### Physical limit to keep in mind

The minimum "invisible" VRAM is the working set: textures actually seen + render targets + geometry/BVH.
Below that there is either quality loss or pop-in. Tessera must **measure and show** that minimum per game/area.

The savings come from memory that is **resident but unused** (streaming pools full of off-screen textures),
VRAM taken by other processes, duplicates and uncompressed textures.

---

## 2. Architecture

```
┌──────────────────────────┐    files     ┌───────────────────────────────┐
│ tessera.exe (preparation)│ ───────────▶ │ tessera layer (inside game)   │
│ - detects game/API/engine│   caches     │ - d3d11/d3d12/dxgi proxy      │
│ - processes session logs │ ◀─────────── │ - vtable patching             │
│ - runs the helper tools  │   logs       │ - memory, pacing, prefetch    │
└──────────┬───────────────┘              └───────────────────────────────┘
           │ separate processes
           ▼
  tessera_warm.exe (shader warming, copied under the game's exe name)
  DXBC instrumentation in the preparation app
  tessera_texcomp.exe (BC7/BC6H + FLIP)         [optional]
  tessera_d3d12_capture.dll (pipeline recorder + offline replay)
  Vulkan recording layer + tessera_vulkan_replay [optional, recording and replay only]
```

Stack: the layer and the GPU tools are C++20 (CMake + Ninja, clang-cl with ThinLTO, toolchain in `.toolchain/`). The preparation app is Rust
(`crates/core` for the logic, `crates/app` with a Slint UI). Binary formats: the C headers in
`include/tessera` are the source of truth, mirrored in Rust.

### Usage cycle

1. The user opens Tessera → "Prepare". The app:
   - detects the API (exe imports), engine, anti-cheat (refuses), exe hash and driver version (invalidates caches);
   - processes the previous session's logs (shaders, textures, co-occurrence, I/O);
   - warms shaders out of process; installs the DLL (chaining any existing one). From here on the app does not act on the game: it may stay open (or launch a recording session with its layer set up beforehand and wait for it to exit), but it never touches the running process, and installs or removes the layer only while the game is closed.
2. The user launches the game normally (Steam, etc.).
3. The DLL: opens caches by memory-mapping → creates the real device and patches vtables → detects features
   and components (Reflex, frame gen, own streaming, other layers) → preallocates the tile pool.
4. During play: hooks only do work on resource creation; per-frame work in `Present`;
   prefetch thread with low I/O priority.
5. On exit: writes the logs. Nothing stays running.

### DLL installation and chaining

- D3D11 → `d3d11.dll` proxy (or `dxgi.dll`). D3D12 → `d3d12.dll` proxy (a `dxgi.dll` only sees the device through
  the swapchain and may miss resources created earlier).
- If a DLL with the same name already exists (ReShade, DXVK, Special K, OptiScaler):
  rename it to `<name>.next.dll` and load it as the next layer (`next=` in `tessera.ini`).
  Uninstalling restores the original name.
- Patch the vtables of the **real implementation** (obtained when the proxy creates the device), not of wrappers
  such as Streamline (`sl.interposer.dll`).
- Agility SDK: vtable patching works; confirm that the objects patched are the ones the game actually receives.

---

## 3. Optimization catalogue

Format: **realistic** (first version) → **potential** (full version).

### Memory (M)

- **M1 Adjusted budget** — hooks on `DXGI_ADAPTER_DESC::DedicatedVideoMemory` and `QueryVideoMemoryInfo`
  → also NVAPI/AGS; automatic honest mode for games with good streaming (reports the real figure minus what
  Tessera uses).
- **M2 Residency priorities** — D3D11 `SetEvictionPriority`, D3D12 `SetResidencyPriority`: render targets
  and buffers at maximum → dynamic LRU priorities. Off by default (`residency=0`): on NVIDIA driver 621.14
  `SetEvictionPriority` hangs the GPU or crashes FF7R (7/7 runs) — never call it on that driver.
- **M3 Tile pool + tiled/reserved textures** — D3D11.2 tiled resources for read-only textures
  (requested as `IMMUTABLE` → created `DEFAULT`, upload done by Tessera); D3D12 reserved resources
  for committed textures → D3D12 heaps holding only textures, backed virtually. Aliased heaps
  or render targets: never touch.
- **M4 LRU residency under pressure** — while everything fits the budget, do nothing; when it gets tight,
  `SetResourceMinLOD`/`ResourceMinLODClamp` first and only then free the tiles of the largest mip of the
  coldest textures → gradual removal, one mip at a time, with hysteresis.
- **M5 Pop-in-free restore** — removed mips kept in STAGING textures (GPU-readable RAM);
  D3D11: in the bind hook (`*SetShaderResources`), copy the mip **before the draw** (the GPU executes in order);
  cap of synchronous MB per frame (e.g. 32 MB); deferred contexts: restore before `ExecuteCommandList`.
  D3D12: own copy queue + `ID3D12CommandQueue::Wait` before the game's `ExecuteCommandLists`.
  Game writes to managed textures (`UpdateSubresource`, `CopySubresourceRegion`, `GenerateMips`)
  require mapping the tiles first (Tier 2 drops writes to unmapped tiles).
- **M6 Mip feedback** — instrument shaders to record (texture index, requested mip):
  DXBC in D3D11 (simple format, known checksum) → DXIL in D3D12 (DXC, re-sign with `dxil.dll`),
  aggregated per wave. Done during preparation; new shaders pass through uninstrumented.
- **M7 Prediction** — material sets (textures bound together) → learned co-occurrence graph
  + mip margin self-tuned by "late updates".
- **M8 Deduplication** — `IMMUTABLE` textures with the same hash share the object (`AddRef`)
  → copy-on-write for `DEFAULT`.
- **M9 Quality-checked recompression** — RGBA8 → BC7, RGBA16F → BC6H, read-only textures only,
  only if FLIP confirms it is imperceptible; done offline in the app, applied by hash at creation; hooks on `GetDesc`
  and `CreateShaderResourceView` to lie about the format (incl. sRGB) → D3D12 with descriptor swapping.
  Never recompress NTC outputs or unrecognised formats.
- **M10 RAM budget** — removed mips go to RAM: fixed cap → dynamic + LZ4 of the mips in RAM.
- **M11 Working-set meter** — measures the invisible minimum per area → suggests the ideal budget per game.
- **R1 Render target reuse** — big render targets the game releases are kept (fixed cap) and handed back
  on the next create with the same descriptor; avoids free+alloc close to the budget. Off by default
  (`rt_recycle`). Runs below tiled tier 2; no `SetEvictionPriority`. FF7R at 4K: hitches
  >50 ms/min 8.0 → 1.5.

### Shaders (S) — goal: full coverage without the player having to record

Measure cache reuse per shader stage and pipeline on the tested GPU and driver. Verify which root signature and render state changes require compilation.

- **S1 Exhaustive bytecode search** — every file of the install, including the exe and DLLs;
  try zlib, zstd, LZ4, Oodle, Brotli on compressed blocks; validate DXBC/DXIL containers. Encrypted
  archives need a key: Unreal AES keys are found in the executable; the app no longer asks for a key file.
- **S2 Engine PSO caches** — Unreal: `.stable.upipelinecache` (packed inside IoStore containers),
  with full PSO descriptions recorded by the developers. Tessera's own format aligned with
  **SODB** (Advanced Shader Delivery, SQLite, public Microsoft specification).
- **S3 Root signatures** — embedded (RTS0) → exact; serialised blobs in files/exe; per-engine rules
  **automatic capture at startup**: the app launches the game up to the
  menu, the DLL records root signatures and global PSOs, the app closes it. Bindless engines have few root
  signatures → cross them with every shader.
- **S4 Valid pipelines** — NVIDIA: each stage in any valid pipeline (pair by signatures,
  "per-stage coverage"). AMD: VS compiled according to the PS and the state → generate likely format variants.
- **S5 Out-of-process warming** — `tessera_warm.exe` copied under the game's exe name (the cache is
  keyed by the exe name). In-process fallback at startup when the key does not match
  (AMD profiles that swap the key, devices created through AGS). D3D11 (NVIDIA): draw into a 1×1 target with
  zero-count indirect arguments. Never kill the warming process midway; never run it at the
  same time as the game; relaunch isolating suspect items if the device is removed.
- **S6 Self-correction** — the DLL passively records the leftover compilations during normal play;
  the next preparation compiles them. **Coverage meter** in the app (found / compiled / failed).
- **S7 Driver cache** — check that it is set to unlimited (NVIDIA); respect the 16 GiB limit on AMD;
  recompile after a driver update.
- Impossible without running the game: shaders compiled at runtime from HLSL, ray tracing on AMD (only
  exact repeats), encrypted archives without a key, view instancing on NVIDIA.

### Streaming and I/O (I)

- **I1 Read log** — hooks on `CreateFileW`/`ReadFile`, only in the learning session.
- **I2 List prefetch** — thread in the DLL with low I/O priority → bounded by available RAM.
- **I3 Predictive prefetch** — link reads to the texture graph.
- **I4 Instant allocations** — creating a texture = virtual resource + tiles from the preallocated pool.
- **I5 Split upload** — small mips first; full mip in parts.

### Engine (U) — Unreal console variables without calling the engine

- **U1 UE4/UE5 cvars** — the app finds offline, in the executable, the value slot of each cvar (decoded
  registration, int/float/bool type) and writes `ue_image` + `cvar.<name>=<value> <type> <form> <rva>` into
  `tessera.ini`. The DLL checks the build and every slot (VirtualQuery, vtable, pointer), only writes values and
  restores them once a second in `Present`. Rule: writes only, never calls into the engine, scan always
  offline. Cvars read only at startup do not work (the write happens after the first device). Profile
  `ue_streaming` (pool from the VRAM budget, 3 textures/frame) and test variants in the ini
  (`ue_drs_off`, `ue_rt_grow`, `ue_pool_vram`, `ue_shaders_on_load`, `ue_load_limits`), all off by
  default until an A/B shows a gain.

### Pacing and system (P) — all at process level only

- **P1 Precise limiter** — high-resolution timer + final busy-wait. The core (`limiter.cpp`) has no graphics
  API dependency. Default low-latency mode: after a present, hold the next frame's start until the target
  minus the predicted work (p90 of the last 32 frames, stutters excluded) minus an adaptive margin, then align
  the present to the target. After a late frame the cadence restarts from its release (no catch-up burst).
  Modes 0 (wait before `Present`) and 2 (front edge only) stay selectable. When the user sets a cap, it is in
  charge: no other limiter takes precedence (see P6).
- **P2 Flip model + tearing** for D3D11 in borderless windows: convert blit swapchains (`DISCARD` →
  `FLIP_DISCARD`, `SEQUENTIAL` → `FLIP_SEQUENTIAL`, at least 2 buffers) and, with VRR, `ALLOW_TEARING` at
  `SyncInterval 0`. Windowed only, game swapchains only (C3). Exclude: MSAA back buffers,
  sRGB/flip-unsupported formats, child/GDI windows. Keep the flag on `ResizeBuffers`; no tearing in exclusive
  fullscreen. Flip unbinds the back buffer from the OM on every `Present`: restore the game's render targets
  after `Present`. Off by default until validated in real games.
- **P3 Frame latency** — `SetMaximumFrameLatency`, waitable object.
- **P4 CPU sets** — the thread that calls `Present` on P-cores (hybrid CPUs).
- **P5 Process** — timer resolution, EcoQoS off (`SetProcessInformation`), moderate priority.
- **P6 No back-off** — by the author's decision, a cap the user turned on never yields: not to Reflex or
  NVAPI sleep intervals, `VK_NV_low_latency2`, frame generation runtimes, RTSS or the NVIDIA limiter.

### Diagnostics (D)

- **D1** Frame times (QPC per outermost `Present`) + API events, always on and cheap.
- **D2** Stutter report by cause, built in the app: shaders, resource creation, VRAM over budget,
  I/O, pacing, game logic.
- **D3** Optional system test, in the app, outside the game: DPC, disk, throttling.
- **D4** Stats in the RTSS OSD (optional; no own overlay).

### Infrastructure and coexistence (C)

- **C1** Proxy DLL, API detection, clean install/uninstall.
- **C2** Chaining of existing DLLs.
- **C3** Per-module exclusion (return address + `GetModuleHandleEx` at resource creation).
- **C4** Feature detection (`CheckFeatureSupport`) and per-module degradation.
- **C5** Safe mode (crash counter).
- **C6** Cache invalidation by exe hash and driver version.
- **C7** Anti-cheat detection → refuse.
- **C8** Detection of the game's own streaming / NTC → back off on those resources and on the budget.

---

## 4. Out-of-reach problems (warn only)

Antivirus scanning files, third-party overlays, background tasks, exhausted system RAM and pagefile,
driver DPCs, throttling, unstable overclocks/XMP, slow SSD, very high mouse polling,
garbage collection, game logic spikes, DRM, the CPU side of traversal.

---

## 5. Roadmap

| Phase | Includes | Exit criterion |
|---|---|---|
| 0. Foundations | C1, C2, C5, C6, C7, D1 (D3D11) | Game runs the same, cost < 1% |
| 1. Safe wins | M1, M2, P1–P3, P5, P6, S7, basic D2 | Better pacing, no regressions |
| 2. Shaders | S1, S2, S5 (D3D11 NVIDIA + D3D12), S6 + meter | Already in the 1st session: compilation stutters and pop-in from uncompiled PSOs at or near zero (only the impossible cases of section S); zero in the 2nd |
| 3. D3D11 memory | M3, M4, M5, M10, M11, C3, C4 | VRAM going down, zero late restores |
| 4. Prediction and I/O | M7, M8, I1–I5, P4 | Fewer traversal stutters, pop-in ~0 |
| 5. Fine quality | M6 (DXBC), M9 | Extra savings with no FLIP difference |
| 6. D3D12 | M1/M2/P/S → M3/M4 committed → C8 → M6 (DXIL) → M7; S3 startup capture | FF7 Remake: VRAM going down, zero PSO stutters |
| 7. Expansion | D3, D4, M9 in D3D12, virtual heaps, AMD (S4) | Works with popular mods/overlays |

Test games: a VRAM-heavy D3D11 game (phases 0–5) — FF7 Remake forced to D3D11 with `-dx11` so far;
FF7 Remake (D3D12) in phase 6.

### Status (2026-11-24)

The user asked to start the optimizations before the Phase 0 gate was formally closed, so phases 0–5 were
built side by side in D3D11, with pipeline recording for D3D12 and Vulkan added after. Everything below is on
`master`.

**Done (D3D11 layer, x64 and x86):**
- Phase 0: C1, C2, C5, C6, C7, D1 — proxy with chaining, safe mode, session recording, app
  (Rust + Slint) with discovery (Steam/Epic/game and library folders, recompiled games), API/engine/anti-cheat
  detection, install/uninstall. Overhead measured with `tools/measure_overhead.ps1`. 32-bit D3D11 games
  supported (separate x86 layer and warm-up).
- Phase 1: M1 (DXGI + NVAPI budget/description hooks, also before the first device), M2 (off by default),
  P1 limiter with cadence reset after late frames, P2 flip model (off by
  default), P3, P5, P6, S7, basic D2, optional slow D3D11 context call records.
- Phase 2: S1 across engines — Unreal PAK (legacy and modern indexes), IoStore (incl. encrypted blocks),
  shader libraries up to UE 5.8, AES key detected in the executable; Unity bundles, WebGL archives, APK, SMOL-V;
  recompiled games' embedded shaders and disk caches. Bytecode exported per API (D3D11, D3D12, Vulkan,
  OpenGL). S5 `tessera_warm.exe` (watchdog that never kills, incremental warm-up of pending shaders, geometry
  shaders and pixel shaders paired with compatible game vertex shaders), S6, coverage meter, NVIDIA cache
  status and per-game cache clearing in the app.
- Phase 3: M3/M4/M5/M10 mip residency (tiled tier 2 only, off by default), M11 working-set meter on the
  presenting device, C3 per-module exclusion, deferred-context support, device replacement takeover,
  opt-in feature level 11_1 request (`d3d11_prefer_11_1`).
- Phase 4 (off by default): P4 performance-core CPU sets, M8 immutable texture dedup, I1–I3 file-range learning
  and prefetch with material association, M7 co-occurrence prediction, I4–I5 split uploads throttled by a
  GPU fence.
- Phase 5 (off by default): M6 DXBC mip feedback (offline instrumentation, one sampled draw per 60 frames),
  M9 BC7/BC6H recompression checked with FLIP (`tessera_texcomp.exe`): DirectXTex DirectCompute encoder on the
  GPU (2048² BC7: 554 s on the CPU → 3.1–3.5 s; 1024²: 137–141 s → ~1 s, RTX 4060), CPU encoder as fallback
  or with `--cpu`. Its shaders are built by `tessera_hlsl_header` (d3dcompiler_47; output matched fxc 10.0.26100 here).
- Extra: R1 render target reuse, U1 Unreal cvars (+ test variants), SLOW_CREATE records,
  `tools/global_test.ps1` A/B harness.
- P1 low-latency limiter (`limiter_mode=1`, default). Synthetic bench (`--limiter-bench`, noisy load with
  stutters and a load step, 60 FPS): mean latency 16.5 → 10.8 ms against waiting before `Present`, cadence p99
  1.4 ms, no misses, no short frames after stutters; at 120 FPS −2.3 ms. The front-edge mode is 2.3 ms lower
  but with 4 ms cadence p99 and ~5% missed intervals.
- D3D12 games: the same layer built as `dxgi.dll` (`tessera_dxgi.dll`, x64 and x86) hooks swap chains from
  DXGI factories; the limiter, P5/P6 and session recording work there (same bench numbers on a D3D12 flip
  swap chain). The app installs it for games that use D3D12 and not D3D11. This is not yet the phase 6 layer
  (no D3D12 memory or residency work; M1 stays out of it entirely).
- Vulkan games: the same layer built as `vulkan-1.dll` (`tessera_vulkan.dll`, x64 and x86) wraps
  `vkQueuePresentKHR` from the export and from `vkGet*ProcAddr`; limiter, P5 and session recording only.
  Same bench numbers as D3D11 (`tessera_vulkan_present`, in the smoke test when built). The app installs it
  for Vulkan-only games. Open: export coverage of the dxgi/vulkan proxies on other Windows builds, a game
  test on each API.

**Pipeline recording (towards phase 6, S2/S3):**
- D3D12: `tessera_d3d12_capture.dll` records compute, graphics (pipeline state streams) and ray tracing state
  objects, including Agility device factories; the app imports recordings and replays them offline.
  Recordings (format v3) carry the game build (exe size and write time): a patched game starts a new one.
  `AddToStateObject` records name their parent by hash. Replay keeps partial results and relaunches past a
  driver crash, skipping the pipeline that caused it.
- Vulkan: an optional layer (`-Vulkan`) records compute and core graphics pipelines and cache feedback;
  `tessera_vulkan_replay` rebuilds them into a driver cache with resumable checkpoints (saved by elapsed
  time). The layer's present hook takes no lock and saves its cache checkpoint off-thread. Added for recompiled
  games that only ship Vulkan/D3D12 renderers. Its scope is recording and offline replay; the Vulkan frame
  limiter is the separate `vulkan-1.dll` proxy above, and no other Vulkan optimization is planned before D3D12.
- The app has **Record pipelines**, **Import pipeline recording** and **Play and verify cache**.

**Findings that shape the next steps (FF7R, D3D11, RTX 4060, NVIDIA 621.14):**
- FF7R requests feature level 11_0, where this GPU reports tiled tier 1; 11_1 reports tier 2. Mip residency
  also needs `IMMUTABLE` textures and FF7R creates `DEFAULT` ones, and it peaked at 6044 of 6763 MiB, so neither
  eligibility nor memory pressure is proven there.
- `SetEvictionPriority` crashes or hangs the driver (M2 and an earlier R1 version).
- `ue_streaming` at a 60 cap: 47.8 / 51.5 → 59.8 / 59.8 displayed FPS, p99 26.5 / 25.4 → 23.2 / 23.1 ms.
- Remaining stutter: streaming texture creates with initial data on 4 worker threads (100–240 ms each, driver
  contention), plus 4K render target churn every 10–12 s (R1 cut >50 ms hitches from 8.0 to 1.5 per minute).
- Phase 4 in FF7R: material tagging went from 0 to 3039 ranges and prefetch issued 42.6 MiB; a reduction
  in stutter is not shown yet (the comparison had no frame over 33.3 ms in any run).
- Phase 5 in FF7R: 98 pixel shaders instrumented, but only 5 mip samples, all before the test route;
  the 12,262 textures created in the route were all `DEFAULT` and already BC (no dedup or recompression input).

**What is left:**
1. U1 A/B in FF7R (not run yet): base `ue_streaming` vs `ue_drs_off` vs `ue_rt_grow` (2 rounds), then
   `ue_pool_vram`, `ue_shaders_on_load`, `ue_load_limits`. Promote what helps to defaults/app options.
2. Decide whether `ue_streaming` and R1 become defaults for Unreal games (both measured as gains on FF7R).
3. Open M1 design points: write `vram_budget_mb` only for D3D11 games; `g_other_api` is sticky (a D3D11 game
   probing D3D12 loses M1); volk is not detected.
4. Phase 2 exit criterion still needs a first-session play test; measure whether D3D12/Vulkan replay removes
   in-game compilation stutters (frame times, not only cache hits).
5. Phase 3/5 need an eligible game: tier 2 with `IMMUTABLE` textures and real memory pressure (or test
   `d3d11_prefer_11_1` in FF7R). Find why active route shaders get no mip sample before adding opcodes.
6. Phase 4: A/B on a route with reproducible traversal stutter; check prefetch arrives before demand.
   P4 needs a hybrid CPU. Dedup for `DEFAULT` textures would need copy-on-write.
7. Phase 6: native D3D12 layer — proxy and installer with correct device/queue/Present/Agility handling, then
   residency, C8 back-off, DXIL feedback and prediction.
8. Deferred with reasons: mip-by-mip eviction (needs per-mip tile layout), LZ4 of evicted mips (decode on the
   bind would stall; needs predictive restore first), AGS (AMD), safe mode does not cover a crash in the IAT
   patch before the first device, Vulkan dynamic rendering / pipeline libraries.

### Phase 4 — preview in D3D11

These settings are off by default. Use the app's Textures tab for streaming options and Settings for
performance cores. The following keys can also be set in the game's `[tessera]` section:

- **P4** `present_cpu_sets=1` selects performance cores for the presenting thread on a hybrid CPU, when the game has not already selected CPU sets.
- **M8** `texture_dedup=1` shares matching immutable shader textures on the same device. It verifies every pixel or compressed block, excluding row padding. The preview covers single-slice BC and 8-bit RGBA/BGRA textures up to 2 MiB. The session log counts eligible textures, avoided creates, and bytes avoided.
- **I1–I3** `stream_io=1` records file ranges from a learning session. `stream_io=2` reuses the trace for bounded startup prefetch at low I/O priority. Add `mip_prediction=1` in both sessions to associate reads with sampled materials and prefetch matching ranges during play. Material prefetch starts with a limited share of the session budget and releases the reserve gradually. Replay skips ranges fully covered by a recent read and prioritizes ranges followed by substantial demand in previous replays of the same trace. The app sets the install folder relative to the executable. A manual `stream_root` overrides it. The trace is keyed by the executable path and version, and playback checks the asset file's size and modification time.
- **M7** `mip_prediction=1` learns which textures are bound together. With `mip_residency=1` and tiled resources Tier 2, it can restore likely partners in advance when VRAM has room. Mip prediction uses the workset bind hooks.
- **I4–I5** `mip_streaming=1` with `mip_residency=1` reuses an exact-size spare tile pool when available. It uploads the smallest mips first, with up to 2 MiB of speculative copies per Present. Each Present can attempt one split upload or one predicted restore. A swap-chain-owned D3D11 fence measures bytes copied from staging that have yet to complete. An 8 MiB window throttles split uploads, and predicted restores wait until the tracked copies finish. First use still queues the rest before the draw. A predicted restore larger than the window can run by itself when idle. Without a working fence the fixed per-Present limit still applies.

The targeted smoke test is `powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_phase4.ps1` after building the `d3d11_smoke` target. Traversal benefits in a real game and performance-core assignment on hybrid hardware still need validation.

### Phase 5 — D3D11 quality preview

- **M6:** `mip_feedback=1` samples up to one compatible immediate-context draw per 60 Presents, looking across several frames with a fixed attempt budget. Preparation instruments the first supported implicit or explicit-level sample per statically indexed 2D texture slot in recorded `ps_5_0` shaders, up to eight slots. The layer skips existing UAVs and draw states with an occupied output slot 7, restores the shader and bindings after the sample, and reads the buffer without waiting. The session records the texture fingerprint and requested mip.
- **M9:** `texture_quality=1` records up to 32 eligible immutable textures within a 32 MiB capture budget. Preparation converts opaque RGBA8 or nonnegative RGBA16F to BC7 or BC6H and checks every mip with FLIP and channel bounds. `texture_quality=2` applies a prepared replacement only when the input pixels match the captured data. Texture and view descriptions keep the original format. A copy from the resource uses the exact original pixels. Textures that need that fallback retain the original allocation for subsequent copies.

Both options are off by default. Build with `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Quality`.
In the Textures tab, select Capture and play a session if testing recompression. With the game closed, select
Prepare quality. The tab shows captured, approved and applied textures. Apply becomes available once textures
are approved. Mip feedback becomes available once compatible shaders are prepared.
For scripts, use `& .\build\windows-x64\cargo\release\tessera.exe --prepare-quality "<game executable>" | Out-Host` and
`cargo run -p tessera-core --example session -- <file.tsess> --mips` to inspect the results.
Real game impact still needs measurement.

On Windows, build the optional targets and run the focused test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Quality
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_quality.ps1
```

### Phase 0 — concrete tasks

1. Repository: root CMake, `src/dll`, `src/warm`, `include/tessera` (formats), `crates/` (app), `tools/`, `tests/`.
2. `tessera.dll` as a `d3d11.dll` proxy: forward every export to the system DLL
   (`GetSystemDirectoryW`) or to the `next=` of `tessera.ini`.
3. Vtable patching utility (save the original, `VirtualProtect`, reentrancy guard).
4. Logging hooks: `CreateTexture2D`, `CreateBuffer`, `CreateShaderResourceView`, `Present`.
5. Binary session log in `%LOCALAPPDATA%\Tessera\` (resources, frame times).
6. Safe mode: start/crash counter.
7. Minimal app: game list from a chosen folder, API detection from imports, anti-cheat detection
   (known files: EasyAntiCheat, BattlEye, etc.), install/uninstall with chaining.
8. Measure: DLL overhead vs no DLL; report of where the VRAM goes.

---

## 6. Hypotheses to validate (measure, don't assume)

- Real cost of `UpdateTileMappings` and of sampling tiled resources on each driver.
- Cost of shader instrumentation (target: a few %).
- D3D11 on AMD: can it be warmed by recording full state combinations?
- Acceptance of modified DXIL (re-signing with `dxil.dll`).
- Share of tileable textures in real D3D11 games.
- How much of the UE4 streaming pool is off screen in FF7 Remake's sewers.
- FF7R: does dynamic resolution resizing with `r.SceneRenderTargetResizeMethod=0` cause the 4K render target
  churn? Test `ue_drs_off` and `ue_rt_grow`. Caveat: the recreated targets had the same descriptor.

---

## 7. References

- **CUE4Parse** (Unreal pak/IoStore reader, C# — port the logic), **ree-pak-rs** (RE Engine).
- **Advanced Shader Delivery / SODB**: https://microsoft.github.io/DirectX-Specs/d3d/StateObjectDatabase.html
- **Unreal bundled PSO caches**: Epic's documentation on `.upipelinecache` / `.spc` / `.shk`.
- **NVIDIA RTX Neural Texture Compression** (RTX Kit): Tessera coexists, does not replace.
- **Zenardi/ff7r-pc-optimization**: FF7R Engine.ini profiles (no measurements); useful parts are
  `r.DynamicRes.OperationMode=0`, `r.Streaming.LimitPoolSizeToVRAM=1`, `r.CreateShadersOnLoad=1`.
- Libraries: xxHash, Slint, bc7enc_rdo, NVIDIA FLIP, DXC, Catch2.
- Technique references (study only): DXVK, Special K, ReShade.
