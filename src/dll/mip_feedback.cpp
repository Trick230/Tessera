// SPDX-License-Identifier: GPL-3.0-or-later
#include "mip_feedback.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

#include "config.h"
#include "log.h"
#include "session.h"
#include "tessera/feedback_format.h"
#include "vtable.h"
#include "workset.h"

namespace tessera {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID kVariant = {0xbecdb178, 0x6c47, 0x4e77, {0xa8, 0x99, 0x67, 0xc0, 0xfb, 0xe1, 0x5d, 0x01}};
constexpr GUID kSlots = {0x58a4d0c0, 0xe00b, 0x49ca, {0x83, 0x3c, 0xec, 0xcb, 0xb1, 0x21, 0x3d, 0x04}};
constexpr GUID kCapture = {0x005a6bc5, 0xd1f9, 0x4e51, {0xb1, 0x51, 0x35, 0x1a, 0x60, 0x59, 0x09, 0xab}};
constexpr UINT kUav = TESSERA_FEEDBACK_UAV, kTextures = TESSERA_FEEDBACK_TEXTURES;
constexpr unsigned kInterval = 60;

const TesseraFeedbackEntry* g_entries[TESSERA_FEEDBACK_MAX_ENTRIES];
uint32_t g_count = 0;
INIT_ONCE g_cache_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK load_cache(PINIT_ONCE, void*, void**) {
    wchar_t path[MAX_PATH];
    if (!session_data_path(L"feedback", L"tmip", path)) return TRUE;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return TRUE;
    LARGE_INTEGER size = {};
    HANDLE map = nullptr;
    if (GetFileSizeEx(f, &size) && size.QuadPart >= sizeof(TesseraFeedbackHeader) && size.QuadPart <= (128 << 20))
        map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    if (!map) return TRUE;
    auto* view = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(map);
    if (!view) return TRUE;
    const auto& h = *reinterpret_cast<const TesseraFeedbackHeader*>(view);
    const uint8_t* cursor = view + sizeof h;
    const uint8_t* end = view + size.QuadPart;
    bool valid = std::memcmp(h.magic, TESSERA_FEEDBACK_MAGIC, 8) == 0 && h.count <= _countof(g_entries);
    for (uint32_t i = 0; valid && i < h.count; ++i) {
        if (static_cast<size_t>(end - cursor) < sizeof(TesseraFeedbackEntry)) {
            valid = false;
            break;
        }
        const auto* v = reinterpret_cast<const TesseraFeedbackEntry*>(cursor);
        cursor += sizeof *v;
        valid = v->bytes >= 32 && v->bytes <= TESSERA_FEEDBACK_MAX_SHADER_BYTES && !(v->bytes & 3) &&
                v->bytes <= static_cast<size_t>(end - cursor) && v->count && v->count <= _countof(v->slots) &&
                std::all_of(v->slots, v->slots + v->count, [](uint8_t slot) { return slot < kTextures; }) &&
                (i == 0 || std::memcmp(g_entries[i - 1]->checksum, v->checksum, sizeof v->checksum) < 0);
        if (valid) {
            g_entries[i] = v;
            cursor += v->bytes;
        }
    }
    if (!valid || cursor != end || !h.count) {
        UnmapViewOfFile(view);
        return TRUE;
    }
    g_count = h.count;  // keep immutable bytes mapped for the life of the process
    return TRUE;
}

bool have_cache() {
    InitOnceExecuteOnce(&g_cache_once, load_cache, nullptr, nullptr);
    return g_count != 0;
}

struct Capture;
SRWLOCK g_active_lock = SRWLOCK_INIT;
Capture* g_active = nullptr;  // the swapchain owns the state
std::atomic<bool> g_armed{false};
struct Slot {
    uint64_t texture;
    UINT first_mip, binding;
};
struct Capture final : IUnknown {
    std::atomic<ULONG> refs{1};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    IDXGISwapChain* swapchain = nullptr;  // weak, owner
    ID3D11DeviceContext* context = nullptr;  // immediate context, owned by the device
    ComPtr<ID3D11Buffer> gpu, readback;
    ComPtr<ID3D11UnorderedAccessView> view;
    Slot slots[TESSERA_FEEDBACK_SLOTS] = {};
    unsigned count = 0, frame = 0, attempts = 0;
    bool pending = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        AddRef();
        *out = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n) return n;
        AcquireSRWLockExclusive(&g_active_lock);
        if (g_active == this) {
            g_active = nullptr;
            g_armed.store(false, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&g_active_lock);
        delete this;
        return 0;
    }
};

Capture* acquire(ID3D11DeviceContext* ctx, IDXGISwapChain* sc) {
    if (!TryAcquireSRWLockShared(&g_active_lock)) return nullptr;
    auto* cap = g_active;
    if (cap && (ctx ? ctx == cap->context : sc == cap->swapchain)) {
        ULONG refs = cap->refs.load(std::memory_order_relaxed);
        while (refs && !cap->refs.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {}
        if (!refs) cap = nullptr;
    } else {
        cap = nullptr;
    }
    ReleaseSRWLockShared(&g_active_lock);
    if (cap && cap->busy.test_and_set(std::memory_order_acquire)) {
        cap->Release();
        return nullptr;
    }
    return cap;
}

void release(Capture& cap) {
    cap.busy.clear(std::memory_order_release);
    cap.Release();
}

bool create_buffers(Capture& cap, ID3D11Device* dev) {
    D3D11_BUFFER_DESC b = {};
    b.ByteWidth = kTextures * sizeof(UINT);
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(dev->CreateBuffer(&b, nullptr, &cap.gpu))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = kTextures;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(cap.gpu.Get(), &u, &cap.view))) return false;
    b.Usage = D3D11_USAGE_STAGING;
    b.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    b.BindFlags = b.MiscFlags = 0;
    return SUCCEEDED(dev->CreateBuffer(&b, nullptr, &cap.readback));
}

bool capture_bindings(Capture& cap, ID3D11PixelShader& original) {
    uint8_t indices[TESSERA_FEEDBACK_SLOTS];
    UINT n = sizeof indices;
    if (FAILED(original.GetPrivateData(kSlots, &n, indices)) || !n || n > sizeof indices) return false;
    cap.count = 0;
    auto* ctx = cap.context;
    for (UINT j = 0; j < n; ++j) {
        ComPtr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(indices[j], 1, &srv);
        if (!srv) continue;
        const auto texture = workset_view_asset(srv.Get());
        if (!texture) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        srv->GetDesc(&d);
        if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
            cap.slots[cap.count++] = {texture, d.Texture2D.MostDetailedMip, indices[j]};
    }
    return cap.count != 0;
}

bool free_output_slot(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ctx->OMGetRenderTargets(_countof(rt), rt, nullptr);
    const bool available = !rt[kUav];
    for (auto* view : rt)
        if (view) view->Release();
    if (!available) return false;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, kUav, 1, &uav);
    return !uav;
}

template <typename F, typename... A>
void draw(ID3D11DeviceContext* ctx, F original_draw, A... args) {
    if (!g_armed.load(std::memory_order_relaxed)) {
        original_draw(ctx, args...);
        return;
    }
    Capture* cap = acquire(ctx, nullptr);
    if (!cap) {
        original_draw(ctx, args...);
        return;
    }
    ComPtr<ID3D11PixelShader> original, variant;
    if (cap->attempts && !cap->pending) {
        if (--cap->attempts == 0) g_armed.store(false, std::memory_order_relaxed);
        ctx->PSGetShader(&original, nullptr, nullptr);
        UINT size = sizeof(ID3D11PixelShader*);
        if (original) original->GetPrivateData(kVariant, &size, variant.GetAddressOf());
    }
    if (!variant || !capture_bindings(*cap, *original.Get()) || !free_output_slot(ctx)) {
        release(*cap);
        original_draw(ctx, args...);
        return;
    }
    g_armed.store(false, std::memory_order_relaxed);
    constexpr UINT clear[4] = {UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX};
    ctx->ClearUnorderedAccessViewUint(cap->view.Get(), clear);
    auto* uav = cap->view.Get();
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                   kUav, 1, &uav, nullptr);
    ctx->PSSetShader(variant.Get(), nullptr, 0);
    original_draw(ctx, args...);
    ctx->PSSetShader(original.Get(), nullptr, 0);
    uav = nullptr;
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
                                                   kUav, 1, &uav, nullptr);
    ctx->CopyResource(cap->readback.Get(), cap->gpu.Get());
    cap->pending = true;
    release(*cap);
}

void STDMETHODCALLTYPE hk_draw(ID3D11DeviceContext*, UINT, UINT);
void STDMETHODCALLTYPE hk_draw_indexed(ID3D11DeviceContext*, UINT, UINT, INT);
void STDMETHODCALLTYPE hk_draw_instanced(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
void STDMETHODCALLTYPE hk_draw_indexed_instanced(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
VtHook g_draw = {13, reinterpret_cast<void*>(&hk_draw)};
VtHook g_draw_indexed = {12, reinterpret_cast<void*>(&hk_draw_indexed)};
VtHook g_draw_instanced = {21, reinterpret_cast<void*>(&hk_draw_instanced)};
VtHook g_draw_indexed_instanced = {20, reinterpret_cast<void*>(&hk_draw_indexed_instanced)};

void STDMETHODCALLTYPE hk_draw(ID3D11DeviceContext* c, UINT n, UINT first) {
    draw(c, reinterpret_cast<decltype(&hk_draw)>(vt_orig(g_draw, c)), n, first);
}
void STDMETHODCALLTYPE hk_draw_indexed(ID3D11DeviceContext* c, UINT n, UINT first, INT base) {
    draw(c, reinterpret_cast<decltype(&hk_draw_indexed)>(vt_orig(g_draw_indexed, c)), n, first, base);
}
void STDMETHODCALLTYPE hk_draw_instanced(ID3D11DeviceContext* c, UINT n, UINT count, UINT first, UINT start) {
    draw(c, reinterpret_cast<decltype(&hk_draw_instanced)>(vt_orig(g_draw_instanced, c)), n, count, first, start);
}
void STDMETHODCALLTYPE hk_draw_indexed_instanced(ID3D11DeviceContext* c, UINT n, UINT count, UINT first, INT base,
                                                  UINT start) {
    draw(c, reinterpret_cast<decltype(&hk_draw_indexed_instanced)>(vt_orig(g_draw_indexed_instanced, c)),
         n, count, first, base, start);
}
}  // namespace

void feedback_on_shader(ID3D11Device* dev, CreatePixelShaderFn create, ID3D11PixelShader* original,
                        const void* bytes, SIZE_T size) {
    if (!config().mip_feedback || size < 32 || std::memcmp(bytes, "DXBC", 4) != 0 || !have_cache()) return;
    const auto* checksum = static_cast<const uint8_t*>(bytes) + 4;
    auto end = g_entries + g_count;
    auto match = std::lower_bound(g_entries, end, checksum, [](const auto* entry, const uint8_t* key) {
        return std::memcmp(entry->checksum, key, 16) < 0;
    });
    if (match == end || std::memcmp((*match)->checksum, checksum, 16) != 0) return;
    const auto& entry = **match;
    ComPtr<ID3D11PixelShader> variant;
    if (SUCCEEDED(create(dev, &entry + 1, entry.bytes, nullptr, &variant)) &&
        SUCCEEDED(original->SetPrivateData(kSlots, entry.count, entry.slots)))
        original->SetPrivateDataInterface(kVariant, variant.Get());
}

void feedback_on_swapchain(IDXGISwapChain* sc) {
    if (!config().mip_feedback || !config().workset || !have_cache()) return;
    ComPtr<IUnknown> existing;
    UINT size = sizeof(IUnknown*);
    if (SUCCEEDED(sc->GetPrivateData(kCapture, &size, existing.GetAddressOf()))) return;
    ComPtr<ID3D11Device> dev;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))) || dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    auto* cap = new (std::nothrow) Capture;
    if (!cap) return;
    cap->context = ctx.Get();
    cap->swapchain = sc;
    if (ctx && create_buffers(*cap, dev.Get()) && vt_hook(g_draw, ctx.Get()) && vt_hook(g_draw_indexed, ctx.Get()) &&
        vt_hook(g_draw_instanced, ctx.Get()) && vt_hook(g_draw_indexed_instanced, ctx.Get()) &&
        SUCCEEDED(sc->SetPrivateDataInterface(kCapture, cap))) {
        AcquireSRWLockExclusive(&g_active_lock);
        if (!g_active) g_active = cap;
        ReleaseSRWLockExclusive(&g_active_lock);
        logf("mip feedback: prepared %u pixel shaders, sampling one draw per %u frames", g_count, kInterval);
    }
    cap->Release();
}

void feedback_present(IDXGISwapChain* sc) {
    if (!config().mip_feedback || !have_cache()) return;
    Capture* cap = acquire(nullptr, sc);
    if (!cap) {
        // If a different swapchain was already prepared, adopt it after
        // the original one is released.
        ComPtr<IUnknown> saved;
        UINT size = sizeof(IUnknown*);
        if (SUCCEEDED(sc->GetPrivateData(kCapture, &size, saved.GetAddressOf())) && saved &&
            TryAcquireSRWLockExclusive(&g_active_lock)) {
            if (!g_active) g_active = static_cast<Capture*>(saved.Get());
            ReleaseSRWLockExclusive(&g_active_lock);
        }
        return;
    }
    if (cap->pending) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = cap->context->Map(cap->readback.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr)) {
            const auto* levels = static_cast<const UINT*>(m.pData);
            LARGE_INTEGER time;
            QueryPerformanceCounter(&time);
            for (unsigned i = 0; i < cap->count; ++i) {
                const auto& slot = cap->slots[i];
                const UINT mip = levels[slot.binding];
                if (mip == UINT_MAX) continue;
                TesseraRecord r = {};
                r.kind = TESSERA_REC_MIP_FEEDBACK;
                r.qpc = static_cast<uint64_t>(time.QuadPart);
                r.a = slot.binding;
                r.b = std::min(mip, 65535u) + slot.first_mip;
                r.bytes = slot.texture;
                session_push(r);
            }
            cap->context->Unmap(cap->readback.Get(), 0);
            cap->pending = false;
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            cap->pending = false;
        }
    }
    cap->attempts = cap->frame++ % kInterval == 0 && !cap->pending ? 32 : 0;
    g_armed.store(cap->attempts != 0, std::memory_order_relaxed);
    release(*cap);
}

void feedback_after_present(IDXGISwapChain* sc) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (auto* cap = acquire(nullptr, sc)) {
        // DXGI's Present may replace the immediate context's Draw methods.
        // Restore our chain only while a sample is armed.
        const bool ok = vt_rehook(g_draw, cap->context) && vt_rehook(g_draw_indexed, cap->context) &&
                        vt_rehook(g_draw_instanced, cap->context) && vt_rehook(g_draw_indexed_instanced, cap->context);
        if (!ok) g_armed.store(false, std::memory_order_relaxed);
        release(*cap);
    }
}
}  // namespace tessera
