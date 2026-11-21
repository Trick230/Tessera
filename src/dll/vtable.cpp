// SPDX-License-Identifier: GPL-3.0-or-later
#include "vtable.h"

#include <windows.h>

#include <cstdlib>

namespace tessera {
namespace {
SRWLOCK g_patch_lock = SRWLOCK_INIT;

// Under g_patch_lock. A writable page (a heap vtable copy, maybe next to
// another tool's trampolines) is written in place, its protection never
// flipped. Otherwise execute rights are kept if the page has them.
bool write_slot(void** vt, int slot, void* fn) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(&vt[slot], &mi, sizeof mi)) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (mi.Protect & writable) {
        InterlockedExchangePointer(&vt[slot], fn);
        return true;
    }
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), (mi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
        return false;
    InterlockedExchangePointer(&vt[slot], fn);
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

// The module holding p's code, nullptr outside any (a trampoline).
HMODULE module_of(const void* p) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(p), &m);
    return m;
}

// Under g_patch_lock. A module loaded from the system directory (the D3D
// runtime), never our proxy or another tool. The few seen are remembered.
bool is_system_module(HMODULE m) {
    static HMODULE known[8];
    static int n_known = 0;
    if (!m) return false;
    for (int k = 0; k < n_known; ++k)
        if (known[k] == m) return true;
    wchar_t sys[MAX_PATH], path[MAX_PATH];
    const UINT ns = GetSystemDirectoryW(sys, MAX_PATH);
    const DWORD np = GetModuleFileNameW(m, path, MAX_PATH);
    if (!ns || ns >= MAX_PATH || !np || np >= MAX_PATH || np <= ns || path[ns] != L'\\') return false;
    path[ns] = 0;
    const bool yes = CompareStringOrdinal(sys, -1, path, -1, TRUE) == CSTR_EQUAL &&
                     !wcschr(path + ns + 1, L'\\');  // directly in it, not below
    if (yes && n_known < static_cast<int>(_countof(known))) known[n_known++] = m;
    return yes;
}

bool hook_impl(VtHook& h, void* obj, bool readopt) {
    void** vt = *static_cast<void***>(obj);
    AcquireSRWLockExclusive(&g_patch_lock);
    bool ok = true;
    long i = 0;
    if (vt[h.slot] == h.fn) goto done;  // patched, or a copied vtable that already points at us
    while (i < h.n && h.vt[i] != vt) ++i;
    if (i < h.n) {
        if (!readopt) goto done;  // patched before; whatever is there now chains to us or is not ours to fix
        // A vtable at this address was patched before: a new object's (the
        // old one is gone), or the runtime wrote over the patch. Only a
        // method of the same system module is adopted; anything else is
        // another tool's hook, maybe chained to ours: patching again would loop.
        const HMODULE m = module_of(vt[h.slot]);
        if (!m || m != module_of(h.orig[i]) || !is_system_module(m)) {
            ok = false;
            goto done;
        }
        h.orig[i] = vt[h.slot];
    } else {
        if (h.n == static_cast<long>(_countof(h.vt))) {
            ok = false;
            goto done;
        }
        // Publish the original before the hook can be reached through this vtable.
        h.vt[h.n] = vt;
        h.orig[h.n] = vt[h.slot];
        InterlockedIncrement(&h.n);
    }
    ok = write_slot(vt, h.slot, h.fn);
done:
    ReleaseSRWLockExclusive(&g_patch_lock);
    return ok;
}
}  // namespace

bool vt_hook(VtHook& h, void* obj, bool readopt) { return hook_impl(h, obj, readopt); }

bool vt_rehook(VtHook& h, void* obj) {
    void** vt = *static_cast<void***>(obj);
    if (vt[h.slot] == h.fn) return true;
    AcquireSRWLockShared(&g_patch_lock);
    long i = 0;
    while (i < h.n && h.vt[i] != vt) ++i;
    const bool known = i < h.n;
    ReleaseSRWLockShared(&g_patch_lock);
    return known && hook_impl(h, obj, true);  // only for a vtable patched before
}

void* vt_orig(const VtHook& h, void* obj) {
    void** vt = *static_cast<void***>(obj);
    const long n = h.n;
    for (long i = 0; i < n; ++i)
        if (h.vt[i] == vt) return h.orig[i];
    return h.orig[0];
}
}  // namespace tessera
