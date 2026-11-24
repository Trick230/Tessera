// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace tessera::vulkan_recipe {
inline constexpr char legacy_magic[8] = "TSRVKC1";
inline constexpr char magic[8] = "TSRVKC2";
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
struct Sampler {
    VkSamplerCreateFlags flags;
    VkFilter mag;
    VkFilter min;
    VkSamplerMipmapMode mip;
    VkSamplerAddressMode u, v, w;
    float bias;
    VkBool32 anisotropy;
    float max_anisotropy;
    VkBool32 compare;
    VkCompareOp compare_op;
    float min_lod, max_lod;
    VkBorderColor border;
    VkBool32 unnormalized;

    explicit Sampler(const VkSamplerCreateInfo& s) : flags(s.flags), mag(s.magFilter), min(s.minFilter),
        mip(s.mipmapMode), u(s.addressModeU), v(s.addressModeV), w(s.addressModeW), bias(s.mipLodBias),
        anisotropy(s.anisotropyEnable), max_anisotropy(s.maxAnisotropy), compare(s.compareEnable),
        compare_op(s.compareOp), min_lod(s.minLod), max_lod(s.maxLod), border(s.borderColor),
        unnormalized(s.unnormalizedCoordinates) {}
    Sampler() = default;
    VkSamplerCreateInfo info() const {
        return {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr, flags, mag, min, mip, u, v, w,
            bias, anisotropy, max_anisotropy, compare, compare_op, min_lod, max_lod, border, unnormalized};
    }
};
struct DescriptorBinding {
    Binding state{};
    VkDescriptorBindingFlags flags{};
    std::vector<Sampler> immutable;
};
struct DescriptorSet {
    VkDescriptorSetLayoutCreateFlags flags{};
    std::vector<DescriptorBinding> bindings;
    size_t size() const {
        size_t bytes = bindings.size() * sizeof(DescriptorBinding);
        for (const auto& binding : bindings) bytes += binding.immutable.size() * sizeof(Sampler);
        return bytes;
    }
};
struct Specialization {
    uint32_t id;
    uint32_t offset;
    uint32_t size;
};
struct Layout {
    std::vector<DescriptorSet> sets;
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
static_assert(sizeof(Sampler) == 64);
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

template<class T>
void put_value(std::vector<uint8_t>& bytes, const T& value) {
    put<T>(bytes, {&value, 1});
}

inline void put_layout(std::vector<uint8_t>& bytes, const Layout& layout) {
    put_value(bytes, static_cast<uint32_t>(layout.sets.size()));
    for (const auto& set : layout.sets) {
        put_value(bytes, set.flags);
        put_value(bytes, static_cast<uint32_t>(set.bindings.size()));
        for (const auto& binding : set.bindings) {
            put_value(bytes, binding.state);
            put_value(bytes, binding.flags);
            put<Sampler>(bytes, binding.immutable);
        }
    }
    put<VkPushConstantRange>(bytes, layout.push);
}

inline std::vector<uint8_t> encode(const Recipe& recipe) {
    std::vector<uint8_t> bytes;
    put<uint32_t>(bytes, recipe.code);
    put<char>(bytes, recipe.entry);
    put_layout(bytes, recipe.layout);
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
    template<class T>
    bool value(T& result) {
        uint32_t count = 0;
        if (bytes_.size() < sizeof(count) + sizeof(result)) return false;
        std::memcpy(&count, bytes_.data(), sizeof(count));
        if (count != 1) return false;
        std::memcpy(&result, bytes_.data() + sizeof(count), sizeof(result));
        bytes_ = bytes_.subspan(sizeof(count) + sizeof(result));
        return true;
    }

private:
    std::span<const uint8_t> bytes_;
};

inline bool read_layout(Reader& reader, Layout& layout, bool legacy) {
    uint32_t count = 0;
    if (!reader.value(count) || count > set_limit) return false;
    layout.sets.resize(count);
    for (auto& set : layout.sets) {
        if (legacy) {
            std::vector<Binding> bindings;
            if (!reader.take<Binding>(bindings, binding_limit)) return false;
            for (const auto& binding : bindings) set.bindings.push_back({binding, 0, {}});
        } else {
            if (!reader.value(set.flags) || set.flags & ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
                || !reader.value(count) || count > binding_limit) return false;
            set.bindings.resize(count);
            for (auto& binding : set.bindings) {
                if (!reader.value(binding.state) || !reader.value(binding.flags)
                    || binding.flags & ~(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                        | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                        | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)
                    || !reader.take<Sampler>(binding.immutable, binding_limit)) return false;
                if (!binding.immutable.empty() && (binding.immutable.size() != binding.state.count
                    || (binding.state.type != VK_DESCRIPTOR_TYPE_SAMPLER
                        && binding.state.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))) return false;
            }
        }
        for (const auto& b : set.bindings) {
            if (b.state.type < VK_DESCRIPTOR_TYPE_SAMPLER || b.state.type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                || !b.state.count) return false;
        }
    }
    return reader.take<VkPushConstantRange>(layout.push, push_limit);
}

inline bool decode(std::span<const uint8_t> bytes, Recipe& recipe, bool legacy) {
    Reader reader(bytes);
    if (!reader.take<uint32_t>(recipe.code, word_limit) || recipe.code.size() < 5 || recipe.code[0] != spirv_magic
        || !reader.take<char>(recipe.entry, entry_limit) || recipe.entry.empty()
        || recipe.entry.find('\0') != std::string::npos || !read_layout(reader, recipe.layout, legacy))
        return false;
    if (!reader.take<Specialization>(recipe.constants, constant_limit)
        || !reader.take<uint8_t>(recipe.data, data_limit) || !reader.finished()) return false;
    for (const auto& entry : recipe.constants) {
        if (!entry.size || entry.offset > recipe.data.size() || entry.size > recipe.data.size() - entry.offset)
            return false;
    }
    return true;
}
}  // namespace tessera::vulkan_recipe
