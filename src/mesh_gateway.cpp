#include "mesh_conductor.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "position_solver.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include <Arduino.h>

// Gateway self-heartbeat timer — updates own battery in PeerTable
static TimerHandle_t s_gwHeartbeatTimer = nullptr;

static void gwHeartbeatCb(TimerHandle_t t) {
    (void)t;
    PeerTable::updateSelf((uint16_t)PowerManager::batteryMv());
}

void Gateway::begin() {
    m_peerCount = 0;
    Serial.println("[gateway] Gateway role active");

    // Initialize Phase 2 subsystems
    PeerTable::init();
    FtmManager::init();
    PositionSolver::init();
    FtmScheduler::init();

    // Start self-heartbeat timer (update own battery in PeerTable)
    uint32_t hbInterval = (uint32_t)NvsConfigManager::heartbeatInterval_s;
    if (s_gwHeartbeatTimer == nullptr) {
        s_gwHeartbeatTimer = xTimerCreate("gwHb", pdMS_TO_TICKS(hbInterval * 1000),
                                           pdTRUE, nullptr, gwHeartbeatCb);
    } else {
        xTimerChangePeriod(s_gwHeartbeatTimer, pdMS_TO_TICKS(hbInterval * 1000), 0);
    }
    xTimerStart(s_gwHeartbeatTimer, 0);
}

void Gateway::end() {
    Serial.println("[gateway] Gateway role stopping");

    if (s_gwHeartbeatTimer) {
        xTimerStop(s_gwHeartbeatTimer, 0);
    }

    FtmScheduler::shutdown();
    PeerTable::shutdown();
}

void Gateway::onPeerJoined(const uint8_t* mac) {
    m_peerCount++;
    Serial.printf("[gateway] Peer joined (%u total): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Peer will send heartbeat shortly — PeerTable entry created on first heartbeat.
    // If we want immediate FTM, queue the new node once it appears in PeerTable.
}

void Gateway::onPeerLeft(const uint8_t* mac) {
    if (m_peerCount > 0) m_peerCount--;
    Serial.printf("[gateway] Peer left (%u remaining): %02X:%02X:%02X:%02X:%02X:%02X\n",
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
