// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

// Debug build configuration
// Set DEBUG=1 in Makefile or environment to enable debug output
// Production builds should have DEBUG=0 (or undefined)

#ifndef DEBUG
#define DEBUG 0
#endif

// Debug logging macros - only emit code when DEBUG is enabled
#if DEBUG
    #define DBG_PRINT(fmt, ...)     printf("[DBG] " fmt "\n", ##__VA_ARGS__)
    #define DBG_ERROR(fmt, ...)     fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#else
    #define DBG_PRINT(fmt, ...)     ((void)0)
    #define DBG_ERROR(fmt, ...)     ((void)0)
#endif

// For MTP logging - always include in UI-visible log regardless of debug build
// These go to the on-screen log viewer
#define MTP_LOG_ALWAYS 1

// nxlink/network debugging - only enabled in DEBUG builds
#if DEBUG
    #define NXLINK_ENABLED 1
#else
    #define NXLINK_ENABLED 0
#endif
