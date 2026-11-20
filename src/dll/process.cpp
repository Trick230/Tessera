// SPDX-License-Identifier: GPL-3.0-or-later
// P5: per-process scheduling hints, applied once at session start. Nothing
// outside the game process is touched; all of it ends with the process.
#include "process.h"

#include <windows.h>
#include <timeapi.h>

#include "log.h"

namespace tessera {
void apply_process_tweaks() {
    // Opt out of EcoQoS (efficiency cores, lower clocks) and keep timer
    // resolution requests honoured while the window is occluded or minimized.
    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;
    const bool qos = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt) != 0;

    // 1 ms timer: games and our limiter sleep with ms precision. Per process
    // since Windows 10 2004; released by the OS when the process exits.
    const bool timer = timeBeginPeriod(1) == TIMERR_NOERROR;
    logf("process: EcoQoS off %s, 1 ms timer %s", qos ? "ok" : "failed", timer ? "ok" : "failed");
}
}  // namespace tessera
