#include <Arduino.h>
#include <esp_log.h>
#include <WiFi.h>
#include "bsp_papermono.h"
#include "clock_mode.h"
#include "epd_driver.h"
#include "touch_manager.h"
#include "mesh_service.h"
#include "storage_manager.h"
#include "ui_engine.h"

static constexpr const char* TAG = "MonoMesh-Main";

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
    if (now < 60000) return; // let the first readings settle (boot/USB transients)

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

    // Wake conditions. GPIO wake is deliberately avoided (see BSP::lowPowerLightSleep), so the
    // physical buttons and the PM1 power button latch are polled at every wake. The touch panel is
    // NOT a wake source: the digitizer stays powered (hibernating it is a known trap) but a tap
    // must not pull the device back to the full UI from a nightstand.
    M5.update();
    bool wake = M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || M5.BtnPWR.wasClicked() || M5.BtnPWR.wasHold();
    if (!wake && bsp.checkPowerButton()) {
        wake = true;
    }

    // Ignore anything that happens in the first moment after entering the mode: the press that
    // opened the menu, the PM1 latch and I2C transients would otherwise bounce us straight back.
    const bool dwellOver = (millis() - bsp.getLowPowerStartMillis()) > 1500;

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
    const uint32_t requestMs = radioOn ? BSP::STANDBY_SLEEP_MS
                                       : ClockMode::getInstance().msToNextMinute(BSP::CLOCK_SLEEP_MS);

    bool slept = false;
    uint32_t sleptMs = 0;
    esp_sleep_wakeup_cause_t wakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
    if (!radioOn) {
        // Radio parked: no mutex dance needed, nothing else touches the SPI bus.
        const uint32_t before = millis();
        wakeCause = bsp.lowPowerLightSleep(requestMs);
        sleptMs = millis() - before;
        slept = true;
    } else if (mesh.waitRadioIdle(100) && mesh.lockRadio(200)) {
        // The radio mutex is held across the sleep so the core-0 task cannot be frozen in the middle
        // of an SPI transaction (light sleep stalls the other core wherever it is).
        const uint32_t before = millis();
        wakeCause = bsp.lowPowerLightSleep(requestMs);
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
    // that returned far too early or did not sleep at all.
    const bool phantom = (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED || sleptMs < (requestMs / 2)) &&
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

    // 3. Physical buttons: BtnA = page up (hold = quick menu), BtnB = page down (hold = full refresh).
    // M5Unified reports a click only for a short press, so a hold can never trigger both actions.
    if (M5.BtnA.wasHold()) {
        ESP_LOGI(TAG, "BtnA held -> Opening Quick Menu");
        UIEngine::getInstance().openQuickMenu();
    } else if (M5.BtnA.wasClicked()) {
        bool moved = UIEngine::getInstance().scrollCurrentView(-1);
        if (moved) ESP_LOGI(TAG, "BtnA clicked -> page up");
        else ESP_LOGD(TAG, "BtnA clicked -> page up ignored (overlay open or first page)");
    }

    if (M5.BtnB.wasHold()) {
        ESP_LOGI(TAG, "BtnB held -> Manual Full Clear Anti-Ghosting");
        EPDDriver::getInstance().fullClear();
        UIEngine::getInstance().requestFullRefresh();
    } else if (M5.BtnB.wasClicked()) {
        bool moved = UIEngine::getInstance().scrollCurrentView(+1);
        if (moved) ESP_LOGI(TAG, "BtnB clicked -> page down");
        else ESP_LOGD(TAG, "BtnB clicked -> page down ignored (overlay open or last page)");
    }

    // 4. Physical Power Button Handling (PM1 PMIC). The configured action is one of the four
    // low-power modes: 0 = power off, 1 = standby, 2 = clock, 3 = standby+clock.
    if (BSP::getInstance().checkPowerButton()) {
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
