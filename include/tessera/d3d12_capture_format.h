// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace tessera::d3d12_capture {
inline constexpr char magic[8] = "TSRD12C";
// 2: AddToStateObject records name their parent by hash instead of embedding it.
inline constexpr uint32_t version = 2;
inline constexpr uint32_t max_shader_bytes = 16 * 1024 * 1024;
inline constexpr uint32_t max_root_bytes = 1024 * 1024;
inline constexpr uint32_t max_graphics_bytes = 64 * 1024 * 1024;
inline constexpr uint64_t max_capture_bytes = 512ull * 1024 * 1024;
inline constexpr uint32_t compute_kind = 0;
inline constexpr uint32_t graphics_kind = 1;
inline constexpr uint32_t stream_kind = 2;
inline constexpr uint32_t raytracing_kind = 3;
inline constexpr uint32_t raytracing_add_kind = 4;
inline constexpr size_t graphics_stages = 5;

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t reserved;
};

// Each record contains a payload and a serialized root signature. A root size
// of zero lets the runtime use the root signature embedded in the shader.
struct Record {
    uint32_t payload_size;
    uint32_t root_size;
    uint32_t node_mask;
    uint32_t root_node_mask;
    uint32_t flags;
    uint32_t kind;
    uint64_t create_ns;
};

// The graphics payload consists of this state, then VS/PS/DS/HS/GS bytecode,
// then input elements with NUL-terminated semantic names.
struct GraphicsState {
    uint32_t code_sizes[graphics_stages];
    uint32_t input_elements;
    D3D12_BLEND_DESC blend;
    uint32_t sample_mask;
    D3D12_RASTERIZER_DESC rasterizer;
    D3D12_DEPTH_STENCIL_DESC depth_stencil;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
    uint32_t render_targets;
    DXGI_FORMAT formats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    DXGI_FORMAT depth_format;
    DXGI_SAMPLE_DESC samples;
};

struct InputElement {
    uint32_t name_size;
    uint32_t semantic_index;
    DXGI_FORMAT format;
    uint32_t slot;
    uint32_t offset;
    D3D12_INPUT_CLASSIFICATION classification;
    uint32_t step_rate;
};

static_assert(sizeof(Header) == 16);
static_assert(sizeof(Record) == 32);
static_assert(sizeof(InputElement) == 28);
}  // namespace tessera::d3d12_capture
