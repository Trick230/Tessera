// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace tessera {
namespace {
HANDLE g_log = INVALID_HANDLE_VALUE;
SRWLOCK g_log_lock = SRWLOCK_INIT;
bool g_exiting = false;
}  // namespace

void log_exiting() { g_exiting = true; }

void log_open(const wchar_t* path) {
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void logf(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(buf, sizeof buf, _TRUNCATE, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond,
                        t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof buf - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n += m < 0 ? static_cast<int>(strlen(buf + n)) : m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (g_exiting) {
        if (!TryAcquireSRWLockExclusive(&g_log_lock)) return;
    } else {
        AcquireSRWLockExclusive(&g_log_lock);
    }
    DWORD w;
    WriteFile(g_log, buf, static_cast<DWORD>(n), &w, nullptr);
    ReleaseSRWLockExclusive(&g_log_lock);
}
}  // namespace tessera
