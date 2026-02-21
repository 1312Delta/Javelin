// SPDX-FileCopyrightText: 2026 1312delta
// SPDX-License-Identifier: MIT
//
#include "tickets/ticket_browser.h"
#include "i18n/Localization.h"
#include "mtp_log.h"
extern "C" {
#include "ipcext/es.h"
#include "service/es.h"
#include <switch/services/ns.h>
#include <switch/crypto/aes.h>
}
#include "core/GuiEvents.h"
#include "core/GuiManager.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <strings.h>
#include <algorithm>

namespace Javelin {

// -----------------------------------------------------------------------
// Key loading helpers (adapted from gamecard_dump.cpp)
// -----------------------------------------------------------------------

static void trim_whitespace(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' '  || s[len-1] == '\t')) {
        s[--len] = '\0';
    }
    char* start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static u32 hex_to_bytes(const char* hex, u8* out, u32 max_len) {
    u32 hex_len = strlen(hex);
    if (hex_len % 2 != 0) return 0;
    u32 byte_len = hex_len / 2;
    if (byte_len > max_len) byte_len = max_len;
    for (u32 i = 0; i < byte_len; i++) {
        char b[3] = {hex[i*2], hex[i*2+1], '\0'};
        char* end = NULL;
        out[i] = (u8)strtoul(b, &end, 16);
        if (end != b + 2) return 0;
    }
    return byte_len;
}

static void bytes_to_hex(const u8* data, u32 len, char* out) {
    for (u32 i = 0; i < len; i++) {
        snprintf(out + i * 2, 3, "%02X", data[i]);
    }
}

static bool load_titlekek(u8 key_generation, u8* out_key) {
    FILE* fp = fopen("/switch/prod.keys", "r");
    if (!fp) return false;

    char target[32];
    snprintf(target, sizeof(target), "titlekek_%02x", key_generation);

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* name = line;
        char* value = eq + 1;
        trim_whitespace(name);
        trim_whitespace(value);

        if (strcasecmp(name, target) == 0) {
            u32 len = hex_to_bytes(value, out_key, 0x10);
            found = (len == 0x10);
            break;
        }
    }

    fclose(fp);
    return found;
}

// -----------------------------------------------------------------------
// Ticket blob parsing
// -----------------------------------------------------------------------

static u32 getSignatureSize(u32 sig_type) {
    switch (sig_type) {
        case 0x010000: return 0x200; // RSA-4096 SHA-1 (signature only, padding handled separately)
        case 0x010001: return 0x200; // RSA-4096 SHA-256
        case 0x010003: return 0x100; // RSA-2048 SHA-256
        case 0x010004: return 0x3C;  // ECDSA SHA-1
        case 0x010005: return 0x3C;  // ECDSA SHA-256
        default:       return 0;
    }
}

static const char* getSignatureName(u32 sig_type) {
    switch (sig_type) {
        case 0x010000: return "RSA-4096 + SHA-1";
        case 0x010001: return "RSA-4096 + SHA-256";
        case 0x010002: return "RSA-2048 + SHA-1";
        case 0x010003: return "RSA-2048 + SHA-256";
        case 0x010004: return "ECDSA + SHA-1";
        case 0x010005: return "ECDSA + SHA-256";
        default:       return "Unknown";
    }
}

static const char* getLicenseTypeName(u8 license_type) {
    switch (license_type) {
        case 0: return TR("tickets.license_permanent");
        case 1: return TR("tickets.license_demo");
        case 2: return TR("tickets.license_trial");
        case 3: return TR("tickets.license_rental");
        case 4: return TR("tickets.license_subscription");
        case 5: return TR("tickets.license_service");
        default: {
            static char unknown_buf[32];
            snprintf(unknown_buf, sizeof(unknown_buf), "Unknown (0x%02X)", license_type);
            return unknown_buf;
        }
    }
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static void formatRightsId(const EsRightsId* id, char* out) {
    const u8* bytes = id->fs_id.c;
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02X", bytes[i]);
    }
}

static bool tryResolveName(u64 id, NsApplicationControlData* ctrl, char* outName, size_t outSize) {
    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, ctrl, sizeof(*ctrl), &actual);
    if (R_SUCCEEDED(rc) && actual >= sizeof(ctrl->nacp)) {
        for (int k = 0; k < 16; k++) {
            if (ctrl->nacp.lang[k].name[0]) {
                strncpy(outName, ctrl->nacp.lang[k].name, outSize - 1);
                outName[outSize - 1] = '\0';
                return true;
            }
        }
    }
    return false;
}

static void resolveTicketName(u64 titleId, char* outName, size_t outSize) {
    NsApplicationControlData* ctrl = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (ctrl) {
        // Try exact title ID first
        if (tryResolveName(titleId, ctrl, outName, outSize)) {
            free(ctrl);
            return;
        }
        // Try base application ID (mask off lower 13 bits for DLC/update)
        u64 baseId = titleId & 0xFFFFFFFFFFFFE000ULL;
        if (baseId != titleId && tryResolveName(baseId, ctrl, outName, outSize)) {
            free(ctrl);
            return;
        }
        free(ctrl);
    }
    snprintf(outName, outSize, "%016llX", (unsigned long long)titleId);
}

// -----------------------------------------------------------------------
// Fetch ticket detail
// -----------------------------------------------------------------------

static bool fetchTicketDetail(TicketBrowserState* state, int index) {
    if (index < 0 || index >= (int)state->tickets.size()) return false;

    TicketDetail& detail = state->detail;
    memset(&detail, 0, sizeof(detail));

    const TicketEntry& entry = state->tickets[index];
    detail.is_common = !entry.isPersonalized;

    if (detail.is_common) {
        Result rc = esInitialize();
        if (R_FAILED(rc)) return false;

        u8* tikBuf = (u8*)malloc(SIGNED_TIK_MAX_SIZE);
        if (!tikBuf) {
            esExit();
            return false;
        }
        memset(tikBuf, 0, SIGNED_TIK_MAX_SIZE);

        u64 out_size = 0;
        rc = esGetCommonTicketData(&out_size, &entry.rightsId, tikBuf, SIGNED_TIK_MAX_SIZE);
        if (R_SUCCEEDED(rc) && out_size >= sizeof(TikCommonBlock)) {
            // Check if data starts with signature type or issuer
            LOG_DEBUG("Ticket Browser: out_size=0x%lX", out_size);
            LOG_DEBUG("Ticket Browser: Bytes at 0x00: %02X %02X %02X %02X %02X %02X %02X %02X",
                     tikBuf[0], tikBuf[1], tikBuf[2], tikBuf[3], tikBuf[4], tikBuf[5], tikBuf[6], tikBuf[7]);
            LOG_DEBUG("Ticket Browser: Bytes at 0x40: %02X %02X %02X %02X %02X %02X %02X %02X",
                     tikBuf[0x40], tikBuf[0x41], tikBuf[0x42], tikBuf[0x43], tikBuf[0x44], tikBuf[0x45], tikBuf[0x46], tikBuf[0x47]);
            LOG_DEBUG("Ticket Browser: Bytes at 0x80: %02X %02X %02X %02X %02X %02X %02X %02X",
                     tikBuf[0x80], tikBuf[0x81], tikBuf[0x82], tikBuf[0x83], tikBuf[0x84], tikBuf[0x85], tikBuf[0x86], tikBuf[0x87]);
            LOG_DEBUG("Ticket Browser: Bytes at 0x140: %02X %02X %02X %02X %02X %02X %02X %02X",
                     tikBuf[0x140], tikBuf[0x141], tikBuf[0x142], tikBuf[0x143], tikBuf[0x144], tikBuf[0x145], tikBuf[0x146], tikBuf[0x147]);
            LOG_DEBUG("Ticket Browser: Bytes at 0x240: %02X %02X %02X %02X %02X %02X %02X %02X",
                     tikBuf[0x240], tikBuf[0x241], tikBuf[0x242], tikBuf[0x243], tikBuf[0x244], tikBuf[0x245], tikBuf[0x246], tikBuf[0x247]);

            // The ticket body starts at offset 0x140 (after signature + padding)
            detail.signature_type = 0x00010004;  // ECDSA (from debug output)
            u32 body_offset = 0x140;

            // Need at least 0x180 bytes in the body to read all fields including account_id at 0x174
            if (body_offset + 0x180 <= out_size) {
                // Parse ticket manually to avoid struct packing issues
                const u8* body = tikBuf + body_offset;

                // 0x00: issuer (64 bytes)
                memcpy(detail.block.issuer, body + 0x00, 0x40);
                detail.block.issuer[0x3F] = '\0';

                // 0x40: titlekey_block (256 bytes)
                memcpy(detail.block.titlekey_block, body + 0x40, 0x100);

                // 0x140: format_version (1 byte)
                detail.block.format_version = body[0x140];

                // 0x141: titlekey_type (1 byte)
                detail.block.titlekey_type = body[0x141];

                // 0x142: ticket_version (2 bytes, big endian)
                detail.block.ticket_version = (body[0x142] << 8) | body[0x143];

                // 0x144: license_type (1 byte)
                detail.block.license_type = body[0x144];
                LOG_DEBUG("Ticket Browser: Raw license_type byte at 0x144 = %u (0x%02X)",
                         detail.block.license_type, detail.block.license_type);

                // 0x145: key_generation (1 byte)
                detail.block.key_generation = body[0x145];

                // 0x146: property_mask (2 bytes, big endian)
                detail.block.property_mask = (body[0x146] << 8) | body[0x147];

                // 0x148: reserved (8 bytes)
                memcpy(detail.block.reserved, body + 0x148, 8);

                // Debug: Check bytes at various offsets to find where ticket_id might be
                LOG_DEBUG("Ticket Browser: Bytes at body+0x10 (alt ticket_id?): %02X %02X %02X %02X %02X %02X %02X %02X",
                         body[0x10], body[0x11], body[0x12], body[0x13], body[0x14], body[0x15], body[0x16], body[0x17]);
                LOG_DEBUG("Ticket Browser: Bytes at body+0x150 (ticket_id): %02X %02X %02X %02X %02X %02X %02X %02X",
                         body[0x150], body[0x151], body[0x152], body[0x153], body[0x154], body[0x155], body[0x156], body[0x157]);
                LOG_DEBUG("Ticket Browser: Bytes at body+0x158 (device_id): %02X %02X %02X %02X %02X %02X %02X %02X",
                         body[0x158], body[0x159], body[0x15A], body[0x15B], body[0x15C], body[0x15D], body[0x15E], body[0x15F]);
                LOG_DEBUG("Ticket Browser: Bytes at body+0x160 (rights_id): %02X %02X %02X %02X %02X %02X %02X %02X",
                         body[0x160], body[0x161], body[0x162], body[0x163], body[0x164], body[0x165], body[0x166], body[0x167]);
                LOG_DEBUG("Ticket Browser: Bytes at body+0x170 (account_id): %02X %02X %02X %02X",
                         body[0x170], body[0x171], body[0x172], body[0x173]);

                // 0x150: ticket_id (8 bytes, big endian)
                detail.block.ticket_id = 0;
                for (int i = 0; i < 8; i++) {
                    detail.block.ticket_id = (detail.block.ticket_id << 8) | body[0x150 + i];
                }

                // 0x158: device_id (8 bytes, big endian)
                detail.block.device_id = 0;
                for (int i = 0; i < 8; i++) {
                    detail.block.device_id = (detail.block.device_id << 8) | body[0x158 + i];
                }

                // 0x160: rights_id (16 bytes)
                memcpy(detail.block.rights_id, body + 0x160, 0x10);

                // 0x170: account_id (4 bytes, big endian)
                detail.block.account_id = (body[0x170] << 24) | (body[0x171] << 16) |
                                         (body[0x172] << 8) | body[0x173];

                LOG_DEBUG("Ticket Browser: Parsed ticket_id=0x%016lX, device_id=0x%016lX, account_id=0x%08X",
                         detail.block.ticket_id, detail.block.device_id, detail.block.account_id);

                // Debug: Log ticket structure values
                LOG_DEBUG("Ticket Browser: format_version=%u, titlekey_type=%u, ticket_version=%u",
                         detail.block.format_version, detail.block.titlekey_type, detail.block.ticket_version);
                LOG_DEBUG("Ticket Browser: license_type=%u, key_generation=%u, property_mask=0x%04X",
                         detail.block.license_type, detail.block.key_generation, detail.block.property_mask);

                // Debug: Show first 16 bytes of titlekey_block
                char keyHex[33];
                bytes_to_hex(detail.block.titlekey_block, 0x10, keyHex);
                LOG_DEBUG("Ticket Browser: titlekey_block (first 16): %s", keyHex);

                // Try to decrypt the titlekey using rights ID key generation (not ticket body key gen)
                u8 titlekek[0x10];
                u8 key_gen_to_use = entry.keyGeneration;
                LOG_DEBUG("Ticket Browser: Using key_generation=%u from rights ID (ticket has %u)",
                         key_gen_to_use, detail.block.key_generation);

                if (load_titlekek(key_gen_to_use, titlekek)) {
                    Aes128Context ctx;
                    aes128ContextCreate(&ctx, titlekek, false);
                    aes128DecryptBlock(&ctx, detail.decrypted_titlekey, detail.block.titlekey_block);
                    detail.has_decrypted_titlekey = true;

                    // Debug: Show decrypted titlekey
                    bytes_to_hex(detail.decrypted_titlekey, 0x10, keyHex);
                    LOG_DEBUG("Ticket Browser: decrypted_titlekey: %s", keyHex);
                } else {
                    LOG_WARN("Ticket Browser: Failed to load titlekek for key_generation=%u", key_gen_to_use);
                }

                detail.loaded = true;
            }
        }

        free(tikBuf);
        esExit();
    } else {
        // Personalized - no cmd 17 on modern FW, just mark as loaded with basic info
        detail.loaded = true;
    }

    return detail.loaded;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

void ticketBrowserInit(TicketBrowserState* state) {
    state->initialized = false;
    state->loading = false;
    state->selectedFilter = 0;
    memset(state->searchBuf, 0, sizeof(state->searchBuf));
    state->tickets.clear();
    state->selectedTicket = -1;
    state->showDetailPopup = false;
    memset(&state->detail, 0, sizeof(state->detail));
}

void ticketBrowserRefresh(TicketBrowserState* state) {
    state->loading = true;
    state->tickets.clear();
    state->selectedTicket = -1;
    state->showDetailPopup = false;

    Result rc = nsInitialize();
    if (R_FAILED(rc)) {
        showError(TR("tickets.ns_init_failed"));
        state->loading = false;
        return;
    }

    rc = esInitialize();
    if (R_FAILED(rc)) {
        showError(TR("tickets.es_init_failed"));
        state->loading = false;
        return;
    }

    u32 commonCount = esCountCommonTicket();
    if (commonCount > 0) {
        EsRightsId* commonIds = (EsRightsId*)malloc(commonCount * sizeof(EsRightsId));
        if (commonIds) {
            u32 written = 0;
            rc = esListCommonTicket(&written, commonIds, commonCount * sizeof(EsRightsId));
            if (R_SUCCEEDED(rc)) {
                for (u32 i = 0; i < written; i++) {
                    TicketEntry entry;
                    entry.titleId = esGetRightsIdApplicationId(&commonIds[i]);
                    entry.keyGeneration = esGetRightsIdKeyGeneration(&commonIds[i]);
                    entry.isPersonalized = false;
                    entry.rightsId = commonIds[i];
                    formatRightsId(&commonIds[i], entry.rightsIdStr);
                    resolveTicketName(entry.titleId, entry.titleName, sizeof(entry.titleName));
                    state->tickets.push_back(entry);
                }
            }
            free(commonIds);
        }
    }

    u32 personalCount = esCountPersonalizedTicket();
    if (personalCount > 0) {
        EsRightsId* personalIds = (EsRightsId*)malloc(personalCount * sizeof(EsRightsId));
        if (personalIds) {
            u32 written = 0;
            rc = esListPersonalizedTicket(&written, personalIds, personalCount * sizeof(EsRightsId));
            if (R_SUCCEEDED(rc)) {
                for (u32 i = 0; i < written; i++) {
                    TicketEntry entry;
                    entry.titleId = esGetRightsIdApplicationId(&personalIds[i]);
                    entry.keyGeneration = esGetRightsIdKeyGeneration(&personalIds[i]);
                    entry.isPersonalized = true;
                    entry.rightsId = personalIds[i];
                    formatRightsId(&personalIds[i], entry.rightsIdStr);
                    resolveTicketName(entry.titleId, entry.titleName, sizeof(entry.titleName));
                    state->tickets.push_back(entry);
                }
            }
            free(personalIds);
        }
    }

    esExit();
    nsExit();

    std::sort(state->tickets.begin(), state->tickets.end(),
        [](const TicketEntry& a, const TicketEntry& b) {
            return strcmp(a.titleName, b.titleName) < 0;
        });

    state->initialized = true;
    state->loading = false;

    char msg[128];
    snprintf(msg, sizeof(msg), TR("tickets.found"), state->tickets.size());
    showSuccess(msg);
}

void ticketBrowserExit(TicketBrowserState* state) {
    state->tickets.clear();
    state->initialized = false;
}

// -----------------------------------------------------------------------
// Detail popup rendering
// -----------------------------------------------------------------------

static void renderDetailPopup(TicketBrowserState* state) {
    if (state->showDetailPopup &&
        state->selectedTicket >= 0 &&
        state->selectedTicket < (int)state->tickets.size()) {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(700, 0), ImGuiCond_Always);
        ImGui::OpenPopup("##TicketDetailPopup");
        state->showDetailPopup = false;
    }

    if (ImGui::BeginPopupModal("##TicketDetailPopup", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (state->selectedTicket < 0 || state->selectedTicket >= (int)state->tickets.size()) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const TicketEntry& entry = state->tickets[state->selectedTicket];
        const TicketDetail& detail = state->detail;
        // Title header
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", entry.titleName);
        ImGui::PopStyleColor();

        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%016lX", entry.titleId);
        ImGui::Separator();
        ImGui::Spacing();

        if (detail.loaded && detail.is_common) {
            // Full common ticket detail
            ImGui::Text("%s: %s", TR("tickets.detail_rights_id"), entry.rightsIdStr);
            ImGui::Spacing();

            if (detail.signature_type != 0) {
                ImGui::Text("%s: %s", TR("tickets.detail_signature"), getSignatureName(detail.signature_type));
                ImGui::Spacing();
            }

            if (detail.block.issuer[0] != '\0') {
                ImGui::Text("%s: %s", TR("tickets.detail_issuer"), detail.block.issuer);
            }
            ImGui::Spacing();

            // Encrypted titlekey
            // Show encrypted titlekey (only if it's actually encrypted/meaningful)
            // For common tickets, the encrypted block is just the wrapped key
            char keyHex[33];
            bytes_to_hex(detail.block.titlekey_block, 0x10, keyHex);

            // Only show encrypted titlekey if it's not all zeros and we're showing decrypted
            bool show_encrypted = false;
            if (detail.has_decrypted_titlekey) {
                // Check if titlekey_block is not all zeros
                bool all_zero = true;
                for (int i = 0; i < 16; i++) {
                    if (detail.block.titlekey_block[i] != 0) {
                        all_zero = false;
                        break;
                    }
                }
                show_encrypted = !all_zero;
            }

            if (show_encrypted || !detail.has_decrypted_titlekey) {
                ImGui::Text("%s:", TR("tickets.detail_titlekey"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1.0f), "%s", keyHex);
            }

            // Decrypted titlekey (show this as the primary key for common tickets)
            if (detail.has_decrypted_titlekey) {
                char decHex[33];
                bytes_to_hex(detail.decrypted_titlekey, 0x10, decHex);
                ImGui::Text("%s:", TR("tickets.detail_titlekey_dec"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", decHex);
            }
            ImGui::Spacing();

            ImGui::Text("%s: %s", TR("tickets.detail_license"), getLicenseTypeName(detail.block.license_type));
            // Show key generation from ticket (this is the authoritative value)
            ImGui::Text("%s: %u", TR("tickets.col_keygen"), detail.block.key_generation);
            // If rights ID key gen differs, show it too
            if (entry.keyGeneration != detail.block.key_generation) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f), "(Rights ID: %u)", entry.keyGeneration);
            }
            ImGui::Spacing();

            // Format version and ticket version
            ImGui::Text("%s: %u", TR("tickets.detail_format_version"), detail.block.format_version);
            ImGui::Text("%s: %u", TR("tickets.detail_ticket_version"), detail.block.ticket_version);
            ImGui::Spacing();

            // Property flags (always show, even if 0)
            u16 props = detail.block.property_mask;
            ImGui::Text("%s: 0x%04X", TR("tickets.detail_properties"), props);
            if (props != 0) {
                if (props & (1 << 0)) ImGui::BulletText("%s", TR("tickets.prop_preinstall"));
                if (props & (1 << 1)) ImGui::BulletText("%s", TR("tickets.prop_shared"));
                if (props & (1 << 2)) ImGui::BulletText("%s", TR("tickets.prop_all_contents"));
                if (props & (1 << 3)) ImGui::BulletText("%s", TR("tickets.prop_device_link"));
                if (props & (1 << 4)) ImGui::BulletText("%s", TR("tickets.prop_volatile"));
                if (props & (1 << 5)) ImGui::BulletText("%s", TR("tickets.prop_elicense"));
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(None)");
            }
            ImGui::Spacing();

            // Ticket type
            const char* ticketTypeStr = detail.block.titlekey_type == 0 ? TR("tickets.type_common") : TR("tickets.type_personalized");
            ImGui::Text("%s: %s", TR("tickets.col_type"), ticketTypeStr);

            // IDs (show even if zero for completeness)
            ImGui::Text("%s: %016llX", TR("tickets.detail_ticket_id"), (unsigned long long)detail.block.ticket_id);
            if (detail.block.ticket_id == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Not set)");
            }

            ImGui::Text("%s: %016llX", TR("tickets.detail_device_id"), (unsigned long long)detail.block.device_id);
            if (detail.block.device_id == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Not tied to device)");
            }

            ImGui::Text("%s: %08X", TR("tickets.detail_account_id"), detail.block.account_id);
            if (detail.block.account_id == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Not set)");
            }

        } else if (detail.loaded && !detail.is_common) {
            // Personalized - limited info
            // Note: For personalized tickets, we only have the rights ID key generation
            // since we can't read the full ticket data
            ImGui::Text("%s: %s", TR("tickets.detail_rights_id"), entry.rightsIdStr);
            ImGui::Text("%s: %u", TR("tickets.col_keygen"), entry.keyGeneration);
            ImGui::Text("%s: %s", TR("tickets.col_type"), TR("tickets.type_personalized"));
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f), "%s", TR("tickets.detail_unavailable"));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Delete button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button(TR("tickets.delete"), ImVec2(120, 35))) {
            Result rc = esInitialize();
            if (R_SUCCEEDED(rc)) {
                rc = esDeleteTicket(&entry.rightsId);
                esExit();
                if (R_SUCCEEDED(rc)) {
                    showSuccess(TR("tickets.delete_success"));
                    ImGui::CloseCurrentPopup();
                    ImGui::PopStyleColor(2);
                    ImGui::EndPopup();
                    ticketBrowserRefresh(state);
                    return;
                } else {
                    showError(TR("tickets.delete_failed"));
                }
            } else {
                showError(TR("tickets.delete_failed"));
            }
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button(TR("tickets.close"), ImVec2(120, 35)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------
// Main render
// -----------------------------------------------------------------------

void renderTicketScreen(TicketBrowserState* state) {
    float windowWidth = ImGui::GetContentRegionAvail().x;

    if (!state->initialized && !state->loading) {
        ticketBrowserRefresh(state);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.70f, 0.40f, 1.0f));
    const char* title = TR("tickets.title");
    ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(title).x) / 2);
    ImGui::Text("%s", title);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!state->initialized && !state->loading) {
        float btnWidth = 200.0f;
        ImGui::SetCursorPosX((windowWidth - btnWidth) / 2);
        if (ImGui::Button(TR("tickets.load"), ImVec2(btnWidth, 45))) {
            ticketBrowserRefresh(state);
        }
    } else if (state->loading) {
        const char* loadMsg = TR("tickets.loading");
        ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(loadMsg).x) / 2);
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", loadMsg);
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        const char* filters[] = {
            TR("tickets.filter_all"),
            TR("tickets.filter_common"),
            TR("tickets.filter_personalized")
        };
        for (int i = 0; i < 3; i++) {
            if (i > 0) ImGui::SameLine();
            bool selected = (state->selectedFilter == i);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
            }
            if (ImGui::Button(filters[i], ImVec2(120, 35))) {
                state->selectedFilter = i;
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::SameLine(0, 20);
        if (ImGui::Button(TR("tickets.refresh"), ImVec2(100, 35))) {
            ticketBrowserRefresh(state);
        }

        ImGui::Spacing();

        int filteredCount = 0;
        for (const auto& t : state->tickets) {
            if (state->selectedFilter == 0 ||
                (state->selectedFilter == 1 && !t.isPersonalized) ||
                (state->selectedFilter == 2 && t.isPersonalized)) {
                filteredCount++;
            }
        }

        ImGui::Text(TR("tickets.showing"), filteredCount, state->tickets.size());
        ImGui::Spacing();

        ImGui::BeginChild("TicketList", ImVec2(0, -50), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

        if (ImGui::BeginTable("Tickets", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {

            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(TR("tickets.col_title"), ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn(TR("tickets.col_id"), ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGui::TableSetupColumn(TR("tickets.col_type"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn(TR("tickets.col_keygen"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (int idx = 0; idx < (int)state->tickets.size(); idx++) {
                const TicketEntry& ticket = state->tickets[idx];
                if (state->selectedFilter == 1 && ticket.isPersonalized) continue;
                if (state->selectedFilter == 2 && !ticket.isPersonalized) continue;

                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                bool isSelected = (state->selectedTicket == idx);

                char selectLabel[64];
                snprintf(selectLabel, sizeof(selectLabel), "##tik%d", idx);

                if (ImGui::Selectable(selectLabel, isSelected,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                    state->selectedTicket = idx;
                    if (fetchTicketDetail(state, idx)) {
                        state->showDetailPopup = true;
                    }
                }
                if (ImGui::IsItemFocused()) {
                    state->selectedTicket = idx;
                }
                ImGui::SameLine();
                ImGui::Text("%s", ticket.titleName);

                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%016lX", ticket.titleId);

                ImGui::TableNextColumn();
                if (ticket.isPersonalized) {
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.9f, 1.0f), "%s", TR("tickets.type_personalized"));
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", TR("tickets.type_common"));
                }

                ImGui::TableNextColumn();
                ImGui::Text("%u", ticket.keyGeneration);
            }

            ImGui::EndTable();
        }

        ImGui::EndChild();

        // Render the detail popup
        renderDetailPopup(state);

        ImGui::PopStyleVar();
    }

    ImGui::Spacing();

    bool popupOpen = ImGui::IsPopupOpen("##TicketDetailPopup");
    if (ImGui::Button(TR("tickets.back"), ImVec2(100, 40)) ||
        (!popupOpen && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))) {
        ScreenChangeEvent event(Screen_MainMenu);
        EventBus::getInstance().post(event);
    }
}

} // namespace Javelin
