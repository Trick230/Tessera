// SPDX-License-Identifier: GPL-3.0-or-later
// Export forwarding. Every export is an asm stub (exports_d3d11.asm) that
// jumps through tessera_real[]; the first call lands in tessera_resolve,
// which loads the real DLL lazily (never in DllMain): the DLL named by
// tessera.ini `next=` (chained proxy) or the system d3d11.dll.
#include <windows.h>
#include <intrin.h>

#include <cwchar>

#include "config.h"
#include "exports_d3d11_gen.h"
#include "flip.h"
#include "hooks.h"
#include "log.h"
#include "module.h"
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

bool prefer_11_1(void* caller, IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, const D3D_FEATURE_LEVEL* levels,
                 UINT count, ID3D11Device** device) {
    if (!device || type != (adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE)) return false;
    tessera::config_load();
    if (!tessera::config().d3d11_prefer_11_1 || !tessera::is_game_code(caller)) return false;
    const bool can_promote = levels ? count && levels[0] == D3D_FEATURE_LEVEL_11_0 : count == 0;
    return can_promote && !tessera::session_safe_mode();
}

// Keep the trial outputs separate so the original request sees untouched
// output arguments, even when a chained DLL returns partial results.
template <class Create>
HRESULT create_at_11_1(Create create, IDXGISwapChain** sc, ID3D11Device** dev, D3D_FEATURE_LEVEL* level,
                      ID3D11DeviceContext** ctx) {
    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_1;
    IDXGISwapChain* trial_sc = nullptr;
    ID3D11Device* trial_dev = nullptr;
    ID3D11DeviceContext* trial_ctx = nullptr;
    D3D_FEATURE_LEVEL trial_level = {};
    const HRESULT hr = create(&requested, 1, sc ? &trial_sc : nullptr, &trial_dev,
                              level ? &trial_level : nullptr, ctx ? &trial_ctx : nullptr);
    if (SUCCEEDED(hr)) {
        if (sc) *sc = trial_sc;
        *dev = trial_dev;
        if (level) *level = trial_level;
        if (ctx) *ctx = trial_ctx;
    } else {
        if (trial_sc) trial_sc->Release();
        if (trial_ctx) trial_ctx->Release();
        if (trial_dev) trial_dev->Release();
    }
    return hr;
}

void log_11_1_trial(HRESULT hr) {
    if (SUCCEEDED(hr)) tessera::logf("d3d11: optional feature level 11_1 request succeeded");
    else tessera::logf("d3d11: optional feature level 11_1 request failed 0x%x; used original feature levels", hr);
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
    const bool trial = prefer_11_1(_ReturnAddress(), adapter, type, levels, nlevels, dev);
    HRESULT trial_hr = E_FAIL;
    if (trial) {
        trial_hr = create_at_11_1([&](const D3D_FEATURE_LEVEL* trial_levels, UINT count, IDXGISwapChain**,
                                    ID3D11Device** device, D3D_FEATURE_LEVEL* actual, ID3D11DeviceContext** context) {
            return fn(adapter, type, software, flags, trial_levels, count, sdk, device, actual, context);
        }, nullptr, dev, level, ctx);
    }
    const HRESULT hr = SUCCEEDED(trial_hr) ? trial_hr : fn(adapter, type, software, flags, levels, nlevels, sdk, dev, level, ctx);
    if (SUCCEEDED(hr) && dev && *dev) tessera::on_device(*dev, flags);
    if (trial) log_11_1_trial(trial_hr);
    return hr;
}

extern "C" HRESULT WINAPI Tessera_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
    UINT nlevels, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc, ID3D11Device** dev,
    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    void* const ret = _ReturnAddress();
    auto fn = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        real(tessera::exports::kD3D11CreateDeviceAndSwapChain));
    // P2: a qualifying blit swapchain is created as flip model from the start.
    DXGI_SWAP_CHAIN_DESC flip;
    bool tearing = false;
    bool conv = desc && sc && tessera::session_init() &&
                tessera::flip_convert(nullptr, tessera::is_game_code(ret), *desc, &flip, &tearing);
    const bool trial = prefer_11_1(ret, adapter, type, levels, nlevels, dev);
    HRESULT trial_hr = E_FAIL;
    auto create = [&](const DXGI_SWAP_CHAIN_DESC* swap_desc) {
        if (trial) {
            trial_hr = create_at_11_1([&](const D3D_FEATURE_LEVEL* trial_levels, UINT count, IDXGISwapChain** swap,
                                        ID3D11Device** device, D3D_FEATURE_LEVEL* actual, ID3D11DeviceContext** context) {
                return fn(adapter, type, software, flags, trial_levels, count, sdk, swap_desc, swap, device, actual, context);
            }, sc, dev, level, ctx);
            if (SUCCEEDED(trial_hr)) return trial_hr;
        }
        return fn(adapter, type, software, flags, levels, nlevels, sdk, swap_desc, sc, dev, level, ctx);
    };
    // d3d11 creates the swapchain through the hooked factory: recorded once, here.
    HRESULT hr = E_FAIL;
    {
        tessera::InternalCall internal;
        if (conv) hr = create(&flip);
        if (conv && FAILED(hr)) {
            tessera::logf("flip: D3D11CreateDeviceAndSwapChain failed 0x%x with flip model, retrying as asked", hr);
            conv = false;
        }
        if (!conv) hr = create(desc);
    }
    if (SUCCEEDED(hr) && dev && *dev) {
        tessera::on_device(*dev, flags);
        if (sc && *sc && tessera::session_init()) tessera::on_legacy_swapchain(*sc, *dev, conv ? &flip : desc, conv, tearing);
    }
    if (trial) log_11_1_trial(trial_hr);
    return hr;
}
