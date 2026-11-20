// SPDX-License-Identifier: GPL-3.0-or-later
// Shader store: bytecode of every shader a game created, kept across sessions
// so the next preparation can warm them (S6). Written by tessera.dll, read by
// the app and tessera_warm.exe. Source of truth; the Rust side mirrors it.
//
// File: %LOCALAPPDATA%\Tessera\shaders\<exe name>-<path key>.tshd (same key as
// the safe-mode counter). Header, then entries appended until EOF. Each entry
// is a TesseraShaderEntry followed by `size` bytes of bytecode, padded to 8.
// Index: <same name>.tshi, a header then one TesseraShaderIndexEntry per
// store entry, written after it. The DLL loads it at device creation to know
// which shaders are already stored. An index entry whose `end` lies past the
// store size, and store bytes past the last indexed `end`, are a cut-short
// tail (crash): the next writer truncates both, readers ignore them.
// Bytecode does not depend on the driver, so the store survives driver updates.
#pragma once
#include <stdint.h>

#define TESSERA_SHADER_MAGIC "TSRSHDR"  // 8 bytes with the terminator
#define TESSERA_SHADER_VERSION 1u
#define TESSERA_SHADER_INDEX_MAGIC "TSRSIDX"

enum TesseraShaderStage {
    TESSERA_STAGE_VS = 0,
    TESSERA_STAGE_HS = 1,
    TESSERA_STAGE_DS = 2,
    TESSERA_STAGE_GS = 3,
    TESSERA_STAGE_PS = 4,
    TESSERA_STAGE_CS = 5,
    TESSERA_STAGE_GS_SO = 6,  // geometry shader with stream output (needs the SO declaration)
};

// Bits in TesseraShaderEntry.flags and in `d` of SHADER session records.
#define TESSERA_SHADER_DXBC 0x1u  // container with "DXBC" magic; hash is its checksum
#define TESSERA_SHADER_DXIL 0x2u  // the container holds a DXIL part
#define TESSERA_SHADER_NEW 0x4u   // session record only: not in the store when created

#pragma pack(push, 1)
typedef struct TesseraShaderHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t entry_size;
    uint32_t reserved;
} TesseraShaderHeader;

typedef struct TesseraShaderEntry {
    uint64_t hash;   // first 8 bytes of the DXBC checksum, or FNV-1a 64 of the bytes
    uint32_t size;   // bytecode bytes that follow
    uint16_t stage;  // TesseraShaderStage
    uint16_t flags;
} TesseraShaderEntry;

typedef struct TesseraShaderIndexEntry {
    uint64_t hash;
    uint64_t end;  // store offset just past this entry (bytecode and padding)
} TesseraShaderIndexEntry;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(TesseraShaderHeader) == 24, "header layout");
static_assert(sizeof(TesseraShaderEntry) == 16, "entry layout");
static_assert(sizeof(TesseraShaderIndexEntry) == 16, "index entry layout");
#endif
