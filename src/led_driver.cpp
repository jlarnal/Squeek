#include "led_driver.h"
#include "bsp.hpp"
#include <Adafruit_NeoPixel.h>
#include <freertos/task.h>

// --- File-scope state (was private static members) ---

static Adafruit_NeoPixel rgb_led(1, RBG_BUILTIN, NEO_GRB + NEO_KHZ800);
static TaskHandle_t      blinkTaskHandle = nullptr;
static volatile bool     rgbBlinkEnabled    = false;
static uint16_t          rgbPeriod_ms       = 1000;
static uint16_t          rgbDuty_ptt        = 5000;
static RgbColor          rgbColor           = { 30, 10, 0 };

static volatile bool     statusBlinkEnabled = false;
static uint16_t          statusPeriod_ms    = 1000;
static uint16_t          statusDuty_ptt     = 5000;

static void blinkTask(void* pvParameters);

// --- Color conversions ---

HsvColor RgbColor::toHsv() const
{
    HsvColor hsv;
    uint8_t max = std::max({ r, g, b });
    uint8_t min = std::min({ r, g, b });
    hsv.v       = max * 100 / 255;

    if (max == 0) {
        hsv.s = 0;
        hsv.h = 0;
        return hsv;
    }

    hsv.s = (max - min) * 100 / max;

    if (max == min) {
        hsv.h = 0;
    } else if (max == r) {
        hsv.h = (60 * (g - b) / (max - min) + 360) % 360;
    } else if (max == g) {
        hsv.h = (60 * (b - r) / (max - min) + 120) % 360;
    } else {
        hsv.h = (60 * (r - g) / (max - min) + 240) % 360;
    }
    return hsv;
}

RgbColor HsvColor::toRgb() const
{
    RgbColor rgb;
    if (s == 0) {
        rgb.r = rgb.g = rgb.b = v * 255 / 100;
        return rgb;
    }

    uint16_t region    = h / 60;
    uint16_t remainder = (h % 60) * 255 / 60;

    uint8_t p = v * 255 / 100 * (100 - s) / 100;
    uint8_t q = v * 255 / 100 * (100 - (s * remainder) / 255) / 100;
    uint8_t t = v * 255 / 100 * (100 - (s * (255 - remainder)) / 255) / 100;

    switch (region) {
        case 0:
            rgb.r = v * 255 / 100;
            rgb.g = t;
            rgb.b = p;
            break;
        case 1:
            rgb.r = q;
            rgb.g = v * 255 / 100;
            rgb.b = p;
            break;
        case 2:
            rgb.r = p;
            rgb.g = v * 255 / 100;
            rgb.b = t;
            break;
        case 3:
            rgb.r = p;
            rgb.g = q;
            rgb.b = v * 255 / 100;
            break;
        case 4:
            rgb.r = t;
            rgb.g = p;
            rgb.b = v * 255 / 100;
            break;
        default:
            rgb.r = v * 255 / 100;
            rgb.g = p;
            rgb.b = q;
            break;
    }
    return rgb;
}

// --- LedDriver implementation ---

void LedDriver::init()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    rgb_led.begin();
    rgb_led.clear();
    rgb_led.show();

    if (blinkTaskHandle != nullptr) {
        vTaskDelete(blinkTaskHandle);
        blinkTaskHandle = nullptr;
        ESP_LOGW("LedDriver", "Stray blink task found and deleted during init.");
    }

    auto err = xTaskCreateUniversal(
        blinkTask, "BlinkTask", 2048, nullptr, tskIDLE_PRIORITY + 1,
        &blinkTaskHandle, tskNO_AFFINITY);
    if (err != pdPASS) {
        ESP_LOGE("LedDriver", "Failed to create blink task: %d", err);
    }
}

void LedDriver::statusOn()
{
    statusBlinkEnabled = false;
    digitalWrite(LED_BUILTIN, HIGH);
}

void LedDriver::statusOff()
{
    statusBlinkEnabled = false;
    digitalWrite(LED_BUILTIN, LOW);
}

void LedDriver::statusFlash(uint16_t onMs, uint16_t offMs, uint8_t count)
{
    bool wasEnabled = statusBlinkEnabled;
    statusBlinkEnabled = false;
    for (uint8_t i = 0; i < count; i++) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(onMs);
        digitalWrite(LED_BUILTIN, LOW);
        if (i < count - 1)
            delay(offMs);
    }
    statusBlinkEnabled = wasEnabled;
}

void LedDriver::statusBlink(uint16_t period_ms, uint16_t duty_ptt)
{
    statusPeriod_ms    = period_ms;
    statusDuty_ptt     = duty_ptt;
    statusBlinkEnabled = true;
}

void LedDriver::statusBlink(bool enable, bool leaveOn)
{
    statusBlinkEnabled = enable;
    if (!enable && !leaveOn) {
        digitalWrite(LED_BUILTIN, LOW);
    }
}

void LedDriver::rgbSet(RgbColor color)
{
    rgbBlinkEnabled = false;
    rgbColor = color;
    rgb_led.setPixelColor(0, rgb_led.Color(color.r, color.g, color.b));
    rgb_led.show();
}

void LedDriver::rgbBlink(RgbColor color, uint16_t period_ms, int16_t duty_ptt)
{
    rgbColor     = color;
    rgbPeriod_ms = period_ms;
    if (duty_ptt >= 0)
        rgbDuty_ptt = static_cast<uint16_t>(duty_ptt);
    rgbBlinkEnabled = true;
}

void LedDriver::rgbBlink(bool enable, bool leaveOn)
{
    rgbBlinkEnabled = enable;
    if (!enable && !leaveOn) {
        rgb_led.setPixelColor(0, 0);
        rgb_led.show();
    }
}

// --- Blink task (round-robin) ---

static void blinkTask(void* pvParameters)
{
    uint16_t rgbElapsed_ms    = 0;
    uint16_t statusElapsed_ms = 0;
    bool     rgbIsOn          = false;
    bool     statusIsOn       = false;

    while (true) {
        bool rgbActive    = rgbBlinkEnabled;
        bool statusActive = statusBlinkEnabled;

        // Nothing enabled — reset tracking and idle
        if (!rgbActive && !statusActive) {
            rgbElapsed_ms    = 0;
            statusElapsed_ms = 0;
            rgbIsOn          = false;
            statusIsOn       = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Snapshot parameters
        uint16_t rgbPeriod    = rgbPeriod_ms;
        uint16_t rgbDuty      = rgbDuty_ptt;
        RgbColor color        = rgbColor;
        uint16_t statusPeriod = statusPeriod_ms;
        uint16_t statusDuty   = statusDuty_ptt;

        // Reset disabled LEDs
        if (!rgbActive) {
            rgbElapsed_ms = 0;
            rgbIsOn       = false;
        }
        if (!statusActive) {
            statusElapsed_ms = 0;
            statusIsOn       = false;
        }

        uint16_t sleepMs = UINT16_MAX;

        // --- RGB LED ---
        if (rgbActive && rgbPeriod > 0) {
            // Clamp elapsed if period changed mid-cycle
            if (rgbElapsed_ms >= rgbPeriod)
                rgbElapsed_ms = 0;

            uint16_t rgbOnTime = static_cast<uint32_t>(rgbPeriod) * rgbDuty / 10000;
            bool shouldBeOn    = (rgbElapsed_ms < rgbOnTime);

            if (shouldBeOn != rgbIsOn) {
                if (shouldBeOn)
                    rgb_led.setPixelColor(0, rgb_led.Color(color.r, color.g, color.b));
                else
                    rgb_led.setPixelColor(0, 0);
                rgb_led.show();
                rgbIsOn = shouldBeOn;
            }

            uint16_t timeToNext = shouldBeOn
                ? (rgbOnTime - rgbElapsed_ms)
                : (rgbPeriod - rgbElapsed_ms);
            if (timeToNext < sleepMs)
                sleepMs = timeToNext;
        }

        // --- Status LED ---
        if (statusActive && statusPeriod > 0) {
            if (statusElapsed_ms >= statusPeriod)
                statusElapsed_ms = 0;

            uint16_t statusOnTime = static_cast<uint32_t>(statusPeriod) * statusDuty / 10000;
            bool shouldBeOn       = (statusElapsed_ms < statusOnTime);

            if (shouldBeOn != statusIsOn) {
                digitalWrite(LED_BUILTIN, shouldBeOn ? HIGH : LOW);
                statusIsOn = shouldBeOn;
            }

            uint16_t timeToNext = shouldBeOn
                ? (statusOnTime - statusElapsed_ms)
                : (statusPeriod - statusElapsed_ms);
            if (timeToNext < sleepMs)
                sleepMs = timeToNext;
        }

        // Guard against zero sleep (e.g. period=0 or duty edge)
        if (sleepMs == 0 || sleepMs == UINT16_MAX)
            sleepMs = 1;

        vTaskDelay(pdMS_TO_TICKS(sleepMs));

        // Advance elapsed counters and wrap
        if (rgbActive && rgbPeriod > 0) {
            rgbElapsed_ms += sleepMs;
            if (rgbElapsed_ms >= rgbPeriod)
                rgbElapsed_ms = 0;
        }
        if (statusActive && statusPeriod > 0) {
            statusElapsed_ms += sleepMs;
            if (statusElapsed_ms >= statusPeriod)
                statusElapsed_ms = 0;
        }
    }
}
