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
#include "setup_delegate.h"
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
        // No WiFi creds — enter Setup Delegate mode
        // Lone gateway (0 peers) handles it itself
        if (m_peerCount == 0) {
            uint8_t ownMac[6];
            esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
            SqLog.println("[gateway] No WiFi creds, self-delegating for setup");
            SetupDelegate::begin(ownMac);
        } else {
            // TODO: designate a peer as Setup Delegate (send MSG_TYPE_SETUP_DELEGATE)
            // For now, self-delegate even with peers
            uint8_t ownMac[6];
            esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
            SqLog.println("[gateway] No WiFi creds, self-delegating for setup (has peers)");
            SetupDelegate::begin(ownMac);
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
