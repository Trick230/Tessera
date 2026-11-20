// SPDX-License-Identifier: GPL-3.0-or-later
#include "module.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cwchar>

namespace tessera {
namespace {
struct Range {
    uintptr_t base = 0, end = 0;
    bool game = false;
};
thread_local Range t_cache[4];
thread_local unsigned t_next = 0;

INIT_ONCE g_dir_once = INIT_ONCE_STATIC_INIT;
wchar_t g_game_dir[MAX_PATH];  // exe folder with a trailing '\'
size_t g_game_dir_len = 0;

BOOL CALLBACK init_dir(PINIT_ONCE, void*, void**) {
    const DWORD n = GetModuleFileNameW(nullptr, g_game_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    if (wchar_t* s = wcsrchr(g_game_dir, L'\\')) {
        s[1] = 0;
        g_game_dir_len = wcslen(g_game_dir);
    }
    return TRUE;
}

Range lookup(uintptr_t addr) {
    Range r;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &m))
        return r;  // JIT or generated code: not ours to judge, never cached
    MODULEINFO mi = {};
    if (!K32GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return r;
    r.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    r.end = r.base + mi.SizeOfImage;
    if (m == GetModuleHandleW(nullptr)) {
        r.game = true;
    } else if (g_game_dir_len) {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
        r.game = n > g_game_dir_len && n < MAX_PATH && _wcsnicmp(path, g_game_dir, g_game_dir_len) == 0;
    }
    return r;
}
}  // namespace

bool is_game_code(void* ret) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    for (const Range& r : t_cache)
        if (a >= r.base && a < r.end) return r.game;
    InitOnceExecuteOnce(&g_dir_once, init_dir, nullptr, nullptr);
    const Range r = lookup(a);
    if (r.end) t_cache[t_next++ & 3] = r;
    return r.game;
}
}  // namespace tessera
