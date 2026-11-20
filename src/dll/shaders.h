// SPDX-License-Identifier: GPL-3.0-or-later
// S6: records every shader the game creates (stage, size, creation time,
// whether it was already in the store) and appends new bytecode to the
// per-game shader store, so the next preparation can warm it.
#pragma once
#include <d3d11.h>

namespace tessera {
// From on_device: loads the store index once, hooks the shader creation methods.
void shaders_on_device(ID3D11Device* dev);
// Writer thread: appends the bytecode collected since the last call.
void shaders_background_tick();
// Process exit (DllMain, other threads gone): last append, close the files.
void shaders_shutdown();
}  // namespace tessera
