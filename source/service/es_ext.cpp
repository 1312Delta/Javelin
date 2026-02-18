// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
// Extended ES service functions
// These functions extend libnx-es with additional commands
//

#include <switch.h>
#include <ipcext/es.h>

extern "C" {

Result esGetTitleKey(const EsRightsId *rights_id, u32 key_generation, void *outBuf, size_t bufSize) {
    Service esService;
    Result rc = smGetService(&esService, "es");
    if (R_FAILED(rc)) return rc;

    const struct {
        EsRightsId rights_id;
        u32 key_generation;
    } in = { *rights_id, key_generation };

    rc = serviceDispatchIn(&esService, 8, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { outBuf, bufSize } },
    );

    serviceClose(&esService);
    return rc;
}

Result esGetCommonTicketData(u64 *out_size, const EsRightsId *rights_id, void *outBuf, size_t bufSize) {
    Service esService;
    Result rc = smGetService(&esService, "es");
    if (R_FAILED(rc)) return rc;

    rc = serviceDispatchInOut(&esService, 16, *rights_id, *out_size,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { outBuf, bufSize } },
    );

    serviceClose(&esService);
    return rc;
}

} // extern "C"