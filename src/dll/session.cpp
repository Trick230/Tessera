// SPDX-License-Identifier: GPL-3.0-or-later
// Session log: producers (hooks) push fixed records into a lock-free ring; a
// writer thread drains it to disk every 250 ms. Also owns the safe-mode
// crash counter: incremented at start, cleared on a clean process exit.
#include "session.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwchar>

#include "config.h"
#include "dedup.h"
#include "log.h"
#include "memory.h"
#include "mip_feedback.h"
#include "nvapi.h"
#include "pacing.h"
#include "prediction.h"
#include "process.h"
#include "shaders.h"
#include "residency.h"
#include "stream_io.h"
#include "texture_quality.h"
#include "workset.h"

namespace tessera {
namespace {
constexpr uint64_t kRing = 1u << 16;  // 64k records, 3 MB
constexpr int kSafeModeCrashes = 2;

struct Slot {
    std::atomic<uint64_t> seq;  // index + 1 once written, 0 while writing
    TesseraRecord rec;
};

Slot* g_ring = nullptr;
std::atomic<uint64_t> g_head{0};
std::atomic<int64_t> g_last_present{0};
uint64_t g_tail = 0;  // writer only
uint64_t g_dropped = 0;
uint64_t g_written = 0;
int g_stuck = 0;  // consecutive flushes blocked on the same slot
std::atomic<uint64_t> g_claim_failed{0};
uint64_t g_init_qpc = 0;
TesseraRecord g_out[2048];

HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_stop = nullptr;
LARGE_INTEGER g_freq;
bool g_enabled = false;
bool g_started = false;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

wchar_t g_exe_path[MAX_PATH];
const wchar_t* g_exe_name = L"unknown.exe";
wchar_t g_dir[MAX_PATH];
wchar_t g_safe_path[MAX_PATH];

void make_dir(const wchar_t* fmt, const wchar_t* a = L"") {
    wchar_t p[MAX_PATH];
    swprintf_s(p, fmt, g_dir, a);
    CreateDirectoryW(p, nullptr);
}

bool resolve_data_dir() {
    DWORD n = GetEnvironmentVariableW(L"TESSERA_DATA_DIR", g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wchar_t base[MAX_PATH];
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return false;
        swprintf_s(g_dir, L"%s\\Tessera", base);
    }
    CreateDirectoryW(g_dir, nullptr);
    make_dir(L"%s\\logs");
    make_dir(L"%s\\sessions");
    make_dir(L"%s\\safemode");
    return GetFileAttributesW(g_dir) != INVALID_FILE_ATTRIBUTES;
}

// FNV-1a 64 over the exe path as UTF-16 code units, with a leading "\\?\"
// dropped, '/' read as '\' and ASCII A-Z lowercased. Keys the safe-mode
// counter so games sharing an exe name stay apart. Must match
// safe_mode_key() in crates/core/src/session.rs.
uint64_t path_key(const wchar_t* p) {
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    uint64_t h = 14695981039346656037ull;
    for (; *p; ++p) {
        wchar_t c = *p == L'/' ? L'\\' : *p;
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
        h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    }
    return h;
}

int read_counter(const wchar_t* path = g_safe_path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char buf[16] = {};
    DWORD r = 0;
    ReadFile(f, buf, sizeof buf - 1, &r, nullptr);
    CloseHandle(f);
    return atoi(buf);
}

void write_counter(int v) {
    HANDLE f = CreateFileW(g_safe_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[16];
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%d", v);
    DWORD w;
    WriteFile(f, buf, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(f);
}

void write_out(int n) {
    if (n <= 0) return;
    DWORD w;
    WriteFile(g_file, g_out, static_cast<DWORD>(n * sizeof(TesseraRecord)), &w, nullptr);
    g_written += static_cast<uint64_t>(n);
}

// Single consumer: the writer thread, or DllMain at exit once it is gone.
void flush(bool final) {
    const uint64_t head = g_head.load(std::memory_order_acquire);
    uint64_t t = g_tail;
    if (head - t > kRing) {
        g_dropped += head - kRing - t;
        t = head - kRing;
    }
    int n = 0;
    while (t < head) {
        Slot& s = g_ring[t & (kRing - 1)];
        const uint64_t s1 = s.seq.load(std::memory_order_acquire);
        if (s1 != t + 1) {
            // Lapped, or never completed: a producer that dropped its record
            // (claim failed) or was killed. Skip after ~1 s, or at exit.
            if (s1 > t + 1 || final || ++g_stuck > 4) {
                g_stuck = 0;
                ++g_dropped;
                ++t;
                continue;
            }
            break;  // still being written
        }
        g_stuck = 0;
        g_out[n] = s.rec;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != s1) {
            ++g_dropped;
            ++t;
            continue;
        }
        ++t;
        if (++n == static_cast<int>(_countof(g_out))) {
            write_out(n);
            n = 0;
            g_tail = t;  // committed only once on disk
        }
    }
    write_out(n);
    g_tail = t;
}

// Clears the crash counter once the game has presented frames for 60 s: only
// crashes early in a session count, and games that exit with
// TerminateProcess (no DLL detach) do not trip safe mode.
DWORD WINAPI writer_main(void*) {
    bool stable = false;
    unsigned tick = 0;
    while (WaitForSingleObject(g_stop, 250) == WAIT_TIMEOUT) {
        flush(false);
        ++tick;
        if ((tick & 3) == 1) {  // every 1 s, first after 250 ms
            memory_background_tick();
            workset_background_tick();
            residency_background_tick();
            if (config().present_cpu_sets) present_cpu_sets_tick();
            if (config().texture_dedup) dedup_background_tick();
        }
        if ((tick & 7) == 1) pacing_background_tick();  // every 2 s, first after 250 ms
        shaders_background_tick();
        nvapi_background_tick();
        if (config().stream_io == 1) stream_background_tick();
        if (config().texture_quality == 1) quality_background_tick();
        if (config().mip_prediction && config().mip_residency) prediction_background_tick();
        if (config().mip_streaming && config().mip_residency) residency_streaming_tick();
        if (!stable) {
            const int64_t last = g_last_present.load(std::memory_order_relaxed);
            if (last && static_cast<uint64_t>(last) - g_init_qpc > 60 * static_cast<uint64_t>(g_freq.QuadPart)) {
                write_counter(0);
                logf("session stable, crash counter cleared");
                stable = true;
            }
        }
    }
    flush(false);
    return 0;
}

bool open_session_file() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%s\\sessions\\%s_%04u%02u%02u-%02u%02u%02u_%lu.tsess", g_dir, g_exe_name, t.wYear, t.wMonth,
               t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    g_file = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return false;

    TesseraSessionHeader h = {};
    memcpy(h.magic, TESSERA_SESSION_MAGIC, sizeof h.magic);
    h.version = TESSERA_SESSION_VERSION;
    h.header_size = sizeof h;
    h.record_size = sizeof(TesseraRecord);
    h.pid = GetCurrentProcessId();
    h.qpc_frequency = static_cast<uint64_t>(g_freq.QuadPart);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    h.start_qpc = static_cast<uint64_t>(now.QuadPart);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    h.start_filetime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    wcsncpy_s(reinterpret_cast<wchar_t*>(h.exe_path), _countof(h.exe_path), g_exe_path, _TRUNCATE);
    DWORD w;
    WriteFile(g_file, &h, sizeof h, &w, nullptr);
    logf("session file: %ls", p);
    return true;
}

BOOL CALLBACK init_once(PINIT_ONCE, void*, void**) {
    // Pinned: the writer thread must never outlive our code (a game may
    // FreeLibrary d3d11 after probing it).
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&init_once), &self);
    QueryPerformanceFrequency(&g_freq);
    GetModuleFileNameW(nullptr, g_exe_path, MAX_PATH);
    if (const wchar_t* s = wcsrchr(g_exe_path, L'\\')) g_exe_name = s + 1;
    if (!resolve_data_dir()) return TRUE;

    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%s\\logs\\%s.log", g_dir, g_exe_name);
    log_open(p);
    logf("tessera layer, exe %ls", g_exe_path);

    // _snwprintf_s: swprintf_s would kill the game on an over-long path.
    if (_snwprintf_s(g_safe_path, _TRUNCATE, L"%s\\safemode\\%s-%016llx.count", g_dir, g_exe_name,
                     path_key(g_exe_path)) < 0) {
        logf("data path too long, layer disabled");
        return TRUE;
    }
    const int crashes = read_counter();
    if (crashes >= kSafeModeCrashes) {
        logf("safe mode: %d unclean exits in a row, layer disabled (delete %ls to re-enable)", crashes,
             g_safe_path);
        return TRUE;
    }
    write_counter(crashes + 1);
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_init_qpc = static_cast<uint64_t>(now.QuadPart);
    }
    g_started = true;
    config_load();
    config_log();
    if (config().process) apply_process_tweaks();

    g_ring = static_cast<Slot*>(VirtualAlloc(nullptr, kRing * sizeof(Slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_ring || !g_stop || !open_session_file()) {
        logf("session setup failed (%lu), layer disabled", GetLastError());
        return TRUE;
    }
    HANDLE th = CreateThread(nullptr, 64 * 1024, writer_main, nullptr, 0, nullptr);
    if (!th) {
        logf("writer thread failed (%lu), layer disabled", GetLastError());
        return TRUE;
    }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    g_enabled = true;
    stream_start(g_exe_path);
    return TRUE;
}
}  // namespace

bool session_safe_mode() {
    // Same paths as init_once, without creating anything: no data dir means
    // no counter, so not tripped.
    wchar_t dir[MAX_PATH], exe[MAX_PATH], path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TESSERA_DATA_DIR", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wchar_t base[MAX_PATH];
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH || _snwprintf_s(dir, _TRUNCATE, L"%s\\Tessera", base) < 0) return false;
    }
    n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    if (_snwprintf_s(path, _TRUNCATE, L"%s\\safemode\\%s-%016llx.count", dir, name, path_key(exe)) < 0) return false;
    return read_counter(path) >= kSafeModeCrashes;
}

bool session_data_path(const wchar_t* sub, const wchar_t* ext, wchar_t* out) {
    if (!g_started) return false;
    make_dir(L"%s\\%s", sub);
    return _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s-%016llx.%s", g_dir, sub, g_exe_name,
                        path_key(g_exe_path), ext) >= 0;
}

bool session_init() {
    InitOnceExecuteOnce(&g_once, init_once, nullptr, nullptr);
    return g_enabled;
}

void session_push(TesseraRecord& r) {
    const uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    Slot& s = g_ring[i & (kRing - 1)];
    // Claim the slot from its previous lap; if another producer still owns
    // it (we lapped a preempted writer), drop rather than tear its record.
    uint64_t expected = i >= kRing ? i + 1 - kRing : 0;
    if (!s.seq.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) {
        g_claim_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::atomic_thread_fence(std::memory_order_release);
    s.rec = r;
    s.seq.store(i + 1, std::memory_order_release);
}

void session_frame(uint32_t wait_us, uint32_t start_wait_us) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t prev = g_last_present.exchange(now.QuadPart, std::memory_order_relaxed);
    TesseraRecord r = {};
    r.qpc = static_cast<uint64_t>(now.QuadPart);
    r.kind = TESSERA_REC_FRAME;
    r.b = wait_us;
    r.c = start_wait_us;
    // prev > now: two threads presenting (secondary swapchain); report 0.
    if (prev && prev <= now.QuadPart) {
        const uint64_t us = static_cast<uint64_t>(now.QuadPart - prev) * 1000000u / static_cast<uint64_t>(g_freq.QuadPart);
        r.a = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
    }
    session_push(r);
    workset_frame();
    residency_frame();
}

void session_shutdown() {
    if (!g_started) return;
    if (g_enabled) {
        // Process exit: other threads are already gone, so we are the only consumer.
        log_exiting();
        flush(true);
        shaders_shutdown();
        if (config().stream_io == 1) stream_log_summary();
        if (config().texture_dedup) dedup_log_summary();
        if (config().mip_feedback) feedback_log_summary();
        g_dropped += g_claim_failed.load(std::memory_order_relaxed);
        TesseraRecord end = {};
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        end.qpc = static_cast<uint64_t>(now.QuadPart);
        end.kind = TESSERA_REC_END;
        end.a = static_cast<uint32_t>(g_dropped);
        end.bytes = g_written;
        DWORD w;
        WriteFile(g_file, &end, sizeof end, &w, nullptr);
        CloseHandle(g_file);
        logf("clean exit: %llu records, %llu dropped", g_written, g_dropped);
    }
    write_counter(0);
}
}  // namespace tessera
