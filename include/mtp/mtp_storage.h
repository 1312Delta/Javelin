// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_STORAGE_SDCARD         0x00010001
#define MTP_STORAGE_NAND_USER      0x00010002
#define MTP_STORAGE_NAND_SYSTEM    0x00010003
#define MTP_STORAGE_INSTALLED      0x00010004
#define MTP_STORAGE_SD_INSTALL     0x00010005
#define MTP_STORAGE_NAND_INSTALL   0x00010006
#define MTP_STORAGE_SAVES          0x00010007
#define MTP_STORAGE_ALBUM          0x00010008
#define MTP_STORAGE_GAMECARD       0x00010009
#define MTP_STORAGE_USER           MTP_STORAGE_NAND_USER
#define MTP_STORAGE_DUMP           0x00040001

#define MTP_STORAGE_INSTALL_SD     MTP_STORAGE_SD_INSTALL
#define MTP_STORAGE_INSTALL_NAND   MTP_STORAGE_NAND_INSTALL

#define MTP_HANDLE_MASK            0x0FFF0000
#define MTP_HANDLE_SDCARD_BASE     0x00010000
#define MTP_HANDLE_NAND_USER_BASE  0x00020000
#define MTP_HANDLE_NAND_SYSTEM_BASE 0x00030000
#define MTP_HANDLE_INSTALLED_BASE  0x00040000
#define MTP_HANDLE_SD_INSTALL_BASE  0x00050000
#define MTP_HANDLE_NAND_INSTALL_BASE 0x00060000
#define MTP_HANDLE_SAVES_BASE       0x00070000
#define MTP_HANDLE_ALBUM_BASE       0x00080000
#define MTP_HANDLE_GAMECARD_BASE    0x00090000

#define MTP_HANDLE_INSTALL_SD_BASE  MTP_HANDLE_SD_INSTALL_BASE
#define MTP_HANDLE_INSTALL_NAND_BASE MTP_HANDLE_NAND_INSTALL_BASE

#define MTP_MAX_OBJECTS             50000
#define MTP_MAX_FILENAME            256
#define MTP_MAX_PATH               1024
#define MTP_MAX_SCANNED_FOLDERS     4096

#define MTP_OBJECT_TYPE_UNDEFINED   0x0000
#define MTP_OBJECT_TYPE_FOLDER       0x0001
#define MTP_OBJECT_TYPE_FILE         0x0000

#define MTP_FORMAT_UNDEFINED        0x3000
#define MTP_FORMAT_ASSOCIATION      0x3001
#define MTP_FORMAT_JPEG             0x3801
#define MTP_FORMAT_PNG              0x3807
#define MTP_FORMAT_BMP              0x3804
#define MTP_FORMAT_MP3              0xB903
#define MTP_FORMAT_WAV              0xB902
#define MTP_FORMAT_AVI              0xB982
#define MTP_FORMAT_MPEG             0xB983
#define MTP_FORMAT_TEXT             0xB904
#define MTP_FORMAT_HTML             0xB905

typedef struct MtpStorageContext MtpStorageContext;

typedef struct {
    u32 handle;
    u32 storage_id;
    u32 parent_handle;
    u8 object_type;
    u16 format;
    u64 size;
    char filename[MTP_MAX_FILENAME];
    char full_path[MTP_MAX_PATH];
} MtpObject;

typedef struct {
    u32 storage_id;
    u16 storage_type;
    u16 filesystem_type;
    u16 access_capability;
    u64 max_capacity;
    u64 free_space;
    char description[64];
    char volume_label[32];
    bool mounted;
} MtpStorageInfo;

struct MtpStorageContext {
    MtpObject* objects;
    u32 object_count;
    u32 max_objects;
    u32 next_handle;
    Mutex cache_mutex;

    MtpStorageInfo sdcard;
    MtpStorageInfo user;
    MtpStorageInfo system;
    MtpStorageInfo album;

    bool initialized;

    Thread index_thread;
    bool index_thread_running;
    bool indexing;
    bool index_should_stop;
    bool indexing_in_progress;

    bool index_thread_stop;
    u32 object_count_compat;

    u32 scanned_folder_count;
    u32 scanned_folders[4096];

    bool user_fs_mounted;
    bool system_fs_mounted;
    FsFileSystem user_fs;
    FsFileSystem system_fs;

    bool album_on_nand;  // true if album is on NAND USER, false if on SD

    bool sdcard_needs_scan;
    bool user_needs_scan;
    bool system_needs_scan;
};

u16 mtpStorageGetFormat(const char* filename);
Result mtpStorageInit(MtpStorageContext* ctx);
void mtpStorageExit(MtpStorageContext* ctx);
const char* mtpStorageGetBasePath(MtpStorageContext* ctx, u32 storage_id);
void mtpStorageRefresh(MtpStorageContext* ctx, u32 storage_id);
u32 mtpStorageGetIds(MtpStorageContext* ctx, u32* ids, u32 max_ids);
bool mtpStorageGetInfo(MtpStorageContext* ctx, u32 storage_id, MtpStorageInfo* out);
u32 mtpStorageGetObjectCount(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle);
u32 mtpStorageEnumObjects(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                         u32* handles, u32 max_handles);
bool mtpStorageGetObject(MtpStorageContext* ctx, u32 handle, MtpObject* out);
s64 mtpStorageReadObject(MtpStorageContext* ctx, u32 handle, u64 offset, void* buffer, u64 size);
s64 mtpStorageWriteObject(MtpStorageContext* ctx, u32 handle, u64 offset, const void* buffer, u64 size);

// Streaming file handle - keeps file open across multiple reads/writes
typedef struct MtpFileHandle MtpFileHandle;
MtpFileHandle* mtpStorageOpenRead(MtpStorageContext* ctx, u32 handle);
s64 mtpStorageReadFile(MtpFileHandle* fh, void* buffer, u64 size);
void mtpStorageCloseFile(MtpFileHandle* fh);

MtpFileHandle* mtpStorageOpenWrite(MtpStorageContext* ctx, u32 handle);
s64 mtpStorageWriteFile(MtpFileHandle* fh, const void* buffer, u64 size);
void mtpStorageFlushFile(MtpFileHandle* fh);

u32 mtpStorageCreateObject(MtpStorageContext* ctx, u32 storage_id, u32 parent_handle,
                          const char* filename, u16 format, u64 size);
bool mtpStorageDeleteObject(MtpStorageContext* ctx, u32 handle);
void mtpStorageUpdateObjectSize(MtpStorageContext* ctx, u32 handle, u64 new_size);
void mtpStorageStartBackgroundIndex(MtpStorageContext* ctx);
void mtpStorageStopBackgroundIndex(MtpStorageContext* ctx);
bool mtpStorageIsIndexing(MtpStorageContext* ctx);

#ifdef __cplusplus
}
#endif
