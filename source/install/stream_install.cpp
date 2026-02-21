// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/stream_install.h"
#include "install/nca_install.h"
#include "install/cnmt.h"
#include "install/ticket_utils.h"
#include "mtp_log.h"
#include <switch.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <cstdio>

extern "C" {
#include "ipcext/es.h"
}

#pragma pack(push, 1)
typedef struct {
    u32 magic;
    u32 num_files;
    u32 string_table_size;
    u32 reserved;
} Pfs0Header;

typedef struct {
    u64 offset;
    u64 size;
    u32 string_offset;
    u32 reserved;
} Pfs0FileEntry;
#pragma pack(pop)

Result streamInstallInit(StreamInstallContext* ctx, InstallTarget target) {
    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(ctx, 0, sizeof(StreamInstallContext));
    ctx->target = target;
    ctx->state = STREAM_STATE_IDLE;
    ctx->cnmt_scanned = false;

    ctx->buffer = (u8*)malloc(STREAM_BUFFER_SIZE);
    if (!ctx->buffer) {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    ctx->buffer_size = STREAM_BUFFER_SIZE;

    // Pre-allocate write buffer for NCA installation to avoid per-call malloc/free
    ctx->write_buffer_size = 1024 * 1024;
    ctx->write_buffer = (u8*)malloc(ctx->write_buffer_size);
    if (!ctx->write_buffer) {
        free(ctx->buffer);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    ctx->nca_ctx = (NcaInstallContext*)malloc(sizeof(NcaInstallContext));
    if (!ctx->nca_ctx) {
        free(ctx->write_buffer);
        free(ctx->buffer);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    Result rc = ncaInstallInit(ctx->nca_ctx, target);
    if (R_FAILED(rc)) {
        free(ctx->nca_ctx);
        free(ctx->write_buffer);
        free(ctx->buffer);
        return rc;
    }

#if DEBUG_INSTALL
    LOG_INFO("Stream Install: Initialized with %zu MB buffer", STREAM_BUFFER_SIZE / (1024 * 1024));
#endif

    return 0;
}

void streamInstallExit(StreamInstallContext* ctx) {
    if (!ctx) return;

    if (ctx->nca_ctx) {
        ncaInstallExit(ctx->nca_ctx);
        free(ctx->nca_ctx);
        ctx->nca_ctx = NULL;
    }

    if (ctx->buffer) {
        free(ctx->buffer);
        ctx->buffer = NULL;
    }

    if (ctx->write_buffer) {
        free(ctx->write_buffer);
        ctx->write_buffer = NULL;
    }

    if (ctx->pfs0.file_entries) {
        free(ctx->pfs0.file_entries);
        ctx->pfs0.file_entries = NULL;
    }

    if (ctx->pfs0.string_table) {
        free(ctx->pfs0.string_table);
        ctx->pfs0.string_table = NULL;
    }

    if (ctx->cnmt_found) {
        cnmtFree(&ctx->cnmt_ctx);
        ctx->cnmt_found = false;
    }

    if (ctx->ticket_data) {
        free(ctx->ticket_data);
        ctx->ticket_data = NULL;
    }

    if (ctx->cert_data) {
        free(ctx->cert_data);
        ctx->cert_data = NULL;
    }

    memset(ctx, 0, sizeof(StreamInstallContext));
}

void streamInstallReset(StreamInstallContext* ctx) {
    if (!ctx) return;

    ctx->buffer_pos = 0;
    ctx->read_pos = 0;
    ctx->total_received = 0;
    ctx->current_nca_index = 0;
    ctx->nca_offset = 0;
    ctx->ncaInstalling = false;
    ctx->cnmt_found = false;
    ctx->cnmt_scanned = false;
    ctx->state = STREAM_STATE_IDLE;
    ctx->file_type = STREAM_TYPE_UNKNOWN;
    ctx->stream_file_offset = 0;

    // Free and reset PFS0 header caches
    if (ctx->pfs0.file_entries) {
        free(ctx->pfs0.file_entries);
        ctx->pfs0.file_entries = NULL;
    }
    if (ctx->pfs0.string_table) {
        free(ctx->pfs0.string_table);
        ctx->pfs0.string_table = NULL;
    }
    ctx->pfs0.header_cached = false;
    ctx->pfs0.header_parsed = false;

    // Free and reset ticket/cert caches
    if (ctx->ticket_data) {
        free(ctx->ticket_data);
        ctx->ticket_data = NULL;
    }
    if (ctx->cert_data) {
        free(ctx->cert_data);
        ctx->cert_data = NULL;
    }
    ctx->ticket_size = 0;
    ctx->cert_size = 0;
    ctx->ticket_imported = false;

    (void)ctx->nca_ctx;
}

Result streamInstallStart(StreamInstallContext* ctx, const char* filename, u64 file_size) {
    if (!ctx || !filename) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (ctx->state != STREAM_STATE_IDLE) {
        LOG_WARN("Stream Install: Already busy, resetting");
        streamInstallReset(ctx);
    }

    strncpy(ctx->filename, filename, sizeof(ctx->filename) - 1);
    ctx->file_size = file_size;
    ctx->state = STREAM_STATE_RECEIVING;

    size_t len = strlen(filename);
    const char* ext = filename + (len > 4 ? len - 4 : 0);

    if (strcasecmp(ext, ".nsp") == 0) {
        ctx->file_type = STREAM_TYPE_NSP;
    } else if (strcasecmp(ext, ".xci") == 0) {
        ctx->file_type = STREAM_TYPE_XCI;
    } else {
        LOG_ERROR("Stream Install: Unknown file type: %s", ext);
        ctx->state = STREAM_STATE_ERROR;
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    ctx->buffer_pos = 0;
    ctx->read_pos = 0;
    ctx->total_received = 0;

    LOG_INFO("Stream Install: ========== Started receiving %s (%.2f MB) ==========",
            filename, file_size / (1024.0 * 1024.0));

    return 0;
}

static u64 streamRead(StreamInstallContext* ctx, void* out, u64 size) {
    if (!ctx || !out) return 0;

    u64 available = ctx->buffer_pos - ctx->read_pos;
    if (size > available) size = available;

    u64 read = 0;
    while (read < size) {
        u64 buffer_offset = ctx->read_pos % ctx->buffer_size;
        u64 chunk = ctx->buffer_size - buffer_offset;
        if (chunk > (size - read)) chunk = size - read;

        memcpy((u8*)out + read, ctx->buffer + buffer_offset, chunk);
        ctx->read_pos += chunk;
        read += chunk;
    }

    return read;
}

static void streamSkip(StreamInstallContext* ctx, u64 size) {
    if (!ctx) return;

    u64 available = ctx->buffer_pos - ctx->read_pos;
    if (size > available) size = available;
    ctx->read_pos += size;
}

static u64 streamAvailable(const StreamInstallContext* ctx) {
    if (!ctx) return 0;
    return ctx->buffer_pos - ctx->read_pos;
}

static bool parsePfs0Header(StreamInstallContext* ctx) {
    // Check if we have enough data for the initial header
    if (streamAvailable(ctx) < sizeof(Pfs0Header)) {
        return false;  // Not enough data yet
    }

    // Read the basic header first (without consuming it yet)
    Pfs0Header header;
    u64 saved_read_pos = ctx->read_pos;
    if (streamRead(ctx, &header, sizeof(header)) != sizeof(header)) {
        ctx->read_pos = saved_read_pos;
        return false;
    }

    if (header.magic != 0x30534650) {
        LOG_ERROR("Stream Install: Invalid PFS0 magic: 0x%08X", header.magic);
        return false;
    }

    // Calculate total header size including file entries and string table
    u64 file_entries_size = header.num_files * sizeof(Pfs0FileEntry);
    u64 total_header_size = sizeof(Pfs0Header) + file_entries_size + header.string_table_size;

    // Check if we have the complete header
    // We already consumed sizeof(Pfs0Header), so check for the rest
    if (streamAvailable(ctx) < file_entries_size + header.string_table_size) {
        // Reset and wait for more data
        ctx->read_pos = saved_read_pos;
        return false;
    }

    // Now we can cache everything
    ctx->pfs0.num_files = header.num_files;
    ctx->pfs0.string_table_size = header.string_table_size;
    ctx->pfs0.data_offset = total_header_size;
    ctx->pfs0.file_entries_size = file_entries_size;

    // Allocate and read file entries
    ctx->pfs0.file_entries = (u8*)malloc(file_entries_size);
    if (!ctx->pfs0.file_entries) {
        LOG_ERROR("Stream Install: Failed to allocate file entries cache");
        return false;
    }
    if (streamRead(ctx, ctx->pfs0.file_entries, file_entries_size) != file_entries_size) {
        free(ctx->pfs0.file_entries);
        ctx->pfs0.file_entries = NULL;
        return false;
    }

    // Allocate and read string table
    ctx->pfs0.string_table_alloc = header.string_table_size;
    ctx->pfs0.string_table = (char*)malloc(header.string_table_size);
    if (!ctx->pfs0.string_table) {
        LOG_ERROR("Stream Install: Failed to allocate string table cache");
        free(ctx->pfs0.file_entries);
        ctx->pfs0.file_entries = NULL;
        return false;
    }
    if (streamRead(ctx, ctx->pfs0.string_table, header.string_table_size) != header.string_table_size) {
        free(ctx->pfs0.string_table);
        ctx->pfs0.string_table = NULL;
        free(ctx->pfs0.file_entries);
        ctx->pfs0.file_entries = NULL;
        return false;
    }

    ctx->pfs0.header_parsed = true;
    ctx->pfs0.header_cached = true;
    ctx->stream_file_offset = total_header_size;  // We're now at the data section

#if DEBUG_INSTALL
    LOG_INFO("Stream Install: PFS0 header cached - %u files, data at offset 0x%lX",
             header.num_files, ctx->pfs0.data_offset);
#endif

    return true;
}

// Get a file entry from the cached PFS0 header
static bool getPfs0FileEntry(StreamInstallContext* ctx, u32 index, Pfs0FileEntry* entry) {
    if (!ctx || !entry || !ctx->pfs0.header_cached) return false;
    if (index >= ctx->pfs0.num_files) return false;
    if (!ctx->pfs0.file_entries) return false;

    memcpy(entry, ctx->pfs0.file_entries + index * sizeof(Pfs0FileEntry), sizeof(Pfs0FileEntry));
    return true;
}

// Get a filename from the cached string table
static const char* getPfs0Filename(StreamInstallContext* ctx, u32 string_offset) {
    if (!ctx || !ctx->pfs0.string_table) return NULL;
    if (string_offset >= ctx->pfs0.string_table_alloc) return NULL;
    return ctx->pfs0.string_table + string_offset;
}

static Result installCurrentNca(StreamInstallContext* ctx) {
    if (!ctx->ncaInstalling) return 0;

    // Use pre-allocated write buffer instead of per-call malloc/free
    u8* buffer = ctx->write_buffer;
    u64 buf_size = ctx->write_buffer_size;

    Result rc = 0;
    u64 total_written = 0;

    while (ctx->nca_offset < ctx->nca_size) {
        u64 available = streamAvailable(ctx);
        if (available == 0) break;

        u64 to_write = ctx->nca_size - ctx->nca_offset;
        if (to_write > buf_size) to_write = buf_size;
        if (to_write > available) to_write = available;

        u64 read = streamRead(ctx, buffer, to_write);
        if (read == 0) break;

        // Track our position in the file stream
        ctx->stream_file_offset += read;

        rc = ncmContentStorageWritePlaceHolder(&ctx->nca_ctx->content_storage,
                                               &ctx->placeholder_id,
                                               ctx->nca_offset, buffer, read);
        if (R_FAILED(rc)) {
            LOG_ERROR("Stream Install: Write failed at offset 0x%lX: 0x%08X", ctx->nca_offset, rc);
            break;
        }

        ctx->nca_offset += read;
        total_written += read;

#if DEBUG_INSTALL
        if ((total_written % (10 * 1024 * 1024)) == 0 || ctx->nca_offset == ctx->nca_size) {
            LOG_DEBUG("Stream Install: NCA progress: %lu / %lu bytes",
                     ctx->nca_offset, ctx->nca_size);
        }
#endif
    }

    if (ctx->nca_offset >= ctx->nca_size) {
        rc = ncmContentStorageRegister(&ctx->nca_ctx->content_storage,
                                       &ctx->nca_id, &ctx->placeholder_id);
        ncmContentStorageDeletePlaceHolder(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);

        if (R_FAILED(rc)) {
            // 0x00000805 = content already exists, treat as success
            if (rc == 0x805) {
                LOG_INFO("Stream Install: NCA already exists, continuing");
                rc = 0;
            } else {
                LOG_ERROR("Stream Install: Failed to register NCA: 0x%08X", rc);
                ctx->state = STREAM_STATE_ERROR;
                return rc;
            }
        }

        ctx->ncaInstalling = false;
        ctx->nca_offset = 0;

#if DEBUG_INSTALL
        LOG_INFO("Stream Install: NCA installed successfully");
#endif
    }

    return rc;
}

// Start installing the next file in sequence (processes files in stream order)
static Result startNextFile(StreamInstallContext* ctx) {
    if (ctx->file_type != STREAM_TYPE_NSP || !ctx->pfs0.header_cached) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    // If we haven't started yet, find the CNMT NCA location first (we need it for metadata)
    if (ctx->current_nca_index == 0 && !ctx->cnmt_found && !ctx->cnmt_scanned) {
        // Scan all files to find the CNMT NCA and check ticket/cert locations
        u64 ticket_offset = UINT64_MAX;
        u64 cert_offset = UINT64_MAX;

        for (u32 i = 0; i < ctx->pfs0.num_files; i++) {
            Pfs0FileEntry entry;
            if (!getPfs0FileEntry(ctx, i, &entry)) continue;

            const char* filename = getPfs0Filename(ctx, entry.string_offset);
            if (!filename) continue;

#if DEBUG_INSTALL
            LOG_DEBUG("Stream Install: File %u: %s (offset=0x%lX, size=%lu)", i, filename, entry.offset, entry.size);
#endif

            size_t len = strlen(filename);
            if (len > 9 && strcasecmp(filename + len - 9, ".cnmt.nca") == 0) {
                LOG_INFO("Stream Install: Found CNMT NCA: %s (index %u, offset=0x%lX)", filename, i, entry.offset);

                // Parse content ID from filename
                for (int j = 0; j < 16 && filename[j * 2] != '\0'; j++) {
                    char hex_byte[3] = {filename[j * 2], filename[j * 2 + 1], '\0'};
                    ctx->cnmt_id.c[j] = (u8)strtoul(hex_byte, NULL, 16);
                }
                ctx->cnmt_size = entry.size;
                // Don't set cnmt_found yet - we'll set it after parsing
            }

            // Track ticket/cert offsets
            if (len > 4 && strcasecmp(filename + len - 4, ".tik") == 0) {
                ticket_offset = entry.offset;
            }
            if (len > 5 && strcasecmp(filename + len - 5, ".cert") == 0) {
                cert_offset = entry.offset;
            }
        }

        // Mark that we've scanned for CNMT
        ctx->cnmt_scanned = true;

        // Only try pre-caching if ticket/cert are within first 32MB
        // Otherwise skip pre-caching and handle during normal sequential processing
        u64 max_precache_offset = 32 * 1024 * 1024;  // 32MB

        if (ticket_offset > max_precache_offset || cert_offset > max_precache_offset) {
            LOG_INFO("Stream Install: Ticket/cert too far for pre-caching (ticket=0x%lX, cert=0x%lX), will cache during install",
                    ticket_offset, cert_offset);
        }
    }

    // Process files in order starting from current position
    for (u32 i = ctx->current_nca_index; i < ctx->pfs0.num_files; i++) {
        Pfs0FileEntry entry;
        if (!getPfs0FileEntry(ctx, i, &entry)) {
            LOG_ERROR("Stream Install: Failed to get file entry %u", i);
            ctx->current_nca_index = i + 1;
            continue;
        }

        const char* filename = getPfs0Filename(ctx, entry.string_offset);
        if (!filename) {
            LOG_ERROR("Stream Install: Failed to get filename for entry %u", i);
            ctx->current_nca_index = i + 1;
            continue;
        }

        size_t len = strlen(filename);
        u64 file_offset = ctx->pfs0.data_offset + entry.offset;

        // Check file type
        bool is_ticket = (len > 4 && strcasecmp(filename + len - 4, ".tik") == 0);
        bool is_cert = (len > 5 && strcasecmp(filename + len - 5, ".cert") == 0);
        bool is_nca = (len > 4 && strcasecmp(filename + len - 4, ".nca") == 0);
        bool is_cnmt_nca = (len > 9 && strcasecmp(filename + len - 9, ".cnmt.nca") == 0);

        // Skip to file position if needed
        if (file_offset > ctx->stream_file_offset) {
            u64 to_skip = file_offset - ctx->stream_file_offset;
            u64 available = streamAvailable(ctx);
            if (to_skip > available) {
                // Not enough data yet, wait for more
                return 0;
            }
            streamSkip(ctx, to_skip);
            ctx->stream_file_offset += to_skip;
        }

        // Handle ticket file
        if (is_ticket) {
            LOG_INFO("Stream Install: Found ticket file: %s (size=%lu bytes, available=%lu, ticket_data=%p)",
                    filename, entry.size, streamAvailable(ctx), ctx->ticket_data);

            if (streamAvailable(ctx) >= entry.size && !ctx->ticket_data) {
                ctx->ticket_data = (u8*)malloc(entry.size);
                if (ctx->ticket_data) {
                    u64 read = streamRead(ctx, ctx->ticket_data, entry.size);
                    if (read == entry.size) {
                        ctx->ticket_size = (u32)entry.size;
                        ctx->stream_file_offset += entry.size;
                        LOG_INFO("Stream Install: ✓ Cached ticket: %s (%u bytes)", filename, ctx->ticket_size);

                        // Check if ticket is personalized
                        u8 rights_id[16];
                        u64 device_id;
                        u32 account_id;
                        if (checkTicketMismatch(ctx->ticket_data, ctx->ticket_size,
                                                    rights_id, &device_id, &account_id)) {
                            ctx->ticket_is_personalized = true;
                            ctx->waiting_for_user_response = true;
                            LOG_INFO("Stream Install: Detected personalized ticket (Device: 0x%016lX, Account: 0x%08X)",
                                    device_id, account_id);
                            // Event will be posted from mtp_protocol.cpp during progress update
                        } else {
                            ctx->ticket_is_personalized = false;
                            LOG_INFO("Stream Install: Ticket is common (no restrictions)");
                        }
                    } else {
                        LOG_ERROR("Stream Install: Failed to read ticket - expected %lu, got %lu", entry.size, read);
                        free(ctx->ticket_data);
                        ctx->ticket_data = NULL;
                    }
                } else {
                    LOG_ERROR("Stream Install: Failed to allocate memory for ticket (%lu bytes)", entry.size);
                }
            } else if (ctx->ticket_data) {
                LOG_WARN("Stream Install: Skipping ticket %s - already have ticket cached", filename);
            } else if (streamAvailable(ctx) < entry.size) {
                LOG_DEBUG("Stream Install: Need more data for ticket - have %lu, need %lu",
                         streamAvailable(ctx), entry.size);
                return 0; // Need more data
            }
            ctx->current_nca_index = i + 1;
            continue;
        }

        // Handle certificate file
        if (is_cert) {
            LOG_INFO("Stream Install: Found cert file: %s (size=%lu bytes, available=%lu, cert_data=%p)",
                    filename, entry.size, streamAvailable(ctx), ctx->cert_data);

            if (streamAvailable(ctx) >= entry.size && !ctx->cert_data) {
                ctx->cert_data = (u8*)malloc(entry.size);
                if (ctx->cert_data) {
                    u64 read = streamRead(ctx, ctx->cert_data, entry.size);
                    if (read == entry.size) {
                        ctx->cert_size = (u32)entry.size;
                        ctx->stream_file_offset += entry.size;
                        LOG_INFO("Stream Install: ✓ Cached cert: %s (%u bytes)", filename, ctx->cert_size);
                    } else {
                        LOG_ERROR("Stream Install: Failed to read cert - expected %lu, got %lu", entry.size, read);
                        free(ctx->cert_data);
                        ctx->cert_data = NULL;
                    }
                } else {
                    LOG_ERROR("Stream Install: Failed to allocate memory for cert (%lu bytes)", entry.size);
                }
            } else if (ctx->cert_data) {
                LOG_WARN("Stream Install: Skipping cert %s - already have cert cached", filename);
            } else if (streamAvailable(ctx) < entry.size) {
                LOG_DEBUG("Stream Install: Need more data for cert - have %lu, need %lu",
                         streamAvailable(ctx), entry.size);
                return 0; // Need more data
            }
            ctx->current_nca_index = i + 1;
            continue;
        }

        // Handle NCA files
        if (is_nca) {
            // Parse content ID from filename
            NcmContentId nca_id = {0};
            for (int j = 0; j < 16 && filename[j * 2] != '\0'; j++) {
                char hex_byte[3] = {filename[j * 2], filename[j * 2 + 1], '\0'};
                nca_id.c[j] = (u8)strtoul(hex_byte, NULL, 16);
            }

            ctx->current_nca_index = i;
            ctx->nca_id = nca_id;
            ctx->nca_size = entry.size;
            ctx->nca_offset = 0;

            // Delete any existing content with the same ID (from previous failed installs)
            ncmContentStorageDelete(&ctx->nca_ctx->content_storage, &nca_id);

            // Create placeholder
            ncmContentStorageGeneratePlaceHolderId(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);
            ncmContentStorageDeletePlaceHolder(&ctx->nca_ctx->content_storage, &ctx->placeholder_id);

            Result rc = ncmContentStorageCreatePlaceHolder(&ctx->nca_ctx->content_storage,
                                                           &nca_id,
                                                           &ctx->placeholder_id,
                                                           entry.size);
            if (R_FAILED(rc)) {
                LOG_ERROR("Stream Install: Failed to create placeholder: 0x%08X", rc);
                return rc;
            }

            ctx->ncaInstalling = true;

            if (is_cnmt_nca) {
                LOG_INFO("Stream Install: Installing CNMT NCA: %s (size: %lu)", filename, entry.size);
            } else {
                LOG_INFO("Stream Install: Installing NCA %u/%u: %s (size: %lu)",
                         i + 1, ctx->pfs0.num_files, filename, entry.size);
            }
            return 0;
        }

        // Skip unknown file types
        LOG_DEBUG("Stream Install: Skipping unknown file: %s", filename);
        if (streamAvailable(ctx) >= entry.size) {
            streamSkip(ctx, entry.size);
            ctx->stream_file_offset += entry.size;
        } else {
            return 0; // Need more data
        }
        ctx->current_nca_index = i + 1;
    }

    // All files processed
    ctx->state = STREAM_STATE_COMPLETE;
    return 0;
}

// Import cached ticket/cert if available
static Result importCachedTicket(StreamInstallContext* ctx) {
    if (ctx->ticket_imported) {
        LOG_INFO("Stream Install: Ticket already imported - skipping");
        return 0;  // Already imported
    }

    // If waiting for user response on personalized ticket, don't proceed yet
    if (ctx->waiting_for_user_response) {
        LOG_DEBUG("Stream Install: Waiting for user response on personalized ticket");
        return 0;  // Return success but don't mark as imported - will retry later
    }

    if (!ctx->ticket_data || ctx->ticket_size == 0) {
        LOG_WARN("Stream Install: ⚠️  No ticket to import (standard/free title or ticket not cached!)");
        ctx->ticket_imported = true;  // Mark as done even if no ticket
        return 0;
    }

    LOG_INFO("Stream Install: Attempting to import ticket (%u bytes)...", ctx->ticket_size);

    // Initialize ES service if needed
    Result rc = esInitialize();
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to initialize ES service: 0x%08X", rc);
        return rc;
    }

    // Import the ticket with certificate
    if (ctx->cert_data && ctx->cert_size > 0) {
        rc = esImportTicket(ctx->ticket_data, ctx->ticket_size,
                           ctx->cert_data, ctx->cert_size);
    } else {
        // Try importing without cert (some tickets are self-contained)
        rc = esImportTicket(ctx->ticket_data, ctx->ticket_size, NULL, 0);
    }

    esExit();

    if (R_FAILED(rc)) {
        // Error 0x1A05 means ticket already exists, which is fine
        if (rc == 0x1A05) {
            LOG_INFO("Stream Install: Ticket already exists (0x1A05)");
            ctx->ticket_imported = true;
            return 0;
        }
        LOG_ERROR("Stream Install: Failed to import ticket: 0x%08X", rc);
        return rc;
    }

    LOG_INFO("Stream Install: Ticket imported successfully");
    ctx->ticket_imported = true;
    return 0;
}

static Result readCnmtFromInstalledNca(StreamInstallContext* ctx) {
    // Note: Ticket import is deferred to streamInstallFinalize() since the ticket
    // may appear after the CNMT NCA in streaming mode. CNMT NCAs are typically
    // not encrypted, so we can read them without the ticket.

    char cnmt_path[FS_MAX_PATH];
    Result rc = ncmContentStorageGetPath(&ctx->nca_ctx->content_storage, cnmt_path,
                                         sizeof(cnmt_path), &ctx->cnmt_id);
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to get CNMT path: 0x%08X", rc);
        return rc;
    }

    FsFileSystem cnmt_fs;
    FsContentAttributes attrs = {};
    rc = fsOpenFileSystemWithId(&cnmt_fs, 0, FsFileSystemType_ContentMeta, cnmt_path, attrs);
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to open CNMT filesystem: 0x%08X", rc);
        return rc;
    }

    FsDir dir;
    rc = fsFsOpenDirectory(&cnmt_fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        fsFsClose(&cnmt_fs);
        return rc;
    }

    FsDirectoryEntry entry;
    s64 total_entries;
    char cnmt_file_path[FS_MAX_PATH] = {0};

    while (fsDirRead(&dir, &total_entries, 1, &entry) == 0 && total_entries > 0) {
        size_t len = strlen(entry.name);
        if (len > 5 && strcasecmp(entry.name + len - 5, ".cnmt") == 0) {
            snprintf(cnmt_file_path, sizeof(cnmt_file_path), "/%s", entry.name);
            break;
        }
    }
    fsDirClose(&dir);

    if (cnmt_file_path[0] == '\0') {
        fsFsClose(&cnmt_fs);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    FsFile cnmt_file;
    rc = fsFsOpenFile(&cnmt_fs, cnmt_file_path, FsOpenMode_Read, &cnmt_file);
    if (R_FAILED(rc)) {
        fsFsClose(&cnmt_fs);
        return rc;
    }

    s64 cnmt_size;
    fsFileGetSize(&cnmt_file, &cnmt_size);

    u8* cnmt_data = (u8*)malloc(cnmt_size);
    if (!cnmt_data) {
        fsFileClose(&cnmt_file);
        fsFsClose(&cnmt_fs);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    u64 bytes_read;
    rc = fsFileRead(&cnmt_file, 0, cnmt_data, cnmt_size, FsReadOption_None, &bytes_read);
    fsFileClose(&cnmt_file);
    fsFsClose(&cnmt_fs);

    if (R_FAILED(rc) || bytes_read != (u64)cnmt_size) {
        free(cnmt_data);
        return rc;
    }

    if (!cnmtParse(&ctx->cnmt_ctx, cnmt_data, cnmt_size)) {
        free(cnmt_data);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    free(cnmt_data);
    ctx->cnmt_found = true;

    LOG_INFO("Stream Install: CNMT parsed - Title ID: 0x%016lX, %u contents",
             ctx->cnmt_ctx.header.title_id, ctx->cnmt_ctx.content_count);

    return 0;
}

s64 streamInstallProcessData(StreamInstallContext* ctx, const void* data, u64 size) {
    if (!ctx || !data || size == 0) return -1;
    if (ctx->state == STREAM_STATE_ERROR) return -1;

    u64 write_remaining = size;
    u64 data_offset = 0;

    // Loop until we've consumed all input data or can't make progress
    while (write_remaining > 0 || streamAvailable(ctx) > 0) {
        // First, try to write incoming data to the ring buffer
        while (write_remaining > 0) {
            u64 buffer_offset = ctx->buffer_pos % ctx->buffer_size;
            u64 contiguous_space = ctx->buffer_size - buffer_offset;

            u64 unread_data = ctx->buffer_pos - ctx->read_pos;
            u64 free_space = ctx->buffer_size - unread_data;

            if (free_space == 0) {
                break;  // Buffer full, need to process some data first
            }

            u64 to_write = contiguous_space;
            if (to_write > free_space) to_write = free_space;
            if (to_write > write_remaining) to_write = write_remaining;

            memcpy(ctx->buffer + buffer_offset, (const u8*)data + data_offset, to_write);
            ctx->buffer_pos += to_write;
            ctx->total_received += to_write;
            data_offset += to_write;
            write_remaining -= to_write;
        }

        // Now process data from the buffer
        Result rc = 0;
        u64 available_before = streamAvailable(ctx);

        switch (ctx->state) {
            case STREAM_STATE_RECEIVING:
                // Try to parse PFS0 header - it will return false if we need more data
                if (!ctx->pfs0.header_cached) {
                    if (!parsePfs0Header(ctx)) {
                        if (ctx->pfs0.header_parsed && !ctx->pfs0.header_cached) {
                            LOG_WARN("Stream Install: Header partially parsed, waiting for more data");
                        }
                        break;
                    }
                    ctx->state = STREAM_STATE_INSTALLING;

                    rc = startNextFile(ctx);
                    if (R_FAILED(rc)) {
                        LOG_ERROR("Stream Install: Failed to start file processing: 0x%08X", rc);
                        ctx->state = STREAM_STATE_ERROR;
                        return -1;
                    }
                }
                break;

            case STREAM_STATE_PARSING:
            case STREAM_STATE_INSTALLING:
                if (ctx->ncaInstalling) {
                    rc = installCurrentNca(ctx);
                    if (R_FAILED(rc)) {
                        ctx->state = STREAM_STATE_ERROR;
                        return -1;
                    }

                    if (!ctx->ncaInstalling) {
                        bool was_cnmt = (memcmp(&ctx->nca_id, &ctx->cnmt_id, sizeof(NcmContentId)) == 0);

                        if (was_cnmt && !ctx->cnmt_found) {
                            rc = readCnmtFromInstalledNca(ctx);
                            if (R_SUCCEEDED(rc)) {
                                LOG_INFO("Stream Install: CNMT loaded successfully");
                            } else {
                                LOG_ERROR("Stream Install: Failed to read CNMT: 0x%08X", rc);
                                ctx->state = STREAM_STATE_ERROR;
                                return -1;
                            }
                        }

                        ctx->current_nca_index++;
                        rc = startNextFile(ctx);
                        if (R_FAILED(rc)) {
                            ctx->state = STREAM_STATE_ERROR;
                            return -1;
                        }
                        // If startNextFile returned success but didn't start installing an NCA,
                        // it means we're waiting for more data (e.g., ticket pre-caching)
                        if (R_SUCCEEDED(rc) && !ctx->ncaInstalling) {
                            break;  // Exit loop and wait for more data
                        }
                    }
                } else {
                    u32 old_index = ctx->current_nca_index;
                    rc = startNextFile(ctx);
                    if (R_FAILED(rc)) {
                        ctx->state = STREAM_STATE_ERROR;
                        return -1;
                    }
                    // If startNextFile returned success but didn't start installing an NCA
                    // and didn't advance the index, we're waiting for more data
                    if (R_SUCCEEDED(rc) && !ctx->ncaInstalling && ctx->current_nca_index == old_index) {
                        break;  // Exit loop and wait for more data
                    }
                }
                break;

            case STREAM_STATE_COMPLETE:
            case STREAM_STATE_ERROR:
                // Return all data as processed
                return size;

            default:
                break;
        }

        // If we didn't consume any data from the buffer, we can't make progress
        u64 available_after = streamAvailable(ctx);
        if (available_after >= available_before && write_remaining > 0) {
            // Buffer is full and we couldn't drain it - this shouldn't happen
            // but return what we've processed so far
            break;
        }

        // If there's no more data to write and we're not making progress, exit
        if (write_remaining == 0 && available_after == available_before) {
            break;
        }
    }

    return data_offset;
}

Result streamInstallFinalize(StreamInstallContext* ctx) {
    if (!ctx) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    // Wait for any pending installation to complete
    while (ctx->ncaInstalling) {
        Result rc = installCurrentNca(ctx);
        if (R_FAILED(rc)) {
            ctx->state = STREAM_STATE_ERROR;
            return rc;
        }
        if (!ctx->ncaInstalling) break;
        svcSleepThread(1000000ULL);
    }

    if (ctx->state == STREAM_STATE_ERROR) {
        LOG_ERROR("Stream Install: Finalized with error");
        return ctx->last_error;
    }

    if (!ctx->cnmt_found) {
        LOG_ERROR("Stream Install: No CNMT found");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    // Import ticket now that all files have been processed and ticket is cached (if present)
    Result rc = importCachedTicket(ctx);
    if (R_FAILED(rc)) {
        LOG_WARN("Stream Install: Ticket import failed: 0x%08X (may be free title)", rc);
        // Continue anyway - might be a free title that doesn't need a ticket
    }

    NcmContentInfo cnmt_info;
    cnmt_info.content_id = ctx->cnmt_id;
    ncmU64ToContentInfoSize(ctx->cnmt_size & 0xFFFFFFFFFFFFULL, &cnmt_info);
    cnmt_info.content_type = NcmContentType_Meta;

    u8* install_meta_buffer;
    size_t install_meta_size;
    rc = cnmtBuildInstallContentMeta(&ctx->cnmt_ctx, &cnmt_info, false,
                                     &install_meta_buffer, &install_meta_size);
    if (R_FAILED(rc)) {
        LOG_ERROR("Stream Install: Failed to build meta: 0x%08X", rc);
        return rc;
    }

    NcmContentMetaKey meta_key = cnmtGetContentMetaKey(&ctx->cnmt_ctx);
    rc = ncmContentMetaDatabaseSet(&ctx->nca_ctx->meta_db, &meta_key,
                                   (NcmContentMetaHeader*)install_meta_buffer,
                                   install_meta_size);
    if (R_SUCCEEDED(rc)) {
        ncmContentMetaDatabaseCommit(&ctx->nca_ctx->meta_db);
        LOG_DEBUG("Stream Install: Content metadata registered");
    }
    free(install_meta_buffer);

    if (R_FAILED(rc)) {
        return rc;
    }

    u64 title_id = ctx->cnmt_ctx.header.title_id;
    ctx->title_id = title_id;

    u64 base_title_id;
    NcmContentMetaType content_type = (NcmContentMetaType)ctx->cnmt_ctx.header.type;

    switch (content_type) {
        case NcmContentMetaType_Patch:
            base_title_id = title_id ^ 0x800;
            break;
        case NcmContentMetaType_AddOnContent:
            base_title_id = (title_id ^ 0x1000) & ~0xFFFULL;
            break;
        default:
            base_title_id = title_id;
            break;
    }

    struct {
        NcmContentMetaKey meta_record;
        NcmStorageId storage_id;
    } storage_record;

    storage_record.meta_record = meta_key;
    storage_record.storage_id = ctx->nca_ctx->storage_id;

    Service ns_app_man_srv;
    bool got_ns_service = false;

    if (hosversionBefore(3, 0, 0)) {
        Service* srv = nsGetServiceSession_ApplicationManagerInterface();
        if (srv) {
            memcpy(&ns_app_man_srv, srv, sizeof(Service));
            got_ns_service = true;
        }
    } else {
        rc = nsGetApplicationManagerInterface(&ns_app_man_srv);
        got_ns_service = R_SUCCEEDED(rc);
    }

    if (got_ns_service) {
        struct {
            u8 last_modified_event;
            u8 padding[7];
            u64 application_id;
        } __attribute__((packed)) in = {
            .last_modified_event = 1,
            .application_id = base_title_id
        };

        rc = serviceDispatchIn(&ns_app_man_srv, 16, in,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
            .buffers = { { &storage_record, sizeof(storage_record) } },
        );

        if (hosversionAtLeast(3, 0, 0)) {
            serviceClose(&ns_app_man_srv);
        }

        if (R_SUCCEEDED(rc)) {
            LOG_INFO("Stream Install: ✓ Application record pushed!");
        } else {
            LOG_WARN("Stream Install: Failed to push record: 0x%08X", rc);
        }
    }

    ctx->state = STREAM_STATE_COMPLETE;
    LOG_INFO("Stream Install: ✓✓✓ COMPLETE! TitleID=0x%016lX, file=%s", title_id, ctx->filename);

    return 0;
}

StreamInstallState streamInstallGetState(const StreamInstallContext* ctx) {
    if (!ctx) return STREAM_STATE_ERROR;
    return ctx->state;
}

float streamInstallGetProgress(const StreamInstallContext* ctx) {
    if (!ctx || ctx->file_size == 0) return 0.0f;
    return (float)ctx->total_received / (float)ctx->file_size;
}

u64 streamInstallGetTitleId(const StreamInstallContext* ctx) {
    if (!ctx) return 0;
    return ctx->title_id;
}

const char* streamInstallGetStageString(const StreamInstallContext* ctx) {
    if (!ctx) return "Unknown";

    switch (ctx->state) {
        case STREAM_STATE_IDLE:
            return "Idle";
        case STREAM_STATE_RECEIVING:
            return "Receiving";
        case STREAM_STATE_PARSING:
            return "Parsing";
        case STREAM_STATE_INSTALLING:
            if (ctx->ticket_data && !ctx->ticket_imported) {
                return "Installing Ticket";
            }
            return "Installing";
        case STREAM_STATE_COMPLETE:
            return "Complete";
        case STREAM_STATE_ERROR:
            return "Error";
        default:
            return "Unknown";
    }
}

bool streamInstallIsWaitingForTicketResponse(const StreamInstallContext* ctx) {
    if (!ctx) return false;
    return ctx->waiting_for_user_response;
}

bool streamInstallGetTicketInfo(const StreamInstallContext* ctx, u8* out_rights_id, u64* out_device_id, u32* out_account_id) {
    if (!ctx || !ctx->ticket_is_personalized || !ctx->ticket_data) {
        return false;
    }

    return checkTicketMismatch(ctx->ticket_data, ctx->ticket_size,
                                   out_rights_id, out_device_id, out_account_id) != 0;
}

void streamInstallSetTicketConversionApproved(StreamInstallContext* ctx, bool approved) {
    if (!ctx) return;

    ctx->ticket_conversion_approved = approved;
    ctx->waiting_for_user_response = false;

    if (approved && ctx->ticket_data) {
        // Convert the ticket to common
        convertTicketToCommon(ctx->ticket_data, ctx->ticket_size);
        LOG_INFO("Stream Install: User approved ticket conversion");
    } else if (!approved) {
        // User rejected - set error state to stop installation
        ctx->state = STREAM_STATE_ERROR;
        ctx->last_error = MAKERESULT(Module_Libnx, LibnxError_NotFound);  // Use a meaningful error code
        LOG_INFO("Stream Install: User rejected ticket conversion - cancelling install");
    }
}

bool streamInstallShouldPostTicketEvent(StreamInstallContext* ctx) {
    if (!ctx) return false;

    // Only post event once
    if (ctx->waiting_for_user_response && !ctx->ticket_event_posted) {
        ctx->ticket_event_posted = true;
        return true;
    }
    return false;
}
