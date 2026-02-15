#include "mesh_conductor.h"
#include "bsp.hpp"
#include <Arduino.h>

void Gateway::begin() {
    m_peerCount = 0;
    Serial.println("[gateway] Gateway role active — SoftAP stub (Phase 1)");
}

void Gateway::end() {
    Serial.println("[gateway] Gateway role stopping");
    // Phase 1 stub — SoftAP teardown will go here
}

void Gateway::onPeerJoined(const uint8_t* mac) {
    m_peerCount++;
    Serial.printf("[gateway] Peer joined (%u total): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void Gateway::onPeerLeft(const uint8_t* mac) {
    if (m_peerCount > 0) m_peerCount--;
    Serial.printf("[gateway] Peer left (%u remaining): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void Gateway::printStatus() {
    Serial.println("--- Gateway Status ---");
    Serial.printf("Peers: %u\n", m_peerCount);
}
