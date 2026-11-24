// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "device_recipe.h"

#include <windows.h>

#include <mutex>
#include <unordered_map>

namespace tessera::vulkan_recipe {
class Capture {
public:
    Capture(const std::wstring& path, uint32_t api, const VkPhysicalDeviceProperties& properties, bool graphics)
        : graphics_(graphics) {
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        Header header{};
        std::memcpy(header.tag, graphics ? mixed_magic : magic, sizeof(magic));
        header.api = api ? api : VK_API_VERSION_1_0;
        header.vendor = properties.vendorID;
        header.device = properties.deviceID;
        std::memcpy(header.cache_uuid, properties.pipelineCacheUUID, VK_UUID_SIZE);
        if (!write(&header, sizeof(header))) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

    ~Capture() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    bool available() const { return file_ != INVALID_HANDLE_VALUE; }

    // Written first, before any pipeline, so the replay can create a matching device.
    void device(const VkDeviceCreateInfo& info) {
        if (!graphics_) return;
        const std::lock_guard guard(lock_);
        write_packet(device_packet, encode_device(info));
    }

    void shader(VkShaderModule module, const VkShaderModuleCreateInfo& info) {
        if (info.flags || info.pNext || info.codeSize % sizeof(uint32_t)) return;
        const auto words = info.codeSize / sizeof(uint32_t);
        if (words < 5 || words > word_limit || info.pCode[0] != spirv_magic) return;
        const std::lock_guard guard(lock_);
        constexpr size_t tracked_limit = 128 * 1024 * 1024;
        if (info.codeSize > tracked_limit - tracked_code_) return;
        std::vector<uint32_t> code(info.pCode, info.pCode + words);
        if (modules_.try_emplace(module, std::move(code)).second) tracked_code_ += info.codeSize;
    }

    void remove_shader(VkShaderModule module) {
        const std::lock_guard guard(lock_);
        const auto it = modules_.find(module);
        if (it == modules_.end()) return;
        tracked_code_ -= it->second.size() * sizeof(uint32_t);
        modules_.erase(it);
    }

    void descriptor_set(VkDescriptorSetLayout set, const VkDescriptorSetLayoutCreateInfo& info) {
        if (info.flags & ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT || info.bindingCount > binding_limit)
            return;
        const VkDescriptorBindingFlags* flags = nullptr;
        if (info.pNext) {
            const auto* indexing = static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(info.pNext);
            if (indexing->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO || indexing->pNext
                || (indexing->bindingCount && indexing->bindingCount != info.bindingCount))
                return;
            if (indexing->bindingCount) flags = indexing->pBindingFlags;
        }
        DescriptorSet recorded{info.flags, {}};
        recorded.bindings.reserve(info.bindingCount);
        const std::lock_guard guard(lock_);
        size_t sampler_count = 0;
        for (uint32_t i = 0; i < info.bindingCount; ++i) {
            const auto& b = info.pBindings[i];
            if (!b.descriptorCount || b.descriptorType < VK_DESCRIPTOR_TYPE_SAMPLER
                || b.descriptorType > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) return;
            DescriptorBinding binding{{b.binding, b.descriptorType, b.descriptorCount, b.stageFlags}, flags ? flags[i] : 0, {}};
            if (b.pImmutableSamplers && (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER
                || b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
                if (b.descriptorCount > binding_limit - sampler_count) return;
                sampler_count += b.descriptorCount;
                for (uint32_t index = 0; index < b.descriptorCount; ++index) {
                    const auto found = samplers_.find(b.pImmutableSamplers[index]);
                    if (found == samplers_.end()) return;
                    binding.immutable.push_back(found->second);
                }
            }
            recorded.bindings.push_back(std::move(binding));
        }
        const size_t size = recorded.size();
        if (sets_.size() < 4096 && size <= layout_limit_ - tracked_layouts_
            && sets_.try_emplace(set, std::move(recorded)).second) tracked_layouts_ += size;
    }

    void sampler(VkSampler handle, const VkSamplerCreateInfo& info) {
        if (info.pNext || info.flags) return;
        const std::lock_guard guard(lock_);
        if (samplers_.size() < 4096) samplers_.try_emplace(handle, info);
    }

    void remove_sampler(VkSampler handle) {
        const std::lock_guard guard(lock_);
        samplers_.erase(handle);
    }

    void remove_set(VkDescriptorSetLayout set) {
        const std::lock_guard guard(lock_);
        const auto it = sets_.find(set);
        if (it == sets_.end()) return;
        tracked_layouts_ -= it->second.size();
        sets_.erase(it);
    }

    void pipeline_layout(VkPipelineLayout handle, const VkPipelineLayoutCreateInfo& info) {
        if (info.flags || info.pNext || info.setLayoutCount > set_limit || info.pushConstantRangeCount > push_limit)
            return;
        Layout layout;
        const std::lock_guard guard(lock_);
        if (layouts_.size() >= 4096) return;
        size_t size = info.pushConstantRangeCount * sizeof(VkPushConstantRange);
        if (size > layout_limit_ - tracked_layouts_) return;
        for (uint32_t i = 0; i < info.setLayoutCount; ++i) {
            const auto it = sets_.find(info.pSetLayouts[i]);
            if (it == sets_.end()) return;
            size += it->second.size();
            if (size > layout_limit_ - tracked_layouts_) return;
            layout.sets.push_back(it->second);
        }
        if (info.pushConstantRangeCount) {
            layout.push.assign(info.pPushConstantRanges, info.pPushConstantRanges + info.pushConstantRangeCount);
        }
        if (layouts_.try_emplace(handle, std::move(layout)).second) tracked_layouts_ += size;
    }

    void remove_layout(VkPipelineLayout layout) {
        const std::lock_guard guard(lock_);
        const auto it = layouts_.find(layout);
        if (it == layouts_.end()) return;
        tracked_layouts_ -= it->second.push.size() * sizeof(VkPushConstantRange);
        for (const auto& set : it->second.sets) tracked_layouts_ -= set.size();
        layouts_.erase(it);
    }

    void render_pass(VkRenderPass handle, const VkRenderPassCreateInfo& info) {
        if (!graphics_) return;
        auto desc = info;
        std::vector<uint8_t> packet;
        if (!render_pass_offsets(packet, desc, true)) return;
        const std::lock_guard guard(lock_);
        const auto size = packet.size();
        if (passes_.size() < 16384 && size <= layout_limit_ - tracked_layouts_
            && passes_.try_emplace(handle, std::move(packet)).second) tracked_layouts_ += size;
    }

    void remove_pass(VkRenderPass handle) {
        const std::lock_guard guard(lock_);
        const auto found = passes_.find(handle);
        if (found == passes_.end()) return;
        tracked_layouts_ -= found->second.size();
        passes_.erase(found);
    }

    void graphics(const VkGraphicsPipelineCreateInfo& info) {
        if (!graphics_ || info.flags || !pipeline_feedback_only(info.pNext) || !info.stageCount || info.stageCount > max_stages
            || (info.pRasterizationState && info.pRasterizationState->rasterizerDiscardEnable)) return;
        const std::lock_guard guard(lock_);
        if (written_ >= file_limit) return;
        const auto pass = passes_.find(info.renderPass);
        const auto layout = layouts_.find(info.layout);
        if (pass == passes_.end() || layout == layouts_.end()) return;
        VkShaderModuleCreateInfo shaders[max_stages]{};
        for (uint32_t i = 0; i < info.stageCount; ++i) {
            const auto shader = modules_.find(info.pStages[i].module);
            if (shader == modules_.end()) return;
            shaders[i] = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                shader->second.size() * sizeof(uint32_t), shader->second.data()};
        }
        std::vector<uint8_t> layout_packet;
        put_layout(layout_packet, layout->second);
        if (layout_packet.size() > packet_limit) return;
        GraphicsWire wire{info, shaders, pass->second.data(), layout_packet.data(),
            static_cast<uint32_t>(pass->second.size()), static_cast<uint32_t>(layout_packet.size())};
        std::vector<uint8_t> pass_copy = pass->second;
        VkRenderPassCreateInfo pass_info{};
        if (!render_pass_offsets(pass_copy, pass_info, false) || info.subpass >= pass_info.subpassCount) return;
        drop_ignored_state(wire.pipeline, pass_info.pSubpasses[info.subpass]);
        wire.pipeline.pNext = nullptr;
        wire.pipeline.layout = VK_NULL_HANDLE;
        wire.pipeline.renderPass = VK_NULL_HANDLE;
        wire.pipeline.basePipelineHandle = VK_NULL_HANDLE;
        wire.pipeline.basePipelineIndex = -1;
        std::vector<uint8_t> packet;
        if (graphics_offsets(packet, wire, true)) write_packet(graphics_packet, packet);
    }

    void pipeline(const VkComputePipelineCreateInfo& info) {
        if (info.flags || info.stage.flags || info.stage.pNext || info.stage.stage != VK_SHADER_STAGE_COMPUTE_BIT
            || !info.stage.pName) return;
        for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext) {
            if (next->sType != VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) return;
        }
        const size_t length = strnlen_s(info.stage.pName, entry_limit + 1);
        if (!length || length > entry_limit) return;
        Recipe recipe;
        {
            const std::lock_guard guard(lock_);
            if (written_ >= file_limit) return;
            const auto module = modules_.find(info.stage.module);
            const auto layout = layouts_.find(info.layout);
            if (module == modules_.end() || layout == layouts_.end()) return;
            recipe.code = module->second;
            recipe.layout = layout->second;
        }
        recipe.entry.assign(info.stage.pName, length);
        if (const auto* spec = info.stage.pSpecializationInfo) {
            if (spec->mapEntryCount > constant_limit || spec->dataSize > data_limit) return;
            for (uint32_t i = 0; i < spec->mapEntryCount; ++i) {
                const auto& entry = spec->pMapEntries[i];
                if (!entry.size || entry.offset > spec->dataSize || entry.size > spec->dataSize - entry.offset) return;
                recipe.constants.push_back({entry.constantID, entry.offset, static_cast<uint32_t>(entry.size)});
            }
            if (spec->dataSize) {
                const auto* data = static_cast<const uint8_t*>(spec->pData);
                recipe.data.assign(data, data + spec->dataSize);
            }
        }
        const auto bytes = encode(recipe);
        const std::lock_guard guard(lock_);
        write_packet(compute_packet, bytes);
    }

private:
    void write_packet(uint32_t kind, const std::vector<uint8_t>& bytes) {
        const size_t prefix = graphics_ ? sizeof(kind) : 0;
        if (bytes.size() + prefix > packet_limit || file_limit - written_ < bytes.size() + prefix + sizeof(uint32_t)) return;
        const auto size = static_cast<uint32_t>(bytes.size() + prefix);
        if (!write(&size, sizeof(size)) || (graphics_ && !write(&kind, sizeof(kind)))
            || !write(bytes.data(), static_cast<uint32_t>(bytes.size()))) written_ = file_limit;
    }

    bool write(const void* data, uint32_t size) {
        DWORD written = 0;
        if (!WriteFile(file_, data, size, &written, nullptr) || written != size) return false;
        written_ += size;
        return true;
    }

    HANDLE file_{INVALID_HANDLE_VALUE};
    bool graphics_{};
    std::mutex lock_;
    uint32_t written_{};
    size_t tracked_code_{};
    size_t tracked_layouts_{};
    static constexpr size_t layout_limit_ = 64 * 1024 * 1024;
    std::unordered_map<VkShaderModule, std::vector<uint32_t>> modules_;
    std::unordered_map<VkSampler, Sampler> samplers_;
    std::unordered_map<VkDescriptorSetLayout, DescriptorSet> sets_;
    std::unordered_map<VkPipelineLayout, Layout> layouts_;
    std::unordered_map<VkRenderPass, std::vector<uint8_t>> passes_;
};
}  // namespace tessera::vulkan_recipe
