// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "mtp/usb_mtp.h"
#include "mtp/mtp_log.h"
#include "core/Debug.h"
#include <string.h>
#include <malloc.h>
#include <stdio.h>

// Uses libnx usbDs wrappers rather than raw IPC so firmware differences are handled transparently
#define USB_CLASS_IMAGE 0x06
#define USB_SUBCLASS_MTP 0x01
#define USB_PROTOCOL_MTP 0x01

#if DEBUG
#define USB_DEBUG 1
#else
#define USB_DEBUG 0
#endif

static bool g_initialized = false;
static bool g_shutting_down = false;
static UsbDsInterface* g_interface = NULL;
static UsbDsEndpoint* g_epIn = NULL;
static UsbDsEndpoint* g_epOut = NULL;
static UsbDsEndpoint* g_epInterrupt = NULL;

static u8* g_bufferIn = NULL;
static u8* g_bufferOut = NULL;

static u32 g_maxPacketSize = 512;  // Default to High Speed; updated on first ready
static bool g_ever_ready = false;
static bool g_speed_detected = false;

static u32 g_inflight_write_urb = 0;
static u32 g_inflight_write_size = 0;
static bool g_inflight_write_active = false;

static u32 g_inflight_read_urb = 0;
static u32 g_inflight_read_size = 0;
static bool g_inflight_read_active = false;

Result usbMtpInitialize(void) {
    if (g_initialized) {
        return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }

#if USB_DEBUG
    DBG_PRINT("Initializing USB MTP...");
#endif

    Result rc = 0;

    rc = usbDsInitialize();
    if (R_FAILED(rc)) return rc;

    if (hosversionAtLeast(5, 0, 0)) {
        u8 iManufacturer, iProduct, iSerialNumber;
        static const u16 supported_langs[1] = {0x0409}; // English

        rc = usbDsAddUsbLanguageStringDescriptor(NULL, supported_langs,
                                                  sizeof(supported_langs) / sizeof(u16));
        if (R_FAILED(rc)) goto cleanup;

        rc = usbDsAddUsbStringDescriptor(&iManufacturer, "Nintendo");
        if (R_FAILED(rc)) goto cleanup;

        rc = usbDsAddUsbStringDescriptor(&iProduct, "Switch MTP");
        if (R_FAILED(rc)) goto cleanup;

        rc = usbDsAddUsbStringDescriptor(&iSerialNumber, "SerialNumber");
        if (R_FAILED(rc)) goto cleanup;

        // "MSFT100" triggers Microsoft OS descriptor negotiation, enabling MTP on Windows
        u8 iMsOsDescriptor;
        rc = usbDsAddUsbStringDescriptor(&iMsOsDescriptor, "MSFT100");
        if (R_FAILED(rc)) goto cleanup;

        struct usb_device_descriptor device_descriptor = {
            .bLength = USB_DT_DEVICE_SIZE,
            .bDescriptorType = USB_DT_DEVICE,
            .bcdUSB = 0x0110,
            .bDeviceClass = 0x00,
            .bDeviceSubClass = 0x00,
            .bDeviceProtocol = 0x00,
            .bMaxPacketSize0 = 0x40,
            .idVendor = 0x057e,  // Nintendo
            .idProduct = 0x201d, // MTP mode (same as DBI)
            .bcdDevice = 0x0100,
            .iManufacturer = iManufacturer,
            .iProduct = iProduct,
            .iSerialNumber = iSerialNumber,
            .bNumConfigurations = 0x01
        };

        rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device_descriptor);
        if (R_FAILED(rc)) goto cleanup;

        device_descriptor.bcdUSB = 0x0200;
        rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);
        if (R_FAILED(rc)) goto cleanup;

        device_descriptor.bcdUSB = 0x0300;
        device_descriptor.bMaxPacketSize0 = 0x09;
        rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);
        if (R_FAILED(rc)) goto cleanup;

        u8 bos[0x16] = {
            0x05, // .bLength
            USB_DT_BOS, // .bDescriptorType
            0x16, 0x00, // .wTotalLength
            0x02, // .bNumDeviceCaps

            // USB 2.0 Extension
            0x07, // .bLength
            USB_DT_DEVICE_CAPABILITY, // .bDescriptorType
            0x02, // .bDevCapabilityType
            0x02, 0x00, 0x00, 0x00,

            // USB 3.0 SuperSpeed
            0x0A, // .bLength
            USB_DT_DEVICE_CAPABILITY, // .bDescriptorType
            0x03, // .bDevCapabilityType
            0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00
        };
        rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
        if (R_FAILED(rc)) goto cleanup;
    }

    rc = usbDsRegisterInterface(&g_interface);
    if (R_FAILED(rc)) goto cleanup;

    {
        u8 iInterface;
        rc = usbDsAddUsbStringDescriptor(&iInterface, "MTP");
        if (R_FAILED(rc)) goto cleanup;

        struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = g_interface->interface_index,
        .bAlternateSetting = 0,
        .bNumEndpoints = 3,  // Bulk IN, Bulk OUT, Interrupt IN
        .bInterfaceClass = USB_CLASS_IMAGE,
        .bInterfaceSubClass = USB_SUBCLASS_MTP,
        .bInterfaceProtocol = USB_PROTOCOL_MTP,
        .iInterface = iInterface,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
        .bInterval = 0,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
        .bInterval = 0,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_interrupt = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = 0x18,  // 24 bytes (same as DBI)
        .bInterval = 6,
    };

    struct usb_ss_endpoint_companion_descriptor endpoint_companion = {
        .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0x0F,
        .bmAttributes = 0x00,
        .wBytesPerInterval = 0x00,
    };

    endpoint_descriptor_in.bEndpointAddress += g_interface->interface_index + 1;
    endpoint_descriptor_out.bEndpointAddress += g_interface->interface_index + 1;
    endpoint_descriptor_interrupt.bEndpointAddress += g_interface->interface_index + 2;  // EP2 IN

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Full,
                                                 &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Full,
                                                 &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Full,
                                                 &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Full,
                                                 &endpoint_descriptor_interrupt, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    endpoint_descriptor_in.wMaxPacketSize = 0x200;  // High Speed: 512 byte packets
    endpoint_descriptor_out.wMaxPacketSize = 0x200;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High,
                                                 &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High,
                                                 &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High,
                                                 &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High,
                                                 &endpoint_descriptor_interrupt, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    endpoint_descriptor_in.wMaxPacketSize = 0x400;  // Super Speed: 1024 byte packets
    endpoint_descriptor_out.wMaxPacketSize = 0x400;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &endpoint_descriptor_interrupt, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super,
                                                 &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_RegisterEndpoint(g_interface, &g_epIn,
                                         endpoint_descriptor_in.bEndpointAddress);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_RegisterEndpoint(g_interface, &g_epOut,
                                         endpoint_descriptor_out.bEndpointAddress);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_RegisterEndpoint(g_interface, &g_epInterrupt,
                                         endpoint_descriptor_interrupt.bEndpointAddress);
    if (R_FAILED(rc)) goto cleanup;

    rc = usbDsInterface_EnableInterface(g_interface);
    if (R_FAILED(rc)) goto cleanup;

    if (hosversionAtLeast(5, 0, 0)) {
        rc = usbDsEnable();
        if (R_FAILED(rc)) goto cleanup;
    }

    g_bufferIn = (u8*)memalign(USB_BUFFER_ALIGN, USB_BUFFER_SIZE);
    g_bufferOut = (u8*)memalign(USB_BUFFER_ALIGN, USB_BUFFER_SIZE);

    if (!g_bufferIn || !g_bufferOut) {
        rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        goto cleanup;
    }

    // Only zero the first page — zeroing all of USB_BUFFER_SIZE would be wasteful
    memset(g_bufferIn, 0, USB_BUFFER_ALIGN);
    memset(g_bufferOut, 0, USB_BUFFER_ALIGN);

    g_maxPacketSize = 512;
    g_initialized = true;
    g_shutting_down = false;
#if USB_DEBUG
    DBG_PRINT("USB MTP initialized (buffers: %u KB)", USB_BUFFER_SIZE / 1024);
#endif
    return 0;
    }

cleanup:
    usbMtpExit();
    return rc;
}

void usbMtpExit(void) {
    if (!g_initialized) return;

#if USB_DEBUG
    DBG_PRINT("Shutting down USB MTP");
#endif
    g_ever_ready = false;
    g_shutting_down = true;

    usbMtpResetEndpoints();
    svcSleepThread(50000000ULL); // 50ms — let pending transfers drain

    if (g_bufferIn) {
        free(g_bufferIn);
        g_bufferIn = NULL;
    }
    if (g_bufferOut) {
        free(g_bufferOut);
        g_bufferOut = NULL;
    }

    g_epIn = NULL;
    g_epOut = NULL;
    g_epInterrupt = NULL;
    g_interface = NULL;

    // usbDsExit handles all internal cleanup
    if (g_initialized) {
        usbDsExit();
        g_initialized = false;
#if USB_DEBUG
        DBG_PRINT("USB MTP shut down complete");
#endif
    }
}

bool usbMtpIsReady(void) {
    if (!g_initialized) return false;

    Result rc = usbDsWaitReady(0);
    bool ready = R_SUCCEEDED(rc);

    if (ready && !g_ever_ready) {
        g_ever_ready = true;
#if USB_DEBUG
        DBG_PRINT("Device is now ready - host configured the device");
#endif
        if (!g_speed_detected) {
            UsbDeviceSpeed speed;
            rc = usbDsGetSpeed(&speed);
            if (R_SUCCEEDED(rc)) {
                g_speed_detected = true;
                switch (speed) {
                    case UsbDeviceSpeed_Super:
                        g_maxPacketSize = 1024;
                        LOG_INFO("USB 3.0 SuperSpeed detected (1024 byte packets)");
                        break;
                    case UsbDeviceSpeed_High:
                        g_maxPacketSize = 512;
                        LOG_INFO("USB 2.0 High Speed detected (512 byte packets)");
                        break;
                    case UsbDeviceSpeed_Full:
                        g_maxPacketSize = 64;
                        LOG_INFO("USB 1.1 Full Speed detected (64 byte packets)");
                        break;
                    default:
                        g_maxPacketSize = 512;
                        LOG_INFO("Unknown USB speed, defaulting to 512 byte packets");
                        break;
                }
            }
        }
    }

    return ready;
}

size_t usbMtpRead(void* buffer, size_t size, u64 timeout_ns) {
    if (!g_initialized || g_shutting_down || !buffer || size == 0) {
#if USB_DEBUG
        DBG_PRINT("Read: invalid params (init=%d, buf=%p, size=%zu)",
               g_initialized, buffer, size);
#endif
        return 0;
    }

    Result rc = 0;
    u8* bufptr = (u8*)buffer;
    size_t total_transferred = 0;

    rc = usbDsWaitReady(timeout_ns);
    if (R_FAILED(rc)) {
        return 0;
    }

    while (size > 0) {
        u32 urbId = 0;
        u32 tmp_transferred = 0;
        UsbDsReportData reportdata;

        u32 chunksize = (size > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)size;

        int retries = 0;
        bool success = false;

        while (retries < USB_MAX_RETRIES && !success) {
            if (retries == 0) {
                eventClear(&g_epOut->CompletionEvent);
            }

            // OUT endpoint = host->device from device perspective
            rc = usbDsEndpoint_PostBufferAsync(g_epOut, g_bufferOut, chunksize, &urbId);
            if (R_FAILED(rc)) {
                retries++;
                if (retries < USB_MAX_RETRIES) {
                    svcSleepThread((u64)USB_RETRY_DELAY_MS * 1000000ULL);
                    usbMtpClearStall();
                    eventClear(&g_epOut->CompletionEvent);
                    continue;
                }
#if USB_DEBUG
                DBG_PRINT("Read: PostBufferAsync failed after retries: 0x%08X", rc);
#endif
                return total_transferred;
            }

            rc = eventWait(&g_epOut->CompletionEvent, timeout_ns);
            if (R_FAILED(rc)) {
                usbDsEndpoint_Cancel(g_epOut);
                eventWait(&g_epOut->CompletionEvent, 500000000ULL);
                eventClear(&g_epOut->CompletionEvent);
                return total_transferred;
            }

            success = true;
        }

        if (!success) {
            return total_transferred;
        }

        rc = usbDsEndpoint_GetReportData(g_epOut, &reportdata);
        if (R_FAILED(rc)) {
#if USB_DEBUG
            DBG_PRINT("Read: GetReportData failed: 0x%08X", rc);
#endif
            return total_transferred;
        }

        bool found = false;
        for (u32 i = 0; i < reportdata.report_count; i++) {
            if (reportdata.report[i].id == urbId) {
                tmp_transferred = reportdata.report[i].transferredSize;
                found = true;
                break;
            }
        }

        if (!found) {
            rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferred);
            if (R_FAILED(rc)) {
#if USB_DEBUG
                DBG_PRINT("Read: URB %u not found (count=%u)", urbId, reportdata.report_count);
#endif
                if (total_transferred > 0) return total_transferred;
                return 0;
            }
        }

        if (tmp_transferred > chunksize) tmp_transferred = chunksize;

        if (tmp_transferred > 0) {
            memcpy(bufptr, g_bufferOut, tmp_transferred);
        }

        bufptr += tmp_transferred;
        size -= tmp_transferred;
        total_transferred += tmp_transferred;

        // Short transfer signals end of data from host
        if (tmp_transferred < chunksize) break;
    }

#if USB_DEBUG
    if (total_transferred > 0) {
        DBG_PRINT("Read: %zu bytes", total_transferred);
    }
#endif

    return total_transferred;
}

size_t usbMtpWrite(const void* buffer, size_t size, u64 timeout_ns) {
    if (!g_initialized || g_shutting_down || !buffer || size == 0) {
#if USB_DEBUG
        DBG_PRINT("Write: invalid params (init=%d, buf=%p, size=%zu)",
               g_initialized, buffer, size);
#endif
        return 0;
    }

    Result rc = 0;
    const u8* bufptr = (const u8*)buffer;
    size_t total_transferred = 0;

    rc = usbDsWaitReady(timeout_ns);
    if (R_FAILED(rc)) {
#if USB_DEBUG
        DBG_PRINT("Write: USB not ready (rc=0x%08X)", rc);
#endif
        return 0;
    }

    while (size > 0) {
        u32 urbId = 0;
        u32 tmp_transferred = 0;
        UsbDsReportData reportdata;

        u32 chunksize = (size > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)size;
        memcpy(g_bufferIn, bufptr, chunksize);

        int retries = 0;
        bool success = false;

        while (retries < USB_MAX_RETRIES && !success) {
            if (retries == 0) {
                eventClear(&g_epIn->CompletionEvent);
            }

            // IN endpoint = device->host from device perspective
            rc = usbDsEndpoint_PostBufferAsync(g_epIn, g_bufferIn, chunksize, &urbId);
            if (R_FAILED(rc)) {
                retries++;
                if (retries < USB_MAX_RETRIES) {
                    svcSleepThread((u64)USB_RETRY_DELAY_MS * 1000000ULL);
                    usbMtpClearStall();
                    eventClear(&g_epIn->CompletionEvent);
                    continue;
                }
#if USB_DEBUG
                DBG_PRINT("Write: PostBufferAsync failed after retries: 0x%08X", rc);
#endif
                return total_transferred;
            }

            rc = eventWait(&g_epIn->CompletionEvent, timeout_ns);
            if (R_FAILED(rc)) {
#if USB_DEBUG
                DBG_PRINT("Write: eventWait failed: 0x%08X", rc);
#endif
                usbDsEndpoint_Cancel(g_epIn);
                eventWait(&g_epIn->CompletionEvent, 500000000ULL);
                eventClear(&g_epIn->CompletionEvent);
                return total_transferred;
            }

            success = true;
        }

        if (!success) {
            return total_transferred;
        }

        rc = usbDsEndpoint_GetReportData(g_epIn, &reportdata);
        if (R_FAILED(rc)) {
#if USB_DEBUG
            DBG_PRINT("Write: GetReportData failed: 0x%08X", rc);
#endif
            return total_transferred;
        }

        bool found = false;
        for (u32 i = 0; i < reportdata.report_count; i++) {
            if (reportdata.report[i].id == urbId) {
                tmp_transferred = reportdata.report[i].transferredSize;
                found = true;
                break;
            }
        }

        if (!found) {
            rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferred);
            if (R_FAILED(rc)) {
#if USB_DEBUG
                DBG_PRINT("Write: URB %u not found (count=%u, rc=0x%08X)",
                       urbId, reportdata.report_count, rc);
#endif
                if (total_transferred > 0) return total_transferred;
                return 0;
            }
        }

        if (tmp_transferred > chunksize) tmp_transferred = chunksize;

        bufptr += tmp_transferred;
        size -= tmp_transferred;
        total_transferred += tmp_transferred;

        // Short transfer signals end of data from host
        if (tmp_transferred < chunksize) break;
    }

#if USB_DEBUG
    if (total_transferred > 0) {
        DBG_PRINT("Write: %zu bytes", total_transferred);
    }
#endif
    return total_transferred;
}

size_t usbMtpReadDirect(void* aligned_buffer, size_t size, u64 timeout_ns) {
    if (!g_initialized || g_shutting_down || !aligned_buffer || size == 0) {
        return 0;
    }

    Result rc = 0;
    u32 urbId = 0;
    u32 tmp_transferred = 0;
    UsbDsReportData reportdata;

    u32 chunksize = (size > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)size;

    eventClear(&g_epOut->CompletionEvent);

    rc = usbDsEndpoint_PostBufferAsync(g_epOut, aligned_buffer, chunksize, &urbId);
    if (R_FAILED(rc)) {
        return 0;
    }

    rc = eventWait(&g_epOut->CompletionEvent, timeout_ns);
    if (R_FAILED(rc)) {
        usbDsEndpoint_Cancel(g_epOut);
        eventWait(&g_epOut->CompletionEvent, 500000000ULL);
        eventClear(&g_epOut->CompletionEvent);
        return 0;
    }

    rc = usbDsEndpoint_GetReportData(g_epOut, &reportdata);
    if (R_FAILED(rc)) {
        return 0;
    }

    // Fast path: check reports directly
    for (u32 i = 0; i < reportdata.report_count; i++) {
        if (reportdata.report[i].id == urbId) {
            tmp_transferred = reportdata.report[i].transferredSize;
            if (tmp_transferred > chunksize) tmp_transferred = chunksize;
            return tmp_transferred;
        }
    }

    // Fallback
    rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferred);
    if (R_FAILED(rc)) {
        return 0;
    }
    if (tmp_transferred > chunksize) tmp_transferred = chunksize;
    return tmp_transferred;
}

size_t usbMtpWriteDirect(const void* aligned_buffer, size_t size, u64 timeout_ns) {
    if (!g_initialized || g_shutting_down || !aligned_buffer || size == 0) {
        return 0;
    }

    Result rc = 0;
    u32 urbId = 0;
    u32 tmp_transferred = 0;
    UsbDsReportData reportdata;

    u32 chunksize = (size > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)size;

    eventClear(&g_epIn->CompletionEvent);

    rc = usbDsEndpoint_PostBufferAsync(g_epIn, (void*)aligned_buffer, chunksize, &urbId);
    if (R_FAILED(rc)) {
        return 0;
    }

    rc = eventWait(&g_epIn->CompletionEvent, timeout_ns);
    if (R_FAILED(rc)) {
        usbDsEndpoint_Cancel(g_epIn);
        eventWait(&g_epIn->CompletionEvent, 500000000ULL);
        eventClear(&g_epIn->CompletionEvent);
        return 0;
    }

    rc = usbDsEndpoint_GetReportData(g_epIn, &reportdata);
    if (R_FAILED(rc)) {
        return 0;
    }

    // Fast path: check reports directly
    for (u32 i = 0; i < reportdata.report_count; i++) {
        if (reportdata.report[i].id == urbId) {
            tmp_transferred = reportdata.report[i].transferredSize;
            if (tmp_transferred > chunksize) tmp_transferred = chunksize;
            return tmp_transferred;
        }
    }

    // Fallback
    rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferred);
    if (R_FAILED(rc)) {
        return 0;
    }
    if (tmp_transferred > chunksize) tmp_transferred = chunksize;
    return tmp_transferred;
}

bool usbMtpWriteDirectStart(const void* aligned_buffer, size_t size) {
    if (!g_initialized || g_shutting_down || !aligned_buffer || size == 0) {
        return false;
    }

    u32 chunksize = (size > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)size;

    eventClear(&g_epIn->CompletionEvent);

    u32 urbId = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(g_epIn, (void*)aligned_buffer, chunksize, &urbId);
    if (R_FAILED(rc)) {
        return false;
    }

    g_inflight_write_urb = urbId;
    g_inflight_write_size = chunksize;
    g_inflight_write_active = true;
    return true;
}

size_t usbMtpWriteDirectFinish(u64 timeout_ns) {
    if (!g_inflight_write_active) {
        return 0;
    }

    g_inflight_write_active = false;

    Result rc = eventWait(&g_epIn->CompletionEvent, timeout_ns);
    if (R_FAILED(rc)) {
        usbDsEndpoint_Cancel(g_epIn);
        eventWait(&g_epIn->CompletionEvent, 500000000ULL);
        eventClear(&g_epIn->CompletionEvent);
        return 0;
    }

    UsbDsReportData reportdata;
    rc = usbDsEndpoint_GetReportData(g_epIn, &reportdata);
    if (R_FAILED(rc)) {
        return 0;
    }

    u32 tmp_transferred = 0;
    for (u32 i = 0; i < reportdata.report_count; i++) {
        if (reportdata.report[i].id == g_inflight_write_urb) {
            tmp_transferred = reportdata.report[i].transferredSize;
            if (tmp_transferred > g_inflight_write_size) tmp_transferred = g_inflight_write_size;
            return tmp_transferred;
        }
    }

    rc = usbDsParseReportData(&reportdata, g_inflight_write_urb, NULL, &tmp_transferred);
    if (R_FAILED(rc)) {
        return 0;
    }
    if (tmp_transferred > g_inflight_write_size) tmp_transferred = g_inflight_write_size;
    return tmp_transferred;
}

bool usbMtpReadDirectStart(void* aligned_buffer, size_t size) {
    if (!g_initialized || g_shutting_down || !aligned_buffer || size == 0) {
        return false;
    }

    u32 chunksize = (size > USB_BUFFER_SIZE) ? USB_BUFFER_SIZE : (u32)size;

    eventClear(&g_epOut->CompletionEvent);

    u32 urbId = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(g_epOut, aligned_buffer, chunksize, &urbId);
    if (R_FAILED(rc)) {
        return false;
    }

    g_inflight_read_urb = urbId;
    g_inflight_read_size = chunksize;
    g_inflight_read_active = true;
    return true;
}

size_t usbMtpReadDirectFinish(u64 timeout_ns) {
    if (!g_inflight_read_active) {
        return 0;
    }

    g_inflight_read_active = false;

    Result rc = eventWait(&g_epOut->CompletionEvent, timeout_ns);
    if (R_FAILED(rc)) {
        usbDsEndpoint_Cancel(g_epOut);
        eventWait(&g_epOut->CompletionEvent, 500000000ULL);
        eventClear(&g_epOut->CompletionEvent);
        return 0;
    }

    UsbDsReportData reportdata;
    rc = usbDsEndpoint_GetReportData(g_epOut, &reportdata);
    if (R_FAILED(rc)) {
        return 0;
    }

    u32 tmp_transferred = 0;
    for (u32 i = 0; i < reportdata.report_count; i++) {
        if (reportdata.report[i].id == g_inflight_read_urb) {
            tmp_transferred = reportdata.report[i].transferredSize;
            if (tmp_transferred > g_inflight_read_size) tmp_transferred = g_inflight_read_size;
            return tmp_transferred;
        }
    }

    rc = usbDsParseReportData(&reportdata, g_inflight_read_urb, NULL, &tmp_transferred);
    if (R_FAILED(rc)) {
        return 0;
    }
    if (tmp_transferred > g_inflight_read_size) tmp_transferred = g_inflight_read_size;
    return tmp_transferred;
}

u32 usbMtpGetMaxPacketSize(void) {
    return g_maxPacketSize;
}

void usbMtpResetEndpoints(void) {
    if (!g_initialized) return;

    g_inflight_write_active = false;
    g_inflight_read_active = false;

    if (g_epIn) {
        usbDsEndpoint_Cancel(g_epIn);
        eventClear(&g_epIn->CompletionEvent);
    }
    if (g_epOut) {
        usbDsEndpoint_Cancel(g_epOut);
        eventClear(&g_epOut->CompletionEvent);
    }
    if (g_epInterrupt) {
        usbDsEndpoint_Cancel(g_epInterrupt);
        eventClear(&g_epInterrupt->CompletionEvent);
    }

#if USB_DEBUG
    DBG_PRINT("Endpoints reset");
#endif
}

void usbMtpClearStall(void) {
    if (!g_initialized) return;

    if (g_epIn) {
        usbDsEndpoint_Stall(g_epIn);
        svcSleepThread(1000000ULL);  // 1ms delay
        // Note: libnx doesn't have a direct unstall, stall+wait usually clears it
    }
    if (g_epOut) {
        usbDsEndpoint_Stall(g_epOut);
        svcSleepThread(1000000ULL);  // 1ms delay
    }
}
