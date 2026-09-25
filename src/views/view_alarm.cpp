#include "view_alarm.h"
#include "../alarm_service.h"
#include "../bsp_papermono.h"
#include "../epd_driver.h"
#include "../ui_engine.h"
#include <esp_log.h>
#include <cstring>

static constexpr const char* TAG = "MonoMesh-AlarmView";

namespace MonoMesh {

using namespace SWSettings;

namespace {

// ---- list geometry ----
constexpr int CARD_H = 60;
constexpr int CARD_STEP = 68; // 8 px gap, same as the chat/node lists
constexpr int LIST_PAGE_SIZE = 8;
constexpr int SWITCH_X = CONTENT_X + CONTENT_W - 16 - 76; // switch tap zone starts here
constexpr int FAB_CX = 428;
constexpr int FAB_CY = 678;
constexpr int FAB_R = 30;
constexpr int TIME_RIGHT = 356;

// ---- editor rows ----
enum class EditRow : uint8_t { Name = 0, Time, Repeat, Date, Days, Backlight, WakeFromOff, Ringtone, Delete };
constexpr int DAYS_H = 64;
constexpr int DAY_BTN_W = 56;
constexpr int DAY_BTN_H = 56;
constexpr int DAY_BTN_GAP = 6;
constexpr int DAY_STRIP_W = 7 * DAY_BTN_W + 6 * DAY_BTN_GAP;

const char* REPEAT_LABELS[] = {"ONE DATE", "WEEKLY"};
const char* TONE_LABELS[] = {"GENTLE", "CALM", "STRONG"};
const char* DAY_LABELS[7] = {"M", "T", "W", "T", "F", "S", "S"}; // Monday first

int buildEditRows(const AlarmRecord& a, int* ys, int* hs, EditRow* kinds) {
    int n = 0;
    int y = CONTENT_TOP;
    auto add = [&](EditRow kind, int h) {
        kinds[n] = kind;
        ys[n] = y;
        hs[n] = h;
        ++n;
        y += h + GAP;
    };
    add(EditRow::Name, ROW_H);
    add(EditRow::Time, ROW_H);
    add(EditRow::Repeat, ROW_H);
    if (a.repeat == 0) {
        add(EditRow::Date, ROW_H);
    } else {
        add(EditRow::Days, DAYS_H);
    }
    add(EditRow::Backlight, ROW_H);
    add(EditRow::WakeFromOff, ROW_H);
    add(EditRow::Ringtone, ROW_H);
    add(EditRow::Delete, ROW_H);
    return n;
}

int dayButtonX(int i) {
    return CONTENT_X + (CONTENT_W - DAY_STRIP_W) / 2 + i * (DAY_BTN_W + DAY_BTN_GAP);
}

void formatSummary(const AlarmRecord& a, char* out, size_t cap) {
    if (a.repeat == 0) {
        snprintf(out, cap, "%02u/%02u/%04u", (unsigned)a.day, (unsigned)a.month, (unsigned)a.year);
        return;
    }
    if (a.daysMask == 0) {
        snprintf(out, cap, "NO DAYS");
        return;
    }
    if (a.daysMask == 0x7F) {
        snprintf(out, cap, "EVERY DAY");
        return;
    }
    static const char* NAMES[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < 7; ++i) {
        if (!(a.daysMask & (1u << i))) continue;
        const int written = snprintf(out + len, cap - len, "%s%s", len ? " " : "", NAMES[i]);
        if (written <= 0 || (size_t)written >= cap - len) break;
        len += (size_t)written;
    }
}

void addDays(int& y, int& m, int& d, int days) {
    d += days;
    while (d > daysInMonth(y, m)) { // SWSettings::daysInMonth (shared with the date picker)
        d -= daysInMonth(y, m);
        if (++m > 12) {
            m = 1;
            ++y;
        }
    }
}

void defaultAlarm(AlarmRecord& a) {
    memset(&a, 0, sizeof(a));
    strncpy(a.name, "ALARM", sizeof(a.name) - 1);
    a.hour = 7;
    a.minute = 0;
    a.repeat = 0; // one date
    a.tone = 1;   // calm
    a.backlight = true;
    a.powerWake = false;
    a.enabled = true;

    int y = 2026, mo = 1, d = 1, hh = 0, mm = 0, ss = 0;
    BSP::getInstance().getLocalDateTime(y, mo, d, hh, mm, ss);
    if (!BSP::getInstance().isRtcValid()) {
        a.year = 2026;
        a.month = 1;
        a.day = 1;
        return;
    }
    if (hh * 60 + mm >= a.hour * 60 + a.minute) addDays(y, mo, d, 1); // 07:00 already past today
    a.year = (uint16_t)y;
    a.month = (uint8_t)mo;
    a.day = (uint8_t)d;
}

const char* toneLabel(uint8_t tone) {
    return (tone <= 2) ? TONE_LABELS[tone] : TONE_LABELS[1];
}

void drawTrashIcon(M5GFX& g, int cx, int cy, uint16_t color) {
    g.fillRect(cx - 10, cy - 8, 20, 3, color); // lid
    g.fillRect(cx - 3, cy - 12, 6, 4, color);  // handle
    g.drawRect(cx - 7, cy - 5, 14, 16, color); // can
    g.fillRect(cx - 5, cy - 3, 2, 12, color);
    g.fillRect(cx + 3, cy - 3, 2, 12, color);
}

} // namespace

// ---------------------------------------------------------------- lifecycle
void ViewAlarm::onEnter() {
    _listPage = 1;
    _page = Page::List;
    _modal = Modal::None;
    _arrowHeld = 0;
}

void ViewAlarm::onExit() {
    if (_savePending) {
        _savePending = false;
        AlarmService::getInstance().save();
        ESP_LOGI(TAG, "Flushed pending alarm save on exit");
    }
    _page = Page::List;
    _modal = Modal::None;
    _arrowHeld = 0;
}

void ViewAlarm::markSavePending() {
    _savePending = true;
    _saveAtMs = millis() + 1000; // one NVS/SD write per burst of changes
}

void ViewAlarm::update() {
    if (_savePending && (int32_t)(millis() - _saveAtMs) >= 0) {
        _savePending = false;
        AlarmService::getInstance().save();
    }
    // Arrow auto-repeat while the finger stays on a time/date arrow (same feel as the settings page).
    if (_arrowHeld != 0) {
        if (!TouchManager::getInstance().isPressed()) {
            _arrowHeld = 0;
        } else if ((int32_t)(millis() - _arrowNextRepeatMs) >= 0) {
            if (stepArrow(_arrowHeld)) {
                if (_modal == Modal::Time) {
                    TimePicker::draw(M5.Display, _timeH, _timeM, true);
                    EPDDriver::getInstance().flushRect(TimePicker::DIGITS_X, TimePicker::DIGITS_Y,
                                                       TimePicker::DIGITS_W, TimePicker::DIGITS_H);
                } else if (_modal == Modal::Date) {
                    DatePicker::draw(M5.Display, _dateY, _dateM, _dateD, true);
                    EPDDriver::getInstance().flushRect(DatePicker::DIGITS_X, DatePicker::DIGITS_Y,
                                                       DatePicker::DIGITS_W, DatePicker::DIGITS_H);
                }
            }
            _arrowNextRepeatMs = millis() + 120;
        }
    }
}

bool ViewAlarm::scrollPage(int8_t direction) {
    if (_modal != Modal::None) return false;
    if (_page == Page::Edit) {
        if (direction < 0) {
            closeEditor();
            return true;
        }
        return false;
    }
    // BtnA/BtnB page the list up/down.
    const size_t count = AlarmService::getInstance().alarms().size();
    int total = (int)((count + LIST_PAGE_SIZE - 1) / LIST_PAGE_SIZE);
    if (total < 1) total = 1;
    if (_listPage > total) _listPage = total;
    const int next = _listPage + ((direction > 0) ? 1 : -1);
    if (next < 1 || next > total) return true; // edges consume the click
    _listPage = next;
    UIEngine::getInstance().requestFullRefresh();
    return true;
}

bool ViewAlarm::resetToRoot() {
    if (_page != Page::Edit) return false;
    closeEditor();
    return true;
}

// ---------------------------------------------------------------- navigation
void ViewAlarm::openEditor(int index) {
    AlarmService& svc = AlarmService::getInstance();
    AlarmRecord a = {};
    if (index < 0) {
        // "+": draft only. The alarm enters the service (and gets persisted) on the first real
        // change in applyEdit(), so backing out of the untouched editor leaves no armed alarm.
        if (svc.alarms().size() >= MAX_ALARMS) {
            ESP_LOGW(TAG, "Alarm list full (%u): not adding", (unsigned)MAX_ALARMS);
            BSP::getInstance().click();
            return;
        }
        defaultAlarm(a);
    } else {
        a = svc.alarms()[(size_t)index];
    }
    _editIndex = index;
    _draft = a;
    _page = Page::Edit;
    _modal = Modal::None;
    UIEngine::getInstance().requestFullRefresh();
}

void ViewAlarm::closeEditor() {
    if (_savePending) {
        _savePending = false;
        AlarmService::getInstance().save();
    }
    _editIndex = -1;
    _page = Page::List;
    _modal = Modal::None;
    _arrowHeld = 0;
    UIEngine::getInstance().requestFullRefresh();
}

void ViewAlarm::applyEdit() {
    AlarmService& svc = AlarmService::getInstance();
    if (_editIndex < 0) {
        // First change on a "+"/new alarm: this is the user confirming it, only now it is added
        // (addAlarm also persists). A never-touched draft simply evaporates on closeEditor().
        _editIndex = svc.addAlarm(_draft);
        if (_editIndex < 0) return; // list full: the draft stays on screen unsaved
        _draft = svc.alarms()[(size_t)_editIndex]; // sanitized copy
        markSavePending();
        return;
    }
    svc.updateAlarm((size_t)_editIndex, _draft);
    _draft = svc.alarms()[(size_t)_editIndex]; // sanitized copy
    markSavePending();
}

// ---------------------------------------------------------------- draw
void ViewAlarm::draw(M5GFX& gfx) {
    if (_page == Page::Edit) {
        drawEditor(gfx);
    } else {
        drawList(gfx);
    }
    if (_modal != Modal::None) drawModal(gfx);
}

void ViewAlarm::drawList(M5GFX& gfx) {
    gfx.fillRect(0, 44, 480, 686, TFT_WHITE);
    header(gfx, "ALARMS");

    const auto& alarms = AlarmService::getInstance().alarms();
    int totalPages = (int)((alarms.size() + LIST_PAGE_SIZE - 1) / LIST_PAGE_SIZE);
    if (totalPages < 1) totalPages = 1;
    if (_listPage < 1) _listPage = 1;
    if (_listPage > totalPages) _listPage = totalPages;

    char pageText[16];
    snprintf(pageText, sizeof(pageText), "[%d/%d]", _listPage, totalPages);
    gfx.setTextColor(TFT_BLACK);
    gfx.setTextDatum(textdatum_t::middle_right);
    gfx.setTextSize(2);
    gfx.drawString(pageText, 466, HEADER_Y + HEADER_H / 2);

    if (alarms.empty()) {
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString("No alarms", 240, 320);
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.drawString("Tap + to add one", 240, 350);
    }

    const size_t startIdx = (size_t)(_listPage - 1) * LIST_PAGE_SIZE;
    const size_t endIdx = (startIdx + LIST_PAGE_SIZE < alarms.size()) ? startIdx + LIST_PAGE_SIZE
                                                                      : alarms.size();
    for (size_t i = startIdx; i < endIdx; ++i) {
        const AlarmRecord& a = alarms[i];
        const uint16_t bg = a.enabled ? TFT_BLACK : TFT_WHITE;
        const uint16_t fg = a.enabled ? TFT_WHITE : TFT_BLACK;
        const int y = CONTENT_TOP + (int)(i - startIdx) * CARD_STEP;

        gfx.drawRoundRect(CONTENT_X, y, CONTENT_W, CARD_H, 6, TFT_BLACK);
        gfx.fillRoundRect(CONTENT_X + 2, y + 2, CONTENT_W - 4, CARD_H - 4, 4, bg);

        char name[24];
        snprintf(name, sizeof(name), "%.18s", a.name);
        gfx.setTextColor(fg, bg);
        gfx.setTextDatum(textdatum_t::middle_left);
        gfx.setTextSize(2);
        gfx.drawString(name, CONTENT_X + 12, y + 20);

        char summary[40];
        formatSummary(a, summary, sizeof(summary));
        gfx.setTextSize(1);
        gfx.setTextDatum(textdatum_t::top_left);
        gfx.drawString(summary, CONTENT_X + 12, y + CARD_H - 18);

        char timeStr[8];
        snprintf(timeStr, sizeof(timeStr), "%02u:%02u", (unsigned)a.hour, (unsigned)a.minute);
        gfx.setTextSize(3);
        gfx.setTextDatum(textdatum_t::middle_right);
        gfx.drawString(timeStr, TIME_RIGHT, y + CARD_H / 2);

        switchControl(gfx, SWITCH_X, y + (CARD_H - 34) / 2, a.enabled, fg, bg);
    }

    // Floating + (bottom right, above the nav bar).
    gfx.fillCircle(FAB_CX, FAB_CY, FAB_R, TFT_BLACK);
    gfx.fillRect(FAB_CX - 13, FAB_CY - 3, 27, 7, TFT_WHITE);
    gfx.fillRect(FAB_CX - 3, FAB_CY - 13, 7, 27, TFT_WHITE);
}

void ViewAlarm::drawEditor(M5GFX& gfx) {
    gfx.fillRect(0, 44, 480, 686, TFT_WHITE);
    header(gfx, "EDIT ALARM");

    int ys[10], hs[10];
    EditRow kinds[10];
    const int n = buildEditRows(_draft, ys, hs, kinds);

    char value[40];
    for (int i = 0; i < n; ++i) {
        RowRect r;
        r.y = ys[i];
        r.h = hs[i];
        switch (kinds[i]) {
            case EditRow::Name:
                valueRow(gfx, r, "Name", _draft.name);
                break;
            case EditRow::Time:
                snprintf(value, sizeof(value), "%02u:%02u", (unsigned)_draft.hour, (unsigned)_draft.minute);
                valueRow(gfx, r, "Time", value);
                break;
            case EditRow::Repeat:
                valueRow(gfx, r, "Repeat", REPEAT_LABELS[_draft.repeat <= 1 ? _draft.repeat : 0]);
                break;
            case EditRow::Date:
                snprintf(value, sizeof(value), "%02u/%02u/%04u", (unsigned)_draft.day,
                         (unsigned)_draft.month, (unsigned)_draft.year);
                valueRow(gfx, r, "Date", value);
                break;
            case EditRow::Days: {
                card(gfx, CONTENT_X, r.y, CONTENT_W, r.h);
                for (int d = 0; d < 7; ++d) {
                    const bool on = (_draft.daysMask & (1u << d)) != 0;
                    const int bx = dayButtonX(d);
                    const int by = r.y + (r.h - DAY_BTN_H) / 2;
                    gfx.drawRoundRect(bx, by, DAY_BTN_W, DAY_BTN_H, 5, TFT_BLACK);
                    gfx.fillRoundRect(bx + 1, by + 1, DAY_BTN_W - 2, DAY_BTN_H - 2, 4,
                                      on ? TFT_BLACK : TFT_WHITE);
                    gfx.setTextColor(on ? TFT_WHITE : TFT_BLACK, on ? TFT_BLACK : TFT_WHITE);
                    gfx.setTextDatum(textdatum_t::middle_center);
                    gfx.setTextSize(2);
                    gfx.drawString(DAY_LABELS[d], bx + DAY_BTN_W / 2, by + DAY_BTN_H / 2);
                }
                break;
            }
            case EditRow::Backlight:
                toggleRow(gfx, r, "Backlight", _draft.backlight);
                break;
            case EditRow::WakeFromOff:
                toggleRow(gfx, r, "Wake from off", _draft.powerWake);
                break;
            case EditRow::Ringtone:
                valueRow(gfx, r, "Ringtone", toneLabel(_draft.tone));
                break;
            case EditRow::Delete:
                card(gfx, CONTENT_X, r.y, CONTENT_W, r.h);
                drawTrashIcon(gfx, CONTENT_X + 34, r.y + r.h / 2, TFT_BLACK);
                gfx.setTextColor(TFT_BLACK, TFT_WHITE);
                gfx.setTextDatum(textdatum_t::middle_left);
                gfx.setTextSize(2);
                gfx.drawString("Delete alarm", CONTENT_X + 64, r.y + r.h / 2);
                break;
        }
    }
}

void ViewAlarm::drawModal(M5GFX& gfx) {
    switch (_modal) {
        case Modal::Select: {
            const char* labels[4];
            for (int i = 0; i < _selCount && i < 4; ++i) labels[i] = _selLabels[i];
            selectModal(gfx, _selTitle, labels, _selCount, _selIndex);
            break;
        }
        case Modal::Time:
            TimePicker::draw(gfx, _timeH, _timeM);
            break;
        case Modal::Date:
            DatePicker::draw(gfx, _dateY, _dateM, _dateD);
            break;
        case Modal::Confirm:
            confirmModal(gfx, "DELETE ALARM", "Remove this alarm?");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- touch
bool ViewAlarm::handleTouch(const TouchEvent& ev) {
    if (_modal != Modal::None) return handleModalTouch(ev);
    if (_page == Page::Edit) return handleEditorTouch(ev);
    return handleListTouch(ev);
}

bool ViewAlarm::handleListTouch(const TouchEvent& ev) {
    // Same paging gestures as the chat/node lists: swipe pages, Click or Up opens.
    // Swipe Up -> Next page
    if (ev.type == TouchEventType::SwipeUp) {
        const size_t count = AlarmService::getInstance().alarms().size();
        int totalPages = (int)((count + LIST_PAGE_SIZE - 1) / LIST_PAGE_SIZE);
        if (totalPages < 1) totalPages = 1;
        if (_listPage < totalPages) {
            _listPage++;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    // Swipe Down -> Previous page
    if (ev.type == TouchEventType::SwipeDown) {
        if (_listPage > 1) {
            _listPage--;
            UIEngine::getInstance().requestFullRefresh();
        }
        return true;
    }

    // Click or Up: a tap with drift / longer than 1s arrives as Up (TouchManager) and must work too
    if (ev.type != TouchEventType::Click && ev.type != TouchEventType::Up) return false;

    // Floating + (square hit box a little larger than the circle).
    if (ev.x >= FAB_CX - FAB_R - 6 && ev.x <= FAB_CX + FAB_R + 6 &&
        ev.y >= FAB_CY - FAB_R - 6 && ev.y <= FAB_CY + FAB_R + 6) {
        openEditor(-1);
        return true;
    }

    const auto& alarms = AlarmService::getInstance().alarms();
    const size_t startIdx = (size_t)(_listPage - 1) * LIST_PAGE_SIZE;
    const size_t endIdx = (startIdx + LIST_PAGE_SIZE < alarms.size()) ? startIdx + LIST_PAGE_SIZE
                                                                      : alarms.size();
    for (size_t i = startIdx; i < endIdx; ++i) {
        const int y = CONTENT_TOP + (int)(i - startIdx) * CARD_STEP;
        if (!inRect(ev, CONTENT_X, y, CONTENT_W, CARD_H)) continue;
        if (ev.x >= SWITCH_X) {
            AlarmService::getInstance().setEnabled(i, !alarms[i].enabled);
            drawList(M5.Display);
            EPDDriver::getInstance().flushRect(0, HEADER_Y, 480, 686);
        } else {
            openEditor((int)i);
        }
        return true;
    }
    return false;
}

bool ViewAlarm::handleEditorTouch(const TouchEvent& ev) {
    // Phone-style back gesture: edge swipe right returns to the list.
    if (isEdgeSwipeRight(ev)) {
        closeEditor();
        return true;
    }
    if (ev.type != TouchEventType::Click) return false;

    int ys[10], hs[10];
    EditRow kinds[10];
    const int n = buildEditRows(_draft, ys, hs, kinds);

    for (int i = 0; i < n; ++i) {
        RowRect r;
        r.y = ys[i];
        r.h = hs[i];
        if (!inRow(ev, r)) continue;
        switch (kinds[i]) {
            case EditRow::Name:
                UIEngine::getInstance().openKeyboard(_draft.name, [this](const std::string& text) {
                    if (!text.empty()) {
                        strncpy(_draft.name, text.c_str(), sizeof(_draft.name) - 1);
                        _draft.name[sizeof(_draft.name) - 1] = '\0';
                        applyEdit();
                    }
                    UIEngine::getInstance().requestFullRefresh();
                });
                return true;
            case EditRow::Time:
                _timeH = _draft.hour;
                _timeM = _draft.minute;
                _modal = Modal::Time;
                drawModal(M5.Display);
                EPDDriver::getInstance().flushRect(TimePicker::X, TimePicker::Y, TimePicker::W, TimePicker::H);
                return true;
            case EditRow::Repeat:
                _selTarget = SelectTarget::Repeat;
                _selTitle = "REPEAT";
                _selLabels[0] = REPEAT_LABELS[0];
                _selLabels[1] = REPEAT_LABELS[1];
                _selCount = 2;
                _selIndex = _draft.repeat <= 1 ? _draft.repeat : 0;
                _modal = Modal::Select;
                drawModal(M5.Display);
                EPDDriver::getInstance().flushRect(SEL_X, SEL_TOP, SEL_W, selHeight(_selCount));
                return true;
            case EditRow::Date:
                _dateY = _draft.year;
                _dateM = _draft.month;
                _dateD = _draft.day;
                if (_dateY < 2023) _dateY = 2026;
                if (_dateM < 1 || _dateM > 12) _dateM = 1;
                if (_dateD < 1 || _dateD > daysInMonth(_dateY, _dateM)) _dateD = 1;
                _modal = Modal::Date;
                drawModal(M5.Display);
                EPDDriver::getInstance().flushRect(DatePicker::X, DatePicker::Y, DatePicker::W, DatePicker::H);
                return true;
            case EditRow::Days: {
                for (int d = 0; d < 7; ++d) {
                    const int by = r.y + (r.h - DAY_BTN_H) / 2;
                    if (!inRect(ev, dayButtonX(d), by, DAY_BTN_W, DAY_BTN_H)) continue;
                    // Keep at least one active day: daysMask == 0 would save a WEEKLY alarm that
                    // alarmMatches() can never fire (same rationale as the WEEKLY preselection).
                    const uint8_t bit = (uint8_t)(1u << d);
                    if ((_draft.daysMask & bit) && _draft.daysMask == bit) return true;
                    _draft.daysMask ^= bit;
                    applyEdit();
                    drawEditor(M5.Display);
                    EPDDriver::getInstance().flushRect(CONTENT_X, r.y, CONTENT_W, r.h);
                    return true;
                }
                return true;
            }
            case EditRow::Backlight:
                _draft.backlight = !_draft.backlight;
                applyEdit();
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRect(CONTENT_X, r.y, CONTENT_W, r.h);
                return true;
            case EditRow::WakeFromOff:
                _draft.powerWake = !_draft.powerWake;
                applyEdit();
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRect(CONTENT_X, r.y, CONTENT_W, r.h);
                return true;
            case EditRow::Ringtone:
                _selTarget = SelectTarget::Ringtone;
                _selTitle = "RINGTONE";
                _selLabels[0] = TONE_LABELS[0];
                _selLabels[1] = TONE_LABELS[1];
                _selLabels[2] = TONE_LABELS[2];
                _selCount = 3;
                _selIndex = _draft.tone <= 2 ? _draft.tone : 1;
                _modal = Modal::Select;
                drawModal(M5.Display);
                EPDDriver::getInstance().flushRect(SEL_X, SEL_TOP, SEL_W, selHeight(_selCount));
                return true;
            case EditRow::Delete:
                _modal = Modal::Confirm;
                drawModal(M5.Display);
                EPDDriver::getInstance().flushRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H);
                return true;
        }
    }
    return false;
}

bool ViewAlarm::handleModalTouch(const TouchEvent& ev) {
    switch (_modal) {
        case Modal::Select: {
            if (ev.type != TouchEventType::Click) return true;
            const int hit = selectModalHit(ev, _selCount);
            if (hit >= 0) {
                if (_selTarget == SelectTarget::Repeat) {
                    _draft.repeat = (uint8_t)hit;
                    if (_draft.repeat == 1 && _draft.daysMask == 0) {
                        // Weekly with no day selected would never fire: preselect today.
                        const int wd = BSP::getInstance().getLocalWeekday(); // 0 = Sunday
                        _draft.daysMask = (uint8_t)(1u << ((wd + 6) % 7));
                    }
                } else if (_selTarget == SelectTarget::Ringtone) {
                    _draft.tone = (uint8_t)hit;
                    AlarmService::getInstance().previewTone((AlarmTone)_draft.tone);
                }
                applyEdit();
            }
            _modal = Modal::None;
            drawEditor(M5.Display);
            EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
            return true;
        }
        case Modal::Time: {
            if (ev.type == TouchEventType::Down) {
                const int hit = TimePicker::arrowHit(ev);
                if (hit != 0) {
                    _arrowHeld = (uint8_t)hit;
                    _arrowNextRepeatMs = millis() + 450;
                    if (stepArrow(_arrowHeld)) {
                        TimePicker::draw(M5.Display, _timeH, _timeM, true);
                        EPDDriver::getInstance().flushRect(TimePicker::DIGITS_X, TimePicker::DIGITS_Y,
                                                           TimePicker::DIGITS_W, TimePicker::DIGITS_H);
                    }
                }
                return true;
            }
            if (ev.type == TouchEventType::Up) {
                _arrowHeld = 0;
                return true;
            }
            if (ev.type != TouchEventType::Click) return true;
            if (TimePicker::hitCancel(ev)) {
                _modal = Modal::None;
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
                return true;
            }
            if (TimePicker::hitOk(ev)) {
                _draft.hour = (uint8_t)_timeH;
                _draft.minute = (uint8_t)_timeM;
                applyEdit();
                _modal = Modal::None;
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
                return true;
            }
            return true;
        }
        case Modal::Date: {
            if (ev.type == TouchEventType::Down) {
                const int hit = DatePicker::arrowHit(ev);
                if (hit != 0) {
                    _arrowHeld = (uint8_t)hit;
                    _arrowNextRepeatMs = millis() + 450;
                    if (stepArrow(_arrowHeld)) {
                        DatePicker::draw(M5.Display, _dateY, _dateM, _dateD, true);
                        EPDDriver::getInstance().flushRect(DatePicker::DIGITS_X, DatePicker::DIGITS_Y,
                                                           DatePicker::DIGITS_W, DatePicker::DIGITS_H);
                    }
                }
                return true;
            }
            if (ev.type == TouchEventType::Up) {
                _arrowHeld = 0;
                return true;
            }
            if (ev.type != TouchEventType::Click) return true;
            if (DatePicker::hitCancel(ev)) {
                _modal = Modal::None;
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
                return true;
            }
            if (DatePicker::hitOk(ev)) {
                _draft.year = (uint16_t)_dateY;
                _draft.month = (uint8_t)_dateM;
                _draft.day = (uint8_t)_dateD;
                applyEdit();
                _modal = Modal::None;
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
                return true;
            }
            return true;
        }
        case Modal::Confirm: {
            if (ev.type != TouchEventType::Click) return true;
            if (confirmHitOk(ev)) {
                if (_editIndex >= 0) {
                    AlarmService::getInstance().removeAlarm((size_t)_editIndex);
                    _editIndex = -1;
                }
                _savePending = false;
                _modal = Modal::None;
                _page = Page::List;
                UIEngine::getInstance().requestFullRefresh();
            } else if (confirmHitCancel(ev) || !inConfirmCard(ev)) {
                _modal = Modal::None;
                drawEditor(M5.Display);
                EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
            }
            return true;
        }
        default:
            return true;
    }
}

bool ViewAlarm::stepArrow(uint8_t button) {
    if (_modal == Modal::Time) return TimePicker::step(_timeH, _timeM, button);
    if (_modal == Modal::Date) return DatePicker::step(_dateY, _dateM, _dateD, button);
    return false;
}

} // namespace MonoMesh
