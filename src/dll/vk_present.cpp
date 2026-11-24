// SPDX-License-Identifier: GPL-3.0-or-later
// vulkan-1.dll proxy (TESSERA_PROXY_VULKAN): the frame limiter for Vulkan
// games. Only the present path is touched: vkQueuePresentKHR is wrapped,
// whether the game calls the export or gets it from vkGet*ProcAddr. Every
// other function is the real loader's. VK_NV_low_latency2 sleep modes are
// watched so the limiter yields to Reflex, as it does for DXGI.
// Needs no Vulkan SDK: the few types used are declared here.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "exports_vulkan_gen.h"
#include "log.h"
#include "pacing.h"
#include "session.h"

extern "C" void* tessera_real[];
void* resolve_export(unsigned idx);

namespace {
#ifdef _M_IX86
#define VKAPI __stdcall
#else
#define VKAPI
#endif
using VkResult = int32_t;
using VkHandle = void*;           // dispatchable: instance, device, queue
using VkSwapchainKHR = uint64_t;  // non-dispatchable handles are 64-bit everywhere
using PFN_vkVoidFunction = void(VKAPI*)();
using PFN_GetInstanceProcAddr = PFN_vkVoidFunction(VKAPI*)(VkHandle, const char*);
using PFN_GetDeviceProcAddr = PFN_vkVoidFunction(VKAPI*)(VkHandle, const char*);
struct VkPresentInfoKHR {
    int32_t sType;
    const void* pNext;
    uint32_t waitSemaphoreCount;
    const uint64_t* pWaitSemaphores;
    uint32_t swapchainCount;
    const VkSwapchainKHR* pSwapchains;
    const uint32_t* pImageIndices;
    VkResult* pResults;
};
struct VkLatencySleepModeInfoNV {
    int32_t sType;
    const void* pNext;
    uint32_t lowLatencyMode;
    uint32_t lowLatencyBoost;
    uint32_t minimumIntervalUs;
};
using PFN_QueuePresent = VkResult(VKAPI*)(VkHandle, const VkPresentInfoKHR*);
using PFN_SetLatencySleepMode = VkResult(VKAPI*)(VkHandle, VkSwapchainKHR, const VkLatencySleepModeInfoNV*);
constexpr VkResult kSuccess = 0, kSuboptimal = 1000001003, kExtensionNotPresent = -7,
                   kDeviceLost = -4;

template <class F>
F real(int idx) {
    void* p = tessera_real[idx];
    return reinterpret_cast<F>(p ? p : resolve_export(static_cast<unsigned>(idx)));
}

// The loader's vkQueuePresentKHR export dispatches on the queue, so it is the
// right target for every device, with whatever layers the game enabled.
std::atomic<PFN_QueuePresent> g_present{nullptr};
PFN_QueuePresent present_target() {
    PFN_QueuePresent p = g_present.load(std::memory_order_acquire);
    if (!p) {
        p = real<PFN_QueuePresent>(tessera::exports::kvkQueuePresentKHR);
        g_present.store(p, std::memory_order_release);
    }
    return p;
}

// vkSetLatencySleepModeNV is device-level: keep the real pointer per device
// (dispatch key), plus the instance-level loader trampoline that works for any.
struct SleepEntry {
    std::atomic<void*> key{nullptr};
    std::atomic<PFN_SetLatencySleepMode> fn{nullptr};
};
SleepEntry g_sleep[8];
std::atomic<PFN_SetLatencySleepMode> g_sleep_any{nullptr};
void* dispatch_key(VkHandle h) { return h ? *static_cast<void**>(h) : nullptr; }

bool remember_sleep(VkHandle device, PFN_SetLatencySleepMode fn) {
    void* key = dispatch_key(device);
    for (SleepEntry& e : g_sleep) {
        void* k = e.key.load(std::memory_order_acquire);
        if (k == key) {
            e.fn.store(fn, std::memory_order_release);
            return true;
        }
        void* expected = nullptr;
        if (!k && e.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
            e.fn.store(fn, std::memory_order_release);
            return true;
        }
        if (expected == key) {
            e.fn.store(fn, std::memory_order_release);
            return true;
        }
    }
    return false;  // table full: the caller hands out the real pointer
}

PFN_SetLatencySleepMode find_sleep(VkHandle device) {
    void* key = dispatch_key(device);
    for (SleepEntry& e : g_sleep)
        if (e.key.load(std::memory_order_acquire) == key)
            if (PFN_SetLatencySleepMode fn = e.fn.load(std::memory_order_acquire)) return fn;
    return g_sleep_any.load(std::memory_order_acquire);
}

std::atomic<uint32_t> g_start_wait{0};
std::atomic<bool> g_logged{false};
thread_local int t_depth = 0;

bool layer_on() {
    static std::atomic<int> state{0};  // 0 unknown, 1 on, 2 off (safe mode or init failure)
    int s = state.load(std::memory_order_acquire);
    if (!s) {
        s = tessera::session_init() ? 1 : 2;
        state.store(s, std::memory_order_release);
    }
    return s == 1;
}
}  // namespace

extern "C" VkResult VKAPI Tessera_vkQueuePresentKHR(VkHandle queue, const VkPresentInfoKHR* info);
extern "C" VkResult VKAPI Tessera_vkSetLatencySleepModeNV(VkHandle device, VkSwapchainKHR sc,
                                                          const VkLatencySleepModeInfoNV* mode);
extern "C" PFN_vkVoidFunction VKAPI Tessera_vkGetDeviceProcAddr(VkHandle device, const char* name);

extern "C" VkResult VKAPI Tessera_vkQueuePresentKHR(VkHandle queue, const VkPresentInfoKHR* info) {
    const PFN_QueuePresent fn = present_target();
    if (!fn) return kDeviceLost;  // a valid vkQueuePresentKHR error
    const bool frame = t_depth == 0 && info && info->swapchainCount && info->pSwapchains && layer_on();
    if (!frame) return fn(queue, info);
    if (!g_logged.exchange(true, std::memory_order_relaxed)) tessera::logf("vulkan: present hooked");
    // The first swap chain of the call identifies the chain the limiter paces.
    const void* chain = reinterpret_cast<const void*>(static_cast<uintptr_t>(info->pSwapchains[0]));
    ++t_depth;
    tessera::session_frame(tessera::pace_before_present_any(chain), g_start_wait.exchange(0, std::memory_order_relaxed));
    const VkResult r = fn(queue, info);
    if (r == kSuccess || r == kSuboptimal)
        g_start_wait.store(tessera::pace_after_present_any(chain), std::memory_order_relaxed);
    --t_depth;
    return r;
}

extern "C" VkResult VKAPI Tessera_vkSetLatencySleepModeNV(VkHandle device, VkSwapchainKHR sc,
                                                          const VkLatencySleepModeInfoNV* mode) {
    const PFN_SetLatencySleepMode fn = find_sleep(device);
    if (!fn) return kExtensionNotPresent;
    const VkResult r = fn(device, sc, mode);
    if (r == kSuccess && layer_on())
        tessera::pacing_set_external_pacing(mode && (mode->lowLatencyMode || mode->minimumIntervalUs));
    return r;
}

// Device-level lookups: present and the Reflex sleep mode get wrappers.
extern "C" PFN_vkVoidFunction VKAPI Tessera_vkGetDeviceProcAddr(VkHandle device, const char* name) {
    const auto gdpa = real<PFN_GetDeviceProcAddr>(tessera::exports::kvkGetDeviceProcAddr);
    if (!gdpa) return nullptr;
    const PFN_vkVoidFunction p = gdpa(device, name);
    if (!p || !name) return p;
    if (!strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkQueuePresentKHR);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkGetDeviceProcAddr);
    if (!strcmp(name, "vkSetLatencySleepModeNV")) {
        if (!remember_sleep(device, reinterpret_cast<PFN_SetLatencySleepMode>(p))) return p;
        return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkSetLatencySleepModeNV);
    }
    return p;
}

extern "C" PFN_vkVoidFunction VKAPI Tessera_vkGetInstanceProcAddr(VkHandle instance, const char* name) {
    const auto gipa = real<PFN_GetInstanceProcAddr>(tessera::exports::kvkGetInstanceProcAddr);
    if (!gipa) return nullptr;
    const PFN_vkVoidFunction p = gipa(instance, name);
    if (!p || !name) return p;
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkGetInstanceProcAddr);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkGetDeviceProcAddr);
    if (!strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkQueuePresentKHR);
    if (!strcmp(name, "vkSetLatencySleepModeNV")) {
        // A loader trampoline: valid for every device of this instance.
        g_sleep_any.store(reinterpret_cast<PFN_SetLatencySleepMode>(p), std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(&Tessera_vkSetLatencySleepModeNV);
    }
    return p;
}
