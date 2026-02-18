// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#pragma once
#include <switch.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ImGui_ImplSwitch_Init(void);
void ImGui_ImplSwitch_Shutdown(void);
void ImGui_ImplSwitch_NewFrame(void);
bool ImGui_ImplSwitch_ShouldClose(void);
void ImGui_ImplSwitch_SetShouldClose(bool should_close);

#ifdef __cplusplus
}
#endif
