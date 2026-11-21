// SPDX-License-Identifier: GPL-3.0-or-later
#include "residency.h"

#include <windows.h>
#include <d3d11_3.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <new>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "module.h"
#include "session.h"
#include "vtable.h"
#include "workset.h"

namespace tessera {
namespace {
// Vtable slots (SDK declaration order).
constexpr int kCtxCopySubresourceRegion = 46;
constexpr int kCtxCopyResource = 47;
constexpr int kCtxSetResourceMinLOD = 55;
constexpr int kCtxGetResourceMinLOD = 56;
constexpr int kCtxExecuteCommandList = 58;
constexpr int kCtxFlush = 111;
constexpr int kCtx1CopySubresourceRegion1 = 115;
constexpr int kCtx1SwapDeviceContextState = 131;
constexpr int kCtx2UpdateTileMappings = 134;
constexpr int kCtx2CopyTileMappings = 135;
constexpr int kCtx2ResizeTilePool = 138;
constexpr int kDev3CreateSRV1 = 57;
constexpr int kTexGetDesc = 10;
constexpr int kTex1GetDesc1 = 11;

constexpr uint32_t kMaxRes = 16384;
constexpr uint64_t kTile = 64 * 1024;
constexpr uint64_t kMinTop = 1ull << 20;             // top mips below 1 MB are not worth managing
constexpr uint64_t kPendingCap = 512ull << 20;       // staging copies waiting for their upload
constexpr uint64_t kUploadPerFrame = 64ull << 20;
constexpr uint64_t kEvictPerFrameBytes = 64ull << 20;
constexpr int kEvictPerFrame = 16;
constexpr int kCand = 256;
constexpr int kDropPerFrame = 64;

enum : uint32_t { kFree, kPending, kResident, kEvicted, kBusy };
enum : uint32_t {
    kQueuedUpload = 1,
    kQueuedNeed = 2,
    kQueuedDrop = 4,
    kQueued = kQueuedUpload | kQueuedNeed | kQueuedDrop,
    kPinned = 8,
    kDeferredBound = 16,  // bound on a deferred context at least once
};

// One managed texture. Atomics are read by any thread; the rest belongs to
// whoever holds g_op (the immediate context's thread) once published.
struct Res {
    std::atomic<uint32_t> state;
    std::atomic<uint32_t> last;   // frame it was last bound or copied from
    std::atomic<uint32_t> flags;
    std::atomic<uint32_t> epoch;  // g_epoch when found bound at an eviction batch
    std::atomic<std::atomic<int>*> views;  // its tracked views alive (workset)
    ID3D11Texture2D* tex;      // our own reference
    ID3D11Buffer* pool;        // tile pool: the tail (mips 1..) at 0, then mip 0 unless evicted
    ID3D11Texture2D* staging;  // Pending: the whole texture; Evicted: the top mip
    uint32_t tiles0, tail;     // tiles of mip 0 and of the rest
    uint64_t up_bytes, top_bytes;
    uint32_t width, height, mips;
    DXGI_FORMAT format;
    float lod;  // the game's resource min LOD
};

struct Queue {
    SRWLOCK lock;
    uint32_t* items;
    uint32_t n;
    uint32_t bit;
};

Res* g_res = nullptr;
uint32_t* g_free = nullptr;
uint32_t g_free_n = 0;
std::atomic<uint32_t> g_hi{0};
SRWLOCK g_alloc = SRWLOCK_INIT;
Queue g_up = {SRWLOCK_INIT, nullptr, 0, kQueuedUpload};
Queue g_need = {SRWLOCK_INIT, nullptr, 0, kQueuedNeed};
Queue g_drop = {SRWLOCK_INIT, nullptr, 0, kQueuedDrop};
SRWLOCK g_op = SRWLOCK_INIT;  // one texture operation on the immediate context at a time
thread_local bool t_in_op = false;

// Weak: valid while a managed texture exists (each holds the device).
std::atomic<ID3D11Device*> g_dev{nullptr};
ID3D11DeviceContext* g_imm = nullptr;
ID3D11DeviceContext2* g_ctx2 = nullptr;
ID3D11Device2* g_dev2 = nullptr;
IDXGIDevice3* g_dxgi3 = nullptr;  // weak, like g_dev
// A device created while textures of the current one were still alive:
// compared only, never used, until it creates a texture (then it is alive).
std::atomic<uintptr_t> g_next_dev{0};
// residency_present's cache: the last swapchain and whether it is g_dev's.
std::atomic<IDXGISwapChain*> g_present_sc{nullptr};
std::atomic<bool> g_present_ours{false};
// Present thread: a released pool goes to the driver's free cache, still
// counted as ours; Trim gives it back. Once the GPU is past the evictions.
uint32_t g_trim_after = 0;  // frame from which to trim, 0 = nothing to trim
uint64_t g_trim_qpc = 0;
uint64_t g_trim_bytes = 0;  // freed since the last Trim
std::atomic<CreateTex2DFn> g_create{nullptr};
std::atomic<bool> g_on{false};
std::atomic<uint8_t> g_fmt[256];  // 0 unknown, 1 tiled, 2 not

std::atomic<uint32_t> g_frame{1};
std::atomic<uint64_t> g_pending{0}, g_ram{0};
uint64_t g_ram_cap = 0;
std::atomic<uint32_t> g_live{0}, g_restores{0}, g_evictions{0}, g_late{0};
std::atomic<bool> g_lists_reused{false}, g_evict_off{false}, g_restore_due{false};
uint32_t g_epoch = 0;  // Present thread

// Cold textures picked by the background tick, consumed by Present.
SRWLOCK g_cand_lock = SRWLOCK_INIT;
uint32_t g_cand[kCand];
int g_cand_n = 0, g_cand_pos = 0;
uint32_t g_cand_cutoff = 0;
uint64_t g_want = 0;

void STDMETHODCALLTYPE hk_CopySub(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                  const D3D11_BOX*);
void STDMETHODCALLTYPE hk_CopyRes(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
void STDMETHODCALLTYPE hk_CopySub1(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*,
                                   UINT, const D3D11_BOX*, UINT);
void STDMETHODCALLTYPE hk_SetLod(ID3D11DeviceContext*, ID3D11Resource*, FLOAT);
FLOAT STDMETHODCALLTYPE hk_GetLod(ID3D11DeviceContext*, ID3D11Resource*);
void STDMETHODCALLTYPE hk_Exec(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
void STDMETHODCALLTYPE hk_SwapState(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
HRESULT STDMETHODCALLTYPE hk_CreateSRV1(ID3D11Device3*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC1*,
                                        ID3D11ShaderResourceView1**);
void STDMETHODCALLTYPE hk_GetDesc(ID3D11Texture2D*, D3D11_TEXTURE2D_DESC*);
void STDMETHODCALLTYPE hk_GetDesc1(ID3D11Texture2D1*, D3D11_TEXTURE2D_DESC1*);

VtHook g_copy_sub = {kCtxCopySubresourceRegion, reinterpret_cast<void*>(&hk_CopySub)};
VtHook g_copy_res = {kCtxCopyResource, reinterpret_cast<void*>(&hk_CopyRes)};
HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext2*, ID3D11Resource*, UINT, const D3D11_TILED_RESOURCE_COORDINATE*,
                                 const D3D11_TILE_REGION_SIZE*, ID3D11Buffer*, UINT, const UINT*, const UINT*,
                                 const UINT*, UINT);
HRESULT STDMETHODCALLTYPE hk_CopyMap(ID3D11DeviceContext2*, ID3D11Resource*, const D3D11_TILED_RESOURCE_COORDINATE*,
                                     ID3D11Resource*, const D3D11_TILED_RESOURCE_COORDINATE*,
                                     const D3D11_TILE_REGION_SIZE*, UINT);
HRESULT STDMETHODCALLTYPE hk_ResizePool(ID3D11DeviceContext2*, ID3D11Buffer*, UINT64);
void STDMETHODCALLTYPE hk_Flush(ID3D11DeviceContext*);
VtHook g_map = {kCtx2UpdateTileMappings, reinterpret_cast<void*>(&hk_Map)};
VtHook g_copy_map = {kCtx2CopyTileMappings, reinterpret_cast<void*>(&hk_CopyMap)};
VtHook g_resize_pool = {kCtx2ResizeTilePool, reinterpret_cast<void*>(&hk_ResizePool)};
VtHook g_flush = {kCtxFlush, reinterpret_cast<void*>(&hk_Flush)};
VtHook g_set_lod = {kCtxSetResourceMinLOD, reinterpret_cast<void*>(&hk_SetLod)};
VtHook g_get_lod = {kCtxGetResourceMinLOD, reinterpret_cast<void*>(&hk_GetLod)};
VtHook g_exec = {kCtxExecuteCommandList, reinterpret_cast<void*>(&hk_Exec)};
VtHook g_copy_sub1 = {kCtx1CopySubresourceRegion1, reinterpret_cast<void*>(&hk_CopySub1)};
VtHook g_swap_state = {kCtx1SwapDeviceContextState, reinterpret_cast<void*>(&hk_SwapState)};
VtHook g_srv1 = {kDev3CreateSRV1, reinterpret_cast<void*>(&hk_CreateSRV1)};
VtHook g_get_desc = {kTexGetDesc, reinterpret_cast<void*>(&hk_GetDesc)};
VtHook g_get_desc1 = {kTex1GetDesc1, reinterpret_cast<void*>(&hk_GetDesc1)};

template <class F>
F orig(const VtHook& h, void* self) {
    return reinterpret_cast<F>(vt_orig(h, self));
}

using CopySubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                           ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using MapFn = decltype(&hk_Map);
using SetLodFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, FLOAT);

void copy_top(ID3D11Resource* dst, ID3D11Resource* src) {
    orig<CopySubFn>(g_copy_sub, g_imm)(g_imm, dst, 0, 0, 0, 0, src, 0, nullptr);
}
void set_lod(ID3D11Resource* res, float lod) { orig<SetLodFn>(g_set_lod, g_imm)(g_imm, res, lod); }

void push(Queue& q, uint32_t i) {
    if (g_res[i].flags.fetch_or(q.bit, std::memory_order_acq_rel) & q.bit) return;  // already queued
    AcquireSRWLockExclusive(&q.lock);
    q.items[q.n++] = i;  // each index is in a queue at most once: never overflows
    ReleaseSRWLockExclusive(&q.lock);
}

// Pops one index (its queued bit cleared first, so a later push re-queues it).
bool pop(Queue& q, uint32_t* i) {
    AcquireSRWLockExclusive(&q.lock);
    const bool any = q.n != 0;
    if (any) *i = q.items[--q.n];
    ReleaseSRWLockExclusive(&q.lock);
    if (any) g_res[*i].flags.fetch_and(~q.bit, std::memory_order_acq_rel);
    return any;
}

uint32_t queued(Queue& q) {
    AcquireSRWLockShared(&q.lock);
    const uint32_t n = q.n;
    ReleaseSRWLockShared(&q.lock);
    return n;
}

bool alloc_index(uint32_t* i) {
    AcquireSRWLockExclusive(&g_alloc);
    bool ok = true;
    if (g_free_n)
        *i = g_free[--g_free_n];
    else if (g_hi.load(std::memory_order_relaxed) < kMaxRes)
        *i = g_hi.fetch_add(1, std::memory_order_relaxed);
    else
        ok = false;
    ReleaseSRWLockExclusive(&g_alloc);
    return ok;
}

void free_index(uint32_t i) {
    AcquireSRWLockExclusive(&g_alloc);
    g_free[g_free_n++] = i;
    ReleaseSRWLockExclusive(&g_alloc);
}

// Keeps a managed texture's current pool alive as long as the texture
// itself: views and command lists hold it through internal references the
// public refcount does not show, so drop() may release ours too early.
// Set as the texture's private data; the runtime releases it on destruction.
// {6B1E4C2A-9D37-4F0B-A5E1-3C8D2F7B9A61}
constexpr GUID kPoolGuid = {0x6b1e4c2a, 0x9d37, 0x4f0b, {0xa5, 0xe1, 0x3c, 0x8d, 0x2f, 0x7b, 0x9a, 0x61}};
struct PoolHolder final : IUnknown {
    std::atomic<long> refs{1};
    ID3D11Buffer* pool = nullptr;  // under g_op, or by the last Release
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(refs.fetch_add(1) + 1); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = refs.fetch_sub(1) - 1;
        if (n == 0) {
            if (pool) pool->Release();
            this->~PoolHolder();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return static_cast<ULONG>(n);
    }
};

// Under g_op (or before the texture is published): the holder's pool becomes pool.
void hold_pool(ID3D11Texture2D* tex, ID3D11Buffer* pool) {
    IUnknown* u = nullptr;
    UINT size = sizeof u;
    if (FAILED(tex->GetPrivateData(kPoolGuid, &size, &u)) || !u) return;
    auto* h = static_cast<PoolHolder*>(u);
    pool->AddRef();
    if (h->pool) h->pool->Release();
    h->pool = pool;
    u->Release();  // GetPrivateData added one
}

bool attach_holder(ID3D11Texture2D* tex, ID3D11Buffer* pool) {
    void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(PoolHolder));
    if (!mem) return false;
    auto* h = new (mem) PoolHolder();
    pool->AddRef();
    h->pool = pool;
    const bool ok = SUCCEEDED(tex->SetPrivateDataInterface(kPoolGuid, h));
    h->Release();  // the texture's reference is the one left
    return ok;
}

ID3D11Buffer* make_pool(ID3D11Device* dev, uint32_t tiles) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = tiles * static_cast<UINT>(kTile);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.MiscFlags = D3D11_RESOURCE_MISC_TILE_POOL;
    ID3D11Buffer* b = nullptr;
    InternalCall internal;  // ours, not a game buffer in the session
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &b)) ? b : nullptr;
}

// The tail (standard mips 1.. and the packed ones, walked linearly from
// subresource 1) at pool offset 0, mip 0 after it when with_top; the rest
// unmapped. A texture maps from one pool at a time.
HRESULT map_tiles(ID3D11Texture2D* tex, ID3D11Buffer* pool, uint32_t tiles0, uint32_t tail, bool with_top) {
    D3D11_TILED_RESOURCE_COORDINATE c[2] = {};
    D3D11_TILE_REGION_SIZE s[2] = {};
    UINT flags[2] = {0, with_top ? 0u : static_cast<UINT>(D3D11_TILE_RANGE_NULL)};
    UINT start[2] = {0, tail}, count[2] = {tail, tiles0};
    c[0].Subresource = 1;
    s[0].NumTiles = tail;
    s[1].NumTiles = tiles0;
    const HRESULT hr = orig<MapFn>(g_map, g_imm)(g_ctx2, tex, 2, c, s, pool, 2, flags, start, count, 0);
    // The runtime has just swapped the context's copy methods over our patch.
    residency_check_hooks(g_imm);
    return hr;
}

// Under g_op. Moves the texture to a new pool, with or without mip 0. The
// pool is what holds the memory: drivers keep a shrunk pool's allocation
// whole, only releasing a pool gives it back. Mips 1.. are carried over
// through a scratch texture (GPU copies, a third of mip 0 at most).
bool repool(Res& r, bool with_top) {
    ID3D11Device* dev = g_dev.load(std::memory_order_relaxed);
    ID3D11Buffer* pool = make_pool(dev, with_top ? r.tail + r.tiles0 : r.tail);
    if (!pool) return false;
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = r.width > 1 ? r.width >> 1 : 1;
    sd.Height = r.height > 1 ? r.height >> 1 : 1;
    sd.MipLevels = r.mips - 1;
    sd.ArraySize = 1;
    sd.Format = r.format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture2D* scratch = nullptr;
    if (FAILED(g_create.load(std::memory_order_relaxed)(dev, &sd, nullptr, &scratch))) {
        pool->Release();
        return false;
    }
    // The original is fetched again after map_tiles: the runtime switches it.
    auto copy = orig<CopySubFn>(g_copy_sub, g_imm);
    for (uint32_t m = 1; m < r.mips; ++m) copy(g_imm, scratch, m - 1, 0, 0, 0, r.tex, m, nullptr);
    if (FAILED(map_tiles(r.tex, pool, r.tiles0, r.tail, with_top))) {
        // The old mapping, whose tiles still hold the data.
        map_tiles(r.tex, r.pool, r.tiles0, r.tail, !with_top);
        scratch->Release();
        pool->Release();
        return false;
    }
    copy = orig<CopySubFn>(g_copy_sub, g_imm);
    for (uint32_t m = 1; m < r.mips; ++m) copy(g_imm, r.tex, m, 0, 0, 0, scratch, m - 1, nullptr);
    scratch->Release();
    hold_pool(r.tex, pool);
    r.pool->Release();  // freed once the GPU is past the copies
    r.pool = pool;
    return true;
}


// Under g_op. Evicted -> whole, cheaply, for the bind that needs it: the
// pool grows by mip 0's tiles (a new allocation behind the same pool, mips
// 1.. untouched) and mip 0 is mapped there. Shrinking does not give memory
// back, so evict() still moves the tail to a fresh pool, at Present.
bool grow_top(Res& r) {
    using ResizeFn = decltype(&hk_ResizePool);
    const uint64_t bytes = uint64_t(r.tail + r.tiles0) * kTile;
    HRESULT hr = orig<ResizeFn>(g_resize_pool, g_imm)(g_ctx2, r.pool, bytes);
    residency_check_hooks(g_imm);
    if (FAILED(hr)) return false;
    D3D11_TILED_RESOURCE_COORDINATE c = {};
    D3D11_TILE_REGION_SIZE s = {};
    s.NumTiles = r.tiles0;
    UINT flag = 0, start = r.tail, count = r.tiles0;
    hr = orig<MapFn>(g_map, g_imm)(g_ctx2, r.tex, 1, &c, &s, r.pool, 1, &flag, &start, &count, 0);
    residency_check_hooks(g_imm);
    return SUCCEEDED(hr);  // a failed map leaves the grown pool: no harm, freed with it
}
// Under g_op on the immediate context's thread. Pending -> Resident.
bool upload(Res& r) {
    if (FAILED(map_tiles(r.tex, r.pool, r.tiles0, r.tail, true))) return false;
    orig<CopyResFn>(g_copy_res, g_imm)(g_imm, r.tex, r.staging);
    r.staging->Release();
    r.staging = nullptr;
    g_pending.fetch_sub(r.up_bytes, std::memory_order_relaxed);
    r.state.store(kResident, std::memory_order_release);
    return true;
}

// Under g_op. Evicted -> Resident: a pool with room for the top mip, the
// mip copied back, the clamp lifted. All queued before the caller's draw.
bool restore(Res& r) {
    if (!grow_top(r)) return false;
    copy_top(r.tex, r.staging);
    set_lod(r.tex, r.lod);
    r.staging->Release();
    r.staging = nullptr;
    g_ram.fetch_sub(r.top_bytes, std::memory_order_relaxed);
    r.state.store(kResident, std::memory_order_release);
    g_restores.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// On the immediate context's thread: makes texture i whole now.
void make_whole(uint32_t i) {
    Res& r = g_res[i];
    const bool outer = !t_in_op;
    t_in_op = true;
    AcquireSRWLockExclusive(&g_op);
    const uint32_t st = r.state.load(std::memory_order_acquire);
    if (st == kPending) {
        if (!upload(r)) push(g_up, i);
    } else if (st == kEvicted) {
        if (!restore(r)) {
            g_late.fetch_add(1, std::memory_order_relaxed);
            push(g_need, i);
        }
    }
    ReleaseSRWLockExclusive(&g_op);
    if (outer) t_in_op = false;
    // Our copies ran through the switched-in thunk, which switched back over our patch.
    if (st == kPending || st == kEvicted) residency_check_hooks(g_imm);
}

// Under g_op, Present thread. Resident -> Evicted, unless used since cutoff.
bool evict(uint32_t i, uint32_t cutoff) {
    Res& r = g_res[i];
    if (r.state.load(std::memory_order_acquire) != kResident) return false;
    const uint32_t f = r.flags.load(std::memory_order_relaxed);
    if ((f & kPinned) || ((f & kDeferredBound) && g_lists_reused.load(std::memory_order_relaxed))) return false;
    if (r.epoch.load(std::memory_order_relaxed) == g_epoch) return false;  // bound right now
    if (g_ram.load(std::memory_order_relaxed) + r.top_bytes > g_ram_cap) return false;
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = r.width;
    sd.Height = r.height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = r.format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_create.load(std::memory_order_relaxed)(g_dev.load(std::memory_order_relaxed), &sd, nullptr, &st)))
        return false;
    // Pairs with residency_on_use: a binder stores last then loads state; we
    // store state then load last. One of the two sees the other.
    r.state.store(kBusy, std::memory_order_seq_cst);
    if (r.last.load(std::memory_order_seq_cst) >= cutoff) {
        r.state.store(kResident, std::memory_order_release);
        st->Release();
        return false;
    }
    copy_top(st, r.tex);
    set_lod(r.tex, r.lod > 1.0f ? r.lod : 1.0f);
    if (!repool(r, false)) {
        set_lod(r.tex, r.lod);
        r.state.store(kResident, std::memory_order_release);
        st->Release();
        return false;
    }
    r.staging = st;
    g_ram.fetch_add(r.top_bytes, std::memory_order_relaxed);
    r.state.store(kEvicted, std::memory_order_release);
    g_evictions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Present thread. Frees a texture the game has let go of: only our
// reference is left and no tracked view of it exists.
void drop(uint32_t i) {
    Res& r = g_res[i];
    uint32_t st = r.state.load(std::memory_order_acquire);
    if (st == kFree || st == kBusy) return;
    std::atomic<int>* views = r.views.load(std::memory_order_relaxed);
    if (views && views->load(std::memory_order_relaxed) != 0) return;
    r.tex->AddRef();
    if (r.tex->Release() != 1) return;
    AcquireSRWLockExclusive(&g_op);
    st = r.state.load(std::memory_order_acquire);
    if (st == kPending) g_pending.fetch_sub(r.up_bytes, std::memory_order_relaxed);
    if (st == kEvicted) g_ram.fetch_sub(r.top_bytes, std::memory_order_relaxed);
    workset_clear_res(r.tex);
    ID3D11Texture2D* tex = r.tex;
    ID3D11Buffer* pool = r.pool;
    ID3D11Texture2D* staging = r.staging;
    r.tex = nullptr;
    r.pool = nullptr;
    r.staging = nullptr;
    r.views.store(nullptr, std::memory_order_relaxed);
    r.state.store(kFree, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_op);
    if (staging) staging->Release();
    tex->Release();   // still alive while views or command lists hold it
    pool->Release();  // the texture's holder keeps it until then
    free_index(i);
    g_live.fetch_sub(1, std::memory_order_relaxed);
}

// Restores every evicted texture matching mask (0 = all), now.
void restore_all(uint32_t mask) {
    const uint32_t hi = g_hi.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < hi; ++i) {
        if (g_res[i].state.load(std::memory_order_acquire) != kEvicted) continue;
        if (mask && !(g_res[i].flags.load(std::memory_order_relaxed) & mask)) continue;
        make_whole(i);
    }
}

void drain_need() {
    // Failures re-queue themselves: one pass over what is queued now.
    for (uint32_t n = queued(g_need), i; n && pop(g_need, &i); --n) make_whole(i);
}

void use_resource(ID3D11Resource* res, ID3D11DeviceContext* ctx) {
    if (!res || t_in_op || !g_on.load(std::memory_order_relaxed)) return;
    if (const uint32_t r = workset_texture_res(res)) residency_on_use(r - 1, ctx);
}

// --- context hooks -------------------------------------------------------

// The runtime switches the immediate context's copy methods between two
// versions of its own: on tiled texture creation, tile mappings, swapchain
// creation and Present, and the switched-in one switches back when called.
// Our patch goes each time; put it back right after (a copy in between, on
// another thread, goes unseen).
void rehook_copy(VtHook& h, ID3D11DeviceContext* ctx) {
    if (!g_evict_off.load(std::memory_order_relaxed) && !vt_rehook(h, ctx) && !g_evict_off.exchange(true)) {
        logf("residency: context hooks overwritten; evictions off");
        g_restore_due.store(true, std::memory_order_release);
    }
}

void STDMETHODCALLTYPE hk_CopySub(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z,
                                  ID3D11Resource* src, UINT ss, const D3D11_BOX* box) {
    use_resource(src, self);
    orig<CopySubFn>(g_copy_sub, self)(self, dst, ds, x, y, z, src, ss, box);
    if (self == g_imm) rehook_copy(g_copy_sub, self);
}

void STDMETHODCALLTYPE hk_CopyRes(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src) {
    use_resource(src, self);
    orig<CopyResFn>(g_copy_res, self)(self, dst, src);
    if (self == g_imm) rehook_copy(g_copy_res, self);
}

void STDMETHODCALLTYPE hk_CopySub1(ID3D11DeviceContext1* self, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z,
                                   ID3D11Resource* src, UINT ss, const D3D11_BOX* box, UINT flags) {
    use_resource(src, self);
    using F = decltype(&hk_CopySub1);
    orig<F>(g_copy_sub1, self)(self, dst, ds, x, y, z, src, ss, box, flags);
}

// The game's own clamp is kept; while the top mip is away ours is at least 1.
void STDMETHODCALLTYPE hk_SetLod(ID3D11DeviceContext* self, ID3D11Resource* res, FLOAT lod) {
    const uint32_t r = (!t_in_op && g_on.load(std::memory_order_relaxed)) ? workset_texture_res(res) : 0;
    if (!r) return orig<SetLodFn>(g_set_lod, self)(self, res, lod);
    AcquireSRWLockExclusive(&g_op);
    Res& x = g_res[r - 1];
    x.lod = lod;
    const bool away = x.state.load(std::memory_order_acquire) == kEvicted;
    orig<SetLodFn>(g_set_lod, self)(self, res, away && lod < 1.0f ? 1.0f : lod);
    ReleaseSRWLockExclusive(&g_op);
}

FLOAT STDMETHODCALLTYPE hk_GetLod(ID3D11DeviceContext* self, ID3D11Resource* res) {
    const uint32_t r = (!t_in_op && g_on.load(std::memory_order_relaxed)) ? workset_texture_res(res) : 0;
    if (r) return g_res[r - 1].lod;
    using F = decltype(&hk_GetLod);
    return orig<F>(g_get_lod, self)(self, res);
}

void note_list(ID3D11CommandList* cl) {
    // Lists executed in a later frame than first seen are cached and
    // replayed: textures bound in deferred contexts stay resident from now on.
    // Hashed by address (immediate context thread only); a collision forgets
    // the older list, an address reused by a new list reads as a replay
    // (the safe side).
    constexpr uint32_t kSeen = 4096;
    static ID3D11CommandList* seen[kSeen];
    static uint32_t frame[kSeen];
    if (g_lists_reused.load(std::memory_order_relaxed)) return;
    const uint32_t f = g_frame.load(std::memory_order_relaxed);
    const uint64_t a = reinterpret_cast<uintptr_t>(cl);
    const uint32_t k = static_cast<uint32_t>((a >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kSeen - 1);
    if (seen[k] == cl) {
        if (frame[k] != f && !g_lists_reused.exchange(true)) {
            logf("residency: the game replays command lists; deferred-bound textures stay resident");
            restore_all(kDeferredBound);
        }
        return;
    }
    seen[k] = cl;
    frame[k] = f;
}

void STDMETHODCALLTYPE hk_Exec(ID3D11DeviceContext* self, ID3D11CommandList* cl, BOOL keep) {
    if (self == g_imm && !t_in_op && g_on.load(std::memory_order_relaxed) && g_live.load(std::memory_order_relaxed)) {
        if (cl) note_list(cl);
        drain_need();
    }
    using F = decltype(&hk_Exec);
    orig<F>(g_exec, self)(self, cl, keep);
}

// The game streams through tiled resources of its own: it manages its
// memory, we step back (and the runtime swaps the copy methods we hook on
// each call). Immediate context only, as these methods are.
void game_maps_tiles() {
    if (t_in_op || !g_on.load(std::memory_order_relaxed)) return;
    if (!g_evict_off.exchange(true)) {
        logf("residency: the game maps tiles itself; evictions off");
        restore_all(0);
    }
}

HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext2* self, ID3D11Resource* res, UINT n,
                                 const D3D11_TILED_RESOURCE_COORDINATE* c, const D3D11_TILE_REGION_SIZE* s,
                                 ID3D11Buffer* pool, UINT ranges, const UINT* flags, const UINT* start,
                                 const UINT* count, UINT f) {
    game_maps_tiles();
    const HRESULT hr = orig<MapFn>(g_map, self)(self, res, n, c, s, pool, ranges, flags, start, count, f);
    residency_check_hooks(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_CopyMap(ID3D11DeviceContext2* self, ID3D11Resource* dst,
                                     const D3D11_TILED_RESOURCE_COORDINATE* dc, ID3D11Resource* src,
                                     const D3D11_TILED_RESOURCE_COORDINATE* sc, const D3D11_TILE_REGION_SIZE* s,
                                     UINT f) {
    game_maps_tiles();
    using F = decltype(&hk_CopyMap);
    const HRESULT hr = orig<F>(g_copy_map, self)(self, dst, dc, src, sc, s, f);
    residency_check_hooks(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_ResizePool(ID3D11DeviceContext2* self, ID3D11Buffer* pool, UINT64 bytes) {
    game_maps_tiles();
    using F = decltype(&hk_ResizePool);
    const HRESULT hr = orig<F>(g_resize_pool, self)(self, pool, bytes);
    residency_check_hooks(self);
    return hr;
}

// Flush switches the copy methods too: a copy right after would go unseen.
void STDMETHODCALLTYPE hk_Flush(ID3D11DeviceContext* self) {
    using F = decltype(&hk_Flush);
    orig<F>(g_flush, self)(self);
    if (self == g_imm) residency_check_hooks(self);
}

// A swapped-in state rebinds views we never see bound: stop evicting.
void STDMETHODCALLTYPE hk_SwapState(ID3D11DeviceContext1* self, ID3DDeviceContextState* state,
                                    ID3DDeviceContextState** prev) {
    if (g_on.load(std::memory_order_relaxed) && !g_evict_off.exchange(true)) {
        logf("residency: the game swaps device context states; evictions off");
        if (static_cast<ID3D11DeviceContext*>(self) == g_imm) restore_all(0);
    }
    using F = decltype(&hk_SwapState);
    orig<F>(g_swap_state, self)(self, state, prev);
}

HRESULT STDMETHODCALLTYPE hk_CreateSRV1(ID3D11Device3* self, ID3D11Resource* res,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC1* d, ID3D11ShaderResourceView1** out) {
    void* const ret = _ReturnAddress();
    using F = decltype(&hk_CreateSRV1);
    const HRESULT hr = orig<F>(g_srv1, self)(self, res, d, out);
    if (SUCCEEDED(hr) && out && *out && res && workset_texture_res(res)) {
        if (is_game_code(ret))
            workset_on_srv(res, *out);
        else
            residency_pin_resource(res);
    }
    return hr;
}

// The game created an immutable texture and sees one.
void STDMETHODCALLTYPE hk_GetDesc(ID3D11Texture2D* self, D3D11_TEXTURE2D_DESC* d) {
    using F = decltype(&hk_GetDesc);
    orig<F>(g_get_desc, self)(self, d);
    if (d && (d->MiscFlags & D3D11_RESOURCE_MISC_TILED) && workset_texture_res(self)) {
        d->Usage = D3D11_USAGE_IMMUTABLE;
        d->MiscFlags &= ~static_cast<UINT>(D3D11_RESOURCE_MISC_TILED);
    }
}

void STDMETHODCALLTYPE hk_GetDesc1(ID3D11Texture2D1* self, D3D11_TEXTURE2D_DESC1* d) {
    using F = decltype(&hk_GetDesc1);
    orig<F>(g_get_desc1, self)(self, d);
    if (d && (d->MiscFlags & D3D11_RESOURCE_MISC_TILED) && workset_texture_res(self)) {
        d->Usage = D3D11_USAGE_IMMUTABLE;
        d->MiscFlags &= ~static_cast<UINT>(D3D11_RESOURCE_MISC_TILED);
        d->TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    }
}

bool hook_context(ID3D11DeviceContext* ctx, bool immediate) {
    const bool re = !immediate;  // deferred contexts' heap vtables get reused
    bool ok = vt_hook(g_copy_sub, ctx, re) && vt_hook(g_copy_res, ctx, re) && vt_hook(g_set_lod, ctx, re) &&
              vt_hook(g_get_lod, ctx, re);
    if (immediate) ok = ok && vt_hook(g_exec, ctx) && vt_hook(g_map, ctx) && vt_hook(g_copy_map, ctx) &&
                        vt_hook(g_resize_pool, ctx) && vt_hook(g_flush, ctx);
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c1)))) {
        ok = ok && vt_hook(g_copy_sub1, c1, re) && vt_hook(g_swap_state, c1, re);
        c1->Release();
    }
    return ok;
}

bool tiled_format(ID3D11Device* dev, DXGI_FORMAT f) {
    const unsigned v = static_cast<unsigned>(f);
    if (v >= 256) return false;
    uint8_t k = g_fmt[v].load(std::memory_order_relaxed);
    if (!k) {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s = {f, 0};
        const bool yes = SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s, sizeof s)) &&
                         (s.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_TILED);
        k = yes ? 1 : 2;
        g_fmt[v].store(k, std::memory_order_relaxed);
    }
    return k == 1;
}
// Off, and nothing left pointing at the device (no managed texture holds it).
void forget_device() {
    g_on.store(false, std::memory_order_release);
    g_dev.store(nullptr, std::memory_order_release);
    g_imm = nullptr;
    g_ctx2 = nullptr;
    g_dev2 = nullptr;
    g_dxgi3 = nullptr;
    g_trim_after = 0;
}
}  // namespace

void residency_on_device(ID3D11Device* dev) {
    if (!config().mip_residency) return;
    if (g_live.load(std::memory_order_acquire) != 0) {
        // The previous device's textures still hold it: this one takes over
        // once they are gone (residency_present, then residency_create).
        if (g_next_dev.exchange(reinterpret_cast<uintptr_t>(dev)) != reinterpret_cast<uintptr_t>(dev))
            logf("residency: new device, waiting for the previous one's textures to go");
        return;
    }
    g_next_dev.store(0, std::memory_order_relaxed);
    // No managed texture holds the previous device: forget it before trying
    // this one, so a failure below leaves nothing pointing at a freed device.
    forget_device();
    D3D11_FEATURE_DATA_D3D11_OPTIONS1 o1 = {};
    if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &o1, sizeof o1)) ||
        o1.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2) {
        logf("residency: off (tiled resources tier %u, needs 2)", static_cast<unsigned>(o1.TiledResourcesTier));
        return;
    }
    ID3D11Device2* dev2 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev2)))) return;
    ID3D11DeviceContext2* ctx2 = nullptr;
    dev2->GetImmediateContext2(&ctx2);
    if (!ctx2) {
        dev2->Release();
        return;
    }
    if (!g_res) {
        const size_t bytes = kMaxRes * sizeof(Res) + 4 * kMaxRes * sizeof(uint32_t);
        auto* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!p) {
            logf("residency: no memory for the tables, off");
            ctx2->Release();
            dev2->Release();
            return;
        }
        g_res = reinterpret_cast<Res*>(p);
        auto* q = reinterpret_cast<uint32_t*>(p + kMaxRes * sizeof(Res));
        g_free = q;
        g_up.items = q + kMaxRes;
        g_need.items = q + 2 * kMaxRes;
        g_drop.items = q + 3 * kMaxRes;
    }
    bool ok = hook_context(ctx2, true);
    ID3D11Device3* dev3 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3)))) {
        ok = vt_hook(g_srv1, dev3) && ok;
        dev3->Release();
    }
    if (!ok) {
        logf("residency: context hooks FAILED, off");
        ctx2->Release();
        dev2->Release();
        return;
    }
    uint64_t cap = static_cast<uint64_t>(config().mip_ram_mb) << 20;
    if (!cap) {
        MEMORYSTATUSEX ms = {sizeof ms};
        GlobalMemoryStatusEx(&ms);
        cap = std::min<uint64_t>(4096ull << 20, ms.ullTotalPhys / 16);
    }
    g_ram_cap = cap;
    g_imm = ctx2;
    g_ctx2 = ctx2;
    g_dev2 = dev2;
    IDXGIDevice3* dxgi3 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi3)))) {
        g_dxgi3 = dxgi3;
        dxgi3->Release();
    }
    g_dev.store(dev, std::memory_order_release);
    g_present_sc.store(nullptr, std::memory_order_relaxed);
    // Weak from here on (see g_dev).
    ctx2->Release();
    dev2->Release();
    g_on.store(true, std::memory_order_release);
    logf("residency: on, system memory cap %llu MB%s", static_cast<unsigned long long>(cap >> 20),
         config().mip_force_evict ? ", force evict (test)" : "");
}

bool residency_create(ID3D11Device* dev, CreateTex2DFn create, const D3D11_TEXTURE2D_DESC& d,
                      const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out, uint64_t bytes) {
    // The device waiting to take over creates a texture: it is alive now.
    if (uintptr_t next = g_next_dev.load(std::memory_order_relaxed);
        next == reinterpret_cast<uintptr_t>(dev) && !g_on.load(std::memory_order_acquire) &&
        g_next_dev.compare_exchange_strong(next, 0))
        residency_on_device(dev);
    if (!g_on.load(std::memory_order_acquire) || dev != g_dev.load(std::memory_order_relaxed) ||
        g_evict_off.load(std::memory_order_relaxed))
        return false;
    if (d.Usage != D3D11_USAGE_IMMUTABLE || d.BindFlags != D3D11_BIND_SHADER_RESOURCE || d.CPUAccessFlags ||
        d.MiscFlags || d.SampleDesc.Count != 1 || d.ArraySize != 1 || !init || !out)
        return false;
    if (d.MipLevels == 1 || (d.Width < 2 && d.Height < 2)) return false;
    D3D11_TEXTURE2D_DESC top_desc = d;
    top_desc.MipLevels = 1;
    const uint64_t top_bytes = texture2d_bytes(top_desc);
    if (top_bytes < kMinTop || !tiled_format(dev, d.Format)) return false;
    if (g_pending.fetch_add(bytes, std::memory_order_relaxed) + bytes > kPendingCap) {
        g_pending.fetch_sub(bytes, std::memory_order_relaxed);
        return false;
    }
    uint32_t i;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11Buffer* pool = nullptr;
    D3D11_TEXTURE2D_DESC got = {};
    std::atomic<int>* views = nullptr;
    UINT total = 0, n_sub = 1, tiles0 = 0;
    D3D11_PACKED_MIP_DESC packed = {};
    D3D11_TILE_SHAPE shape = {};
    D3D11_SUBRESOURCE_TILING sub0 = {};
    D3D11_TEXTURE2D_DESC sd = d, td = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.MiscFlags = D3D11_RESOURCE_MISC_TILED;
    if (!alloc_index(&i)) goto fail_pending;
    // The data now (the game may free it on return); the upload when the
    // immediate context is ours to use.
    if (FAILED(create(dev, &sd, init, &staging))) goto fail;
    if (FAILED(create(dev, &td, nullptr, &tex))) goto fail;
    residency_check_hooks(g_imm);
    g_dev2->GetResourceTiling(tex, &total, &packed, &shape, &n_sub, 0, &sub0);
    tiles0 = sub0.WidthInTiles * sub0.HeightInTiles * sub0.DepthInTiles;
    if (packed.NumStandardMips < 1 || n_sub != 1 || tiles0 < 4 || total <= tiles0) goto fail;
    tex->GetDesc(&got);  // the mip count when the game asked for a full chain
    pool = make_pool(dev, total);  // memory committed when the tiles are mapped
    if (!pool || !attach_holder(tex, pool)) goto fail;
    views = workset_on_texture(tex, bytes, i + 1);
    if (!views) goto fail;
    {
        Res& r = g_res[i];
        r.tex = tex;
        r.pool = pool;
        r.staging = staging;
        r.tiles0 = tiles0;
        r.tail = total - tiles0;
        r.up_bytes = bytes;
        r.top_bytes = top_bytes;
        r.width = d.Width;
        r.height = d.Height;
        r.mips = got.MipLevels;
        r.format = d.Format;
        r.lod = 0.0f;
        r.views.store(views, std::memory_order_relaxed);
        r.epoch.store(0, std::memory_order_relaxed);
        r.last.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
        // Queued bits survive index reuse: the queues still hold the index.
        r.flags.fetch_and(kQueued, std::memory_order_relaxed);
        r.state.store(kPending, std::memory_order_release);
    }
    g_live.fetch_add(1, std::memory_order_relaxed);
    g_create.store(create, std::memory_order_relaxed);
    vt_hook(g_get_desc, tex);
    {
        ID3D11Texture2D1* t1 = nullptr;
        if (SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&t1)))) {
            vt_hook(g_get_desc1, t1);
            t1->Release();
        }
    }
    push(g_up, i);
    tex->AddRef();  // ours: the texture lives until drop() sees the game let go
    *out = tex;
    return true;

fail:
    if (pool) pool->Release();
    if (tex) tex->Release();
    if (staging) staging->Release();
    free_index(i);
fail_pending:
    g_pending.fetch_sub(bytes, std::memory_order_relaxed);
    return false;
}

void residency_on_use(uint32_t i, ID3D11DeviceContext* ctx) {
    if (t_in_op || i >= kMaxRes) return;
    Res& r = g_res[i];
    const uint32_t f = g_frame.load(std::memory_order_relaxed);
    if (r.last.load(std::memory_order_relaxed) != f) r.last.store(f, std::memory_order_seq_cst);
    const uint32_t st = r.state.load(std::memory_order_seq_cst);
    const bool deferred = ctx != g_imm;
    if (deferred && !(r.flags.load(std::memory_order_relaxed) & kDeferredBound))
        r.flags.fetch_or(kDeferredBound, std::memory_order_relaxed);
    if (st == kResident || st == kFree) return;
    // A deferred context cannot touch the tiles: whole before the list runs.
    if (deferred)
        push(g_need, i);
    else
        make_whole(i);
}

void residency_after_present() { residency_check_hooks(g_imm); }

void residency_check_hooks(ID3D11DeviceContext* imm) {
    // Residency relies on the meter's bind hooks too.
    const bool binds = workset_check_hooks(imm);
    // Also once evictions are off: what is still evicted needs its copies seen.
    if (!g_on.load(std::memory_order_acquire) || imm != g_imm) return;
    bool ok = binds;
    for (VtHook* h : {&g_copy_sub, &g_copy_res, &g_set_lod, &g_get_lod, &g_exec, &g_copy_sub1, &g_swap_state})
        if (h->n) ok = vt_rehook(*h, g_imm) && ok;
    if (!ok && !g_evict_off.exchange(true)) {
        logf("residency: context hooks overwritten; evictions off");
        g_restore_due.store(true, std::memory_order_release);
    }
}

void residency_on_context(ID3D11DeviceContext* ctx) {
    if (!g_on.load(std::memory_order_acquire) || (ctx && hook_context(ctx, false))) return;
    // Its binds and copies go unseen: stop evicting, restore at the next Present.
    if (!g_evict_off.exchange(true)) {
        logf("residency: deferred context hooks FAILED; evictions off");
        g_restore_due.store(true, std::memory_order_release);
    }
}

void residency_pin(uint32_t i) {
    if (!g_res || i >= kMaxRes) return;
    g_res[i].flags.fetch_or(kPinned, std::memory_order_relaxed);
    if (g_res[i].state.load(std::memory_order_acquire) == kEvicted) push(g_need, i);
}

void residency_pin_resource(ID3D11Resource* res) {
    if (!g_on.load(std::memory_order_acquire)) return;
    if (const uint32_t r = workset_texture_res(res)) residency_pin(r - 1);
}

static void trim_if_due();

bool residency_present(IDXGISwapChain* sc) {
    if (!g_on.load(std::memory_order_acquire) || !g_live.load(std::memory_order_relaxed) || t_in_op) return false;
    if (g_present_sc.load(std::memory_order_relaxed) != sc) {
        ID3D11Device* d = nullptr;
        bool ours = false;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&d)))) {
            ours = d == g_dev.load(std::memory_order_relaxed);
            d->Release();
        }
        g_present_ours.store(ours, std::memory_order_relaxed);
        g_present_sc.store(sc, std::memory_order_relaxed);
    }
    // Drops need no context: on any Present, so textures of a device the game
    // replaced (and that device with them) are freed once the game lets go.
    uint32_t i;
    for (int k = 0; k < kDropPerFrame && pop(g_drop, &i); ++k) drop(i);
    if (g_live.load(std::memory_order_acquire) == 0 && g_next_dev.load(std::memory_order_relaxed)) {
        // The last one is gone and the previous device may be with it.
        forget_device();
        logf("residency: previous device's textures gone, the new device takes over");
        return false;
    }
    if (!g_present_ours.load(std::memory_order_relaxed)) return false;
    trim_if_due();

    drain_need();
    uint64_t up = 0;
    while (up < kUploadPerFrame && pop(g_up, &i)) {
        if (g_res[i].state.load(std::memory_order_acquire) != kPending) continue;
        up += g_res[i].up_bytes;
        make_whole(i);
    }
    if (g_evict_off.load(std::memory_order_relaxed)) {
        if (g_restore_due.exchange(false)) restore_all(0);
        return true;
    }

    // What to evict: every texture not bound this frame (test), or the
    // coldest ones the tick picked while VRAM is short.
    const bool force = config().mip_force_evict;
    uint32_t cand[kCand];
    int n = 0;
    uint32_t cutoff = 0;
    uint64_t want = 0;
    if (force) {
        cutoff = g_frame.load(std::memory_order_relaxed);
        want = ~0ull;
    } else if (TryAcquireSRWLockExclusive(&g_cand_lock)) {
        while (g_cand_pos < g_cand_n && n < kEvictPerFrame) cand[n++] = g_cand[g_cand_pos++];
        cutoff = g_cand_cutoff;
        want = g_want;
        ReleaseSRWLockExclusive(&g_cand_lock);
    }
    if (!force && (!n || !want)) return true;

    // Views bound right now may not be bound again for many frames.
    ++g_epoch;
    ID3D11ShaderResourceView* bound[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    constexpr UINT kSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    for (int stage = 0; stage < 6; ++stage) {
        switch (stage) {
            case 0: g_imm->PSGetShaderResources(0, kSlots, bound); break;
            case 1: g_imm->VSGetShaderResources(0, kSlots, bound); break;
            case 2: g_imm->GSGetShaderResources(0, kSlots, bound); break;
            case 3: g_imm->HSGetShaderResources(0, kSlots, bound); break;
            case 4: g_imm->DSGetShaderResources(0, kSlots, bound); break;
            default: g_imm->CSGetShaderResources(0, kSlots, bound); break;
        }
        for (ID3D11ShaderResourceView* v : bound) {
            if (!v) continue;
            if (const uint32_t r = workset_view_res(v)) g_res[r - 1].epoch.store(g_epoch, std::memory_order_relaxed);
            v->Release();
        }
    }

    t_in_op = true;
    uint64_t freed = 0;
    int done = 0;
    auto one = [&](uint32_t j) {
        AcquireSRWLockExclusive(&g_op);
        if (evict(j, cutoff)) {
            freed += g_res[j].top_bytes;
            ++done;
        }
        ReleaseSRWLockExclusive(&g_op);
    };
    if (force) {
        const uint32_t hi = g_hi.load(std::memory_order_acquire);
        for (uint32_t j = 0; j < hi; ++j) one(j);
        if (done) logf("residency: force evict %d (%llu MB)", done, static_cast<unsigned long long>(freed >> 20));
    } else {
        for (int k = 0; k < n && freed < want && freed < kEvictPerFrameBytes; ++k) one(cand[k]);
        AcquireSRWLockExclusive(&g_cand_lock);
        g_want = g_want > freed ? g_want - freed : 0;
        ReleaseSRWLockExclusive(&g_cand_lock);
    }
    t_in_op = false;
    residency_check_hooks(g_imm);  // our copies switched the copy methods back
    g_trim_bytes += freed;
    if (done) g_trim_after = g_frame.load(std::memory_order_relaxed) + 3;  // past the frame latency
    return true;
}

static void trim_if_due() {
    if (!g_trim_after || !g_dxgi3 || g_frame.load(std::memory_order_relaxed) < g_trim_after) return;
    const bool test = config().mip_force_evict;
    // Worth about a millisecond (plus the driver refilling its caches) only
    // for a sizeable amount: smaller amounts wait for more.
    constexpr uint64_t kTrimMin = 128ull << 20;
    if (!test && g_trim_bytes < kTrimMin) return;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    // At most once every 2 s; every time in tests.
    if (!test && static_cast<uint64_t>(t.QuadPart) - g_trim_qpc < 2 * static_cast<uint64_t>(f.QuadPart)) return;
    g_trim_qpc = static_cast<uint64_t>(t.QuadPart);
    g_trim_after = 0;
    g_trim_bytes = 0;
    g_dxgi3->Trim();
}

void residency_frame() { g_frame.fetch_add(1, std::memory_order_relaxed); }

void residency_background_tick() {
    if (!g_on.load(std::memory_order_acquire)) return;
    static uint32_t ring[10];
    static unsigned ticks = 0;
    static uint32_t last_restores = 0, last_evictions = 0;
    static struct Cold {
        uint32_t last, i;
    } cold[kMaxRes];
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    if (ticks == 0)
        for (uint32_t& r : ring) r = now;
    const uint32_t cutoff = ring[ticks % 10];  // the frame counter 10 s ago
    ring[ticks % 10] = now;
    ++ticks;

    // VRAM is short above 95% of the budget; evict down to 90%.
    uint64_t usage = 0, budget = 0, want = 0;
    if (memory_local(&usage, &budget) && usage > budget / 100 * 95) want = usage - budget / 100 * 90;

    uint32_t alive = 0, away = 0, nc = 0;
    const uint32_t hi = g_hi.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < hi; ++i) {
        Res& r = g_res[i];
        const uint32_t st = r.state.load(std::memory_order_acquire);
        if (st == kFree) continue;
        ++alive;
        if (st == kEvicted) ++away;
        std::atomic<int>* views = r.views.load(std::memory_order_relaxed);
        if (views && views->load(std::memory_order_relaxed) == 0) push(g_drop, i);
        const uint32_t last = r.last.load(std::memory_order_relaxed);
        if (want && st == kResident && last < cutoff && !(r.flags.load(std::memory_order_relaxed) & kPinned))
            cold[nc++] = {last, i};
    }
    const uint32_t keep = nc < kCand ? nc : kCand;
    if (keep < nc)
        std::nth_element(cold, cold + keep, cold + nc, [](const Cold& a, const Cold& b) { return a.last < b.last; });
    std::sort(cold, cold + keep, [](const Cold& a, const Cold& b) { return a.last < b.last; });
    AcquireSRWLockExclusive(&g_cand_lock);
    for (uint32_t k = 0; k < keep; ++k) g_cand[k] = cold[k].i;
    g_cand_n = static_cast<int>(keep);
    g_cand_pos = 0;
    g_cand_cutoff = cutoff;
    g_want = want;
    ReleaseSRWLockExclusive(&g_cand_lock);

    const uint32_t restores = g_restores.load(std::memory_order_relaxed);
    const uint32_t evictions = g_evictions.load(std::memory_order_relaxed);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    TesseraRecord rec = {};
    rec.qpc = static_cast<uint64_t>(t.QuadPart);
    rec.kind = TESSERA_REC_RESIDENCY;
    rec.a = alive;
    rec.b = away;
    rec.c = restores - last_restores;
    rec.d = evictions - last_evictions;
    rec.flags = g_late.load(std::memory_order_relaxed);
    rec.bytes = g_ram.load(std::memory_order_relaxed);
    session_push(rec);
    last_restores = restores;
    last_evictions = evictions;
}
}  // namespace tessera
