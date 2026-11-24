// SPDX-License-Identifier: GPL-3.0-or-later
// Creates one Vulkan device through the Tessera layer with a feature chain and a
// presentation extension, so the capture records the game's device state.
#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) return 2;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    const char* layer = "VK_LAYER_TESSERA_telemetry";
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "fixture", 1, "fixture", 1, VK_API_VERSION_1_2};
    const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 1, &layer, 0, nullptr};
    VkInstance instance{};
    if (!create_instance || create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        fprintf(stderr, "cannot create an instance with the Tessera layer\n");
        return 2;
    }
    const auto fn = [&](const char* name) { return get(instance, name); };
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fn("vkEnumeratePhysicalDevices"));
    uint32_t count = 1;
    VkPhysicalDevice physical{};
    if (enumerate(instance, &count, &physical) < 0 || !count) return 2;
    const auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(fn("vkGetPhysicalDeviceFeatures2"));
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
    VkPhysicalDeviceFeatures2 all{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    features2(physical, &all);
    // A game-like subset: robust buffer access off, descriptor indexing on when available.
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    want12.descriptorIndexing = v12.descriptorIndexing;
    want12.runtimeDescriptorArray = v12.runtimeDescriptorArray;
    want12.timelineSemaphore = v12.timelineSemaphore;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &want12};
    want.features.shaderInt64 = all.features.shaderInt64;
    const auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        fn("vkGetPhysicalDeviceQueueFamilyProperties"));
    count = 0;
    queues(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    queues(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) return 2;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
    // The swapchain extension must be recorded but left out of the replay (it needs a surface).
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want, 0, 1, &queue, 0, nullptr, 1, extensions, nullptr};
    const auto create_device = reinterpret_cast<PFN_vkCreateDevice>(fn("vkCreateDevice"));
    VkDevice device{};
    if (create_device(physical, &info, nullptr, &device) != VK_SUCCESS) {
        fprintf(stderr, "cannot create the fixture device\n");
        return 2;
    }
    const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(fn("vkGetDeviceProcAddr"));
    reinterpret_cast<PFN_vkDestroyDevice>(get_device(device, "vkDestroyDevice"))(device, nullptr);
    reinterpret_cast<PFN_vkDestroyInstance>(fn("vkDestroyInstance"))(instance, nullptr);
    return 0;
}
