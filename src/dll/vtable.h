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
    void** vt[32];  // every deferred context has its own (heap) vtable
    void* orig[32];
};

// Patches obj's vtable at h.slot (once per vtable). Saves the current
// pointer first, so a hook installed by another tool keeps running. With
// readopt (deferred contexts: a heap vtable freed with its object and
// reallocated at the same address), a vtable patched before whose slot now
// holds a method of the same system module is patched again.
bool vt_hook(VtHook& h, void* obj, bool readopt = false);

// For a vtable vt_hook patched: when its owner (the system D3D runtime) has
// written one of its own methods over the patch, adopts it as the original
// and patches again. False if the slot now holds anything else.
bool vt_rehook(VtHook& h, void* obj);

// Original pointer for obj's vtable. Only called from inside h.fn.
void* vt_orig(const VtHook& h, void* obj);
}  // namespace tessera
