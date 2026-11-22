// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "mip_feedback.h"
#include "module.h"
#include "session.h"
#include "tessera/shader_store_format.h"
#include "vtable.h"

namespace tessera {
namespace {
// ID3D11Device vtable slots.
constexpr int kDevCreateVS = 12;
constexpr int kDevCreateGS = 13;
constexpr int kDevCreateGSSO = 14;
constexpr int kDevCreatePS = 15;
constexpr int kDevCreateHS = 16;
constexpr int kDevCreateDS = 17;
constexpr int kDevCreateCS = 18;

thread_local int t_depth = 0;
struct Depth {
    Depth() { ++t_depth; }
    ~Depth() { --t_depth; }
    bool outer() const { return t_depth == 1; }
};

// Hashes of stored (or this session's) shaders: open addressing, lock-free
// insert. Past 3/4 full nothing is inserted and every shader counts as new.
constexpr uint32_t kSetCap = 1u << 18;  // 2 MB
std::atomic<uint64_t>* g_set = nullptr;
std::atomic<uint32_t> g_set_count{0};

// Bytecode waiting for the writer: two preallocated buffers, producers append
// to the active one under a short lock (memcpy only), the writer swaps them.
constexpr size_t kBufBytes = 8u << 20;
struct Buf {
    uint8_t* p = nullptr;
    size_t used = 0;
};
Buf g_buf[2];
int g_active = 0;
SRWLOCK g_buf_lock = SRWLOCK_INIT;
std::atomic<uint32_t> g_dropped{0};

HANDLE g_store = INVALID_HANDLE_VALUE;
HANDLE g_index = INVALID_HANDLE_VALUE;
uint64_t g_store_end = 0;
TesseraShaderIndexEntry g_idx[4096];  // writer only
std::atomic<bool> g_store_ok{false};
INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

uint64_t now_qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

uint64_t qpc_freq() {
    static const uint64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return static_cast<uint64_t>(v.QuadPart);
    }();
    return f;
}

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Hash and container flags. A DXBC container carries a 16-byte checksum of
// its contents: its first 8 bytes are the hash, free of cost.
uint16_t identify(const void* code, size_t n, uint64_t* hash) {
    const uint8_t* b = static_cast<const uint8_t*>(code);
    uint16_t flags = 0;
    uint64_t h = 0;
    if (n >= 32 && memcmp(b, "DXBC", 4) == 0) {
        flags |= TESSERA_SHADER_DXBC;
        memcpy(&h, b + 4, 8);
        uint32_t parts = 0;
        memcpy(&parts, b + 28, 4);
        if (parts < 64 && 32 + 4ull * parts <= n) {
            for (uint32_t i = 0; i < parts; ++i) {
                uint32_t off = 0;
                memcpy(&off, b + 32 + 4 * i, 4);
                if (off + 8ull <= n && memcmp(b + off, "DXIL", 4) == 0) flags |= TESSERA_SHADER_DXIL;
            }
        }
    }
    if (h == 0) h = fnv(b, n);
    *hash = h ? h : 1;
    return flags;
}

// True when h was not in the set (and is now).
bool set_insert(uint64_t h) {
    if (!g_set || g_set_count.load(std::memory_order_relaxed) >= kSetCap / 4 * 3) return true;
    uint32_t i = static_cast<uint32_t>(h ^ (h >> 32)) & (kSetCap - 1);
    for (int probe = 0; probe < 256; ++probe, i = (i + 1) & (kSetCap - 1)) {
        uint64_t cur = g_set[i].load(std::memory_order_relaxed);
        if (cur == h) return false;
        if (cur == 0) {
            if (g_set[i].compare_exchange_strong(cur, h, std::memory_order_relaxed)) {
                g_set_count.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (cur == h) return false;
        }
    }
    return true;
}

void append_blob(uint64_t hash, uint16_t stage, uint16_t flags, const void* code, size_t n) {
    const size_t need = sizeof(TesseraShaderEntry) + ((n + 7) & ~size_t{7});
    if (!g_buf[0].p || need > kBufBytes / 4) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TesseraShaderEntry e = {hash, static_cast<uint32_t>(n), stage, flags};
    AcquireSRWLockExclusive(&g_buf_lock);
    Buf& b = g_buf[g_active];
    if (b.used + need <= kBufBytes) {
        memcpy(b.p + b.used, &e, sizeof e);
        memcpy(b.p + b.used + sizeof e, code, n);
        memset(b.p + b.used + sizeof e + n, 0, need - sizeof e - n);
        b.used += need;
    } else {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_buf_lock);
}

// Shared by all stages: time the real call, record it, keep new bytecode.
void after_create(uint16_t stage, const void* code, SIZE_T n, uint64_t t0, void* ret) {
    const uint64_t t1 = now_qpc();
    uint64_t hash = 0;
    uint16_t flags = identify(code, n, &hash);
    TesseraRecord r = {};
    r.qpc = t1;
    r.kind = TESSERA_REC_SHADER;
    r.a = stage;
    r.b = static_cast<uint32_t>(n);
    const uint64_t us = (t1 - t0) * 1000000u / qpc_freq();
    r.c = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
    r.bytes = hash;
    if (!is_game_code(ret)) {
        r.flags = TESSERA_FLAG_FOREIGN;  // overlays and SDKs: not ours to warm
    } else if (set_insert(hash)) {
        if (g_store_ok.load(std::memory_order_acquire)) r.d |= TESSERA_SHADER_NEW;
        // Stream-output GS needs its declaration to be recreated: not stored.
        if (stage != TESSERA_STAGE_GS_SO) append_blob(hash, stage, flags, code, n);
    }
    r.d |= flags;
    session_push(r);
}

template <class F>
F orig(const VtHook& h, void* self) {
    return reinterpret_cast<F>(vt_orig(h, self));
}

template <class T>
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, T**);

template <uint16_t Stage, VtHook& H, class T>
HRESULT STDMETHODCALLTYPE hk_create(ID3D11Device* self, const void* code, SIZE_T n, ID3D11ClassLinkage* link,
                                    T** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    const uint64_t t0 = now_qpc();
    const auto create = orig<CreateFn<T>>(H, self);
    const HRESULT hr = create(self, code, n, link, out);
    if (SUCCEEDED(hr) && code && n && depth.outer()) {
        after_create(Stage, code, n, t0, ret);
        if constexpr (Stage == TESSERA_STAGE_PS) {
            if (!link && out && *out && is_game_code(ret)) feedback_on_shader(self, create, *out, code, n);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_create_gs_so(ID3D11Device*, const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT,
                                          const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**);

extern VtHook g_vs, g_hs, g_ds, g_gs, g_ps, g_cs, g_gs_so;
VtHook g_vs = {kDevCreateVS, reinterpret_cast<void*>(&hk_create<TESSERA_STAGE_VS, g_vs, ID3D11VertexShader>)};
VtHook g_hs = {kDevCreateHS, reinterpret_cast<void*>(&hk_create<TESSERA_STAGE_HS, g_hs, ID3D11HullShader>)};
VtHook g_ds = {kDevCreateDS, reinterpret_cast<void*>(&hk_create<TESSERA_STAGE_DS, g_ds, ID3D11DomainShader>)};
VtHook g_gs = {kDevCreateGS, reinterpret_cast<void*>(&hk_create<TESSERA_STAGE_GS, g_gs, ID3D11GeometryShader>)};
VtHook g_ps = {kDevCreatePS, reinterpret_cast<void*>(&hk_create<TESSERA_STAGE_PS, g_ps, ID3D11PixelShader>)};
VtHook g_cs = {kDevCreateCS, reinterpret_cast<void*>(&hk_create<TESSERA_STAGE_CS, g_cs, ID3D11ComputeShader>)};
VtHook g_gs_so = {kDevCreateGSSO, reinterpret_cast<void*>(&hk_create_gs_so)};

HRESULT STDMETHODCALLTYPE hk_create_gs_so(ID3D11Device* self, const void* code, SIZE_T n,
                                          const D3D11_SO_DECLARATION_ENTRY* decl, UINT ndecl, const UINT* strides,
                                          UINT nstrides, UINT rast, ID3D11ClassLinkage* link,
                                          ID3D11GeometryShader** out) {
    void* const ret = _ReturnAddress();
    Depth depth;
    const uint64_t t0 = now_qpc();
    using F = decltype(&hk_create_gs_so);
    const HRESULT hr = orig<F>(g_gs_so, self)(self, code, n, decl, ndecl, strides, nstrides, rast, link, out);
    if (SUCCEEDED(hr) && code && n && depth.outer()) after_create(TESSERA_STAGE_GS_SO, code, n, t0, ret);
    return hr;
}

bool read_all(HANDLE f, void* p, DWORD n) {
    DWORD r = 0;
    return ReadFile(f, p, n, &r, nullptr) && r == n;
}

bool write_all(HANDLE f, const void* p, DWORD n) {
    DWORD w = 0;
    return WriteFile(f, p, n, &w, nullptr) && w == n;
}

bool seek(HANDLE f, uint64_t at) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(at);
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) != 0;
}

uint64_t file_size(HANDLE f) {
    LARGE_INTEGER li;
    return GetFileSizeEx(f, &li) ? static_cast<uint64_t>(li.QuadPart) : 0;
}

// Opens (or creates) a file with our header; false if the header is foreign.
bool open_with_header(const wchar_t* path, const char* magic, uint32_t entry_size, HANDLE* out) {
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    TesseraShaderHeader h = {};
    if (file_size(f) == 0) {
        memcpy(h.magic, magic, sizeof h.magic);
        h.version = TESSERA_SHADER_VERSION;
        h.header_size = sizeof h;
        h.entry_size = entry_size;
        if (!write_all(f, &h, sizeof h)) {
            CloseHandle(f);
            return false;
        }
    } else if (!read_all(f, &h, sizeof h) || memcmp(h.magic, magic, sizeof h.magic) != 0 ||
               h.version != TESSERA_SHADER_VERSION || h.header_size != sizeof h || h.entry_size != entry_size) {
        CloseHandle(f);
        return false;
    }
    *out = f;
    return true;
}

// Loads the index into the set and cuts a crash-shortened tail off both files.
BOOL CALLBACK init_store(PINIT_ONCE, void*, void**) {
    g_set = static_cast<std::atomic<uint64_t>*>(
        VirtualAlloc(nullptr, kSetCap * sizeof(uint64_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t* bufs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * kBufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (bufs) {
        g_buf[0].p = bufs;
        g_buf[1].p = bufs + kBufBytes;
    }
    wchar_t store[MAX_PATH], index[MAX_PATH];
    if (!g_set || !bufs || !session_data_path(L"shaders", L"tshd", store) ||
        !session_data_path(L"shaders", L"tshi", index)) {
        logf("shaders: store unavailable");
        return TRUE;
    }
    if (!open_with_header(store, TESSERA_SHADER_MAGIC, sizeof(TesseraShaderEntry), &g_store) ||
        !open_with_header(index, TESSERA_SHADER_INDEX_MAGIC, sizeof(TesseraShaderIndexEntry), &g_index)) {
        logf("shaders: cannot open the store (in use or unknown format): %ls", store);
        if (g_store != INVALID_HANDLE_VALUE) CloseHandle(g_store);
        g_store = INVALID_HANDLE_VALUE;
        return TRUE;
    }
    const uint64_t store_size = file_size(g_store);
    const uint64_t n = (file_size(g_index) - sizeof(TesseraShaderHeader)) / sizeof(TesseraShaderIndexEntry);
    uint64_t valid = 0, end = sizeof(TesseraShaderHeader);
    bool cut = false;
    while (valid < n && !cut) {
        const DWORD chunk = static_cast<DWORD>(n - valid < 4096 ? n - valid : 4096);
        if (!read_all(g_index, g_idx, chunk * sizeof(TesseraShaderIndexEntry))) break;
        for (DWORD i = 0; i < chunk; ++i) {
            if (g_idx[i].end <= end || g_idx[i].end > store_size) {
                cut = true;
                break;
            }
            end = g_idx[i].end;
            set_insert(g_idx[i].hash);
            ++valid;
        }
    }
    seek(g_store, end);
    SetEndOfFile(g_store);
    seek(g_index, sizeof(TesseraShaderHeader) + valid * sizeof(TesseraShaderIndexEntry));
    SetEndOfFile(g_index);
    g_store_end = end;
    g_store_ok.store(true, std::memory_order_release);
    logf("shaders: store has %llu shaders (%llu KB)%s", valid, end / 1024,
         store_size > end ? ", cut a partial tail" : "");
    return TRUE;
}

// Writer thread (or exit): appends one buffer to the store, then its index.
void write_buffer(const Buf& b) {
    if (!g_store_ok.load(std::memory_order_acquire) || b.used == 0) return;
    if (!write_all(g_store, b.p, static_cast<DWORD>(b.used))) {
        logf("shaders: store write failed (%lu), store off", GetLastError());
        g_store_ok.store(false, std::memory_order_release);
        return;
    }
    size_t at = 0;
    int k = 0;
    while (at + sizeof(TesseraShaderEntry) <= b.used) {
        TesseraShaderEntry e;
        memcpy(&e, b.p + at, sizeof e);
        at += sizeof e + ((e.size + 7) & ~size_t{7});
        g_idx[k++] = {e.hash, g_store_end + at};
        if (k == 4096 || at >= b.used) {
            write_all(g_index, g_idx, static_cast<DWORD>(k * sizeof(TesseraShaderIndexEntry)));
            k = 0;
        }
    }
    g_store_end += b.used;
}
}  // namespace

void shaders_on_device(ID3D11Device* dev) {
    InitOnceExecuteOnce(&g_init_once, init_store, nullptr, nullptr);
    const bool ok = vt_hook(g_vs, dev) && vt_hook(g_hs, dev) && vt_hook(g_ds, dev) && vt_hook(g_gs, dev) &&
                    vt_hook(g_ps, dev) && vt_hook(g_cs, dev) && vt_hook(g_gs_so, dev);
    if (!ok) logf("shaders: device hooks FAILED");
}

void shaders_background_tick() {
    if (!g_store_ok.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_buf_lock);
    const int full = g_active;
    if (g_buf[full].used == 0) {
        ReleaseSRWLockExclusive(&g_buf_lock);
        return;
    }
    g_active ^= 1;
    ReleaseSRWLockExclusive(&g_buf_lock);
    write_buffer(g_buf[full]);
    g_buf[full].used = 0;
}

void shaders_shutdown() {
    shaders_background_tick();
    shaders_background_tick();
    const uint32_t dropped = g_dropped.load(std::memory_order_relaxed);
    if (dropped) logf("shaders: %u bytecode blobs dropped (buffer full or too big)", dropped);
    if (g_store != INVALID_HANDLE_VALUE) CloseHandle(g_store);
    if (g_index != INVALID_HANDLE_VALUE) CloseHandle(g_index);
    g_store = g_index = INVALID_HANDLE_VALUE;
    g_store_ok.store(false, std::memory_order_release);
}
}  // namespace tessera
