// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_quality.h"

#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <cwchar>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "prediction.h"
#include "session.h"
#include "vtable.h"

namespace tessera {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID kEntry = {0x9b63a112, 0xe8e3, 0x4dd7, {0x9a, 0x5e, 0x44, 0x8a, 0x85, 0x4e, 0x63, 0x7a}};
constexpr GUID kSource = {0xaa552bb0, 0x41ae, 0x4009, {0x8f, 0xd8, 0x9f, 0x5c, 0x2d, 0x68, 0x34, 0x28}};
constexpr GUID kViewFormat = {0xeb869e0d, 0xf320, 0x4453, {0x93, 0x7e, 0x31, 0x84, 0xda, 0x7c, 0xed, 0x21}};
constexpr GUID kDevice = {0x7e219802, 0xd345, 0x46b6, {0x83, 0xfe, 0x4f, 0x97, 0xd7, 0xce, 0x4c, 0x25}};
constexpr uint32_t kCaptureBytes = 32u << 20;
constexpr uint64_t kCacheBytes = 128ull << 20;
constexpr uint32_t kTextureBytes = 8u << 20;
constexpr unsigned kEntries = 32;
const QualityEntry* g_cache[kEntries];
std::atomic<unsigned> g_cache_count{0};
uint8_t* g_capture = nullptr;
wchar_t g_capture_dir[MAX_PATH];
struct Upload {
    QualityEntry header;
    uint32_t offset;
};
Upload g_upload[kEntries];
std::atomic<unsigned> g_ready{0};
uint32_t g_used = 0;
SRWLOCK g_capture_lock = SRWLOCK_INIT;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
std::atomic<bool> g_replaced{false};
thread_local unsigned t_native_quality = 0;

void slots(const QualityEntry& h, const uint8_t* data, bool compressed, D3D11_SUBRESOURCE_DATA* out) {
    for (unsigned m = 0; m < h.mips; ++m) {
        const auto layout = quality_mip(h, m, compressed);
        out[m] = {data, layout.pitch, 0};
        data += layout.bytes();
    }
}

bool same_pixels(const QualityEntry& h, const D3D11_SUBRESOURCE_DATA* pixels) {
    const auto* data = reinterpret_cast<const uint8_t*>(&h + 1);
    for (unsigned m = 0; m < h.mips; ++m) {
        const auto* p = static_cast<const uint8_t*>(pixels[m].pSysMem);
        const auto layout = quality_mip(h, m, false);
        for (unsigned y = 0; y < layout.rows; ++y, data += layout.pitch)
            if (std::memcmp(p + size_t(y) * pixels[m].SysMemPitch, data, layout.pitch) != 0) return false;
    }
    return true;
}

bool describe(const D3D11_TEXTURE2D_DESC& d, const D3D11_SUBRESOURCE_DATA* pixels, QualityEntry& h) {
    if (!pixels || d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.SampleDesc.Quality ||
        d.Usage != D3D11_USAGE_IMMUTABLE || d.BindFlags != D3D11_BIND_SHADER_RESOURCE ||
        d.CPUAccessFlags || d.MiscFlags)
        return false;
    std::memcpy(h.magic, TESSERA_QUALITY_MAGIC, 8);
    h.width = d.Width;
    h.height = d.Height;
    h.mips = d.MipLevels;
    h.format = d.Format;
    if (!quality_shape(h)) return false;
    h.original_bytes = quality_bytes(h, false);
    if (h.original_bytes > kTextureBytes) return false;
    for (unsigned m = 0; m < h.mips; ++m)
        if (!pixels[m].pSysMem || pixels[m].SysMemPitch < quality_mip(h, m, false).pitch) return false;
    h.key = texture_prediction_key(d, pixels);
    return h.key != 0;
}

const QualityEntry* map_entry(const wchar_t* path, uint64_t* budget) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size = {};
    HANDLE mapping = nullptr;
    if (GetFileSizeEx(f, &size) && size.QuadPart >= sizeof(QualityEntry) && uint64_t(size.QuadPart) <= *budget)
        mapping = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    if (!mapping) return nullptr;
    const auto* view = static_cast<const QualityEntry*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!view) return nullptr;
    if (!quality_header(*view, size.QuadPart, true) || view->original_bytes > kTextureBytes) {
        UnmapViewOfFile(view);
        return nullptr;
    }
    *budget -= size.QuadPart;
    return view;
}

BOOL CALLBACK initialize(PINIT_ONCE, void*, void**) {
    if (config().texture_quality == 1) {
        if (session_data_path(L"quality", L"raw", g_capture_dir) &&
            (CreateDirectoryW(g_capture_dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)) {
            g_capture = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCaptureBytes, MEM_RESERVE | MEM_COMMIT,
                                                          PAGE_READWRITE));
            if (g_capture) logf("texture quality: recording immutable texture data");
        }
    } else if (config().texture_quality == 2) {
        wchar_t dir[MAX_PATH], path[MAX_PATH];
        if (!session_data_path(L"quality", L"packed", dir) ||
            _snwprintf_s(path, _TRUNCATE, L"%ls\\*.tbc", dir) < 0)
            return TRUE;
        WIN32_FIND_DATAW data = {};
        HANDLE list = FindFirstFileW(path, &data);
        if (list == INVALID_HANDLE_VALUE) return TRUE;
        uint64_t budget = kCacheBytes;
        unsigned count = 0;
        do {
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (_snwprintf_s(path, _TRUNCATE, L"%ls\\%ls", dir, data.cFileName) < 0) continue;
            if (const auto* entry = map_entry(path, &budget)) g_cache[count++] = entry;
        } while (count < kEntries && FindNextFileW(list, &data));
        FindClose(list);
        g_cache_count.store(count, std::memory_order_release);
        logf("texture quality: mapped %u prepared textures", count);
    }
    return TRUE;
}

bool write_capture(const Upload& upload) {
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    if (_snwprintf_s(path, _TRUNCATE, L"%ls\\%016llx.txr", g_capture_dir, upload.header.key) < 0 ||
        _snwprintf_s(temp, _TRUNCATE, L"%ls.%lu.tmp", path, GetCurrentProcessId()) < 0)
        return false;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto& h = upload.header;
    const bool ok = WriteFile(f, &h, sizeof h, &written, nullptr) && written == sizeof h &&
                    WriteFile(f, g_capture + upload.offset, h.original_bytes, &written, nullptr) &&
                    written == h.original_bytes;
    CloseHandle(f);
    if (ok && MoveFileExW(temp, path, 0)) return true;
    DeleteFileW(temp);
    return false;
}

void STDMETHODCALLTYPE hk_view_desc(ID3D11ShaderResourceView* view, D3D11_SHADER_RESOURCE_VIEW_DESC* out);
void STDMETHODCALLTYPE hk_view_desc1(ID3D11ShaderResourceView1* view, D3D11_SHADER_RESOURCE_VIEW_DESC1* out);
VtHook g_view_desc = {8, reinterpret_cast<void*>(&hk_view_desc)};
VtHook g_view_desc1 = {9, reinterpret_cast<void*>(&hk_view_desc1)};

template <class View, class Desc>
void view_desc(const VtHook& hook, View* view, Desc* out) {
    reinterpret_cast<void(STDMETHODCALLTYPE*)(View*, Desc*)>(vt_orig(hook, view))(view, out);
    if (!out || t_native_quality) return;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT size = sizeof format;
    if (SUCCEEDED(view->GetPrivateData(kViewFormat, &size, &format))) out->Format = format;
}
void STDMETHODCALLTYPE hk_view_desc(ID3D11ShaderResourceView* v, D3D11_SHADER_RESOURCE_VIEW_DESC* out) {
    view_desc(g_view_desc, v, out);
}
void STDMETHODCALLTYPE hk_view_desc1(ID3D11ShaderResourceView1* v, D3D11_SHADER_RESOURCE_VIEW_DESC1* out) {
    view_desc(g_view_desc1, v, out);
}
}  // namespace

void quality_init() {
    if (config().texture_quality) InitOnceExecuteOnce(&g_once, initialize, nullptr, nullptr);
}

void quality_capture(const D3D11_TEXTURE2D_DESC& d, const D3D11_SUBRESOURCE_DATA* pixels) {
    if (config().texture_quality != 1) return;
    quality_init();
    QualityEntry h = {};
    if (!g_capture || !describe(d, pixels, h) || !TryAcquireSRWLockExclusive(&g_capture_lock)) return;
    const unsigned n = g_ready.load(std::memory_order_relaxed);
    bool found = false;
    for (unsigned i = 0; i < n; ++i) found |= g_upload[i].header.key == h.key;
    if (!found && n < _countof(g_upload) && h.original_bytes <= kCaptureBytes - g_used) {
        g_upload[n] = {h, g_used};
        uint8_t* out = g_capture + g_used;
        for (unsigned m = 0; m < h.mips; ++m) {
            const auto layout = quality_mip(h, m, false);
            const auto* in = static_cast<const uint8_t*>(pixels[m].pSysMem);
            for (unsigned y = 0; y < layout.rows; ++y, out += layout.pitch)
                std::memcpy(out, in + size_t(y) * pixels[m].SysMemPitch, layout.pitch);
        }
        g_used += h.original_bytes;
        g_ready.store(n + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_capture_lock);
}

void quality_background_tick() {
    static unsigned done = 0;
    const auto count = g_ready.load(std::memory_order_acquire);
    for (unsigned limit = 0; done < count && limit < 4; ++limit, ++done)
        if (!write_capture(g_upload[done])) logf("texture quality: could not save a capture");
}

void quality_register_device(ID3D11Device* dev) {
    if (config().texture_quality != 2) return;
    quality_init();
    if (!g_cache_count.load(std::memory_order_acquire) || !quality_device_hooks(dev)) return;
    constexpr UINT enabled = 1;
    dev->SetPrivateData(kDevice, sizeof enabled, &enabled);
}

bool quality_replace(ID3D11Device* dev, CreateTex2DFn create, const D3D11_TEXTURE2D_DESC& d,
                     const D3D11_SUBRESOURCE_DATA* pixels, ID3D11Texture2D** out, uint64_t* bytes) {
    if (config().texture_quality != 2) return false;
    const unsigned count = g_cache_count.load(std::memory_order_acquire);
    if (!count) return false;
    QualityEntry wanted = {};
    if (!describe(d, pixels, wanted)) return false;
    UINT bridge = 0, size = sizeof bridge;
    if (FAILED(dev->GetPrivateData(kDevice, &size, &bridge)) || !bridge) return false;
    for (unsigned i = 0; i < count; ++i) {
        const auto& h = *g_cache[i];
        if (h.key != wanted.key || h.format != wanted.format || h.width != wanted.width ||
            h.height != wanted.height || h.mips != wanted.mips || !same_pixels(h, pixels))
            continue;
        D3D11_SUBRESOURCE_DATA sub[TESSERA_QUALITY_MIPS];
        slots(h, reinterpret_cast<const uint8_t*>(&h + 1) + h.original_bytes, true, sub);
        D3D11_TEXTURE2D_DESC desc = d;
        desc.Format = static_cast<DXGI_FORMAT>(h.compressed);
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(create(dev, &desc, sub, &tex)) || !quality_texture_hooks(tex.Get()) ||
            FAILED(tex->SetPrivateData(kEntry, sizeof g_cache[i], &g_cache[i])))
            return false;
        *out = tex.Detach();
        *bytes = h.compressed_bytes;
        if (!g_replaced.exchange(true)) logf("texture quality: replaced an approved texture");
        return true;
    }
    return false;
}

const QualityEntry* quality_entry(ID3D11Resource* resource) {
    if (!resource || !g_replaced.load(std::memory_order_relaxed) || t_native_quality) return nullptr;
    const QualityEntry* entry = nullptr;
    UINT size = sizeof entry;
    return SUCCEEDED(resource->GetPrivateData(kEntry, &size, &entry)) && size == sizeof entry ? entry : nullptr;
}

bool quality_has_replacements() { return g_replaced.load(std::memory_order_relaxed); }

NativeQuality::NativeQuality(bool enable) : enabled(enable) {
    if (enabled) ++t_native_quality;
}
NativeQuality::~NativeQuality() {
    if (enabled) --t_native_quality;
}

ID3D11Texture2D* quality_copy_source(ID3D11Resource* src) {
    const auto* h = quality_entry(src);
    if (!h) return nullptr;
    ComPtr<ID3D11Texture2D> original;
    UINT size = sizeof(ID3D11Texture2D*);
    if (SUCCEEDED(src->GetPrivateData(kSource, &size, original.GetAddressOf())) && original)
        return original.Detach();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = h->width;
    d.Height = h->height;
    d.MipLevels = h->mips;
    d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = static_cast<DXGI_FORMAT>(h->format);
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub[TESSERA_QUALITY_MIPS];
    slots(*h, reinterpret_cast<const uint8_t*>(h + 1), false, sub);
    ComPtr<ID3D11Device> dev;
    src->GetDevice(&dev);
    InternalCall internal;
    if (FAILED(dev->CreateTexture2D(&d, sub, &original))) return nullptr;
    src->SetPrivateDataInterface(kSource, original.Get());
    return original.Detach();
}

void quality_on_view(const QualityEntry* entry, ID3D11ShaderResourceView* view) {
    if (!entry || !view) return;
    const auto format = static_cast<DXGI_FORMAT>(entry->format);
    if (vt_hook(g_view_desc, view))
        view->SetPrivateData(kViewFormat, sizeof format, &format);
    ComPtr<ID3D11ShaderResourceView1> v1;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&v1)))) vt_hook(g_view_desc1, v1.Get());
}
}  // namespace tessera
