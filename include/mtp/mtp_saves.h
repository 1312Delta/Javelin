// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include "mtp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum counts
#define MTP_SAVES_MAX_GAMES         512
#define MTP_SAVES_MAX_USERS         8       // Max accounts on Switch
#define MTP_SAVES_MAX_USER_FOLDERS  4096    // games * users
#define MTP_SAVES_MAX_TYPES         16384   // Save type subfolders
#define MTP_SAVES_MAX_FILES         8192

// Handle scheme for DBI-style hierarchy:
// Category folders (Installed/Not Installed)
#define MTP_HANDLE_SAVES_CATEGORY_START     0x00070001
#define MTP_HANDLE_SAVES_CATEGORY_INSTALLED 0x00070001
#define MTP_HANDLE_SAVES_CATEGORY_NOT_INST  0x00070002
#define MTP_HANDLE_SAVES_CATEGORY_END       0x0007000F

// Game folder handles
#define MTP_HANDLE_SAVES_GAME_START         0x00070010
#define MTP_HANDLE_SAVES_GAME_END           0x000701FF

// User folder handles (per-game user subfolders)
#define MTP_HANDLE_SAVES_USER_START         0x00070200
#define MTP_HANDLE_SAVES_USER_END           0x00070FFF

// Save type handles (Account/Device/BCAT/Cache under user or game)
#define MTP_HANDLE_SAVES_TYPE_START         0x00071000
#define MTP_HANDLE_SAVES_TYPE_END           0x00074FFF

// File handles
#define MTP_HANDLE_SAVES_FILE_START         0x00075000

// User account entry (tracks user subfolders under games)
typedef struct {
    AccountUid uid;
    char username[32];          // From AccountProfileBase.nickname
    u32 handle;                 // Virtual handle for this user folder
    u32 parent_game_handle;     // Which game this user folder belongs to
    u32 game_index;             // Index in games array
    s32 user_index;             // Index in user_uids array
    bool types_scanned;         // Have we scanned save types for this user/game?
} UserFolderEntry;

// Save type subfolder (Account, Device, Cache, BCAT, etc.)
typedef struct {
    u8 save_type;               // FsSaveDataType
    u8 space_id;                // FsSaveDataSpaceId (User, System, SdCache)
    char name[48];              // Display name (e.g., "Account", "Device", "SD_Cache")
    u32 handle;
    u32 parent_handle;          // User folder handle OR game folder (for Device/BCAT)
    u32 game_index;
    s32 user_index;             // Which user this save belongs to (-1 for device-wide)
    s16 cache_index;            // For cache saves (-1 if not cache)
    bool scanned;
    bool mounted;
    FsFileSystem save_fs;
    char mount_name[32];
} SaveTypeEntry;

// Save file entry (files and subdirectories within a save type)
typedef struct {
    char filename[256];
    char full_path[512];
    u64 size;
    bool is_directory;
    bool scanned;               // For directories: have we scanned contents?
    u32 handle;
    u32 parent_handle;          // Handle of parent folder (save type or subdir)
    u32 game_index;             // Which game this belongs to
    u32 type_index;             // Which save type this belongs to
} SaveFileEntry;

// Game save entry (represents a game/DLC/update folder in the saves view)
// Title ID format: 0xTTTTBBBBBBBBBBBB where TTTT=type (0100=app, 0101=DLC, 0102=update)
typedef struct {
    u64 application_id;         // Application ID (base app, DLC, or update)
    char game_name[256];        // Human-readable name (e.g., "Game Name [DLC 1]" or "Game Name [Update]")
    u32 folder_handle;          // MTP handle for this game's folder
    u32 game_index;             // Index in games array
    bool is_installed;          // true if nsGetApplicationControlData succeeded (for base apps)
    u32 category_handle;        // Parent handle (Installed or Not Installed)
    bool users_scanned;         // Have we scanned user saves for this game?
} GameSaveEntry;

// Saves context
typedef struct {
    bool initialized;
    bool acc_initialized;
    bool ns_initialized;

    // All user accounts on the system
    AccountUid user_uids[MTP_SAVES_MAX_USERS];
    char user_names[MTP_SAVES_MAX_USERS][32];
    s32 user_count;

    // Category counts
    u32 installed_game_count;
    u32 not_installed_game_count;

    // List of games with save data
    GameSaveEntry* games;
    u32 game_count;
    u32 max_games;

    // User subfolders per game
    UserFolderEntry* user_folders;
    u32 user_folder_count;
    u32 max_user_folders;
    u32 next_user_handle;

    // Save type subfolders (Account, Device, Cache, etc.)
    SaveTypeEntry* types;
    u32 type_count;
    u32 max_types;
    u32 next_type_handle;

    // Global pool of all files/subdirs across all saves
    SaveFileEntry* files;
    u32 file_count;
    u32 max_files;
    u32 next_file_handle;

    // Thread safety
    Mutex saves_mutex;

    // Refresh tracking
    bool needs_refresh;
    bool refresh_in_progress;  // Prevent concurrent refreshes
    u64 last_refresh_time;
} SavesContext;

// Initialize saves subsystem
Result savesInit(SavesContext* ctx);

// Pre-initialize system services (must be called from main thread before MTP thread starts)
void savesPreInitServices(SavesContext* ctx);

// Refresh saves (must be called from main thread)
void savesRefreshIfNeeded(SavesContext* ctx);

// Cleanup saves subsystem
void savesExit(SavesContext* ctx);

// Check if a storage ID is the saves virtual storage
bool savesIsVirtualStorage(u32 storage_id);

// Check if a handle is a saves virtual handle
bool savesIsVirtualHandle(u32 handle);

// Get storage info for saves virtual storage
bool savesGetStorageInfo(SavesContext* ctx, u32 storage_id, MtpStorageInfo* out);

// Get object count for saves folder
u32 savesGetObjectCount(SavesContext* ctx, u32 storage_id, u32 parent_handle);

// Enumerate objects in saves folders
u32 savesEnumObjects(SavesContext* ctx, u32 storage_id, u32 parent_handle, u32* handles, u32 max_handles);

// Get object info for a saves handle
bool savesGetObjectInfo(SavesContext* ctx, u32 handle, MtpObject* out);

// Read save file data (for backup/download)
s64 savesReadObject(SavesContext* ctx, u32 handle, u64 offset, void* buffer, u64 size);

// Create object in saves (for restore/upload)
u32 savesCreateObject(SavesContext* ctx, u32 storage_id, u32 parent_handle,
                      const char* filename, u16 format, u64 size);

// Write save file data (for restore/upload)
s64 savesWriteObject(SavesContext* ctx, u32 handle, u64 offset, const void* buffer, u64 size);

// Delete save file
bool savesDeleteObject(SavesContext* ctx, u32 handle);

// Commit changes to save data (required after writes)
bool savesCommitObject(SavesContext* ctx, u32 handle);

// Refresh the list of games with save data
Result savesRefresh(SavesContext* ctx);

// Get the game entry for a handle
GameSaveEntry* savesGetGameForHandle(SavesContext* ctx, u32 handle);

#ifdef __cplusplus
}
#endif
