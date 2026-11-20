// SPDX-License-Identifier: GPL-3.0-or-later
// Nothing heavy here: loading the real DLL and all setup happen lazily on the
// first export call, outside the loader lock.
#include <windows.h>

#include "nvapi.h"
#include "session.h"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    // Before the game's code runs: it resolves NVAPI early.
    if (reason == DLL_PROCESS_ATTACH) tessera::nvapi_patch_imports();
    if (reason == DLL_PROCESS_DETACH) tessera::session_shutdown();
    return TRUE;
}
