// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_protocol.h"
#include "mtp/mtp_storage.h"
#include "mtp/mtp_install.h"
#include "mtp/mtp_saves.h"
#include "mtp/mtp_dump.h"
#include "mtp/mtp_gamecard.h"
#include "mtp/mtp_log.h"
#include "mtp/usb_mtp.h"
#include "install/stream_install.h"
#include "core/TransferEvents.h"
#include "core/Event.h"
#include "core/Settings.h"
#include "core/Debug.h"
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <unordered_map>

using namespace Javelin;

#define MTP_TIMEOUT_NS 5000000000ULL

static std::unordered_map<std::string, bool> g_transfer_cancelled;
static Mutex g_transfer_mutex = {0};

static inline void transfer_cancel_set(const std::string& path, bool value) {
    mutexLock(&g_transfer_mutex);
    g_transfer_cancelled[path] = value;
    mutexUnlock(&g_transfer_mutex);
}

static inline bool transfer_cancel_check(const std::string& path) {
    mutexLock(&g_transfer_mutex);
    auto it = g_transfer_cancelled.find(path);
    bool cancelled = (it != g_transfer_cancelled.end() && it->second);
    mutexUnlock(&g_transfer_mutex);
    return cancelled;
}

static inline void transfer_cancel_erase(const std::string& path) {
    mutexLock(&g_transfer_mutex);
    g_transfer_cancelled.erase(path);
    mutexUnlock(&g_transfer_mutex);
}

static void send_response(MtpProtocolContext* ctx, u16 response_code, u32 transaction_id, u32* params, u32 param_count) {
    MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;

    hdr->length = sizeof(MtpContainerHeader) + (param_count * sizeof(u32));
    hdr->type = MTP_CONTAINER_TYPE_RESPONSE;
    hdr->code = response_code;
    hdr->transaction_id = transaction_id;

    if (param_count > 0 && params != NULL) {
        memcpy(ctx->tx_buffer + sizeof(MtpContainerHeader), params, param_count * sizeof(u32));
    }

    size_t written = usbMtpWrite(ctx->tx_buffer, hdr->length, MTP_TIMEOUT_NS);

    if (written != hdr->length) {
        LOG_ERROR("Response 0x%04X: write failed! wrote %zu of %u bytes",
                  response_code, written, hdr->length);
    } else {
#if DEBUG_MTP_PROTO
        LOG_DEBUG("Response 0x%04X sent OK (%u bytes)", response_code, hdr->length);
#endif
    }
}

static void send_data(MtpProtocolContext* ctx, u16 operation_code, u32 transaction_id, const void* data, u32 data_size) {
    MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;

    hdr->length = sizeof(MtpContainerHeader) + data_size;
    hdr->type = MTP_CONTAINER_TYPE_DATA;
    hdr->code = operation_code;
    hdr->transaction_id = transaction_id;

    if (data_size > 0) {
        memcpy(ctx->tx_buffer + sizeof(MtpContainerHeader), data, data_size);
    }

    size_t written = usbMtpWrite(ctx->tx_buffer, hdr->length, MTP_TIMEOUT_NS);

    if (written != hdr->length) {
        LOG_ERROR("Data 0x%04X: write failed! wrote %zu of %u bytes",
                  operation_code, written, hdr->length);
    } else {
#if DEBUG_MTP_PROTO
        LOG_DEBUG("Data 0x%04X sent OK (%u bytes payload)", operation_code, data_size);
#endif
    }
}

static u32 write_mtp_string(u8* buffer, const char* str) {
    if (!str || !str[0]) {
        *buffer = 0;
        return 1;
    }

    size_t len = strlen(str);
    u8* ptr = buffer;

    *ptr++ = (u8)(len + 1);

    for (size_t i = 0; i < len; i++) {
        *ptr++ = (u8)str[i];
        *ptr++ = 0x00;
    }

    *ptr++ = 0x00;
    *ptr++ = 0x00;

    return (ptr - buffer);
}

// Convert UTF-16LE to UTF-8
// Returns number of bytes written to out (excluding null terminator)
static size_t utf16le_to_utf8(const u8* utf16_data, size_t utf16_chars, char* out, size_t out_size) {
    if (!utf16_data || !out || out_size == 0 || utf16_chars == 0) {
        if (out && out_size > 0) out[0] = '\0';
        return 0;
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < utf16_chars && out_pos < out_size - 1; i++) {
        u16 c = utf16_data[i * 2] | ((u16)utf16_data[i * 2 + 1] << 8);

        if (c == 0) break;  // Null terminator

        // Handle surrogate pairs (for characters outside BMP)
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < utf16_chars) {
            u16 c2 = utf16_data[(i + 1) * 2] | ((u16)utf16_data[(i + 1) * 2 + 1] << 8);
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                // Valid surrogate pair
                u32 codepoint = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i++;  // Skip the low surrogate

                // Encode as 4-byte UTF-8
                if (out_pos + 4 <= out_size - 1) {
                    out[out_pos++] = (char)(0xF0 | ((codepoint >> 18) & 0x07));
                    out[out_pos++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                    out[out_pos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    out[out_pos++] = (char)(0x80 | (codepoint & 0x3F));
                } else {
                    break;  // Not enough space
                }
                continue;
            }
        }

        // BMP character
        if (c < 0x80) {
            out[out_pos++] = (char)c;
        } else if (c < 0x800) {
            if (out_pos + 2 <= out_size - 1) {
                out[out_pos++] = (char)(0xC0 | ((c >> 6) & 0x1F));
                out[out_pos++] = (char)(0x80 | (c & 0x3F));
            } else {
                break;
            }
        } else {
            if (out_pos + 3 <= out_size - 1) {
                out[out_pos++] = (char)(0xE0 | ((c >> 12) & 0x0F));
                out[out_pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[out_pos++] = (char)(0x80 | (c & 0x3F));
            } else {
                break;
            }
        }
    }

    out[out_pos] = '\0';
    return out_pos;
}

// Sanitize filename for FAT32 compatibility
// Replaces illegal characters with '_' and handles UTF-8 characters that may cause issues
static void sanitize_filename_fat32(char* filename) {
    if (!filename) return;

    // FAT32 illegal characters: \ / : * ? " < > |
    // Also handle control characters (0x00-0x1F)
    for (char* p = filename; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20) {
            *p = '_';  // Control characters
        } else {
            switch (*p) {
                case '\\':
                case '/':
                case ':':
                case '*':
                case '?':
                case '"':
                case '<':
                case '>':
                case '|':
                    *p = '_';
                    break;
            }
        }
    }

    // Trim trailing spaces and dots (FAT32 doesn't allow them at the end)
    size_t len = strlen(filename);
    while (len > 0 && (filename[len - 1] == ' ' || filename[len - 1] == '.')) {
        filename[--len] = '\0';
    }
}

static u32 write_u32_array(u8* buffer, const u32* array, u32 count) {
    u8* ptr = buffer;

    memcpy(ptr, &count, sizeof(u32));
    ptr += sizeof(u32);

    if (count > 0) {
        memcpy(ptr, array, count * sizeof(u32));
        ptr += count * sizeof(u32);
    }

    return (ptr - buffer);
}

static void handle_get_device_info(MtpProtocolContext* ctx, u32 transaction_id) {
    u8* data = ctx->tx_buffer + sizeof(MtpContainerHeader);
    u8* ptr = data;

    *(u16*)ptr = 100;
    ptr += 2;

    *(u32*)ptr = 0x00000006;
    ptr += 4;

    *(u16*)ptr = 100;
    ptr += 2;

    ptr += write_mtp_string(ptr, "microsoft.com: 1.0; android.com: 1.0;");

    *(u16*)ptr = 0x0000;
    ptr += 2;

    u16 operations[] = {
        MTP_OP_GET_DEVICE_INFO,
        MTP_OP_OPEN_SESSION,
        MTP_OP_CLOSE_SESSION,
        MTP_OP_GET_STORAGE_IDS,
        MTP_OP_GET_STORAGE_INFO,
        MTP_OP_GET_NUM_OBJECTS,
        MTP_OP_GET_OBJECT_HANDLES,
        MTP_OP_GET_OBJECT_INFO,
        MTP_OP_GET_OBJECT,
        MTP_OP_DELETE_OBJECT,
        MTP_OP_SEND_OBJECT_INFO,
        MTP_OP_SEND_OBJECT,
    };
    u32 op_count = sizeof(operations) / sizeof(u16);
    *(u32*)ptr = op_count;
    ptr += 4;
    memcpy(ptr, operations, op_count * sizeof(u16));
    ptr += op_count * sizeof(u16);

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    u16 formats[] = {0x3000, 0x3001};
    u32 format_count = sizeof(formats) / sizeof(u16);
    *(u32*)ptr = format_count;
    ptr += 4;
    memcpy(ptr, formats, format_count * sizeof(u16));
    ptr += format_count * sizeof(u16);

    ptr += write_mtp_string(ptr, "Nintendo");
    ptr += write_mtp_string(ptr, "Switch");
    ptr += write_mtp_string(ptr, "1.0");
    ptr += write_mtp_string(ptr, "123456789");

    u32 data_size = ptr - data;
    send_data(ctx, MTP_OP_GET_DEVICE_INFO, transaction_id, data, data_size);
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_open_session(MtpProtocolContext* ctx, u32 transaction_id, u32 session_id) {
    LOG_DEBUG("OpenSession: session_id=%u (current: open=%d, id=%u)",
             session_id, ctx->session_open, ctx->session_id);

    if (ctx->session_open) {
        LOG_WARN("OpenSession: Session already open!");
        u32 params[] = {ctx->session_id};
        send_response(ctx, MTP_RESP_SESSION_ALREADY_OPEN, transaction_id, params, 1);
        return;
    }

    ctx->session_open = true;
    ctx->session_id = session_id;
    LOG_DEBUG("OpenSession: SUCCESS - session %u opened", session_id);
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_close_session(MtpProtocolContext* ctx, u32 transaction_id) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    ctx->session_open = false;
    ctx->session_id = 0;
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_get_storage_ids(MtpProtocolContext* ctx, u32 transaction_id) {
    LOG_DEBUG("GetStorageIDs: session_open=%d", ctx->session_open);

    if (!ctx->session_open) {
        LOG_WARN("GetStorageIDs: Session not open, sending error response");
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    u8* data = ctx->tx_buffer + sizeof(MtpContainerHeader);

    u32 storage_ids[16];
    u32 count = mtpStorageGetIds(&ctx->storage, storage_ids, 12);

    LOG_DEBUG("GetStorageIDs: real storages=%u (sd=%d, user=%d, system=%d)",
             count, ctx->storage.sdcard.mounted, ctx->storage.user.mounted, ctx->storage.system.mounted);

    storage_ids[count++] = MTP_STORAGE_INSTALL_SD;
    storage_ids[count++] = MTP_STORAGE_INSTALL_NAND;
    storage_ids[count++] = MTP_STORAGE_SAVES;
    storage_ids[count++] = MTP_STORAGE_DUMP;
    storage_ids[count++] = MTP_STORAGE_GAMECARD;

    if (ctx->storage.album.mounted) {
        storage_ids[count++] = MTP_STORAGE_ALBUM;
    }

    LOG_DEBUG("GetStorageIDs: returning %u total storages", count);

    u32 data_size = write_u32_array(data, storage_ids, count);

    send_data(ctx, MTP_OP_GET_STORAGE_IDS, transaction_id, data, data_size);
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_get_storage_info(MtpProtocolContext* ctx, u32 transaction_id, u32 storage_id) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

#if DEBUG_MTP_PROTO
    LOG_DEBUG("GetStorageInfo: storage_id=0x%08X", storage_id);
#endif

    MtpStorageInfo info;

    if (installIsVirtualStorage(storage_id)) {
        if (!installGetStorageInfo(&ctx->install, storage_id, &info)) {
            send_response(ctx, MTP_RESP_INVALID_STORAGE_ID, transaction_id, NULL, 0);
            return;
        }
    } else if (savesIsVirtualStorage(storage_id)) {
        if (!savesGetStorageInfo(&ctx->saves, storage_id, &info)) {
            send_response(ctx, MTP_RESP_INVALID_STORAGE_ID, transaction_id, NULL, 0);
            return;
        }
    } else if (dumpIsVirtualStorage(storage_id)) {
        if (!dumpGetStorageInfo(&ctx->dump, storage_id, &info)) {
            send_response(ctx, MTP_RESP_INVALID_STORAGE_ID, transaction_id, NULL, 0);
            return;
        }
    } else if (gcIsVirtualStorage(storage_id)) {
        if (!gcGetStorageInfo(&ctx->gamecard, storage_id, &info)) {
            send_response(ctx, MTP_RESP_INVALID_STORAGE_ID, transaction_id, NULL, 0);
            return;
        }
    } else if (!mtpStorageGetInfo(&ctx->storage, storage_id, &info)) {
        send_response(ctx, MTP_RESP_INVALID_STORAGE_ID, transaction_id, NULL, 0);
        return;
    }

    LOG_DEBUG("GetStorageInfo: storage=0x%08X, desc=%s", storage_id, info.description);

    u8* data = ctx->tx_buffer + sizeof(MtpContainerHeader);
    u8* ptr = data;

    *(u16*)ptr = info.storage_type;
    ptr += 2;

    *(u16*)ptr = info.filesystem_type;
    ptr += 2;

    *(u16*)ptr = info.access_capability;
    ptr += 2;

    *(u64*)ptr = info.max_capacity;
    ptr += 8;

    *(u64*)ptr = info.free_space;
    ptr += 8;

    *(u32*)ptr = 0xFFFFFFFF;
    ptr += 4;

    ptr += write_mtp_string(ptr, info.description);
    ptr += write_mtp_string(ptr, info.volume_label);

    u32 data_size = ptr - data;
    send_data(ctx, MTP_OP_GET_STORAGE_INFO, transaction_id, data, data_size);
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_get_num_objects(MtpProtocolContext* ctx, u32 transaction_id,
                                    u32 storage_id, u32 format, u32 parent_handle) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    u32 count;

    if (installIsVirtualStorage(storage_id)) {
        count = installGetObjectCount(&ctx->install, storage_id, parent_handle);
    } else if (savesIsVirtualStorage(storage_id)) {
        count = savesGetObjectCount(&ctx->saves, storage_id, parent_handle);
    } else if (dumpIsVirtualStorage(storage_id)) {
        count = dumpGetObjectCount(&ctx->dump, storage_id, parent_handle);
    } else if (gcIsVirtualStorage(storage_id)) {
        count = gcGetObjectCount(&ctx->gamecard, storage_id, parent_handle);
    } else {
        count = mtpStorageGetObjectCount(&ctx->storage, storage_id, parent_handle);
    }

#if DEBUG_MTP_PROTO
    LOG_DEBUG("GetNumObjects: storage=0x%08X, parent=0x%08X, count=%u",
              storage_id, parent_handle, count);
#endif

    u32 params[] = {count};
    send_response(ctx, MTP_RESP_OK, transaction_id, params, 1);
}

static void handle_get_object_handles(MtpProtocolContext* ctx, u32 transaction_id,
                                       u32 storage_id, u32 format, u32 parent_handle) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    u8* data = ctx->tx_buffer + sizeof(MtpContainerHeader);

    u32 handles[1024];
    u32 count = 0;

    if (installIsVirtualStorage(storage_id)) {
        if (parent_handle == 0 || parent_handle == 0xFFFFFFFF) {
            count = installEnumObjects(&ctx->install, storage_id, parent_handle, handles, 1024);
        } else {
            count = 0;
        }
    } else if (savesIsVirtualStorage(storage_id)) {
        count = savesEnumObjects(&ctx->saves, storage_id, parent_handle, handles, 1024);
    } else if (dumpIsVirtualStorage(storage_id)) {
        count = dumpEnumObjects(&ctx->dump, storage_id, parent_handle, handles, 1024);
    } else if (gcIsVirtualStorage(storage_id)) {
        count = gcEnumObjects(&ctx->gamecard, storage_id, parent_handle, handles, 1024);
    } else {
        count = mtpStorageEnumObjects(&ctx->storage, storage_id, parent_handle, handles, 1024);
    }

#if DEBUG_MTP_PROTO
    LOG_DEBUG("GetObjectHandles: returning %u handles", count);
#endif

    u32 data_size = write_u32_array(data, handles, count);

    send_data(ctx, MTP_OP_GET_OBJECT_HANDLES, transaction_id, data, data_size);
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_get_object_info(MtpProtocolContext* ctx, u32 transaction_id, u32 handle) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    MtpObject obj;

    if (installIsVirtualHandle(handle)) {
        if (!installGetObjectInfo(&ctx->install, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }
    } else if (savesIsVirtualHandle(handle)) {
        if (!savesGetObjectInfo(&ctx->saves, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }
    } else if (dumpIsVirtualHandle(handle)) {
        if (!dumpGetObjectInfo(&ctx->dump, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }
    } else if (gcIsVirtualHandle(handle)) {
        if (!gcGetObjectInfo(&ctx->gamecard, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }
    } else if (!mtpStorageGetObject(&ctx->storage, handle, &obj)) {
        send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
        return;
    }

#if DEBUG_MTP_PROTO
    LOG_DEBUG("GetObjectInfo: handle=0x%08X, name=%s", handle, obj.filename);
#endif

    u8* data = ctx->tx_buffer + sizeof(MtpContainerHeader);
    u8* ptr = data;

    *(u32*)ptr = obj.storage_id;
    ptr += 4;

    *(u16*)ptr = obj.format;
    ptr += 2;

    *(u16*)ptr = 0x0000;
    ptr += 2;

    *(u32*)ptr = (u32)(obj.size & 0xFFFFFFFF);
    ptr += 4;

    *(u16*)ptr = 0x0000;
    ptr += 2;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = (obj.parent_handle == 0xFFFFFFFF) ? 0 : obj.parent_handle;
    ptr += 4;

    *(u16*)ptr = (obj.object_type == MTP_OBJECT_TYPE_FOLDER) ? 0x0001 : 0x0000;
    ptr += 2;

    *(u32*)ptr = 0;
    ptr += 4;

    *(u32*)ptr = 0;
    ptr += 4;

    ptr += write_mtp_string(ptr, obj.filename);
    ptr += write_mtp_string(ptr, "");
    ptr += write_mtp_string(ptr, "");
    ptr += write_mtp_string(ptr, "");

    u32 data_size = ptr - data;
    send_data(ctx, MTP_OP_GET_OBJECT_INFO, transaction_id, data, data_size);
    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
}

static void handle_get_object(MtpProtocolContext* ctx, u32 transaction_id, u32 handle) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    u32 placeholder_sd = MTP_HANDLE_INSTALL_SD_BASE | 0x0001;
    u32 placeholder_nand = MTP_HANDLE_INSTALL_NAND_BASE | 0x0001;

    if (handle == placeholder_sd || handle == placeholder_nand) {
        const char* placeholder_text =
            "Javelin NSP/XCI Installer\n"
            "=======================\n\n"
            "Drag and drop NSP or XCI files here to install them to your Nintendo Switch.\n\n"
            "Supported formats:\n"
            "  - .nsp (Nintendo Submission Package)\n"
            "  - .nsz (Compressed NSP)\n"
            "  - .xci (Gamecard Image)\n"
            "  - .xcz (Compressed XCI)\n\n"
            "Installation will begin automatically when you transfer a file.\n";

        u32 text_len = strlen(placeholder_text);

        MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;
        hdr->length = sizeof(MtpContainerHeader) + text_len;
        hdr->type = MTP_CONTAINER_TYPE_DATA;
        hdr->code = MTP_OP_GET_OBJECT;
        hdr->transaction_id = transaction_id;

        usbMtpWrite(ctx->tx_buffer, sizeof(MtpContainerHeader), MTP_TIMEOUT_NS);

        memcpy(ctx->tx_buffer, placeholder_text, text_len);
        usbMtpWrite(ctx->tx_buffer, text_len, MTP_TIMEOUT_NS);

        send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
        return;
    }

    if (installIsVirtualHandle(handle)) {
#if DEBUG_MTP_PROTO
        LOG_DEBUG("GetObject: Cannot read from virtual install folder (write-only)");
#endif
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
        return;
    }

    if (savesIsVirtualHandle(handle)) {
        MtpObject obj;
        if (!savesGetObjectInfo(&ctx->saves, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }

        if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
            send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
            return;
        }

#if DEBUG_MTP_PROTO
        LOG_DEBUG("GetObject (saves): handle=0x%08X, name=%s, size=%lu",
                  handle, obj.filename, (unsigned long)obj.size);
#endif

        MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;
        hdr->length = sizeof(MtpContainerHeader) + obj.size;
        hdr->type = MTP_CONTAINER_TYPE_DATA;
        hdr->code = MTP_OP_GET_OBJECT;
        hdr->transaction_id = transaction_id;

        usbMtpWrite(ctx->tx_buffer, sizeof(MtpContainerHeader), MTP_TIMEOUT_NS);

        u64 offset = 0;
        u64 remaining = obj.size;

        while (remaining > 0) {
            u32 chunk_size = (remaining > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining;

            s64 read = savesReadObject(&ctx->saves, handle, offset, ctx->tx_buffer, chunk_size);
            if (read <= 0) {
                break;
            }

            usbMtpWriteDirect(ctx->tx_buffer, read, MTP_TIMEOUT_NS);

            offset += read;
            remaining -= read;
        }

        send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
        return;
    }

    if (dumpIsVirtualHandle(handle)) {
        MtpObject obj;
        if (!dumpGetObjectInfo(&ctx->dump, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }

        if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
            send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
            return;
        }

#if DEBUG_MTP_PROTO
        LOG_DEBUG("GetObject (dump): handle=0x%08X, name=%s, size=%lu",
                  handle, obj.filename, (unsigned long)obj.size);
#endif

        MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;
        if (obj.size > 0xFFFFFFFFULL - sizeof(MtpContainerHeader)) {
            hdr->length = 0xFFFFFFFF;
        } else {
            hdr->length = sizeof(MtpContainerHeader) + (u32)obj.size;
        }
        hdr->type = MTP_CONTAINER_TYPE_DATA;
        hdr->code = MTP_OP_GET_OBJECT;
        hdr->transaction_id = transaction_id;

        usbMtpWrite(ctx->tx_buffer, sizeof(MtpContainerHeader), MTP_TIMEOUT_NS);

        TransferStartEvent startEvt(
            std::string(obj.filename),
            obj.size,
            TransferStartEvent::Direction::Download
        );
        startEvt.cancelledPtr = nullptr;
        EventBus::getInstance().post(startEvt);

        u64 transfer_start_time = armGetSystemTick();
        u64 offset = 0;
        u64 remaining = obj.size;
        bool transfer_failed = false;
        u64 progress_tick_interval = armGetSystemTickFreq() / 10;
        u64 last_progress_tick = transfer_start_time;
        u64 last_progress_bytes = 0;

        u8* dump_read_buf = ctx->tx_buffer;
        u8* dump_write_buf = ctx->alt_buffer;
        s64 dump_pending_write = 0;

        {
            u32 chunk_size = (remaining > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining;
            s64 rd = dumpReadObject(&ctx->dump, handle, offset, dump_read_buf, chunk_size);
            if (rd <= 0) {
                transfer_failed = true;
            } else {
                dump_pending_write = rd;
                u8* temp = dump_read_buf;
                dump_read_buf = dump_write_buf;
                dump_write_buf = temp;
            }
        }

        while (!transfer_failed && dump_pending_write > 0) {
            u64 after_write = offset + dump_pending_write;
            u64 remaining_after = (after_write < obj.size) ? (obj.size - after_write) : 0;
            s64 next_read_size = 0;

            if (!usbMtpWriteDirectStart(dump_write_buf, dump_pending_write)) {
                transfer_failed = true;
                break;
            }

            if (remaining_after > 0) {
                u32 next_chunk = (remaining_after > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining_after;
                next_read_size = dumpReadObject(&ctx->dump, handle, after_write, dump_read_buf, next_chunk);
            }

            size_t usb_written = usbMtpWriteDirectFinish(MTP_TIMEOUT_NS);
            if (usb_written == 0) {
                transfer_failed = true;
                break;
            }
            offset += usb_written;
            remaining -= usb_written;

            u64 now_ticks = armGetSystemTick();
            if ((now_ticks - last_progress_tick) >= progress_tick_interval) {
                float percent = (obj.size > 0)
                                ? ((float)offset / (float)obj.size) * 100.0f
                                : 100.0f;
                u64 recent_ticks = now_ticks - last_progress_tick;
                float recent_sec = (float)((double)recent_ticks / armGetSystemTickFreq());
                u64 recent_bytes = offset - last_progress_bytes;
                float speed = (recent_sec > 0.01f)
                              ? ((float)recent_bytes / (1024.0f * 1024.0f)) / recent_sec
                              : 0.0f;

                TransferProgressEvent progressEvt(
                    std::string(obj.filename), offset, obj.size, percent, speed);
                EventBus::getInstance().post(progressEvt);
                last_progress_tick = now_ticks;
                last_progress_bytes = offset;
            }

            if (remaining_after > 0 && next_read_size > 0) {
                dump_pending_write = next_read_size;
                u8* temp = dump_read_buf;
                dump_read_buf = dump_write_buf;
                dump_write_buf = temp;
            } else if (remaining_after > 0 && next_read_size <= 0) {
                transfer_failed = true;
                dump_pending_write = 0;
            } else {
                dump_pending_write = 0;
            }
        }

        if (!transfer_failed && obj.size > 0) {
            u64 elapsed_ticks = armGetSystemTick() - transfer_start_time;
            float elapsed_sec = (float)((double)elapsed_ticks / armGetSystemTickFreq());
            float speed = (elapsed_sec > 0.0f)
                          ? ((float)offset / (1024.0f * 1024.0f)) / elapsed_sec : 0.0f;
            TransferProgressEvent finalEvt(
                std::string(obj.filename), offset, obj.size, 100.0f, speed);
            EventBus::getInstance().post(finalEvt);
        }

        send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);

        TransferCompleteEvent completeEvt(
            std::string(obj.filename),
            offset,
            !transfer_failed,
            transfer_failed ? "Dump read failed" : "");
        EventBus::getInstance().post(completeEvt);

        return;
    }

    if (gcIsVirtualHandle(handle)) {
        MtpObject obj;
        if (!gcGetObjectInfo(&ctx->gamecard, handle, &obj)) {
            send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
            return;
        }

#if DEBUG_MTP_PROTO
        LOG_DEBUG("GetObject (gamecard): handle=0x%08X, name=%s, size=%llu",
                  handle, obj.filename, (unsigned long long)obj.size);
#endif

        // Games commonly exceed 4 GB; signal extended length (0xFFFFFFFF) when
        // the payload overflows a u32, as required by the MTP spec.
        MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;
        if (obj.size > 0xFFFFFFFFULL - sizeof(MtpContainerHeader)) {
            hdr->length = 0xFFFFFFFF;
        } else {
            hdr->length = (u32)(sizeof(MtpContainerHeader) + obj.size);
        }
        hdr->type = MTP_CONTAINER_TYPE_DATA;
        hdr->code = MTP_OP_GET_OBJECT;
        hdr->transaction_id = transaction_id;

        usbMtpWrite(ctx->tx_buffer, sizeof(MtpContainerHeader), MTP_TIMEOUT_NS);

        TransferStartEvent startEvt(
            std::string(obj.filename),
            obj.size,
            TransferStartEvent::Direction::Download
        );
        startEvt.cancelledPtr = nullptr;
        EventBus::getInstance().post(startEvt);

        u64 transfer_start_time = armGetSystemTick();
        u64 offset = 0;
        u64 remaining = obj.size;
        bool transfer_failed = false;
        u64 progress_tick_interval = armGetSystemTickFreq() / 10;
        u64 last_progress_tick = transfer_start_time;
        u64 last_progress_bytes = 0;

        // Double-buffered gamecard download: read next chunk from gamecard while
        // USB DMA for the current chunk is in-flight.
        u8* gc_read_buf = ctx->tx_buffer;
        u8* gc_write_buf = ctx->alt_buffer;
        s64 gc_pending_write = 0;

        {
            u32 chunk_size = (remaining > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining;
            s64 read = gcReadObject(&ctx->gamecard, handle, offset, gc_read_buf, chunk_size);
            if (read <= 0) {
                LOG_ERROR("GetObject (gamecard): gcReadObject returned %lld at offset %llu",
                          (long long)read, (unsigned long long)offset);
                transfer_failed = true;
            } else {
                gc_pending_write = read;
                u8* temp = gc_read_buf;
                gc_read_buf = gc_write_buf;
                gc_write_buf = temp;
            }
        }

        while (!transfer_failed && gc_pending_write > 0) {
            u64 after_write = offset + gc_pending_write;
            u64 remaining_after = (after_write < obj.size) ? (obj.size - after_write) : 0;
            s64 next_read_size = 0;

            if (!usbMtpWriteDirectStart(gc_write_buf, gc_pending_write)) {
                transfer_failed = true;
                break;
            }

            if (remaining_after > 0) {
                u32 next_chunk = (remaining_after > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining_after;
                next_read_size = gcReadObject(&ctx->gamecard, handle, after_write, gc_read_buf, next_chunk);
            }

            size_t usb_written = usbMtpWriteDirectFinish(MTP_TIMEOUT_NS);
            if (usb_written == 0) {
                transfer_failed = true;
                break;
            }
            offset += usb_written;
            remaining -= usb_written;

            u64 now_ticks = armGetSystemTick();
            if ((now_ticks - last_progress_tick) >= progress_tick_interval) {
                float percent = (obj.size > 0)
                                ? ((float)offset / (float)obj.size) * 100.0f
                                : 100.0f;
                u64 recent_ticks = now_ticks - last_progress_tick;
                float recent_sec = (float)((double)recent_ticks / armGetSystemTickFreq());
                u64 recent_bytes = offset - last_progress_bytes;
                float speed = (recent_sec > 0.01f)
                              ? ((float)recent_bytes / (1024.0f * 1024.0f)) / recent_sec
                              : 0.0f;

                TransferProgressEvent progressEvt(
                    std::string(obj.filename), offset, obj.size, percent, speed);
                EventBus::getInstance().post(progressEvt);
                last_progress_tick = now_ticks;
                last_progress_bytes = offset;
            }

            if (remaining_after > 0 && next_read_size > 0) {
                gc_pending_write = next_read_size;
                u8* temp = gc_read_buf;
                gc_read_buf = gc_write_buf;
                gc_write_buf = temp;
            } else if (remaining_after > 0 && next_read_size <= 0) {
                LOG_ERROR("GetObject (gamecard): gcReadObject returned %lld at offset %llu",
                          (long long)next_read_size, (unsigned long long)after_write);
                transfer_failed = true;
                gc_pending_write = 0;
            } else {
                gc_pending_write = 0;
            }
        }

        if (!transfer_failed && obj.size > 0) {
            u64 elapsed_ticks = armGetSystemTick() - transfer_start_time;
            float elapsed_sec = (float)((double)elapsed_ticks / armGetSystemTickFreq());
            float speed = (elapsed_sec > 0.0f)
                          ? ((float)offset / (1024.0f * 1024.0f)) / elapsed_sec : 0.0f;
            TransferProgressEvent finalEvt(
                std::string(obj.filename), offset, obj.size, 100.0f, speed);
            EventBus::getInstance().post(finalEvt);
        }

        send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);

        TransferCompleteEvent completeEvt(
            std::string(obj.filename),
            offset,
            !transfer_failed,
            transfer_failed ? "Gamecard read failed" : "");
        EventBus::getInstance().post(completeEvt);

        return;
    }

    MtpObject obj;
    if (!mtpStorageGetObject(&ctx->storage, handle, &obj)) {
        send_response(ctx, MTP_RESP_INVALID_OBJECT_HANDLE, transaction_id, NULL, 0);
        return;
    }

    if (obj.object_type == MTP_OBJECT_TYPE_FOLDER) {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
        return;
    }

#if DEBUG_MTP_PROTO
    LOG_DEBUG("GetObject: handle=0x%08X, name=%s, size=%lu",
              handle, obj.filename, (unsigned long)obj.size);
#endif

    transfer_cancel_set(obj.full_path, false);

    mutexLock(&g_transfer_mutex);
    bool* cancel_ptr = &g_transfer_cancelled[obj.full_path];
    mutexUnlock(&g_transfer_mutex);

    TransferStartEvent startEvent(
        obj.full_path,
        obj.size,
        TransferStartEvent::Direction::Download
    );
    startEvent.cancelled = false;
    startEvent.cancelledPtr = cancel_ptr;
    EventBus::getInstance().post(startEvent);

    MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->tx_buffer;
    hdr->length = sizeof(MtpContainerHeader) + obj.size;
    hdr->type = MTP_CONTAINER_TYPE_DATA;
    hdr->code = MTP_OP_GET_OBJECT;
    hdr->transaction_id = transaction_id;

    usbMtpWrite(ctx->tx_buffer, sizeof(MtpContainerHeader), MTP_TIMEOUT_NS);

    u64 transfer_start_time = armGetSystemTick();

    u64 offset = 0;
    u64 remaining = obj.size;
    bool transfer_failed = false;
    u64 last_progress_update = 0;
    u64 last_progress_ticks = transfer_start_time;
    u64 last_progress_bytes = 0;
    u64 chunks_since_cancel_check = 0;

    // Open file once for streaming read to avoid repeated open/close overhead.
    MtpFileHandle* file_handle = mtpStorageOpenRead(&ctx->storage, handle);
    if (!file_handle) {
        transfer_failed = true;
    } else {
        // Double-buffered download: overlaps SD card reads with USB DMA writes.
        // Uses usbMtpWriteDirect (zero-copy) to avoid an extra memcpy per chunk.
        u8* read_buf = ctx->tx_buffer;
        u8* write_buf = ctx->alt_buffer;
        s64 pending_write_size = 0;
        u64 progress_tick_interval = armGetSystemTickFreq() / 10;
        u64 last_progress_tick = transfer_start_time;

        {
            u32 chunk_size = (remaining > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining;
            s64 read = mtpStorageReadFile(file_handle, read_buf, chunk_size);
            if (read <= 0) {
                transfer_failed = true;
            } else {
                pending_write_size = read;
                u8* temp = read_buf;
                read_buf = write_buf;
                write_buf = temp;
            }
        }

        while (!transfer_failed && pending_write_size > 0) {
            if (++chunks_since_cancel_check > 5) {
                if (__atomic_load_n(cancel_ptr, __ATOMIC_ACQUIRE)) {
                    transfer_failed = true;
                    break;
                }
                chunks_since_cancel_check = 0;
            }

            u64 after_write = offset + pending_write_size;
            u64 remaining_after = (after_write < obj.size) ? (obj.size - after_write) : 0;
            s64 next_read_size = 0;

            if (!usbMtpWriteDirectStart(write_buf, pending_write_size)) {
                transfer_failed = true;
                break;
            }

            if (remaining_after > 0) {
                u32 next_chunk = (remaining_after > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining_after;
                next_read_size = mtpStorageReadFile(file_handle, read_buf, next_chunk);
            }

            size_t usb_written = usbMtpWriteDirectFinish(MTP_TIMEOUT_NS);
            if (usb_written == 0) {
                transfer_failed = true;
                break;
            }
            offset += usb_written;
            remaining -= usb_written;

            u64 now_ticks = armGetSystemTick();
            if ((now_ticks - last_progress_tick) >= progress_tick_interval) {
                float percent = ((float)offset / obj.size) * 100.0f;
                u64 recent_ticks = now_ticks - last_progress_ticks;
                float recent_sec = (float)((double)recent_ticks / armGetSystemTickFreq());
                u64 recent_bytes = offset - last_progress_bytes;
                float speed = (recent_sec > 0.01f) ? (recent_bytes / (1024.0f * 1024.0f)) / recent_sec : 0.0f;

                TransferProgressEvent progressEvent(obj.full_path, offset, obj.size, percent, speed);
                EventBus::getInstance().post(progressEvent);
                last_progress_ticks = now_ticks;
                last_progress_bytes = offset;
                last_progress_tick = now_ticks;
            }

            if (remaining_after > 0 && next_read_size > 0) {
                pending_write_size = next_read_size;
                u8* temp = read_buf;
                read_buf = write_buf;
                write_buf = temp;
            } else if (remaining_after > 0 && next_read_size <= 0) {
                transfer_failed = true;
                pending_write_size = 0;
            } else {
                pending_write_size = 0;
            }
        }

        mtpStorageCloseFile(file_handle);
    }

    if (!transfer_failed && offset > 0) {
        u64 elapsed_ticks = armGetSystemTick() - transfer_start_time;
        float elapsed_sec = (float)((double)elapsed_ticks / armGetSystemTickFreq());
        float speed = (elapsed_sec > 0) ? (offset / (1024.0f * 1024.0f)) / elapsed_sec : 0.0f;
        TransferProgressEvent finalProgressEvent(obj.full_path, offset, obj.size, 100.0f, speed);
        EventBus::getInstance().post(finalProgressEvent);
    }

    send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);

    bool was_cancelled = __atomic_load_n(cancel_ptr, __ATOMIC_ACQUIRE);
    TransferCompleteEvent completeEvent(
        obj.full_path,
        offset,
        !transfer_failed,
        transfer_failed ? (was_cancelled ? "Transfer cancelled" : "Transfer failed") : ""
    );
    EventBus::getInstance().post(completeEvent);

    transfer_cancel_erase(obj.full_path);
}

static u32 g_pending_storage_id = 0;
static u32 g_pending_parent_handle = 0;
static u32 g_pending_object_handle = 0;
static u64 g_pending_object_size = 0;
static char g_pending_filename[MTP_MAX_FILENAME] = "";

static void handle_send_object_info(MtpProtocolContext* ctx, u32 transaction_id,
                                     u32 storage_id, u32 parent_handle) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

#if DEBUG_MTP_PROTO
    LOG_DEBUG("SendObjectInfo: storage=0x%08X, parent=0x%08X", storage_id, parent_handle);
#endif

    size_t read_bytes = usbMtpRead(ctx->rx_buffer, ctx->buffer_size, MTP_TIMEOUT_NS);
    if (read_bytes < sizeof(MtpContainerHeader)) {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
        return;
    }

    MtpContainerHeader* data_hdr = (MtpContainerHeader*)ctx->rx_buffer;
    if (data_hdr->type != MTP_CONTAINER_TYPE_DATA) {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
        return;
    }

    u8* data = ctx->rx_buffer + sizeof(MtpContainerHeader);

    u16 format = *(u16*)(data + 4);
    u32 obj_size = *(u32*)(data + 8);

    u8* str_ptr = data + 52;

    char filename[MTP_MAX_FILENAME];
    memset(filename, 0, sizeof(filename));

    u8 str_len = str_ptr[0];
    if (str_len > 0 && str_len < 128) {
        str_ptr++;
        // Convert UTF-16LE to UTF-8 (str_len includes null terminator, so actual chars = str_len - 1)
        utf16le_to_utf8(str_ptr, str_len - 1, filename, sizeof(filename));
    }

    // Sanitize filename for FAT32 compatibility
    sanitize_filename_fat32(filename);

    if (filename[0] == '\0') {
        send_response(ctx, MTP_RESP_INVALID_PARAMETER, transaction_id, NULL, 0);
        return;
    }

    if (storage_id == 0 || storage_id == 0xFFFFFFFF) {
        storage_id = MTP_STORAGE_SDCARD;
    }

    if (storage_id == MTP_STORAGE_ALBUM) {
        send_response(ctx, MTP_RESP_STORE_READ_ONLY, transaction_id, NULL, 0);
        return;
    }

    u32 new_handle = 0;

    if (installIsVirtualStorage(storage_id)) {
        new_handle = installCreateObject(&ctx->install, storage_id, parent_handle,
                                         filename, format, obj_size);
        if (new_handle == 0) {
            send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
            return;
        }
    } else if (savesIsVirtualStorage(storage_id)) {
        new_handle = savesCreateObject(&ctx->saves, storage_id, parent_handle,
                                       filename, format, obj_size);
        if (new_handle == 0) {
            send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
            return;
        }
    } else {
        new_handle = mtpStorageCreateObject(&ctx->storage, storage_id, parent_handle,
                                             filename, format, obj_size);
        if (new_handle == 0) {
            send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
            return;
        }
    }

    g_pending_storage_id = storage_id;
    g_pending_parent_handle = parent_handle;
    g_pending_object_handle = new_handle;
    g_pending_object_size = obj_size;
    strncpy(g_pending_filename, filename, sizeof(g_pending_filename) - 1);
    g_pending_filename[sizeof(g_pending_filename) - 1] = '\0';

    u32 params[3] = {storage_id, parent_handle, new_handle};
    send_response(ctx, MTP_RESP_OK, transaction_id, params, 3);
}

static inline s64 write_chunk(MtpProtocolContext* ctx, bool is_install, bool is_saves,
                               MtpFileHandle* file_handle, u32 handle, u64 offset,
                               const void* buffer, size_t size) {
    if (is_install) {
        return installWriteObject(&ctx->install, handle, offset, buffer, size);
    } else if (is_saves) {
        return savesWriteObject(&ctx->saves, handle, offset, buffer, size);
    } else if (file_handle) {
        return mtpStorageWriteFile(file_handle, buffer, size);
    } else {
        return mtpStorageWriteObject(&ctx->storage, handle, offset, buffer, size);
    }
}

static void handle_send_object(MtpProtocolContext* ctx, u32 transaction_id) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    if (g_pending_object_handle == 0) {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
        return;
    }

    bool is_install = installIsVirtualStorage(g_pending_storage_id);
    bool is_saves = savesIsVirtualStorage(g_pending_storage_id);

    MtpObject obj;
    char obj_filename[MTP_MAX_FILENAME];
    if (g_pending_filename[0] != '\0') {
        strncpy(obj_filename, g_pending_filename, sizeof(obj_filename) - 1);
        obj_filename[sizeof(obj_filename) - 1] = '\0';
    } else {
        strncpy(obj_filename, "upload", sizeof(obj_filename) - 1);
        obj_filename[sizeof(obj_filename) - 1] = '\0';
    }

    // Try to get the actual object name from storage (in case it was updated)
    if (mtpStorageGetObject(&ctx->storage, g_pending_object_handle, &obj)) {
        strncpy(obj_filename, obj.filename, sizeof(obj_filename) - 1);
        obj_filename[sizeof(obj_filename) - 1] = '\0';
    }

    static std::unordered_map<u32, TransferStartEvent*> upload_events;
    static std::unordered_map<u32, u64> upload_start_times;

    transfer_cancel_set(std::string(obj_filename), false);

    mutexLock(&g_transfer_mutex);
    bool* cancel_ptr = &g_transfer_cancelled[std::string(obj_filename)];
    mutexUnlock(&g_transfer_mutex);

    // Post different events for install vs transfer
    if (is_install) {
        InstallStartEvent installStartEvt(obj_filename, obj_filename);
        EventBus::getInstance().post(installStartEvt);
    }

    TransferStartEvent* startEvent = new TransferStartEvent(obj_filename, g_pending_object_size,
                                                            TransferStartEvent::Direction::Upload);
    startEvent->cancelledPtr = cancel_ptr;
    upload_events[g_pending_object_handle] = startEvent;
    upload_start_times[g_pending_object_handle] = armGetSystemTick();
    EventBus::getInstance().post(*startEvent);

    size_t read_bytes = usbMtpRead(ctx->rx_buffer, ctx->buffer_size, MTP_TIMEOUT_NS);
    if (read_bytes < sizeof(MtpContainerHeader)) {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);

        TransferCompleteEvent failEvent(obj_filename, 0, false, "Failed to read data header");
        EventBus::getInstance().post(failEvent);

        u32 saved_handle = g_pending_object_handle;
        g_pending_object_handle = 0;
        delete upload_events[saved_handle];
        upload_events.erase(saved_handle);
        transfer_cancel_erase(std::string(obj_filename));
        return;
    }

    MtpContainerHeader* data_hdr = (MtpContainerHeader*)ctx->rx_buffer;
    if (data_hdr->type != MTP_CONTAINER_TYPE_DATA) {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);

        TransferCompleteEvent failEvent(obj_filename, 0, false, "Invalid data container type");
        EventBus::getInstance().post(failEvent);

        u32 saved_handle = g_pending_object_handle;
        g_pending_object_handle = 0;
        delete upload_events[saved_handle];
        upload_events.erase(saved_handle);
        transfer_cancel_erase(std::string(obj_filename));
        return;
    }

    u64 data_size = data_hdr->length - sizeof(MtpContainerHeader);
    u64 offset = 0;
    u64 total_written = 0;
    u64 last_progress_ticks = upload_start_times[g_pending_object_handle];
    u64 last_progress_bytes = 0;
    u64 progress_tick_interval = armGetSystemTickFreq() / 10;

    MtpFileHandle* file_handle = nullptr;
    if (!is_install && !is_saves) {
        file_handle = mtpStorageOpenWrite(&ctx->storage, g_pending_object_handle);
    }

    if (read_bytes > sizeof(MtpContainerHeader)) {
        size_t first_chunk = read_bytes - sizeof(MtpContainerHeader);
        if (first_chunk > data_size) first_chunk = data_size;

        s64 written = write_chunk(ctx, is_install, is_saves, file_handle,
                                  g_pending_object_handle, 0,
                                  ctx->rx_buffer + sizeof(MtpContainerHeader), first_chunk);
        if (written > 0) {
            offset += written;
            total_written += written;
        }
    }

    // Double-buffered upload: overlaps storage writes with USB DMA reads.
    // While chunk N is written to storage, USB DMA for chunk N+1 is already in-flight.
    u8* read_buffer = ctx->rx_buffer;
    u8* write_buffer = ctx->alt_buffer;
    size_t pending_write_size = 0;
    u64 pending_write_offset = 0;

    bool cancel_requested = false;
    u64 chunks_since_cancel_check = 0;

    bool read_posted = false;
    if (offset < data_size) {
        u64 remaining = data_size - offset;
        u32 chunk_size = (remaining > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining;
        read_posted = usbMtpReadDirectStart(read_buffer, chunk_size);
    }

    while (offset < data_size && read_posted) {
        if (++chunks_since_cancel_check > 5) {
            if (__atomic_load_n(cancel_ptr, __ATOMIC_ACQUIRE)) {
                cancel_requested = true;
                // Drain the in-flight DMA before aborting to avoid leaving the
                // USB endpoint in a wedged state.
                usbMtpReadDirectFinish(MTP_TIMEOUT_NS);
                break;
            }
            chunks_since_cancel_check = 0;
        }

        if (pending_write_size > 0) {
            s64 written = write_chunk(ctx, is_install, is_saves, file_handle,
                                      g_pending_object_handle, pending_write_offset,
                                      write_buffer, pending_write_size);
            if (written > 0) {
                total_written += written;

                u64 now_ticks = armGetSystemTick();
                if ((now_ticks - last_progress_ticks) > progress_tick_interval) {
                    float percent = (float)total_written / data_size * 100.0f;
                    u64 recent_ticks = now_ticks - last_progress_ticks;
                    float recent_sec = (float)((double)recent_ticks / armGetSystemTickFreq());
                    u64 recent_bytes = total_written - last_progress_bytes;
                    float speed = (recent_sec > 0.01f) ? (recent_bytes / (1024.0f * 1024.0f)) / recent_sec : 0.0f;

                    // Post install-specific events with stage information
                    if (is_install && ctx->install.stream_ctx) {
                        // Check if we need to prompt user about personalized ticket (only once)
                        if (streamInstallShouldPostTicketEvent(ctx->install.stream_ctx)) {
                            u8 rights_id[16];
                            u64 device_id;
                            u32 account_id;
                            if (streamInstallGetTicketInfo(ctx->install.stream_ctx, rights_id, &device_id, &account_id)) {
                                const char* display_name = ctx->install.stream_ctx->filename;
                                if (display_name[0] == '\0') {
                                    display_name = obj_filename;
                                }
                                PersonalizedTicketEvent ticketEvt(display_name, display_name,
                                                                 rights_id, device_id, account_id,
                                                                 ctx->install.stream_ctx);
                                EventBus::getInstance().post(ticketEvt);
                                LOG_INFO("MTP: Posted PersonalizedTicketEvent for %s", display_name);
                            }
                        }

                        const char* stage = streamInstallGetStageString(ctx->install.stream_ctx);

                        // Use filename from stream context instead of MTP object name
                        const char* display_name = ctx->install.stream_ctx->filename;
                        if (display_name[0] == '\0') {
                            display_name = obj_filename;  // Fallback to MTP object name
                        }

                        InstallProgressEvent installProgressEvt(display_name, "", percent, total_written, data_size, stage);
                        EventBus::getInstance().post(installProgressEvt);
                    }

                    TransferProgressEvent progressEvent(obj_filename, total_written, data_size, percent, speed);
                    EventBus::getInstance().post(progressEvent);
                    last_progress_ticks = now_ticks;
                    last_progress_bytes = total_written;
                }
            }
        }

        size_t chunk_read = usbMtpReadDirectFinish(MTP_TIMEOUT_NS);
        if (chunk_read == 0) {
            break;
        }

        u8* temp = read_buffer;
        read_buffer = write_buffer;
        write_buffer = temp;

        pending_write_size = chunk_read;
        pending_write_offset = offset;
        offset += chunk_read;

        read_posted = false;
        if (offset < data_size) {
            u64 remaining = data_size - offset;
            u32 next_chunk = (remaining > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)remaining;
            read_posted = usbMtpReadDirectStart(read_buffer, next_chunk);
        }
    }

    if (!cancel_requested && pending_write_size > 0) {
        s64 written = write_chunk(ctx, is_install, is_saves, file_handle,
                                  g_pending_object_handle, pending_write_offset,
                                  write_buffer, pending_write_size);
        if (written > 0) {
            total_written += written;
        }
    }

    if (file_handle) {
        mtpStorageFlushFile(file_handle);
        mtpStorageCloseFile(file_handle);
        file_handle = nullptr;
    }

    {
        float percent = (data_size > 0) ? (float)total_written / data_size * 100.0f : 100.0f;
        u64 elapsed_ticks = armGetSystemTick() - upload_start_times[g_pending_object_handle];
        float elapsed_sec = (float)((double)elapsed_ticks / armGetSystemTickFreq());
        float speed = (elapsed_sec > 0) ? (total_written / (1024.0f * 1024.0f)) / elapsed_sec : 0.0f;

        // Post install-specific final progress event with stage
        if (is_install && ctx->install.stream_ctx) {
            const char* stage = streamInstallGetStageString(ctx->install.stream_ctx);
            InstallProgressEvent installProgressEvt(obj_filename, "", percent, total_written, data_size, stage);
            EventBus::getInstance().post(installProgressEvt);
        }

        TransferProgressEvent progressEvent(obj_filename, total_written, data_size, percent, speed);
        EventBus::getInstance().post(progressEvent);
    }

    u32 saved_handle = g_pending_object_handle;
    bool was_install = installIsVirtualStorage(g_pending_storage_id);
    bool was_saves = savesIsVirtualStorage(g_pending_storage_id);
    bool was_cancelled = cancel_requested;

    g_pending_object_handle = 0;
    g_pending_object_size = 0;
    g_pending_storage_id = 0;
    g_pending_filename[0] = '\0';

    bool transfer_success = (total_written > 0 || data_size == 0);

    if (was_install) {
        if (total_written > 0 && !was_cancelled) {
            installFinalizeObject(&ctx->install, saved_handle);
        } else {
            installDeleteObject(&ctx->install, saved_handle);
            transfer_success = false;
        }
    } else if (was_saves) {
        if (total_written > 0 && !was_cancelled) {
            savesCommitObject(&ctx->saves, saved_handle);
        } else {
            savesDeleteObject(&ctx->saves, saved_handle);
            transfer_success = false;
        }
    } else {
        if (total_written == data_size && !was_cancelled) {
            mtpStorageUpdateObjectSize(&ctx->storage, saved_handle, total_written);
        } else {
            mtpStorageDeleteObject(&ctx->storage, saved_handle);
            transfer_success = false;
        }
    }

    // Post install-specific complete event
    if (was_install) {
        InstallCompleteEvent installCompleteEvt(obj_filename, transfer_success,
                                               transfer_success ? "" : (was_cancelled ? "Installation cancelled" : "Installation failed"));
        EventBus::getInstance().post(installCompleteEvt);
    }

    TransferCompleteEvent completeEvent(obj_filename, total_written, transfer_success,
                                       transfer_success ? "" : (was_cancelled ? "Transfer cancelled" : "Transfer failed"));
    EventBus::getInstance().post(completeEvent);

    delete upload_events[saved_handle];
    upload_events.erase(saved_handle);
    upload_start_times.erase(saved_handle);
    transfer_cancel_erase(std::string(obj_filename));

    if (was_cancelled) {
        send_response(ctx, MTP_RESP_TRANSACTION_CANCELLED, transaction_id, NULL, 0);
    } else if (total_written == data_size) {
        send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
    } else {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
    }
}

static void handle_delete_object(MtpProtocolContext* ctx, u32 transaction_id, u32 handle) {
    if (!ctx->session_open) {
        send_response(ctx, MTP_RESP_SESSION_NOT_OPEN, transaction_id, NULL, 0);
        return;
    }

    bool success = false;
    u32 handle_base = handle & MTP_HANDLE_MASK;
    if (dumpIsVirtualHandle(handle) || gcIsVirtualHandle(handle) || handle_base == MTP_HANDLE_ALBUM_BASE) {
        send_response(ctx, MTP_RESP_OBJECT_WRITE_PROTECTED, transaction_id, NULL, 0);
        return;
    } else if (installIsVirtualHandle(handle)) {
        success = installDeleteObject(&ctx->install, handle);
    } else if (savesIsVirtualHandle(handle)) {
        success = savesDeleteObject(&ctx->saves, handle);
    } else {
        success = mtpStorageDeleteObject(&ctx->storage, handle);
    }

    if (success) {
        send_response(ctx, MTP_RESP_OK, transaction_id, NULL, 0);
    } else {
        send_response(ctx, MTP_RESP_GENERAL_ERROR, transaction_id, NULL, 0);
    }
}

Result mtpProtocolInit(MtpProtocolContext* ctx) {
    memset(ctx, 0, sizeof(MtpProtocolContext));

    // Larger buffers reduce per-chunk overhead on big file transfers.
    // Size is user-configurable; clamp to the supported range.
    size_t buffer_size = mtpProtocolGetConfiguredBufferSize();
    if (buffer_size < MTP_BUFFER_MIN) buffer_size = MTP_BUFFER_MIN;
    if (buffer_size > MTP_BUFFER_MAX) buffer_size = MTP_BUFFER_MAX;
    LOG_INFO("MTP: Initializing with buffer size: %zu KB", buffer_size / 1024);

    ctx->rx_buffer = (u8*)memalign(0x1000, buffer_size);
    ctx->tx_buffer = (u8*)memalign(0x1000, buffer_size);
    ctx->alt_buffer = (u8*)memalign(0x1000, buffer_size);

    if (!ctx->rx_buffer || !ctx->tx_buffer || !ctx->alt_buffer) {
        mtpProtocolExit(ctx);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    ctx->buffer_size = buffer_size;

    Result rc = mtpStorageInit(&ctx->storage);
    if (R_FAILED(rc)) {
        mtpProtocolExit(ctx);
        return rc;
    }

    installInit(&ctx->install);
    savesInit(&ctx->saves);
    dumpInit(&ctx->dump);
    gcInit(&ctx->gamecard);

    return 0;
}

void mtpProtocolExit(MtpProtocolContext* ctx) {
    gcExit(&ctx->gamecard);
    dumpExit(&ctx->dump);
    savesExit(&ctx->saves);
    installExit(&ctx->install);
    mtpStorageExit(&ctx->storage);

    if (ctx->rx_buffer) {
        free(ctx->rx_buffer);
        ctx->rx_buffer = NULL;
    }
    if (ctx->tx_buffer) {
        free(ctx->tx_buffer);
        ctx->tx_buffer = NULL;
    }
    if (ctx->alt_buffer) {
        free(ctx->alt_buffer);
        ctx->alt_buffer = NULL;
    }

    memset(ctx, 0, sizeof(MtpProtocolContext));
}

static bool s_logged_usb_ready = false;
static u64 s_usb_check_count = 0;

bool mtpProtocolProcess(MtpProtocolContext* ctx) {
    if (!usbMtpIsReady()) {
        s_usb_check_count++;
        if (s_usb_check_count % 1000 == 1) {
#if DEBUG_MTP_PROTO
            LOG_DEBUG("Waiting for USB host to configure device...");
#endif
        }
        s_logged_usb_ready = false;
        return true;
    }

    if (!s_logged_usb_ready) {
        LOG_INFO("USB ready! Waiting for MTP commands from host...");
        s_logged_usb_ready = true;
        s_usb_check_count = 0;
    }

    size_t read_bytes = usbMtpRead(ctx->rx_buffer, ctx->buffer_size, 10000000ULL);

    if (read_bytes == 0) {
        return true;
    }

    if (read_bytes < sizeof(MtpContainerHeader)) {
        return true;
    }

    MtpContainerHeader* hdr = (MtpContainerHeader*)ctx->rx_buffer;

    if (hdr->length < sizeof(MtpContainerHeader) || hdr->length > ctx->buffer_size) {
        return true;
    }

    if (read_bytes < hdr->length) {
        u32 remaining = hdr->length - read_bytes;
        size_t additional = usbMtpRead(ctx->rx_buffer + read_bytes, remaining, MTP_TIMEOUT_NS);
        if (additional < remaining) {
            return true;
        }
    }

    if (hdr->type == MTP_CONTAINER_TYPE_COMMAND) {
        u32* params = (u32*)(ctx->rx_buffer + sizeof(MtpContainerHeader));
        u32 payload_size = hdr->length - sizeof(MtpContainerHeader);

        LOG_DEBUG("MTP Command: 0x%04X, txn=%u, payload=%u bytes",
                 hdr->code, hdr->transaction_id, payload_size);

        switch (hdr->code) {
            case MTP_OP_GET_DEVICE_INFO:
                handle_get_device_info(ctx, hdr->transaction_id);
                break;

            case MTP_OP_OPEN_SESSION:
                handle_open_session(ctx, hdr->transaction_id, payload_size >= 4 ? params[0] : 0);
                break;

            case MTP_OP_CLOSE_SESSION:
                handle_close_session(ctx, hdr->transaction_id);
                break;

            case MTP_OP_GET_STORAGE_IDS:
                handle_get_storage_ids(ctx, hdr->transaction_id);
                break;

            case MTP_OP_GET_STORAGE_INFO:
                handle_get_storage_info(ctx, hdr->transaction_id, payload_size >= 4 ? params[0] : 0);
                break;

            case MTP_OP_GET_NUM_OBJECTS: {
                u32 storage_id = (payload_size >= 4) ? params[0] : 0xFFFFFFFF;
                u32 format = (payload_size >= 8) ? params[1] : 0;
                u32 parent = (payload_size >= 12) ? params[2] : 0;
                handle_get_num_objects(ctx, hdr->transaction_id, storage_id, format, parent);
                break;
            }

            case MTP_OP_GET_OBJECT_HANDLES: {
                u32 storage_id = (payload_size >= 4) ? params[0] : 0xFFFFFFFF;
                u32 format = (payload_size >= 8) ? params[1] : 0;
                u32 parent = (payload_size >= 12) ? params[2] : 0;
                handle_get_object_handles(ctx, hdr->transaction_id, storage_id, format, parent);
                break;
            }

            case MTP_OP_GET_OBJECT_INFO:
                handle_get_object_info(ctx, hdr->transaction_id, payload_size >= 4 ? params[0] : 0);
                break;

            case MTP_OP_GET_OBJECT:
                handle_get_object(ctx, hdr->transaction_id, payload_size >= 4 ? params[0] : 0);
                break;

            case MTP_OP_SEND_OBJECT_INFO: {
                u32 storage_id = (payload_size >= 4) ? params[0] : MTP_STORAGE_SDCARD;
                u32 parent = (payload_size >= 8) ? params[1] : 0xFFFFFFFF;
                handle_send_object_info(ctx, hdr->transaction_id, storage_id, parent);
                break;
            }

            case MTP_OP_SEND_OBJECT:
                handle_send_object(ctx, hdr->transaction_id);
                break;

            case MTP_OP_DELETE_OBJECT:
                handle_delete_object(ctx, hdr->transaction_id, payload_size >= 4 ? params[0] : 0);
                break;

            default:
                send_response(ctx, MTP_RESP_OPERATION_NOT_SUPPORTED, hdr->transaction_id, NULL, 0);
                break;
        }
    }

    return true;
}

size_t mtpProtocolGetConfiguredBufferSize(void) {
    const Settings* settings = settingsGet();
    if (settings) {
        return settings->mtp_buffer_size;
    }
    return MTP_BUFFER_SIZE;
}
