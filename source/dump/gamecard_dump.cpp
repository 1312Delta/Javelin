// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "dump/gamecard_dump.h"
#include "mtp/mtp_log.h"
#include "core/GuiEvents.h"
#include "core/Event.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>

using namespace Javelin;

// ---------------------------------------------------------------------------
// XCI / HFS0 on-disk structures (packed, no padding)
// ---------------------------------------------------------------------------

typedef struct {
    u8  signature[0x100];    // RSA-2048 signature (zeroed for synthetic)
    u8  magic[4];            // "HEAD"
    u32 secure_offset;       // Offset of the secure HFS0 (relative to XCI start)
    u32 _backupAreaStartPage;
    u8  _kekIndex;
    u8  _titleKeyDecIndex;
    u8  gcSize;              // 1=1GB, 2=2GB, 4=4GB, 8=8GB, 16=16GB, 32=32GB
    u8  _gcHeaderVersion;
    u32 gcFlags;
    u64 packageId;
    u64 validDataEndPage;
    u8  _iv[0x10];
    u64 hfs0Offset;          // Offset of root HFS0 from XCI start
    u64 hfs0HeaderSize;      // Size of root HFS0 header
    u8  _rootHfs0Hash[0x20]; // SHA-256 of root HFS0 header (zeroed)
    u8  _initialDataHash[0x20];
    u32 _selSecureMode;
    u32 _selT1Key;
    u32 _selKey;
    u32 limitedArea;
    u8  _reserved[0x48];
} __attribute__((packed)) GcmHeader;

// HFS0 (Hashed File System 0) header
typedef struct {
    u32 magic;               // "HFS0"
    u32 file_count;
    u32 string_table_size;
    u32 _reserved;
} __attribute__((packed)) Hfs0Header;

// HFS0 file entry
typedef struct {
    u64 offset;              // Relative to start of data section
    u64 size;
    u32 string_offset;
    u32 hash_target_size;
    u8  hash[0x20];          // SHA-256 (zeroed for synthetic)
    u32 _reserved;
} __attribute__((packed)) Hfs0FileEntry;

// PFS0 structures for NSP layout
typedef struct {
    u8  magic[4];       // "PFS0"
    u32 file_count;
    u32 string_table_size;
    u32 _reserved;
} __attribute__((packed)) Pfs0Header;

typedef struct {
    u64 data_offset;
    u64 data_size;
    u32 string_offset;
    u32 _reserved;
} __attribute__((packed)) Pfs0FileEntry;

// ---------------------------------------------------------------------------
// Key loading helpers
// ---------------------------------------------------------------------------

static void trim_whitespace(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' '  || s[len-1] == '\t')) {
        s[--len] = '\0';
    }
    char* start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static u32 hex_to_bytes(const char* hex, u8* out, u32 max_len) {
    u32 hex_len = strlen(hex);
    if (hex_len % 2 != 0) return 0;
    u32 byte_len = hex_len / 2;
    if (byte_len > max_len) byte_len = max_len;
    for (u32 i = 0; i < byte_len; i++) {
        char b[3] = {hex[i*2], hex[i*2+1], '\0'};
        char* end = NULL;
        out[i] = (u8)strtoul(b, &end, 16);
        if (end != b + 2) return 0;
    }
    return byte_len;
}

static void load_key_file(const char* path, GcKeySet* ks) {
    ks->count = 0;
    ks->loaded = false;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOG_INFO("[GC] Key file not found: %s", path);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) && ks->count < GC_MAX_KEYS) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* name = line;
        char* value = eq + 1;
        trim_whitespace(name);
        trim_whitespace(value);

        if (name[0] == '\0' || value[0] == '\0') continue;

        GcKeyEntry* entry = &ks->keys[ks->count];
        strncpy(entry->name, name, GC_KEY_NAME_MAX - 1);
        entry->name[GC_KEY_NAME_MAX - 1] = '\0';
        entry->value_len = hex_to_bytes(value, entry->value, GC_KEY_VALUE_MAX);

        if (entry->value_len > 0) {
            ks->count++;
        }
    }

    fclose(fp);
    ks->loaded = (ks->count > 0);
    LOG_INFO("[GC] Loaded %u keys from %s", ks->count, path);
}

static void ensure_keys_checked(GcContext* ctx) {
    if (ctx->keys_checked) return;
    ctx->keys_checked = true;

    load_key_file(GC_PROD_KEYS_PATH, &ctx->prod_keys);
    load_key_file(GC_TITLE_KEYS_PATH, &ctx->title_keys);

    ctx->keys_loaded = ctx->prod_keys.loaded;

    if (!ctx->prod_keys.loaded) {
        NotificationEvent evt(
            "No prod.keys found at /switch/prod.keys - XCI dump will proceed without key data.",
            NotificationEvent::Type::Warning,
            6000
        );
        EventBus::getInstance().post(evt);
        LOG_WARN("[GC] prod.keys not found - dump will lack key info");
    }
    if (!ctx->title_keys.loaded) {
        LOG_INFO("[GC] title.keys not found (optional)");
    }
}

// ---------------------------------------------------------------------------
// Gamecard detection and filesystem helpers
// ---------------------------------------------------------------------------

static void close_gc_fs(GcContext* ctx) {
    if (ctx->gc_fs_open) {
        fsFsClose(&ctx->gc_fs);
        ctx->gc_fs_open = false;
    }
    ctx->gc_handle_valid = false;
}

static bool open_gc_fs(GcContext* ctx) {
    close_gc_fs(ctx);

    if (!ctx->dev_op_open) {
        Result rc = fsOpenDeviceOperator(&ctx->dev_op);
        if (R_FAILED(rc)) {
            LOG_ERROR("[GC] fsOpenDeviceOperator failed: 0x%08X", rc);
            return false;
        }
        ctx->dev_op_open = true;
    }

    bool inserted = false;
    Result rc = fsDeviceOperatorIsGameCardInserted(&ctx->dev_op, &inserted);
    if (R_FAILED(rc) || !inserted) {
        ctx->card_inserted = false;
        return false;
    }

    rc = fsDeviceOperatorGetGameCardHandle(&ctx->dev_op, &ctx->gc_handle);
    if (R_FAILED(rc)) {
        LOG_ERROR("[GC] GetGameCardHandle failed: 0x%08X", rc);
        ctx->card_inserted = false;
        return false;
    }
    ctx->gc_handle_valid = true;
    ctx->card_inserted = true;

    rc = fsOpenGameCardFileSystem(&ctx->gc_fs, &ctx->gc_handle,
                                  FsGameCardPartition_Secure);
    if (R_FAILED(rc)) {
        LOG_ERROR("[GC] fsOpenGameCardFileSystem(Secure) failed: 0x%08X", rc);
        ctx->gc_handle_valid = false;
        ctx->card_inserted = false;
        return false;
    }
    ctx->gc_fs_open = true;

    LOG_INFO("[GC] Gamecard secure partition opened successfully");
    return true;
}

// ---------------------------------------------------------------------------
// Virtual XCI layout construction
// ---------------------------------------------------------------------------

static void free_nsp_layout(GcContext* ctx) {
    GcVirtualNspLayout* nsp = &ctx->nsp_layout;
    if (nsp->hdr_data) {
        free(nsp->hdr_data);
        nsp->hdr_data = NULL;
    }
    nsp->hdr_size = 0;
    nsp->file_count = 0;
    nsp->total_size = 0;
    nsp->data_region_start = 0;
    nsp->computed = false;
}

static void free_layout(GcContext* ctx) {
    GcVirtualXciLayout* layout = &ctx->layout;
    if (layout->hdr_data) {
        free(layout->hdr_data);
        layout->hdr_data = NULL;
    }
    layout->hdr_size = 0;
    layout->file_count = 0;
    layout->total_size = 0;
    layout->data_region_start = 0;
    layout->computed = false;

    free_nsp_layout(ctx);
}

static inline u64 align_up_u64(u64 v, u64 a) {
    return (v + a - 1) & ~(a - 1);
}
static inline u32 align_up_u32(u32 v, u32 a) {
    return (v + a - 1) & ~(a - 1);
}

static bool enumerate_nca_files(GcContext* ctx) {
    FsDir dir;
    Result rc = fsFsOpenDirectory(&ctx->gc_fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        LOG_ERROR("[GC] fsFsOpenDirectory('/') failed: 0x%08X", rc);
        return false;
    }

    GcVirtualXciLayout* layout = &ctx->layout;
    layout->file_count = 0;

    FsDirectoryEntry entries[GC_MAX_NCA_FILES];
    s64 read_count = 0;
    rc = fsDirRead(&dir, &read_count, GC_MAX_NCA_FILES, entries);
    fsDirClose(&dir);

    if (R_FAILED(rc)) {
        LOG_ERROR("[GC] fsDirRead failed: 0x%08X", rc);
        return false;
    }

    LOG_INFO("[GC] Found %lld files in secure partition", (long long)read_count);

    u64 data_offset = 0;
    for (s64 i = 0; i < read_count && (u32)i < GC_MAX_NCA_FILES; i++) {
        if (entries[i].type != FsDirEntryType_File) continue;

        GcNcaEntry* entry = &layout->files[layout->file_count];
        strncpy(entry->name, entries[i].name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        entry->size = (u64)entries[i].file_size;
        entry->offset = data_offset;
        data_offset += align_up_u64(entry->size, 0x200);

        layout->file_count++;
        LOG_INFO("[GC]   File[%u]: %s (%llu bytes)", layout->file_count - 1,
                 entry->name, (unsigned long long)entry->size);
    }

    return true;
}

static void try_get_title_info(GcContext* ctx) {
    ctx->title_id = 0;
    ctx->game_name[0] = '\0';
    ctx->version_str[0] = '\0';

    // Use NCM content meta database to get the real title ID from the gamecard
    NcmContentMetaDatabase meta_db;
    Result rc = ncmOpenContentMetaDatabase(&meta_db, NcmStorageId_GameCard);
    if (R_SUCCEEDED(rc)) {
        NcmContentMetaKey keys[16];
        s32 total = 0, written = 0;
        rc = ncmContentMetaDatabaseList(&meta_db, &total, &written, keys, 16,
                                         NcmContentMetaType_Application, 0, 0, UINT64_MAX,
                                         NcmContentInstallType_Full);
        if (R_SUCCEEDED(rc) && written > 0) {
            ctx->title_id = keys[0].id;
            LOG_INFO("[GC] Got title ID from NCM: %016lX", (unsigned long)ctx->title_id);
        } else {
            LOG_WARN("[GC] ncmContentMetaDatabaseList failed or empty: 0x%08X (written=%d)", rc, written);
        }
        ncmContentMetaDatabaseClose(&meta_db);
    } else {
        LOG_WARN("[GC] ncmOpenContentMetaDatabase(GameCard) failed: 0x%08X", rc);
    }

    // Resolve game name and version via NACP
    if (ctx->title_id != 0) {
        u64 app_id = ctx->title_id & ~0xFFFULL;

        NsApplicationControlData* ctrl =
            (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
        if (ctrl) {
            u64 actual = 0;

            // Try Storage source first (covers installed + gamecard)
            rc = nsGetApplicationControlData(
                NsApplicationControlSource_Storage, app_id,
                ctrl, sizeof(*ctrl), &actual);

            // Fall back to CacheOnly if Storage fails (gamecard not always visible via Storage)
            if (R_FAILED(rc) || actual < sizeof(ctrl->nacp)) {
                actual = 0;
                rc = nsGetApplicationControlData(
                    NsApplicationControlSource_CacheOnly, app_id,
                    ctrl, sizeof(*ctrl), &actual);
            }

            if (R_SUCCEEDED(rc) && actual >= sizeof(ctrl->nacp)) {
                for (int k = 0; k < 16; k++) {
                    if (ctrl->nacp.lang[k].name[0]) {
                        strncpy(ctx->game_name, ctrl->nacp.lang[k].name,
                                sizeof(ctx->game_name) - 1);
                        ctx->game_name[sizeof(ctx->game_name) - 1] = '\0';
                        break;
                    }
                }
                strncpy(ctx->version_str, ctrl->nacp.display_version,
                        sizeof(ctx->version_str) - 1);
                ctx->version_str[sizeof(ctx->version_str) - 1] = '\0';
            } else {
                LOG_WARN("[GC] NACP lookup failed for %016lX: 0x%08X",
                         (unsigned long)app_id, rc);
            }
            free(ctrl);
        }
    }

    // Fallback display name
    if (ctx->game_name[0] == '\0') {
        if (ctx->title_id != 0) {
            snprintf(ctx->game_name, sizeof(ctx->game_name),
                     "%016lX", (unsigned long)ctx->title_id);
        } else {
            strncpy(ctx->game_name, "GameCard", sizeof(ctx->game_name) - 1);
        }
    }

    LOG_INFO("[GC] Title: '%s' (ID: %016lX, ver: %s)",
             ctx->game_name, (unsigned long)ctx->title_id, ctx->version_str);
}

static bool build_layout(GcContext* ctx) {
    free_layout(ctx);

    if (!ctx->gc_fs_open) {
        if (!open_gc_fs(ctx)) {
            LOG_ERROR("[GC] Cannot open gamecard FS");
            return false;
        }
    }

    if (!enumerate_nca_files(ctx)) {
        return false;
    }

    try_get_title_info(ctx);

    GcVirtualXciLayout* layout = &ctx->layout;

    if (layout->file_count == 0) {
        LOG_WARN("[GC] No NCA files found in secure partition");
    }

    // Build secure partition HFS0 header
    u32 sec_string_table_size = 0;
    for (u32 i = 0; i < layout->file_count; i++) {
        sec_string_table_size += (u32)strlen(layout->files[i].name) + 1;
    }
    u32 sec_string_table_padded = align_up_u32(sec_string_table_size, 4);

    u32 sec_hdr_size = (u32)sizeof(Hfs0Header)
                     + layout->file_count * (u32)sizeof(Hfs0FileEntry)
                     + sec_string_table_padded;
    sec_hdr_size = align_up_u32(sec_hdr_size, 0x200);

    // Build root HFS0 header (one entry: "secure")
    const char* secure_name = "secure";
    u32 root_string_table_padded = align_up_u32((u32)strlen(secure_name) + 1, 4);
    u32 root_hdr_size = (u32)sizeof(Hfs0Header)
                      + 1 * (u32)sizeof(Hfs0FileEntry)
                      + root_string_table_padded;
    root_hdr_size = align_up_u32(root_hdr_size, 0x200);

    // Fixed layout positions
    const u64 GCM_HDR_START  = 0x200;
    const u64 ROOT_HFS0_OFF  = 0x10000;

    u64 secure_start = ROOT_HFS0_OFF + (u64)root_hdr_size;
    u64 nca_data_start = secure_start + (u64)sec_hdr_size;
    u64 header_region_size = nca_data_start;

    u64 nca_data_size = 0;
    for (u32 i = 0; i < layout->file_count; i++) {
        nca_data_size += align_up_u64(layout->files[i].size, 0x200);
    }

    layout->data_region_start = nca_data_start;
    layout->total_size        = nca_data_start + nca_data_size;

    layout->hdr_data = (u8*)calloc(1, header_region_size);
    if (!layout->hdr_data) {
        LOG_ERROR("[GC] OOM allocating XCI header region (%llu bytes)",
                  (unsigned long long)header_region_size);
        return false;
    }
    layout->hdr_size = (u32)header_region_size;

    // GCM header at offset 0x200
    GcmHeader* gcm = (GcmHeader*)(layout->hdr_data + GCM_HDR_START);
    memcpy(gcm->magic, "HEAD", 4);
    gcm->hfs0Offset     = ROOT_HFS0_OFF;
    gcm->hfs0HeaderSize = root_hdr_size;
    gcm->packageId      = ctx->title_id;
    {
        u64 sz = layout->total_size;
        if      (sz <= 1ULL*1024*1024*1024)  gcm->gcSize = 1;
        else if (sz <= 2ULL*1024*1024*1024)  gcm->gcSize = 2;
        else if (sz <= 4ULL*1024*1024*1024)  gcm->gcSize = 4;
        else if (sz <= 8ULL*1024*1024*1024)  gcm->gcSize = 8;
        else if (sz <= 16ULL*1024*1024*1024) gcm->gcSize = 16;
        else                                  gcm->gcSize = 32;
    }

    // Root HFS0 at offset 0x10000
    u8* root_region = layout->hdr_data + ROOT_HFS0_OFF;
    Hfs0Header* root_hfs0 = (Hfs0Header*)root_region;
    root_hfs0->magic             = 0x30534648; // "HFS0"
    root_hfs0->file_count        = 1;
    root_hfs0->string_table_size = root_string_table_padded;
    root_hfs0->_reserved         = 0;

    Hfs0FileEntry* root_entry =
        (Hfs0FileEntry*)(root_region + sizeof(Hfs0Header));
    root_entry->offset           = 0;
    root_entry->size             = (u64)sec_hdr_size + nca_data_size;
    root_entry->string_offset    = 0;
    root_entry->hash_target_size = sec_hdr_size;
    memset(root_entry->hash, 0, 0x20);
    root_entry->_reserved        = 0;

    char* root_strings = (char*)(root_region + sizeof(Hfs0Header)
                                 + sizeof(Hfs0FileEntry));
    strcpy(root_strings, secure_name);

    // Secure HFS0 at secure_start
    u8* sec_region = layout->hdr_data + secure_start;
    Hfs0Header* sec_hfs0 = (Hfs0Header*)sec_region;
    sec_hfs0->magic             = 0x30534648; // "HFS0"
    sec_hfs0->file_count        = layout->file_count;
    sec_hfs0->string_table_size = sec_string_table_padded;
    sec_hfs0->_reserved         = 0;

    Hfs0FileEntry* sec_entries =
        (Hfs0FileEntry*)(sec_region + sizeof(Hfs0Header));
    char* sec_strings = (char*)(sec_region + sizeof(Hfs0Header)
                                + layout->file_count * sizeof(Hfs0FileEntry));

    u32 str_off = 0;
    for (u32 i = 0; i < layout->file_count; i++) {
        sec_entries[i].offset           = layout->files[i].offset;
        sec_entries[i].size             = layout->files[i].size;
        sec_entries[i].string_offset    = str_off;
        sec_entries[i].hash_target_size = (u32)((layout->files[i].size > 0x200)
                                            ? 0x200 : layout->files[i].size);
        memset(sec_entries[i].hash, 0, 0x20);
        sec_entries[i]._reserved        = 0;

        u32 name_len = (u32)strlen(layout->files[i].name) + 1;
        memcpy(sec_strings + str_off, layout->files[i].name, name_len);
        str_off += name_len;
    }

    layout->computed = true;

    LOG_INFO("[GC] XCI layout built: %u NCAs, total size = %llu MB",
             layout->file_count,
             (unsigned long long)(layout->total_size / (1024*1024)));

    return true;
}

static bool build_nsp_layout(GcContext* ctx) {
    free_nsp_layout(ctx);

    GcVirtualXciLayout* xci = &ctx->layout;
    if (!xci->computed || xci->file_count == 0) {
        return false;
    }

    GcVirtualNspLayout* nsp = &ctx->nsp_layout;
    u32 file_count = xci->file_count;

    u32 string_table_size = 0;
    for (u32 i = 0; i < file_count; i++) {
        string_table_size += (u32)strlen(xci->files[i].name) + 1;
    }
    // Pad string table to 0x20 alignment (must match header allocation)
    u32 padded_string_table_size = (string_table_size + 0x1F) & ~0x1Fu;

    u32 hdr_size = (u32)sizeof(Pfs0Header)
                 + file_count * (u32)sizeof(Pfs0FileEntry)
                 + padded_string_table_size;

    nsp->file_count = file_count;
    u64 data_offset = 0;
    for (u32 i = 0; i < file_count; i++) {
        nsp->files[i] = xci->files[i];
        nsp->files[i].offset = data_offset;
        data_offset += align_up_u64(xci->files[i].size, 0x200);
    }

    nsp->data_region_start = hdr_size;
    nsp->total_size        = hdr_size + data_offset;

    nsp->hdr_data = (u8*)calloc(1, hdr_size);
    if (!nsp->hdr_data) {
        LOG_ERROR("[GC] OOM allocating NSP header (%u bytes)", hdr_size);
        return false;
    }
    nsp->hdr_size = hdr_size;

    Pfs0Header* phdr = (Pfs0Header*)nsp->hdr_data;
    memcpy(phdr->magic, "PFS0", 4);
    phdr->file_count        = file_count;
    phdr->string_table_size = padded_string_table_size;
    phdr->_reserved         = 0;

    Pfs0FileEntry* entries =
        (Pfs0FileEntry*)(nsp->hdr_data + sizeof(Pfs0Header));
    char* strings =
        (char*)(nsp->hdr_data + sizeof(Pfs0Header) + file_count * sizeof(Pfs0FileEntry));

    u32 str_off_nsp = 0;
    for (u32 i = 0; i < file_count; i++) {
        entries[i].data_offset  = nsp->files[i].offset;
        entries[i].data_size    = nsp->files[i].size;
        entries[i].string_offset = str_off_nsp;
        entries[i]._reserved    = 0;

        u32 name_len = (u32)strlen(xci->files[i].name) + 1;
        memcpy(strings + str_off_nsp, xci->files[i].name, name_len);
        str_off_nsp += name_len;
    }

    nsp->computed = true;

    LOG_INFO("[GC] NSP layout built: %u NCAs, total size = %llu MB",
             file_count,
             (unsigned long long)(nsp->total_size / (1024 * 1024)));

    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result gcInit(GcContext* ctx) {
    if (ctx->initialized) return 0;

    memset(ctx, 0, sizeof(GcContext));
    mutexInit(&ctx->gc_mutex);
    ctx->needs_rescan = true;
    ctx->initialized = true;

    return 0;
}

void gcExit(GcContext* ctx) {
    if (!ctx->initialized) return;

    mutexLock(&ctx->gc_mutex);

    free_layout(ctx);
    close_gc_fs(ctx);

    if (ctx->dev_op_open) {
        fsDeviceOperatorClose(&ctx->dev_op);
        ctx->dev_op_open = false;
    }

    mutexUnlock(&ctx->gc_mutex);
    ctx->initialized = false;
}

bool gcRefreshCardState(GcContext* ctx) {
    if (!ctx->initialized) return false;
    if (!ctx->dev_op_open) return false;

    bool inserted = false;
    Result rc = fsDeviceOperatorIsGameCardInserted(&ctx->dev_op, &inserted);
    if (R_FAILED(rc)) return false;

    bool changed = (inserted != ctx->card_inserted);
    if (changed) {
        LOG_INFO("[GC] Card insertion state changed: %s", inserted ? "inserted" : "removed");

        mutexLock(&ctx->gc_mutex);
        free_layout(ctx);
        close_gc_fs(ctx);
        ctx->card_inserted = inserted;
        ctx->needs_rescan = true;
        mutexUnlock(&ctx->gc_mutex);

        if (inserted) {
            NotificationEvent evt("Gamecard inserted.",
                                  NotificationEvent::Type::Info, 3000);
            EventBus::getInstance().post(evt);
        } else {
            NotificationEvent evt("Gamecard removed.",
                                  NotificationEvent::Type::Warning, 3000);
            EventBus::getInstance().post(evt);
        }
    }
    return changed;
}

void gcPreInitServices(GcContext* ctx) {
    if (!ctx->initialized) {
        memset(ctx, 0, sizeof(GcContext));
        mutexInit(&ctx->gc_mutex);
        ctx->needs_rescan = true;
        ctx->initialized = true;
    }

    if (!ctx->dev_op_open) {
        Result rc = fsOpenDeviceOperator(&ctx->dev_op);
        if (R_SUCCEEDED(rc)) {
            ctx->dev_op_open = true;
            LOG_INFO("[GC] Device operator opened");
        } else {
            LOG_WARN("[GC] Failed to open device operator: 0x%08X", rc);
        }
    }

    ensure_keys_checked(ctx);
}

void gcRefreshIfNeeded(GcContext* ctx) {
    if (!ctx->initialized) return;

    if (ctx->dev_op_open) {
        gcRefreshCardState(ctx);
    }

    mutexLock(&ctx->gc_mutex);
    bool needs = ctx->needs_rescan && ctx->card_inserted && !ctx->layout.computed;
    mutexUnlock(&ctx->gc_mutex);

    if (needs) {
        mutexLock(&ctx->gc_mutex);
        ctx->needs_rescan = false;
        mutexUnlock(&ctx->gc_mutex);

        LOG_INFO("[GC] Building XCI layout...");
        if (build_layout(ctx)) {
            LOG_INFO("[GC] XCI layout ready: '%s', %llu bytes",
                     ctx->game_name,
                     (unsigned long long)ctx->layout.total_size);

            LOG_INFO("[GC] Building NSP layout...");
            if (build_nsp_layout(ctx)) {
                LOG_INFO("[GC] NSP layout ready: %llu bytes",
                         (unsigned long long)ctx->nsp_layout.total_size);
            } else {
                LOG_WARN("[GC] Failed to build NSP layout (XCI still available)");
            }

            NotificationEvent evt(
                std::string("Gamecard ready: ") + ctx->game_name,
                NotificationEvent::Type::Success, 4000);
            EventBus::getInstance().post(evt);
        } else {
            LOG_ERROR("[GC] Failed to build XCI layout");
        }
    }
}

s64 gcReadObject(GcContext* ctx, u32 handle, u64 offset, void* buffer, u64 size) {
    if (!ctx->initialized) return -1;
    if (handle != MTP_HANDLE_GC_XCI_FILE && handle != MTP_HANDLE_GC_NSP_FILE) return -1;

    const bool is_nsp = (handle == MTP_HANDLE_GC_NSP_FILE);

    mutexLock(&ctx->gc_mutex);

    bool layout_ok = is_nsp
        ? (ctx->nsp_layout.computed && ctx->card_inserted)
        : (ctx->layout.computed     && ctx->card_inserted);

    if (!layout_ok) {
        mutexUnlock(&ctx->gc_mutex);
        return -1;
    }

    u64 total = is_nsp ? ctx->nsp_layout.total_size : ctx->layout.total_size;
    if (offset >= total) {
        mutexUnlock(&ctx->gc_mutex);
        return 0;
    }
    if (offset + size > total) {
        size = total - offset;
    }

    u64 hdr_size        = is_nsp ? ctx->nsp_layout.hdr_size        : ctx->layout.hdr_size;
    u64 data_region_off = is_nsp ? ctx->nsp_layout.data_region_start : ctx->layout.data_region_start;
    u32 file_count      = is_nsp ? ctx->nsp_layout.file_count       : ctx->layout.file_count;

    GcNcaEntry files_snap[GC_MAX_NCA_FILES];
    memcpy(files_snap,
           is_nsp ? ctx->nsp_layout.files : ctx->layout.files,
           file_count * sizeof(GcNcaEntry));

    mutexUnlock(&ctx->gc_mutex);

    u8* out = (u8*)buffer;
    u64 bytes_done = 0;

    while (bytes_done < size) {
        u64 cur = offset + bytes_done;
        u64 rem = size - bytes_done;

        if (cur < hdr_size) {
            u64 avail = hdr_size - cur;
            u64 to_copy = (rem < avail) ? rem : avail;

            mutexLock(&ctx->gc_mutex);
            u8* hdr_ptr = is_nsp ? ctx->nsp_layout.hdr_data : ctx->layout.hdr_data;
            if (hdr_ptr) {
                memcpy(out + bytes_done, hdr_ptr + cur, to_copy);
            } else {
                memset(out + bytes_done, 0, to_copy);
            }
            mutexUnlock(&ctx->gc_mutex);

            bytes_done += to_copy;
            continue;
        }

        if (cur < data_region_off) {
            u64 avail = data_region_off - cur;
            u64 to_zero = (rem < avail) ? rem : avail;
            memset(out + bytes_done, 0, to_zero);
            bytes_done += to_zero;
            continue;
        }

        u64 nca_region_offset = cur - data_region_off;

        bool found = false;
        for (u32 i = 0; i < file_count; i++) {
            u64 file_start = files_snap[i].offset;
            u64 file_size  = files_snap[i].size;
            u64 file_end   = file_start + align_up_u64(file_size, 0x200);

            if (nca_region_offset >= file_start && nca_region_offset < file_end) {
                u64 file_local_off = nca_region_offset - file_start;
                u64 data_avail;
                u64 to_read;

                if (file_local_off >= file_size) {
                    data_avail = align_up_u64(file_size, 0x200) - file_local_off;
                    to_read    = (rem < data_avail) ? rem : data_avail;
                    memset(out + bytes_done, 0, to_read);
                    bytes_done += to_read;
                    found = true;
                    break;
                }

                data_avail = file_size - file_local_off;
                u64 align_remain = align_up_u64(file_size, 0x200) - file_local_off;
                if (data_avail > align_remain) data_avail = align_remain;
                to_read = (rem < data_avail) ? rem : data_avail;

                FsFile file_handle;
                char nca_path[260];
                snprintf(nca_path, sizeof(nca_path), "/%s", files_snap[i].name);
                Result rc = fsFsOpenFile(&ctx->gc_fs, nca_path,
                                         FsOpenMode_Read, &file_handle);
                if (R_FAILED(rc)) {
                    LOG_ERROR("[GC] fsFsOpenFile('%s') failed: 0x%08X",
                              files_snap[i].name, rc);
                    memset(out + bytes_done, 0, to_read);
                    bytes_done += to_read;
                    found = true;
                    break;
                }

                u64 actually_read = 0;
                rc = fsFileRead(&file_handle, (s64)file_local_off,
                                out + bytes_done, to_read,
                                FsReadOption_None, &actually_read);
                fsFileClose(&file_handle);

                if (R_FAILED(rc) || actually_read == 0) {
                    LOG_ERROR("[GC] fsFileRead failed: 0x%08X (read %llu of %llu)",
                              rc, (unsigned long long)actually_read,
                              (unsigned long long)to_read);
                    memset(out + bytes_done, 0, to_read);
                    bytes_done += to_read;
                    found = true;
                    break;
                }

                if (actually_read < to_read) {
                    memset(out + bytes_done + actually_read, 0,
                           to_read - actually_read);
                }
                bytes_done += to_read;
                found = true;
                break;
            }
        }

        if (!found) {
            u64 to_zero = (rem < 0x200) ? rem : 0x200;
            memset(out + bytes_done, 0, to_zero);
            bytes_done += to_zero;
        }
    }

    return (s64)bytes_done;
}
