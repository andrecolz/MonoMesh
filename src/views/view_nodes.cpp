#include "view_nodes.h"
#include "view_chat.h"
#include "view_map.h"
#include "../mesh_service.h"
#include "../bsp_papermono.h"
#include "../ui_engine.h"
#include "../epd_driver.h"

namespace MonoMesh {

void ViewNodes::onEnter() {
    _modalOpen = false;
    _tracerouteOpen = false;
    _selectedNodeNum = 0;
    const auto& nodes = MeshService::getInstance().getNodes();
    int totalPages = nodes.empty() ? 1 : (int)((nodes.size() + 6) / 7);
    if (_currentPage < 1) _currentPage = 1;
    if (_currentPage > totalPages) _currentPage = totalPages;
}

void ViewNodes::update() {
    // While the traceroute panel is open, repaint when a new result lands or after a retry timeout
    if (!_tracerouteOpen) return;
    const auto& tr = MeshService::getInstance().getLastTraceroute();
    if (tr.startMillis != _tracerouteLastSeen) {
        _tracerouteLastSeen = tr.startMillis;
        UIEngine::getInstance().requestFullRefresh();
    } else if (tr.pending && _tracerouteStarted != 0 && (millis() - _tracerouteStarted > 45000)) {
        _tracerouteStarted = millis();
        UIEngine::getInstance().requestFullRefresh();
    }
}

// Physical buttons: -1 = previous page (up), +1 = next page (down). Ignored while the context menu
// or the traceroute panel is open, so a click can never move the list behind the modal.
bool ViewNodes::scrollPage(int8_t direction) {
    if (direction == 0) return false;
    if (_modalOpen || _tracerouteOpen) return false;

    const auto& nodes = MeshService::getInstance().getNodes();
    constexpr size_t PAGE_SIZE = 7;
    int totalPages = nodes.empty() ? 1 : (int)((nodes.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    if (_currentPage < 1) _currentPage = 1;
    if (_currentPage > totalPages) _currentPage = totalPages;
    const int next = _currentPage + ((direction > 0) ? 1 : -1);
    if (next < 1 || next > totalPages) return true; // at the edge: consume the click, no redraw
    _currentPage = next;
    UIEngine::getInstance().requestFullRefresh();
    return true;
}

// Small padlock glyph: closed = key known / authenticated, open = no public key exchanged yet
void ViewNodes::drawPadlock(M5GFX& gfx, int x, int y, bool locked) {
    // Shackle
    int shackleCx = locked ? x + 9 : x + 13;
    gfx.drawCircle(shackleCx, y + 7, 6, TFT_BLACK);
    gfx.drawCircle(shackleCx, y + 7, 5, TFT_BLACK);
    gfx.fillRect(shackleCx - 7, y + 7, 14, 8, TFT_WHITE);
    gfx.fillRect(shackleCx - 7, y + 4, 14, 3, TFT_WHITE);
    // Body
    gfx.fillRoundRect(x, y + 12, 18, 14, 3, TFT_BLACK);
    gfx.fillCircle(x + 9, y + 19, 2, TFT_WHITE);
    gfx.fillRect(x + 8, y + 19, 3, 5, TFT_WHITE);
}

void ViewNodes::drawTraceroutePanel(M5GFX& gfx) {
    const auto& tr = MeshService::getInstance().getLastTraceroute();

    gfx.fillRoundRect(20, 130, 440, 440, 8, TFT_WHITE);
    gfx.drawRoundRect(20, 130, 440, 440, 8, TFT_BLACK);
    gfx.drawRoundRect(21, 131, 438, 438, 7, TFT_BLACK);
    gfx.fillRect(22, 132, 436, 40, TFT_LIGHTGRAY);

    gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(2);
    char title[48];
    snprintf(title, sizeof(title), "TRACEROUTE !%08x", (unsigned)tr.target);
    gfx.drawString(title, 240, 152);

    gfx.setTextDatum(textdatum_t::top_left);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextSize(2);

    const char* txt = (tr.text[0] != '\0') ? tr.text : (tr.pending ? "Traceroute in progress..." : "No results");
    // Simple word wrap at 34 chars per line, max 9 lines
    int line = 0;
    int lineY = 190;
    char buf[200];
    snprintf(buf, sizeof(buf), "%s", txt);
    char* p = buf;
    while (*p && line < 9) {
        char out[40];
        size_t take = 0;
        while (take < 34 && p[take] != '\0') take++;
        if (p[take] != '\0') {
            size_t cut = take;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut > 0) take = cut;
        }
        size_t copyLen = take < sizeof(out) - 1 ? take : sizeof(out) - 1;
        memcpy(out, p, copyLen);
        out[copyLen] = '\0';
        gfx.drawString(out, 36, lineY + line * 28);
        p += take;
        while (*p == ' ') p++;
        line++;
    }

    if (!tr.pending) {
        char info[64];
        snprintf(info, sizeof(info), "Forward: %u hop | Return: %u hop",
                 (unsigned)tr.forward.size(), (unsigned)tr.back.size());
        gfx.setTextSize(1);
        gfx.drawString(info, 36, 450);
    }

    // Extra node details (MAC / environment metrics / peer status string)
    const MeshNode* infoNode = MeshService::getInstance().findNode(tr.target);
    if (infoNode) {
        gfx.setTextSize(1);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::top_left);
        char l1[80];
        if (infoNode->hasMac) {
            snprintf(l1, sizeof(l1), "MAC: %02x:%02x:%02x:%02x:%02x:%02x | Bat %u%% %.2fV",
                     infoNode->mac[0], infoNode->mac[1], infoNode->mac[2], infoNode->mac[3], infoNode->mac[4],
                     infoNode->mac[5], (unsigned)infoNode->battery, infoNode->voltage);
        } else {
            snprintf(l1, sizeof(l1), "MAC: -- | Bat %u%% %.2fV", (unsigned)infoNode->battery, infoNode->voltage);
        }
        gfx.drawString(l1, 36, 416);

        char l2[80];
        if (infoNode->hasEnvMetrics) {
            if (infoNode->pm25 > 0.0f) {
                snprintf(l2, sizeof(l2), "Environment: %.1fC %.0f%% %.0fhPa IAQ %.0f PM2.5 %.0f", infoNode->temperature,
                         infoNode->humidity, infoNode->pressure, infoNode->iaq, infoNode->pm25);
            } else {
                snprintf(l2, sizeof(l2), "Environment: %.1fC %.0f%% %.0fhPa IAQ %.0f", infoNode->temperature,
                         infoNode->humidity, infoNode->pressure, infoNode->iaq);
            }
        } else {
            snprintf(l2, sizeof(l2), "Environment: no telemetry data");
        }
        gfx.drawString(l2, 36, 432);

        if (infoNode->statusText[0] != '\0') {
            char l3[64];
            snprintf(l3, sizeof(l3), "Status: %.44s", infoNode->statusText);
            gfx.drawString(l3, 36, 466);
        }
    }

    // Re-run button
    gfx.drawRoundRect(40, 490, 180, 54, 6, TFT_BLACK);
    gfx.fillRoundRect(42, 492, 176, 50, 5, TFT_WHITE);
    gfx.setTextSize(2);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.drawString("Repeat", 130, 517);

    // Close button
    gfx.drawRoundRect(260, 490, 180, 54, 6, TFT_BLACK);
    gfx.fillRoundRect(262, 492, 176, 50, 5, TFT_LIGHTGRAY);
    gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    gfx.drawString("Close", 350, 517);
}

void ViewNodes::draw(M5GFX& gfx) {
    gfx.fillRect(0, 44, 480, 686, TFT_WHITE);

    const auto& nodes = MeshService::getInstance().getNodes();
    constexpr size_t PAGE_SIZE = 7;
    int totalPages = nodes.empty() ? 1 : (int)((nodes.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    if (_currentPage < 1) _currentPage = 1;
    if (_currentPage > totalPages) _currentPage = totalPages;

    // Header (dot-screen bar, black text)
    headerBackground(gfx, 44, 32);
    gfx.drawFastHLine(0, 76, 480, TFT_BLACK);
    gfx.setTextColor(TFT_BLACK);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2);
    char headerText[48];
    snprintf(headerText, sizeof(headerText), "MESH NODES (%u)", (unsigned)nodes.size());
    gfx.drawString(headerText, 18, 60);

    // Page indicator [P/N] on the top right
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "[%d/%d]", _currentPage, totalPages);
    gfx.setTextDatum(textdatum_t::middle_right);
    gfx.drawString(pageText, 466, 60);

    // List of Nodes
    if (nodes.empty()) {
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString("No active nodes", 240, 320);
    } else {
        size_t startIdx = (size_t)(_currentPage - 1) * PAGE_SIZE;
        size_t endIdx = std::min(startIdx + PAGE_SIZE, nodes.size());

        int startY = 82;
        for (size_t i = startIdx; i < endIdx; ++i) {
            const auto& node = nodes[i];
            int cardIndexOnPage = (int)(i - startIdx);
            int cardY = startY + cardIndexOnPage * 88;

        gfx.drawRoundRect(14, cardY, 452, 80, 6, TFT_BLACK);
        gfx.fillRoundRect(16, cardY + 2, 448, 76, 4, TFT_WHITE);

        // Short Name badge (pure black on white with bold border for maximum contrast)
        gfx.fillRoundRect(22, cardY + 8, 70, 64, 4, TFT_WHITE);
        gfx.drawRoundRect(22, cardY + 8, 70, 64, 4, TFT_BLACK);
        gfx.drawRoundRect(23, cardY + 9, 68, 62, 3, TFT_BLACK);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString(node.shortName, 57, cardY + 40);

        // Long Name
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::top_left);
        gfx.setTextSize(2);
        gfx.drawString(node.longName, 100, cardY + 12);

        // Details line: ID, SNR, hop distance, battery
        gfx.setTextSize(1);
        char details[96];
        const char* roleTxt = (node.role == 2) ? "ROUTER" : (node.role == 1 ? "MUTE" : "CLIENT");
        char batTxt[16];
        if (node.battery > 0) snprintf(batTxt, sizeof(batTxt), "%u%%", (unsigned)node.battery);
        else snprintf(batTxt, sizeof(batTxt), "--");
        if (node.hasPosition) {
            snprintf(details, sizeof(details), "ID: %s | %.1fdB | %uhop | Bat %s", node.idStr, node.snr,
                     (unsigned)node.hopsAway, batTxt);
        } else {
            snprintf(details, sizeof(details), "ID: %s | %.1fdB | %uhop | Bat %s", node.idStr, node.snr,
                     (unsigned)node.hopsAway, batTxt);
        }
        gfx.drawString(details, 100, cardY + 42);

        // Role + Last Heard
        char ageTxt[20];
        MeshService::formatAgeLabel(MeshService::getInstance().getNodeAgeSeconds(node), ageTxt, sizeof(ageTxt));
        char status[48];
        snprintf(status, sizeof(status), "%s | %s", roleTxt, ageTxt);
        gfx.drawString(status, 100, cardY + 58);

        // PKI padlock: closed when we hold a verified/known public key for this node
        drawPadlock(gfx, 426, cardY + 44, node.hasPublicKey);
        }
    }

    if (_tracerouteOpen) {
        drawTraceroutePanel(gfx);
        return;
    }

    // Modal Context Menu (if open)
    if (_modalOpen) {
        const MeshNode* sel = MeshService::getInstance().findNode(_selectedNodeNum);
        if (sel) {
            // Darkened backdrop box
            gfx.fillRoundRect(30, 180, 420, 360, 8, TFT_WHITE);
            gfx.drawRoundRect(30, 180, 420, 360, 8, TFT_BLACK);
            gfx.drawRoundRect(31, 181, 418, 358, 7, TFT_BLACK);

            // Modal Header
            gfx.fillRect(32, 182, 416, 40, TFT_LIGHTGRAY);
            gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
            gfx.setTextDatum(textdatum_t::middle_center);
            gfx.setTextSize(2);
            char modalTitle[48];
            snprintf(modalTitle, sizeof(modalTitle), "Node: %s (%s)", sel->shortName, sel->idStr);
            gfx.drawString(modalTitle, 240, 202);

            // Button 1: Send DM (Y: 240..290)
            gfx.drawRoundRect(50, 240, 380, 50, 6, TFT_BLACK);
            gfx.fillRoundRect(52, 242, 376, 46, 4, TFT_WHITE);
            gfx.setTextColor(TFT_BLACK, TFT_WHITE);
            gfx.drawString("Send direct message (DM)", 240, 265);

            // Button 2: View on map (Y: 310..360). Greyed out with an X when the node has no position.
            const bool canShowOnMap = sel->hasPosition && (sel->lat != 0.0 || sel->lon != 0.0);
            gfx.drawRoundRect(50, 310, 380, 50, 6, TFT_BLACK);
            gfx.fillRoundRect(52, 312, 376, 46, 4, canShowOnMap ? TFT_WHITE : TFT_LIGHTGRAY);
            gfx.setTextColor(TFT_BLACK, canShowOnMap ? TFT_WHITE : TFT_LIGHTGRAY);
            gfx.drawString("VIEW ON MAP", 240, 335);
            if (!canShowOnMap) {
                // Not clickable: cross the whole button out
                gfx.drawLine(52, 312, 428, 358, TFT_BLACK);
                gfx.drawLine(52, 313, 427, 358, TFT_BLACK);
                gfx.drawLine(428, 312, 52, 358, TFT_BLACK);
                gfx.drawLine(428, 313, 53, 358, TFT_BLACK);
            }
            gfx.setTextColor(TFT_BLACK, TFT_WHITE);

            // Button 3: Traceroute / Details (Y: 380..430)
            gfx.drawRoundRect(50, 380, 380, 50, 6, TFT_BLACK);
            gfx.fillRoundRect(52, 382, 376, 46, 4, TFT_WHITE);
            gfx.drawString("Node details / Traceroute", 240, 405);

            // Button 4: Close (Y: 460..510)
            gfx.drawRoundRect(50, 460, 380, 50, 6, TFT_BLACK);
            gfx.fillRoundRect(52, 462, 376, 46, 4, TFT_LIGHTGRAY);
            gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
            gfx.drawString("Close", 240, 485);
        }
    }
}

// Same rationale as in ViewChat: a DM that fails to leave the radio must not disappear silently.
// Keep the text in the keyboard and retry, instead of jumping to CHAT with an empty conversation.
static void sendNodeDm(uint32_t nodeNum, const std::string& text) {
    if (text.empty()) return;
    if (!MeshService::getInstance().sendDirectMessage(nodeNum, text.c_str())) {
        ESP_LOGW("ViewNodes", "DM to !%08x failed: text restored to keyboard", (unsigned)nodeNum);
        BSP::getInstance().click();
        UIEngine::getInstance().openKeyboard(text, [nodeNum](const std::string& retry) {
            sendNodeDm(nodeNum, retry);
        });
        return;
    }
    ViewChat::openDM(nodeNum);
    UIEngine::getInstance().setView(0); // Jump to CHAT view
}

bool ViewNodes::handleTouch(const TouchEvent& ev) {
    if (_tracerouteOpen) {
        if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return true;
        // "Ripeti"
        if (ev.x >= 40 && ev.x <= 220 && ev.y >= 490 && ev.y <= 544) {
            if (_selectedNodeNum != 0) {
                MeshService::getInstance().sendTraceroute(_selectedNodeNum);
                _tracerouteStarted = millis();
                UIEngine::getInstance().requestFullRefresh();
            }
            return true;
        }
        // "Chiudi"
        if (ev.x >= 260 && ev.x <= 440 && ev.y >= 490 && ev.y <= 544) {
            _tracerouteOpen = false;
            _modalOpen = false;
            UIEngine::getInstance().requestFullRefresh();
            return true;
        }
        return true;
    }

    if (_modalOpen) {
        if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;

        // Modal Button 1: Send DM (Y: 240..290)
        if (ev.x >= 50 && ev.x <= 430 && ev.y >= 240 && ev.y <= 290) {
            uint32_t targetNode = _selectedNodeNum;
            _modalOpen = false;
            UIEngine::getInstance().openKeyboard("", [targetNode](const std::string& text) {
                sendNodeDm(targetNode, text);
            });
            return true;
        }

        // Modal Button 2: view the node on the map (zoom 14). Disabled when it has no coordinates.
        if (ev.x >= 50 && ev.x <= 430 && ev.y >= 310 && ev.y <= 360) {
            const MeshNode* sel = MeshService::getInstance().findNode(_selectedNodeNum);
            if (sel && sel->hasPosition && (sel->lat != 0.0 || sel->lon != 0.0)) {
                _modalOpen = false;
                ViewMap::focusNodeOnMap(_selectedNodeNum, 14);
            } else {
                ESP_LOGW("ViewNodes", "VIEW ON MAP: node has no coordinates");
            }
            return true;
        }

        // Modal Button 3: Traceroute / Info (Y: 380..430)
        if (ev.x >= 50 && ev.x <= 430 && ev.y >= 380 && ev.y <= 430) {
            _tracerouteOpen = true;
            _tracerouteStarted = millis();
            if (_selectedNodeNum != 0) {
                MeshService::getInstance().sendTraceroute(_selectedNodeNum);
            }
            UIEngine::getInstance().requestFullRefresh();
            return true;
        }

        // Modal Button 4: Close (Y: 460..510)
        if (ev.x >= 50 && ev.x <= 430 && ev.y >= 460 && ev.y <= 510) {
            _modalOpen = false;
            UIEngine::getInstance().requestFullRefresh();
            return true;
        }
        return true;
    }

    // Swipe Up -> Next page
    if (ev.type == TouchEventType::SwipeUp) {
        const auto& nodes = MeshService::getInstance().getNodes();
        int totalPages = nodes.empty() ? 1 : (int)((nodes.size() + 6) / 7);
        if (_currentPage < totalPages) {
            _currentPage++;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    // Swipe Down -> Previous page
    if (ev.type == TouchEventType::SwipeDown) {
        if (_currentPage > 1) {
            _currentPage--;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;

    // Tapped a node card in the list
    const auto& nodes = MeshService::getInstance().getNodes();
    constexpr size_t PAGE_SIZE = 7;
    int totalPages = nodes.empty() ? 1 : (int)((nodes.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    if (_currentPage < 1) _currentPage = 1;
    if (_currentPage > totalPages) _currentPage = totalPages;

    size_t startIdx = (size_t)(_currentPage - 1) * PAGE_SIZE;
    size_t endIdx = std::min(startIdx + PAGE_SIZE, nodes.size());

    int startY = 82;
    for (size_t i = startIdx; i < endIdx; ++i) {
        int cardIndexOnPage = (int)(i - startIdx);
        int cardY = startY + cardIndexOnPage * 88;
        if (ev.y >= cardY && ev.y <= cardY + 80) {
            _selectedNodeNum = nodes[i].nodeNum;
            _modalOpen = true;
            UIEngine::getInstance().requestFullRefresh();
            return true;
        }
    }

    return false;
}

} // namespace MonoMesh
