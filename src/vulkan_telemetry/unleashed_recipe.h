// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "graphics_recipe.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace tessera::vulkan_recipe::unleashed {
inline constexpr char magic[8] = "TSRURV1";

struct Settings {
    uint8_t samples, bicubic, transparency, reserved;
};

struct Input {
    std::array<uint8_t, 85> state{};
    uint32_t mask{};
    std::vector<uint8_t> elements;
    std::array<std::vector<uint32_t>, 2> code;

    uint32_t field(size_t offset) const {
        uint32_t value;
        std::memcpy(&value, state.data() + offset, sizeof(value));
        return value;
    }
};

template<class T>
bool read_array(FILE* file, std::vector<T>& result, uint32_t limit) {
    uint32_t size;
    if (fread(&size, sizeof(size), 1, file) != 1 || size > limit || size % sizeof(T)) return false;
    result.resize(size / sizeof(T));
    return !size || fread(result.data(), 1, size, file) == size;
}

inline bool read(FILE* file, Input& input) {
    return fread(input.state.data(), input.state.size(), 1, file) == 1
        && fread(&input.mask, sizeof(input.mask), 1, file) == 1
        && read_array(file, input.elements, 64 * 12) && input.elements.size() % 12 == 0
        && read_array(file, input.code[0], word_limit * sizeof(uint32_t))
        && read_array(file, input.code[1], word_limit * sizeof(uint32_t))
        && !input.code[0].empty()
        && std::all_of(input.code.begin(), input.code.end(), [](const auto& code) {
            return code.empty() || (code.size() >= 5 && code[0] == spirv_magic);
        });
}

inline VkFormat vertex_format(uint32_t type, bool normal) {
    switch (type) {
        case 0x2c83a4: return VK_FORMAT_R32_SFLOAT;
        case 0x2c23a5: return VK_FORMAT_R32G32_SFLOAT;
        case 0x2a23b9: return normal ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT;
        case 0x1a23a6: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 0x182886: return VK_FORMAT_B8G8R8A8_UNORM;
        case 0x1a2286: case 0x1a2386: return VK_FORMAT_R8G8B8A8_UINT;
        case 0x2c2359: return VK_FORMAT_R16G16_SINT;
        case 0x1a235a: return VK_FORMAT_R16G16B16A16_SINT;
        case 0x1a2086: case 0x1a2186: return VK_FORMAT_R8G8B8A8_UNORM;
        case 0x2c2159: return VK_FORMAT_R16G16_SNORM;
        case 0x1a215a: return VK_FORMAT_R16G16B16A16_SNORM;
        case 0x2c2059: return VK_FORMAT_R16G16_UNORM;
        case 0x1a205a: return VK_FORMAT_R16G16B16A16_UNORM;
        case 0x2c82a1: case 0x2a2190: case 0x2a2390: return VK_FORMAT_R32_UINT;
        case 0x2c235f: return VK_FORMAT_R16G16_SFLOAT;
        case 0x1a2360: return VK_FORMAT_R16G16B16A16_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

inline bool vertices(const Input& input, std::vector<VkVertexInputBindingDescription>& bindings,
                     std::vector<VkVertexInputAttributeDescription>& attributes) {
    // Guest semantic and index for each Vulkan shader input location.
    constexpr std::pair<int, int> semantics[]{
        {0,0}, {3,0}, {6,0}, {7,0}, {5,0}, {5,1}, {5,2}, {5,3},
        {10,0}, {2,0}, {1,0}, {10,1}, {5,4}, {5,5}, {5,6}, {5,7}
    };
    bool present[16]{};
    for (size_t at = 0; at < input.elements.size(); at += 12) {
        const auto* e = input.elements.data() + at;
        const std::pair<int, int> semantic{e[9], e[10]};
        if (semantic == std::pair{0,2}) continue;
        const auto found = std::find(std::begin(semantics), std::end(semantics), semantic);
        if (found == std::end(semantics) && semantic != std::pair{0,1}) return false;
        const auto location = found == std::end(semantics) ? 15u : static_cast<uint32_t>(found - std::begin(semantics));
        const auto stream = (uint32_t(e[0]) << 8) | e[1];
        const auto offset = (uint32_t(e[2]) << 8) | e[3];
        const auto type = (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7];
        const auto format = vertex_format(type, location >= 1 && location <= 3);
        if (stream >= 16 || format == VK_FORMAT_UNDEFINED || present[location]) return false;
        present[location] = true;
        attributes.push_back({location, stream, format, offset});
    }
    constexpr uint32_t defaults[]{0,1,2,3,4,5,6,7,8,10,9};
    const auto real = attributes.size();
    for (const auto location : defaults) {
        if (present[location]) continue;
        const bool integer = (location >= 1 && location <= 3) || location == 9;
        attributes.push_back({location, 15, integer ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT, 0});
    }
    bool used[16]{}, assigned[16]{};
    for (size_t i = 0; i < real; ++i) used[attributes[i].binding] = true;
    for (const auto& attribute : attributes) {
        const auto slot = attribute.binding;
        if (assigned[slot]) continue;
        assigned[slot] = true;
        const uint32_t stride = used[slot] ? input.state[52 + slot] : 0;
        const bool instanced = input.state[0] && slot != 0 && slot != 15;
        bindings.push_back({slot, stride, instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    return true;
}

inline VkFormat target(uint32_t value) {
    switch (value) {
        case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 20: return VK_FORMAT_R8G8B8A8_UNORM;
        case 24: return VK_FORMAT_B8G8R8A8_UNORM;
        case 26: return VK_FORMAT_R16G16_SFLOAT;
        case 32: return VK_FORMAT_D32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

inline std::vector<uint8_t> layout_data() {
    Layout layout;
    constexpr auto flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
        | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    const auto add = [&](VkDescriptorType type, uint32_t count) {
        layout.sets.push_back({VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            {{{0, type, count, VK_SHADER_STAGE_ALL}, flags, {}}}});
    };
    for (int i = 0; i < 3; ++i) add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 65536);
    add(VK_DESCRIPTOR_TYPE_SAMPLER, 1024);
    layout.push.push_back({VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24});
    std::vector<uint8_t> bytes;
    put_layout(bytes, layout);
    return bytes;
}

inline bool packet(const Input& input, const Settings& settings, bool msaa, std::vector<uint8_t>& bytes) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    if (!vertices(input, bindings, attributes)) return false;
    const auto topology = input.field(48);
    const auto cull = input.field(11);
    const bool depth = input.state[1] != 0;
    const auto compare = depth ? input.field(15) : 2;
    const auto write_mask = input.field(44);
    const auto color = write_mask ? target(input.field(68)) : VK_FORMAT_UNDEFINED;
    const auto depth_format = depth ? target(input.field(72)) : VK_FORMAT_UNDEFINED;
    const bool has_depth = depth_format != VK_FORMAT_UNDEFINED;
    if (topology < 1 || topology > 6 || cull < 1 || cull > 3 || compare < 1 || compare > 8
        || write_mask > 15 || (write_mask && input.field(68) && color == VK_FORMAT_UNDEFINED)
        || (depth && input.field(72) && !has_depth)) return false;
    const bool has_color = color != VK_FORMAT_UNDEFINED;
    const auto samples = static_cast<VkSampleCountFlagBits>(msaa ? settings.samples : input.field(76));
    if (samples != VK_SAMPLE_COUNT_1_BIT && samples != VK_SAMPLE_COUNT_2_BIT
        && samples != VK_SAMPLE_COUNT_4_BIT && samples != VK_SAMPLE_COUNT_8_BIT) return false;
    constexpr uint32_t alpha_test = 1 << 1, bicubic = 1 << 2, alpha_to_coverage = 1 << 3;
    uint32_t specialization = input.field(81) | (settings.bicubic ? bicubic : 0);
    const bool coverage = (msaa && settings.transparency && (specialization & alpha_test)) || input.state[80];
    if (msaa && coverage) specialization = (specialization & ~alpha_test) | alpha_to_coverage;
    specialization &= input.mask;
    const VkSpecializationMapEntry entry{0, 0, sizeof(specialization)};
    const VkSpecializationInfo spec{1, &entry, sizeof(specialization), &specialization};
    VkPipelineShaderStageCreateInfo stages[2]{};
    VkShaderModuleCreateInfo modules[2]{};
    const uint32_t stage_count = input.code[1].empty() ? 1u : 2u;
    for (uint32_t i = 0; i < stage_count; ++i) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_NULL_HANDLE, "main", specialization ? &spec : nullptr};
        modules[i] = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
            input.code[i].size() * sizeof(uint32_t), input.code[i].data()};
    }

    VkAttachmentDescription descriptions[2]{};
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{has_color ? 1u : 0u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (has_color) descriptions[0] = {0, color, samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, color_ref.layout, color_ref.layout};
    if (has_depth) descriptions[depth_ref.attachment] = {0, depth_format, samples, VK_ATTACHMENT_LOAD_OP_LOAD,
        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, depth_ref.layout, depth_ref.layout};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = has_color;
    subpass.pColorAttachments = has_color ? &color_ref : nullptr;
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0,
        uint32_t(has_color) + uint32_t(has_depth), descriptions, 1, &subpass, 0, nullptr};
    std::vector<uint8_t> render;
    if (!render_pass_offsets(render, pass, true)) return false;
    const auto layout = layout_data();
    const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr, 0, uint32_t(bindings.size()), bindings.data(), uint32_t(attributes.size()), attributes.data()};
    const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr, 0, static_cast<VkPrimitiveTopology>(topology - 1), topology == 3 || topology == 5};
    const uint32_t viewport_count = has_color || has_depth;
    const VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr, 0, viewport_count, nullptr, viewport_count, nullptr};
    const VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, cull - 1, VK_FRONT_FACE_CLOCKWISE,
        VK_TRUE, 0, 0, 0, 1};
    const VkPipelineMultisampleStateCreateInfo multi{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr, 0, samples, VK_FALSE, 0, nullptr, coverage, VK_FALSE};
    const bool blend = has_color && input.state[19];
    constexpr VkBlendFactor factors[]{VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    const auto factor = [&](size_t offset, VkBlendFactor fallback) {
        const auto value = input.field(offset);
        return blend && value >= 1 && value <= std::size(factors) ? factors[value - 1] : fallback;
    };
    const auto op = [&](size_t offset) {
        const auto value = input.field(offset);
        return blend && value >= 1 && value <= 5 ? static_cast<VkBlendOp>(value - 1) : VK_BLEND_OP_ADD;
    };
    const VkPipelineColorBlendAttachmentState attachment{blend, factor(3, VK_BLEND_FACTOR_ONE),
        factor(7, VK_BLEND_FACTOR_ZERO), op(20), factor(32, VK_BLEND_FACTOR_ONE), factor(36, VK_BLEND_FACTOR_ZERO),
        op(40), write_mask};
    const VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr, 0, VK_FALSE, VK_LOGIC_OP_NO_OP, uint32_t(has_color), &attachment, {}};
    const VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        nullptr, 0, depth, depth && input.state[2], static_cast<VkCompareOp>(compare - 1),
        VK_FALSE, VK_FALSE, {}, {}, 0, 1};
    constexpr VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        nullptr, 0, uint32_t(std::size(states)), states};
    GraphicsWire wire{};
    wire.pipeline = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0, stage_count, stages,
        &vertex, &assembly, nullptr, &viewport, &raster, &multi, &depth_state, &color_blend, &dynamic,
        VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    wire.modules = modules;
    wire.render_data = render.data();
    wire.render_size = static_cast<uint32_t>(render.size());
    wire.layout_data = layout.data();
    wire.layout_size = static_cast<uint32_t>(layout.size());
    return graphics_offsets(bytes, wire, true);
}
} // namespace tessera::vulkan_recipe::unleashed
