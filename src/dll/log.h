// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace tessera {
// Text log for events (never per frame). No-op until log_open succeeds.
void log_open(const wchar_t* path);
void logf(const char* fmt, ...);
// Process exit: a killed thread may hold the lock; from now on never wait for it.
void log_exiting();
}  // namespace tessera
