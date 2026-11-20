// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>

#include "config.h"
#include "log.h"
#include "session.h"
#include "vtable.h"

namespace tessera {
namespace {
constexpr int kAdapter3QueryVideoMemoryInfo = 14;

HRESULT STDMETHODCALLTYPE hk_QueryVideoMemoryInfo(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP,
                                                  DXGI_QUERY_VIDEO_MEMORY_INFO*);
VtHook g_qvmi = {kAdapter3QueryVideoMemoryInfo, reinterpret_cast<void*>(&hk_QueryVideoMemoryInfo)};
using Qvmi = decltype(&hk_QueryVideoMemoryInfo);

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
    const uint64_t cap = static_cast<uint64_t>(config().vram_budget_mb) << 20;
    if (SUCCEEDED(hr) && info && cap && group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL && info->Budget > cap)
        info->Budget = cap;
    return hr;
}
}  // namespace

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
    if (config().vram_budget_mb > 0) {
        const bool ok = vt_hook(g_qvmi, a3);
        logf("memory: budget cap %d MB %s", config().vram_budget_mb, ok ? "on" : "FAILED");
    }
    IDXGIAdapter3* expected = nullptr;
    if (!g_adapter.compare_exchange_strong(expected, a3, std::memory_order_acq_rel)) {
        a3->Release();
        return;
    }
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
