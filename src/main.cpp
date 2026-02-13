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

    LedDriver::init();
    power_init();
    rtc_map_init();
    mesh_init();
    mesh_start();
}

void loop() {
    // Heartbeat: brief RGB flash to show mesh state
    if (mesh_is_root()) {
        LedDriver::rgbSet(0, 0, 255);   // blue = gateway
    } else if (mesh_is_connected()) {
        LedDriver::rgbSet(0, 255, 0);   // green = connected peer
    } else {
        LedDriver::rgbSet(255, 0, 0);   // red = disconnected
    }
    delay(50);
    LedDriver::rgbOff();

    Serial.printf("Battery: %lu mV\n", power_battery_mv());

    rtc_map_save();

    // Light sleep between heartbeats
    power_enter_light_sleep(5);
}
