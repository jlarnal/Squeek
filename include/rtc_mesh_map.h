#ifndef RTC_MESH_MAP_H
#define RTC_MESH_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "bsp.hpp"

#define RTC_MAP_MAGIC  0x53514B01  // "SQK" + version 1

// Peer flags
#define PEER_FLAG_ALIVE    0x01
#define PEER_FLAG_SLEEPING 0x02

struct rtc_peer_entry_t {
    uint8_t  mac[6];
    uint8_t  short_id;
    uint8_t  flags;
};

struct rtc_mesh_map_t {
    uint32_t magic;
    uint8_t  own_mac[6];
    uint8_t  own_short_id;
    uint8_t  own_role;       // 0=peer, 1=gateway
    uint8_t  gateway_mac[6];
    uint8_t  mesh_channel;
    uint8_t  peer_count;
    rtc_peer_entry_t peers[MESH_MAX_NODES];
    float    own_position[3];   // placeholder for Phase 2
    uint32_t ftm_epoch;         // placeholder for Phase 2
    uint32_t mesh_generation;
    uint32_t checksum;
};

void            rtc_map_init();
bool            rtc_map_is_valid();
void            rtc_map_save();
void            rtc_map_clear();
rtc_mesh_map_t* rtc_map_get();
void            rtc_map_print();

#endif // RTC_MESH_MAP_H
