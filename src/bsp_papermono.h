#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <M5PM1.h>
#include <M5IOE1.h>
#include <M5Unified.h>

namespace MonoMesh {

// Pin Definitions
namespace Pins {
    // System I2C
    constexpr int I2C_SDA = 47;
    constexpr int I2C_SCL = 48;

    // Keys
    constexpr int KEY1 = 2; // Button A
    constexpr int KEY2 = 3; // Button B
    constexpr int PM1_IRQ = 1; // PMIC M5PM1 IRQ line (power button wake, RTC-capable GPIO)

    // Display (SSD1677 on SPI2)
    constexpr int EPD_MOSI = 14;
    constexpr int EPD_SCLK = 15;
    constexpr int EPD_CS   = 16;
    constexpr int EPD_DC   = 17;
    constexpr int EPD_BUSY = 18;

    // Touch (FT6336G)
    constexpr int TOUCH_INT = 4;

    // LoRa (SX1262 on SPI3)
    constexpr int LORA_MOSI = 38;
    constexpr int LORA_MISO = 40;
    constexpr int LORA_SCLK = 39;
    constexpr int LORA_NSS  = 41;
    constexpr int LORA_BUSY = 21;
    constexpr int LORA_DIO1 = 5;

    // Buzzer
    constexpr int BUZZER_PWM = 42;
}

// I2C Addresses
namespace I2CAddr {
    constexpr uint8_t PMIC_PM1    = 0x6E;
    constexpr uint8_t IO_EXPANDER = 0x4F;
    constexpr uint8_t RTC_RX8130  = 0x32;
    constexpr uint8_t IMU_BMI270  = 0x68;
    constexpr uint8_t NFC_ST25R   = 0x50;
    constexpr uint8_t CHG_IP2315  = 0x75;
    constexpr uint8_t TOUCH_FT    = 0x38;
}

// M5IOE1 Pin Assignments
namespace IOEPins {
    constexpr uint8_t TF_DET      = M5IOE1_PIN_1;
    constexpr uint8_t LORA_ANT_SW = M5IOE1_PIN_2;
    constexpr uint8_t EPD_3V3_EN  = M5IOE1_PIN_3;
    constexpr uint8_t NFC_EN      = M5IOE1_PIN_4;
    constexpr uint8_t EPD_RST     = M5IOE1_PIN_5;
    constexpr uint8_t TOUCH_RST   = M5IOE1_PIN_6;
    constexpr uint8_t RGB_GREEN   = M5IOE1_PIN_8;
    constexpr uint8_t RGB_BLUE    = M5IOE1_PIN_9;
    constexpr uint8_t LORA_NRST   = M5IOE1_PIN_10;
    constexpr uint8_t CHG_I2C_EN  = M5IOE1_PIN_11;
    constexpr uint8_t PDM_EN      = M5IOE1_PIN_12;
    constexpr uint8_t TOUCH_VDD_EN= M5IOE1_PIN_13;
    constexpr uint8_t TF_EN       = M5IOE1_PIN_14;
}

// M5PM1 GPIO Assignments
namespace PM1Pins {
    constexpr m5pm1_gpio_num_t LORA_EN     = M5PM1_GPIO_NUM_2;
    constexpr m5pm1_gpio_num_t FRONTLIGHT  = M5PM1_GPIO_NUM_3;
}

struct BatteryState {
    int voltageMv;
    int percentage;
    bool isCharging;
};

class BSP {
public:
    static BSP& getInstance() {
        static BSP instance;
        return instance;
    }

    bool init(uint8_t initialBrightness = 30);
    void update();
    bool checkPowerButton();
    void powerOff(const char* label = "Power Off");
    // Battery protection: draws a warning screen and powers the board down. The RTC stays on its
    // always-on domain, so the wall clock (and the clock face after a reboot) is preserved.
    void lowBatteryShutdown();
    // Warning/cutoff thresholds on the resting pack voltage. The firmware percentage is optimistic
    // at the bottom of the curve (3.45 V reads "26%"), so the protection works on millivolts.
    static constexpr int LOW_BATTERY_WARN_MV = 3550;
    static constexpr int LOW_BATTERY_CUTOFF_MV = 3450;

    // ---- Low-power power modes (selected by the power-button setting) ----
    // 1 = Standby (radio listening), 2 = Clock (radio off, big HH:MM), 3 = both.
    enum class PowerMode : uint8_t {
        None = 0,
        Standby = 1,
        Clock = 2,
        StandbyClock = 3
    };

    void enterLowPower(PowerMode mode);
    void exitLowPower(uint8_t restoreBrightness = 30);
    bool isLowPower() const { return _powerMode != PowerMode::None; }
    PowerMode getPowerMode() const { return _powerMode; }
    uint32_t getLowPowerStartMillis() const { return _lowPowerStartMillis; }

    // Low-power sleep in short timer slices with polling of radio + buttons between them: GPIO wake
    // is deliberately avoided because it reconfigures the pins and caused an interrupt storm on DIO1.
    // The standby slice keeps the button response instant (short taps cannot slip between polls); the
    // clock-only mode can use longer slices because nothing but the minute change and the buttons
    // depends on the wake period.
    static constexpr uint32_t STANDBY_SLEEP_MS = 100;
    static constexpr uint32_t CLOCK_SLEEP_MS = 1000;
    esp_sleep_wakeup_cause_t lowPowerLightSleep(uint32_t maxSleepMs);
    // True while a notification beep or an LED activity pulse is still running: light sleep does not
    // stop the LEDC timer, so without this the beep/LED outlived its configured duration.
    bool hasPendingActivity() const { return _buzzerActive || _ledOffUntil > 0; }

    void setFrontlight(uint8_t brightnessPercent);
    uint8_t getFrontlight() const { return _currentBrightness; }
    void setLed(bool red, uint8_t greenPercent, uint8_t bluePercent);
    void setLedRxActivity(uint16_t durationMs = 60);
    void setLedTxActivity(uint16_t durationMs = 80);

    void setLedNotifications(bool enable) { _ledNotificationsEnabled = enable; }
    bool getLedNotifications() const { return _ledNotificationsEnabled; }

    void setBuzzerVolume(uint8_t vol);
    uint8_t getBuzzerVolume() const { return _buzzerVolume; }
    uint8_t getLastNonZeroVolume() const { return _lastNonZeroVolume; }
    void setLastNonZeroVolume(uint8_t vol) { if (vol > 0 && vol <= 3) _lastNonZeroVolume = vol; }

    // What is allowed to make the buzzer sound (the volume gates everything on top of this).
    enum class BuzzerEvent : uint8_t {
        UiClick = 0,      // UI feedback (send failed, position set...)
        Message = 1,      // incoming broadcast message
        DirectMessage = 2,// incoming direct message
        Test = 3          // explicit preview (quick menu / settings test row)
    };
    enum class BuzzerMode : uint8_t {
        All = 0,          // everything
        Notifications = 1,// messages only (no UI clicks)
        DirectOnly = 2,   // direct messages only
        SystemOnly = 3,   // UI feedback only (silent on RX)
        Disabled = 4      // never
    };
    void setBuzzerMode(BuzzerMode mode) { _buzzerMode = mode; }
    BuzzerMode getBuzzerMode() const { return _buzzerMode; }
    bool buzzerEventAllowed(BuzzerEvent ev) const;

    // Buzzer (PWM on GPIO 42). The event is filtered by the buzzer mode; Test (an explicit preview
    // from the quick menu or the settings test row) is allowed by every mode except Disabled.
    void beep(uint16_t freqHz = 0, uint16_t durationMs = 50, BuzzerEvent ev = BuzzerEvent::Test);
    // Tono di notifica condiviso: il quick menu lo usa per l'anteprima e MeshService per i bip di
    // ricezione, così il suono in standby coincide con quello scelto dall'utente.
    static constexpr uint16_t NOTIFY_TONE_HZ = 2400;
    void click();

    BatteryState getBatteryState();
    void getRtcTime(int& hour, int& minute, int& second);
    void setRtcTime(int hour, int minute, int second);
    uint32_t getRtcUnix();
    void syncRtcFromUnix(uint32_t unixTimestamp, int8_t tzOffsetHours = 1);
    // Date/time handling: the RX8130CE stores *local* wall-clock time, so the configured timezone
    // is needed both to convert to UTC and to write a time parsed from the on-screen keyboard.
    void setTimezoneOffset(int8_t hours) { _tzOffset = hours; }
    int8_t getTimezoneOffset() const { return _tzOffset; }
    void setLocalDateTime(int year, int month, int day, int hour, int minute, int second);
    void getLocalDateTime(int& year, int& month, int& day, int& hour, int& minute, int& second);
    void addRtcSeconds(int32_t deltaSeconds); // shift the stored time keeping the same instant
    bool isRtcValid();

    M5PM1& getPM1() { return _pm1; }
    M5IOE1& getIOE1() { return _ioe1; }

    // Idle-rail helpers used by every low-power mode: the SD rail is cut (after unmounting the FAT),
    // the touch controller stays powered (hibernating it left the panel unresponsive), while the EPD
    // rail stays on because the panel driver would otherwise lose its configuration.
    void lowPowerPeripheralsOff();
    void lowPowerPeripheralsOn();

private:
    BSP() = default;
    ~BSP() = default;

    void drawStandbyScreen();
    void drawShutdownScreen(const char* title, const char* subtitle, const char* bottomLabel);
    void shutdownHardware();

    M5PM1 _pm1;
    M5IOE1 _ioe1;
    bool _pm1Ready = false;
    bool _ioe1Ready = false;
    PowerMode _powerMode = PowerMode::None;
    uint32_t _wakeCooldownUntil = 0;
    uint8_t _currentBrightness = 30;

    // Buzzer & LED configuration & state
    bool _ledNotificationsEnabled = true;
    uint8_t _buzzerVolume = 2;
    BuzzerMode _buzzerMode = BuzzerMode::All;
    uint8_t _lastNonZeroVolume = 2;
    uint32_t _buzzerOffUntil = 0;
    uint32_t _ledOffUntil = 0;
    bool _buzzerActive = false;
    uint32_t _lowPowerStartMillis = 0;
    int8_t _tzOffset = 1;   // hours east of UTC (Italy: +1 winter, +2 summer)
};

} // namespace MonoMesh
