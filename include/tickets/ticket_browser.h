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
    char issuer[0x40];
    u8 titlekey_block[0x100];
    u8 format_version;
    u8 titlekey_type;        // 0=Common, 1=Personalized
    u16 ticket_version;
    u8 license_type;         // 0-5: Permanent/Demo/Trial/Rental/Subscription/Service
    u8 key_generation;
    u16 property_mask;       // bit0=PreInstall, 1=Shared, 2=AllContents, 3=DeviceLinkIndep, 4=Volatile, 5=ELicenseRequired
    u8 reserved[0x8];
    u64 ticket_id;
    u64 device_id;
    u8 rights_id[0x10];
    u32 account_id;
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
