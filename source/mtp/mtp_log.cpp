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

    // Filter debug logs based on component debug flags
    if (level == MTP_LOG_DEBUG) {
        // Check if this debug message should be shown based on component flags
        // For now, only show debug if DEBUG_INSTALL is enabled and message contains install-related keywords
        #if DEBUG_INSTALL
            // Allow install-related debug logs
            if (strstr(message, "Install") ||
                strstr(message, "install") ||
                strstr(message, "Ticket") ||
                strstr(message, "ticket") ||
                strstr(message, "NCA") ||
                strstr(message, "CNMT") ||
                strstr(message, "Stream")) {
                // Show this debug message
            } else {
                // Hide other debug messages
                return;
            }
        #else
            // Hide all debug logs if DEBUG_INSTALL is not enabled
            return;
        #endif
    }

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

    // Only print to nxlink if this message should be shown based on debug flags
#if NXLINK_ENABLED
    // For debug messages, only show if they match enabled component flags
    bool should_print = false;
    if (level != MTP_LOG_DEBUG) {
        // Always print INFO and ERROR messages
        should_print = true;
    } else {
        // Debug messages - check component flags
        // Only show debug logs if at least one debug flag is enabled
        #if DEBUG_INSTALL || DEBUG_MTP_PROTO || DEBUG_MTP_STORAGE || DEBUG_USB || DEBUG_SAVES || DEBUG_GUI || DEBUG_MEMORY
            #if DEBUG_INSTALL
                if (strstr(message, "Install") ||
                    strstr(message, "install") ||
                    strstr(message, "Ticket") ||
                    strstr(message, "ticket") ||
                    strstr(message, "NCA") ||
                    strstr(message, "CNMT") ||
                    strstr(message, "Stream")) {
                    should_print = true;
                }
            #endif
            #if DEBUG_MTP_PROTO
                if (strstr(message, "MTP Command") ||
                    strstr(message, "MTP:") ||
                    strstr(message, "Response") ||
                    strstr(message, "Data ") ||
                    strstr(message, "GetStorage")) {
                    should_print = true;
                }
            #endif
            #if DEBUG_MTP_STORAGE
                if (strstr(message, "Storage") ||
                    strstr(message, "storage")) {
                    should_print = true;
                }
            #endif
            #if DEBUG_USB
                if (strstr(message, "USB") ||
                    strstr(message, "Read:") ||
                    strstr(message, "Write:")) {
                    should_print = true;
                }
            #endif
            #if DEBUG_SAVES
                if (strstr(message, "Save") ||
                    strstr(message, "save")) {
                    should_print = true;
                }
            #endif
            #if DEBUG_GUI
                if (strstr(message, "GUI") ||
                    strstr(message, "Render")) {
                    should_print = true;
                }
            #endif
            #if DEBUG_MEMORY
                if (strstr(message, "Alloc") ||
                    strstr(message, "Free")) {
                    should_print = true;
                }
            #endif
        #endif
    }

    if (should_print) {
        printf("[MTP] %s\n", message);
    }
#endif
}
