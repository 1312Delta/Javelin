// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include "install/nca_install.h"
#include "install/cnmt.h"
#include <string.h>
#include <stdlib.h>
#include <sys/statvfs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_BUFFER_SIZE (16 * 1024 * 1024)

typedef enum {
    STREAM_STATE_IDLE,
    STREAM_STATE_RECEIVING,
    STREAM_STATE_PARSING,
    STREAM_STATE_INSTALLING,
    STREAM_STATE_COMPLETE,
    STREAM_STATE_ERROR
} StreamInstallState;

typedef enum {
    STREAM_TYPE_UNKNOWN,
    STREAM_TYPE_NSP,
    STREAM_TYPE_XCI
} StreamInstallType;

typedef struct {
    // Buffer management
    u8* buffer;
    u64 buffer_size;
    u64 buffer_pos;     // Current write position
    u64 read_pos;       // Current read position
    u64 total_received; // Total bytes received from MTP

    // File info
    char filename[256];
    u64 file_size;
    StreamInstallType file_type;

    // PFS0/NSP parsing state
    struct {
        u32 num_files;
        u64 string_table_size;
        u64 data_offset;
        bool header_parsed;
    } pfs0;

    // Current NCA being installed
    u32 current_nca_index;
    u64 nca_offset;        // Offset within current NCA
    u64 nca_size;          // Size of current NCA
    NcmContentId nca_id;
    NcmPlaceHolderId placeholder_id;
    bool ncaInstalling;

    // Installation context
    NcaInstallContext* nca_ctx;
    InstallTarget target;

    // State
    StreamInstallState state;
    Result last_error;
    u64 title_id;

    // CNMT info
    bool cnmt_found;
    CnmtContext cnmt_ctx;
    NcmContentId cnmt_id;
    u64 cnmt_size;

    // Pre-allocated write buffer for NCA installation (avoids per-call malloc)
    u8* write_buffer;
    u64 write_buffer_size;

} StreamInstallContext;

Result streamInstallInit(StreamInstallContext* ctx, InstallTarget target);
void streamInstallExit(StreamInstallContext* ctx);
Result streamInstallStart(StreamInstallContext* ctx, const char* filename, u64 file_size);
s64 streamInstallProcessData(StreamInstallContext* ctx, const void* data, u64 size);
Result streamInstallFinalize(StreamInstallContext* ctx);
StreamInstallState streamInstallGetState(const StreamInstallContext* ctx);
float streamInstallGetProgress(const StreamInstallContext* ctx);
u64 streamInstallGetTitleId(const StreamInstallContext* ctx);
void streamInstallReset(StreamInstallContext* ctx);

#ifdef __cplusplus
}
#endif
