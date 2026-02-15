#include <Arduino.h>
#include "sdkconfig.h"
#include "bsp.hpp"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_conductor.h"
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

    PowerManager::init();
    RtcMap::init();
    MeshConductor::init();
    MeshConductor::start();
    LedDriver::rgbSet(0, 20, 0); // dim green = init done.
}

void loop()
{
    // Heartbeat: brief RGB flash to show mesh state
    if (MeshConductor::isGateway()) {
        LedDriver::rgbSet(0, 0, 255); // blue = gateway
    } else if (MeshConductor::isConnected()) {
        LedDriver::rgbSet(0, 255, 0); // green = connected peer
    } else {
        LedDriver::rgbSet(255, 0, 0); // red = disconnected
    }
    delay(50);
    LedDriver::rgbOff();

    Serial.printf("Battery: %lu mV\n", PowerManager::batteryMv());

    RtcMap::save();

    SQ_POWER_DELAY(5000);
}
