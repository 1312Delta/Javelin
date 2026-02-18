// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "ui/imgui_impl_switch.h"
#include "mtp/usb_mtp.h"
#include "mtp/mtp_protocol.h"
#include "mtp/mtp_log.h"
#include "dump/game_dump.h"
#include "dump/gamecard_dump.h"
#include "core/GuiManager.h"
#include "core/GuiEvents.h"
#include "tickets/ticket_browser.h"
#include "i18n/Localization.h"
#include "core/Settings.h"
#include "core/Debug.h"
#include <cmath>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

using namespace Javelin;

void renderSettingsScreen();
void renderDumpScreen();
void renderInstallScreen();

// Dump subsystem contexts (shared between main menu and dump screen)
static DumpContext g_dump_ctx = {0};
static GcContext g_gc_ctx = {0};
static bool g_dump_services_inited = false;

// Dump thread state
static Thread g_dump_thread;
static bool g_dump_thread_running = false;
static bool g_dump_thread_needs_join = false;
static bool g_dump_should_cancel = false;

struct DumpTaskInfo {
    enum Mode { MERGED, SEPARATE, GC_XCI, GC_NSP };
    Mode mode;
    u32 game_index;
    u32 meta_index;  // for SEPARATE mode
};
static DumpTaskInfo g_dump_task = {};

static void dumpThreadFunc(void* arg);

// Install screen state
static Thread g_install_thread;
static bool g_install_thread_running = false;
static bool g_install_thread_needs_join = false;

struct InstallFileEntry {
    char filename[256];
    char fullpath[512];
    u64 file_size;
    bool is_xci; // true = XCI, false = NSP
};

#define INSTALL_MAX_FILES 256
static InstallFileEntry g_install_files[INSTALL_MAX_FILES];
static u32 g_install_file_count = 0;
static bool g_install_files_scanned = false;

struct InstallTaskInfo {
    char filepath[512];
    char filename[256];
    bool is_xci;
    InstallTarget target;
};
static InstallTaskInfo g_install_task = {};

static void installThreadFunc(void* arg);

static void scanBackupsFolder() {
    g_install_file_count = 0;

    const char* dir_path = "sdmc:/switch/Javelin/backups";
    DIR* dir = opendir(dir_path);
    if (!dir) {
        g_install_files_scanned = true;
        return;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && g_install_file_count < INSTALL_MAX_FILES) {
        size_t len = strlen(ent->d_name);
        if (len < 5) continue;

        const char* ext = ent->d_name + len - 4;
        bool is_nsp = (strcasecmp(ext, ".nsp") == 0);
        bool is_xci = (strcasecmp(ext, ".xci") == 0);

        if (ent->d_type == DT_REG && (is_nsp || is_xci)) {
            // Regular file: .nsp or .xci
            InstallFileEntry* entry = &g_install_files[g_install_file_count];
            strncpy(entry->filename, ent->d_name, sizeof(entry->filename) - 1);
            entry->filename[sizeof(entry->filename) - 1] = '\0';
            snprintf(entry->fullpath, sizeof(entry->fullpath), "%s/%s", dir_path, ent->d_name);
            entry->is_xci = is_xci;

            struct stat st;
            if (stat(entry->fullpath, &st) == 0) {
                entry->file_size = st.st_size;
            } else {
                entry->file_size = 0;
            }

            g_install_file_count++;
        } else if (ent->d_type == DT_DIR && (is_nsp || is_xci)) {
            // Split directory: GameName.nsp/ or GameName.xci/ with numbered parts inside
            InstallFileEntry* entry = &g_install_files[g_install_file_count];
            strncpy(entry->filename, ent->d_name, sizeof(entry->filename) - 1);
            entry->filename[sizeof(entry->filename) - 1] = '\0';
            snprintf(entry->fullpath, sizeof(entry->fullpath), "%s/%s", dir_path, ent->d_name);
            entry->is_xci = is_xci;

            // Sum up all split part sizes
            u64 total_size = 0;
            for (u32 i = 0; i < 64; i++) {
                char part_path[528];
                snprintf(part_path, sizeof(part_path), "%s/%02u", entry->fullpath, i);
                struct stat st;
                if (stat(part_path, &st) != 0) break;
                total_size += (u64)st.st_size;
            }
            entry->file_size = total_size;

            g_install_file_count++;
        }
    }

    closedir(dir);
    g_install_files_scanned = true;
}

struct InstallProgressCtx {
    std::string filepath;
    u64 last_update_tick;
};

static void installProgressCb(u64 bytes_written, u64 total_bytes, void* user_data) {
    InstallProgressCtx* pctx = (InstallProgressCtx*)user_data;

    u64 now = armGetSystemTick();
    // Throttle updates to ~100ms
    if (now - pctx->last_update_tick < armNsToTicks(100000000ULL) && bytes_written < total_bytes) {
        return;
    }
    pctx->last_update_tick = now;

    float percent = (total_bytes > 0) ? ((float)bytes_written / (float)total_bytes) * 100.0f : 0.0f;
    float speed = 0.0f;
    // Speed not easily computed here, leave at 0
    TransferProgressEvent evt(pctx->filepath, bytes_written, total_bytes, percent, speed);
    EventBus::getInstance().post(evt);
}

static void installThreadFunc(void* arg) {
    (void)arg;
    InstallTaskInfo task = g_install_task;

    {
        TransferStartEvent evt;
        evt.filePath = task.filepath;
        evt.totalBytes = 0;
        EventBus::getInstance().post(evt);
    }

    NcaInstallContext nca_ctx;
    Result rc = ncaInstallInit(&nca_ctx, task.target);
    if (R_FAILED(rc)) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "Failed to initialize install context: 0x%08X", rc);
        LOG_ERROR("NCA Install: %s", err_msg);
        showError(err_msg);
        TransferCompleteEvent evt;
        evt.filePath = task.filepath;
        evt.success = false;
        evt.errorMessage = err_msg;
        EventBus::getInstance().post(evt);
        g_install_thread_running = false;
        return;
    }

    InstallProgressCtx progress_ctx;
    progress_ctx.filepath = task.filepath;
    progress_ctx.last_update_tick = 0;
    nca_ctx.progress_cb = installProgressCb;
    nca_ctx.progress_user_data = &progress_ctx;

    u64 title_id = 0;
    if (task.is_xci) {
        rc = ncaInstallXci(&nca_ctx, task.filepath, &title_id);
    } else {
        rc = ncaInstallNsp(&nca_ctx, task.filepath, &title_id);
    }

    ncaInstallExit(&nca_ctx);

    {
        TransferCompleteEvent evt;
        evt.filePath = task.filepath;
        evt.success = R_SUCCEEDED(rc);
        if (R_FAILED(rc)) {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "Installation failed: 0x%08X", rc);
            evt.errorMessage = err_msg;
            LOG_ERROR("NCA Install: %s (path=%s)", err_msg, task.filepath);
        }
        EventBus::getInstance().post(evt);
    }

    if (R_SUCCEEDED(rc)) {
        showSuccess(TR("install.complete"));
    } else {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "%s (0x%08X)", TR("install.failed"), rc);
        showError(err_msg);
    }

    g_install_thread_running = false;
}

static void startInstallThread(InstallTaskInfo task) {
    if (g_install_thread_running) return;

    if (g_install_thread_needs_join) {
        threadWaitForExit(&g_install_thread);
        threadClose(&g_install_thread);
        g_install_thread_needs_join = false;
    }

    g_install_task = task;
    g_install_thread_running = true;
    g_install_thread_needs_join = true;

    Result rc = threadCreate(&g_install_thread, installThreadFunc, NULL, NULL, 0x20000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&g_install_thread);
    } else {
        g_install_thread_running = false;
        g_install_thread_needs_join = false;
        showError(TR("install.failed"));
    }
}

static void centerText(const char* text) {
    float windowWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(text).x) / 2);
}

static void applyJavelinTheme() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;

    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]          = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    c[ImGuiCol_ChildBg]           = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg]           = ImVec4(0.10f, 0.10f, 0.15f, 0.96f);
    c[ImGuiCol_Border]            = ImVec4(0.20f, 0.22f, 0.30f, 0.60f);
    c[ImGuiCol_BorderShadow]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]           = ImVec4(0.14f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.18f, 0.20f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.22f, 0.24f, 0.34f, 1.00f);
    c[ImGuiCol_TitleBg]           = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.10f, 0.10f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.08f, 0.08f, 0.12f, 0.75f);
    c[ImGuiCol_MenuBarBg]         = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
    c[ImGuiCol_Tab]               = ImVec4(0.14f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_TabHovered]        = ImVec4(0.28f, 0.42f, 0.72f, 0.80f);
    c[ImGuiCol_TabSelected]       = ImVec4(0.22f, 0.36f, 0.65f, 1.00f);
    c[ImGuiCol_Button]            = ImVec4(0.16f, 0.28f, 0.36f, 1.00f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(0.20f, 0.38f, 0.50f, 1.00f);
    c[ImGuiCol_ButtonActive]      = ImVec4(0.14f, 0.46f, 0.58f, 1.00f);
    c[ImGuiCol_Header]            = ImVec4(0.18f, 0.20f, 0.28f, 1.00f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.24f, 0.32f, 0.48f, 0.80f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.26f, 0.38f, 0.56f, 1.00f);
    c[ImGuiCol_Separator]         = ImVec4(0.20f, 0.22f, 0.30f, 0.50f);
    c[ImGuiCol_SeparatorHovered]  = ImVec4(0.30f, 0.50f, 0.70f, 0.78f);
    c[ImGuiCol_SeparatorActive]   = ImVec4(0.30f, 0.50f, 0.70f, 1.00f);
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.08f, 0.08f, 0.12f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.24f, 0.26f, 0.34f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.34f, 0.44f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.36f, 0.40f, 0.52f, 1.00f);
    c[ImGuiCol_SliderGrab]        = ImVec4(0.30f, 0.55f, 0.70f, 1.00f);
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.36f, 0.65f, 0.82f, 1.00f);
    c[ImGuiCol_CheckMark]         = ImVec4(0.40f, 0.75f, 0.90f, 1.00f);
    c[ImGuiCol_PlotHistogram]     = ImVec4(0.30f, 0.60f, 0.80f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.40f, 0.70f, 0.90f, 1.00f);
    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.12f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.16f, 0.18f, 0.24f, 1.00f);
    c[ImGuiCol_TableRowBg]        = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(0.12f, 0.12f, 0.16f, 0.40f);
    c[ImGuiCol_NavHighlight]      = ImVec4(0.30f, 0.60f, 0.85f, 1.00f);
    c[ImGuiCol_Text]              = ImVec4(0.92f, 0.93f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]      = ImVec4(0.40f, 0.42f, 0.48f, 1.00f);
}

static bool renderMenuCard(const char* label, const char* description,
                           const ImVec4& accentColor, float width, bool enabled = true) {
    bool clicked = false;
    float cardHeight = 80.0f;

    ImGui::PushID(label);

    if (!enabled) ImGui::BeginDisabled(true);

    ImVec2 cursor = ImGui::GetCursorScreenPos();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.18f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.18f, 0.24f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.20f, 0.28f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    clicked = ImGui::Button("##card", ImVec2(width, cardHeight));

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        cursor,
        ImVec2(cursor.x + 4.0f, cursor.y + cardHeight),
        ImGui::GetColorU32(accentColor),
        4.0f, ImDrawFlags_RoundCornersLeft);

    ImVec2 textPos(cursor.x + 20.0f, cursor.y + 14.0f);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.1f,
                      textPos, ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.98f, 1.0f)), label);

    ImVec2 descPos(cursor.x + 20.0f, cursor.y + 44.0f);
    drawList->AddText(descPos, ImGui::GetColorU32(ImVec4(0.55f, 0.58f, 0.65f, 1.0f)), description);

    if (!enabled) ImGui::EndDisabled();

    ImGui::PopID();
    return clicked;
}

static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

static bool initEgl() {
    hidInitializeNpad();
    hidInitializeTouchScreen();

    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_display) return false;
    if (!eglInitialize(s_display, nullptr, nullptr)) return false;
    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
        eglTerminate(s_display);
        return false;
    }
    EGLConfig config;
    EGLint numConfigs;
    static const EGLint framebufferAttributeList[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
    };
    eglChooseConfig(s_display, framebufferAttributeList, &config, 1, &numConfigs);
    if (numConfigs == 0) { eglTerminate(s_display); return false; }
    s_surface = eglCreateWindowSurface(s_display, config, nwindowGetDefault(), nullptr);
    if (!s_surface) { eglTerminate(s_display); return false; }
    static const EGLint contextAttributeList[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE
    };
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributeList);
    if (!s_context) { eglDestroySurface(s_display, s_surface); eglTerminate(s_display); return false; }
    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    return true;
}

static void deinitEgl() {
    if (s_display) {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_context) eglDestroyContext(s_display, s_context);
        if (s_surface) eglDestroySurface(s_display, s_surface);
        eglTerminate(s_display);
    }
}

void renderMainMenu(bool& mtp_running) {
    float windowWidth = ImGui::GetContentRegionAvail().x;
    float cardWidth = 560.0f;
    float centerX = (windowWidth - cardWidth) / 2;

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.75f, 0.90f, 1.0f));
    centerText("JAVELIN");
    ImGui::Text("%s", "JAVELIN");
    ImGui::PopStyleColor();

    const char* subtitle = TR("app.subtitle");
    centerText(subtitle);
    ImGui::TextColored(ImVec4(0.45f, 0.48f, 0.55f, 1.0f), "%s", subtitle);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::SetCursorPosX(centerX);
    if (renderMenuCard(TR("menu.mtp_title"),
                       TR("menu.mtp_desc"),
                       ImVec4(0.30f, 0.70f, 0.90f, 1.0f), cardWidth)) {
        ScreenChangeEvent event(Screen_MTP);
        EventBus::getInstance().post(event);
    }

    ImGui::Spacing();

    ImGui::SetCursorPosX(centerX);
    if (renderMenuCard(TR("menu.tickets_title"),
                       TR("menu.tickets_desc"),
                       ImVec4(0.90f, 0.70f, 0.40f, 1.0f), cardWidth)) {
        ScreenChangeEvent event(Screen_Tickets);
        EventBus::getInstance().post(event);
    }

    ImGui::Spacing();

    ImGui::SetCursorPosX(centerX);
    if (renderMenuCard(TR("menu.dump_title"),
                       TR("menu.dump_desc"),
                       ImVec4(0.80f, 0.45f, 0.30f, 1.0f), cardWidth)) {
        ScreenChangeEvent event(Screen_Dump);
        EventBus::getInstance().post(event);
    }

    ImGui::Spacing();

    ImGui::SetCursorPosX(centerX);
    if (renderMenuCard(TR("menu.install_title"),
                       TR("menu.install_desc"),
                       ImVec4(0.45f, 0.80f, 0.45f, 1.0f), cardWidth)) {
        ScreenChangeEvent event(Screen_Install);
        EventBus::getInstance().post(event);
    }

    ImGui::Spacing();

    ImGui::SetCursorPosX(centerX);
    if (renderMenuCard(TR("menu.settings_title"),
                       TR("menu.settings_desc"),
                       ImVec4(0.60f, 0.55f, 0.75f, 1.0f), cardWidth)) {
        ScreenChangeEvent event(Screen_Settings);
        EventBus::getInstance().post(event);
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 60.0f);
    ImGui::Separator();
    ImGui::Spacing();

    if (mtp_running) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.50f, 1.0f), "  %s", TR("status.mtp_running"));
        ImGui::SameLine(windowWidth - 160);
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.48f, 0.55f, 1.0f), "  %s", TR("status.press_plus_exit"));
        ImGui::SameLine(windowWidth - 160);
    }
    ImGui::TextColored(ImVec4(0.35f, 0.38f, 0.45f, 1.0f), "v1.0.0");
}

void renderMTPScreen(MtpProtocolContext& mtp_ctx, bool& usb_initialized, bool& mtp_running, char* status_msg) {
    const char* mtpTitle = TR("mtp.title");
    centerText(mtpTitle);
    ImGui::Text("%s", mtpTitle);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("%s %s", TR("mtp.status"), status_msg);
    ImGui::Spacing();

    if (mtp_running) {
        ImGui::Text("%s", TR("mtp.storage"));
        if (mtp_ctx.storage.sdcard.mounted) {
            ImGui::Text("  %s %.1f GB free / %.1f GB total", TR("mtp.sd_card"),
                (float)mtp_ctx.storage.sdcard.free_space / (1024*1024*1024),
                (float)mtp_ctx.storage.sdcard.max_capacity / (1024*1024*1024));
        }
        if (mtp_ctx.storage.user.mounted) {
            ImGui::Text("  %s: %.1f GB free / %.1f GB total",
                mtp_ctx.storage.user.description,
                (float)mtp_ctx.storage.user.free_space / (1024*1024*1024),
                (float)mtp_ctx.storage.user.max_capacity / (1024*1024*1024));
        }
        if (mtp_ctx.storage.system.mounted) {
            ImGui::Text("  %s: %.1f GB free / %.1f GB total",
                mtp_ctx.storage.system.description,
                (float)mtp_ctx.storage.system.free_space / (1024*1024*1024),
                (float)mtp_ctx.storage.system.max_capacity / (1024*1024*1024));
        }
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "  %s", TR("mtp.install_sd"));
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "  %s", TR("mtp.install_nand"));
        ImGui::Text("  %s %u", TR("mtp.objects"), mtp_ctx.storage.object_count);
        if (mtpStorageIsIndexing(&mtp_ctx.storage)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", TR("mtp.indexing"));
        }
        if (mtp_ctx.install.install_pending) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "  %s %s (%u%%)", TR("mtp.transferring"),
                mtp_ctx.install.install_filename,
                mtp_ctx.install.install_size > 0 ?
                    (unsigned int)((mtp_ctx.install.install_written * 100) / mtp_ctx.install.install_size) : 0);
        }
        ImGui::Spacing();
    }

    if (!mtp_running) {
        if (ImGui::Button(TR("mtp.start"), ImVec2(200, 50))) {
            Result rc = usbMtpInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = mtpProtocolInit(&mtp_ctx);
                if (R_SUCCEEDED(rc)) {
                    usb_initialized = true;
                    mtp_running = true;
                    snprintf(status_msg, 256, "%s", TR("mtp.status_running"));
                    showSuccess(TR("mtp.init_success"));
                } else {
                    usbMtpExit();
                    snprintf(status_msg, 256, "MTP init failed: 0x%08X", rc);
                    showError(TR("mtp.init_failed"));
                }
            } else {
                snprintf(status_msg, 256, "USB init failed: 0x%08X", rc);
                showError(TR("mtp.usb_failed"));
            }
        }
    } else {
        if (ImGui::Button(TR("mtp.stop"), ImVec2(200, 50))) {
            mtp_running = false;
            snprintf(status_msg, 256, "%s", TR("mtp.status_stopping"));
            showInfo(TR("mtp.stopping"));
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("mtp.refresh"), ImVec2(100, 50))) {
            if (mtp_ctx.storage.sdcard.mounted)
                mtpStorageRefresh(&mtp_ctx.storage, MTP_STORAGE_SDCARD);
            if (mtp_ctx.storage.user.mounted)
                mtpStorageRefresh(&mtp_ctx.storage, MTP_STORAGE_USER);
            if (mtp_ctx.storage.system.mounted)
                mtpStorageRefresh(&mtp_ctx.storage, MTP_STORAGE_NAND_SYSTEM);
            snprintf(status_msg, 256, "%s", TR("mtp.storage_refreshed"));
            showSuccess(TR("mtp.storage_refreshed"));
        }
    }

    ImGui::Spacing();

    if (ImGui::Button(TR("mtp.back"), ImVec2(100, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ScreenChangeEvent event(Screen_MainMenu);
        EventBus::getInstance().post(event);
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("%s", TR("mtp.log"));
    ImGui::BeginChild("LogScroll", ImVec2(0, 200), true);
    int log_count = mtpLogGetCount();
    for (int i = 0; i < log_count; i++) {
        const char* entry = mtpLogGetEntry(i);
        MtpLogLevel level = mtpLogGetLevel(i);
        ImVec4 color = (level == MTP_LOG_ERROR) ? ImVec4(1,0.3f,0.3f,1) :
                       (level == MTP_LOG_WARNING) ? ImVec4(1,0.8f,0,1) :
                       (level == MTP_LOG_DEBUG) ? ImVec4(0.6f,0.6f,0.6f,1) : ImVec4(1,1,1,1);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(entry);
        ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (ImGui::Button(TR("mtp.clear_log"), ImVec2(100, 30))) {
        mtpLogClear();
    }
}

static void sanitizeFilename(char* name) {
    // Replace characters illegal on FAT32
    for (char* p = name; *p; p++) {
        switch (*p) {
            case ':': case '*': case '?': case '"':
            case '<': case '>': case '|': case '\\':
            case '/':
                *p = '_';
                break;
            default:
                break;
        }
    }
}

static void dumpThreadFunc(void* arg) {
    (void)arg;
    DumpTaskInfo task = g_dump_task;
    g_dump_should_cancel = false;

    char out_dir[] = "sdmc:/switch/Javelin/backups";
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/Javelin", 0777);
    mkdir(out_dir, 0777);

    char filepath[512] = {0};
    u64 total_size = 0;
    bool is_gamecard = (task.mode == DumpTaskInfo::GC_XCI || task.mode == DumpTaskInfo::GC_NSP);

    if (is_gamecard) {
        const char* ext = (task.mode == DumpTaskInfo::GC_XCI) ? "xci" : "nsp";
        u64 sz = (task.mode == DumpTaskInfo::GC_XCI) ? g_gc_ctx.layout.total_size : g_gc_ctx.nsp_layout.total_size;
        total_size = sz;

        char safe_name[256];
        strncpy(safe_name, g_gc_ctx.game_name, sizeof(safe_name) - 1);
        safe_name[sizeof(safe_name) - 1] = '\0';
        sanitizeFilename(safe_name);

        if (g_gc_ctx.version_str[0] != '\0') {
            snprintf(filepath, sizeof(filepath), "%s/%s [%016lX][%s].%s",
                     out_dir, safe_name,
                     (unsigned long)g_gc_ctx.title_id,
                     g_gc_ctx.version_str, ext);
        } else {
            snprintf(filepath, sizeof(filepath), "%s/%s [%016lX].%s",
                     out_dir, safe_name,
                     (unsigned long)g_gc_ctx.title_id, ext);
        }
    } else {
        mutexLock(&g_dump_ctx.dump_mutex);
        if (task.game_index >= g_dump_ctx.game_count) {
            mutexUnlock(&g_dump_ctx.dump_mutex);
            g_dump_thread_running = false;
            return;
        }
        DumpGameEntry* game = &g_dump_ctx.games[task.game_index];

        if (task.mode == DumpTaskInfo::MERGED) {
            dumpEnsureMergedLayout(&g_dump_ctx, game);
            total_size = game->merged_layout.total_nsp_size;
            snprintf(filepath, sizeof(filepath), "%s/%s [%016lX].nsp",
                     out_dir, game->game_name, (unsigned long)game->application_id);
        } else {
            if (task.meta_index >= game->content_meta_count) {
                mutexUnlock(&g_dump_ctx.dump_mutex);
                g_dump_thread_running = false;
                return;
            }
            dumpEnsureSeparateLayout(&g_dump_ctx, game, task.meta_index);
            DumpContentMetaEntry* cme = &game->content_metas[task.meta_index];
            total_size = cme->layout.total_nsp_size;
            snprintf(filepath, sizeof(filepath), "%s/%s", out_dir, cme->filename);
        }
        mutexUnlock(&g_dump_ctx.dump_mutex);
    }

    if (total_size == 0) {
        showError(TR("dump.dump_failed"));
        g_dump_thread_running = false;
        return;
    }

    {
        TransferStartEvent evt;
        evt.filePath = filepath;
        evt.totalBytes = total_size;
        evt.cancelledPtr = &g_dump_should_cancel;
        EventBus::getInstance().post(evt);
    }

    // FAT32 split: if >4GB, create a directory and write numbered parts
    const u64 FAT32_MAX = 0xFFFF0000ULL; // ~4GB minus margin for FAT32
    bool needs_split = (total_size > FAT32_MAX);

    FILE* fp = NULL;
    char split_dir[512] = {0};
    char split_path[528] = {0};
    u32 split_index = 0;
    u64 split_written = 0; // bytes written to current split part

    if (needs_split) {
        // Create directory named after the file (e.g. "Game.nsp/")
        strncpy(split_dir, filepath, sizeof(split_dir) - 1);
        mkdir(split_dir, 0777);
        snprintf(split_path, sizeof(split_path), "%s/%02u", split_dir, split_index);
        fp = fopen(split_path, "wb");
    } else {
        fp = fopen(filepath, "wb");
    }

    if (!fp) {
        showError(TR("dump.dump_failed"));
        TransferCompleteEvent evt;
        evt.filePath = filepath;
        evt.success = false;
        evt.errorMessage = TR("dump.error_create_file");
        EventBus::getInstance().post(evt);
        g_dump_thread_running = false;
        return;
    }

    const u64 CHUNK_SIZE = 0x400000; // 4MB
    u8* buf = (u8*)malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(fp);
        g_dump_thread_running = false;
        return;
    }

    u64 offset = 0;
    bool success = true;
    u64 last_progress_tick = 0;
    u64 dump_start_tick = armGetSystemTick();

    while (offset < total_size && !g_dump_should_cancel) {
        u64 remaining = total_size - offset;
        u64 chunk = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        // If splitting, don't exceed the FAT32 limit for this part
        if (needs_split && (split_written + chunk > FAT32_MAX)) {
            chunk = FAT32_MAX - split_written;
            if (chunk == 0) {
                // Roll over to the next split file
                fclose(fp);
                split_index++;
                split_written = 0;
                snprintf(split_path, sizeof(split_path), "%s/%02u", split_dir, split_index);
                fp = fopen(split_path, "wb");
                if (!fp) {
                    success = false;
                    break;
                }
                continue;
            }
        }

        s64 rd = 0;
        if (is_gamecard) {
            u32 handle = (task.mode == DumpTaskInfo::GC_XCI)
                ? MTP_HANDLE_GC_XCI_FILE : MTP_HANDLE_GC_NSP_FILE;
            rd = gcReadObject(&g_gc_ctx, handle, offset, buf, chunk);
        } else {
            DumpNspLayout* layout = NULL;
            mutexLock(&g_dump_ctx.dump_mutex);
            DumpGameEntry* game = &g_dump_ctx.games[task.game_index];
            if (task.mode == DumpTaskInfo::MERGED) {
                layout = &game->merged_layout;
            } else {
                layout = &game->content_metas[task.meta_index].layout;
            }
            mutexUnlock(&g_dump_ctx.dump_mutex);

            rd = dumpReadNspData(&g_dump_ctx, layout, offset, buf, chunk);
        }

        if (rd <= 0) {
            success = false;
            break;
        }

        size_t written = fwrite(buf, 1, (size_t)rd, fp);
        if (written != (size_t)rd) {
            success = false;
            break;
        }

        offset += rd;
        split_written += rd;

        u64 now = armGetSystemTick();
        if (now - last_progress_tick > armNsToTicks(100000000ULL)) {
            float pct = (float)offset / (float)total_size * 100.0f;
            u64 elapsed_ns = armTicksToNs(now - dump_start_tick);
            float elapsed_sec = elapsed_ns / 1000000000.0f;
            float speed = (elapsed_sec > 0.1f) ? (offset / (1024.0f * 1024.0f)) / elapsed_sec : 0.0f;
            TransferProgressEvent evt;
            evt.filePath = filepath;
            evt.bytesTransferred = offset;
            evt.totalBytes = total_size;
            evt.progressPercent = pct;
            evt.speedMBps = speed;
            EventBus::getInstance().post(evt);
            last_progress_tick = now;
        }
    }

    fclose(fp);
    free(buf);

    if (g_dump_should_cancel) {
        if (needs_split) {
            // Remove all split parts and the directory
            for (u32 i = 0; i <= split_index; i++) {
                char rm_path[528];
                snprintf(rm_path, sizeof(rm_path), "%s/%02u", split_dir, i);
                remove(rm_path);
            }
            rmdir(split_dir);
        } else {
            remove(filepath);
        }
        success = false;
    }

    {
        TransferCompleteEvent evt;
        evt.filePath = filepath;
        evt.success = success;
        evt.totalBytes = offset;
        if (!success && !g_dump_should_cancel) {
            evt.errorMessage = TR("dump.error_write");
        }
        EventBus::getInstance().post(evt);
    }

    if (success) {
        showSuccess(TR("dump.dump_complete"));
    } else if (!g_dump_should_cancel) {
        showError(TR("dump.dump_failed"));
    }

    g_dump_thread_running = false;
}

static void startDumpThread(DumpTaskInfo task) {
    if (g_dump_thread_running) return;

    // Join previous thread if needed
    if (g_dump_thread_needs_join) {
        threadWaitForExit(&g_dump_thread);
        threadClose(&g_dump_thread);
        g_dump_thread_needs_join = false;
    }

    g_dump_task = task;
    g_dump_should_cancel = false;
    g_dump_thread_running = true;
    g_dump_thread_needs_join = true;

    Result rc = threadCreate(&g_dump_thread, dumpThreadFunc, NULL, NULL, 0x20000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&g_dump_thread);
    } else {
        g_dump_thread_running = false;
        g_dump_thread_needs_join = false;
        showError(TR("dump.dump_failed"));
    }
}

void renderDumpScreen() {
    const char* dumpTitle = TR("dump.title");
    centerText(dumpTitle);
    ImGui::Text("%s", dumpTitle);
    ImGui::Separator();
    ImGui::Spacing();

    // Initialize dump services on first visit
    if (!g_dump_services_inited) {
        dumpPreInitServices(&g_dump_ctx);
        gcPreInitServices(&g_gc_ctx);
        g_dump_services_inited = true;
    }

    // Refresh each frame
    dumpRefreshIfNeeded(&g_dump_ctx);
    gcRefreshIfNeeded(&g_gc_ctx);

    // Ensure games are enumerated
    mutexLock(&g_dump_ctx.dump_mutex);
    if (!g_dump_ctx.games_enumerated) {
        dumpEnumerateGames(&g_dump_ctx);
    }
    mutexUnlock(&g_dump_ctx.dump_mutex);

    static int activeTab = 0; // 0 = installed, 1 = gamecard
    static int selectedGame = 0;
    static bool showDumpModal = false;
    static int dumpModalGame = -1;

    // Tab bar
    {
        ImGui::PushStyleColor(ImGuiCol_Button, (activeTab == 0)
            ? ImVec4(0.22f, 0.36f, 0.65f, 1.0f) : ImVec4(0.14f, 0.14f, 0.20f, 1.0f));
        if (ImGui::Button(TR("dump.tab_installed"), ImVec2(200, 36))) {
            activeTab = 0;
            selectedGame = 0;
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, (activeTab == 1)
            ? ImVec4(0.22f, 0.36f, 0.65f, 1.0f) : ImVec4(0.14f, 0.14f, 0.20f, 1.0f));
        if (ImGui::Button(TR("dump.tab_gamecard"), ImVec2(200, 36))) {
            activeTab = 1;
        }
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    if (activeTab == 0) {
        // --- Installed games tab ---
        u32 game_count = g_dump_ctx.game_count;

        // Lazily compute merged layout for a couple games per frame (for size display)
        {
            static u32 layout_scan_idx = 0;
            if (layout_scan_idx >= game_count) layout_scan_idx = 0;
            int computed_this_frame = 0;
            for (u32 i = layout_scan_idx; i < game_count && computed_this_frame < 2; i++) {
                DumpGameEntry* game = &g_dump_ctx.games[i];
                if (!game->merged_layout.computed) {
                    mutexLock(&g_dump_ctx.dump_mutex);
                    dumpEnsureMergedLayout(&g_dump_ctx, game);
                    mutexUnlock(&g_dump_ctx.dump_mutex);
                    computed_this_frame++;
                }
                layout_scan_idx = i + 1;
            }
        }

        if (game_count == 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", TR("dump.no_games"));
        } else {
            float listHeight = 380.0f;
            ImGui::BeginChild("GameList", ImVec2(0, listHeight),
                               ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

            for (u32 i = 0; i < game_count; i++) {
                DumpGameEntry* game = &g_dump_ctx.games[i];

                char label[512];
                const char* loc = game->is_on_sd ? TR("dump.game_location_sd") : TR("dump.game_location_nand");

                if (game->merged_layout.computed && game->merged_layout.total_nsp_size > 0) {
                    float size_mb = (float)game->merged_layout.total_nsp_size / (1024.0f * 1024.0f);
                    if (size_mb >= 1024.0f) {
                        snprintf(label, sizeof(label), "%s  [%s]  (%u content(s))  %.2f GB##game%u",
                                 game->game_name, loc, game->content_meta_count,
                                 size_mb / 1024.0f, i);
                    } else {
                        snprintf(label, sizeof(label), "%s  [%s]  (%u content(s))  %.0f MB##game%u",
                                 game->game_name, loc, game->content_meta_count,
                                 size_mb, i);
                    }
                } else {
                    snprintf(label, sizeof(label), "%s  [%s]  (%u content(s))##game%u",
                             game->game_name, loc, game->content_meta_count, i);
                }

                bool isSelected = ((int)i == selectedGame);

                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedGame = (int)i;
                    if (!g_dump_thread_running) {
                        showDumpModal = true;
                        dumpModalGame = (int)i;
                    }
                }
                if (ImGui::IsItemFocused()) {
                    selectedGame = (int)i;
                }

                // Show TitleID on same line
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140);
                ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "%016lX",
                                   (unsigned long)game->application_id);
            }

            ImGui::EndChild();

            if (g_dump_thread_running) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", TR("dump.dumping"));
            }
        }

        // Dump mode selection modal
        if (showDumpModal && dumpModalGame >= 0 && dumpModalGame < (int)g_dump_ctx.game_count) {
            ImGui::OpenPopup("##DumpModePopup");
            showDumpModal = false;
        }

        if (ImGui::BeginPopup("##DumpModePopup")) {
            DumpGameEntry* game = &g_dump_ctx.games[dumpModalGame];
            ImGui::Text("%s", game->game_name);
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Selectable(TR("dump.dump_merged"))) {
                DumpTaskInfo task;
                task.mode = DumpTaskInfo::MERGED;
                task.game_index = dumpModalGame;
                task.meta_index = 0;
                startDumpThread(task);
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::Selectable(TR("dump.dump_separate"))) {
                DumpTaskInfo task;
                task.mode = DumpTaskInfo::SEPARATE;
                task.game_index = dumpModalGame;
                task.meta_index = 0;
                startDumpThread(task);
                ImGui::CloseCurrentPopup();
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Selectable(TR("modal.cancel"))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    } else {
        // --- Gamecard tab ---
        static bool showGcDumpModal = false;

        if (!g_gc_ctx.card_inserted) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", TR("dump.no_gamecard"));
        } else {
            char gcLabel[512];
            snprintf(gcLabel, sizeof(gcLabel), "%s", g_gc_ctx.game_name);

            ImGui::BeginChild("GamecardList", ImVec2(0, 120),
                               ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

            if (ImGui::Selectable(gcLabel, true)) {
                if (!g_dump_thread_running) {
                    showGcDumpModal = true;
                }
            }

            // Info below the selectable
            if (g_gc_ctx.title_id != 0) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140);
                ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.5f, 1.0f), "%016lX",
                                   (unsigned long)g_gc_ctx.title_id);
            }

            if (g_gc_ctx.version_str[0] != '\0') {
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.4f, 1.0f), "  %s", g_gc_ctx.version_str);
            }

            if (g_gc_ctx.layout.computed) {
                ImGui::Text("  XCI: %.2f GB", (float)g_gc_ctx.layout.total_size / (1024.0f * 1024.0f * 1024.0f));
            }
            if (g_gc_ctx.nsp_layout.computed) {
                ImGui::SameLine();
                ImGui::Text("  NSP: %.2f GB", (float)g_gc_ctx.nsp_layout.total_size / (1024.0f * 1024.0f * 1024.0f));
            }

            ImGui::EndChild();

            if (g_dump_thread_running) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", TR("dump.dumping"));
            }
        }

        // Gamecard dump mode popup
        if (showGcDumpModal) {
            ImGui::OpenPopup("##GcDumpModePopup");
            showGcDumpModal = false;
        }

        if (ImGui::BeginPopup("##GcDumpModePopup")) {
            ImGui::Text("%s", g_gc_ctx.game_name);
            ImGui::Separator();
            ImGui::Spacing();

            if (g_gc_ctx.layout.computed) {
                if (ImGui::Selectable(TR("dump.dump_xci"))) {
                    DumpTaskInfo task;
                    task.mode = DumpTaskInfo::GC_XCI;
                    task.game_index = 0;
                    task.meta_index = 0;
                    startDumpThread(task);
                    ImGui::CloseCurrentPopup();
                }
            }

            if (g_gc_ctx.nsp_layout.computed) {
                if (ImGui::Selectable(TR("dump.dump_nsp"))) {
                    DumpTaskInfo task;
                    task.mode = DumpTaskInfo::GC_NSP;
                    task.game_index = 0;
                    task.meta_index = 0;
                    startDumpThread(task);
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Selectable(TR("modal.cancel"))) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(TR("dump.back"), ImVec2(100, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ScreenChangeEvent event(Screen_MainMenu);
        EventBus::getInstance().post(event);
        selectedGame = 0;
    }
}

void renderInstallScreen() {
    const char* installTitle = TR("install.title");
    centerText(installTitle);
    ImGui::Text("%s", installTitle);
    ImGui::Separator();
    ImGui::Spacing();

    // Scan on first visit
    if (!g_install_files_scanned) {
        scanBackupsFolder();
    }

    // Show free space
    {
        struct statvfs st;
        if (statvfs("sdmc:/", &st) == 0) {
            float free_gb = (float)(st.f_bfree * st.f_frsize) / (1024.0f * 1024.0f * 1024.0f);
            ImGui::Text(TR("install.sd_space"), free_gb);
        }
    }
    ImGui::Spacing();

    // Refresh button
    if (ImGui::Button(TR("install.refresh"), ImVec2(120, 36))) {
        g_install_files_scanned = false;
        scanBackupsFolder();
    }

    ImGui::Spacing();

    static int selectedFile = -1;
    static bool showInstallModal = false;
    static int installModalFile = -1;

    if (g_install_file_count == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", TR("install.no_files"));
    } else {
        float listHeight = 380.0f;
        ImGui::BeginChild("InstallFileList", ImVec2(0, listHeight),
                           ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

        for (u32 i = 0; i < g_install_file_count; i++) {
            InstallFileEntry* entry = &g_install_files[i];

            char label[512];
            const char* type = entry->is_xci ? "XCI" : "NSP";
            float size_mb = (float)entry->file_size / (1024.0f * 1024.0f);

            if (size_mb >= 1024.0f) {
                snprintf(label, sizeof(label), "%s  [%s]  (%.2f GB)##file%u",
                         entry->filename, type, size_mb / 1024.0f, i);
            } else {
                snprintf(label, sizeof(label), "%s  [%s]  (%.1f MB)##file%u",
                         entry->filename, type, size_mb, i);
            }

            bool isSelected = ((int)i == selectedFile);

            if (ImGui::Selectable(label, isSelected)) {
                selectedFile = (int)i;
                if (!g_install_thread_running) {
                    showInstallModal = true;
                    installModalFile = (int)i;
                }
            }
            if (ImGui::IsItemFocused()) {
                selectedFile = (int)i;
            }
        }

        ImGui::EndChild();

        if (g_install_thread_running) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", TR("install.installing"));
        }
    }

    // Install target selection modal
    if (showInstallModal && installModalFile >= 0 && installModalFile < (int)g_install_file_count) {
        ImGui::OpenPopup("##InstallTargetPopup");
        showInstallModal = false;
    }

    if (ImGui::BeginPopup("##InstallTargetPopup")) {
        InstallFileEntry* entry = &g_install_files[installModalFile];
        ImGui::Text("%s", entry->filename);
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Selectable(TR("install.install_sd"))) {
            InstallTaskInfo task;
            strncpy(task.filepath, entry->fullpath, sizeof(task.filepath) - 1);
            task.filepath[sizeof(task.filepath) - 1] = '\0';
            strncpy(task.filename, entry->filename, sizeof(task.filename) - 1);
            task.filename[sizeof(task.filename) - 1] = '\0';
            task.is_xci = entry->is_xci;
            task.target = INSTALL_TARGET_SD;
            startInstallThread(task);
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Selectable(TR("install.install_nand"))) {
            InstallTaskInfo task;
            strncpy(task.filepath, entry->fullpath, sizeof(task.filepath) - 1);
            task.filepath[sizeof(task.filepath) - 1] = '\0';
            strncpy(task.filename, entry->filename, sizeof(task.filename) - 1);
            task.filename[sizeof(task.filename) - 1] = '\0';
            task.is_xci = entry->is_xci;
            task.target = INSTALL_TARGET_NAND;
            startInstallThread(task);
            ImGui::CloseCurrentPopup();
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Selectable(TR("modal.cancel"))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(TR("install.back"), ImVec2(100, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ScreenChangeEvent event(Screen_MainMenu);
        EventBus::getInstance().post(event);
        selectedFile = -1;
    }
}

void renderSettingsScreen() {
    static bool s_first_frame = true;
    static int s_mtp_buffer_index = 1;
    static const char* s_buffer_size_labels[] = {
        "256 KB", "512 KB", "1 MB", "2 MB", "4 MB", "8 MB", "16 MB"
    };
    static u32 s_buffer_sizes[] = {
        256 * 1024, 512 * 1024, 1 * 1024 * 1024, 2 * 1024 * 1024,
        4 * 1024 * 1024, 8 * 1024 * 1024, 16 * 1024 * 1024
    };

    if (s_first_frame) {
        const Settings* settings = settingsGet();
        u32 current_size = settings->mtp_buffer_size;
        for (int i = 0; i < 7; i++) {
            if (s_buffer_sizes[i] == current_size) {
                s_mtp_buffer_index = i;
                break;
            }
        }
        s_first_frame = false;
    }

    const char* settingsTitle = TR("settings.title");
    centerText(settingsTitle);
    ImGui::Text("%s", settingsTitle);
    ImGui::Separator();
    ImGui::Spacing();

    // Language Section
    auto availableLanguages = Localization::getInstance().getAvailableLanguages();
    const char* currentLangCode = Localization::getInstance().getLanguage();

    ImGui::Text("%s:", TR("settings.language"));
    ImGui::SameLine();
    const char* currentLangName = Localization::getInstance().getLanguageName(currentLangCode);
    ImGui::TextColored(ImVec4(0.3f, 0.7f, 0.9f, 1.0f), "%s", currentLangName);
    ImGui::Spacing();

    float listWidth = 380.0f;
    float listHeight = 180.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

    ImGui::BeginChild("LanguageList", ImVec2(listWidth, listHeight),
                       ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

    for (size_t i = 0; i < availableLanguages.size(); i++) {
        bool isCurrent = (strcmp(availableLanguages[i].code, currentLangCode) == 0);

        if (isCurrent) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.55f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        }

        char langLabel[128];
        snprintf(langLabel, sizeof(langLabel), "%s##lang%zu", availableLanguages[i].name, i);

        if (ImGui::Selectable(langLabel, isCurrent)) {
            Localization::getInstance().setLanguage(availableLanguages[i].code);
            settingsSetLanguage(availableLanguages[i].code);
            settingsSave();
        }

        if (isCurrent) {
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PopStyleColor(1);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::Spacing();

    ImGui::Separator();
    ImGui::Spacing();

    // MTP Buffer Size Section
    ImGui::Text("%s", TR("settings.mtp_buffer"));
    ImGui::Spacing();

    ImGui::PushItemWidth(listWidth);
    if (ImGui::Combo("##mtp_buffer", &s_mtp_buffer_index, s_buffer_size_labels, 7)) {
        settingsSetMtpBufferSize(s_buffer_sizes[s_mtp_buffer_index]);
        settingsSave();
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::TextDisabled("(%s)", TR("settings.mtp_buffer_desc"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(TR("settings.back"), ImVec2(100, 40)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        // Reset first frame flag for next time we enter settings
        s_first_frame = true;
        ScreenChangeEvent event(Screen_MainMenu);
        EventBus::getInstance().post(event);
    }
}

static bool* g_mtp_thread_should_stop = nullptr;
static void mtpThreadFunc(void* arg) {
    MtpProtocolContext* ctx = (MtpProtocolContext*)arg;

    while (!*g_mtp_thread_should_stop) {
        mtpProtocolProcess(ctx);
        // Removed 1ms sleep - let USB completion events drive pacing
        // This provides ~3-5x throughput improvement for large file transfers
    }
}

int main(int argc, char* argv[]) {
#if DEBUG
    socketInitializeDefault();
    nxlinkStdio();
    DBG_PRINT("Javelin starting in DEBUG mode");
#endif

    mtpLogInit();
    Result rc = romfsInit();

    if (!initEgl()) {
        romfsExit();
        return 1;
    }

    gladLoadGL();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.MouseDrawCursor = false;
    ImGui::StyleColorsDark();
    applyJavelinTheme();
    ImGui_ImplSwitch_Init();
    ImGui_ImplOpenGL3_Init("#version 430 core");

    GuiManager::getInstance().initialize();

    settingsInit();
    Localization::getInstance().initialize();

    const Settings* settings = settingsGet();
    Localization::getInstance().setLanguage(settings->language);

    bool usb_initialized = false;
    bool mtp_running = false;
    char status_msg[256];
    strncpy(status_msg, TR("mtp.status_initial"), sizeof(status_msg) - 1);
    MtpProtocolContext mtp_ctx = {0};
    TicketBrowserState ticketState;
    ticketBrowserInit(&ticketState);

    Thread mtp_thread;
    bool mtp_thread_running = false;
    bool mtp_thread_should_stop = false;

    u64 lastTime = armGetSystemTick();
    bool sleep_locked = false;

    float virtualMouseX = 640.0f;
    float virtualMouseY = 360.0f;
    bool virtualMouseActive = false;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        u64 currentTime = armGetSystemTick();
        float deltaTime = (currentTime - lastTime) / 1000000.0f;
        lastTime = currentTime;

        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        u64 kHeld = padGetButtons(&pad);

        HidAnalogStickState stickL = padGetStickPos(&pad, 0);
        HidAnalogStickState stickR = padGetStickPos(&pad, 1);

        if (kDown & HidNpadButton_Plus) {
            break;
        }

        static HidTouchScreenState touchState = {0};
        static bool touchWasDown = false;
        hidGetTouchScreenStates(&touchState, 1);
        bool touchActive = touchState.count > 0;

        static bool leftStickWasActive = false;
        bool leftStickActive = (abs(stickL.x) > 2000 || abs(stickL.y) > 2000);

        if (leftStickActive) {
            virtualMouseActive = true;
            float speed = 14.0f;
            virtualMouseX += stickL.x / 32768.0f * speed;
            virtualMouseY -= stickL.y / 32768.0f * speed;

            virtualMouseX = fmaxf(0.0f, fminf(1280.0f, virtualMouseX));
            virtualMouseY = fmaxf(0.0f, fminf(720.0f, virtualMouseY));

            io.AddMousePosEvent(virtualMouseX, virtualMouseY);
            leftStickWasActive = true;
        } else {
            leftStickWasActive = false;
        }

        // Right stick acts as dpad/scroll navigation
        float rsX = stickR.x / 32768.0f;
        float rsY = stickR.y / 32768.0f;
        float rDeadzone = 0.15f;
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  rsX < -rDeadzone, (rsX < -rDeadzone) ? -rsX : 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, rsX >  rDeadzone, (rsX >  rDeadzone) ?  rsX : 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    rsY >  rDeadzone, (rsY >  rDeadzone) ?  rsY : 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  rsY < -rDeadzone, (rsY < -rDeadzone) ? -rsY : 0.0f);

        bool rightStickActive = (abs(stickR.x) > 2000 || abs(stickR.y) > 2000);
        if (rightStickActive ||
            (kHeld & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right)) != 0) {
            virtualMouseActive = false;
        }

        if (virtualMouseActive || leftStickWasActive) {
            static bool zlWasDown = false;
            static bool zrWasDown = false;
            static bool aMouseWasDown = false;
            static bool bMouseWasDown = false;
            bool zlDown = kHeld & HidNpadButton_ZL;
            bool zrDown = kHeld & HidNpadButton_ZR;
            bool aDown = kHeld & HidNpadButton_A;
            bool bDown = kHeld & HidNpadButton_B;

            // ZR or A = left click
            bool leftClick = zrDown || aDown;
            bool leftWas = zrWasDown || aMouseWasDown;
            if (leftClick && !leftWas) {
                io.AddMouseButtonEvent(0, true);
            } else if (!leftClick && leftWas) {
                io.AddMouseButtonEvent(0, false);
            }

            // ZL or B = right click
            bool rightClick = zlDown || bDown;
            bool rightWas = zlWasDown || bMouseWasDown;
            if (rightClick && !rightWas) {
                io.AddMouseButtonEvent(1, true);
            } else if (!rightClick && rightWas) {
                io.AddMouseButtonEvent(1, false);
            }

            zlWasDown = zlDown;
            zrWasDown = zrDown;
            aMouseWasDown = aDown;
            bMouseWasDown = bDown;
        }

        if (touchActive) {
            virtualMouseActive = false;
            float touchX = touchState.touches[0].x;
            float touchY = touchState.touches[0].y;
            virtualMouseX = touchX;
            virtualMouseY = touchY;
            io.AddMousePosEvent(touchX, touchY);

            if (!touchWasDown) {
                io.AddMouseButtonEvent(0, true);
                touchWasDown = true;
            }
        } else {
            if (touchWasDown) {
                io.AddMouseButtonEvent(0, false);
                touchWasDown = false;
            }
        }

        io.NavActive = true;
        io.NavVisible = true;

        io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (kHeld & HidNpadButton_Up) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (kHeld & HidNpadButton_Down) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (kHeld & HidNpadButton_Left) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (kHeld & HidNpadButton_Right) != 0);

        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (kHeld & HidNpadButton_X) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (kHeld & HidNpadButton_Y) != 0);

        if (!virtualMouseActive && !leftStickWasActive) {
            io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (kHeld & HidNpadButton_A) != 0);
        } else {
            io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
        }

        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (kHeld & HidNpadButton_B) != 0);

        io.AddKeyEvent(ImGuiKey_GamepadL1, (kHeld & HidNpadButton_L) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadR1, (kHeld & HidNpadButton_R) != 0);

        ImGui_ImplSwitch_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        GuiManager::getInstance().updateNotifications(deltaTime);

        int currentScreen = GuiManager::getInstance().getCurrentScreen();
        static int prevScreen = -1;
        bool screenChanged = (currentScreen != prevScreen);
        prevScreen = currentScreen;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1280, 720));

        ImGui::Begin("Javelin", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        // Auto-focus the first widget when switching screens
        if (screenChanged) {
            ImGui::SetKeyboardFocusHere(0);
        }

        switch (currentScreen) {
            case Screen_MainMenu:
                renderMainMenu(mtp_running);
                break;
            case Screen_MTP:
                renderMTPScreen(mtp_ctx, usb_initialized, mtp_running, status_msg);
                break;
            case Screen_Tickets:
                renderTicketScreen(&ticketState);
                break;
            case Screen_Settings:
                renderSettingsScreen();
                break;
            case Screen_Dump:
                renderDumpScreen();
                break;
            case Screen_Install:
                renderInstallScreen();
                break;
            default:
                renderMainMenu(mtp_running);
                break;
        }

        ImGui::End();

        GuiManager::getInstance().renderStatusBar();
        GuiManager::getInstance().renderNotifications();

        GuiManager::getInstance().renderModals();

        if (mtp_running) {
            savesRefreshIfNeeded(&mtp_ctx.saves);
            dumpRefreshIfNeeded(&mtp_ctx.dump);
            gcRefreshIfNeeded(&mtp_ctx.gamecard);
        }

        // Prevent auto-sleep when MTP, dump, or install is active
        {
            bool need_wake = mtp_running || g_dump_thread_running || g_install_thread_running;
            if (need_wake && !sleep_locked) {
                appletSetMediaPlaybackState(true);
                sleep_locked = true;
            } else if (!need_wake && sleep_locked) {
                appletSetMediaPlaybackState(false);
                sleep_locked = false;
            }
        }

        if (mtp_running && !mtp_thread_running) {
            savesPreInitServices(&mtp_ctx.saves);
            dumpPreInitServices(&mtp_ctx.dump);
            gcPreInitServices(&mtp_ctx.gamecard);

            g_mtp_thread_should_stop = &mtp_thread_should_stop;
            mtp_thread_should_stop = false;
            Result rc = threadCreate(&mtp_thread, mtpThreadFunc, &mtp_ctx, NULL, 0x20000, 0x2C, -2);
            if (R_SUCCEEDED(rc)) {
                threadStart(&mtp_thread);
                mtp_thread_running = true;
            }
        } else if (!mtp_running && mtp_thread_running) {
            mtp_thread_should_stop = true;
            threadWaitForExit(&mtp_thread);
            threadClose(&mtp_thread);
            mtp_thread_running = false;

            mtpProtocolExit(&mtp_ctx);
            usbMtpExit();
            usb_initialized = false;
            snprintf(status_msg, 256, "%s", TR("mtp.status_stopped"));
            showInfo(TR("mtp.stopped"));
        }

        if (virtualMouseActive || leftStickWasActive) {
            ImDrawList* drawList = ImGui::GetForegroundDrawList();
            ImVec2 cursorPos(virtualMouseX, virtualMouseY);
            drawList->AddCircleFilled(cursorPos, 8.0f, IM_COL32(255, 255, 255, 200));
            drawList->AddCircle(cursorPos, 8.0f, IM_COL32(0, 0, 0, 255), 0, 2.0f);
            drawList->AddCircleFilled(cursorPos, 2.0f, IM_COL32(0, 150, 255, 255));
        }

        ImGui::Render();
        glViewport(0, 0, 1280, 720);
        glClearColor(0.06f, 0.06f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(s_display, s_surface);

        if (ImGui_ImplSwitch_ShouldClose()) break;
    }

    // Release sleep lock
    if (sleep_locked) {
        appletSetMediaPlaybackState(false);
        sleep_locked = false;
    }

    // Clean up install thread if running or needs join
    if (g_install_thread_needs_join) {
        threadWaitForExit(&g_install_thread);
        threadClose(&g_install_thread);
        g_install_thread_running = false;
        g_install_thread_needs_join = false;
    }

    // Clean up dump thread if running or needs join
    if (g_dump_thread_running) {
        g_dump_should_cancel = true;
    }
    if (g_dump_thread_needs_join) {
        threadWaitForExit(&g_dump_thread);
        threadClose(&g_dump_thread);
        g_dump_thread_running = false;
        g_dump_thread_needs_join = false;
    }

    // Clean up dump subsystem contexts
    if (g_dump_services_inited) {
        dumpExit(&g_dump_ctx);
        gcExit(&g_gc_ctx);
    }

    ticketBrowserExit(&ticketState);

    if (mtp_thread_running) {
        usbMtpResetEndpoints();
        mtp_thread_should_stop = true;

        u64 timeout_start = armGetSystemTick();
        const u64 timeout_ticks = armNsToTicks(3000000000ULL);
        bool thread_exited = false;

        while (!thread_exited) {
            if (threadWaitForExit(&mtp_thread) == 0) {
                thread_exited = true;
            } else {
                u64 elapsed = armGetSystemTick() - timeout_start;
                if (elapsed > timeout_ticks) break;
                svcSleepThread(10000000ULL);
            }
        }

        threadClose(&mtp_thread);
        mtp_thread_running = false;
    }

    if (mtp_running) {
        mtpProtocolExit(&mtp_ctx);
        mtp_running = false;
    }

    if (usb_initialized) {
        usbMtpExit();
        usb_initialized = false;
    }

    svcSleepThread(100000000ULL);
    glFinish();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSwitch_Shutdown();
    ImGui::DestroyContext();
    deinitEgl();
    romfsExit();
    socketExit();

    return 0;
}
