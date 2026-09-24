#pragma once

#include <Arduino.h>
#include <vector>
#include "storage_manager.h"

namespace MonoMesh {

// The three buzzer ringtones. They differ in rhythm and pitch (the buzzer is always audible: the
// alarm ignores the buzzer mute and the buzzer mode on purpose).
enum class AlarmTone : uint8_t {
    Gentle = 0,
    Calm = 1,
    Strong = 2
};

// Alarm clock engine: owns the alarm list, matches the RTC wall clock (the same time base shown by
// the status bar and the clock face) and drives the ringtone. The visual side (popup in the UI,
// clock face + name in standby/clock) is owned by UIEngine and main.cpp, which follow isRinging().
//
// Trigger rule: an enabled alarm whose hour/minute match the RTC minute fires once; until the minute
// changes no other alarm can fire (two alarms on the same minute: the first one rings). A one-date
// alarm disables itself when it starts ringing, never when it is dismissed.
class AlarmService {
public:
    static AlarmService& getInstance() {
        static AlarmService instance;
        return instance;
    }

    // Load the list from storage and, when the boot was caused by the RTC alarm (PM1 wake), start
    // ringing the alarm that was due at this minute.
    void init();

    // Normal UI loop: match + ringtone player. Cheap when idle (one RTC read per loop, which the UI
    // already performs anyway for the status bar).
    void update();

    // Low-power loop: same as update(), but the RTC is read at most every LOW_POWER_POLL_MS. The
    // light-sleep slices already wake the CPU every 100 ms (standby) or at the minute boundary
    // (clock), so this is the only added cost of the alarm feature and it is negligible.
    void pollLowPower();

    bool isRinging() const { return _ringing; }
    const char* ringingName() const;
    // The ringing alarm asked for the frontlight while in standby/clock.
    bool ringBacklight() const;

    const std::vector<AlarmRecord>& alarms() const { return _alarms; }
    // CRUD. add() returns the index of the new alarm, or -1 when the list is full.
    int addAlarm(const AlarmRecord& a);
    bool updateAlarm(size_t index, const AlarmRecord& a);
    bool removeAlarm(size_t index);
    bool setEnabled(size_t index, bool enabled);
    // Persist now (the editor debounces its own saves and calls this on exit).
    void save();

    // Single ringtone note for the editor preview (does not loop).
    void previewTone(AlarmTone tone);

    // Milliseconds until the next scheduled note while ringing (0 when nothing is scheduled): the
    // low-power loop sleeps exactly up to it, so the pattern is not chopped by the sleep slices.
    uint32_t msToNextTone() const;

    // Stop the ringtone and the visual alarm state. Safe to call when not ringing.
    void dismiss();

    // Power-off integration: program the RX8130 alarm for the next enabled powerWake alarm and arm
    // the PM1 G0 line so the shut-down board is powered back on at the alarm time. Returns false when
    // there is nothing to arm (no such alarm, RTC not usable).
    bool armRtcAlarm();
    void disarmRtcAlarm();

    static constexpr uint32_t LOW_POWER_POLL_MS = 500;
    // Frontlight used while ringing when the user's brightness is 0 (a dark room must still show
    // which alarm is going off).
    static constexpr uint8_t DEFAULT_BACKLIGHT_PERCENT = 30;
    // Delay before the first note: the popup/clock face redraw is a blocking e-paper refresh, so the
    // tone would otherwise start right before it and its first note would be stretched.
    static constexpr uint32_t FIRST_TONE_DELAY_MS = 1000;

private:
    AlarmService() = default;
    ~AlarmService() = default;

    struct ToneStep {
        uint16_t freq;
        uint16_t onMs;
        uint16_t offMs;
    };

    // Last fired alarm minute, as a day*1440+minute key: suppresses re-triggers within the minute and
    // keeps two same-minute alarms from ringing one after the other.
    void checkNow();
    void startRinging(size_t index);
    void tickTone();
    void tickPreview();
    static void tonePattern(AlarmTone tone, const ToneStep*& steps, uint8_t& count);
    static void sanitize(AlarmRecord& a);

    std::vector<AlarmRecord> _alarms;
    bool _ringing = false;
    size_t _ringingIndex = 0;
    uint32_t _lastPollMs = 0;
    int32_t _firedKey = -1;

    const ToneStep* _toneSteps = nullptr;
    uint8_t _toneCount = 0;
    uint8_t _toneStep = 0;
    uint32_t _toneNextMs = 0;

    // Editor preview: one full pattern pass, no loop.
    bool _previewing = false;
    const ToneStep* _previewSteps = nullptr;
    uint8_t _previewCount = 0;
    uint8_t _previewStep = 0;
    uint32_t _previewNextMs = 0;
};

} // namespace MonoMesh
