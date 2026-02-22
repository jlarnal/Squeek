#include <Arduino.h>
#include "sdkconfig.h"
#include "bsp.hpp"
#include "nvs_config.h"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_conductor.h"
#include "rtc_mesh_map.h"
#include "audio_tweeter.h"
#include "audio_engine.h"

#ifdef DEBUG_MENU_ENABLED
#include "debug_cli.h"
#endif

void setup()
{
    Serial.begin(115200);
    NvsConfigManager::begin();

    LedDriver::init();
    NvsConfigManager::ledsEnabled.setBeforeChange(
        [](bool, bool newVal, bool*, bool*) { LedDriver::setEnabled(newVal); });
    LedDriver::setEnabled(NvsConfigManager::ledsEnabled);
    LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorInit), 1000, 1000); // dim slow orange flash

#ifdef DEBUG_MENU_ENABLED
    debug_cli_init();
#endif

    PowerManager::init();
    RtcMap::init();
    MeshConductor::init();
    MeshConductor::start();

    PiezoDriver::instance().begin();
    AudioEngine::init(&PiezoDriver::instance());

    LedDriver::rgbSet(RgbColor(NvsConfigManager::colorReady)); // dim green = init done.
}

void loop()
{
    // Heartbeat: brief RGB flash to show mesh state
    if (MeshConductor::isGateway()) {
        LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorGateway),2000,500); // blue = gateway
    } else if (MeshConductor::isConnected()) {
        LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorPeer),2000,500); // green = connected peer
    } else {
        LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorDisconnected),500,1000); // red = disconnected
    }

    RtcMap::save();

    SQ_POWER_DELAY(5000);
}
