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

void setup()
{
    Serial.begin(115200);
    LedDriver::init();
    LedDriver::rgbBlink(RgbColor { 20, 8, 0 }, 1000, 1000); // dim slow orange flash

#ifdef DEBUG_MENU_ENABLED
    debug_menu();
#endif

    power_init();
    rtc_map_init();
    mesh_init();
    mesh_start();
    LedDriver::rgbSet(0, 20, 0); // dim green = init done.
}

void loop()
{
    // Heartbeat: brief RGB flash to show mesh state
    if (mesh_is_root()) {
        LedDriver::rgbSet(0, 0, 255); // blue = gateway
    } else if (mesh_is_connected()) {
        LedDriver::rgbSet(0, 255, 0); // green = connected peer
    } else {
        LedDriver::rgbSet(255, 0, 0); // red = disconnected
    }
    delay(50);
    LedDriver::rgbOff();

    Serial.printf("Battery: %lu mV\n", power_battery_mv());

    rtc_map_save();

    // Light sleep between heartbeats
    power_enter_light_sleep(5);
}
