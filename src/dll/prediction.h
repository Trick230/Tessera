// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <cstdint>

namespace tessera {
struct TextureHint {
    uint64_t key;
    uint32_t resident_index;  // one-based
};
constexpr unsigned kMaterialTextures = 8;
uint64_t texture_prediction_key(const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* init);
void prediction_material(const TextureHint* textures, unsigned count);
void prediction_background_tick();
}  // namespace tessera
