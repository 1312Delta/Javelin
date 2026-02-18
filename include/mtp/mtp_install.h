// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include "install/stream_install.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_STORAGE_SD_INSTALL    0x00010005
#define MTP_STORAGE_NAND_INSTALL  0x00010006

#define MTP_HANDLE_SD_INSTALL_BASE  0x00050000
#define MTP_HANDLE_NAND_INSTALL_BASE 0x00060000

#define INSTALL_MAX_FILENAME 512

typedef struct {
    bool initialized;
    bool install_pending;
    bool use_streaming;  // Use streaming instead of SD staging
    char install_filename[INSTALL_MAX_FILENAME];
    u64 install_size;
    u64 install_written;
    u32 install_progress;
    u32 install_storage_id;

    // Legacy SD staging (deprecated, kept for fallback)
    char staging_path[768];
    void* staging_file;

    // Streaming installation context
    StreamInstallContext* stream_ctx;
} InstallContext;

bool installIsVirtualStorage(u32 storage_id);
bool installIsVirtualHandle(u32 handle);
Result installInit(InstallContext* ctx);
void installExit(InstallContext* ctx);
bool installGetStorageInfo(InstallContext* ctx, u32 storage_id, void* out);
u32 installGetObjectCount(InstallContext* ctx, u32 storage_id, u32 parent_handle);
u32 installEnumObjects(InstallContext* ctx, u32 storage_id, u32 parent_handle,
                       u32* handles, u32 max_handles);
bool installGetObjectInfo(InstallContext* ctx, u32 handle, void* out);
u32 installCreateObject(InstallContext* ctx, u32 storage_id, u32 parent_handle,
                       const char* filename, u16 format, u64 size);
s64 installWriteObject(InstallContext* ctx, u32 handle, u64 offset,
                      const void* buffer, u64 size);
Result installFinalizeObject(InstallContext* ctx, u32 handle);
bool installDeleteObject(InstallContext* ctx, u32 handle);

#ifdef __cplusplus
}
#endif
