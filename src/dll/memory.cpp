// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory.h"

#include <windows.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <atomic>

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
SIZE_T reported_dedicated(SIZE_T real, const LUID& id);

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
    if (SUCCEEDED(hr) && info && group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL && is_game_code(_ReturnAddress()))
        info->Budget = reported(info->Budget);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* d) {
    using F = decltype(&hk_GetDesc);
    const HRESULT hr = reinterpret_cast<F>(vt_orig(g_desc, self))(self, d);
    if (SUCCEEDED(hr) && d && is_game_code(_ReturnAddress()))
        d->DedicatedVideoMemory = reported_dedicated(d->DedicatedVideoMemory, d->AdapterLuid);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDesc1(IDXGIAdapter1* self, DXGI_ADAPTER_DESC1* d) {
    using F = decltype(&hk_GetDesc1);
    const HRESULT hr = reinterpret_cast<F>(vt_orig(g_desc1, self))(self, d);
    if (SUCCEEDED(hr) && d && is_game_code(_ReturnAddress()))
        d->DedicatedVideoMemory = reported_dedicated(d->DedicatedVideoMemory, d->AdapterLuid);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_GetDesc2(IDXGIAdapter2* self, DXGI_ADAPTER_DESC2* d) {
    using F = decltype(&hk_GetDesc2);
    const HRESULT hr = reinterpret_cast<F>(vt_orig(g_desc2, self))(self, d);
    if (SUCCEEDED(hr) && d && is_game_code(_ReturnAddress()))
        d->DedicatedVideoMemory = reported_dedicated(d->DedicatedVideoMemory, d->AdapterLuid);
    return hr;
}

// LUID and vendor of the first device's adapter; written once before the hooks go in.
LUID g_luid = {};
std::atomic<uint32_t> g_vendor{0};

SIZE_T reported_dedicated(SIZE_T real, const LUID& id) {
    if (id.LowPart != g_luid.LowPart || id.HighPart != g_luid.HighPart) return real;
    const uint64_t v = reported(real);
    return static_cast<SIZE_T>(v);
}
}  // namespace

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
    memory_background_tick();  // baseline before the game allocates anything
}

void memory_background_tick() {
    IDXGIAdapter3* a = g_adapter.load(std::memory_order_acquire);
    if (!a) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
    // The real values, not the capped ones the game sees.
    const Qvmi q = real_qvmi(a);
    if (FAILED(q(a, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return;
    q(a, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
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
