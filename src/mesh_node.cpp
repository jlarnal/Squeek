#include "mesh_conductor.h"
#include "ftm_manager.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include "sq_log.h"
#include "rtc_state.h"
#include "web_server.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
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

    // Route heartbeat to logical gateway (may differ from ESP-IDF root after role transfer)
    const uint8_t* gw = MeshConductor::gatewayMac();
    static const uint8_t zero[6] = {0};
    if (memcmp(gw, zero, 6) != 0) {
        MeshConductor::sendToNode(gw, &hb, sizeof(hb));
    } else {
        // Gateway MAC not yet known (pre-election) — fall back to ESP-IDF root
        MeshConductor::sendToRoot(&hb, sizeof(hb));
    }
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

    // Check for unpushed creds from a previous delegate session
    rtc_state_t* rtc = RtcState::get();
    if (rtc->delegate_active) {
        char ssid[33], pass[65];
        if (SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
            SqLog.println("[node] Unpushed creds detected — will push after mesh join");
            xTaskCreate([](void*) {
                for (int i = 0; i < 30; i++) {
                    if (MeshConductor::isConnected()) break;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (!MeshConductor::isConnected()) {
                    SqLog.println("[node] Mesh not connected — skipping cred push");
                    RtcState::get()->delegate_active = 0;
                    RtcState::save();
                    vTaskDelete(nullptr);
                    return;
                }

                char ssid[33], pass[65];
                if (SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
                    WifiCredsMsg msg = {};
                    msg.type = MSG_TYPE_WIFI_CREDS;
                    strncpy(msg.ssid, ssid, 32);
                    strncpy(msg.password, pass, 64);

                    for (int i = 0; i < 10; i++) {
                        SqLog.printf("[node] Pushing WiFi creds to gateway (attempt %d)\n", i + 1);
                        MeshConductor::sendToRoot(&msg, sizeof(msg));
                        vTaskDelay(pdMS_TO_TICKS(3000));
                    }

                    DelegateResultMsg dr = { .type = MSG_TYPE_DELEGATE_RESULT, .success = 1 };
                    MeshConductor::sendToRoot(&dr, sizeof(dr));
                }

                RtcState::get()->delegate_active = 0;
                RtcState::save();

                mesh_addr_t rt[MESH_MAX_NODES];
                int rtSize = 0;
                esp_mesh_get_routing_table(rt, sizeof(rt), &rtSize);
                MergeCheckMsg mc = { .type = MSG_TYPE_MERGE_CHECK, .root_table_size = (uint8_t)rtSize };
                MeshConductor::broadcastToAll(&mc, sizeof(mc));
                SqLog.println("[node] Merge check broadcast sent");

                vTaskDelete(nullptr);
            }, "credpush", 3072, nullptr, 2, nullptr);
        } else {
            SqLog.println("[node] Was delegate but no creds found — reporting failure");
            xTaskCreate([](void*) {
                for (int i = 0; i < 30; i++) {
                    if (MeshConductor::isConnected()) break;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                if (MeshConductor::isConnected()) {
                    DelegateResultMsg dr = { .type = MSG_TYPE_DELEGATE_RESULT, .success = 0 };
                    MeshConductor::sendToRoot(&dr, sizeof(dr));
                }
                RtcState::get()->delegate_active = 0;
                RtcState::save();
                vTaskDelete(nullptr);
            }, "dlgfail", 2048, nullptr, 2, nullptr);
        }
    }
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
    Serial.println("--- Node Status ---");
    Serial.printf("Gateway alive: %s\n", m_gatewayAlive ? "yes" : "no");
}
