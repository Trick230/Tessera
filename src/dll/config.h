// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace tessera {
// Per-game settings from [tessera] in tessera.ini next to the layer. Every
// optimization has its own switch so a misbehaving one can be turned off alone.
struct Config {
    bool process = true;        // P5: EcoQoS off, 1 ms timer resolution
    bool residency = true;      // M2: eviction priority of render targets and buffers
    int fps_limit = 0;          // P1: frame limiter, 0 = off
    int max_frame_latency = 0;  // P3: DXGI maximum frame latency, 0 = leave the game's
    int vram_budget_mb = 0;     // M1: highest VRAM budget reported to the game, 0 = the real one
};

// Reads tessera.ini once (from session init, never DllMain).
void config_load();
// Valid after config_load; defaults before.
const Config& config();
}  // namespace tessera
