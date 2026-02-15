#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>

struct HsvColor;

struct __attribute__((packed)) RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    HsvColor toHsv() const;
};

struct __attribute__((packed)) HsvColor {
    uint16_t h; // 0-360
    uint8_t s; // 0-100
    uint8_t v; // 0-100

    RgbColor toRgb() const;
};

class LedDriver {
  public:
    static void init();
    /// Master enable/disable. When disabled, all "turn on" calls are silently
    /// ignored and both LEDs are forced off.  Blink config is preserved so
    /// blinking resumes automatically when re-enabled.
    static void setEnabled(bool enabled);
    static void statusOn();
    static void statusOff();
    /// Blocking flash for debug/testing use
    static void statusFlash(uint16_t onMs, uint16_t offMs, uint8_t count);
    /// Configure and enable non-blocking status LED blink
    static void statusBlink(uint16_t period_ms, uint16_t duty_ptt);
    /// Enable or disable status LED blinking
    static void statusBlink(bool enable, bool leaveOn);
    static void rgbSet(RgbColor color);
    static void rgbSet(const HsvColor& color) { rgbSet(color.toRgb()); }
    static void rgbSet(uint8_t r, uint8_t g, uint8_t b) { rgbSet(RgbColor { r, g, b }); }
    /**
     * @brief  Blink the RGB LED with the specified color, period, and duty cycle.
     *         Calling this method enables blinking immediately.
     * @param  color The color to blink the RGB LED.
     * @param  period_ms The period of the blinking in milliseconds.
     * @param  duty_ptt The duty cycle per ten thousand (ptt). 10000 = on 100%
     *         of the period, 5000 = on 50%, 1000 = on 10%, and so on.
     *         If set to -1, the duty is left unchanged from the previous setting.
     */
    static void rgbBlink(RgbColor color, uint16_t period_ms, int16_t duty_ptt = -1);
    /**
     * @brief  Enable or disable RGB LED blinking.
     * @param  enable Set to true to enable blinking, or false to disable it.
     * @param  leaveOn If true when disabling, the RGB LED remains on with the
     *         last set color. If false, the RGB LED is turned off.
     */
    static void rgbBlink(bool enable, bool leaveOn);



    inline static void rgbOff()
    {
        rgbBlink(false, false);
    }
};

#endif // LED_DRIVER_H
