// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include "Event.h"
#include <string>

namespace Javelin {

enum EventTypes {
    EventType_Gui = 1000,
    EventType_ButtonClick,
    EventType_ScreenChange,
    EventType_Notification
};

class ScreenChangeEvent : public Event {
public:
    static constexpr EventTypeID StaticEventType = EventType_ScreenChange;

    ScreenChangeEvent(int screenId) : screenId(screenId) {}

    const char* getEventName() const override { return "ScreenChangeEvent"; }
    EventTypeID getEventType() const override { return StaticEventType; }

    int screenId;
};

class NotificationEvent : public Event {
public:
    static constexpr EventTypeID StaticEventType = EventType_Notification;

    enum class Type {
        Info,
        Success,
        Warning,
        Error
    };

    NotificationEvent(const std::string& message, Type type = Type::Info, int durationMs = 3000)
        : message(message), type(type), durationMs(durationMs) {}

    const char* getEventName() const override { return "NotificationEvent"; }
    EventTypeID getEventType() const override { return StaticEventType; }

    std::string message;
    Type type;
    int durationMs;
};

inline void showNotification(const std::string& message,
                            NotificationEvent::Type type = NotificationEvent::Type::Info) {
    NotificationEvent event(message, type);
    EventBus::getInstance().post(event);
}

inline void showInfo(const std::string& message) {
    showNotification(message, NotificationEvent::Type::Info);
}

inline void showSuccess(const std::string& message) {
    showNotification(message, NotificationEvent::Type::Success);
}

inline void showWarning(const std::string& message) {
    showNotification(message, NotificationEvent::Type::Warning);
}

inline void showError(const std::string& message) {
    showNotification(message, NotificationEvent::Type::Error);
}

} // namespace Javelin
