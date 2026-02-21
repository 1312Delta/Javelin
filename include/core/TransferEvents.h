// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <string>
#include <cstdint>
#include <cstring>
#include "Event.h"

namespace Javelin {

struct TransferStartEvent : public Event {
    static constexpr EventTypeID StaticEventType = 1;

    enum Direction {
        Upload,
        Download
    };

    std::string filePath;
    uint64_t totalBytes;
    bool* cancelledPtr;  // Pointer to cancelled flag
    Direction direction;

    TransferStartEvent() = default;
    TransferStartEvent(const std::string& path, uint64_t bytes, Direction dir)
        : filePath(path), totalBytes(bytes), cancelledPtr(nullptr), direction(dir) {}

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "TransferStart"; }
};

struct TransferProgressEvent : public Event {
    static constexpr EventTypeID StaticEventType = 2;

    std::string filePath;
    uint64_t bytesTransferred;
    uint64_t totalBytes;
    float progressPercent;
    float speedMBps;
    bool isComplete;
    bool success;
    std::string errorMessage;

    TransferProgressEvent() = default;
    TransferProgressEvent(const std::string& path, uint64_t transferred, uint64_t total,
                         float percent, float speed)
        : filePath(path), bytesTransferred(transferred), totalBytes(total),
          progressPercent(percent), speedMBps(speed), isComplete(false),
          success(true), errorMessage() {}

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "TransferProgress"; }
};

struct TransferCompleteEvent : public Event {
    static constexpr EventTypeID StaticEventType = 3;

    std::string filePath;
    bool success;
    std::string errorMessage;
    uint64_t totalBytes;
    uint64_t durationMs;

    TransferCompleteEvent() = default;
    TransferCompleteEvent(const std::string& path, uint64_t bytes, bool ok, const std::string& error)
        : filePath(path), totalBytes(bytes), success(ok), errorMessage(error),
          durationMs(0) {}

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "TransferComplete"; }
};

struct InstallStartEvent : public Event {
    static constexpr EventTypeID StaticEventType = 4;

    std::string titleName;
    std::string filePath;

    InstallStartEvent() = default;
    InstallStartEvent(const std::string& title, const std::string& path)
        : titleName(title), filePath(path) {}

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "InstallStart"; }
};

struct InstallProgressEvent : public Event {
    static constexpr EventTypeID StaticEventType = 5;

    std::string titleName;
    std::string currentFile;
    float progressPercent;
    uint64_t bytesWritten;
    uint64_t totalBytes;
    std::string stage;

    InstallProgressEvent() = default;
    InstallProgressEvent(const std::string& title, const std::string& file,
                         float percent, uint64_t written, uint64_t total,
                         const std::string& installStage = "")
        : titleName(title), currentFile(file), progressPercent(percent),
          bytesWritten(written), totalBytes(total), stage(installStage) {}

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "InstallProgress"; }
};

struct InstallCompleteEvent : public Event {
    static constexpr EventTypeID StaticEventType = 6;

    std::string titleName;
    bool success;
    std::string errorMessage;

    InstallCompleteEvent() = default;
    InstallCompleteEvent(const std::string& title, bool ok, const std::string& error)
        : titleName(title), success(ok), errorMessage(error) {}

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "InstallComplete"; }
};

struct PersonalizedTicketEvent : public Event {
    static constexpr EventTypeID StaticEventType = 7;

    std::string titleName;
    std::string filePath;
    uint8_t rightsId[16];
    uint64_t deviceId;
    uint32_t accountId;
    void* streamContext;    // Pointer to StreamInstallContext

    PersonalizedTicketEvent() : streamContext(nullptr) {
        memset(rightsId, 0, sizeof(rightsId));
        deviceId = 0;
        accountId = 0;
    }

    PersonalizedTicketEvent(const std::string& title, const std::string& path,
                           const uint8_t* rights_id, uint64_t dev_id, uint32_t acc_id,
                           void* stream_ctx = nullptr)
        : titleName(title), filePath(path), deviceId(dev_id), accountId(acc_id),
          streamContext(stream_ctx) {
        if (rights_id) {
            memcpy(rightsId, rights_id, sizeof(rightsId));
        } else {
            memset(rightsId, 0, sizeof(rightsId));
        }
    }

    EventTypeID getEventType() const override { return StaticEventType; }
    const char* getEventName() const override { return "PersonalizedTicket"; }
};

} // namespace Javelin
