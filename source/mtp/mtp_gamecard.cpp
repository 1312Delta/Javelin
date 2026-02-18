// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_gamecard.h"
#include "mtp/mtp_log.h"

#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// MTP virtual storage adapter functions
// ---------------------------------------------------------------------------

bool gcIsVirtualStorage(u32 storage_id) {
    return storage_id == MTP_STORAGE_GAMECARD;
}

bool gcIsVirtualHandle(u32 handle) {
    return handle >= MTP_HANDLE_GAMECARD_BASE &&
           handle <= (MTP_HANDLE_GAMECARD_BASE | 0x0FFF);
}

bool gcGetStorageInfo(GcContext* ctx, u32 storage_id, MtpStorageInfo* out) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_GAMECARD) return false;

    memset(out, 0, sizeof(MtpStorageInfo));
    out->storage_id       = MTP_STORAGE_GAMECARD;
    out->storage_type     = 0x0004; // Removable RAM (closest to gamecard)
    out->filesystem_type  = 0x0002; // Generic hierarchical
    out->access_capability = 0x0001; // Read-only

    mutexLock(&ctx->gc_mutex);
    bool inserted = ctx->card_inserted && ctx->layout.computed;
    if (inserted) {
        out->max_capacity = ctx->layout.total_size;
        out->free_space   = 0;
    } else {
        out->max_capacity = 0;
        out->free_space   = 0;
    }
    out->mounted = inserted;
    strncpy(out->description, "Game Cartridge", sizeof(out->description) - 1);
    strncpy(out->volume_label, "GAMECARD",       sizeof(out->volume_label) - 1);
    mutexUnlock(&ctx->gc_mutex);

    return true;
}

u32 gcGetObjectCount(GcContext* ctx, u32 storage_id, u32 parent_handle) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_GAMECARD) return 0;

    mutexLock(&ctx->gc_mutex);
    bool have_xci = ctx->layout.computed && ctx->card_inserted;
    bool have_nsp = ctx->nsp_layout.computed && ctx->card_inserted;
    mutexUnlock(&ctx->gc_mutex);

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        u32 count = 0;
        if (have_xci) count++;
        if (have_nsp) count++;
        return count;
    }
    return 0;
}

u32 gcEnumObjects(GcContext* ctx, u32 storage_id, u32 parent_handle,
                  u32* handles, u32 max) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_GAMECARD) return 0;

    mutexLock(&ctx->gc_mutex);
    bool have_xci = ctx->layout.computed && ctx->card_inserted;
    bool have_nsp = ctx->nsp_layout.computed && ctx->card_inserted;
    mutexUnlock(&ctx->gc_mutex);

    if (parent_handle != 0 && parent_handle != 0xFFFFFFFF) return 0;

    u32 count = 0;
    if (have_xci && count < max) {
        handles[count++] = MTP_HANDLE_GC_XCI_FILE;
    }
    if (have_nsp && count < max) {
        handles[count++] = MTP_HANDLE_GC_NSP_FILE;
    }
    return count;
}

bool gcGetObjectInfo(GcContext* ctx, u32 handle, MtpObject* out) {
    if (!ctx->initialized) return false;
    if (handle != MTP_HANDLE_GC_XCI_FILE && handle != MTP_HANDLE_GC_NSP_FILE) return false;

    const bool want_nsp = (handle == MTP_HANDLE_GC_NSP_FILE);

    mutexLock(&ctx->gc_mutex);

    bool have_layout;
    u64  total_size;

    if (want_nsp) {
        have_layout = ctx->nsp_layout.computed && ctx->card_inserted;
        total_size  = ctx->nsp_layout.total_size;
    } else {
        have_layout = ctx->layout.computed && ctx->card_inserted;
        total_size  = ctx->layout.total_size;
    }

    char name[256];
    if (have_layout) {
        const char* ext = want_nsp ? "nsp" : "xci";
        if (ctx->version_str[0] != '\0') {
            snprintf(name, sizeof(name), "%s [%016lX][%s].%s",
                     ctx->game_name,
                     (unsigned long)ctx->title_id,
                     ctx->version_str,
                     ext);
        } else {
            snprintf(name, sizeof(name), "%s [%016lX].%s",
                     ctx->game_name,
                     (unsigned long)ctx->title_id,
                     ext);
        }
    } else {
        snprintf(name, sizeof(name), "GameCard.%s", want_nsp ? "nsp" : "xci");
    }

    mutexUnlock(&ctx->gc_mutex);

    if (!have_layout) return false;

    memset(out, 0, sizeof(MtpObject));
    out->handle        = handle;
    out->parent_handle = 0xFFFFFFFF;
    out->storage_id    = MTP_STORAGE_GAMECARD;
    out->format        = MTP_FORMAT_UNDEFINED;
    out->object_type   = MTP_OBJECT_TYPE_FILE;
    out->size          = total_size;
    strncpy(out->filename, name, MTP_MAX_FILENAME - 1);
    out->filename[MTP_MAX_FILENAME - 1] = '\0';

    return true;
}
