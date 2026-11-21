// SPDX-License-Identifier: GPL-3.0-or-later
// M1: video memory budget. Samples the OS budget and the game's usage for the
// session log (D2 "VRAM above budget") and, when vram_budget_mb is set,
// reports at most that budget to the game so it sizes its pools to fit.
#pragma once
#include <d3d11.h>

#include <cstdint>

namespace tessera {
// From device creation: keeps the adapter for sampling; hooks
// QueryVideoMemoryInfo only when a budget cap is configured.
void memory_on_device(ID3D11Device* dev);
// Dedicated memory a vendor API (NVAPI) should report: the honest / capped
// budget when the game renders on that vendor's GPU, else the real value.
uint64_t memory_reported_for(uint32_t vendor_id, uint64_t real);
// A D3D11 device of ours exists (so the session is up); any thread.
bool memory_device_seen();
// From the writer thread, about once a second: pushes a VRAM record.
void memory_background_tick();
// Real local (dedicated) usage and budget at the last tick; false before one.
bool memory_local(uint64_t* usage, uint64_t* budget);
}  // namespace tessera
