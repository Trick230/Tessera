// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_3.h>
#include <cstdint>

#include "residency.h"
#include "tessera/quality_format.h"

namespace tessera {
using QualityEntry = TesseraQualityHeader;
void quality_init();
void quality_capture(const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* pixels);
void quality_background_tick();
bool quality_replace(ID3D11Device* dev, CreateTex2DFn create, const D3D11_TEXTURE2D_DESC& desc,
                     const D3D11_SUBRESOURCE_DATA* pixels, ID3D11Texture2D** out, uint64_t* bytes);
const QualityEntry* quality_entry(ID3D11Resource* resource);
ID3D11Texture2D* quality_copy_source(ID3D11Resource* source);  // caller releases
void quality_on_view(const QualityEntry* entry, ID3D11ShaderResourceView* view);
void quality_register_device(ID3D11Device* dev);
bool quality_device_hooks(ID3D11Device* dev);
bool quality_context_hooks(ID3D11DeviceContext* ctx, bool deferred);
void quality_after_present(IDXGISwapChain* sc);
bool quality_texture_hooks(ID3D11Texture2D* tex);
bool quality_has_replacements();
struct NativeQuality {
    explicit NativeQuality(bool enable);
    ~NativeQuality();
    bool enabled;
};
}  // namespace tessera
