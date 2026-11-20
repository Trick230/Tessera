// SPDX-License-Identifier: GPL-3.0-or-later
// P5: per-process scheduling hints, applied once at session start. Nothing
// outside the game process is touched; all of it ends with the process.
#include "process.h"

#include <windows.h>
#include <timeapi.h>

#include "config.h"
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

    // One step up, never more: above normal keeps the game ahead of
    // background work without starving input, audio or the compositor. A
    // class the game or the user chose (anything but normal) is left alone.
    if (config().priority) {
        const DWORD cls = GetPriorityClass(GetCurrentProcess());
        if (cls == NORMAL_PRIORITY_CLASS) {
            const bool ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
            logf("process: above-normal priority %s", ok ? "ok" : "failed");
        } else {
            logf("process: priority class 0x%lx left alone", cls);
        }
    }
}
}  // namespace tessera
