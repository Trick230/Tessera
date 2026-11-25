// SPDX-License-Identifier: GPL-3.0-or-later
// Frame pacing for DXGI and Vulkan: precise low-latency limiter (P1, timing in
// limiter.cpp) and frame latency (P3). When the user sets a cap, it is in charge:
// it never steps aside for Reflex, a driver cap, another limiter or frame generation.
#include "pacing.h"

#include <windows.h>

#include "config.h"
#include "limiter.h"
#include "log.h"

namespace tessera {
uint32_t pace_before_present_any(const void* chain) {
    const int fps = config().fps_limit;
    return fps > 0 ? limiter::before_present(chain, fps, config().limiter_mode) : 0;
}

uint32_t pace_after_present_any(const void* chain) {
    const int fps = config().fps_limit;
    return fps > 0 ? limiter::after_present(chain, fps, config().limiter_mode) : 0;
}

uint32_t pace_before_present(IDXGISwapChain* sc) { return pace_before_present_any(sc); }
uint32_t pace_after_present(IDXGISwapChain* sc) { return pace_after_present_any(sc); }

void pacing_on_swapchain(IDXGISwapChain* sc) {
    const int n = config().max_frame_latency;
    if (n <= 0) return;
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d)) && (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
        logf("max frame latency left alone: waitable swap chain (the game manages latency)");
        return;
    }
    IDXGIDevice1* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return;
    const HRESULT hr = dev->SetMaximumFrameLatency(static_cast<UINT>(n));
    dev->Release();
    logf("max frame latency %d: %s", n, SUCCEEDED(hr) ? "ok" : "failed");
}
}  // namespace tessera
