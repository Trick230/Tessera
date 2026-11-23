// SPDX-License-Identifier: GPL-3.0-or-later
// A game DLL that resolves NVAPI on its own, loaded after the layer: its
// GetProcAddress import must be patched as well.
#include <windows.h>
#include <dxgi.h>

extern "C" __declspec(dllexport) void* probe_qi() {
    HMODULE nv = LoadLibraryW(sizeof(void*) == 4 ? L"nvapi.dll" : L"nvapi64.dll");
    return nv ? reinterpret_cast<void*>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
}

// M1: this DLL imports the DXGI factory (the test reads the slot) and resolves it by name.
extern "C" __declspec(dllexport) void* probe_factory_import() { return reinterpret_cast<void*>(&CreateDXGIFactory1); }
extern "C" __declspec(dllexport) void* probe_factory() {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    return dxgi ? reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
}
