// SPDX-License-Identifier: GPL-3.0-or-later
// U1: Unreal Engine console variables set from the game profile. The app finds
// each variable's value slot in the executable offline (crates/core/src/
// uecvars.rs) and writes `cvar.<name>=<value> <i|f|b> <p|d> <rva>` lines plus
// `ue_image=<timestamp> <size of image>` to tessera.ini. The layer never calls
// into the engine: it checks that the running image is the scanned build,
// checks each slot, and writes the value (both game- and render-thread copies
// of a TAutoConsoleVariable). Code that sets a variable later (scalability,
// game settings) is undone once a second, and logged.
#pragma once

namespace tessera {
// First device creation (the engine's static console variables exist by
// then): validates and applies. Any doubt about the image or a slot: that
// variable (or all, for the image) is left alone and the reason logged.
void uecvars_on_device();
// Present (outer, once per frame): once a second, rewrites values the game
// changed.
void uecvars_frame();
}  // namespace tessera
