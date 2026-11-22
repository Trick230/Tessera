// SPDX-License-Identifier: GPL-3.0-or-later
// Session log written by tessera.dll during play and read by the app on the
// next launch. Source of truth for the format; the Rust side mirrors it.
//
// File: header, then fixed-size records until EOF. A file without a
// TESSERA_REC_END record was cut short (crash or kill) and is still valid
// up to its last whole record.
#pragma once
#include <stdint.h>

#define TESSERA_SESSION_MAGIC "TSRSESS"  // 8 bytes with the terminator
#define TESSERA_SESSION_VERSION 1u

// Bit in `flags` of TEXTURE2D / BUFFER records: created by code outside the
// game's folder (overlay, SDK, runtime); never managed by Tessera.
#define TESSERA_FLAG_FOREIGN 0x80000000u
// Bit in `flags` of TEXTURE2D / BUFFER records: eviction priority raised (M2).
#define TESSERA_FLAG_PRIORITY 0x40000000u
// Bit in `flags` of TEXTURE2D records: created as a tiled texture whose top
// mip Tessera may move to system memory while it is cold (M3/M4).
#define TESSERA_FLAG_TILED 0x20000000u
// Bit in `flags` of TEXTURE2D records: a render target the game had released,
// handed back instead of a new one (R1).
#define TESSERA_FLAG_RECYCLED 0x10000000u
// Bit in `flags` of TEXTURE2D / BUFFER records: created with initial data.
#define TESSERA_FLAG_INIT_DATA 0x08000000u
// Texture bytes describe compressed storage rather than the visible format.
#define TESSERA_FLAG_RECOMPRESSED 0x04000000u
// A game texture that passes the descriptor and tiled format checks for mip residency.
#define TESSERA_FLAG_RESIDENCY_CANDIDATE 0x02000000u

enum TesseraRecordKind {
    TESSERA_REC_FRAME = 1,      // a = CPU frame time in us (0 on the first frame),
                                // b = frame limiter wait before this Present in us
    TESSERA_REC_TEXTURE2D = 2,  // a = width, b = height, c = mips | array << 8,
                                // d = usage | misc << 8, flags = bind, bytes = estimate
    TESSERA_REC_BUFFER = 3,     // a = byte width, d = usage | misc << 8, flags = bind, bytes = a
    TESSERA_REC_SRV = 4,        // a = view dimension (0 if no desc)
    TESSERA_REC_DEVICE = 5,     // a = feature level, b = tiled resources tier (C4), d = create flags
    TESSERA_REC_SWAPCHAIN = 6,  // a = width, b = height, c = buffers, d = swap effect, flags = desc flags
                                // (| 0x100 when Tessera converted it to flip model, P2)
    TESSERA_REC_VRAM = 7,       // ~1 s samples: a = local budget MB (real), b = local usage MB,
                                // c = shared (non-local) usage MB, d = budget cap MB reported to
                                // the game (0 = none), bytes = local usage
    TESSERA_REC_SHADER = 8,     // a = stage, b = bytecode bytes, c = creation call us, d = TESSERA_SHADER_*
                                // bits (shader_store_format.h), bytes = shader hash
    TESSERA_REC_WORKSET = 9,    // ~1 s, M11: a = KB of sampled game textures bound in the last second,
                                // b = the same over the last 10 s, c = such textures alive, d = left
                                // untracked (tables full), flags = frames in the last second,
                                // bytes = bytes of the tracked textures alive. "Last second"
                                // includes the frame in flight at the previous tick, so at low
                                // frame rates the window is a frame longer.
    TESSERA_REC_RESIDENCY = 10, // ~1 s, M3-M5/M10: a = managed textures alive, b = with the top mip
                                // in system memory now, c = restores since the last record, d =
                                // evictions since the last record, flags = late restores (total),
                                // bytes = system memory holding top mips
    TESSERA_REC_SLOW_CREATE = 11, // a game texture/buffer create that took >= 1 ms, right after its
                                // record: a = call us, b = thread id, c = kind of that record,
                                // d = 1 if the thread also presents; format and bytes as there
    TESSERA_REC_MIP_FEEDBACK = 12,  // a = PS texture slot, b = requested mip, bytes = texture fingerprint
    TESSERA_REC_SLOW_D3D11 = 13, // qpc = end, bytes = start, format = TesseraD3D11Call,
                                // a = elapsed us, b = thread id, c = map mode or Flush1 context type,
                                // d = HRESULT (0 for void), flags = API flags and TESSERA_CALL_PRESENT_THREAD
    TESSERA_REC_MIP_SHADER_MISS = 14, // a = TesseraMipShaderMiss, b/c = shader object key low/high,
                                     // bytes = pixel shader hash (0 if unknown)
    TESSERA_REC_STREAM_IO = 15, // format = TesseraStreamIoKind, a = byte count, b/c = file key low/high,
                                // bytes = file offset, qpc = demand start or successful prefetch return
    TESSERA_REC_MIP_SHADER_CREATE = 16, // b/c = shader object key low/high, bytes = hash, flags = FOREIGN if applicable
    TESSERA_REC_END = 0xFFFF,   // a = records dropped (ring overrun), bytes = records written
};

enum TesseraMipShaderMiss {
    TESSERA_MIP_NO_SHADER = 1,
    TESSERA_MIP_UNTRACKED_SHADER = 2,
    TESSERA_MIP_CLASS_LINKAGE = 3,
    TESSERA_MIP_UNSUPPORTED_BYTECODE = 4,
    TESSERA_MIP_CACHE_MISS = 5,
    TESSERA_MIP_VARIANT_INSTALL_FAILED = 6,
};

enum TesseraStreamIoKind {
    TESSERA_STREAM_DEMAND = 1,
    TESSERA_STREAM_STARTUP_PREFETCH = 2,
    TESSERA_STREAM_MATERIAL_PREFETCH = 3,
};

#define TESSERA_CALL_PRESENT_THREAD 0x80000000u
enum TesseraD3D11Call {
    TESSERA_CALL_MAP = 1,
    TESSERA_CALL_GET_DATA = 2,
    TESSERA_CALL_FLUSH = 3,
    TESSERA_CALL_UPDATE_SUBRESOURCE = 4,
    TESSERA_CALL_FLUSH1 = 5,
    TESSERA_CALL_UPDATE_SUBRESOURCE1 = 6,
};

#pragma pack(push, 1)
typedef struct TesseraSessionHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t record_size;
    uint32_t pid;
    uint64_t qpc_frequency;
    uint64_t start_qpc;
    uint64_t start_filetime;   // UTC FILETIME
    uint16_t exe_path[260];    // UTF-16, NUL-terminated
} TesseraSessionHeader;

typedef struct TesseraRecord {
    uint64_t qpc;
    uint16_t kind;
    uint16_t format;  // DXGI_FORMAT where it applies
    uint32_t flags;
    uint32_t a, b, c, d;
    uint64_t bytes;
} TesseraRecord;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(TesseraSessionHeader) == 568, "header layout");
static_assert(sizeof(TesseraRecord) == 40, "record layout");
#endif
