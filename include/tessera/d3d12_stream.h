// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tessera/d3d12_capture_format.h"
#include "tessera/packet_offsets.h"

#include <cstddef>
#include <cstring>
#include <vector>

namespace tessera::d3d12_capture {
struct StreamHeader {
    uint32_t size;
    uint32_t reserved;
};

using StreamRelocator = PacketOffsets<max_graphics_bytes>;

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
