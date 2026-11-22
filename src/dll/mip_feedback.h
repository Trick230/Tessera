// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <dxgi.h>

namespace tessera {
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
                                                        ID3D11ClassLinkage*, ID3D11PixelShader**);
void feedback_on_shader(ID3D11Device* dev, CreatePixelShaderFn create, ID3D11PixelShader* original,
                        const void* bytes, SIZE_T size);
void feedback_on_context(ID3D11DeviceContext* context);
void feedback_on_swapchain(IDXGISwapChain* swapchain);
void feedback_present(IDXGISwapChain* swapchain);
void feedback_after_present(IDXGISwapChain* swapchain);
void feedback_after_ps_bind(ID3D11DeviceContext* context);
void feedback_log_summary();
}  // namespace tessera
