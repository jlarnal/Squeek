#include "mesh_conductor.h"
#include "bsp.hpp"
#include <Arduino.h>
#include <esp_system.h>

void MeshNode::begin() {
    m_gatewayAlive = true;
    Serial.println("[node] MeshNode role active");
}

void MeshNode::end() {
    Serial.println("[node] MeshNode role stopping");
}

void MeshNode::onPeerJoined(const uint8_t* mac) {
    Serial.printf("[node] Peer joined: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void MeshNode::onPeerLeft(const uint8_t* mac) {
    Serial.printf("[node] Peer left: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void MeshNode::onGatewayLost() {
    Serial.println("[node] WARNING: Gateway lost — sleep and reboot for re-election");
    m_gatewayAlive = false;
    MeshConductor::stop();
    SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
    esp_restart();
}

void MeshNode::printStatus() {
    Serial.println("--- Node Status ---");
    Serial.printf("Gateway alive: %s\n", m_gatewayAlive ? "yes" : "no");
}
