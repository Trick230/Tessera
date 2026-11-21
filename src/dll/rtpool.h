// SPDX-License-Identifier: GPL-3.0-or-later
// R1 (rt_recycle, off by default): big render targets the game releases are
// kept for a while and handed back on its next create with the same
// description. Near the VRAM budget a free followed by a new allocation makes
// the driver wait inside Present (FF7R: 40-54 ms every ~10 s at 4K).
// Contents of a handed-back texture are the previous owner's (D3D11 leaves
// them undefined for a create without initial data).
#pragma once
#include <d3d11.h>

#include <cstdint>

namespace tessera {
// Game CreateTexture2D without initial data: true and *out set (one reference
// for the game) when a released texture with this description is reused.
bool rtpool_take(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d, uint64_t bytes, ID3D11Texture2D** out);
// After a game CreateTexture2D that made a new texture without initial data:
// keeps a reference to it when it qualifies.
void rtpool_track(ID3D11Device* dev, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, uint64_t bytes);
// Device creation, after the device hooks (srv_ok: the CreateSRV hook in
// hooks.cpp is in place): hooks the other view creations the pool must see,
// as a view keeps its texture alive without a public reference.
void rtpool_on_device(ID3D11Device* dev, bool srv_ok);
// Any view made on any resource (the SRV hook in hooks.cpp calls this).
void rtpool_on_view(ID3D11Resource* res, ID3D11View* view);
// No more textures handed out or kept, for the rest of the session (a hook
// missing, or a deferred context whose command lists hold textures unseen).
// Kept ones are still watched and freed.
void rtpool_stop(const char* why);
// Present (outer, once per frame): looks at one kept texture; frees it when
// the game has not used it for 30 s or the unused ones exceed 512 MB.
void rtpool_frame();
}  // namespace tessera
