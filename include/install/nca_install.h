// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INSTALL_TARGET_SD,
    INSTALL_TARGET_NAND
} InstallTarget;

// Progress callback: (bytes_written, total_bytes, user_data)
typedef void (*NcaInstallProgressCb)(u64 bytes_written, u64 total_bytes, void* user_data);

typedef struct {
    NcmContentStorage content_storage;
    NcmContentMetaDatabase meta_db;
    NcmStorageId storage_id;
    bool storage_open;
    bool meta_db_open;
    bool ncm_initialized;

    InstallTarget target;

    NcaInstallProgressCb progress_cb;
    void* progress_user_data;
} NcaInstallContext;

Result ncaInstallInit(NcaInstallContext* ctx, InstallTarget target);
void ncaInstallExit(NcaInstallContext* ctx);
Result ncaInstallFile(NcaInstallContext* ctx, const char* nca_path,
                      NcmContentId* out_content_id);
Result ncaInstallNsp(NcaInstallContext* ctx, const char* nsp_path, u64* out_title_id);
Result ncaInstallXci(NcaInstallContext* ctx, const char* xci_path, u64* out_title_id);

#ifdef __cplusplus
}
#endif
