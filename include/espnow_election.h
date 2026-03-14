#ifndef ESPNOW_ELECTION_H
#define ESPNOW_ELECTION_H

#include <stdint.h>
#include <stdbool.h>

#define ELECTION_MAX_CANDIDATES  16   // == MESH_MAX_NODES
#define ELECTION_FRAME_SIZE       9   // mac[6] + tenure_score[2] + target_channel[1]

struct ElectionCandidate {
    uint8_t  mac[6];
    uint16_t tenure_score;
    uint8_t  target_channel;
};

struct ElectionResult {
    uint8_t  winner_mac[6];
    uint8_t  target_channel;      // channel the mesh should use
    bool     i_am_winner;         // true if this node won
    uint8_t  candidate_count;     // how many nodes participated
};

class EspNowElection {
public:
    /// Run the full election sequence (blocking, ~5-14s).
    /// Call AFTER MeshConductor::init() (WiFi STA is up) and BEFORE esp_mesh_start().
    /// @return election result with winner info and target channel
    static ElectionResult run();

private:
    EspNowElection() = delete;
};

#endif // ESPNOW_ELECTION_H
