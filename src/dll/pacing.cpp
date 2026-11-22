// SPDX-License-Identifier: GPL-3.0-or-later
// Frame pacing: precise limiter (P1), frame latency (P3), back-off when the
// game or a driver SDK paces frames itself (P6).
#include "pacing.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>

#include "config.h"
#include "log.h"

namespace tessera {
namespace {
// The limiter paces one swap chain: the first one presenting, replaced when
// it stops presenting for a second (the game recreated it).
SRWLOCK g_pacing_lock = SRWLOCK_INIT;
IDXGISwapChain* g_primary = nullptr;
int64_t g_primary_last = 0;
int64_t g_deadline = 0;
bool g_driver_pacing = false;
unsigned g_seen_revision = 0;
std::atomic<unsigned> g_pacing_revision{0};
std::atomic<bool> g_smooth_motion{false};
constexpr GUID kDriverPacing = {0x9325db93, 0x1d96, 0x4eab, {0x92, 0x1e, 0xd4, 0xd1, 0xa2, 0xd0, 0x65, 0xba}};
const int64_t g_freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);  // never fails since Windows XP
    return f.QuadPart;
}();
thread_local HANDLE t_timer = nullptr;

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Sleeps on a high-resolution waitable timer until ~0.5 ms before the
// deadline, then spins: the timer alone is late by up to a scheduler tick.
void wait_until(int64_t deadline) {
    const int64_t spin = g_freq / 2000;  // 0.5 ms
    int64_t now = qpc();
    if (deadline - now > spin) {
        if (!t_timer)
            t_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
        if (t_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((deadline - now - spin) * 10000000 / g_freq);  // relative, 100 ns units
            if (SetWaitableTimer(t_timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(t_timer, INFINITE);
        }
    }
    while (qpc() < deadline) YieldProcessor();
}
bool driver_pacing(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    bool active = false;
    UINT size = sizeof active;
    const HRESULT hr = dev->GetPrivateData(kDriverPacing, &size, &active);
    dev->Release();
    return SUCCEEDED(hr) && active;
}

uint32_t pace_primary(IDXGISwapChain* sc, int fps) {
    const bool changed = g_primary != sc;
    if (changed) {
        if (g_primary && qpc() - g_primary_last < g_freq) return 0;
        g_primary = sc;
        g_deadline = 0;
    }
    const unsigned revision = g_pacing_revision.load(std::memory_order_acquire);
    if (changed || revision != g_seen_revision) {
        const bool active = driver_pacing(sc);
        if (active != g_driver_pacing)
            logf("limiter: %s", active ? "yielding to NVAPI pacing" : "NVAPI pacing off, resuming");
        g_driver_pacing = active;
        g_seen_revision = revision;
    }
    const int64_t start = qpc();
    if (g_driver_pacing || g_smooth_motion.load(std::memory_order_relaxed)) {
        g_primary_last = start;
        g_deadline = 0;
        return 0;
    }

    int64_t release = start;
    if (start < g_deadline) {
        wait_until(g_deadline);
        release = qpc();
    }
    // Count from the actual release, including after a late arrival or timer wake.
    // Catching up to an older clock phase would squeeze the next interval.
    g_deadline = release + (g_freq + fps - 1) / fps;
    g_primary_last = release;
    return static_cast<uint32_t>((release - start) * 1000000 / g_freq);
}
}  // namespace

uint32_t pace_before_present(IDXGISwapChain* sc) {
    const auto& cfg = config();
    if (cfg.fps_limit <= 0 || (cfg.ext_fps_limit > 0 && cfg.ext_fps_limit <= cfg.fps_limit)) return 0;
    // Another swap chain may present concurrently. Skip it instead of blocking
    // its thread or racing the primary chain's deadline.
    if (!TryAcquireSRWLockExclusive(&g_pacing_lock)) return 0;
    const uint32_t waited = pace_primary(sc, cfg.fps_limit);
    ReleaseSRWLockExclusive(&g_pacing_lock);
    return waited;
}

void pacing_set_driver_mode(IUnknown* device, bool active) {
    if (!device || config().fps_limit <= 0) return;
    ID3D11Device* dev = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev)))) return;
    const HRESULT hr = dev->SetPrivateData(kDriverPacing, active ? sizeof active : 0, active ? &active : nullptr);
    dev->Release();
    if (SUCCEEDED(hr)) g_pacing_revision.fetch_add(1, std::memory_order_release);
}

void pacing_background_tick() {
    if (config().fps_limit <= 0) return;
    // Smooth Motion is a D3D11 driver runtime. It has no public state query
    // here. Leave its cadence alone while its presentation hook is loaded.
    const bool loaded = GetModuleHandleW(L"nvpresent64.dll") != nullptr;
    if (g_smooth_motion.exchange(loaded, std::memory_order_relaxed) != loaded)
        logf("limiter: %s", loaded ? "yielding to NVIDIA Smooth Motion runtime" : "Smooth Motion runtime unloaded, resuming");
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
