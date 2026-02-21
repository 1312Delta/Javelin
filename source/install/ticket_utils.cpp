// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "install/ticket_utils.h"
#include "mtp_log.h"
#include <switch.h>
#include <string.h>
#include <cstdio>

// Get console device ID
Result getConsoleDeviceId(u64* out_device_id) {
    if (!out_device_id) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = setcalInitialize();
    if (R_FAILED(rc)) {
        LOG_WARN("Ticket Utils: Failed to initialize setcal: 0x%08X", rc);
        return rc;
    }

    rc = setcalGetDeviceId(out_device_id);
    setcalExit();

    return rc;
}

// Check if ticket is personalized and mismatches console
// Returns: 0 = common/matches console, 1 = personalized and mismatches
int checkTicketMismatch(const u8* ticket_data, u32 ticket_size,
                       u8* out_rights_id, u64* out_device_id, u32* out_account_id) {
    if (!ticket_data || ticket_size < 0x2C0) {
        return 0;  // Not enough data for a ticket
    }

    // Ticket structure (simplified):
    // 0x000: Signature (variable, RSA-2048 = 0x100 bytes after type)
    // After signature: TikCommonBlock

    // Skip signature type (4 bytes) and signature data
    // Most tickets use RSA-2048 (0x10000) = 4 + 0x100 + 0x3C padding
    u32 signature_type = *(u32*)ticket_data;
    const u8* common_block = ticket_data;

    LOG_INFO("Ticket Utils: Checking ticket - signature_type: 0x%08X, size: %u", signature_type, ticket_size);

    // Debug: Print first 100 bytes of ticket
    LOG_INFO("Ticket Utils: Ticket header (first 100 bytes):");
    for (int i = 0; i < 100 && i < ticket_size; i += 16) {
        char hex[128];
        int pos = 0;
        for (int j = 0; j < 16 && i + j < ticket_size && i + j < 100; j++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", ticket_data[i + j]);
        }
        LOG_INFO("  [%04X] %s", i, hex);
    }

    // Signature types are stored in big-endian format
    size_t signature_size = 0;
    if (signature_type == 0x00010000) {  // RSA-2048 SHA256
        signature_size = 4 + 0x100 + 0x3C;
        LOG_INFO("Ticket Utils: Detected RSA-2048 signature");
    } else if (signature_type == 0x00010001) {  // RSA-4096 SHA256
        signature_size = 4 + 0x200 + 0x3C;
        LOG_INFO("Ticket Utils: Detected RSA-4096 signature");
    } else if (signature_type == 0x00010003) {  // ECDSA SHA256
        signature_size = 4 + 0x3C;
        LOG_INFO("Ticket Utils: Detected ECDSA signature (0x00010003)");
    } else if (signature_type == 0x00010004) {  // ECDSA SHA256 (variant)
        // ECDSA signature: 4 bytes type + 0x3C bytes signature
        signature_size = 4 + 0x3C;
        LOG_INFO("Ticket Utils: Detected ECDSA signature (0x00010004)");
    } else {
        LOG_WARN("Ticket Utils: Unknown signature type: 0x%08X", signature_type);
        return 0;
    }

    common_block += signature_size;
    LOG_INFO("Ticket Utils: Signature size: 0x%zX, common_block offset: 0x%zX", signature_size, common_block - ticket_data);

    // Check if we have enough data for the fields we need to access
    // We need to access up to device_id at offset 0x268 within common block
    // So we need: signature_size + 0x268 + 8 (for device_id)
    size_t min_required = signature_size + 0x268 + 8;
    if (ticket_size < min_required) {
        LOG_WARN("Ticket Utils: Not enough data for ticket fields - need 0x%zX bytes, have %u bytes",
                min_required, ticket_size);
        return 0;
    }

    // TikCommonBlock starts here
    // +0x40: issuer
    // +0x140: titlekey_block
    // +0x240: format_version
    // +0x241: titlekey_type (0=Common, 1=Personalized)
    u8 titlekey_type = common_block[0x241];
    LOG_INFO("Ticket Utils: titlekey_type = %u (0=Common, 1=Personalized) at offset 0x%zX",
            titlekey_type, common_block - ticket_data + 0x241);

    // Debug: Print the titlekey area
    LOG_INFO("Ticket Utils: Titlekey area (offset 0x%zX):", common_block - ticket_data + 0x240);
    for (int i = 0; i < 16 && (common_block - ticket_data) + 0x240 + i < ticket_size; i++) {
        char hex[8];
        snprintf(hex, sizeof(hex), "%02X ", common_block[0x240 + i]);
        LOG_INFO("  [0x%02X] %s", 0x240 + i, hex);
    }

    // Additional bounds check before accessing personalized fields
    if ((common_block - ticket_data) + 0x270 > ticket_size) {
        LOG_WARN("Ticket Utils: Would read past end of ticket - aborting check");
        return 0;
    }

    if (titlekey_type == 1) {
        // Personalized ticket - extract rights ID, device ID, account ID
        // +0x250: rights_id[16]
        // +0x260: account_id
        // +0x268: device_id
        u64 ticket_device_id = 0;
        u32 ticket_account_id = 0;

        memcpy(&ticket_device_id, common_block + 0x268, 8);
        memcpy(&ticket_account_id, common_block + 0x260, 4);

        LOG_INFO("Ticket Utils: Personalized ticket - device_id: 0x%016lX, account_id: 0x%08X",
                ticket_device_id, ticket_account_id);

        // If device ID and account ID are both 0, ticket is personalized but not tied to a specific console
        // This is safe to install without warning
        if (ticket_device_id == 0 && ticket_account_id == 0) {
            LOG_INFO("Ticket Utils: Personalized ticket with no device/account restriction");
            return 0;  // No warning needed
        }

        // Get console's device ID to check for mismatch
        u64 console_device_id = 0;
        Result rc = getConsoleDeviceId(&console_device_id);

        if (R_SUCCEEDED(rc)) {
            // Check if ticket device ID matches console device ID
            if (ticket_device_id == console_device_id) {
                LOG_INFO("Ticket Utils: Personalized ticket matches console device ID");
                return 0;  // Matches - no warning needed
            }

            LOG_WARN("Ticket Utils: Personalized ticket device ID mismatch! Ticket: 0x%016lX, Console: 0x%016lX",
                    ticket_device_id, console_device_id);
        } else {
            LOG_WARN("Ticket Utils: Could not get console device ID (0x%08X), assuming mismatch", rc);
        }

        // Extract ticket info for warning modal
        if (out_rights_id) {
            memcpy(out_rights_id, common_block + 0x250, 16);
        }
        if (out_device_id) {
            *out_device_id = ticket_device_id;
        }
        if (out_account_id) {
            *out_account_id = ticket_account_id;
        }

        return 1;  // Personalized and mismatches - warn user
    }

    LOG_INFO("Ticket Utils: Common ticket detected");
    return 0;  // Common ticket
}

// Convert personalized ticket to common by changing titlekey_type
void convertTicketToCommon(u8* ticket_data, u32 ticket_size) {
    if (!ticket_data || ticket_size < 0x2C0) {
        return;
    }

    u32 signature_type = *(u32*)ticket_data;
    u8* common_block = ticket_data;

    // Signature types are stored in big-endian format
    size_t signature_size = 0;
    if (signature_type == 0x00010000) {  // RSA-2048 SHA256
        signature_size = 4 + 0x100 + 0x3C;
    } else if (signature_type == 0x00010001) {  // RSA-4096 SHA256
        signature_size = 4 + 0x200 + 0x3C;
    } else if (signature_type == 0x00010003) {  // ECDSA SHA256
        signature_size = 4 + 0x3C;
    } else if (signature_type == 0x00010004) {  // ECDSA SHA256 (variant)
        signature_size = 4 + 0x3C;
    } else {
        return;
    }

    common_block += signature_size;

    // Check if we have enough data for the fields we need to access
    size_t min_required = signature_size + 0x268 + 8;
    if (ticket_size < min_required) {
        return;
    }

    // Change titlekey_type from 1 (personalized) to 0 (common)
    common_block[0x241] = 0;

    LOG_INFO("Ticket Utils: Converted personalized ticket to common");
}
