// SPDX-License-Identifier: GPL-3.0-or-later
// Loaded only to raise a fresh loader notification: the layer patches game
// DLLs whose imports were not bound yet on the next load.
#include <windows.h>
#include <d3d11_3.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

namespace {
int __cdecl set_sleep_mode(IUnknown*, const unsigned char* params) {
    return params && params[12] != 0 ? -1 : 0;
}

// Stand in for slow driver work. The chained DLL sees the game context
// before Tessera and forwards every call with its original arguments.
template <unsigned Slot, class Result, class... Args>
struct SlowCallProbe {
    static inline Result (STDMETHODCALLTYPE* next)(Args...) = nullptr;

    static Result STDMETHODCALLTYPE call(Args... args) {
        Sleep(3);
        return next(args...);
    }

    static bool install(IUnknown* context) {
        void** slot = *reinterpret_cast<void***>(context) + Slot;
        DWORD protect = 0;
        if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protect)) return false;
        next = reinterpret_cast<decltype(next)>(*slot);
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&call));
        DWORD unused;
        return VirtualProtect(slot, sizeof *slot, protect, &unused) != FALSE;
    }
};

bool install_call_probes(ID3D11DeviceContext* ctx) {
    using Map = SlowCallProbe<14, HRESULT, ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*>;
    using GetData = SlowCallProbe<29, HRESULT, ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT>;
    using Update = SlowCallProbe<48, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT>;
    using Flush = SlowCallProbe<111, void, ID3D11DeviceContext*>;
    using Update1 = SlowCallProbe<116, void, ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT>;
    using Flush1 = SlowCallProbe<144, void, ID3D11DeviceContext3*, D3D11_CONTEXT_TYPE, HANDLE>;
    if (!Map::install(ctx) || !GetData::install(ctx) || !Update::install(ctx) || !Flush::install(ctx)) return false;
    ID3D11DeviceContext1* c1 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c1)))) return false;
    const bool updated = Update1::install(c1);
    c1->Release();
    ID3D11DeviceContext3* c3 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c3)))) return false;
    const bool flushed = Flush1::install(c3);
    c3->Release();
    return updated && flushed;
}
}

// Observe the wrapper without requiring a particular GPU or NVIDIA driver.
extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(unsigned id) {
    return id == 0xAC1CA9E0 ? reinterpret_cast<void*>(&set_sleep_mode) : nullptr;
}

#pragma comment(linker, "/export:D3D11CreateDevice=probe_d3d11_create_device")
extern "C" HRESULT WINAPI probe_d3d11_create_device(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
    UINT count, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    wchar_t path[MAX_PATH];
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\d3d11.dll") != 0) return E_FAIL;
    const HMODULE native = LoadLibraryW(path);
    const auto create = native ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(native, "D3D11CreateDevice")) : nullptr;
    if (!create) return E_FAIL;
    const HRESULT hr = create(adapter, type, software, flags, levels, count, sdk, dev, level, ctx);
    if (SUCCEEDED(hr) && ctx && *ctx && !install_call_probes(*ctx)) return E_FAIL;
    return hr;
}
