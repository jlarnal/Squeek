#include "rtc_state.h"
#include <Arduino.h>
#include <string.h>
#include <esp_mac.h>
#include <esp_rom_crc.h>

RTC_NOINIT_ATTR static rtc_state_t s_state;

static uint32_t computeCrc() {
    // CRC covers everything except the crc field itself
    size_t len = offsetof(rtc_state_t, crc);
    return esp_rom_crc32_le(0, (const uint8_t*)&s_state, len);
}

void RtcState::init() {
    if (!isValid()) {
        clear();
        esp_read_mac(s_state.own_mac, ESP_MAC_WIFI_STA);
        s_state.magic = RTC_STATE_MAGIC;
        s_state.mesh_channel = MESH_CHANNEL;
        save();
    }
}

bool RtcState::isValid() {
    if (s_state.magic != RTC_STATE_MAGIC) return false;
    return s_state.crc == computeCrc();
}

void RtcState::save() {
    s_state.crc = computeCrc();
}

void RtcState::clear() {
    memset(&s_state, 0, sizeof(s_state));
}

rtc_state_t* RtcState::get() {
    return &s_state;
}

void RtcState::print() {
    Serial.println("=== RTC State ===");
    Serial.printf("Valid: %s\n", isValid() ? "yes" : "no");
    Serial.printf("Own MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        s_state.own_mac[0], s_state.own_mac[1], s_state.own_mac[2],
        s_state.own_mac[3], s_state.own_mac[4], s_state.own_mac[5]);
    Serial.printf("Short ID: %u  Role: %s\n",
        s_state.own_short_id,
        s_state.own_role == 1 ? "gateway" : "peer");
    Serial.printf("Gateway MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        s_state.gateway_mac[0], s_state.gateway_mac[1], s_state.gateway_mac[2],
        s_state.gateway_mac[3], s_state.gateway_mac[4], s_state.gateway_mac[5]);
    Serial.printf("Channel: %u  Peers: %u  Generation: %lu\n",
        s_state.mesh_channel, s_state.peer_count, s_state.mesh_generation);
    Serial.printf("Delegate active: %s\n", s_state.delegate_active ? "yes" : "no");

    for (uint8_t i = 0; i < s_state.peer_count && i < MESH_MAX_NODES; i++) {
        const rtc_peer_entry_t& p = s_state.peers[i];
        Serial.printf("  Peer[%u] MAC=%02X:%02X:%02X:%02X:%02X:%02X id=%u flags=0x%02X\n",
            i, p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
            p.short_id, p.flags);
    }
}
