// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once

#include "Event.h"
#include "GuiEvents.h"
#include "TransferEvents.h"
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <memory>

struct ImVec4;

namespace Javelin {

enum ScreenId {
    Screen_MainMenu = 0,
    Screen_MTP = 1,
    Screen_Install = 2,
    Screen_Settings = 3,
    Screen_Tickets = 4,
    Screen_Dump = 5
};

struct Notification {
    std::string message;
    NotificationEvent::Type type;
    float endTime;
};

struct TransferProgress {
    std::string filePath;
    uint64_t bytesTransferred;
    uint64_t totalBytes;
    float progressPercent;
    float speedMBps;
    bool isComplete;
    bool success;
    std::string errorMessage;
    bool* cancelledPtr;  // Pointer to cancelled flag in the event
};

struct InstallProgress {
    std::string titleName;
    std::string filePath;
    float progressPercent;
    std::string currentFile;
    uint64_t bytesWritten;
    uint64_t totalBytes;
    std::string stage;
    bool isComplete;
    bool success;
    std::string errorMessage;
    bool* cancelledPtr;  // Pointer to cancelled flag in the event
};

struct PersonalizedTicketPrompt {
    std::string titleName;
    std::string filePath;
    uint8_t rightsId[16];
    uint64_t deviceId;
    uint32_t accountId;
    bool active;
    void* streamContext;  // Pointer to StreamInstallContext
};

class GuiManager {
public:
    static GuiManager& getInstance() {
        static GuiManager instance;
        return instance;
    }

    void initialize();
    void updateNotifications(float deltaTime);
    void renderNotifications();
    void renderModals();
    void renderStatusBar();
    int getCurrentScreen() const { return currentScreen; }
    void setCurrentScreen(int screen) { currentScreen = screen; }

    bool isModalActive() const;

private:
    GuiManager() = default;
    ~GuiManager() = default;

    void onScreenChange(const ScreenChangeEvent& event);
    void onNotification(const NotificationEvent& event);
    void onTransferStart(const TransferStartEvent& event);
    void onTransferProgress(const TransferProgressEvent& event);
    void onTransferComplete(const TransferCompleteEvent& event);
    void onInstallStart(const InstallStartEvent& event);
    void onInstallProgress(const InstallProgressEvent& event);
    void onInstallComplete(const InstallCompleteEvent& event);
    void onPersonalizedTicket(const PersonalizedTicketEvent& event);

    ImVec4 getNotificationColor(NotificationEvent::Type type) const;
    const char* getNotificationIcon(NotificationEvent::Type type) const;
    bool renderTransferModal(const TransferProgress& transfer);
    void renderInstallModal(const InstallProgress& install);
    void renderPersonalizedTicketModal();

    int currentScreen = Screen_MainMenu;
    std::vector<Notification> notifications;
    std::unordered_map<std::string, TransferProgress> activeTransfers;
    std::unordered_map<std::string, InstallProgress> activeInstalls;
    PersonalizedTicketPrompt ticketPrompt;
    std::vector<uint64_t> eventSubscriptions;
};

} // namespace Javelin
