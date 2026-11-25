// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include <dxgi.h>

namespace tessera {
// P1: called by the outermost Present hook before the real Present. Waits
// until the next frame deadline when fps_limit is set, the swap chain is the
// primary one. Nothing else (Reflex, driver caps, other tools) takes precedence.
// Returns the time waited in microseconds. The timer is created once per thread.
uint32_t pace_before_present(IDXGISwapChain* sc);
// P1: called by the outermost Present hook after the real Present. In the
// low-latency mode, holds the start of the next frame until its predicted
// work fits before the next deadline. Returns the time waited in microseconds.
uint32_t pace_after_present(IDXGISwapChain* sc);
// The same for a present path without DXGI (Vulkan): `chain` identifies the
// swap chain.
uint32_t pace_before_present_any(const void* chain);
uint32_t pace_after_present_any(const void* chain);
// P3: applies max_frame_latency to a new swap chain's device, unless the
// game manages latency itself with a waitable swap chain.
void pacing_on_swapchain(IDXGISwapChain* sc);
}  // namespace tessera
