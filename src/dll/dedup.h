// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <cstdint>

namespace tessera {
struct DedupLookup {
    uint64_t signature = 0;
    bool eligible = false;
};
// With tex == nullptr, look up existing content. With tex, publish a successful
// normal creation using the same fingerprint. A hit returns an owned reference.
ID3D11Texture2D* dedup_texture(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& desc,
                              const D3D11_SUBRESOURCE_DATA* init, uint64_t bytes, DedupLookup& lookup,
                              ID3D11Texture2D* tex = nullptr);
void dedup_background_tick();
void dedup_log_summary();
}  // namespace tessera
