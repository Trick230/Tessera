// SPDX-License-Identifier: GPL-3.0-or-later
// COM vtable patching. One VtHook per (interface method, hook function);
// it remembers the original pointer of every vtable it patched so objects
// of different classes sharing the hook each chain to their own original.
#pragma once

namespace tessera {
struct VtHook {
    int slot;
    void* fn;
    volatile long n;
    void** vt[8];
    void* orig[8];
};

// Patches obj's vtable at h.slot (once per vtable). Saves the current
// pointer first, so a hook installed by another tool keeps running.
bool vt_hook(VtHook& h, void* obj);

// Original pointer for obj's vtable. Only called from inside h.fn.
void* vt_orig(const VtHook& h, void* obj);
}  // namespace tessera
