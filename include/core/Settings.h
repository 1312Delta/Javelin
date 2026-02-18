// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_PATH "sdmc:/switch/Javelin/config.json"
#define SETTINGS_MAX_PATH 256

// MTP buffer size limits (in bytes)
#define MTP_BUFFER_MIN (256 * 1024)      // 256 KB
#define MTP_BUFFER_MAX (16 * 1024 * 1024) // 16 MB
#define MTP_BUFFER_DEFAULT (16 * 1024 * 1024) // 16 MB - increased for better large file throughput

typedef struct {
    char language[8];      // Language code (e.g., "en", "es")
    u32 mtp_buffer_size;   // MTP transfer buffer size in bytes
} Settings;

/**
 * Initialize the settings system and load settings from disk.
 * Creates default settings if file doesn't exist.
 */
void settingsInit(void);

/**
 * Get the current settings (read-only, don't modify directly).
 */
const Settings* settingsGet(void);

/**
 * Set the language preference.
 * @param langCode Language code (e.g., "en", "es")
 */
void settingsSetLanguage(const char* langCode);

/**
 * Set the MTP buffer size.
 * @param size Buffer size in bytes (clamped to MIN/MAX)
 */
void settingsSetMtpBufferSize(u32 size);

/**
 * Save current settings to disk.
 * @return true if successful, false otherwise
 */
bool settingsSave(void);

/**
 * Reload settings from disk.
 * @return true if successful, false otherwise
 */
bool settingsLoad(void);

#ifdef __cplusplus
}
#endif
