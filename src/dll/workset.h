// SPDX-License-Identifier: GPL-3.0-or-later
// M11: texture working set. Tracks the sampled textures the game creates and
// marks the ones it binds, so the session shows how much of its texture
// memory play really touches: the data that decides M3/M4/M5. Not seen:
// textures made by D3DX11 in System32 (foreign), CreateTexture2D1 and
// CreateShaderResourceView1 (not hooked).
#pragma once
#include <d3d11.h>
#include <stdint.h>

namespace tessera {
// From device creation: hooks SetShaderResources of the immediate and the
// deferred context classes.
void workset_on_device(ID3D11Device* dev);
// A game texture that is only sampled (no target binds, not shared).
void workset_on_texture(ID3D11Texture2D* tex, uint64_t bytes);
// Any SRV; kept only when it views a tracked texture.
void workset_on_srv(ID3D11Resource* res, ID3D11ShaderResourceView* view);
// From Present (outer, real frames).
void workset_frame();
// From the writer thread, about once a second: pushes a WORKSET record.
void workset_background_tick();
}  // namespace tessera
