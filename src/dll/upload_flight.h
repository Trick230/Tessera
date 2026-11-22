// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace tessera {
// The GPU fence signals cumulative bytes in immediate-context order. A
// second swap chain may report an older value from its own fence.
struct UploadFlight {
    static constexpr uint64_t limit = 8ull << 20;
    uint64_t submitted = 0;
    uint64_t completed = 0;

    void observe(uint64_t value) {
        if (value > completed && value <= submitted) completed = value;
    }
    uint64_t bytes() const { return submitted - completed; }
    uint64_t budget(uint64_t frame_room) const {
        const uint64_t pending = bytes();
        if (pending >= limit) return 0;
        const uint64_t room = limit - pending;
        return frame_room < room ? frame_room : room;
    }
    // A predicted restore may involve allocation and mapping as well as a copy.
    // Let previous uploads finish before starting one.
    bool can_restore() const { return bytes() == 0; }
};
}  // namespace tessera
