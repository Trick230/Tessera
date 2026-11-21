// SPDX-License-Identifier: GPL-3.0-or-later
#include "vtable.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdlib>
#include <new>

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

// Under g_patch_lock: the hooks that patched anything, for vt_forget.
// About 40 hooks exist today; past this, a new hook fails its first patch.
VtHook* g_hooks[128];
int g_n_hooks = 0;

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
        if (!h.listed) {
            if (g_n_hooks == static_cast<int>(_countof(g_hooks))) {
                ok = false;
                goto done;
            }
            g_hooks[g_n_hooks++] = &h;
            h.listed = true;
        }
        long j = 0;
        while (j < h.n && h.vt[j]) ++j;  // a forgotten entry first
        if (j == kVtMax) {
            ok = false;
            goto done;
        }
        // Publish the original before the hook can be reached through this vtable.
        h.orig[j] = vt[h.slot];
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&h.vt[j]), vt);
        if (j == h.n) InterlockedIncrement(&h.n);
    }
    ok = write_slot(vt, h.slot, h.fn);
done:
    ReleaseSRWLockExclusive(&g_patch_lock);
    return ok;
}

void forget(void** vt) {
    AcquireSRWLockExclusive(&g_patch_lock);
    for (int k = 0; k < g_n_hooks; ++k) {
        VtHook& h = *g_hooks[k];
        for (long i = 0; i < h.n; ++i)
            if (h.vt[i] == vt) {
                // A call still made through it (later in the destructor) goes here.
                h.forgotten = h.orig[i];
                h.forgotten_vt = vt;
                InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&h.vt[i]), nullptr);
            }
    }
    ReleaseSRWLockExclusive(&g_patch_lock);
}

// Private data of a tracked object: the runtime releases it when the object
// is destroyed, before its memory (and the vtable in it) can be reused.
// {3F7A2C19-8E45-4B6D-9C21-5A0E7D3B8F14}
constexpr GUID kTrackGuid = {0x3f7a2c19, 0x8e45, 0x4b6d, {0x9c, 0x21, 0x5a, 0x0e, 0x7d, 0x3b, 0x8f, 0x14}};
struct Tracker final : IUnknown {
    std::atomic<long> refs{1};
    void** vt = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(refs.fetch_add(1) + 1); }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = refs.fetch_sub(1) - 1;
        if (n == 0) {
            forget(vt);
            this->~Tracker();
            HeapFree(GetProcessHeap(), 0, this);
        }
        return static_cast<ULONG>(n);
    }
};
}  // namespace

void vt_track(ID3D11DeviceChild* obj) {
    void** vt = *reinterpret_cast<void***>(obj);
    // Only a vtable inside the object itself: one in a module image is shared.
    const auto a = reinterpret_cast<uintptr_t>(vt), o = reinterpret_cast<uintptr_t>(obj);
    if (a < o || a >= o + 4096 || module_of(vt)) return;
    // Tracked already (a second creation hook for the same object): replacing
    // the tracker would release it and forget a vtable still in use.
    IUnknown* had = nullptr;
    UINT size = sizeof had;
    if (SUCCEEDED(obj->GetPrivateData(kTrackGuid, &size, &had))) {
        if (had) had->Release();
        return;
    }
    void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(Tracker));
    if (!mem) return;
    auto* t = new (mem) Tracker();
    t->vt = vt;
    obj->SetPrivateDataInterface(kTrackGuid, t);
    t->Release();  // the object's reference is the one left
}
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
    // A vtable forgotten while its object was being destroyed.
    if (vt == h.forgotten_vt) return h.forgotten;
    return h.orig[0];
}
}  // namespace tessera
