// SPDX-License-Identifier: GPL-3.0-or-later
#include "prediction.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "config.h"
#include "residency.h"
#include "stream_io.h"
#include "texture_data.h"

namespace tessera {
namespace {
uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

struct Sample {
    TextureHint textures[kMaterialTextures];
    unsigned count;
};
Sample g_samples[192];
unsigned g_count = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

// Written and read by the writer only. Keep the most frequent partners
// of each texture, with bounded memory and no object references.
struct Edge {
    TextureHint texture;
    unsigned score;
};
struct Node {
    uint64_t key;
    Edge edges[4];
};
Node g_graph[1024];

Node& node(uint64_t key) { return g_graph[(key ^ (key >> 27)) % _countof(g_graph)]; }

void learn(Node& n, const TextureHint& partner) {
    Edge* weakest = &n.edges[0];
    for (auto& e : n.edges) {
        if (e.texture.key == partner.key) {
            e.texture = partner;
            e.score = std::min(e.score + 1, 32u);
            return;
        }
        if (e.score < weakest->score) weakest = &e;
    }
    if (weakest->score > 1)
        --weakest->score;
    else
        *weakest = {partner, 1};
}

bool contains(const Sample& sample, uint64_t key) {
    for (unsigned i = 0; i < sample.count; ++i)
        if (sample.textures[i].key == key) return true;
    return false;
}
}  // namespace

uint64_t texture_prediction_key(const D3D11_TEXTURE2D_DESC& d, const D3D11_SUBRESOURCE_DATA* init) {
    if (!init || !init->pSysMem || !d.Width || !d.Height || d.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        d.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || d.ArraySize != 1 || d.SampleDesc.Count != 1)
        return 0;
    const auto format = texture_layout(d.Format);
    if (!format.block) return 0;
    const UINT rows = blocks(d.Height, format.block), bytes = blocks(d.Width, format.block) * format.bytes;
    if (init->SysMemPitch < bytes) return 0;
    uint64_t h = mix(mix(mix(14695981039346656037ull, d.Width), d.Height), d.Format);
    const auto* src = static_cast<const BYTE*>(init->pSysMem);
    const UINT row_samples[] = {0, rows / 2, rows - 1};
    const UINT size = std::min(24u, bytes);
    for (UINT row : row_samples) {
        const auto* p = src + size_t(row) * init->SysMemPitch + (bytes - size) / 2;
        for (UINT i = 0; i < size; ++i) h = mix(h, p[i]);
    }
    return h ? h : 1;
}

void prediction_material(const TextureHint* textures, unsigned count) {
    if (config().stream_io) {
        uint64_t material = 14695981039346656037ull;
        for (unsigned i = 0; i < count; ++i) material = mix(material, textures[i].key);
        stream_material(material);
    }
    if (!config().mip_residency || !TryAcquireSRWLockExclusive(&g_lock)) return;
    if (g_count < _countof(g_samples)) {
        auto& sample = g_samples[g_count++];
        sample.count = count;
        std::memcpy(sample.textures, textures, count * sizeof(*textures));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void prediction_background_tick() {
    static Sample samples[_countof(g_samples)];
    AcquireSRWLockExclusive(&g_lock);
    const unsigned n = g_count;
    std::memcpy(samples, g_samples, n * sizeof(Sample));
    g_count = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (unsigned j = 0; j < n; ++j) {
        const auto& sample = samples[j];
        for (unsigned i = 0; i < sample.count; ++i) {
            const uint64_t key = sample.textures[i].key;
            auto& entry = node(key);
            if (entry.key != key) entry = {key, {}};
            for (unsigned k = 0; k < sample.count; ++k)
                if (sample.textures[k].key != key && sample.textures[k].resident_index)
                    learn(entry, sample.textures[k]);
        }
    }
    // Prefer the latest material cues and only partners seen together
    // repeatedly. The render thread rechecks liveness and VRAM headroom.
    for (unsigned j = n; j > 0; --j) {
        const auto& sample = samples[j - 1];
        for (unsigned i = 0; i < sample.count; ++i) {
            const uint64_t key = sample.textures[i].key;
            const auto& entry = node(key);
            if (entry.key != key) continue;
            for (const auto& e : entry.edges)
                if (e.score >= 3 && !contains(sample, e.texture.key))
                    residency_predict(e.texture.resident_index - 1, e.texture.key);
        }
    }
}
}  // namespace tessera
