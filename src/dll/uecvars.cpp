// SPDX-License-Identifier: GPL-3.0-or-later
#include "uecvars.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "config.h"
#include "log.h"

namespace tessera {
namespace {
constexpr int kMax = 32;
constexpr ULONGLONG kCheckMs = 1000;
constexpr int kLogRestores = 3;  // per variable

struct Cvar {
    char name[96];
    char kind;              // 'i' int32, 'f' float, 'b' bool
    volatile LONG* words;   // int / float copies
    volatile char* bytes;   // bool copies
    const void* const* slot; // p form: the static Ref, checked again before each write (null: d form)
    int copies;             // 2: TAutoConsoleVariable (game, render thread); 1: referenced variable
    LONG bits;              // value (float bits for 'f', 0/1 for 'b')
    int restores;
    bool gone;              // p form whose static Ref changed: left alone from then on
};

Cvar g_cvars[kMax];
int g_count;
std::atomic<bool> g_active{false};
std::atomic<ULONGLONG> g_next_check{0};
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

const IMAGE_NT_HEADERS64* image_headers(const uint8_t* base) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    return nt;
}

// [rva, rva + n) inside one section: writable data (write) or read-only data.
bool in_section(const IMAGE_NT_HEADERS64* nt, uint64_t rva, uint64_t n, bool write) {
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (rva < s->VirtualAddress || rva + n > uint64_t(s->VirtualAddress) + s->Misc.VirtualSize) continue;
        const DWORD c = s->Characteristics;
        if (c & IMAGE_SCN_MEM_EXECUTE) return false;
        return write == ((c & IMAGE_SCN_MEM_WRITE) != 0);
    }
    return false;
}

// n bytes of the image the loader really left writable (a protector or CFG
// can make a data page read-only whatever the section header says).
bool image_rw(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m)) return false;
    if (m.State != MEM_COMMIT || m.Type != MEM_IMAGE || (m.Protect & PAGE_GUARD)) return false;
    const DWORD prot = m.Protect & 0xFF;
    if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= uintptr_t(m.BaseAddress) + m.RegionSize;
}

// n bytes of committed, private, read-write memory outside the image (the
// engine's heap holds console variable objects).
bool heap_rw(const void* p, size_t n, const uint8_t* base, size_t image_size) {
    const auto a = reinterpret_cast<uintptr_t>(p);
    if (!a || (a & (n >= 4 ? 3 : n - 1)) || (a >= uintptr_t(base) && a < uintptr_t(base) + image_size)) return false;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m)) return false;
    if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) return false;
    if ((m.Protect & 0xFF) != PAGE_READWRITE || (m.Protect & PAGE_GUARD)) return false;
    return a + n <= uintptr_t(m.BaseAddress) + m.RegionSize;
}

const wchar_t* skip_spaces(const wchar_t* s) {
    while (*s == L' ' || *s == L'\t') ++s;
    return s;
}

void log_value(const char* what, const Cvar& c, LONG bits) {
    if (c.kind == 'f') {
        float f;
        memcpy(&f, &bits, sizeof f);
        logf("ue: %s %s %g", what, c.name, f);
    } else {
        logf("ue: %s %s %ld", what, c.name, static_cast<long>(bits));
    }
}

LONG read_copy(const Cvar& c, int i) {
    return c.kind == 'b' ? static_cast<LONG>(c.bytes[i]) : c.words[i];
}

void write_all(Cvar& c) {
    for (int i = 0; i < c.copies; ++i) {
        if (c.kind == 'b')
            c.bytes[i] = static_cast<char>(c.bits);
        else
            InterlockedExchange(&c.words[i], c.bits);
    }
}

// One `cvar.<name>=<value> <kind> <form> <rva>` line; false: left alone (logged).
bool parse_and_check(const wchar_t* key, const wchar_t* val, const uint8_t* base, const IMAGE_NT_HEADERS64* nt,
                     Cvar& c) {
    memset(&c, 0, sizeof c);
    WideCharToMultiByte(CP_UTF8, 0, key, -1, c.name, sizeof c.name - 1, nullptr, nullptr);
    wchar_t* end = nullptr;
    const wchar_t* v = skip_spaces(val);
    LONG bits = 0;
    const wchar_t* p = v;
    // Value first; its type is only known after the kind field, so find that first.
    while (*p && *p != L' ' && *p != L'\t') ++p;
    p = skip_spaces(p);
    const wchar_t kind = *p;
    if ((kind != L'i' && kind != L'f' && kind != L'b') || (p[1] != L' ' && p[1] != L'\t')) {
        logf("ue: %s: bad line, left alone", c.name);
        return false;
    }
    if (kind == L'f') {
        const float f = wcstof(v, &end);
        memcpy(&bits, &f, sizeof bits);
    } else {
        const long n = wcstol(v, &end, 10);
        bits = static_cast<LONG>(n);
        if (kind == L'b' && n != 0 && n != 1) end = nullptr;
    }
    if (!end || end == v || (*end != L' ' && *end != L'\t')) {
        logf("ue: %s: bad value, left alone", c.name);
        return false;
    }
    p = skip_spaces(p + 1);
    const wchar_t form = *p;
    if ((form != L'p' && form != L'd') || (p[1] != L' ' && p[1] != L'\t')) {
        logf("ue: %s: bad line, left alone", c.name);
        return false;
    }
    const unsigned long rva = wcstoul(skip_spaces(p + 1), &end, 16);
    if (!end || (*skip_spaces(end) != 0)) {
        logf("ue: %s: bad line, left alone", c.name);
        return false;
    }
    c.kind = static_cast<char>(kind);
    c.bits = bits;
    const size_t image_size = nt->OptionalHeader.SizeOfImage;
    const size_t width = kind == L'b' ? 1 : 4;
    if (form == L'd') {
        // FAutoConsoleVariableRef: the variable lives in the executable's data.
        if ((rva % width) || !in_section(nt, rva, width, true) || !image_rw(base + rva, width)) {
            logf("ue: %s: slot 0x%lx not in writable data, left alone", c.name, rva);
            return false;
        }
        c.copies = 1;
        c.words = reinterpret_cast<volatile LONG*>(const_cast<uint8_t*>(base) + rva);
        c.bytes = reinterpret_cast<volatile char*>(const_cast<uint8_t*>(base) + rva);
        return true;
    }
    // TAutoConsoleVariable { vtable, IConsoleObject* Target, T (*Ref)[2] }: rva is Ref.
    if ((rva % 8) || rva < 16 || !in_section(nt, rva - 16, 24, true) || !image_rw(base + rva - 16, 24)) {
        logf("ue: %s: slot 0x%lx not in writable data, left alone", c.name, rva);
        return false;
    }
    const auto* obj = reinterpret_cast<const uint8_t* const*>(base + rva - 16);
    const uint8_t* vtable = obj[0];
    const uint8_t* target = obj[1];
    const uint8_t* ref = obj[2];
    const bool vt_ok = vtable > base && in_section(nt, uint64_t(vtable - base), 8, false);
    const bool target_ok = heap_rw(target, 8, base, image_size) &&
                           *reinterpret_cast<const uint8_t* const*>(target) > base &&
                           in_section(nt, uint64_t(*reinterpret_cast<const uint8_t* const*>(target) - base), 8, false);
    // Ref is &FConsoleVariable::Data: inside the object Target points to.
    const bool ref_ok = ref > target && size_t(ref - target) < 0x200 && heap_rw(ref, 2 * width, base, image_size);
    if (!vt_ok || !target_ok || !ref_ok) {
        logf("ue: %s: object at 0x%lx does not look like a console variable (vtable %d target %d value %d), left "
             "alone",
             c.name, rva - 16, vt_ok, target_ok, ref_ok);
        return false;
    }
    c.copies = 2;
    c.slot = reinterpret_cast<const void* const*>(base + rva);
    c.words = reinterpret_cast<volatile LONG*>(const_cast<uint8_t*>(ref));
    c.bytes = reinterpret_cast<volatile char*>(const_cast<uint8_t*>(ref));
    return true;
}

BOOL CALLBACK apply_once(PINIT_ONCE, void*, void**) {
    if (!config().ue_cvars || !*config_ini_path()) return TRUE;
    static wchar_t section[16384];
    const DWORD n = GetPrivateProfileSectionW(L"tessera", section, ARRAYSIZE(section), config_ini_path());
    if (n == 0 || n >= ARRAYSIZE(section) - 2) return TRUE;
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const IMAGE_NT_HEADERS64* nt = image_headers(base);
    bool any = false, image_ok = false, image_seen = false;
    for (const wchar_t* line = section; *line; line += wcslen(line) + 1) {
        if (_wcsnicmp(line, L"ue_image=", 9) == 0) {
            image_seen = true;
            wchar_t* end = nullptr;
            const unsigned long ts = wcstoul(line + 9, &end, 16);
            const unsigned long size = end ? wcstoul(end, nullptr, 16) : 0;
            image_ok = nt && nt->FileHeader.TimeDateStamp == ts && nt->OptionalHeader.SizeOfImage == size;
            if (!image_ok)
                logf("ue: executable is not the scanned build (0x%lx 0x%lx vs 0x%lx 0x%lx), console variables off", ts,
                     size, nt ? static_cast<unsigned long>(nt->FileHeader.TimeDateStamp) : 0ul,
                     nt ? static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage) : 0ul);
        }
        if (_wcsnicmp(line, L"cvar.", 5) == 0) any = true;
    }
    if (!any) return TRUE;
    if (!image_seen) logf("ue: no ue_image line, console variables off");
    if (!image_ok) return TRUE;
    for (const wchar_t* line = section; *line && g_count < kMax; line += wcslen(line) + 1) {
        if (_wcsnicmp(line, L"cvar.", 5) != 0) continue;
        const wchar_t* eq = wcschr(line, L'=');
        if (!eq) continue;
        wchar_t key[96] = {};
        const size_t klen = static_cast<size_t>(eq - (line + 5));
        if (klen == 0 || klen >= ARRAYSIZE(key)) continue;
        wmemcpy(key, line + 5, klen);
        Cvar& c = g_cvars[g_count];
        if (!parse_and_check(key, eq + 1, base, nt, c)) continue;
        for (int i = 0; i < c.copies; ++i) log_value(i ? "render-thread copy of" : "game value of", c, read_copy(c, i));
        write_all(c);
        log_value("set", c, c.bits);
        ++g_count;
    }
    if (g_count) {
        g_next_check.store(GetTickCount64() + kCheckMs);
        g_active.store(true, std::memory_order_release);
    }
    return TRUE;
}
}  // namespace

void uecvars_on_device() { InitOnceExecuteOnce(&g_once, apply_once, nullptr, nullptr); }

void uecvars_frame() {
    if (!g_active.load(std::memory_order_acquire)) return;
    const ULONGLONG now = GetTickCount64();
    ULONGLONG due = g_next_check.load(std::memory_order_relaxed);
    if (now < due || !g_next_check.compare_exchange_strong(due, now + kCheckMs)) return;
    for (int k = 0; k < g_count; ++k) {
        Cvar& c = g_cvars[k];
        if (c.gone) continue;
        // The engine replaced or freed the variable (shutdown): never touch it again.
        if (c.slot && *c.slot != const_cast<const LONG*>(c.words)) {
            c.gone = true;
            logf("ue: %s moved, no longer restored", c.name);
            continue;
        }
        LONG seen = c.bits;
        for (int i = 0; i < c.copies; ++i)
            if (read_copy(c, i) != c.bits) seen = read_copy(c, i);
        if (seen == c.bits) continue;
        if (c.restores++ < kLogRestores) log_value("game changed, restoring", c, seen);
        write_all(c);
    }
}
}  // namespace tessera
