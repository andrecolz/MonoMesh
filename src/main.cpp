#include <Arduino.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <WiFi.h>
#include "alarm_service.h"
#include "bsp_papermono.h"
#include "clock_mode.h"
#include "epd_driver.h"
#include "touch_manager.h"
#include "mesh_service.h"
#include "storage_manager.h"
#include "ui_engine.h"

static constexpr const char* TAG = "MonoMesh-Main";

// Boot counter in the RTC domain: it survives software resets but not a power
// cycle. If it grows on every wake attempt, the board is resetting instead of
// resuming from light sleep (the "red LED blinks, nothing happens" symptom).
RTC_DATA_ATTR static uint32_t s_bootCount = 0;

static const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// Clock peek (CLOCK / CLOCK+RX): BtnA/BtnB light the frontlight for a few seconds instead of
// leaving the mode. 25% unless the user set a different non-zero brightness.
static constexpr uint8_t CLOCK_PEEK_BRIGHTNESS = 25;
static constexpr uint32_t CLOCK_PEEK_MS = 3500;
static constexpr uint32_t CLOCK_PEEK_SLICE_MS = 250; // sleep cap so the light goes off on time

using namespace MonoMesh;

void setup() {
    Serial.begin(115200);
    delay(200);

    // Disable WiFi and Bluetooth to minimize ESP32-S3 power consumption
    WiFi.mode(WIFI_OFF);
    btStop();

    if (Serial) {
        Serial.println("\n\n=================================================");
        Serial.printf("  MonoMesh v%s (Meshtastic Standalone)         \n", MONOMESH_VERSION);
        Serial.println("  Hardware: M5Stack Paper Mono (ESP32-S3 + SX1262)");
        Serial.println("=================================================");
    }
    ESP_LOGI(TAG, "Booting MonoMesh...");
    ++s_bootCount;
    ESP_LOGI(TAG, "Reset reason: %s (%d), boot #%u",
             resetReasonName(esp_reset_reason()), (int)esp_reset_reason(),
             (unsigned)s_bootCount);

    // Retrieve saved brightness from NVS before initializing display to prevent 100% blast
    uint8_t initBrightness = StorageManager::getQuickFrontlight();

    // 1. Initialize Board Support Package (Power rails, I2C, M5PM1, M5IOE1) with saved brightness
    if (!BSP::getInstance().init(initBrightness)) {
        ESP_LOGE(TAG, "BSP Initialization failed!");
    }

    // 2. Initialize Storage Subsystem (NVS + MicroSD SDMMC)
    if (!StorageManager::getInstance().init()) {
        ESP_LOGW(TAG, "StorageManager partial init");
    }

    // Ensure buttons have internal pull-ups enabled
    pinMode(GPIO_NUM_2, INPUT_PULLUP);
    pinMode(GPIO_NUM_3, INPUT_PULLUP);

    // 3. Initialize E-Paper Driver (SSD1677)
    if (!EPDDriver::getInstance().init()) {
        ESP_LOGE(TAG, "EPD Driver Initialization failed!");
    }

    // 4. Initialize Touch Manager (FT6336G)
    if (!TouchManager::getInstance().init()) {
        ESP_LOGE(TAG, "Touch Manager Initialization failed!");
    }

    // 5. Initialize LoRa Meshtastic Service (SX1262)
    if (!MeshService::getInstance().init()) {
        ESP_LOGE(TAG, "Mesh Service Initialization failed!");
    }

    // 6. Initialize UI Engine and Views
    if (!UIEngine::getInstance().init()) {
        ESP_LOGE(TAG, "UI Engine Initialization failed!");
    }

    // 7. Alarms (after storage and UI: a boot caused by the RTC alarm rings immediately through the
    // UI popup). The RTC wake source was latched by the BSP init before anything cleared it.
    AlarmService::getInstance().init();

    // Ensure BSP frontlight matches loaded configuration
    BSP::getInstance().setFrontlight(MeshService::getInstance().getFrontlight());

    // Initial render
    UIEngine::getInstance().render();

    ESP_LOGI(TAG, "MonoMesh System Ready! Node ID: %s (%s)",
             MeshService::getInstance().getLocalIdStr(),
             MeshService::getInstance().getLocalLongName());
}

// ---- Low-battery protection ----
// The warning is drawn by the status bar (BSP::LOW_BATTERY_WARN_MV); this is the cutoff. Three
// consecutive samples below BSP::LOW_BATTERY_CUTOFF_MV while on battery: a TX pulse (or a cold cell)
// can sag the rail for a moment, so a single dip must not shut the device down. On VIN the voltage
// is the charger's, so charging always wins over the cutoff.
static void checkLowBattery() {
    static uint32_t s_lastCheck = 0;
    static uint8_t s_lowSamples = 0;
    const uint32_t now = millis();
    if (now - s_lastCheck < 10000) return;
    s_lastCheck = now;
    // Timer-based: a "millis() < 60000" test re-armed itself for a minute every time millis() wrapped.
    // 20 s is enough for the boot/USB transients to settle (charging state is checked right below).
    if (esp_timer_get_time() < 20000000LL) return;

    BatteryState bs = BSP::getInstance().getBatteryState();
    if (bs.isCharging) {
        s_lowSamples = 0;
        return;
    }
    if (bs.voltageMv > 0 && bs.voltageMv <= BSP::LOW_BATTERY_CUTOFF_MV) {
        if (++s_lowSamples >= 3) {
            s_lowSamples = 0;
            ESP_LOGW(TAG, "Battery at %d mV (<= %d mV): low-battery shutdown",
                     bs.voltageMv, BSP::LOW_BATTERY_CUTOFF_MV);
            MeshService::getInstance().saveConfig();
            BSP::getInstance().lowBatteryShutdown();
        }
    } else {
        s_lowSamples = 0;
    }
}

// ---- Low-power modes: Standby (radio listening), Clock (radio off), Standby+Clock ----
// Keeps the buttons serviced, updates the clock face when the minute changes and sleeps the CPU in
// timer slices. Returns to the normal loop when the mode is left.
static void handleLowPowerMode() {
    BSP& bsp = BSP::getInstance();
    MeshService& mesh = MeshService::getInstance();
    const BSP::PowerMode mode = bsp.getPowerMode();
    const bool radioOn = (mode != BSP::PowerMode::Clock);
    const bool clockOn = (mode != BSP::PowerMode::Standby);
    static uint32_t s_phantomWakes = 0;

    // In the listening modes the radio stays in continuous RX and the core-0 task drains its FIFO,
    // so incoming messages are never lost while the CPU sleeps.
    if (radioOn) mesh.update();

    // Wake conditions. The button level is sampled at every wake and, in the clock modes, the light
    // sleep also wakes on the button GPIO so a short tap is not lost (see lowPowerLightSleep). The
    // touch panel is NOT a wake source: the digitizer stays powered (hibernating it is a known trap)
    // but a tap must not pull the device back to the full UI from a nightstand.
    M5.update();
    const bool peekPressed = M5.BtnA.wasClicked() || M5.BtnA.wasHold() ||
                             M5.BtnB.wasClicked() || M5.BtnB.wasHold();
    // Live press state: the clock-mode light sleep wakes on the button GPIO, so the CPU is back the
    // moment the button closes and this is what makes a short tap work (wasClicked() would only fire
    // on release, which may land in the next long sleep slice).
    const bool buttonDown = M5.BtnA.isPressed() || M5.BtnB.isPressed();
    bool powerWake = M5.BtnPWR.wasClicked() || M5.BtnPWR.wasHold();
    bool wake = peekPressed || powerWake;
    if (!wake && bsp.checkPowerButton()) {
        wake = true;
        powerWake = true;
    }

    // Ignore anything that happens in the first moment after entering the mode: the press that
    // opened the menu, the PM1 latch and I2C transients would otherwise bounce us straight back.
    const bool dwellOver = (millis() - bsp.getLowPowerStartMillis()) > 1500;

    // ---- Clock peek light ----
    // In the clock modes BtnA/BtnB are a "peek" control, not an exit: they light the frontlight for a
    // few seconds (25%, or the user's brightness when it is not 0) and leave the clock on screen.
    // Only the power button leaves the mode. In STANDBY the buttons keep exiting as before.
    static uint32_t s_clockLightUntil = 0;
    const bool clockMode = (mode == BSP::PowerMode::Clock || mode == BSP::PowerMode::StandbyClock);

    // ---- Alarm ----
    // Checked at every wake: a due alarm brings the clock face with the alarm name on the panel,
    // turns the frontlight on when the alarm asks for it and loops the ringtone. Any button or a tap
    // STOPS the alarm and KEEPS the mode: leaving needs a second press.
    AlarmService& alarms = AlarmService::getInstance();
    alarms.pollLowPower();
    static bool s_alarmScreen = false;

    // Per-session state reset. These statics outlive the mode (the wake path returns to the normal
    // loop without a hook), so e.g. a phantom-wake count left at 7 would abort the next session on
    // its first hiccup, and a stale alarm banner flag would skip the banner redraw after a valve exit.
    static uint32_t s_sessionStart = 0;
    const uint32_t sessionStart = bsp.getLowPowerStartMillis();
    if (sessionStart != s_sessionStart) {
        s_sessionStart = sessionStart;
        s_phantomWakes = 0;
        s_alarmScreen = false;
        s_clockLightUntil = 0;
    }

    if (alarms.isRinging()) {
        if (!s_alarmScreen) {
            s_alarmScreen = true;
            if (alarms.ringBacklight()) {
                const uint8_t userFl = mesh.getFrontlight();
                bsp.setFrontlight(userFl > 0 ? userFl : AlarmService::DEFAULT_BACKLIGHT_PERCENT);
            }
            // The clock modes already show the face: this redraw adds the name banner. STANDBY shows
            // the logo instead, so the alarm brings the clock up on its own.
            ClockMode::getInstance().drawFace(mesh.getClockLandscape(), mesh.getClockFlip180(),
                                              alarms.ringingName());
        }

        // The touch panel is not a wake source, but while the alarm rings a tap must stop it.
        bool stop = wake || buttonDown;
        TouchManager::getInstance().update();
        if (TouchManager::getInstance().hasEvent()) {
            TouchManager::getInstance().popEvent();
            stop = true;
        }
        if (stop && dwellOver) {
            alarms.dismiss();
            if (s_alarmScreen) {
                s_alarmScreen = false;
                bsp.setFrontlight(0);
                bsp.redrawLowPowerScreen();
            }
            // Drain the click that stopped the alarm so it does not leave the mode too.
            M5.update();
            M5.BtnA.wasClicked();
            M5.BtnB.wasClicked();
            M5.BtnPWR.wasClicked();
            M5.BtnPWR.wasHold();
            bool dummyFlag = false;
            bsp.getPM1().btnGetFlag(&dummyFlag);
        }

        // Keep the minute change alive while the alarm clock face is on the panel. Drive the buzzer
        // tone deadline (alarmTone is a one-shot like beep) and keep the battery protection running.
        ClockMode::getInstance().maybeRefresh(mesh.getClockLandscape(), mesh.getClockFlip180());
        bsp.update();
        checkLowBattery();
        if (bsp.hasPendingActivity()) {
            // A note is playing: stay awake, so the light sleep cannot gate/chop the LEDC output and
            // the note keeps its full duration and volume.
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            // Sleep exactly up to the next note (capped at the usual short slice).
            uint32_t slice = alarms.msToNextTone();
            if (slice == 0 || slice > BSP::STANDBY_SLEEP_MS) slice = BSP::STANDBY_SLEEP_MS;
            if (radioOn) {
                // Same radio-mutex rule as the normal sleep path: never freeze the radio task mid-SPI.
                if (mesh.waitRadioIdle(100) && mesh.lockRadio(200)) {
                    bsp.lowPowerLightSleep(slice);
                    mesh.unlockRadio();
                    mesh.waitRadioIdle(150);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else {
                bsp.lowPowerLightSleep(slice);
            }
        }
        return;
    }
    if (s_alarmScreen) {
        // Defensive: the alarm ended somewhere else (dismiss from the UI, alarm disabled...).
        s_alarmScreen = false;
        bsp.setFrontlight(0);
        bsp.redrawLowPowerScreen();
    }

    // Apply the clock peek: light up and remember the deadline (the light is turned off down here,
    // after the slice is chosen so the sleep cannot overshoot it). This runs after the ringing
    // branch, so while an alarm rings the same press dismisses instead of lighting.
    if (clockMode && !powerWake && (peekPressed || buttonDown)) {
        wake = false;
        const uint8_t userFl = mesh.getFrontlight();
        bsp.setFrontlight(userFl > 0 ? userFl : CLOCK_PEEK_BRIGHTNESS);
        s_clockLightUntil = millis() + CLOCK_PEEK_MS;
        if (s_clockLightUntil == 0) s_clockLightUntil = 1; // wrap-to-0 would leave the light on forever
        ESP_LOGI(TAG, "Clock peek: frontlight on for %u ms (%u%%)",
                 (unsigned)CLOCK_PEEK_MS,
                 (unsigned)(userFl > 0 ? userFl : CLOCK_PEEK_BRIGHTNESS));
    }
    const bool clockLightOn = (int32_t)(millis() - s_clockLightUntil) < 0;
    if (!clockLightOn && s_clockLightUntil != 0) {
        s_clockLightUntil = 0;
        bsp.setFrontlight(0);
    }

    if (wake && dwellOver) {
        ESP_LOGI(TAG, "Low-power wake -> Exiting %s mode",
                 (mode == BSP::PowerMode::Standby) ? "Standby" :
                 (mode == BSP::PowerMode::Clock) ? "Clock" : "Standby+Clock");
        // Wait for the button release to avoid re-triggering actions
        uint32_t wStart = millis();
        while (millis() - wStart < 1500) {
            M5.update();
            if (!M5.BtnA.isPressed() && !M5.BtnB.isPressed() && !M5.BtnPWR.isPressed()) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        M5.update(); // refresh M5Unified state before returning to the normal loop
        M5.BtnPWR.wasClicked();
        M5.BtnPWR.wasHold();
        M5.BtnA.wasClicked();
        M5.BtnB.wasClicked();

        bsp.exitLowPower(mesh.getFrontlight());
        // The peek-light deadline must not survive the exit: re-entering within CLOCK_PEEK_MS would
        // otherwise "expire" a light nobody turned on and force the user's frontlight to 0.
        s_clockLightUntil = 0;
        mesh.resumeRadio();
        // The SD was remounted: persist here the nodes/messages collected while it was off
        mesh.requestNodePersist();
        UIEngine::getInstance().requestFullRefresh();
        return;
    }

    // Clock face: no-op unless the RTC minute changed (with a full quality clean every 10 partials).
    if (clockOn) {
        ClockMode::getInstance().maybeRefresh(mesh.getClockLandscape(), mesh.getClockFlip180());
    }

    // The park at entry can lose the race against a TX in flight: retry until the chip is really
    // asleep, otherwise the radio-off mode would keep paying the ~10 mA RX current.
    if (!radioOn && !mesh.isRadioSuspended()) {
        mesh.suspendRadio();
    }

    // Battery protection runs here too: the clock alone draws next to nothing, but the listening
    // modes keep receiving and can still drain a nearly empty cell.
    checkLowBattery();

    // Process BSP timers (buzzer off, LED activity off) even in low power
    bsp.update();

    // Still work to do (frames queued by the radio task, or a CRITICAL broadcast pending)? Loop
    // again and process them before going back to sleep, so the notification beep fires right after
    // the packet arrives rather than up to one sleep period later.
    if (radioOn && (mesh.hasQueuedFrames() || mesh.hasPendingRadioWork() || bsp.hasPendingActivity())) {
        vTaskDelay(pdMS_TO_TICKS(5));
        return;
    }

    // The Clock mode sleeps straight to the next minute boundary (radio parked, no FIFO to poll);
    // the listening modes keep the short 100 ms slice, because the SX1262 has a single frame buffer
    // and a longer slice would let a second packet overwrite the first one.
    uint32_t requestMs = radioOn ? BSP::STANDBY_SLEEP_MS
                                 : ClockMode::getInstance().msToNextMinute(BSP::CLOCK_SLEEP_MS);
    // While the peek light is on, wake often enough to switch it off on time: the clock mode would
    // otherwise sleep up to a minute and the frontlight would stay on far too long.
    if (clockLightOn && requestMs > CLOCK_PEEK_SLICE_MS) requestMs = CLOCK_PEEK_SLICE_MS;

    // In the clock modes the minute-aligned slices also wake on the two buttons, so a short tap is
    // caught instantly instead of slipping through the sleep: the PM1 IRQ line (power button, the
    // only exit from CLOCK) is armed with the same gating, and lowPowerLightSleep() skips the arm
    // when the line is not idle. Never armed while a button is already down (it would wake on the
    // level immediately) or while the power button is held.
    const bool armButtonWake = clockMode && !buttonDown && !M5.BtnPWR.isPressed();
    const bool armPm1Wake = armButtonWake;

    bool slept = false;
    uint32_t sleptMs = 0;
    esp_sleep_wakeup_cause_t wakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
    if (!radioOn) {
        // Radio parked: no mutex dance needed, nothing else touches the SPI bus.
        const uint32_t before = millis();
        wakeCause = bsp.lowPowerLightSleep(requestMs, armButtonWake, armPm1Wake);
        sleptMs = millis() - before;
        slept = true;
    } else if (mesh.waitRadioIdle(100) && mesh.lockRadio(200)) {
        // The radio mutex is held across the sleep so the core-0 task cannot be frozen in the middle
        // of an SPI transaction (light sleep stalls the other core wherever it is).
        const uint32_t before = millis();
        wakeCause = bsp.lowPowerLightSleep(requestMs, armButtonWake, armPm1Wake);
        mesh.unlockRadio();
        sleptMs = millis() - before;
        // Let the radio task hand over a frame that arrived during the sleep
        mesh.waitRadioIdle(150);
        slept = true;
    } else {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!slept) return;

    // Safety valve: a rejected light sleep (cause UNDEFINED) or a wake source we cannot clear would
    // otherwise spin here forever with the UI frozen. Wake source is timer-only, so do not depend on
    // ESP_SLEEP_WAKEUP_GPIO (with gpio_wakeup_enable it left a level ISR storm). Count any cycle
    // that returned far too early or did not sleep at all. A GPIO wake is a real button press in the
    // clock modes (timer + button sources), not a phantom cycle.
    const bool phantom = (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED || sleptMs < (requestMs / 2)) &&
                         wakeCause != ESP_SLEEP_WAKEUP_GPIO &&
                         !(radioOn && (mesh.hasQueuedFrames() || mesh.hasPendingRadioWork()));
    if (phantom) {
        if (++s_phantomWakes >= 8) {
            ESP_LOGE(TAG, "Low-power aborted after %u immediate wake cycles: returning to normal mode",
                     (unsigned)s_phantomWakes);
            s_phantomWakes = 0;
            bsp.exitLowPower(mesh.getFrontlight());
            mesh.resumeRadio();
            mesh.requestNodePersist();
            UIEngine::getInstance().requestFullRefresh();
        }
    } else {
        s_phantomWakes = 0;
    }
}

void loop() {
    // 0. Low-power modes: Standby (radio listening), Clock (radio off), Standby+Clock
    if (BSP::getInstance().isLowPower()) {
        handleLowPowerMode();
        return;
    }

    // 0. Update Board Support Package (Buzzer, LEDs, GPS)
    BSP::getInstance().update();

    // 1. Poll Touch digitizer (reads physical coordinates via getTouchPointRaw)
    TouchManager::getInstance().update();

    // 2. Poll LoRa Radio and Mesh packets
    MeshService::getInstance().update();
    // A radio resume that lost the mutex race when leaving the clock mode is retried here: the node
    // must never stay deaf. No-op when the radio was not suspended.
    MeshService::getInstance().resumeRadio();

    // 2b. Battery protection (warn on the status bar first, shut down at the cutoff)
    checkLowBattery();

    // 2c. Alarms: match the RTC and drive the ringtone. UIEngine turns isRinging() into the popup.
    AlarmService::getInstance().update();

    // 3. Physical buttons: BtnA = page up (hold = quick menu), BtnB = page down (hold = full refresh).
    // M5Unified reports a click only for a short press, so a hold can never trigger both actions.
    // While the alarm rings any button only stops it: the view underneath must not page.
    const bool alarmStop = AlarmService::getInstance().isRinging() &&
                           (M5.BtnA.wasClicked() || M5.BtnA.wasHold() ||
                            M5.BtnB.wasClicked() || M5.BtnB.wasHold());
    if (alarmStop) {
        AlarmService::getInstance().dismiss();
    }

    if (!alarmStop && M5.BtnA.wasHold()) {
        ESP_LOGI(TAG, "BtnA held -> Opening Quick Menu");
        UIEngine::getInstance().openQuickMenu();
    } else if (!alarmStop && M5.BtnA.wasClicked()) {
        bool moved = UIEngine::getInstance().scrollCurrentView(-1);
        if (moved) ESP_LOGI(TAG, "BtnA clicked -> page up");
        else ESP_LOGD(TAG, "BtnA clicked -> page up ignored (overlay open or first page)");
    }

    if (!alarmStop && M5.BtnB.wasHold()) {
        ESP_LOGI(TAG, "BtnB held -> Manual Full Clear Anti-Ghosting");
        EPDDriver::getInstance().fullClear();
        UIEngine::getInstance().requestFullRefresh();
    } else if (!alarmStop && M5.BtnB.wasClicked()) {
        bool moved = UIEngine::getInstance().scrollCurrentView(+1);
        if (moved) ESP_LOGI(TAG, "BtnB clicked -> page down");
        else ESP_LOGD(TAG, "BtnB clicked -> page down ignored (overlay open or last page)");
    }

    // 4. Physical Power Button Handling (PM1 PMIC). The configured action is one of the four
    // low-power modes: 0 = power off, 1 = standby, 2 = clock, 3 = standby+clock. While the alarm
    // rings the first click only stops it.
    if (BSP::getInstance().checkPowerButton()) {
        if (AlarmService::getInstance().isRinging()) {
            AlarmService::getInstance().dismiss();
        } else {
            MeshService::getInstance().saveConfig();
            const uint8_t action = MeshService::getInstance().getPowerBtnAction();
            if (action == 0) {
                ESP_LOGI(TAG, "Physical power button clicked -> Powering Off");
                BSP::getInstance().powerOff("Power Off");
            } else {
                ESP_LOGI(TAG, "Physical power button clicked -> Entering low-power mode %u", (unsigned)action);
                BSP::getInstance().enterLowPower((BSP::PowerMode)action);
                return;
            }
        }
    }

    // 5. Update UI logic
    UIEngine::getInstance().update();

    // 6. Render active frame if dirty
    UIEngine::getInstance().render();

    // Small yield for FreeRTOS tasks & watchdog
    vTaskDelay(pdMS_TO_TICKS(15));

    // Periodic heartbeat to serial (only if terminal is active)
    static uint32_t s_lastHeartbeat = 0;
    if (Serial && (millis() - s_lastHeartbeat >= 10000)) {
        s_lastHeartbeat = millis();
        Serial.printf("[HEARTBEAT] Uptime: %lu s | Heap: %u B | PSRAM: %u B | Nodes: %u\n", 
            millis() / 1000, 
            (unsigned)ESP.getFreeHeap(), 
            (unsigned)ESP.getFreePsram(),
            (unsigned)MeshService::getInstance().getNodes().size());
    }
}
