// SPDX-License-Identifier: GPL-3.0-or-later
#include "workset.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>

#include "config.h"
#include "log.h"
#include "prediction.h"
#include "residency.h"
#include "session.h"
#include "vtable.h"

namespace tessera {
namespace {
// ID3D11DeviceContext slots of the six *SetShaderResources.
constexpr int kCtxPS = 8, kCtxVS = 25, kCtxGS = 31, kCtxHS = 59, kCtxDS = 63, kCtxCS = 67;

// Open-addressing tables keyed by object pointer. Inserts run under g_lock
// (resource creation); lookups from the bind hooks are lock-free. Each live
// slot is attached to its object as private data, so the object's
// destruction releases it and turns the key into a tombstone: a freed
// address can never match a stale slot.
void* const kTomb = reinterpret_cast<void*>(1);
constexpr int kProbe = 32;  // bounds a lookup; past it the object is left untracked
// {3e9b6f2d-41a7-4c58-9d0e-7b2a5c81f6e3}
constexpr GUID kSlotGuid = {0x3e9b6f2d, 0x41a7, 0x4c58, {0x9d, 0x0e, 0x7b, 0x2a, 0x5c, 0x81, 0xf6, 0xe3}};

// What a bind touches: key and frame in one 16-byte entry, so a lookup in a
// table of tens of thousands of views stays one cache miss in a compact array.
struct Hot {
    std::atomic<void*> key;
    std::atomic<uint32_t> last;  // SRV: frame it was last bound in, 0 = never
    std::atomic<uint32_t> res;   // SRV: residency index + 1 of its texture (M3), 0 = unmanaged
};
// The rest, in a parallel array. A slot is a COM object by layout (vtable
// pointer first), written by hand so the tables stay plain zeroed memory:
// allocated only when the meter is on, committed page by page as used.
struct Slot;
struct SlotVtbl {
    HRESULT(STDMETHODCALLTYPE* query)(Slot*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* add_ref)(Slot*);
    ULONG(STDMETHODCALLTYPE* release)(Slot*);
};
struct Slot {
    const SlotVtbl* vtbl;
    std::atomic<long> refs;
    std::atomic<uint32_t> mark;  // texture: tick that found it bound (1 s / 10 s windows)
    std::atomic<uint32_t> mark10;
    std::atomic<int> views;       // texture: tracked views alive
    Hot* hot;                     // its key; written under g_lock before publishing
    std::atomic<uint64_t> bytes;  // texture: size estimate
    std::atomic<uint64_t> asset;  // texture: content fingerprint for prediction
    std::atomic<Slot*> tex;       // SRV: the texture slot it views
    std::atomic<uint32_t> res;    // texture: residency index + 1 (M3), 0 = unmanaged
};
struct Table {
    Hot* hot;
    Slot* slot;
    int bits;
};

HRESULT STDMETHODCALLTYPE slot_query(Slot* s, REFIID id, void** out) {
    if (!out) return E_POINTER;
    if (id != __uuidof(IUnknown)) {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    s->refs.fetch_add(1);
    *out = s;
    return S_OK;
}
ULONG STDMETHODCALLTYPE slot_add_ref(Slot* s) { return static_cast<ULONG>(s->refs.fetch_add(1) + 1); }
ULONG STDMETHODCALLTYPE slot_release(Slot* s) {
    const long n = s->refs.fetch_sub(1) - 1;
    if (n == 0) {
        // A view dies before its texture (it holds a reference to it).
        if (Slot* t = s->tex.load(std::memory_order_relaxed)) t->views.fetch_sub(1, std::memory_order_relaxed);
        s->hot->key.store(kTomb, std::memory_order_release);
    }
    return static_cast<ULONG>(n);
}
const SlotVtbl kSlotVtbl = {&slot_query, &slot_add_ref, &slot_release};

constexpr int kTexBits = 15, kSrvBits = 16;  // 32K textures, 64K views (~6 MB reserved)
Table g_tex = {nullptr, nullptr, kTexBits};
Table g_srv = {nullptr, nullptr, kSrvBits};
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<uint32_t> g_frame{1};
std::atomic<uint32_t> g_untracked{0};
std::atomic<bool> g_on{false};
ID3D11DeviceContext* g_imm = nullptr;  // weak, like the device

uint32_t hash(const void* p, int bits) {
    return static_cast<uint32_t>(((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

// Index of key's entry, -1 if absent.
int find(const Table& t, const void* key) {
    const uint32_t mask = (1u << t.bits) - 1, h = hash(key, t.bits);
    for (int i = 0; i < kProbe; ++i) {
        const uint32_t j = (h + i) & mask;
        const void* k = t.hot[j].key.load(std::memory_order_acquire);
        if (k == key) return static_cast<int>(j);
        if (!k) return -1;
    }
    return -1;
}

// Under g_lock: a free entry for key (first tombstone or empty on its probe run), -1 if none.
int claim(const Table& t, const void* key) {
    const uint32_t mask = (1u << t.bits) - 1, h = hash(key, t.bits);
    int tomb = -1;
    for (int i = 0; i < kProbe; ++i) {
        const uint32_t j = (h + i) & mask;
        void* k = t.hot[j].key.load(std::memory_order_relaxed);
        if (k == kTomb && tomb < 0) tomb = static_cast<int>(j);
        if (!k) return tomb >= 0 ? tomb : static_cast<int>(j);
    }
    return tomb;
}

// Under g_lock: publishes entry j for obj and ties its life to obj's.
bool attach(const Table& t, int j, ID3D11DeviceChild* obj) {
    Slot* s = &t.slot[j];
    s->vtbl = &kSlotVtbl;
    s->hot = &t.hot[j];
    s->refs.store(0, std::memory_order_relaxed);
    t.hot[j].key.store(obj, std::memory_order_release);
    if (SUCCEEDED(obj->SetPrivateDataInterface(kSlotGuid, reinterpret_cast<IUnknown*>(s)))) return true;
    t.hot[j].key.store(kTomb, std::memory_order_release);
    return false;
}

// One lookup per view; the texture is resolved in the tick, off the frame path.
template <bool Predict>
void mark(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    // The runtime drops an out-of-range call without reading v: so must we.
    if (!v || start >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return;
    if (n > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - start) return;
    const uint32_t f = g_frame.load(std::memory_order_relaxed);
    thread_local uint32_t t_frame = 0;
    thread_local unsigned t_samples = 0;
    if constexpr (Predict) {
        if (t_frame != f) {
            t_frame = f;
            t_samples = 0;
        }
    }
    TextureHint hints[kMaterialTextures];
    unsigned count = 0;
    // Start every view's cache miss before waiting on the first.
    for (UINT i = 0; i < n; ++i)
        if (v[i]) _mm_prefetch(reinterpret_cast<const char*>(&g_srv.hot[hash(v[i], kSrvBits)]), _MM_HINT_T0);
    for (UINT i = 0; i < n; ++i) {
        if (!v[i]) continue;
        const int j = find(g_srv, v[i]);
        if (j < 0) continue;
        // Write only on the first bind in a frame: no cache line ping-pong.
        std::atomic<uint32_t>& last = g_srv.hot[j].last;
        if (last.load(std::memory_order_relaxed) != f) last.store(f, std::memory_order_relaxed);
        // M5: a managed texture is made whole before the draw that samples it.
        const uint32_t r = g_srv.hot[j].res.load(std::memory_order_relaxed);
        if (r) residency_on_use(r - 1, ctx);
        if constexpr (Predict) {
            if (count < kMaterialTextures && t_samples < 12) {
                if (const auto* t = g_srv.slot[j].tex.load(std::memory_order_relaxed)) {
                    if (const uint64_t key = t->asset.load(std::memory_order_relaxed)) hints[count++] = {key, r};
                }
            }
        }
    }
    if constexpr (Predict) {
        if (count) {
            ++t_samples;
            prediction_material(hints, count);
        }
    }
}

template <int I>
void STDMETHODCALLTYPE hk_set(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);

VtHook g_set[6] = {
    {kCtxPS, reinterpret_cast<void*>(&hk_set<0>)}, {kCtxVS, reinterpret_cast<void*>(&hk_set<1>)},
    {kCtxGS, reinterpret_cast<void*>(&hk_set<2>)}, {kCtxHS, reinterpret_cast<void*>(&hk_set<3>)},
    {kCtxDS, reinterpret_cast<void*>(&hk_set<4>)}, {kCtxCS, reinterpret_cast<void*>(&hk_set<5>)},
};

template <int I>
void STDMETHODCALLTYPE hk_set(ID3D11DeviceContext* self, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    if (config().mip_prediction)
        mark<true>(self, start, n, v);
    else
        mark<false>(self, start, n, v);
    using F = decltype(&hk_set<I>);
    reinterpret_cast<F>(vt_orig(g_set[I], self))(self, start, n, v);
}
}  // namespace

void workset_on_device(ID3D11Device* dev) {
    if (!config().workset || g_on.load(std::memory_order_acquire)) return;
    if (!g_tex.hot) {
        // Committed at once, but zero pages cost nothing until touched.
        const size_t nt = size_t{1} << kTexBits, ns = size_t{1} << kSrvBits;
        const size_t bytes = (nt + ns) * (sizeof(Hot) + sizeof(Slot));
        auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!p) {
            logf("workset: no memory for the tables, meter off");
            return;
        }
        g_tex.hot = reinterpret_cast<Hot*>(p);
        g_srv.hot = g_tex.hot + nt;
        g_tex.slot = reinterpret_cast<Slot*>(g_srv.hot + ns);
        g_srv.slot = g_tex.slot + nt;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    bool ok = true;
    for (VtHook& h : g_set) ok = vt_hook(h, ctx) && ok;
    g_imm = ctx;
    ctx->Release();
    logf("workset: bind hooks %s", ok ? "ok" : "FAILED");
    // A partial set would undercount; the meter is only on with all six.
    if (ok) g_on.store(true, std::memory_order_release);
}

bool workset_adopt(ID3D11DeviceContext* imm) {
    if (!g_on.load(std::memory_order_acquire)) return false;
    if (imm == g_imm) return true;
    // A device that replaced the first one: its binds are the ones that count now.
    bool ok = true;
    for (VtHook& h : g_set) ok = vt_hook(h, imm) && ok;
    logf("workset: bind hooks on the new device %s", ok ? "ok" : "FAILED");
    if (ok) g_imm = imm;
    return ok;
}

bool workset_check_hooks(ID3D11DeviceContext* imm) {
    // g_imm is weak: compared, never followed unless it is imm (alive).
    if (!g_on.load(std::memory_order_acquire) || imm != g_imm) return true;
    bool ok = true;
    for (VtHook& h : g_set) ok = vt_rehook(h, g_imm) && ok;
    static std::atomic<bool> s_logged{false};
    if (!ok && !s_logged.exchange(true)) logf("workset: bind hooks overwritten, binds go uncounted");
    return ok;
}

void workset_on_context(ID3D11DeviceContext* ctx) {
    if (!g_on.load(std::memory_order_acquire)) return;
    bool ok = true;
    for (VtHook& h : g_set) ok = vt_hook(h, ctx, true) && ok;
    static std::atomic<bool> s_logged{false};
    if (!ok && !s_logged.exchange(true)) logf("workset: deferred context hooks FAILED, its binds go uncounted");
}

std::atomic<int>* workset_on_texture(ID3D11Texture2D* tex, uint64_t bytes, uint32_t res, uint64_t key) {
    if (!g_on.load(std::memory_order_acquire)) return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    std::atomic<int>* views = nullptr;
    const int j = claim(g_tex, tex);
    if (j >= 0) {
        Slot& s = g_tex.slot[j];
        s.bytes.store(bytes, std::memory_order_relaxed);
        s.asset.store(key, std::memory_order_relaxed);
        s.mark.store(0, std::memory_order_relaxed);
        s.mark10.store(0, std::memory_order_relaxed);
        s.views.store(0, std::memory_order_relaxed);
        s.tex.store(nullptr, std::memory_order_relaxed);
        s.res.store(res, std::memory_order_relaxed);
        if (attach(g_tex, j, tex)) views = &s.views;
    }
    if (!views) g_untracked.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);
    return views;
}

void workset_on_srv(ID3D11Resource* res, ID3D11ShaderResourceView* view) {
    if (!res || !g_on.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_lock);
    // The view holds a reference to its texture: the slot outlives the view's.
    const int t = find(g_tex, res);
    uint32_t pin = 0;
    if (t >= 0) {
        Slot* ts = &g_tex.slot[t];
        const uint32_t managed = ts->res.load(std::memory_order_relaxed);
        const int j = claim(g_srv, view);
        if (j >= 0) {
            g_srv.slot[j].tex.store(ts, std::memory_order_relaxed);
            g_srv.hot[j].last.store(0, std::memory_order_relaxed);
            g_srv.hot[j].res.store(managed, std::memory_order_relaxed);
        }
        if (j >= 0 && attach(g_srv, j, view)) {
            ts->views.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_untracked.fetch_add(1, std::memory_order_relaxed);
            pin = managed;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    // Its binds go unseen: the texture must never lose a mip.
    if (pin) residency_pin(pin - 1);
}

uint32_t workset_texture_res(const void* tex) {
    if (!tex || !g_on.load(std::memory_order_acquire)) return 0;
    const int j = find(g_tex, tex);
    return j < 0 ? 0 : g_tex.slot[j].res.load(std::memory_order_relaxed);
}

uint32_t workset_view_res(const void* view) {
    if (!view || !g_on.load(std::memory_order_acquire)) return 0;
    const int j = find(g_srv, view);
    return j < 0 ? 0 : g_srv.hot[j].res.load(std::memory_order_relaxed);
}

void workset_clear_res(const void* tex) {
    if (!tex || !g_on.load(std::memory_order_acquire)) return;
    const int j = find(g_tex, tex);
    if (j >= 0) g_tex.slot[j].res.store(0, std::memory_order_relaxed);
}

void workset_frame() { g_frame.fetch_add(1, std::memory_order_relaxed); }

void workset_background_tick() {
    if (!g_on.load(std::memory_order_acquire)) return;
    // Frame counter at each of the last 10 ticks: [0] one second ago.
    static uint32_t ring[10];
    static unsigned ticks = 0;
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    if (ticks == 0)
        for (uint32_t& r : ring) r = now;
    const uint32_t since1 = ring[(ticks + 9) % 10];
    const uint32_t since10 = ring[ticks % 10];
    ring[ticks % 10] = now;
    ++ticks;
    // Tick ids start at 1 so a fresh slot's 0 never matches.
    const uint32_t id = ticks;

    // Views bound in the window mark their texture (once however many views
    // it has). A view and its texture can die between the two loads; the
    // texture slot may then be reused and wrongly marked: noise, not a fault.
    for (int i = 0; i < 1 << kSrvBits; ++i) {
        const Hot& h = g_srv.hot[i];
        const void* k = h.key.load(std::memory_order_acquire);
        if (!k || k == kTomb) continue;
        const uint32_t last = h.last.load(std::memory_order_relaxed);
        Slot* t = g_srv.slot[i].tex.load(std::memory_order_relaxed);
        if (!last || !t) continue;
        if (last >= since1) t->mark.store(id, std::memory_order_relaxed);
        if (last >= since10) t->mark10.store(id, std::memory_order_relaxed);
    }
    uint64_t live = 0, used1 = 0, used10 = 0;
    uint32_t count = 0;
    for (int i = 0; i < 1 << kTexBits; ++i) {
        const Slot& s = g_tex.slot[i];
        const void* k = g_tex.hot[i].key.load(std::memory_order_acquire);
        if (!k || k == kTomb) continue;
        ++count;
        const uint64_t b = s.bytes.load(std::memory_order_relaxed);
        live += b;
        if (s.mark.load(std::memory_order_relaxed) == id) used1 += b;
        if (s.mark10.load(std::memory_order_relaxed) == id) used10 += b;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    TesseraRecord r = {};
    r.qpc = static_cast<uint64_t>(t.QuadPart);
    r.kind = TESSERA_REC_WORKSET;
    r.flags = now - since1;
    r.a = static_cast<uint32_t>(used1 >> 10);
    r.b = static_cast<uint32_t>(used10 >> 10);
    r.c = count;
    r.d = g_untracked.load(std::memory_order_relaxed);
    r.bytes = live;
    session_push(r);
}
}  // namespace tessera
