#include "alarm_service.h"
#include "bsp_papermono.h"
#include <esp_log.h>
#include <Preferences.h>
#include <cstring>

static constexpr const char* TAG = "MonoMesh-Alarm";

namespace MonoMesh {

namespace {

const char* toneName(uint8_t tone) {
    switch (tone) {
        case 0: return "gentle";
        case 2: return "strong";
        default: return "calm";
    }
}

int daysInMonth(int year, int month) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (month == 2) {
        const bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return d[month - 1];
}

// Howard Hinnant's days_from_civil: serial day number, used only for "which alarm comes first".
int32_t toSerialDays(int y, int m, int d) {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

void addDays(int& y, int& m, int& d, int days) {
    d += days;
    while (d > daysInMonth(y, m)) {
        d -= daysInMonth(y, m);
        if (++m > 12) {
            m = 1;
            ++y;
        }
    }
}

// Monday-indexed bit of an RTC weekday (RTC: 0 = Sunday ... 6 = Saturday).
inline uint8_t mondayBit(int rtcWeekday) {
    return (uint8_t)((rtcWeekday + 6) % 7);
}

bool alarmMatches(const AlarmRecord& a, int y, int mo, int d, int rtcWeekday, int hh, int mm) {
    if (!a.enabled || a.hour != hh || a.minute != mm) return false;
    if (a.repeat == 0) return (a.year == y && a.month == mo && a.day == d);
    if (a.daysMask == 0) return false;
    return (a.daysMask & (1u << mondayBit(rtcWeekday))) != 0;
}

} // namespace

// The "already fired in this minute" marker must survive an unexpected reboot inside the same
// minute: checkNow() would otherwise ring the alarm a second time right after a crash/reset.
static void persistFiredKey(int32_t key) {
    Preferences p;
    if (p.begin("monomesh", false)) {
        p.putInt("firedMin", key);
        p.end();
    }
}

static int32_t loadFiredKey() {
    Preferences p;
    int32_t v = -1;
    if (p.begin("monomesh", true)) {
        v = p.getInt("firedMin", -1);
        p.end();
    }
    return v;
}

void AlarmService::sanitize(AlarmRecord& a) {
    a.name[sizeof(a.name) - 1] = '\0';
    for (char* p = a.name; *p; ++p) {
        if (*p == '|' || *p == '\n' || *p == '\r') *p = ' ';
    }
    if (a.hour > 23) a.hour = 23;
    if (a.minute > 59) a.minute = 59;
    if (a.repeat > 1) a.repeat = 1;
    if (a.tone > 2) a.tone = 1;
    if (a.repeat == 0) {
        if (a.month < 1 || a.month > 12) a.month = 1;
        if (a.year < 2023 || a.year > 2099) {
            // Current year, not a hardcoded fallback: "2026" silently breaks every repaired date
            // as soon as the clock passes it.
            int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
            BSP::getInstance().getLocalDateTime(y, mo, d, hh, mm, ss);
            a.year = (y >= 2023 && y <= 2099) ? (uint16_t)y : 2026;
        }
        if (a.day < 1 || a.day > (uint8_t)daysInMonth(a.year, a.month)) {
            a.day = (uint8_t)daysInMonth(a.year, a.month);
        }
    }
}

void AlarmService::init() {
    _alarms.clear();
    _ringing = false;
    _ringingIndex = 0;
    _firedKey = loadFiredKey();
    StorageManager::getInstance().loadAlarms(_alarms);
    if (_alarms.size() > MAX_ALARMS) _alarms.resize(MAX_ALARMS);

    // A boot caused by the RTC/PM1 wake means an armed alarm just fired while the board was off:
    // find the alarm due at this minute and ring it (M5Unified already cleared the RTC IRQ; the PM1
    // wake source is what tells us).
    if (BSP::getInstance().wasWokenByRtcAlarm()) {
        if (!BSP::getInstance().isRtcValid()) {
            ESP_LOGW(TAG, "RTC alarm wake but the clock is invalid: not ringing");
        } else {
            int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
            BSP::getInstance().getLocalDateTime(y, mo, d, hh, mm, ss);
            int wd = BSP::getInstance().getLocalWeekday();
            const int wakeHh = hh, wakeMm = mm;
            // The boot (SD mount, radio init, e-paper splash) takes seconds: if it crosses the
            // minute the RTC alarm fired in, the exact-minute match below would never hit and the
            // alarm would be silently lost. Try the previous minute too (same day; a wake across
            // midnight is rare enough to skip).
            for (int back = 0; back < 2 && !_ringing; ++back) {
                for (size_t i = 0; i < _alarms.size(); ++i) {
                    if (alarmMatches(_alarms[i], y, mo, d, wd, hh, mm)) {
                        ESP_LOGI(TAG, "Power-on wake by the RTC alarm: ringing '%s'", _alarms[i].name);
                        startRinging(i);
                        _firedKey = ((int32_t)toSerialDays(y, mo, d) * 1440) + wakeHh * 60 + wakeMm;
                        persistFiredKey(_firedKey);
                        break;
                    }
                }
                if (_ringing || back == 1) break;
                if (--mm < 0) {
                    if (hh == 0) break;   // previous day not covered
                    mm = 59;
                    --hh;
                }
            }
            if (!_ringing) {
                ESP_LOGW(TAG, "RTC alarm wake, but no enabled alarm matches %02d:%02d", wakeHh, wakeMm);
            }
        }
    }
    ESP_LOGI(TAG, "Alarm service ready: %u alarm(s)", (unsigned)_alarms.size());
}

void AlarmService::update() {
    if (_ringing) {
        _previewing = false; // an alarm always wins over a preview
        tickTone();
        return;
    }
    tickPreview();
    checkNow();
}

void AlarmService::pollLowPower() {
    tickTone();
    if (_ringing) return;
    const uint32_t now = millis();
    if (now - _lastPollMs < LOW_POWER_POLL_MS) return;
    _lastPollMs = now;
    checkNow();
}

void AlarmService::checkNow() {
    if (_ringing || _alarms.empty()) return;
    // The RTC may not be set yet (or was lost): never fire against a bogus clock.
    if (!BSP::getInstance().isRtcValid()) return;

    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    BSP::getInstance().getLocalDateTime(y, mo, d, hh, mm, ss);
    const int32_t key = ((int32_t)toSerialDays(y, mo, d) * 1440) + hh * 60 + mm;
    if (key == _firedKey) return; // already fired (and maybe dismissed) in this minute

    const int wd = BSP::getInstance().getLocalWeekday();
    for (size_t i = 0; i < _alarms.size(); ++i) {
        if (!alarmMatches(_alarms[i], y, mo, d, wd, hh, mm)) continue;
        startRinging(i);
        _firedKey = key;
        persistFiredKey(key);
        return;
    }
}

void AlarmService::startRinging(size_t index) {
    if (_ringing || index >= _alarms.size()) return;
    _ringing = true;
    _ringingIndex = index;
    _toneStep = 0;
    _previewing = false;
    // First note delayed by one redraw: the popup (UI) or the clock face (standby/clock) is a
    // blocking e-paper refresh, and a tone started right before it would be stretched.
    _toneNextMs = millis() + FIRST_TONE_DELAY_MS;

    const AlarmRecord& a = _alarms[index];
    tonePattern((AlarmTone)(a.tone <= 2 ? a.tone : 1), _toneSteps, _toneCount);
    ESP_LOGI(TAG, "Alarm '%s' %02u:%02u (%s) -> RINGING",
             a.name, (unsigned)a.hour, (unsigned)a.minute, toneName(a.tone));

    // A one-date alarm fires once: disable it now (never on dismiss, so a reboot mid-ring keeps it
    // off) and persist. A weekly alarm stays enabled for the next occurrence.
    if (a.repeat == 0) {
        _alarms[index].enabled = false;
        save();
        ESP_LOGI(TAG, "One-date alarm '%s' disabled after firing", a.name);
    }
}

void AlarmService::dismiss() {
    if (!_ringing) return;
    _ringing = false;
    _ringingIndex = 0;
    _toneStep = 0;
    _toneNextMs = 0;
    BSP::getInstance().stopBuzzer();
    ESP_LOGI(TAG, "Alarm dismissed");
}

const char* AlarmService::ringingName() const {
    if (_ringingIndex < _alarms.size()) return _alarms[_ringingIndex].name;
    return "ALARM";
}

bool AlarmService::ringBacklight() const {
    if (_ringingIndex < _alarms.size()) return _alarms[_ringingIndex].backlight;
    return false;
}

void AlarmService::tonePattern(AlarmTone tone, const ToneStep*& steps, uint8_t& count) {
    // Each step is one note: play freq for onMs, then stay quiet for offMs. The last off is the pause
    // before the pattern loops. The notes are long and the gaps short: the alarm must dominate the
    // room, not chirp politely. Same patterns for the preview, so what you pick is what rings.
    static const ToneStep GENTLE[] = {
        {2700, 500, 260},
        {2700, 500, 900},
    };
    static const ToneStep CALM[] = {
        {2700, 420, 70},
        {2500, 420, 70},
        {2700, 420, 620},
    };
    static const ToneStep STRONG[] = {
        {2850, 380, 40},
        {2400, 380, 40},
        {2850, 380, 40},
        {2400, 380, 40},
    };
    switch (tone) {
        case AlarmTone::Gentle: steps = GENTLE; count = sizeof(GENTLE) / sizeof(GENTLE[0]); break;
        case AlarmTone::Strong: steps = STRONG; count = sizeof(STRONG) / sizeof(STRONG[0]); break;
        default: steps = CALM; count = sizeof(CALM) / sizeof(CALM[0]); break;
    }
}

void AlarmService::tickTone() {
    if (!_ringing || _toneSteps == nullptr || _toneCount == 0) return;
    const uint32_t now = millis();
    if ((int32_t)(now - _toneNextMs) < 0) return;
    const ToneStep& st = _toneSteps[_toneStep];
    BSP::getInstance().alarmTone(st.freq, st.onMs);
    _toneNextMs = now + st.onMs + st.offMs;
    _toneStep = (uint8_t)((_toneStep + 1) % _toneCount);
}

// Editor preview: the FULL pattern (one pass), so the ringtone you select is the one that will ring.
void AlarmService::previewTone(AlarmTone tone) {
    tonePattern(tone, _previewSteps, _previewCount);
    _previewStep = 0;
    _previewing = (_previewCount > 0);
    if (_previewing) {
        // Same intro delay as a real alarm: the modal close flushes the panel (blocking) and a note
        // started right before it would come out stretched.
        _previewNextMs = millis() + FIRST_TONE_DELAY_MS;
    }
}

void AlarmService::tickPreview() {
    if (!_previewing) return;
    const uint32_t now = millis();
    if ((int32_t)(now - _previewNextMs) < 0) return;
    if ((int32_t)(now - _previewNextMs) > 4000) {
        _previewing = false; // stale (e.g. the device went through a low-power mode): drop it
        return;
    }
    if (_previewStep >= _previewCount) {
        _previewing = false; // one pass done, no loop
        return;
    }
    const ToneStep& st = _previewSteps[_previewStep];
    BSP::getInstance().alarmTone(st.freq, st.onMs);
    _previewNextMs = now + st.onMs + st.offMs;
    ++_previewStep;
}

uint32_t AlarmService::msToNextTone() const {
    if (!_ringing) return 0;
    const uint32_t now = millis();
    if ((int32_t)(now - _toneNextMs) >= 0) return 1;
    return _toneNextMs - now;
}

int AlarmService::addAlarm(const AlarmRecord& a) {
    if (_alarms.size() >= MAX_ALARMS) return -1;
    AlarmRecord copy = a;
    sanitize(copy);
    _alarms.push_back(copy);
    save();
    ESP_LOGI(TAG, "Alarm added: '%s' %02u:%02u (%u total)", copy.name, (unsigned)copy.hour,
             (unsigned)copy.minute, (unsigned)_alarms.size());
    return (int)_alarms.size() - 1;
}

bool AlarmService::updateAlarm(size_t index, const AlarmRecord& a) {
    if (index >= _alarms.size()) return false;
    AlarmRecord copy = a;
    sanitize(copy);
    _alarms[index] = copy;
    return true; // the caller persists (debounced)
}

bool AlarmService::removeAlarm(size_t index) {
    if (index >= _alarms.size()) return false;
    ESP_LOGI(TAG, "Alarm removed: '%s'", _alarms[index].name);
    _alarms.erase(_alarms.begin() + (int)index);
    save();
    return true;
}

bool AlarmService::setEnabled(size_t index, bool enabled) {
    if (index >= _alarms.size()) return false;
    if (_alarms[index].enabled == enabled) return true;
    _alarms[index].enabled = enabled;
    save();
    return true;
}

void AlarmService::save() {
    StorageManager::getInstance().saveAlarms(_alarms);
}

bool AlarmService::armRtcAlarm() {
    if (_alarms.empty()) return false;
    if (!BSP::getInstance().isRtcValid()) {
        ESP_LOGW(TAG, "RTC not valid: power-off alarm not armed");
        return false;
    }

    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    BSP::getInstance().getLocalDateTime(y, mo, d, hh, mm, ss);
    const int wd = BSP::getInstance().getLocalWeekday(); // 0 = Sunday
    const int wdMon = mondayBit(wd);
    const int today = toSerialDays(y, mo, d);
    const int nowMinutes = hh * 60 + mm;

    int bestIdx = -1;
    int bestDelta = 0;      // days from today
    int bestDayOfMonth = 0; // one-date: day of month for the RX8130 day alarm
    int bestWeekday = -1;   // weekly: RTC weekday for the RX8130 week alarm

    for (size_t i = 0; i < _alarms.size(); ++i) {
        const AlarmRecord& a = _alarms[i];
        if (!a.enabled || !a.powerWake) continue;

        if (a.repeat == 1) {
            if (a.daysMask == 0) continue;
            int bestK = -1;
            for (int k = 0; k < 7; ++k) {
                if (!(a.daysMask & (1u << ((wdMon + k) % 7)))) continue;
                if (k == 0 && (a.hour * 60 + a.minute) <= nowMinutes) continue; // today already past
                bestK = k;
                break; // nearest occurrence of this weekly alarm
            }
            // Today selected but its time already passed: the next occurrence is in 7 days (same
            // weekday), which the RX8130 week alarm can still represent.
            if (bestK < 0 && (a.daysMask & (1u << wdMon))) bestK = 7;
            if (bestK < 0) continue;
            if (bestIdx < 0 || bestK < bestDelta) {
                bestIdx = (int)i;
                bestDelta = bestK;
                bestDayOfMonth = 0; // 0 -> the RX8130 week alarm is the one to use
                bestWeekday = (wd + bestK) % 7;
            }
        } else {
            const int delta = toSerialDays(a.year, a.month, a.day) - today;
            if (delta < 0) continue; // past date: it will never fire again
            if (delta == 0 && (a.hour * 60 + a.minute) <= nowMinutes) continue;
            // The RX8130 day alarm matches the day of month only (month/year are ignored): it can
            // represent this date only when the next occurrence of that day of month IS the target
            // date. A farther date (e.g. +40 days) must not arm "day 20" and ring a month early; the
            // alarm still rings normally while the device is on, it just cannot wake from power off.
            // The scan must start from *today*: skipping it meant that arming a next-month date while
            // today shares its day of month (15/01 for a 15/02 alarm) rang tonight, a month early.
            int cy = y, cm = mo, cd = d;
            bool dayMatches = false;
            for (int guard = 0; guard < 32; ++guard) {
                if (cd == a.day) {
                    dayMatches = (cy == a.year && cm == a.month && cd == a.day);
                    break;
                }
                addDays(cy, cm, cd, 1);
            }
            if (!dayMatches) {
                ESP_LOGW(TAG, "Alarm '%s' is beyond the RTC day-alarm range: not a power-on wake", a.name);
                continue;
            }
            if (bestIdx < 0 || delta < bestDelta) {
                bestIdx = (int)i;
                bestDelta = delta;
                bestDayOfMonth = a.day;
                bestWeekday = -1;
            }
        }
    }

    if (bestIdx < 0) {
        ESP_LOGI(TAG, "No enabled power-wake alarm: normal power off");
        disarmRtcAlarm();
        return false;
    }

    const AlarmRecord& a = _alarms[bestIdx];
    ESP_LOGI(TAG, "Arming RTC alarm for '%s' %02u:%02u in %d day(s) (power-wake)",
             a.name, (unsigned)a.hour, (unsigned)a.minute, bestDelta);
    return BSP::getInstance().armRtcAlarmAt(a.hour, a.minute, bestDayOfMonth, bestWeekday);
}

void AlarmService::disarmRtcAlarm() {
    BSP::getInstance().disarmRtcAlarm();
}

} // namespace MonoMesh
