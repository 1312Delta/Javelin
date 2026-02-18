// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/stream_install.h"
#include "install/nca_install.h"
#include "install/cnmt.h"
#include "mtp_log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <cstdio>

#define STREAM_DEBUG 1

#pragma pack(push, 1)
typedef struct {
    u32 magic;
    u32 num_files;
    u64 string_table_size;
    u32 reserved;
} Pfs0Header;

typedef struct {
    u64 offset;
    u64 size;
    u32 string_offset;
    u32 reserved;
} Pfs0FileEntry;
#pragma pack(pop)

Result streamInstallInit(StreamInstallContext* ctx, InstallTarget target) {
    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(ctx, 0, sizeof(StreamInstallContext));
    ctx->target = target;
    ctx->state = STREAM_STATE_IDLE;

    ctx->buffer = (u8*)malloc(STREAM_BUFFER_SIZE);
    if (!ctx->buffer) {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    ctx->buffer_size = STREAM_BUFFER_SIZE;

    // Pre-allocate write buffer for NCA installation to avoid per-call malloc/free
    ctx->write_buffer_size = 1024 * 1024;
    ctx->write_buffer = (u8*)malloc(ctx->write_buffer_size);
    if (!ctx->write_buffer) {
        free(ctx->buffer);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    ctx->nca_ctx = (NcaInstallContext*)malloc(sizeof(NcaInstallContext));
    if (!ctx->nca_ctx) {
        free(ctx->write_buffer);
        free(ctx->buffer);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    Result rc = ncaInstallInit(ctx->nca_ctx, target);
    if (R_FAILED(rc)) {
        free(ctx->nca_ctx);
        free(ctx->write_buffer);
        free(ctx->buffer);
        return rc;
    }

#if STREAM_DEBUG
    LOG_INFO("Stream Install: Initialized with %zu MB buffer", STREAM_BUFFER_SIZE / (1024 * 1024));
#endif

    return 0;
}

void streamInstallExit(StreamInstallContext* ctx) {
    if (!ctx) return;

    if (ctx->nca_ctx) {
        ncaInstallExit(ctx->nca_ctx);
        free(ctx->nca_ctx);
        ctx->nca_ctx = NULL;
    }

    if (ctx->buffer) {
        free(ctx->buffer);
        ctx->buffer = NULL;
    }

    if (ctx->write_buffer) {
        free(ctx->write_buffer);
        ctx->write_buffer = NULL;
    }

    if (ctx->cnmt_found) {
        cnmtFree(&ctx->cnmt_ctx);
        ctx->cnmt_found = false;
    }

    memset(ctx, 0, sizeof(StreamInstallContext));
}

void streamInstallReset(StreamInstallContext* ctx) {
    if (!ctx) return;

    ctx->buffer_pos = 0;
    ctx->read_pos = 0;
    ctx->total_received = 0;
    ctx->current_nca_index = 0;
    ctx->nca_offset = 0;
    ctx->ncaInstalling = false;
    ctx->cnmt_found = false;
    ctx->state = STREAM_STATE_IDLE;
    ctx->file_type = STREAM_TYPE_UNKNOWN;

    (void)ctx->nca_ctx;
}

Result streamInstallStart(StreamInstallContext* ctx, const char* filename, u64 file_size) {
    if (!ctx || !filename) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (ctx->state != STREAM_STATE_IDLE) {
        LOG_WARN("Stream Install: Already busy, resetting");
        streamInstallReset(ctx);
    }

    strncpy(ctx->filename, filename, sizeof(ctx->filename) - 1);
    ctx->file_size = file_size;
    ctx->state = STREAM_STATE_RECEIVING;

    size_t len = strlen(filename);
    const char* ext = filename + (len > 4 ? len - 4 : 0);

    if (strcasecmp(ext, ".nsp") == 0) {
        ctx->file_type = STREAM_TYPE_NSP;
    } else if (strcasecmp(ext, ".xci") == 0) {
        ctx->file_type = STREAM_TYPE_XCI;
    } else {
        LOG_ERROR("Stream Install: Unknown file type: %s", ext);
        ctx->state = STREAM_STATE_ERROR;
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    ctx->buffer_pos = 0;
    ctx->read_pos = 0;
    ctx->total_received = 0;

#if STREAM_DEBUG
    LOG_INFO("Stream Install: Started receiving %s (%.2f MB)", filename, file_size / (1024.0 * 1024.0));
#endif

    return 0;
}

static u64 streamRead(StreamInstallContext* ctx, void* out, u64 size) {
    if (!ctx || !out) return 0;

    u64 available = ctx->buffer_pos - ctx->read_pos;
    if (size > available) size = available;

    u64 read = 0;
    while (read < size) {
        u64 buffer_offset = ctx->read_pos % ctx->buffer_size;
        u64 chunk = ctx->buffer_size - buffer_offset;
        if (chunk > (size - read)) chunk = size - read;

        memcpy((u8*)out + read, ctx->buffer + buffer_offset, chunk);
        ctx->read_pos += chunk;
        read += chunk;
    }

    return read;
}

static void streamSkip(StreamInstallContext* ctx, u64 size) {
    if (!ctx) return;

    u64 available = ctx->buffer_pos - ctx->read_pos;
    if (size > available) size = available;
    ctx->read_pos += size;
}

static u64 streamAvailable(const StreamInstallContext* ctx) {
    if (!ctx) return 0;
    return ctx->buffer_pos - ctx->read_pos;
}

static bool parsePfs0Header(StreamInstallContext* ctx) {
    Pfs0Header header;
    if (streamRead(ctx, &header, sizeof(header)) != sizeof(header)) {
        return false;  // Not enough data yet
    }

    if (header.magic != 0x30534650) {
        LOG_ERROR("Stream Install: Invalid PFS0 magic: 0x%08X", header.magic);
        return false;
    }

    ctx->pfs0.num_files = header.num_files;
    ctx->pfs0.string_table_size = header.string_table_size;
    ctx->pfs0.data_offset = sizeof(Pfs0Header) + header.num_files * sizeof(Pfs0FileEntry) + header.string_table_size;
    ctx->pfs0.header_parsed = true;

#if STREAM_DEBUG
    LOG_INFO("Stream Install: PFS0 header - %u files, data at offset 0x%lX",
             header.num_files, ctx->pfs0.data_offset);
#endif

    return true;
}

static bool getPfs0FileEntry(StreamInstallContext* ctx, u32 index, Pfs0FileEntry* entry) {
    if (!ctx || !entry || !ctx->pfs0.header_parsed) return false;
    if (index >= ctx->pfs0.num_files) return false;

    u64 entry_offset = sizeof(Pfs0Header) + index * sizeof(Pfs0FileEntry);
    u64 current_offset = ctx->buffer_pos - ctx->read_pos;

    if (entry_offset > current_offset) return false;

    streamSkip(ctx, entry_offset);
    if (streamRead(ctx, entry, sizeof(Pfs0FileEntry)) != sizeof(Pfs0FileEntry)) {
        return false;
    }

    ctx->read_pos = sizeof(Pfs0Header);
    return true;
}

static Result installCurrentNca(StreamInstallContext* ctx) {
    if (!ctx->ncaInstalling) return 0;

    // Use pre-allocated write buffer instead of per-call malloc/free
    u8* buffer = ctx->write_buffer;
    u64 buf_size = ctx->write_buffer_size;

    Result rc = 0;
    u64 total_written = 0;

    while (ctx->nca_offset < ctx->nca_size) {
        u64 available = streamAvailable(ctx);
        if (available == 0) break;

        u64 to_write = ctx->nca_size - ctx->nca_offset;
        if (to_write > buf_size) to_write = buf_size;
        if (to_write > available) to_write = available;

        u64 read = streamRead(ctx, buffer, to_write);
        if (read == 0) break;

        rc = ncmContentStorageWritePlaceHolder(&ctx->nca_ctx->content_storage,
                                               &ctx->placeholder_id,
                                               ctx->nca_offset, buffer, read);
        if (R_FAILED(rc)) {
            LOG_ERROR("Stream Install: Write failed at offset 0x%lX: 0x%08X", ctx->nca_offset, rc);
            break;
        }

        ctx->nca_offset += read;
        total_written += read;

#if STREAM_DEBUG
        if ((total_written % (10 * 1024 * 1024)) == 0 || ctx->nca_offset == ctx->nca_size) {
            LOG_DEBUG("Stream Install: NCA progress: %lu / %lu bytes",
                     ctx->nca_offset, ctx->nca_size);
        }
#endif
    }

    if (ctx->nca_offset >= ctx->nca_size) {
        rc = ncmContentStorageRegister(&ctx->nca_ctx->content_storage,
                                       &ctx->nca_id, &ctx->placeholder_id);
        ncmContentStorageDeletePlaceHolder(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);

        if (R_FAILED(rc)) {
            LOG_ERROR("Stream Install: Failed to register NCA: 0x%08X", rc);
            ctx->state = STREAM_STATE_ERROR;
            return rc;
        }

        ctx->ncaInstalling = false;
        ctx->nca_offset = 0;

#if STREAM_DEBUG
        LOG_INFO("Stream Install: NCA installed successfully");
#endif
    }

    return rc;
}

static Result installCnmtNca(StreamInstallContext* ctx) {
    if (ctx->file_type != STREAM_TYPE_NSP || !ctx->pfs0.header_parsed) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    for (u32 i = 0; i < ctx->pfs0.num_files; i++) {
        Pfs0FileEntry entry;
        if (!getPfs0FileEntry(ctx, i, &entry)) continue;

        // Get filename
        ctx->read_pos = sizeof(Pfs0Header) + ctx->pfs0.num_files * sizeof(Pfs0FileEntry) + entry.string_offset;

        char filename[64];
        u64 read = streamRead(ctx, filename, sizeof(filename) - 1);
        filename[read] = '\0';

        size_t len = strlen(filename);
        if (len > 9 && strcasecmp(filename + len - 9, ".cnmt.nca") == 0) {
            LOG_INFO("Stream Install: Found CNMT NCA: %s", filename);

            for (int j = 0; j < 16 && filename[j * 2] != '\0'; j++) {
                char hex_byte[3] = {filename[j * 2], filename[j * 2 + 1], '\0'};
                ctx->cnmt_id.c[j] = (u8)strtoul(hex_byte, NULL, 16);
            }

            ctx->cnmt_size = entry.size;
            ctx->current_nca_index = i;
            ctx->nca_size = entry.size;
            ctx->nca_offset = 0;
            ctx->nca_id = ctx->cnmt_id;

            // Create placeholder
            ncmContentStorageGeneratePlaceHolderId(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);
            ncmContentStorageDeletePlaceHolder(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);

            Result rc = ncmContentStorageCreatePlaceHolder(&ctx->nca_ctx->content_storage,
                                                           &ctx->nca_id,
                                                           &ctx->placeholder_id,
                                                           ctx->cnmt_size);
            if (R_FAILED(rc)) {
                LOG_ERROR("Stream Install: Failed to create CNMT placeholder: 0x%08X", rc);
                return rc;
            }

            ctx->ncaInstalling = true;
            ctx->read_pos = ctx->pfs0.data_offset + entry.offset;
            return 0;
        }
    }

    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

static Result startNextContentNca(StreamInstallContext* ctx) {
    if (!ctx->cnmt_found) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    for (u32 i = ctx->current_nca_index + 1; i < ctx->pfs0.num_files; i++) {
        Pfs0FileEntry entry;
        if (!getPfs0FileEntry(ctx, i, &entry)) {
            return 0x189;
        }

        ctx->read_pos = sizeof(Pfs0Header) + ctx->pfs0.num_files * sizeof(Pfs0FileEntry) + entry.string_offset;

        char filename[64];
        streamRead(ctx, filename, sizeof(filename) - 1);
        filename[strlen(filename)] = '\0';

        bool found = false;
        NcmContentId content_id = {0};

        for (u32 j = 0; j < ctx->cnmt_ctx.content_count; j++) {
            char nca_filename[64];
            snprintf(nca_filename, sizeof(nca_filename),
                    "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.nca",
                    ctx->cnmt_ctx.content_records[j].content_id.c[0],
                    ctx->cnmt_ctx.content_records[j].content_id.c[1],
                    ctx->cnmt_ctx.content_records[j].content_id.c[2],
                    ctx->cnmt_ctx.content_records[j].content_id.c[3],
                    ctx->cnmt_ctx.content_records[j].content_id.c[4],
                    ctx->cnmt_ctx.content_records[j].content_id.c[5],
                    ctx->cnmt_ctx.content_records[j].content_id.c[6],
                    ctx->cnmt_ctx.content_records[j].content_id.c[7],
                    ctx->cnmt_ctx.content_records[j].content_id.c[8],
                    ctx->cnmt_ctx.content_records[j].content_id.c[9],
                    ctx->cnmt_ctx.content_records[j].content_id.c[10],
                    ctx->cnmt_ctx.content_records[j].content_id.c[11],
                    ctx->cnmt_ctx.content_records[j].content_id.c[12],
                    ctx->cnmt_ctx.content_records[j].content_id.c[13],
                    ctx->cnmt_ctx.content_records[j].content_id.c[14],
                    ctx->cnmt_ctx.content_records[j].content_id.c[15]);

            if (strcmp(filename, nca_filename) == 0) {
                content_id = ctx->cnmt_ctx.content_records[j].content_id;
                found = true;
                break;
            }
        }

        if (found) {
            ctx->current_nca_index = i;
            ctx->nca_id = content_id;
            ctx->nca_size = entry.size;
            ctx->nca_offset = 0;

            // Create placeholder
            ncmContentStorageGeneratePlaceHolderId(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);
            ncmContentStorageDeletePlaceHolder(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);

            Result rc = ncmContentStorageCreatePlaceHolder(&ctx->nca_ctx->content_storage,
                                                           &ctx->nca_id,
                                                           &ctx->placeholder_id,
                                                           ctx->nca_size);
            if (R_FAILED(rc)) {
                LOG_ERROR("Stream Install: Failed to create placeholder: 0x%08X", rc);
                return rc;
            }

            ctx->ncaInstalling = true;
            ctx->read_pos = ctx->pfs0.data_offset + entry.offset;
            LOG_INFO("Stream Install: Installing NCA %u/%u: %s",
                     ctx->current_nca_index, ctx->pfs0.num_files, filename);
            return 0;
        }
    }

    ctx->state = STREAM_STATE_COMPLETE;
    return 0;
}

static Result readCnmtFromInstalledNca(StreamInstallContext* ctx) {
    char cnmt_path[FS_MAX_PATH];
    Result rc = ncmContentStorageGetPath(&ctx->nca_ctx->content_storage, cnmt_path,
                                         sizeof(cnmt_path), &ctx->cnmt_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to get CNMT path: 0x%08X", rc);
        return rc;
    }

    FsFileSystem cnmt_fs;
    FsContentAttributes attrs = {};
    rc = fsOpenFileSystemWithId(&cnmt_fs, 0, FsFileSystemType_ContentMeta, cnmt_path, attrs);
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to open CNMT filesystem: 0x%08X", rc);
        return rc;
    }

    FsDir dir;
    rc = fsFsOpenDirectory(&cnmt_fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        fsFsClose(&cnmt_fs);
        return rc;
    }

    FsDirectoryEntry entry;
    s64 total_entries;
    char cnmt_file_path[FS_MAX_PATH] = {0};

    while (fsDirRead(&dir, &total_entries, 1, &entry) == 0 && total_entries > 0) {
        size_t len = strlen(entry.name);
        if (len > 5 && strcasecmp(entry.name + len - 5, ".cnmt") == 0) {
            snprintf(cnmt_file_path, sizeof(cnmt_file_path), "/%s", entry.name);
            break;
        }
    }
    fsDirClose(&dir);

    if (cnmt_file_path[0] == '\0') {
        fsFsClose(&cnmt_fs);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    FsFile cnmt_file;
    rc = fsFsOpenFile(&cnmt_fs, cnmt_file_path, FsOpenMode_Read, &cnmt_file);
    if (R_FAILED(rc)) {
        fsFsClose(&cnmt_fs);
        return rc;
    }

    s64 cnmt_size;
    fsFileGetSize(&cnmt_file, &cnmt_size);

    u8* cnmt_data = (u8*)malloc(cnmt_size);
    if (!cnmt_data) {
        fsFileClose(&cnmt_file);
        fsFsClose(&cnmt_fs);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    u64 bytes_read;
    rc = fsFileRead(&cnmt_file, 0, cnmt_data, cnmt_size, FsReadOption_None, &bytes_read);
    fsFileClose(&cnmt_file);
    fsFsClose(&cnmt_fs);

    if (R_FAILED(rc) || bytes_read != (u64)cnmt_size) {
        free(cnmt_data);
        return rc;
    }

    if (!cnmtParse(&ctx->cnmt_ctx, cnmt_data, cnmt_size)) {
        free(cnmt_data);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    free(cnmt_data);
    ctx->cnmt_found = true;

    LOG_INFO("Stream Install: CNMT parsed - Title ID: 0x%016lX, %u contents",
             ctx->cnmt_ctx.header.title_id, ctx->cnmt_ctx.content_count);

    return 0;
}

s64 streamInstallProcessData(StreamInstallContext* ctx, const void* data, u64 size) {
    if (!ctx || !data || size == 0) return -1;
    if (ctx->state == STREAM_STATE_ERROR) return -1;

    u64 write_remaining = size;
    u64 data_offset = 0;

    while (write_remaining > 0) {
        u64 buffer_offset = ctx->buffer_pos % ctx->buffer_size;
        u64 contiguous_space = ctx->buffer_size - buffer_offset;

        u64 unread_data = ctx->buffer_pos - ctx->read_pos;
        u64 free_space = ctx->buffer_size - unread_data;

        if (free_space == 0) {
            break;
        }

        u64 to_write = contiguous_space;
        if (to_write > free_space) to_write = free_space;
        if (to_write > write_remaining) to_write = write_remaining;

        memcpy(ctx->buffer + buffer_offset, (const u8*)data + data_offset, to_write);
        ctx->buffer_pos += to_write;
        ctx->total_received += to_write;
        data_offset += to_write;
        write_remaining -= to_write;
    }

    Result rc = 0;

    switch (ctx->state) {
        case STREAM_STATE_RECEIVING:
            if (!ctx->pfs0.header_parsed && streamAvailable(ctx) >= sizeof(Pfs0Header)) {
                if (!parsePfs0Header(ctx)) {
                    LOG_WARN("Stream Install: Not PFS0, trying XCI");
                    ctx->state = STREAM_STATE_ERROR;
                    return -1;
                }
                ctx->state = STREAM_STATE_PARSING;

                rc = installCnmtNca(ctx);
                if (R_FAILED(rc)) {
                    LOG_ERROR("Stream Install: Failed to start CNMT: 0x%08X", rc);
                    ctx->state = STREAM_STATE_ERROR;
                    return -1;
                }
            }
            break;

        case STREAM_STATE_PARSING:
        case STREAM_STATE_INSTALLING:
            if (ctx->ncaInstalling) {
                rc = installCurrentNca(ctx);
                if (R_FAILED(rc)) {
                    ctx->state = STREAM_STATE_ERROR;
                    return -1;
                }

                if (!ctx->ncaInstalling && !ctx->cnmt_found) {
                    rc = readCnmtFromInstalledNca(ctx);
                    if (R_SUCCEEDED(rc)) {
                        LOG_INFO("Stream Install: CNMT loaded, starting content NCAs");
                        ctx->current_nca_index++;
                        rc = startNextContentNca(ctx);
                    } else {
                        LOG_ERROR("Stream Install: Failed to read CNMT: 0x%08X", rc);
                        ctx->state = STREAM_STATE_ERROR;
                    }
                } else if (!ctx->ncaInstalling && ctx->cnmt_found) {
                    rc = startNextContentNca(ctx);
                }
            }
            break;

        case STREAM_STATE_COMPLETE:
        case STREAM_STATE_ERROR:
            break;

        default:
            break;
    }

    return data_offset;
}

Result streamInstallFinalize(StreamInstallContext* ctx) {
    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // Wait for any pending installation to complete
    while (ctx->ncaInstalling) {
        Result rc = installCurrentNca(ctx);
        if (R_FAILED(rc)) {
            ctx->state = STREAM_STATE_ERROR;
            return rc;
        }
        if (!ctx->ncaInstalling) break;
        svcSleepThread(1000000ULL);
    }

    if (ctx->state == STREAM_STATE_ERROR) {
        LOG_ERROR("Stream Install: Finalized with error");
        return ctx->last_error;
    }

    if (!ctx->cnmt_found) {
        LOG_ERROR("Stream Install: No CNMT found");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    NcmContentInfo cnmt_info;
    cnmt_info.content_id = ctx->cnmt_id;
    ncmU64ToContentInfoSize(ctx->cnmt_size & 0xFFFFFFFFFFFFULL, &cnmt_info);
    cnmt_info.content_type = NcmContentType_Meta;

    u8* install_meta_buffer;
    size_t install_meta_size;
    Result rc = cnmtBuildInstallContentMeta(&ctx->cnmt_ctx, &cnmt_info, false,
                                           &install_meta_buffer, &install_meta_size);
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to build meta: 0x%08X", rc);
        return rc;
    }

    NcmContentMetaKey meta_key = cnmtGetContentMetaKey(&ctx->cnmt_ctx);
    rc = ncmContentMetaDatabaseSet(&ctx->nca_ctx->meta_db, &meta_key,
                                   (NcmContentMetaHeader*)install_meta_buffer,
                                   install_meta_size);
    if (R_SUCCEEDED(rc)) {
        ncmContentMetaDatabaseCommit(&ctx->nca_ctx->meta_db);
        LOG_DEBUG("Stream Install: Content metadata registered");
    }
    free(install_meta_buffer);

    if (R_FAILED(rc)) {
        return rc;
    }

    u64 title_id = ctx->cnmt_ctx.header.title_id;
    ctx->title_id = title_id;

    u64 base_title_id;
    NcmContentMetaType content_type = (NcmContentMetaType)ctx->cnmt_ctx.header.type;

    switch (content_type) {
        case NcmContentMetaType_Patch:
            base_title_id = title_id ^ 0x800;
            break;
        case NcmContentMetaType_AddOnContent:
            base_title_id = (title_id ^ 0x1000) & ~0xFFFULL;
            break;
        default:
            base_title_id = title_id;
            break;
    }

    struct {
        NcmContentMetaKey meta_record;
        NcmStorageId storage_id;
    } storage_record;

    storage_record.meta_record = meta_key;
    storage_record.storage_id = ctx->nca_ctx->storage_id;

    Service ns_app_man_srv;
    bool got_ns_service = false;

    if (hosversionBefore(3, 0, 0)) {
        Service* srv = nsGetServiceSession_ApplicationManagerInterface();
        if (srv) {
            memcpy(&ns_app_man_srv, srv, sizeof(Service));
            got_ns_service = true;
        }
    } else {
        rc = nsGetApplicationManagerInterface(&ns_app_man_srv);
        got_ns_service = R_SUCCEEDED(rc);
    }

    if (got_ns_service) {
        struct {
            u8 last_modified_event;
            u8 padding[7];
            u64 application_id;
        } __attribute__((packed)) in = {
            .last_modified_event = 1,
            .application_id = base_title_id
        };

        rc = serviceDispatchIn(&ns_app_man_srv, 16, in,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
            .buffers = { { &storage_record, sizeof(storage_record) } },
        );

        if (hosversionAtLeast(3, 0, 0)) {
            serviceClose(&ns_app_man_srv);
        }

        if (R_SUCCEEDED(rc)) {
            LOG_INFO("Stream Install: ✓ Application record pushed!");
        } else {
            LOG_WARN("Stream Install: Failed to push record: 0x%08X", rc);
        }
    }

    ctx->state = STREAM_STATE_COMPLETE;
    LOG_INFO("Stream Install: ✓✓✓ COMPLETE! TitleID=0x%016lX", title_id);

    return 0;
}

StreamInstallState streamInstallGetState(const StreamInstallContext* ctx) {
    if (!ctx) return STREAM_STATE_ERROR;
    return ctx->state;
}

float streamInstallGetProgress(const StreamInstallContext* ctx) {
    if (!ctx || ctx->file_size == 0) return 0.0f;
    return (float)ctx->total_received / (float)ctx->file_size;
}

u64 streamInstallGetTitleId(const StreamInstallContext* ctx) {
    if (!ctx) return 0;
    return ctx->title_id;
}
