#include <Arduino.h>
#include "sdkconfig.h"
#include "bsp.hpp"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_manager.h"
#include "rtc_mesh_map.h"

#ifdef DEBUG_MENU_ENABLED
#include "debug_menu.h"
#endif

void setup() {
    Serial.begin(115200);

    #ifdef DEBUG_MENU_ENABLED
    debug_menu();
    #endif

    led_init();
    power_init();
    rtc_map_init();
    mesh_init();
    mesh_start();
}

void loop() {
    // Heartbeat: brief RGB flash to show mesh state
    if (mesh_is_root()) {
        led_rgb_flash(0, 0, 255, 50);   // blue = gateway
    } else if (mesh_is_connected()) {
        led_rgb_flash(0, 255, 0, 50);   // green = connected peer
    } else {
        led_rgb_flash(255, 0, 0, 50);   // red = disconnected
    }

    Serial.printf("Battery: %lu mV\n", power_battery_mv());

    rtc_map_save();

    // Light sleep between heartbeats
    power_enter_light_sleep(5);
}
