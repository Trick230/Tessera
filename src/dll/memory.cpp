// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "log.h"
#include "module.h"
#include "session.h"
#include "vtable.h"

namespace tessera {
namespace {
constexpr int kAdapterGetDesc = 8;
constexpr int kAdapter1GetDesc1 = 10;
constexpr int kAdapter2GetDesc2 = 11;
constexpr int kAdapter3QueryVideoMemoryInfo = 14;

HRESULT STDMETHODCALLTYPE hk_QueryVideoMemoryInfo(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP,
                                                  DXGI_QUERY_VIDEO_MEMORY_INFO*);
HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
HRESULT STDMETHODCALLTYPE hk_GetDesc2(IDXGIAdapter2*, DXGI_ADAPTER_DESC2*);
VtHook g_qvmi = {kAdapter3QueryVideoMemoryInfo, reinterpret_cast<void*>(&hk_QueryVideoMemoryInfo)};
VtHook g_desc = {kAdapterGetDesc, reinterpret_cast<void*>(&hk_GetDesc)};
VtHook g_desc1 = {kAdapter1GetDesc1, reinterpret_cast<void*>(&hk_GetDesc1)};
VtHook g_desc2 = {kAdapter2GetDesc2, reinterpret_cast<void*>(&hk_GetDesc2)};
using Qvmi = decltype(&hk_QueryVideoMemoryInfo);

// VRAM Tessera itself holds on the game's GPU (M3 tile pool and the like;
// none yet). The honest mode always reports the budget minus this.
std::atomic<uint64_t> g_own{0};

// What the game is told it may use, from the real budget.
uint64_t reported(uint64_t real) {
    const uint64_t own = g_own.load(std::memory_order_relaxed);
    uint64_t v = real > own ? real - own : 0;
    const uint64_t cap = static_cast<uint64_t>(config().vram_budget_mb) << 20;
    if (cap && v > cap) v = cap;
    return v;
}

// Dedicated memory in adapter descriptions: some games size their pools
// from it instead of the budget. Only the adapter the game renders on.
SIZE_T reported_dedicated(SIZE_T real, const LUID& id, UINT vendor);

// First device's adapter, one reference held for the life of the process.
std::atomic<IDXGIAdapter3*> g_adapter{nullptr};

Qvmi real_qvmi(IDXGIAdapter3* a) {
    // The original when we hooked this vtable, else the current entry.
    if (void* o = vt_orig(g_qvmi, a)) return reinterpret_cast<Qvmi>(o);
    return reinterpret_cast<Qvmi>((*reinterpret_cast<void***>(a))[kAdapter3QueryVideoMemoryInfo]);
}

HRESULT STDMETHODCALLTYPE hk_QueryVideoMemoryInfo(IDXGIAdapter3* self, UINT node, DXGI_MEMORY_SEGMENT_GROUP group,
                                                  DXGI_QUERY_VIDEO_MEMORY_INFO* info) {
    const HRESULT hr = reinterpret_cast<Qvmi>(vt_orig(g_qvmi, self))(self, node, group, info);
    // Budgets only for the device's adapter: before it, the early hooks cap
    // just the descriptions (which is what pools are sized from).
    if (SUCCEEDED(hr) && info && group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL && memory_device_seen() &&
        is_game_code(_ReturnAddress()))
        info->Budget = reported(info->Budget);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* d) {
    using F = decltype(&hk_GetDesc);
    const HRESULT hr = reinterpret_cast<F>(vt_orig(g_desc, self))(self, d);
    if (SUCCEEDED(hr) && d && is_game_code(_ReturnAddress()))
        d->DedicatedVideoMemory = reported_dedicated(d->DedicatedVideoMemory, d->AdapterLuid, d->VendorId);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGIAdapter1* self, DXGI_ADAPTER_DESC1* d) {
    using F = decltype(&hk_GetDesc1);
    const HRESULT hr = reinterpret_cast<F>(vt_orig(g_desc1, self))(self, d);
    if (SUCCEEDED(hr) && d && is_game_code(_ReturnAddress()))
        d->DedicatedVideoMemory = reported_dedicated(d->DedicatedVideoMemory, d->AdapterLuid, d->VendorId);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDesc2(IDXGIAdapter2* self, DXGI_ADAPTER_DESC2* d) {
    using F = decltype(&hk_GetDesc2);
    const HRESULT hr = reinterpret_cast<F>(vt_orig(g_desc2, self))(self, d);
    if (SUCCEEDED(hr) && d && is_game_code(_ReturnAddress()))
        d->DedicatedVideoMemory = reported_dedicated(d->DedicatedVideoMemory, d->AdapterLuid, d->VendorId);
    return hr;
}

// LUID and vendor of the first device's adapter; written once before the hooks go in.
LUID g_luid = {};
std::atomic<uint32_t> g_vendor{0};

constexpr UINT kSoftwareVendor = 0x1414;  // Microsoft Basic Render Driver (WARP)

SIZE_T reported_dedicated(SIZE_T real, const LUID& id, UINT vendor) {
    // Before the first device the game's adapter is unknown: any hardware
    // adapter gets the cap (an integrated GPU's small size is below it).
    if (!g_vendor.load(std::memory_order_acquire)) return vendor == kSoftwareVendor ? real : static_cast<SIZE_T>(reported(real));
    if (id.LowPart != g_luid.LowPart || id.HighPart != g_luid.HighPart) return real;
    const uint64_t v = reported(real);
    return static_cast<SIZE_T>(v);
}

bool hook_adapter(IDXGIAdapter* a) {
    IDXGIAdapter3* a3 = nullptr;
    if (FAILED(a->QueryInterface(IID_PPV_ARGS(&a3)))) return false;
    IDXGIAdapter2* a2 = nullptr;
    a3->QueryInterface(IID_PPV_ARGS(&a2));
    const bool ok = vt_hook(g_qvmi, a3) && vt_hook(g_desc, a3) && vt_hook(g_desc1, a3) && (!a2 || vt_hook(g_desc2, a2));
    if (a2) a2->Release();
    a3->Release();
    return ok;
}

// The exe's CreateDXGIFactory imports: engines (Unreal's D3D11 RHI) read the
// adapter's dedicated memory to size their texture pool before creating the
// device, so the adapters are hooked as soon as the game gets a factory.
using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
CreateFactory g_cf = nullptr, g_cf1 = nullptr;  // written once in DllMain, before the patch
CreateFactory2 g_cf2 = nullptr;
std::atomic<bool> g_early_done{false};
std::atomic<int> g_early_ok{-1};  // -1 not tried, 0 failed, 1 on; logged with the device

void on_factory(IUnknown* f) {
    if (g_early_done.load(std::memory_order_acquire) || g_adapter.load(std::memory_order_acquire)) return;
    // Settings only, no session: a process that never creates a D3D11 device
    // (a D3D12 or Vulkan game linking both, a launcher) must not count as an
    // unclean exit for safe mode. Safe mode was checked before the patch.
    config_load();
    IDXGIFactory* fac = nullptr;
    if (FAILED(f->QueryInterface(IID_PPV_ARGS(&fac)))) return;
    IDXGIAdapter* a = nullptr;
    bool ok = false;
    if (fac->EnumAdapters(0, &a) == S_OK) {
        ok = hook_adapter(a);  // adapters of one DXGI share their vtables
        a->Release();
    }
    fac->Release();
    if (!g_early_done.exchange(true, std::memory_order_acq_rel)) g_early_ok.store(ok ? 1 : 0, std::memory_order_release);
}

HRESULT WINAPI hk_CreateDXGIFactory(REFIID riid, void** out) {
    const HRESULT hr = g_cf(riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(static_cast<IUnknown*>(*out));
    return hr;
}
HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void** out) {
    const HRESULT hr = g_cf1(riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(static_cast<IUnknown*>(*out));
    return hr;
}
HRESULT WINAPI hk_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = g_cf2(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(static_cast<IUnknown*>(*out));
    return hr;
}

void write_import(void* volatile* slot, void* value) {
    // As in vtable.cpp: never drop execute rights from a page that shares code.
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
}  // namespace

void memory_patch_imports() {
    // The hooks live in this module: stay loaded. Safe under the loader lock.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&hk_CreateDXGIFactory), &self))
        return;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    // The exe's static imports are bound before any DllMain runs.
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk || _stricmp(reinterpret_cast<const char*>(base + imp->Name), "dxgi.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* name = reinterpret_cast<const char*>(
                reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
            void* const current = reinterpret_cast<void*>(iat->u1.Function);
            if (!current || iat->u1.Function == names->u1.AddressOfData) continue;  // not bound
            // Chain to what is there (dxgi's export or another tool's hook).
            void* hook = nullptr;
            if (!g_cf && std::strcmp(name, "CreateDXGIFactory") == 0) {
                g_cf = reinterpret_cast<CreateFactory>(current);
                hook = reinterpret_cast<void*>(&hk_CreateDXGIFactory);
            } else if (!g_cf1 && std::strcmp(name, "CreateDXGIFactory1") == 0) {
                g_cf1 = reinterpret_cast<CreateFactory>(current);
                hook = reinterpret_cast<void*>(&hk_CreateDXGIFactory1);
            } else if (!g_cf2 && std::strcmp(name, "CreateDXGIFactory2") == 0) {
                g_cf2 = reinterpret_cast<CreateFactory2>(current);
                hook = reinterpret_cast<void*>(&hk_CreateDXGIFactory2);
            }
            if (hook) write_import(reinterpret_cast<void* volatile*>(&iat->u1.Function), hook);
        }
    }
}

uint64_t memory_reported_for(uint32_t vendor, uint64_t real) {
    // Before the first device the adapter is unknown: report the truth.
    return g_vendor.load(std::memory_order_acquire) == vendor ? reported(real) : real;
}

bool memory_device_seen() { return g_vendor.load(std::memory_order_acquire) != 0; }

void memory_on_device(ID3D11Device* dev) {
    if (g_adapter.load(std::memory_order_acquire)) return;
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return;
    IDXGIAdapter* adapter = nullptr;
    IDXGIAdapter3* a3 = nullptr;
    if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        adapter->QueryInterface(IID_PPV_ARGS(&a3));  // Windows 10+
        adapter->Release();
    }
    dxgi->Release();
    if (!a3) {
        logf("memory: no IDXGIAdapter3, budget sampling off");
        return;
    }
    IDXGIAdapter3* expected = nullptr;
    if (!g_adapter.compare_exchange_strong(expected, a3, std::memory_order_acq_rel)) {
        a3->Release();
        return;
    }
    // Honest mode always (real budget minus Tessera's own VRAM), capped when
    // vram_budget_mb is set. Descriptions too: games that read the adapter
    // before creating their device still see the real size.
    DXGI_ADAPTER_DESC d = {};
    if (SUCCEEDED(a3->GetDesc(&d))) {
        g_luid = d.AdapterLuid;
        g_vendor.store(d.VendorId, std::memory_order_release);
    }
    IDXGIAdapter2* a2 = nullptr;
    a3->QueryInterface(IID_PPV_ARGS(&a2));
    const bool ok = vt_hook(g_qvmi, a3) && vt_hook(g_desc, a3) && vt_hook(g_desc1, a3) && (!a2 || vt_hook(g_desc2, a2));
    if (a2) a2->Release();
    logf("memory: budget hooks %s, cap %d MB", ok ? "on" : "FAILED", config().vram_budget_mb);
    const int early = g_early_ok.load(std::memory_order_acquire);
    if (early >= 0) logf("memory: adapter hooks before the device %s", early ? "on" : "FAILED");
    memory_background_tick();  // baseline before the game allocates anything
}

namespace {
std::atomic<uint64_t> g_local_usage{0}, g_local_budget{0};
}

bool memory_local(uint64_t* usage, uint64_t* budget) {
    *budget = g_local_budget.load(std::memory_order_acquire);
    *usage = g_local_usage.load(std::memory_order_relaxed);
    return *budget != 0;
}

void memory_background_tick() {
    IDXGIAdapter3* a = g_adapter.load(std::memory_order_acquire);
    if (!a) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
    // The real values, not the capped ones the game sees.
    const Qvmi q = real_qvmi(a);
    if (FAILED(q(a, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return;
    q(a, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
    g_local_usage.store(local.CurrentUsage, std::memory_order_relaxed);
    g_local_budget.store(local.Budget, std::memory_order_release);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    TesseraRecord r = {};
    r.qpc = static_cast<uint64_t>(t.QuadPart);
    r.kind = TESSERA_REC_VRAM;
    r.a = static_cast<uint32_t>(local.Budget >> 20);
    r.b = static_cast<uint32_t>(local.CurrentUsage >> 20);
    r.c = static_cast<uint32_t>(shared.CurrentUsage >> 20);
    r.d = static_cast<uint32_t>(config().vram_budget_mb);
    r.bytes = local.CurrentUsage;
    session_push(r);
}
}  // namespace tessera
