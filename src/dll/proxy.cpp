// SPDX-License-Identifier: GPL-3.0-or-later
// Export forwarding. Every export is an asm stub (exports_d3d11.asm) that
// jumps through tessera_real[]; the first call lands in tessera_resolve,
// which loads the real DLL lazily (never in DllMain): the DLL named by
// tessera.ini `next=` (chained proxy) or the system d3d11.dll.
#include <windows.h>

#include <cwchar>

#include "exports_d3d11_gen.h"
#include "hooks.h"
#include "log.h"
#include "session.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" void* tessera_real[];

extern "C" HRESULT tessera_fail_stub() { return E_NOTIMPL; }
void* resolve_export(unsigned idx);

namespace {
INIT_ONCE g_load_once = INIT_ONCE_STATIC_INIT;
HMODULE g_next = nullptr;    // chained proxy from tessera.ini, if any
HMODULE g_system = nullptr;  // System32\d3d11.dll

HMODULE load_checked(const wchar_t* path) {
    HMODULE m = LoadLibraryW(path);
    if (m == reinterpret_cast<HMODULE>(&__ImageBase)) {  // misconfigured: points at ourselves
        FreeLibrary(m);
        return nullptr;
    }
    return m;
}

BOOL CALLBACK load_real(PINIT_ONCE, void*, void**) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
        wchar_t ini[MAX_PATH], next[MAX_PATH];
        swprintf_s(ini, L"%s\\tessera.ini", dir);
        GetPrivateProfileStringW(L"tessera", L"next", L"", next, MAX_PATH, ini);
        if (next[0]) {
            if (wcschr(next, L':'))
                wcscpy_s(path, next);
            else
                swprintf_s(path, L"%s\\%s", dir, next);
            g_next = load_checked(path);
        }
    }
    // Always loaded: a chained proxy may export only the functions it wraps.
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len > 0 && len < MAX_PATH - 12) {
        wcscat_s(path, L"\\d3d11.dll");
        g_system = load_checked(path);
    }
    return TRUE;
}

void* real(int idx) {
    void* p = tessera_real[idx];
    return p ? p : resolve_export(static_cast<unsigned>(idx));
}
}  // namespace

void* resolve_export(unsigned idx) {
    InitOnceExecuteOnce(&g_load_once, load_real, nullptr, nullptr);
    void* p = nullptr;
    if (idx < static_cast<unsigned>(tessera::exports::kCount)) {
        const char* name = tessera::exports::kNames[idx];
        if (g_next) p = reinterpret_cast<void*>(GetProcAddress(g_next, name));
        if (!p && g_system) p = reinterpret_cast<void*>(GetProcAddress(g_system, name));
    }
    if (!p) p = reinterpret_cast<void*>(&tessera_fail_stub);
    InterlockedExchangePointer(&tessera_real[idx], p);
    return p;
}

extern "C" void* tessera_resolve(unsigned idx) { return resolve_export(idx); }

extern "C" HRESULT WINAPI Tessera_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software,
                                                    UINT flags, const D3D_FEATURE_LEVEL* levels, UINT nlevels,
                                                    UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* level,
                                                    ID3D11DeviceContext** ctx) {
    auto fn = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(real(tessera::exports::kD3D11CreateDevice));
    const HRESULT hr = fn(adapter, type, software, flags, levels, nlevels, sdk, dev, level, ctx);
    if (SUCCEEDED(hr) && dev && *dev) tessera::on_device(*dev, flags);
    return hr;
}

extern "C" HRESULT WINAPI Tessera_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
    UINT nlevels, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc, ID3D11Device** dev,
    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    auto fn = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        real(tessera::exports::kD3D11CreateDeviceAndSwapChain));
    const HRESULT hr = fn(adapter, type, software, flags, levels, nlevels, sdk, desc, sc, dev, level, ctx);
    if (SUCCEEDED(hr) && dev && *dev) {
        tessera::on_device(*dev, flags);
        if (sc && *sc && tessera::session_init()) {
            if (desc)
                tessera::logf("swapchain (CreateDeviceAndSwapChain) %ux%u buffers %u", desc->BufferDesc.Width,
                              desc->BufferDesc.Height, desc->BufferCount);
            tessera::on_swapchain(*sc);
        }
    }
    return hr;
}
