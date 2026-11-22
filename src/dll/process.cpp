// SPDX-License-Identifier: GPL-3.0-or-later
// P5: per-process scheduling hints, applied once at session start. Nothing
// outside the game process is touched; all of it ends with the process.
#include "process.h"

#include <windows.h>
#include <timeapi.h>

#include "config.h"
#include "hooks.h"
#include "log.h"

namespace tessera {
namespace {
constexpr ULONG kMaxSets = 1024;
struct CpuSet {
    ULONG id;
    WORD group;
    BYTE processor;
    BYTE efficiency;
};
CpuSet g_sets[kMaxSets];
ULONG g_count = 0;
bool g_probed = false;
HANDLE g_selected = nullptr;
DWORD g_candidate = 0, g_attempted = 0;
ULONG g_assigned[kMaxSets], g_assigned_count = 0;

void probe_cpu_sets() {
    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
    if (!size || size > 1024 * 1024) return;
    auto* buffer = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!buffer) return;
    if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer), size, &size,
                                   GetCurrentProcess(), 0)) {
        for (ULONG offset = 0; size - offset >= sizeof(SYSTEM_CPU_SET_INFORMATION);) {
            const auto& item = *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer + offset);
            if (item.Size < sizeof item || item.Size > size - offset) break;
            if (item.Type == CpuSetInformation && (!item.CpuSet.Allocated || item.CpuSet.AllocatedToTargetProcess)) {
                if (g_count == kMaxSets) {
                    g_count = 0;
                    break;
                }
                g_sets[g_count++] = {item.CpuSet.Id, item.CpuSet.Group, item.CpuSet.LogicalProcessorIndex,
                                     item.CpuSet.EfficiencyClass};
            }
            offset += item.Size;
        }
    }
    HeapFree(GetProcessHeap(), 0, buffer);
}

void release_selected_thread() {
    if (!g_selected) return;
    ULONG current[kMaxSets], count = 0;
    if (GetThreadSelectedCpuSets(g_selected, current, kMaxSets, &count) && count == g_assigned_count) {
        bool same = true;
        for (ULONG i = 0; i < count; ++i) same &= current[i] == g_assigned[i];
        if (same) SetThreadSelectedCpuSets(g_selected, nullptr, 0);
    }
    CloseHandle(g_selected);
    g_selected = nullptr;
}

bool select_fast_cores(HANDLE thread) {
    ULONG selected = 0, defaults = 0;
    GROUP_AFFINITY affinity = {};
    if (!GetThreadSelectedCpuSets(thread, nullptr, 0, &selected) || selected ||
        !GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &defaults) || defaults ||
        !GetThreadGroupAffinity(thread, &affinity))
        return false;
    BYTE fastest = 0;
    ULONG available = 0;
    g_assigned_count = 0;
    for (ULONG i = 0; i < g_count; ++i) {
        const auto& cpu = g_sets[i];
        if (cpu.group != affinity.Group || cpu.processor >= sizeof(KAFFINITY) * 8 ||
            !(affinity.Mask & (KAFFINITY{1} << cpu.processor)))
            continue;
        ++available;
        if (cpu.efficiency < fastest) continue;
        if (cpu.efficiency > fastest) {
            fastest = cpu.efficiency;
            g_assigned_count = 0;
        }
        g_assigned[g_assigned_count++] = cpu.id;
    }
    return g_assigned_count && g_assigned_count < available &&
           SetThreadSelectedCpuSets(thread, g_assigned, g_assigned_count);
}
}  // namespace

void present_cpu_sets_tick() {
    if (!g_probed) {
        probe_cpu_sets();
        g_probed = true;
    }
    if (!g_count) return;
    const DWORD tid = last_present_thread();
    // Wait for the same presenter on two successive samples before migrating it.
    if (g_candidate != tid) {
        g_candidate = tid;
        return;
    }
    if (!tid || tid == g_attempted) return;
    g_attempted = tid;
    release_selected_thread();
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, tid);
    if (!thread) return;
    if (GetProcessIdOfThread(thread) == GetCurrentProcessId() && select_fast_cores(thread)) {
        g_selected = thread;
        logf("CPU sets: Present thread %lu uses %lu performance CPU sets", tid, g_assigned_count);
    } else {
        CloseHandle(thread);
    }
}

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
