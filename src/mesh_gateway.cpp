#include "mesh_conductor.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "position_solver.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include "sq_log.h"
#include "orchestrator.h"
#include "clock_sync.h"
#include "web_server.h"
#include "mesh_delegate.h"
#include "rtc_state.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_mac.h>

// Gateway self-heartbeat timer — updates own battery in PeerTable
static TimerHandle_t s_gwHeartbeatTimer = nullptr;

static void gwHeartbeatCb(TimerHandle_t t) {
    (void)t;
    PeerTable::updateSelf((uint16_t)PowerManager::batteryMv());
}

void Gateway::begin() {
    m_peerCount = 0;
    SqLog.println("[gateway] Gateway role active");

    // Clear delegate flag — we're running as gateway now
    RtcState::get()->delegate_active = 0;
    RtcState::save();

    // Initialize Phase 2 subsystems
    PeerTable::init();
    FtmManager::init();
    PositionSolver::init();
    FtmScheduler::init();
    ClockSync::init();

    // Start self-heartbeat timer (update own battery in PeerTable)
    uint32_t hbInterval = (uint32_t)NvsConfigManager::heartbeatInterval_s;
    if (s_gwHeartbeatTimer == nullptr) {
        s_gwHeartbeatTimer = xTimerCreate("gwHb", pdMS_TO_TICKS(hbInterval * 1000),
                                           pdTRUE, nullptr, gwHeartbeatCb);
    } else {
        xTimerChangePeriod(s_gwHeartbeatTimer, pdMS_TO_TICKS(hbInterval * 1000), 0);
    }
    xTimerStart(s_gwHeartbeatTimer, 0);

    // Phase 5: Web UI
    if (SqWebServer::hasWifiCreds()) {
        SqWebServer::start();
    } else {
        // No WiFi creds — need Setup Delegate mode
        // Lone gateway (0 peers) reboots itself as delegate
        if (m_peerCount == 0) {
            SqLog.println("[gateway] No WiFi creds, rebooting as delegate for setup");
            rtc_state_t* rtc = RtcState::get();
            rtc->next_role = (uint8_t)RoleId::DELEGATE;
            RtcState::save();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            // Has peers — pick best alive peer and designate as delegate
            uint8_t bestIdx = 0;
            uint16_t bestBat = 0;
            uint8_t count = PeerTable::peerCount();
            for (uint8_t i = 1; i < count; i++) {
                PeerEntry* e = PeerTable::getEntryByIndex(i);
                if (!e || (e->flags & PEER_STATUS_DEAD)) continue;
                if (e->battery_mv > bestBat) {
                    bestBat = e->battery_mv;
                    bestIdx = i;
                }
            }

            if (bestIdx > 0) {
                PeerEntry* delegate = PeerTable::getEntryByIndex(bestIdx);
                SqLog.printf("[gateway] Designating peer %02X:%02X:%02X:%02X:%02X:%02X as delegate\n",
                    delegate->mac[0], delegate->mac[1], delegate->mac[2],
                    delegate->mac[3], delegate->mac[4], delegate->mac[5]);

                uint8_t ownMac[6];
                esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
                SetupDelegateMsg msg = {};
                msg.type = MSG_TYPE_SETUP_DELEGATE;
                memcpy(msg.gateway_mac, ownMac, 6);
                MeshConductor::sendToNode(delegate->mac, &msg, sizeof(msg));
            } else {
                // No alive peers — self-delegate
                SqLog.println("[gateway] No alive peers, rebooting as delegate for setup");
                rtc_state_t* rtc = RtcState::get();
                rtc->next_role = (uint8_t)RoleId::DELEGATE;
                RtcState::save();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }
    }
}

void Gateway::end() {
    SqLog.println("[gateway] Gateway role stopping");

    // Phase 5: Web UI — stop before mesh teardown
    SqWebServer::stop();

    if (s_gwHeartbeatTimer) {
        xTimerStop(s_gwHeartbeatTimer, 0);
    }

    Orchestrator::setMode(ORCH_OFF);
    ClockSync::stop();

    FtmScheduler::shutdown();
    PeerTable::shutdown();
}

void Gateway::onPeerJoined(const uint8_t* mac) {
    m_peerCount++;
    SqLog.printf("[gateway] Peer joined (%u total): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Peer will send heartbeat shortly — PeerTable entry created on first heartbeat.
    // If we want immediate FTM, queue the new node once it appears in PeerTable.
}

void Gateway::onPeerLeft(const uint8_t* mac) {
    if (m_peerCount > 0) m_peerCount--;
    SqLog.printf("[gateway] Peer left (%u remaining): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Mark peer as dead in PeerTable
    PeerEntry* e = PeerTable::getEntry(mac);
    if (e) {
        e->flags = PEER_STATUS_DEAD;
    }
}

void Gateway::printStatus() {
    Serial.println("--- Gateway Status ---");
    Serial.printf("Peers: %u\n", m_peerCount);
    PeerTable::print();
}
