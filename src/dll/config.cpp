// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <windows.h>

#include <cwchar>

#include "log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace tessera {
namespace {
Config g_config;
wchar_t g_ini[MAX_PATH];

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK load_once(PINIT_ONCE, void*, void**) {
    wchar_t ini[MAX_PATH];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), ini, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    wchar_t* s = wcsrchr(ini, L'\\');
    if (!s || static_cast<size_t>(s - ini) + 13 >= MAX_PATH) return TRUE;
    wcscpy_s(s + 1, MAX_PATH - (s + 1 - ini), L"tessera.ini");

    auto get = [&](const wchar_t* key, int def) {
        return static_cast<int>(GetPrivateProfileIntW(L"tessera", key, def, ini));
    };
    Config c;
    c.process = get(L"process", 1) != 0;
    c.priority = get(L"priority", 1) != 0;
    c.present_cpu_sets = get(L"present_cpu_sets", 0) != 0;
    // M2 off by default: in FF7R (NVIDIA 621.14) it crashed the driver (AV in
    // nvwgf2umx, render thread hang, device removed) in 7 of 7 runs.
    c.residency = get(L"residency", 0) != 0;
    c.fps_limit = clamp_int(get(L"fps_limit", 0), 0, 1000);
    if (c.fps_limit && c.fps_limit < 20) c.fps_limit = 0;  // nonsense values: off
    c.ext_fps_limit = clamp_int(get(L"ext_fps_limit", 0), 0, 1000);
    c.max_frame_latency = clamp_int(get(L"max_frame_latency", 0), 0, 16);
    c.d3d11_call_diagnostics = get(L"d3d11_call_diagnostics", 0) != 0;
    c.vram_budget_mb = clamp_int(get(L"vram_budget_mb", 0), 0, 1 << 20);
    if (c.vram_budget_mb && c.vram_budget_mb < 1024) c.vram_budget_mb = 0;  // below 1 GB: a typo, off
    c.flip_model = get(L"flip_model", 0) != 0;
    c.d3d11_prefer_11_1 = get(L"d3d11_prefer_11_1", 0) != 0;
    c.workset = get(L"workset", 1) != 0;
    c.mip_residency = get(L"mip_residency", 0) != 0;
    c.mip_ram_mb = clamp_int(get(L"mip_ram_mb", 0), 0, 1 << 20);
    c.mip_force_evict = get(L"mip_force_evict", 0) != 0;
    c.texture_dedup = get(L"texture_dedup", 0) != 0;
    c.stream_io = clamp_int(get(L"stream_io", 0), 0, 2);
    c.mip_prediction = get(L"mip_prediction", 0) != 0;
    c.mip_feedback = get(L"mip_feedback", 0) != 0;
    c.texture_quality = clamp_int(get(L"texture_quality", 0), 0, 2);
    c.mip_streaming = get(L"mip_streaming", 0) != 0;
    c.rt_recycle = get(L"rt_recycle", 0) != 0;
    c.ue_cvars = get(L"ue_cvars", 1) != 0;
    g_config = c;
    wcscpy_s(g_ini, ini);
    return TRUE;
}
}  // namespace

// Hooks read the settings concurrently: they are written once, never again.
void config_load() { InitOnceExecuteOnce(&g_once, load_once, nullptr, nullptr); }

void config_log() {
    const Config& c = g_config;
    logf("config: process %d priority %d residency %d fps_limit %d ext_fps_limit %d max_frame_latency %d vram_budget_mb %d "
         "flip_model %d workset %d mip_residency %d mip_ram_mb %d mip_force_evict %d rt_recycle %d ue_cvars %d "
         "present_cpu_sets %d texture_dedup %d stream_io %d mip_prediction %d mip_streaming %d mip_feedback %d texture_quality %d",
         c.process, c.priority, c.residency, c.fps_limit, c.ext_fps_limit, c.max_frame_latency, c.vram_budget_mb,
         c.flip_model, c.workset, c.mip_residency, c.mip_ram_mb, c.mip_force_evict, c.rt_recycle, c.ue_cvars,
         c.present_cpu_sets, c.texture_dedup, c.stream_io, c.mip_prediction, c.mip_streaming, c.mip_feedback, c.texture_quality);
    if (c.fps_limit > 0 && c.ext_fps_limit > 0 && c.ext_fps_limit <= c.fps_limit)
        logf("limiter: yielding to the configured external cap");
}

const Config& config() { return g_config; }
const wchar_t* config_ini_path() { return g_ini; }
}  // namespace tessera
