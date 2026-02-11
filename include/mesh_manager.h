#ifndef MESH_MANAGER_H
#define MESH_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "rtc_mesh_map.h"

void     mesh_init();
void     mesh_start();
void     mesh_stop();
bool     mesh_is_connected();
bool     mesh_is_root();
uint8_t  mesh_get_peer_count();
void     mesh_get_peers(rtc_peer_entry_t* out, uint8_t* count);
uint8_t  mesh_get_layer();
void     mesh_force_reelection();
void     mesh_print_status();

#endif // MESH_MANAGER_H
