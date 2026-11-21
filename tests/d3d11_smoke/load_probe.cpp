// SPDX-License-Identifier: GPL-3.0-or-later
// Loaded only to raise a fresh loader notification: the layer patches game
// DLLs whose imports were not bound yet on the next load.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
