// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <cstdint>

namespace tessera {
// Formats with a simple linear layout. Everything else stays on the driver's path.
struct TextureLayout {
    UINT block, bytes;
};

inline TextureLayout texture_layout(DXGI_FORMAT f) {
    if ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) ||
        (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM))
        return {4, 8};
    if ((f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) ||
        (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
        (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB))
        return {4, 16};
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return {1, 4};
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return {1, 8};
    return {};
}

inline UINT blocks(UINT extent, UINT block) { return (extent - 1) / block + 1; }
}  // namespace tessera
