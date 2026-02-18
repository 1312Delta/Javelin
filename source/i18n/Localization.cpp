// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "i18n/Localization.h"
#include "i18n/EmbeddedTranslations.h"
#include "switch.h"
#include "core/Debug.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace Javelin {

void Localization::initialize() {
    currentLanguage = "en";

    const auto& embeddedLangs = getEmbeddedLanguages();
    for (const auto& lang : embeddedLangs) {
        if (lang.code && lang.keys && lang.values) {
            std::unordered_map<std::string, std::string> langStrings;
            for (size_t i = 0; lang.keys[i] != nullptr && lang.values[i] != nullptr; i++) {
                langStrings[lang.keys[i]] = lang.values[i];
            }
            allStrings[lang.code] = langStrings;
        }
    }

    DBG_PRINT("Loaded %zu embedded languages", allStrings.size());

    const char* langCodes[] = {
        "af", "ar", "ca", "cs", "da", "de", "el", "en", "es", "fi", "fr",
        "he", "hu", "it", "ja", "ko", "nl", "no", "pl", "pt", "ro", "ru",
        "sr", "sv", "tr", "uk", "vi", "zh"
    };

    for (size_t i = 0; i < sizeof(langCodes) / sizeof(langCodes[0]); i++) {
        loadFromRomfs(langCodes[i]);
    }

    auto it = allStrings.find("en");
    if (it != allStrings.end()) {
        strings = it->second;
    }

    DBG_PRINT("Initialized with %zu languages (including RomFS overrides)", allStrings.size());
}

void Localization::loadLanguage(const char* langCode) {
    if (loadFromRomfs(langCode)) {
        currentLanguage = langCode;
    }
}

void Localization::setLanguage(const char* langCode) {
    std::string codeStr(langCode);
    auto it = allStrings.find(codeStr);
    if (it != allStrings.end()) {
        currentLanguage = codeStr;
        strings = it->second;
        DBG_PRINT("Language changed to: %s (loaded %zu strings)", langCode, strings.size());
    } else {
        DBG_PRINT("Language not found: %s (available: %zu)", langCode, allStrings.size());
    }
}

const char* Localization::getLanguageName(const char* code) const {
    const auto& embeddedLangs = getEmbeddedLanguages();
    for (const auto& lang : embeddedLangs) {
        if (lang.code && strcmp(lang.code, code) == 0) {
            return lang.name;
        }
    }
    return "English";
}

const char* Localization::tr(const char* key) const {
    auto it = strings.find(key);
    if (it != strings.end()) {
        return it->second.c_str();
    }

    auto enTranslations = getEmbeddedTranslations("en");
    auto enIt = enTranslations.find(key);
    if (enIt != enTranslations.end()) {
        return enIt->second.c_str();
    }

    return key;
}

const char* Localization::tr(const char* key, const char* fallback) const {
    auto it = strings.find(key);
    if (it != strings.end()) {
        return it->second.c_str();
    }
    return fallback;
}

std::vector<Language> Localization::getAvailableLanguages() const {
    std::vector<Language> languages;

    const auto& embeddedLangs = getEmbeddedLanguages();
    for (const auto& lang : embeddedLangs) {
        if (lang.code && lang.name) {
            languages.push_back({lang.code, lang.name});
        }
    }

    return languages;
}

bool Localization::hasLanguage(const char* langCode) const {
    return allStrings.find(langCode) != allStrings.end();
}

static char* findJsonValue(const char* json, const char* key, char* buffer, size_t bufferSize) {
    char searchKey[256];
    snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);

    const char* keyPos = strstr(json, searchKey);
    if (!keyPos) return nullptr;

    const char* colon = strchr(keyPos, ':');
    if (!colon) return nullptr;

    const char* valueStart = colon + 1;
    while (*valueStart == ' ' || *valueStart == '\t' || *valueStart == '\n') valueStart++;

    if (*valueStart != '"') return nullptr;
    valueStart++;

    const char* valueEnd = strchr(valueStart, '"');
    if (!valueEnd) return nullptr;

    size_t length = valueEnd - valueStart;
    if (length >= bufferSize) length = bufferSize - 1;

    memcpy(buffer, valueStart, length);
    buffer[length] = '\0';

    return buffer;
}

bool Localization::loadFromRomfs(const char* langCode) {
    char path[128];
    snprintf(path, sizeof(path), "javelin/i18n/%s.json", langCode);

    FILE* f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "i18n/%s.json", langCode);
        f = fopen(path, "r");
    }

    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return false;
    }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    std::unordered_map<std::string, std::string> langStrings;
    auto embeddedFallback = getEmbeddedTranslations(langCode);
    if (embeddedFallback.empty()) {
        embeddedFallback = getEmbeddedTranslations("en");
    }
    langStrings = embeddedFallback;

    char valueBuffer[512];
    int translatedCount = 0;

    auto enTranslations = getEmbeddedTranslations("en");
    for (const auto& entry : enTranslations) {
        char* value = findJsonValue(buffer, entry.first.c_str(), valueBuffer, sizeof(valueBuffer));
        if (value && strlen(value) > 0) {
            langStrings[entry.first] = value;
            translatedCount++;
        }
    }

    free(buffer);
    allStrings[langCode] = langStrings;

    if (translatedCount > 0) {
        DBG_PRINT("Loaded %s from RomFS: %d translations", langCode, translatedCount);
    }

    return true;
}

}

