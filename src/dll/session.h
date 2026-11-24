// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tessera/session_format.h"

namespace tessera {
// First call (from a hooked export, never DllMain): safe-mode check, data
// dirs, log, session file, writer thread. Returns false when hooks must stay
// off (safe mode tripped or setup failed). Thread-safe, idempotent.
bool session_init();

// Safe mode tripped for this exe (the counter session_init checks), without
// opening the log or creating anything: for DllMain, before any patch.
bool session_safe_mode();

// Lock-free; callable from any thread. Never blocks, never allocates.
void session_push(TesseraRecord& r);
// wait_us: time the frame limiter waited before this Present; start_wait_us:
// time it held the start of this frame, after the previous Present.
void session_frame(uint32_t wait_us, uint32_t start_wait_us);

// Per-game data file: <data dir>\<sub>\<exe name>-<path key>.<ext> (same key as
// the safe-mode counter); creates <sub>. out holds MAX_PATH. After session_init.
bool session_data_path(const wchar_t* sub, const wchar_t* ext, wchar_t* out);

// DllMain(DLL_PROCESS_DETACH): final flush, END record, clears the crash counter.
void session_shutdown();
}  // namespace tessera
