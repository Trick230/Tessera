// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "compute_recipe.h"

#include <windows.h>

#include <mutex>
#include <unordered_map>

namespace tessera::vulkan_recipe {
class Capture {
public:
    Capture(const std::wstring& path, uint32_t api, const VkPhysicalDeviceProperties& properties) {
        file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        Header header{};
        std::memcpy(header.tag, magic, sizeof(magic));
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
        if (info.flags || info.pNext || info.bindingCount > binding_limit) return;
        std::vector<Binding> bindings;
        bindings.reserve(info.bindingCount);
        for (uint32_t i = 0; i < info.bindingCount; ++i) {
            const auto& b = info.pBindings[i];
            if (b.pImmutableSamplers || !b.descriptorCount || b.descriptorType < VK_DESCRIPTOR_TYPE_SAMPLER
                || b.descriptorType > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) return;
            bindings.push_back({b.binding, b.descriptorType, b.descriptorCount, b.stageFlags});
        }
        const std::lock_guard guard(lock_);
        const size_t size = bindings.size() * sizeof(Binding);
        if (sets_.size() < 4096 && size <= layout_limit_ - tracked_layouts_
            && sets_.try_emplace(set, std::move(bindings)).second) tracked_layouts_ += size;
    }

    void remove_set(VkDescriptorSetLayout set) {
        const std::lock_guard guard(lock_);
        const auto it = sets_.find(set);
        if (it == sets_.end()) return;
        tracked_layouts_ -= it->second.size() * sizeof(Binding);
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
            size += it->second.size() * sizeof(Binding);
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
        for (const auto& set : it->second.sets) tracked_layouts_ -= set.size() * sizeof(Binding);
        layouts_.erase(it);
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
        if (bytes.size() > packet_limit) return;
        const auto size = static_cast<uint32_t>(bytes.size());
        const std::lock_guard guard(lock_);
        if (file_limit - written_ < size + sizeof(size)) return;
        if (!write(&size, sizeof(size)) || !write(bytes.data(), size)) written_ = file_limit;
    }

private:
    bool write(const void* data, uint32_t size) {
        DWORD written = 0;
        if (!WriteFile(file_, data, size, &written, nullptr) || written != size) return false;
        written_ += size;
        return true;
    }

    HANDLE file_{INVALID_HANDLE_VALUE};
    std::mutex lock_;
    uint32_t written_{};
    size_t tracked_code_{};
    size_t tracked_layouts_{};
    static constexpr size_t layout_limit_ = 64 * 1024 * 1024;
    std::unordered_map<VkShaderModule, std::vector<uint32_t>> modules_;
    std::unordered_map<VkDescriptorSetLayout, std::vector<Binding>> sets_;
    std::unordered_map<VkPipelineLayout, Layout> layouts_;
};
}  // namespace tessera::vulkan_recipe
