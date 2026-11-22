// SPDX-License-Identifier: GPL-3.0-or-later
#include "dedup.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "texture_data.h"

namespace tessera {
namespace {
constexpr uint64_t kMaxTexture = 2ull << 20;
constexpr uint64_t kMaxCache = 16ull << 20;

struct Entry {
    ID3D11Device* device;
    ID3D11Texture2D* texture;
    BYTE* data;
    uint64_t bytes, signature;
    UINT width, height, mips;
    DXGI_FORMAT format;
};
Entry g_cache[48];
uint64_t g_bytes = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

struct Mip {
    const BYTE* data;
    UINT rows, row_bytes, pitch;
};

UINT describe(const D3D11_TEXTURE2D_DESC& d, const D3D11_SUBRESOURCE_DATA* init, Mip (&mips)[15], uint64_t bytes) {
    const TextureLayout layout = texture_layout(d.Format);
    if (!layout.block || !d.Width || !d.Height || d.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        d.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || d.MipLevels > _countof(mips))
        return 0;
    UINT w = d.Width, h = d.Height;
    uint64_t total = 0;
    for (UINT i = 0; i < d.MipLevels; ++i) {
        const UINT rows = blocks(h, layout.block), row_bytes = blocks(w, layout.block) * layout.bytes;
        if (!init[i].pSysMem || init[i].SysMemPitch < row_bytes) return 0;
        mips[i] = {static_cast<const BYTE*>(init[i].pSysMem), rows, row_bytes, init[i].SysMemPitch};
        total += uint64_t(rows) * row_bytes;
        if (w == 1 && h == 1 && i + 1 < d.MipLevels) return 0;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return total == bytes ? d.MipLevels : 0;
}

uint64_t signature(const Mip* mips, UINT n) {
    uint64_t h = 14695981039346656037ull;
    for (UINT i = 0; i < n; ++i) {
        const auto& m = mips[i];
        const UINT sample = std::min(16u, m.row_bytes);
        const auto* mid = m.data + size_t(m.rows / 2) * m.pitch + m.row_bytes / 2 - sample / 2;
        for (UINT k = 0; k < sample; ++k) h = (h ^ mid[k]) * 1099511628211ull;
    }
    return h;
}

// Ignore row padding, which the driver doesn't consume and need not be initialized.
bool same_pixels(const Mip* mips, UINT n, const BYTE* cached) {
    for (UINT i = 0; i < n; ++i) {
        const auto& m = mips[i];
        for (UINT y = 0; y < m.rows; ++y, cached += m.row_bytes)
            if (std::memcmp(m.data + size_t(y) * m.pitch, cached, m.row_bytes)) return false;
    }
    return true;
}

void copy_pixels(const Mip* mips, UINT n, BYTE* cached) {
    for (UINT i = 0; i < n; ++i) {
        const auto& m = mips[i];
        for (UINT y = 0; y < m.rows; ++y, cached += m.row_bytes)
            std::memcpy(cached, m.data + size_t(y) * m.pitch, m.row_bytes);
    }
}
}  // namespace

ID3D11Texture2D* dedup_texture(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d,
                              const D3D11_SUBRESOURCE_DATA* init, uint64_t bytes, ID3D11Texture2D* tex) {
    if (!init || bytes < 4096 || bytes > kMaxTexture || d.Usage != D3D11_USAGE_IMMUTABLE ||
        d.BindFlags != D3D11_BIND_SHADER_RESOURCE || d.MiscFlags || d.CPUAccessFlags || d.ArraySize != 1 ||
        d.SampleDesc.Count != 1 || d.SampleDesc.Quality || (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED))
        return nullptr;
    Mip mips[15];
    const UINT n = describe(d, init, mips, bytes);
    if (!n) return nullptr;
    const uint64_t hash = signature(mips, n);
    if (!TryAcquireSRWLockExclusive(&g_lock)) return nullptr;
    ID3D11Texture2D* hit = nullptr;
    Entry* free = nullptr;
    for (auto& e : g_cache) {
        if (!e.texture) {
            if (!free) free = &e;
            continue;
        }
        if (e.device != dev || e.signature != hash || e.width != d.Width || e.height != d.Height ||
            e.mips != n || e.format != d.Format || !same_pixels(mips, n, e.data))
            continue;
        e.texture->AddRef();
        hit = e.texture;
        break;
    }
    if (!hit && tex && free && g_bytes + bytes <= kMaxCache) {
        auto* data = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(bytes)));
        if (data) {
            copy_pixels(mips, n, data);
            tex->AddRef();
            *free = {dev, tex, data, bytes, hash, d.Width, d.Height, n, d.Format};
            g_bytes += bytes;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return hit;
}

void dedup_background_tick() {
    Entry retired[_countof(g_cache)];
    size_t n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (auto& e : g_cache) {
        if (!e.texture) continue;
        e.texture->AddRef();
        if (e.texture->Release() != 1) continue;
        retired[n++] = e;
        g_bytes -= e.bytes;
        e = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < n; ++i) {
        retired[i].texture->Release();
        HeapFree(GetProcessHeap(), 0, retired[i].data);
    }
}
}  // namespace tessera
