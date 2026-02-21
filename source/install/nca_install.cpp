// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/nca_install.h"
#include "install/nsp_parser.h"
#include "install/xci_parser.h"
#include "install/ticket_utils.h"
#include "core/TransferEvents.h"
#include "core/Event.h"
#include "mtp_log.h"
#include "install/cnmt.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

Result ncaInstallInit(NcaInstallContext* ctx, InstallTarget target) {
    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(ctx, 0, sizeof(NcaInstallContext));
    ctx->target = target;

    if (target == INSTALL_TARGET_SD) {
        ctx->storage_id = NcmStorageId_SdCard;
    } else {
        ctx->storage_id = NcmStorageId_BuiltInUser;
    }

    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to initialize NCM: 0x%08X", rc);
        return rc;
    }
    ctx->ncm_initialized = true;

    rc = ncmOpenContentStorage(&ctx->content_storage, ctx->storage_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to open content storage: 0x%08X", rc);
        ncaInstallExit(ctx);
        return rc;
    }
    ctx->storage_open = true;

    rc = ncmOpenContentMetaDatabase(&ctx->meta_db, ctx->storage_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to open metadata database: 0x%08X", rc);
        ncaInstallExit(ctx);
        return rc;
    }
    ctx->meta_db_open = true;

    return 0;
}

void ncaInstallExit(NcaInstallContext* ctx) {
    if (!ctx) return;

    if (ctx->meta_db_open) {
        ncmContentMetaDatabaseClose(&ctx->meta_db);
        ctx->meta_db_open = false;
    }

    if (ctx->storage_open) {
        ncmContentStorageClose(&ctx->content_storage);
        ctx->storage_open = false;
    }

    if (ctx->ncm_initialized) {
        ncmExit();
        ctx->ncm_initialized = false;
    }

    memset(ctx, 0, sizeof(NcaInstallContext));
}

Result ncaInstallFile(NcaInstallContext* ctx, const char* nca_path,
                      NcmContentId* out_content_id) {
    if (!ctx || !ctx->storage_open || !nca_path) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    FILE* nca_fp = fopen(nca_path, "rb");
    if (!nca_fp) {
        LOG_ERROR("NCA Install: Failed to open NCA file: %s", nca_path);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    fseek(nca_fp, 0, SEEK_END);
    u64 nca_size = ftell(nca_fp);
    fseek(nca_fp, 0, SEEK_SET);

    NcmPlaceHolderId placeholder_id;
    Result rc = ncmContentStorageGeneratePlaceHolderId(&ctx->content_storage, &placeholder_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to generate placeholder ID: 0x%08X", rc);
        fclose(nca_fp);
        return rc;
    }

    NcmContentId content_id = {0};
    const char* filename = strrchr(nca_path, '/');
    if (filename) filename++;
    else filename = nca_path;

    for (int i = 0; i < 16 && filename[i * 2] != '\0'; i++) {
        char hex_byte[3] = {filename[i * 2], filename[i * 2 + 1], '\0'};
        content_id.c[i] = (u8)strtoul(hex_byte, NULL, 16);
    }

    rc = ncmContentStorageCreatePlaceHolder(&ctx->content_storage, &content_id,
                                            &placeholder_id, nca_size);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to create placeholder: 0x%08X", rc);
        fclose(nca_fp);
        return rc;
    }

    u8* buffer = (u8*)malloc(1024 * 1024);
    if (!buffer) {
        LOG_ERROR("NCA Install: Failed to allocate transfer buffer");
        ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
        fclose(nca_fp);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    u64 offset = 0;
    bool write_success = true;

    while (offset < nca_size) {
        u64 chunk_size = (nca_size - offset > 1024 * 1024) ? 1024 * 1024 : (nca_size - offset);

        size_t read_bytes = fread(buffer, 1, chunk_size, nca_fp);
        if (read_bytes != chunk_size) {
            LOG_ERROR("NCA Install: Read error at offset 0x%lX", offset);
            write_success = false;
            break;
        }

        rc = ncmContentStorageWritePlaceHolder(&ctx->content_storage, &placeholder_id,
                                               offset, buffer, read_bytes);
        if (R_FAILED(rc)) {
            LOG_ERROR("NCA Install: Write error at offset 0x%lX: 0x%08X", offset, rc);
            write_success = false;
            break;
        }

        offset += read_bytes;
    }

    free(buffer);
    fclose(nca_fp);

    if (!write_success) {
        ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    rc = ncmContentStorageRegister(&ctx->content_storage, &content_id, &placeholder_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to register content: 0x%08X", rc);
        ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
        return rc;
    }

    ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);

    if (out_content_id) {
        memcpy(out_content_id, &content_id, sizeof(NcmContentId));
    }

    return 0;
}

static Result readCnmtFromNca(NcaInstallContext* ctx, const NcmContentId* cnmt_id,
                              CnmtContext* out_cnmt) {
    char cnmt_path[FS_MAX_PATH];
    Result rc = ncmContentStorageGetPath(&ctx->content_storage, cnmt_path, sizeof(cnmt_path), cnmt_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to get CNMT NCA path: 0x%08X", rc);
        return rc;
    }

    LOG_INFO("NCA Install: Reading CNMT from: %s", cnmt_path);

    FsFileSystem cnmt_fs;
    rc = fsOpenFileSystemWithId(&cnmt_fs, 0, FsFileSystemType_ContentMeta, cnmt_path, FsContentAttributes_All);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to open CNMT filesystem: 0x%08X", rc);
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
        LOG_ERROR("NCA Install: No .cnmt file found in NCA");
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

    if (!cnmtParse(out_cnmt, cnmt_data, cnmt_size)) {
        free(cnmt_data);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    free(cnmt_data);
    return 0;
}

Result ncaInstallNsp(NcaInstallContext* ctx, const char* nsp_path, u64* out_title_id) {
    if (!ctx || !nsp_path) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    LOG_INFO("NCA Install: Installing NSP: %s", nsp_path);

    NspContext nsp;
    if (!nspOpen(&nsp, nsp_path)) {
        LOG_ERROR("NCA Install: Failed to open NSP file");
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    u32 file_count = nspGetFileCount(&nsp);
    LOG_INFO("NCA Install: NSP contains %u files", file_count);

    LOG_INFO("NCA Install: Step 1/4 - Installing tickets and certificates");
    for (u32 i = 0; i < file_count; i++) {
        const char* filename = nspGetFilename(&nsp, i);
        size_t len = filename ? strlen(filename) : 0;

        if (len > 4 && strcasecmp(filename + len - 4, ".tik") == 0) {
            char cert_name[256];
            strncpy(cert_name, filename, sizeof(cert_name) - 1);
            strcpy(cert_name + len - 4, ".cert");

            s32 cert_idx = nspFindFile(&nsp, cert_name);
            if (cert_idx < 0) continue;

            u64 tik_size = nspGetFileSize(&nsp, i);
            u8* tik_data = (u8*)malloc(tik_size);
            if (!tik_data) continue;

            if (nspReadFile(&nsp, i, 0, tik_data, tik_size) != (s64)tik_size) {
                free(tik_data);
                continue;
            }

            // Check if ticket is personalized and mismatches console
            u8 rights_id[16];
            u64 device_id;
            u32 account_id;
            if (checkTicketMismatch(tik_data, (u32)tik_size, rights_id, &device_id, &account_id)) {
                LOG_WARN("NCA Install: Personalized ticket detected with device ID mismatch!");
                LOG_WARN("NCA Install: Ticket Device ID: 0x%016lX, Account ID: 0x%08X", device_id, account_id);
                LOG_WARN("NCA Install: This ticket is tied to a different console.");
                LOG_WARN("NCA Install: Converting to common ticket...");

                // Auto-convert to common for regular installs
                // TODO: Add modal prompt like MTP install
                convertTicketToCommon(tik_data, (u32)tik_size);
            }

            u64 cert_size = nspGetFileSize(&nsp, cert_idx);
            u8* cert_data = (u8*)malloc(cert_size);
            if (!cert_data) {
                free(tik_data);
                continue;
            }

            if (nspReadFile(&nsp, cert_idx, 0, cert_data, cert_size) != (s64)cert_size) {
                free(cert_data);
                free(tik_data);
                continue;
            }

            Service es_srv;
            Result rc = smGetService(&es_srv, "es");
            if (R_SUCCEEDED(rc)) {
                rc = serviceDispatch(&es_srv, 1,
                    .buffer_attrs = {
                        SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                        SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                    },
                    .buffers = {
                        { tik_data, tik_size },
                        { cert_data, cert_size },
                    },
                );
                serviceClose(&es_srv);

                if (R_FAILED(rc)) {
                    LOG_WARN("NCA Install: Failed to import ticket: 0x%08X (may be okay)", rc);
                } else {
                    LOG_DEBUG("NCA Install: Imported ticket successfully");
                }
            }

            free(cert_data);
            free(tik_data);
        }
    }

    LOG_INFO("NCA Install: Step 2/4 - Installing CNMT NCAs");
    CnmtContext cnmt_ctx;
    NcmContentId cnmt_content_id = {0};
    bool found_cnmt = false;

    for (u32 i = 0; i < file_count; i++) {
        const char* filename = nspGetFilename(&nsp, i);
        size_t len = filename ? strlen(filename) : 0;

        if (len > 9 && strcasecmp(filename + len - 9, ".cnmt.nca") == 0) {
            LOG_INFO("NCA Install: Found CNMT NCA: %s", filename);

            for (int j = 0; j < 16 && filename[j * 2] != '\0'; j++) {
                char hex_byte[3] = {filename[j * 2], filename[j * 2 + 1], '\0'};
                cnmt_content_id.c[j] = (u8)strtoul(hex_byte, NULL, 16);
            }

            NcmPlaceHolderId placeholder_id;
            ncmContentStorageGeneratePlaceHolderId(&ctx->content_storage, &placeholder_id);

            u64 nca_size = nspGetFileSize(&nsp, i);

            // Remove existing content if already registered (e.g. from a previous attempt)
            bool already_exists = false;
            ncmContentStorageHas(&ctx->content_storage, &already_exists, &cnmt_content_id);
            if (already_exists) {
                ncmContentStorageDelete(&ctx->content_storage, &cnmt_content_id);
                LOG_INFO("NCA Install: Removed existing CNMT NCA before reinstall");
            }

            Result rc = ncmContentStorageCreatePlaceHolder(&ctx->content_storage,
                                                           &cnmt_content_id,
                                                           &placeholder_id, nca_size);
            if (R_FAILED(rc)) {
                LOG_ERROR("NCA Install: Failed to create CNMT placeholder: 0x%08X", rc);
                continue;
            }

            u8* buffer = (u8*)malloc(1024 * 1024);
            u64 offset = 0;
            bool write_ok = true;
            while (offset < nca_size) {
                u64 to_read = (nca_size - offset > 1024 * 1024) ?
                             1024 * 1024 : (nca_size - offset);

                s64 read = nspReadFile(&nsp, i, offset, buffer, to_read);
                if (read <= 0) {
                    LOG_ERROR("NCA Install: CNMT read failed at offset 0x%lX (got %ld)", offset, (long)read);
                    write_ok = false;
                    break;
                }

                Result wrc = ncmContentStorageWritePlaceHolder(&ctx->content_storage, &placeholder_id,
                                                  offset, buffer, read);
                if (R_FAILED(wrc)) {
                    LOG_ERROR("NCA Install: CNMT placeholder write failed: 0x%08X at offset 0x%lX", wrc, offset);
                    write_ok = false;
                    break;
                }
                offset += read;
            }
            free(buffer);

            if (!write_ok || offset != nca_size) {
                LOG_ERROR("NCA Install: CNMT NCA incomplete: wrote %lu of %lu bytes", (unsigned long)offset, (unsigned long)nca_size);
                ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
                continue;
            }

            rc = ncmContentStorageRegister(&ctx->content_storage, &cnmt_content_id, &placeholder_id);
            if (R_FAILED(rc)) {
                LOG_ERROR("NCA Install: Failed to register CNMT NCA: 0x%08X", rc);
                ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
                continue;
            }
            ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);

            LOG_INFO("NCA Install: CNMT NCA registered successfully (%lu bytes)", (unsigned long)nca_size);

            // Verify it's actually registered
            bool has_content = false;
            ncmContentStorageHas(&ctx->content_storage, &has_content, &cnmt_content_id);
            LOG_INFO("NCA Install: CNMT NCA present in storage: %s", has_content ? "yes" : "no");

            rc = readCnmtFromNca(ctx, &cnmt_content_id, &cnmt_ctx);
            if (R_SUCCEEDED(rc)) {
                if (cnmt_ctx.header.title_id == 0) {
                    LOG_ERROR("NCA Install: CNMT has invalid title ID (0)");
                    cnmtFree(&cnmt_ctx);
                    continue;
                }
                LOG_INFO("NCA Install: CNMT parsed - Title ID: 0x%016lX", cnmt_ctx.header.title_id);
                found_cnmt = true;
                break;
            } else {
                LOG_WARN("NCA Install: Failed to read CNMT from NCA: 0x%08X", rc);
            }
        }
    }

    if (!found_cnmt) {
        LOG_ERROR("NCA Install: No valid CNMT found");
        nspClose(&nsp);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    // Compute total size for progress reporting
    u64 total_install_size = 0;
    for (u32 i = 0; i < cnmt_ctx.content_count; i++) {
        NcmContentId* cid = &cnmt_ctx.content_records[i].content_id;
        char fn[64];
        snprintf(fn, sizeof(fn),
                "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.nca",
                cid->c[0], cid->c[1], cid->c[2], cid->c[3],
                cid->c[4], cid->c[5], cid->c[6], cid->c[7],
                cid->c[8], cid->c[9], cid->c[10], cid->c[11],
                cid->c[12], cid->c[13], cid->c[14], cid->c[15]);
        s32 idx = nspFindFile(&nsp, fn);
        if (idx >= 0) total_install_size += nspGetFileSize(&nsp, idx);
    }
    u64 total_bytes_written = 0;

    LOG_INFO("NCA Install: Step 3/4 - Installing %u content NCAs (%lu MB)",
             cnmt_ctx.content_count, (unsigned long)(total_install_size / (1024 * 1024)));
    for (u32 i = 0; i < cnmt_ctx.content_count; i++) {
        NcmContentId* content_id = &cnmt_ctx.content_records[i].content_id;

        char nca_filename[64];
        snprintf(nca_filename, sizeof(nca_filename),
                "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.nca",
                content_id->c[0], content_id->c[1], content_id->c[2], content_id->c[3],
                content_id->c[4], content_id->c[5], content_id->c[6], content_id->c[7],
                content_id->c[8], content_id->c[9], content_id->c[10], content_id->c[11],
                content_id->c[12], content_id->c[13], content_id->c[14], content_id->c[15]);

        s32 nca_idx = nspFindFile(&nsp, nca_filename);
        if (nca_idx < 0) {
            LOG_WARN("NCA Install: NCA not found in NSP: %s", nca_filename);
            continue;
        }

        LOG_INFO("NCA Install: Installing NCA: %s", nca_filename);

        // Remove existing content if already registered
        bool already_exists = false;
        ncmContentStorageHas(&ctx->content_storage, &already_exists, content_id);
        if (already_exists) {
            ncmContentStorageDelete(&ctx->content_storage, content_id);
            LOG_INFO("NCA Install: Removed existing NCA before reinstall");
        }

        NcmPlaceHolderId placeholder_id;
        ncmContentStorageGeneratePlaceHolderId(&ctx->content_storage, &placeholder_id);

        u64 nca_size = nspGetFileSize(&nsp, nca_idx);
        Result prc = ncmContentStorageCreatePlaceHolder(&ctx->content_storage, content_id,
                                          &placeholder_id, nca_size);
        if (R_FAILED(prc)) {
            LOG_ERROR("NCA Install: Failed to create placeholder: 0x%08X", prc);
            continue;
        }

        u8* buffer = (u8*)malloc(1024 * 1024);
        u64 offset = 0;
        bool write_ok = true;
        while (offset < nca_size) {
            u64 to_read = (nca_size - offset > 1024 * 1024) ?
                         1024 * 1024 : (nca_size - offset);

            s64 read = nspReadFile(&nsp, nca_idx, offset, buffer, to_read);
            if (read <= 0) {
                LOG_ERROR("NCA Install: Read failed at offset 0x%lX", offset);
                write_ok = false;
                break;
            }

            Result wrc = ncmContentStorageWritePlaceHolder(&ctx->content_storage, &placeholder_id,
                                              offset, buffer, read);
            if (R_FAILED(wrc)) {
                LOG_ERROR("NCA Install: Write failed: 0x%08X at offset 0x%lX", wrc, offset);
                write_ok = false;
                break;
            }
            offset += read;
            total_bytes_written += read;

            if (ctx->progress_cb) {
                ctx->progress_cb(total_bytes_written, total_install_size, ctx->progress_user_data);
            }
        }
        free(buffer);

        if (!write_ok || offset != nca_size) {
            LOG_ERROR("NCA Install: NCA incomplete: %lu of %lu bytes", (unsigned long)offset, (unsigned long)nca_size);
            ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
            continue;
        }

        prc = ncmContentStorageRegister(&ctx->content_storage, content_id, &placeholder_id);
        if (R_FAILED(prc)) {
            LOG_ERROR("NCA Install: Failed to register NCA: 0x%08X", prc);
        }
        ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
    }

    LOG_INFO("NCA Install: Step 4/4 - Registering with system");

    u64 cnmt_nca_size = 0;
    for (u32 i = 0; i < file_count; i++) {
        const char* filename = nspGetFilename(&nsp, i);
        size_t len = filename ? strlen(filename) : 0;
        if (len > 9 && strcasecmp(filename + len - 9, ".cnmt.nca") == 0) {
            cnmt_nca_size = nspGetFileSize(&nsp, i);
            break;
        }
    }

    NcmContentInfo cnmt_info;
    cnmt_info.content_id = cnmt_content_id;
    ncmU64ToContentInfoSize(cnmt_nca_size & 0xFFFFFFFFFFFFULL, &cnmt_info);
    cnmt_info.content_type = NcmContentType_Meta;

    u8* install_meta_buffer;
    size_t install_meta_size;
    Result rc = cnmtBuildInstallContentMeta(&cnmt_ctx, &cnmt_info, false,
                                           &install_meta_buffer, &install_meta_size);
    if (R_FAILED(rc)) {
        cnmtFree(&cnmt_ctx);
        nspClose(&nsp);
        return rc;
    }

    NcmContentMetaKey meta_key = cnmtGetContentMetaKey(&cnmt_ctx);
    rc = ncmContentMetaDatabaseSet(&ctx->meta_db, &meta_key,
                                   (NcmContentMetaHeader*)install_meta_buffer,
                                   install_meta_size);
    if (R_SUCCEEDED(rc)) {
        ncmContentMetaDatabaseCommit(&ctx->meta_db);
        LOG_DEBUG("NCA Install: Content metadata registered");
    } else {
        LOG_ERROR("NCA Install: Failed to register content metadata: 0x%08X", rc);
        free(install_meta_buffer);
        cnmtFree(&cnmt_ctx);
        nspClose(&nsp);
        return rc;
    }

    free(install_meta_buffer);

    u64 title_id = cnmt_ctx.header.title_id;

    u64 base_title_id;
    NcmContentMetaType content_type = (NcmContentMetaType)cnmt_ctx.header.type;

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

    LOG_INFO("NCA Install: Title ID: 0x%016lX (Base: 0x%016lX)", title_id, base_title_id);
    LOG_INFO("NCA Install: Content Type: %u, Version: %u",
             cnmt_ctx.header.type, cnmt_ctx.header.version);

    struct {
        NcmContentMetaKey meta_record;
        NcmStorageId storage_id;
    } storage_record;

    storage_record.meta_record = meta_key;
    storage_record.storage_id = ctx->storage_id;

    nsInitialize();

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
            LOG_INFO("NCA Install: Application record pushed to NS successfully!");
        } else {
            LOG_WARN("NCA Install: Failed to push application record: 0x%08X", rc);
            LOG_INFO("NCA Install: Content is installed, reboot may be required");
        }
    } else {
        LOG_WARN("NCA Install: Failed to get NS ApplicationManagerInterface");
        LOG_INFO("NCA Install: Content is installed, reboot may be required");
    }

    nsExit();

    if (out_title_id) {
        *out_title_id = title_id;
    }

    LOG_INFO("NCA Install: NSP installed! TitleID=0x%016lX Type=%u",
             title_id, cnmt_ctx.header.type);

    cnmtFree(&cnmt_ctx);
    nspClose(&nsp);

    return 0;
}

static Result installNcaFromXci(NcaInstallContext* ctx, XciContext* xci, u32 file_idx,
                                 const NcmContentId* content_id) {
    u64 nca_size = xciGetFileSize(xci, file_idx);

    NcmPlaceHolderId placeholder_id;
    Result rc = ncmContentStorageGeneratePlaceHolderId(&ctx->content_storage, &placeholder_id);
    if (R_FAILED(rc)) return rc;

    ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
    rc = ncmContentStorageCreatePlaceHolder(&ctx->content_storage, content_id,
                                            &placeholder_id, nca_size);
    if (R_FAILED(rc)) return rc;

    u8* buffer = (u8*)malloc(1024 * 1024);
    if (!buffer) {
        ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    u64 offset = 0;
    bool success = true;
    while (offset < nca_size) {
        u64 to_read = (nca_size - offset > 1024 * 1024) ? 1024 * 1024 : (nca_size - offset);

        s64 read = xciReadFile(xci, file_idx, offset, buffer, to_read);
        if (read <= 0) {
            success = false;
            break;
        }

        rc = ncmContentStorageWritePlaceHolder(&ctx->content_storage, &placeholder_id,
                                               offset, buffer, read);
        if (R_FAILED(rc)) {
            success = false;
            break;
        }
        offset += read;
    }
    free(buffer);

    if (!success) {
        ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    rc = ncmContentStorageRegister(&ctx->content_storage, content_id, &placeholder_id);
    ncmContentStorageDeletePlaceHolder(&ctx->content_storage, &placeholder_id);
    return rc;
}

Result ncaInstallXci(NcaInstallContext* ctx, const char* xci_path, u64* out_title_id) {
    if (!ctx || !xci_path) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    LOG_INFO("NCA Install: Installing XCI: %s", xci_path);

    XciContext xci;
    if (!xciOpen(&xci, xci_path)) {
        LOG_ERROR("NCA Install: Failed to open XCI file");
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    u32 file_count = xciGetFileCount(&xci);
    LOG_INFO("NCA Install: XCI secure partition contains %u files", file_count);

    Result rc = 0;

    LOG_INFO("NCA Install: Step 1/3 - Installing CNMT NCA");
    CnmtContext cnmt_ctx;
    NcmContentId cnmt_content_id = {0};
    bool found_cnmt = false;
    u64 cnmt_nca_size = 0;

    for (u32 i = 0; i < file_count; i++) {
        const char* filename = xciGetFilename(&xci, i);
        if (!filename) continue;
        size_t len = strlen(filename);

        if (len > 9 && strcasecmp(filename + len - 9, ".cnmt.nca") == 0) {
            LOG_INFO("NCA Install: Found CNMT NCA: %s", filename);

            for (int j = 0; j < 16 && filename[j * 2] != '\0'; j++) {
                char hex_byte[3] = {filename[j * 2], filename[j * 2 + 1], '\0'};
                cnmt_content_id.c[j] = (u8)strtoul(hex_byte, NULL, 16);
            }

            cnmt_nca_size = xciGetFileSize(&xci, i);

            rc = installNcaFromXci(ctx, &xci, i, &cnmt_content_id);
            if (R_FAILED(rc)) {
                LOG_ERROR("NCA Install: Failed to install CNMT NCA: 0x%08X", rc);
                continue;
            }

            rc = readCnmtFromNca(ctx, &cnmt_content_id, &cnmt_ctx);
            if (R_SUCCEEDED(rc)) {
                if (cnmt_ctx.header.title_id == 0) {
                    LOG_ERROR("NCA Install: CNMT has invalid title ID (0)");
                    cnmtFree(&cnmt_ctx);
                    continue;
                }
                LOG_INFO("NCA Install: CNMT parsed - Title ID: 0x%016lX", cnmt_ctx.header.title_id);
                found_cnmt = true;
                break;
            } else {
                LOG_WARN("NCA Install: Failed to read CNMT from NCA: 0x%08X", rc);
            }
        }
    }

    if (!found_cnmt) {
        LOG_ERROR("NCA Install: No valid CNMT found in XCI");
        xciClose(&xci);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    LOG_INFO("NCA Install: Step 2/3 - Installing %u content NCAs", cnmt_ctx.content_count);
    for (u32 i = 0; i < cnmt_ctx.content_count; i++) {
        NcmContentId* content_id = &cnmt_ctx.content_records[i].content_id;

        char nca_filename[64];
        snprintf(nca_filename, sizeof(nca_filename),
                "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.nca",
                content_id->c[0], content_id->c[1], content_id->c[2], content_id->c[3],
                content_id->c[4], content_id->c[5], content_id->c[6], content_id->c[7],
                content_id->c[8], content_id->c[9], content_id->c[10], content_id->c[11],
                content_id->c[12], content_id->c[13], content_id->c[14], content_id->c[15]);

        s32 nca_idx = xciFindFile(&xci, nca_filename);
        if (nca_idx < 0) {
            LOG_WARN("NCA Install: NCA not found in XCI: %s", nca_filename);
            continue;
        }

        LOG_DEBUG("NCA Install: Installing NCA: %s", nca_filename);

        rc = installNcaFromXci(ctx, &xci, nca_idx, content_id);
        if (R_FAILED(rc)) {
            LOG_ERROR("NCA Install: Failed to install NCA: %s (0x%08X)", nca_filename, rc);
        }
    }

    LOG_INFO("NCA Install: Step 3/3 - Registering with system");

    NcmContentInfo cnmt_info;
    cnmt_info.content_id = cnmt_content_id;
    ncmU64ToContentInfoSize(cnmt_nca_size & 0xFFFFFFFFFFFFULL, &cnmt_info);
    cnmt_info.content_type = NcmContentType_Meta;

    u8* install_meta_buffer;
    size_t install_meta_size;
    rc = cnmtBuildInstallContentMeta(&cnmt_ctx, &cnmt_info, false,
                                     &install_meta_buffer, &install_meta_size);
    if (R_FAILED(rc)) {
        LOG_ERROR("NCA Install: Failed to build install content meta: 0x%08X", rc);
        cnmtFree(&cnmt_ctx);
        xciClose(&xci);
        return rc;
    }

    NcmContentMetaKey meta_key = cnmtGetContentMetaKey(&cnmt_ctx);
    rc = ncmContentMetaDatabaseSet(&ctx->meta_db, &meta_key,
                                   (NcmContentMetaHeader*)install_meta_buffer,
                                   install_meta_size);
    if (R_SUCCEEDED(rc)) {
        ncmContentMetaDatabaseCommit(&ctx->meta_db);
        LOG_DEBUG("NCA Install: Content metadata registered");
    } else {
        LOG_ERROR("NCA Install: Failed to register content metadata: 0x%08X", rc);
        free(install_meta_buffer);
        cnmtFree(&cnmt_ctx);
        xciClose(&xci);
        return rc;
    }

    free(install_meta_buffer);

    u64 title_id = cnmt_ctx.header.title_id;

    u64 base_title_id;
    NcmContentMetaType content_type = (NcmContentMetaType)cnmt_ctx.header.type;

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

    LOG_INFO("NCA Install: Title ID: 0x%016lX (Base: 0x%016lX)", title_id, base_title_id);
    LOG_INFO("NCA Install: Content Type: %u, Version: %u",
             cnmt_ctx.header.type, cnmt_ctx.header.version);

    struct {
        NcmContentMetaKey meta_record;
        NcmStorageId storage_id;
    } storage_record;

    storage_record.meta_record = meta_key;
    storage_record.storage_id = ctx->storage_id;

    nsInitialize();

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
            LOG_INFO("NCA Install: Application record pushed to NS successfully!");
        } else {
            LOG_WARN("NCA Install: Failed to push application record: 0x%08X", rc);
            LOG_INFO("NCA Install: Content is installed, reboot may be required");
        }
    } else {
        LOG_WARN("NCA Install: Failed to get NS ApplicationManagerInterface");
        LOG_INFO("NCA Install: Content is installed, reboot may be required");
    }

    nsExit();

    if (out_title_id) {
        *out_title_id = title_id;
    }

    LOG_INFO("NCA Install: XCI installed! TitleID=0x%016lX Type=%u",
             title_id, cnmt_ctx.header.type);

    cnmtFree(&cnmt_ctx);
    xciClose(&xci);

    return 0;
}

