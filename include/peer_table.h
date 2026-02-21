#ifndef PEER_TABLE_H
#define PEER_TABLE_H

#include "bsp.hpp"
#include <stdint.h>

// Forward declaration (defined in mesh_conductor.h)
struct PeerSyncEntry;

// Peer status flags
#define PEER_STATUS_ALIVE     0x01
#define PEER_STATUS_SLEEPING  0x02
#define PEER_STATUS_DEAD      0x04
#define PEER_STATUS_FTM_READY 0x08

struct PeerEntry {
    uint8_t  mac[6];
    uint8_t  softap_mac[6];            // SoftAP MAC for FTM targeting
    uint16_t battery_mv;
    uint32_t last_seen_ms;              // millis() of last heartbeat
    uint8_t  flags;                     // PEER_STATUS_*
    float    distances[MESH_MAX_NODES]; // distance to each peer by index (-1 = unknown)
    float    position[3];               // x, y, z in cm
    float    confidence;                // from Kalman covariance
    uint8_t  ftm_epoch;                 // which sweep this data is from
};

class PeerTable {
public:
    static void init();
    static void shutdown();

    // Heartbeat handling
    static void updateFromHeartbeat(const uint8_t* mac, uint16_t battery_mv,
                                    uint8_t flags, const uint8_t* softap_mac);
    static void updateSelf(uint16_t battery_mv);

    // Staleness & re-election
    static void scanStaleness();
    static void checkReelection();

    // Lookup
    static PeerEntry* getEntry(const uint8_t* mac);
    static PeerEntry* getEntryByIndex(uint8_t idx);
    static int8_t     getIndex(const uint8_t* mac);
    static uint8_t    peerCount();
    static uint8_t    alivePeerCount();

    // FTM distance update
    static void setDistance(uint8_t idxA, uint8_t idxB, float distance_cm);
    static float getDistance(uint8_t idxA, uint8_t idxB);

    // Position update
    static void setPosition(uint8_t idx, float x, float y, float z, float confidence);

    // Dimension tracking
    static uint8_t getDimension();  // 1, 2, or 3

    // Seed from peer shadow (used during role transfer)
    static void seedFromShadow(const PeerSyncEntry* entries, uint8_t count);

    // Sync to peers
    static void broadcastSync();

    // Debug
    static void print();

private:
    PeerTable() = delete;
};

#endif // PEER_TABLE_H
