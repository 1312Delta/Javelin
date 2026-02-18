// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
/*
 * Generate embedded translations C++ source from JSON files.
 * Scans romfs/javelin/i18n/ for *.json files and creates embedded translation arrays.
 * en.json is the source of truth - all other languages fall back to English for missing keys.
 *
 * Compile with: g++ -std=c++17 -O2 tools/gen_translations.cpp -o build/gen_translations
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <filesystem>

#ifdef _WIN32
    #include <direct.h>
    #define mkdir(path, mode) _mkdir(path)
    #define PATH_SEPARATOR "\\"
#else
    #define PATH_SEPARATOR "/"
#endif

namespace fs = std::filesystem;

const char* ROMFS_DIR = "romfs/javelin/i18n";
const char* OUTPUT_FILE_DEST = "source/i18n/embedded_translations.cpp";
const char* HEADER_FILE_DEST = "include/i18n/EmbeddedTranslations.h";

// Display names for known language codes (cosmetic only, for the settings UI)
static const std::map<std::string, std::string> LANGUAGE_NAMES = {
    {"af", "Afrikaans"},
    {"ar", "Arabic"},
    {"ca", "Catalan"},
    {"cs", "Czech"},
    {"da", "Danish"},
    {"de", "German"},
    {"el", "Greek"},
    {"en", "English"},
    {"es", "Spanish"},
    {"fi", "Finnish"},
    {"fr", "French"},
    {"he", "Hebrew"},
    {"hu", "Hungarian"},
    {"it", "Italian"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"nl", "Dutch"},
    {"no", "Norwegian"},
    {"pl", "Polish"},
    {"pt", "Portuguese"},
    {"pt-BR", "Portuguese, Brazilian"},
    {"ro", "Romanian"},
    {"ru", "Russian"},
    {"sr", "Serbian"},
    {"sv", "Swedish"},
    {"tr", "Turkish"},
    {"uk", "Ukrainian"},
    {"vi", "Vietnamese"},
    {"zh", "Chinese"},
    {"zh-CN", "Chinese Simplified"},
    {"zh-TW", "Chinese Traditional"},
};

std::string getLanguageName(const std::string& code) {
    auto it = LANGUAGE_NAMES.find(code);
    if (it != LANGUAGE_NAMES.end()) return it->second;
    return code;
}

std::string escapeCString(const std::string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '\"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

bool parseJsonTranslations(const std::string& jsonContent, std::map<std::string, std::string>& outTranslations) {
    outTranslations.clear();

    size_t pos = 0;
    while (pos < jsonContent.size() && (jsonContent[pos] == ' ' || jsonContent[pos] == '\t' ||
           jsonContent[pos] == '\n' || jsonContent[pos] == '\r' || jsonContent[pos] == '{')) {
        pos++;
    }

    while (pos < jsonContent.size()) {
        while (pos < jsonContent.size() && (jsonContent[pos] == ' ' || jsonContent[pos] == '\t' ||
               jsonContent[pos] == '\n' || jsonContent[pos] == '\r' || jsonContent[pos] == ',' || jsonContent[pos] == '}')) {
            pos++;
        }
        if (pos >= jsonContent.size()) break;

        if (jsonContent[pos] != '"') break;
        pos++;
        size_t keyStart = pos;

        while (pos < jsonContent.size() && jsonContent[pos] != '"') {
            if (jsonContent[pos] == '\\') {
                pos++;
                if (pos < jsonContent.size()) pos++;
            } else {
                pos++;
            }
        }
        if (pos >= jsonContent.size()) break;
        std::string key = jsonContent.substr(keyStart, pos - keyStart);
        pos++;

        while (pos < jsonContent.size() && (jsonContent[pos] == ' ' || jsonContent[pos] == '\t' ||
               jsonContent[pos] == '\n' || jsonContent[pos] == '\r')) {
            pos++;
        }
        if (pos >= jsonContent.size() || jsonContent[pos] != ':') break;
        pos++;

        while (pos < jsonContent.size() && (jsonContent[pos] == ' ' || jsonContent[pos] == '\t' ||
               jsonContent[pos] == '\n' || jsonContent[pos] == '\r')) {
            pos++;
        }
        if (pos >= jsonContent.size() || jsonContent[pos] != '"') break;
        pos++;
        size_t valueStart = pos;

        while (pos < jsonContent.size() && jsonContent[pos] != '"') {
            if (jsonContent[pos] == '\\') {
                pos++;
                if (pos < jsonContent.size()) pos++;
            } else {
                pos++;
            }
        }
        if (pos >= jsonContent.size()) break;
        std::string value = jsonContent.substr(valueStart, pos - valueStart);
        pos++;

        outTranslations[key] = value;
    }

    return !outTranslations.empty();
}

bool readFile(const std::string& path, std::string& outContent) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    outContent.resize(size);
    file.read(&outContent[0], size);
    return file.good();
}

bool writeFile(const std::string& path, const std::string& content) {
    size_t lastSlash = path.find_last_of(PATH_SEPARATOR);
    if (lastSlash != std::string::npos) {
        std::string dir = path.substr(0, lastSlash);
        std::string cmd;
        #ifdef _WIN32
            cmd = "if not exist \"" + dir + "\" mkdir \"" + dir + "\"";
        #else
            cmd = "mkdir -p \"" + dir + "\"";
        #endif
        system(cmd.c_str());
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file << content;
    return file.good();
}

std::string codeToIdentifier(const std::string& code) {
    std::string result = code;
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '-') {
            result[i] = '_';
        }
    }
    return result;
}

// Discover all .json files in the i18n directory and return sorted language codes
std::vector<std::string> discoverLanguages(const std::string& romfsDir) {
    std::vector<std::string> codes;

    for (const auto& entry : fs::directory_iterator(romfsDir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (filename.size() < 6) continue; // minimum: "x.json"
        if (filename.substr(filename.size() - 5) != ".json") continue;

        std::string code = filename.substr(0, filename.size() - 5);
        codes.push_back(code);
    }

    // Sort with "en" always first, rest alphabetical
    std::sort(codes.begin(), codes.end(), [](const std::string& a, const std::string& b) {
        if (a == "en") return true;
        if (b == "en") return false;
        return a < b;
    });

    return codes;
}

struct LanguageData {
    std::string code;
    std::string name;
    std::map<std::string, std::string> translations;
};

std::string generateHeader() {
    std::stringstream ss;
    ss << "// Auto-generated by tools/gen_translations.cpp\n";
    ss << "// DO NOT EDIT - Generated from " << ROMFS_DIR << "/*.json\n\n";
    ss << "#pragma once\n\n";
    ss << "#include <vector>\n";
    ss << "#include <unordered_map>\n";
    ss << "#include <string>\n\n";
    ss << "namespace Javelin {\n\n";
    ss << "struct EmbeddedLanguage {\n";
    ss << "    const char* code;\n";
    ss << "    const char* name;\n";
    ss << "    const char* const* keys;\n";
    ss << "    const char* const* values;\n";
    ss << "};\n\n";
    ss << "const std::vector<EmbeddedLanguage>& getEmbeddedLanguages();\n\n";
    ss << "std::unordered_map<std::string, std::string> getEmbeddedTranslations(const char* langCode);\n\n";
    ss << "} // namespace Javelin\n";
    return ss.str();
}

std::string generateSource(const std::vector<LanguageData>& languages) {
    std::stringstream ss;

    ss << "// Auto-generated by tools/gen_translations.cpp\n";
    ss << "// DO NOT EDIT - Generated from " << ROMFS_DIR << "/*.json\n\n";
    ss << "#include \"i18n/EmbeddedTranslations.h\"\n";
    ss << "#include <string>\n";
    ss << "#include <vector>\n";
    ss << "#include <cstring>\n\n";
    ss << "namespace Javelin {\n\n";

    for (const auto& lang : languages) {
        std::string codeId = codeToIdentifier(lang.code);

        ss << "// " << lang.name << " (" << lang.code << ") - " << lang.translations.size() << " strings\n";
        ss << "static const char* const " << codeId << "_keys[] = {\n";
        for (const auto& entry : lang.translations) {
            ss << "    \"" << escapeCString(entry.first) << "\",\n";
        }
        ss << "    nullptr\n";
        ss << "};\n\n";

        ss << "static const char* const " << codeId << "_values[] = {\n";
        for (const auto& entry : lang.translations) {
            ss << "    \"" << escapeCString(entry.second) << "\",\n";
        }
        ss << "    nullptr\n";
        ss << "};\n\n";
    }

    ss << "const std::vector<EmbeddedLanguage>& getEmbeddedLanguages() {\n";
    ss << "    static std::vector<EmbeddedLanguage> langs;\n";
    ss << "    if (langs.empty()) {\n";
    for (const auto& lang : languages) {
        std::string codeId = codeToIdentifier(lang.code);
        ss << "        langs.push_back(EmbeddedLanguage{\"" << lang.code
           << "\", \"" << escapeCString(lang.name) << "\", " << codeId
           << "_keys, " << codeId << "_values});\n";
    }
    ss << "    }\n";
    ss << "    return langs;\n";
    ss << "}\n\n";

    ss << "std::unordered_map<std::string, std::string> getEmbeddedTranslations(const char* langCode) {\n";
    ss << "    std::unordered_map<std::string, std::string> result;\n\n";

    for (const auto& lang : languages) {
        ss << "    if (strcmp(langCode, \"" << lang.code << "\") == 0) {\n";
        for (const auto& entry : lang.translations) {
            ss << "        result[\"" << escapeCString(entry.first) << "\"] = \""
               << escapeCString(entry.second) << "\";\n";
        }
        ss << "        return result;\n";
        ss << "    }\n\n";
    }

    ss << "    return result;\n";
    ss << "}\n\n";
    ss << "} // namespace Javelin\n";

    return ss.str();
}

int main(int argc, char* argv[]) {
    std::cout << "[gen_translations] Generating embedded translations..." << std::endl;

    const char* baseDir = (argc > 1) ? argv[1] : ".";

    std::string romfsDir = std::string(baseDir) + "/" + ROMFS_DIR;
    std::string outputFile = std::string(baseDir) + "/" + OUTPUT_FILE_DEST;
    std::string headerFile = std::string(baseDir) + "/" + HEADER_FILE_DEST;

    if (!fs::exists(romfsDir)) {
        std::cerr << "[gen_translations] Error: " << romfsDir << " not found" << std::endl;
        return 1;
    }

    // Discover languages from actual JSON files on disk
    std::vector<std::string> langCodes = discoverLanguages(romfsDir);

    if (langCodes.empty() || langCodes[0] != "en") {
        std::cerr << "[gen_translations] Error: en.json is required" << std::endl;
        return 1;
    }

    std::cout << "[gen_translations] Found " << langCodes.size() << " language files: ";
    for (size_t i = 0; i < langCodes.size(); i++) {
        if (i > 0) std::cout << ", ";
        std::cout << langCodes[i];
    }
    std::cout << std::endl;

    // Load English first as the base
    std::map<std::string, std::string> enTranslations;
    {
        std::string enPath = romfsDir + "/en.json";
        std::string enContent;
        if (!readFile(enPath, enContent) || !parseJsonTranslations(enContent, enTranslations)) {
            std::cerr << "[gen_translations] Error: Failed to load en.json" << std::endl;
            return 1;
        }
        std::cout << "[gen_translations] Loaded en: " << enTranslations.size() << " strings (source of truth)" << std::endl;
    }

    // Build language data for all discovered languages
    std::vector<LanguageData> languages;

    for (const auto& code : langCodes) {
        LanguageData lang;
        lang.code = code;
        lang.name = getLanguageName(code);

        if (code == "en") {
            lang.translations = enTranslations;
        } else {
            std::string jsonPath = romfsDir + "/" + code + ".json";
            std::string jsonContent;

            if (readFile(jsonPath, jsonContent)) {
                std::map<std::string, std::string> trans;
                if (parseJsonTranslations(jsonContent, trans)) {
                    // Fill in missing keys from English
                    for (const auto& entry : enTranslations) {
                        if (trans.find(entry.first) == trans.end()) {
                            trans[entry.first] = entry.second;
                        }
                    }
                    lang.translations = trans;
                    std::cout << "[gen_translations] Loaded " << code << ": "
                              << trans.size() << " strings" << std::endl;
                } else {
                    std::cout << "[gen_translations] Warning: Failed to parse " << code
                              << ".json, using English" << std::endl;
                    lang.translations = enTranslations;
                }
            }
        }

        languages.push_back(lang);
    }

    std::string headerContent = generateHeader();
    if (writeFile(headerFile, headerContent)) {
        std::cout << "[gen_translations] Generated " << headerFile << std::endl;
    } else {
        std::cerr << "[gen_translations] Error: Failed to write " << headerFile << std::endl;
        return 1;
    }

    std::string sourceContent = generateSource(languages);
    if (writeFile(outputFile, sourceContent)) {
        std::cout << "[gen_translations] Generated " << outputFile << std::endl;
    } else {
        std::cerr << "[gen_translations] Error: Failed to write " << outputFile << std::endl;
        return 1;
    }

    std::cout << "[gen_translations] Done! Embedded " << languages.size() << " languages" << std::endl;
    return 0;
}
