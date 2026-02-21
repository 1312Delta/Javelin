// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

// ============================================================================
// COMPONENT-SPECIFIC DEBUG FLAGS
// Set these to 1 to enable detailed logging for specific components
// This allows fine-grained debugging without enabling all debug output
// ============================================================================

// MTP Protocol debugging (shows ALL MTP commands/responses - VERY VERBOSE)
#define DEBUG_MTP_PROTO     0       // MTP protocol commands/responses

// MTP Storage debugging
#define DEBUG_MTP_STORAGE   0       // MTP storage operations

// USB debugging
#define DEBUG_USB           0       // USB layer operations

// Installation debugging
#define DEBUG_INSTALL       0       // Installation process (tickets, NCA installation, CNMT parsing, etc.)

// Save file debugging
#define DEBUG_SAVES         0       // Save file operations

// GUI debugging
#define DEBUG_GUI           0       // GUI events and rendering

// Memory debugging
#define DEBUG_MEMORY        0       // Memory allocations/deallocations

// ============================================================================
// LEGACY DEBUG MACROS
// ============================================================================

// Debug build configuration
// Set DEBUG=1 in Makefile or environment to enable debug output
// Production builds should have DEBUG undefined (not defined as 0)
// Note: We intentionally DON'T define DEBUG=0 here, to avoid preprocessor issues

// Debug logging macros - only emit code when DEBUG is defined and non-zero
#ifdef DEBUG
    #define DBG_PRINT_ENABLED 1
#else
    #define DBG_PRINT_ENABLED 0
#endif

#if DBG_PRINT_ENABLED
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
#if DBG_PRINT_ENABLED
    #define NXLINK_ENABLED 1
#else
    #define NXLINK_ENABLED 0
#endif
