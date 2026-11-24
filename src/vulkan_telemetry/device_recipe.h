// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The game's device extensions and enabled features, recorded once per capture so
// offline replay compiles pipelines for the same device state (robustness,
// descriptor indexing and similar features change driver codegen and cache keys).

#include "graphics_recipe.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace tessera::vulkan_recipe {
inline constexpr uint32_t device_packet = 2;
inline constexpr uint32_t extension_limit = 512;
inline constexpr uint32_t feature_struct_limit = 128;

struct FeatureType {
    VkStructureType type;
    uint32_t size;
};

// Feature structures replayed when the game chains them. Each is a header
// followed by VkBool32 fields only, so the payload is portable between x86 and x64.
template<class T> constexpr FeatureType feature(VkStructureType type) { return {type, sizeof(T)}; }
inline constexpr FeatureType feature_types[] = {
    feature<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2),
    feature<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES),
    feature<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES),
    feature<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES),
    feature<VkPhysicalDeviceVulkan14Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES),
    feature<VkPhysicalDevice16BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES),
    feature<VkPhysicalDevice8BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES),
    feature<VkPhysicalDeviceMultiviewFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES),
    feature<VkPhysicalDeviceVariablePointersFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES),
    feature<VkPhysicalDeviceShaderDrawParametersFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES),
    feature<VkPhysicalDeviceSamplerYcbcrConversionFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES),
    feature<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES),
    feature<VkPhysicalDeviceDescriptorIndexingFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES),
    feature<VkPhysicalDeviceScalarBlockLayoutFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES),
    feature<VkPhysicalDeviceTimelineSemaphoreFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES),
    feature<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES),
    feature<VkPhysicalDeviceVulkanMemoryModelFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES),
    feature<VkPhysicalDeviceShaderAtomicInt64Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES),
    feature<VkPhysicalDeviceUniformBufferStandardLayoutFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES),
    feature<VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES),
    feature<VkPhysicalDeviceImageRobustnessFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES),
    feature<VkPhysicalDeviceDynamicRenderingFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES),
    feature<VkPhysicalDeviceSynchronization2Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES),
    feature<VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES),
    feature<VkPhysicalDeviceSubgroupSizeControlFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES),
    feature<VkPhysicalDeviceShaderTerminateInvocationFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES),
    feature<VkPhysicalDeviceShaderIntegerDotProductFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES),
    feature<VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES),
    feature<VkPhysicalDeviceMaintenance4Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES),
    feature<VkPhysicalDevicePipelineRobustnessFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES),
    feature<VkPhysicalDeviceRobustness2FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT),
    feature<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT),
    feature<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT),
    feature<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT),
    feature<VkPhysicalDeviceDepthClipEnableFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT),
    feature<VkPhysicalDeviceCustomBorderColorFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT),
    feature<VkPhysicalDeviceTransformFeedbackFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT),
    feature<VkPhysicalDeviceConditionalRenderingFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT),
    feature<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT),
    feature<VkPhysicalDeviceShaderAtomicFloatFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT),
    feature<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT),
    feature<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT),
    feature<VkPhysicalDeviceMeshShaderFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT),
    feature<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR),
    feature<VkPhysicalDeviceIndexTypeUint8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES),
    feature<VkPhysicalDeviceVertexAttributeDivisorFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES),
    feature<VkPhysicalDeviceLineRasterizationFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES),
};
inline constexpr uint32_t feature_header = sizeof(VkBaseOutStructure);

inline const FeatureType* find_feature(VkStructureType type) {
    for (const auto& known : feature_types) if (known.type == type) return &known;
    return nullptr;
}

// Presentation extensions need surface instance extensions and never change codegen.
inline bool replayable_extension(const char* name) {
    for (const char* skip : {"swapchain", "present", "display", "full_screen", "surface", "low_latency"})
        if (std::strstr(name, skip)) return false;
    return true;
}

struct DeviceRecipe {
    std::vector<std::string> extensions;
    bool core = false;
    VkPhysicalDeviceFeatures features{};
    // Each entry holds a full structure (sType set, pNext relinked by the replay).
    std::vector<std::vector<uint8_t>> structs;
    uint32_t unknown = 0;
};

inline void put(std::vector<uint8_t>& out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

// Layout: u32 extension count, (u32 length, chars)*; u32 core flag + VkPhysicalDeviceFeatures;
// u32 unknown structure count; u32 struct count, (u32 sType, u32 payload size, payload)*.
inline std::vector<uint8_t> encode_device(const VkDeviceCreateInfo& info) {
    std::vector<uint8_t> out;
    uint32_t count = 0;
    for (uint32_t i = 0; i < info.enabledExtensionCount && i < extension_limit; ++i)
        if (info.ppEnabledExtensionNames[i]) ++count;
    put(out, &count, sizeof(count));
    for (uint32_t i = 0; i < info.enabledExtensionCount && i < extension_limit; ++i) {
        const char* name = info.ppEnabledExtensionNames[i];
        if (!name) continue;
        const auto length = static_cast<uint32_t>(strnlen(name, VK_MAX_EXTENSION_NAME_SIZE));
        put(out, &length, sizeof(length));
        put(out, name, length);
    }
    const uint32_t core = info.pEnabledFeatures != nullptr;
    put(out, &core, sizeof(core));
    const VkPhysicalDeviceFeatures none{};
    put(out, core ? info.pEnabledFeatures : &none, sizeof(none));
    std::vector<const VkBaseInStructure*> known;
    uint32_t unknown = 0;
    for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next && known.size() + unknown < 4096;
            next = next->pNext) {
        if (find_feature(next->sType) && known.size() < feature_struct_limit) known.push_back(next);
        else if (next->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
            && next->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) ++unknown;
    }
    put(out, &unknown, sizeof(unknown));
    const auto structs = static_cast<uint32_t>(known.size());
    put(out, &structs, sizeof(structs));
    for (const auto* next : known) {
        const auto type = static_cast<uint32_t>(next->sType);
        const uint32_t payload = find_feature(next->sType)->size - feature_header;
        put(out, &type, sizeof(type));
        put(out, &payload, sizeof(payload));
        put(out, reinterpret_cast<const uint8_t*>(next) + feature_header, payload);
    }
    return out;
}

class DeviceReader {
public:
    explicit DeviceReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    bool read(void* out, size_t size) {
        if (size > bytes_.size() - at_) return false;
        std::memcpy(out, bytes_.data() + at_, size);
        at_ += size;
        return true;
    }
    bool skip(size_t size) {
        if (size > bytes_.size() - at_) return false;
        at_ += size;
        return true;
    }
    bool done() const { return at_ == bytes_.size(); }
private:
    const std::vector<uint8_t>& bytes_;
    size_t at_ = 0;
};

inline bool decode_device(const std::vector<uint8_t>& bytes, DeviceRecipe& recipe) {
    DeviceReader in(bytes);
    uint32_t count = 0;
    if (!in.read(&count, sizeof(count)) || count > extension_limit) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length = 0;
        if (!in.read(&length, sizeof(length)) || !length || length >= VK_MAX_EXTENSION_NAME_SIZE) return false;
        std::string name(length, '\0');
        if (!in.read(name.data(), length) || name.find('\0') != std::string::npos) return false;
        recipe.extensions.push_back(std::move(name));
    }
    uint32_t core = 0;
    if (!in.read(&core, sizeof(core)) || core > 1 || !in.read(&recipe.features, sizeof(recipe.features))) return false;
    recipe.core = core != 0;
    uint32_t structs = 0;
    if (!in.read(&recipe.unknown, sizeof(recipe.unknown)) || !in.read(&structs, sizeof(structs))
        || structs > feature_struct_limit) return false;
    for (uint32_t i = 0; i < structs; ++i) {
        uint32_t type = 0, payload = 0;
        if (!in.read(&type, sizeof(type)) || !in.read(&payload, sizeof(payload))) return false;
        const auto* known = find_feature(static_cast<VkStructureType>(type));
        if (!known) {
            // Written by a newer build: skip it and report it like an unrecorded structure.
            if (!in.skip(payload)) return false;
            ++recipe.unknown;
            continue;
        }
        std::vector<uint8_t> full(known->size);
        reinterpret_cast<VkBaseOutStructure*>(full.data())->sType = known->type;
        const uint32_t used = payload < known->size - feature_header ? payload : known->size - feature_header;
        if (!in.read(full.data() + feature_header, used) || !in.skip(payload - used)) return false;
        recipe.structs.push_back(std::move(full));
    }
    return in.done();
}
}  // namespace tessera::vulkan_recipe
