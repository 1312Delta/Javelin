// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include "mtp_storage.h"
#include "dump/game_dump.h"

#ifdef __cplusplus
extern "C" {
#endif

// Check if a storage ID is dump virtual storage
bool dumpIsVirtualStorage(u32 storage_id);

// Check if a handle is a dump virtual handle
bool dumpIsVirtualHandle(u32 handle);

// Get storage info for dump virtual storage
bool dumpGetStorageInfo(DumpContext* ctx, u32 storage_id, MtpStorageInfo* out);

// Get object count for dump folder
u32 dumpGetObjectCount(DumpContext* ctx, u32 storage_id, u32 parent_handle);

// Enumerate objects in dump folders
u32 dumpEnumObjects(DumpContext* ctx, u32 storage_id, u32 parent_handle, u32* handles, u32 max);

// Get object info for a dump handle
bool dumpGetObjectInfo(DumpContext* ctx, u32 handle, MtpObject* out);

// Read virtual NSP data (streaming, by MTP handle)
s64 dumpReadObject(DumpContext* ctx, u32 handle, u64 offset, void* buffer, u64 size);

#ifdef __cplusplus
}
#endif
