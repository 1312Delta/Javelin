// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MTP_LOG_DEBUG = 0,
    MTP_LOG_INFO = 1,
    MTP_LOG_WARNING = 2,
    MTP_LOG_ERROR = 3,
} MtpLogLevel;

void mtpLogInit(void);
void mtpLogClear(void);
int mtpLogGetCount(void);
const char* mtpLogGetEntry(int index);
MtpLogLevel mtpLogGetLevel(int index);
void mtpLogAdd(MtpLogLevel level, const char* message);
#define LOG_ERROR(fmt, ...)   do { char buf[512]; snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); mtpLogAdd(MTP_LOG_ERROR, buf); } while(0)
#define LOG_WARN(fmt, ...)    do { char buf[512]; snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); mtpLogAdd(MTP_LOG_WARNING, buf); } while(0)
#define LOG_INFO(fmt, ...)    do { char buf[512]; snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); mtpLogAdd(MTP_LOG_INFO, buf); } while(0)
#define LOG_DEBUG(fmt, ...)   do { char buf[512]; snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); mtpLogAdd(MTP_LOG_DEBUG, buf); } while(0)

#ifdef __cplusplus
}
#endif
