#include "mesh_conductor.h"
#include "ftm_manager.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include "sq_log.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <string.h>

// Heartbeat timers
static TimerHandle_t s_hbTimer      = nullptr;
static TimerHandle_t s_earlyHbTimer = nullptr;

static void heartbeatTimerCb(TimerHandle_t t) {
    (void)t;

    HeartbeatMsg hb;
    hb.type = MSG_TYPE_HEARTBEAT;
    esp_read_mac(hb.mac, ESP_MAC_WIFI_STA);
    hb.battery_mv = (uint16_t)PowerManager::batteryMv();
    hb.flags = 0;  // awake
    esp_read_mac(hb.softap_mac, ESP_MAC_WIFI_SOFTAP);

    MeshConductor::sendToRoot(&hb, sizeof(hb));
}

void MeshNode::begin() {
    m_gatewayAlive = true;
    SqLog.println("[node] MeshNode role active");

    // Initialize FTM manager (for responding to FTM_WAKE/GO)
    FtmManager::init();

    // Start heartbeat timer
    uint32_t hbInterval = (uint32_t)NvsConfigManager::heartbeatInterval_s;
    if (s_hbTimer == nullptr) {
        s_hbTimer = xTimerCreate("nodeHb", pdMS_TO_TICKS(hbInterval * 1000),
                                  pdTRUE, nullptr, heartbeatTimerCb);
    } else {
        xTimerChangePeriod(s_hbTimer, pdMS_TO_TICKS(hbInterval * 1000), 0);
    }
    xTimerStart(s_hbTimer, 0);

    // Send first heartbeat immediately
    heartbeatTimerCb(nullptr);

    // Send a second heartbeat after 5s so the gateway gets it after election completes
    if (s_earlyHbTimer == nullptr) {
        s_earlyHbTimer = xTimerCreate("earlyHb", pdMS_TO_TICKS(5000),
                                       pdFALSE, nullptr, heartbeatTimerCb);
    }
    xTimerStart(s_earlyHbTimer, 0);
}

void MeshNode::end() {
    SqLog.println("[node] MeshNode role stopping");
    if (s_hbTimer) {
        xTimerStop(s_hbTimer, 0);
    }
}

void MeshNode::onPeerJoined(const uint8_t* mac) {
    SqLog.printf("[node] Peer joined: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void MeshNode::onPeerLeft(const uint8_t* mac) {
    SqLog.printf("[node] Peer left: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void MeshNode::onGatewayLost() {
    SqLog.println("[node] WARNING: Gateway lost — sleep and reboot for re-election");
    m_gatewayAlive = false;
    if (s_hbTimer) {
        xTimerStop(s_hbTimer, 0);
    }
    MeshConductor::stop();
    SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
    esp_restart();
}

void MeshNode::printStatus() {
    SqLog.println("--- Node Status ---");
    SqLog.printf("Gateway alive: %s\n", m_gatewayAlive ? "yes" : "no");
}
