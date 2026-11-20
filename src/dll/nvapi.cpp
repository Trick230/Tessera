// SPDX-License-Identifier: GPL-3.0-or-later
// NVAPI, seen from the game's side. NVAPI has one export, nvapi_QueryInterface,
// which the game resolves with GetProcAddress, usually before it creates a
// D3D device. The game's import of GetProcAddress is patched when the layer
// loads (chaining the previous pointer), and nvapi_QueryInterface is handed
// back wrapped; every other name and id goes to the original untouched.
//   M1: NvAPI_GPU_GetMemoryInfo reports at most the budget the game is given.
//   P6: NvAPI_D3D_SetSleepMode with low latency on = Reflex: the limiter backs off.
#include "nvapi.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "memory.h"
#include "module.h"
#include "pacing.h"
#include "session.h"

namespace tessera {
namespace {
using NvStatus = int;
using QueryInterface = void*(__cdecl*)(unsigned);
using GetMemoryInfo = NvStatus(__cdecl*)(void*, uint32_t*);
using SetSleepMode = NvStatus(__cdecl*)(IUnknown*, const uint8_t*);
using GetProc = FARPROC(WINAPI*)(HMODULE, LPCSTR);

// Ids from the public NVAPI interface table.
constexpr unsigned kGetMemoryInfo = 0x07F9B368;
constexpr unsigned kSetSleepMode = 0xAC1CA9E0;

GetProc g_get_proc = nullptr;  // written once in DllMain, before the patch
std::atomic<QueryInterface> g_qi{nullptr};
std::atomic<GetMemoryInfo> g_mem{nullptr};
std::atomic<SetSleepMode> g_sleep{nullptr};

constexpr uint32_t nv_version(uint32_t size, uint32_t v) { return size | (v << 16); }

// NV_DISPLAY_DRIVER_MEMORY_INFO V1-V3 (KB): version, dedicated, available
// dedicated, system, shared, [V2+] current available dedicated, ...
NvStatus __cdecl hk_get_memory_info(void* gpu, uint32_t* info) {
    const NvStatus st = g_mem.load(std::memory_order_relaxed)(gpu, info);
    // Only once our device exists (the session is up) and only for the
    // game's own calls: SDKs and overlays keep the real numbers.
    if (st != 0 || !info || !memory_device_seen() || !is_game_code(_ReturnAddress())) return st;
    const uint32_t v = info[0];
    const bool v1 = v == nv_version(20, 1), v2 = v == nv_version(24, 2), v3 = v == nv_version(32, 3);
    if (!v1 && !v2 && !v3) return st;
    const uint64_t dedicated = static_cast<uint64_t>(info[1]) << 10;
    const uint64_t told = memory_reported_for(0x10DE, dedicated);
    if (told >= dedicated) return st;
    const uint32_t told_kb = static_cast<uint32_t>(told >> 10);
    if (!v1) {
        // Keep what the game already uses; current available is measured
        // against available dedicated: used = available - current available.
        const uint32_t used = info[2] > info[5] ? info[2] - info[5] : 0;
        const uint32_t free_kb = told_kb > used ? told_kb - used : 0;
        if (info[5] > free_kb) info[5] = free_kb;
    }
    info[1] = told_kb;
    if (info[2] > told_kb) info[2] = told_kb;
    return st;
}

// NV_SET_SLEEP_MODE_PARAMS V1: version, bLowLatencyMode (byte at 4), ...
NvStatus __cdecl hk_set_sleep_mode(IUnknown* dev, const uint8_t* p) {
    const NvStatus st = g_sleep.load(std::memory_order_relaxed)(dev, p);
    // Reflex needs a device; ours existing means the session is already up.
    if (st == 0 && p && p[4] && memory_device_seen() && session_init())
        pacing_backoff("Reflex (NVAPI low latency mode)");
    return st;
}

void* __cdecl hk_query_interface(unsigned id) {
    void* const p = g_qi.load(std::memory_order_relaxed)(id);
    if (!p) return p;
    // The first pointer the game gets for each id is the one we chain to;
    // NVAPI returns the same pointer for an id every time.
    if (id == kGetMemoryInfo) {
        GetMemoryInfo expected = nullptr;
        g_mem.compare_exchange_strong(expected, reinterpret_cast<GetMemoryInfo>(p));
        return reinterpret_cast<void*>(&hk_get_memory_info);
    }
    if (id == kSetSleepMode) {
        SetSleepMode expected = nullptr;
        g_sleep.compare_exchange_strong(expected, reinterpret_cast<SetSleepMode>(p));
        return reinterpret_cast<void*>(&hk_set_sleep_mode);
    }
    return p;
}

FARPROC WINAPI hk_get_proc_address(HMODULE m, LPCSTR name) {
    const FARPROC p = g_get_proc(m, name);
    // HIWORD 0: lookup by ordinal, no name to read.
    if (!p || !(reinterpret_cast<uintptr_t>(name) >> 16) || name[0] != 'n' ||
        std::strcmp(name, "nvapi_QueryInterface") != 0)
        return p;
    QueryInterface expected = nullptr;
    g_qi.compare_exchange_strong(expected, reinterpret_cast<QueryInterface>(p));
    // A second nvapi64.dll (unlikely) would not match what we chain to.
    if (g_qi.load(std::memory_order_relaxed) != reinterpret_cast<QueryInterface>(p)) return p;
    return reinterpret_cast<FARPROC>(&hk_query_interface);
}
}  // namespace

void nvapi_patch_imports() {
    // The exe will call into us from now on: stay loaded even if the game
    // frees d3d11.dll after a probe. Safe under the loader lock.
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&hk_get_proc_address), &self))
        return;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;  // no name table to match against
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "GetProcAddress") != 0) continue;
            auto* const current = reinterpret_cast<GetProc>(iat->u1.Function);
            // Every import of it resolves to one function; chain to the
            // first one seen and leave any entry pointing elsewhere alone.
            if (!g_get_proc) g_get_proc = current;
            if (current != g_get_proc) continue;
            // As in vtable.cpp: write in place when writable, and never drop
            // execute rights from a page that shares code (merged sections).
            void* volatile* slot = reinterpret_cast<void* volatile*>(&iat->u1.Function);
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(const_cast<void**>(slot), &mi, sizeof mi)) continue;
            const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
            if (mi.Protect & writable) {
                InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hk_get_proc_address));
                continue;
            }
            const DWORD rw = (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ)) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
            DWORD old;
            if (!VirtualProtect(const_cast<void**>(slot), sizeof(void*), rw, &old)) continue;
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hk_get_proc_address));
            VirtualProtect(const_cast<void**>(slot), sizeof(void*), old, &old);
        }
    }
}
}  // namespace tessera
