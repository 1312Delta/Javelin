// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "GuiManager.h"
#include "i18n/Localization.h"
#include "install/stream_install.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace Javelin {

void GuiManager::initialize() {
    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<ScreenChangeEvent>(
            [this](ScreenChangeEvent& e) { onScreenChange(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<NotificationEvent>(
            [this](NotificationEvent& e) { onNotification(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<TransferStartEvent>(
            [this](TransferStartEvent& e) { onTransferStart(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<TransferProgressEvent>(
            [this](TransferProgressEvent& e) { onTransferProgress(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<TransferCompleteEvent>(
            [this](TransferCompleteEvent& e) { onTransferComplete(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<InstallStartEvent>(
            [this](InstallStartEvent& e) { onInstallStart(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<InstallProgressEvent>(
            [this](InstallProgressEvent& e) { onInstallProgress(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<InstallCompleteEvent>(
            [this](InstallCompleteEvent& e) { onInstallComplete(e); }
        )
    );

    eventSubscriptions.push_back(
        EventBus::getInstance().subscribe<PersonalizedTicketEvent>(
            [this](PersonalizedTicketEvent& e) { onPersonalizedTicket(e); }
        )
    );

    // Initialize ticket prompt
    ticketPrompt.active = false;
}

void GuiManager::updateNotifications(float deltaTime) {
    for (auto& notif : notifications) {
        notif.endTime -= deltaTime;
    }

    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const Notification& n) {
                return n.endTime <= 0.0f;
            }),
        notifications.end()
    );
}

void GuiManager::renderNotifications() {
    if (notifications.empty()) return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;
    ImVec2 windowPos(workPos.x + workSize.x - 350.0f, workPos.y + 10.0f);

    ImGui::SetNextWindowPos(windowPos);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.9f);

    if (ImGui::Begin("Notifications", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {

        for (const auto& notification : notifications) {
            const char* icon = getNotificationIcon(notification.type);
            ImVec4 color = getNotificationColor(notification.type);

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Text("%s", icon);
            ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::TextWrapped("%s", notification.message.c_str());
            ImGui::Separator();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void GuiManager::renderModals() {
    // Ticket prompt has highest priority - block all other modals when active
    if (ticketPrompt.active) {
        renderPersonalizedTicketModal();
        return;
    }

    std::vector<std::string> transfersToRemove;

    for (auto& pair : activeTransfers) {
        bool shouldRemove = renderTransferModal(pair.second);
        if (shouldRemove) {
            transfersToRemove.push_back(pair.first);
        }
    }

    for (const auto& key : transfersToRemove) {
        activeTransfers.erase(key);
    }

    for (auto& pair : activeInstalls) {
        renderInstallModal(pair.second);
    }
}

bool GuiManager::isModalActive() const {
    return ticketPrompt.active || !activeTransfers.empty() || !activeInstalls.empty();
}

void GuiManager::renderStatusBar() {
    // Find the most relevant active operation to show
    const TransferProgress* activeTransfer = nullptr;
    for (const auto& pair : activeTransfers) {
        if (!pair.second.isComplete) {
            activeTransfer = &pair.second;
            break;
        }
    }

    const InstallProgress* activeInstall = nullptr;
    for (const auto& pair : activeInstalls) {
        if (!pair.second.isComplete) {
            activeInstall = &pair.second;
            break;
        }
    }

    if (!activeTransfer && !activeInstall) return;

    const float barHeight = 28.0f;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImVec2 barMin(0.0f, 0.0f);
    ImVec2 barMax(1280.0f, barHeight);

    // Background
    drawList->AddRectFilled(barMin, barMax, IM_COL32(16, 16, 24, 230));
    // Bottom border
    drawList->AddLine(ImVec2(0.0f, barHeight), ImVec2(1280.0f, barHeight), IM_COL32(40, 60, 100, 200));

    float x = 8.0f;
    float textY = (barHeight - ImGui::GetFontSize()) * 0.5f;

    if (activeTransfer) {
        // Label
        const char* label = "[Transferring]";
        drawList->AddText(ImVec2(x, textY), IM_COL32(100, 180, 255, 255), label);
        x += ImGui::CalcTextSize(label).x + 8.0f;

        // Filename (truncated)
        const char* path = activeTransfer->filePath.c_str();
        const char* fname = path;
        for (const char* p = path; *p; p++) {
            if (*p == '/' || *p == '\\') fname = p + 1;
        }
        char truncName[40];
        snprintf(truncName, sizeof(truncName), "%s", fname);
        drawList->AddText(ImVec2(x, textY), IM_COL32(220, 220, 230, 255), truncName);
        x += ImGui::CalcTextSize(truncName).x + 12.0f;

        // Percentage and speed
        char stats[64];
        snprintf(stats, sizeof(stats), "%.1f%%  %.1f MB/s",
                 activeTransfer->progressPercent, activeTransfer->speedMBps);
        drawList->AddText(ImVec2(x, textY), IM_COL32(180, 200, 220, 255), stats);
        x += ImGui::CalcTextSize(stats).x + 12.0f;

        // Progress bar
        float barW = 200.0f;
        float barX = 1280.0f - barW - 10.0f;
        float barY0 = 8.0f;
        float barY1 = barHeight - 8.0f;
        float fill = activeTransfer->progressPercent / 100.0f;
        if (fill > 1.0f) fill = 1.0f;
        drawList->AddRectFilled(ImVec2(barX, barY0), ImVec2(barX + barW, barY1),
                                IM_COL32(30, 30, 45, 255), 3.0f);
        if (fill > 0.0f) {
            drawList->AddRectFilled(ImVec2(barX, barY0), ImVec2(barX + barW * fill, barY1),
                                    IM_COL32(60, 140, 220, 255), 3.0f);
        }
    } else if (activeInstall) {
        const char* label = "[Installing]";
        drawList->AddText(ImVec2(x, textY), IM_COL32(100, 255, 140, 255), label);
        x += ImGui::CalcTextSize(label).x + 8.0f;

        char truncName[40];
        snprintf(truncName, sizeof(truncName), "%s", activeInstall->titleName.c_str());
        drawList->AddText(ImVec2(x, textY), IM_COL32(220, 220, 230, 255), truncName);
        x += ImGui::CalcTextSize(truncName).x + 12.0f;

        char stats[64];
        snprintf(stats, sizeof(stats), "%.1f%%", activeInstall->progressPercent);
        drawList->AddText(ImVec2(x, textY), IM_COL32(180, 200, 220, 255), stats);

        float barW = 200.0f;
        float barX = 1280.0f - barW - 10.0f;
        float barY0 = 8.0f;
        float barY1 = barHeight - 8.0f;
        float fill = activeInstall->progressPercent / 100.0f;
        if (fill > 1.0f) fill = 1.0f;
        drawList->AddRectFilled(ImVec2(barX, barY0), ImVec2(barX + barW, barY1),
                                IM_COL32(30, 30, 45, 255), 3.0f);
        if (fill > 0.0f) {
            drawList->AddRectFilled(ImVec2(barX, barY0), ImVec2(barX + barW * fill, barY1),
                                    IM_COL32(60, 200, 100, 255), 3.0f);
        }
    }
}

void GuiManager::onScreenChange(const ScreenChangeEvent& event) {
    currentScreen = event.screenId;
}

void GuiManager::onNotification(const NotificationEvent& event) {
    float duration = event.durationMs / 1000.0f;

    Notification notif;
    notif.message = event.message;
    notif.type = event.type;
    notif.endTime = duration;

    notifications.push_back(notif);

    if (notifications.size() > 10) {
        notifications.erase(notifications.begin());
    }
}

void GuiManager::onTransferStart(const TransferStartEvent& event) {
    TransferProgress progress;
    progress.filePath = event.filePath;
    progress.bytesTransferred = 0;
    progress.totalBytes = event.totalBytes;
    progress.progressPercent = 0.0f;
    progress.speedMBps = 0.0f;
    progress.isComplete = false;
    progress.success = false;
    progress.cancelledPtr = event.cancelledPtr;

    activeTransfers[event.filePath] = progress;
}

void GuiManager::onTransferProgress(const TransferProgressEvent& event) {
    auto it = activeTransfers.find(event.filePath);
    if (it != activeTransfers.end()) {
        it->second.bytesTransferred = event.bytesTransferred;
        it->second.progressPercent = event.progressPercent;
        it->second.speedMBps = event.speedMBps;
    }
}

void GuiManager::onTransferComplete(const TransferCompleteEvent& event) {
    auto it = activeTransfers.find(event.filePath);
    if (it != activeTransfers.end()) {
        it->second.isComplete = true;
        it->second.success = event.success;
        it->second.errorMessage = event.errorMessage;
        it->second.bytesTransferred = event.totalBytes;
        it->second.progressPercent = 100.0f;
    }
}

void GuiManager::onInstallStart(const InstallStartEvent& event) {
    InstallProgress progress;
    progress.titleName = event.titleName;
    progress.filePath = event.filePath;
    progress.progressPercent = 0.0f;
    progress.bytesWritten = 0;
    progress.totalBytes = 0;
    progress.isComplete = false;
    progress.success = false;
    progress.cancelledPtr = nullptr;

    activeInstalls[event.titleName] = progress;
}

void GuiManager::onInstallProgress(const InstallProgressEvent& event) {
    auto it = activeInstalls.find(event.titleName);
    if (it != activeInstalls.end()) {
        it->second.progressPercent = event.progressPercent;
        it->second.currentFile = event.currentFile;
        it->second.bytesWritten = event.bytesWritten;
        it->second.totalBytes = event.totalBytes;
        it->second.stage = event.stage;
    }
}

void GuiManager::onInstallComplete(const InstallCompleteEvent& event) {
    auto it = activeInstalls.find(event.titleName);
    if (it != activeInstalls.end()) {
        it->second.isComplete = true;
        it->second.success = event.success;
        it->second.errorMessage = event.errorMessage;
        it->second.progressPercent = event.success ? 100.0f : 0.0f;
    }
}

void GuiManager::onPersonalizedTicket(const PersonalizedTicketEvent& event) {
    // Store ticket info and activate prompt
    ticketPrompt.titleName = event.titleName;
    ticketPrompt.filePath = event.filePath;
    memcpy(ticketPrompt.rightsId, event.rightsId, sizeof(ticketPrompt.rightsId));
    ticketPrompt.deviceId = event.deviceId;
    ticketPrompt.accountId = event.accountId;
    ticketPrompt.streamContext = event.streamContext;
    ticketPrompt.active = true;
}

ImVec4 GuiManager::getNotificationColor(NotificationEvent::Type type) const {
    switch (type) {
        case NotificationEvent::Type::Info:
            return ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
        case NotificationEvent::Type::Success:
            return ImVec4(0.3f, 1.0f, 0.5f, 1.0f);
        case NotificationEvent::Type::Warning:
            return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        case NotificationEvent::Type::Error:
            return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

const char* GuiManager::getNotificationIcon(NotificationEvent::Type type) const {
    switch (type) {
        case NotificationEvent::Type::Info: return TR("icon.info");
        case NotificationEvent::Type::Success: return TR("icon.success");
        case NotificationEvent::Type::Warning: return TR("icon.warning");
        case NotificationEvent::Type::Error: return TR("icon.error");
        default: return "[?]";
    }
}

bool GuiManager::renderTransferModal(const TransferProgress& transfer) {
    bool shouldRemove = false;

    if (transfer.isComplete) {
        ImGui::SetNextWindowPos(ImVec2(640, 360), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

        const char* title = transfer.success ? TR("modal.transfer_complete") : TR("modal.transfer_failed");

        if (ImGui::Begin(title, nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {

            ImGui::Text("%s %s", TR("modal.file"), transfer.filePath.c_str());

            if (transfer.success) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f),
                                  TR("modal.success"),
                                  transfer.bytesTransferred / (1024.0f * 1024.0f));
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                  TR("modal.error"), transfer.errorMessage.c_str());
            }

            ImGui::Spacing();
            if (ImGui::Button(TR("modal.ok"), ImVec2(200, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                shouldRemove = true;
            }
        }
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(640, 360), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

        if (ImGui::Begin(TR("modal.transferring"), nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {

            ImGui::Text("%s %s", TR("modal.file"), transfer.filePath.c_str());
            ImGui::Spacing();

            char progressText[256];
            snprintf(progressText, sizeof(progressText),
                    "%.1f%% (%.2f MB / %.2f MB) - %.2f MB/s",
                    transfer.progressPercent,
                    transfer.bytesTransferred / (1024.0f * 1024.0f),
                    transfer.totalBytes / (1024.0f * 1024.0f),
                    transfer.speedMBps);

            ImGui::ProgressBar(transfer.progressPercent / 100.0f, ImVec2(480, 0), progressText);

            ImGui::Spacing();

            if (ImGui::Button(TR("modal.cancel"), ImVec2(200, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
                if (transfer.cancelledPtr) {
                    __atomic_store_n(transfer.cancelledPtr, true, __ATOMIC_RELEASE);
                }
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                          "%s", TR("modal.please_wait"));
        }
        ImGui::End();
    }

    return shouldRemove;
}

void GuiManager::renderInstallModal(const InstallProgress& install) {
    if (install.isComplete) {
        ImGui::SetNextWindowPos(ImVec2(640, 360), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

        const char* title = install.success ? TR("modal.install_complete") : TR("modal.install_failed");

        if (ImGui::Begin(title, nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {

            ImGui::Text("%s %s", TR("modal.title"), install.titleName.c_str());
            ImGui::Spacing();

            if (install.success) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f),
                                  TR("modal.install_success"),
                                  install.bytesWritten / (1024.0f * 1024.0f));
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                  TR("modal.error"), install.errorMessage.c_str());
            }

            ImGui::Spacing();
            if (ImGui::Button(TR("modal.ok"), ImVec2(200, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                activeInstalls.erase(install.titleName);
            }
        }
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(640, 360), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 0), ImGuiCond_Appearing);

        if (ImGui::Begin(TR("modal.installing"), nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {

            char titleText[512];
            snprintf(titleText, sizeof(titleText), "%s %s", TR("modal.title"), install.titleName.c_str());
            ImGui::Text("%s", titleText);
            ImGui::Spacing();

            if (!install.currentFile.empty()) {
                char fileText[512];
                snprintf(fileText, sizeof(fileText), TR("modal.installing_file"), install.currentFile.c_str());
                ImGui::Text("%s", fileText);
            }

            if (!install.stage.empty()) {
                char stageText[256];
                snprintf(stageText, sizeof(stageText), TR("modal.stage"), install.stage.c_str());
                ImGui::Text("%s", stageText);
            }

            ImGui::Spacing();

            char progressText[256];
            snprintf(progressText, sizeof(progressText),
                    "%.1f%% (%.2f MB / %.2f MB)",
                    install.progressPercent,
                    install.bytesWritten / (1024.0f * 1024.0f),
                    install.totalBytes / (1024.0f * 1024.0f));

            ImGui::ProgressBar(install.progressPercent / 100.0f, ImVec2(480, 0), progressText);

            ImGui::Spacing();

            if (ImGui::Button(TR("modal.cancel"), ImVec2(200, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
                if (install.cancelledPtr) {
                    __atomic_store_n(install.cancelledPtr, true, __ATOMIC_RELEASE);
                }
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                          "%s", TR("modal.please_wait"));
        }
        ImGui::End();
    }
}

void GuiManager::renderPersonalizedTicketModal() {
    if (!ticketPrompt.active) return;

    ImGui::SetNextWindowPos(ImVec2(640, 360), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Appearing);

    if (ImGui::Begin(TR("ticket.personalized_warning"), nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {

        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 680);

        // Warning icon and title
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), TR("icon.warning"));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), TR("ticket.personalized_detected"));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Explanation text
        ImGui::TextWrapped(TR("ticket.personalized_explanation"));

        ImGui::Spacing();

        // Ticket details
        ImGui::Text(TR("ticket.detail_device_id"));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "0x%016lX", ticketPrompt.deviceId);

        ImGui::Text(TR("ticket.detail_account_id"));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "0x%08X", ticketPrompt.accountId);

        // Rights ID
        ImGui::Text(TR("ticket.detail_rights_id"));
        ImGui::SameLine();
        char rightsIdStr[33];
        for (int i = 0; i < 16; i++) {
            snprintf(rightsIdStr + i * 2, 3, "%02X", ticketPrompt.rightsId[i]);
        }
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", rightsIdStr);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Conversion explanation
        ImGui::TextWrapped(TR("ticket.conversion_explanation"));

        ImGui::Spacing();

        ImGui::PopTextWrapPos();

        // Buttons
        ImGui::Spacing();
        float buttonWidth = 300.0f;
        float totalWidth = buttonWidth * 2 + 20.0f;
        float startX = (ImGui::GetWindowWidth() - totalWidth) / 2.0f;

        ImGui::SetCursorPosX(startX);

        if (ImGui::Button(TR("ticket.convert_to_common"), ImVec2(buttonWidth, 50)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
            // User approved conversion - notify the install context
            if (ticketPrompt.streamContext) {
                streamInstallSetTicketConversionApproved((StreamInstallContext*)ticketPrompt.streamContext, true);
            }
            ticketPrompt.active = false;
        }

        ImGui::SameLine();

        if (ImGui::Button(TR("ticket.cancel_install"), ImVec2(buttonWidth, 50)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            // User rejected - cancel installation
            if (ticketPrompt.streamContext) {
                streamInstallSetTicketConversionApproved((StreamInstallContext*)ticketPrompt.streamContext, false);
            }
            ticketPrompt.active = false;
        }

        ImGui::Spacing();
    }
    ImGui::End();
}

}
