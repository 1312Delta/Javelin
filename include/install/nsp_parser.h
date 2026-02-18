// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PFS0_MAGIC 0x30534650  // "PFS0" as little-endian u32
#define NSP_MAX_FILES 64
#define NSP_MAX_SPLIT_PARTS 64

typedef struct {
    u32 magic;
    u32 file_count;
    u32 string_table_size;
    u32 _padding;
} __attribute__((packed)) Pfs0Header;

// offsets are relative to the data section start
typedef struct {
    u64 offset;
    u64 size;
    u32 string_offset;
    u32 _padding;
} __attribute__((packed)) Pfs0FileEntry;

typedef struct {
    FILE* fp;
    Pfs0Header header;
    Pfs0FileEntry* entries;
    char* string_table;
    u64 data_offset;
    bool is_open;

    // Split file support (directory with numbered parts: 00, 01, 02, ...)
    bool is_split;
    FILE* split_fps[NSP_MAX_SPLIT_PARTS];
    u64 split_sizes[NSP_MAX_SPLIT_PARTS];
    u32 split_count;
    u64 virtual_pos;
} NspContext;

bool nspOpen(NspContext* ctx, const char* path);
void nspClose(NspContext* ctx);
u32 nspGetFileCount(NspContext* ctx);
const char* nspGetFilename(NspContext* ctx, u32 index);
u64 nspGetFileSize(NspContext* ctx, u32 index);
bool nspGetFileEntry(NspContext* ctx, u32 index, Pfs0FileEntry* out);
bool nspIsNcaFile(NspContext* ctx, u32 index);
bool nspExtractFile(NspContext* ctx, u32 index, const char* dest_path);
s64 nspReadFile(NspContext* ctx, u32 index, u64 offset, void* buffer, u64 size);
s32 nspFindFile(NspContext* ctx, const char* filename);

#ifdef __cplusplus
}
#endif
