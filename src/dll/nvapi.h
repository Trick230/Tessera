// SPDX-License-Identifier: GPL-3.0-or-later
// M1 / P6 through NVAPI: memory reported by NvAPI_GPU_GetMemoryInfo and
// Reflex turned on with NvAPI_D3D_SetSleepMode.
#pragma once

namespace tessera {
// From DllMain (attach): points the game executable's GetProcAddress import
// at ours. Only memory writes and VirtualProtect, safe under the loader lock.
// Games that resolve NVAPI from another module are not seen.
void nvapi_patch_imports();
}  // namespace tessera
