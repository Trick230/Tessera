// SPDX-License-Identifier: GPL-3.0-or-later
// M1: video memory budget. Samples the OS budget and the game's usage for the
// session log (D2 "VRAM above budget") and, when vram_budget_mb is set,
// reports at most that budget to the game so it sizes its pools to fit.
#pragma once
#include <d3d11.h>

namespace tessera {
// From device creation: keeps the adapter for sampling; hooks
// QueryVideoMemoryInfo only when a budget cap is configured.
void memory_on_device(ID3D11Device* dev);
// From the writer thread, about once a second: pushes a VRAM record.
void memory_background_tick();
}  // namespace tessera
