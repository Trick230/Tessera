// SPDX-License-Identifier: GPL-3.0-or-later
// Low-latency frame limiter.
//
// Every frame has a target present time on a fixed cadence. In kLowLatency
// mode the hook holds the start of the next frame (right after the present
// returns, before the game samples input) until target - predicted work -
// margin, then aligns the present itself to the target. Input is therefore
// sampled about `margin` before the frame is presented, instead of up to a
// full interval as with a wait just before the present.
//
// Stutter: a frame that arrives late is presented at once and the cadence
// restarts from it, so nothing is caught up (no burst of short frames) and no
// latency builds up. A single long frame does not enter the work prediction;
// a sustained heavier load does after a few frames. The margin grows on small
// misses (prediction error) and shrinks back while frames are on time.
#include "limiter.h"

#include <windows.h>

#include <algorithm>
#include <atomic>

namespace tessera::limiter {
namespace {
const int64_t g_freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);  // never fails since Windows XP
    return f.QuadPart;
}();
int64_t us_to_qpc(int64_t us) { return us * g_freq / 1000000; }
uint32_t qpc_to_us(int64_t t) { return t > 0 ? static_cast<uint32_t>(t * 1000000 / g_freq) : 0; }

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

thread_local HANDLE t_timer = nullptr;

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

constexpr int kWindow = 32;     // frames of work history
constexpr int kMinSamples = 8;  // before this, no start-of-frame wait
constexpr int kHeavyRun = 3;    // consecutive long frames that count as a new load

SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_reset{false};  // applied by the next paced frame
const void* g_primary = nullptr;
int64_t g_primary_last = 0;  // last release of the primary chain
int g_fps = 0;
int g_mode = -1;
int64_t g_interval = 0;
int64_t g_target = 0;       // next present target, 0 = none yet
int64_t g_start = 0;        // start of the current frame, 0 = unknown
int64_t g_next_start = 0;   // kFrameStart: next frame start
bool g_in_frame = false;    // before_present ran for the primary chain
uint32_t g_work[kWindow];   // work of recent frames, qpc ticks (clamped)
int g_work_n = 0, g_work_at = 0, g_heavy = 0;
int64_t g_predicted = 0;    // high percentile of g_work
int64_t g_margin = 0;
int g_clean = 0;            // on-time frames since the last margin change

int64_t margin_floor() { return std::min(us_to_qpc(250), g_interval / 8); }
int64_t margin_cap() { return std::min(us_to_qpc(2000), g_interval / 4); }

void clear_timing() {
    g_target = g_start = g_next_start = 0;
    g_in_frame = false;
    g_work_n = g_work_at = g_heavy = 0;
    g_predicted = 0;
    g_margin = us_to_qpc(500);
    g_clean = 0;
}

void add_work(int64_t work) {
    // One long frame is a stutter, not the load: keep it out of the
    // prediction unless the load stays that heavy.
    if (g_work_n >= kMinSamples && work > 2 * g_predicted + us_to_qpc(2000)) {
        if (++g_heavy < kHeavyRun) return;
    } else {
        g_heavy = 0;
    }
    g_work[g_work_at] = static_cast<uint32_t>(std::min<int64_t>(work, 0x7FFFFFFF));
    g_work_at = (g_work_at + 1) % kWindow;
    if (g_work_n < kWindow) ++g_work_n;
}

void predict() {
    if (g_work_n < kMinSamples) {
        g_predicted = g_interval;  // unknown load: start at once
        return;
    }
    uint32_t copy[kWindow];
    std::copy(g_work, g_work + g_work_n, copy);
    const int at = (g_work_n * 9) / 10;  // 90th percentile
    std::nth_element(copy, copy + at, copy + g_work_n);
    g_predicted = std::min<int64_t>(copy[at], g_interval);
}

// Late arrival by a small amount: the prediction was short, widen the margin.
// A long stutter says nothing about the prediction.
void adapt_margin(int64_t now) {
    if (!g_target) return;
    const int64_t late = now - g_target;
    if (late > 0 && late < g_interval / 2) {
        g_margin = std::min(margin_cap(), g_margin + us_to_qpc(100));
        g_clean = 0;
    } else if (late <= 0 && ++g_clean >= 120) {
        g_margin = std::max(margin_floor(), g_margin - us_to_qpc(50));
        g_clean = 0;
    }
}

// The caller holds g_lock. False when `chain` is not the one to pace.
bool select(const void* chain, int fps, int mode, int64_t now) {
    if (g_primary != chain) {
        if (g_primary && now - g_primary_last < g_freq) return false;
        g_primary = chain;
        clear_timing();
    }
    if (fps != g_fps || mode != g_mode) {
        g_fps = fps;
        g_mode = mode;
        g_interval = g_freq / fps;
        clear_timing();
    }
    if (g_reset.exchange(false, std::memory_order_acquire)) clear_timing();
    return true;
}
}  // namespace

uint32_t before_present(const void* chain, int fps, int mode) {
    if (fps <= 0 || !TryAcquireSRWLockExclusive(&g_lock)) return 0;
    const int64_t now = qpc();
    uint32_t waited = 0;
    if (select(chain, fps, mode, now)) {
        int64_t release = now;
        if (mode == kFrameStart) {
            // Cadence is held at the start of the frame; present when done.
        } else {
            if (mode == kLowLatency) {
                if (g_start) add_work(now - g_start);
                adapt_margin(now);
            }
            if (g_target && now < g_target) {
                wait_until(g_target);
                release = qpc();
            }
            // On the grid (early or within the spin overshoot): keep it, so
            // the cadence does not drift. Late: restart from this present,
            // never squeezing the next interval to catch up.
            g_target = g_target && release - g_target <= us_to_qpc(200) ? g_target + g_interval
                                                                         : release + g_interval;
        }
        waited = qpc_to_us(release - now);
        g_primary_last = release;
        g_in_frame = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return waited;
}

uint32_t after_present(const void* chain, int fps, int mode) {
    if (fps <= 0 || mode == kPresentEdge || !TryAcquireSRWLockExclusive(&g_lock)) return 0;
    uint32_t waited = 0;
    if (chain == g_primary && g_in_frame && fps == g_fps && mode == g_mode) {
        g_in_frame = false;
        const int64_t now = qpc();
        int64_t start = now;
        if (mode == kLowLatency) {
            predict();
            const int64_t at = g_target - g_predicted - g_margin;
            if (g_target && now < at) {
                wait_until(at);
                start = qpc();
            }
        } else if (g_next_start && now < g_next_start) {  // kFrameStart
            wait_until(g_next_start);
            start = qpc();
        }
        if (mode == kFrameStart)
            g_next_start = g_next_start && start - g_next_start <= us_to_qpc(200) ? g_next_start + g_interval
                                                                                   : start + g_interval;
        g_start = start;
        g_primary_last = start;
        waited = qpc_to_us(start - now);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return waited;
}

void reset() { g_reset.store(true, std::memory_order_release); }
}  // namespace tessera::limiter
