#pragma once

#include "view_base.h"
#include <string>
#include <vector>

namespace MonoMesh {

enum class ChatMode {
    ConversationList,
    ChannelChat,
    DirectMessage
};

struct ConvItem {
    bool isChannel;
    std::string channelPreset;
    uint32_t nodeNum;
    std::string shortName;
    uint32_t unread;
    uint32_t latestSeq;
    uint32_t lastHeard;
    uint32_t lastTimestamp;
    char timeStr[16];
    bool hasPublicKey = false;   // node: we know its PKI key (closed padlock / DM deliverable)
    bool keyVerified = false;    // node: we authenticated a PKI packet from it
};

class ViewChat : public ViewBase {
public:
    ViewChat() = default;
    ~ViewChat() override = default;

    void onEnter() override;
    void draw(M5GFX& gfx) override;
    bool handleTouch(const TouchEvent& ev) override;
    void update() override;
    bool scrollPage(int8_t direction) override;
    // Tapping the CHAT tab while a conversation is open returns to the conversation list.
    bool resetToRoot() override;

    static void openDM(uint32_t nodeNum);
    static void returnToList();

private:
    std::vector<ConvItem> getConversationList() const;
    void drawConversationList(M5GFX& gfx);
    void drawChatMessages(M5GFX& gfx);
    bool handleListTouch(const TouchEvent& ev);
    bool handleChatTouch(const TouchEvent& ev);

    static ChatMode s_mode;
    static std::string s_activeChannelPreset;
    static uint32_t s_activeTargetNode;
    static int s_currentPage;
    static int s_listPage;

    static size_t s_lastMessageCount;
};

} // namespace MonoMesh
