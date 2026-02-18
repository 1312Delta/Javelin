// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_dump.h"
#include "mtp/mtp_log.h"
#include "install/cnmt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

// ---------------------------------------------------------------------------
// Handle classification helpers
// ---------------------------------------------------------------------------

static bool is_merged_file_handle(u32 handle) {
    return handle >= MTP_HANDLE_DUMP_MERGED_START && handle <= MTP_HANDLE_DUMP_MERGED_END;
}

static bool is_sep_folder_handle(u32 handle) {
    return handle >= MTP_HANDLE_DUMP_SEP_FOLDER_START && handle <= MTP_HANDLE_DUMP_SEP_FOLDER_END;
}

static bool is_sep_file_handle(u32 handle) {
    return handle >= MTP_HANDLE_DUMP_SEP_FILE_START && handle <= MTP_HANDLE_DUMP_SEP_FILE_END;
}

// ---------------------------------------------------------------------------
// Game lookup by handle
// ---------------------------------------------------------------------------

static DumpGameEntry* find_game_by_merged_handle(DumpContext* ctx, u32 handle) {
    for (u32 i = 0; i < ctx->game_count; i++) {
        if (ctx->games[i].merged_handle == handle) return &ctx->games[i];
    }
    return NULL;
}

static DumpGameEntry* find_game_by_sep_folder(DumpContext* ctx, u32 handle) {
    for (u32 i = 0; i < ctx->game_count; i++) {
        if (ctx->games[i].separate_folder_handle == handle) return &ctx->games[i];
    }
    return NULL;
}

static DumpGameEntry* find_game_by_sep_file(DumpContext* ctx, u32 handle, u32* meta_idx_out) {
    for (u32 i = 0; i < ctx->game_count; i++) {
        for (u32 j = 0; j < ctx->games[i].content_meta_count; j++) {
            if (ctx->games[i].content_metas[j].handle == handle) {
                if (meta_idx_out) *meta_idx_out = j;
                return &ctx->games[i];
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Version string helper (for merged filename generation)
// ---------------------------------------------------------------------------

static void parse_version_string(u32 version_key, char* out_version, size_t out_size) {
    u16 major = (version_key >> 16) & 0xFFFF;
    u8 minor = (version_key >> 8) & 0xFF;
    u8 micro = version_key & 0xFF;

    if (version_key == 0) {
        snprintf(out_version, out_size, "v1.0");
    } else if (micro > 0) {
        snprintf(out_version, out_size, "v%u.%u.%u", major, minor, micro);
    } else if (minor > 0) {
        snprintf(out_version, out_size, "v%u.%u", major, minor);
    } else {
        snprintf(out_version, out_size, "v%u.0", major);
    }
}

static void ensure_version_prefix(char* version, size_t size) {
    if (version[0] != '\0' && version[0] != 'v' && version[0] != 'V') {
        char temp[64];
        snprintf(temp, sizeof(temp), "v%s", version);
        strncpy(version, temp, size - 1);
        version[size - 1] = '\0';
    }
}

static bool get_display_version_from_ns(u64 app_id, char* out_version, size_t out_size) {
    NsApplicationControlData* ctrl = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (!ctrl) return false;

    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, ctrl, sizeof(*ctrl), &actual);
    if (R_SUCCEEDED(rc) && actual >= sizeof(ctrl->nacp)) {
        if (ctrl->nacp.display_version[0] != '\0') {
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
                                NcmStorageId storage_id, char* out_version, size_t out_size) {
    out_version[0] = '\0';

    if (meta_key->type == NcmContentMetaType_Patch) {
        u64 base_app_id = meta_key->id & ~0x800ULL;
        if (get_display_version_from_ns(base_app_id, out_version, out_size)) {
            ensure_version_prefix(out_version, out_size);
            return;
        }
    }

    if (nacpGetDisplayVersionFromControlNca(meta_db, meta_key, storage_id, out_version, out_size)) {
        ensure_version_prefix(out_version, out_size);
        return;
    }

    if (get_display_version_from_ns(meta_key->id, out_version, out_size)) {
        ensure_version_prefix(out_version, out_size);
        return;
    }

    NcmContentId cnmt_id;
    Result rc_cnmt = ncmContentMetaDatabaseGetContentIdByType(meta_db, &cnmt_id, meta_key, NcmContentType_Meta);
    if (R_SUCCEEDED(rc_cnmt)) {
        CnmtContext cnmt_ctx;
        if (cnmtReadFromInstalledNca(&cnmt_id, storage_id, &cnmt_ctx)) {
            cnmtGetDisplayVersion(&cnmt_ctx, out_version, out_size);
            cnmtFree(&cnmt_ctx);
            if (out_version[0] != '\0') {
                ensure_version_prefix(out_version, out_size);
                return;
            }
        }
    }

    if (out_version[0] == '\0') {
        parse_version_string(meta_key->version, out_version, out_size);
    }
}

// ---------------------------------------------------------------------------
// MTP virtual storage adapter functions
// ---------------------------------------------------------------------------

bool dumpIsVirtualStorage(u32 storage_id) {
    return storage_id == MTP_STORAGE_DUMP;
}

bool dumpIsVirtualHandle(u32 handle) {
    return handle >= MTP_HANDLE_DUMP_BASE && handle <= MTP_HANDLE_DUMP_SEP_FILE_END;
}

bool dumpGetStorageInfo(DumpContext* ctx, u32 storage_id, MtpStorageInfo* out) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_DUMP) return false;

    memset(out, 0, sizeof(MtpStorageInfo));
    out->storage_id = MTP_STORAGE_DUMP;
    out->storage_type = 0x0003;
    out->filesystem_type = 0x0002;
    out->access_capability = 0x0001;
    out->max_capacity = 64ULL * 1024 * 1024 * 1024;
    out->free_space = 0;
    strncpy(out->description, "Installed Games", sizeof(out->description));
    strncpy(out->volume_label, "INSTALLED", sizeof(out->volume_label));
    out->mounted = true;
    return true;
}

u32 dumpGetObjectCount(DumpContext* ctx, u32 storage_id, u32 parent_handle) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_DUMP) return 0;

    mutexLock(&ctx->dump_mutex);

    if (!ctx->games_enumerated) {
        dumpEnumerateGames(ctx);
    }

    u32 count = 0;

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        count = 2;
    } else if (parent_handle == MTP_HANDLE_DUMP_CAT_MERGED) {
        count = ctx->game_count;
    } else if (parent_handle == MTP_HANDLE_DUMP_CAT_SEPARATE) {
        count = ctx->game_count;
    } else if (is_sep_folder_handle(parent_handle)) {
        DumpGameEntry* game = find_game_by_sep_folder(ctx, parent_handle);
        if (game) count = game->content_meta_count;
    }

    mutexUnlock(&ctx->dump_mutex);
    return count;
}

u32 dumpEnumObjects(DumpContext* ctx, u32 storage_id, u32 parent_handle, u32* handles, u32 max) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_DUMP) return 0;

    mutexLock(&ctx->dump_mutex);

    if (!ctx->games_enumerated) {
        dumpEnumerateGames(ctx);
    }

    u32 count = 0;

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        if (count < max) handles[count++] = MTP_HANDLE_DUMP_CAT_MERGED;
        if (count < max) handles[count++] = MTP_HANDLE_DUMP_CAT_SEPARATE;
    } else if (parent_handle == MTP_HANDLE_DUMP_CAT_MERGED) {
        for (u32 i = 0; i < ctx->game_count && count < max; i++) {
            handles[count++] = ctx->games[i].merged_handle;
        }
    } else if (parent_handle == MTP_HANDLE_DUMP_CAT_SEPARATE) {
        for (u32 i = 0; i < ctx->game_count && count < max; i++) {
            handles[count++] = ctx->games[i].separate_folder_handle;
        }
    } else if (is_sep_folder_handle(parent_handle)) {
        DumpGameEntry* game = find_game_by_sep_folder(ctx, parent_handle);
        if (game) {
            for (u32 i = 0; i < game->content_meta_count && count < max; i++) {
                handles[count++] = game->content_metas[i].handle;
            }
        }
    }

    mutexUnlock(&ctx->dump_mutex);
    return count;
}

bool dumpGetObjectInfo(DumpContext* ctx, u32 handle, MtpObject* out) {
    if (!ctx->initialized || !dumpIsVirtualHandle(handle)) return false;

    mutexLock(&ctx->dump_mutex);
    memset(out, 0, sizeof(MtpObject));
    bool found = false;

    if (handle == MTP_HANDLE_DUMP_CAT_MERGED) {
        out->handle = handle;
        out->parent_handle = 0xFFFFFFFF;
        out->storage_id = MTP_STORAGE_DUMP;
        out->format = MTP_FORMAT_ASSOCIATION;
        out->object_type = MTP_OBJECT_TYPE_FOLDER;
        strncpy(out->filename, "Merged", MTP_MAX_FILENAME - 1);
        found = true;
    } else if (handle == MTP_HANDLE_DUMP_CAT_SEPARATE) {
        out->handle = handle;
        out->parent_handle = 0xFFFFFFFF;
        out->storage_id = MTP_STORAGE_DUMP;
        out->format = MTP_FORMAT_ASSOCIATION;
        out->object_type = MTP_OBJECT_TYPE_FOLDER;
        strncpy(out->filename, "Separate", MTP_MAX_FILENAME - 1);
        found = true;
    } else if (is_merged_file_handle(handle)) {
        DumpGameEntry* game = find_game_by_merged_handle(ctx, handle);
        if (game) {
            dumpEnsureMergedLayout(ctx, game);

            out->handle = handle;
            out->parent_handle = MTP_HANDLE_DUMP_CAT_MERGED;
            out->storage_id = MTP_STORAGE_DUMP;
            out->format = MTP_FORMAT_UNDEFINED;
            out->object_type = MTP_OBJECT_TYPE_FILE;
            out->size = game->merged_layout.total_nsp_size;

            u32 update_count = 0;
            u32 dlc_count = 0;
            char highest_update_version[32] = {0};
            u32 highest_update_ver_key = 0;

            for (u32 i = 0; i < game->content_meta_count; i++) {
                u8 type = game->content_metas[i].key.type;
                if (type == NcmContentMetaType_Patch) {
                    update_count++;
                    if (game->content_metas[i].key.version > highest_update_ver_key) {
                        highest_update_ver_key = game->content_metas[i].key.version;
                    }
                } else if (type == NcmContentMetaType_AddOnContent) {
                    dlc_count++;
                }
            }

            NcmContentMetaDatabase* meta_db = game->is_on_sd ? &ctx->sd_meta_db : &ctx->nand_meta_db;
            NcmStorageId storage = game->is_on_sd ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

            if (update_count > 0) {
                for (u32 i = 0; i < game->content_meta_count; i++) {
                    if ((u8)game->content_metas[i].key.type == NcmContentMetaType_Patch &&
                        game->content_metas[i].key.version == highest_update_ver_key) {
                        get_display_version(meta_db, &game->content_metas[i].key, storage,
                                          highest_update_version, sizeof(highest_update_version));
                        break;
                    }
                }
            }

            char suffix[64] = {0};
            if (update_count > 0 && dlc_count > 0) {
                snprintf(suffix, sizeof(suffix), "+%uUPD+%uDLC", update_count, dlc_count);
            } else if (update_count > 0) {
                snprintf(suffix, sizeof(suffix), "+%uUPD", update_count);
            } else if (dlc_count > 0) {
                snprintf(suffix, sizeof(suffix), "+%uDLC", dlc_count);
            }

            if (highest_update_version[0] != '\0') {
                snprintf(out->filename, MTP_MAX_FILENAME - 1,
                    "%s [%016lX][%s]%s.nsp",
                    game->game_name, (unsigned long)game->application_id,
                    highest_update_version, suffix);
            } else {
                char base_version[32];
                parse_version_string(game->version, base_version, sizeof(base_version));
                snprintf(out->filename, MTP_MAX_FILENAME - 1,
                    "%s [%016lX][%s]%s.nsp",
                    game->game_name, (unsigned long)game->application_id,
                    base_version, suffix);
            }
            found = true;
        }
    } else if (is_sep_folder_handle(handle)) {
        DumpGameEntry* game = find_game_by_sep_folder(ctx, handle);
        if (game) {
            out->handle = handle;
            out->parent_handle = MTP_HANDLE_DUMP_CAT_SEPARATE;
            out->storage_id = MTP_STORAGE_DUMP;
            out->format = MTP_FORMAT_ASSOCIATION;
            out->object_type = MTP_OBJECT_TYPE_FOLDER;

            snprintf(out->filename, MTP_MAX_FILENAME - 1,
                "%s [%016lX]",
                game->game_name, (unsigned long)game->application_id);
            found = true;
        }
    } else if (is_sep_file_handle(handle)) {
        u32 meta_idx = 0;
        DumpGameEntry* game = find_game_by_sep_file(ctx, handle, &meta_idx);
        if (game && meta_idx < game->content_meta_count) {
            DumpContentMetaEntry* cme = &game->content_metas[meta_idx];

            dumpEnsureSeparateLayout(ctx, game, meta_idx);

            out->handle = handle;
            out->parent_handle = game->separate_folder_handle;
            out->storage_id = MTP_STORAGE_DUMP;
            out->format = MTP_FORMAT_UNDEFINED;
            out->object_type = MTP_OBJECT_TYPE_FILE;
            out->size = cme->layout.total_nsp_size;

            strncpy(out->filename, cme->filename, MTP_MAX_FILENAME - 1);
            found = true;
        }
    }

    mutexUnlock(&ctx->dump_mutex);
    return found;
}

s64 dumpReadObject(DumpContext* ctx, u32 handle, u64 offset, void* buffer, u64 size) {
    if (!ctx->initialized) return -1;

    DumpNspLayout* layout = NULL;

    mutexLock(&ctx->dump_mutex);
    {
        DumpGameEntry* game = NULL;

        if (is_merged_file_handle(handle)) {
            game = find_game_by_merged_handle(ctx, handle);
            if (game) {
                dumpEnsureMergedLayout(ctx, game);
                layout = &game->merged_layout;
            }
        } else if (is_sep_file_handle(handle)) {
            u32 meta_idx = 0;
            game = find_game_by_sep_file(ctx, handle, &meta_idx);
            if (game && meta_idx < game->content_meta_count) {
                dumpEnsureSeparateLayout(ctx, game, meta_idx);
                layout = &game->content_metas[meta_idx].layout;
            }
        }
    }
    mutexUnlock(&ctx->dump_mutex);

    if (!layout) return -1;

    return dumpReadNspData(ctx, layout, offset, buffer, size);
}
