// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNMT_MAX_CONTENT_RECORDS 32

typedef struct {
    u8 hash[0x20];
    NcmContentInfo content_info;
} __attribute__((packed)) CnmtPackagedContentInfo;

typedef struct {
    u64 title_id;
    u32 version;
    u8 type;                      // NcmContentMetaType
    u8 _0xd;
    u16 extended_header_size;
    u16 content_count;
    u16 content_meta_count;
    u8 attributes;
    u8 storage_id;
    u8 install_type;
    u8 committed;
    u32 required_system_version;
    u32 _0x1c;
} __attribute__((packed)) CnmtPackagedHeader;

typedef struct {
    CnmtPackagedHeader header;
    NcmContentInfo content_records[CNMT_MAX_CONTENT_RECORDS];
    u32 content_count;
    u8* extended_header_data;
    u32 extended_header_size;
    u8* extended_data;
    u32 extended_data_size;
} CnmtContext;

bool cnmtParse(CnmtContext* ctx, const u8* data, size_t size);
void cnmtFree(CnmtContext* ctx);
NcmContentMetaKey cnmtGetContentMetaKey(const CnmtContext* ctx);
Result cnmtBuildInstallContentMeta(const CnmtContext* ctx,
                                   const NcmContentInfo* cnmt_content_info,
                                   bool ignore_req_firmware,
                                   u8** out_buffer,
                                   size_t* out_size);
void cnmtGetDisplayVersion(const CnmtContext* ctx, char* out_version, size_t out_size);
void cnmtGetDlcDisplayName(const CnmtContext* ctx, const char* base_game_name,
                           char* out_name, size_t out_size);
bool cnmtReadFromInstalledNca(const NcmContentId* cnmt_id, NcmStorageId storage_id,
                               CnmtContext* out_ctx);
bool nacpGetDisplayVersionFromControlNca(NcmContentMetaDatabase* meta_db,
                                          const NcmContentMetaKey* meta_key,
                                          NcmStorageId storage_id,
                                          char* out_version, size_t out_size);
bool nacpGetDlcName(NcmContentMetaDatabase* meta_db,
                    const NcmContentMetaKey* meta_key,
                    NcmStorageId storage_id,
                    char* out_name, size_t out_size);

#ifdef __cplusplus
}
#endif
