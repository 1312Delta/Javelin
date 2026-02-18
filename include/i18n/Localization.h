// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Javelin {

struct Language {
    const char* code;
    const char* name;
};

class Localization {
public:
    static Localization& getInstance() {
        static Localization instance;
        return instance;
    }

    void initialize();
    void loadLanguage(const char* langCode);
    void setLanguage(const char* langCode);
    const char* getLanguage() const { return currentLanguage.c_str(); }
    const char* getLanguageName(const char* code) const;

    const char* tr(const char* key) const;
    const char* tr(const char* key, const char* fallback) const;

    std::vector<Language> getAvailableLanguages() const;
    bool hasLanguage(const char* langCode) const;

private:

    std::string currentLanguage;
    std::unordered_map<std::string, std::string> strings;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> allStrings;

    bool loadFromRomfs(const char* langCode);
};

inline const char* tr(const char* key) {
    return Localization::getInstance().tr(key);
}

inline const char* tr(const char* key, const char* fallback) {
    return Localization::getInstance().tr(key, fallback);
}

}

#define TR(key) Javelin::tr(key)
