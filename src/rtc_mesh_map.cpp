#include "rtc_mesh_map.h"
#include <Arduino.h>
#include <string.h>
#include <esp_mac.h>

RTC_DATA_ATTR static rtc_mesh_map_t mesh_map;

static uint32_t compute_checksum() {
    const uint8_t* data = (const uint8_t*)&mesh_map;
    // Checksum covers everything except the checksum field itself
    size_t len = offsetof(rtc_mesh_map_t, checksum);
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
        sum = (sum << 1) | (sum >> 31);  // rotate left
    }
    return sum;
}

void RtcMap::init() {
    if (!isValid()) {
        clear();
        esp_read_mac(mesh_map.own_mac, ESP_MAC_WIFI_STA);
        mesh_map.magic = RTC_MAP_MAGIC;
        mesh_map.mesh_channel = MESH_CHANNEL;
        save();
    }
}

bool RtcMap::isValid() {
    if (mesh_map.magic != RTC_MAP_MAGIC) return false;
    return mesh_map.checksum == compute_checksum();
}

void RtcMap::save() {
    mesh_map.checksum = compute_checksum();
}

void RtcMap::clear() {
    memset(&mesh_map, 0, sizeof(mesh_map));
}

rtc_mesh_map_t* RtcMap::get() {
    return &mesh_map;
}

void RtcMap::print() {
    Serial.println("=== RTC Mesh Map ===");
    Serial.printf("Valid: %s\n", isValid() ? "yes" : "no");
    Serial.printf("Own MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mesh_map.own_mac[0], mesh_map.own_mac[1], mesh_map.own_mac[2],
        mesh_map.own_mac[3], mesh_map.own_mac[4], mesh_map.own_mac[5]);
    Serial.printf("Short ID: %u  Role: %s\n",
        mesh_map.own_short_id,
        mesh_map.own_role == 1 ? "gateway" : "peer");
    Serial.printf("Gateway MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        mesh_map.gateway_mac[0], mesh_map.gateway_mac[1], mesh_map.gateway_mac[2],
        mesh_map.gateway_mac[3], mesh_map.gateway_mac[4], mesh_map.gateway_mac[5]);
    Serial.printf("Channel: %u  Peers: %u  Generation: %lu\n",
        mesh_map.mesh_channel, mesh_map.peer_count, mesh_map.mesh_generation);

    for (uint8_t i = 0; i < mesh_map.peer_count && i < MESH_MAX_NODES; i++) {
        const rtc_peer_entry_t& p = mesh_map.peers[i];
        Serial.printf("  Peer[%u] MAC=%02X:%02X:%02X:%02X:%02X:%02X id=%u flags=0x%02X\n",
            i, p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
            p.short_id, p.flags);
    }
}
