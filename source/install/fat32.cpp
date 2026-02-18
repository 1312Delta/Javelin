// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//

#include "install/fat32.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// FAT32 illegal characters: \ / : * ? " < > |
// Also control characters (0x00-0x1F) are not allowed

void fat32SanitizeFilename(char* filename, size_t max_len) {
    if (!filename || max_len == 0) return;

    char* p = filename;
    char* write = filename;

    while (*p && (size_t)(write - filename) < max_len - 1) {
        unsigned char c = (unsigned char)*p;

        // Skip control characters
        if (c < 0x20) {
            p++;
            continue;
        }

        // Replace illegal characters with underscore
        switch (c) {
            case '\\':
            case '/':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                *write++ = '_';
                break;
            default:
                *write++ = c;
                break;
        }
        p++;
    }
    *write = '\0';

    // Trim trailing spaces and dots (FAT32 doesn't allow them at the end)
    size_t len = strlen(filename);
    while (len > 0 && (filename[len - 1] == ' ' || filename[len - 1] == '.')) {
        filename[--len] = '\0';
    }
}

// Convert UTF-16LE to UTF-8
// Returns number of bytes written (excluding null terminator)
size_t fat32Utf16leToUtf8(const uint8_t* utf16_data, size_t utf16_chars,
                           char* out, size_t out_size) {
    if (!utf16_data || !out || out_size == 0 || utf16_chars == 0) {
        if (out && out_size > 0) out[0] = '\0';
        return 0;
    }

    size_t out_pos = 0;
    for (size_t i = 0; i < utf16_chars && out_pos < out_size - 1; i++) {
        uint16_t c = utf16_data[i * 2] | ((uint16_t)utf16_data[i * 2 + 1] << 8);

        if (c == 0) break;  // Null terminator

        // Handle surrogate pairs (for characters outside BMP)
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < utf16_chars) {
            uint16_t c2 = utf16_data[(i + 1) * 2] | ((uint16_t)utf16_data[(i + 1) * 2 + 1] << 8);
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                // Valid surrogate pair
                uint32_t codepoint = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i++;  // Skip the low surrogate

                // Encode as 4-byte UTF-8
                if (out_pos + 4 <= out_size - 1) {
                    out[out_pos++] = (char)(0xF0 | ((codepoint >> 18) & 0x07));
                    out[out_pos++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                    out[out_pos++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    out[out_pos++] = (char)(0x80 | (codepoint & 0x3F));
                } else {
                    break;  // Not enough space
                }
                continue;
            }
        }

        // BMP character
        if (c < 0x80) {
            out[out_pos++] = (char)c;
        } else if (c < 0x800) {
            if (out_pos + 2 <= out_size - 1) {
                out[out_pos++] = (char)(0xC0 | ((c >> 6) & 0x1F));
                out[out_pos++] = (char)(0x80 | (c & 0x3F));
            } else {
                break;
            }
        } else {
            if (out_pos + 3 <= out_size - 1) {
                out[out_pos++] = (char)(0xE0 | ((c >> 12) & 0x0F));
                out[out_pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[out_pos++] = (char)(0x80 | (c & 0x3F));
            } else {
                break;
            }
        }
    }

    out[out_pos] = '\0';
    return out_pos;
}

// Full sanitization: UTF-16LE input to sanitized UTF-8 FAT32-compatible filename
size_t fat32SanitizeFromUtf16le(const uint8_t* utf16_data, size_t utf16_chars,
                                  char* out, size_t out_size) {
    size_t written = fat32Utf16leToUtf8(utf16_data, utf16_chars, out, out_size);
    if (written > 0) {
        fat32SanitizeFilename(out, out_size);
        return strlen(out);
    }
    return 0;
}
