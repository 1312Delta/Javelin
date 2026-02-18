// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <switch.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_BUFFER_SIZE (2 * 1024 * 1024)
#define USB_BUFFER_ALIGN 0x1000
#define USB_MAX_RETRIES 3
#define USB_RETRY_DELAY_MS 5

Result usbMtpInitialize(void);
void usbMtpExit(void);
bool usbMtpIsReady(void);
size_t usbMtpRead(void* buffer, size_t size, u64 timeout_ns);
size_t usbMtpWrite(const void* buffer, size_t size, u64 timeout_ns);

// Zero-copy variants: buffer MUST be memalign(0x1000, ...) and size <= USB_BUFFER_SIZE.
// Returns bytes from a single USB transaction; does not loop for short transfers.
size_t usbMtpReadDirect(void* aligned_buffer, size_t size, u64 timeout_ns);
size_t usbMtpWriteDirect(const void* aligned_buffer, size_t size, u64 timeout_ns);

/**
 * Async split write: post DMA and return immediately.
 * Buffer MUST remain valid until usbMtpWriteDirectFinish() completes.
 * Returns true if DMA was posted successfully.
 */
bool usbMtpWriteDirectStart(const void* aligned_buffer, size_t size);

/**
 * Async split write: wait for in-flight DMA to complete.
 * Returns bytes transferred, or 0 on timeout/error.
 */
size_t usbMtpWriteDirectFinish(u64 timeout_ns);

/**
 * Async split read: post DMA and return immediately.
 * Buffer MUST remain valid until usbMtpReadDirectFinish() completes.
 * Returns true if DMA was posted successfully.
 */
bool usbMtpReadDirectStart(void* aligned_buffer, size_t size);

/**
 * Async split read: wait for in-flight DMA to complete.
 * Returns bytes transferred, or 0 on timeout/error.
 */
size_t usbMtpReadDirectFinish(u64 timeout_ns);

u32 usbMtpGetMaxPacketSize(void);
void usbMtpResetEndpoints(void);
void usbMtpClearStall(void);

#ifdef __cplusplus
}
#endif
