// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include "mtp_storage.h"
#include "mtp_install.h"
#include "mtp_saves.h"
#include "mtp_dump.h"
#include "mtp_gamecard.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_CONTAINER_TYPE_COMMAND          0x0001
#define MTP_CONTAINER_TYPE_DATA             0x0002
#define MTP_CONTAINER_TYPE_RESPONSE         0x0003
#define MTP_CONTAINER_TYPE_EVENT            0x0004

#define MTP_OP_GET_DEVICE_INFO              0x1001
#define MTP_OP_OPEN_SESSION                 0x1002
#define MTP_OP_CLOSE_SESSION                0x1003
#define MTP_OP_GET_STORAGE_IDS              0x1004
#define MTP_OP_GET_STORAGE_INFO             0x1005
#define MTP_OP_GET_NUM_OBJECTS              0x1006
#define MTP_OP_GET_OBJECT_HANDLES           0x1007
#define MTP_OP_GET_OBJECT_INFO              0x1008
#define MTP_OP_GET_OBJECT                   0x1009
#define MTP_OP_GET_PARTIAL_OBJECT           0x101B
#define MTP_OP_SEND_OBJECT_INFO             0x100C
#define MTP_OP_SEND_OBJECT                  0x100D
#define MTP_OP_DELETE_OBJECT                0x100B

#define MTP_RESP_OK                         0x2001
#define MTP_RESP_GENERAL_ERROR              0x2002
#define MTP_RESP_SESSION_NOT_OPEN           0x2003
#define MTP_RESP_INVALID_TRANSACTION_ID     0x2004
#define MTP_RESP_OPERATION_NOT_SUPPORTED    0x2005
#define MTP_RESP_PARAMETER_NOT_SUPPORTED    0x2006
#define MTP_RESP_INVALID_STORAGE_ID         0x2008
#define MTP_RESP_INVALID_OBJECT_HANDLE      0x2009
#define MTP_RESP_INVALID_PARAMETER          0x201D
#define MTP_RESP_SESSION_ALREADY_OPEN       0x201E
#define MTP_RESP_SPECIFICATION_BY_FORMAT_UNSUPPORTED 0x2014
#define MTP_RESP_STORE_FULL                 0x200C
#define MTP_RESP_STORE_READ_ONLY            0x200E
#define MTP_RESP_OBJECT_WRITE_PROTECTED     0x200F
#define MTP_RESP_TRANSACTION_CANCELLED      0x201F

typedef struct {
    u32 length;
    u16 type;
    u16 code;
    u32 transaction_id;
} __attribute__((packed)) MtpContainerHeader;

#define MTP_BUFFER_SIZE     (4 * 1024 * 1024)  // Default, can be overridden via settings
#define MTP_FILE_BUFFER     (1024 * 1024)

// Get the configured MTP buffer size from settings
size_t mtpProtocolGetConfiguredBufferSize(void);

typedef struct {
    bool session_open;
    u32 session_id;
    u32 transaction_id;

    u8* rx_buffer;
    u8* tx_buffer;
    u8* alt_buffer;
    size_t buffer_size;

    MtpStorageContext storage;
    InstallContext install;
    SavesContext saves;
    DumpContext dump;
    GcContext gamecard;
} MtpProtocolContext;

Result mtpProtocolInit(MtpProtocolContext* ctx);
void mtpProtocolExit(MtpProtocolContext* ctx);
bool mtpProtocolProcess(MtpProtocolContext* ctx);

#ifdef __cplusplus
}
#endif
