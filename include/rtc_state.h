#ifndef RTC_STATE_H
#define RTC_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp.hpp"

#define RTC_STATE_MAGIC  0x53514B03  // "SQK" + version 3

// Peer flags (carried over from rtc_mesh_map.h)
#define PEER_FLAG_ALIVE    0x01
#define PEER_FLAG_SLEEPING 0x02

struct rtc_peer_entry_t {
    uint8_t  mac[6];
    uint8_t  short_id;
    uint8_t  flags;
};

struct rtc_state_t {
    uint32_t magic;

    // Mesh map
    uint8_t  own_mac[6];
    uint8_t  own_short_id;
    uint8_t  own_role;           // 0=peer, 1=gateway, 2=delegate
    uint8_t  gateway_mac[6];
    uint8_t  mesh_channel;
    uint8_t  peer_count;
    rtc_peer_entry_t peers[MESH_MAX_NODES];
    float    own_position[3];
    uint32_t ftm_epoch;
    uint32_t mesh_generation;

    // Boot state
    uint8_t  delegate_active;    // nonzero = was in setup delegate mode
    uint8_t  next_role;          // role to boot into (0xFF = normal election)
    uint8_t  _reserved[2];      // alignment padding

    // Integrity (must be last)
    uint32_t crc;                // esp_rom_crc32_le over everything above
};

class RtcState {
public:
    static void           init();
    static bool           isValid();
    static void           save();
    static void           clear();
    static rtc_state_t*   get();
    static void           print();

private:
    RtcState() = delete;
};

#endif // RTC_STATE_H
