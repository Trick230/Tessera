// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <cstdint>

namespace tessera {
// With tex == nullptr, look up existing content. With tex, publish a successful
// normal creation. A hit returns an owned reference to the shared texture.
ID3D11Texture2D* dedup_texture(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& desc,
                              const D3D11_SUBRESOURCE_DATA* init, uint64_t bytes, ID3D11Texture2D* tex = nullptr);
void dedup_background_tick();
}  // namespace tessera
