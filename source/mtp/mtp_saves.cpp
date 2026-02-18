// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_saves.h"
#include "mtp/mtp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define SAVES_MOUNT_PREFIX "sv"

typedef struct {
    u64 application_id;
    AccountUid uid;
    u8 save_data_type;
    u8 space_id;
    u16 save_data_index;
} SaveInfoEntry;

#define MAX_SAVE_INFO_ENTRIES 4096
static SaveInfoEntry s_save_info[MAX_SAVE_INFO_ENTRIES];
static u32 s_save_info_count = 0;

static void start_background_refresh(SavesContext* ctx);
static void stop_background_refresh(void);
static void do_refresh_internal(SavesContext* ctx);

static bool is_game_handle(u32 handle) {
    return handle >= MTP_HANDLE_SAVES_GAME_START && handle <= MTP_HANDLE_SAVES_GAME_END;
}

static bool is_user_handle(u32 handle) {
    return handle >= MTP_HANDLE_SAVES_USER_START && handle <= MTP_HANDLE_SAVES_USER_END;
}

static bool is_type_handle(u32 handle) {
    return handle >= MTP_HANDLE_SAVES_TYPE_START && handle <= MTP_HANDLE_SAVES_TYPE_END;
}

static bool is_file_handle(u32 handle) {
    return handle >= MTP_HANDLE_SAVES_FILE_START;
}

static u32 get_game_index(u32 handle) {
    if (!is_game_handle(handle)) return 0xFFFFFFFF;
    return handle - MTP_HANDLE_SAVES_GAME_START;
}

static u32 make_game_handle(u32 index) {
    return MTP_HANDLE_SAVES_GAME_START + index;
}

typedef struct {
    u64 app_id;
    char name[256];
    bool is_installed;
    bool fetched;
} NameCacheEntry;

#define NAME_CACHE_SIZE 256
static NameCacheEntry s_name_cache[NAME_CACHE_SIZE];
static u32 s_name_cache_count = 0;
static Mutex s_name_cache_mutex = {0};  // Zero-initialized is valid for libnx Mutex

static NameCacheEntry* get_name_cache_entry(u64 app_id) {
    mutexLock(&s_name_cache_mutex);

    for (u32 i = 0; i < s_name_cache_count; i++) {
        if (s_name_cache[i].app_id == app_id) {
            mutexUnlock(&s_name_cache_mutex);
            return &s_name_cache[i];
        }
    }

    if (s_name_cache_count < NAME_CACHE_SIZE) {
        NameCacheEntry* entry = &s_name_cache[s_name_cache_count++];
        entry->app_id = app_id;
        entry->fetched = false;
        entry->is_installed = false;
        snprintf(entry->name, sizeof(entry->name), "%016lX", app_id);
        mutexUnlock(&s_name_cache_mutex);
        return entry;
    }

    mutexUnlock(&s_name_cache_mutex);
    return NULL;
}

static void fetch_name_for_entry(NameCacheEntry* entry) {
    if (!entry || entry->fetched) return;

    NsApplicationControlData ctrl;
    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, entry->app_id, &ctrl, sizeof(ctrl), &actual);

    if (R_SUCCEEDED(rc) && actual >= sizeof(ctrl.nacp)) {
        entry->is_installed = true;
        for (int i = 0; i < 16; i++) {
            if (ctrl.nacp.lang[i].name[0]) {
                mutexLock(&s_name_cache_mutex);
                strncpy(entry->name, ctrl.nacp.lang[i].name, sizeof(entry->name) - 1);
                entry->name[sizeof(entry->name) - 1] = '\0';
                mutexUnlock(&s_name_cache_mutex);
                break;
            }
        }
    }
    entry->fetched = true;
}

static u64 get_base_title_id(u64 app_id) {
    u64 type = (app_id >> 48) & 0xFFFF;
    u64 base = app_id & 0xFFFFFFFFFFFFULL;
    if (type == 0x0101 || type == 0x0102) {
        return 0x0100ULL << 48 | base;
    }
    return app_id;
}

static const char* get_title_type_string(u64 app_id) {
    u64 type = (app_id >> 48) & 0xFFFF;
    switch (type) {
        case 0x0100: return "App";
        case 0x0101: return "DLC";
        case 0x0102: return "Update";
        default: return "Unknown";
    }
}

static bool check_installation_status(u64 app_id, char* name_out, size_t name_size) {
    NameCacheEntry* entry = get_name_cache_entry(app_id);
    if (!entry) {
        snprintf(name_out, name_size, "%016lX", app_id);
        return false;
    }

    if (!entry->fetched) {
        fetch_name_for_entry(entry);
    }

    u64 type = (app_id >> 48) & 0xFFFF;
    if (type == 0x0101 || type == 0x0102) {
        u64 base_id = get_base_title_id(app_id);

        NameCacheEntry* base_entry = get_name_cache_entry(base_id);
        if (base_entry && !base_entry->fetched) {
            fetch_name_for_entry(base_entry);
        }

        if (base_entry && base_entry->fetched && base_entry->name[0]) {
            if (type == 0x0101) {
                u32 dlc_num = app_id & 0xFFF;
                if (dlc_num > 0) {
                    snprintf(name_out, name_size, "%s [DLC %u]", base_entry->name, dlc_num);
                } else {
                    snprintf(name_out, name_size, "%s [DLC]", base_entry->name);
                }
            } else {
                snprintf(name_out, name_size, "%s [Update]", base_entry->name);
            }
            name_out[name_size - 1] = '\0';
            return true;
        }
    }

    strncpy(name_out, entry->name, name_size - 1);
    name_out[name_size - 1] = '\0';
    return entry->is_installed;
}

static bool enumerate_all_users(SavesContext* ctx) {
    ctx->user_count = 0;
    memset(ctx->user_uids, 0, sizeof(ctx->user_uids));
    memset(ctx->user_names, 0, sizeof(ctx->user_names));

    s32 count = 0;
    Result rc = accountListAllUsers(ctx->user_uids, MTP_SAVES_MAX_USERS, &count);
    if (R_FAILED(rc)) {
        LOG_DEBUG("Saves: accountListAllUsers failed: 0x%08X", rc);
        return false;
    }

    ctx->user_count = count;

    for (s32 i = 0; i < count; i++) {
        AccountProfile profile;
        rc = accountGetProfile(&profile, ctx->user_uids[i]);
        if (R_SUCCEEDED(rc)) {
            AccountProfileBase base;
            AccountUserData data;
            rc = accountProfileGet(&profile, &data, &base);
            if (R_SUCCEEDED(rc) && base.nickname[0]) {
                strncpy(ctx->user_names[i], base.nickname, 31);
                ctx->user_names[i][31] = '\0';
            }
            accountProfileClose(&profile);
        }

        if (ctx->user_names[i][0] == '\0') {
            snprintf(ctx->user_names[i], 32, "%016lX", ctx->user_uids[i].uid[0]);
        }
    }

    return count > 0;
}

static s32 find_user_index(SavesContext* ctx, AccountUid uid) {
    for (s32 i = 0; i < ctx->user_count; i++) {
        if (ctx->user_uids[i].uid[0] == uid.uid[0] && ctx->user_uids[i].uid[1] == uid.uid[1]) {
            return i;
        }
    }
    return -1;
}

static bool has_save_info(u64 app_id, AccountUid uid, u8 save_type, u8 space_id, u16 save_idx) {
    for (u32 i = 0; i < s_save_info_count; i++) {
        SaveInfoEntry* e = &s_save_info[i];
        if (e->application_id == app_id &&
            e->save_data_type == save_type &&
            e->space_id == space_id) {
            if (save_type == FsSaveDataType_Account || save_type == FsSaveDataType_Cache) {
                if (e->uid.uid[0] == uid.uid[0] && e->uid.uid[1] == uid.uid[1]) {
                    if (save_type == FsSaveDataType_Cache) {
                        if (e->save_data_index == save_idx) return true;
                    } else {
                        return true;
                    }
                }
            } else {
                if (save_type == FsSaveDataType_Cache) {
                    if (e->save_data_index == save_idx) return true;
                } else {
                    return true;
                }
            }
        }
    }
    return false;
}

static GameSaveEntry* find_or_create_game_fast(SavesContext* ctx, u64 app_id) {
    for (u32 i = 0; i < ctx->game_count; i++) {
        if (ctx->games[i].application_id == app_id) {
            return &ctx->games[i];
        }
    }

    if (ctx->game_count >= ctx->max_games) return NULL;

    GameSaveEntry* g = &ctx->games[ctx->game_count];
    memset(g, 0, sizeof(GameSaveEntry));

    g->application_id = app_id;
    g->game_index = ctx->game_count;
    g->folder_handle = make_game_handle(ctx->game_count);
    g->is_installed = check_installation_status(app_id, g->game_name, sizeof(g->game_name));

    ctx->game_count++;
    return g;
}

static SaveTypeEntry* find_type_by_handle(SavesContext* ctx, u32 handle) {
    for (u32 i = 0; i < ctx->type_count; i++) {
        if (ctx->types[i].handle == handle) return &ctx->types[i];
    }
    return NULL;
}

static UserFolderEntry* find_user_folder_by_handle(SavesContext* ctx, u32 handle) {
    for (u32 i = 0; i < ctx->user_folder_count; i++) {
        if (ctx->user_folders[i].handle == handle) return &ctx->user_folders[i];
    }
    return NULL;
}

static SaveFileEntry* find_file_by_handle(SavesContext* ctx, u32 handle) {
    for (u32 i = 0; i < ctx->file_count; i++) {
        if (ctx->files[i].handle == handle) return &ctx->files[i];
    }
    return NULL;
}

static bool mount_save_type(SavesContext* ctx, SaveTypeEntry* type) {
    if (type->mounted) {
        LOG_DEBUG("[SAVES_MOUNT] Type 0x%08X already mounted", type->handle);
        return true;
    }

    LOG_DEBUG("[SAVES_MOUNT] Mounting type 0x%08X (game_idx=%u, save_type=%u)",
              type->handle, type->game_index, type->save_type);


    GameSaveEntry* game = &ctx->games[type->game_index];

    FsFileSystem fs;
    FsSaveDataAttribute attr = {0};
    attr.application_id = game->application_id;

    if (type->user_index >= 0 && type->user_index < ctx->user_count) {
        attr.uid = ctx->user_uids[type->user_index];
    }

    attr.save_data_type = (FsSaveDataType)type->save_type;
    if (type->save_type == FsSaveDataType_Cache) {
        attr.save_data_index = type->cache_index >= 0 ? type->cache_index : 0;
    }

    FsSaveDataSpaceId space_id = (FsSaveDataSpaceId)type->space_id;
    Result rc = fsOpenSaveDataFileSystem(&fs, space_id, &attr);
    if (R_FAILED(rc)) {
        LOG_DEBUG("[SAVES_MOUNT] Failed to open save data filesystem: 0x%08X", rc);
        return false;
    }

    char mount[32];
    snprintf(mount, sizeof(mount), "%s%u", SAVES_MOUNT_PREFIX, type->handle & 0xFFFF);

    if (fsdevMountDevice(mount, fs) < 0) {
        LOG_DEBUG("[SAVES_MOUNT] fsdevMountDevice failed for '%s'", mount);
        fsFsClose(&fs);
        return false;
    }

    type->save_fs = fs;
    type->mounted = true;
    strncpy(type->mount_name, mount, sizeof(type->mount_name) - 1);
    LOG_DEBUG("[SAVES_MOUNT] Successfully mounted type 0x%08X as '%s'", type->handle, mount);
    return true;
}

static void unmount_save_type(SaveTypeEntry* type) {
    if (!type->mounted) return;
    LOG_DEBUG("[SAVES_UNMOUNT] Unmounting type 0x%08X ('%s')", type->handle, type->mount_name);
    fsdevUnmountDevice(type->mount_name);
    type->mounted = false;
}

static void scan_directory(SavesContext* ctx, u32 type_idx, u32 parent_handle, const char* path) {
    if (ctx->file_count >= ctx->max_files) {
        LOG_DEBUG("[SAVES_SCAN] Max files reached (%u)", ctx->max_files);
        return;
    }

    LOG_DEBUG("[SAVES_SCAN] Scanning directory '%s' (parent=0x%08X, type_idx=%u)", path, parent_handle, type_idx);

    DIR* dir = opendir(path);
    if (!dir) {
        LOG_DEBUG("[SAVES_SCAN] Failed to open directory '%s'", path);
        return;
    }

    u32 start_count = ctx->file_count;
    struct dirent* ent;
    while ((ent = readdir(dir)) && ctx->file_count < ctx->max_files) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        SaveFileEntry* f = &ctx->files[ctx->file_count];
        memset(f, 0, sizeof(SaveFileEntry));

        strncpy(f->filename, ent->d_name, sizeof(f->filename) - 1);
        snprintf(f->full_path, sizeof(f->full_path), "%s/%s", path, ent->d_name);

        char* ds = strstr(f->full_path, "//");
        if (ds) memmove(ds, ds + 1, strlen(ds));

        f->handle = ctx->next_file_handle++;
        f->parent_handle = parent_handle;
        f->game_index = ctx->types[type_idx].game_index;
        f->type_index = type_idx;

        struct stat st;
        if (stat(f->full_path, &st) == 0) {
            f->is_directory = S_ISDIR(st.st_mode);
            if (!f->is_directory) f->size = st.st_size;
        } else {
            f->is_directory = (ent->d_type == DT_DIR);
        }

        ctx->file_count++;
    }
    closedir(dir);
    LOG_DEBUG("[SAVES_SCAN] Found %u entries in '%s' (total now: %u)",
              ctx->file_count - start_count, path, ctx->file_count);
}

static void build_game_structure(SavesContext* ctx, u32 game_idx) {
    if (game_idx >= ctx->game_count) {
        LOG_DEBUG("[SAVES_BUILD_STRUCT] Invalid game_idx %u (count=%u)", game_idx, ctx->game_count);
        return;
    }
    GameSaveEntry* game = &ctx->games[game_idx];
    if (game->users_scanned) {
        LOG_DEBUG("[SAVES_BUILD_STRUCT] Game %u (%016lX) already scanned", game_idx, game->application_id);
        return;
    }

    LOG_DEBUG("[SAVES_BUILD_STRUCT] Building structure for game %u (%016lX)", game_idx, game->application_id);

    AccountUid empty_uid = {0};
    u32 initial_user_folders = ctx->user_folder_count;
    u32 initial_types = ctx->type_count;

    for (s32 ui = 0; ui < ctx->user_count && ctx->user_folder_count < ctx->max_user_folders; ui++) {
        bool user_has_saves = false;

        for (u32 i = 0; i < s_save_info_count && !user_has_saves; i++) {
            SaveInfoEntry* e = &s_save_info[i];
            if (e->application_id == game->application_id &&
                e->uid.uid[0] == ctx->user_uids[ui].uid[0] &&
                e->uid.uid[1] == ctx->user_uids[ui].uid[1] &&
                (e->save_data_type == FsSaveDataType_Account || e->save_data_type == FsSaveDataType_Cache)) {
                user_has_saves = true;
            }
        }

        if (user_has_saves) {
            LOG_DEBUG("[SAVES_BUILD_STRUCT] User %d has saves for this game", ui);
            UserFolderEntry* uf = &ctx->user_folders[ctx->user_folder_count];
            memset(uf, 0, sizeof(UserFolderEntry));

            uf->uid = ctx->user_uids[ui];
            strncpy(uf->username, ctx->user_names[ui], sizeof(uf->username) - 1);
            uf->handle = ctx->next_user_handle++;
            uf->parent_game_handle = game->folder_handle;
            uf->game_index = game_idx;
            uf->user_index = ui;

            ctx->user_folder_count++;

            if (has_save_info(game->application_id, ctx->user_uids[ui], FsSaveDataType_Account, FsSaveDataSpaceId_User, 0)) {
                if (ctx->type_count < ctx->max_types) {
                    SaveTypeEntry* t = &ctx->types[ctx->type_count];
                    memset(t, 0, sizeof(SaveTypeEntry));
                    t->save_type = FsSaveDataType_Account;
                    t->space_id = FsSaveDataSpaceId_User;
                    t->handle = ctx->next_type_handle++;
                    t->parent_handle = uf->handle;
                    t->game_index = game_idx;
                    t->user_index = ui;
                    t->cache_index = -1;
                    strncpy(t->name, "Account", sizeof(t->name) - 1);
                    ctx->type_count++;
                    LOG_DEBUG("[SAVES_BUILD_STRUCT] Added Account save type");
                }
            }

            for (u16 idx = 0; idx < 16 && ctx->type_count < ctx->max_types; idx++) {
                if (has_save_info(game->application_id, ctx->user_uids[ui], FsSaveDataType_Cache, FsSaveDataSpaceId_User, idx)) {
                    SaveTypeEntry* t = &ctx->types[ctx->type_count];
                    memset(t, 0, sizeof(SaveTypeEntry));
                    t->save_type = FsSaveDataType_Cache;
                    t->space_id = FsSaveDataSpaceId_User;
                    t->handle = ctx->next_type_handle++;
                    t->parent_handle = uf->handle;
                    t->game_index = game_idx;
                    t->user_index = ui;
                    t->cache_index = idx;
                    snprintf(t->name, sizeof(t->name), "Cache.%04d", idx);
                    ctx->type_count++;
                }
            }
        }
    }

    if (has_save_info(game->application_id, empty_uid, FsSaveDataType_Device, FsSaveDataSpaceId_User, 0)) {
        if (ctx->type_count < ctx->max_types) {
            SaveTypeEntry* t = &ctx->types[ctx->type_count];
            memset(t, 0, sizeof(SaveTypeEntry));
            t->save_type = FsSaveDataType_Device;
            t->space_id = FsSaveDataSpaceId_User;
            t->handle = ctx->next_type_handle++;
            t->parent_handle = game->folder_handle;
            t->game_index = game_idx;
            t->user_index = -1;
            t->cache_index = -1;
            strncpy(t->name, "Device", sizeof(t->name) - 1);
            ctx->type_count++;
            LOG_DEBUG("[SAVES_BUILD_STRUCT] Added Device save type");
        }
    }

    if (has_save_info(game->application_id, empty_uid, FsSaveDataType_Bcat, FsSaveDataSpaceId_User, 0)) {
        if (ctx->type_count < ctx->max_types) {
            SaveTypeEntry* t = &ctx->types[ctx->type_count];
            memset(t, 0, sizeof(SaveTypeEntry));
            t->save_type = FsSaveDataType_Bcat;
            t->space_id = FsSaveDataSpaceId_User;
            t->handle = ctx->next_type_handle++;
            t->parent_handle = game->folder_handle;
            t->game_index = game_idx;
            t->user_index = -1;
            t->cache_index = -1;
            strncpy(t->name, "BCAT", sizeof(t->name) - 1);
            ctx->type_count++;
            LOG_DEBUG("[SAVES_BUILD_STRUCT] Added BCAT save type");
        }
    }

    if (has_save_info(game->application_id, empty_uid, FsSaveDataType_Temporary, FsSaveDataSpaceId_User, 0)) {
        if (ctx->type_count < ctx->max_types) {
            SaveTypeEntry* t = &ctx->types[ctx->type_count];
            memset(t, 0, sizeof(SaveTypeEntry));
            t->save_type = FsSaveDataType_Temporary;
            t->space_id = FsSaveDataSpaceId_User;
            t->handle = ctx->next_type_handle++;
            t->parent_handle = game->folder_handle;
            t->game_index = game_idx;
            t->user_index = -1;
            t->cache_index = -1;
            strncpy(t->name, "Temporary", sizeof(t->name) - 1);
            ctx->type_count++;
            LOG_DEBUG("[SAVES_BUILD_STRUCT] Added Temporary save type");
        }
    }

    for (u16 idx = 0; idx < 16 && ctx->type_count < ctx->max_types; idx++) {
        if (has_save_info(game->application_id, empty_uid, FsSaveDataType_Cache, FsSaveDataSpaceId_SdUser, idx)) {
            SaveTypeEntry* t = &ctx->types[ctx->type_count];
            memset(t, 0, sizeof(SaveTypeEntry));
            t->save_type = FsSaveDataType_Cache;
            t->space_id = FsSaveDataSpaceId_SdUser;
            t->handle = ctx->next_type_handle++;
            t->parent_handle = game->folder_handle;
            t->game_index = game_idx;
            t->user_index = -1;
            t->cache_index = idx;
            snprintf(t->name, sizeof(t->name), "SD_Cache.%04d", idx);
            ctx->type_count++;
        }
    }

    game->users_scanned = true;
    LOG_DEBUG("[SAVES_BUILD_STRUCT] Structure complete for game %u: %u user folders, %u types",
              game_idx, ctx->user_folder_count - initial_user_folders, ctx->type_count - initial_types);
}

static void ensure_type_scanned(SavesContext* ctx, SaveTypeEntry* type) {
    if (type->scanned) {
        LOG_DEBUG("[SAVES_ENSURE_TYPE] Type 0x%08X already scanned", type->handle);
        return;
    }

    LOG_DEBUG("[SAVES_ENSURE_TYPE] Scanning type 0x%08X", type->handle);

    if (!mount_save_type(ctx, type)) {
        LOG_DEBUG("[SAVES_ENSURE_TYPE] Failed to mount type 0x%08X, marking as scanned", type->handle);
        type->scanned = true;
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s:/", type->mount_name);

    u32 type_idx = 0;
    for (u32 i = 0; i < ctx->type_count; i++) {
        if (&ctx->types[i] == type) { type_idx = i; break; }
    }

    scan_directory(ctx, type_idx, type->handle, path);
    type->scanned = true;
    LOG_DEBUG("[SAVES_ENSURE_TYPE] Type 0x%08X scan complete", type->handle);
}

static void ensure_file_scanned(SavesContext* ctx, SaveFileEntry* file) {
    if (!file->is_directory || file->scanned) return;
    scan_directory(ctx, file->type_index, file->handle, file->full_path);
    file->scanned = true;
}

static bool ensure_services(SavesContext* ctx) {
    if (ctx->user_count > 0) return true;

    if (!ctx->acc_initialized) {
        accountInitialize(AccountServiceType_System);
        ctx->acc_initialized = true;
    }

    if (!ctx->ns_initialized) {
        nsInitialize();
        ctx->ns_initialized = true;
    }

    return enumerate_all_users(ctx);
}

Result savesInit(SavesContext* ctx) {
    LOG_DEBUG("[SAVES_INIT] Initializing SavesContext");
    memset(ctx, 0, sizeof(SavesContext));
    mutexInit(&ctx->saves_mutex);

    ctx->max_games = MTP_SAVES_MAX_GAMES;
    ctx->games = (GameSaveEntry*)malloc(sizeof(GameSaveEntry) * ctx->max_games);

    ctx->max_user_folders = MTP_SAVES_MAX_USER_FOLDERS;
    ctx->user_folders = (UserFolderEntry*)malloc(sizeof(UserFolderEntry) * ctx->max_user_folders);

    ctx->max_types = MTP_SAVES_MAX_TYPES;
    ctx->types = (SaveTypeEntry*)malloc(sizeof(SaveTypeEntry) * ctx->max_types);

    ctx->max_files = MTP_SAVES_MAX_FILES;
    ctx->files = (SaveFileEntry*)malloc(sizeof(SaveFileEntry) * ctx->max_files);

    if (!ctx->games || !ctx->user_folders || !ctx->types || !ctx->files) {
        LOG_ERROR("Saves: Memory allocation failed");
        free(ctx->games);
        free(ctx->user_folders);
        free(ctx->types);
        free(ctx->files);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    memset(ctx->games, 0, sizeof(GameSaveEntry) * ctx->max_games);
    memset(ctx->user_folders, 0, sizeof(UserFolderEntry) * ctx->max_user_folders);
    memset(ctx->types, 0, sizeof(SaveTypeEntry) * ctx->max_types);
    memset(ctx->files, 0, sizeof(SaveFileEntry) * ctx->max_files);

    ctx->next_user_handle = MTP_HANDLE_SAVES_USER_START;
    ctx->next_type_handle = MTP_HANDLE_SAVES_TYPE_START;
    ctx->next_file_handle = MTP_HANDLE_SAVES_FILE_START;
    ctx->initialized = true;
    ctx->needs_refresh = true;
    ctx->refresh_in_progress = false;

    LOG_INFO("Saves: Initialized (will refresh on first access)");

    return 0;
}

void savesPreInitServices(SavesContext* ctx) {
    if (!ctx->initialized) return;

    if (!ctx->acc_initialized) {
        accountInitialize(AccountServiceType_System);
        ctx->acc_initialized = true;
    }
    if (!ctx->ns_initialized) {
        nsInitialize();
        ctx->ns_initialized = true;
    }

    LOG_INFO("Saves: Services pre-initialized");
}

void savesRefreshIfNeeded(SavesContext* ctx) {
    if (!ctx->initialized) return;

    mutexLock(&ctx->saves_mutex);
    if (!ctx->needs_refresh || ctx->refresh_in_progress) {
        mutexUnlock(&ctx->saves_mutex);
        return;
    }
    ctx->needs_refresh = false;
    ctx->refresh_in_progress = true;
    mutexUnlock(&ctx->saves_mutex);

    LOG_INFO("Saves: Starting refresh from main thread");

    do_refresh_internal(ctx);

    mutexLock(&ctx->saves_mutex);
    ctx->refresh_in_progress = false;
    mutexUnlock(&ctx->saves_mutex);
    LOG_INFO("Saves: Refresh complete, %u games found", ctx->game_count);
}

void savesExit(SavesContext* ctx) {
    if (!ctx->initialized) return;

    stop_background_refresh();

    mutexLock(&ctx->saves_mutex);

    for (u32 i = 0; i < ctx->type_count; i++) {
        unmount_save_type(&ctx->types[i]);
    }

    free(ctx->files);
    free(ctx->types);
    free(ctx->user_folders);
    free(ctx->games);

    ctx->files = NULL;
    ctx->types = NULL;
    ctx->user_folders = NULL;
    ctx->games = NULL;
    ctx->file_count = 0;
    ctx->type_count = 0;
    ctx->user_folder_count = 0;
    ctx->game_count = 0;

    mutexUnlock(&ctx->saves_mutex);

    if (ctx->ns_initialized) {
        nsExit();
    }
    if (ctx->acc_initialized) {
        accountExit();
    }

    ctx->initialized = false;

    mutexLock(&s_name_cache_mutex);
    s_name_cache_count = 0;
    mutexUnlock(&s_name_cache_mutex);

    LOG_INFO("Saves: Cleanup complete");
}

bool savesIsVirtualStorage(u32 storage_id) {
    return storage_id == MTP_STORAGE_SAVES;
}

bool savesIsVirtualHandle(u32 handle) {
    return (handle & MTP_HANDLE_MASK) == MTP_HANDLE_SAVES_BASE;
}

bool savesGetStorageInfo(SavesContext* ctx, u32 storage_id, MtpStorageInfo* out) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_SAVES) return false;

    memset(out, 0, sizeof(MtpStorageInfo));
    out->storage_id = MTP_STORAGE_SAVES;
    out->storage_type = 0x0003;
    out->filesystem_type = 0x0002;
    out->access_capability = 0x0000;
    out->max_capacity = 32ULL * 1024 * 1024 * 1024;
    out->free_space = 8ULL * 1024 * 1024 * 1024;
    strncpy(out->description, "Game Saves", sizeof(out->description));
    strncpy(out->volume_label, "SAVES", sizeof(out->volume_label));
    out->mounted = true;
    return true;
}

static Thread s_refresh_thread;
static bool s_refresh_running = false;
static bool s_refresh_stop = false;
static SavesContext* s_refresh_ctx = NULL;

static void do_refresh_internal(SavesContext* ctx) {
    LOG_DEBUG("[SAVES_REFRESH] do_refresh_internal: START");

    if (!ctx->acc_initialized) {
        accountInitialize(AccountServiceType_System);
        ctx->acc_initialized = true;
    }
    if (!ctx->ns_initialized) {
        nsInitialize();
        ctx->ns_initialized = true;
    }

    LOG_DEBUG("[SAVES_REFRESH] Enumerating users");
    ctx->user_count = 0;
    s32 count = 0;
    Result rc = accountListAllUsers(ctx->user_uids, MTP_SAVES_MAX_USERS, &count);
    if (R_SUCCEEDED(rc)) {
        LOG_DEBUG("[SAVES_REFRESH] Found %d users", count);
        ctx->user_count = count;
        for (s32 i = 0; i < count; i++) {
            AccountProfile profile;
            rc = accountGetProfile(&profile, ctx->user_uids[i]);
            if (R_SUCCEEDED(rc)) {
                AccountProfileBase base;
                AccountUserData data;
                rc = accountProfileGet(&profile, &data, &base);
                if (R_SUCCEEDED(rc) && base.nickname[0]) {
                    strncpy(ctx->user_names[i], base.nickname, 31);
                }
                accountProfileClose(&profile);
            }
            if (ctx->user_names[i][0] == '\0') {
                snprintf(ctx->user_names[i], 32, "%016lX", ctx->user_uids[i].uid[0]);
            }
        }
    } else {
        LOG_ERROR("[SAVES_REFRESH] accountListAllUsers failed: 0x%08X", rc);
    }

    LOG_DEBUG("[SAVES_REFRESH] Enumerating saves");
    FsSaveDataSpaceId spaces[] = { FsSaveDataSpaceId_User, FsSaveDataSpaceId_SdUser };

    for (size_t s = 0; s < sizeof(spaces)/sizeof(spaces[0]); s++) {
        FsSaveDataInfoReader reader;
        if (R_FAILED(fsOpenSaveDataInfoReader(&reader, spaces[s]))) {
            LOG_DEBUG("[SAVES_REFRESH] Failed to open reader for space %zu", s);
            continue;
        }

        FsSaveDataInfo info[64];
        while (true) {
            s64 cnt = 0;
            if (R_FAILED(fsSaveDataInfoReaderRead(&reader, info, 64, &cnt)) || cnt == 0) break;

            LOG_DEBUG("[SAVES_REFRESH] Read %lld save info entries", cnt);
            for (s64 i = 0; i < cnt; i++) {
                u64 app_id = info[i].application_id;
                if (app_id == 0) continue;

                u64 title_type = (app_id >> 48) & 0xFFFF;
                if (title_type != 0x0100 && title_type != 0x0101 && title_type != 0x0102) continue;

                mutexLock(&ctx->saves_mutex);
                if (s_save_info_count < MAX_SAVE_INFO_ENTRIES) {
                    SaveInfoEntry* e = &s_save_info[s_save_info_count++];
                    e->application_id = app_id;
                    e->uid = info[i].uid;
                    e->save_data_type = info[i].save_data_type;
                    e->space_id = spaces[s];
                    e->save_data_index = info[i].save_data_index;
                }
                find_or_create_game_fast(ctx, app_id);
                mutexUnlock(&ctx->saves_mutex);
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }

    LOG_DEBUG("[SAVES_REFRESH] Fetching names for %u games", ctx->game_count);
    for (u32 i = 0; i < ctx->game_count; i++) {
        GameSaveEntry* g = &ctx->games[i];
        NameCacheEntry* entry = get_name_cache_entry(g->application_id);
        if (entry && !entry->fetched) {
            fetch_name_for_entry(entry);
            mutexLock(&ctx->saves_mutex);
            strncpy(g->game_name, entry->name, sizeof(g->game_name) - 1);
            g->is_installed = entry->is_installed;
            mutexUnlock(&ctx->saves_mutex);
        }
        if ((i % 10) == 0) svcSleepThread(1000000ULL);
    }

    mutexLock(&ctx->saves_mutex);
    ctx->needs_refresh = false;
    mutexUnlock(&ctx->saves_mutex);
    LOG_DEBUG("[SAVES_REFRESH] do_refresh_internal: END - found %u games", ctx->game_count);
}

static void refresh_thread_func(void* arg) {
    SavesContext* ctx = (SavesContext*)arg;
    LOG_DEBUG("[SAVES_BG_THREAD] Background refresh thread started");

    if (s_refresh_stop) {
        s_refresh_running = false;
        return;
    }

    do_refresh_internal(ctx);

    s_refresh_running = false;
    LOG_INFO("[SAVES_BG_THREAD] Background refresh complete (%u games)", ctx->game_count);
}

static void start_background_refresh(SavesContext* ctx) {
    if (s_refresh_running) return;

    s_refresh_stop = false;
    s_refresh_ctx = ctx;
    s_refresh_running = true;

    Result rc = threadCreate(&s_refresh_thread, refresh_thread_func, ctx, NULL, 0x20000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&s_refresh_thread);
    } else {
        s_refresh_running = false;
        LOG_ERROR("[SAVES_BG_START] Failed to start refresh thread: 0x%08X", rc);
    }
}

static void stop_background_refresh(void) {
    if (!s_refresh_running) return;

    s_refresh_stop = true;

    Result rc = threadWaitForExit(&s_refresh_thread);
    if (R_FAILED(rc)) {
        LOG_ERROR("[SAVES_BG_STOP] threadWaitForExit failed: 0x%08X", rc);
    }

    rc = threadClose(&s_refresh_thread);
    if (R_FAILED(rc)) {
        LOG_ERROR("[SAVES_BG_STOP] threadClose failed: 0x%08X", rc);
    }

    s_refresh_running = false;
}

Result savesRefresh(SavesContext* ctx) {
    if (!ctx->initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    mutexLock(&ctx->saves_mutex);
    for (u32 i = 0; i < ctx->type_count; i++) {
        unmount_save_type(&ctx->types[i]);
    }
    ctx->game_count = 0;
    ctx->user_folder_count = 0;
    ctx->type_count = 0;
    ctx->file_count = 0;
    ctx->next_user_handle = MTP_HANDLE_SAVES_USER_START;
    ctx->next_type_handle = MTP_HANDLE_SAVES_TYPE_START;
    ctx->next_file_handle = MTP_HANDLE_SAVES_FILE_START;
    s_save_info_count = 0;
    ctx->needs_refresh = true;
    mutexUnlock(&ctx->saves_mutex);

    return 0;
}

u32 savesGetObjectCount(SavesContext* ctx, u32 storage_id, u32 parent_handle) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_SAVES) {
        LOG_DEBUG("[SAVES_GET_COUNT] Invalid context or storage_id");
        return 0;
    }

    LOG_DEBUG("[SAVES_GET_COUNT] Getting count for parent_handle 0x%08X", parent_handle);
    mutexLock(&ctx->saves_mutex);
    u32 count = 0;

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        count = ctx->game_count;
        LOG_DEBUG("[SAVES_GET_COUNT] Root level: %u games", count);
    }
    else if (is_game_handle(parent_handle)) {
        u32 idx = get_game_index(parent_handle);
        if (idx < ctx->game_count) {
            LOG_DEBUG("[SAVES_GET_COUNT] Game handle 0x%08X (index %u)", parent_handle, idx);
            build_game_structure(ctx, idx);

            for (u32 i = 0; i < ctx->user_folder_count; i++) {
                if (ctx->user_folders[i].parent_game_handle == parent_handle) count++;
            }
            for (u32 i = 0; i < ctx->type_count; i++) {
                if (ctx->types[i].parent_handle == parent_handle) count++;
            }
            LOG_DEBUG("[SAVES_GET_COUNT] Game has %u children", count);
        }
    }
    else if (is_user_handle(parent_handle)) {
        for (u32 i = 0; i < ctx->type_count; i++) {
            if (ctx->types[i].parent_handle == parent_handle) count++;
        }
        LOG_DEBUG("[SAVES_GET_COUNT] User handle 0x%08X: %u types", parent_handle, count);
    }
    else if (is_type_handle(parent_handle)) {
        SaveTypeEntry* t = find_type_by_handle(ctx, parent_handle);
        if (t) {
            LOG_DEBUG("[SAVES_GET_COUNT] Type handle 0x%08X, scanning...", parent_handle);
            ensure_type_scanned(ctx, t);
            for (u32 i = 0; i < ctx->file_count; i++) {
                if (ctx->files[i].parent_handle == parent_handle) count++;
            }
            LOG_DEBUG("[SAVES_GET_COUNT] Type has %u files", count);
        }
    }
    else if (is_file_handle(parent_handle)) {
        SaveFileEntry* f = find_file_by_handle(ctx, parent_handle);
        if (f && f->is_directory) {
            LOG_DEBUG("[SAVES_GET_COUNT] File handle 0x%08X (directory), scanning...", parent_handle);
            ensure_file_scanned(ctx, f);
            for (u32 i = 0; i < ctx->file_count; i++) {
                if (ctx->files[i].parent_handle == parent_handle) count++;
            }
            LOG_DEBUG("[SAVES_GET_COUNT] Directory has %u children", count);
        }
    }

    mutexUnlock(&ctx->saves_mutex);
    return count;
}

u32 savesEnumObjects(SavesContext* ctx, u32 storage_id, u32 parent_handle, u32* handles, u32 max) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_SAVES) {
        LOG_DEBUG("[SAVES_ENUM] Invalid context or storage_id");
        return 0;
    }

    if (ctx->needs_refresh) {
        LOG_INFO("[SAVES_ENUM] Refresh needed but deferred to main thread");
        return 0;
    }

    LOG_DEBUG("[SAVES_ENUM] Enumerating objects for parent_handle 0x%08X (max %u)", parent_handle, max);
    mutexLock(&ctx->saves_mutex);
    u32 count = 0;

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        LOG_DEBUG("[SAVES_ENUM] Root level, %u games available", ctx->game_count);
        for (u32 i = 0; i < ctx->game_count && count < max; i++) {
            handles[count++] = ctx->games[i].folder_handle;
        }
    }
    else if (is_game_handle(parent_handle)) {
        u32 idx = get_game_index(parent_handle);
        if (idx < ctx->game_count) {
            LOG_DEBUG("[SAVES_ENUM] Game handle 0x%08X (index %u)", parent_handle, idx);
            build_game_structure(ctx, idx);

            for (u32 i = 0; i < ctx->user_folder_count && count < max; i++) {
                if (ctx->user_folders[i].parent_game_handle == parent_handle) {
                    handles[count++] = ctx->user_folders[i].handle;
                }
            }
            for (u32 i = 0; i < ctx->type_count && count < max; i++) {
                if (ctx->types[i].parent_handle == parent_handle) {
                    handles[count++] = ctx->types[i].handle;
                }
            }
            LOG_DEBUG("[SAVES_ENUM] Returning %u handles for game", count);
        }
    }
    else if (is_user_handle(parent_handle)) {
        for (u32 i = 0; i < ctx->type_count && count < max; i++) {
            if (ctx->types[i].parent_handle == parent_handle) {
                handles[count++] = ctx->types[i].handle;
            }
        }
        LOG_DEBUG("[SAVES_ENUM] Returning %u type handles for user", count);
    }
    else if (is_type_handle(parent_handle)) {
        SaveTypeEntry* t = find_type_by_handle(ctx, parent_handle);
        if (t) {
            LOG_DEBUG("[SAVES_ENUM] Type handle 0x%08X, scanning files", parent_handle);
            ensure_type_scanned(ctx, t);
            for (u32 i = 0; i < ctx->file_count && count < max; i++) {
                if (ctx->files[i].parent_handle == parent_handle) {
                    handles[count++] = ctx->files[i].handle;
                }
            }
            LOG_DEBUG("[SAVES_ENUM] Returning %u file handles", count);
        }
    }
    else if (is_file_handle(parent_handle)) {
        SaveFileEntry* f = find_file_by_handle(ctx, parent_handle);
        if (f && f->is_directory) {
            LOG_DEBUG("[SAVES_ENUM] Directory file handle 0x%08X, scanning contents", parent_handle);
            ensure_file_scanned(ctx, f);
            for (u32 i = 0; i < ctx->file_count && count < max; i++) {
                if (ctx->files[i].parent_handle == parent_handle) {
                    handles[count++] = ctx->files[i].handle;
                }
            }
            LOG_DEBUG("[SAVES_ENUM] Returning %u child handles", count);
        }
    }

    mutexUnlock(&ctx->saves_mutex);
    return count;
}

bool savesGetObjectInfo(SavesContext* ctx, u32 handle, MtpObject* out) {
    if (!ctx->initialized || !savesIsVirtualHandle(handle)) return false;

    mutexLock(&ctx->saves_mutex);
    memset(out, 0, sizeof(MtpObject));
    bool found = false;

    if (is_game_handle(handle)) {
        u32 idx = get_game_index(handle);
        if (idx < ctx->game_count) {
            GameSaveEntry* g = &ctx->games[idx];
            out->handle = handle;
            out->parent_handle = 0xFFFFFFFF;
            out->storage_id = MTP_STORAGE_SAVES;
            out->format = MTP_FORMAT_ASSOCIATION;
            out->object_type = MTP_OBJECT_TYPE_FOLDER;

            snprintf(out->filename, MTP_MAX_FILENAME - 1, "%s [%016lX]",
                     g->game_name, g->application_id);
            found = true;
        }
    }
    else if (is_user_handle(handle)) {
        UserFolderEntry* uf = find_user_folder_by_handle(ctx, handle);
        if (uf) {
            out->handle = handle;
            out->parent_handle = uf->parent_game_handle;
            out->storage_id = MTP_STORAGE_SAVES;
            out->format = MTP_FORMAT_ASSOCIATION;
            out->object_type = MTP_OBJECT_TYPE_FOLDER;
            snprintf(out->filename, MTP_MAX_FILENAME - 1, "User: %s", uf->username);
            found = true;
        }
    }
    else if (is_type_handle(handle)) {
        SaveTypeEntry* t = find_type_by_handle(ctx, handle);
        if (t) {
            out->handle = handle;
            out->parent_handle = t->parent_handle;
            out->storage_id = MTP_STORAGE_SAVES;
            out->format = MTP_FORMAT_ASSOCIATION;
            out->object_type = MTP_OBJECT_TYPE_FOLDER;
            strncpy(out->filename, t->name, MTP_MAX_FILENAME - 1);
            found = true;
        }
    }
    else if (is_file_handle(handle)) {
        SaveFileEntry* f = find_file_by_handle(ctx, handle);
        if (f) {
            out->handle = handle;
            out->parent_handle = f->parent_handle;
            out->storage_id = MTP_STORAGE_SAVES;
            out->format = f->is_directory ? MTP_FORMAT_ASSOCIATION : MTP_FORMAT_UNDEFINED;
            out->size = f->size;
            out->object_type = f->is_directory ? MTP_OBJECT_TYPE_FOLDER : MTP_OBJECT_TYPE_FILE;
            strncpy(out->filename, f->filename, MTP_MAX_FILENAME - 1);
            strncpy(out->full_path, f->full_path, MTP_MAX_PATH - 1);
            found = true;
        }
    }

    mutexUnlock(&ctx->saves_mutex);
    return found;
}

GameSaveEntry* savesGetGameForHandle(SavesContext* ctx, u32 handle) {
    if (is_game_handle(handle)) {
        u32 idx = get_game_index(handle);
        return (idx < ctx->game_count) ? &ctx->games[idx] : NULL;
    }
    if (is_user_handle(handle)) {
        UserFolderEntry* uf = find_user_folder_by_handle(ctx, handle);
        return uf ? &ctx->games[uf->game_index] : NULL;
    }
    if (is_type_handle(handle)) {
        SaveTypeEntry* t = find_type_by_handle(ctx, handle);
        return t ? &ctx->games[t->game_index] : NULL;
    }
    if (is_file_handle(handle)) {
        SaveFileEntry* f = find_file_by_handle(ctx, handle);
        return f ? &ctx->games[f->game_index] : NULL;
    }
    return NULL;
}

s64 savesReadObject(SavesContext* ctx, u32 handle, u64 offset, void* buf, u64 size) {
    if (!ctx->initialized || !is_file_handle(handle)) return -1;

    // Copy path under lock, then release before file I/O to reduce lock contention
    char path[512];
    {
        mutexLock(&ctx->saves_mutex);
        SaveFileEntry* f = find_file_by_handle(ctx, handle);
        if (!f || f->is_directory) {
            mutexUnlock(&ctx->saves_mutex);
            return -1;
        }
        strncpy(path, f->full_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        mutexUnlock(&ctx->saves_mutex);
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;

    setvbuf(fp, NULL, _IOFBF, 256 * 1024);

    fseek(fp, offset, SEEK_SET);
    size_t rd = fread(buf, 1, size, fp);
    fclose(fp);

    return (s64)rd;
}

u32 savesCreateObject(SavesContext* ctx, u32 storage_id, u32 parent, const char* name, u16 fmt, u64 size) {
    if (!ctx->initialized || storage_id != MTP_STORAGE_SAVES) return 0;
    if (ctx->file_count >= ctx->max_files) return 0;

    mutexLock(&ctx->saves_mutex);

    char path[512];
    u32 game_idx = 0, type_idx = 0;

    if (is_type_handle(parent)) {
        SaveTypeEntry* t = find_type_by_handle(ctx, parent);
        if (!t || !mount_save_type(ctx, t)) {
            mutexUnlock(&ctx->saves_mutex);
            return 0;
        }
        snprintf(path, sizeof(path), "%s:/", t->mount_name);
        game_idx = t->game_index;
        for (u32 i = 0; i < ctx->type_count; i++) {
            if (&ctx->types[i] == t) { type_idx = i; break; }
        }
    } else if (is_file_handle(parent)) {
        SaveFileEntry* pf = find_file_by_handle(ctx, parent);
        if (!pf || !pf->is_directory) {
            mutexUnlock(&ctx->saves_mutex);
            return 0;
        }
        strncpy(path, pf->full_path, sizeof(path) - 1);
        game_idx = pf->game_index;
        type_idx = pf->type_index;
    } else {
        mutexUnlock(&ctx->saves_mutex);
        return 0;
    }

    SaveFileEntry* f = &ctx->files[ctx->file_count];
    memset(f, 0, sizeof(SaveFileEntry));

    strncpy(f->filename, name, sizeof(f->filename) - 1);
    snprintf(f->full_path, sizeof(f->full_path), "%s/%s", path, name);

    char* ds = strstr(f->full_path, "//");
    if (ds) memmove(ds, ds + 1, strlen(ds));

    f->handle = ctx->next_file_handle++;
    f->parent_handle = parent;
    f->game_index = game_idx;
    f->type_index = type_idx;
    f->is_directory = (fmt == MTP_FORMAT_ASSOCIATION);
    f->size = size;

    if (f->is_directory) {
        if (mkdir(f->full_path, 0755) != 0) {
            mutexUnlock(&ctx->saves_mutex);
            return 0;
        }
    } else {
        FILE* fp = fopen(f->full_path, "wb");
        if (!fp) {
            mutexUnlock(&ctx->saves_mutex);
            return 0;
        }
        fclose(fp);
    }

    ctx->file_count++;
    mutexUnlock(&ctx->saves_mutex);
    return f->handle;
}

s64 savesWriteObject(SavesContext* ctx, u32 handle, u64 offset, const void* buf, u64 size) {
    if (!ctx->initialized || !is_file_handle(handle)) return -1;

    // Copy path under lock, then release before file I/O to reduce lock contention
    char path[512];
    u32 file_index = 0;
    {
        mutexLock(&ctx->saves_mutex);
        SaveFileEntry* f = find_file_by_handle(ctx, handle);
        if (!f || f->is_directory) {
            mutexUnlock(&ctx->saves_mutex);
            return -1;
        }
        strncpy(path, f->full_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        // Find the index so we can update size after write
        for (u32 i = 0; i < ctx->file_count; i++) {
            if (ctx->files[i].handle == handle) { file_index = i; break; }
        }
        mutexUnlock(&ctx->saves_mutex);
    }

    FILE* fp = fopen(path, "r+b");
    if (!fp) fp = fopen(path, "wb");
    if (!fp) return -1;

    setvbuf(fp, NULL, _IOFBF, 256 * 1024);

    fseek(fp, offset, SEEK_SET);
    size_t wr = fwrite(buf, 1, size, fp);
    fclose(fp);

    // Update file size under lock
    if (wr > 0) {
        mutexLock(&ctx->saves_mutex);
        if (file_index < ctx->file_count && ctx->files[file_index].handle == handle) {
            if (offset + wr > ctx->files[file_index].size) {
                ctx->files[file_index].size = offset + wr;
            }
        }
        mutexUnlock(&ctx->saves_mutex);
    }

    return (s64)wr;
}

bool savesDeleteObject(SavesContext* ctx, u32 handle) {
    if (!ctx->initialized) return false;
    if (is_game_handle(handle) || is_user_handle(handle)) return false;
    if (is_type_handle(handle)) return false;
    if (!is_file_handle(handle)) return false;

    // Copy path and type under lock, then release before filesystem I/O
    char path[512];
    bool is_dir = false;
    {
        mutexLock(&ctx->saves_mutex);
        SaveFileEntry* f = find_file_by_handle(ctx, handle);
        if (!f) {
            mutexUnlock(&ctx->saves_mutex);
            return false;
        }
        strncpy(path, f->full_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        is_dir = f->is_directory;
        mutexUnlock(&ctx->saves_mutex);
    }

    int res = is_dir ? rmdir(path) : remove(path);
    return res == 0;
}

bool savesCommitObject(SavesContext* ctx, u32 handle) {
    if (!ctx->initialized) return false;

    mutexLock(&ctx->saves_mutex);

    SaveTypeEntry* t = NULL;
    if (is_type_handle(handle)) {
        t = find_type_by_handle(ctx, handle);
    } else if (is_file_handle(handle)) {
        SaveFileEntry* f = find_file_by_handle(ctx, handle);
        if (f && f->type_index < ctx->type_count) {
            t = &ctx->types[f->type_index];
        }
    }

    if (!t || !t->mounted) {
        mutexUnlock(&ctx->saves_mutex);
        return false;
    }

    Result rc = fsdevCommitDevice(t->mount_name);
    mutexUnlock(&ctx->saves_mutex);
    return R_SUCCEEDED(rc);
}
