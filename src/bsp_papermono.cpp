#include "bsp_papermono.h"
#include "clock_mode.h"
#include "epd_driver.h"
#include "mesh_service.h"
#include "storage_manager.h"
#include <esp_log.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

static constexpr const char* TAG = "MonoMesh-BSP";

// Shared PM1/IOE1 bus speeds: Fast Mode is attempted first, 100 kHz is the guaranteed fallback
static constexpr uint32_t I2C_FREQ_FAST = 400000;
static constexpr uint32_t I2C_FREQ_SAFE = 100000;

namespace MonoMesh {

bool BSP::init(uint8_t initialBrightness) {
    ESP_LOGI(TAG, "Initializing M5Unified and Peripherals (Initial Brightness: %u%%)...", initialBrightness);
    _currentBrightness = initialBrightness;

    // 1. Initialize M5Unified first (sets up M5.In_I2C and board autodetect)
    auto cfg = M5.config();
    cfg.clear_display = false;
    cfg.internal_mic  = false;
    cfg.internal_spk  = false;
    cfg.internal_imu  = false;
    M5.begin(cfg);

    // Immediately enforce desired brightness to eliminate the 100% startup flash
    M5.Display.setBrightness(initialBrightness * 255 / 100);

    // Configure Display rotation to portrait 480x800
    M5.Display.setRotation(0);
    M5.Display.setAutoDisplay(false);

    ESP_LOGI(TAG, "M5Unified initialized (Board=%d, Display %dx%d)",
             static_cast<int>(M5.getBoard()), M5.Display.width(), M5.Display.height());

    // 2. Initialize PMIC M5PM1 on M5.In_I2C. Fast Mode (400 kHz) first: begin() verifies the device
    // id at that speed, so a failure is self-detecting and we fall back to the safe 100 kHz.
    for (int attempt = 0; attempt < 2 && !_pm1Ready; ++attempt) {
        const uint32_t speed = (attempt == 0) ? I2C_FREQ_FAST : I2C_FREQ_SAFE;
        for (int retry = 0; retry < 3 && !_pm1Ready; ++retry) {
            if (_pm1.begin(&M5.In_I2C, I2CAddr::PMIC_PM1, speed) == M5PM1_OK) {
                _pm1Ready = true;
                ESP_LOGI(TAG, "M5PM1 PMIC initialized at %u Hz", (unsigned)speed);
                break;
            }
            delay(100);
        }
    }

    if (_pm1Ready) {
        ESP_LOGI(TAG, "M5PM1 PMIC initialized");
        _pm1.setI2cSleepTime(0);
        _pm1.irqClearGpioAll();
        _pm1.irqClearSysAll();
        _pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_0, false);
        _pm1.gpioSetWakeEnable(M5PM1_GPIO_NUM_4, false);

        // Crucial for battery power hold & software button capture
        _pm1.ldoSetPowerHold(true);
        _pm1.setSingleResetDisable(true);

        // Clear initial button flag/IRQ so boot press is not seen as immediate shutdown
        uint8_t initIrq = 0;
        _pm1.irqGetBtnStatus(&initIrq, M5PM1_CLEAN_ALL);
        bool initFlag = false;
        _pm1.btnGetFlag(&initFlag);

        // Enable LoRa Power via PM1 GPIO 2
        _pm1.gpioSetFunc(PM1Pins::LORA_EN, M5PM1_GPIO_FUNC_GPIO);
        _pm1.gpioSet(PM1Pins::LORA_EN, M5PM1_GPIO_MODE_OUTPUT, 1, M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);
    } else {
        ESP_LOGE(TAG, "M5PM1 PMIC init failed!");
    }

    // 3. Initialize IO Expander M5IOE1 on M5.In_I2C (same 400 kHz probe, then 100 kHz fallback).
    // Done before any pin configuration so a failed probe cannot leave the rails in a wrong state.
    for (int attempt = 0; attempt < 2 && !_ioe1Ready; ++attempt) {
        const uint32_t speed = (attempt == 0) ? I2C_FREQ_FAST : I2C_FREQ_SAFE;
        for (int retry = 0; retry < 3 && !_ioe1Ready; ++retry) {
            if (_ioe1.begin(&M5.In_I2C, I2CAddr::IO_EXPANDER, speed, -1, M5IOE1_INT_MODE_DISABLED) == M5IOE1_OK) {
                _ioe1Ready = true;
                ESP_LOGI(TAG, "M5IOE1 IO Expander initialized at %u Hz", (unsigned)speed);
                break;
            }
            delay(100);
        }
    }

    if (_ioe1Ready) {
        ESP_LOGI(TAG, "M5IOE1 IO Expander initialized");
        
        // EPD Power & Reset
        _ioe1.pinMode(IOEPins::EPD_3V3_EN, OUTPUT);
        _ioe1.setDriveMode(IOEPins::EPD_3V3_EN, M5IOE1_DRIVE_PUSHPULL);
        _ioe1.digitalWrite(IOEPins::EPD_3V3_EN, HIGH); // Enable EPD 3.3V power

        _ioe1.pinMode(IOEPins::EPD_RST, OUTPUT);
        _ioe1.setDriveMode(IOEPins::EPD_RST, M5IOE1_DRIVE_PUSHPULL);

        // Touch Power & Reset
        _ioe1.pinMode(IOEPins::TOUCH_VDD_EN, OUTPUT);
        _ioe1.setDriveMode(IOEPins::TOUCH_VDD_EN, M5IOE1_DRIVE_PUSHPULL);
        _ioe1.digitalWrite(IOEPins::TOUCH_VDD_EN, HIGH); // Enable Touch VDD

        _ioe1.pinMode(IOEPins::TOUCH_RST, OUTPUT);
        _ioe1.setDriveMode(IOEPins::TOUCH_RST, M5IOE1_DRIVE_PUSHPULL);

        // LoRa Control (Reset and Antenna Switch)
        _ioe1.pinMode(IOEPins::LORA_ANT_SW, OUTPUT);
        _ioe1.setDriveMode(IOEPins::LORA_ANT_SW, M5IOE1_DRIVE_PUSHPULL);
        _ioe1.digitalWrite(IOEPins::LORA_ANT_SW, HIGH); // Antenna Switch to LoRa RF

        _ioe1.pinMode(IOEPins::LORA_NRST, OUTPUT);
        _ioe1.setDriveMode(IOEPins::LORA_NRST, M5IOE1_DRIVE_PUSHPULL);

        // Hardware Reset Sequences
        // EPD reset pulse
        _ioe1.digitalWrite(IOEPins::EPD_RST, LOW);
        delay(10);
        _ioe1.digitalWrite(IOEPins::EPD_RST, HIGH);
        delay(20);

        // Touch reset pulse
        _ioe1.digitalWrite(IOEPins::TOUCH_RST, LOW);
        delay(10);
        _ioe1.digitalWrite(IOEPins::TOUCH_RST, HIGH);
        delay(20);

        // LoRa reset pulse
        _ioe1.digitalWrite(IOEPins::LORA_NRST, LOW);
        delay(10);
        _ioe1.digitalWrite(IOEPins::LORA_NRST, HIGH);
        delay(20);

        // RGB LED pins
        _ioe1.pinMode(IOEPins::RGB_GREEN, OUTPUT);
        _ioe1.pinMode(IOEPins::RGB_BLUE, OUTPUT);
        _ioe1.setDriveMode(IOEPins::RGB_GREEN, M5IOE1_DRIVE_PUSHPULL);
        _ioe1.setDriveMode(IOEPins::RGB_BLUE, M5IOE1_DRIVE_PUSHPULL);
        _ioe1.setPwmFrequency(5000);

        // Ensure IP2316 charger remains disconnected from I2C bus to prevent bus lockups
        _ioe1.pinMode(IOEPins::CHG_I2C_EN, OUTPUT);
        _ioe1.setDriveMode(IOEPins::CHG_I2C_EN, M5IOE1_DRIVE_PUSHPULL);
        _ioe1.digitalWrite(IOEPins::CHG_I2C_EN, LOW);
    } else {
        ESP_LOGE(TAG, "M5IOE1 IO Expander init failed!");
    }

    // User buttons setup
    pinMode(Pins::KEY1, INPUT_PULLUP);
    pinMode(Pins::KEY2, INPUT_PULLUP);

    // Buzzer setup (PWM on GPIO 42)
    ledcSetup(0, 2700, 8);
    ledcAttachPin(Pins::BUZZER_PWM, 0);
    ledcWrite(0, 0);

    // Force the PM1 status LED off: after power-on it defaults to RED and nothing in the firmware
    // ever cleared it, so it stayed lit until the first standby cycle.
    setLed(false, 0, 0);

    return true;
}

void BSP::setFrontlight(uint8_t brightnessPercent) {
    if (brightnessPercent > 100) brightnessPercent = 100;
    _currentBrightness = brightnessPercent;
    M5.Display.setBrightness(brightnessPercent * 255 / 100);
}

void BSP::setLed(bool red, uint8_t greenPercent, uint8_t bluePercent) {
    if (!_ledNotificationsEnabled && (red || greenPercent > 0 || bluePercent > 0)) {
        return;
    }
    if (_pm1Ready) {
        _pm1.setLedEnLevel(red);
    }
    if (_ioe1Ready) {
        _ioe1.setPwmDuty(M5IOE1_PWM_CH2, greenPercent, false, greenPercent > 0);
        _ioe1.setPwmDuty(M5IOE1_PWM_CH1, bluePercent, false, bluePercent > 0);
    }
}

BatteryState BSP::getBatteryState() {
    static uint32_t s_lastCheck = 0;
    static uint32_t s_lastVinCheck = 0;
    static BatteryState s_cachedState = {4000, 100, false};
    uint32_t now = millis();

    // Fast VIN check every 300ms for instantaneous charging plug/unplug feedback
    if (now - s_lastVinCheck >= 300 || s_lastVinCheck == 0) {
        s_lastVinCheck = now;
        uint16_t vinMv = 0;
        if (_pm1Ready && _pm1.readVin(&vinMv) == M5PM1_OK) {
            s_cachedState.isCharging = (vinMv >= 4400);
        } else {
            s_cachedState.isCharging = false;
        }
    }

    if (now - s_lastCheck >= 4000 || s_lastCheck == 0) {
        s_lastCheck = now;
        s_cachedState.voltageMv = M5.Power.getBatteryVoltage();
        if (s_cachedState.voltageMv < 3200) {
            s_cachedState.percentage = 0;
        } else if (s_cachedState.voltageMv >= 4150) {
            s_cachedState.percentage = 100;
        } else {
            s_cachedState.percentage = (s_cachedState.voltageMv - 3200) * 100 / (4150 - 3200);
        }
    }

    return s_cachedState;
}

void BSP::getRtcTime(int& hour, int& minute, int& second) {
    auto time = M5.Rtc.getTime();
    hour   = time.hours;
    minute = time.minutes;
    second = time.seconds;
}

void BSP::setRtcTime(int hour, int minute, int second) {
    m5::rtc_time_t time;
    time.hours   = hour;
    time.minutes = minute;
    time.seconds = second;
    M5.Rtc.setTime(time);
}

uint32_t BSP::getRtcUnix() {
    auto dt = M5.Rtc.getDateTime();
    struct tm t = dt.get_tm();
    time_t s = mktime(&t);
    if (s <= 0) return 0;
    // The RTC holds local wall-clock time; return true UTC so mesh timestamps stay comparable.
    int64_t utc = (int64_t)s - (int64_t)_tzOffset * 3600;
    return (utc > 0) ? (uint32_t)utc : 0;
}

bool BSP::isRtcValid() {
    auto dt = M5.Rtc.getDateTime();
    struct tm t = dt.get_tm();
    return (t.tm_year + 1900) >= 2023;
}

void BSP::getLocalDateTime(int& year, int& month, int& day, int& hour, int& minute, int& second) {
    auto dt = M5.Rtc.getDateTime();
    struct tm t = dt.get_tm();
    year = t.tm_year + 1900;
    month = t.tm_mon + 1;
    day = t.tm_mday;
    hour = t.tm_hour;
    minute = t.tm_min;
    second = t.tm_sec;
}

void BSP::setLocalDateTime(int year, int month, int day, int hour, int minute, int second) {
    if (year < 2023 || year > 2099) return;
    if (month < 1 || month > 12 || day < 1 || day > 31) return;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return;

    // Build via struct tm so the M5 RTC helpers derive the weekday themselves
    struct tm tmInfo;
    memset(&tmInfo, 0, sizeof(tmInfo));
    tmInfo.tm_year = year - 1900;
    tmInfo.tm_mon = month - 1;
    tmInfo.tm_mday = day;
    tmInfo.tm_hour = hour;
    tmInfo.tm_min = minute;
    tmInfo.tm_sec = second;
    tmInfo.tm_isdst = -1;
    mktime(&tmInfo); // fills tm_wday
    m5::rtc_date_t d(tmInfo);
    m5::rtc_time_t ti(tmInfo);
    M5.Rtc.setDateTime(&d, &ti);
    ESP_LOGI(TAG, "RTC set manually to %04d-%02d-%02d %02d:%02d:%02d (local, UTC%+d)",
             year, month, day, hour, minute, second, (int)_tzOffset);
}

// Shift the stored wall clock by a delta: used when the timezone offset changes so the displayed
// local time follows immediately while the underlying instant (and UTC) stays put.
void BSP::addRtcSeconds(int32_t deltaSeconds) {
    if (deltaSeconds == 0) return;
    auto dt = M5.Rtc.getDateTime();
    struct tm t = dt.get_tm();
    t.tm_isdst = -1;
    time_t s = mktime(&t);
    if (s <= 0) return;
    s += deltaSeconds;
    struct tm out;
    gmtime_r(&s, &out);
    m5::rtc_date_t d(out);
    m5::rtc_time_t ti(out);
    M5.Rtc.setDateTime(&d, &ti);
    ESP_LOGI(TAG, "RTC shifted by %ld s (timezone change)", (long)deltaSeconds);
}

bool BSP::checkPowerButton() {
    if (!_pm1Ready) return false;
    if (millis() < 2500) {
        // Ignore bootup / power-on transient and clear state
        M5.BtnPWR.wasClicked();
        return false;
    }

    if (millis() < _wakeCooldownUntil) {
        // Drain any lingering power button events from waking up
        M5.BtnPWR.wasClicked();
        M5.BtnPWR.wasHold();
        bool flag = false;
        _pm1.btnGetFlag(&flag);
        return false;
    }

    // 1. Check debounced click from M5Unified
    if (M5.BtnPWR.wasClicked()) {
        return true;
    }

    // 2. Direct PMIC register flag check in case M5Unified polling missed the latch
    static uint32_t s_lastPoll = 0;
    if (millis() - s_lastPoll >= 100) {
        s_lastPoll = millis();
        bool flag = false;
        if (_pm1.btnGetFlag(&flag) == M5PM1_OK && flag) {
            return true;
        }
    }

    return false;
}

void BSP::powerOff(const char* label) {
    ESP_LOGI(TAG, "Entering Sleep / Power Off: %s", label ? label : "Power Off");
    
    drawShutdownScreen(nullptr, nullptr, label ? label : "Power Off");
    shutdownHardware();
}

// Battery protection path: unlike the user-initiated power off this one is not optional, so it says
// why the board is going away. The RTC keeps running on the PM1 always-on domain (~20 uA).
void BSP::lowBatteryShutdown() {
    BatteryState bs = getBatteryState();
    ESP_LOGW(TAG, "Battery critically low (%d mV, %d%%): shutting down (RTC stays powered)",
             bs.voltageMv, bs.percentage);
    char subtitle[48];
    snprintf(subtitle, sizeof(subtitle), "%.2f V - charge before power on", (double)bs.voltageMv / 1000.0);
    drawShutdownScreen("Battery Low", subtitle, "Power Off");
    shutdownHardware();
}

// Last image on the panel before the rails go down. title == nullptr keeps the plain "MonoMesh"
// power-off screen; otherwise the title/subtitle are added above the bottom-right label.
void BSP::drawShutdownScreen(const char* title, const char* subtitle, const char* bottomLabel) {
    // The clock mode leaves the panel in landscape: the shutdown screen is portrait like the UI.
    M5.Display.setRotation(0);
    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_center);

    if (!title) {
        M5.Display.setTextSize(6); // Large, clean, bold logo
        M5.Display.drawString("MonoMesh", 240, 400);
    } else {
        M5.Display.setTextSize(4);
        M5.Display.drawString("MonoMesh", 240, 320);
        M5.Display.setTextSize(5);
        M5.Display.drawString(title, 240, 440);
        if (subtitle) {
            M5.Display.setTextSize(2);
            M5.Display.drawString(subtitle, 240, 520);
        }
    }

    M5.Display.setTextDatum(textdatum_t::bottom_right);
    M5.Display.setTextSize(2);
    M5.Display.drawString(bottomLabel ? bottomLabel : "Power Off", 460, 780);

    M5.Display.display();
    M5.Display.waitDisplay();
}

void BSP::shutdownHardware() {
    // Turn off frontlight and LEDs
    setFrontlight(0);
    setLed(false, 0, 0);

    // Disable LoRa power rail
    if (_pm1Ready) {
        _pm1.gpioSet(PM1Pins::LORA_EN, M5PM1_GPIO_MODE_OUTPUT, 0, M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);
    }

    // Disable EPD 3.3V, Touch VDD and TF Card rail
    if (_ioe1Ready) {
        // Unmount the FAT before the card loses power
        StorageManager::getInstance().unmountSd();
        _ioe1.digitalWrite(IOEPins::EPD_3V3_EN, LOW);
        _ioe1.digitalWrite(IOEPins::TOUCH_VDD_EN, LOW);
        _ioe1.digitalWrite(IOEPins::LORA_ANT_SW, LOW);
        _ioe1.digitalWrite(IOEPins::TF_EN, LOW);
    }

    // Release PM1 power hold & command PMIC shutdown
    if (_pm1Ready) {
        _pm1.ldoSetPowerHold(false);
        _pm1.shutdown();
    }

    // Power off via M5.Power
    M5.Power.powerOff();

    vTaskDelay(pdMS_TO_TICKS(150));

    // Fallback: esp deep sleep. Never sleep without a wake source: when USB/VBUS is connected the
    // PMIC may refuse to cut the power, and an un-wakeable sleep would leave the device apparently
    // bricked (screen off, no reaction) until a hardware reset. Wake on the PM1 IRQ line (the power
    // button, active low on GPIO 1 - same pin the official M5Stack demo uses) plus a 60s timer as a
    // safety net so the device always comes back.
    const gpio_num_t pm1Irq = (gpio_num_t)Pins::PM1_IRQ;
    rtc_gpio_deinit(pm1Irq);
    rtc_gpio_init(pm1Irq);
    rtc_gpio_set_direction(pm1Irq, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(pm1Irq);
    rtc_gpio_pullup_en(pm1Irq);
    if (esp_sleep_enable_ext0_wakeup(pm1Irq, 0) != ESP_OK) {
        ESP_LOGW(TAG, "EXT0 wake on PM1 IRQ failed, relying on the timer wake");
    }
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    ESP_LOGW(TAG, "Deep sleep fallback: wake on PM1 IRQ (GPIO %d, low) or 60s timer", Pins::PM1_IRQ);
    esp_deep_sleep_start();
}

// ---- Low-power modes: Standby (radio listening), Clock (radio off), Standby+Clock ----

void BSP::drawStandbyScreen() {
    M5.Display.setRotation(0);
    M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextSize(6);
    M5.Display.drawString("MonoMesh", 240, 400);

    M5.Display.setTextDatum(textdatum_t::bottom_right);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Standby", 460, 780);

    M5.Display.display();
    M5.Display.waitDisplay();
}

void BSP::enterLowPower(PowerMode mode) {
    if (_powerMode != PowerMode::None || mode == PowerMode::None) return;
    const bool clock = (mode == PowerMode::Clock || mode == PowerMode::StandbyClock);
    ESP_LOGI(TAG, "Entering %s mode...",
             (mode == PowerMode::Standby) ? "Standby Listening" :
             (mode == PowerMode::Clock) ? "Clock (radio off)" : "Standby+Clock (radio listening)");

    _powerMode = mode;
    _lowPowerStartMillis = millis();

    // Forget the accumulated partial refreshes: the low-power image is a full redraw anyway, and
    // carrying the counter over would fire an anti-ghosting full clear on the first partial update
    // after waking up.
    EPDDriver::getInstance().resetPartialCounter();

    if (clock) {
        ClockMode::getInstance().drawFace(MeshService::getInstance().getClockLandscape(),
                                          MeshService::getInstance().getClockFlip180());
    } else {
        drawStandbyScreen();
    }

    // Turn off frontlight, buzzer, and LEDs
    setFrontlight(0);
    setLed(false, 0, 0);
    _ledOffUntil = 0;
    ledcWrite(0, 0);
    _buzzerActive = false;
    _buzzerOffUntil = 0;

    // Drain and clear all pending button events so the entry click does not trigger an immediate wake
    uint32_t waitStart = millis();
    while (millis() - waitStart < 2000) {
        M5.update();
        if (!M5.BtnPWR.isPressed() && !M5.BtnA.isPressed() && !M5.BtnB.isPressed()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    M5.BtnPWR.wasClicked();
    M5.BtnPWR.wasHold();
    M5.BtnA.wasClicked();
    M5.BtnB.wasClicked();
    bool dummyFlag = false;
    _pm1.btnGetFlag(&dummyFlag);

    // The Clock mode leaves the radio out of the picture on purpose: park it before idling the rails
    // so it is not left receiving while its neighbours disappear.
    if (mode == PowerMode::Clock) {
        MeshService::getInstance().suspendRadio();
    }

    // Idle the peripherals only now that the display work and the button drain are done: the EPD
    // rail stays on (power-cycling it would lose the panel config), while the SD rail is cut after
    // unmounting the FAT. The touch controller stays powered on purpose (see lowPowerPeripheralsOff).
    lowPowerPeripheralsOff();
}

void BSP::exitLowPower(uint8_t restoreBrightness) {
    if (_powerMode == PowerMode::None) return;
    const bool clock = (_powerMode == PowerMode::Clock || _powerMode == PowerMode::StandbyClock);
    ESP_LOGI(TAG, "Waking up from low-power mode (%u)...", (unsigned)_powerMode);

    _powerMode = PowerMode::None;

    if (clock) {
        // The UI is portrait only: undo the clock rotation and forget the drawn time.
        ClockMode::getInstance().reset();
        M5.Display.setRotation(0);
    }

    // Restore the peripherals we idled on entry (SD rail + FAT remount)
    lowPowerPeripheralsOn();

    // Set 1200ms cooldown so the wake press is not processed as a power-off / sleep command
    _wakeCooldownUntil = millis() + 1200;

    // Clear PM1 and M5Unified button states
    M5.BtnPWR.wasClicked();
    M5.BtnPWR.wasHold();
    bool flag = false;
    _pm1.btnGetFlag(&flag);

    // Restore frontlight
    _currentBrightness = restoreBrightness;
    setFrontlight(_currentBrightness);
}

// ---- Low-power peripheral rail management ----

void BSP::lowPowerPeripheralsOff() {
    // The touch controller is deliberately NOT hibernated: after a hibernate/wake cycle it left the
    // panel unresponsive (the device could not be woken from standby), and it can always be polled
    // through M5Unified this way. It costs a couple of mA, reliability comes first.
    // BMI270 accelerometer/gyro: suspend sampling (PWR_CTRL = 0x00). We never use the IMU.
    bool imuOk = M5.In_I2C.writeRegister8(I2CAddr::IMU_BMI270, 0x7D, 0x00, I2C_FREQ_SAFE);

    // SD: the FAT must be unmounted before the rail goes down
    StorageManager::getInstance().unmountSd();
    if (_ioe1Ready) {
        _ioe1.digitalWrite(IOEPins::TF_EN, LOW);
    }

    // Let the PM1 signal button events on its IRQ line (GPIO1 low) so the power button can wake us
    if (_pm1Ready) {
        // Unmask button events only: GPIO/system events (charger, VIN, RTC...) would keep the IRQ
        // line low and turn the light-sleep wake into a busy loop.
        _pm1.irqSetGpioMaskAll(M5PM1_IRQ_MASK_ENABLE); // "enable mask" = masked (ignored)
        _pm1.irqSetSysMaskAll(M5PM1_IRQ_MASK_ENABLE);
        _pm1.irqSetBtnMaskAll(M5PM1_IRQ_MASK_DISABLE); // unmasked -> asserts the IRQ pin
        _pm1.irqClearGpioAll();
        _pm1.irqClearSysAll();
        bool flag = false;
        _pm1.btnGetFlag(&flag);
    }
    ESP_LOGI(TAG, "Low-power peripherals idled (SD rail off, IMU suspend=%d, PM1 IRQ armed)", imuOk ? 1 : 0);
}

void BSP::lowPowerPeripheralsOn() {
    if (_ioe1Ready) {
        _ioe1.digitalWrite(IOEPins::TF_EN, HIGH);
        delay(50);
    }
    StorageManager::getInstance().remountSd();
    // BMI270: re-enable accelerometer + gyroscope
    bool imuOk = M5.In_I2C.writeRegister8(I2CAddr::IMU_BMI270, 0x7D, 0x06, I2C_FREQ_SAFE);
    ESP_LOGI(TAG, "Low-power peripherals restored (IMU resume=%d, SD=%d)", imuOk ? 1 : 0,
             StorageManager::getInstance().isSdMounted() ? 1 : 0);
}

esp_sleep_wakeup_cause_t BSP::lowPowerLightSleep(uint32_t maxSleepMs) {
    // TIMER-ONLY light sleep. GPIO wake (gpio_wakeup_enable) reconfigures the pin as an RTC input and
    // leaves a level-triggered interrupt behind: after the first transmission DIO1 stays high until
    // the radio IRQ flags are cleared, which produced an interrupt storm on CPU1 and an Interrupt WDT
    // panic right after the next TX. Sleeping in short slices and polling instead keeps the radio and
    // the GPIO configuration exactly as RadioLib set them, at the cost of ~1 mA average.
    // The radio stays in continuous RX and latches RX_DONE, so packets are read on the next poll;
    // in the Clock mode the radio is parked and only the RTC minute change matters.
    esp_sleep_enable_timer_wakeup((uint64_t)maxSleepMs * 1000ULL);

    uint32_t before = millis();
    esp_err_t sleepErr = esp_light_sleep_start();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (sleepErr != ESP_OK) {
        ESP_LOGE(TAG, "esp_light_sleep_start failed: %d (0x%x)", (int)sleepErr, (unsigned)sleepErr);
    }
    // Log every suspicious cycle (instant wake) and only occasionally the normal ones, so a busy mesh
    // does not flood the serial log.
    static uint32_t s_cycleCount = 0;
    uint32_t sleptMs = millis() - before;
    if (sleptMs < 100 || (++s_cycleCount % 20) == 0) {
        ESP_LOGI(TAG, "Low-power sleep cycle: %u ms asleep, cause=%d, lines DIO1=%d KEY1=%d KEY2=%d PM1=%d",
                 (unsigned)sleptMs, (int)cause, digitalRead(Pins::LORA_DIO1), digitalRead(Pins::KEY1),
                 digitalRead(Pins::KEY2), digitalRead(Pins::PM1_IRQ));
    }

    // Safety: if the PM1 or the radio is still holding a wake line asserted we would spin in a wake
    // loop. Clear the PM1 flags so its open-drain IRQ releases.
    if (_pm1Ready && digitalRead(Pins::PM1_IRQ) == LOW) {
        _pm1.irqClearGpioAll();
        _pm1.irqClearSysAll();
        bool flag = false;
        _pm1.btnGetFlag(&flag);
    }
    return cause;
}

void BSP::setBuzzerVolume(uint8_t vol) {
    if (vol > 3) vol = 3;
    _buzzerVolume = vol;
    if (_buzzerVolume > 0) {
        _lastNonZeroVolume = _buzzerVolume;
    }
}

bool BSP::buzzerEventAllowed(BuzzerEvent ev) const {
    switch (_buzzerMode) {
        case BuzzerMode::All:
            return true;
        case BuzzerMode::Notifications:
            return ev == BuzzerEvent::Message || ev == BuzzerEvent::DirectMessage || ev == BuzzerEvent::Test;
        case BuzzerMode::DirectOnly:
            return ev == BuzzerEvent::DirectMessage || ev == BuzzerEvent::Test;
        case BuzzerMode::SystemOnly:
            return ev == BuzzerEvent::UiClick || ev == BuzzerEvent::Test;
        case BuzzerMode::Disabled:
            return false;
    }
    return true;
}

void BSP::beep(uint16_t freqHz, uint16_t durationMs, BuzzerEvent ev) {
    if (_buzzerVolume == 0) return;
    if (!buzzerEventAllowed(ev)) return;

    uint16_t freq = 2300;
    uint8_t duty = 55;

    switch (_buzzerVolume) {
        case 1: // Basso / Discreto: 1500 Hz, duty cycle ~18
            freq = (freqHz > 0) ? (uint16_t)(freqHz * 1500 / 2700) : 1500;
            duty = 18;
            break;
        case 2: // Medio: 2300 Hz, duty cycle ~55
            freq = (freqHz > 0) ? (uint16_t)(freqHz * 2300 / 2700) : 2300;
            duty = 55;
            break;
        case 3: // Alto (Risonanza): 2800 Hz, duty cycle 127
            freq = (freqHz > 0) ? freqHz : 2800;
            duty = 127;
            break;
        default:
            return;
    }

    ledcSetup(0, freq, 8);
    ledcWrite(0, duty);
    _buzzerActive = true;
    _buzzerOffUntil = millis() + durationMs;
}

void BSP::click() {
    beep(0, 15, BuzzerEvent::UiClick);
}

void BSP::setLedRxActivity(uint16_t durationMs) {
    if (!_ledNotificationsEnabled) return;
    setLed(false, 100, 0); // Green on M5IOE1
    _ledOffUntil = millis() + durationMs;
}

void BSP::setLedTxActivity(uint16_t durationMs) {
    if (!_ledNotificationsEnabled) return;
    setLed(false, 0, 100); // Blue on M5IOE1
    _ledOffUntil = millis() + durationMs;
}

void BSP::syncRtcFromUnix(uint32_t unixTimestamp, int8_t tzOffsetHours) {
    if (unixTimestamp < 1700000000UL) return; // Skip timestamps before 2023
    time_t t = (time_t)(unixTimestamp + (int32_t)tzOffsetHours * 3600);
    struct tm tmInfo;
    gmtime_r(&t, &tmInfo);
    m5::rtc_date_t d(tmInfo);
    m5::rtc_time_t ti(tmInfo);
    M5.Rtc.setDateTime(&d, &ti);
    ESP_LOGI(TAG, "RTC synchronized to %04d-%02d-%02d %02d:%02d:%02d via mesh packet (UTC%+d)",
             tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday,
             tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec, (int)tzOffsetHours);
}

void BSP::update() {
    uint32_t now = millis();
    if (_buzzerActive && now >= _buzzerOffUntil) {
        _buzzerActive = false;
        ledcWrite(0, 0);
    }
    if (_ledOffUntil > 0 && now >= _ledOffUntil) {
        _ledOffUntil = 0;
        setLed(false, 0, 0);
    }
}

} // namespace MonoMesh
