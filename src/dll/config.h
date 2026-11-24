// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace tessera {
// Per-game settings from [tessera] in tessera.ini next to the layer. Every
// optimization has its own switch so a misbehaving one can be turned off alone.
struct Config {
    bool process = true;        // P5: EcoQoS off, 1 ms timer resolution
    bool priority = true;       // P5: above-normal priority when the game runs at normal
    bool present_cpu_sets = false;  // P4: select performance cores for the presenting thread
    bool residency = true;      // M2: eviction priority of render targets and buffers
    int fps_limit = 0;          // P1: frame limiter, 0 = off
    int ext_fps_limit = 0;      // P1: lowest limit of another tool (RTSS, NVIDIA), written by the app
    int limiter_mode = 1;       // P1: 0 wait before Present, 1 low latency, 2 wait at frame start
    int max_frame_latency = 0;  // P3: DXGI maximum frame latency, 0 = leave the game's
    bool d3d11_call_diagnostics = false;
    int vram_budget_mb = 0;     // M1: highest VRAM budget reported to the game, 0 = the real one
    bool flip_model = false;    // P2: windowed blit swapchains become flip model (+ tearing)
    bool d3d11_prefer_11_1 = false;  // experiment: try feature level 11_1 before the game's 11_0 request
    bool workset = true;        // M11: texture working-set meter (bind hooks)
    bool mip_residency = false; // M3-M5: cold top mips of sampled textures move to system memory
    int mip_ram_mb = 0;         // M10: system memory for those mips, 0 = min(4 GB, RAM / 16);
                                // lower while less than RAM / 8 (at least 1 GB) is free
    bool mip_force_evict = false;  // tests: evict every texture not bound in the current frame
    bool texture_dedup = false;  // M8: share identical immutable shader textures
    int stream_io = 0;           // I1-I3: 1 learn reads, 2 prefetch a saved trace
    bool stream_io_diagnostics = false;  // Record demand and accepted prefetch ranges.
    bool mip_prediction = false;  // M7: predict texture use
    bool mip_feedback = false;    // M6: sample prepared pixel shaders at runtime
    bool mip_feedback_diagnostics = false;  // Record the pixel shaders skipped by feedback sampling.
    int texture_quality = 0;      // M9: 1 = capture textures, 2 = use prepared replacements
    bool mip_streaming = false;   // I4-I5: spare tile pool and split background uploads
    bool rt_recycle = false;    // R1: released big render targets handed back on the next matching create
    bool ue_cvars = true;       // U1: Unreal console variables from cvar.* lines (written by the app)
};

// Reads tessera.ini once, whoever calls first (session init or the early
// factory hook, never DllMain); later calls do nothing.
void config_load();
// Writes the settings to the log (after log_open).
void config_log();
// Valid after config_load; defaults before.
const Config& config();
// Full path of tessera.ini (empty before config_load).
const wchar_t* config_ini_path();
}  // namespace tessera
