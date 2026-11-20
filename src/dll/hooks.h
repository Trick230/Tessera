// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace tessera {
// Called after the real D3D11CreateDevice* succeeded.
void on_device(ID3D11Device* dev, UINT create_flags);
void on_swapchain(IDXGISwapChain* sc);

// Estimated VRAM footprint of a 2D texture (all mips, slices and samples).
uint64_t texture2d_bytes(const D3D11_TEXTURE2D_DESC& d);
}  // namespace tessera
