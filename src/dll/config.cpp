// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <windows.h>

#include <cwchar>

#include "log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace tessera {
namespace {
Config g_config;

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

void config_load() {
    wchar_t ini[MAX_PATH];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), ini, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t* s = wcsrchr(ini, L'\\');
    if (!s || static_cast<size_t>(s - ini) + 13 >= MAX_PATH) return;
    wcscpy_s(s + 1, MAX_PATH - (s + 1 - ini), L"tessera.ini");

    auto get = [&](const wchar_t* key, int def) {
        return static_cast<int>(GetPrivateProfileIntW(L"tessera", key, def, ini));
    };
    Config c;
    c.process = get(L"process", 1) != 0;
    c.residency = get(L"residency", 1) != 0;
    c.fps_limit = clamp_int(get(L"fps_limit", 0), 0, 1000);
    if (c.fps_limit && c.fps_limit < 20) c.fps_limit = 0;  // nonsense values: off
    c.max_frame_latency = clamp_int(get(L"max_frame_latency", 0), 0, 16);
    c.vram_budget_mb = clamp_int(get(L"vram_budget_mb", 0), 0, 1 << 20);
    if (c.vram_budget_mb && c.vram_budget_mb < 1024) c.vram_budget_mb = 0;  // below 1 GB: a typo, off
    c.flip_model = get(L"flip_model", 0) != 0;
    g_config = c;
    logf("config: process %d residency %d fps_limit %d max_frame_latency %d vram_budget_mb %d flip_model %d", c.process,
         c.residency, c.fps_limit, c.max_frame_latency, c.vram_budget_mb, c.flip_model);
}

const Config& config() { return g_config; }
}  // namespace tessera
