// SPDX-License-Identifier: GPL-3.0-or-later
// Frame pacing for DXGI and Vulkan: precise low-latency limiter (P1, timing in
// limiter.cpp), frame latency (P3), back-off when the game or a driver SDK
// paces frames itself (P6).
#include "pacing.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>

#include "config.h"
#include "limiter.h"
#include "log.h"

namespace tessera {
namespace {
// Back-off state, rechecked when a swap chain or an NVAPI mode changes.
SRWLOCK g_check_lock = SRWLOCK_INIT;
std::atomic<IDXGISwapChain*> g_checked{nullptr};
std::atomic<unsigned> g_seen_revision{0};
std::atomic<bool> g_driver_pacing{false};
std::atomic<unsigned> g_pacing_revision{0};
std::atomic<bool> g_smooth_motion{false};  // a frame generation runtime is loaded
// NVAPI pacing on a non-D3D11 device. A D3D12 swap chain reports its queue, not the
// device, so it cannot be matched per device: all non-D3D11 swap chains yield.
std::atomic<bool> g_other_paced{false};
constexpr GUID kDriverPacing = {0x9325db93, 0x1d96, 0x4eab, {0x92, 0x1e, 0xd4, 0xd1, 0xa2, 0xd0, 0x65, 0xba}};

bool driver_pacing(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return g_other_paced.load(std::memory_order_relaxed);
    bool active = false;
    UINT size = sizeof active;
    const HRESULT hr = dev->GetPrivateData(kDriverPacing, &size, &active);
    dev->Release();
    return SUCCEEDED(hr) && active;
}

// True while another pacer is in charge of `sc`. The device query runs only
// when the swap chain or an NVAPI mode changed, never per frame.
bool yielding(IDXGISwapChain* sc) {
    const unsigned revision = g_pacing_revision.load(std::memory_order_acquire);
    if ((sc != g_checked.load(std::memory_order_relaxed) || revision != g_seen_revision.load(std::memory_order_relaxed))
        && TryAcquireSRWLockExclusive(&g_check_lock)) {
        const bool active = driver_pacing(sc);
        if (active != g_driver_pacing.exchange(active, std::memory_order_relaxed))
            logf("limiter: %s", active ? "yielding to NVAPI pacing" : "NVAPI pacing off, resuming");
        g_checked.store(sc, std::memory_order_relaxed);
        g_seen_revision.store(revision, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_check_lock);
    }
    return g_driver_pacing.load(std::memory_order_relaxed) || g_smooth_motion.load(std::memory_order_relaxed);
}

int active_limit() {
    const auto& cfg = config();
    if (cfg.fps_limit <= 0 || (cfg.ext_fps_limit > 0 && cfg.ext_fps_limit <= cfg.fps_limit)) return 0;
    return cfg.fps_limit;
}
}  // namespace

uint32_t pace_before_present(IDXGISwapChain* sc) {
    const int fps = active_limit();
    if (!fps) return 0;
    if (yielding(sc)) {
        limiter::reset();
        return 0;
    }
    return limiter::before_present(sc, fps, config().limiter_mode);
}

uint32_t pace_after_present(IDXGISwapChain* sc) {
    const int fps = active_limit();
    if (!fps || yielding(sc)) return 0;
    return limiter::after_present(sc, fps, config().limiter_mode);
}

uint32_t pace_before_present_any(const void* chain) {
    const int fps = active_limit();
    if (!fps) return 0;
    if (g_other_paced.load(std::memory_order_relaxed) || g_smooth_motion.load(std::memory_order_relaxed)) {
        limiter::reset();
        return 0;
    }
    return limiter::before_present(chain, fps, config().limiter_mode);
}

uint32_t pace_after_present_any(const void* chain) {
    const int fps = active_limit();
    if (!fps || g_other_paced.load(std::memory_order_relaxed) || g_smooth_motion.load(std::memory_order_relaxed))
        return 0;
    return limiter::after_present(chain, fps, config().limiter_mode);
}

void pacing_set_external_pacing(bool active) {
    if (config().fps_limit <= 0) return;
    if (g_other_paced.exchange(active, std::memory_order_relaxed) != active)
        logf("limiter: %s", active ? "yielding to the driver low-latency mode" : "driver low-latency mode off, resuming");
}

void pacing_set_driver_mode(IUnknown* device, bool active) {
    if (!device || config().fps_limit <= 0) return;
    ID3D11Device* dev = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev)))) {
        g_other_paced.store(active, std::memory_order_relaxed);
        g_pacing_revision.fetch_add(1, std::memory_order_release);
        return;
    }
    const HRESULT hr = dev->SetPrivateData(kDriverPacing, active ? sizeof active : 0, active ? &active : nullptr);
    dev->Release();
    if (SUCCEEDED(hr)) g_pacing_revision.fetch_add(1, std::memory_order_release);
}
void pacing_background_tick() {
    if (config().fps_limit <= 0) return;
    // Frame generation runtimes present generated frames from their own
    // thread and pace them themselves: NVIDIA Smooth Motion (no public state
    // query), XeSS-FG and the FidelityFX frame interpolation DLLs. Leave the
    // cadence alone while one is loaded (DLSS-FG runs with Reflex: NVAPI path).
    static constexpr const wchar_t* kFrameGen[] = {
        L"nvpresent64.dll", L"nvpresent32.dll", L"libxess_fg.dll",
        L"ffx_frameinterpolation_x64.dll", L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_framegeneration_vk.dll"};
    const wchar_t* found = nullptr;
    for (const wchar_t* m : kFrameGen)
        if (GetModuleHandleW(m)) {
            found = m;
            break;
        }
    const bool loaded = found != nullptr;
    if (g_smooth_motion.exchange(loaded, std::memory_order_relaxed) != loaded) {
        if (loaded) logf("limiter: yielding to the frame generation runtime %ls", found);
        else logf("limiter: frame generation runtime unloaded, resuming");
    }
}

void pacing_on_swapchain(IDXGISwapChain* sc) {
    if (config().fps_limit > 0)
        g_pacing_revision.fetch_add(1, std::memory_order_release);  // DXGI can reuse an old swap chain's address.
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
