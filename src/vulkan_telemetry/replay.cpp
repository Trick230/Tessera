// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "compute_recipe.h"

#include <cstdio>
#include <memory>

namespace {
using namespace tessera::vulkan_recipe;

struct Context {
    HMODULE loader{};
    VkInstance instance{};
    VkDevice device{};
    VkPipelineCache cache{};
    PFN_vkGetInstanceProcAddr get_instance{};
    PFN_vkGetDeviceProcAddr get_device{};

    ~Context() {
        if (cache) fn<PFN_vkDestroyPipelineCache>("vkDestroyPipelineCache")(device, cache, nullptr);
        if (device) fn<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
        if (instance) instance_fn<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (loader) FreeLibrary(loader);
    }

    template<class T>
    T instance_fn(const char* name) const { return reinterpret_cast<T>(get_instance(instance, name)); }

    template<class T>
    T fn(const char* name) const { return reinterpret_cast<T>(get_device(device, name)); }

    bool initialize(const Header& header) {
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader) return false;
        get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!get_instance) return false;
        const auto create = instance_fn<PFN_vkCreateInstance>("vkCreateInstance");
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Tessera", 1,
            "Tessera", 1, header.api};
        const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,
            &app, 0, nullptr, 0, nullptr};
        if (!create || create(&instance_info, nullptr, &instance) != VK_SUCCESS) return false;
        const auto count_devices = instance_fn<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        const auto properties = instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        uint32_t count = 0;
        if (!count_devices || !properties || count_devices(instance, &count, nullptr) != VK_SUCCESS || !count || count > 256)
            return false;
        std::vector<VkPhysicalDevice> physical_devices(count);
        if (count_devices(instance, &count, physical_devices.data()) != VK_SUCCESS) return false;
        VkPhysicalDevice physical{};
        for (const auto found : physical_devices) {
            VkPhysicalDeviceProperties props{};
            properties(found, &props);
            if (props.apiVersion >= header.api && props.vendorID == header.vendor && props.deviceID == header.device
                && std::memcmp(props.pipelineCacheUUID, header.cache_uuid, VK_UUID_SIZE) == 0) {
                physical = found;
                break;
            }
        }
        if (!physical) {
            fprintf(stderr, "the recorded Vulkan device and driver cache UUID are unavailable\n");
            return false;
        }
        const auto queue_properties = instance_fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
        const auto features = instance_fn<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
        const auto make_device = instance_fn<PFN_vkCreateDevice>("vkCreateDevice");
        get_device = instance_fn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        if (!queue_properties || !features || !make_device || !get_device) return false;
        count = 0;
        queue_properties(physical, &count, nullptr);
        if (!count || count > 256) return false;
        std::vector<VkQueueFamilyProperties> queues(count);
        queue_properties(physical, &count, queues.data());
        uint32_t index = 0;
        while (index < count && (!(queues[index].queueFlags & VK_QUEUE_COMPUTE_BIT) || !queues[index].queueCount)) ++index;
        if (index == count) return false;
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, index, 1, &priority};
        VkPhysicalDeviceFeatures available{};
        features(physical, &available);
        const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queue, 0,
            nullptr, 0, nullptr, &available};
        const VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, 0, nullptr};
        return make_device(physical, &info, nullptr, &device) == VK_SUCCESS
            && fn<PFN_vkCreatePipelineCache>("vkCreatePipelineCache")(device, &cache_info, nullptr, &cache) == VK_SUCCESS;
    }
};

struct PipelineObjects {
    const Context& context;
    std::vector<VkDescriptorSetLayout> sets;
    VkPipelineLayout layout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};

    ~PipelineObjects() {
        const auto device = context.device;
        if (pipeline) context.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, pipeline, nullptr);
        if (shader) context.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, shader, nullptr);
        if (layout) context.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, layout, nullptr);
        const auto destroy = context.fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
        for (const auto set : sets) if (set) destroy(device, set, nullptr);
    }
};

VkResult replay(const Context& context, const Recipe& recipe) {
    PipelineObjects objects{context, std::vector<VkDescriptorSetLayout>(recipe.layout.sets.size())};
    const auto make_set = context.fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (size_t i = 0; i < objects.sets.size(); ++i) {
        bindings.clear();
        for (const auto& b : recipe.layout.sets[i]) bindings.push_back({b.number, b.type, b.count, b.stages, nullptr});
        const VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr,
            0, static_cast<uint32_t>(bindings.size()), bindings.data()};
        const auto status = make_set(context.device, &info, nullptr, &objects.sets[i]);
        if (status != VK_SUCCESS) return status;
    }
    const VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
        static_cast<uint32_t>(objects.sets.size()), objects.sets.data(),
        static_cast<uint32_t>(recipe.layout.push.size()), recipe.layout.push.data()};
    auto status = context.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(
        context.device, &layout, nullptr, &objects.layout);
    if (status != VK_SUCCESS) return status;
    const VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        recipe.code.size() * sizeof(uint32_t), recipe.code.data()};
    status = context.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &module, nullptr, &objects.shader);
    if (status != VK_SUCCESS) return status;
    std::vector<VkSpecializationMapEntry> constants;
    constants.reserve(recipe.constants.size());
    for (const auto& entry : recipe.constants) constants.push_back({entry.id, entry.offset, entry.size});
    const VkSpecializationInfo spec{static_cast<uint32_t>(constants.size()), constants.data(),
        recipe.data.size(), recipe.data.data()};
    const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
        0, VK_SHADER_STAGE_COMPUTE_BIT, objects.shader, recipe.entry.c_str(), constants.empty() ? nullptr : &spec};
    const VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        stage, objects.layout, VK_NULL_HANDLE, -1};
    return context.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        context.device, context.cache, 1, &info, nullptr, &objects.pipeline);
}

bool save_cache(const Context& context, const wchar_t* filename) {
    const auto read = context.fn<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
    size_t size = 0;
    if (read(context.device, context.cache, &size, nullptr) != VK_SUCCESS
        || size < sizeof(VkPipelineCacheHeaderVersionOne) || size > file_limit) return false;
    std::vector<char> cache(size);
    if (read(context.device, context.cache, &size, cache.data()) != VK_SUCCESS) return false;
    const std::wstring temporary = std::wstring(filename) + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, cache.data(), static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(file);
    return complete && MoveFileExW(temporary.c_str(), filename, MOVEFILE_WRITE_THROUGH);
}

int run(const wchar_t* recording, const wchar_t* output) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, recording, L"rb") != 0 || !file) {
        fprintf(stderr, "cannot open Vulkan recording\n");
        return 1;
    }
    const std::unique_ptr<FILE, decltype(&fclose)> guard(file, fclose);
    Header header{};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) || std::memcmp(header.tag, magic, sizeof(magic))
        || VK_API_VERSION_VARIANT(header.api) || VK_API_VERSION_MAJOR(header.api) != 1) {
        fprintf(stderr, "invalid Vulkan recording header\n");
        return 1;
    }
    Context context;
    if (!context.initialize(header)) {
        fprintf(stderr, "cannot create a compatible Vulkan device or pipeline cache\n");
        return 1;
    }
    size_t total = sizeof(header), prepared = 0, failed = 0;
    std::vector<uint8_t> packet;
    while (true) {
        uint32_t size = 0;
        const size_t got = fread(&size, 1, sizeof(size), file);
        if (!got && feof(file)) break;
        if (got != sizeof(size) || !size || size > packet_limit || size + sizeof(size) > file_limit - total) {
            fprintf(stderr, "invalid Vulkan record size after %zu pipelines\n", prepared + failed);
            return 1;
        }
        total += size + sizeof(size);
        packet.resize(size);
        Recipe recipe;
        if (fread(packet.data(), 1, size, file) != size || !decode(packet, recipe)) {
            fprintf(stderr, "invalid Vulkan recipe after %zu pipelines\n", prepared + failed);
            return 1;
        }
        const auto result = replay(context, recipe);
        if (result == VK_SUCCESS) ++prepared;
        else {
            ++failed;
            fprintf(stderr, "Vulkan pipeline %zu failed (%d)\n", prepared + failed, result);
            if (result == VK_ERROR_DEVICE_LOST) break;
        }
    }
    printf("Vulkan compute pipelines prepared: %zu, %zu failed\n", prepared, failed);
    if (!prepared || failed) return 1;
    if (save_cache(context, output)) return 0;
    fprintf(stderr, "cannot save Vulkan pipeline cache\n");
    return 1;
}
}  // namespace

int wmain(int count, wchar_t** args) {
    if (count != 3) {
        fprintf(stderr, "usage: tessera_vulkan_replay <recording> <new cache file>\n");
        return 1;
    }
    try {
        return run(args[1], args[2]);
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
