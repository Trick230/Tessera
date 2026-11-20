// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace tessera {
// C3: is the code at `ret` (a hook's return address) part of the game, i.e.
// a module loaded from the game exe's folder or below? Overlays, SDKs,
// runtime-internal calls and unknown code (no module) answer false and their
// resources are never managed. Cached per thread; a miss costs a loader lookup.
bool is_game_code(void* ret);
}  // namespace tessera
