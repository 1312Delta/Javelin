// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/cnmt.h"
#include "mtp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

void cnmtGetDisplayVersion(const CnmtContext* ctx, char* out_version, size_t out_size) {
    if (!ctx || !out_version || out_size == 0) {
        if (out_version && out_size > 0) out_version[0] = '\0';
        return;
    }

    u32 ver = ctx->header.version;

    u16 major = (ver >> 16) & 0xFFFF;
    u8 minor = (ver >> 8) & 0xFF;
    u8 micro = ver & 0xFF;

    if (ver == 0) {
        out_version[0] = '\0';
        return;
    }

    if (micro > 0) {
        snprintf(out_version, out_size, "%u.%u.%u", major, minor, micro);
    } else if (minor > 0) {
        snprintf(out_version, out_size, "%u.%u", major, minor);
    } else {
        snprintf(out_version, out_size, "%u.0", major);
    }
}

void cnmtGetDlcDisplayName(const CnmtContext* ctx, const char* base_game_name,
                           char* out_name, size_t out_size) {
    if (!ctx || !out_name || out_size == 0) {
        if (out_name && out_size > 0) out_name[0] = '\0';
        return;
    }

    u32 dlc_number = ctx->header.title_id & 0xFFF;

    if (dlc_number != 0) {
        snprintf(out_name, out_size, "DLC %u", dlc_number);
    } else {
        snprintf(out_name, out_size, "DLC");
    }
}

bool cnmtReadFromInstalledNca(const NcmContentId* cnmt_id, NcmStorageId storage_id,
                               CnmtContext* out_ctx) {
    if (!cnmt_id || !out_ctx) return false;

    NcmContentStorage storage;
    Result rc = ncmOpenContentStorage(&storage, storage_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("CNMT: Failed to open content storage: 0x%08X", rc);
        return false;
    }

    char nca_path[FS_MAX_PATH];
    rc = ncmContentStorageGetPath(&storage, nca_path, sizeof(nca_path), cnmt_id);
    ncmContentStorageClose(&storage);
    if (R_FAILED(rc)) {
        LOG_ERROR("CNMT: Failed to get NCA path: 0x%08X", rc);
        return false;
    }

    LOG_DEBUG("CNMT: Reading from NCA: %s", nca_path);

    char fs_path[FS_MAX_PATH];
    const char* rest = NULL;

    if (strncmp(nca_path, "@SdCardContent://", 15) == 0) {
        rest = nca_path + 15;
        while (rest[0] == '/') rest++;
        snprintf(fs_path, sizeof(fs_path), "sdmc:/Nintendo/Contents/%s", rest);
    } else if (strncmp(nca_path, "@UserContent://", 15) == 0) {
        rest = nca_path + 15;
        while (rest[0] == '/') rest++;
        snprintf(fs_path, sizeof(fs_path), "bis_user:/Nintendo/Contents/%s", rest);
    } else {
        LOG_ERROR("CNMT: Unknown NCA path format: %s", nca_path);
        return false;
    }

    FsFileSystem cnmt_fs;
    FsContentAttributes attrs = {};
    rc = fsOpenFileSystemWithId(&cnmt_fs, 0, FsFileSystemType_ContentMeta, nca_path, attrs);

    if (R_SUCCEEDED(rc)) {
        FsDir dir;
        rc = fsFsOpenDirectory(&cnmt_fs, "/", FsDirOpenMode_ReadFiles, &dir);
        if (R_SUCCEEDED(rc)) {
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

            if (cnmt_file_path[0] != '\0') {
                FsFile cnmt_file;
                rc = fsFsOpenFile(&cnmt_fs, cnmt_file_path, FsOpenMode_Read, &cnmt_file);
                if (R_SUCCEEDED(rc)) {
                    s64 cnmt_size;
                    fsFileGetSize(&cnmt_file, &cnmt_size);

                    if (cnmt_size > 0 && cnmt_size <= 0x100000) {
                        u8* cnmt_data = (u8*)malloc(cnmt_size);
                        if (cnmt_data) {
                            u64 bytes_read;
                            rc = fsFileRead(&cnmt_file, 0, cnmt_data, cnmt_size, FsReadOption_None, &bytes_read);
                            fsFileClose(&cnmt_file);
                            fsFsClose(&cnmt_fs);

                            if (R_SUCCEEDED(rc) && bytes_read == (u64)cnmt_size) {
                                bool result = cnmtParse(out_ctx, cnmt_data, cnmt_size);
                                free(cnmt_data);
                                return result;
                            }
                            free(cnmt_data);
                        } else {
                            fsFileClose(&cnmt_file);
                        }
                    } else {
                        fsFileClose(&cnmt_file);
                    }
                }
            }
        }
        fsFsClose(&cnmt_fs);
    }

    FILE* fp = fopen(fs_path, "rb");
    if (fp) {
        u8 header[0x400];
        size_t read_len = fread(header, 1, sizeof(header), fp);
        fclose(fp);

        if (read_len >= 0x400) {
            u32* magic = (u32*)(header + 0x200);
            if (*magic == 0x32434E4E) {
                LOG_WARN("CNMT: Direct NCA read detected but CNMT parsing requires decryption");
            }
        }
    }

    LOG_ERROR("CNMT: Failed to read CNMT from NCA: %s", fs_path);
    return false;
}

bool cnmtParse(CnmtContext* ctx, const u8* data, size_t size) {
    if (!ctx || !data || size < sizeof(CnmtPackagedHeader)) {
        LOG_ERROR("CNMT: Invalid input parameters");
        return false;
    }

    memset(ctx, 0, sizeof(CnmtContext));

    memcpy(&ctx->header, data, sizeof(CnmtPackagedHeader));

    LOG_DEBUG("CNMT: Parsed header - TitleID: 0x%016lX, Type: %u, Version: 0x%08X, ContentCount: %u",
             ctx->header.title_id, ctx->header.type, ctx->header.version, ctx->header.content_count);

    if (ctx->header.content_count > CNMT_MAX_CONTENT_RECORDS) {
        LOG_ERROR("CNMT: Too many content records: %u (max %u)",
                  ctx->header.content_count, CNMT_MAX_CONTENT_RECORDS);
        return false;
    }

    size_t extended_header_offset = sizeof(CnmtPackagedHeader);
    size_t content_info_offset = extended_header_offset + ctx->header.extended_header_size;

    size_t required_size = content_info_offset +
                          (ctx->header.content_count * sizeof(CnmtPackagedContentInfo));
    if (size < required_size) {
        LOG_ERROR("CNMT: Buffer too small: %zu < %zu", size, required_size);
        return false;
    }

    if (ctx->header.extended_header_size > 0) {
        ctx->extended_header_data = (u8*)malloc(ctx->header.extended_header_size);
        if (!ctx->extended_header_data) {
            LOG_ERROR("CNMT: Failed to allocate extended header");
            return false;
        }
        memcpy(ctx->extended_header_data, data + extended_header_offset,
               ctx->header.extended_header_size);
        ctx->extended_header_size = ctx->header.extended_header_size;
    }

    const CnmtPackagedContentInfo* packaged_info =
        (const CnmtPackagedContentInfo*)(data + content_info_offset);

    ctx->content_count = 0;
    for (u32 i = 0; i < ctx->header.content_count; i++) {
        if (packaged_info[i].content_info.content_type <= 5) {
            memcpy(&ctx->content_records[ctx->content_count],
                   &packaged_info[i].content_info,
                   sizeof(NcmContentInfo));
            ctx->content_count++;
        }
    }

    LOG_DEBUG("CNMT: Parsed %u content records (skipped deltas)", ctx->content_count);

    if (ctx->header.type == NcmContentMetaType_Patch && ctx->extended_header_data) {
        NcmPatchMetaExtendedHeader* patch_header =
            (NcmPatchMetaExtendedHeader*)ctx->extended_header_data;

        if (patch_header->extended_data_size > 0) {
            size_t extended_data_offset = content_info_offset +
                (ctx->header.content_count * sizeof(CnmtPackagedContentInfo));

            if (size >= extended_data_offset + patch_header->extended_data_size) {
                ctx->extended_data = (u8*)malloc(patch_header->extended_data_size);
                if (ctx->extended_data) {
                    memcpy(ctx->extended_data, data + extended_data_offset,
                           patch_header->extended_data_size);
                    ctx->extended_data_size = patch_header->extended_data_size;
                }
            }
        }
    }

    return true;
}

void cnmtFree(CnmtContext* ctx) {
    if (!ctx) return;

    if (ctx->extended_header_data) {
        free(ctx->extended_header_data);
        ctx->extended_header_data = NULL;
    }

    if (ctx->extended_data) {
        free(ctx->extended_data);
        ctx->extended_data = NULL;
    }

    memset(ctx, 0, sizeof(CnmtContext));
}

NcmContentMetaKey cnmtGetContentMetaKey(const CnmtContext* ctx) {
    NcmContentMetaKey key;
    memset(&key, 0, sizeof(NcmContentMetaKey));

    if (!ctx) return key;

    key.id = ctx->header.title_id;
    key.version = ctx->header.version;
    key.type = (NcmContentMetaType)ctx->header.type;
    key.install_type = ctx->header.install_type;

    return key;
}

Result cnmtBuildInstallContentMeta(const CnmtContext* ctx,
                                   const NcmContentInfo* cnmt_content_info,
                                   bool ignore_req_firmware,
                                   u8** out_buffer,
                                   size_t* out_size) {
    if (!ctx || !cnmt_content_info || !out_buffer || !out_size) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    size_t total_size = sizeof(NcmContentMetaHeader) +
                       ctx->extended_header_size +
                       ((ctx->content_count + 1) * sizeof(NcmContentInfo)) +
                       ctx->extended_data_size;

    u8* buffer = (u8*)malloc(total_size);
    if (!buffer) {
        LOG_ERROR("CNMT: Failed to allocate install buffer (%zu bytes)", total_size);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    u8* ptr = buffer;

    NcmContentMetaHeader* meta_header = (NcmContentMetaHeader*)ptr;
    meta_header->extended_header_size = ctx->extended_header_size;
    meta_header->content_count = ctx->content_count + 1;
    meta_header->content_meta_count = ctx->header.content_meta_count;
    meta_header->attributes = ctx->header.attributes;
    meta_header->storage_id = 0;
    ptr += sizeof(NcmContentMetaHeader);

    if (ctx->extended_header_size > 0) {
        memcpy(ptr, ctx->extended_header_data, ctx->extended_header_size);

        if (ignore_req_firmware &&
            (ctx->header.type == NcmContentMetaType_Application ||
             ctx->header.type == NcmContentMetaType_Patch)) {
            *(u32*)(ptr + 8) = 0;
            LOG_DEBUG("CNMT: Ignoring required firmware version");
        }

        ptr += ctx->extended_header_size;
    }

    memcpy(ptr, cnmt_content_info, sizeof(NcmContentInfo));
    ptr += sizeof(NcmContentInfo);

    for (u32 i = 0; i < ctx->content_count; i++) {
        memcpy(ptr, &ctx->content_records[i], sizeof(NcmContentInfo));
        ptr += sizeof(NcmContentInfo);
    }

    if (ctx->extended_data_size > 0) {
        memcpy(ptr, ctx->extended_data, ctx->extended_data_size);
        ptr += ctx->extended_data_size;
    }

    LOG_DEBUG("CNMT: Built install content meta (%zu bytes)", total_size);

    *out_buffer = buffer;
    *out_size = total_size;

    return 0;
}

#define NACP_DISPLAY_VERSION_OFFSET 0x3060
#define NACP_DISPLAY_VERSION_SIZE   0x10

#define NACP_APPLICATION_TITLE_COUNT 16
typedef struct {
    char name[0x200];
    char publisher[0x100];
} NacpApplicationTitle;

bool nacpGetDisplayVersionFromControlNca(NcmContentMetaDatabase* meta_db,
                                          const NcmContentMetaKey* meta_key,
                                          NcmStorageId storage_id,
                                          char* out_version, size_t out_size) {
    if (!meta_db || !meta_key || !out_version || out_size == 0) {
        if (out_version && out_size > 0) out_version[0] = '\0';
        return false;
    }

    out_version[0] = '\0';

    NcmContentId control_id;
    Result rc = ncmContentMetaDatabaseGetContentIdByType(meta_db, &control_id,
                                                          meta_key, NcmContentType_Control);
    if (R_FAILED(rc)) {
        LOG_DEBUG("NACP: No Control NCA found for title 0x%016lX: 0x%08X", meta_key->id, rc);
        return false;
    }

    NcmContentStorage storage;
    rc = ncmOpenContentStorage(&storage, storage_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NACP: Failed to open content storage: 0x%08X", rc);
        return false;
    }

    char nca_path[FS_MAX_PATH];
    rc = ncmContentStorageGetPath(&storage, nca_path, sizeof(nca_path), &control_id);
    ncmContentStorageClose(&storage);
    if (R_FAILED(rc)) {
        LOG_ERROR("NACP: Failed to get Control NCA path: 0x%08X", rc);
        return false;
    }

    FsFileSystem control_fs;
    FsContentAttributes attrs = {};
    rc = fsOpenFileSystemWithId(&control_fs, meta_key->id, FsFileSystemType_ContentControl, nca_path, attrs);
    if (R_FAILED(rc)) {
        LOG_DEBUG("NACP: Failed to mount Control NCA: 0x%08X", rc);
        return false;
    }

    FsFile nacp_file;
    rc = fsFsOpenFile(&control_fs, "/control.nacp", FsOpenMode_Read, &nacp_file);
    if (R_FAILED(rc)) {
        fsFsClose(&control_fs);
        LOG_DEBUG("NACP: Failed to open control.nacp: 0x%08X", rc);
        return false;
    }

    char version_buf[NACP_DISPLAY_VERSION_SIZE + 1] = {0};
    u64 bytes_read;
    rc = fsFileRead(&nacp_file, NACP_DISPLAY_VERSION_OFFSET, version_buf,
                    NACP_DISPLAY_VERSION_SIZE, FsReadOption_None, &bytes_read);
    fsFileClose(&nacp_file);
    fsFsClose(&control_fs);

    if (R_FAILED(rc) || bytes_read != NACP_DISPLAY_VERSION_SIZE) {
        LOG_DEBUG("NACP: Failed to read display_version: 0x%08X", rc);
        return false;
    }

    version_buf[NACP_DISPLAY_VERSION_SIZE] = '\0';
    if (version_buf[0] != '\0') {
        strncpy(out_version, version_buf, out_size - 1);
        out_version[out_size - 1] = '\0';
        LOG_DEBUG("NACP: Got display version: %s", out_version);
        return true;
    }

    return false;
}

bool nacpGetDlcName(NcmContentMetaDatabase* meta_db,
                    const NcmContentMetaKey* meta_key,
                    NcmStorageId storage_id,
                    char* out_name, size_t out_size) {
    if (!meta_db || !meta_key || !out_name || out_size == 0) {
        if (out_name && out_size > 0) out_name[0] = '\0';
        return false;
    }

    out_name[0] = '\0';

    NcmContentId control_id;
    Result rc = ncmContentMetaDatabaseGetContentIdByType(meta_db, &control_id,
                                                          meta_key, NcmContentType_Control);
    if (R_FAILED(rc)) {
        LOG_DEBUG("NACP: No Control NCA found for DLC 0x%016lX: 0x%08X", meta_key->id, rc);
        return false;
    }

    NcmContentStorage storage;
    rc = ncmOpenContentStorage(&storage, storage_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("NACP: Failed to open content storage: 0x%08X", rc);
        return false;
    }

    char nca_path[FS_MAX_PATH];
    rc = ncmContentStorageGetPath(&storage, nca_path, sizeof(nca_path), &control_id);
    ncmContentStorageClose(&storage);
    if (R_FAILED(rc)) {
        LOG_ERROR("NACP: Failed to get DLC Control NCA path: 0x%08X", rc);
        return false;
    }

    FsFileSystem control_fs;
    FsContentAttributes attrs = {};
    rc = fsOpenFileSystemWithId(&control_fs, meta_key->id, FsFileSystemType_ContentControl, nca_path, attrs);
    if (R_FAILED(rc)) {
        LOG_DEBUG("NACP: Failed to mount DLC Control NCA: 0x%08X", rc);
        return false;
    }

    FsFile nacp_file;
    rc = fsFsOpenFile(&control_fs, "/control.nacp", FsOpenMode_Read, &nacp_file);
    if (R_FAILED(rc)) {
        fsFsClose(&control_fs);
        LOG_DEBUG("NACP: Failed to open DLC control.nacp: 0x%08X", rc);
        return false;
    }

    bool found = false;
    for (int lang = 0; lang < NACP_APPLICATION_TITLE_COUNT && !found; lang++) {
        NacpApplicationTitle title;
        memset(&title, 0, sizeof(title));

        u64 bytes_read;
        size_t offset = lang * sizeof(NacpApplicationTitle);
        rc = fsFileRead(&nacp_file, offset, &title, sizeof(title), FsReadOption_None, &bytes_read);

        if (R_SUCCEEDED(rc) && bytes_read == sizeof(title) && title.name[0] != '\0') {
            strncpy(out_name, title.name, out_size - 1);
            out_name[out_size - 1] = '\0';
            found = true;
            LOG_DEBUG("NACP: Got DLC name (lang %d): %s", lang, out_name);
        }
    }

    fsFileClose(&nacp_file);
    fsFsClose(&control_fs);

    return found;
}
