// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace tessera {
bool stream_import_wanted(const char* name);
void* stream_import_hook(const char* name, void* original);
void stream_start(const wchar_t* exe);
void stream_background_tick();
void stream_material(uint64_t key);
}  // namespace tessera
