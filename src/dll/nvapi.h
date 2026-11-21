// SPDX-License-Identifier: GPL-3.0-or-later
// M1 / P6 through NVAPI: memory reported by NvAPI_GPU_GetMemoryInfo and
// Reflex turned on with NvAPI_D3D_SetSleepMode.
#pragma once

namespace tessera {
// From DllMain (attach), unless safe mode is on: points the GetProcAddress
// imports of the game exe and of the DLLs in its folder at ours, now and as
// more load (loader notifications); also their imports memory.cpp wants
// (memory_import_hook). Only memory writes and VirtualProtect,
// safe under the loader lock. NVAPI resolved from modules outside the game
// folder (overlays, SDKs) is left alone.
void nvapi_patch_imports();
// Writer thread: game DLLs whose imports were not bound when the loader
// announced them, and no DLL loaded since, are patched here.
void nvapi_background_tick();
}  // namespace tessera
