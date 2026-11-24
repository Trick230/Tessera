// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace tessera::vulkan_recipe {
inline constexpr char magic[8] = "TSRVKC1";
inline constexpr uint32_t word_limit = 4 * 1024 * 1024;
inline constexpr uint32_t set_limit = 32;
inline constexpr uint32_t binding_limit = 4096;
inline constexpr uint32_t push_limit = 32;
inline constexpr uint32_t constant_limit = 4096;
inline constexpr uint32_t data_limit = 1024 * 1024;
inline constexpr uint32_t entry_limit = 256;
inline constexpr uint32_t packet_limit = 32 * 1024 * 1024;
inline constexpr uint32_t file_limit = 256 * 1024 * 1024;
inline constexpr uint32_t spirv_magic = 0x07230203;

struct Header {
    char tag[8];
    uint32_t api;
    uint32_t vendor;
    uint32_t device;
    uint8_t cache_uuid[VK_UUID_SIZE];
};
struct Binding {
    uint32_t number;
    VkDescriptorType type;
    uint32_t count;
    VkShaderStageFlags stages;
};
struct Specialization {
    uint32_t id;
    uint32_t offset;
    uint32_t size;
};
struct Layout {
    std::vector<std::vector<Binding>> sets;
    std::vector<VkPushConstantRange> push;
};
struct Recipe {
    std::vector<uint32_t> code;
    std::string entry;
    Layout layout;
    std::vector<Specialization> constants;
    std::vector<uint8_t> data;
};
static_assert(sizeof(Header) == 36);
static_assert(sizeof(Binding) == 16);
static_assert(sizeof(Specialization) == 12);
static_assert(sizeof(VkPushConstantRange) == 12);

template<class T>
void put(std::vector<uint8_t>& bytes, std::span<const T> items) {
    const auto count = static_cast<uint32_t>(items.size());
    const auto start = bytes.size();
    bytes.resize(start + sizeof(count) + items.size_bytes());
    std::memcpy(bytes.data() + start, &count, sizeof(count));
    if (count) std::memcpy(bytes.data() + start + sizeof(count), items.data(), items.size_bytes());
}

inline std::vector<uint8_t> encode(const Recipe& recipe) {
    std::vector<uint8_t> bytes;
    put<uint32_t>(bytes, recipe.code);
    put<char>(bytes, recipe.entry);
    const auto sets = static_cast<uint32_t>(recipe.layout.sets.size());
    put<uint32_t>(bytes, {&sets, 1});
    for (const auto& bindings : recipe.layout.sets) put<Binding>(bytes, bindings);
    put<VkPushConstantRange>(bytes, recipe.layout.push);
    put<Specialization>(bytes, recipe.constants);
    put<uint8_t>(bytes, recipe.data);
    return bytes;
}

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    template<class T, class Out>
    bool take(Out& items, uint32_t limit) {
        if (bytes_.size() < sizeof(uint32_t)) return false;
        uint32_t count{};
        std::memcpy(&count, bytes_.data(), sizeof(count));
        bytes_ = bytes_.subspan(sizeof(count));
        if (count > limit || count > bytes_.size() / sizeof(T)) return false;
        const size_t length = count * sizeof(T);
        items.resize(count);
        if (count) std::memcpy(items.data(), bytes_.data(), length);
        bytes_ = bytes_.subspan(length);
        return true;
    }

    bool finished() const { return bytes_.empty(); }

private:
    std::span<const uint8_t> bytes_;
};

inline bool decode(std::span<const uint8_t> bytes, Recipe& recipe) {
    Reader reader(bytes);
    std::vector<uint32_t> count;
    if (!reader.take<uint32_t>(recipe.code, word_limit) || recipe.code.size() < 5 || recipe.code[0] != spirv_magic
        || !reader.take<char>(recipe.entry, entry_limit) || recipe.entry.empty()
        || recipe.entry.find('\0') != std::string::npos
        || !reader.take<uint32_t>(count, 1) || count.size() != 1 || count[0] > set_limit)
        return false;
    recipe.layout.sets.resize(count[0]);
    for (auto& bindings : recipe.layout.sets) {
        if (!reader.take<Binding>(bindings, binding_limit)) return false;
        for (const auto& binding : bindings) {
            if (binding.type < VK_DESCRIPTOR_TYPE_SAMPLER || binding.type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                || !binding.count) return false;
        }
    }
    if (!reader.take<VkPushConstantRange>(recipe.layout.push, push_limit)
        || !reader.take<Specialization>(recipe.constants, constant_limit)
        || !reader.take<uint8_t>(recipe.data, data_limit) || !reader.finished()) return false;
    for (const auto& entry : recipe.constants) {
        if (!entry.size || entry.offset > recipe.data.size() || entry.size > recipe.data.size() - entry.offset)
            return false;
    }
    return true;
}
}  // namespace tessera::vulkan_recipe
