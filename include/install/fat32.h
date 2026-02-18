// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sanitize a filename for FAT32 compatibility.
 * Replaces illegal characters (\ / : * ? " < > |) with underscores.
 * Removes control characters and trims trailing spaces/dots.
 *
 * @param filename In-place buffer containing the filename to sanitize
 * @param max_len Maximum length of the buffer
 */
void fat32SanitizeFilename(char* filename, size_t max_len);

/**
 * Convert UTF-16LE data to UTF-8.
 *
 * @param utf16_data Pointer to UTF-16LE encoded data
 * @param utf16_chars Number of UTF-16 code units (not bytes)
 * @param out Output buffer for UTF-8 string
 * @param out_size Size of output buffer
 * @return Number of bytes written (excluding null terminator)
 */
size_t fat32Utf16leToUtf8(const uint8_t* utf16_data, size_t utf16_chars,
                           char* out, size_t out_size);

/**
 * Convert UTF-16LE filename to sanitized UTF-8 FAT32-compatible filename.
 * Combines UTF-16LE to UTF-8 conversion with FAT32 sanitization.
 *
 * @param utf16_data Pointer to UTF-16LE encoded filename
 * @param utf16_chars Number of UTF-16 code units
 * @param out Output buffer for sanitized UTF-8 filename
 * @param out_size Size of output buffer
 * @return Number of bytes written (excluding null terminator)
 */
size_t fat32SanitizeFromUtf16le(const uint8_t* utf16_data, size_t utf16_chars,
                                  char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
