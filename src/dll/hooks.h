// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace tessera {

// Flush is shared by the diagnostics and residency hooks.
bool hook_context_flush(ID3D11DeviceContext* ctx, bool readopt = false);
void rehook_context_flush(ID3D11DeviceContext* ctx);
// Called after the real D3D11CreateDevice* succeeded.
void on_device(ID3D11Device* dev, UINT create_flags);
void on_swapchain(IDXGISwapChain* sc);
// dxgi.dll proxy: a factory the game created (after session_init succeeded).
void on_dxgi_factory(IUnknown* factory);

// P2: true while the swapchain hooks flip model relies on can still be installed.
bool flip_hooks_ready();
DWORD last_present_thread();

// A call the layer makes itself (proxy exports): hooked calls inside it pass straight through.
struct InternalCall {
    InternalCall();
    ~InternalCall();
};
// D3D11CreateDeviceAndSwapChain: d is the desc actually used (flip-converted or the game's).
void on_legacy_swapchain(IDXGISwapChain* sc, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC* d, bool converted,
                         bool tearing);

// Estimated VRAM footprint of a 2D texture (all mips, slices and samples).
uint64_t texture2d_bytes(const D3D11_TEXTURE2D_DESC& d);
}  // namespace tessera
