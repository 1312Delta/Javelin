// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_install.h"
#include "mtp/mtp_storage.h"
#include "install/nca_install.h"
#include "install/stream_install.h"
#include "mtp_log.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

bool installIsVirtualStorage(u32 storage_id) {
    return storage_id == MTP_STORAGE_SD_INSTALL ||
           storage_id == MTP_STORAGE_NAND_INSTALL;
}

bool installIsVirtualHandle(u32 handle) {
    u32 base = handle & 0x0FFF0000;
    return base == MTP_HANDLE_SD_INSTALL_BASE ||
           base == MTP_HANDLE_NAND_INSTALL_BASE;
}

Result installInit(InstallContext* ctx) {
    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(ctx, 0, sizeof(InstallContext));
    ctx->initialized = true;
    ctx->use_streaming = true;

    return 0;
}

void installExit(InstallContext* ctx) {
    if (!ctx) return;

    if (ctx->stream_ctx) {
        streamInstallExit(ctx->stream_ctx);
        free(ctx->stream_ctx);
        ctx->stream_ctx = NULL;
    }

    if (ctx->staging_file) {
        fclose((FILE*)ctx->staging_file);
        ctx->staging_file = NULL;
    }
    if (ctx->staging_path[0]) {
        remove(ctx->staging_path);
        ctx->staging_path[0] = '\0';
    }

    ctx->initialized = false;
}

bool installGetStorageInfo(InstallContext* ctx, u32 storage_id, void* out) {
    (void)ctx;
    if (!out) return false;

    MtpStorageInfo* info = (MtpStorageInfo*)out;
    memset(info, 0, sizeof(MtpStorageInfo));

    info->storage_id = storage_id;
    info->storage_type = 0x0003;
    info->filesystem_type = 0x0002;
    info->access_capability = 0x0000;
    info->mounted = true;

    struct statvfs vfs;
    if (statvfs("sdmc:/", &vfs) == 0) {
        info->max_capacity = (u64)vfs.f_blocks * vfs.f_frsize;
        info->free_space = (u64)vfs.f_bfree * vfs.f_frsize;
    } else {
        info->max_capacity = 32ULL * 1024 * 1024 * 1024;
        info->free_space = 16ULL * 1024 * 1024 * 1024;
    }

    if (storage_id == MTP_STORAGE_SD_INSTALL) {
        strncpy(info->description, "Install to SD Card (Streaming)", sizeof(info->description));
        strncpy(info->volume_label, "INSTALL_SD", sizeof(info->volume_label));
    } else if (storage_id == MTP_STORAGE_NAND_INSTALL) {
        strncpy(info->description, "Install to NAND (Streaming)", sizeof(info->description));
        strncpy(info->volume_label, "INSTALL_NAND", sizeof(info->volume_label));
    } else {
        return false;
    }

    return true;
}

u32 installGetObjectCount(InstallContext* ctx, u32 storage_id, u32 parent_handle) {
    (void)ctx;
    (void)storage_id;
    return (parent_handle == 0 || parent_handle == 0xFFFFFFFF) ? 1 : 0;
}

u32 installEnumObjects(InstallContext* ctx, u32 storage_id, u32 parent_handle,
                       u32* handles, u32 max_handles) {
    (void)ctx;
    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        if (max_handles > 0) {
            handles[0] = (storage_id == MTP_STORAGE_SD_INSTALL) ?
                         0x00050001 : 0x00060001;
            return 1;
        }
    }
    return 0;
}

bool installGetObjectInfo(InstallContext* ctx, u32 handle, void* out) {
    (void)ctx;
    if (!out) return false;

    MtpObject* obj = (MtpObject*)out;
    memset(obj, 0, sizeof(MtpObject));

    obj->handle = handle;
    obj->object_type = MTP_OBJECT_TYPE_FILE;
    obj->format = MTP_FORMAT_UNDEFINED;
    obj->parent_handle = 0xFFFFFFFF;
    obj->size = 0;

    u32 base = handle & 0x0FFF0000;
    if (base == MTP_HANDLE_SD_INSTALL_BASE) {
        obj->storage_id = MTP_STORAGE_SD_INSTALL;
        strncpy(obj->filename, "Drop NSP or XCI here (SD) - Direct Install.txt", MTP_MAX_FILENAME - 1);
    } else if (base == MTP_HANDLE_NAND_INSTALL_BASE) {
        obj->storage_id = MTP_STORAGE_NAND_INSTALL;
        strncpy(obj->filename, "Drop NSP or XCI here (NAND) - Direct Install.txt", MTP_MAX_FILENAME - 1);
    } else {
        return false;
    }

    return true;
}

static bool isInstallableFile(const char* filename) {
    if (!filename) return false;
    size_t len = strlen(filename);
    if (len < 5) return false;

    const char* ext = filename + len - 4;
    return (strcasecmp(ext, ".nsp") == 0 || strcasecmp(ext, ".xci") == 0);
}

u32 installCreateObject(InstallContext* ctx, u32 storage_id, u32 parent_handle,
                       const char* filename, u16 format, u64 size) {
    (void)parent_handle;
    (void)format;

    if (!ctx || !filename) return 0;

    if (!isInstallableFile(filename)) {
        LOG_WARN("Install: Rejecting non-installable file: %s", filename);
        return 0;
    }

    if (ctx->stream_ctx) {
        streamInstallExit(ctx->stream_ctx);
        free(ctx->stream_ctx);
        ctx->stream_ctx = NULL;
    }
    if (ctx->staging_file) {
        fclose((FILE*)ctx->staging_file);
        ctx->staging_file = NULL;
    }
    if (ctx->staging_path[0]) {
        remove(ctx->staging_path);
        ctx->staging_path[0] = '\0';
    }

    ctx->install_pending = true;
    ctx->install_size = size;
    ctx->install_written = 0;
    ctx->install_progress = 0;
    ctx->install_storage_id = storage_id;
    strncpy(ctx->install_filename, filename, INSTALL_MAX_FILENAME - 1);
    ctx->install_filename[INSTALL_MAX_FILENAME - 1] = '\0';

    if (ctx->use_streaming) {
        ctx->stream_ctx = (StreamInstallContext*)malloc(sizeof(StreamInstallContext));
        if (!ctx->stream_ctx) {
            LOG_ERROR("Install: Failed to allocate stream context");
            ctx->install_pending = false;
            return 0;
        }

        InstallTarget target = (storage_id == MTP_STORAGE_SD_INSTALL)
                                ? INSTALL_TARGET_SD : INSTALL_TARGET_NAND;

        Result rc = streamInstallInit(ctx->stream_ctx, target);
        if (R_FAILED(rc)) {
            LOG_ERROR("Install: Failed to init stream: 0x%08X", rc);
            free(ctx->stream_ctx);
            ctx->stream_ctx = NULL;
            ctx->install_pending = false;
            return 0;
        }

        rc = streamInstallStart(ctx->stream_ctx, filename, size);
        if (R_FAILED(rc)) {
            LOG_ERROR("Install: Failed to start stream: 0x%08X", rc);
            streamInstallExit(ctx->stream_ctx);
            free(ctx->stream_ctx);
            ctx->stream_ctx = NULL;
            ctx->install_pending = false;
            return 0;
        }

        LOG_INFO("Install: Started streaming %s (%.2f MB)",
                 filename, size / (1024.0 * 1024.0));
    } else {
        LOG_WARN("Install: SD staging fallback not implemented");
    }

    return (storage_id == MTP_STORAGE_SD_INSTALL) ? 0x00050001 : 0x00060001;
}

s64 installWriteObject(InstallContext* ctx, u32 handle, u64 offset,
                      const void* buffer, u64 size) {
    (void)handle;
    (void)offset;

    if (!ctx || !buffer || !ctx->install_pending) return -1;

    if (ctx->stream_ctx) {
        s64 consumed = streamInstallProcessData(ctx->stream_ctx, buffer, size);
        if (consumed < 0) {
            LOG_ERROR("Install: Stream processing failed");
            return -1;
        }

        ctx->install_written += consumed;
        if (ctx->install_size > 0) {
            ctx->install_progress = (u32)((ctx->install_written * 100) / ctx->install_size);
        }

        return consumed;
    } else {
        if (!ctx->staging_file) return -1;

        FILE* fp = (FILE*)ctx->staging_file;
        long current_pos = ftell(fp);
        if (current_pos < 0 || (u64)current_pos != offset) {
            if (fseek(fp, offset, SEEK_SET) != 0) {
                LOG_ERROR("Install: Failed to seek to offset %lu", offset);
                return -1;
            }
        }

        size_t written = fwrite(buffer, 1, size, fp);
        if (written != size) {
            LOG_ERROR("Install: Write failed - expected %lu, wrote %zu", size, written);
            return -1;
        }

        ctx->install_written += size;
        if (ctx->install_size > 0) {
            ctx->install_progress = (u32)((ctx->install_written * 100) / ctx->install_size);
        }

        return (s64)size;
    }
}

Result installFinalizeObject(InstallContext* ctx, u32 handle) {
    (void)handle;

    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = 0;

    if (ctx->stream_ctx) {
        rc = streamInstallFinalize(ctx->stream_ctx);

        if (R_SUCCEEDED(rc)) {
            u64 title_id = streamInstallGetTitleId(ctx->stream_ctx);
            LOG_INFO("Install: Stream install SUCCESS! TitleID=0x%016lX", title_id);
        } else {
            LOG_ERROR("Install: Stream install FAILED: 0x%08X", rc);
        }

        streamInstallExit(ctx->stream_ctx);
        free(ctx->stream_ctx);
        ctx->stream_ctx = NULL;
    } else {
        if (ctx->staging_file) {
            fflush((FILE*)ctx->staging_file);
            fclose((FILE*)ctx->staging_file);
            ctx->staging_file = NULL;
        }

        if (ctx->staging_path[0] == '\0') {
            LOG_ERROR("Install: No staging file path");
            ctx->install_pending = false;
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }

        struct stat st;
        if (stat(ctx->staging_path, &st) != 0) {
            LOG_ERROR("Install: Staging file not found: %s", ctx->staging_path);
            remove(ctx->staging_path);
            ctx->staging_path[0] = '\0';
            ctx->install_pending = false;
            return MAKERESULT(Module_Libnx, LibnxError_IoError);
        }

        LOG_INFO("Install: Staging complete - %s (%ld bytes)", ctx->staging_path, st.st_size);

        InstallTarget target = (ctx->install_storage_id == MTP_STORAGE_SD_INSTALL)
                                ? INSTALL_TARGET_SD : INSTALL_TARGET_NAND;

        NcaInstallContext nca_ctx;
        rc = ncaInstallInit(&nca_ctx, target);
        if (R_FAILED(rc)) {
            LOG_ERROR("Install: Failed to init NCA context: 0x%08X", rc);
            remove(ctx->staging_path);
            ctx->staging_path[0] = '\0';
            ctx->install_pending = false;
            return rc;
        }

        u64 title_id = 0;
        size_t filename_len = strlen(ctx->install_filename);
        const char* ext = ctx->install_filename + filename_len - 4;

        if (strcasecmp(ext, ".nsp") == 0) {
            LOG_INFO("Install: Installing NSP to %s...",
                     target == INSTALL_TARGET_SD ? "SD Card" : "NAND");
            rc = ncaInstallNsp(&nca_ctx, ctx->staging_path, &title_id);
        } else if (strcasecmp(ext, ".xci") == 0) {
            LOG_INFO("Install: Installing XCI to %s...",
                     target == INSTALL_TARGET_SD ? "SD Card" : "NAND");
            rc = ncaInstallXci(&nca_ctx, ctx->staging_path, &title_id);
        }

        ncaInstallExit(&nca_ctx);

        remove(ctx->staging_path);
        ctx->staging_path[0] = '\0';

        ctx->install_pending = false;
        ctx->install_progress = 0;
        ctx->install_filename[0] = '\0';

        if (R_SUCCEEDED(rc)) {
            LOG_INFO("Install: SUCCESS! Title ID: 0x%016lX", title_id);
        } else {
            LOG_ERROR("Install: FAILED with result 0x%08X", rc);
        }
    }

    ctx->install_pending = false;
    ctx->install_progress = 0;
    ctx->install_filename[0] = '\0';

    return rc;
}

bool installDeleteObject(InstallContext* ctx, u32 handle) {
    (void)ctx;
    (void)handle;
    return true;
}
