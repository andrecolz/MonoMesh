#pragma once

#include "view_base.h"
#include "settings_widgets.h"
#include <string>

namespace MonoMesh {

// Settings screen, phone-style: a vertical list of categories, each opening a page of rows
// (sliders, toggles, steppers, selects) plus small modal dialogs (time/date pickers, option lists,
// confirmations). Every interaction repaints and flushes only the row it changed.
class ViewSettings : public ViewBase {
public:
    ViewSettings() = default;
    ~ViewSettings() override = default;

    void onEnter() override;
    void onExit() override;
    void draw(M5GFX& gfx) override;
    bool handleTouch(const TouchEvent& ev) override;
    void update() override;
    bool scrollPage(int8_t direction) override;
    // Nav bar: tapping SET while a category page is open returns to the category list.
    bool resetToRoot() override;

private:
    enum class Page : uint8_t {
        Root = 0,
        System,
        Radio,
        Channels,
        Node,
        Mesh,
        DateTime,
        Storage,
        Count
    };

    enum class Modal : uint8_t { None = 0, Select, Time, Date, Confirm };

    enum class SelectTarget : uint8_t {
        None = 0,
        BuzzerMode,
        ClockFace,
        PowerButton,
        Region,
        Preset,
        DutyCycle,
        Role,
        NodeInfoInt,
        TelemetryInt,
        PositionInt,
        NeighborInt,
        Rebroadcast
    };

    enum class ConfirmTarget : uint8_t {
        None = 0,
        Reboot,
        PowerOff,
        Regenerate,
        ClearChat,
        ClearNodes
    };

    // ---- navigation state ----
    Page _page = Page::Root;
    Modal _modal = Modal::None;

    // ---- select modal ----
    SelectTarget _selTarget = SelectTarget::None;
    const char* _selTitle = "";
    int _selCount = 0;
    int _selIndex = 0;
    const char* _selLabels[8] = {nullptr};

    // ---- time / date modals ----
    int _timeH = 12, _timeM = 0;
    int _dateY = 2026, _dateM = 1, _dateD = 1;

    // ---- confirm modal ----
    ConfirmTarget _confirmTarget = ConfirmTarget::None;
    const char* _confirmTitle = "";
    const char* _confirmMsg = "";

    // ---- slider drag ----
    int8_t _dragRow = -1;
    uint8_t _sliderBrightness = 30; // live copy while dragging (released to MeshService/BSP)
    uint8_t _sliderVolume = 2;
    uint32_t _lastSliderDrawMs = 0; // throttle: one row repaint every ~60 ms while dragging

    // Arrow auto-repeat while held (time/date pickers). Encoded: 0 = none, 1..6 = button id.
    uint8_t _arrowHeld = 0;
    uint32_t _arrowNextRepeatMs = 0;

    // ---- debounced persistence ----
    bool _savePending = false;
    uint32_t _saveAtMs = 0;

    // Partial refreshes since the last grayscale "settle" pass (keeps ghosting in check without a
    // full-screen flash).
    uint16_t _partialCount = 0;
    uint32_t _lastInteractionMs = 0;

    // ---- drawing ----
    void drawPage(M5GFX& gfx, int onlyRow = -1);
    void drawRoot(M5GFX& gfx, int onlyRow = -1);
    void drawSystem(M5GFX& gfx, int onlyRow = -1);
    void drawRadio(M5GFX& gfx, int onlyRow = -1);
    void drawChannels(M5GFX& gfx, int onlyRow = -1);
    void drawNode(M5GFX& gfx, int onlyRow = -1);
    void drawMesh(M5GFX& gfx, int onlyRow = -1);
    void drawDateTime(M5GFX& gfx, int onlyRow = -1);
    void drawStorage(M5GFX& gfx, int onlyRow = -1);
    void drawModal(M5GFX& gfx);
    void drawTimeModal(M5GFX& gfx, bool digitsOnly = false);
    void drawDateModal(M5GFX& gfx, bool digitsOnly = false);

    // ---- touch ----
    bool handleModalTouch(const TouchEvent& ev);
    bool handleRootTouch(const TouchEvent& ev);
    bool handleSystemTouch(const TouchEvent& ev);
    bool handleRadioTouch(const TouchEvent& ev);
    bool handleChannelsTouch(const TouchEvent& ev);
    bool handleNodeTouch(const TouchEvent& ev);
    bool handleMeshTouch(const TouchEvent& ev);
    bool handleDateTimeTouch(const TouchEvent& ev);
    bool handleStorageTouch(const TouchEvent& ev);

    void openSelect(SelectTarget target);
    void applySelect(int option);
    void openConfirm(ConfirmTarget target);
    void applyConfirm();
    bool stepArrow(uint8_t button); // time/date arrow: steps the matching field, returns true if changed

    // ---- helpers ----
    void gotoPage(Page page);
    void flushContentGrayscale();
    void flushRow(const SWSettings::RowRect& r);
    void noteInteraction();
    void markSavePending();
    void redrawRow(int rowIndex);
};

} // namespace MonoMesh
