// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "dump/game_dump.h"
#include "mtp/mtp_log.h"
#include "core/Debug.h"

extern "C" {
#include "ipcext/es.h"
#include "service/es.h"
}

#include "install/cnmt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

typedef struct
{
    char magic[4];
    u32 file_count;
    u32 string_table_size;
    u32 reserved;
} __attribute__((packed)) Pfs0Header;

typedef struct
{
    u64 offset;
    u64 size;
    u32 string_offset;
    u32 reserved;
} __attribute__((packed)) Pfs0FileEntry;

static void build_nca_filename(char* out, size_t out_size, const NcmContentId* id)
{
    snprintf(out, out_size,
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.nca",
             id->c[0], id->c[1], id->c[2], id->c[3],
             id->c[4], id->c[5], id->c[6], id->c[7],
             id->c[8], id->c[9], id->c[10], id->c[11],
             id->c[12], id->c[13], id->c[14], id->c[15]);
}

static void build_cnmt_nca_filename(char* out, size_t out_size, const NcmContentId* id)
{
    snprintf(out, out_size,
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.cnmt.nca",
             id->c[0], id->c[1], id->c[2], id->c[3],
             id->c[4], id->c[5], id->c[6], id->c[7],
             id->c[8], id->c[9], id->c[10], id->c[11],
             id->c[12], id->c[13], id->c[14], id->c[15]);
}

static u64 get_nca_size(const NcmContentInfo* info)
{
    u64 size = 0;
    ncmContentInfoSizeToU64(info, &size);
    return size;
}

static s64 read_nca_data(DumpContext* ctx, const DumpNspFileEntry* entry, u64 offset, void* buffer, u64 size)
{
    NcmContentStorage* storage = NULL;
    if (entry->storage_id == NcmStorageId_SdCard)
    {
        if (!ctx->sd_storage_open)
        {
            LOG_ERROR("[Dump] SD storage not open");
            return -1;
        }
        storage = &ctx->sd_storage;
    }
    else
    {
        if (!ctx->nand_storage_open)
        {
            LOG_ERROR("[Dump] NAND storage not open");
            return -1;
        }
        storage = &ctx->nand_storage;
    }

    u64 bytes_read_total = 0;
    u64 current_offset = offset;
    u8* out_buffer = (u8*)buffer;
    u64 remaining = size;

    while (remaining > 0)
    {
        size_t chunk_size = remaining;
        if (chunk_size > 0x400000) chunk_size = 0x400000;

        Result rc = ncmContentStorageReadContentIdFile(storage, out_buffer + bytes_read_total,
                                                       chunk_size, &entry->content_id,
                                                       (s64)current_offset);
        if (R_FAILED(rc))
        {
            LOG_ERROR("[Dump] Failed to read NCA content: 0x%08X (offset: 0x%lX, size: 0x%lX)",
                      rc, current_offset, chunk_size);
            return -1;
        }

        bytes_read_total += chunk_size;
        current_offset += chunk_size;
        remaining -= chunk_size;
    }

    return (s64)bytes_read_total;
}

static bool ensure_es_service(DumpContext* ctx)
{
    if (ctx->es_initialized) return true;

    Result rc = esInitialize();
    if (R_SUCCEEDED(rc))
    {
        ctx->es_initialized = true;
        LOG_INFO("[Dump] ES service initialized");
        return true;
    }
    LOG_ERROR("[Dump] Failed to initialize ES service: 0x%08X", rc);
    return false;
}

static void extract_ticket_cert(DumpContext* ctx, DumpNspLayout* layout, const u8* rights_id)
{
    bool all_zero = true;
    for (int i = 0; i < 16; i++)
    {
        if (rights_id[i] != 0)
        {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return;

    if (layout->ticket_data && layout->ticket_size > 0) return;

    if (!ensure_es_service(ctx)) return;

    // Build the EsRightsId to search for
    EsRightsId target_rights_id;
    memcpy(target_rights_id.fs_id.c, rights_id, 16);

    // Try common tickets first
    u32 common_count = esCountCommonTicket();
    if (common_count > 0)
    {
        EsRightsId* rights_ids = (EsRightsId*)malloc(sizeof(EsRightsId) * common_count);
        if (rights_ids)
        {
            u32 written = 0;
            Result rc = esListCommonTicket(&written, rights_ids, sizeof(EsRightsId) * common_count);
            if (R_SUCCEEDED(rc))
            {
                for (u32 i = 0; i < written; i++)
                {
                    if (memcmp(rights_ids[i].fs_id.c, rights_id, 16) == 0)
                    {
                        LOG_INFO("[Dump] Found matching common ticket for rights ID");

                        // Extract the ticket data
                        u8* tik_buf = (u8*)malloc(0x400);
                        if (tik_buf)
                        {
                            memset(tik_buf, 0, 0x400);
                            u64 out_size = 0;
                            rc = esGetCommonTicketData(&out_size, &rights_ids[i], tik_buf, 0x400);
                            if (R_SUCCEEDED(rc) && out_size > 0)
                            {
                                layout->ticket_data = tik_buf;
                                layout->ticket_size = (u32)out_size;
                                LOG_INFO("[Dump] Extracted common ticket: %u bytes", (u32)out_size);
                            }
                            else
                            {
                                LOG_ERROR("[Dump] Failed to get common ticket data: 0x%08X", rc);
                                free(tik_buf);
                            }
                        }
                        break;
                    }
                }
            }
            free(rights_ids);
        }
    }

    // Try personalized tickets if common not found
    if (!layout->ticket_data)
    {
        u32 personalized_count = esCountPersonalizedTicket();
        if (personalized_count > 0)
        {
            EsRightsId* rights_ids = (EsRightsId*)malloc(sizeof(EsRightsId) * personalized_count);
            if (rights_ids)
            {
                u32 written = 0;
                Result rc = esListPersonalizedTicket(&written, rights_ids, sizeof(EsRightsId) * personalized_count);
                if (R_SUCCEEDED(rc))
                {
                    for (u32 i = 0; i < written; i++)
                    {
                        if (memcmp(rights_ids[i].fs_id.c, rights_id, 16) == 0)
                        {
                            LOG_INFO("[Dump] Found matching personalized ticket - extracting via common path");
                            // Personalized tickets can sometimes be read via the common API
                            u8* tik_buf = (u8*)malloc(0x400);
                            if (tik_buf)
                            {
                                memset(tik_buf, 0, 0x400);
                                u64 out_size = 0;
                                rc = esGetCommonTicketData(&out_size, &rights_ids[i], tik_buf, 0x400);
                                if (R_SUCCEEDED(rc) && out_size > 0)
                                {
                                    layout->ticket_data = tik_buf;
                                    layout->ticket_size = (u32)out_size;
                                    LOG_INFO("[Dump] Extracted personalized ticket: %u bytes", (u32)out_size);
                                }
                                else
                                {
                                    LOG_WARN("[Dump] Could not extract personalized ticket: 0x%08X", rc);
                                    free(tik_buf);
                                }
                            }
                            break;
                        }
                    }
                }
                free(rights_ids);
            }
        }
    }

    if (!layout->ticket_data)
    {
        LOG_DEBUG("[Dump] No ticket found for this title (may not require one)");
    }
}

static void generate_cert_chain(DumpContext* ctx, DumpNspLayout* layout)
{
    if (layout->cert_data && layout->cert_size > 0) return;
    (void)ctx;
    (void)layout;
}

static void try_extract_rights_id(DumpContext* ctx, DumpNspLayout* layout)
{
    for (u32 i = 0; i < layout->file_count; i++)
    {
        DumpNspFileEntry* entry = &layout->files[i];
        if (entry->in_memory) continue;
        if (entry->size < 0x400) continue;

        u8 header[0x400];
        s64 rd = read_nca_data(ctx, entry, 0, header, sizeof(header));
        if (rd < (s64)sizeof(header)) continue;

        // rights_id is at offset 0x230 in the NCA header
        u8* rights_id = header + 0x230;

        bool has_rights = false;
        for (int j = 0; j < 16; j++)
        {
            if (rights_id[j] != 0)
            {
                has_rights = true;
                break;
            }
        }

        if (has_rights)
        {
            extract_ticket_cert(ctx, layout, rights_id);
            if (layout->ticket_data) break;
        }
    }
}

static u32 align_up(u32 value, u32 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static void parse_version_string(u32 version_key, char* out_version, size_t out_size)
{
    u16 major = (version_key >> 16) & 0xFFFF;
    u8 minor = (version_key >> 8) & 0xFF;
    u8 micro = version_key & 0xFF;

    if (version_key == 0)
    {
        snprintf(out_version, out_size, "v1.0");
    }
    else if (micro > 0)
    {
        snprintf(out_version, out_size, "v%u.%u.%u", major, minor, micro);
    }
    else if (minor > 0)
    {
        snprintf(out_version, out_size, "v%u.%u", major, minor);
    }
    else
    {
        snprintf(out_version, out_size, "v%u.0", major);
    }
}

static void get_game_name_from_ns(u64 app_id, char* out_name, size_t out_size)
{
    NsApplicationControlData* ctrl = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (ctrl)
    {
        u64 actual = 0;
        Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, ctrl, sizeof(*ctrl),
                                                &actual);
        if (R_SUCCEEDED(rc) && actual >= sizeof(ctrl->nacp))
        {
            for (int k = 0; k < 16; k++)
            {
                if (ctrl->nacp.lang[k].name[0])
                {
                    strncpy(out_name, ctrl->nacp.lang[k].name, out_size - 1);
                    out_name[out_size - 1] = '\0';
                    free(ctrl);
                    return;
                }
            }
        }
        free(ctrl);
    }
    snprintf(out_name, out_size, "%016lX", app_id);
}

static bool get_display_version_from_ns(u64 app_id, char* out_version, size_t out_size)
{
    NsApplicationControlData* ctrl = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (!ctrl) return false;

    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, ctrl, sizeof(*ctrl), &actual);
    if (R_SUCCEEDED(rc) && actual >= sizeof(ctrl->nacp))
    {
        if (ctrl->nacp.display_version[0] != '\0')
        {
            strncpy(out_version, ctrl->nacp.display_version, out_size - 1);
            out_version[out_size - 1] = '\0';
            free(ctrl);
            return true;
        }
    }
    free(ctrl);
    return false;
}

static void get_display_version(NcmContentMetaDatabase* meta_db, const NcmContentMetaKey* meta_key,
                                NcmStorageId storage_id, char* out_version, size_t out_size)
{
    out_version[0] = '\0';

    if (meta_key->type == NcmContentMetaType_Patch)
    {
        u64 base_app_id = meta_key->id & ~0x800ULL;
        if (get_display_version_from_ns(base_app_id, out_version, out_size))
        {
            if (out_version[0] != '\0' && out_version[0] != 'v' && out_version[0] != 'V')
            {
                char temp[64];
                snprintf(temp, sizeof(temp), "v%s", out_version);
                strncpy(out_version, temp, out_size - 1);
                out_version[out_size - 1] = '\0';
            }
            return;
        }
    }

    if (nacpGetDisplayVersionFromControlNca(meta_db, meta_key, storage_id, out_version, out_size))
    {
        if (out_version[0] != '\0' && out_version[0] != 'v' && out_version[0] != 'V')
        {
            char temp[64];
            snprintf(temp, sizeof(temp), "v%s", out_version);
            strncpy(out_version, temp, out_size - 1);
            out_version[out_size - 1] = '\0';
        }
        return;
    }

    if (get_display_version_from_ns(meta_key->id, out_version, out_size))
    {
        if (out_version[0] != '\0' && out_version[0] != 'v' && out_version[0] != 'V')
        {
            char temp[64];
            snprintf(temp, sizeof(temp), "v%s", out_version);
            strncpy(out_version, temp, out_size - 1);
            out_version[out_size - 1] = '\0';
        }
        return;
    }

    NcmContentId cnmt_id;
    Result rc_cnmt = ncmContentMetaDatabaseGetContentIdByType(meta_db, &cnmt_id, meta_key, NcmContentType_Meta);
    if (R_SUCCEEDED(rc_cnmt))
    {
        CnmtContext cnmt_ctx;
        if (cnmtReadFromInstalledNca(&cnmt_id, storage_id, &cnmt_ctx))
        {
            cnmtGetDisplayVersion(&cnmt_ctx, out_version, out_size);
            cnmtFree(&cnmt_ctx);
            if (out_version[0] != '\0')
            {
                char temp[64];
                snprintf(temp, sizeof(temp), "v%s", out_version);
                strncpy(out_version, temp, out_size - 1);
                out_version[out_size - 1] = '\0';
                return;
            }
        }
    }

    if (out_version[0] == '\0')
    {
        parse_version_string(meta_key->version, out_version, out_size);
    }
}

static void open_ncm_handles(DumpContext* ctx)
{
    if (!ctx->ncm_initialized) return;

    if (!ctx->sd_storage_open)
    {
        Result rc = ncmOpenContentStorage(&ctx->sd_storage, NcmStorageId_SdCard);
        if (R_SUCCEEDED(rc))
        {
            ctx->sd_storage_open = true;
        }
    }

    if (!ctx->sd_meta_db_open)
    {
        Result rc = ncmOpenContentMetaDatabase(&ctx->sd_meta_db, NcmStorageId_SdCard);
        if (R_SUCCEEDED(rc))
        {
            ctx->sd_meta_db_open = true;
        }
    }

    if (!ctx->nand_storage_open)
    {
        Result rc = ncmOpenContentStorage(&ctx->nand_storage, NcmStorageId_BuiltInUser);
        if (R_SUCCEEDED(rc))
        {
            ctx->nand_storage_open = true;
        }
    }

    if (!ctx->nand_meta_db_open)
    {
        Result rc = ncmOpenContentMetaDatabase(&ctx->nand_meta_db, NcmStorageId_BuiltInUser);
        if (R_SUCCEEDED(rc))
        {
            ctx->nand_meta_db_open = true;
        }
    }
}

static void build_pfs0_header(DumpNspLayout* layout)
{
    if (layout->file_count == 0) return;

    u32 string_table_size = 0;
    for (u32 i = 0; i < layout->file_count; i++)
    {
        string_table_size += strlen(layout->files[i].filename) + 1;
    }
    u32 padded_string_table_size = align_up(string_table_size, 0x20);

    u32 header_size = sizeof(Pfs0Header) +
        (sizeof(Pfs0FileEntry) * layout->file_count) +
        padded_string_table_size;

    layout->pfs0_header = (u8*)calloc(1, header_size);
    if (!layout->pfs0_header) return;
    layout->pfs0_header_size = header_size;

    Pfs0Header* hdr = (Pfs0Header*)layout->pfs0_header;
    memcpy(hdr->magic, "PFS0", 4);
    hdr->file_count = layout->file_count;
    hdr->string_table_size = padded_string_table_size;
    hdr->reserved = 0;

    Pfs0FileEntry* entries = (Pfs0FileEntry*)(layout->pfs0_header + sizeof(Pfs0Header));
    u8* string_table = layout->pfs0_header + sizeof(Pfs0Header) + (sizeof(Pfs0FileEntry) * layout->file_count);

    u64 data_offset = 0;
    u32 string_offset = 0;

    for (u32 i = 0; i < layout->file_count; i++)
    {
        DumpNspFileEntry* file = &layout->files[i];

        entries[i].offset = data_offset;
        entries[i].size = file->in_memory ? file->memory_size : file->size;
        entries[i].string_offset = string_offset;
        entries[i].reserved = 0;

        file->offset_in_data = data_offset;
        data_offset += entries[i].size;

        u32 name_len = strlen(file->filename) + 1;
        memcpy(string_table + string_offset, file->filename, name_len);
        string_offset += name_len;
    }

    layout->total_nsp_size = (u64)header_size + data_offset;
}

static void compute_nsp_layout(DumpContext* ctx, DumpNspLayout* layout,
                               const NcmContentMetaKey* keys, u32 key_count,
                               NcmStorageId primary_storage)
{
    if (layout->computed) return;

    memset(layout->files, 0, sizeof(layout->files));
    layout->file_count = 0;
    layout->ticket_data = NULL;
    layout->ticket_size = 0;
    layout->cert_data = NULL;
    layout->cert_size = 0;
    layout->pfs0_header = NULL;
    layout->pfs0_header_size = 0;

    NcmContentMetaDatabase* meta_db = NULL;
    NcmStorageId storage_id = primary_storage;

    if (storage_id == NcmStorageId_SdCard)
    {
        if (!ctx->sd_meta_db_open || !ctx->sd_storage_open)
        {
            if (ctx->nand_meta_db_open && ctx->nand_storage_open)
            {
                storage_id = NcmStorageId_BuiltInUser;
            }
            else
            {
                layout->computed = true;
                return;
            }
        }
    }
    else
    {
        if (!ctx->nand_meta_db_open || !ctx->nand_storage_open)
        {
            if (ctx->sd_meta_db_open && ctx->sd_storage_open)
            {
                storage_id = NcmStorageId_SdCard;
            }
            else
            {
                layout->computed = true;
                return;
            }
        }
    }

    if (storage_id == NcmStorageId_SdCard)
    {
        meta_db = &ctx->sd_meta_db;
    }
    else
    {
        meta_db = &ctx->nand_meta_db;
    }

    (void)primary_storage;

    for (u32 k = 0; k < key_count && layout->file_count < DUMP_MAX_FILES_PER_NSP; k++)
    {
        NcmContentInfo content_infos[32];
        s32 content_count = 0;

        Result rc = ncmContentMetaDatabaseListContentInfo(meta_db,
                                                          &content_count, content_infos, 32, &keys[k], 0);
        if (R_FAILED(rc))
        {
            DBG_PRINT("ERROR: ncmContentMetaDatabaseListContentInfo failed: 0x%08X", rc);
            continue;
        }

        for (s32 i = 0; i < content_count && layout->file_count < DUMP_MAX_FILES_PER_NSP; i++)
        {
            DumpNspFileEntry* entry = &layout->files[layout->file_count];
            memset(entry, 0, sizeof(DumpNspFileEntry));

            entry->content_id = content_infos[i].content_id;
            entry->storage_id = storage_id;
            entry->size = get_nca_size(&content_infos[i]);
            entry->in_memory = false;

            if (content_infos[i].content_type == NcmContentType_Meta)
            {
                build_cnmt_nca_filename(entry->filename, sizeof(entry->filename), &content_infos[i].content_id);
            }
            else
            {
                build_nca_filename(entry->filename, sizeof(entry->filename), &content_infos[i].content_id);
            }

            layout->file_count++;
        }
    }

    if (layout->file_count == 0)
    {
        layout->computed = true;
        return;
    }

    try_extract_rights_id(ctx, layout);

    if (layout->ticket_data && layout->ticket_size > 0 && layout->file_count < DUMP_MAX_FILES_PER_NSP)
    {
        DumpNspFileEntry* tik_entry = &layout->files[layout->file_count];
        memset(tik_entry, 0, sizeof(DumpNspFileEntry));

        snprintf(tik_entry->filename, sizeof(tik_entry->filename),
                 "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.tik",
                 layout->ticket_data[0x150], layout->ticket_data[0x151],
                 layout->ticket_data[0x152], layout->ticket_data[0x153],
                 layout->ticket_data[0x154], layout->ticket_data[0x155],
                 layout->ticket_data[0x156], layout->ticket_data[0x157],
                 layout->ticket_data[0x158], layout->ticket_data[0x159],
                 layout->ticket_data[0x15A], layout->ticket_data[0x15B],
                 layout->ticket_data[0x15C], layout->ticket_data[0x15D],
                 layout->ticket_data[0x15E], layout->ticket_data[0x15F]);

        tik_entry->in_memory = true;
        tik_entry->memory_data = layout->ticket_data;
        tik_entry->memory_size = layout->ticket_size;
        tik_entry->size = layout->ticket_size;

        layout->file_count++;
    }

    generate_cert_chain(ctx, layout);
    if (layout->cert_data && layout->cert_size > 0 && layout->file_count < DUMP_MAX_FILES_PER_NSP)
    {
        DumpNspFileEntry* cert_entry = &layout->files[layout->file_count];
        memset(cert_entry, 0, sizeof(DumpNspFileEntry));

        if (layout->ticket_data && layout->ticket_size > 0x160)
        {
            snprintf(cert_entry->filename, sizeof(cert_entry->filename),
                     "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.cert",
                     layout->ticket_data[0x150], layout->ticket_data[0x151],
                     layout->ticket_data[0x152], layout->ticket_data[0x153],
                     layout->ticket_data[0x154], layout->ticket_data[0x155],
                     layout->ticket_data[0x156], layout->ticket_data[0x157],
                     layout->ticket_data[0x158], layout->ticket_data[0x159],
                     layout->ticket_data[0x15A], layout->ticket_data[0x15B],
                     layout->ticket_data[0x15C], layout->ticket_data[0x15D],
                     layout->ticket_data[0x15E], layout->ticket_data[0x15F]);
        }
        else
        {
            snprintf(cert_entry->filename, sizeof(cert_entry->filename), "cert.cert");
        }

        cert_entry->in_memory = true;
        cert_entry->memory_data = layout->cert_data;
        cert_entry->memory_size = layout->cert_size;
        cert_entry->size = layout->cert_size;

        layout->file_count++;
    }

    build_pfs0_header(layout);

    layout->computed = true;
}

static void free_layout(DumpNspLayout* layout)
{
    if (layout->pfs0_header)
    {
        free(layout->pfs0_header);
        layout->pfs0_header = NULL;
    }
    if (layout->ticket_data)
    {
        free(layout->ticket_data);
        layout->ticket_data = NULL;
    }
    if (layout->cert_data)
    {
        free(layout->cert_data);
        layout->cert_data = NULL;
    }
    layout->computed = false;
    layout->file_count = 0;
    layout->total_nsp_size = 0;
    layout->pfs0_header_size = 0;
}

static bool ensure_dump_services(DumpContext* ctx)
{
    if (ctx->ncm_initialized && ctx->ns_initialized)
    {
        return true;
    }

    if (!ctx->ncm_initialized)
    {
        Result rc = ncmInitialize();
        if (R_SUCCEEDED(rc))
        {
            ctx->ncm_initialized = true;
        }
    }

    if (!ctx->ns_initialized)
    {
        Result rc = nsInitialize();
        if (R_SUCCEEDED(rc))
        {
            ctx->ns_initialized = true;
        }
    }

    open_ncm_handles(ctx);

    return ctx->ns_initialized && ctx->ncm_initialized;
}

void dumpEnumerateGames(DumpContext* ctx)
{
    if (ctx->games_enumerated) return;

    DBG_PRINT("Enumerating installed games...\n");

    if (!ensure_dump_services(ctx))
    {
        DBG_PRINT("ERROR: Services failed to initialize\n");
        ctx->games_enumerated = true;
        return;
    }

    if (!ctx->sd_meta_db_open && !ctx->nand_meta_db_open)
    {
        DBG_PRINT("ERROR: No meta DB available\n");
        ctx->games_enumerated = true;
        return;
    }

    NcmApplicationContentMetaKey* sd_app_keys = (NcmApplicationContentMetaKey*)malloc(
        sizeof(NcmApplicationContentMetaKey) * 512);
    NcmApplicationContentMetaKey* nand_app_keys = (NcmApplicationContentMetaKey*)malloc(
        sizeof(NcmApplicationContentMetaKey) * 512);

    if (!sd_app_keys || !nand_app_keys)
    {
        DBG_PRINT("Failed to allocate buffers for enumeration\n");
        free(sd_app_keys);
        free(nand_app_keys);
        ctx->games_enumerated = true;
        return;
    }

    u32 merged_handle = MTP_HANDLE_DUMP_MERGED_START;
    u32 sep_folder_handle = MTP_HANDLE_DUMP_SEP_FOLDER_START;
    u32 sep_file_handle = MTP_HANDLE_DUMP_SEP_FILE_START;

    s32 sd_count = 0;
    s32 sd_total = 0;
    if (ctx->sd_meta_db_open)
    {
        Result rc = ncmContentMetaDatabaseListApplication(&ctx->sd_meta_db, &sd_total, &sd_count,
                                                          sd_app_keys, 512, NcmContentMetaType_Application);
        if (R_FAILED(rc))
        {
            DBG_PRINT("SD query failed: 0x%08X\n", rc);
        }
    }

    s32 nand_count = 0;
    s32 nand_total = 0;
    if (ctx->nand_meta_db_open)
    {
        Result rc = ncmContentMetaDatabaseListApplication(&ctx->nand_meta_db, &nand_total, &nand_count,
                                                          nand_app_keys, 512, NcmContentMetaType_Application);
        if (R_FAILED(rc))
        {
            DBG_PRINT("NAND query failed: 0x%08X\n", rc);
        }
    }

    for (s32 i = 0; i < sd_count && ctx->game_count < ctx->max_games; i++)
    {
        u64 app_id = sd_app_keys[i].application_id;

        DumpGameEntry* game = &ctx->games[ctx->game_count];
        memset(game, 0, sizeof(DumpGameEntry));
        game->application_id = app_id;
        game->is_on_sd = true;
        game->merged_handle = merged_handle++;
        game->separate_folder_handle = sep_folder_handle++;
        game->version = sd_app_keys[i].key.version;

        get_game_name_from_ns(app_id, game->game_name, sizeof(game->game_name));

        NcmContentMetaDatabase* meta_db = &ctx->sd_meta_db;
        u8 meta_buffer[0x4000];

        DumpContentMetaEntry* cme = &game->content_metas[game->content_meta_count];
        memset(cme, 0, sizeof(DumpContentMetaEntry));
        cme->key = sd_app_keys[i].key;
        cme->handle = sep_file_handle++;

        char display_version[32];
        get_display_version(meta_db, &sd_app_keys[i].key, NcmStorageId_SdCard, display_version,
                            sizeof(display_version));

        snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX][%s].nsp",
                 game->game_name, (unsigned long)sd_app_keys[i].key.id, display_version);
        game->content_meta_count++;

        NcmContentMetaKey patch_keys[32];
        s32 patch_count = 0;
        s32 patch_total = 0;
        Result rc_patch = ncmContentMetaDatabaseList(meta_db, &patch_total, &patch_count,
                                                     patch_keys, 32, NcmContentMetaType_Patch, 0, 0,
                                                     0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Full);

        if (R_SUCCEEDED(rc_patch) && patch_count > 0)
        {
            for (s32 p = 0; p < patch_count && game->content_meta_count < DUMP_MAX_CONTENT_METAS - 1; p++)
            {
                u64 meta_size = 0;
                Result rc_read = ncmContentMetaDatabaseGet(meta_db, &patch_keys[p], &meta_size, meta_buffer,
                                                           sizeof(meta_buffer));
                if (R_FAILED(rc_read) || meta_size < sizeof(NcmContentMetaHeader) + sizeof(NcmPatchMetaExtendedHeader))
                {
                    continue;
                }

                NcmContentMetaHeader* meta_hdr = (NcmContentMetaHeader*)meta_buffer;
                (void)meta_hdr;
                NcmPatchMetaExtendedHeader* ext_hdr = (NcmPatchMetaExtendedHeader*)(meta_buffer + sizeof(
                    NcmContentMetaHeader));

                if (ext_hdr->application_id == app_id)
                {
                    cme = &game->content_metas[game->content_meta_count];
                    memset(cme, 0, sizeof(DumpContentMetaEntry));
                    cme->key = patch_keys[p];
                    cme->handle = sep_file_handle++;

                    char display_version[32];
                    get_display_version(meta_db, &patch_keys[p], NcmStorageId_SdCard, display_version,
                                        sizeof(display_version));

                    snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX][%s] (Update).nsp",
                             game->game_name, (unsigned long)patch_keys[p].id, display_version);
                    DBG_PRINT("    Added patch: %s\n", cme->filename);
                    game->content_meta_count++;
                }
            }
        }

        NcmContentMetaKey dlc_keys[32];
        s32 dlc_count = 0;
        s32 dlc_total = 0;
        Result rc_dlc = ncmContentMetaDatabaseList(meta_db, &dlc_total, &dlc_count,
                                                   dlc_keys, 32, NcmContentMetaType_AddOnContent, 0, 0,
                                                   0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Full);

        if (R_SUCCEEDED(rc_dlc) && dlc_count > 0)
        {
            for (s32 d = 0; d < dlc_count && game->content_meta_count < DUMP_MAX_CONTENT_METAS - 1; d++)
            {
                u64 meta_size = 0;
                Result rc_read = ncmContentMetaDatabaseGet(meta_db, &dlc_keys[d], &meta_size, meta_buffer,
                                                           sizeof(meta_buffer));
                if (R_FAILED(rc_read) || meta_size < sizeof(NcmContentMetaHeader) + sizeof(
                    NcmLegacyAddOnContentMetaExtendedHeader))
                {
                    continue;
                }

                NcmContentMetaHeader* meta_hdr = (NcmContentMetaHeader*)meta_buffer;

                u64 dlc_app_id = 0;
                if (meta_hdr->extended_header_size >= sizeof(NcmAddOnContentMetaExtendedHeader))
                {
                    NcmAddOnContentMetaExtendedHeader* ext_hdr = (NcmAddOnContentMetaExtendedHeader*)(meta_buffer +
                        sizeof(NcmContentMetaHeader));
                    dlc_app_id = ext_hdr->application_id;
                }
                else if (meta_hdr->extended_header_size >= sizeof(NcmLegacyAddOnContentMetaExtendedHeader))
                {
                    NcmLegacyAddOnContentMetaExtendedHeader* ext_hdr = (NcmLegacyAddOnContentMetaExtendedHeader*)(
                        meta_buffer + sizeof(NcmContentMetaHeader));
                    dlc_app_id = ext_hdr->application_id;
                }

                if (dlc_app_id == app_id)
                {
                    cme = &game->content_metas[game->content_meta_count];
                    memset(cme, 0, sizeof(DumpContentMetaEntry));
                    cme->key = dlc_keys[d];
                    cme->handle = sep_file_handle++;

                    char dlc_name[256] = "DLC";
                    if (!nacpGetDlcName(meta_db, &dlc_keys[d], NcmStorageId_SdCard, dlc_name, sizeof(dlc_name)))
                    {
                        NcmContentId cnmt_id;
                        Result rc_cnmt = ncmContentMetaDatabaseGetContentIdByType(meta_db, &cnmt_id,
                            &dlc_keys[d], NcmContentType_Meta);
                        if (R_SUCCEEDED(rc_cnmt))
                        {
                            CnmtContext cnmt_ctx;
                            if (cnmtReadFromInstalledNca(&cnmt_id, NcmStorageId_SdCard, &cnmt_ctx))
                            {
                                cnmtGetDlcDisplayName(&cnmt_ctx, game->game_name, dlc_name, sizeof(dlc_name));
                                cnmtFree(&cnmt_ctx);
                            }
                        }
                    }

                    char display_version[32] = {0};
                    get_display_version(meta_db, &dlc_keys[d], NcmStorageId_SdCard, display_version,
                                        sizeof(display_version));

                    if (display_version[0] != '\0')
                    {
                        snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX][%s] (%s).nsp",
                                 game->game_name, (unsigned long)dlc_keys[d].id, display_version, dlc_name);
                    }
                    else
                    {
                        snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX] (%s).nsp",
                                 game->game_name, (unsigned long)dlc_keys[d].id, dlc_name);
                    }
                    game->content_meta_count++;
                }
            }
        }

        ctx->game_count++;
    }

    for (s32 i = 0; i < nand_count && ctx->game_count < ctx->max_games; i++)
    {
        u64 app_id = nand_app_keys[i].application_id;

        DumpGameEntry* game = &ctx->games[ctx->game_count];
        memset(game, 0, sizeof(DumpGameEntry));
        game->application_id = app_id;
        game->is_on_sd = false;
        game->merged_handle = merged_handle++;
        game->separate_folder_handle = sep_folder_handle++;
        game->version = nand_app_keys[i].key.version;

        get_game_name_from_ns(app_id, game->game_name, sizeof(game->game_name));

        NcmContentMetaDatabase* meta_db = &ctx->nand_meta_db;
        u8 meta_buffer[0x4000];

        DumpContentMetaEntry* cme = &game->content_metas[game->content_meta_count];
        memset(cme, 0, sizeof(DumpContentMetaEntry));
        cme->key = nand_app_keys[i].key;
        cme->handle = sep_file_handle++;

        char display_version[32];
        get_display_version(meta_db, &nand_app_keys[i].key, NcmStorageId_BuiltInUser, display_version,
                            sizeof(display_version));

        snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX][%s].nsp",
                 game->game_name, (unsigned long)nand_app_keys[i].key.id, display_version);
        game->content_meta_count++;

        NcmContentMetaKey patch_keys[32];
        s32 patch_count = 0;
        s32 patch_total = 0;
        Result rc_patch = ncmContentMetaDatabaseList(meta_db, &patch_total, &patch_count,
                                                     patch_keys, 32, NcmContentMetaType_Patch, 0, 0,
                                                     0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Full);

        if (R_SUCCEEDED(rc_patch) && patch_count > 0)
        {
            for (s32 p = 0; p < patch_count && game->content_meta_count < DUMP_MAX_CONTENT_METAS - 1; p++)
            {
                u64 meta_size = 0;
                Result rc_read = ncmContentMetaDatabaseGet(meta_db, &patch_keys[p], &meta_size, meta_buffer,
                                                           sizeof(meta_buffer));
                if (R_FAILED(rc_read) || meta_size < sizeof(NcmContentMetaHeader) + sizeof(NcmPatchMetaExtendedHeader))
                {
                    continue;
                }

                NcmPatchMetaExtendedHeader* ext_hdr = (NcmPatchMetaExtendedHeader*)(meta_buffer + sizeof(
                    NcmContentMetaHeader));

                if (ext_hdr->application_id == app_id)
                {
                    cme = &game->content_metas[game->content_meta_count];
                    memset(cme, 0, sizeof(DumpContentMetaEntry));
                    cme->key = patch_keys[p];
                    cme->handle = sep_file_handle++;

                    char display_version[32];
                    get_display_version(meta_db, &patch_keys[p], NcmStorageId_BuiltInUser, display_version,
                                        sizeof(display_version));

                    snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX][%s] (Update).nsp",
                             game->game_name, (unsigned long)patch_keys[p].id, display_version);
                    game->content_meta_count++;
                }
            }
        }

        NcmContentMetaKey dlc_keys[32];
        s32 dlc_count = 0;
        s32 dlc_total = 0;
        Result rc_dlc = ncmContentMetaDatabaseList(meta_db, &dlc_total, &dlc_count,
                                                   dlc_keys, 32, NcmContentMetaType_AddOnContent, 0, 0,
                                                   0xFFFFFFFFFFFFFFFFULL, NcmContentInstallType_Full);

        if (R_SUCCEEDED(rc_dlc) && dlc_count > 0)
        {
            for (s32 d = 0; d < dlc_count && game->content_meta_count < DUMP_MAX_CONTENT_METAS - 1; d++)
            {
                u64 meta_size = 0;
                Result rc_read = ncmContentMetaDatabaseGet(meta_db, &dlc_keys[d], &meta_size, meta_buffer,
                                                           sizeof(meta_buffer));
                if (R_FAILED(rc_read) || meta_size < sizeof(NcmContentMetaHeader) + sizeof(
                    NcmLegacyAddOnContentMetaExtendedHeader))
                {
                    continue;
                }

                NcmContentMetaHeader* meta_hdr = (NcmContentMetaHeader*)meta_buffer;

                u64 dlc_app_id = 0;
                if (meta_hdr->extended_header_size >= sizeof(NcmAddOnContentMetaExtendedHeader))
                {
                    NcmAddOnContentMetaExtendedHeader* ext_hdr = (NcmAddOnContentMetaExtendedHeader*)(meta_buffer +
                        sizeof(NcmContentMetaHeader));
                    dlc_app_id = ext_hdr->application_id;
                }
                else if (meta_hdr->extended_header_size >= sizeof(NcmLegacyAddOnContentMetaExtendedHeader))
                {
                    NcmLegacyAddOnContentMetaExtendedHeader* ext_hdr = (NcmLegacyAddOnContentMetaExtendedHeader*)(
                        meta_buffer + sizeof(NcmContentMetaHeader));
                    dlc_app_id = ext_hdr->application_id;
                }

                if (dlc_app_id == app_id)
                {
                    cme = &game->content_metas[game->content_meta_count];
                    memset(cme, 0, sizeof(DumpContentMetaEntry));
                    cme->key = dlc_keys[d];
                    cme->handle = sep_file_handle++;

                    char dlc_name[256] = "DLC";
                    if (!nacpGetDlcName(meta_db, &dlc_keys[d], NcmStorageId_BuiltInUser, dlc_name, sizeof(dlc_name)))
                    {
                        NcmContentId cnmt_id;
                        Result rc_cnmt = ncmContentMetaDatabaseGetContentIdByType(meta_db, &cnmt_id,
                            &dlc_keys[d], NcmContentType_Meta);
                        if (R_SUCCEEDED(rc_cnmt))
                        {
                            CnmtContext cnmt_ctx;
                            if (cnmtReadFromInstalledNca(&cnmt_id, NcmStorageId_BuiltInUser, &cnmt_ctx))
                            {
                                cnmtGetDlcDisplayName(&cnmt_ctx, game->game_name, dlc_name, sizeof(dlc_name));
                                cnmtFree(&cnmt_ctx);
                            }
                        }
                    }

                    char display_version[32] = {0};
                    get_display_version(meta_db, &dlc_keys[d], NcmStorageId_BuiltInUser, display_version,
                                        sizeof(display_version));

                    if (display_version[0] != '\0')
                    {
                        snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX][%s] (%s).nsp",
                                 game->game_name, (unsigned long)dlc_keys[d].id, display_version, dlc_name);
                    }
                    else
                    {
                        snprintf(cme->filename, sizeof(cme->filename), "%s [%016lX] (%s).nsp",
                                 game->game_name, (unsigned long)dlc_keys[d].id, dlc_name);
                    }
                    game->content_meta_count++;
                }
            }
        }

        ctx->game_count++;
    }

    free(sd_app_keys);
    free(nand_app_keys);

    ctx->games_enumerated = true;
    DBG_PRINT("Enumeration complete, %u games found\n", ctx->game_count);
    fflush(stdout);
}

void dumpEnsureMergedLayout(DumpContext* ctx, DumpGameEntry* game)
{
    if (game->merged_layout.computed) return;

    NcmContentMetaKey keys[DUMP_MAX_CONTENT_METAS];
    u32 key_count = 0;

    for (u32 i = 0; i < game->content_meta_count && key_count < DUMP_MAX_CONTENT_METAS; i++)
    {
        keys[key_count++] = game->content_metas[i].key;
    }

    NcmStorageId storage = game->is_on_sd ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    compute_nsp_layout(ctx, &game->merged_layout, keys, key_count, storage);
}

void dumpEnsureSeparateLayout(DumpContext* ctx, DumpGameEntry* game, u32 meta_idx)
{
    if (meta_idx >= game->content_meta_count) return;

    DumpContentMetaEntry* cme = &game->content_metas[meta_idx];
    if (cme->layout.computed) return;

    NcmStorageId storage = game->is_on_sd ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    compute_nsp_layout(ctx, &cme->layout, &cme->key, 1, storage);
}

Result dumpInit(DumpContext* ctx)
{
    if (ctx->initialized) return 0;

    memset(ctx, 0, sizeof(DumpContext));
    mutexInit(&ctx->dump_mutex);

    ctx->ncm_initialized = false;
    ctx->ns_initialized = false;
    ctx->es_initialized = false;
    ctx->max_games = DUMP_MAX_GAMES;
    ctx->games = (DumpGameEntry*)calloc(ctx->max_games, sizeof(DumpGameEntry));
    if (!ctx->games)
    {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    ctx->games_enumerated = false;
    ctx->needs_refresh = true;
    ctx->initialized = true;

    return 0;
}

void dumpExit(DumpContext* ctx)
{
    if (!ctx->initialized) return;

    mutexLock(&ctx->dump_mutex);

    for (u32 i = 0; i < ctx->game_count; i++)
    {
        free_layout(&ctx->games[i].merged_layout);
        for (u32 j = 0; j < ctx->games[i].content_meta_count; j++)
        {
            free_layout(&ctx->games[i].content_metas[j].layout);
        }
    }

    free(ctx->games);
    ctx->games = NULL;

    if (ctx->sd_storage_open)
    {
        ncmContentStorageClose(&ctx->sd_storage);
        ctx->sd_storage_open = false;
    }
    if (ctx->nand_storage_open)
    {
        ncmContentStorageClose(&ctx->nand_storage);
        ctx->nand_storage_open = false;
    }
    if (ctx->sd_meta_db_open)
    {
        ncmContentMetaDatabaseClose(&ctx->sd_meta_db);
        ctx->sd_meta_db_open = false;
    }
    if (ctx->nand_meta_db_open)
    {
        ncmContentMetaDatabaseClose(&ctx->nand_meta_db);
        ctx->nand_meta_db_open = false;
    }

    if (ctx->ncm_initialized)
    {
        ncmExit();
        ctx->ncm_initialized = false;
    }

    if (ctx->ns_initialized)
    {
        nsExit();
        ctx->ns_initialized = false;
    }

    if (ctx->es_initialized)
    {
        esExit();
        ctx->es_initialized = false;
    }

    mutexUnlock(&ctx->dump_mutex);
    ctx->initialized = false;
}

void dumpPreInitServices(DumpContext* ctx)
{
    if (!ctx->initialized)
    {
        memset(ctx, 0, sizeof(DumpContext));
        mutexInit(&ctx->dump_mutex);
        ctx->max_games = DUMP_MAX_GAMES;
        ctx->games = (DumpGameEntry*)calloc(ctx->max_games, sizeof(DumpGameEntry));
        if (!ctx->games) return;
        ctx->games_enumerated = false;
        ctx->needs_refresh = true;
        ctx->initialized = true;
    }

    if (!ctx->ns_initialized)
    {
        Result rc = nsInitialize();
        if (R_SUCCEEDED(rc))
        {
            ctx->ns_initialized = true;
        }
    }

    if (!ctx->ncm_initialized)
    {
        Result rc = ncmInitialize();
        if (R_SUCCEEDED(rc))
        {
            ctx->ncm_initialized = true;
        }
    }

    open_ncm_handles(ctx);
}

void dumpRefreshIfNeeded(DumpContext* ctx)
{
    if (!ctx->initialized) return;
    if (!ctx->needs_refresh) return;

    mutexLock(&ctx->dump_mutex);
    ctx->needs_refresh = false;

    for (u32 i = 0; i < ctx->game_count; i++)
    {
        free_layout(&ctx->games[i].merged_layout);
        for (u32 j = 0; j < ctx->games[i].content_meta_count; j++)
        {
            free_layout(&ctx->games[i].content_metas[j].layout);
        }
    }
    ctx->game_count = 0;
    ctx->games_enumerated = false;

    mutexUnlock(&ctx->dump_mutex);
}

s64 dumpReadNspData(DumpContext* ctx, DumpNspLayout* layout, u64 offset, void* buffer, u64 size)
{
    if (!layout || !layout->computed || layout->total_nsp_size == 0)
    {
        return -1;
    }

    if (offset >= layout->total_nsp_size)
    {
        return 0;
    }
    if (offset + size > layout->total_nsp_size)
    {
        size = layout->total_nsp_size - offset;
    }

    u64 bytes_read = 0;
    u8* out = (u8*)buffer;

    while (bytes_read < size)
    {
        u64 cur_offset = offset + bytes_read;
        u64 remaining = size - bytes_read;

        if (cur_offset < layout->pfs0_header_size)
        {
            u64 header_remaining = layout->pfs0_header_size - cur_offset;
            u64 to_copy = (remaining < header_remaining) ? remaining : header_remaining;
            memcpy(out + bytes_read, layout->pfs0_header + cur_offset, to_copy);
            bytes_read += to_copy;
        }
        else
        {
            u64 data_offset = cur_offset - layout->pfs0_header_size;
            s64 rd = 0;
            bool found_entry = false;
            DumpNspFileEntry entry_copy;
            memset(&entry_copy, 0, sizeof(entry_copy));

            mutexLock(&ctx->dump_mutex);
            {
                for (u32 i = 0; i < layout->file_count; i++)
                {
                    DumpNspFileEntry* entry = &layout->files[i];
                    u64 file_size = entry->in_memory ? entry->memory_size : entry->size;
                    u64 file_start = entry->offset_in_data;
                    u64 file_end = file_start + file_size;

                    if (data_offset >= file_start && data_offset < file_end)
                    {
                        u64 file_offset = data_offset - file_start;
                        u64 file_remaining = file_size - file_offset;
                        u64 to_read = (remaining < file_remaining) ? remaining : file_remaining;

                        if (entry->in_memory)
                        {
                            memcpy(out + bytes_read, entry->memory_data + file_offset, to_read);
                            rd = to_read;
                            bytes_read += rd;
                            found_entry = true;
                        }
                        else
                        {
                            memcpy(&entry_copy, entry, sizeof(DumpNspFileEntry));
                            found_entry = true;
                        }
                        break;
                    }
                }
            }
            mutexUnlock(&ctx->dump_mutex);

            if (found_entry && !entry_copy.in_memory && entry_copy.size > 0)
            {
                u64 file_offset = data_offset - entry_copy.offset_in_data;
                u64 file_size = entry_copy.size;
                u64 file_remaining = file_size - file_offset;
                u64 to_read = (remaining < file_remaining) ? remaining : file_remaining;

                rd = read_nca_data(ctx, &entry_copy, file_offset, out + bytes_read, to_read);
                if (rd <= 0)
                {
                    memset(out + bytes_read, 0, to_read);
                    rd = to_read;
                }
                bytes_read += rd;
            }
            else if (!found_entry)
            {
                break;
            }
        }
    }

    return (s64)bytes_read;
}
