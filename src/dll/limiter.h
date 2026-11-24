// SPDX-License-Identifier: GPL-3.0-or-later
// Frame limiter core shared by every presentation hook (DXGI for D3D11/D3D12,
// vkQueuePresentKHR for Vulkan). No graphics API dependency, no allocation.
#pragma once
#include <cstdint>

namespace tessera::limiter {
enum Mode : int {
    // Waits just before the present: steady cadence, the frame waits after the
    // game sampled input (latency up to one interval).
    kPresentEdge = 0,
    // Waits at the start of the next frame for the time its work is predicted
    // not to need, then aligns the present to the cadence: steady cadence
    // with input sampled as late as the frame allows.
    kLowLatency = 1,
    // Waits only at the start of the frame, on a fixed cadence; presents
    // whenever the frame is done (comparison mode).
    kFrameStart = 2,
};

// The outermost present call of `chain`, before the real present. Only the
// primary chain (the first presenting, replaced after a second of silence) is
// paced; another chain presenting concurrently is never blocked.
// Returns the time waited in microseconds.
uint32_t before_present(const void* chain, int fps, int mode);

// After the real present of `chain` returned. Returns the time waited in
// microseconds (the start of the next frame was held).
uint32_t after_present(const void* chain, int fps, int mode);

// Forgets the cadence and prediction (another pacer took over, or resumed).
void reset();
}  // namespace tessera::limiter
