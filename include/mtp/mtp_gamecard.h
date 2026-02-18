// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include "mtp_storage.h"
#include "dump/gamecard_dump.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true if storage_id is the gamecard storage
bool gcIsVirtualStorage(u32 storage_id);

// Returns true if handle is a gamecard virtual handle
bool gcIsVirtualHandle(u32 handle);

// Fill out MTP storage info for the gamecard storage
bool gcGetStorageInfo(GcContext* ctx, u32 storage_id, MtpStorageInfo* out);

// Return object count for a given parent
u32 gcGetObjectCount(GcContext* ctx, u32 storage_id, u32 parent_handle);

// Enumerate object handles
u32 gcEnumObjects(GcContext* ctx, u32 storage_id, u32 parent_handle,
                  u32* handles, u32 max);

// Get object info for a handle
bool gcGetObjectInfo(GcContext* ctx, u32 handle, MtpObject* out);

#ifdef __cplusplus
}
#endif
