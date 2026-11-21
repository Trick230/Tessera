// SPDX-License-Identifier: GPL-3.0-or-later
// A game DLL that resolves NVAPI on its own, loaded after the layer: its
// GetProcAddress import must be patched as well.
#include <windows.h>

extern "C" __declspec(dllexport) void* probe_qi() {
    HMODULE nv = LoadLibraryW(L"nvapi64.dll");
    return nv ? reinterpret_cast<void*>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
}
