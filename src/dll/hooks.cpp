// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 0 hooks: observe resource creation and Present, change nothing.
#include "hooks.h"

#include <windows.h>
#include <intrin.h>

#include "config.h"
#include "log.h"
#include "module.h"
#include "pacing.h"
#include "session.h"
#include "vtable.h"

namespace tessera {
namespace {
// Vtable slots (order of the interface declarations in the SDK headers).
constexpr int kDevCreateBuffer = 3;
constexpr int kDevCreateTexture2D = 5;
constexpr int kDevCreateSRV = 7;
constexpr int kFactoryCreateSwapChain = 10;
constexpr int kFactory2CreateSwapChainForHwnd = 15;
constexpr int kFactory2CreateSwapChainForCoreWindow = 16;
constexpr int kFactory2CreateSwapChainForComposition = 24;
constexpr int kSwapPresent = 8;
constexpr int kSwap1Present1 = 22;

// Depth of hooked calls on this thread: the runtime may call back through
// the vtable (Present -> Present1); only the outermost call is recorded.
thread_local int t_depth = 0;
struct Depth {
    Depth() { ++t_depth; }
    ~Depth() { --t_depth; }
    bool outer() const { return t_depth == 1; }
};

uint64_t now_qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*,
                                          ID3D11Buffer**);
HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                             const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
HRESULT STDMETHODCALLTYPE hk_CreateSRV(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                       ID3D11ShaderResourceView**);
HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                    IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForCoreWindow(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                          const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                          IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForComposition(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);
HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain*, UINT, UINT);
HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

VtHook g_create_buffer = {kDevCreateBuffer, reinterpret_cast<void*>(&hk_CreateBuffer)};
VtHook g_create_tex2d = {kDevCreateTexture2D, reinterpret_cast<void*>(&hk_CreateTexture2D)};
VtHook g_create_srv = {kDevCreateSRV, reinterpret_cast<void*>(&hk_CreateSRV)};
VtHook g_create_sc = {kFactoryCreateSwapChain, reinterpret_cast<void*>(&hk_CreateSwapChain)};
VtHook g_create_sc_hwnd = {kFactory2CreateSwapChainForHwnd, reinterpret_cast<void*>(&hk_CreateSwapChainForHwnd)};
VtHook g_create_sc_core = {kFactory2CreateSwapChainForCoreWindow,
                           reinterpret_cast<void*>(&hk_CreateSwapChainForCoreWindow)};
VtHook g_create_sc_comp = {kFactory2CreateSwapChainForComposition,
                           reinterpret_cast<void*>(&hk_CreateSwapChainForComposition)};
VtHook g_present = {kSwapPresent, reinterpret_cast<void*>(&hk_Present)};
VtHook g_present1 = {kSwap1Present1, reinterpret_cast<void*>(&hk_Present1)};

template <class F>
F orig(const VtHook& h, void* self) {
    return reinterpret_cast<F>(vt_orig(h, self));
}

constexpr UINT kSharedMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                              D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_GDI_COMPATIBLE;

// M2: render targets, depth, UAVs and GPU buffers of the game go to maximum
// eviction priority, so under VRAM pressure the OS pages out sampled
// textures first. Shared, staging and dynamic resources are left alone.
// Returns TESSERA_FLAG_PRIORITY when applied.
uint32_t raise_priority(ID3D11Resource* res, D3D11_USAGE usage, UINT misc, bool target) {
    if (!config().residency || !target || (misc & kSharedMisc)) return 0;
    if (usage != D3D11_USAGE_DEFAULT && usage != D3D11_USAGE_IMMUTABLE) return 0;
    res->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
    return TESSERA_FLAG_PRIORITY;
}

HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device* self, const D3D11_BUFFER_DESC* d,
                                          const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    using F = decltype(&hk_CreateBuffer);
    const HRESULT hr = orig<F>(g_create_buffer, self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out && d && depth.outer()) {
        TesseraRecord r = {};
        r.qpc = now_qpc();
        r.kind = TESSERA_REC_BUFFER;
        r.flags = d->BindFlags;
        if (!is_game_code(ret))
            r.flags |= TESSERA_FLAG_FOREIGN;
        else
            // Small buffers (constants, per-draw data) are many and suballocated
            // by the driver: skip them, the call is a kernel round trip.
            r.flags |= raise_priority(*out, d->Usage, d->MiscFlags, d->ByteWidth >= 64 * 1024);
        r.a = d->ByteWidth;
        r.d = static_cast<uint32_t>(d->Usage) | (d->MiscFlags << 8);
        r.bytes = d->ByteWidth;
        session_push(r);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* d,
                                             const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    using F = decltype(&hk_CreateTexture2D);
    const HRESULT hr = orig<F>(g_create_tex2d, self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out && d && depth.outer()) {
        TesseraRecord r = {};
        r.qpc = now_qpc();
        r.kind = TESSERA_REC_TEXTURE2D;
        r.format = static_cast<uint16_t>(d->Format);
        r.flags = d->BindFlags;
        constexpr UINT kTargetBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
        if (!is_game_code(ret))
            r.flags |= TESSERA_FLAG_FOREIGN;
        else
            r.flags |= raise_priority(*out, d->Usage, d->MiscFlags, (d->BindFlags & kTargetBinds) != 0);
        r.a = d->Width;
        r.b = d->Height;
        r.c = d->MipLevels | (d->ArraySize << 8);
        r.d = static_cast<uint32_t>(d->Usage) | (d->MiscFlags << 8);
        r.bytes = texture2d_bytes(*d);
        session_push(r);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSRV(ID3D11Device* self, ID3D11Resource* res,
                                       const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** out) {
    Depth depth;
    using F = decltype(&hk_CreateSRV);
    const HRESULT hr = orig<F>(g_create_srv, self)(self, res, d, out);
    if (SUCCEEDED(hr) && out && *out && depth.outer()) {
        TesseraRecord r = {};
        r.qpc = now_qpc();
        r.kind = TESSERA_REC_SRV;
        if (d) {
            r.format = static_cast<uint16_t>(d->Format);
            r.a = static_cast<uint32_t>(d->ViewDimension);
        }
        session_push(r);
    }
    return hr;
}

void record_swapchain(UINT w, UINT h, DXGI_FORMAT fmt, UINT buffers, DXGI_SWAP_EFFECT effect, UINT flags) {
    TesseraRecord r = {};
    r.qpc = now_qpc();
    r.kind = TESSERA_REC_SWAPCHAIN;
    r.format = static_cast<uint16_t>(fmt);
    r.flags = flags;
    r.a = w;
    r.b = h;
    r.c = buffers;
    r.d = static_cast<uint32_t>(effect);
    session_push(r);
    logf("swapchain %ux%u format %u buffers %u effect %u flags 0x%x", w, h, fmt, buffers, effect, flags);
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                             IDXGISwapChain** out) {
    Depth depth;
    using F = decltype(&hk_CreateSwapChain);
    const HRESULT hr = orig<F>(g_create_sc, self)(self, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (d && depth.outer())
            record_swapchain(d->BufferDesc.Width, d->BufferDesc.Height, d->BufferDesc.Format, d->BufferCount,
                             d->SwapEffect, d->Flags);
        on_swapchain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND wnd,
                                                    const DXGI_SWAP_CHAIN_DESC1* d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                    IDXGISwapChain1** out) {
    Depth depth;
    using F = decltype(&hk_CreateSwapChainForHwnd);
    const HRESULT hr = orig<F>(g_create_sc_hwnd, self)(self, dev, wnd, d, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (d && depth.outer())
            record_swapchain(d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags);
        on_swapchain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* dev, IUnknown* window,
                                                          const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* output,
                                                          IDXGISwapChain1** out) {
    Depth depth;
    using F = decltype(&hk_CreateSwapChainForCoreWindow);
    const HRESULT hr = orig<F>(g_create_sc_core, self)(self, dev, window, d, output, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (d && depth.outer())
            record_swapchain(d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags);
        on_swapchain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* dev,
                                                           const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* output,
                                                           IDXGISwapChain1** out) {
    Depth depth;
    using F = decltype(&hk_CreateSwapChainForComposition);
    const HRESULT hr = orig<F>(g_create_sc_comp, self)(self, dev, d, output, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (d && depth.outer())
            record_swapchain(d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags);
        on_swapchain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    Depth depth;
    if (depth.outer() && !(flags & DXGI_PRESENT_TEST)) session_frame(pace_before_present(self));
    using F = decltype(&hk_Present);
    return orig<F>(g_present, self)(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* params) {
    Depth depth;
    if (depth.outer() && !(flags & DXGI_PRESENT_TEST)) session_frame(pace_before_present(self));
    using F = decltype(&hk_Present1);
    return orig<F>(g_present1, self)(self, sync, flags, params);
}

void hook_factory(IDXGIFactory* f) {
    bool ok = vt_hook(g_create_sc, f);
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f2)))) {
        ok = vt_hook(g_create_sc_hwnd, f2) && ok;
        ok = vt_hook(g_create_sc_core, f2) && ok;
        ok = vt_hook(g_create_sc_comp, f2) && ok;
        f2->Release();
    }
    if (!ok) logf("factory hook failed");
}

bool is_block_compressed(DXGI_FORMAT f) {
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
           (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

uint32_t bits_per_pixel(DXGI_FORMAT f) {
    const int v = static_cast<int>(f);
    if (v >= 1 && v <= 4) return 128;
    if (v >= 5 && v <= 8) return 96;
    if (v >= 9 && v <= 22) return 64;
    if (v >= 23 && v <= 47) return 32;
    if (v >= 48 && v <= 59) return 16;
    if (v >= 60 && v <= 65) return 8;
    if (v == DXGI_FORMAT_R1_UNORM) return 1;
    if (v == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) return 32;
    if (v == DXGI_FORMAT_BC1_TYPELESS || v == DXGI_FORMAT_BC1_UNORM || v == DXGI_FORMAT_BC1_UNORM_SRGB ||
        (v >= DXGI_FORMAT_BC4_TYPELESS && v <= DXGI_FORMAT_BC4_SNORM))
        return 4;
    if (is_block_compressed(f)) return 8;
    if (v >= DXGI_FORMAT_B8G8R8A8_UNORM && v <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return 32;
    return 16;  // packed 16-bit and video formats
}
}  // namespace

uint64_t texture2d_bytes(const D3D11_TEXTURE2D_DESC& d) {
    uint32_t mips = d.MipLevels;
    if (mips == 0) {
        uint32_t m = d.Width > d.Height ? d.Width : d.Height;
        while (m) {
            ++mips;
            m >>= 1;
        }
    }
    const bool bc = is_block_compressed(d.Format);
    const uint64_t bpp = bits_per_pixel(d.Format);
    uint64_t total = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        uint64_t w = d.Width >> i, h = d.Height >> i;
        if (w == 0) w = 1;
        if (h == 0) h = 1;
        if (bc)
            total += ((w + 3) / 4) * ((h + 3) / 4) * bpp * 2;  // 16 texels per block
        else
            total += (w * h * bpp + 7) / 8;
    }
    const uint64_t samples = d.SampleDesc.Count ? d.SampleDesc.Count : 1;
    return total * (d.ArraySize ? d.ArraySize : 1) * samples;
}

void on_swapchain(IDXGISwapChain* sc) {
    bool ok = vt_hook(g_present, sc);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        ok = vt_hook(g_present1, sc1) && ok;
        sc1->Release();
    }
    if (!ok) logf("present hook failed");
    pacing_on_swapchain(sc);
}

void on_device(ID3D11Device* dev, UINT create_flags) {
    if (!session_init()) return;
    TesseraRecord r = {};
    r.qpc = now_qpc();
    r.kind = TESSERA_REC_DEVICE;
    r.a = static_cast<uint32_t>(dev->GetFeatureLevel());
    r.d = create_flags;
    session_push(r);

    const bool ok = vt_hook(g_create_buffer, dev) && vt_hook(g_create_tex2d, dev) && vt_hook(g_create_srv, dev);
    logf("device fl 0x%x flags 0x%x, device hooks %s", r.a, create_flags, ok ? "ok" : "FAILED");

    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                hook_factory(factory);
                factory->Release();
            }
            adapter->Release();
        }
        dxgi->Release();
    }
}
}  // namespace tessera
