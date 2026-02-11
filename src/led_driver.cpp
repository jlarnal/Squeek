#include "led_driver.h"
#include "bsp.hpp"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel rgb_led(1, RBG_BUILTIN, NEO_GRB + NEO_KHZ800);

void led_init() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    rgb_led.begin();
    rgb_led.clear();
    rgb_led.show();
}

void led_status_on() {
    digitalWrite(LED_BUILTIN, HIGH);
}

void led_status_off() {
    digitalWrite(LED_BUILTIN, LOW);
}

void led_status_blink(uint16_t on_ms, uint16_t off_ms, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        led_status_on();
        delay(on_ms);
        led_status_off();
        if (i < count - 1) delay(off_ms);
    }
}

void led_rgb_set(uint8_t r, uint8_t g, uint8_t b) {
    rgb_led.setPixelColor(0, rgb_led.Color(r, g, b));
    rgb_led.show();
}

void led_rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms) {
    led_rgb_set(r, g, b);
    delay(duration_ms);
    led_rgb_off();
}

void led_rgb_off() {
    rgb_led.setPixelColor(0, 0);
    rgb_led.show();
}
