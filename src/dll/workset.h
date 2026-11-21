// SPDX-License-Identifier: GPL-3.0-or-later
// M11: texture working set. Tracks the sampled textures the game creates and
// marks the ones it binds, so the session shows how much of its texture
// memory play really touches: the data that decides M3/M4/M5. Not seen:
// textures made by D3DX11 in System32 (foreign), CreateTexture2D1 and
// CreateShaderResourceView1 (not hooked).
#pragma once
#include <d3d11.h>
#include <stdint.h>

#include <atomic>

namespace tessera {
// From device creation: hooks SetShaderResources of the immediate context.
void workset_on_device(ID3D11Device* dev);
// Every new deferred context: each has a vtable of its own.
void workset_on_context(ID3D11DeviceContext* ctx);
// The runtime rewrites some immediate-context methods (when a swapchain is
// made): patches them again for imm if it is the hooked one. False if a hook
// is gone.
bool workset_check_hooks(ID3D11DeviceContext* imm);
// Residency moves to a device that replaced the first: hooks its immediate
// context too and makes it the checked one. False if the meter is off or a
// hook failed.
bool workset_adopt(ID3D11DeviceContext* imm);
// A game texture that is only sampled (no target binds, not shared). res is
// its M3 residency index + 1 (0 = not managed): binds of its views then
// call residency_on_use. Returns the texture's live-view counter, nullptr
// when it could not be tracked.
std::atomic<int>* workset_on_texture(ID3D11Texture2D* tex, uint64_t bytes, uint32_t res = 0);
// Any SRV; kept only when it views a tracked texture. A view of a managed
// texture that cannot be tracked pins the texture (residency_pin).
void workset_on_srv(ID3D11Resource* res, ID3D11ShaderResourceView* view);
// Lock-free lookups, any thread: residency index + 1 of a tracked texture
// or of the texture a tracked view shows, 0 otherwise.
uint32_t workset_texture_res(const void* tex);
uint32_t workset_view_res(const void* view);
// The texture is no longer managed (it may outlive our reference).
void workset_clear_res(const void* tex);
// From Present (outer, real frames).
void workset_frame();
// From the writer thread, about once a second: pushes a WORKSET record.
void workset_background_tick();
}  // namespace tessera
