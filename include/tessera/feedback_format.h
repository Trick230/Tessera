// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#define TESSERA_FEEDBACK_MAGIC "TSRMIP1"
#define TESSERA_FEEDBACK_MAX_ENTRIES 4096
#define TESSERA_FEEDBACK_MAX_SHADER_BYTES (1024u * 1024u)
#define TESSERA_FEEDBACK_UAV 7
#define TESSERA_FEEDBACK_TEXTURES 128
#define TESSERA_FEEDBACK_SLOTS 8

// The header is followed by entries sorted by original DXBC checksum, each
// immediately followed by its instrumented DXBC bytes.
typedef struct TesseraFeedbackHeader {
    char magic[8];
    uint32_t count, reserved;
} TesseraFeedbackHeader;

typedef struct TesseraFeedbackEntry {
    uint8_t checksum[16];
    uint32_t bytes, count;
    uint8_t slots[TESSERA_FEEDBACK_SLOTS];
} TesseraFeedbackEntry;

#ifdef __cplusplus
static_assert(sizeof(TesseraFeedbackHeader) == 16 && sizeof(TesseraFeedbackEntry) == 32);
#endif
