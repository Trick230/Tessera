// SPDX-License-Identifier: GPL-3.0-or-later
#include "vtable.h"

#include <windows.h>

#include <cstdlib>

namespace tessera {
namespace {
SRWLOCK g_patch_lock = SRWLOCK_INIT;
}

bool vt_hook(VtHook& h, void* obj) {
    void** vt = *static_cast<void***>(obj);
    AcquireSRWLockExclusive(&g_patch_lock);
    bool ok = true;
    for (long i = 0; i < h.n; ++i)
        if (h.vt[i] == vt) goto done;
    if (vt[h.slot] == h.fn) goto done;  // a copied vtable that already points at us
    if (h.n == static_cast<long>(_countof(h.vt))) {
        ok = false;
        goto done;
    }
    // Publish the original before the hook can be reached through this vtable.
    h.vt[h.n] = vt;
    h.orig[h.n] = vt[h.slot];
    InterlockedIncrement(&h.n);
    {
        // Writable already (a heap vtable copy, maybe next to another tool's
        // trampolines): write in place, never flip its protection. Otherwise
        // keep execute rights if the page has them.
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(&vt[h.slot], &mi, sizeof mi)) {
            ok = false;
            goto done;
        }
        const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if (mi.Protect & writable) {
            InterlockedExchangePointer(&vt[h.slot], h.fn);
        } else {
            const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ;
            DWORD old;
            if (!VirtualProtect(&vt[h.slot], sizeof(void*), (mi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
                                &old)) {
                ok = false;
                goto done;
            }
            InterlockedExchangePointer(&vt[h.slot], h.fn);
            VirtualProtect(&vt[h.slot], sizeof(void*), old, &old);
        }
    }
done:
    ReleaseSRWLockExclusive(&g_patch_lock);
    return ok;
}

void* vt_orig(const VtHook& h, void* obj) {
    void** vt = *static_cast<void***>(obj);
    const long n = h.n;
    for (long i = 0; i < n; ++i)
        if (h.vt[i] == vt) return h.orig[i];
    return h.orig[0];
}
}  // namespace tessera
