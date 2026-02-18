// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
// Extended ES service functions
// This header adds the new functions to the ipcext/es.h namespace
//

#ifndef JAVELIN2_ES_H
#define JAVELIN2_ES_H

#ifdef __cplusplus
extern "C" {
#endif

// These functions are defined in source/service/es.cpp
// They extend the ES service with additional commands
#include <switch.h>
#include <ipcext/es.h>

Result esGetTitleKey(const EsRightsId *rights_id, u32 key_generation, void *outBuf, size_t bufSize);
Result esGetCommonTicketData(u64 *out_size, const EsRightsId *rights_id, void *outBuf, size_t bufSize);

#ifdef __cplusplus
}
#endif

#endif //JAVELIN2_ES_H