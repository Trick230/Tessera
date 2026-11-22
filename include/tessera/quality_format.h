// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#define TESSERA_QUALITY_MAGIC "TSRTEX1"
#define TESSERA_QUALITY_MIPS 12

// Tight mip rows, largest first. The original data is always present. A
// prepared entry appends the compressed mip rows in the same order.
typedef struct TesseraQualityHeader {
    char magic[8];
    uint32_t width, height, mips, format;
    uint32_t compressed, original_bytes, compressed_bytes, reserved;
    uint64_t key;
} TesseraQualityHeader;

#ifdef __cplusplus
#include <dxgiformat.h>
#include <algorithm>
#include <cstring>

static_assert(sizeof(TesseraQualityHeader) == 48);

namespace tessera {
inline DXGI_FORMAT quality_compressed_format(uint32_t format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_BC6H_UF16;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

struct QualityMip {
    uint32_t pitch, rows;
    uint32_t bytes() const { return pitch * rows; }
};

inline QualityMip quality_mip(const TesseraQualityHeader& h, unsigned mip, bool compressed) {
    const auto w = std::max(1u, h.width >> mip), y = std::max(1u, h.height >> mip);
    if (compressed) return {(w + 3) / 4 * 16, (y + 3) / 4};
    return {w * (h.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u), y};
}

inline uint32_t quality_bytes(const TesseraQualityHeader& h, bool compressed) {
    uint32_t total = 0;
    for (unsigned mip = 0; mip < h.mips; ++mip) total += quality_mip(h, mip, compressed).bytes();
    return total;
}

inline bool quality_shape(const TesseraQualityHeader& h) {
    return h.width >= 4 && h.height >= 4 && !(h.width & 3) && !(h.height & 3) &&
           h.width <= 2048 && h.height <= 2048 && h.mips >= 2 && h.mips <= TESSERA_QUALITY_MIPS &&
           (std::max(h.width, h.height) >> (h.mips - 1)) != 0 &&
           quality_compressed_format(h.format) != DXGI_FORMAT_UNKNOWN;
}

inline bool quality_header(const TesseraQualityHeader& h, uint64_t size, bool compressed) {
    return std::memcmp(h.magic, TESSERA_QUALITY_MAGIC, 8) == 0 && h.key &&
           quality_shape(h) && h.original_bytes == quality_bytes(h, false) &&
           h.compressed == (compressed ? uint32_t(quality_compressed_format(h.format)) : 0) &&
           h.compressed_bytes == (compressed ? quality_bytes(h, true) : 0) &&
           size == sizeof h + uint64_t(h.original_bytes) + h.compressed_bytes;
}
}  // namespace tessera
#endif
