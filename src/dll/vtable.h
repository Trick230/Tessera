// SPDX-License-Identifier: GPL-3.0-or-later
// COM vtable patching. One VtHook per (interface method, hook function);
// it remembers the original pointer of every vtable it patched so objects
// of different classes sharing the hook each chain to their own original.
#pragma once

struct ID3D11DeviceChild;

namespace tessera {
constexpr int kVtMax = 64;
struct VtHook {
    int slot;
    void* fn;
    volatile long n;
    void** volatile vt[kVtMax];  // nullptr: a forgotten vtable's entry, free for a new one
    void* orig[kVtMax];
    void** volatile forgotten_vt;  // the last forgotten vtable, and its original:
    void* volatile forgotten;      // vt_orig for a call made through it while its object dies
    bool listed;  // in the list vt_forget walks
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

// For an object whose vtable lives and dies with it (a deferred context's,
// inside the object): once it is destroyed, every hook forgets that vtable,
// so objects created all session long do not fill the tables. Before any
// vt_hook on obj.
void vt_track(ID3D11DeviceChild* obj);
}  // namespace tessera
