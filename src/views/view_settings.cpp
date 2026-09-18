#include "view_settings.h"
#include "../mesh_service.h"
#include "../bsp_papermono.h"
#include "../epd_driver.h"
#include "../storage_manager.h"
#include "../ui_engine.h"
#include <esp_log.h>

static constexpr const char* TAG = "MonoMesh-Settings";

namespace MonoMesh {

using namespace SWSettings;

// ---------------------------------------------------------------- local tables
static const float FREQ_VALUES[] = {869.525f, 906.875f, 433.175f, 920.800f, 915.000f};
static const char* FREQ_LABELS[] = {
    "EU868  (869.525 MHz)", "US915  (906.875 MHz)", "EU433  (433.175 MHz)",
    "JP920  (920.800 MHz)", "ANZ915 (915.000 MHz)"
};
static constexpr int NUM_FREQ = 5;

static const int8_t TX_POWERS[] = {14, 17, 20, 22};
static constexpr int NUM_TX_POWERS = 4;

static const int DUTY_VALUES[] = {0, 1, 5, 10, 25, 50, 100};
static const char* DUTY_LABELS[] = {"REGION DEFAULT", "1 %", "5 %", "10 %", "25 %", "50 %", "100 % (OVERRIDE)"};
static constexpr int NUM_DUTY = 7;

static const char* PRESET_LABELS[] = {
    "LONG FAST", "LONG MODERATE", "LONG SLOW", "MEDIUM SLOW", "MEDIUM FAST", "SHORT FAST", "SHORT SLOW"
};
static constexpr int NUM_PRESETS = 7;

static const char* BUZZER_LABELS[] = {"ALL", "NOTIFICATIONS", "DIRECT MSG", "SYSTEM ONLY", "DISABLED"};
static constexpr int NUM_BUZZER_MODES = 5;

static const char* CLOCK_LABELS[] = {"PORTRAIT", "LANDSCAPE", "PORTRAIT 180", "LANDSCAPE 180"};
static constexpr int NUM_CLOCK_FACES = 4;
static const char* POWER_BTN_LABELS[] = {"POWER OFF", "STANDBY", "CLOCK", "CLOCK+RX"};
static const char* ROLE_LABELS[] = {"CLIENT", "CLIENT MUTE", "ROUTER"};
static const char* RBC_LABELS[] = {"ALL", "KNOWN ONLY", "OFF"};

static const uint32_t NI_VALUES[] = {1800, 3600, 7200, 10800, 21600, 43200, 86400};
static const char* NI_LABELS[] = {"30 MIN", "1 HOUR", "2 HOURS", "3 HOURS", "6 HOURS", "12 HOURS", "24 HOURS"};
static constexpr int NUM_NI = 7;
static const uint32_t TEL_VALUES[] = {900, 1800, 3600, 7200, 10800, 21600, 43200};
static const char* TEL_LABELS[] = {"15 MIN", "30 MIN", "1 HOUR", "2 HOURS", "3 HOURS", "6 HOURS", "12 HOURS"};
static constexpr int NUM_TEL = 7;
static const uint32_t POS_VALUES[] = {0, 300, 900, 1800, 3600, 7200, 10800};
static const char* POS_LABELS[] = {"OFF", "5 MIN", "15 MIN", "30 MIN", "1 HOUR", "2 HOURS", "3 HOURS"};
static constexpr int NUM_POS = 7;
static const uint32_t NB_VALUES[] = {0, 900, 1800, 3600, 7200, 21600};
static const char* NB_LABELS[] = {"OFF", "15 MIN", "30 MIN", "1 HOUR", "2 HOURS", "6 HOURS"};
static constexpr int NUM_NB = 6;

// ---------------------------------------------------------------- page layouts
// Single source of truth for both drawing and touch. The row indexes used by the handlers below
// follow these tables.
static const RowDef ROOT_ICONS_LABELS[7] = {
    {RowKind::Value, "SYSTEM"},            // 0
    {RowKind::Value, "RADIO"},             // 1
    {RowKind::Value, "CHANNELS"},          // 2
    {RowKind::Value, "NODE"},              // 3
    {RowKind::Value, "MESH"},              // 4
    {RowKind::Value, "DATE & TIME"},       // 5
    {RowKind::Value, "STORAGE & INFO"}     // 6
};

static const RowDef SYSTEM_ROWS[] = {
    {RowKind::Slider, "Brightness"},     // 0
    {RowKind::Slider, "Volume"},         // 1
    {RowKind::Toggle, "LED notifications"}, // 2
    {RowKind::Value, "Buzzer mode"},     // 3
    {RowKind::Value, "Clock face"},      // 4
    {RowKind::Value, "Power button"},    // 5
    {RowKind::Buttons3, "Low power"},    // 6
    {RowKind::Action, "Reboot"},         // 7
    {RowKind::Action, "Power off"}       // 8
};
static constexpr int SYSTEM_N = 9;

static const RowDef RADIO_ROWS[] = {
    {RowKind::Value, "Region"},     // 0
    {RowKind::Value, "Preset"},     // 1
    {RowKind::Stepper, "TX power"}, // 2
    {RowKind::Stepper, "Hop limit"},// 3
    {RowKind::Value, "Duty cycle"}, // 4
    {RowKind::Info, "Airtime 1h"}   // 5
};
static constexpr int RADIO_N = 6;

static const RowDef CHANNEL_ROWS[] = {
    {RowKind::Value, "Channel Ch0"},   // 0
    {RowKind::Value, "PSK key"},       // 1
    {RowKind::Info, "Local hash"},     // 2
    {RowKind::Info, "Learned hash"},   // 3
    {RowKind::Toggle, "PKI"},          // 4
    {RowKind::Value, "Node role"},     // 5
    {RowKind::Action, "Regenerate keys + ID"} // 6
};
static constexpr int CHANNEL_N = 7;

static const RowDef NODE_ROWS[] = {
    {RowKind::Value, "Long name"},     // 0
    {RowKind::Value, "Short name"},    // 1
    {RowKind::Info, "Node ID"},        // 2
    {RowKind::Info, "PKI fingerprint"},// 3
    {RowKind::Info, "Key status"}      // 4
};
static constexpr int NODE_N = 5;

static const RowDef MESH_ROWS[] = {
    {RowKind::Value, "NodeInfo interval"},  // 0
    {RowKind::Value, "Telemetry interval"}, // 1
    {RowKind::Value, "Position interval"},  // 2
    {RowKind::Value, "Neighbor interval"},  // 3
    {RowKind::Value, "Rebroadcast"},        // 4
    {RowKind::Toggle, "Mesh time sync"},    // 5
    {RowKind::Action, "Broadcast NodeInfo now"} // 6
};
static constexpr int MESH_N = 7;

static const RowDef DATETIME_ROWS[] = {
    {RowKind::Stepper, "Timezone"},  // 0
    {RowKind::Value, "Set time"},    // 1
    {RowKind::Value, "Set date"},    // 2
    {RowKind::Info, "RTC status"}    // 3
};
static constexpr int DATETIME_N = 4;

static const RowDef STORAGE_ROWS[] = {
    {RowKind::SdCard, "SD card"},           // 0
    {RowKind::Action, "Save all to SD & NVS"}, // 1
    {RowKind::Action, "Clear chat history"},   // 2
    {RowKind::Action, "Clear discovered nodes"}, // 3
    {RowKind::Action, "Refresh screen"},       // 4
    {RowKind::Info, "Firmware"},               // 5
    {RowKind::Info, "Memory"},                 // 6
    {RowKind::Info, "Radio counters"},         // 7
    {RowKind::Info, "Last RX / TX"},           // 8
    {RowKind::Info, "TX channel"}              // 9
};
static constexpr int STORAGE_N = 10;

// Time picker geometry (shared by draw and touch).
static constexpr int TIME_X = 40;
static constexpr int TIME_Y = 170;
static constexpr int TIME_W = 400;
static constexpr int TIME_H = 460;
static constexpr int TIME_ARROW_W = 80;
static constexpr int TIME_ARROW_H = 56;
static constexpr int TIME_UP_Y = TIME_Y + 56;
static constexpr int TIME_DOWN_Y = TIME_Y + 268;
static constexpr int TIME_DIGITS_CY = TIME_Y + 190;
static constexpr int TIME_HOUR_CX = 195;
static constexpr int TIME_MIN_CX = 285;
static constexpr int TIME_BTN_Y = TIME_Y + TIME_H - 18 - MODAL_BTN_H;
static constexpr int TIME_BTN_W = 170;
// Digits flush rect (used while the auto-repeat runs).
static constexpr int TIME_DIGITS_X = TIME_X + 80;
static constexpr int TIME_DIGITS_Y = TIME_DIGITS_CY - 30;
static constexpr int TIME_DIGITS_W = TIME_W - 160;
static constexpr int TIME_DIGITS_H = 60;

// Date picker geometry.
static constexpr int DATE_X = 30;
static constexpr int DATE_Y = 170;
static constexpr int DATE_W = 420;
static constexpr int DATE_H = 460;
static constexpr int DATE_DD_CX = 105;
static constexpr int DATE_MM_CX = 240;
static constexpr int DATE_YY_CX = 375;
static constexpr int DATE_DIGITS_CY = DATE_Y + 190;
static constexpr int DATE_DIGITS_X = DATE_X + 12;
static constexpr int DATE_DIGITS_Y = DATE_DIGITS_CY - 28;
static constexpr int DATE_DIGITS_W = DATE_W - 24;
static constexpr int DATE_DIGITS_H = 56;

// ---------------------------------------------------------------- small helpers
static void formatInterval(uint32_t sec, char* out, size_t cap) {
    if (sec == 0) {
        snprintf(out, cap, "OFF");
    } else if (sec % 3600 == 0) {
        snprintf(out, cap, "%uh", (unsigned)(sec / 3600));
    } else if (sec < 3600) {
        snprintf(out, cap, "%um", (unsigned)(sec / 60));
    } else {
        snprintf(out, cap, "%uh %um", (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60));
    }
}

static int daysInMonth(int year, int month) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (month == 2) {
        const bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return d[month - 1];
}

static const char* powerModeLabel(uint8_t action) {
    switch (action) {
        case 0: return "POWER OFF";
        case 2: return "CLOCK";
        case 3: return "CLOCK+RX";
        default: return "STANDBY";
    }
}

static const char* buzzerModeLabel(uint8_t mode) {
    return (mode < NUM_BUZZER_MODES) ? BUZZER_LABELS[mode] : "ALL";
}

static const char* volumeLabel(uint8_t vol) {
    switch (vol) {
        case 0: return "MUTE";
        case 1: return "MIN";
        case 2: return "MED";
        default: return "MAX";
    }
}

// Clock face selection index, shared by the value row and the picker: 0 = portrait, 1 = landscape,
// 2 = portrait 180°, 3 = landscape 180°. The low bit is landscape, the high bit the 180° flip, so
// CLOCK_LABELS can stay indexed by it.
static int clockFaceIndex(const MeshService& ms) {
    return (ms.getClockLandscape() ? 1 : 0) | (ms.getClockFlip180() ? 2 : 0);
}

static int dutyIndexFromValue(int pct) {
    for (int i = 0; i < NUM_DUTY; ++i) {
        if (DUTY_VALUES[i] == pct) return i;
    }
    return -1; // value not in the list (e.g. hand-edited config.json): no row highlighted
}

// ---------------------------------------------------------------- lifecycle
void ViewSettings::onEnter() {
    _dragRow = -1;
    _sliderBrightness = MeshService::getInstance().getFrontlight();
    _sliderVolume = BSP::getInstance().getBuzzerVolume();
    _arrowHeld = 0;
    // A pending save is flushed when leaving the tab anyway; nothing to restore here.
}

void ViewSettings::onExit() {
    if (_savePending) {
        _savePending = false;
        MeshService::getInstance().saveConfig(false);
        ESP_LOGI(TAG, "Flushed pending settings save on exit");
    }
    _dragRow = -1;
    _arrowHeld = 0;
    _modal = Modal::None;
}

void ViewSettings::markSavePending() {
    _savePending = true;
    _saveAtMs = millis() + 1000; // debounce: one NVS/SD write per burst of changes
}

void ViewSettings::noteInteraction() {
    _partialCount++;
    _lastInteractionMs = millis();
}

void ViewSettings::update() {
    // Debounced persistence: sliders and steppers only touch RAM until the user stops.
    if (_savePending && (int32_t)(millis() - _saveAtMs) >= 0) {
        _savePending = false;
        MeshService::getInstance().saveConfig(false);
    }

    // Arrow auto-repeat while the finger stays on a time/date arrow.
    if (_arrowHeld != 0) {
        if (!TouchManager::getInstance().isPressed()) {
            _arrowHeld = 0;
        } else if ((int32_t)(millis() - _arrowNextRepeatMs) >= 0) {
            if (stepArrow(_arrowHeld)) {
                if (_modal == Modal::Time) {
                    drawTimeModal(M5.Display, true);
                    EPDDriver::getInstance().flushRect(TIME_DIGITS_X, TIME_DIGITS_Y, TIME_DIGITS_W, TIME_DIGITS_H);
                } else if (_modal == Modal::Date) {
                    drawDateModal(M5.Display, true);
                    EPDDriver::getInstance().flushRect(DATE_DIGITS_X, DATE_DIGITS_Y, DATE_DIGITS_W, DATE_DIGITS_H);
                }
                noteInteraction();
            }
            _arrowNextRepeatMs = millis() + 120;
        }
    }

    // Ghosting control: after a burst of small partial updates and a moment of quiet, repaint the
    // content in the 4-level waveform (no flash, unlike a full clear). Skipped while the keyboard is
    // up: the overlay is drawn over the content and the pass would touch it for nothing.
    if (_partialCount >= 30 && _modal == Modal::None && !UIEngine::getInstance().isKeyboardOpen() &&
        (millis() - _lastInteractionMs) > 1500) {
        _partialCount = 0;
        EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
    }
}

bool ViewSettings::scrollPage(int8_t direction) {
    if (_modal != Modal::None) return false; // the modal owns the screen
    if (direction < 0) {
        if (_page == Page::Root) return false;
        gotoPage(Page::Root);
        return true;
    }
    const int last = (int)Page::Count - 1;
    int p = (int)_page;
    p = (p == 0) ? 1 : (p % last) + 1;
    gotoPage((Page)p);
    return true;
}

// Nav bar: tapping SET while this view is on screen and a category is open goes back to the
// category list (same behaviour as the CHAT tab returning to the conversation list).
bool ViewSettings::resetToRoot() {
    if (_page == Page::Root) return false;
    gotoPage(Page::Root);
    return true;
}

// ---------------------------------------------------------------- navigation
void ViewSettings::gotoPage(Page page) {
    _page = page;
    _modal = Modal::None;
    _dragRow = -1;
    _arrowHeld = 0;
    if (page == Page::System) {
        _sliderBrightness = MeshService::getInstance().getFrontlight();
        _sliderVolume = BSP::getInstance().getBuzzerVolume();
    }
    drawPage(M5.Display);
    // Same refresh the whole UI uses when leaving standby: one full-panel pass in the 4-level
    // waveform (epd_text), no quality flash. The canvas was just repainted from scratch above, so
    // this is what actually puts the new page on the glass.
    UIEngine::getInstance().requestFullRefresh();
    noteInteraction();
}

// Repaint the content area with the 4-level waveform: used when a modal closes (its card and black
// buttons would ghost under a 1-bit partial) without paying a full quality pass.
void ViewSettings::flushContentGrayscale() {
    EPDDriver::getInstance().flushRectGrayscale(0, HEADER_Y, 480, 686);
}

void ViewSettings::flushRow(const RowRect& r) {
    EPDDriver::getInstance().flushRect(CONTENT_X, r.y, CONTENT_W, r.h);
}

void ViewSettings::redrawRow(int rowIndex) {
    if (rowIndex < 0) return;
    drawPage(M5.Display, rowIndex);
    // Look up the row rect (tables are static, so this is a cheap linear walk).
    RowRect rows[MAX_ROWS];
    int n = 0;
    switch (_page) {
        case Page::System: n = layoutRows(SYSTEM_ROWS, SYSTEM_N, rows); break;
        case Page::Radio: n = layoutRows(RADIO_ROWS, RADIO_N, rows); break;
        case Page::Channels: n = layoutRows(CHANNEL_ROWS, CHANNEL_N, rows); break;
        case Page::Node: n = layoutRows(NODE_ROWS, NODE_N, rows); break;
        case Page::Mesh: n = layoutRows(MESH_ROWS, MESH_N, rows); break;
        case Page::DateTime: n = layoutRows(DATETIME_ROWS, DATETIME_N, rows); break;
        case Page::Storage: n = layoutRows(STORAGE_ROWS, STORAGE_N, rows); break;
        default: return;
    }
    if (rowIndex < n) flushRow(rows[rowIndex]);
    noteInteraction();
}

// ---------------------------------------------------------------- draw
void ViewSettings::draw(M5GFX& gfx) {
    drawPage(gfx, -1);
    if (_modal != Modal::None) drawModal(gfx);
}

void ViewSettings::drawPage(M5GFX& gfx, int onlyRow) {
    if (onlyRow < 0) {
        // Clear header + content before painting a whole page. Without this the previous page's rows
        // stayed in the framebuffer wherever the new page has no card (the gaps between rows, below
        // the last one...), and the two screens showed through each other as two overlapping layers.
        gfx.fillRect(0, HEADER_Y, 480, 686, TFT_WHITE);
    }
    switch (_page) {
        case Page::Root: drawRoot(gfx, onlyRow); break;
        case Page::System: drawSystem(gfx, onlyRow); break;
        case Page::Radio: drawRadio(gfx, onlyRow); break;
        case Page::Channels: drawChannels(gfx, onlyRow); break;
        case Page::Node: drawNode(gfx, onlyRow); break;
        case Page::Mesh: drawMesh(gfx, onlyRow); break;
        case Page::DateTime: drawDateTime(gfx, onlyRow); break;
        case Page::Storage: drawStorage(gfx, onlyRow); break;
        default: break;
    }
}

void ViewSettings::drawRoot(M5GFX& gfx, int onlyRow) {
    if (onlyRow < 0) header(gfx, "SETTINGS");

    const RootIcon icons[7] = {RootIcon::System, RootIcon::Radio, RootIcon::Channels, RootIcon::Node,
                               RootIcon::Mesh, RootIcon::Clock, RootIcon::Storage};

    for (int i = 0; i < 7; ++i) {
        if (onlyRow >= 0 && onlyRow != i) continue;
        const int y = CONTENT_TOP + i * (ROOT_ROW_H + ROOT_GAP);
        card(gfx, CONTENT_X, y, CONTENT_W, ROOT_ROW_H);
        rootIcon(gfx, icons[i], CONTENT_X + 38, y + ROOT_ROW_H / 2, TFT_BLACK, TFT_WHITE);

        // No subtitle any more: the category label alone, uppercase and a touch larger, centred in
        // the shorter card.
        gfx.setTextColor(TFT_BLACK, TFT_WHITE);
        gfx.setTextDatum(textdatum_t::middle_left);
        gfx.setTextSize(2.5f);
        gfx.drawString(ROOT_ICONS_LABELS[i].label, CONTENT_X + 78, y + ROOT_ROW_H / 2);
        chevron(gfx, CONTENT_X + CONTENT_W - 22, y + ROOT_ROW_H / 2);
    }
}

void ViewSettings::drawSystem(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "SYSTEM");
    RowRect r[SYSTEM_N];
    layoutRows(SYSTEM_ROWS, SYSTEM_N, r);
    // While not dragging, follow the live values: the quick menu can change brightness/volume with
    // this page on screen.
    if (_dragRow < 0) {
        _sliderBrightness = ms.getFrontlight();
        _sliderVolume = BSP::getInstance().getBuzzerVolume();
    }

    if (onlyRow < 0 || onlyRow == 0) {
        char v[16];
        snprintf(v, sizeof(v), "%u%%", (unsigned)_sliderBrightness);
        sliderRow(gfx, r[0], SYSTEM_ROWS[0].label, v, _sliderBrightness / 5, 20, false);
    }
    if (onlyRow < 0 || onlyRow == 1) {
        sliderRow(gfx, r[1], SYSTEM_ROWS[1].label, volumeLabel(_sliderVolume), _sliderVolume, 3, true);
    }
    if (onlyRow < 0 || onlyRow == 2) {
        toggleRow(gfx, r[2], SYSTEM_ROWS[2].label, BSP::getInstance().getLedNotifications());
    }
    if (onlyRow < 0 || onlyRow == 3) {
        valueRow(gfx, r[3], SYSTEM_ROWS[3].label, buzzerModeLabel((uint8_t)BSP::getInstance().getBuzzerMode()));
    }
    if (onlyRow < 0 || onlyRow == 4) {
        valueRow(gfx, r[4], SYSTEM_ROWS[4].label, CLOCK_LABELS[clockFaceIndex(ms)]);
    }
    if (onlyRow < 0 || onlyRow == 5) {
        valueRow(gfx, r[5], SYSTEM_ROWS[5].label, powerModeLabel(ms.getPowerBtnAction()));
    }
    if (onlyRow < 0 || onlyRow == 6) {
        buttons3Row(gfx, r[6], "STANDBY", "CLOCK", "CLOCK+RX");
    }
    if (onlyRow < 0 || onlyRow == 7) actionRow(gfx, r[7], SYSTEM_ROWS[7].label);
    if (onlyRow < 0 || onlyRow == 8) actionRow(gfx, r[8], SYSTEM_ROWS[8].label);
}

void ViewSettings::drawRadio(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "RADIO");
    RowRect r[RADIO_N];
    layoutRows(RADIO_ROWS, RADIO_N, r);

    if (onlyRow < 0 || onlyRow == 0) {
        valueRow(gfx, r[0], RADIO_ROWS[0].label, ms.getFrequencyRegionName(ms.getFrequency()));
    }
    if (onlyRow < 0 || onlyRow == 1) {
        valueRow(gfx, r[1], RADIO_ROWS[1].label, ms.getPresetName(ms.getModemPreset()));
    }
    if (onlyRow < 0 || onlyRow == 2) {
        char v[16];
        snprintf(v, sizeof(v), "%d dBm", (int)ms.getTxPower());
        stepperRow(gfx, r[2], RADIO_ROWS[2].label, v,
                   ms.getTxPower() > TX_POWERS[0], ms.getTxPower() < TX_POWERS[NUM_TX_POWERS - 1]);
    }
    if (onlyRow < 0 || onlyRow == 3) {
        char v[16];
        snprintf(v, sizeof(v), "%u hop", (unsigned)ms.getHopLimit());
        stepperRow(gfx, r[3], RADIO_ROWS[3].label, v, ms.getHopLimit() > 1, ms.getHopLimit() < 7);
    }
    if (onlyRow < 0 || onlyRow == 4) {
        char v[32];
        const uint8_t pct = ms.getDutyCyclePct();
        if (pct == 0) {
            snprintf(v, sizeof(v), "REGION %u%%", (unsigned)ms.getRegionDutyCyclePct());
        } else if (pct >= 100) {
            snprintf(v, sizeof(v), "OVERRIDE (100%%)");
        } else {
            snprintf(v, sizeof(v), "%u%%", (unsigned)pct);
        }
        valueRow(gfx, r[4], RADIO_ROWS[4].label, v);
    }
    if (onlyRow < 0 || onlyRow == 5) {
        char v[48];
        const uint8_t limit = ms.getEffectiveDutyCyclePct();
        if (limit == 0 || limit >= 100) {
            snprintf(v, sizeof(v), "%.2f%% (no limit)", (double)ms.getHourlyAirtimePct());
        } else if (ms.isTxLimited()) {
            snprintf(v, sizeof(v), "%.2f%% / %u%%  LIMIT", (double)ms.getHourlyAirtimePct(), (unsigned)limit);
        } else {
            snprintf(v, sizeof(v), "%.2f%% / %u%%", (double)ms.getHourlyAirtimePct(), (unsigned)limit);
        }
        infoRow(gfx, r[5], RADIO_ROWS[5].label, v);
    }
}

void ViewSettings::drawChannels(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "CHANNELS");
    RowRect r[CHANNEL_N];
    layoutRows(CHANNEL_ROWS, CHANNEL_N, r);

    if (onlyRow < 0 || onlyRow == 0) {
        valueRow(gfx, r[0], CHANNEL_ROWS[0].label, ms.getPrimaryChannelName());
    }
    if (onlyRow < 0 || onlyRow == 1) {
        char v[32];
        if (ms.isPrimaryChannelDefaultPsk()) {
            snprintf(v, sizeof(v), "DEFAULT (%u B)", (unsigned)ms.getPrimaryChannelPskLen());
        } else if (ms.getPrimaryChannelPskLen() == 0) {
            snprintf(v, sizeof(v), "NONE (clear)");
        } else {
            snprintf(v, sizeof(v), "CUSTOM (%u B)", (unsigned)ms.getPrimaryChannelPskLen());
        }
        valueRow(gfx, r[1], CHANNEL_ROWS[1].label, v);
    }
    if (onlyRow < 0 || onlyRow == 2) {
        char v[64];
        if (ms.isLocalChannelHashConfirmed()) {
            snprintf(v, sizeof(v), "0x%02x  confirmed by !%08x", (unsigned)ms.getChannelHash(),
                     (unsigned)ms.getLocalHashConfirmedBy());
        } else {
            snprintf(v, sizeof(v), "0x%02x  never heard", (unsigned)ms.getChannelHash());
        }
        infoRow(gfx, r[2], CHANNEL_ROWS[2].label, v);
    }
    if (onlyRow < 0 || onlyRow == 3) {
        char v[48];
        if (ms.getLearnedHashHits() > 0) {
            snprintf(v, sizeof(v), "0x%02x  %u hits", (unsigned)ms.getLearnedChannelHash(),
                     (unsigned)ms.getLearnedHashHits());
        } else {
            snprintf(v, sizeof(v), "none");
        }
        infoRow(gfx, r[3], CHANNEL_ROWS[3].label, v);
    }
    if (onlyRow < 0 || onlyRow == 4) {
        toggleRow(gfx, r[4], CHANNEL_ROWS[4].label, ms.isPkiEnabled());
    }
    if (onlyRow < 0 || onlyRow == 5) {
        const uint8_t role = (uint8_t)ms.getNodeRole();
        valueRow(gfx, r[5], CHANNEL_ROWS[5].label, ROLE_LABELS[role <= 2 ? role : 0]);
    }
    if (onlyRow < 0 || onlyRow == 6) {
        actionRow(gfx, r[6], CHANNEL_ROWS[6].label);
    }
}

void ViewSettings::drawNode(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "NODE");
    RowRect r[NODE_N];
    layoutRows(NODE_ROWS, NODE_N, r);

    if (onlyRow < 0 || onlyRow == 0) valueRow(gfx, r[0], NODE_ROWS[0].label, ms.getLocalLongName());
    if (onlyRow < 0 || onlyRow == 1) valueRow(gfx, r[1], NODE_ROWS[1].label, ms.getLocalShortName());
    if (onlyRow < 0 || onlyRow == 2) infoRow(gfx, r[2], NODE_ROWS[2].label, ms.getLocalIdStr());
    if (onlyRow < 0 || onlyRow == 3) {
        char v[32];
        snprintf(v, sizeof(v), "0x%08x", (unsigned)ms.getPkiKeyFingerprint());
        infoRow(gfx, r[3], NODE_ROWS[3].label, v);
    }
    if (onlyRow < 0 || onlyRow == 4) {
        const char* st = ms.hasPkiKeys() ? (ms.isPkiEnabled() ? "ACTIVE" : "DISABLED") : "ABSENT";
        infoRow(gfx, r[4], NODE_ROWS[4].label, st);
    }
}

void ViewSettings::drawMesh(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "MESH");
    RowRect r[MESH_N];
    layoutRows(MESH_ROWS, MESH_N, r);
    char v[32];

    if (onlyRow < 0 || onlyRow == 0) {
        formatInterval(ms.getNodeInfoIntervalSec(), v, sizeof(v));
        valueRow(gfx, r[0], MESH_ROWS[0].label, v);
    }
    if (onlyRow < 0 || onlyRow == 1) {
        formatInterval(ms.getTelemetryIntervalSec(), v, sizeof(v));
        valueRow(gfx, r[1], MESH_ROWS[1].label, v);
    }
    if (onlyRow < 0 || onlyRow == 2) {
        formatInterval(ms.getPositionIntervalSec(), v, sizeof(v));
        valueRow(gfx, r[2], MESH_ROWS[2].label, v);
    }
    if (onlyRow < 0 || onlyRow == 3) {
        formatInterval(ms.getNeighborInfoIntervalSec(), v, sizeof(v));
        valueRow(gfx, r[3], MESH_ROWS[3].label, v);
    }
    if (onlyRow < 0 || onlyRow == 4) {
        const int m = (int)ms.getRebroadcastMode();
        valueRow(gfx, r[4], MESH_ROWS[4].label, RBC_LABELS[(m >= 0 && m <= 2) ? m : 0]);
    }
    if (onlyRow < 0 || onlyRow == 5) {
        toggleRow(gfx, r[5], MESH_ROWS[5].label, ms.getMeshTimeSync());
    }
    if (onlyRow < 0 || onlyRow == 6) actionRow(gfx, r[6], MESH_ROWS[6].label);
}

void ViewSettings::drawDateTime(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "DATE & TIME");
    RowRect r[DATETIME_N];
    layoutRows(DATETIME_ROWS, DATETIME_N, r);

    if (onlyRow < 0 || onlyRow == 0) {
        char v[16];
        snprintf(v, sizeof(v), "UTC%+d", (int)ms.getTimezoneOffset());
        stepperRow(gfx, r[0], DATETIME_ROWS[0].label, v,
                   ms.getTimezoneOffset() > -12, ms.getTimezoneOffset() < 14);
    }
    int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0;
    BSP::getInstance().getLocalDateTime(yy, mo, dd, hh, mm, ss);
    const bool rtcValid = BSP::getInstance().isRtcValid();
    if (onlyRow < 0 || onlyRow == 1) {
        char v[16];
        if (rtcValid) snprintf(v, sizeof(v), "%02d:%02d", hh, mm);
        else snprintf(v, sizeof(v), "--:--");
        valueRow(gfx, r[1], DATETIME_ROWS[1].label, v);
    }
    if (onlyRow < 0 || onlyRow == 2) {
        char v[16];
        if (rtcValid) snprintf(v, sizeof(v), "%02d/%02d/%04d", dd, mo, yy);
        else snprintf(v, sizeof(v), "not set");
        valueRow(gfx, r[2], DATETIME_ROWS[2].label, v);
    }
    if (onlyRow < 0 || onlyRow == 3) {
        infoRow(gfx, r[3], DATETIME_ROWS[3].label, rtcValid ? "VALID" : "NOT SET");
    }
}

void ViewSettings::drawStorage(M5GFX& gfx, int onlyRow) {
    MeshService& ms = MeshService::getInstance();
    if (onlyRow < 0) header(gfx, "STORAGE & INFO");
    RowRect r[STORAGE_N];
    layoutRows(STORAGE_ROWS, STORAGE_N, r);

    if (onlyRow < 0 || onlyRow == 0) {
        char v[40];
        float frac = 0.0f;
        const bool mounted = StorageManager::getInstance().isSdMounted();
        if (mounted) {
            const uint64_t total = StorageManager::getInstance().getSdTotalBytes();
            const uint64_t freeB = StorageManager::getInstance().getSdFreeBytes();
            const uint64_t used = (total >= freeB) ? (total - freeB) : 0;
            frac = (total > 0) ? (float)((double)used / (double)total) : 0.0f;
            snprintf(v, sizeof(v), "%.1f / %.1f GB", (double)used / (1024.0 * 1024.0 * 1024.0),
                     (double)total / (1024.0 * 1024.0 * 1024.0));
        } else {
            snprintf(v, sizeof(v), "NOT MOUNTED");
        }
        sdCardRow(gfx, r[0], v, frac, mounted);
    }
    if (onlyRow < 0 || onlyRow == 1) actionRow(gfx, r[1], STORAGE_ROWS[1].label);
    if (onlyRow < 0 || onlyRow == 2) actionRow(gfx, r[2], STORAGE_ROWS[2].label);
    if (onlyRow < 0 || onlyRow == 3) actionRow(gfx, r[3], STORAGE_ROWS[3].label);
    if (onlyRow < 0 || onlyRow == 4) actionRow(gfx, r[4], STORAGE_ROWS[4].label);
    if (onlyRow < 0 || onlyRow == 5) {
        char v[48];
        const uint32_t up = millis() / 1000;
        snprintf(v, sizeof(v), "v%s  |  up %luh %02lum", MONOMESH_VERSION, (unsigned long)(up / 3600),
                 (unsigned long)((up % 3600) / 60));
        infoRow(gfx, r[5], STORAGE_ROWS[5].label, v);
    }
    if (onlyRow < 0 || onlyRow == 6) {
        char v[48];
        snprintf(v, sizeof(v), "heap %u KB  |  PSRAM %u KB",
                 (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
        infoRow(gfx, r[6], STORAGE_ROWS[6].label, v);
    }
    if (onlyRow < 0 || onlyRow == 7) {
        char v[64];
        snprintf(v, sizeof(v), "RX %u  OK %u  KO %u  TX %u", (unsigned)ms.getRxTotal(),
                 (unsigned)ms.getRxDecoded(), (unsigned)ms.getRxUndecodable(), (unsigned)ms.getTxTotal());
        infoRow(gfx, r[7], STORAGE_ROWS[7].label, v);
    }
    if (onlyRow < 0 || onlyRow == 8) {
        char v[64];
        char rx[24];
        if (ms.getLastRxMillis() == 0) {
            snprintf(rx, sizeof(rx), "none");
        } else {
            snprintf(rx, sizeof(rx), "%us", (unsigned)((millis() - ms.getLastRxMillis()) / 1000));
        }
        char tx[24];
        if (ms.getLastTxMillis() == 0) {
            snprintf(tx, sizeof(tx), "none");
        } else {
            snprintf(tx, sizeof(tx), "%us", (unsigned)((millis() - ms.getLastTxMillis()) / 1000));
        }
        snprintf(v, sizeof(v), "RX %s  |  TX %s  |  relays %u", rx, tx, (unsigned)ms.getOpaqueRelayCount());
        infoRow(gfx, r[8], STORAGE_ROWS[8].label, v);
    }
    if (onlyRow < 0 || onlyRow == 9) {
        char v[48];
        snprintf(v, sizeof(v), "0x%02x %s", (unsigned)ms.getTxChannelHash(),
                 ms.isTxChannelAdapted() ? "(adapted)" : "(local)");
        infoRow(gfx, r[9], STORAGE_ROWS[9].label, v);
    }
}

// ---------------------------------------------------------------- modals (draw)
void ViewSettings::drawModal(M5GFX& gfx) {
    switch (_modal) {
        case Modal::Select: {
            const char* labels[8];
            for (int i = 0; i < _selCount; ++i) labels[i] = _selLabels[i];
            selectModal(gfx, _selTitle, labels, _selCount, _selIndex);
            break;
        }
        case Modal::Time: drawTimeModal(gfx, false); break;
        case Modal::Date: drawDateModal(gfx, false); break;
        case Modal::Confirm: confirmModal(gfx, _confirmTitle, _confirmMsg); break;
        default: break;
    }
}

void ViewSettings::drawTimeModal(M5GFX& gfx, bool digitsOnly) {
    if (!digitsOnly) {
        modalCard(gfx, TIME_X, TIME_Y, TIME_W, TIME_H, "SET TIME");
        // Up / down arrows under each field.
        const int centers[2] = {TIME_HOUR_CX, TIME_MIN_CX};
        for (int i = 0; i < 2; ++i) {
            for (int up = 0; up < 2; ++up) {
                const int by = up ? TIME_DOWN_Y : TIME_UP_Y;
                const int bx = centers[i] - TIME_ARROW_W / 2;
                gfx.drawRoundRect(bx, by, TIME_ARROW_W, TIME_ARROW_H, 5, TFT_BLACK);
                gfx.fillRoundRect(bx + 1, by + 1, TIME_ARROW_W - 2, TIME_ARROW_H - 2, 4, TFT_LIGHTGRAY);
                const int cx = centers[i];
                const int cy = by + TIME_ARROW_H / 2;
                if (up) {
                    gfx.fillTriangle(cx - 14, cy + 8, cx + 14, cy + 8, cx, cy - 10, TFT_BLACK);
                } else {
                    gfx.fillTriangle(cx - 14, cy - 8, cx + 14, cy - 8, cx, cy + 10, TFT_BLACK);
                }
            }
        }
        // Cancel / OK
        gfx.drawRoundRect(TIME_X + 18, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H, 6, TFT_BLACK);
        gfx.fillRoundRect(TIME_X + 19, TIME_BTN_Y + 1, TIME_BTN_W - 2, MODAL_BTN_H - 2, 5, TFT_LIGHTGRAY);
        gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString("CANCEL", TIME_X + 18 + TIME_BTN_W / 2, TIME_BTN_Y + MODAL_BTN_H / 2);
        const int okX = TIME_X + TIME_W - 18 - TIME_BTN_W;
        gfx.drawRoundRect(okX, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H, 6, TFT_BLACK);
        gfx.fillRoundRect(okX + 1, TIME_BTN_Y + 1, TIME_BTN_W - 2, MODAL_BTN_H - 2, 5, TFT_BLACK);
        gfx.setTextColor(TFT_WHITE, TFT_BLACK);
        gfx.drawString("OK", okX + TIME_BTN_W / 2, TIME_BTN_Y + MODAL_BTN_H / 2);
    }
    // Digits
    gfx.fillRect(TIME_DIGITS_X, TIME_DIGITS_Y, TIME_DIGITS_W, TIME_DIGITS_H, TFT_WHITE);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(5);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", _timeH, _timeM);
    gfx.drawString(buf, TIME_X + TIME_W / 2, TIME_DIGITS_CY);
}

void ViewSettings::drawDateModal(M5GFX& gfx, bool digitsOnly) {
    if (!digitsOnly) {
        modalCard(gfx, DATE_X, DATE_Y, DATE_W, DATE_H, "SET DATE");
        const int centers[3] = {DATE_DD_CX, DATE_MM_CX, DATE_YY_CX};
        const int widths[3] = {100, 100, 130};
        for (int i = 0; i < 3; ++i) {
            for (int up = 0; up < 2; ++up) {
                const int by = up ? TIME_DOWN_Y : TIME_UP_Y;
                const int bx = centers[i] - widths[i] / 2;
                gfx.drawRoundRect(bx, by, widths[i], TIME_ARROW_H, 5, TFT_BLACK);
                gfx.fillRoundRect(bx + 1, by + 1, widths[i] - 2, TIME_ARROW_H - 2, 4, TFT_LIGHTGRAY);
                const int cx = centers[i];
                const int cy = by + TIME_ARROW_H / 2;
                if (up) {
                    gfx.fillTriangle(cx - 14, cy + 8, cx + 14, cy + 8, cx, cy - 10, TFT_BLACK);
                } else {
                    gfx.fillTriangle(cx - 14, cy - 8, cx + 14, cy - 8, cx, cy + 10, TFT_BLACK);
                }
            }
        }
        gfx.drawRoundRect(DATE_X + 18, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H, 6, TFT_BLACK);
        gfx.fillRoundRect(DATE_X + 19, TIME_BTN_Y + 1, TIME_BTN_W - 2, MODAL_BTN_H - 2, 5, TFT_LIGHTGRAY);
        gfx.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextSize(2);
        gfx.drawString("CANCEL", DATE_X + 18 + TIME_BTN_W / 2, TIME_BTN_Y + MODAL_BTN_H / 2);
        const int okX = DATE_X + DATE_W - 18 - TIME_BTN_W;
        gfx.drawRoundRect(okX, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H, 6, TFT_BLACK);
        gfx.fillRoundRect(okX + 1, TIME_BTN_Y + 1, TIME_BTN_W - 2, MODAL_BTN_H - 2, 5, TFT_BLACK);
        gfx.setTextColor(TFT_WHITE, TFT_BLACK);
        gfx.drawString("OK", okX + TIME_BTN_W / 2, TIME_BTN_Y + MODAL_BTN_H / 2);
    }
    gfx.fillRect(DATE_DIGITS_X, DATE_DIGITS_Y, DATE_DIGITS_W, DATE_DIGITS_H, TFT_WHITE);
    gfx.setTextColor(TFT_BLACK, TFT_WHITE);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextSize(4);
    char d[4], m[4], y[8];
    snprintf(d, sizeof(d), "%02d", _dateD);
    snprintf(m, sizeof(m), "%02d", _dateM);
    snprintf(y, sizeof(y), "%04d", _dateY);
    gfx.drawString(d, DATE_DD_CX, DATE_DIGITS_CY);
    gfx.drawString(m, DATE_MM_CX, DATE_DIGITS_CY);
    gfx.drawString(y, DATE_YY_CX, DATE_DIGITS_CY);
}

// ---------------------------------------------------------------- modal logic
void ViewSettings::openSelect(SelectTarget target) {
    MeshService& ms = MeshService::getInstance();
    _selTarget = target;
    _selCount = 0;
    _selIndex = 0;
    switch (target) {
        case SelectTarget::BuzzerMode:
            _selTitle = "BUZZER MODE";
            for (int i = 0; i < NUM_BUZZER_MODES; ++i) _selLabels[i] = BUZZER_LABELS[i];
            _selCount = NUM_BUZZER_MODES;
            _selIndex = (int)BSP::getInstance().getBuzzerMode();
            break;
        case SelectTarget::ClockFace:
            _selTitle = "CLOCK FACE";
            for (int i = 0; i < NUM_CLOCK_FACES; ++i) _selLabels[i] = CLOCK_LABELS[i];
            _selCount = NUM_CLOCK_FACES;
            _selIndex = clockFaceIndex(ms);
            break;
        case SelectTarget::PowerButton:
            _selTitle = "POWER BUTTON";
            for (int i = 0; i < 4; ++i) _selLabels[i] = POWER_BTN_LABELS[i];
            _selCount = 4;
            _selIndex = ms.getPowerBtnAction() <= 3 ? ms.getPowerBtnAction() : 1;
            break;
        case SelectTarget::Region:
            _selTitle = "REGION";
            for (int i = 0; i < NUM_FREQ; ++i) _selLabels[i] = FREQ_LABELS[i];
            _selCount = NUM_FREQ;
            _selIndex = -1;
            for (int i = 0; i < NUM_FREQ; ++i) {
                if (fabsf(FREQ_VALUES[i] - ms.getFrequency()) < 0.05f) _selIndex = i;
            }
            break;
        case SelectTarget::Preset:
            _selTitle = "MODEM PRESET";
            for (int i = 0; i < NUM_PRESETS; ++i) _selLabels[i] = PRESET_LABELS[i];
            _selCount = NUM_PRESETS;
            _selIndex = (int)ms.getModemPreset();
            break;
        case SelectTarget::DutyCycle:
            _selTitle = "DUTY CYCLE";
            for (int i = 0; i < NUM_DUTY; ++i) _selLabels[i] = DUTY_LABELS[i];
            _selCount = NUM_DUTY;
            _selIndex = dutyIndexFromValue(ms.getDutyCyclePct());
            break;
        case SelectTarget::Role:
            _selTitle = "NODE ROLE";
            for (int i = 0; i < 3; ++i) _selLabels[i] = ROLE_LABELS[i];
            _selCount = 3;
            _selIndex = (int)ms.getNodeRole() <= 2 ? (int)ms.getNodeRole() : 0;
            break;
        case SelectTarget::NodeInfoInt:
            _selTitle = "NODEINFO INTERVAL";
            for (int i = 0; i < NUM_NI; ++i) _selLabels[i] = NI_LABELS[i];
            _selCount = NUM_NI;
            _selIndex = -1;
            for (int i = 0; i < NUM_NI; ++i) if (NI_VALUES[i] == ms.getNodeInfoIntervalSec()) _selIndex = i;
            break;
        case SelectTarget::TelemetryInt:
            _selTitle = "TELEMETRY INTERVAL";
            for (int i = 0; i < NUM_TEL; ++i) _selLabels[i] = TEL_LABELS[i];
            _selCount = NUM_TEL;
            _selIndex = -1;
            for (int i = 0; i < NUM_TEL; ++i) if (TEL_VALUES[i] == ms.getTelemetryIntervalSec()) _selIndex = i;
            break;
        case SelectTarget::PositionInt:
            _selTitle = "POSITION INTERVAL";
            for (int i = 0; i < NUM_POS; ++i) _selLabels[i] = POS_LABELS[i];
            _selCount = NUM_POS;
            _selIndex = -1;
            for (int i = 0; i < NUM_POS; ++i) if (POS_VALUES[i] == ms.getPositionIntervalSec()) _selIndex = i;
            break;
        case SelectTarget::NeighborInt:
            _selTitle = "NEIGHBOR INTERVAL";
            for (int i = 0; i < NUM_NB; ++i) _selLabels[i] = NB_LABELS[i];
            _selCount = NUM_NB;
            _selIndex = -1;
            for (int i = 0; i < NUM_NB; ++i) if (NB_VALUES[i] == ms.getNeighborInfoIntervalSec()) _selIndex = i;
            break;
        case SelectTarget::Rebroadcast:
            _selTitle = "REBROADCAST";
            for (int i = 0; i < 3; ++i) _selLabels[i] = RBC_LABELS[i];
            _selCount = 3;
            _selIndex = (int)ms.getRebroadcastMode() <= 2 ? (int)ms.getRebroadcastMode() : 0;
            break;
        default:
            return;
    }
    _modal = Modal::Select;
    drawModal(M5.Display);
    EPDDriver::getInstance().flushRect(SEL_X, SEL_TOP, SEL_W, selHeight(_selCount));
    noteInteraction();
}

void ViewSettings::applySelect(int option) {
    MeshService& ms = MeshService::getInstance();
    if (option >= 0 && option < _selCount) _selIndex = option;
    switch (_selTarget) {
        case SelectTarget::BuzzerMode:
            BSP::getInstance().setBuzzerMode((BSP::BuzzerMode)_selIndex);
            break;
        case SelectTarget::ClockFace:
            ms.setClockLandscape((_selIndex & 1) != 0);
            ms.setClockFlip180((_selIndex & 2) != 0);
            break;
        case SelectTarget::PowerButton:
            ms.setPowerBtnAction((uint8_t)_selIndex);
            break;
        case SelectTarget::Region:
            if (_selIndex >= 0 && _selIndex < NUM_FREQ) {
                ms.setFrequency(FREQ_VALUES[_selIndex]);
            }
            break;
        case SelectTarget::Preset:
            ms.setModemPreset((ModemPreset)_selIndex);
            break;
        case SelectTarget::DutyCycle:
            ms.setDutyCyclePct((uint8_t)DUTY_VALUES[_selIndex]);
            break;
        case SelectTarget::Role:
            ms.setNodeRoleAndPersist((NodeRole)_selIndex);
            break;
        case SelectTarget::NodeInfoInt:
            ms.setNodeInfoIntervalSec(NI_VALUES[_selIndex]);
            break;
        case SelectTarget::TelemetryInt:
            ms.setTelemetryIntervalSec(TEL_VALUES[_selIndex]);
            break;
        case SelectTarget::PositionInt:
            ms.setPositionIntervalSec(POS_VALUES[_selIndex]);
            break;
        case SelectTarget::NeighborInt:
            ms.setNeighborInfoIntervalSec(NB_VALUES[_selIndex]);
            break;
        case SelectTarget::Rebroadcast:
            ms.setRebroadcastMode((RebroadcastMode)_selIndex);
            break;
        default:
            break;
    }
    markSavePending();
}

void ViewSettings::openConfirm(ConfirmTarget target) {
    _confirmTarget = target;
    switch (target) {
        case ConfirmTarget::Reboot:
            _confirmTitle = "REBOOT";
            _confirmMsg = "Reboot the device?";
            break;
        case ConfirmTarget::PowerOff:
            _confirmTitle = "POWER OFF";
            _confirmMsg = "Power the device off?";
            break;
        case ConfirmTarget::Regenerate:
            _confirmTitle = "REGENERATE KEYS + ID";
            _confirmMsg = "New keys and a new node ID.\nPeers will see a new node.";
            break;
        case ConfirmTarget::ClearChat:
            _confirmTitle = "CLEAR CHAT";
            _confirmMsg = "Delete the whole chat history?\nThis cannot be undone.";
            break;
        case ConfirmTarget::ClearNodes:
            _confirmTitle = "CLEAR NODES";
            _confirmMsg = "Forget every discovered node?\nKeys and names are lost.";
            break;
        default:
            return;
    }
    _modal = Modal::Confirm;
    drawModal(M5.Display);
    EPDDriver::getInstance().flushRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H);
    noteInteraction();
}

void ViewSettings::applyConfirm() {
    MeshService& ms = MeshService::getInstance();
    const ConfirmTarget target = _confirmTarget;
    _modal = Modal::None;
    _confirmTarget = ConfirmTarget::None;
    switch (target) {
        case ConfirmTarget::Reboot:
            // A clean restart: the PMIC shutdown path would cut the rails before esp_restart()
            // gets a chance to run.
            MeshService::getInstance().saveConfig();
            esp_restart();
            break;
        case ConfirmTarget::PowerOff:
            ms.saveConfig();
            BSP::getInstance().powerOff("Power Off");
            break;
        case ConfirmTarget::Regenerate:
            ms.regenerateIdentity(true);
            break;
        case ConfirmTarget::ClearChat:
            ms.clearChatHistory();
            break;
        case ConfirmTarget::ClearNodes:
            ms.clearDiscoveredNodes();
            break;
        default:
            break;
    }
    // The modal is gone: repaint the page underneath it and refresh the status bar.
    drawPage(M5.Display);
    flushContentGrayscale();
    UIEngine::getInstance().requestStatusBarRedraw();
    noteInteraction();
}

bool ViewSettings::stepArrow(uint8_t button) {
    if (_modal == Modal::Time) {
        switch (button) {
            case 1: _timeH = (_timeH + 1) % 24; break;
            case 2: _timeH = (_timeH + 23) % 24; break;
            case 3: _timeM = (_timeM + 1) % 60; break;
            case 4: _timeM = (_timeM + 59) % 60; break;
            default: return false;
        }
        return true;
    }
    if (_modal == Modal::Date) {
        const int dim = daysInMonth(_dateY, _dateM);
        switch (button) {
            case 1: _dateD = (_dateD % dim) + 1; break;
            case 2: _dateD = (_dateD <= 1) ? dim : _dateD - 1; break;
            case 3: _dateM = (_dateM % 12) + 1; break;
            case 4: _dateM = (_dateM <= 1) ? 12 : _dateM - 1; break;
            case 5: if (_dateY < 2099) _dateY++; break;
            case 6: if (_dateY > 2023) _dateY--; break;
            default: return false;
        }
        const int dimAfter = daysInMonth(_dateY, _dateM);
        if (_dateD > dimAfter) _dateD = dimAfter;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- touch
bool ViewSettings::handleTouch(const TouchEvent& ev) {
    // Modals consume everything while they are up.
    if (_modal != Modal::None) return handleModalTouch(ev);
    // Phone-style back gesture: left-to-right swipe that starts at the left edge (a horizontal drag
    // in the middle of the page must not navigate away).
    if (isEdgeSwipeRight(ev) && _page != Page::Root) {
        gotoPage(Page::Root);
        return true;
    }
    switch (_page) {
        case Page::Root: return handleRootTouch(ev);
        case Page::System: return handleSystemTouch(ev);
        case Page::Radio: return handleRadioTouch(ev);
        case Page::Channels: return handleChannelsTouch(ev);
        case Page::Node: return handleNodeTouch(ev);
        case Page::Mesh: return handleMeshTouch(ev);
        case Page::DateTime: return handleDateTimeTouch(ev);
        case Page::Storage: return handleStorageTouch(ev);
        default: return false;
    }
}

bool ViewSettings::handleRootTouch(const TouchEvent& ev) {
    // Click only: an Up is also emitted for slow/long presses whose finger may have drifted into a
    // different row, and opening the category under the release point would be a misfire.
    if (ev.type != TouchEventType::Click) return false;
    for (int i = 0; i < 7; ++i) {
        const int y = CONTENT_TOP + i * (ROOT_ROW_H + ROOT_GAP);
        if (inRect(ev, CONTENT_X, y, CONTENT_W, ROOT_ROW_H)) {
            gotoPage((Page)((int)Page::System + i));
            return true;
        }
    }
    return false;
}

bool ViewSettings::handleSystemTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[SYSTEM_N];
    layoutRows(SYSTEM_ROWS, SYSTEM_N, r);

    // --- sliders (Down/Move/Up, no Click: the tap that starts the drag must not fire twice) ---
    if (ev.type == TouchEventType::Down) {
        _dragRow = -1;
        if (inRow(ev, r[0])) _dragRow = 0;
        else if (inRow(ev, r[1])) _dragRow = 1;
        if (_dragRow == 0) {
            _sliderBrightness = (uint8_t)(sliderPosFromX(ev.x, 20) * 5);
            ms.setFrontlight(_sliderBrightness);
            BSP::getInstance().setFrontlight(_sliderBrightness);
            redrawRow(0);
            return true;
        }
        if (_dragRow == 1) {
            _sliderVolume = (uint8_t)sliderPosFromX(ev.x, 3);
            BSP::getInstance().setBuzzerVolume(_sliderVolume);
            redrawRow(1);
            return true;
        }
    }
    if (ev.type == TouchEventType::Move && _dragRow >= 0) {
        // The gesture must have started inside the row: a Move that lands here after a gesture begun
        // on the nav/status bar must not hijack the slider (startX/startY are the press point).
        const RowRect& dr = r[_dragRow];
        const bool startedHere = (ev.startX >= CONTENT_X && ev.startX < CONTENT_X + CONTENT_W &&
                                  ev.startY >= dr.y && ev.startY < dr.y + dr.h);
        if (!startedHere) {
            _dragRow = -1;
            return false;
        }
        // The value follows the finger on every sample; only the panel repaint is rate-limited
        // (a flush takes 20-40 ms and repainting on each sample would starve the touch polling).
        bool needsDraw = false;
        if (_dragRow == 0) {
            const uint8_t v = (uint8_t)(sliderPosFromX(ev.x, 20) * 5);
            if (v != _sliderBrightness) {
                _sliderBrightness = v;
                ms.setFrontlight(v);
                BSP::getInstance().setFrontlight(v);
                needsDraw = true;
            }
        } else {
            const uint8_t v = (uint8_t)sliderPosFromX(ev.x, 3);
            if (v != _sliderVolume) {
                _sliderVolume = v;
                BSP::getInstance().setBuzzerVolume(v);
                needsDraw = true;
            }
        }
        const uint32_t now = millis();
        if (needsDraw && (uint32_t)(now - _lastSliderDrawMs) >= 60) {
            _lastSliderDrawMs = now;
            redrawRow(_dragRow);
        }
        return true;
    }
    // Release: TouchManager reports Click for short taps/drags and Swipe* for long ones, so Up alone
    // is not enough - the old code saved and repainted only on Up and the value was never persisted.
    if ((ev.type == TouchEventType::Up || ev.type == TouchEventType::Click ||
         ev.type == TouchEventType::SwipeLeft || ev.type == TouchEventType::SwipeRight ||
         ev.type == TouchEventType::SwipeUp || ev.type == TouchEventType::SwipeDown) &&
        _dragRow >= 0) {
        const int8_t row = _dragRow;
        _dragRow = -1;
        // Re-apply the final finger position: the throttle may have skipped the last sample.
        if (row == 0) {
            const uint8_t v = (uint8_t)(sliderPosFromX(ev.x, 20) * 5);
            _sliderBrightness = v;
            ms.setFrontlight(v);
            BSP::getInstance().setFrontlight(v);
        } else {
            const uint8_t v = (uint8_t)sliderPosFromX(ev.x, 3);
            _sliderVolume = v;
            BSP::getInstance().setBuzzerVolume(v);
            if (v > 0) BSP::getInstance().beep(BSP::NOTIFY_TONE_HZ, 60);
        }
        markSavePending();
        redrawRow(row);
        return true;
    }
    if (ev.type != TouchEventType::Click) return false;

    if (inRow(ev, r[2])) { // LED notifications
        BSP::getInstance().setLedNotifications(!BSP::getInstance().getLedNotifications());
        markSavePending();
        redrawRow(2);
        return true;
    }
    if (inRow(ev, r[3])) { openSelect(SelectTarget::BuzzerMode); return true; }
    if (inRow(ev, r[4])) { openSelect(SelectTarget::ClockFace); return true; }
    if (inRow(ev, r[5])) { openSelect(SelectTarget::PowerButton); return true; }
    if (inRow(ev, r[6])) {
        const int which = buttons3Hit(ev, r[6]);
        if (which >= 0) {
            ms.saveConfig(); // entering a low-power mode: persist everything before the rails change
            BSP::getInstance().enterLowPower(which == 0 ? BSP::PowerMode::Standby
                                               : which == 1 ? BSP::PowerMode::Clock
                                                            : BSP::PowerMode::StandbyClock);
            return true;
        }
        return false;
    }
    if (inRow(ev, r[7])) { openConfirm(ConfirmTarget::Reboot); return true; }
    if (inRow(ev, r[8])) { openConfirm(ConfirmTarget::PowerOff); return true; }
    return false;
}

bool ViewSettings::handleRadioTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[RADIO_N];
    layoutRows(RADIO_ROWS, RADIO_N, r);
    if (ev.type != TouchEventType::Click) return false;

    if (inRow(ev, r[0])) { openSelect(SelectTarget::Region); return true; }
    if (inRow(ev, r[1])) { openSelect(SelectTarget::Preset); return true; }
    if (inRow(ev, r[2])) {
        const int8_t cur = ms.getTxPower();
        int idx = 0;
        for (int i = 0; i < NUM_TX_POWERS; ++i) if (TX_POWERS[i] == cur) idx = i;
        if (stepperHitMinus(ev, r[2]) && idx > 0) idx--;
        else if (stepperHitPlus(ev, r[2]) && idx < NUM_TX_POWERS - 1) idx++;
        else return true;
        ms.setTxPower(TX_POWERS[idx]);
        markSavePending();
        redrawRow(2);
        return true;
    }
    if (inRow(ev, r[3])) {
        const uint8_t hops = ms.getHopLimit();
        uint8_t next = hops;
        if (stepperHitMinus(ev, r[3])) next = (hops <= 1) ? 1 : (uint8_t)(hops - 1);
        else if (stepperHitPlus(ev, r[3])) next = (hops >= 7) ? 7 : (uint8_t)(hops + 1);
        else return true;
        if (next != hops) {
            ms.setHopLimit(next);
            markSavePending();
            redrawRow(3);
        }
        return true;
    }
    if (inRow(ev, r[4])) { openSelect(SelectTarget::DutyCycle); return true; }
    return inRow(ev, r[5]); // info row: consume, no action
}

bool ViewSettings::handleChannelsTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[CHANNEL_N];
    layoutRows(CHANNEL_ROWS, CHANNEL_N, r);
    if (ev.type != TouchEventType::Click) return false;

    if (inRow(ev, r[0])) {
        UIEngine::getInstance().openKeyboard(ms.getPrimaryChannelNameRaw(), [](const std::string& name) {
            MeshService::getInstance().setPrimaryChannelName(name.c_str());
            MeshService::getInstance().saveConfig();
            UIEngine::getInstance().requestFullRefresh();
        });
        return true;
    }
    if (inRow(ev, r[1])) {
        char curHex[80] = "";
        if (!ms.isPrimaryChannelDefaultPsk() && ms.getPrimaryChannelPskLen() > 0) {
            const uint8_t* k = ms.getPrimaryChannelPsk();
            for (uint8_t i = 0; i < ms.getPrimaryChannelPskLen(); ++i) snprintf(curHex + i * 2, 3, "%02x", k[i]);
        }
        UIEngine::getInstance().openKeyboard(curHex, [](const std::string& hex) {
            uint8_t key[32] = {0};
            uint8_t keyLen = 0;
            bool valid = true;
            if (hex.empty()) {
                const uint8_t defKey[16] = {0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01};
                MeshService::getInstance().setPrimaryChannelPsk(defKey, 16);
            } else {
                for (size_t i = 0; i < hex.size(); ++i) {
                    if (!isxdigit((unsigned char)hex[i])) { valid = false; break; }
                }
                if (valid && (hex.size() == 32 || hex.size() == 64)) {
                    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                        key[keyLen++] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
                    }
                    MeshService::getInstance().setPrimaryChannelPsk(key, keyLen);
                } else {
                    ESP_LOGW(TAG, "Invalid PSK: 32 or 64 hex characters required");
                }
            }
            MeshService::getInstance().saveConfig();
            UIEngine::getInstance().requestFullRefresh();
        });
        return true;
    }
    if (inRow(ev, r[4])) {
        ms.setPkiEnabled(!ms.isPkiEnabled());
        markSavePending();
        ms.broadcastNodeInfo(false);
        redrawRow(4);
        return true;
    }
    if (inRow(ev, r[5])) { openSelect(SelectTarget::Role); return true; }
    if (inRow(ev, r[6])) { openConfirm(ConfirmTarget::Regenerate); return true; }
    return inRow(ev, r[2]) || inRow(ev, r[3]);
}

bool ViewSettings::handleNodeTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[NODE_N];
    layoutRows(NODE_ROWS, NODE_N, r);
    if (ev.type != TouchEventType::Click) return false;

    if (inRow(ev, r[0])) {
        UIEngine::getInstance().openKeyboard(ms.getLocalLongName(), [](const std::string& name) {
            if (!name.empty()) {
                MeshService::getInstance().setLocalName(name.c_str(), MeshService::getInstance().getLocalShortName());
                MeshService::getInstance().saveConfig();
                MeshService::getInstance().broadcastNodeInfo();
            }
            UIEngine::getInstance().requestFullRefresh();
        });
        return true;
    }
    if (inRow(ev, r[1])) {
        UIEngine::getInstance().openKeyboard(ms.getLocalShortName(), [](const std::string& shortName) {
            if (!shortName.empty()) {
                char sBuf[8] = "";
                strncpy(sBuf, shortName.c_str(), 4);
                sBuf[4] = '\0';
                for (int i = 0; sBuf[i]; ++i) sBuf[i] = toupper((unsigned char)sBuf[i]);
                MeshService::getInstance().setLocalName(MeshService::getInstance().getLocalLongName(), sBuf);
                MeshService::getInstance().saveConfig();
                MeshService::getInstance().broadcastNodeInfo();
            }
            UIEngine::getInstance().requestFullRefresh();
        });
        return true;
    }
    return inRow(ev, r[2]) || inRow(ev, r[3]) || inRow(ev, r[4]); // info rows: consume
}

bool ViewSettings::handleMeshTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[MESH_N];
    layoutRows(MESH_ROWS, MESH_N, r);
    if (ev.type != TouchEventType::Click) return false;

    if (inRow(ev, r[0])) { openSelect(SelectTarget::NodeInfoInt); return true; }
    if (inRow(ev, r[1])) { openSelect(SelectTarget::TelemetryInt); return true; }
    if (inRow(ev, r[2])) { openSelect(SelectTarget::PositionInt); return true; }
    if (inRow(ev, r[3])) { openSelect(SelectTarget::NeighborInt); return true; }
    if (inRow(ev, r[4])) { openSelect(SelectTarget::Rebroadcast); return true; }
    if (inRow(ev, r[5])) {
        ms.setMeshTimeSync(!ms.getMeshTimeSync());
        markSavePending();
        redrawRow(5);
        return true;
    }
    if (inRow(ev, r[6])) {
        ms.broadcastNodeInfo();
        UIEngine::getInstance().requestStatusBarRedraw();
        BSP::getInstance().click();
        return true;
    }
    return false;
}

bool ViewSettings::handleDateTimeTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[DATETIME_N];
    layoutRows(DATETIME_ROWS, DATETIME_N, r);

    if (ev.type == TouchEventType::Click) {
        if (inRow(ev, r[0])) {
            int8_t tz = ms.getTimezoneOffset();
            if (stepperHitMinus(ev, r[0]) && tz > -12) tz--;
            else if (stepperHitPlus(ev, r[0]) && tz < 14) tz++;
            else return true;
            ms.setTimezoneOffset(tz);
            markSavePending();
            redrawRow(0);
            UIEngine::getInstance().requestStatusBarRedraw();
            return true;
        }
        if (inRow(ev, r[1])) { // set time
            int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0;
            BSP::getInstance().getLocalDateTime(yy, mo, dd, hh, mm, ss);
            if (!BSP::getInstance().isRtcValid()) {
                _dateY = 2026; _dateM = 1; _dateD = 1;
                _timeH = 12; _timeM = 0;
            } else {
                _timeH = hh; _timeM = mm;
            }
            _modal = Modal::Time;
            drawModal(M5.Display);
            EPDDriver::getInstance().flushRect(TIME_X, TIME_Y, TIME_W, TIME_H);
            noteInteraction();
            return true;
        }
        if (inRow(ev, r[2])) { // set date
            int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0;
            BSP::getInstance().getLocalDateTime(yy, mo, dd, hh, mm, ss);
            if (!BSP::getInstance().isRtcValid()) {
                _dateY = 2026; _dateM = 1; _dateD = 1;
            } else {
                _dateY = yy; _dateM = mo; _dateD = dd;
            }
            if (_dateY < 2023) _dateY = 2026;
            if (_dateM < 1 || _dateM > 12) _dateM = 1;
            if (_dateD < 1 || _dateD > daysInMonth(_dateY, _dateM)) _dateD = 1;
            _modal = Modal::Date;
            drawModal(M5.Display);
            EPDDriver::getInstance().flushRect(DATE_X, DATE_Y, DATE_W, DATE_H);
            noteInteraction();
            return true;
        }
        return inRow(ev, r[3]);
    }
    return false;
}

bool ViewSettings::handleStorageTouch(const TouchEvent& ev) {
    MeshService& ms = MeshService::getInstance();
    RowRect r[STORAGE_N];
    layoutRows(STORAGE_ROWS, STORAGE_N, r);
    if (ev.type != TouchEventType::Click) return false;

    if (inRow(ev, r[1])) { // save all
        ms.saveConfig();
        BSP::getInstance().click();
        return true;
    }
    if (inRow(ev, r[2])) { openConfirm(ConfirmTarget::ClearChat); return true; }
    if (inRow(ev, r[3])) { openConfirm(ConfirmTarget::ClearNodes); return true; }
    if (inRow(ev, r[4])) { // refresh screen: full quality pass (same as BtnB hold)
        EPDDriver::getInstance().fullClear();
        UIEngine::getInstance().requestFullRefresh();
        return true;
    }
    return inRow(ev, r[0]) || inRow(ev, r[5]) || inRow(ev, r[6]) || inRow(ev, r[7]) || inRow(ev, r[8]) ||
           inRow(ev, r[9]);
}

bool ViewSettings::handleModalTouch(const TouchEvent& ev) {
    switch (_modal) {
        case Modal::Select: {
            if (ev.type != TouchEventType::Click) return true;
            const int hit = selectModalHit(ev, _selCount);
            if (hit >= 0) {
                applySelect(hit);
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                noteInteraction();
            } else {
                // Tap outside the card closes it (the chosen value stays).
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                noteInteraction();
            }
            return true;
        }
        case Modal::Confirm: {
            if (ev.type != TouchEventType::Click) return true;
            if (confirmHitOk(ev)) {
                applyConfirm();
            } else if (confirmHitCancel(ev) || !inConfirmCard(ev)) {
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                noteInteraction();
            }
            return true;
        }
        case Modal::Time: {
            if (ev.type == TouchEventType::Down) {
                // Up/down arrows: hours and minutes.
                const int centers[2] = {TIME_HOUR_CX, TIME_MIN_CX};
                for (int i = 0; i < 2; ++i) {
                    const int bx = centers[i] - TIME_ARROW_W / 2;
                    if (inRect(ev, bx, TIME_UP_Y, TIME_ARROW_W, TIME_ARROW_H)) {
                        _arrowHeld = (uint8_t)(1 + i * 2);
                        _arrowNextRepeatMs = millis() + 450;
                        if (stepArrow(_arrowHeld)) {
                            drawTimeModal(M5.Display, true);
                            EPDDriver::getInstance().flushRect(TIME_DIGITS_X, TIME_DIGITS_Y, TIME_DIGITS_W, TIME_DIGITS_H);
                            noteInteraction();
                        }
                        return true;
                    }
                    if (inRect(ev, bx, TIME_DOWN_Y, TIME_ARROW_W, TIME_ARROW_H)) {
                        _arrowHeld = (uint8_t)(2 + i * 2);
                        _arrowNextRepeatMs = millis() + 450;
                        if (stepArrow(_arrowHeld)) {
                            drawTimeModal(M5.Display, true);
                            EPDDriver::getInstance().flushRect(TIME_DIGITS_X, TIME_DIGITS_Y, TIME_DIGITS_W, TIME_DIGITS_H);
                            noteInteraction();
                        }
                        return true;
                    }
                }
                return true; // swallows taps on the card outside the controls
            }
            if (ev.type == TouchEventType::Up) {
                _arrowHeld = 0;
                return true;
            }
            if (ev.type != TouchEventType::Click) return true;
            // OK / Cancel
            if (inRect(ev, TIME_X + 18, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H)) {
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                noteInteraction();
                return true;
            }
            if (inRect(ev, TIME_X + TIME_W - 18 - TIME_BTN_W, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H)) {
                int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0;
                BSP::getInstance().getLocalDateTime(yy, mo, dd, hh, mm, ss);
                if (!BSP::getInstance().isRtcValid()) {
                    yy = _dateY; mo = _dateM; dd = _dateD;
                    if (yy < 2023) { yy = 2026; mo = 1; dd = 1; }
                }
                BSP::getInstance().setLocalDateTime(yy, mo, dd, _timeH, _timeM, 0);
                MeshService::getInstance().saveConfig(false);
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                UIEngine::getInstance().requestStatusBarRedraw();
                noteInteraction();
                return true;
            }
            return true;
        }
        case Modal::Date: {
            if (ev.type == TouchEventType::Down) {
                const int centers[3] = {DATE_DD_CX, DATE_MM_CX, DATE_YY_CX};
                const int widths[3] = {100, 100, 130};
                for (int i = 0; i < 3; ++i) {
                    const int bx = centers[i] - widths[i] / 2;
                    if (inRect(ev, bx, TIME_UP_Y, widths[i], TIME_ARROW_H)) {
                        _arrowHeld = (uint8_t)(1 + i * 2);
                        _arrowNextRepeatMs = millis() + 450;
                        if (stepArrow(_arrowHeld)) {
                            drawDateModal(M5.Display, true);
                            EPDDriver::getInstance().flushRect(DATE_DIGITS_X, DATE_DIGITS_Y, DATE_DIGITS_W, DATE_DIGITS_H);
                            noteInteraction();
                        }
                        return true;
                    }
                    if (inRect(ev, bx, TIME_DOWN_Y, widths[i], TIME_ARROW_H)) {
                        _arrowHeld = (uint8_t)(2 + i * 2);
                        _arrowNextRepeatMs = millis() + 450;
                        if (stepArrow(_arrowHeld)) {
                            drawDateModal(M5.Display, true);
                            EPDDriver::getInstance().flushRect(DATE_DIGITS_X, DATE_DIGITS_Y, DATE_DIGITS_W, DATE_DIGITS_H);
                            noteInteraction();
                        }
                        return true;
                    }
                }
                return true;
            }
            if (ev.type == TouchEventType::Up) {
                _arrowHeld = 0;
                return true;
            }
            if (ev.type != TouchEventType::Click) return true;
            if (inRect(ev, DATE_X + 18, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H)) {
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                noteInteraction();
                return true;
            }
            if (inRect(ev, DATE_X + DATE_W - 18 - TIME_BTN_W, TIME_BTN_Y, TIME_BTN_W, MODAL_BTN_H)) {
                int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0;
                BSP::getInstance().getLocalDateTime(yy, mo, dd, hh, mm, ss);
                if (!BSP::getInstance().isRtcValid()) { hh = _timeH; mm = _timeM; }
                BSP::getInstance().setLocalDateTime(_dateY, _dateM, _dateD, hh, mm, 0);
                MeshService::getInstance().saveConfig(false);
                _modal = Modal::None;
                drawPage(M5.Display);
                flushContentGrayscale();
                UIEngine::getInstance().requestStatusBarRedraw();
                noteInteraction();
                return true;
            }
            return true;
        }
        default:
            return true;
    }
}

} // namespace MonoMesh
