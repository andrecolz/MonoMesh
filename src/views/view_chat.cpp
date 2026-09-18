#include "view_chat.h"
#include "../mesh_service.h"
#include "../bsp_papermono.h"
#include "../ui_engine.h"
#include "../epd_driver.h"
#include <algorithm>

namespace MonoMesh {

ChatMode ViewChat::s_mode = ChatMode::ConversationList;
std::string ViewChat::s_activeChannelPreset = "";
uint32_t ViewChat::s_activeTargetNode = 0;
int ViewChat::s_currentPage = 0;
int ViewChat::s_listPage = 1;
size_t ViewChat::s_lastMessageCount = 0;

void ViewChat::openDM(uint32_t nodeNum) {
    s_mode = ChatMode::DirectMessage;
    s_activeTargetNode = nodeNum;
    s_currentPage = 0;
    s_lastMessageCount = MeshService::getInstance().getDirectMessageCount(nodeNum);
    MeshService::getInstance().markDirectMessagesRead(nodeNum);
    UIEngine::getInstance().requestStatusBarRedraw();
    UIEngine::getInstance().requestFullRefresh();
}

void ViewChat::returnToList() {
    if (s_mode != ChatMode::ConversationList) {
        if (s_mode == ChatMode::ChannelChat) {
            MeshService::getInstance().markChannelMessagesRead(s_activeChannelPreset.c_str());
        } else if (s_mode == ChatMode::DirectMessage) {
            MeshService::getInstance().markDirectMessagesRead(s_activeTargetNode);
        }
        s_mode = ChatMode::ConversationList;
        s_currentPage = 0;
        s_lastMessageCount = MeshService::getInstance().getConversationMessageCount();
        UIEngine::getInstance().requestStatusBarRedraw();
        UIEngine::getInstance().requestFullRefresh();
    }
}

// Nav bar: tapping CHAT while this view is already on screen goes back to the conversation list
// (the same gesture the settings tab does with its category list).
bool ViewChat::resetToRoot() {
    if (s_mode == ChatMode::ConversationList) return false;
    returnToList();
    return true;
}

void ViewChat::onEnter() {
    if (s_mode == ChatMode::ChannelChat) {
        MeshService::getInstance().markChannelMessagesRead(s_activeChannelPreset.c_str());
        s_lastMessageCount = MeshService::getInstance().getBroadcastMessageCountForPreset(s_activeChannelPreset.c_str());
    } else if (s_mode == ChatMode::DirectMessage) {
        MeshService::getInstance().markDirectMessagesRead(s_activeTargetNode);
        s_lastMessageCount = MeshService::getInstance().getDirectMessageCount(s_activeTargetNode);
    } else {
        s_lastMessageCount = MeshService::getInstance().getConversationMessageCount();
    }
    UIEngine::getInstance().requestStatusBarRedraw();
}

void ViewChat::update() {
    size_t count = 0;
    uint32_t unread = 0;
    if (s_mode == ChatMode::ChannelChat) {
        count = MeshService::getInstance().getBroadcastMessageCountForPreset(s_activeChannelPreset.c_str());
        unread = MeshService::getInstance().getUnreadCountForChannel(s_activeChannelPreset.c_str());
    } else if (s_mode == ChatMode::DirectMessage) {
        count = MeshService::getInstance().getDirectMessageCount(s_activeTargetNode);
        unread = MeshService::getInstance().getUnreadCountForNode(s_activeTargetNode);
    } else {
        count = MeshService::getInstance().getConversationMessageCount();
    }

    // The count alone saturates at MAX_MESSAGES once the ring is full: also act while unread
    // messages remain, otherwise new arrivals in an open conversation are never marked as read.
    if (count != s_lastMessageCount || unread > 0) {
        s_lastMessageCount = count;
        if (s_mode == ChatMode::ChannelChat) {
            MeshService::getInstance().markChannelMessagesRead(s_activeChannelPreset.c_str());
            s_currentPage = 0;
            UIEngine::getInstance().requestStatusBarRedraw();
        } else if (s_mode == ChatMode::DirectMessage) {
            MeshService::getInstance().markDirectMessagesRead(s_activeTargetNode);
            s_currentPage = 0;
            UIEngine::getInstance().requestStatusBarRedraw();
        }
        if (!UIEngine::getInstance().isKeyboardOpen()) {
            UIEngine::getInstance().requestFullRefresh();
        }
    }
}

static void drawCheckmark(M5GFX& gfx, int x, int y, bool isDouble, uint16_t color) {
    auto drawOne = [&](int ox) {
        gfx.drawLine(ox, y + 4, ox + 3, y + 7, color);
        gfx.drawLine(ox, y + 5, ox + 3, y + 8, color);
        gfx.drawLine(ox + 3, y + 7, ox + 8, y + 1, color);
        gfx.drawLine(ox + 3, y + 8, ox + 8, y + 2, color);
    };

    if (isDouble) {
        drawOne(x);
        drawOne(x + 5);
    } else {
        drawOne(x);
    }
}

static void drawErrorIcon(M5GFX& gfx, int x, int y, uint16_t color) {
    gfx.fillCircle(x + 5, y + 5, 6, color);
    gfx.setTextColor(TFT_WHITE, color);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(1);
    gfx.drawString("!", x + 5, y + 5);
}

// ---------------------------------------------------------------- chat bubble geometry
// Bubbles are content sized (up to a max width) and aligned left for incoming, right for outgoing,
// so sender and receiver are obvious at a glance instead of two identical full-width boxes.
static constexpr int BUBBLE_MARGIN = 14;   // screen margin on both sides (matches every list)
static constexpr int BUBBLE_MAX_W = 360;   // widest bubble: leaves a clear free side
static constexpr int BUBBLE_PAD_X = 12;    // text inset inside the bubble
static constexpr int BUBBLE_HEAD_Y = 7;    // sender / time row, from the bubble top
static constexpr int BUBBLE_TEXT_Y = 26;   // first text line, from the bubble top
static constexpr int BUBBLE_LINE_H = 20;   // size-2 wrapped line pitch
static constexpr int BUBBLE_GAP = 8;       // vertical gap between bubbles
// Size-2 GLCD glyphs are 12 px wide: characters that fit in the widest text area.
static constexpr size_t BUBBLE_LINE_CHARS = (BUBBLE_MAX_W - 2 * BUBBLE_PAD_X) / 12;

static std::vector<std::string> wrapText(const char* text, size_t maxCharsPerLine = 31) {
    std::vector<std::string> lines;
    if (!text || *text == '\0') return lines;

    std::string currentLine;
    const char* p = text;

    while (*p) {
        while (*p == ' ') {
            if (!currentLine.empty()) {
                if (currentLine.length() + 1 <= maxCharsPerLine) {
                    currentLine += ' ';
                } else {
                    lines.push_back(currentLine);
                    currentLine.clear();
                }
            }
            p++;
        }
        if (*p == '\0') break;

        const char* wordStart = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        size_t wordLen = p - wordStart;
        std::string wrd(wordStart, wordLen);

        if (currentLine.empty()) {
            if (wrd.length() <= maxCharsPerLine) {
                currentLine = wrd;
            } else {
                size_t offset = 0;
                while (offset < wrd.length()) {
                    size_t chunk = std::min(maxCharsPerLine, wrd.length() - offset);
                    lines.push_back(wrd.substr(offset, chunk));
                    offset += chunk;
                }
            }
        } else {
            if (currentLine.length() + wrd.length() <= maxCharsPerLine) {
                currentLine += wrd;
            } else {
                lines.push_back(currentLine);
                if (wrd.length() <= maxCharsPerLine) {
                    currentLine = wrd;
                } else {
                    size_t offset = 0;
                    while (offset < wrd.length()) {
                        size_t chunk = std::min(maxCharsPerLine, wrd.length() - offset);
                        if (offset + chunk == wrd.length()) {
                            currentLine = wrd.substr(offset, chunk);
                        } else {
                            lines.push_back(wrd.substr(offset, chunk));
                        }
                        offset += chunk;
                    }
                }
            }
        }

        if (*p == '\n') {
            if (!currentLine.empty()) {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            p++;
        }
    }

    if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }

    if (lines.empty()) lines.push_back("");
    return lines;
}

// Chat pagination is height-driven, not a fixed count: the bubble area runs from y=82 down to the
// compose bar (y=652), and each bubble is 32 + 20 px per wrapped line. Filling greedily packs as
// many messages as physically fit (a full screen of short messages instead of a fixed 5).
static std::vector<size_t> computeChatPageStarts(const std::vector<ChatMessage>& msgs) {
    constexpr int TOP_Y = 82;
    constexpr int BOTTOM_Y = 652;

    std::vector<size_t> starts;
    size_t i = 0;
    while (i < msgs.size()) {
        starts.push_back(i);
        int y = TOP_Y;
        while (i < msgs.size()) {
            int boxH = 32 + (int)wrapText(msgs[i].text, BUBBLE_LINE_CHARS).size() * BUBBLE_LINE_H;
            if (y + boxH > BOTTOM_Y && y > TOP_Y) break; // always show at least one bubble
            y += boxH + BUBBLE_GAP;
            ++i;
        }
    }
    if (starts.empty()) starts.push_back(0);
    return starts;
}

static std::string formatLastMessageTime(uint32_t msgTimestamp, const char* msgTimeStr) {
    if (!msgTimeStr || !msgTimeStr[0] || strcmp(msgTimeStr, "--:--") == 0) {
        return "";
    }

    uint32_t nowUnix = BSP::getInstance().getRtcUnix();
    if (nowUnix > 1700000000UL && msgTimestamp > 1700000000UL) {
        // Compare calendar days in local time, not UTC (otherwise "today" breaks near midnight)
        const int32_t tzSecs = (int32_t)BSP::getInstance().getTimezoneOffset() * 3600;
        time_t nowT = (time_t)(nowUnix + tzSecs);
        time_t msgT = (time_t)(msgTimestamp + tzSecs);
        struct tm nowTm;
        struct tm msgTm;
        gmtime_r(&nowT, &nowTm);
        gmtime_r(&msgT, &msgTm);

        // Check if same calendar day ("in giornata")
        if (nowTm.tm_year == msgTm.tm_year &&
            nowTm.tm_mon == msgTm.tm_mon &&
            nowTm.tm_mday == msgTm.tm_mday) {
            return std::string(msgTimeStr);
        }

        int64_t diffSec = (int64_t)nowUnix - (int64_t)msgTimestamp;
        if (diffSec < 0) diffSec = 0;
        int days = (int)(diffSec / 86400LL);
        if (days == 0) days = 1;

        char buf[16];
        if (days < 7) {
            snprintf(buf, sizeof(buf), "%d D", days);
        } else if (days < 30) {
            int weeks = std::max(1, days / 7);
            snprintf(buf, sizeof(buf), "%d W", weeks);
        } else if (days < 365) {
            int months = std::max(1, days / 30);
            snprintf(buf, sizeof(buf), "%d M", months);
        } else {
            int years = std::max(1, days / 365);
            snprintf(buf, sizeof(buf), "%d Y", years);
        }
        return std::string(buf);
    }

    // Fallback if full date not available yet
    return std::string(msgTimeStr);
}

std::vector<ConvItem> ViewChat::getConversationList() const {
    std::vector<ConvItem> items;

    // 1. Broadcast Channels per preset
    auto activePresets = MeshService::getInstance().getActiveBroadcastPresets();
    for (const auto& pr : activePresets) {
        ConvItem ci = {};
        ci.isChannel = true;
        ci.channelPreset = pr;
        ci.nodeNum = 0;
        ci.unread = MeshService::getInstance().getUnreadCountForChannel(pr.c_str());
        auto msgs = MeshService::getInstance().getBroadcastMessagesForPreset(pr.c_str());
        ci.latestSeq = msgs.empty() ? 0 : msgs.back().seq;
        ci.lastHeard = 0;
        if (!msgs.empty()) {
            ci.lastTimestamp = msgs.back().timestamp;
            std::string t = formatLastMessageTime(msgs.back().timestamp, msgs.back().timeStr);
            strncpy(ci.timeStr, t.c_str(), sizeof(ci.timeStr) - 1);
            ci.timeStr[sizeof(ci.timeStr) - 1] = '\0';
        } else {
            ci.lastTimestamp = 0;
            ci.timeStr[0] = '\0';
        }
        items.push_back(ci);
    }

    // 2. Direct Messages (Nodes)
    const auto& nodes = MeshService::getInstance().getNodes();
    uint32_t localNum = MeshService::getInstance().getLocalNodeNum();
    for (const auto& n : nodes) {
        if (n.nodeNum == localNum) continue;
        ConvItem ci = {};
        ci.isChannel = false;
        ci.channelPreset = "";
        ci.nodeNum = n.nodeNum;
        ci.shortName = (strlen(n.shortName) > 0) ? n.shortName : n.idStr;
        ci.unread = MeshService::getInstance().getUnreadCountForNode(n.nodeNum);
        ci.hasPublicKey = n.hasPublicKey;
        ci.keyVerified = n.keyVerified;
        auto msgs = MeshService::getInstance().getDirectMessages(n.nodeNum);
        // Skip conversations that were never started: a node we merely heard is not a chat.
        if (msgs.empty() && ci.unread == 0) continue;
        ci.latestSeq = msgs.empty() ? 0 : msgs.back().seq;
        ci.lastHeard = n.lastHeard;
        if (!msgs.empty()) {
            ci.lastTimestamp = msgs.back().timestamp;
            std::string t = formatLastMessageTime(msgs.back().timestamp, msgs.back().timeStr);
            strncpy(ci.timeStr, t.c_str(), sizeof(ci.timeStr) - 1);
            ci.timeStr[sizeof(ci.timeStr) - 1] = '\0';
        } else {
            ci.lastTimestamp = 0;
            ci.timeStr[0] = '\0';
        }
        items.push_back(ci);
    }

    // Sort: most recent activity (highest latestSeq) first
    std::sort(items.begin(), items.end(), [](const ConvItem& a, const ConvItem& b) {
        if (a.latestSeq != b.latestSeq) {
            return a.latestSeq > b.latestSeq;
        }
        if (a.isChannel != b.isChannel) {
            return a.isChannel; // active channels before nodes without messages
        }
        if (!a.isChannel && !b.isChannel) {
            return a.lastHeard > b.lastHeard;
        }
        return a.channelPreset < b.channelPreset;
    });

    return items;
}

// Conversation list card geometry, shared by the draw and the touch path: 60 px cards with an 8 px
// gap (the same spacing as the mesh node list) and the same 452 px box length used everywhere else.
static constexpr int CONV_TOP_Y = 84;
static constexpr int CONV_CARD_H = 60;
static constexpr int CONV_CARD_STEP = CONV_CARD_H + 8;

void ViewChat::drawConversationList(M5GFX& gfx) {
    gfx.fillRect(0, 44, 480, 686, TFT_WHITE);

    auto items = getConversationList();

    constexpr size_t PAGE_SIZE = 9;
    int totalPages = items.empty() ? 1 : (int)((items.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    if (s_listPage < 1) s_listPage = 1;
    if (s_listPage > totalPages) s_listPage = totalPages;

    // Header (dot-screen bar, black text). The title is inset 3 px from the card edge (x 14):
    // with the 1 px card border the text at 14 looked glued to / slightly left of the list boxes.
    headerBackground(gfx, 44, 32);
    gfx.drawFastHLine(0, 76, 480, TFT_BLACK);
    gfx.setTextColor(TFT_BLACK);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2);
    gfx.drawString("CHAT", 18, 60);

    // Page indicator [P/N] on the top right
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "[%d/%d]", s_listPage, totalPages);
    gfx.setTextDatum(textdatum_t::middle_right);
    gfx.drawString(pageText, 466, 60);

    if (items.empty()) {
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString("No conversations", 240, 320);
        return;
    }

    size_t startIdx = (size_t)(s_listPage - 1) * PAGE_SIZE;
    size_t endIdx = std::min(startIdx + PAGE_SIZE, items.size());

    int y = CONV_TOP_Y;
    for (size_t i = startIdx; i < endIdx; ++i) {
        const auto& it = items[i];
        int cardH = CONV_CARD_H;
        gfx.drawRoundRect(14, y, 452, cardH, 6, TFT_BLACK);
        gfx.fillRoundRect(16, y + 2, 448, cardH - 4, 4, TFT_WHITE);

        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_left);
        gfx.setTextSize(2);

        char label[64];
        if (it.isChannel) {
            snprintf(label, sizeof(label), "PRIMARY CHANNEL %s", it.channelPreset.c_str());
        } else {
            snprintf(label, sizeof(label), "%s", it.shortName.c_str());
        }
        gfx.drawString(label, 26, y + cardH / 2);

        // PKI hint on DM rows: peers without a known key cannot receive our DMs
        if (!it.isChannel) {
            gfx.setTextSize(1);
            gfx.setTextDatum(textdatum_t::top_left);
            gfx.setTextColor(TFT_BLACK, TFT_WHITE);
            if (it.hasPublicKey) {
                gfx.drawString("PKI OK", 26, y + cardH - 16);
            } else {
                gfx.drawString("NO KEY", 26, y + cardH - 16);
            }
            gfx.setTextSize(2);
            gfx.setTextDatum(textdatum_t::middle_left);
        }

        // Top-bar style unread badge [N]
        int timeX = 452;
        if (it.unread > 0) {
            int bw = 46;
            int bh = 26;
            int bx = 410;
            int by = y + (cardH - bh) / 2;
            gfx.fillRoundRect(bx, by, bw, bh, 4, TFT_BLACK);
            gfx.setTextColor(TFT_WHITE, TFT_BLACK);
            gfx.setTextDatum(textdatum_t::middle_center);
            gfx.setTextSize(2);
            char bStr[8];
            snprintf(bStr, sizeof(bStr), "[%u]", (unsigned)it.unread);
            gfx.drawString(bStr, bx + bw / 2, by + bh / 2);
            timeX = 402;
        }

        // Timestamp / Relative elapsed time on the right
        if (it.timeStr[0] != '\0') {
            gfx.setTextColor(TFT_BLACK, TFT_WHITE);
            gfx.setTextDatum(textdatum_t::middle_right);
            gfx.setTextSize(2);
            gfx.drawString(it.timeStr, timeX, y + cardH / 2);
        }

        y += CONV_CARD_STEP;
    }
}

void ViewChat::drawChatMessages(M5GFX& gfx) {
    gfx.fillRect(0, 44, 480, 686, TFT_WHITE);

    // Messages for active conversation
    std::vector<ChatMessage> msgs;
    if (s_mode == ChatMode::ChannelChat) {
        msgs = MeshService::getInstance().getBroadcastMessagesForPreset(s_activeChannelPreset.c_str());
    } else {
        msgs = MeshService::getInstance().getDirectMessages(s_activeTargetNode);
    }

    auto pageStarts = computeChatPageStarts(msgs);
    int totalPages = (int)pageStarts.size();
    if (s_currentPage < 1 || s_currentPage > totalPages) {
        s_currentPage = totalPages;
    }

    // Header Y: 44..76 (dot-screen bar, black text)
    headerBackground(gfx, 44, 32);
    gfx.drawFastHLine(0, 76, 480, TFT_BLACK);
    gfx.setTextColor(TFT_BLACK);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2);

    char headerText[64];
    if (s_mode == ChatMode::ChannelChat) {
        snprintf(headerText, sizeof(headerText), "PRIMARY CHANNEL %s", s_activeChannelPreset.c_str());
    } else {
        const MeshNode* n = MeshService::getInstance().findNode(s_activeTargetNode);
        const char* sName = n ? n->shortName : "";
        snprintf(headerText, sizeof(headerText), "DM: %s", (sName && strlen(sName) > 0) ? sName : "NODE");
    }
    gfx.drawString(headerText, 18, 60);

    // Page indicator [P/N] on the top right (aligned with the bubbles' right edge)
    char pageText[16];
    snprintf(pageText, sizeof(pageText), "[%d/%d]", s_currentPage, totalPages);
    gfx.setTextDatum(textdatum_t::middle_right);
    gfx.drawString(pageText, 466, 60);

    if (msgs.empty()) {
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString("No messages", 240, 300);
        gfx.drawString("Tap the bar below to write", 240, 330);
    } else {
        size_t startIdx = pageStarts[(size_t)(s_currentPage - 1)];
        size_t endIdx = (s_currentPage < totalPages) ? pageStarts[(size_t)s_currentPage] : msgs.size();

        int curY = 82;
        for (size_t i = startIdx; i < endIdx; ++i) {
            const auto& msg = msgs[i];
            auto lines = wrapText(msg.text, BUBBLE_LINE_CHARS);
            int boxH = 32 + (int)lines.size() * BUBBLE_LINE_H;

            // Header left side: "YOU" for outgoing, sender (+ SNR on channels) for incoming.
            char headLeft[48];
            if (msg.isOutgoing) {
                snprintf(headLeft, sizeof(headLeft), "YOU");
            } else if (s_mode == ChatMode::ChannelChat) {
                snprintf(headLeft, sizeof(headLeft), "%s | SNR: %.0fdB", msg.senderShort, msg.snr);
            } else {
                snprintf(headLeft, sizeof(headLeft), "%s", msg.senderShort);
            }
            const char* timeStr = msg.timeStr[0] ? msg.timeStr : "--:--";

            // Content-sized bubble: as wide as the longest text line / header row (up to
            // BUBBLE_MAX_W); outgoing hugs the right edge, incoming the left one.
            gfx.setTextSize(2);
            int contentW = 0;
            for (const auto& line : lines) {
                contentW = std::max(contentW, gfx.textWidth(line.c_str()));
            }
            gfx.setTextSize(1);
            int headW = gfx.textWidth(headLeft) + 12 + gfx.textWidth(timeStr);
            if (msg.isOutgoing) headW += 20; // room for the single/double check or the error icon
            int boxW = std::min(BUBBLE_MAX_W, std::max(contentW, headW) + 2 * BUBBLE_PAD_X);
            int boxX = msg.isOutgoing ? (480 - BUBBLE_MARGIN - boxW) : BUBBLE_MARGIN;
            int boxY = curY;

            // Bubble: outgoing light gray, incoming white, each with a 1 px black border.
            const uint16_t fill = msg.isOutgoing ? TFT_LIGHTGRAY : TFT_WHITE;
            gfx.drawRoundRect(boxX, boxY, boxW, boxH, 6, TFT_BLACK);
            gfx.fillRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 5, fill);

            const int textX = boxX + BUBBLE_PAD_X;
            const int headRight = boxX + boxW - BUBBLE_PAD_X;

            // Header row: sender info on the left, timestamp (+ delivery state for outgoing) right.
            gfx.setTextColor(TFT_BLACK, fill);
            gfx.setTextDatum(textdatum_t::top_left);
            gfx.setTextSize(1);
            gfx.drawString(headLeft, textX, boxY + BUBBLE_HEAD_Y);

            gfx.setTextDatum(textdatum_t::top_right);
            if (msg.isOutgoing && msg.isFailed) {
                gfx.drawString(timeStr, headRight - 16, boxY + BUBBLE_HEAD_Y);
                drawErrorIcon(gfx, headRight - 11, boxY + BUBBLE_HEAD_Y, TFT_BLACK);
            } else if (msg.isOutgoing) {
                // Checkmark (single = sent, double = ACK) flush with the bubble's right padding.
                if (msg.isAcked) {
                    gfx.drawString(timeStr, headRight - 17, boxY + BUBBLE_HEAD_Y);
                    drawCheckmark(gfx, headRight - 13, boxY + BUBBLE_HEAD_Y, true, TFT_BLACK);
                } else {
                    gfx.drawString(timeStr, headRight - 12, boxY + BUBBLE_HEAD_Y);
                    drawCheckmark(gfx, headRight - 8, boxY + BUBBLE_HEAD_Y, false, TFT_BLACK);
                }
            } else {
                gfx.drawString(timeStr, headRight, boxY + BUBBLE_HEAD_Y);
            }

            // Wrapped message text
            gfx.setTextColor(TFT_BLACK, fill);
            gfx.setTextDatum(textdatum_t::top_left);
            gfx.setTextSize(2);
            for (size_t l = 0; l < lines.size(); ++l) {
                gfx.drawString(lines[l].c_str(), textX, boxY + BUBBLE_TEXT_Y + (int)l * BUBBLE_LINE_H);
            }

            curY += boxH + BUBBLE_GAP;
        }
    }

    // Compose Bar at Bottom of Workspace (Y: 664..724)
    gfx.drawFastHLine(0, 664, 480, TFT_BLACK);
    gfx.drawRoundRect(10, 672, 460, 48, 6, TFT_BLACK);
    gfx.fillRoundRect(12, 674, 456, 44, 4, TFT_LIGHTGRAY);
    gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.setTextSize(2);
    gfx.drawString(" > Write message...", 25, 696);
}

void ViewChat::draw(M5GFX& gfx) {
    if (s_mode == ChatMode::ConversationList) {
        drawConversationList(gfx);
    } else {
        drawChatMessages(gfx);
    }
}

bool ViewChat::handleListTouch(const TouchEvent& ev) {
    // 1. Swipe Up -> Next page
    if (ev.type == TouchEventType::SwipeUp) {
        auto items = getConversationList();
        int totalPages = items.empty() ? 1 : (int)((items.size() + 8) / 9);
        if (s_listPage < totalPages) {
            s_listPage++;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    // 2. Swipe Down -> Previous page
    if (ev.type == TouchEventType::SwipeDown) {
        if (s_listPage > 1) {
            s_listPage--;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;

    auto items = getConversationList();
    constexpr size_t PAGE_SIZE = 9;
    int totalPages = items.empty() ? 1 : (int)((items.size() + PAGE_SIZE - 1) / PAGE_SIZE);
    if (s_listPage < 1) s_listPage = 1;
    if (s_listPage > totalPages) s_listPage = totalPages;

    size_t startIdx = (size_t)(s_listPage - 1) * PAGE_SIZE;
    size_t endIdx = std::min(startIdx + PAGE_SIZE, items.size());

    int y = CONV_TOP_Y;
    for (size_t i = startIdx; i < endIdx; ++i) {
        if (ev.y >= y && ev.y <= y + CONV_CARD_H) {
            if (items[i].isChannel) {
                s_mode = ChatMode::ChannelChat;
                s_activeChannelPreset = items[i].channelPreset;
                s_currentPage = 0;
                s_lastMessageCount = MeshService::getInstance().getBroadcastMessageCountForPreset(s_activeChannelPreset.c_str());
                MeshService::getInstance().markChannelMessagesRead(s_activeChannelPreset.c_str());
            } else {
                s_mode = ChatMode::DirectMessage;
                s_activeTargetNode = items[i].nodeNum;
                s_currentPage = 0;
                s_lastMessageCount = MeshService::getInstance().getDirectMessageCount(s_activeTargetNode);
                MeshService::getInstance().markDirectMessagesRead(s_activeTargetNode);
            }
            UIEngine::getInstance().requestStatusBarRedraw();
            UIEngine::getInstance().requestFullRefresh();
            return true;
        }
        y += CONV_CARD_STEP;
    }
    return false;
}

// Sending can still fail (radio busy, frame too large, peer key not known yet). Never swallow the text
// silently: hand it back to the keyboard so the user can retry instead of losing the message, and log
// the reason. sendX() returning false used to be ignored, so the message simply vanished.
static void sendChannelText(const std::string& preset, const std::string& text) {
    if (text.empty()) return;
    if (MeshService::getInstance().sendBroadcastMessage(text.c_str(), preset.c_str())) return;
    ESP_LOGW("ViewChat", "Channel '%s' send failed: text returned to the keyboard", preset.c_str());
    BSP::getInstance().click();
    UIEngine::getInstance().openKeyboard(text, [preset](const std::string& retry) {
        sendChannelText(preset, retry);
    });
}

static void sendDirectText(uint32_t nodeNum, const std::string& text) {
    if (text.empty()) return;
    if (MeshService::getInstance().sendDirectMessage(nodeNum, text.c_str())) return;
    ESP_LOGW("ViewChat", "DM send to !%08x failed: text returned to the keyboard", (unsigned)nodeNum);
    BSP::getInstance().click();
    UIEngine::getInstance().openKeyboard(text, [nodeNum](const std::string& retry) {
        sendDirectText(nodeNum, retry);
    });
}

// Pagination from the physical buttons (BtnA click = up, BtnB click = down). Same semantics as the
// swipes: in the conversation list -1 goes back to the previous page and +1 to the next one; inside a
// chat -1 shows older messages and +1 newer ones (page 1 is the oldest, page N the newest).
bool ViewChat::scrollPage(int8_t direction) {
    if (direction == 0) return false;

    if (s_mode == ChatMode::ConversationList) {
        auto items = getConversationList();
        constexpr size_t PAGE_SIZE = 9;
        int totalPages = items.empty() ? 1 : (int)((items.size() + PAGE_SIZE - 1) / PAGE_SIZE);
        if (s_listPage < 1) s_listPage = 1;
        if (s_listPage > totalPages) s_listPage = totalPages;
        const int next = s_listPage + ((direction > 0) ? 1 : -1);
        if (next < 1 || next > totalPages) return true; // at the edge: consume the click, no redraw
        s_listPage = next;
        UIEngine::getInstance().requestFullRefresh();
        return true;
    }

    std::vector<ChatMessage> msgs;
    if (s_mode == ChatMode::ChannelChat) {
        msgs = MeshService::getInstance().getBroadcastMessagesForPreset(s_activeChannelPreset.c_str());
    } else {
        msgs = MeshService::getInstance().getDirectMessages(s_activeTargetNode);
    }
    const int totalPages = (int)computeChatPageStarts(msgs).size();
    if (s_currentPage < 1) s_currentPage = totalPages;
    if (s_currentPage > totalPages) s_currentPage = totalPages;
    const int next = s_currentPage + ((direction > 0) ? 1 : -1);
    if (next < 1 || next > totalPages) return true;
    s_currentPage = next;
    UIEngine::getInstance().requestFullRefresh();
    return true;
}

bool ViewChat::handleChatTouch(const TouchEvent& ev) {
    // 1. Edge swipe right (from the left border) to return to the conversation list, like the
    // settings page: a horizontal drag in the middle of the chat must not exit the conversation.
    if (isEdgeSwipeRight(ev)) {
        returnToList();
        return true;
    }

    // 2. Pagination swipe:
    // SwipeDown -> older page (P - 1)
    if (ev.type == TouchEventType::SwipeDown) {
        if (s_currentPage > 1) {
            s_currentPage--;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }
    // SwipeUp -> newer page (P + 1)
    if (ev.type == TouchEventType::SwipeUp) {
        std::vector<ChatMessage> msgs;
        if (s_mode == ChatMode::ChannelChat) {
            msgs = MeshService::getInstance().getBroadcastMessagesForPreset(s_activeChannelPreset.c_str());
        } else {
            msgs = MeshService::getInstance().getDirectMessages(s_activeTargetNode);
        }
        int totalPages = (int)computeChatPageStarts(msgs).size();
        if (s_currentPage < totalPages) {
            s_currentPage++;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;

    // 3. Tap Header to go back to conversation list
    if (ev.y >= 44 && ev.y <= 76) {
        returnToList();
        return true;
    }

    // 4. Tap Compose Bar (Y: 660..728)
    if (ev.y >= 660 && ev.y <= 728) {
        if (s_mode == ChatMode::ChannelChat) {
            const std::string preset = s_activeChannelPreset;
            UIEngine::getInstance().openKeyboard("", [preset](const std::string& text) {
                sendChannelText(preset, text);
                s_currentPage = 0;
                UIEngine::getInstance().requestFullRefresh();
            });
            return true;
        } else if (s_mode == ChatMode::DirectMessage) {
            uint32_t target = s_activeTargetNode;
            UIEngine::getInstance().openKeyboard("", [target](const std::string& text) {
                sendDirectText(target, text);
                s_currentPage = 0;
                UIEngine::getInstance().requestFullRefresh();
            });
            return true;
        }
    }

    return false;
}

bool ViewChat::handleTouch(const TouchEvent& ev) {
    if (s_mode == ChatMode::ConversationList) {
        return handleListTouch(ev);
    } else {
        return handleChatTouch(ev);
    }
}

} // namespace MonoMesh
