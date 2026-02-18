// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_storage.h"
#include "mtp/mtp_log.h"
#include "install/fat32.h"
#include "core/TransferEvents.h"
#include "core/Event.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <unordered_map>

using namespace Javelin;

#if DEBUG
#define STORAGE_DEBUG 1
#else
#define STORAGE_DEBUG 0
#endif

static bool folder_is_scanned(MtpStorageContext* ctx, u32 storage_id, u32 folder_handle);
static void mark_folder_scanned(MtpStorageContext* ctx, u32 storage_id, u32 folder_handle);
static void scan_directory_shallow(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle, const char* path);

static const char* get_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";
    return dot + 1;
}

u16 mtpStorageGetFormat(const char* filename) {
    const char* ext = get_extension(filename);

    char lower[16] = {0};
    for (int i = 0; ext[i] && i < 15; i++) {
        lower[i] = (ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i] + 32 : ext[i];
    }

    if (strcmp(lower, "jpg") == 0 || strcmp(lower, "jpeg") == 0) return MTP_FORMAT_JPEG;
    if (strcmp(lower, "png") == 0) return MTP_FORMAT_PNG;
    if (strcmp(lower, "bmp") == 0) return MTP_FORMAT_BMP;
    if (strcmp(lower, "mp3") == 0) return MTP_FORMAT_MP3;
    if (strcmp(lower, "wav") == 0) return MTP_FORMAT_WAV;
    if (strcmp(lower, "avi") == 0) return MTP_FORMAT_AVI;
    if (strcmp(lower, "mp4") == 0 || strcmp(lower, "mpeg") == 0 || strcmp(lower, "mpg") == 0)
        return MTP_FORMAT_MPEG;
    if (strcmp(lower, "txt") == 0 || strcmp(lower, "log") == 0 || strcmp(lower, "ini") == 0 ||
        strcmp(lower, "cfg") == 0 || strcmp(lower, "json") == 0 || strcmp(lower, "xml") == 0)
        return MTP_FORMAT_TEXT;
    if (strcmp(lower, "html") == 0 || strcmp(lower, "htm") == 0) return MTP_FORMAT_HTML;

    return MTP_FORMAT_UNDEFINED;
}

u32 mtpStorageGetHandleBase(u32 storage_id) {
    switch (storage_id) {
        case MTP_STORAGE_SDCARD:      return MTP_HANDLE_SDCARD_BASE;
        case MTP_STORAGE_NAND_USER:   return MTP_HANDLE_NAND_USER_BASE;
        case MTP_STORAGE_NAND_SYSTEM: return MTP_HANDLE_NAND_SYSTEM_BASE;
        case MTP_STORAGE_INSTALLED:   return MTP_HANDLE_INSTALLED_BASE;
        case MTP_STORAGE_SD_INSTALL:  return MTP_HANDLE_SD_INSTALL_BASE;
        case MTP_STORAGE_NAND_INSTALL:return MTP_HANDLE_NAND_INSTALL_BASE;
        case MTP_STORAGE_SAVES:       return MTP_HANDLE_SAVES_BASE;
        case MTP_STORAGE_ALBUM:       return MTP_HANDLE_ALBUM_BASE;
        case MTP_STORAGE_GAMECARD:    return MTP_HANDLE_GAMECARD_BASE;
        default:                      return MTP_HANDLE_SDCARD_BASE;
    }
}

static u32 get_storage_from_handle(u32 handle) {
    u32 base = handle & MTP_HANDLE_MASK;
    switch (base) {
        case MTP_HANDLE_SDCARD_BASE:      return MTP_STORAGE_SDCARD;
        case MTP_HANDLE_NAND_USER_BASE:   return MTP_STORAGE_USER;
        case MTP_HANDLE_NAND_SYSTEM_BASE: return MTP_STORAGE_NAND_SYSTEM;
        case MTP_HANDLE_INSTALLED_BASE:   return MTP_STORAGE_INSTALLED;
        case MTP_HANDLE_SD_INSTALL_BASE:  return MTP_STORAGE_SD_INSTALL;
        case MTP_HANDLE_NAND_INSTALL_BASE:return MTP_STORAGE_NAND_INSTALL;
        case MTP_HANDLE_SAVES_BASE:       return MTP_STORAGE_SAVES;
        case MTP_HANDLE_ALBUM_BASE:       return MTP_STORAGE_ALBUM;
        case MTP_HANDLE_GAMECARD_BASE:    return MTP_STORAGE_GAMECARD;
        default:                          return 0xFFFFFFFF;
    }
}

static u32 add_object_unlocked(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                      const char* path, const char* filename, u8 obj_type, u64 size) {
    if (!ctx->objects || ctx->object_count >= ctx->max_objects) {
#if STORAGE_DEBUG
        LOG_ERROR("Object cache full! (%u/%u)", ctx->object_count, ctx->max_objects);
#endif
        return 0;
    }

    if (ctx->object_count > 0 && ctx->object_count % 1000 == 0) {
        u32 percent = (ctx->object_count * 100) / ctx->max_objects;
        if (percent >= 90) {
            LOG_WARN("Object cache %u%% full (%u/%u)", percent, ctx->object_count, ctx->max_objects);
        }
    }

    MtpObject* obj = &ctx->objects[ctx->object_count];

    u32 base = mtpStorageGetHandleBase(storage_id);
    obj->handle = base | (ctx->next_handle++ & 0xFFFF);

    obj->storage_id = storage_id;
    obj->parent_handle = parent_handle;
    obj->object_type = obj_type;
    obj->size = size;

    strncpy(obj->filename, filename, MTP_MAX_FILENAME - 1);
    obj->filename[MTP_MAX_FILENAME - 1] = '\0';

    strncpy(obj->full_path, path, MTP_MAX_PATH - 1);
    obj->full_path[MTP_MAX_PATH - 1] = '\0';

    if (obj_type == MTP_OBJECT_TYPE_FOLDER) {
        obj->format = MTP_FORMAT_ASSOCIATION;
    } else {
        obj->format = mtpStorageGetFormat(filename);
    }

    ctx->object_count++;

    return obj->handle;
}

static u32 add_object(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                      const char* path, const char* filename, u8 obj_type, u64 size) {
    mutexLock(&ctx->cache_mutex);
    u32 handle = add_object_unlocked(ctx, storage_id, parent_handle, path, filename, obj_type, size);
    mutexUnlock(&ctx->cache_mutex);
    return handle;
}

static void scan_directory_shallow(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                                    const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        LOG_WARN("Failed to open directory: %s", path);
        return;
    }

    size_t path_len = strlen(path);
    bool has_slash = (path_len > 0 && path[path_len - 1] == '/');

    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < 2000) {
        if (entry->d_name[0] == '.') {
            if (entry->d_name[1] == '\0' ||
                (entry->d_name[1] == '.' && entry->d_name[2] == '\0')) {
                continue;
            }
        }

        char full_path[MTP_MAX_PATH];
        int written;
        if (has_slash) {
            written = snprintf(full_path, sizeof(full_path), "%s%s", path, entry->d_name);
        } else {
            written = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        }

        if (written < 0 || (size_t)written >= sizeof(full_path)) {
            continue;
        }

        bool is_dir = false;
        bool is_file = false;
        u64 file_size = 0;

#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type != DT_UNKNOWN) {
            is_dir = (entry->d_type == DT_DIR);
            is_file = (entry->d_type == DT_REG);
        } else
#endif
        {
            struct stat st;
            if (stat(full_path, &st) != 0) {
                continue;
            }
            is_dir = S_ISDIR(st.st_mode);
            is_file = S_ISREG(st.st_mode);
            file_size = st.st_size;
        }

        if (is_dir) {
            add_object(ctx, storage_id, parent_handle,
                       full_path, entry->d_name,
                       MTP_OBJECT_TYPE_FOLDER, 0);
        } else if (is_file) {
            add_object(ctx, storage_id, parent_handle,
                       full_path, entry->d_name,
                       MTP_OBJECT_TYPE_FILE, file_size);
        }
        count++;

        if (count % 200 == 0) {
            svcSleepThread(10000ULL);
        }
    }

    closedir(dir);
}

Result mtpStorageInit(MtpStorageContext* ctx) {
    memset(ctx, 0, sizeof(MtpStorageContext));
    mutexInit(&ctx->cache_mutex);

#if STORAGE_DEBUG
    LOG_INFO("Initializing MTP storage...");
#endif

    Result rc = fsInitialize();
    if (R_FAILED(rc)) {
#if STORAGE_DEBUG
        LOG_WARN("fsInitialize failed: 0x%08X (may already be initialized)", rc);
#endif
    }

    FsDeviceOperator devOp;
    rc = fsOpenDeviceOperator(&devOp);
    if (R_SUCCEEDED(rc)) {
        fsDeviceOperatorClose(&devOp);
    }

    ctx->objects = (MtpObject*)malloc(sizeof(MtpObject) * MTP_MAX_OBJECTS);
    if (!ctx->objects) {
#if STORAGE_DEBUG
        LOG_ERROR("Failed to allocate object array!");
#endif
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    memset(ctx->objects, 0, sizeof(MtpObject) * MTP_MAX_OBJECTS);
    ctx->max_objects = MTP_MAX_OBJECTS;
    ctx->next_handle = 1;

    ctx->sdcard.storage_id = MTP_STORAGE_SDCARD;
    ctx->sdcard.storage_type = 0x0004;
    ctx->sdcard.filesystem_type = 0x0002;
    ctx->sdcard.access_capability = 0x0000;
    strncpy(ctx->sdcard.description, "SD Card", sizeof(ctx->sdcard.description));
    strncpy(ctx->sdcard.volume_label, "SDCARD", sizeof(ctx->sdcard.volume_label));

    struct statvfs vfs;
    if (statvfs("sdmc:/", &vfs) == 0) {
        ctx->sdcard.max_capacity = (u64)vfs.f_blocks * vfs.f_frsize;
        ctx->sdcard.free_space = (u64)vfs.f_bfree * vfs.f_frsize;
        ctx->sdcard.mounted = true;
#if STORAGE_DEBUG
        LOG_INFO("SD Card: %lu MB total, %lu MB free",
               (unsigned long)(ctx->sdcard.max_capacity / (1024*1024)),
               (unsigned long)(ctx->sdcard.free_space / (1024*1024)));
#endif
    } else {
        ctx->sdcard.mounted = false;
#if STORAGE_DEBUG
        LOG_WARN("SD Card not accessible");
#endif
    }

    ctx->user.storage_id = MTP_STORAGE_USER;
    ctx->user.storage_type = 0x0003;
    ctx->user.filesystem_type = 0x0002;
    ctx->user.access_capability = 0x0000;
    strncpy(ctx->user.description, "Internal Storage (USER)", sizeof(ctx->user.description));
    strncpy(ctx->user.volume_label, "USER", sizeof(ctx->user.volume_label));

    ctx->system.storage_id = MTP_STORAGE_NAND_SYSTEM;
    ctx->system.storage_type = 0x0003;
    ctx->system.filesystem_type = 0x0002;
    ctx->system.access_capability = 0x0000;
    strncpy(ctx->system.description, "Internal Storage (SYSTEM)", sizeof(ctx->system.description));
    strncpy(ctx->system.volume_label, "SYSTEM", sizeof(ctx->system.volume_label));
    ctx->system.mounted = false;

    ctx->user.mounted = false;

#if STORAGE_DEBUG
    LOG_DEBUG("Attempting BIS USER partition...");
#endif
    rc = fsOpenBisFileSystem(&ctx->user_fs, FsBisPartitionId_User, "");
    if (R_SUCCEEDED(rc)) {
        int mount_result = fsdevMountDevice("user", ctx->user_fs);
        if (mount_result >= 0) {
            ctx->user_fs_mounted = true;
            struct statvfs user_vfs;
            if (statvfs("user:/", &user_vfs) == 0) {
                ctx->user.max_capacity = (u64)user_vfs.f_blocks * user_vfs.f_frsize;
                ctx->user.free_space = (u64)user_vfs.f_bfree * user_vfs.f_frsize;
                ctx->user.mounted = true;
#if STORAGE_DEBUG
                LOG_INFO("BIS USER mounted: %lu MB",
                       (unsigned long)(ctx->user.max_capacity / (1024*1024)));
#endif
            }
        } else {
            fsFsClose(&ctx->user_fs);
#if STORAGE_DEBUG
            LOG_WARN("fsdevMountDevice USER failed: %d", mount_result);
#endif
        }
    } else {
#if STORAGE_DEBUG
        LOG_DEBUG("fsOpenBisFileSystem USER: 0x%08X", rc);
#endif
    }

#if STORAGE_DEBUG
    LOG_DEBUG("Attempting BIS SYSTEM partition...");
#endif
    rc = fsOpenBisFileSystem(&ctx->system_fs, FsBisPartitionId_System, "");
    if (R_SUCCEEDED(rc)) {
        int mount_result = fsdevMountDevice("system", ctx->system_fs);
        if (mount_result >= 0) {
            ctx->system_fs_mounted = true;

            struct statvfs sys_vfs;
            if (statvfs("system:/", &sys_vfs) == 0) {
                ctx->system.max_capacity = (u64)sys_vfs.f_blocks * sys_vfs.f_frsize;
                ctx->system.free_space = (u64)sys_vfs.f_bfree * sys_vfs.f_frsize;
                ctx->system.mounted = true;
#if STORAGE_DEBUG
                LOG_INFO("BIS SYSTEM mounted: %lu MB",
                       (unsigned long)(ctx->system.max_capacity / (1024*1024)));
#endif
            }
        } else {
            fsFsClose(&ctx->system_fs);
#if STORAGE_DEBUG
            LOG_WARN("fsdevMountDevice SYSTEM failed: %d", mount_result);
#endif
        }
    } else {
#if STORAGE_DEBUG
        LOG_DEBUG("fsOpenBisFileSystem SYSTEM: 0x%08X", rc);
#endif
    }

    ctx->album.storage_id = MTP_STORAGE_ALBUM;
    ctx->album.storage_type = 0x0003;
    ctx->album.filesystem_type = 0x0002;
    ctx->album.access_capability = 0x0001; // read-only
    strncpy(ctx->album.description, "Album", sizeof(ctx->album.description));
    strncpy(ctx->album.volume_label, "ALBUM", sizeof(ctx->album.volume_label));
    ctx->album.mounted = false;
    ctx->album_on_nand = false;

    bool album_found = false;

    struct stat album_st;
    if (stat("sdmc:/Nintendo/Album", &album_st) == 0 && S_ISDIR(album_st.st_mode)) {
        struct statvfs sd_vfs;
        if (statvfs("sdmc:/", &sd_vfs) == 0) {
            ctx->album.max_capacity = (u64)sd_vfs.f_blocks * sd_vfs.f_frsize;
            ctx->album.free_space = (u64)sd_vfs.f_bfree * sd_vfs.f_frsize;
            ctx->album.mounted = true;
            album_found = true;
            ctx->album_on_nand = false;
#if STORAGE_DEBUG
            LOG_INFO("Album: mounted (sdmc:/Nintendo/Album)");
#endif
        }
    }

    if (!album_found && ctx->user_fs_mounted) {
        if (stat("user:/Nintendo/Album", &album_st) == 0 && S_ISDIR(album_st.st_mode)) {
            struct statvfs user_vfs;
            if (statvfs("user:/", &user_vfs) == 0) {
                ctx->album.max_capacity = (u64)user_vfs.f_blocks * user_vfs.f_frsize;
                ctx->album.free_space = (u64)user_vfs.f_bfree * user_vfs.f_frsize;
                ctx->album.mounted = true;
                album_found = true;
                ctx->album_on_nand = true;
#if STORAGE_DEBUG
                LOG_INFO("Album: mounted (user:/Nintendo/Album)");
#endif
            }
        }
    }

    if (!album_found) {
#if STORAGE_DEBUG
        LOG_DEBUG("Album directory not found on SD or NAND USER");
#endif
    }

    if (!ctx->user.mounted) {
#if STORAGE_DEBUG
        LOG_DEBUG("Checking save:/ access...");
#endif
        struct statvfs save_vfs;
        if (statvfs("save:/", &save_vfs) == 0) {
            ctx->user.max_capacity = (u64)save_vfs.f_blocks * save_vfs.f_frsize;
            ctx->user.free_space = (u64)save_vfs.f_bfree * save_vfs.f_frsize;
            ctx->user.mounted = true;
            strncpy(ctx->user.description, "Save Data", sizeof(ctx->user.description));
            strncpy(ctx->user.volume_label, "SAVE", sizeof(ctx->user.volume_label));
#if STORAGE_DEBUG
            LOG_INFO("save:/ accessible: %lu MB",
                   (unsigned long)(ctx->user.max_capacity / (1024*1024)));
#endif
        }
    }

    if (!ctx->user.mounted) {
#if STORAGE_DEBUG
        LOG_WARN("Internal storage not accessible (need title override)");
#endif
    }

    ctx->sdcard_needs_scan = ctx->sdcard.mounted;
    ctx->user_needs_scan = ctx->user.mounted;
    ctx->system_needs_scan = ctx->system.mounted;

    ctx->initialized = true;

    LOG_INFO("=== MTP Storage Initialized ===");
    LOG_INFO("SD Card: %s (%.1f GB free)",
             ctx->sdcard.mounted ? "mounted" : "NOT MOUNTED",
             (float)ctx->sdcard.free_space / (1024*1024*1024));
    LOG_INFO("USER: %s", ctx->user.mounted ? "mounted" : "NOT MOUNTED");
    LOG_INFO("SYSTEM: %s", ctx->system.mounted ? "mounted" : "NOT MOUNTED");
    LOG_INFO("Album: %s", ctx->album.mounted ? "mounted" : "NOT MOUNTED");
    LOG_INFO("================================");

    return 0;
}

void mtpStorageExit(MtpStorageContext* ctx) {
#if STORAGE_DEBUG
    LOG_INFO("Shutting down MTP storage");
#endif

    mtpStorageStopBackgroundIndex(ctx);

    if (ctx->user_fs_mounted) {
        fsdevUnmountDevice("user");
        fsFsClose(&ctx->user_fs);
        ctx->user_fs_mounted = false;
    }

    if (ctx->system_fs_mounted) {
        fsdevUnmountDevice("system");
        fsFsClose(&ctx->system_fs);
        ctx->system_fs_mounted = false;
    }

    if (ctx->objects) {
        free(ctx->objects);
        ctx->objects = NULL;
    }

    memset(ctx, 0, sizeof(MtpStorageContext));
}

const char* mtpStorageGetBasePath(MtpStorageContext* ctx, u32 storage_id) {
    if (storage_id == MTP_STORAGE_SDCARD) {
        return "sdmc:/";
    } else if (storage_id == MTP_STORAGE_USER) {
        if (ctx->user_fs_mounted) {
            return "user:/";
        }
        return "save:/";
    } else if (storage_id == MTP_STORAGE_NAND_SYSTEM) {
        if (ctx->system_fs_mounted) {
            return "system:/";
        }
        return NULL;
    } else if (storage_id == MTP_STORAGE_ALBUM) {
        if (ctx->album.mounted) {
            if (ctx->album_on_nand) {
                return "user:/Nintendo/Album/";
            }
            return "sdmc:/Nintendo/Album/";
        }
        return NULL;
    }
    return NULL;
}

void mtpStorageRefresh(MtpStorageContext* ctx, u32 storage_id) {
#if STORAGE_DEBUG
    LOG_DEBUG("Refreshing storage 0x%08X", storage_id);
#endif

    mutexLock(&ctx->cache_mutex);

    u32 write_idx = 0;
    for (u32 i = 0; i < ctx->object_count; i++) {
        if (ctx->objects[i].storage_id != storage_id) {
            if (write_idx != i) {
                ctx->objects[write_idx] = ctx->objects[i];
            }
            write_idx++;
        }
    }
    ctx->object_count = write_idx;
    ctx->scanned_folder_count = 0;

    mutexUnlock(&ctx->cache_mutex);

    if (storage_id == MTP_STORAGE_SDCARD && ctx->sdcard.mounted) {
        scan_directory_shallow(ctx, MTP_STORAGE_SDCARD, 0xFFFFFFFF, "sdmc:/");
        mark_folder_scanned(ctx, MTP_STORAGE_SDCARD, 0xFFFFFFFF);

        struct statvfs vfs;
        if (statvfs("sdmc:/", &vfs) == 0) {
            ctx->sdcard.free_space = (u64)vfs.f_bfree * vfs.f_frsize;
        }
    } else if (storage_id == MTP_STORAGE_USER && ctx->user.mounted) {
        const char* base = mtpStorageGetBasePath(ctx, MTP_STORAGE_USER);
        if (base) {
            scan_directory_shallow(ctx, MTP_STORAGE_USER, 0xFFFFFFFF, base);
            mark_folder_scanned(ctx, MTP_STORAGE_USER, 0xFFFFFFFF);

            struct statvfs vfs;
            if (statvfs(base, &vfs) == 0) {
                ctx->user.free_space = (u64)vfs.f_bfree * vfs.f_frsize;
            }
        }
    } else if (storage_id == MTP_STORAGE_NAND_SYSTEM && ctx->system.mounted) {
        const char* base = mtpStorageGetBasePath(ctx, MTP_STORAGE_NAND_SYSTEM);
        if (base) {
            scan_directory_shallow(ctx, MTP_STORAGE_NAND_SYSTEM, 0xFFFFFFFF, base);
            mark_folder_scanned(ctx, MTP_STORAGE_NAND_SYSTEM, 0xFFFFFFFF);

            struct statvfs vfs;
            if (statvfs(base, &vfs) == 0) {
                ctx->system.free_space = (u64)vfs.f_bfree * vfs.f_frsize;
            }
        }
    } else if (storage_id == MTP_STORAGE_ALBUM && ctx->album.mounted) {
        const char* album_path = ctx->album_on_nand ? "user:/Nintendo/Album/" : "sdmc:/Nintendo/Album/";
        scan_directory_shallow(ctx, MTP_STORAGE_ALBUM, 0xFFFFFFFF, album_path);
        mark_folder_scanned(ctx, MTP_STORAGE_ALBUM, 0xFFFFFFFF);
    }

#if STORAGE_DEBUG
    LOG_DEBUG("Refresh complete. Objects: %u", ctx->object_count);
#endif
}

u32 mtpStorageGetIds(MtpStorageContext* ctx, u32* ids, u32 max_ids) {
    u32 count = 0;

    if (ctx->sdcard.mounted && count < max_ids) {
        ids[count++] = MTP_STORAGE_SDCARD;
    }

    if (ctx->user.mounted && count < max_ids) {
        ids[count++] = MTP_STORAGE_USER;
    }

    if (ctx->system.mounted && count < max_ids) {
        ids[count++] = MTP_STORAGE_NAND_SYSTEM;
    }

    if (ctx->album.mounted && count < max_ids) {
        ids[count++] = MTP_STORAGE_ALBUM;
    }

    return count;
}

bool mtpStorageGetInfo(MtpStorageContext* ctx, u32 storage_id, MtpStorageInfo* out) {
    if (storage_id == MTP_STORAGE_SDCARD && ctx->sdcard.mounted) {
        *out = ctx->sdcard;
        return true;
    }
    if (storage_id == MTP_STORAGE_USER && ctx->user.mounted) {
        *out = ctx->user;
        return true;
    }
    if (storage_id == MTP_STORAGE_NAND_SYSTEM && ctx->system.mounted) {
        *out = ctx->system;
        return true;
    }
    if (storage_id == MTP_STORAGE_ALBUM && ctx->album.mounted) {
        *out = ctx->album;
        return true;
    }
    return false;
}

static bool folder_is_scanned(MtpStorageContext* ctx, u32 storage_id, u32 folder_handle) {
    u32 key = (storage_id & 0xFFFF) ^ folder_handle;

    for (u32 i = 0; i < ctx->scanned_folder_count; i++) {
        if (ctx->scanned_folders[i] == key) {
            return true;
        }
    }
    return false;
}

static void mark_folder_scanned(MtpStorageContext* ctx, u32 storage_id, u32 folder_handle) {
    if (ctx->scanned_folder_count >= MTP_MAX_SCANNED_FOLDERS) {
        u32 half = MTP_MAX_SCANNED_FOLDERS / 2;
        memmove(ctx->scanned_folders, ctx->scanned_folders + half,
                half * sizeof(u32));
        ctx->scanned_folder_count = half;
        LOG_DEBUG("Scanned folder cache eviction");
    }

    u32 key = (storage_id & 0xFFFF) ^ folder_handle;
    ctx->scanned_folders[ctx->scanned_folder_count++] = key;
}

static void ensure_folder_scanned(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle) {
    LOG_DEBUG("ensure_folder_scanned: storage=0x%08X, parent=0x%08X", storage_id, parent_handle);

    if (storage_id == MTP_STORAGE_NAND_SYSTEM) {
        if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
            if (!folder_is_scanned(ctx, storage_id, 0xFFFFFFFF)) {
                const char* base_path = mtpStorageGetBasePath(ctx, storage_id);
                if (base_path) {
                    LOG_DEBUG("Scanning SYSTEM root: %s", base_path);
                    scan_directory_shallow(ctx, storage_id, 0xFFFFFFFF, base_path);
                    mark_folder_scanned(ctx, storage_id, 0xFFFFFFFF);
                }
            }
        } else {
            if (!folder_is_scanned(ctx, storage_id, parent_handle)) {
                for (u32 i = 0; i < ctx->object_count; i++) {
                    MtpObject* obj = &ctx->objects[i];
                    if (obj->handle == parent_handle && obj->object_type == MTP_OBJECT_TYPE_FOLDER) {
                        LOG_DEBUG("Scanning SYSTEM subdir: %s", obj->full_path);
                        scan_directory_shallow(ctx, obj->storage_id, parent_handle, obj->full_path);
                        mark_folder_scanned(ctx, obj->storage_id, parent_handle);
                        return;
                    }
                }
            }
        }
        return;
    }

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        bool has_objects = false;
        for (u32 i = 0; i < ctx->object_count; i++) {
            if (ctx->objects[i].storage_id == storage_id &&
                ctx->objects[i].parent_handle == 0xFFFFFFFF) {
                has_objects = true;
                break;
            }
        }

        if (!has_objects) {
            const char* base_path = mtpStorageGetBasePath(ctx, storage_id);
            if (base_path) {
                LOG_DEBUG("Scanning root of storage 0x%08X: %s (no objects found)", storage_id, base_path);
                scan_directory_shallow(ctx, storage_id, 0xFFFFFFFF, base_path);
                mark_folder_scanned(ctx, storage_id, 0xFFFFFFFF);
            }
        } else {
            LOG_DEBUG("Root already has objects for storage 0x%08X, skipping scan", storage_id);
        }
        return;
    }

    u32 actual_storage = get_storage_from_handle(parent_handle);
    if (actual_storage == 0xFFFFFFFF) {
        actual_storage = storage_id;
    }

    if (folder_is_scanned(ctx, actual_storage, parent_handle)) {
        LOG_DEBUG("Folder already scanned: 0x%08X", parent_handle);
        return;
    }

    LOG_DEBUG("Need to scan folder: 0x%08X, object_count=%u", parent_handle, ctx->object_count);

    for (u32 i = 0; i < ctx->object_count; i++) {
        MtpObject* obj = &ctx->objects[i];
        if (obj->handle == parent_handle && obj->object_type == MTP_OBJECT_TYPE_FOLDER) {
            LOG_DEBUG("Lazy loading: %s (storage 0x%08X)", obj->full_path, obj->storage_id);
            scan_directory_shallow(ctx, obj->storage_id, parent_handle, obj->full_path);
            mark_folder_scanned(ctx, obj->storage_id, parent_handle);
            return;
        }
    }

    LOG_DEBUG("Folder not found in cache: 0x%08X", parent_handle);
}

u32 mtpStorageGetObjectCount(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle) {
    u32 effective_storage = storage_id;
    if (parent_handle != 0 && parent_handle != 0xFFFFFFFF) {
        u32 handle_storage = get_storage_from_handle(parent_handle);
        if (handle_storage != 0xFFFFFFFF) {
            effective_storage = handle_storage;
        }
    }

    if (effective_storage == 0xFFFFFFFF && (parent_handle == 0 || parent_handle == 0xFFFFFFFF)) {
        if (ctx->sdcard.mounted) ensure_folder_scanned(ctx, MTP_STORAGE_SDCARD, parent_handle);
        if (ctx->user.mounted) ensure_folder_scanned(ctx, MTP_STORAGE_USER, parent_handle);
        if (ctx->system.mounted) ensure_folder_scanned(ctx, MTP_STORAGE_NAND_SYSTEM, parent_handle);
    } else if (effective_storage != 0xFFFFFFFF) {
        ensure_folder_scanned(ctx, effective_storage, parent_handle);
    }

    u32 count = 0;

    for (u32 i = 0; i < ctx->object_count; i++) {
        MtpObject* obj = &ctx->objects[i];

        if (effective_storage != 0xFFFFFFFF && obj->storage_id != effective_storage) continue;

        if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
            if (obj->parent_handle != 0xFFFFFFFF) continue;
        } else {
            if (obj->parent_handle != parent_handle) continue;
        }

        count++;
    }

    return count;
}

u32 mtpStorageEnumObjects(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                          u32* handles, u32 max_handles) {
    u32 effective_storage = storage_id;
    if (parent_handle != 0 && parent_handle != 0xFFFFFFFF) {
        u32 handle_storage = get_storage_from_handle(parent_handle);
        if (handle_storage != 0xFFFFFFFF) {
            effective_storage = handle_storage;
        }
    }

    if (effective_storage == 0xFFFFFFFF && (parent_handle == 0 || parent_handle == 0xFFFFFFFF)) {
        if (ctx->sdcard.mounted) ensure_folder_scanned(ctx, MTP_STORAGE_SDCARD, parent_handle);
        if (ctx->user.mounted) ensure_folder_scanned(ctx, MTP_STORAGE_USER, parent_handle);
        if (ctx->system.mounted) ensure_folder_scanned(ctx, MTP_STORAGE_NAND_SYSTEM, parent_handle);
    } else if (effective_storage != 0xFFFFFFFF) {
        ensure_folder_scanned(ctx, effective_storage, parent_handle);
    }

    u32 count = 0;

    for (u32 i = 0; i < ctx->object_count && count < max_handles; i++) {
        MtpObject* obj = &ctx->objects[i];

        if (effective_storage != 0xFFFFFFFF && obj->storage_id != effective_storage) continue;

        if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
            if (obj->parent_handle != 0xFFFFFFFF) continue;
        } else {
            if (obj->parent_handle != parent_handle) continue;
        }

        handles[count++] = obj->handle;
    }

    return count;
}

bool mtpStorageGetObject(MtpStorageContext* ctx, u32 handle, MtpObject* out) {
    for (u32 i = 0; i < ctx->object_count; i++) {
        if (ctx->objects[i].handle == handle) {
            *out = ctx->objects[i];

            if (out->object_type == MTP_OBJECT_TYPE_FILE && out->size == 0) {
                struct stat st;
                if (stat(out->full_path, &st) == 0) {
                    out->size = st.st_size;
                    ctx->objects[i].size = st.st_size;
                }
            }

            return true;
        }
    }
    return false;
}

s64 mtpStorageReadObject(MtpStorageContext* ctx, u32 handle, u64 offset, void* buffer, u64 size) {
    MtpObject obj;
    if (!mtpStorageGetObject(ctx, handle, &obj)) {
        return -1;
    }

    if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
        return -1;
    }

    FILE* f = fopen(obj.full_path, "rb");
    if (!f) {
#if STORAGE_DEBUG
        LOG_WARN("Failed to open file: %s", obj.full_path);
#endif
        return -1;
    }

    setvbuf(f, NULL, _IOFBF, 256 * 1024);

    if (offset > 0) {
        fseek(f, offset, SEEK_SET);
    }

    size_t read = fread(buffer, 1, size, f);
    fclose(f);

    return (s64)read;
}

s64 mtpStorageWriteObject(MtpStorageContext* ctx, u32 handle, u64 offset, const void* buffer, u64 size) {
    MtpObject obj;
    if (!mtpStorageGetObject(ctx, handle, &obj)) {
        return -1;
    }

    if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
        return -1;
    }

    FILE* f = fopen(obj.full_path, offset == 0 ? "wb" : "r+b");
    if (!f) {
        f = fopen(obj.full_path, "wb");
        if (!f) {
#if STORAGE_DEBUG
            LOG_WARN("Failed to open for writing: %s", obj.full_path);
#endif
            return -1;
        }
    }

    setvbuf(f, NULL, _IOFBF, 256 * 1024);

    if (offset > 0) {
        fseek(f, offset, SEEK_SET);
    }

    size_t written = fwrite(buffer, 1, size, f);
    fclose(f);

#if STORAGE_DEBUG
    LOG_DEBUG("Wrote %zu bytes to %s", written, obj.full_path);
#endif

    return (s64)written;
}

struct MtpFileHandle {
    int fd;
    u64 file_size;
};

MtpFileHandle* mtpStorageOpenRead(MtpStorageContext* ctx, u32 handle) {
    MtpObject obj;
    if (!mtpStorageGetObject(ctx, handle, &obj)) {
        return nullptr;
    }

    if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
        return nullptr;
    }

    int fd = open(obj.full_path, O_RDONLY);
    if (fd < 0) {
        return nullptr;
    }

    MtpFileHandle* fh = (MtpFileHandle*)malloc(sizeof(MtpFileHandle));
    if (!fh) {
        close(fd);
        return nullptr;
    }

    fh->fd = fd;
    fh->file_size = obj.size;
    return fh;
}

s64 mtpStorageReadFile(MtpFileHandle* fh, void* buffer, u64 size) {
    if (!fh || fh->fd < 0) {
        return -1;
    }

    u8* buf = (u8*)buffer;
    u64 total = 0;
    while (total < size) {
        ssize_t r = read(fh->fd, buf + total, size - total);
        if (r <= 0) break;  // EOF or error
        total += r;
    }
    return (s64)total;
}

void mtpStorageCloseFile(MtpFileHandle* fh) {
    if (fh) {
        if (fh->fd >= 0) {
            close(fh->fd);
        }
        free(fh);
    }
}

MtpFileHandle* mtpStorageOpenWrite(MtpStorageContext* ctx, u32 handle) {
    MtpObject obj;
    if (!mtpStorageGetObject(ctx, handle, &obj)) {
        return nullptr;
    }

    if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
        return nullptr;
    }

    int fd = open(obj.full_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return nullptr;
    }

    // Pre-allocate FAT32 clusters upfront to avoid metadata extension stalls mid-write
    if (obj.size > 0) {
        off_t end = (off_t)obj.size - 1;
        if (lseek(fd, end, SEEK_SET) == end) {
            u8 zero = 0;
            write(fd, &zero, 1);
            lseek(fd, 0, SEEK_SET);
        }
    }

    MtpFileHandle* fh = (MtpFileHandle*)malloc(sizeof(MtpFileHandle));
    if (!fh) {
        close(fd);
        return nullptr;
    }

    fh->fd = fd;
    fh->file_size = obj.size;
    return fh;
}

s64 mtpStorageWriteFile(MtpFileHandle* fh, const void* buffer, u64 size) {
    if (!fh || fh->fd < 0) {
        return -1;
    }

    const u8* buf = (const u8*)buffer;
    u64 total = 0;
    while (total < size) {
        ssize_t w = write(fh->fd, buf + total, size - total);
        if (w <= 0) break;  // error
        total += w;
    }
    return (s64)total;
}

void mtpStorageFlushFile(MtpFileHandle* fh) {
    if (fh && fh->fd >= 0) {
        fsync(fh->fd);
    }
}

static int find_object_index(MtpStorageContext* ctx, u32 handle) {
    for (u32 i = 0; i < ctx->object_count; i++) {
        if (ctx->objects[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

static void remove_object_at_index(MtpStorageContext* ctx, u32 index) {
    if (index >= ctx->object_count) return;

    for (u32 i = index; i < ctx->object_count - 1; i++) {
        ctx->objects[i] = ctx->objects[i + 1];
    }
    ctx->object_count--;
}

u32 mtpStorageCreateObject(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                           const char* filename, u16 format, u64 size) {
    if (!ctx->objects || ctx->object_count >= ctx->max_objects) {
#if STORAGE_DEBUG
        LOG_WARN("Cannot create object: cache full");
#endif
        return 0;
    }

    char full_path[MTP_MAX_PATH];

    if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
        const char* base = mtpStorageGetBasePath(ctx, storage_id);
        if (!base) return 0;
        int written = snprintf(full_path, sizeof(full_path), "%s%s", base, filename);
        if (written < 0 || (size_t)written >= sizeof(full_path)) {
#if STORAGE_DEBUG
            LOG_WARN("Path too long for root object: %s", filename);
#endif
            return 0;
        }
    } else {
        MtpObject parent;
        if (!mtpStorageGetObject(ctx, parent_handle, &parent)) {
            return 0;
        }
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", parent.full_path, filename);
        if (written < 0 || (size_t)written >= sizeof(full_path)) {
#if STORAGE_DEBUG
            LOG_WARN("Path too long: %s/%s", parent.full_path, filename);
#endif
            return 0;
        }
    }

    bool is_folder = (format == MTP_FORMAT_ASSOCIATION);

    if (is_folder) {
        if (mkdir(full_path, 0755) != 0) {
#if STORAGE_DEBUG
            LOG_WARN("Failed to create dir: %s", full_path);
#endif
            return 0;
        }
#if STORAGE_DEBUG
        LOG_INFO("Created dir: %s", full_path);
#endif
    } else {
        FILE* f = fopen(full_path, "wb");
        if (!f) {
#if STORAGE_DEBUG
            LOG_WARN("Failed to create file: %s", full_path);
#endif
            return 0;
        }
        fclose(f);
#if STORAGE_DEBUG
        LOG_INFO("Created file: %s", full_path);
#endif
    }

    u32 handle = add_object(ctx, storage_id, parent_handle, full_path, filename,
                            is_folder ? MTP_OBJECT_TYPE_FOLDER : MTP_OBJECT_TYPE_FILE, size);

    return handle;
}

static bool delete_directory_recursive(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) return false;

    struct dirent* entry;
    char full_path[MTP_MAX_PATH];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                delete_directory_recursive(full_path);
            } else {
                remove(full_path);
            }
        }
    }

    closedir(dir);
    return rmdir(path) == 0;
}

bool mtpStorageDeleteObject(MtpStorageContext* ctx, u32 handle) {
    int idx = find_object_index(ctx, handle);
    if (idx < 0) {
        return false;
    }

    MtpObject* obj = &ctx->objects[idx];
    bool success = false;

    if (obj->object_type == MTP_OBJECT_TYPE_FOLDER) {
        success = delete_directory_recursive(obj->full_path);

        if (success) {
            u32 i = 0;
            while (i < ctx->object_count) {
                if (ctx->objects[i].parent_handle == handle) {
                    remove_object_at_index(ctx, i);
                } else {
                    i++;
                }
            }
        }
    } else {
        success = (remove(obj->full_path) == 0);
    }

    if (success) {
#if STORAGE_DEBUG
        LOG_INFO("Deleted: %s", obj->full_path);
#endif
        idx = find_object_index(ctx, handle);
        if (idx >= 0) {
            remove_object_at_index(ctx, idx);
        }
    } else {
#if STORAGE_DEBUG
        LOG_WARN("Failed to delete: %s", obj->full_path);
#endif
    }

    return success;
}

void mtpStorageUpdateObjectSize(MtpStorageContext* ctx, u32 handle, u64 new_size) {
    mutexLock(&ctx->cache_mutex);
    int idx = find_object_index(ctx, handle);
    if (idx >= 0) {
        ctx->objects[idx].size = new_size;
    }
    mutexUnlock(&ctx->cache_mutex);
}

static void scan_directory_recursive(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                                     const char* path, int depth) {
    if (depth > 6 || ctx->index_thread_stop) {
        return;
    }

    DIR* dir = opendir(path);
    if (!dir) {
        return;
    }

    size_t path_len = strlen(path);
    bool has_slash = (path_len > 0 && path[path_len - 1] == '/');

    struct dirent* entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL && !ctx->index_thread_stop) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (count >= 1000) {
            break;
        }

        char full_path[MTP_MAX_PATH];
        if (has_slash) {
            snprintf(full_path, sizeof(full_path), "%s%s", path, entry->d_name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        }

        bool is_dir = false;
        u64 file_size = 0;

#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type != DT_UNKNOWN) {
            is_dir = (entry->d_type == DT_DIR);
        } else
#endif
        {
            struct stat st;
            if (stat(full_path, &st) != 0) {
                continue;
            }
            is_dir = S_ISDIR(st.st_mode);
            file_size = st.st_size;
        }

        if (is_dir) {
            u32 folder_handle = add_object(ctx, storage_id, parent_handle,
                                           full_path, entry->d_name,
                                           MTP_OBJECT_TYPE_FOLDER, 0);
            if (folder_handle != 0) {
                scan_directory_recursive(ctx, storage_id, folder_handle, full_path, depth + 1);
            }
        } else {
            add_object(ctx, storage_id, parent_handle,
                       full_path, entry->d_name,
                       MTP_OBJECT_TYPE_FILE, file_size);
        }
        count++;

        if (count % 20 == 0) {
            svcSleepThread(1000000ULL);
        }
    }

    closedir(dir);
}

static void index_thread_func(void* arg) {
    MtpStorageContext* ctx = (MtpStorageContext*)arg;

    LOG_INFO("Background indexing started");
    ctx->indexing_in_progress = true;

    if (ctx->sdcard.mounted && !ctx->index_thread_stop) {
        LOG_DEBUG("Indexing SD card...");
        scan_directory_recursive(ctx, MTP_STORAGE_SDCARD, 0xFFFFFFFF, "sdmc:/", 0);
        LOG_DEBUG("SD card indexing complete. Objects: %u", ctx->object_count);
    }

    if (!ctx->index_thread_stop) svcSleepThread(10000000ULL);

    if (ctx->user.mounted && !ctx->index_thread_stop) {
        const char* base = mtpStorageGetBasePath(ctx, MTP_STORAGE_USER);
        if (base) {
            LOG_DEBUG("Indexing internal storage (%s)...", base);
            scan_directory_recursive(ctx, MTP_STORAGE_USER, 0xFFFFFFFF, base, 0);
            LOG_DEBUG("Internal storage indexing complete. Objects: %u", ctx->object_count);
        }
    }

    if (!ctx->index_thread_stop) svcSleepThread(10000000ULL);

    if (ctx->system.mounted && !ctx->index_thread_stop) {
        const char* base = mtpStorageGetBasePath(ctx, MTP_STORAGE_NAND_SYSTEM);
        if (base) {
            LOG_DEBUG("Indexing SYSTEM storage (%s)...", base);
            scan_directory_recursive(ctx, MTP_STORAGE_NAND_SYSTEM, 0xFFFFFFFF, base, 0);
            LOG_DEBUG("SYSTEM storage indexing complete. Objects: %u", ctx->object_count);
        }
    }

    if (!ctx->index_thread_stop) svcSleepThread(10000000ULL);

    if (ctx->album.mounted && !ctx->index_thread_stop) {
        const char* album_path = ctx->album_on_nand ? "user:/Nintendo/Album/" : "sdmc:/Nintendo/Album/";
        LOG_DEBUG("Indexing Album (%s)...", ctx->album_on_nand ? "NAND" : "SD");
        scan_directory_recursive(ctx, MTP_STORAGE_ALBUM, 0xFFFFFFFF, album_path, 0);
        LOG_DEBUG("Album indexing complete. Objects: %u", ctx->object_count);
    }

    ctx->indexing_in_progress = false;
    ctx->index_thread_running = false;
    LOG_INFO("Background indexing complete. Total objects: %u", ctx->object_count);
}

void mtpStorageStartBackgroundIndex(MtpStorageContext* ctx) {
    if (ctx->index_thread_running) {
        LOG_WARN("Background indexing already running");
        return;
    }

    ctx->index_thread_stop = false;
    ctx->index_thread_running = true;

    Result rc = threadCreate(&ctx->index_thread, index_thread_func, ctx, NULL, 0x40000, 0x2C, -2);
    if (R_FAILED(rc)) {
        LOG_ERROR("Failed to create index thread: 0x%08X", rc);
        ctx->index_thread_running = false;
        return;
    }

    rc = threadStart(&ctx->index_thread);
    if (R_FAILED(rc)) {
        LOG_ERROR("Failed to start index thread: 0x%08X", rc);
        threadClose(&ctx->index_thread);
        ctx->index_thread_running = false;
        return;
    }

    LOG_INFO("Background indexing thread started");
}

void mtpStorageStopBackgroundIndex(MtpStorageContext* ctx) {
    if (!ctx->index_thread_running) return;

    LOG_DEBUG("Stopping background indexing...");
    ctx->index_thread_stop = true;

    for (int i = 0; i < 50 && ctx->indexing_in_progress; i++) {
        svcSleepThread(100000000ULL);
    }

    threadWaitForExit(&ctx->index_thread);
    threadClose(&ctx->index_thread);
    ctx->index_thread_running = false;

    LOG_DEBUG("Background indexing stopped");
}

bool mtpStorageIsIndexing(MtpStorageContext* ctx) {
    return ctx->indexing_in_progress;
}
