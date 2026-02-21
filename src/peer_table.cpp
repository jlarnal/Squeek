#include "peer_table.h"
#include "mesh_conductor.h"
#include "nvs_config.h"
#include "power_manager.h"
#include "sq_log.h"
#include <Arduino.h>
#include <esp_mac.h>
#include <string.h>

static const char* TAG = "ptable";

// --- File-scope state ---

static PeerEntry  s_entries[MESH_MAX_NODES];
static uint8_t    s_count = 0;   // total slots in use (index 0 = gateway self)
static TimerHandle_t s_stalenessTimer = nullptr;
static uint32_t   s_lastBroadcastHash = 0;  // change-detection for broadcastSync

// --- Helpers ---

static int8_t findByMac(const uint8_t* mac) {
    for (uint8_t i = 0; i < s_count; i++) {
        if (memcmp(s_entries[i].mac, mac, 6) == 0)
            return (int8_t)i;
    }
    return -1;
}

static void clearEntry(PeerEntry* e) {
    memset(e, 0, sizeof(PeerEntry));
    for (int i = 0; i < MESH_MAX_NODES; i++)
        e->distances[i] = -1.0f;
    e->position[0] = 0.0f;
    e->position[1] = 0.0f;
    e->position[2] = 0.0f;
    e->confidence = 0.0f;
}

static void stalenessTimerCb(TimerHandle_t t) {
    (void)t;
    PeerTable::scanStaleness();
    PeerTable::checkReelection();
}

// --- Public API ---

void PeerTable::init() {
    s_count = 0;
    for (int i = 0; i < MESH_MAX_NODES; i++)
        clearEntry(&s_entries[i]);

    // Insert self as slot 0
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
    memcpy(s_entries[0].mac, own_mac, 6);

    // Derive SoftAP MAC (typically STA + 1 on last byte)
    uint8_t ap_mac[6];
    esp_read_mac(ap_mac, ESP_MAC_WIFI_SOFTAP);
    memcpy(s_entries[0].softap_mac, ap_mac, 6);

    s_entries[0].battery_mv = (uint16_t)PowerManager::batteryMv();
    s_entries[0].last_seen_ms = millis();
    s_entries[0].flags = PEER_STATUS_ALIVE;
    s_count = 1;

    // Start staleness scanner (every 60s)
    if (s_stalenessTimer == nullptr) {
        s_stalenessTimer = xTimerCreate("staleness", pdMS_TO_TICKS(60000),
                                         pdTRUE, nullptr, stalenessTimerCb);
    }
    xTimerStart(s_stalenessTimer, 0);

    SqLog.printf("[ptable] Initialized, self = slot 0\n");
}

void PeerTable::shutdown() {
    if (s_stalenessTimer) {
        xTimerStop(s_stalenessTimer, 0);
    }
    s_count = 0;
    SqLog.println("[ptable] Shutdown");
}

void PeerTable::updateFromHeartbeat(const uint8_t* mac, uint16_t battery_mv,
                                     uint8_t flags, const uint8_t* softap_mac) {
    int8_t idx = findByMac(mac);
    bool newPeer = false;
    bool wasDeadNowAlive = false;

    if (idx < 0) {
        // New peer — add to table
        if (s_count >= MESH_MAX_NODES) {
            SqLog.println("[ptable] Table full, ignoring new peer");
            return;
        }
        idx = s_count++;
        clearEntry(&s_entries[idx]);
        memcpy(s_entries[idx].mac, mac, 6);
        newPeer = true;
        SqLog.printf("[ptable] New peer at slot %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
            idx, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        wasDeadNowAlive = (s_entries[idx].flags & PEER_STATUS_DEAD) != 0;
    }

    s_entries[idx].battery_mv = battery_mv;
    s_entries[idx].last_seen_ms = millis();
    s_entries[idx].flags = flags | PEER_STATUS_ALIVE;
    s_entries[idx].flags &= ~PEER_STATUS_DEAD;

    if (softap_mac) {
        memcpy(s_entries[idx].softap_mac, softap_mac, 6);
    }

    if (newPeer || wasDeadNowAlive) {
        broadcastSync();
    }
}

void PeerTable::updateSelf(uint16_t battery_mv) {
    s_entries[0].battery_mv = battery_mv;
    s_entries[0].last_seen_ms = millis();
}

void PeerTable::scanStaleness() {
    uint32_t now = millis();
    uint32_t stale_ms = (uint32_t)NvsConfigManager::heartbeatInterval_s
                      * (uint32_t)NvsConfigManager::heartbeatStaleMultiplier
                      * 1000;

    bool anyChanged = false;
    for (uint8_t i = 1; i < s_count; i++) {  // skip self (slot 0)
        if (s_entries[i].flags & PEER_STATUS_DEAD)
            continue;

        if ((now - s_entries[i].last_seen_ms) > stale_ms) {
            s_entries[i].flags = PEER_STATUS_DEAD;
            anyChanged = true;
            SqLog.printf("[ptable] Peer slot %d DEAD (stale %lu ms)\n",
                i, now - s_entries[i].last_seen_ms);
        }
    }

    if (anyChanged) {
        broadcastSync();
    }
}

void PeerTable::checkReelection() {
    uint16_t gw_battery = s_entries[0].battery_mv;
    uint16_t best_battery = 0;
    uint16_t delta = (uint16_t)(uint32_t)NvsConfigManager::reelectionBatteryDelta_mv;

    for (uint8_t i = 1; i < s_count; i++) {
        if (s_entries[i].flags & PEER_STATUS_DEAD)
            continue;
        if (s_entries[i].battery_mv > best_battery)
            best_battery = s_entries[i].battery_mv;
    }

    if (best_battery > gw_battery && (best_battery - gw_battery) >= delta) {
        SqLog.printf("[ptable] Re-election: gateway battery %u mV, best peer %u mV (delta >= %u)\n",
            gw_battery, best_battery, delta);
        MeshConductor::forceReelection();
    }
}

PeerEntry* PeerTable::getEntry(const uint8_t* mac) {
    int8_t idx = findByMac(mac);
    return (idx >= 0) ? &s_entries[idx] : nullptr;
}

PeerEntry* PeerTable::getEntryByIndex(uint8_t idx) {
    return (idx < s_count) ? &s_entries[idx] : nullptr;
}

int8_t PeerTable::getIndex(const uint8_t* mac) {
    return findByMac(mac);
}

uint8_t PeerTable::peerCount() {
    return s_count;
}

uint8_t PeerTable::alivePeerCount() {
    uint8_t alive = 0;
    for (uint8_t i = 0; i < s_count; i++) {
        if (!(s_entries[i].flags & PEER_STATUS_DEAD))
            alive++;
    }
    return alive;
}

void PeerTable::setDistance(uint8_t idxA, uint8_t idxB, float distance_cm) {
    if (idxA < s_count && idxB < s_count) {
        s_entries[idxA].distances[idxB] = distance_cm;
        s_entries[idxB].distances[idxA] = distance_cm;
    }
}

float PeerTable::getDistance(uint8_t idxA, uint8_t idxB) {
    if (idxA < s_count && idxB < s_count)
        return s_entries[idxA].distances[idxB];
    return -1.0f;
}

void PeerTable::setPosition(uint8_t idx, float x, float y, float z, float confidence) {
    if (idx < s_count) {
        s_entries[idx].position[0] = x;
        s_entries[idx].position[1] = y;
        s_entries[idx].position[2] = z;
        s_entries[idx].confidence = confidence;
    }
}

uint8_t PeerTable::getDimension() {
    uint8_t alive = alivePeerCount();
    if (alive <= 2) return 1;
    if (alive == 3) return 2;
    return 3;
}

// --- Seed from shadow (role transfer) ---

void PeerTable::seedFromShadow(const PeerSyncEntry* entries, uint8_t count) {
    // PeerTable::init() was already called by Gateway::begin() and created slot 0 = self.
    // Now populate remaining slots from the shadow data (skipping our own MAC).
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    for (uint8_t i = 0; i < count && s_count < MESH_MAX_NODES; i++) {
        if (memcmp(entries[i].mac, own_mac, 6) == 0) continue;  // skip self
        if (entries[i].flags & PEER_STATUS_DEAD) continue;       // skip dead

        int8_t idx = findByMac(entries[i].mac);
        if (idx >= 0) continue;  // already present

        idx = s_count++;
        clearEntry(&s_entries[idx]);
        memcpy(s_entries[idx].mac, entries[i].mac, 6);
        memcpy(s_entries[idx].softap_mac, entries[i].softap_mac, 6);
        s_entries[idx].battery_mv = entries[i].battery_mv;
        s_entries[idx].last_seen_ms = millis();
        s_entries[idx].flags = PEER_STATUS_ALIVE;

        SqLog.printf("[ptable] Seeded slot %d from shadow: %02X:%02X:%02X:%02X:%02X:%02X\n",
            idx, entries[i].mac[0], entries[i].mac[1], entries[i].mac[2],
            entries[i].mac[3], entries[i].mac[4], entries[i].mac[5]);
    }

    SqLog.printf("[ptable] Seeded %u total entries from shadow\n", s_count);
    broadcastSync();
}

// --- Sync broadcast ---

static uint32_t computeSyncHash() {
    uint32_t h = (uint32_t)s_count;
    for (uint8_t i = 0; i < s_count; i++) {
        h ^= ((uint32_t)s_entries[i].flags << (i & 0x1F));
    }
    return h;
}

void PeerTable::broadcastSync() {
    uint32_t hash = computeSyncHash();
    if (hash == s_lastBroadcastHash) return;
    s_lastBroadcastHash = hash;

    // Build PeerSyncMsg + entries into a stack buffer
    uint8_t buf[2 + MESH_MAX_NODES * sizeof(PeerSyncEntry)];
    PeerSyncMsg* msg = (PeerSyncMsg*)buf;
    msg->type = MSG_TYPE_PEER_SYNC;
    msg->count = s_count;

    PeerSyncEntry* entries = (PeerSyncEntry*)(buf + sizeof(PeerSyncMsg));
    for (uint8_t i = 0; i < s_count; i++) {
        memcpy(entries[i].mac, s_entries[i].mac, 6);
        memcpy(entries[i].softap_mac, s_entries[i].softap_mac, 6);
        entries[i].battery_mv = s_entries[i].battery_mv;
        entries[i].flags = s_entries[i].flags;
    }

    uint16_t totalLen = (uint16_t)(sizeof(PeerSyncMsg) + s_count * sizeof(PeerSyncEntry));
    MeshConductor::broadcastToAll(buf, totalLen);
    SqLog.printf("[ptable] Broadcast peer sync (%u entries)\n", s_count);
}

void PeerTable::print() {
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    SqLog.println("=== Peer Table ===");
    SqLog.printf("Entries: %u, Alive: %u, Dimension: %uD\n",
        s_count, alivePeerCount(), getDimension());

    for (uint8_t i = 0; i < s_count; i++) {
        PeerEntry* e = &s_entries[i];
        const char* status = (e->flags & PEER_STATUS_DEAD) ? "DEAD " :
                             (e->flags & PEER_STATUS_SLEEPING) ? "SLEEP" : "ALIVE";
        bool isGw   = (i == 0);
        bool isSelf = (memcmp(e->mac, own_mac, 6) == 0);
        const char* suffix = (isGw && isSelf) ? " <-- Gateway, this" :
                             isGw             ? " <-- Gateway" :
                             isSelf           ? " <-- this" : "";
        SqLog.printf("  [%u] %02X:%02X:%02X:%02X:%02X:%02X  bat=%umV  %s  pos=(%6.0f,%6.0f,%6.0f) conf=%.2f%s\n",
            i, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
            e->battery_mv, status, e->position[0], e->position[1], e->position[2],
            e->confidence, suffix);
    }
}
