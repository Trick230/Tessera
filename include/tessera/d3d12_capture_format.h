// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace tessera::d3d12_capture {
inline constexpr char magic[8] = "TSRD12C";
inline constexpr uint32_t version = 1;
inline constexpr uint32_t max_shader_bytes = 16 * 1024 * 1024;
inline constexpr uint32_t max_root_bytes = 1024 * 1024;
inline constexpr uint64_t max_capture_bytes = 512ull * 1024 * 1024;

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t reserved;
};

// A complete record is followed by shader_size shader bytes and root_size
// serialized root signature bytes. Root size zero uses the embedded signature.
struct ComputeRecord {
    uint32_t shader_size;
    uint32_t root_size;
    uint32_t node_mask;
    uint32_t root_node_mask;
    uint32_t flags;
    uint32_t reserved;
    uint64_t create_ns;
};

static_assert(sizeof(Header) == 16);
static_assert(sizeof(ComputeRecord) == 32);
}  // namespace tessera::d3d12_capture
