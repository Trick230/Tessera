// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tessera/session_format.h"

namespace tessera {
// First call (from a hooked export, never DllMain): safe-mode check, data
// dirs, log, session file, writer thread. Returns false when hooks must stay
// off (safe mode tripped or setup failed). Thread-safe, idempotent.
bool session_init();

// Lock-free; callable from any thread. Never blocks, never allocates.
void session_push(TesseraRecord& r);
// wait_us: time the frame limiter waited before this Present.
void session_frame(uint32_t wait_us);

// DllMain(DLL_PROCESS_DETACH): final flush, END record, clears the crash counter.
void session_shutdown();
}  // namespace tessera
