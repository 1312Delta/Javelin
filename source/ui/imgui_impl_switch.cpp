// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "imgui_impl_switch.h"
#include "imgui.h"

static bool g_ShouldClose = false;

bool ImGui_ImplSwitch_Init(void) {
    g_ShouldClose = false;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    return true;
}

void ImGui_ImplSwitch_Shutdown(void) {
}

void ImGui_ImplSwitch_NewFrame(void) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
}

bool ImGui_ImplSwitch_ShouldClose(void) {
    return g_ShouldClose;
}

void ImGui_ImplSwitch_SetShouldClose(bool should_close) {
    g_ShouldClose = should_close;
}
