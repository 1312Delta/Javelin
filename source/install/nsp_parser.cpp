// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/nsp_parser.h"
#include "mtp_log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>

// --- Split file I/O helpers ---

static bool nsp_is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static bool nsp_open_split(NspContext* ctx, const char* dir_path) {
    ctx->is_split = true;
    ctx->split_count = 0;
    ctx->virtual_pos = 0;

    for (u32 i = 0; i < NSP_MAX_SPLIT_PARTS; i++) {
        char part_path[528];
        snprintf(part_path, sizeof(part_path), "%s/%02u", dir_path, i);

        FILE* fp = fopen(part_path, "rb");
        if (!fp) break;

        fseek(fp, 0, SEEK_END);
        u64 size = (u64)ftell(fp);
        fseek(fp, 0, SEEK_SET);

        ctx->split_fps[i] = fp;
        ctx->split_sizes[i] = size;
        ctx->split_count++;
    }

    if (ctx->split_count == 0) {
        LOG_ERROR("NSP: No split parts found in: %s", dir_path);
        return false;
    }

    // Use first part as the primary fp for header parsing
    ctx->fp = ctx->split_fps[0];

    LOG_INFO("NSP: Opened split NSP with %u parts from: %s", ctx->split_count, dir_path);
    return true;
}

static int nsp_seek(NspContext* ctx, u64 offset) {
    if (!ctx->is_split) {
        ctx->virtual_pos = offset;
        return fseek(ctx->fp, (long)offset, SEEK_SET);
    }

    ctx->virtual_pos = offset;
    // Find which part contains this offset
    u64 part_start = 0;
    for (u32 i = 0; i < ctx->split_count; i++) {
        if (offset < part_start + ctx->split_sizes[i]) {
            return fseek(ctx->split_fps[i], (long)(offset - part_start), SEEK_SET);
        }
        part_start += ctx->split_sizes[i];
    }
    return -1; // Past end
}

static size_t nsp_read(NspContext* ctx, void* buffer, size_t size) {
    if (!ctx->is_split) {
        size_t rd = fread(buffer, 1, size, ctx->fp);
        ctx->virtual_pos += rd;
        return rd;
    }

    u8* out = (u8*)buffer;
    size_t total_read = 0;

    while (total_read < size) {
        // Find which part contains virtual_pos
        u64 part_start = 0;
        u32 part_idx = 0;
        bool found = false;
        for (u32 i = 0; i < ctx->split_count; i++) {
            if (ctx->virtual_pos < part_start + ctx->split_sizes[i]) {
                part_idx = i;
                found = true;
                break;
            }
            part_start += ctx->split_sizes[i];
        }
        if (!found) break; // Past end of all parts

        u64 offset_in_part = ctx->virtual_pos - part_start;
        u64 avail_in_part = ctx->split_sizes[part_idx] - offset_in_part;
        size_t to_read = (size - total_read < avail_in_part) ? (size - total_read) : (size_t)avail_in_part;

        fseek(ctx->split_fps[part_idx], (long)offset_in_part, SEEK_SET);
        size_t rd = fread(out + total_read, 1, to_read, ctx->split_fps[part_idx]);
        if (rd == 0) break;

        total_read += rd;
        ctx->virtual_pos += rd;
    }

    return total_read;
}

bool nspOpen(NspContext* ctx, const char* path) {
    if (!ctx || !path) return false;

    memset(ctx, 0, sizeof(NspContext));

    if (nsp_is_directory(path)) {
        if (!nsp_open_split(ctx, path)) {
            return false;
        }
    } else {
        ctx->fp = fopen(path, "rb");
        if (!ctx->fp) {
            LOG_ERROR("NSP: Failed to open file: %s", path);
            return false;
        }
    }

    nsp_seek(ctx, 0);
    if (nsp_read(ctx, &ctx->header, sizeof(Pfs0Header)) != sizeof(Pfs0Header)) {
        LOG_ERROR("NSP: Failed to read PFS0 header");
        nspClose(ctx);
        return false;
    }

    if (ctx->header.magic != PFS0_MAGIC) {
        LOG_ERROR("NSP: Invalid PFS0 magic: 0x%08X (expected 0x%08X)",
                  ctx->header.magic, PFS0_MAGIC);
        nspClose(ctx);
        return false;
    }

    if (ctx->header.file_count == 0 || ctx->header.file_count > NSP_MAX_FILES) {
        LOG_ERROR("NSP: Invalid file count: %u", ctx->header.file_count);
        nspClose(ctx);
        return false;
    }

    LOG_INFO("NSP: Opened successfully - %u files, string table size: %u",
             ctx->header.file_count, ctx->header.string_table_size);

    size_t entries_size = sizeof(Pfs0FileEntry) * ctx->header.file_count;
    ctx->entries = (Pfs0FileEntry*)malloc(entries_size);
    if (!ctx->entries) {
        LOG_ERROR("NSP: Failed to allocate file entries");
        nspClose(ctx);
        return false;
    }

    if (nsp_read(ctx, ctx->entries, entries_size) != entries_size) {
        LOG_ERROR("NSP: Failed to read file entries");
        free(ctx->entries);
        ctx->entries = NULL;
        nspClose(ctx);
        return false;
    }

    if (ctx->header.string_table_size > 0) {
        ctx->string_table = (char*)malloc(ctx->header.string_table_size);
        if (!ctx->string_table) {
            LOG_ERROR("NSP: Failed to allocate string table");
            free(ctx->entries);
            ctx->entries = NULL;
            nspClose(ctx);
            return false;
        }

        if (nsp_read(ctx, ctx->string_table, ctx->header.string_table_size) != ctx->header.string_table_size) {
            LOG_ERROR("NSP: Failed to read string table");
            free(ctx->string_table);
            ctx->string_table = NULL;
            free(ctx->entries);
            ctx->entries = NULL;
            nspClose(ctx);
            return false;
        }
    }

    ctx->data_offset = sizeof(Pfs0Header) +
                       (sizeof(Pfs0FileEntry) * ctx->header.file_count) +
                       ctx->header.string_table_size;

    ctx->is_open = true;

    LOG_INFO("NSP: Parse complete - data starts at offset 0x%lX", ctx->data_offset);

    return true;
}

void nspClose(NspContext* ctx) {
    if (!ctx) return;

    if (ctx->is_split) {
        for (u32 i = 0; i < ctx->split_count; i++) {
            if (ctx->split_fps[i]) {
                fclose(ctx->split_fps[i]);
                ctx->split_fps[i] = NULL;
            }
        }
        ctx->fp = NULL; // fp points to split_fps[0], already closed
    } else if (ctx->fp) {
        fclose(ctx->fp);
        ctx->fp = NULL;
    }

    if (ctx->entries) {
        free(ctx->entries);
        ctx->entries = NULL;
    }

    if (ctx->string_table) {
        free(ctx->string_table);
        ctx->string_table = NULL;
    }

    ctx->is_open = false;
}

u32 nspGetFileCount(NspContext* ctx) {
    if (!ctx || !ctx->is_open) return 0;
    return ctx->header.file_count;
}

const char* nspGetFilename(NspContext* ctx, u32 index) {
    if (!ctx || !ctx->is_open || index >= ctx->header.file_count) return NULL;

    u32 string_offset = ctx->entries[index].string_offset;
    if (string_offset >= ctx->header.string_table_size) return NULL;

    return ctx->string_table + string_offset;
}

u64 nspGetFileSize(NspContext* ctx, u32 index) {
    if (!ctx || !ctx->is_open || index >= ctx->header.file_count) return 0;
    return ctx->entries[index].size;
}

bool nspGetFileEntry(NspContext* ctx, u32 index, Pfs0FileEntry* out) {
    if (!ctx || !ctx->is_open || !out || index >= ctx->header.file_count) return false;
    memcpy(out, &ctx->entries[index], sizeof(Pfs0FileEntry));
    return true;
}

bool nspIsNcaFile(NspContext* ctx, u32 index) {
    const char* filename = nspGetFilename(ctx, index);
    if (!filename) return false;

    size_t len = strlen(filename);
    if (len < 5) return false;

    return (strcasecmp(filename + len - 4, ".nca") == 0);
}

bool nspExtractFile(NspContext* ctx, u32 index, const char* dest_path) {
    if (!ctx || !ctx->is_open || !dest_path || index >= ctx->header.file_count) {
        return false;
    }

    Pfs0FileEntry* entry = &ctx->entries[index];
    u64 file_offset = ctx->data_offset + entry->offset;

    if (nsp_seek(ctx, file_offset) != 0) {
        LOG_ERROR("NSP: Failed to seek to file offset 0x%lX", file_offset);
        return false;
    }

    FILE* dest = fopen(dest_path, "wb");
    if (!dest) {
        LOG_ERROR("NSP: Failed to create destination file: %s", dest_path);
        return false;
    }

    u8 buffer[64 * 1024];
    u64 remaining = entry->size;
    bool success = true;

    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : (size_t)remaining;

        if (nsp_read(ctx, buffer, chunk) != chunk) {
            LOG_ERROR("NSP: Read error during extraction");
            success = false;
            break;
        }

        if (fwrite(buffer, 1, chunk, dest) != chunk) {
            LOG_ERROR("NSP: Write error during extraction");
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

s64 nspReadFile(NspContext* ctx, u32 index, u64 offset, void* buffer, u64 size) {
    if (!ctx || !ctx->is_open || !buffer || index >= ctx->header.file_count) {
        return -1;
    }

    Pfs0FileEntry* entry = &ctx->entries[index];

    if (offset >= entry->size) return 0;
    if (offset + size > entry->size) {
        size = entry->size - offset;
    }

    u64 file_offset = ctx->data_offset + entry->offset + offset;

    if (nsp_seek(ctx, file_offset) != 0) {
        return -1;
    }

    size_t read_bytes = nsp_read(ctx, buffer, (size_t)size);
    return (s64)read_bytes;
}

s32 nspFindFile(NspContext* ctx, const char* filename) {
    if (!ctx || !ctx->is_open || !filename) return -1;

    for (u32 i = 0; i < ctx->header.file_count; i++) {
        const char* name = nspGetFilename(ctx, i);
        if (name && strcmp(name, filename) == 0) {
            return (s32)i;
        }
    }

    return -1;
}
