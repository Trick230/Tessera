// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace tessera {
// Called after the real D3D11CreateDevice* succeeded.
void on_device(ID3D11Device* dev, UINT create_flags);
void on_swapchain(IDXGISwapChain* sc);

// P2: true while the swapchain hooks flip model relies on can still be installed.
bool flip_hooks_ready();

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
