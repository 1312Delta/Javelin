// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

// Get console device ID
Result getConsoleDeviceId(u64* out_device_id);

// Check if ticket is personalized and mismatches console
// Returns: 0 = common/matches console, 1 = personalized and mismatches
int checkTicketMismatch(const u8* ticket_data, u32 ticket_size,
                       u8* out_rights_id, u64* out_device_id, u32* out_account_id);

// Convert personalized ticket to common
void convertTicketToCommon(u8* ticket_data, u32 ticket_size);

#ifdef __cplusplus
}
#endif
