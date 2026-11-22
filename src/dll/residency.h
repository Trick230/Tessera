// SPDX-License-Identifier: GPL-3.0-or-later
// M3-M5, M10: mip residency. Large immutable game textures are created as
// tiled textures backed by their own tile pool. When VRAM is short, the top
// mip of a texture not sampled for a while is copied to system memory and
// its tiles freed (M4); the sampler clamps to mip 1 meanwhile. The first
// bind of one of its views (or a copy from it) puts the mip back before the
// call reaches the driver, so no draw ever sees the smaller texture (M5).
// System memory held this way is capped (M10). Off unless mip_residency=1.
#pragma once
#include <d3d11.h>
#include <stdint.h>

namespace tessera {
using CreateTex2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                  const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

// From device creation, after workset_on_device: adopts the device when
// nothing is managed yet and it supports tiled resources tier 2.
void residency_on_device(ID3D11Device* dev);
void residency_on_swapchain(IDXGISwapChain* sc, ID3D11Device* dev);
// Every new deferred context: each has a vtable of its own. nullptr: new
// contexts cannot be hooked (evictions off).
void residency_on_context(ID3D11DeviceContext* ctx);
// After a swapchain is made or a tile mapping: the runtime rewrites some
// immediate-context methods then; patches them again (also the meter's).
// imm: that device's immediate context; others are ignored.
void residency_check_hooks(ID3D11DeviceContext* imm);
void residency_after_flush(ID3D11DeviceContext* ctx);
// Game texture creation (outer call). Creates a managed texture through the
// original create and returns true when the texture qualifies; false leaves
// the creation to the caller.
bool residency_create(ID3D11Device* dev, CreateTex2DFn create, const D3D11_TEXTURE2D_DESC& d,
                      const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out, uint64_t bytes);
// A view of managed texture i is bound (or it is a copy source) on ctx.
// On the immediate context the texture is whole when this returns.
void residency_on_use(uint32_t i, ID3D11DeviceContext* ctx);
void residency_predict(uint32_t i, uint64_t key);
// Its use cannot be seen (untracked or foreign view): never evict it.
void residency_pin(uint32_t i);
void residency_pin_resource(ID3D11Resource* res);
// Before each sequenced Present: restores, uploads and evictions. True if sc
// presents the managed device (then residency_check_hooks after Present).
bool residency_present(IDXGISwapChain* sc);
// After that Present, or a tiled resource the game created: either switches
// the immediate context's copy methods (patches again).
void residency_after_present();
// From session_frame.
void residency_frame();
// From the writer thread, about once a second: picks cold textures and
// pushes a RESIDENCY record.
void residency_background_tick();
void residency_streaming_tick();
}  // namespace tessera
