#include <Arduino.h>
#include "sdkconfig.h"
#include "bsp.hpp"
#include "nvs_config.h"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_conductor.h"
#include "rtc_state.h"
#include "sq_log.h"
#include "audio_tweeter.h"
#include "audio_engine.h"
#include "orchestrator.h"
#include "mesh_delegate.h"

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
    RtcState::init();

    // Read next_role and clear it (one-shot: crash falls back to election)
    uint8_t nextRole = RtcState::get()->next_role;
    RtcState::get()->next_role = 0xFF;
    RtcState::save();

    if (nextRole == (uint8_t)RoleId::DELEGATE) {
        SqLog.println("[boot] RTC says DELEGATE — entering setup mode");
        MeshConductor::init();  // WiFi/netif init only
        MeshConductor::setRole(new Delegate());
    } else if (nextRole == (uint8_t)RoleId::GATEWAY) {
        SqLog.println("[boot] RTC says GATEWAY — fast-boot");
        MeshConductor::setFastBoot(true);
        MeshConductor::init();
        MeshConductor::start();
    } else if (nextRole == (uint8_t)RoleId::PEER) {
        SqLog.println("[boot] RTC says PEER — fast-boot");
        MeshConductor::setFastBoot(true);
        MeshConductor::init();
        MeshConductor::start();
    } else {
        SqLog.println("[boot] Normal boot — mesh scan + election");
        MeshConductor::init();
        MeshConductor::start();
    }

    if (nextRole != (uint8_t)RoleId::DELEGATE) {
        PiezoDriver::instance().begin();
        AudioEngine::init(&PiezoDriver::instance());
        Orchestrator::init();
    }

    LedDriver::rgbSet(RgbColor(NvsConfigManager::colorReady)); // dim green = init done.
}

void loop()
{
    IMeshRole* role = MeshConductor::role();
    if (role && role->roleId() == RoleId::DELEGATE) {
        LedDriver::rgbBlink(RgbColor(40, 0, 30), 2000, 500);
    } else if (MeshConductor::isGateway()) {
        LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorGateway),2000,500);
    } else if (MeshConductor::isConnected()) {
        LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorPeer),2000,500);
    } else {
        LedDriver::rgbBlink(RgbColor(NvsConfigManager::colorDisconnected),500,500);
    }

    RtcState::save();
    SQ_POWER_DELAY(5000);
}
