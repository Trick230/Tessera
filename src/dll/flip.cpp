// SPDX-License-Identifier: GPL-3.0-or-later
#include "flip.h"

#include <windows.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>

#include <atomic>

#include "config.h"
#include "hooks.h"
#include "log.h"

namespace tessera {
namespace {
// Private data on converted swapchains: the entry itself, as an IUnknown the
// swapchain holds. Its last Release (swapchain destroyed) frees the slot, so a
// slot is only reused once nothing can present through it.
// {6c1f4a52-9e3b-4d7a-b1c2-5e8f0a3d7b14}
constexpr GUID kFlipGuid = {0x6c1f4a52, 0x9e3b, 0x4d7a, {0xb1, 0xc2, 0x5e, 0x8f, 0x0a, 0x3d, 0x7b, 0x14}};

struct Entry : IUnknown {
    std::atomic<IDXGISwapChain*> sc{nullptr};
    std::atomic<long> refs{0};
    std::atomic<unsigned> resizes{0};    // ResizeBuffers count: render targets saved before are stale
    ID3D11DeviceContext* ctx = nullptr;  // not owned: the swapchain keeps its device alive
    bool tearing = false;
    HANDLE latency = nullptr;            // P3: frame latency waitable object, owned

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id != __uuidof(IUnknown)) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *out = static_cast<IUnknown*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = --refs;
        if (n == 0) {
            if (latency) CloseHandle(latency);
            latency = nullptr;
            sc.store(nullptr, std::memory_order_release);
        }
        return static_cast<ULONG>(n);
    }
};
// Lock-free lookup for Present; slots are claimed under g_lock. A game with
// more live converted swapchains than slots keeps the rest as they are.
constexpr int kSlots = 32;
Entry g_slots[kSlots];
SRWLOCK g_lock = SRWLOCK_INIT;

INIT_ONCE g_tearing_once = INIT_ONCE_STATIC_INIT;
bool g_tearing = false;

BOOL CALLBACK init_tearing(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    using F = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dxgi ? reinterpret_cast<F>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory5* f5 = nullptr;
    if (create && SUCCEEDED(create(IID_PPV_ARGS(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            g_tearing = allow != FALSE;
        f5->Release();
    }
    logf("flip: tearing %s", g_tearing ? "supported" : "not supported");
    return TRUE;
}

bool tearing_supported() {
    InitOnceExecuteOnce(&g_tearing_once, init_tearing, nullptr, nullptr);
    return g_tearing;
}

// Formats flip model accepts for back buffers (no sRGB, no MSAA).
bool flip_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool d3d11_device(IUnknown* dev) {
    ID3D11Device* d = nullptr;
    if (!dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&d)))) return false;  // D3D12 queues are already flip
    d->Release();
    return true;
}

bool eligible(IUnknown* dev, bool game, HWND wnd, bool windowed, DXGI_SWAP_EFFECT effect, DXGI_FORMAT fmt,
              UINT samples, UINT flags) {
    if (!config().flip_model || !game || !windowed || samples != 1 || !flip_format(fmt)) return false;
    if (effect != DXGI_SWAP_EFFECT_DISCARD && effect != DXGI_SWAP_EFFECT_SEQUENTIAL) return false;
    if (flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) return false;
    if (!wnd || !IsWindow(wnd) || (GetWindowLongW(wnd, GWL_STYLE) & WS_CHILD)) return false;
    if (GetWindowLongW(wnd, GWL_EXSTYLE) & WS_EX_LAYERED) return false;
    if (!flip_hooks_ready()) return false;  // without Present/ResizeBuffers hooks flip would break the game
    return !dev || d3d11_device(dev);       // null: D3D11CreateDeviceAndSwapChain
}

// Flip model keeps a back buffer per BufferCount: a SEQUENTIAL game with one
// buffer may redraw only what changed on top of the last frame, which would
// alternate between two buffers. Only DISCARD gets an extra buffer.
bool buffers_ok(DXGI_SWAP_EFFECT effect, UINT buffers) {
    return effect != DXGI_SWAP_EFFECT_SEQUENTIAL || buffers >= 2;
}

bool slot_free(Entry& e) { return e.sc.load(std::memory_order_acquire) == nullptr && e.refs.load() == 0; }

bool any_slot_free() {
    for (Entry& e : g_slots)
        if (slot_free(e)) return true;
    return false;
}

DXGI_SWAP_EFFECT to_flip(DXGI_SWAP_EFFECT e) {
    return e == DXGI_SWAP_EFFECT_SEQUENTIAL ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD;
}

Entry* find(IDXGISwapChain* sc) {
    for (Entry& e : g_slots)
        if (e.sc.load(std::memory_order_acquire) == sc) return &e;
    return nullptr;
}
}  // namespace

bool flip_convert(IUnknown* dev, bool game, const DXGI_SWAP_CHAIN_DESC& in, DXGI_SWAP_CHAIN_DESC* out,
                  bool* tearing) {
    if (!eligible(dev, game, in.OutputWindow, in.Windowed != FALSE, in.SwapEffect, in.BufferDesc.Format,
                  in.SampleDesc.Count, in.Flags) ||
        !buffers_ok(in.SwapEffect, in.BufferCount) || !any_slot_free())
        return false;
    *out = in;
    out->SwapEffect = to_flip(in.SwapEffect);
    if (out->BufferCount < 2) out->BufferCount = 2;
    *tearing = tearing_supported();
    if (*tearing) out->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (config().max_frame_latency > 0) out->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return true;
}

bool flip_convert1(IUnknown* dev, bool game, HWND wnd, const DXGI_SWAP_CHAIN_DESC1& in,
                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, DXGI_SWAP_CHAIN_DESC1* out, bool* tearing) {
    if (in.Stereo || in.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH) return false;
    if (!eligible(dev, game, wnd, !fs || fs->Windowed, in.SwapEffect, in.Format, in.SampleDesc.Count, in.Flags) ||
        !buffers_ok(in.SwapEffect, in.BufferCount) || !any_slot_free())
        return false;
    *out = in;
    out->SwapEffect = to_flip(in.SwapEffect);
    if (out->BufferCount < 2) out->BufferCount = 2;
    *tearing = tearing_supported();
    if (*tearing) out->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (config().max_frame_latency > 0) out->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    return true;
}

void flip_track(IDXGISwapChain* sc, IUnknown* dev, bool converted, bool tearing) {
    if (!converted) return;
    ID3D11Device* d = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d)))) {
        d->GetImmediateContext(&ctx);
        d->Release();
        if (ctx) ctx->Release();  // kept raw: lives as long as the device
    }
    AcquireSRWLockExclusive(&g_lock);
    Entry* e = nullptr;
    for (Entry& s : g_slots)
        if (slot_free(s)) {
            e = &s;
            break;
        }
    bool ok = false;
    if (e) {
        e->ctx = ctx;
        e->tearing = tearing;
        e->resizes.store(0);
        e->sc.store(sc, std::memory_order_release);
        ok = SUCCEEDED(sc->SetPrivateDataInterface(kFlipGuid, e));
        if (!ok) e->sc.store(nullptr, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lock);
    // P3: the layer asked for a waitable swap chain; Present waits on it after
    // the real Present, where the game's next frame starts.
    const int lat = config().max_frame_latency;
    IDXGISwapChain2* sc2 = nullptr;
    DXGI_SWAP_CHAIN_DESC desc;
    if (ok && lat > 0 && SUCCEEDED(sc->GetDesc(&desc)) && (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
        SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc2)))) {
        const bool set = SUCCEEDED(sc2->SetMaximumFrameLatency(static_cast<UINT>(lat)));
        e->latency = set ? sc2->GetFrameLatencyWaitableObject() : nullptr;
        sc2->Release();
        logf("flip: max frame latency %d through the waitable object: %s", lat, e->latency ? "ok" : "failed");
    }
    if (ok)
        logf("flip: swapchain converted to flip model%s", tearing ? " with tearing" : "");
    else  // another thread took the last slot between convert and track
        logf("flip: cannot track a converted swapchain; its resizes may fail");
}

UINT flip_before_present(IDXGISwapChain* sc, UINT sync, UINT flags, FlipFrame& f) {
    Entry* e = find(sc);
    if (!e) return flags;
    if (flags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
        // "Show the last frame again" (WM_PAINT): flip model has no such
        // present, and DWM keeps showing the last frame anyway.
        f.skip = true;
        return flags;
    }
    if (e->tearing && sync == 0 && !(flags & DXGI_PRESENT_TEST)) {
        BOOL full = FALSE;
        if (SUCCEEDED(sc->GetFullscreenState(&full, nullptr)) && !full) flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    if (!(flags & DXGI_PRESENT_TEST)) f.latency = e->latency;
    if (e->ctx && !(flags & DXGI_PRESENT_TEST)) {
        f.ctx = e->ctx;
        f.entry = e;
        f.resizes = e->resizes.load(std::memory_order_acquire);
        f.ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, f.rtv, &f.dsv);
    }
    return flags;
}

void flip_after_present(FlipFrame& f) {
    // Bounded: a frame held up past this is a stall the wait must not add to.
    if (f.latency) WaitForSingleObjectEx(f.latency, 100, FALSE);
    if (!f.ctx) return;
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (f.rtv[i]) n = i + 1;
    // A resize inside Present (fullscreen exit) made the saved views stale.
    const bool stale = static_cast<Entry*>(f.entry)->resizes.load(std::memory_order_acquire) != f.resizes;
    // Keep the UAVs: RTV and UAV slots overlap, OMSetRenderTargets would unbind them.
    if ((n || f.dsv) && !stale)
        f.ctx->OMSetRenderTargetsAndUnorderedAccessViews(n, f.rtv, f.dsv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, 0,
                                                         nullptr, nullptr);
    for (ID3D11RenderTargetView*& v : f.rtv)
        if (v) v->Release();
    if (f.dsv) f.dsv->Release();
}

void flip_resize(IDXGISwapChain* sc, UINT* buffers, UINT* flags) {
    Entry* e = find(sc);
    if (!e) return;
    e->resizes.fetch_add(1, std::memory_order_acq_rel);
    if (*buffers == 1) *buffers = 2;
    if (e->tearing) *flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (e->latency) *flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;  // must match creation
}
}  // namespace tessera
