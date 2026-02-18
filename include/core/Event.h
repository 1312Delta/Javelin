// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>

namespace Javelin {

using EventTypeID = uint32_t;

class Event {
public:
    virtual ~Event() = default;

    virtual EventTypeID getEventType() const = 0;
    virtual const char* getEventName() const { return "Event"; }

    bool isCancelled() const { return cancelled; }
    void cancel() { cancelled = true; }

    bool cancelled = false;
};

template<typename T>
using EventListener = std::function<void(T&)>;

class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus instance;
        return instance;
    }

    template<typename T>
    uint64_t subscribe(EventListener<T> listener, int priority = 0) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint64_t id = nextListenerId_++;

        EventTypeID eventType = T::StaticEventType;
        auto& listeners = eventListeners_[eventType];

        listeners.push_back({
            id,
            priority,
            [listener](void* eventPtr) {
                T* event = static_cast<T*>(eventPtr);
                if (!event->isCancelled()) {
                    listener(*event);
                }
            }
        });

        std::sort(listeners.begin(), listeners.end(),
            [](const ListenerInfo& a, const ListenerInfo& b) {
                return a.priority > b.priority;
            });

        return id;
    }

    void unsubscribe(uint64_t handle) {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& pair : eventListeners_) {
            auto& listeners = pair.second;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [handle](const ListenerInfo& info) {
                        return info.id == handle;
                    }),
                listeners.end()
            );
        }
    }

    template<typename T>
    void unsubscribeAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        eventListeners_.erase(T::StaticEventType);
    }

    void unsubscribeAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        eventListeners_.clear();
    }

    template<typename T>
    bool post(T& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        EventTypeID eventType = event.getEventType();
        auto it = eventListeners_.find(eventType);

        if (it != eventListeners_.end()) {
            for (const auto& listenerInfo : it->second) {
                if (!event.isCancelled()) {
                    listenerInfo.callback(&event);
                }
                if (event.isCancelled()) break;
            }
        }

        return !event.isCancelled();
    }

    template<typename T>
    size_t getListenerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = eventListeners_.find(T::StaticEventType);
        return it != eventListeners_.end() ? it->second.size() : 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        eventListeners_.clear();
    }

private:
    EventBus() = default;
    ~EventBus() = default;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    struct ListenerInfo {
        uint64_t id;
        int priority;
        std::function<void(void*)> callback;
    };

    mutable std::mutex mutex_;
    std::unordered_map<EventTypeID, std::vector<ListenerInfo>> eventListeners_;
    uint64_t nextListenerId_ = 1;
};

} // namespace Javelin
