// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "compute_recipe.h"
#include "tessera/packet_offsets.h"

namespace tessera::vulkan_recipe {
inline constexpr char mixed_magic[8] = "TSRVKC3";
enum : uint32_t { compute_packet, graphics_packet };
inline constexpr uint32_t max_stages = 5;
inline constexpr uint32_t max_attachments = 32;
inline constexpr uint32_t max_subpasses = 32;

using Offsets = PacketOffsets<packet_limit>;

template<class T, class Visit>
bool optional_object(Offsets& offsets, const T*& pointer, Visit visit) {
    return !pointer || offsets.mapped_array(pointer, 1, 1, visit);
}

inline bool pipeline_feedback_only(const void* chain) {
    const auto* node = static_cast<const VkBaseInStructure*>(chain);
    return !node || (node->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO && !node->pNext);
}

inline bool render_pass_offsets(std::vector<uint8_t>& packet, VkRenderPassCreateInfo& pass, bool capture) {
    if constexpr (sizeof(void*) != 8) return false;
    if (capture) packet.resize(sizeof(pass));
    else {
        if (packet.size() < sizeof(pass)) return false;
        std::memcpy(&pass, packet.data(), sizeof(pass));
    }
    if (pass.sType != VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO || pass.pNext || !pass.subpassCount) return false;
    Offsets offsets(packet, sizeof(pass), capture);
    const auto subpass = [&](VkSubpassDescription& sub) {
        return offsets.array(sub.pInputAttachments, sub.inputAttachmentCount, max_attachments)
            && offsets.array(sub.pColorAttachments, sub.colorAttachmentCount, max_attachments)
            && (!sub.pResolveAttachments || offsets.array(sub.pResolveAttachments, sub.colorAttachmentCount, max_attachments))
            && (!sub.pDepthStencilAttachment || offsets.array(sub.pDepthStencilAttachment, 1, 1))
            && offsets.array(sub.pPreserveAttachments, sub.preserveAttachmentCount, max_attachments);
    };
    if (!offsets.array(pass.pAttachments, pass.attachmentCount, max_attachments)
        || !offsets.mapped_array(pass.pSubpasses, pass.subpassCount, max_subpasses, subpass)
        || !offsets.array(pass.pDependencies, pass.dependencyCount, 256))
        return false;
    if (capture) std::memcpy(packet.data(), &pass, sizeof(pass));
    return true;
}

struct GraphicsWire {
    VkGraphicsPipelineCreateInfo pipeline;
    const VkShaderModuleCreateInfo* modules;
    const uint8_t* render_data;
    const uint8_t* layout_data;
    uint32_t render_size;
    uint32_t layout_size;
};

inline bool graphics_offsets(std::vector<uint8_t>& packet, GraphicsWire& wire, bool capture) {
    if constexpr (sizeof(void*) != 8) return false;
    if (capture) packet.resize(sizeof(wire));
    else {
        if (packet.size() < sizeof(wire)) return false;
        std::memcpy(&wire, packet.data(), sizeof(wire));
    }
    auto& p = wire.pipeline;
    if (p.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || p.flags || p.pNext
        || p.layout || p.renderPass || p.basePipelineHandle || !p.stageCount || p.stageCount > max_stages
        || !wire.render_size || !wire.layout_size)
        return false;
    Offsets offsets(packet, sizeof(wire), capture);
    const auto surface = [&](VkPipelineViewportStateCreateInfo& state) {
        return !state.pNext && state.viewportCount <= 32 && state.scissorCount <= 32
            && (!state.pViewports || offsets.array(state.pViewports, state.viewportCount, 32))
            && (!state.pScissors || offsets.array(state.pScissors, state.scissorCount, 32));
    };
    const auto specialization = [&](VkSpecializationInfo& values) {
        const auto* bytes = static_cast<const uint8_t*>(values.pData);
        if (capture) {
            for (uint32_t i = 0; i < values.mapEntryCount && i < constant_limit; ++i) {
                const auto& value = values.pMapEntries[i];
                if (value.offset > values.dataSize || value.size > values.dataSize - value.offset) return false;
            }
        }
        if (!offsets.array(values.pMapEntries, values.mapEntryCount, constant_limit)
            || !offsets.array(bytes, values.dataSize, data_limit)) return false;
        values.pData = bytes;
        if (!capture) {
            for (uint32_t i = 0; i < values.mapEntryCount; ++i) {
                const auto& value = values.pMapEntries[i];
                if (value.offset > values.dataSize || value.size > values.dataSize - value.offset) return false;
            }
        }
        return true;
    };
    const auto stage = [&](VkPipelineShaderStageCreateInfo& s) {
        if (s.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || s.pNext || s.flags || !s.pName)
            return false;
        if (capture) s.module = VK_NULL_HANDLE;
        else if (s.module) return false;
        return offsets.name(s.pName, entry_limit)
            && optional_object(offsets, s.pSpecializationInfo, specialization);
    };
    const auto module = [&](VkShaderModuleCreateInfo& m) {
        if (m.sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || m.pNext || m.flags
            || m.codeSize < 20 || m.codeSize % 4) return false;
        if (!offsets.array(m.pCode, m.codeSize / 4, word_limit)) return false;
        return capture || m.pCode[0] == spirv_magic;
    };
    const auto vertex = [&](VkPipelineVertexInputStateCreateInfo& state) {
        return !state.pNext && offsets.array(state.pVertexBindingDescriptions, state.vertexBindingDescriptionCount, 256)
            && offsets.array(state.pVertexAttributeDescriptions, state.vertexAttributeDescriptionCount, 256);
    };
    const auto samples = [&](VkPipelineMultisampleStateCreateInfo& state) {
        const auto count = static_cast<uint32_t>(state.rasterizationSamples);
        return !state.pNext && count && count <= 64 && !(count & (count - 1))
            && (!state.pSampleMask || offsets.array(state.pSampleMask, (count + 31) / 32, 2));
    };
    const auto attachments = [&](VkPipelineColorBlendStateCreateInfo& state) {
        return !state.pNext && offsets.array(state.pAttachments, state.attachmentCount, max_attachments);
    };
    const auto dynamic = [&](VkPipelineDynamicStateCreateInfo& state) {
        return !state.pNext && offsets.array(state.pDynamicStates, state.dynamicStateCount, 128);
    };
    const auto core = [](auto& value) { return !value.pNext; };
    if (!offsets.mapped_array(p.pStages, p.stageCount, max_stages, stage)
        || !offsets.mapped_array(wire.modules, p.stageCount, max_stages, module)
        || !optional_object(offsets, p.pVertexInputState, vertex)
        || !optional_object(offsets, p.pInputAssemblyState, core)
        || !optional_object(offsets, p.pTessellationState, core)
        || !optional_object(offsets, p.pViewportState, surface)
        || !optional_object(offsets, p.pRasterizationState, core)
        || !optional_object(offsets, p.pMultisampleState, samples)
        || !optional_object(offsets, p.pDepthStencilState, core)
        || !optional_object(offsets, p.pColorBlendState, attachments)
        || !optional_object(offsets, p.pDynamicState, dynamic)
        || !offsets.array(wire.render_data, wire.render_size, packet_limit)
        || !offsets.array(wire.layout_data, wire.layout_size, packet_limit)) return false;
    if (capture) std::memcpy(packet.data(), &wire, sizeof(wire));
    return true;
}
}  // namespace tessera::vulkan_recipe
