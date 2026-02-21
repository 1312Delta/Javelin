// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include <vector>
#include <string>

extern "C" {
#include "ipcext/es.h"
#include "service/es.h"
}

namespace Javelin {

#define SIGNED_TIK_MAX_SIZE 0x400

struct TikCommonBlock {
    char issuer[0x40];              // 0x000
    u8 titlekey_block[0x100];       // 0x040
    u8 format_version;              // 0x140
    u8 titlekey_type;               // 0x141 (0=Common, 1=Personalized)
    u16 ticket_version;             // 0x142
    u8 license_type;                // 0x144 (0-5: Permanent/Demo/Trial/Rental/Subscription/Service)
    u8 key_generation;              // 0x145
    u16 property_mask;              // 0x146 (bit0=PreInstall, 1=Shared, 2=AllContents, 3=DeviceLinkIndep, 4=Volatile, 5=ELicenseRequired)
    u8 reserved[0x8];               // 0x148
    u64 ticket_id;                  // 0x150
    u64 device_id;                  // 0x158
    u8 rights_id[0x10];             // 0x160
    u32 account_id;                 // 0x170
    u32 sect_total_size;            // 0x174
    u32 sect_hdr_offset;            // 0x178
    u16 sect_hdr_count;             // 0x17C
    u16 sect_hdr_entry_size;        // 0x17E
} __attribute__((packed));

struct TicketDetail {
    bool loaded;
    bool is_common;
    u32 signature_type;
    TikCommonBlock block;
    u8 decrypted_titlekey[0x10];
    bool has_decrypted_titlekey;
};

struct TicketEntry {
    u64 titleId;
    u8 keyGeneration;
    char rightsIdStr[33];
    char titleName[512];
    bool isPersonalized;
    EsRightsId rightsId;
};

struct TicketBrowserState {
    std::vector<TicketEntry> tickets;
    bool initialized;
    bool loading;
    int selectedFilter;
    char searchBuf[128];
    int selectedTicket;
    bool showDetailPopup;
    TicketDetail detail;
};

void ticketBrowserInit(TicketBrowserState* state);
void ticketBrowserRefresh(TicketBrowserState* state);
void ticketBrowserExit(TicketBrowserState* state);

void renderTicketScreen(TicketBrowserState* state);

} // namespace Javelin
