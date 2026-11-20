// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include <dxgi.h>

namespace tessera {
// P1: called by the outermost Present hook before the real Present. Waits
// until the next frame deadline when fps_limit is set, the swap chain is the
// primary one and no Reflex / frame generation is present (P6). Returns the
// time waited in microseconds. No allocation; the timer is created once.
uint32_t pace_before_present(IDXGISwapChain* sc);

// P6: polled from the session writer thread (never the frame path): looks
// for modules that own pacing themselves and makes the limiter back off.
void pacing_background_tick();

// P3: applies max_frame_latency to a new swap chain's device, unless the
// game manages latency itself with a waitable swap chain.
void pacing_on_swapchain(IDXGISwapChain* sc);
}  // namespace tessera
