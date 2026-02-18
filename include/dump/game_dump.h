// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

// Handle ranges for dump indexing (also used by MTP adapter)
#define MTP_HANDLE_DUMP_BASE            0x00040000
#define MTP_HANDLE_DUMP_CAT_MERGED      0x00040001
#define MTP_HANDLE_DUMP_CAT_SEPARATE    0x00040002

#define MTP_HANDLE_DUMP_MERGED_START    0x00040010
#define MTP_HANDLE_DUMP_MERGED_END      0x000401FF

#define MTP_HANDLE_DUMP_SEP_FOLDER_START    0x00040200
#define MTP_HANDLE_DUMP_SEP_FOLDER_END      0x000403FF

#define MTP_HANDLE_DUMP_SEP_FILE_START      0x00040400
#define MTP_HANDLE_DUMP_SEP_FILE_END        0x00041FFF

// Maximum counts
#define DUMP_MAX_GAMES          512
#define DUMP_MAX_FILES_PER_NSP  48
#define DUMP_MAX_CONTENT_METAS  16

// One file entry within a virtual NSP (NCA, ticket, or cert)
typedef struct {
    char filename[64];              // e.g. "abcdef0123456789.nca"
    NcmContentId content_id;        // NCA content ID
    NcmStorageId storage_id;        // Where the NCA lives (SD or NAND)
    u64 size;                       // NCA file size
    u64 offset_in_data;             // Offset within NSP data region
    bool in_memory;                 // True for ticket/cert (small, buffered)
    u8* memory_data;                // Pointer to ticket/cert buffer
    u32 memory_size;                // Size of in-memory data
} DumpNspFileEntry;

// Pre-computed NSP layout for one virtual .nsp
typedef struct {
    bool computed;
    u64 total_nsp_size;             // Full NSP size (header + data)
    u8* pfs0_header;                // PFS0 header + file entries + string table
    u32 pfs0_header_size;           // Size of header region
    DumpNspFileEntry files[DUMP_MAX_FILES_PER_NSP];
    u32 file_count;
    u8* ticket_data;                // Exported ticket (heap alloc, nullable)
    u32 ticket_size;
    u8* cert_data;                  // Cert chain (heap alloc, nullable)
    u32 cert_size;
} DumpNspLayout;

// Individual content meta entry for separate mode
typedef struct {
    NcmContentMetaKey key;
    u32 handle;                     // Handle for this individual .nsp
    char filename[256];
    DumpNspLayout layout;
} DumpContentMetaEntry;

// One installed game entry
typedef struct {
    u64 application_id;
    char game_name[256];
    u32 version;                    // Latest version
    bool is_on_sd;                  // true=SD, false=NAND

    // Merged mode
    u32 merged_handle;              // Handle for merged .nsp virtual file
    DumpNspLayout merged_layout;

    // Separate mode
    u32 separate_folder_handle;     // Handle for game's folder in Separate/
    u32 content_meta_count;
    DumpContentMetaEntry content_metas[DUMP_MAX_CONTENT_METAS];
} DumpGameEntry;

// Top-level dump context
typedef struct {
    bool initialized;
    DumpGameEntry* games;
    u32 game_count;
    u32 max_games;

    // NCM handles
    NcmContentStorage sd_storage;
    NcmContentStorage nand_storage;
    NcmContentMetaDatabase sd_meta_db;
    NcmContentMetaDatabase nand_meta_db;
    bool sd_storage_open, nand_storage_open;
    bool sd_meta_db_open, nand_meta_db_open;
    bool ncm_initialized;
    bool ns_initialized;

    // ES service state (managed by libnx-ext)
    bool es_initialized;

    bool games_enumerated;
    bool needs_refresh;
    Mutex dump_mutex;
} DumpContext;

// Initialize dump subsystem
Result dumpInit(DumpContext* ctx);

// Cleanup dump subsystem
void dumpExit(DumpContext* ctx);

// Pre-initialize services (must be called from main thread)
void dumpPreInitServices(DumpContext* ctx);

// Refresh game list if needed (must be called from main thread)
void dumpRefreshIfNeeded(DumpContext* ctx);

// Enumerate installed games (populates ctx->games); call under dump_mutex or when safe
void dumpEnumerateGames(DumpContext* ctx);

// Ensure NSP layout is computed for a game's merged NSP
void dumpEnsureMergedLayout(DumpContext* ctx, DumpGameEntry* game);

// Ensure NSP layout is computed for a game's individual content meta NSP
void dumpEnsureSeparateLayout(DumpContext* ctx, DumpGameEntry* game, u32 meta_idx);

// Read virtual NSP data from a pre-computed layout (streaming)
// Returns bytes read, or -1 on error, 0 if offset past end
s64 dumpReadNspData(DumpContext* ctx, DumpNspLayout* layout, u64 offset, void* buffer, u64 size);

#ifdef __cplusplus
}
#endif
