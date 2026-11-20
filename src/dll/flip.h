// SPDX-License-Identifier: GPL-3.0-or-later
// P2: windowed blit swapchains of the game become flip model (+ tearing when
// the game presents without vsync), so DWM stops copying every frame and VRR
// works in borderless windows. Off unless flip_model=1 in tessera.ini.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>

namespace tessera {
// Bit in `d` of SWAPCHAIN records: Tessera converted it to flip model.
constexpr unsigned kSwapConverted = 0x100;

// Fills *out with the flip-model version of a legacy desc and returns true
// when the swapchain qualifies (game code, D3D11 device, windowed, no MSAA,
// supported format, top-level window). *tearing tells if ALLOW_TEARING was added.
// dev may be null when the caller knows it is a D3D11 device.
bool flip_convert(IUnknown* dev, bool game, const DXGI_SWAP_CHAIN_DESC& in, DXGI_SWAP_CHAIN_DESC* out,
                  bool* tearing);
bool flip_convert1(IUnknown* dev, bool game, HWND wnd, const DXGI_SWAP_CHAIN_DESC1& in,
                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, DXGI_SWAP_CHAIN_DESC1* out, bool* tearing);

// After a successful creation: remembers a converted swapchain until it is
// destroyed (private data). Does nothing for the others.
void flip_track(IDXGISwapChain* sc, IUnknown* dev, bool converted, bool tearing);

// Present of a converted swapchain: flags to pass on, and the game's render
// targets, which flip model unbinds and the game may expect still bound.
struct FlipFrame {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    void* entry = nullptr;
    unsigned resizes = 0;
    bool skip = false;  // DO_NOT_SEQUENCE on a converted swapchain: return S_OK, present nothing
};
UINT flip_before_present(IDXGISwapChain* sc, UINT sync, UINT flags, FlipFrame& f);
void flip_after_present(FlipFrame& f);

// ResizeBuffers of a converted swapchain keeps the creation flags and >= 2 buffers.
void flip_resize(IDXGISwapChain* sc, UINT* buffers, UINT* flags);
}  // namespace tessera
