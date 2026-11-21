// SPDX-License-Identifier: GPL-3.0-or-later
// M1 / P6 through NVAPI: memory reported by NvAPI_GPU_GetMemoryInfo and
// Reflex turned on with NvAPI_D3D_SetSleepMode.
#pragma once

namespace tessera {
// From DllMain (attach), unless safe mode is on: points the GetProcAddress
// imports of the game exe and of the DLLs in its folder at ours, now and as
// more load (loader notifications). Only memory writes and VirtualProtect,
// safe under the loader lock. NVAPI resolved from modules outside the game
// folder (overlays, SDKs) is left alone.
void nvapi_patch_imports();
}  // namespace tessera
