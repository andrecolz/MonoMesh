#pragma once

#include "view_base.h"
#include "settings_widgets.h"
#include "../storage_manager.h"

namespace MonoMesh {

// ALARMS view: a list of alarm cards (inverted while enabled, switch on the right, tap to edit) with
// a floating + button, plus the editor page. Changes apply to the live AlarmService immediately and
// are persisted with the same debounce used by the settings screen.
class ViewAlarm : public ViewBase {
public:
    ViewAlarm() = default;
    ~ViewAlarm() override = default;

    void onEnter() override;
    void onExit() override;
    void draw(M5GFX& gfx) override;
    bool handleTouch(const TouchEvent& ev) override;
    void update() override;
    bool scrollPage(int8_t direction) override;
    bool resetToRoot() override;

private:
    enum class Page : uint8_t { List = 0, Edit };
    enum class Modal : uint8_t { None = 0, Select, Time, Date, Confirm };
    enum class SelectTarget : uint8_t { None = 0, Repeat, Ringtone };

    // ---- list state ----
    Page _page = Page::List;
    int _listPage = 1;

    // ---- editor state ----
    int _editIndex = -1;   // index in AlarmService while editing
    AlarmRecord _draft = {};

    // ---- modals ----
    Modal _modal = Modal::None;
    SelectTarget _selTarget = SelectTarget::None;
    const char* _selTitle = "";
    int _selCount = 0;
    int _selIndex = 0;
    const char* _selLabels[4] = {nullptr};
    int _timeH = 7, _timeM = 0;
    int _dateY = 2026, _dateM = 1, _dateD = 1;
    uint8_t _arrowHeld = 0; // encoded picker arrow id, 0 = none
    uint32_t _arrowNextRepeatMs = 0;

    // ---- debounced persistence ----
    bool _savePending = false;
    uint32_t _saveAtMs = 0;

    // ---- drawing ----
    void drawList(M5GFX& gfx);
    void drawEditor(M5GFX& gfx);
    void drawModal(M5GFX& gfx);

    // ---- touch ----
    bool handleListTouch(const TouchEvent& ev);
    bool handleEditorTouch(const TouchEvent& ev);
    bool handleModalTouch(const TouchEvent& ev);

    // ---- editor helpers ----
    void openEditor(int index);
    void closeEditor();
    void applyEdit();
    void markSavePending();
    bool stepArrow(uint8_t button); // picker arrow step, returns true when the value changed
};

} // namespace MonoMesh
