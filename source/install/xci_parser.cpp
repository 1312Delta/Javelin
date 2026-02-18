// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/xci_parser.h"
#include "mtp_log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

static bool read_hfs0_partition(FILE* fp, u64 partition_offset,
                                Hfs0Header* header,
                                Hfs0FileEntry** entries,
                                char** string_table,
                                u64* data_offset) {
    if (fseek(fp, partition_offset, SEEK_SET) != 0) {
        return false;
    }

    if (fread(header, sizeof(Hfs0Header), 1, fp) != 1) {
        return false;
    }

    if (header->magic != HFS0_MAGIC) {
        return false;
    }

    if (header->file_count == 0 || header->file_count > XCI_MAX_FILES) {
        return false;
    }

    size_t entries_size = sizeof(Hfs0FileEntry) * header->file_count;
    *entries = (Hfs0FileEntry*)malloc(entries_size);
    if (!*entries) {
        return false;
    }

    if (fread(*entries, entries_size, 1, fp) != 1) {
        free(*entries);
        *entries = NULL;
        return false;
    }

    if (header->string_table_size > 0) {
        *string_table = (char*)malloc(header->string_table_size);
        if (!*string_table) {
            free(*entries);
            *entries = NULL;
            return false;
        }

        if (fread(*string_table, header->string_table_size, 1, fp) != 1) {
            free(*string_table);
            free(*entries);
            *string_table = NULL;
            *entries = NULL;
            return false;
        }
    }

    *data_offset = partition_offset + sizeof(Hfs0Header) +
                   (sizeof(Hfs0FileEntry) * header->file_count) +
                   header->string_table_size;

    return true;
}

bool xciOpen(XciContext* ctx, const char* path) {
    if (!ctx || !path) return false;

    memset(ctx, 0, sizeof(XciContext));

    ctx->fp = fopen(path, "rb");
    if (!ctx->fp) {
        LOG_ERROR("XCI: Failed to open file: %s", path);
        return false;
    }

    u64 root_data_offset;
    if (!read_hfs0_partition(ctx->fp, XCI_HEADER_OFFSET,
                             &ctx->root_header,
                             &ctx->root_entries,
                             &ctx->root_string_table,
                             &root_data_offset)) {
        LOG_ERROR("XCI: Failed to read root HFS0 partition");
        fclose(ctx->fp);
        return false;
    }

    LOG_INFO("XCI: Root partition has %u entries", ctx->root_header.file_count);

    ctx->has_secure = false;
    for (u32 i = 0; i < ctx->root_header.file_count; i++) {
        u32 string_offset = ctx->root_entries[i].string_offset;
        if (string_offset >= ctx->root_header.string_table_size) continue;

        const char* partition_name = ctx->root_string_table + string_offset;

        if (strcmp(partition_name, XCI_PARTITION_SECURE) == 0) {
            ctx->secure_offset = root_data_offset + ctx->root_entries[i].offset;

            u64 secure_data_offset;
            if (read_hfs0_partition(ctx->fp, ctx->secure_offset,
                                   &ctx->secure_header,
                                   &ctx->secure_entries,
                                   &ctx->secure_string_table,
                                   &secure_data_offset)) {
                ctx->secure_data_offset = secure_data_offset;
                LOG_INFO("XCI: Secure partition opened - %u files",
                         ctx->secure_header.file_count);
                ctx->has_secure = true;
            }
            break;
        }
    }

    if (!ctx->has_secure) {
        LOG_WARN("XCI: No secure partition found");
        xciClose(ctx);
        return false;
    }

    ctx->is_open = true;
    return true;
}

void xciClose(XciContext* ctx) {
    if (!ctx) return;

    if (ctx->fp) {
        fclose(ctx->fp);
        ctx->fp = NULL;
    }

    if (ctx->root_entries) {
        free(ctx->root_entries);
        ctx->root_entries = NULL;
    }

    if (ctx->root_string_table) {
        free(ctx->root_string_table);
        ctx->root_string_table = NULL;
    }

    if (ctx->secure_entries) {
        free(ctx->secure_entries);
        ctx->secure_entries = NULL;
    }

    if (ctx->secure_string_table) {
        free(ctx->secure_string_table);
        ctx->secure_string_table = NULL;
    }

    ctx->is_open = false;
    ctx->has_secure = false;
}

u32 xciGetFileCount(XciContext* ctx) {
    if (!ctx || !ctx->is_open || !ctx->has_secure) return 0;
    return ctx->secure_header.file_count;
}

const char* xciGetFilename(XciContext* ctx, u32 index) {
    if (!ctx || !ctx->is_open || !ctx->has_secure) return NULL;
    if (index >= ctx->secure_header.file_count) return NULL;

    u32 string_offset = ctx->secure_entries[index].string_offset;
    if (string_offset >= ctx->secure_header.string_table_size) return NULL;

    return ctx->secure_string_table + string_offset;
}

u64 xciGetFileSize(XciContext* ctx, u32 index) {
    if (!ctx || !ctx->is_open || !ctx->has_secure) return 0;
    if (index >= ctx->secure_header.file_count) return 0;
    return ctx->secure_entries[index].size;
}

bool xciIsNcaFile(XciContext* ctx, u32 index) {
    const char* filename = xciGetFilename(ctx, index);
    if (!filename) return false;

    size_t len = strlen(filename);
    if (len < 5) return false;

    return (strcasecmp(filename + len - 4, ".nca") == 0);
}

bool xciExtractFile(XciContext* ctx, u32 index, const char* dest_path) {
    if (!ctx || !ctx->is_open || !ctx->has_secure || !dest_path) return false;
    if (index >= ctx->secure_header.file_count) return false;

    Hfs0FileEntry* entry = &ctx->secure_entries[index];
    u64 file_offset = ctx->secure_data_offset + entry->offset;

    if (fseek(ctx->fp, file_offset, SEEK_SET) != 0) {
        LOG_ERROR("XCI: Failed to seek to file offset 0x%lX", file_offset);
        return false;
    }

    FILE* dest = fopen(dest_path, "wb");
    if (!dest) {
        LOG_ERROR("XCI: Failed to create destination file: %s", dest_path);
        return false;
    }

    u8 buffer[64 * 1024];
    u64 remaining = entry->size;
    bool success = true;

    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;

        if (fread(buffer, 1, chunk, ctx->fp) != chunk) {
            LOG_ERROR("XCI: Read error during extraction");
            success = false;
            break;
        }

        if (fwrite(buffer, 1, chunk, dest) != chunk) {
            LOG_ERROR("XCI: Write error during extraction");
            success = false;
            break;
        }

        remaining -= chunk;
    }

    fclose(dest);

    if (!success) {
        remove(dest_path);
    }

    return success;
}

s64 xciReadFile(XciContext* ctx, u32 index, u64 offset, void* buffer, u64 size) {
    if (!ctx || !ctx->is_open || !ctx->has_secure || !buffer) return -1;
    if (index >= ctx->secure_header.file_count) return -1;

    Hfs0FileEntry* entry = &ctx->secure_entries[index];

    if (offset >= entry->size) return 0;
    if (offset + size > entry->size) {
        size = entry->size - offset;
    }

    u64 file_offset = ctx->secure_data_offset + entry->offset + offset;

    if (fseek(ctx->fp, file_offset, SEEK_SET) != 0) {
        return -1;
    }

    size_t read_bytes = fread(buffer, 1, size, ctx->fp);
    return (s64)read_bytes;
}

s32 xciFindFile(XciContext* ctx, const char* filename) {
    if (!ctx || !ctx->is_open || !ctx->has_secure || !filename) return -1;

    for (u32 i = 0; i < ctx->secure_header.file_count; i++) {
        const char* name = xciGetFilename(ctx, i);
        if (name && strcmp(name, filename) == 0) {
            return (s32)i;
        }
    }

    return -1;
}
