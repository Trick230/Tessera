// SPDX-License-Identifier: GPL-3.0-or-later
// Frame pacing: precise limiter (P1), frame latency (P3), back-off when the
// game or a driver SDK paces frames itself (P6).
#include "pacing.h"

#include <windows.h>

#include <atomic>

#include "config.h"
#include "log.h"

namespace tessera {
namespace {
// The limiter paces one swap chain: the first one presenting, replaced when
// it stops presenting for a second (the game recreated it).
std::atomic<IDXGISwapChain*> g_primary{nullptr};
std::atomic<int64_t> g_primary_last{0};
std::atomic<bool> g_backoff{false};
int64_t g_deadline = 0;  // next frame deadline (QPC); present thread only
const int64_t g_freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);  // never fails since Windows XP
    return f.QuadPart;
}();
thread_local HANDLE t_timer = nullptr;

// Modules that bring their own pacing: NVIDIA Streamline Reflex and DLSS
// frame generation, AMD FSR 3 frame generation (amd_fidelityfx_dx12 also
// holds the upscaler, but frame generation can be turned on at any time
// from it: back off), Intel XeSS frame generation and the DLSS-G to FSR 3
// mod. Reflex through NVAPI alone is caught at SetSleepMode (nvapi.cpp).
const wchar_t* const kPacingModules[] = {
    L"sl.reflex.dll",           L"sl.dlss_g.dll",
    L"nvngx_dlssg.dll",         L"amd_fidelityfx_dx12.dll",
    L"amd_fidelityfx_framegeneration_dx12.dll", L"libxess_fg.dll",
    L"dlssg_to_fsr3_amd_is_better.dll",
};

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
}  // namespace

uint32_t pace_before_present(IDXGISwapChain* sc) {
    const int fps = config().fps_limit;
    if (fps <= 0) return 0;
    const int64_t start = qpc();
    IDXGISwapChain* primary = g_primary.load(std::memory_order_relaxed);
    if (primary != sc) {
        if (primary && start - g_primary_last.load(std::memory_order_relaxed) < g_freq) return 0;
        g_primary.store(sc, std::memory_order_relaxed);
        g_deadline = 0;
    }
    g_primary_last.store(start, std::memory_order_relaxed);
    if (g_backoff.load(std::memory_order_relaxed)) {
        g_deadline = 0;
        return 0;
    }

    // Another limiter (RTSS, NVIDIA) at or below ours: run 0.3% under it so
    // it never engages and the two never fight. One below ours is the
    // user's choice and stands; ours then only adds precision.
    const int ext = config().ext_fps_limit;
    const int64_t interval = ext > 0 && ext <= fps ? g_freq * 1000 / (static_cast<int64_t>(ext) * 997) : g_freq / fps;
    if (g_deadline == 0 || start > g_deadline + interval) {
        g_deadline = start + interval;  // first frame, or fell behind: resync, never burst to catch up
        return 0;
    }
    if (start < g_deadline) wait_until(g_deadline);
    g_deadline += interval;
    const int64_t waited = qpc() - start;
    return static_cast<uint32_t>(waited * 1000000 / g_freq);
}

void pacing_backoff(const char* why) {
    if (g_backoff.exchange(true, std::memory_order_relaxed) || config().fps_limit <= 0) return;
    logf("limiter off: %s (the game paces frames itself)", why);
}

void pacing_background_tick() {
    if (config().fps_limit <= 0 || g_backoff.load(std::memory_order_relaxed)) return;
    for (const wchar_t* m : kPacingModules) {
        if (GetModuleHandleW(m)) {
            g_backoff.store(true, std::memory_order_relaxed);
            logf("limiter off: %ls is loaded (the game paces frames itself)", m);
            return;
        }
    }
}

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
