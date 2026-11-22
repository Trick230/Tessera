// SPDX-License-Identifier: GPL-3.0-or-later
// Loaded only to raise a fresh loader notification: the layer patches game
// DLLs whose imports were not bound yet on the next load.
#include <windows.h>
#include <unknwn.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

namespace {
int __cdecl set_sleep_mode(IUnknown*, const unsigned char* params) {
    return params && params[12] != 0 ? -1 : 0;
}
}

// Observe the wrapper without requiring a particular GPU or NVIDIA driver.
extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(unsigned id) {
    return id == 0xAC1CA9E0 ? reinterpret_cast<void*>(&set_sleep_mode) : nullptr;
}
