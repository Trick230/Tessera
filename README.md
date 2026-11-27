<div align="center">

# Tessera

**Prepares Windows games before you play so they stutter less and use less video memory, with no visible loss in quality.**

[![ci](https://github.com/Trick230/Tessera/actions/workflows/ci.yml/badge.svg)](https://github.com/Trick230/Tessera/actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078d4)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

</div>

Stutters come from shaders compiled the first time something appears, data loaded while you move through the world, running out of video memory and uneven frame pacing. Tessera does the heavy work in a small app before the game starts; while you play, only a light graphics layer runs, loaded by the game itself like any other DLL in its folder. Nothing is injected, nothing stays in the background, and nothing outside the game is changed.

> [!IMPORTANT]
> Early development. Tested on real Direct3D 11 games (mainly Final Fantasy VII Remake). Most optimizations are off by default until measurements show they help. See [Status](#status).

## Highlights

- **Shaders ready before the first frame.** Finds the game's shaders on disk, or records them while you play, and warms the driver's cache ahead of time.
- **Smoother traversal.** Learns which files the game reads together and loads them early, at low priority.
- **Steady frame times.** A precise frame limiter, lower input latency and per-game scheduling, in Direct3D 11, Direct3D 12 and Vulkan games.
- **Less video memory, no pop-in.** Keeps memory close to what is on screen; removed texture detail comes back before it is drawn.
- **Quality is never traded silently.** A lossy step, such as recompressing a texture, is used only if the [FLIP](https://github.com/NVlabs/flip) metric confirms the difference is invisible.
- **Every session explained.** Frame time percentiles, stutters per minute, video memory used and the likely cause.

## Status

| Area | State |
| --- | --- |
| App: library, detection, install/uninstall, session panel | Works |
| Shader discovery and warming (Unreal, Unity, recompiled games, D3D11/D3D12/Vulkan bytecode) | Works; first-session stutter test still to do |
| D3D12 and Vulkan pipeline recording and offline replay | Works as a tool; game benefit not yet measured |
| Frame limiter (D3D11, D3D12, Vulkan), process tweaks, frame latency (D3D11) | Works on a synthetic bench; game test still to do |
| Unreal texture streaming profile | 47.8 / 51.5 → 59.8 FPS at a 60 cap; off by default |
| Render target reuse | Hitches over 50 ms: 8.0 → 1.5 per minute at 4K; off by default |
| Texture memory saving (tiled textures, deduplication, recompression) | Tested synthetically; needs a game and GPU that qualify |
| File prefetch and texture prediction | Prefetch works in game; stutter reduction not yet shown |
| Direct3D 12 optimization layer | Not started (recording tools exist) |

Game numbers: Final Fantasy VII Remake on an RTX 4060 (Direct3D 11), from Tessera's session recordings with the feature off and on. Findings and what is left: [`docs/PLAN.md`](docs/PLAN.md).

## Installation

There are no prebuilt downloads yet. Building needs nothing installed: the toolchain (clang-cl, Rust, CMake, Ninja, Windows SDK) is downloaded into the repository folder.

```powershell
git clone https://github.com/Trick230/Tessera.git
cd Tessera
powershell -File tools/toolchain.ps1 -AcceptLicense   # once, ~2.7 GB in .toolchain\
powershell -File build.ps1                            # -> build\windows-x64\cargo\release\tessera.exe
```

Needs Windows 10 or 11 (x64) and the [Visual C++ Redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) for x64, which most PCs with games already have. `-AcceptLicense` accepts the Microsoft license of the runtime and SDK files the toolchain downloads. Building from Linux and the optional tools: [`docs/ADVANCED.md`](docs/ADVANCED.md).

## Usage

1. **Open `tessera.exe`.** Steam and Epic games appear in the **Library**; add other folders in **Settings**.
2. **Pick a game.** The app shows its graphics API, engine and anti-cheat.
3. **Shaders → Prepare shaders.** Run it again after a game or driver update; the app tells you when it is out of date.
4. **Install layer** (optional). Settings in the **Session**, **Textures** and **Settings** tabs apply the next time the game starts.
5. **Play as usual**, from any launcher, then close the game and open the **Session** tab.

**Uninstall** puts the game folder back as it was. Direct3D 11 games get every feature; in Direct3D 12 games the layer runs the frame limiter and session recording, in Vulkan games the frame limiter. Options and command-line tools: [`docs/ADVANCED.md`](docs/ADVANCED.md).

## How it works

1. **Prepare:** `tessera.exe` (Rust + Slint) detects the game's API, engine and anti-cheat, reads the last session and builds caches. It never touches a running game.
2. **Install:** the layer goes into the game folder as `d3d11.dll`, `dxgi.dll` or `vulkan-1.dll` and forwards everything to the system DLL.
3. **Play:** the game loads the layer itself. It intercepts calls only by patching vtables (no inline hooks in system DLLs, no injection) and reads the caches memory-mapped.
4. **Review:** the layer writes session logs; the app reads them the next time it opens.

Heavy helpers (shader warm-up, pipeline replay, texture recompression) run as separate processes, so a driver crash cannot take the app down.

## Safety

- **Anti-cheat games are never modified.** If Tessera finds EasyAntiCheat, BattlEye or a similar system, it only reads the game's files.
- **Only the game is affected, only while it runs.** Scheduling, timer and priority changes apply to the game's process alone.
- **It coexists.** A DLL already in the game folder (ReShade, DXVK, Special K, OptiScaler) is loaded next, not replaced, and restored on uninstall.
- **It fails safely.** An unsupported or failing feature turns off alone; after two crashes in a row the layer disables itself.
- **No network code** in the layer. The app and the layer only exchange files in `%LOCALAPPDATA%\Tessera`.

## Development

```powershell
powershell -File build.ps1 -Test        # build, Rust unit tests, D3D11 smoke tests (x64 and x86)
powershell -File build.ps1 -Quality     # also the texture recompression tool
powershell -File build.ps1 -Vulkan      # also the Vulkan recording and replay tools
```

| Path | Contents |
| --- | --- |
| `crates/core`, `crates/app` | Rust: detection, scanning, install, sessions / the Slint app |
| `src/dll` | C++20: the graphics layer |
| `src/warm`, `src/d3d12_capture`, `src/vulkan_telemetry` | shader warm-up, D3D12 and Vulkan recording and replay |
| `src/texcomp` | BC7/BC6H recompression checked with FLIP |
| `include/tessera` | binary formats shared by the layer, the tools and the app |
| `tests`, `tools` | smoke, quality and replay tests; toolchain and test scripts |

CI builds and tests on Windows, and cross-builds from Linux, on every push. Design and catalogue: [`docs/PLAN.md`](docs/PLAN.md).

## Limitations

- Memory, texture and presentation optimizations are Direct3D 11 only.
- Games with anti-cheat get no layer, so only shader preparation applies.
- No prebuilt release yet; Windows on ARM is untested.

## Acknowledgements

[DirectXTex](https://github.com/microsoft/DirectXTex), [FLIP](https://github.com/NVlabs/flip),
[Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), [Slint](https://slint.dev),
[Skia](https://skia.org), [LLVM](https://llvm.org) and [xwin](https://github.com/Jake-Shadle/xwin).

## License

Released under the [GNU General Public License v3.0 or later](LICENSE).
