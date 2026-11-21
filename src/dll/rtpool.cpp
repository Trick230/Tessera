// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtpool.h"

#include <windows.h>
#include <d3d11_3.h>

#include <atomic>
#include <cstring>
#include <new>

#include "config.h"
#include "log.h"
#include "vtable.h"

namespace tessera {
namespace {
constexpr uint64_t kMinBytes = 16ull << 20;  // smaller ones are cheap to make
constexpr uint64_t kMaxIdle = 512ull << 20;  // released textures kept at most
constexpr ULONGLONG kIdleMs = 30000;         // unused this long: freed
constexpr int kSlots = 32;
constexpr UINT kTargetBinds = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
// Video views (decoder, encoder) are not watched.
constexpr UINT kVideoBinds = D3D11_BIND_DECODER | D3D11_BIND_VIDEO_ENCODER;
// Anything else (shared, tiled, GDI, a resource MinLOD clamp...) is left alone.
constexpr UINT kPlainMisc = D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TEXTURECUBE;

struct Slot {
    ID3D11Texture2D* tex;  // our reference; null = free
    ID3D11Device* dev;     // kept alive by tex
    D3D11_TEXTURE2D_DESC desc;
    uint64_t bytes;
    uint64_t seq;          // tracking order: the oldest held one makes room
    ULONGLONG idle_since;  // 0: the game holds it (as of the last look)
    // gen << 32 | live views on it (each holds it internally). gen changes
    // when the slot is emptied, so watches of the old texture do nothing.
    // Lock-free: view watches run inside the runtime's view destruction.
    std::atomic<uint64_t> gv;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Slot g_slots[kSlots];
uint64_t g_idle_bytes;  // sum over slots with idle_since != 0
uint64_t g_seq;
int g_next;  // slot rtpool_frame looks at
uint32_t g_reused;
std::atomic<int> g_count{0};      // slots in use
std::atomic<bool> g_on{false};    // view hooks in place on some device
std::atomic<bool> g_stop{false};  // nothing handed out or kept any more

bool taking() { return g_on.load(std::memory_order_acquire) && !g_stop.load(std::memory_order_acquire); }

bool qualifies(const D3D11_TEXTURE2D_DESC& d, uint64_t bytes) {
    return bytes >= kMinBytes && d.Usage == D3D11_USAGE_DEFAULT && d.CPUAccessFlags == 0 &&
           (d.BindFlags & kTargetBinds) && !(d.BindFlags & kVideoBinds) && !(d.MiscFlags & ~kPlainMisc);
}

// Only our reference left and no view: the game released it. Under the lock;
// nobody else can reach it any more.
bool released(const Slot& s) {
    if (static_cast<uint32_t>(s.gv.load(std::memory_order_acquire))) return false;
    s.tex->AddRef();
    return s.tex->Release() == 1;
}

// Under the lock; the caller releases the returned reference outside it.
ID3D11Texture2D* empty(Slot& s) {
    ID3D11Texture2D* t = s.tex;
    if (s.idle_since) g_idle_bytes -= s.bytes;
    const uint64_t gen = (s.gv.load(std::memory_order_relaxed) >> 32) + 1;
    s.gv.store(gen << 32, std::memory_order_release);
    s.tex = nullptr;
    s.dev = nullptr;
    s.idle_since = 0;
    g_count.fetch_sub(1, std::memory_order_relaxed);
    return t;
}
}  // namespace

bool rtpool_take(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d, uint64_t bytes, ID3D11Texture2D** out) {
    if (!taking() || !qualifies(d, bytes) || g_count.load(std::memory_order_relaxed) == 0) return false;
    ID3D11Texture2D* hit = nullptr;
    uint32_t n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& s : g_slots) {
        if (!s.tex || s.dev != dev || std::memcmp(&s.desc, &d, sizeof d) != 0 || !released(s)) continue;
        if (s.idle_since) g_idle_bytes -= s.bytes;
        s.idle_since = 0;
        s.tex->AddRef();  // the game's
        hit = s.tex;
        n = ++g_reused;
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!hit) return false;
    *out = hit;
    if (n <= 8 || (n & (n - 1)) == 0) logf("rtpool: reused %ux%u format %u (%u so far)", d.Width, d.Height, d.Format, n);
    return true;
}

void rtpool_track(ID3D11Device* dev, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, uint64_t bytes) {
    if (!taking() || !qualifies(d, bytes)) return;
    ID3D11Texture2D* drop = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    Slot* to = nullptr;
    Slot* oldest = nullptr;
    for (Slot& s : g_slots) {
        if (!s.tex) {
            to = &s;
            break;
        }
        if (!s.idle_since && (!oldest || s.seq < oldest->seq)) oldest = &s;
    }
    // Full: the targets the game keeps all session must not shut out the ones
    // it recreates. Dropping our extra reference to one it holds costs nothing.
    if (!to && oldest) {
        drop = empty(*oldest);
        to = oldest;
    }
    if (to) {
        tex->AddRef();
        to->tex = tex;
        to->dev = dev;
        to->desc = d;
        to->bytes = bytes;
        to->seq = ++g_seq;
        to->idle_since = 0;
        g_count.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (drop) drop->Release();  // the game still holds it (as of its last look): rarely the last one
}

void rtpool_stop(const char* why) {
    if (config().rt_recycle && !g_stop.exchange(true, std::memory_order_acq_rel)) logf("rtpool: off, %s", why);
}

namespace {
// The device that presents through sc, compared by address only.
ID3D11Device* device_of(IDXGISwapChain* sc) {
    ID3D11Device* d = nullptr;
    if (!sc || FAILED(sc->GetDevice(IID_PPV_ARGS(&d))) || !d) return nullptr;
    d->Release();
    return d;
}
}  // namespace

void rtpool_frame(IDXGISwapChain* sc) {
    if (g_count.load(std::memory_order_relaxed) == 0 || !TryAcquireSRWLockExclusive(&g_lock)) return;
    Slot& s = g_slots[g_next];
    g_next = (g_next + 1) % kSlots;
    ID3D11Texture2D* drop = nullptr;
    ID3D11Texture2D* over = nullptr;
    if (s.tex) {
        const ULONGLONG now = GetTickCount64();
        if (!released(s)) {
            if (s.idle_since) g_idle_bytes -= s.bytes;
            s.idle_since = 0;
        } else {
            // Its eviction priority stays as it is: SetEvictionPriority hangs the
            // GPU or removes the device in FF7R on NVIDIA 621.14 (M2, R1 alike).
            if (!s.idle_since) {
                s.idle_since = now;
                g_idle_bytes += s.bytes;
            }
            // A spare of a device that no longer presents (replaced) is not
            // asked for again: freed now rather than after kIdleMs.
            if (now - s.idle_since >= kIdleMs || g_stop.load(std::memory_order_relaxed) || s.dev != device_of(sc))
                drop = empty(s);
        }
    }
    // Over the cap: the spare unused the longest goes.
    if (g_idle_bytes > kMaxIdle) {
        Slot* oldest = nullptr;
        for (Slot& o : g_slots)
            if (o.tex && o.idle_since && (!oldest || o.idle_since < oldest->idle_since)) oldest = &o;
        if (oldest) over = empty(*oldest);
    }
    ReleaseSRWLockExclusive(&g_lock);
    // Outside the lock: the driver frees them.
    if (drop) drop->Release();
    if (over) over->Release();
}

void rtpool_resize() {
    if (g_count.load(std::memory_order_relaxed) == 0) return;
    // Spares sized for the old resolution: the next creates will not match them.
    ID3D11Texture2D* drop[kSlots];
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (Slot& s : g_slots)
        if (s.tex && released(s)) drop[n++] = empty(s);
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < n; ++i) drop[i]->Release();
}

namespace {
// Private data of a view on a kept texture: the view's destruction releases
// it, and the texture has one live view less.
class ViewWatch final : public IUnknown {
public:
    ViewWatch(int slot, uint32_t gen) : slot_(slot), gen_(gen) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id != __uuidof(IUnknown)) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *out = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = InterlockedDecrement(&refs_);
        if (n == 0) {
            std::atomic<uint64_t>& gv = g_slots[slot_].gv;
            uint64_t v = gv.load(std::memory_order_acquire);
            while ((v >> 32) == gen_ && static_cast<uint32_t>(v) &&
                   !gv.compare_exchange_weak(v, v - 1, std::memory_order_acq_rel)) {
            }
            this->~ViewWatch();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return static_cast<ULONG>(n);
    }
    void disarm() { gen_ = ~gen_; }

private:
    volatile long refs_ = 1;
    int slot_;
    uint32_t gen_;
};

constexpr GUID kViewGuid = {0x3f8a1d52, 0x7c6e, 0x4b09, {0x9e, 0x2d, 0x51, 0xa7, 0x0c, 0x84, 0xf3, 0x6b}};

// Vtable slots (SDK declaration order).
constexpr int kDevCreateUAV = 8, kDevCreateRTV = 9, kDevCreateDSV = 10;
constexpr int kDev3CreateSRV1 = 57, kDev3CreateUAV1 = 58, kDev3CreateRTV1 = 59;

template <class F>
F orig(const VtHook& h, void* self) {
    return reinterpret_cast<F>(vt_orig(h, self));
}

HRESULT STDMETHODCALLTYPE hk_UAV(ID3D11Device*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*,
                                 ID3D11UnorderedAccessView**);
HRESULT STDMETHODCALLTYPE hk_RTV(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*,
                                 ID3D11RenderTargetView**);
HRESULT STDMETHODCALLTYPE hk_DSV(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*,
                                 ID3D11DepthStencilView**);
HRESULT STDMETHODCALLTYPE hk_SRV1(ID3D11Device3*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC1*,
                                  ID3D11ShaderResourceView1**);
HRESULT STDMETHODCALLTYPE hk_UAV1(ID3D11Device3*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC1*,
                                  ID3D11UnorderedAccessView1**);
HRESULT STDMETHODCALLTYPE hk_RTV1(ID3D11Device3*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC1*,
                                  ID3D11RenderTargetView1**);
VtHook g_uav = {kDevCreateUAV, reinterpret_cast<void*>(&hk_UAV)};
VtHook g_rtv = {kDevCreateRTV, reinterpret_cast<void*>(&hk_RTV)};
VtHook g_dsv = {kDevCreateDSV, reinterpret_cast<void*>(&hk_DSV)};
VtHook g_srv1 = {kDev3CreateSRV1, reinterpret_cast<void*>(&hk_SRV1)};
VtHook g_uav1 = {kDev3CreateUAV1, reinterpret_cast<void*>(&hk_UAV1)};
VtHook g_rtv1 = {kDev3CreateRTV1, reinterpret_cast<void*>(&hk_RTV1)};

#define TESSERA_VIEW_HOOK(name, hook, Dev, Desc, View)                                          \
    HRESULT STDMETHODCALLTYPE name(Dev* self, ID3D11Resource* res, const Desc* d, View** out) { \
        using F = decltype(&name);                                                              \
        const HRESULT hr = orig<F>(hook, self)(self, res, d, out);                              \
        if (SUCCEEDED(hr) && out && *out && res) rtpool_on_view(res, *out);                     \
        return hr;                                                                              \
    }
TESSERA_VIEW_HOOK(hk_UAV, g_uav, ID3D11Device, D3D11_UNORDERED_ACCESS_VIEW_DESC, ID3D11UnorderedAccessView)
TESSERA_VIEW_HOOK(hk_RTV, g_rtv, ID3D11Device, D3D11_RENDER_TARGET_VIEW_DESC, ID3D11RenderTargetView)
TESSERA_VIEW_HOOK(hk_DSV, g_dsv, ID3D11Device, D3D11_DEPTH_STENCIL_VIEW_DESC, ID3D11DepthStencilView)
TESSERA_VIEW_HOOK(hk_SRV1, g_srv1, ID3D11Device3, D3D11_SHADER_RESOURCE_VIEW_DESC1, ID3D11ShaderResourceView1)
TESSERA_VIEW_HOOK(hk_UAV1, g_uav1, ID3D11Device3, D3D11_UNORDERED_ACCESS_VIEW_DESC1, ID3D11UnorderedAccessView1)
TESSERA_VIEW_HOOK(hk_RTV1, g_rtv1, ID3D11Device3, D3D11_RENDER_TARGET_VIEW_DESC1, ID3D11RenderTargetView1)
#undef TESSERA_VIEW_HOOK
}  // namespace

void rtpool_on_device(ID3D11Device* dev, bool srv_ok) {
    if (!config().rt_recycle || g_stop.load(std::memory_order_acquire)) return;
    // M3-M5 hooks CreateShaderResourceView1 itself: one hook per slot. It only
    // runs on tiled resources tier 2 (residency.cpp); below that, no clash.
    if (config().mip_residency) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1)) &&
            o1.TiledResourcesTier >= D3D11_TILED_RESOURCES_TIER_2)
            return rtpool_stop("while mip_residency is on");
    }
    bool ok = srv_ok && vt_hook(g_uav, dev) && vt_hook(g_rtv, dev) && vt_hook(g_dsv, dev);
    ID3D11Device3* dev3 = nullptr;
    if (ok && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3)))) {
        ok = vt_hook(g_srv1, dev3) && vt_hook(g_uav1, dev3) && vt_hook(g_rtv1, dev3);
        dev3->Release();
    }
    // Views on a device without the hooks would go unseen: off for good.
    if (!ok) return rtpool_stop("view hooks FAILED");
    if (!g_on.exchange(true, std::memory_order_acq_rel)) logf("rtpool: on");
}

void rtpool_on_view(ID3D11Resource* res, ID3D11View* view) {
    if (g_count.load(std::memory_order_acquire) == 0) return;
    int slot = -1;
    uint32_t gen = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].tex && static_cast<ID3D11Resource*>(g_slots[i].tex) == res) {
            slot = i;
            // Counted before the watch exists: a failed one keeps the texture held.
            gen = static_cast<uint32_t>(g_slots[i].gv.fetch_add(1, std::memory_order_acq_rel) >> 32);
            break;
        }
    ReleaseSRWLockExclusive(&g_lock);
    if (slot < 0) return;
    void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(ViewWatch));
    if (!mem) return;
    auto* w = new (mem) ViewWatch(slot, gen);
    if (FAILED(view->SetPrivateDataInterface(kViewGuid, w))) w->disarm();  // the count stays up
    w->Release();  // the view's reference is the one left
}
}  // namespace tessera
