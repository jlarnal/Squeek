#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>

void led_init();
void led_status_on();
void led_status_off();
void led_status_blink(uint16_t on_ms, uint16_t off_ms, uint8_t count);
void led_rgb_set(uint8_t r, uint8_t g, uint8_t b);
void led_rgb_flash(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms);
void led_rgb_off();

#endif // LED_DRIVER_H
