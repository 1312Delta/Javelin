// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

// Handle constants for gamecard virtual objects
#define MTP_HANDLE_GC_XCI_FILE      0x00090001  // The virtual XCI file
#define MTP_HANDLE_GC_NSP_FILE      0x00090002  // The virtual NSP file
#define MTP_HANDLE_GC_INVALID       0x00000000

// XCI layout constants (GCM header + HFS0 at 0x10000)
#define GCM_HEADER_SIZE             0x200       // GameCard Main Header
#define XCI_INITIAL_DATA_SIZE       0x200       // Initial data area (before GCM header)
#define XCI_HFS0_OFFSET             0x10000     // Root HFS0 starts here in real XCI

// Maximum NCA files in a gamecard secure partition
#define GC_MAX_NCA_FILES            128

// Key file paths on SD card
#define GC_PROD_KEYS_PATH           "/switch/prod.keys"
#define GC_TITLE_KEYS_PATH          "/switch/title.keys"

// Maximum key name/value length
#define GC_KEY_NAME_MAX             128
#define GC_KEY_VALUE_MAX            64
#define GC_MAX_KEYS                 256

// One loaded key entry
typedef struct {
    char name[GC_KEY_NAME_MAX];
    u8   value[GC_KEY_VALUE_MAX];
    u32  value_len;
} GcKeyEntry;

// Parsed key set
typedef struct {
    GcKeyEntry keys[GC_MAX_KEYS];
    u32 count;
    bool loaded;
} GcKeySet;

// One NCA entry in the gamecard secure partition
typedef struct {
    char name[256];             // Filename (e.g. "abc...def.nca")
    u64  offset;                // Offset within the secure partition data region
    u64  size;                  // NCA file size
} GcNcaEntry;

// Virtual HFS0 layout for the XCI we synthesise
typedef struct {
    bool  computed;

    // Header region (heap allocated)
    u8*   hdr_data;             // PFS-style HFS0 header blob
    u32   hdr_size;             // Size of header blob

    // File entries (mirrors GcNcaEntry but with offsets in the data region)
    GcNcaEntry files[GC_MAX_NCA_FILES];
    u32        file_count;

    // Total virtual XCI size presented to the host
    u64  total_size;

    // Offset where the secure partition data begins within our virtual XCI
    u64  data_region_start;
} GcVirtualXciLayout;

// Virtual NSP (PFS0) layout
typedef struct {
    bool  computed;

    // In-memory PFS0 header blob (magic + entries + string table)
    u8*   hdr_data;
    u32   hdr_size;

    // Offset in the virtual NSP where actual NCA data begins
    u64   data_region_start;

    // Total virtual NSP size presented to the host
    u64   total_size;

    // Per-file data offsets within the NSP data region
    GcNcaEntry files[GC_MAX_NCA_FILES];
    u32        file_count;
} GcVirtualNspLayout;

// Top-level gamecard context
typedef struct {
    bool initialized;

    // FsDeviceOperator for querying gamecard presence
    FsDeviceOperator dev_op;
    bool dev_op_open;

    // Gamecard handle and secure filesystem
    FsGameCardHandle gc_handle;
    bool gc_handle_valid;

    FsFileSystem gc_fs;         // Mounted on FsGameCardPartition_Secure
    bool gc_fs_open;

    // Cached title info
    char game_name[256];        // From NACP (if available, else TitleID hex)
    u64  title_id;              // Primary application TitleID from CNMT
    char version_str[32];

    // Virtual XCI layout
    GcVirtualXciLayout layout;

    // Virtual NSP layout (shares enumerated NCA list with XCI layout)
    GcVirtualNspLayout nsp_layout;

    // Key sets
    GcKeySet prod_keys;
    GcKeySet title_keys;
    bool keys_checked;          // True once we have attempted to load keys
    bool keys_loaded;           // True if at least prod.keys loaded successfully

    // State flags
    bool card_inserted;         // Reflects last-known card insertion state
    bool needs_rescan;          // Set when card insertion changes

    Mutex gc_mutex;
} GcContext;

// Initialize the gamecard dump context
Result gcInit(GcContext* ctx);

// Cleanup
void gcExit(GcContext* ctx);

// Re-check card insertion state; returns true if state changed
bool gcRefreshCardState(GcContext* ctx);

// Called once from the main thread to pre-init FS services
void gcPreInitServices(GcContext* ctx);

// Called from main thread each frame when active
void gcRefreshIfNeeded(GcContext* ctx);

// Read virtual XCI or NSP data (streaming)
// handle must be MTP_HANDLE_GC_XCI_FILE or MTP_HANDLE_GC_NSP_FILE
s64 gcReadObject(GcContext* ctx, u32 handle, u64 offset, void* buffer, u64 size);

#ifdef __cplusplus
}
#endif
