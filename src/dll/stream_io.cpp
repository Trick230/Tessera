// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_io.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>

#include "config.h"
#include "log.h"
#include "nvapi.h"
#include "session.h"

namespace tessera {
namespace {
constexpr unsigned kFiles = 128, kReads = 4096, kHandles = 512;
constexpr unsigned kUntaggedWithPrediction = kReads / 2;
constexpr unsigned kRecentStart = kReads / 2;
constexpr DWORD kMinRead = 4096, kMaxRead = 2u << 20;
constexpr DWORD kPrefixBytes = 8u << 20, kSessionBytes = 64u << 20;
constexpr uint32_t kMagic = 0x314f4954;  // TIO1

struct File {
    uint64_t size, modified;
    wchar_t path[MAX_PATH];
};
struct Range {
    uint64_t offset, material;
    uint32_t file;
    DWORD bytes;
};
struct Trace {
    uint32_t magic, files, ranges, reserved;
    uint64_t exe_size, exe_modified;
    File file[kFiles];
    Range range[kReads];
};
static_assert(sizeof(File) == 536 && sizeof(Range) == 24 && offsetof(Trace, range) == 68640);

Trace g_trace, g_snapshot;
unsigned g_untagged_limit = kReads, g_material_ranges = 0;
unsigned g_saved_ranges = 0, g_saved_material_ranges = 0;
unsigned g_last_range = 0, g_next_recent = kRecentStart;
uint64_t g_revision = 0, g_saved_revision = 0, g_saved_tick = 0;
struct OpenFile {
    std::atomic<HANDLE> handle;
    std::atomic<unsigned> file;
};
OpenFile g_handles[kHandles];
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_learning{false};
std::atomic<uint64_t> g_material{0};
wchar_t g_root[MAX_PATH], g_path[MAX_PATH];
size_t g_root_length = 0;
uint64_t g_exe_size = 0, g_exe_modified = 0;

uint64_t wide(DWORD hi, DWORD lo) { return (uint64_t(hi) << 32) | lo; }

DWORD final_path(HANDLE h, wchar_t (&path)[MAX_PATH]) {
    const DWORD n = GetFinalPathNameByHandleW(h, path, MAX_PATH, FILE_NAME_NORMALIZED);
    return n < MAX_PATH ? n : 0;
}

bool in_root(const wchar_t* path, size_t n) {
    // Local paths only, with a folder boundary and no alternate data streams.
    return n > g_root_length && g_root_length >= 7 &&
           CompareStringOrdinal(path, static_cast<int>(g_root_length), g_root,
                                static_cast<int>(g_root_length), TRUE) == CSTR_EQUAL &&
           !wcschr(path + g_root_length, L':');
}

bool describe(HANDLE h, File& f) {
    BY_HANDLE_FILE_INFORMATION info = {};
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    f.size = wide(info.nFileSizeHigh, info.nFileSizeLow);
    f.modified = wide(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime);
    return f.size >= kMinRead && in_root(f.path, final_path(h, f.path));
}

OpenFile& handle_slot(HANDLE h) { return g_handles[(reinterpret_cast<uintptr_t>(h) >> 2) % kHandles]; }

void remember(HANDLE h, DWORD access, DWORD creation, DWORD flags) {
    if (h == INVALID_HANDLE_VALUE || creation != OPEN_EXISTING || !(access & GENERIC_READ) ||
        (access & (GENERIC_WRITE | GENERIC_ALL | DELETE)) || (flags & FILE_FLAG_DELETE_ON_CLOSE))
        return;
    File f = {};
    if (!describe(h, f) || !TryAcquireSRWLockExclusive(&g_lock)) return;
    unsigned i = 0;
    for (; i < g_trace.files; ++i) {
        const File& seen = g_trace.file[i];
        if (seen.size == f.size && seen.modified == f.modified &&
            CompareStringOrdinal(seen.path, -1, f.path, -1, TRUE) == CSTR_EQUAL)
            break;
    }
    if (i < kFiles) {
        if (i == g_trace.files) g_trace.file[g_trace.files++] = f;
        auto& s = handle_slot(h);
        s.handle.store(nullptr, std::memory_order_relaxed);
        s.file.store(i, std::memory_order_relaxed);
        s.handle.store(h, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

unsigned tracked(HANDLE h) {
    const auto& s = handle_slot(h);
    if (!h || s.handle.load(std::memory_order_acquire) != h) return kFiles;
    const unsigned file = s.file.load(std::memory_order_relaxed);
    return s.handle.load(std::memory_order_acquire) == h ? file : kFiles;
}

void record(unsigned file, uint64_t offset, DWORD bytes) {
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    const uint64_t size = g_trace.file[file].size;
    if (offset < size) {
        const uint64_t material = g_material.load(std::memory_order_relaxed);
        // Early I/O can fill the trace before the first material binds.
        const bool full = g_trace.ranges == kReads;
        if (material || (!full && g_trace.ranges - g_material_ranges < g_untagged_limit)) {
            const auto n = static_cast<DWORD>(std::min<uint64_t>(std::min(bytes, kMaxRead), size - offset));
            const Range& prev = g_trace.range[g_last_range];
            if (!g_trace.ranges || prev.file != file || prev.offset != offset || prev.bytes != n ||
                prev.material != material) {
                if (full) {
                    // Keep the startup and early reads while rotating in newer material reads.
                    g_last_range = g_next_recent++;
                    if (g_next_recent == kReads) g_next_recent = kRecentStart;
                    g_material_ranges -= g_trace.range[g_last_range].material != 0;
                } else {
                    g_last_range = g_trace.ranges++;
                }
                g_trace.range[g_last_range] = {offset, material, file, n};
                g_material_ranges += material != 0;
                ++g_revision;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

HANDLE WINAPI open_file(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD creation,
                        DWORD flags, HANDLE template_file) {
    const HANDLE h = CreateFileW(name, access, share, security, creation, flags, template_file);
    if (g_learning.load(std::memory_order_acquire)) {
        const DWORD error = GetLastError();
        remember(h, access, creation, flags);
        SetLastError(error);
    }
    return h;
}

BOOL WINAPI read_file(HANDLE h, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED ov) {
    if (!g_learning.load(std::memory_order_acquire) || bytes < kMinRead)
        return ReadFile(h, buffer, bytes, read, ov);
    const unsigned file = tracked(h);
    LARGE_INTEGER offset = {};
    const DWORD before = GetLastError();
    bool valid = file != kFiles;
    if (valid && ov)
        offset.QuadPart = static_cast<int64_t>(wide(ov->OffsetHigh, ov->Offset));
    else if (valid)
        valid = SetFilePointerEx(h, {}, &offset, FILE_CURRENT) != 0;
    SetLastError(before);
    const BOOL ok = ReadFile(h, buffer, bytes, read, ov);
    const DWORD error = GetLastError();
    if (valid && offset.QuadPart >= 0 && (ok || (ov && error == ERROR_IO_PENDING))) {
        const DWORD count = ok && !ov && read ? *read : bytes;
        if (count >= kMinRead) record(file, static_cast<uint64_t>(offset.QuadPart), count);
    }
    SetLastError(error);
    return ok;
}

BOOL WINAPI close_file(HANDLE h) {
    if (g_learning.load(std::memory_order_acquire)) {
        auto& s = handle_slot(h);
        HANDLE expected = h;
        s.handle.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
    }
    return CloseHandle(h);
}

DWORD warm_range(const Trace& trace, const Range& r, DWORD limit) {
    if (r.file >= trace.files || r.bytes < kMinRead || r.bytes > kMaxRead) return 0;
    const File& known = trace.file[r.file];
    const wchar_t* end = static_cast<const wchar_t*>(wmemchr(known.path, 0, MAX_PATH));
    if (!end || !in_root(known.path, end - known.path) || r.offset >= known.size ||
        r.bytes > known.size - r.offset)
        return 0;
    // Reject lexical escapes before opening, then validate the resolved file
    // and its version before causing any data reads.
    if (wcsstr(known.path + g_root_length, L"\\..\\") || wcsncmp(known.path + g_root_length, L"..\\", 3) == 0 ||
        wcschr(known.path + g_root_length, L'/'))
        return 0;
    HANDLE h = CreateFileW(known.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    File actual = {};
    DWORD warmed = 0;
    if (describe(h, actual) && actual.size == known.size && actual.modified == known.modified &&
        CompareStringOrdinal(actual.path, -1, known.path, -1, TRUE) == CSTR_EQUAL) {
        SYSTEM_INFO sys = {};
        GetSystemInfo(&sys);
        const uint64_t base = r.offset / sys.dwAllocationGranularity * sys.dwAllocationGranularity;
        const SIZE_T length = static_cast<SIZE_T>(r.offset - base + std::min(r.bytes, limit));
        HANDLE map = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (map) {
            if (void* view = MapViewOfFile(map, FILE_MAP_READ, static_cast<DWORD>(base >> 32),
                                          static_cast<DWORD>(base), length)) {
                WIN32_MEMORY_RANGE_ENTRY range = {static_cast<BYTE*>(view) + (r.offset - base), std::min(r.bytes, limit)};
                if (PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0))
                    warmed = static_cast<DWORD>(range.NumberOfBytes);
                UnmapViewOfFile(view);
            }
            CloseHandle(map);
        }
    }
    CloseHandle(h);
    return warmed;
}

bool has_room() {
    MEMORYSTATUSEX ms = {sizeof ms};
    return GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys > std::max(1ull << 30, ms.ullTotalPhys / 8);
}

void prefetch(const Trace& trace) {
    if (trace.magic != kMagic || trace.files > kFiles || trace.ranges > kReads ||
        trace.exe_size != g_exe_size || trace.exe_modified != g_exe_modified)
        return;
    bool issued[kReads] = {};
    DWORD used = 0, prefix = 0;
    for (unsigned i = 0; i < trace.ranges && i < 32 && used < kPrefixBytes && has_room(); ++i) {
        const auto& r = trace.range[i];
        issued[i] = true;
        const DWORD cap = kPrefixBytes - used;
        used += std::min(r.bytes, cap);
        prefix += warm_range(trace, r, cap);
    }
    logf("stream I/O: prefetched %lu KB from the startup list", prefix >> 10);
    if (!config().mip_prediction || used >= kSessionBytes) return;
    unsigned remaining = 0;
    for (unsigned i = 0; i < trace.ranges; ++i)
        if (!issued[i] && trace.range[i].material) ++remaining;
    logf("stream I/O: %u material ranges available after startup", remaining);
    uint64_t last = 0;
    while (remaining && used < kSessionBytes) {
        Sleep(250);
        const uint64_t current = g_material.load(std::memory_order_relaxed);
        if (!current || current == last || !has_room()) continue;
        last = current;
        DWORD batch = 0;
        DWORD warmed = 0;
        unsigned warmed_ranges = 0;
        for (unsigned i = 0; i < trace.ranges && batch < kMaxRead && used < kSessionBytes; ++i) {
            if (issued[i] || trace.range[i].material != current) continue;
            const DWORD cap = std::min(kMaxRead - batch, kSessionBytes - used);
            const DWORD wanted = std::min(trace.range[i].bytes, cap);
            issued[i] = true;
            --remaining;
            batch += wanted;
            used += wanted;
            const DWORD loaded = warm_range(trace, trace.range[i], cap);
            warmed += loaded;
            warmed_ranges += loaded != 0;
        }
        if (warmed)
            logf("stream I/O: prefetched %lu KB from a material match (%u ranges)", warmed >> 10, warmed_ranges);
    }
}

DWORD WINAPI prefetch_thread(void*) {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) return 0;
    HANDLE in = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(in, &size) && size.QuadPart == sizeof(Trace)) {
        if (HANDLE map = CreateFileMappingW(in, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            if (const auto* view = static_cast<const Trace*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(Trace)))) {
                // The file can be replaced while we play. Keep the mapped version we validated.
                prefetch(*view);
                UnmapViewOfFile(view);
            }
            CloseHandle(map);
        }
    }
    CloseHandle(in);
    return 0;
}
}  // namespace

bool stream_import_wanted(const char* name) {
    return !strcmp(name, "CreateFileW") || !strcmp(name, "ReadFile") || !strcmp(name, "CloseHandle");
}

void* stream_import_hook(const char* name, void* original) {
    // The loader resolves both Kernel32 and the API sets to the same entry
    // points. Leave pointers owned by other layers alone.
    if (!strcmp(name, "CreateFileW") && original == reinterpret_cast<void*>(&CreateFileW))
        return reinterpret_cast<void*>(&open_file);
    if (!strcmp(name, "ReadFile") && original == reinterpret_cast<void*>(&ReadFile))
        return reinterpret_cast<void*>(&read_file);
    if (!strcmp(name, "CloseHandle") && original == reinterpret_cast<void*>(&CloseHandle))
        return reinterpret_cast<void*>(&close_file);
    return nullptr;
}

void stream_start(const wchar_t* exe) {
    const int mode = config().stream_io;
    if (!mode) return;
    wchar_t dir[MAX_PATH];
    const DWORD n = GetPrivateProfileStringW(L"tessera", L"stream_root", L"", dir, MAX_PATH, config_ini_path());
    if (n >= MAX_PATH - 1) return;
    if (!n) {
        wcscpy_s(dir, exe);
        wchar_t* basename = wcsrchr(dir, L'\\');
        if (!basename) return;
        basename[1] = 0;
        const UINT levels = std::min(GetPrivateProfileIntW(L"tessera", L"stream_root_up", 0, config_ini_path()), 12u);
        for (UINT i = 0; i < levels; ++i) {
            *basename = 0;
            wchar_t* parent = wcsrchr(dir, L'\\');
            if (!parent) {
                *basename = L'\\';
                break;
            }
            basename = parent;
            basename[1] = 0;
        }
    }
    HANDLE root = CreateFileW(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (root == INVALID_HANDLE_VALUE) return;
    g_root_length = final_path(root, g_root);
    CloseHandle(root);
    if (g_root_length < 7 || g_root_length >= MAX_PATH - 1 || wcsncmp(g_root, L"\\\\?\\", 4) != 0 ||
        g_root[5] != L':' || g_root[6] != L'\\')
        return;
    if (g_root[g_root_length - 1] != L'\\') g_root[g_root_length++] = L'\\';
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesExW(exe, GetFileExInfoStandard, &attr) ||
        !session_data_path(L"streams", L"tiot", g_path))
        return;
    g_exe_size = wide(attr.nFileSizeHigh, attr.nFileSizeLow);
    g_exe_modified = wide(attr.ftLastWriteTime.dwHighDateTime, attr.ftLastWriteTime.dwLowDateTime);
    if (mode == 1) {
        if (config().mip_prediction) g_untagged_limit = kUntaggedWithPrediction;
        g_trace.magic = kMagic;
        g_trace.exe_size = g_exe_size;
        g_trace.exe_modified = g_exe_modified;
        g_learning.store(true, std::memory_order_release);
        activate_stream_imports();
        logf("stream I/O: learning reads under %ls", g_root);
    } else if (HANDLE thread = CreateThread(nullptr, 0, prefetch_thread, nullptr, 0, nullptr)) {
        CloseHandle(thread);
    }
}

void stream_background_tick() {
    if (!g_learning.load(std::memory_order_acquire)) return;
    const ULONGLONG now = GetTickCount64();
    // A saturated rolling trace can stay dirty throughout play. Bound disk writes.
    if (g_saved_ranges == kReads && now - g_saved_tick < 1000) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_saved_revision == g_revision) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_snapshot = g_trace;
    const unsigned material_ranges = g_material_ranges;
    const uint64_t revision = g_revision;
    ReleaseSRWLockExclusive(&g_lock);

    wchar_t temp[MAX_PATH];
    if (_snwprintf_s(temp, _TRUNCATE, L"%ls.%lu.tmp", g_path, GetCurrentProcessId()) < 0) return;
    HANDLE out = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(out, &g_snapshot, sizeof g_snapshot, &written, nullptr);
    CloseHandle(out);
    if (ok && written == sizeof g_snapshot && MoveFileExW(temp, g_path, MOVEFILE_REPLACE_EXISTING)) {
        if (!g_saved_ranges) logf("stream I/O: saved learning trace");
        g_saved_ranges = g_snapshot.ranges;
        g_saved_material_ranges = material_ranges;
        g_saved_revision = revision;
        g_saved_tick = now;
    }
}

void stream_log_summary() {
    if (g_learning.load(std::memory_order_acquire))
        logf("stream I/O: learning trace contains %u ranges, %u with materials, %llu rolling replacements",
             g_saved_ranges, g_saved_material_ranges,
             static_cast<unsigned long long>(g_saved_revision - g_saved_ranges));
}

void stream_material(uint64_t key) { g_material.store(key, std::memory_order_relaxed); }
}  // namespace tessera
