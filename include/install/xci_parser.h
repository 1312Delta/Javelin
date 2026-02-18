// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HFS0_MAGIC 0x48465330
#define XCI_HEADER_OFFSET 0x10000
#define XCI_MAX_FILES 64
#define XCI_PARTITION_SECURE "secure"

typedef struct {
    u32 magic;
    u32 file_count;
    u32 string_table_size;
    u32 _padding;
} __attribute__((packed)) Hfs0Header;

typedef struct {
    u64 offset;
    u64 size;
    u32 string_offset;
    u32 _padding;
} __attribute__((packed)) Hfs0FileEntry;

typedef struct {
    FILE* fp;

    // Root partition HFS0
    Hfs0Header root_header;
    Hfs0FileEntry* root_entries;
    char* root_string_table;
    u64 root_data_offset;

    // Secure partition HFS0 (contains game NCAs)
    bool has_secure;
    u64 secure_offset;
    Hfs0Header secure_header;
    Hfs0FileEntry* secure_entries;
    char* secure_string_table;
    u64 secure_data_offset;

    bool is_open;
} XciContext;

bool xciOpen(XciContext* ctx, const char* path);
void xciClose(XciContext* ctx);
u32 xciGetFileCount(XciContext* ctx);
const char* xciGetFilename(XciContext* ctx, u32 index);
u64 xciGetFileSize(XciContext* ctx, u32 index);
bool xciIsNcaFile(XciContext* ctx, u32 index);
bool xciExtractFile(XciContext* ctx, u32 index, const char* dest_path);
s64 xciReadFile(XciContext* ctx, u32 index, u64 offset, void* buffer, u64 size);
s32 xciFindFile(XciContext* ctx, const char* filename);

#ifdef __cplusplus
}
#endif
