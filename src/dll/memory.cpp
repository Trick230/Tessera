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

// The game created a D3D12 device or a Vulkan instance before any D3D11
// device: it renders with another API, M1 leaves its memory figures alone.
std::atomic<bool> g_other_api{false};
std::atomic<const char*> g_other_what{nullptr};  // for the log, opened later

// What the game is told it may use, from the real budget.
uint64_t reported(uint64_t real) {
    if (g_other_api.load(std::memory_order_relaxed)) return real;
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

// CreateDXGIFactory/1/2 as the game gets them (its imports, those of the
// DLLs in its folder, GetProcAddress, which delay-load imports go through
// too): engines (Unreal's D3D11 RHI) read the adapter's dedicated memory to
// size their texture pool before creating the device, so the adapters are
// hooked as soon as the game gets a factory. D3D12CreateDevice and
// vkCreateInstance only tell that the game renders with another API.
using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateD3D12 = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateVkInstance = int32_t(WINAPI*)(const void*, const void*, void**);
// The first pointer seen for each name, chained to; another one is not ours.
std::atomic<void*> g_cf{nullptr}, g_cf1{nullptr}, g_cf2{nullptr}, g_d3d12{nullptr}, g_vk{nullptr};
std::atomic<bool> g_early_done{false};
std::atomic<int> g_early_ok{-1};  // -1 not tried, 0 failed, 1 on; logged with the device

void on_factory(IUnknown* f) {
    if (g_early_done.load(std::memory_order_acquire) || g_adapter.load(std::memory_order_acquire) ||
        g_other_api.load(std::memory_order_relaxed))
        return;
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

// The game's own device or instance (not a probe without one, not an
// overlay's) before any D3D11 device: M1 stays out of this process.
void on_other_api(bool made, void* ret, const char* what) {
    if (!made || memory_device_seen() || !is_game_code(ret)) return;
    g_other_what.store(what, std::memory_order_relaxed);
    if (!g_other_api.exchange(true, std::memory_order_release)) logf("memory: the game uses %s, VRAM figures left alone", what);
}

template <class F>
F orig(const std::atomic<void*>& p) {
    return reinterpret_cast<F>(p.load(std::memory_order_relaxed));
}

HRESULT WINAPI hk_CreateDXGIFactory(REFIID riid, void** out) {
    const HRESULT hr = orig<CreateFactory>(g_cf)(riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(static_cast<IUnknown*>(*out));
    return hr;
}
HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void** out) {
    const HRESULT hr = orig<CreateFactory>(g_cf1)(riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(static_cast<IUnknown*>(*out));
    return hr;
}
HRESULT WINAPI hk_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    const HRESULT hr = orig<CreateFactory2>(g_cf2)(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) on_factory(static_cast<IUnknown*>(*out));
    return hr;
}
HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    const HRESULT hr = orig<CreateD3D12>(g_d3d12)(adapter, fl, riid, out);
    on_other_api(SUCCEEDED(hr) && out && *out, _ReturnAddress(), "D3D12");
    return hr;
}
int32_t WINAPI hk_vkCreateInstance(const void* info, const void* alloc, void** out) {
    const int32_t r = orig<CreateVkInstance>(g_vk)(info, alloc, out);
    on_other_api(r == 0 && out && *out, _ReturnAddress(), "Vulkan");
    return r;
}

struct Wanted {
    const char* name;
    std::atomic<void*>* orig;
    void* hook;
};
const Wanted kWanted[] = {
    {"CreateDXGIFactory", &g_cf, reinterpret_cast<void*>(&hk_CreateDXGIFactory)},
    {"CreateDXGIFactory1", &g_cf1, reinterpret_cast<void*>(&hk_CreateDXGIFactory1)},
    {"CreateDXGIFactory2", &g_cf2, reinterpret_cast<void*>(&hk_CreateDXGIFactory2)},
    {"D3D12CreateDevice", &g_d3d12, reinterpret_cast<void*>(&hk_D3D12CreateDevice)},
    {"vkCreateInstance", &g_vk, reinterpret_cast<void*>(&hk_vkCreateInstance)},
};

const Wanted* find_wanted(const char* name) {
    if (name[0] != 'C' && name[0] != 'D' && name[0] != 'v') return nullptr;  // most names, cheaply
    for (const Wanted& w : kWanted)
        if (std::strcmp(name, w.name) == 0) return &w;
    return nullptr;
}
}  // namespace

bool memory_import_wanted(const char* name) { return find_wanted(name) != nullptr; }

void* memory_import_hook(const char* name, void* current) {
    const Wanted* w = find_wanted(name);
    if (!w || !current || current == w->hook) return nullptr;
    void* expected = nullptr;
    w->orig->compare_exchange_strong(expected, current, std::memory_order_acq_rel);
    return w->orig->load(std::memory_order_acquire) == current ? w->hook : nullptr;
}

uint64_t memory_reported_for(uint32_t vendor, uint64_t real) {
    // Before the first device the adapter is unknown: the cap as in the
    // adapter descriptions (NVAPI only knows NVIDIA GPUs).
    const uint32_t v = g_vendor.load(std::memory_order_acquire);
    if (!v) {
        config_load();
        return reported(real);
    }
    return v == vendor ? reported(real) : real;
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
    if (g_other_api.load(std::memory_order_acquire))
        logf("memory: budget hooks %s, cap %d MB NOT applied: the game used %s first", ok ? "on" : "FAILED",
             config().vram_budget_mb, g_other_what.load(std::memory_order_relaxed));
    else
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
