// SPDX-License-Identifier: GPL-3.0-or-later
// NVAPI, seen from the game's side. NVAPI has one export, nvapi_QueryInterface,
// which the game resolves with GetProcAddress, usually before it creates a
// D3D device. The GetProcAddress imports of the game (its exe and the DLLs
// in its folder, also those loaded later) are patched when the layer
// loads (chaining the previous pointer), and nvapi_QueryInterface is handed
// back wrapped; every other name and id goes to the original untouched,
// except the few memory.cpp wants (memory_import_hook), patched the same way
// in import tables and in GetProcAddress results.
//   M1: NvAPI_GPU_GetMemoryInfo(Ex) reports at most the budget the game is given.
#include "nvapi.h"

#include <windows.h>
#include <psapi.h>

#include <intrin.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "memory.h"
#include "module.h"
#include "stream_io.h"

namespace tessera {
namespace {
using NvStatus = int;
using QueryInterface = void*(__cdecl*)(unsigned);
using GetMemoryInfo = NvStatus(__cdecl*)(void*, uint32_t*);
using GetMemoryInfoEx = NvStatus(__cdecl*)(void*, uint64_t*);
using GetProc = FARPROC(WINAPI*)(HMODULE, LPCSTR);

// Ids from the public NVAPI interface table.
constexpr unsigned kGetMemoryInfo = 0x07F9B368;
constexpr unsigned kGetMemoryInfoEx = 0xC0599498;

GetProc g_get_proc = nullptr;  // written once in DllMain, before the patch
std::atomic<QueryInterface> g_qi{nullptr};
std::atomic<GetMemoryInfo> g_mem{nullptr};
std::atomic<GetMemoryInfoEx> g_mem_ex{nullptr};

constexpr uint32_t nv_version(uint32_t size, uint32_t v) { return size | (v << 16); }

// NV_DISPLAY_DRIVER_MEMORY_INFO V1-V3 (KB): version, dedicated, available
// dedicated, system, shared, [V2+] current available dedicated, ...
NvStatus __cdecl hk_get_memory_info(void* gpu, uint32_t* info) {
    const NvStatus st = g_mem.load(std::memory_order_relaxed)(gpu, info);
    // Only for the game's own calls (also before its device, as the DXGI
    // descriptions): SDKs and overlays keep the real numbers.
    if (st != 0 || !info || !is_game_code(_ReturnAddress())) return st;
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

// NV_GPU_MEMORY_INFO_EX V1 (bytes): version (padded to 8), dedicated,
// available dedicated, system, shared, current available dedicated, then
// eviction and promotion counters.
NvStatus __cdecl hk_get_memory_info_ex(void* gpu, uint64_t* info) {
    const NvStatus st = g_mem_ex.load(std::memory_order_relaxed)(gpu, info);
    // As hk_get_memory_info: for the game's calls only.
    if (st != 0 || !info || !is_game_code(_ReturnAddress())) return st;
    if (static_cast<uint32_t>(info[0]) != nv_version(80, 1)) return st;
    const uint64_t dedicated = info[1];
    const uint64_t told = memory_reported_for(0x10DE, dedicated);
    if (told >= dedicated) return st;
    const uint64_t used = info[2] > info[5] ? info[2] - info[5] : 0;
    const uint64_t free_bytes = told > used ? told - used : 0;
    if (info[5] > free_bytes) info[5] = free_bytes;
    info[1] = told;
    if (info[2] > told) info[2] = told;
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
    if (id == kGetMemoryInfoEx) {
        GetMemoryInfoEx expected = nullptr;
        g_mem_ex.compare_exchange_strong(expected, reinterpret_cast<GetMemoryInfoEx>(p));
        return reinterpret_cast<void*>(&hk_get_memory_info_ex);
    }
    return p;
}

std::atomic<bool> g_stream_imports{false};

FARPROC WINAPI hk_get_proc_address(HMODULE m, LPCSTR name) {
    const FARPROC p = g_get_proc(m, name);
    // HIWORD 0: lookup by ordinal, no name to read.
    if (!p || !(reinterpret_cast<uintptr_t>(name) >> 16)) return p;
    if (name[0] != 'n' || std::strcmp(name, "nvapi_QueryInterface") != 0) {
        // M1 (also delay-load imports, whose helper resolves through here).
        if (void* h = memory_import_hook(name, reinterpret_cast<void*>(p))) return reinterpret_cast<FARPROC>(h);
        if (g_stream_imports.load(std::memory_order_relaxed))
            if (void* h = stream_import_hook(name, reinterpret_cast<void*>(p))) return reinterpret_cast<FARPROC>(h);
        return p;
    }
    QueryInterface expected = nullptr;
    g_qi.compare_exchange_strong(expected, reinterpret_cast<QueryInterface>(p));
    // A second nvapi64.dll (unlikely) would not match what we chain to.
    if (g_qi.load(std::memory_order_relaxed) != reinterpret_cast<QueryInterface>(p)) return p;
    return reinterpret_cast<FARPROC>(&hk_query_interface);
}

// kernel32's and kernelbase's GetProcAddress: an import holding either is
// unhooked (modules importing through the api set get kernelbase's).
void* g_sys[2] = {};
bool is_system(void* p) { return p && (p == g_sys[0] || p == g_sys[1]); }

wchar_t g_game_dir[MAX_PATH];  // exe folder with a trailing '\'
size_t g_game_dir_len = 0;
HMODULE g_self = nullptr;

bool in_game_dir(const wchar_t* path, size_t len) {
    return g_game_dir_len && len > g_game_dir_len && _wcsnicmp(path, g_game_dir, g_game_dir_len) == 0;
}

void write_slot(void* volatile* slot, void* value) {
    // As in vtable.cpp: write in place when writable, and never drop
    // execute rights from a page that shares code (merged sections).
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(const_cast<void**>(slot), &mi, sizeof mi)) return;
    const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (mi.Protect & writable) {
        InterlockedExchangePointer(slot, value);
        return;
    }
    const DWORD rw = (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ)) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old;
    if (!VirtualProtect(const_cast<void**>(slot), sizeof(void*), rw, &old)) return;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(const_cast<void**>(slot), sizeof(void*), old, &old);
}

// Committed image memory: where a bound import resolved by the loader points.
bool in_image(const void* p) {
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Type == MEM_IMAGE;
}
// Points one module's GetProcAddress imports at ours. False while the
// loader has not bound them yet (it announces a DLL before binding it).
bool patch_module(uint8_t* base, bool stream_only = false) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return true;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return true;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;
    bool bound = true;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;  // no name table to match against
        // Bound imports (old or new style) hold stale addresses until the loader checks them.
        const bool prebound = imp->TimeDateStamp != 0;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            const char* const name = reinterpret_cast<const char*>(by_name->Name);
            const bool stream = g_stream_imports.load(std::memory_order_relaxed) && stream_import_wanted(name);
            const bool gpa = !stream_only && std::strcmp(name, "GetProcAddress") == 0;
            if (!stream && !gpa && (stream_only || !memory_import_wanted(name))) continue;
            void* const current = reinterpret_cast<void*>(iat->u1.Function);
            if (iat->u1.Function == names->u1.AddressOfData || (prebound && !in_image(current))) {
                bound = false;  // still the name entry
                continue;
            }
            if (!gpa) {
                if (void* h = stream ? stream_import_hook(name, current) : memory_import_hook(name, current))
                    write_slot(reinterpret_cast<void* volatile*>(&iat->u1.Function), h);
                continue;
            }
            if (current == reinterpret_cast<void*>(&hk_get_proc_address)) continue;
            // Chain to the first one seen (the exe's, possibly another
            // tool's hook); elsewhere replace only that one or, when it is
            // the system's, the system's: any other pointer is not ours.
            if (!g_get_proc) g_get_proc = reinterpret_cast<GetProc>(current);
            void* const chained = reinterpret_cast<void*>(g_get_proc);
            if (current != chained && !(is_system(current) && is_system(chained))) continue;
            write_slot(reinterpret_cast<void* volatile*>(&iat->u1.Function),
                       reinterpret_cast<void*>(&hk_get_proc_address));
        }
    }
    return bound;
}

// Game DLLs loaded later wait here until the loader has bound their imports;
// each later load retries them, and the writer tick those it saw already
// (by then the loader is long done with their import table).
constexpr int kPending = 64;
uint8_t* g_pending[kPending];
int g_npending = 0;
uint8_t g_pending_seen[kPending];  // writer ticks that saw the entry
// Notifications are serialised among themselves, not with the writer tick.
SRWLOCK g_pending_lock = SRWLOCK_INIT;
std::atomic<bool> g_any_pending{false};  // a hint for the writer tick

void remove_pending(int i) {
    --g_npending;
    g_pending[i] = g_pending[g_npending];
    g_pending_seen[i] = g_pending_seen[g_npending];
}
void add_pending(uint8_t* base) {
    if (g_npending == kPending) return;
    g_pending_seen[g_npending] = 0;
    g_pending[g_npending++] = base;
}
void retry_pending(bool tick) {
    for (int i = 0; i < g_npending; ++i) {
        if (tick && !g_pending_seen[i]++) continue;
        if (patch_module(g_pending[i])) remove_pending(i--);
    }
}
void note_pending() { g_any_pending.store(g_npending != 0, std::memory_order_relaxed); }

struct Unicode {
    USHORT len, max;  // bytes
    PWSTR s;
};
struct DllNotification {
    ULONG flags;
    const Unicode* full_name;
    const Unicode* base_name;
    void* base;
    ULONG size;
};
using DllCallback = void(CALLBACK*)(ULONG, const DllNotification*, void*);
using LdrRegister = LONG(NTAPI*)(ULONG, DllCallback, void*, void**);
constexpr ULONG kDllLoaded = 1, kDllUnloaded = 2;

void CALLBACK on_dll(ULONG reason, const DllNotification* d, void*) {
    if (!d) return;
    AcquireSRWLockExclusive(&g_pending_lock);
    if (reason == kDllUnloaded) {
        for (int i = 0; i < g_npending; ++i)
            if (g_pending[i] == d->base) remove_pending(i--);
        note_pending();
        ReleaseSRWLockExclusive(&g_pending_lock);
        return;
    }
    if (reason != kDllLoaded) {
        ReleaseSRWLockExclusive(&g_pending_lock);
        return;
    }
    // Loads announced earlier are bound by now (nvapi64.dll itself, loaded
    // by a game DLL, comes through here before that DLL resolves NVAPI).
    retry_pending(false);
    const Unicode* n = d->full_name;
    if (n && n->s && in_game_dir(n->s, n->len / sizeof(wchar_t)) && d->base != g_self &&
        !patch_module(static_cast<uint8_t*>(d->base)))
        add_pending(static_cast<uint8_t*>(d->base));
    note_pending();
    ReleaseSRWLockExclusive(&g_pending_lock);
}
}  // namespace

void nvapi_patch_imports() {
    // The game will call into us from now on: stay loaded even if it frees
    // d3d11.dll after a probe. Safe under the loader lock.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&hk_get_proc_address), &g_self))
        return;
    const DWORD n = GetModuleFileNameW(nullptr, g_game_dir, MAX_PATH);
    if (n && n < MAX_PATH)
        if (wchar_t* s = wcsrchr(g_game_dir, L'\\')) {
            s[1] = 0;
            g_game_dir_len = wcslen(g_game_dir);
        }
    const wchar_t* const sys[2] = {L"kernel32.dll", L"kernelbase.dll"};
    for (int k = 0; k < 2; ++k)
        if (HMODULE h = GetModuleHandleW(sys[k])) g_sys[k] = reinterpret_cast<void*>(GetProcAddress(h, "GetProcAddress"));
    // The exe first: its pointer is the one chained to.
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    patch_module(exe);
    // Game DLLs already loaded (static imports are bound before any DllMain).
    HMODULE mods[512];
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) {
        const DWORD count = (bytes < sizeof mods ? bytes : sizeof mods) / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH];
            const DWORD len = GetModuleFileNameW(mods[i], path, MAX_PATH);
            auto* base = reinterpret_cast<uint8_t*>(mods[i]);
            if (base == exe || mods[i] == g_self || !len || len >= MAX_PATH || !in_game_dir(path, len)) continue;
            if (!patch_module(base)) add_pending(base);
        }
    }
    note_pending();
    // And those loaded later. The callback stays valid: the module is pinned.
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        void* cookie = nullptr;
        if (auto reg = reinterpret_cast<LdrRegister>(GetProcAddress(nt, "LdrRegisterDllNotification")))
            reg(0, &on_dll, nullptr, &cookie);
    }
}

void nvapi_background_tick() {
    if (!g_any_pending.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_pending_lock);
    retry_pending(true);
    note_pending();
    ReleaseSRWLockExclusive(&g_pending_lock);
}

void activate_stream_imports() {
    g_stream_imports.store(true, std::memory_order_relaxed);
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) return;
    const DWORD count = (bytes < sizeof mods ? bytes : sizeof mods) / sizeof(HMODULE);
    const HMODULE exe = GetModuleHandleW(nullptr);
    for (DWORD i = 0; i < count; ++i) {
        if (mods[i] == g_self) continue;
        HMODULE held = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(mods[i]), &held))
            continue;
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(held, path, MAX_PATH);
        if (held == exe || (len && len < MAX_PATH && in_game_dir(path, len)))
            patch_module(reinterpret_cast<uint8_t*>(held), true);
        FreeLibrary(held);
    }
}
}  // namespace tessera
