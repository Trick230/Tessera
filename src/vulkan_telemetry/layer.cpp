// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <vulkan/vk_layer.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

template<typename Handle>
void* dispatch_key(Handle handle) {
    return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
}

struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr get{};
    PFN_GetPhysicalDeviceProcAddr get_physical{};
    uint32_t api{};
};

struct Device {
    PFN_vkGetDeviceProcAddr get{};
    PFN_vkCreateGraphicsPipelines graphics{};
    PFN_vkCreateComputePipelines compute{};
    PFN_vkQueuePresentKHR present{};
    bool feedback{};
    VkDevice handle{};
    VkPipelineCache cache{};
    bool checkpoint_pending{};
};

std::mutex dispatch_lock;
std::unordered_map<void*, Instance> instances;
std::unordered_map<void*, Device> devices;

template<typename Map, typename Handle>
auto dispatch(const Map& map, Handle handle) {
    const std::lock_guard lock(dispatch_lock);
    const auto it = map.find(dispatch_key(handle));
    return it == map.end() ? typename Map::mapped_type{} : it->second;
}

struct Log {
    FILE* file{};
    Clock::time_point start = Clock::now();
    std::mutex lock;

    Log() {
        wchar_t path[32768];
        const DWORD length = GetEnvironmentVariableW(L"TESSERA_VULKAN_TELEMETRY", path, _countof(path));
        if (!length || length >= _countof(path)) return;
        const HANDLE handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_WRONLY);
        if (fd < 0) { CloseHandle(handle); return; }
        file = _fdopen(fd, "wb");
        if (!file) { _close(fd); return; }
        fputs("event,time_us,duration_us,pipelines,valid_feedback,application_cache_hits,result,cache\n", file);
    }

    ~Log() { if (file) fclose(file); }

    void record(const char* event, Clock::time_point begin, Clock::time_point end, uint32_t count,
                uint32_t valid, uint32_t hits, VkResult result, const char* cache = "none") {
        if (!file) return;
        using Us = std::chrono::microseconds;
        const auto timestamp = std::chrono::duration_cast<Us>(begin - start).count();
        const auto duration = std::chrono::duration_cast<Us>(end - begin).count();
        const std::lock_guard guard(lock);
        fprintf(file, "%s,%lld,%lld,%u,%u,%u,%d,%s\n", event, static_cast<long long>(timestamp),
                static_cast<long long>(duration), count, valid, hits, result, cache);
    }
};

Log& telemetry() {
    static Log log;
    return log;
}

template<typename LayerInfo, typename CreateInfo>
LayerInfo* link_info(const CreateInfo* info, VkStructureType type) {
    auto* p = static_cast<const VkBaseInStructure*>(info->pNext);
    while (p) {
        if (p->sType == type) {
            const auto* layer = reinterpret_cast<const LayerInfo*>(p);
            if (layer->function == VK_LAYER_LINK_INFO) return const_cast<LayerInfo*>(layer);
        }
        p = p->pNext;
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkInstance* result) {
    auto* layer = link_info<VkLayerInstanceCreateInfo>(info, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!layer || !layer->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto get = layer->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto physical = layer->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto next = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    layer->u.pLayerInfo = layer->u.pLayerInfo->pNext;
    const VkResult status = next(info, allocator, result);
    if (status != VK_SUCCESS) return status;
    try {
        const std::lock_guard lock(dispatch_lock);
        instances.emplace(dispatch_key(*result), Instance{*result, get, physical,
            info->pApplicationInfo ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0});
    } catch (...) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(*result, "vkDestroyInstance"));
        if (destroy) destroy(*result, allocator);
        *result = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    telemetry();
    return status;
}

VKAPI_ATTR void VKAPI_CALL destroy_instance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    const auto parent = dispatch(instances, instance);
    if (!parent.get) return;
    {
        const std::lock_guard lock(dispatch_lock);
        instances.erase(dispatch_key(instance));
    }
    const auto next = reinterpret_cast<PFN_vkDestroyInstance>(parent.get(instance, "vkDestroyInstance"));
    if (next) next(instance, allocator);
}

bool feedback_enabled(const Instance& parent, VkPhysicalDevice physical, const VkDeviceCreateInfo* info) {
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        if (strcmp(info->ppEnabledExtensionNames[i], VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0) return true;
    }
    if (parent.api < VK_API_VERSION_1_3) return false;
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        parent.get(parent.handle, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties device{};
    if (properties) properties(physical, &device);
    return device.apiVersion >= VK_API_VERSION_1_3;
}

bool supports_feedback(const Instance& parent, VkPhysicalDevice physical) {
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        parent.get(parent.handle, "vkEnumerateDeviceExtensionProperties"));
    uint32_t count = 0;
    if (!enumerate || enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS || !count || count > 4096)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(physical, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(extensions[i].extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0) return true;
    }
    return false;
}

constexpr size_t max_cache_size = 128 * 1024 * 1024;

std::wstring environment_path(const wchar_t* variable) {
    wchar_t path[32768];
    const DWORD length = GetEnvironmentVariableW(variable, path, _countof(path));
    return length && length < _countof(path) ? std::wstring(path, length) : std::wstring{};
}

std::vector<char> read_cache(const std::wstring& path, const VkPhysicalDeviceProperties& properties) {
    if (path.empty()) return {};
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return {};
    const auto close_file = [](FILE* handle) { fclose(handle); };
    const std::unique_ptr<FILE, decltype(close_file)> guard(file, close_file);
    if (_fseeki64(file, 0, SEEK_END) != 0) return {};
    const auto size = _ftelli64(file);
    if (size < static_cast<int64_t>(sizeof(VkPipelineCacheHeaderVersionOne))
        || size > static_cast<int64_t>(max_cache_size) || _fseeki64(file, 0, SEEK_SET) != 0) return {};
    std::vector<char> bytes(static_cast<size_t>(size));
    if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) return {};
    VkPipelineCacheHeaderVersionOne header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.headerSize != sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        || header.vendorID != properties.vendorID || header.deviceID != properties.deviceID
        || std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0) return {};
    return bytes;
}

void create_cache(const Instance& parent, VkPhysicalDevice physical, VkDevice device, Device& d) {
    const auto input = environment_path(L"TESSERA_VULKAN_CACHE_INPUT");
    d.checkpoint_pending = !environment_path(L"TESSERA_VULKAN_CACHE_OUTPUT").empty();
    if (input.empty() && !d.checkpoint_pending) return;
    const auto create = reinterpret_cast<PFN_vkCreatePipelineCache>(d.get(device, "vkCreatePipelineCache"));
    const auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        parent.get(parent.handle, "vkGetPhysicalDeviceProperties"));
    if (!create || !get) return;
    VkPhysicalDeviceProperties properties{};
    get(physical, &properties);
    const auto bytes = read_cache(input, properties);
    const VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0,
        bytes.size(), bytes.data()};
    if (create(device, &info, nullptr, &d.cache) != VK_SUCCESS) d.cache = VK_NULL_HANDLE;
}

void save_cache(VkDevice device, const Device& d) {
    const auto output = environment_path(L"TESSERA_VULKAN_CACHE_OUTPUT");
    if (output.empty() || GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const auto get = reinterpret_cast<PFN_vkGetPipelineCacheData>(d.get(device, "vkGetPipelineCacheData"));
    size_t size = 0;
    if (!get || get(device, d.cache, &size, nullptr) != VK_SUCCESS
        || size < sizeof(VkPipelineCacheHeaderVersionOne) || size > max_cache_size) return;
    std::vector<char> bytes(size);
    if (get(device, d.cache, &size, bytes.data()) != VK_SUCCESS) return;
    const auto temporary = output + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(file);
    if (complete) MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH);
}

void destroy_cache(VkDevice device, const Device& d) {
    const auto destroy = reinterpret_cast<PFN_vkDestroyPipelineCache>(d.get(device, "vkDestroyPipelineCache"));
    if (d.cache && destroy) destroy(device, d.cache, nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkDevice* result) {
    const auto parent = dispatch(instances, physical);
    auto* layer = link_info<VkLayerDeviceCreateInfo>(info, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!parent.get || !layer || !layer->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto get_instance = layer->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto get_device = layer->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto next = reinterpret_cast<PFN_vkCreateDevice>(get_instance(parent.handle, "vkCreateDevice"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    bool feedback = feedback_enabled(parent, physical, info);
    VkDeviceCreateInfo annotated = *info;
    std::vector<const char*> extensions;
    if (!feedback && telemetry().file) {
        try {
            if (supports_feedback(parent, physical)) {
                for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                    extensions.push_back(info->ppEnabledExtensionNames[i]);
                extensions.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
                annotated.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
                annotated.ppEnabledExtensionNames = extensions.data();
                feedback = true;
            }
        } catch (...) {
            // Leave the device's enabled extensions as requested.
        }
    }
    layer->u.pLayerInfo = layer->u.pLayerInfo->pNext;
    const VkResult status = next(physical, &annotated, allocator, result);
    if (status != VK_SUCCESS) return status;
    Device device{get_device,
        reinterpret_cast<PFN_vkCreateGraphicsPipelines>(get_device(*result, "vkCreateGraphicsPipelines")),
        reinterpret_cast<PFN_vkCreateComputePipelines>(get_device(*result, "vkCreateComputePipelines")),
        reinterpret_cast<PFN_vkQueuePresentKHR>(get_device(*result, "vkQueuePresentKHR")),
        feedback, *result};
    try { create_cache(parent, physical, *result, device); } catch (...) {}
    try {
        const std::lock_guard lock(dispatch_lock);
        devices.emplace(dispatch_key(*result), device);
    } catch (...) {
        destroy_cache(*result, device);
        const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(get_device(*result, "vkDestroyDevice"));
        if (destroy) destroy(*result, allocator);
        *result = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return status;
}

VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice device, const VkAllocationCallbacks* allocator) {
    const auto d = dispatch(devices, device);
    if (!d.get) return;
    if (d.cache) {
        try { save_cache(device, d); } catch (...) {}
        destroy_cache(device, d);
    }
    {
        const std::lock_guard lock(dispatch_lock);
        devices.erase(dispatch_key(device));
    }
    const auto next = reinterpret_cast<PFN_vkDestroyDevice>(d.get(device, "vkDestroyDevice"));
    if (next) next(device, allocator);
}

const VkPipelineCreationFeedback* existing_feedback(const void* chain) {
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO)
            return reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo*>(p)->pPipelineCreationFeedback;
    }
    return nullptr;
}

template<typename Info, typename Create>
VkResult measure(const char* event, const Device& d, Create next, VkDevice device,
        VkPipelineCache cache, uint32_t count, const Info* info, const VkAllocationCallbacks* allocator,
        VkPipeline* results) {
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const char* cache_source = cache ? "game" : d.cache ? "layer" : "none";
    if (!cache) cache = d.cache;
    if (!telemetry().file) return next(device, cache, count, info, allocator, results);
    std::vector<Info> annotated;
    std::vector<VkPipelineCreationFeedback> feedback;
    std::vector<VkPipelineCreationFeedbackCreateInfo> extra;
    if (d.feedback && count && count <= 1024) {
        try {
            annotated.assign(info, info + count);
            feedback.resize(count);
            extra.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                if (existing_feedback(info[i].pNext)) continue;
                extra[i] = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                    info[i].pNext, &feedback[i], 0, nullptr};
                annotated[i].pNext = &extra[i];
            }
            info = annotated.data();
        } catch (...) {
            // Forward the original inputs if instrumentation cannot allocate.
        }
    }
    const auto start = Clock::now();
    const VkResult status = next(device, cache, count, info, allocator, results);
    const auto end = Clock::now();
    uint32_t created = 0, valid = 0, hits = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (status != VK_SUCCESS && !results[i]) continue;
        ++created;
        const auto* result = existing_feedback(info[i].pNext);
        if (result && (result->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
            ++valid;
            if (result->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ++hits;
        }
    }
    telemetry().record(event, start, end, created, valid, hits, status, cache_source);
    return status;
}

VKAPI_ATTR VkResult VKAPI_CALL graphics_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* allocator, VkPipeline* results) {
    const auto d = dispatch(devices, device);
    return measure("graphics", d, d.graphics, device, cache, count, info, allocator, results);
}

VKAPI_ATTR VkResult VKAPI_CALL compute_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkComputePipelineCreateInfo* info, const VkAllocationCallbacks* allocator, VkPipeline* results) {
    const auto d = dispatch(devices, device);
    return measure("compute", d, d.compute, device, cache, count, info, allocator, results);
}

VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR* info) {
    const auto d = dispatch(devices, queue);
    if (!d.present) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto start = Clock::now();
    const VkResult status = d.present(queue, info);
    telemetry().record("present", start, Clock::now(), 0, 0, 0, status);
    if ((status == VK_SUCCESS || status == VK_SUBOPTIMAL_KHR) && d.cache && d.checkpoint_pending) {
        bool save = false;
        {
            const std::lock_guard lock(dispatch_lock);
            const auto it = devices.find(dispatch_key(queue));
            if (it != devices.end() && it->second.checkpoint_pending) {
                it->second.checkpoint_pending = false;
                save = true;
            }
        }
        if (save) {
            try { save_cache(d.handle, d); } catch (...) {}
        }
    }
    return status;
}

PFN_vkVoidFunction device_intercept(const char* name) {
    if (strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(destroy_device);
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(graphics_pipelines);
    if (strcmp(name, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(compute_pipelines);
    if (strcmp(name, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(present);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device_proc(VkDevice device, const char* name) {
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(get_device_proc);
    const auto d = dispatch(devices, device);
    const auto next = d.get ? d.get(device, name) : nullptr;
    if (next) {
        if (const auto own = device_intercept(name)) return own;
    }
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_instance_proc(VkInstance instance, const char* name) {
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(get_instance_proc);
    if (strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(create_instance);
    if (!instance) return nullptr;
    if (strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(destroy_instance);
    if (strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(create_device);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(get_device_proc);
    if (const auto own = device_intercept(name)) return own;
    const auto parent = dispatch(instances, instance);
    return parent.get ? parent.get(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_physical_proc(VkInstance instance, const char* name) {
    const auto parent = dispatch(instances, instance);
    return parent.get_physical ? parent.get_physical(instance, name) : nullptr;
}
}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = get_instance_proc;
    info->pfnGetDeviceProcAddr = get_device_proc;
    info->pfnGetPhysicalDeviceProcAddr = get_physical_proc;
    return VK_SUCCESS;
}
