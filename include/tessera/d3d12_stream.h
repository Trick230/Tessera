// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tessera/d3d12_capture_format.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace tessera::d3d12_capture {
struct StreamHeader {
    uint32_t size;
    uint32_t reserved;
};

// The native x64 stream is followed by pointed-to data. Pointer slots hold
// byte offsets from the start of the payload until they are validated on replay.
class StreamRelocator {
public:
    StreamRelocator(std::vector<uint8_t>& bytes, size_t data_start, bool capture)
        : bytes_(bytes), start_(data_start), capture_(capture) {}

    template<class T>
    bool array(const T*& pointer, size_t count, size_t limit) {
        if (!count) {
            pointer = nullptr;
            return true;
        }
        if (!pointer || count > limit) return false;
        const size_t size = count * sizeof(T);
        if (capture_) {
            const size_t offset = (bytes_.size() + alignof(T) - 1) & ~(alignof(T) - 1);
            if (offset > max_graphics_bytes || size > max_graphics_bytes - offset) return false;
            bytes_.resize(offset + size);
            std::memcpy(bytes_.data() + offset, pointer, size);
            pointer = reinterpret_cast<const T*>(offset);
        } else {
            const auto offset = reinterpret_cast<uintptr_t>(pointer);
            if (offset < start_ || offset > bytes_.size() || offset % alignof(T) || size > bytes_.size() - offset)
                return false;
            pointer = reinterpret_cast<const T*>(bytes_.data() + offset);
            start_ = offset + size;
        }
        return true;
    }

    template<class Char>
    bool name(const Char*& pointer, size_t limit = 256) {
        if (!pointer) return true;
        if (capture_) {
            size_t length = 0;
            while (length < limit && pointer[length]) ++length;
            return length < limit && array(pointer, length + 1, limit);
        }
        const auto offset = reinterpret_cast<uintptr_t>(pointer);
        if (offset < start_ || offset >= bytes_.size() || offset % alignof(Char)) return false;
        const auto available = (bytes_.size() - offset) / sizeof(Char);
        const auto* value = reinterpret_cast<const Char*>(bytes_.data() + offset);
        for (size_t i = 0; i < available && i < limit; ++i) {
            if (value[i]) continue;
            pointer = value;
            start_ = offset + (i + 1) * sizeof(Char);
            return true;
        }
        return false;
    }

    template<class T, class Transform>
    bool mapped_array(const T*& pointer, size_t count, size_t limit, Transform transform) {
        const T* original = pointer;
        if (!array(pointer, count, limit)) return false;
        if (!count) return true;
        if (!capture_) original = pointer;
        const size_t offset = capture_ ? reinterpret_cast<uintptr_t>(pointer)
            : static_cast<size_t>(reinterpret_cast<uintptr_t>(pointer) - reinterpret_cast<uintptr_t>(bytes_.data()));
        for (size_t i = 0; i < count; ++i) {
            T item = original[i];
            if (!transform(item)) return false;
            std::memcpy(bytes_.data() + offset + i * sizeof(T), &item, sizeof(T));
        }
        return true;
    }

    template<class T>
    bool named_array(const T*& pointer, size_t count, size_t limit) {
        return mapped_array(pointer, count, limit, [&](T& item) { return name(item.SemanticName); });
    }

private:
    std::vector<uint8_t>& bytes_;
    size_t start_;
    bool capture_;
};

constexpr size_t align_stream(size_t offset, size_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

inline bool relocate_stream(std::vector<uint8_t>& payload, bool capture, ID3D12RootSignature*& root) {
    if constexpr (sizeof(void*) != 8) return false;
    if (payload.size() < sizeof(StreamHeader)) return false;
    StreamHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.reserved || !header.size || header.size > payload.size() - sizeof(header)) return false;
    size_t at = sizeof(header);
    const size_t end = at + header.size;
    uint64_t seen = 0;
    StreamRelocator relocate(payload, end, capture);
    while (at < end) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
        if (end - at < sizeof(type)) return false;
        std::memcpy(&type, payload.data() + at, sizeof(type));
        const uint32_t id = static_cast<uint32_t>(type);
        if (id >= 64 || (seen & (uint64_t{1} << id))) return false;
        seen |= uint64_t{1} << id;
        const auto field = [&]<class T>(auto&& transform) {
            constexpr size_t value_offset = align_stream(sizeof(type), alignof(T));
            constexpr size_t field_size = align_stream(value_offset + sizeof(T), alignof(void*));
            if (end - at < field_size) return false;
            T item{};
            std::memcpy(&item, payload.data() + at + value_offset, sizeof(T));
            if (!transform(item)) return false;
            auto* out = payload.data() + at;
            std::memset(out, 0, field_size);
            std::memcpy(out, &type, sizeof(type));
            std::memcpy(out + value_offset, &item, sizeof(T));
            at += field_size;
            return true;
        };
        const auto scalar = [&]<class T>() { return field.template operator()<T>([](auto&) { return true; }); };
        bool ok = false;
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                ok = field.template operator()<ID3D12RootSignature*>([&](auto& value) {
                    if (capture) { root = value; value = nullptr; }
                    else if (value) return false;
                    else value = root;
                    return true;
                });
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
                ok = field.template operator()<D3D12_SHADER_BYTECODE>([&](auto& shader) {
                    auto* bytes = static_cast<const uint8_t*>(shader.pShaderBytecode);
                    if (!relocate.array(bytes, shader.BytecodeLength, max_shader_bytes)) return false;
                    shader.pShaderBytecode = bytes;
                    return true;
                });
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
                ok = field.template operator()<D3D12_INPUT_LAYOUT_DESC>([&](auto& layout) {
                    return relocate.named_array(layout.pInputElementDescs, layout.NumElements,
                        D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT);
                });
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
                ok = field.template operator()<D3D12_STREAM_OUTPUT_DESC>([&](auto& output) {
                    return relocate.named_array(output.pSODeclaration, output.NumEntries, D3D12_SO_OUTPUT_COMPONENT_COUNT)
                        && relocate.array(output.pBufferStrides, output.NumStrides, D3D12_SO_BUFFER_SLOT_COUNT);
                });
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
                ok = field.template operator()<D3D12_VIEW_INSTANCING_DESC>([&](auto& view) {
                    return relocate.array(view.pViewInstanceLocations, view.ViewInstanceCount,
                        D3D12_MAX_VIEW_INSTANCE_COUNT);
                });
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                ok = field.template operator()<D3D12_CACHED_PIPELINE_STATE>([](auto& value) { value = {}; return true; });
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: ok = scalar.template operator()<D3D12_BLEND_DESC>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: ok = scalar.template operator()<UINT>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: ok = scalar.template operator()<D3D12_RASTERIZER_DESC>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: ok = scalar.template operator()<D3D12_RASTERIZER_DESC1>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: ok = scalar.template operator()<D3D12_RASTERIZER_DESC2>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: ok = scalar.template operator()<D3D12_DEPTH_STENCIL_DESC>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: ok = scalar.template operator()<D3D12_DEPTH_STENCIL_DESC1>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: ok = scalar.template operator()<D3D12_DEPTH_STENCIL_DESC2>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: ok = scalar.template operator()<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: ok = scalar.template operator()<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: ok = scalar.template operator()<D3D12_RT_FORMAT_ARRAY>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: ok = scalar.template operator()<DXGI_FORMAT>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: ok = scalar.template operator()<DXGI_SAMPLE_DESC>(); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: ok = scalar.template operator()<D3D12_PIPELINE_STATE_FLAGS>(); break;
            default: return false;
        }
        if (!ok) return false;
    }
    return true;
}
}  // namespace tessera::d3d12_capture
