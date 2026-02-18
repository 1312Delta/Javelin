// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "core/Settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static Settings g_settings = {
    .language = "en",
    .mtp_buffer_size = MTP_BUFFER_DEFAULT,
};

// Simple JSON parser for our config format
static char* findJsonString(const char* json, const char* key, char* buffer, size_t bufferSize) {
    char searchKey[128];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);

    const char* keyPos = strstr(json, searchKey);
    if (!keyPos) return nullptr;

    const char* colon = strchr(keyPos, ':');
    if (!colon) return nullptr;

    const char* valueStart = colon + 1;
    while (*valueStart == ' ' || *valueStart == '\t' || *valueStart == '\n') valueStart++;

    if (*valueStart != '"') {
        // Not a string, try to parse as number
        if (*valueStart == '-' || (*valueStart >= '0' && *valueStart <= '9')) {
            const char* numEnd = valueStart;
            while (*numEnd == '-' || *numEnd == '.' || (*numEnd >= '0' && *numEnd <= '9')) numEnd++;
            size_t length = numEnd - valueStart;
            if (length >= bufferSize) length = bufferSize - 1;
            memcpy(buffer, valueStart, length);
            buffer[length] = '\0';
            return buffer;
        }
        return nullptr;
    }

    valueStart++;

    const char* valueEnd = strchr(valueStart, '"');
    if (!valueEnd) return nullptr;

    size_t length = valueEnd - valueStart;
    if (length >= bufferSize) length = bufferSize - 1;

    memcpy(buffer, valueStart, length);
    buffer[length] = '\0';

    return buffer;
}

void settingsInit(void) {
    // Ensure config directory exists
    mkdir("sdmc:/switch/Javelin", 0777);

    settingsLoad();
}

const Settings* settingsGet(void) {
    return &g_settings;
}

void settingsSetLanguage(const char* langCode) {
    if (langCode) {
        strncpy(g_settings.language, langCode, sizeof(g_settings.language) - 1);
        g_settings.language[sizeof(g_settings.language) - 1] = '\0';
    }
}

void settingsSetMtpBufferSize(u32 size) {
    if (size < MTP_BUFFER_MIN) size = MTP_BUFFER_MIN;
    if (size > MTP_BUFFER_MAX) size = MTP_BUFFER_MAX;
    g_settings.mtp_buffer_size = size;
}

bool settingsSave(void) {
    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) {
        return false;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"language\": \"%s\",\n", g_settings.language);
    fprintf(f, "  \"mtp_buffer_size\": %u\n", g_settings.mtp_buffer_size);
    fprintf(f, "}\n");

    fclose(f);
    return true;
}

bool settingsLoad(void) {
    FILE* f = fopen(SETTINGS_PATH, "r");
    if (!f) {
        // File doesn't exist, create with defaults
        settingsSave();
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size == 0 || size > 8192) {
        fclose(f);
        return false;
    }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return false;
    }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    char valueBuffer[64];

    // Parse language
    if (findJsonString(buffer, "language", valueBuffer, sizeof(valueBuffer))) {
        strncpy(g_settings.language, valueBuffer, sizeof(g_settings.language) - 1);
        g_settings.language[sizeof(g_settings.language) - 1] = '\0';
    }

    // Parse MTP buffer size
    if (findJsonString(buffer, "mtp_buffer_size", valueBuffer, sizeof(valueBuffer))) {
        u32 size = (u32)atoi(valueBuffer);
        settingsSetMtpBufferSize(size);
    }

    free(buffer);
    return true;
}
