// SPDX-License-Identifier: GPL-3.0-or-later
// Frame limiter benchmark on Vulkan: the same synthetic load as
// d3d11_smoke --limiter-bench, presented through vulkan-1.dll from the exe
// folder (the layer, when copied there). Write the CSV, then read it with
//   d3d11_smoke --limiter-report <sessions dir> <csv> <fps>
//   tessera_vulkan_present <fps> <csv>
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    return 1;
}

constexpr int kFrames = 480;
int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
// Same load as tests/d3d11_smoke/limiter_bench.cpp.
int frame_kind(int i) {
    if (i % 120 == 60) return 2;
    if (i >= 240 && i < 300) return 3;
    if (i % 25 == 7) return 1;
    return 0;
}
double frame_work(int i, uint32_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    const double noise = (rng >> 8) / double(1u << 24);
    switch (frame_kind(i)) {
        case 2: return 2.7;
        case 3: return 0.60 + 0.10 * noise;
        case 1: return 0.66;
        default: return 0.42 + 0.12 * noise;
    }
}
struct Row {
    int64_t start, end;
    int kind;
};
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return fail("usage: tessera_vulkan_present <fps> <csv>");
    const int fps = _wtoi(argv[1]);
    if (fps < 20) return fail("fps");
    // Plain search order: the exe folder first, like a game.
    const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return fail("vulkan-1.dll");
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    const auto present_export = reinterpret_cast<PFN_vkQueuePresentKHR>(GetProcAddress(loader, "vkQueuePresentKHR"));
    if (!gipa) return fail("vkGetInstanceProcAddr");
#define LOAD(inst, name) const auto name = reinterpret_cast<PFN_##name>(gipa(inst, #name))
    LOAD(nullptr, vkCreateInstance);
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "limiter bench", 1, "tessera", 1,
                                VK_API_VERSION_1_1};
    const VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, iext};
    VkInstance inst{};
    if (!vkCreateInstance || vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return fail("instance");
    LOAD(inst, vkEnumeratePhysicalDevices);
    LOAD(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(inst, vkCreateWin32SurfaceKHR);
    LOAD(inst, vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD(inst, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD(inst, vkGetPhysicalDeviceSurfacePresentModesKHR);
    LOAD(inst, vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD(inst, vkCreateDevice);
    LOAD(inst, vkGetDeviceProcAddr);
    LOAD(inst, vkDestroySurfaceKHR);
    LOAD(inst, vkDestroyInstance);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"tessera_vulkan_bench";
    RegisterClassW(&wc);
    const HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"vulkan limiter bench", WS_OVERLAPPEDWINDOW, 0, 0, 320,
                                     180, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return fail("window");
    const VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, wc.hInstance,
                                          wnd};
    VkSurfaceKHR surface{};
    if (vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surface) != VK_SUCCESS) return fail("surface");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(inst, &n, gpus.data());
    VkPhysicalDevice gpu{};
    uint32_t family = UINT32_MAX;
    for (VkPhysicalDevice g : gpus) {
        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &nf, nullptr);
        std::vector<VkQueueFamilyProperties> props(nf);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &nf, props.data());
        for (uint32_t f = 0; f < nf && family == UINT32_MAX; ++f) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, f, surface, &ok);
            if (ok && (props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family = f;
        }
        if (family != UINT32_MAX) {
            gpu = g;
            break;
        }
    }
    if (!gpu) return fail("no presenting GPU");
    const float prio = 1.0f;
    const VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &prio};
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, dext,
                                 nullptr};
    VkDevice dev{};
    if (vkCreateDevice(gpu, &dci, nullptr, &dev) != VK_SUCCESS) return fail("device");
#define DLOAD(name) const auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name))
    DLOAD(vkGetDeviceQueue);
    DLOAD(vkCreateSwapchainKHR);
    DLOAD(vkGetSwapchainImagesKHR);
    DLOAD(vkAcquireNextImageKHR);
    DLOAD(vkQueuePresentKHR);
    DLOAD(vkCreateSemaphore);
    DLOAD(vkCreateFence);
    DLOAD(vkWaitForFences);
    DLOAD(vkResetFences);
    DLOAD(vkCreateCommandPool);
    DLOAD(vkAllocateCommandBuffers);
    DLOAD(vkBeginCommandBuffer);
    DLOAD(vkEndCommandBuffer);
    DLOAD(vkCmdPipelineBarrier);
    DLOAD(vkQueueSubmit);
    DLOAD(vkDeviceWaitIdle);
    DLOAD(vkDestroySwapchainKHR);
    DLOAD(vkDestroySemaphore);
    DLOAD(vkDestroyFence);
    DLOAD(vkDestroyCommandPool);
    DLOAD(vkDestroyDevice);
    // Through the layer, both lookups must land on its present hook.
    std::printf("present via export and device lookup: %s\n",
                present_export && reinterpret_cast<void*>(present_export) == reinterpret_cast<void*>(vkQueuePresentKHR)
                    ? "same"
                    : "different");
    VkQueue queue{};
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &nm, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // the limiter, not vsync, must set the pace
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    if (mode == VK_PRESENT_MODE_FIFO_KHR)
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    uint32_t nfmt = 1;
    VkSurfaceFormatKHR fmt{};
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &nfmt, &fmt);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = {64, 64};
    VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = surface;
    swci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    swci.imageFormat = fmt.format;
    swci.imageColorSpace = fmt.colorSpace;
    swci.imageExtent = extent;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = mode;
    swci.clipped = VK_TRUE;
    VkSwapchainKHR sc{};
    if (vkCreateSwapchainKHR(dev, &swci, nullptr, &sc) != VK_SUCCESS) return fail("swapchain");
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(dev, sc, &ni, nullptr);
    std::vector<VkImage> images(ni);
    vkGetSwapchainImagesKHR(dev, sc, &ni, images.data());

    // One command buffer per image: move it to the present layout.
    const VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, family};
    VkCommandPool pool{};
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    std::vector<VkCommandBuffer> cmds(ni);
    const VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, ni};
    vkAllocateCommandBuffers(dev, &cai, cmds.data());
    for (uint32_t i = 0; i < ni; ++i) {
        const VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmds[i], &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[i];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmds[i], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cmds[i]);
    }
    const VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphore acquired{};
    VkFence done{};
    vkCreateSemaphore(dev, &semi, nullptr, &acquired);
    vkCreateFence(dev, &fi, nullptr, &done);
    std::vector<VkSemaphore> rendered(ni);
    for (auto& s : rendered) vkCreateSemaphore(dev, &semi, nullptr, &s);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double interval = double(freq.QuadPart) / fps;
    std::vector<Row> rows;
    rows.reserve(kFrames);
    uint32_t rng = 12345;
    // Alternate the export and the device-level pointer, as games do either.
    for (int i = -1; i < kFrames; ++i) {
        const int64_t start = qpc();
        if (i >= 0) {
            const int64_t end = start + static_cast<int64_t>(frame_work(i, rng) * interval);
            while (qpc() < end) YieldProcessor();
        }
        const int64_t work_end = qpc();
        vkWaitForFences(dev, 1, &done, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &done);
        uint32_t idx = 0;
        if (vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx) < 0) return fail("acquire");
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acquired, &stage, 1, &cmds[idx], 1,
                              &rendered[idx]};
        if (vkQueueSubmit(queue, 1, &si, done) != VK_SUCCESS) return fail("submit");
        const VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &rendered[idx], 1, &sc, &idx,
                                  nullptr};
        const PFN_vkQueuePresentKHR present = (i & 1) && present_export ? present_export : vkQueuePresentKHR;
        if (present(queue, &pi) < 0) return fail("present");
        if (i >= 0) rows.push_back({start, work_end, frame_kind(i)});
    }
    vkDeviceWaitIdle(dev);
    for (auto s : rendered) vkDestroySemaphore(dev, s, nullptr);
    vkDestroySemaphore(dev, acquired, nullptr);
    vkDestroyFence(dev, done, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroySwapchainKHR(dev, sc, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroySurfaceKHR(inst, surface, nullptr);
    vkDestroyInstance(inst, nullptr);
    DestroyWindow(wnd);

    FILE* f = nullptr;
    if (_wfopen_s(&f, argv[2], L"w") || !f) return fail("open csv");
    std::fprintf(f, "start_qpc,work_end_qpc,kind\n");
    for (const auto& r : rows) std::fprintf(f, "%lld,%lld,%d\n", r.start, r.end, r.kind);
    std::fclose(f);
    return 0;
}
