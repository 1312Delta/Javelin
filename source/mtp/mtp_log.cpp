// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/mtp_log.h"
#include "core/Debug.h"
#include <stdio.h>
#include <string.h>

#define MAX_LOG_ENTRIES 100
#define MAX_LOG_LENGTH 512

typedef struct {
    MtpLogLevel level;
    char message[MAX_LOG_LENGTH];
} LogEntry;

static LogEntry g_log_entries[MAX_LOG_ENTRIES];
static int g_log_count = 0;
static bool g_log_initialized = false;

void mtpLogInit(void) {
    if (g_log_initialized) return;

    memset(g_log_entries, 0, sizeof(g_log_entries));
    g_log_count = 0;
    g_log_initialized = true;
}

void mtpLogClear(void) {
    g_log_count = 0;
    memset(g_log_entries, 0, sizeof(g_log_entries));
}

int mtpLogGetCount(void) {
    return g_log_count;
}

const char* mtpLogGetEntry(int index) {
    if (index < 0 || index >= g_log_count) return "";
    return g_log_entries[index].message;
}

MtpLogLevel mtpLogGetLevel(int index) {
    if (index < 0 || index >= g_log_count) return MTP_LOG_INFO;
    return g_log_entries[index].level;
}

void mtpLogAdd(MtpLogLevel level, const char* message) {
    if (!g_log_initialized) return;

    if (g_log_count < MAX_LOG_ENTRIES) {
        g_log_entries[g_log_count].level = level;
        strncpy(g_log_entries[g_log_count].message, message, MAX_LOG_LENGTH - 1);
        g_log_entries[g_log_count].message[MAX_LOG_LENGTH - 1] = '\0';
        g_log_count++;
    } else {
        for (int i = 0; i < MAX_LOG_ENTRIES - 1; i++) {
            g_log_entries[i] = g_log_entries[i + 1];
        }
        g_log_entries[MAX_LOG_ENTRIES - 1].level = level;
        strncpy(g_log_entries[MAX_LOG_ENTRIES - 1].message, message, MAX_LOG_LENGTH - 1);
        g_log_entries[MAX_LOG_ENTRIES - 1].message[MAX_LOG_LENGTH - 1] = '\0';
    }

#if DEBUG
    printf("[MTP] %s\n", message);
#endif
}
