// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>

#include "device_recipe.h"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace {
using namespace tessera::vulkan_recipe;

bool same_path(const wchar_t* first, const wchar_t* second) {
    wchar_t a[32768], b[32768];
    const auto na = GetFullPathNameW(first, _countof(a), a, nullptr);
    const auto nb = GetFullPathNameW(second, _countof(b), b, nullptr);
    return !na || !nb || na >= _countof(a) || nb >= _countof(b) || !_wcsicmp(a, b);
}

std::vector<uint8_t> read_checkpoint(const wchar_t* path, const Header& header) {
    if (!path) return {};
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") || !file) return {};
    const std::unique_ptr<FILE, decltype(&fclose)> guard(file, fclose);
    if (_fseeki64(file, 0, SEEK_END)) return {};
    const auto size = _ftelli64(file);
    if (size < sizeof(VkPipelineCacheHeaderVersionOne) || size > file_limit || _fseeki64(file, 0, SEEK_SET)) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) return {};
    VkPipelineCacheHeaderVersionOne cache{};
    std::memcpy(&cache, bytes.data(), sizeof(cache));
    if (cache.headerSize != sizeof(cache) || cache.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        || cache.vendorID != header.vendor || cache.deviceID != header.device
        || std::memcmp(cache.pipelineCacheUUID, header.cache_uuid, VK_UUID_SIZE)) return {};
    return bytes;
}

struct Context {
    HMODULE loader{};
    VkInstance instance{};
    VkDevice device{};
    VkPipelineCache cache{};
    PFN_vkGetInstanceProcAddr get_instance{};
    PFN_vkGetDeviceProcAddr get_device{};
    bool descriptor_indexing{};
    bool graphics{};

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

    // Creates the device with the extensions and features the game enabled. Extensions the
    // driver does not list (layer-provided ones) and presentation extensions are left out.
    bool create_recorded(VkPhysicalDevice physical, PFN_vkCreateDevice make_device,
            const VkDeviceQueueCreateInfo& queue, DeviceRecipe& recorded) {
        const auto list = instance_fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        uint32_t count = 0;
        if (!list || list(physical, nullptr, &count, nullptr) != VK_SUCCESS || count > 4096) return false;
        std::vector<VkExtensionProperties> supported(count);
        if (list(physical, nullptr, &count, supported.data()) != VK_SUCCESS) return false;
        std::vector<const char*> names;
        size_t dropped = 0;
        for (const auto& name : recorded.extensions) {
            const bool listed = std::any_of(supported.begin(), supported.begin() + count,
                [&](const VkExtensionProperties& p) { return name == p.extensionName; });
            if (listed && replayable_extension(name.c_str())) names.push_back(name.c_str());
            else if (listed) continue;
            else ++dropped;
        }
        void* chain = nullptr;
        for (auto it = recorded.structs.rbegin(); it != recorded.structs.rend(); ++it) {
            reinterpret_cast<VkBaseOutStructure*>(it->data())->pNext = static_cast<VkBaseOutStructure*>(chain);
            chain = it->data();
        }
        if (dropped || recorded.unknown)
            fprintf(stderr, "Vulkan device state: %zu unavailable extension(s), %u unrecorded feature structure(s)\n",
                dropped, recorded.unknown);
        const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain, 0, 1, &queue, 0, nullptr,
            static_cast<uint32_t>(names.size()), names.data(), recorded.core ? &recorded.features : nullptr};
        if (make_device(physical, &info, nullptr, &device) != VK_SUCCESS) return false;
        fprintf(stderr, "replaying with the recorded Vulkan device state (%zu extension(s), %zu feature structure(s))\n",
            names.size(), recorded.structs.size());
        return true;
    }

    bool initialize(const Header& header, const wchar_t* checkpoint, DeviceRecipe* recorded) {
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
        uint32_t index = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (!queues[i].queueCount || !(queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            if (index == count || (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) index = i;
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
        }
        if (index == count) return false;
        graphics = (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, index, 1, &priority};
        VkPhysicalDeviceFeatures available{};
        features(physical, &available);
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features11.pNext = &features12;
        const auto features2 = instance_fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
        descriptor_indexing = header.api >= VK_API_VERSION_1_2 && features2;
        if (descriptor_indexing) {
            VkPhysicalDeviceFeatures2 query{};
            query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            query.pNext = &features11;
            features2(physical, &query);
        }
        const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            descriptor_indexing ? &features11 : nullptr, 0, 1, &queue, 0,
            nullptr, 0, nullptr, &available};
        // Features such as robustness change driver codegen and cache keys: replay the game's own set.
        if (recorded && create_recorded(physical, make_device, queue, *recorded)) descriptor_indexing = true;
        else if (recorded) fprintf(stderr, "the recorded Vulkan device state was rejected; replaying with all features\n");
        const auto seed = read_checkpoint(checkpoint, header);
        const VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0,
            seed.size(), seed.empty() ? nullptr : seed.data()};
        return (device || make_device(physical, &info, nullptr, &device) == VK_SUCCESS)
            && fn<PFN_vkCreatePipelineCache>("vkCreatePipelineCache")(device, &cache_info, nullptr, &cache) == VK_SUCCESS;
    }
};

struct PipelineObjects {
    const Context& context;
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<std::pair<Sampler, VkSampler>> samplers;
    VkPipelineLayout layout{};
    std::vector<VkShaderModule> shaders;
    VkPipeline pipeline{};
    VkRenderPass render_pass{};

    ~PipelineObjects() {
        const auto device = context.device;
        if (pipeline) context.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, pipeline, nullptr);
        const auto destroy_shader = context.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
        for (const auto shader : shaders) if (shader) destroy_shader(device, shader, nullptr);
        if (render_pass) context.fn<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(device, render_pass, nullptr);
        if (layout) context.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, layout, nullptr);
        const auto destroy = context.fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
        for (const auto set : sets) if (set) destroy(device, set, nullptr);
        const auto destroy_sampler = context.fn<PFN_vkDestroySampler>("vkDestroySampler");
        for (const auto& entry : samplers) destroy_sampler(device, entry.second, nullptr);
    }

    VkResult sampler(const Sampler& desc, VkSampler& handle) {
        const auto found = std::find_if(samplers.begin(), samplers.end(), [&](const auto& item) {
            return std::memcmp(&item.first, &desc, sizeof(desc)) == 0;
        });
        if (found != samplers.end()) { handle = found->second; return VK_SUCCESS; }
        const auto info = desc.info();
        const auto status = context.fn<PFN_vkCreateSampler>("vkCreateSampler")(context.device, &info, nullptr, &handle);
        if (status == VK_SUCCESS) {
            try { samplers.emplace_back(desc, handle); }
            catch (...) {
                context.fn<PFN_vkDestroySampler>("vkDestroySampler")(context.device, handle, nullptr);
                throw;
            }
        }
        return status;
    }
};

VkResult make_layout(const Context& context, PipelineObjects& objects, const Layout& description) {
    objects.sets.resize(description.sets.size());
    const auto make_set = context.fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (size_t i = 0; i < objects.sets.size(); ++i) {
        const auto& set = description.sets[i];
        bindings.clear();
        std::vector<VkDescriptorBindingFlags> flags;
        std::vector<std::vector<VkSampler>> immutable(set.bindings.size());
        for (size_t index = 0; index < set.bindings.size(); ++index) {
            const auto& binding = set.bindings[index];
            const auto& b = binding.state;
            auto& handles = immutable[index];
            handles.resize(binding.immutable.size());
            for (size_t j = 0; j < handles.size(); ++j) {
                const auto status = objects.sampler(binding.immutable[j], handles[j]);
                if (status != VK_SUCCESS) return status;
            }
            bindings.push_back({b.number, b.type, b.count, b.stages, handles.empty() ? nullptr : handles.data()});
            flags.push_back(binding.flags);
        }
        const bool has_flags = std::any_of(flags.begin(), flags.end(), [](auto flag) { return flag != 0; });
        if (has_flags && !context.descriptor_indexing) return VK_ERROR_FEATURE_NOT_PRESENT;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo indexing{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            nullptr, static_cast<uint32_t>(flags.size()), flags.data()};
        const VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            has_flags ? &indexing : nullptr, set.flags, static_cast<uint32_t>(bindings.size()), bindings.data()};
        const auto status = make_set(context.device, &info, nullptr, &objects.sets[i]);
        if (status != VK_SUCCESS) return status;
    }
    const VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0,
        static_cast<uint32_t>(objects.sets.size()), objects.sets.data(),
        static_cast<uint32_t>(description.push.size()), description.push.data()};
    return context.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layout, nullptr, &objects.layout);
}

VkResult replay(const Context& context, const Recipe& recipe) {
    PipelineObjects objects{context, {}, {}, {}, {}};
    auto status = make_layout(context, objects, recipe.layout);
    if (status != VK_SUCCESS) return status;
    const VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        recipe.code.size() * sizeof(uint32_t), recipe.code.data()};
    objects.shaders.resize(1);
    status = context.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &module, nullptr, &objects.shaders[0]);
    if (status != VK_SUCCESS) return status;
    std::vector<VkSpecializationMapEntry> constants;
    constants.reserve(recipe.constants.size());
    for (const auto& entry : recipe.constants) constants.push_back({entry.id, entry.offset, entry.size});
    const VkSpecializationInfo spec{static_cast<uint32_t>(constants.size()), constants.data(),
        recipe.data.size(), recipe.data.data()};
    const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr,
        0, VK_SHADER_STAGE_COMPUTE_BIT, objects.shaders[0], recipe.entry.c_str(), constants.empty() ? nullptr : &spec};
    const VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        stage, objects.layout, VK_NULL_HANDLE, -1};
    return context.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(
        context.device, context.cache, 1, &info, nullptr, &objects.pipeline);
}

VkResult replay_graphics(const Context& context, std::vector<uint8_t>& bytes) {
    if (!context.graphics) return VK_ERROR_FEATURE_NOT_PRESENT;
    GraphicsWire wire{};
    if (!graphics_offsets(bytes, wire, false)) return VK_ERROR_INITIALIZATION_FAILED;
    Reader reader({wire.layout_data, wire.layout_size});
    Layout layout;
    if (!read_layout(reader, layout, false) || !reader.finished()) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<uint8_t> pass_data(wire.render_data, wire.render_data + wire.render_size);
    VkRenderPassCreateInfo pass{};
    if (!render_pass_offsets(pass_data, pass, false) || wire.pipeline.subpass >= pass.subpassCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    PipelineObjects objects{context, {}, {}, {}, {}};
    auto status = make_layout(context, objects, layout);
    if (status != VK_SUCCESS) return status;
    status = context.fn<PFN_vkCreateRenderPass>("vkCreateRenderPass")(context.device, &pass, nullptr, &objects.render_pass);
    if (status != VK_SUCCESS) return status;
    const auto make_shader = context.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    objects.shaders.resize(wire.pipeline.stageCount);
    std::vector<VkPipelineShaderStageCreateInfo> stages(wire.pipeline.pStages, wire.pipeline.pStages + wire.pipeline.stageCount);
    for (uint32_t i = 0; i < wire.pipeline.stageCount; ++i) {
        status = make_shader(context.device, &wire.modules[i], nullptr, &objects.shaders[i]);
        if (status != VK_SUCCESS) return status;
        stages[i].module = objects.shaders[i];
    }
    wire.pipeline.pStages = stages.data();
    wire.pipeline.layout = objects.layout;
    wire.pipeline.renderPass = objects.render_pass;
    return context.fn<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(
        context.device, context.cache, 1, &wire.pipeline, nullptr, &objects.pipeline);
}

bool save_cache(const Context& context, const wchar_t* filename, bool replace = false) {
    const auto read = context.fn<PFN_vkGetPipelineCacheData>("vkGetPipelineCacheData");
    size_t size = 0;
    if (read(context.device, context.cache, &size, nullptr) != VK_SUCCESS
        || size < sizeof(VkPipelineCacheHeaderVersionOne) || size > file_limit) return false;
    std::vector<char> cache(size);
    if (read(context.device, context.cache, &size, cache.data()) != VK_SUCCESS) return false;
    const std::wstring temporary = std::wstring(filename) + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        replace ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, cache.data(), static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(file);
    return complete && MoveFileExW(temporary.c_str(), filename,
        MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
}

int run(const wchar_t* recording, const wchar_t* output, const wchar_t* checkpoint = nullptr) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, recording, L"rb") != 0 || !file) {
        fprintf(stderr, "cannot open Vulkan recording\n");
        return 1;
    }
    const std::unique_ptr<FILE, decltype(&fclose)> guard(file, fclose);
    Header header{};
    const auto header_size = fread(&header, 1, sizeof(header), file);
    const bool legacy = std::memcmp(header.tag, legacy_magic, sizeof(legacy_magic)) == 0;
    const bool mixed = std::memcmp(header.tag, mixed_magic, sizeof(mixed_magic)) == 0;
    if (header_size != sizeof(header) || (!legacy && !mixed && std::memcmp(header.tag, magic, sizeof(magic)))
        || VK_API_VERSION_VARIANT(header.api) || VK_API_VERSION_MAJOR(header.api) != 1) {
        fprintf(stderr, "invalid Vulkan recording header\n");
        return 1;
    }
    Context context;
    // Recordings since the device packet start with the game's device state.
    DeviceRecipe recorded;
    bool has_device = false;
    if (mixed) {
        uint32_t size = 0, kind = 0;
        std::vector<uint8_t> bytes;
        if (fread(&size, 1, sizeof(size), file) == sizeof(size) && size > sizeof(kind) && size <= packet_limit
            && fread(&kind, 1, sizeof(kind), file) == sizeof(kind) && kind == device_packet) {
            bytes.resize(size - sizeof(kind));
            has_device = fread(bytes.data(), 1, bytes.size(), file) == bytes.size() && decode_device(bytes, recorded);
            if (!has_device) fprintf(stderr, "invalid Vulkan device state; replaying with all features\n");
        }
        if (_fseeki64(file, sizeof(header), SEEK_SET)) return 1;
    }
    if (!context.initialize(header, checkpoint, has_device ? &recorded : nullptr)) {
        fprintf(stderr, "cannot create a compatible Vulkan device or pipeline cache\n");
        return 1;
    }
    size_t total = sizeof(header), prepared[2]{}, failed = 0;
    bool lost = false;
    const auto attempted = [&] { return prepared[0] + prepared[1] + failed; };
    std::vector<uint8_t> packet;
    while (true) {
        uint32_t size = 0;
        const size_t got = fread(&size, 1, sizeof(size), file);
        if (!got && feof(file)) break;
        if (got != sizeof(size) || !size || size > packet_limit || size + sizeof(size) > file_limit - total) {
            fprintf(stderr, "invalid Vulkan record size after %zu pipelines\n", attempted());
            return 1;
        }
        total += size + sizeof(size);
        packet.resize(size);
        if (fread(packet.data(), 1, size, file) != size || (mixed && size <= sizeof(uint32_t))) {
            fprintf(stderr, "invalid Vulkan recipe after %zu pipelines\n", attempted());
            return 1;
        }
        uint32_t kind = compute_packet;
        if (mixed) {
            std::memcpy(&kind, packet.data(), sizeof(kind));
            packet.erase(packet.begin(), packet.begin() + sizeof(kind));
        }
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (kind == compute_packet) {
            Recipe recipe;
            if (!decode(packet, recipe, legacy)) {
                fprintf(stderr, "invalid Vulkan recipe after %zu pipelines\n", attempted());
                return 1;
            }
            result = replay(context, recipe);
        } else if (kind == graphics_packet) {
            result = replay_graphics(context, packet);
        }
        if (result == VK_SUCCESS) {
            ++prepared[kind];
            if (checkpoint && (attempted() == 1 || attempted() % 32 == 0)
                && !save_cache(context, checkpoint, true)) {
                fprintf(stderr, "cannot save Vulkan replay checkpoint\n");
                return 1;
            }
        }
        else {
            ++failed;
            fprintf(stderr, "Vulkan pipeline %zu failed (%d)\n", attempted(), result);
            if (result == VK_ERROR_DEVICE_LOST) {
                lost = true;
                break;
            }
        }
    }
    if (prepared[1]) printf("Vulkan pipelines prepared: %zu compute, %zu graphics, %zu failed\n", prepared[0], prepared[1], failed);
    else printf("Vulkan compute pipelines prepared: %zu, %zu failed\n", prepared[0], failed);
    // A pipeline the driver rejects fails again on every run; keep what was built.
    // A lost device leaves the cache unreadable: the last checkpoint resumes next run.
    if (lost || !(prepared[0] + prepared[1])) return 1;
    if (checkpoint && !save_cache(context, checkpoint, true)) {
        fprintf(stderr, "cannot save Vulkan replay checkpoint\n");
        return 1;
    }
    if (save_cache(context, output)) return 0;
    fprintf(stderr, "cannot save Vulkan pipeline cache\n");
    return 1;
}
}  // namespace

int wmain(int count, wchar_t** args) {
    if (count != 3 && (count != 5 || wcscmp(args[3], L"--checkpoint"))) {
        fprintf(stderr, "usage: tessera_vulkan_replay <recording> <new cache file> [--checkpoint <cache file>]\n");
        return 1;
    }
    try {
        if (count == 5) {
            const auto temporary = std::wstring(args[4]) + L".tmp";
            if (same_path(args[4], args[1]) || same_path(args[4], args[2])
                || same_path(temporary.c_str(), args[1]) || same_path(temporary.c_str(), args[2])) {
                fprintf(stderr, "checkpoint paths must differ from the recording and output\n");
                return 1;
            }
        }
        return run(args[1], args[2], count == 5 ? args[4] : nullptr);
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
