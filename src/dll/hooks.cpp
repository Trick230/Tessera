// SPDX-License-Identifier: GPL-3.0-or-later
// Device, factory and swapchain hooks: resource and swapchain creation, Present.
#include "hooks.h"

#include <windows.h>
#include <d3d11_3.h>
#include <intrin.h>

#include "config.h"
#include "dedup.h"
#include "flip.h"
#include "log.h"
#include "module.h"
#include "memory.h"
#include "shaders.h"
#include "pacing.h"
#include "residency.h"
#include "rtpool.h"
#include "uecvars.h"
#include "session.h"
#include "vtable.h"
#include "workset.h"

namespace tessera {
namespace {
// Vtable slots (order of the interface declarations in the SDK headers).
constexpr int kDevCreateBuffer = 3;
constexpr int kDevCreateTexture2D = 5;
constexpr int kDevCreateSRV = 7;
constexpr int kDevCreateDeferredContext = 27;
constexpr int kDev1CreateDeferredContext1 = 44;
constexpr int kDev2CreateDeferredContext2 = 51;
constexpr int kDev3CreateDeferredContext3 = 62;
constexpr int kFactoryCreateSwapChain = 10;
constexpr int kFactory2CreateSwapChainForHwnd = 15;
constexpr int kFactory2CreateSwapChainForCoreWindow = 16;
constexpr int kFactory2CreateSwapChainForComposition = 24;
constexpr int kSwapPresent = 8;
constexpr int kSwapResizeBuffers = 13;
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

std::atomic<DWORD> g_present_tid{0};  // last thread that presented

// After the record r of a game create that began at t0: a SLOW_CREATE record
// when the call took >= 1 ms (where the driver stalls, and on which thread).
void push_slow_create(const TesseraRecord& r, uint64_t t0) {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    const uint64_t us = (r.qpc - t0) * 1000000 / freq;
    if (us < 1000) return;
    TesseraRecord s = {};
    s.qpc = r.qpc;
    s.kind = TESSERA_REC_SLOW_CREATE;
    s.format = r.format;
    s.a = static_cast<uint32_t>(us);
    s.b = GetCurrentThreadId();
    s.c = r.kind;
    s.d = s.b == g_present_tid.load(std::memory_order_relaxed);
    s.bytes = r.bytes;
    session_push(s);
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
HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

VtHook g_create_buffer = {kDevCreateBuffer, reinterpret_cast<void*>(&hk_CreateBuffer)};
VtHook g_create_tex2d = {kDevCreateTexture2D, reinterpret_cast<void*>(&hk_CreateTexture2D)};
VtHook g_create_srv = {kDevCreateSRV, reinterpret_cast<void*>(&hk_CreateSRV)};
HRESULT STDMETHODCALLTYPE hk_CreateDeferred(ID3D11Device*, UINT, ID3D11DeviceContext**);
HRESULT STDMETHODCALLTYPE hk_CreateDeferred1(ID3D11Device1*, UINT, ID3D11DeviceContext1**);
HRESULT STDMETHODCALLTYPE hk_CreateDeferred2(ID3D11Device2*, UINT, ID3D11DeviceContext2**);
HRESULT STDMETHODCALLTYPE hk_CreateDeferred3(ID3D11Device3*, UINT, ID3D11DeviceContext3**);
VtHook g_create_def = {kDevCreateDeferredContext, reinterpret_cast<void*>(&hk_CreateDeferred)};
VtHook g_create_def1 = {kDev1CreateDeferredContext1, reinterpret_cast<void*>(&hk_CreateDeferred1)};
VtHook g_create_def2 = {kDev2CreateDeferredContext2, reinterpret_cast<void*>(&hk_CreateDeferred2)};
VtHook g_create_def3 = {kDev3CreateDeferredContext3, reinterpret_cast<void*>(&hk_CreateDeferred3)};
VtHook g_create_sc = {kFactoryCreateSwapChain, reinterpret_cast<void*>(&hk_CreateSwapChain)};
VtHook g_create_sc_hwnd = {kFactory2CreateSwapChainForHwnd, reinterpret_cast<void*>(&hk_CreateSwapChainForHwnd)};
VtHook g_create_sc_core = {kFactory2CreateSwapChainForCoreWindow,
                           reinterpret_cast<void*>(&hk_CreateSwapChainForCoreWindow)};
VtHook g_create_sc_comp = {kFactory2CreateSwapChainForComposition,
                           reinterpret_cast<void*>(&hk_CreateSwapChainForComposition)};
VtHook g_present = {kSwapPresent, reinterpret_cast<void*>(&hk_Present)};
VtHook g_present1 = {kSwap1Present1, reinterpret_cast<void*>(&hk_Present1)};
VtHook g_resize = {kSwapResizeBuffers, reinterpret_cast<void*>(&hk_ResizeBuffers)};

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
    const uint64_t t0 = now_qpc();
    using F = decltype(&hk_CreateBuffer);
    const HRESULT hr = orig<F>(g_create_buffer, self)(self, d, init, out);
    if (SUCCEEDED(hr) && d && (d->MiscFlags & D3D11_RESOURCE_MISC_TILED) && depth.outer()) residency_after_present();
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
        if (init) r.flags |= TESSERA_FLAG_INIT_DATA;
        session_push(r);
        if (!(r.flags & TESSERA_FLAG_FOREIGN)) push_slow_create(r, t0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* d,
                                             const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    using F = decltype(&hk_CreateTexture2D);
    const F create = orig<F>(g_create_tex2d, self);
    const bool outer = depth.outer();
    const bool game = outer && is_game_code(ret);
    const uint64_t bytes = d ? texture2d_bytes(*d) : 0;
    const uint64_t t0 = now_qpc();
    // M3: large immutable textures become tiled, their top mip movable.
    // R1: a released render target with this description comes back.
    const bool reused = game && d && out && !init && rtpool_take(self, *d, bytes, out);
    const bool can_share = config().texture_dedup && game && d && init && out;
    bool shared = false;
    if (can_share) {
        if (auto* match = dedup_texture(self, *d, init, bytes)) {
            *out = match;
            shared = true;
        }
    }
    const bool tiled = !reused && !shared && game && d && out && residency_create(self, create, *d, init, out, bytes);
    const HRESULT hr = reused || shared || tiled ? S_OK : create(self, d, init, out);
    if (can_share && !shared && !tiled && SUCCEEDED(hr) && *out) {
        if (auto* match = dedup_texture(self, *d, init, bytes, *out)) {
            (*out)->Release();
            *out = match;
            shared = true;
        }
    }
    if (SUCCEEDED(hr) && !reused && game && !init && d && out && *out) rtpool_track(self, *out, *d, bytes);
    if (SUCCEEDED(hr) && !tiled && d && (d->MiscFlags & D3D11_RESOURCE_MISC_TILED)) residency_after_present();
    if (SUCCEEDED(hr) && out && *out && d && outer && !shared) {
        TesseraRecord r = {};
        r.qpc = now_qpc();
        r.kind = TESSERA_REC_TEXTURE2D;
        r.format = static_cast<uint16_t>(d->Format);
        r.flags = d->BindFlags;
        constexpr UINT kTargetBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
        if (!game)
            r.flags |= TESSERA_FLAG_FOREIGN;
        else
            r.flags |= raise_priority(*out, d->Usage, d->MiscFlags, (d->BindFlags & kTargetBinds) != 0);
        if (tiled) r.flags |= TESSERA_FLAG_TILED;
        if (reused) r.flags |= TESSERA_FLAG_RECYCLED;
        r.bytes = bytes;
        // M11 tracks what M4 could manage: game textures that are only sampled
        // (managed ones were registered by residency_create).
        if (game && !tiled && (d->BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
            !(d->BindFlags & kTargetBinds) && !(d->MiscFlags & kSharedMisc) &&
            (d->Usage == D3D11_USAGE_DEFAULT || d->Usage == D3D11_USAGE_IMMUTABLE))
            workset_on_texture(*out, r.bytes);
        r.a = d->Width;
        r.b = d->Height;
        r.c = d->MipLevels | (d->ArraySize << 8);
        r.d = static_cast<uint32_t>(d->Usage) | (d->MiscFlags << 8);
        if (init) r.flags |= TESSERA_FLAG_INIT_DATA;
        session_push(r);
        if (game) push_slow_create(r, t0);
    }
    return hr;
}

// Each deferred context has a vtable of its own: hooked before the game gets it.
void on_deferred(ID3D11DeviceContext* ctx) {
    // R1: command lists hold the textures they use, unseen.
    rtpool_stop("the process records command lists");
    vt_track(ctx);
    workset_on_context(ctx);
    residency_on_context(ctx);
}

HRESULT STDMETHODCALLTYPE hk_CreateDeferred(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out) {
    using F = decltype(&hk_CreateDeferred);
    const HRESULT hr = orig<F>(g_create_def, self)(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) on_deferred(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateDeferred1(ID3D11Device1* self, UINT flags, ID3D11DeviceContext1** out) {
    using F = decltype(&hk_CreateDeferred1);
    const HRESULT hr = orig<F>(g_create_def1, self)(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) on_deferred(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateDeferred2(ID3D11Device2* self, UINT flags, ID3D11DeviceContext2** out) {
    using F = decltype(&hk_CreateDeferred2);
    const HRESULT hr = orig<F>(g_create_def2, self)(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) on_deferred(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_CreateDeferred3(ID3D11Device3* self, UINT flags, ID3D11DeviceContext3** out) {
    using F = decltype(&hk_CreateDeferred3);
    const HRESULT hr = orig<F>(g_create_def3, self)(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) on_deferred(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSRV(ID3D11Device* self, ID3D11Resource* res,
                                       const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    using F = decltype(&hk_CreateSRV);
    const HRESULT hr = orig<F>(g_create_srv, self)(self, res, d, out);
    if (SUCCEEDED(hr) && out && *out && res) rtpool_on_view(res, *out);
    if (SUCCEEDED(hr) && out && *out && depth.outer()) {
        TesseraRecord r = {};
        r.qpc = now_qpc();
        r.kind = TESSERA_REC_SRV;
        if (d) {
            r.format = static_cast<uint16_t>(d->Format);
            r.a = static_cast<uint32_t>(d->ViewDimension);
        }
        session_push(r);
        // Views made by overlays and SDKs are theirs: no private data on them.
        if (is_game_code(ret))
            workset_on_srv(res, *out);
        else
            residency_pin_resource(res);  // its binds go unseen
    }
    return hr;
}

void record_swapchain(UINT w, UINT h, DXGI_FORMAT fmt, UINT buffers, DXGI_SWAP_EFFECT effect, UINT flags,
                      bool converted) {
    TesseraRecord r = {};
    r.qpc = now_qpc();
    r.kind = TESSERA_REC_SWAPCHAIN;
    r.format = static_cast<uint16_t>(fmt);
    r.flags = flags;
    r.a = w;
    r.b = h;
    r.c = buffers;
    r.d = static_cast<uint32_t>(effect) | (converted ? kSwapConverted : 0);
    session_push(r);
    logf("swapchain %ux%u format %u buffers %u effect %u flags 0x%x%s", w, h, fmt, buffers, effect, flags,
         converted ? " (flip model by Tessera)" : "");
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                             IDXGISwapChain** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    using F = decltype(&hk_CreateSwapChain);
    DXGI_SWAP_CHAIN_DESC flip;
    bool tearing = false;
    bool conv = d && depth.outer() && flip_convert(dev, is_game_code(ret), *d, &flip, &tearing);
    HRESULT hr = conv ? orig<F>(g_create_sc, self)(self, dev, &flip, out) : E_FAIL;
    if (conv && FAILED(hr)) {
        logf("flip: CreateSwapChain failed 0x%x, keeping the original swapchain", hr);
        conv = false;
    }
    if (!conv) hr = orig<F>(g_create_sc, self)(self, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        flip_track(*out, dev, conv, tearing);
        const DXGI_SWAP_CHAIN_DESC* u = conv ? &flip : d;
        if (u && depth.outer())
            record_swapchain(u->BufferDesc.Width, u->BufferDesc.Height, u->BufferDesc.Format, u->BufferCount,
                             u->SwapEffect, u->Flags, conv);
        on_swapchain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND wnd,
                                                    const DXGI_SWAP_CHAIN_DESC1* d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                    IDXGISwapChain1** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    using F = decltype(&hk_CreateSwapChainForHwnd);
    DXGI_SWAP_CHAIN_DESC1 flip;
    bool tearing = false;
    bool conv = d && depth.outer() && flip_convert1(dev, is_game_code(ret), wnd, *d, fs, &flip, &tearing);
    HRESULT hr = conv ? orig<F>(g_create_sc_hwnd, self)(self, dev, wnd, &flip, fs, output, out) : E_FAIL;
    if (conv && FAILED(hr)) {
        logf("flip: CreateSwapChainForHwnd failed 0x%x, keeping the original swapchain", hr);
        conv = false;
    }
    if (!conv) hr = orig<F>(g_create_sc_hwnd, self)(self, dev, wnd, d, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) {
        flip_track(*out, dev, conv, tearing);
        const DXGI_SWAP_CHAIN_DESC1* u = conv ? &flip : d;
        if (u && depth.outer())
            record_swapchain(u->Width, u->Height, u->Format, u->BufferCount, u->SwapEffect, u->Flags, conv);
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
        flip_track(*out, dev, false, false);
        if (d && depth.outer())
            record_swapchain(d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags, false);
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
        flip_track(*out, dev, false, false);
        if (d && depth.outer())
            record_swapchain(d->Width, d->Height, d->Format, d->BufferCount, d->SwapEffect, d->Flags, false);
        on_swapchain(*out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    Depth depth;
    FlipFrame ff;
    bool managed = false;
    if (depth.outer()) {
        if (!(flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE))) {
            g_present_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
            managed = residency_present(self);
            session_frame(pace_before_present(self));
            rtpool_frame(self);
            uecvars_frame();
        }
        flags = flip_before_present(self, sync, flags, ff);
        if (ff.skip) return S_OK;
    }
    using F = decltype(&hk_Present);
    const HRESULT hr = orig<F>(g_present, self)(self, sync, flags);
    flip_after_present(ff);
    if (managed) residency_after_present();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* params) {
    Depth depth;
    FlipFrame ff;
    bool managed = false;
    if (depth.outer()) {
        if (!(flags & (DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE))) {
            g_present_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
            managed = residency_present(self);
            session_frame(pace_before_present(self));
            rtpool_frame(self);
            uecvars_frame();
        }
        flags = flip_before_present(self, sync, flags, ff);
        if (ff.skip) return S_OK;
    }
    using F = decltype(&hk_Present1);
    const HRESULT hr = orig<F>(g_present1, self)(self, sync, flags, params);
    flip_after_present(ff);
    if (managed) residency_after_present();
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* self, UINT buffers, UINT w, UINT h, DXGI_FORMAT fmt,
                                           UINT flags) {
    Depth depth;
    flip_resize(self, &buffers, &flags);  // also nested: DXGI resizes inside Present on fullscreen exit
    using F = decltype(&hk_ResizeBuffers);
    const HRESULT hr = orig<F>(g_resize, self)(self, buffers, w, h, fmt, flags);
    if (SUCCEEDED(hr) && depth.outer()) rtpool_resize();
    return hr;
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

DWORD last_present_thread() { return g_present_tid.load(std::memory_order_relaxed); }

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

bool flip_hooks_ready() {
    constexpr long kMax = sizeof(g_present.vt) / sizeof(g_present.vt[0]);
    return g_present.n < kMax && g_present1.n < kMax && g_resize.n < kMax;
}

InternalCall::InternalCall() { ++t_depth; }
InternalCall::~InternalCall() { --t_depth; }


void on_swapchain(IDXGISwapChain* sc) {
    {
        // Making it rewrote copy methods of its device's immediate context.
        // Inside D3D11CreateDeviceAndSwapChain that device is not hooked yet.
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev)))) {
            ID3D11DeviceContext* imm = nullptr;
            dev->GetImmediateContext(&imm);
            residency_check_hooks(imm);
            if (imm) imm->Release();
            dev->Release();
        }
    }
    bool ok = vt_hook(g_present, sc) && vt_hook(g_resize, sc);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        ok = vt_hook(g_present1, sc1) && ok;
        sc1->Release();
    }
    if (!ok) logf("present hook failed");
    pacing_on_swapchain(sc);
}

void on_legacy_swapchain(IDXGISwapChain* sc, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC* d, bool converted,
                         bool tearing) {
    flip_track(sc, dev, converted, tearing);
    if (d)
        record_swapchain(d->BufferDesc.Width, d->BufferDesc.Height, d->BufferDesc.Format, d->BufferCount,
                         d->SwapEffect, d->Flags, converted);
    on_swapchain(sc);
}

void on_device(ID3D11Device* dev, UINT create_flags) {
    if (!session_init()) return;
    TesseraRecord r = {};
    r.qpc = now_qpc();
    r.kind = TESSERA_REC_DEVICE;
    r.a = static_cast<uint32_t>(dev->GetFeatureLevel());
    // C4: what later modules may rely on; each checks this and stays off without it.
    D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1)))
        r.b = static_cast<uint32_t>(o1.TiledResourcesTier);
    r.d = create_flags;
    session_push(r);

    const bool ok = vt_hook(g_create_buffer, dev) && vt_hook(g_create_tex2d, dev) && vt_hook(g_create_srv, dev);
    logf("device fl 0x%x flags 0x%x tiled tier %u, device hooks %s", r.a, create_flags, r.b, ok ? "ok" : "FAILED");
    memory_on_device(dev);
    workset_on_device(dev);
    rtpool_on_device(dev, ok);
    residency_on_device(dev);
    // After the modules: a context made from here on gets their hooks.
    bool def_ok = vt_hook(g_create_def, dev);
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d1)))) {
        def_ok = vt_hook(g_create_def1, d1) && def_ok;
        d1->Release();
    }
    ID3D11Device2* d2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d2)))) {
        def_ok = vt_hook(g_create_def2, d2) && def_ok;
        d2->Release();
    }
    ID3D11Device3* d3 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d3)))) {
        def_ok = vt_hook(g_create_def3, d3) && def_ok;
        d3->Release();
    }
    if (!def_ok) {
        logf("deferred context hooks FAILED");
        rtpool_stop("deferred context hooks FAILED");
        residency_on_context(nullptr);
    }
    shaders_on_device(dev);
    uecvars_on_device();

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
