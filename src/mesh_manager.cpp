#include "mesh_manager.h"
#include "bsp.hpp"
#include <Arduino.h>
#include <string.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <nvs_flash.h>

static const char* TAG = "mesh";

static bool s_mesh_connected = false;
static bool s_mesh_started = false;
static uint8_t s_mesh_id[6] = { 0x53, 0x51, 0x45, 0x45, 0x4B, 0x00 }; // "SQUEEK"

static void update_rtc_map() {
    rtc_mesh_map_t* map = rtc_map_get();
    map->own_role = esp_mesh_is_root() ? 1 : 0;
    map->mesh_channel = MESH_CHANNEL;

    // Get routing table for peer list
    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);

    // Get own MAC
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    uint8_t count = 0;
    for (int i = 0; i < table_size && count < MESH_MAX_NODES; i++) {
        // Skip self
        if (memcmp(routing_table[i].addr, own_mac, 6) == 0) continue;
        memcpy(map->peers[count].mac, routing_table[i].addr, 6);
        map->peers[count].short_id = count + 1;
        map->peers[count].flags = PEER_FLAG_ALIVE;
        count++;
    }
    map->peer_count = count;
    map->mesh_generation++;

    // Root address is captured from MESH_EVENT_ROOT_ADDRESS event
    // If this node is root, set own MAC as gateway
    if (esp_mesh_is_root()) {
        memcpy(map->gateway_mac, own_mac, 6);
    }

    rtc_map_save();
}

static void mesh_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    switch (event_id) {
    case MESH_EVENT_STARTED:
        Serial.println("[mesh] Mesh started");
        s_mesh_started = true;
        break;

    case MESH_EVENT_STOPPED:
        Serial.println("[mesh] Mesh stopped");
        s_mesh_started = false;
        s_mesh_connected = false;
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        Serial.println("[mesh] Parent connected");
        s_mesh_connected = true;
        if (esp_mesh_is_root()) {
            Serial.println("[mesh] I am ROOT (gateway)");
        }
        update_rtc_map();
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED:
        Serial.println("[mesh] Parent disconnected");
        s_mesh_connected = false;
        update_rtc_map();
        break;

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t* child = (mesh_event_child_connected_t*)event_data;
        Serial.printf("[mesh] Child connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5]);
        update_rtc_map();
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t* child = (mesh_event_child_disconnected_t*)event_data;
        Serial.printf("[mesh] Child disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5]);
        update_rtc_map();
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
        update_rtc_map();
        break;

    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t* root = (mesh_event_root_address_t*)event_data;
        Serial.printf("[mesh] Root address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            root->addr[0], root->addr[1], root->addr[2],
            root->addr[3], root->addr[4], root->addr[5]);
        // Store gateway MAC from event
        rtc_mesh_map_t* map = rtc_map_get();
        memcpy(map->gateway_mac, root->addr, 6);
        update_rtc_map();
        break;
    }

    case MESH_EVENT_NO_PARENT_FOUND:
        Serial.println("[mesh] No parent found, scanning...");
        break;

    default:
        Serial.printf("[mesh] Event %ld\n", event_id);
        break;
    }
}

void mesh_init() {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default netif for mesh
    esp_netif_create_default_wifi_mesh_netifs(NULL, NULL);

    // Initialize WiFi
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize mesh
    ESP_ERROR_CHECK(esp_mesh_init());

    // Register mesh event handler
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                &mesh_event_handler, NULL));
}

void mesh_start() {
    // Allow routerless mesh — must be set before esp_mesh_set_config()
    // so the config validation doesn't reject an empty router SSID.
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.channel = MESH_CHANNEL;
    memcpy((uint8_t*)&cfg.mesh_id, s_mesh_id, 6);

    // No external router — self-contained mesh
    memset(&cfg.router, 0, sizeof(cfg.router));

    // Mesh AP settings (no password for Phase 1)
    cfg.mesh_ap.max_connection = 6;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));

    // No encryption for Phase 1
    cfg.crypto_funcs = NULL;

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    // Configure mesh topology
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    // All nodes vote for root election
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1.0));

    // Allow root without external router
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(true));

    ESP_ERROR_CHECK(esp_mesh_start());
    Serial.println("[mesh] Mesh starting...");
}

void mesh_stop() {
    esp_mesh_stop();
    s_mesh_started = false;
    s_mesh_connected = false;
}

bool mesh_is_connected() {
    return s_mesh_connected;
}

bool mesh_is_root() {
    return esp_mesh_is_root();
}

uint8_t mesh_get_peer_count() {
    rtc_mesh_map_t* map = rtc_map_get();
    return map->peer_count;
}

void mesh_get_peers(rtc_peer_entry_t* out, uint8_t* count) {
    rtc_mesh_map_t* map = rtc_map_get();
    uint8_t n = map->peer_count;
    if (n > MESH_MAX_NODES) n = MESH_MAX_NODES;
    memcpy(out, map->peers, n * sizeof(rtc_peer_entry_t));
    *count = n;
}

uint8_t mesh_get_layer() {
    return (uint8_t)esp_mesh_get_layer();
}

void mesh_force_reelection() {
    Serial.println("[mesh] Forcing root waive...");
    if (esp_mesh_is_root()) {
        esp_mesh_waive_root(NULL, MESH_VOTE_REASON_ROOT_INITIATED);
    } else {
        Serial.println("[mesh] Not root, cannot waive. Requesting vote...");
        // Non-root nodes can request a new election by disconnecting briefly
        esp_mesh_stop();
        delay(1000);
        mesh_start();
    }
}

void mesh_print_status() {
    Serial.println("=== Mesh Status ===");
    Serial.printf("Started: %s\n", s_mesh_started ? "yes" : "no");
    Serial.printf("Connected: %s\n", s_mesh_connected ? "yes" : "no");
    Serial.printf("Is Root: %s\n", esp_mesh_is_root() ? "yes" : "no");
    Serial.printf("Layer: %d\n", esp_mesh_get_layer());

    int total = esp_mesh_get_total_node_num();
    Serial.printf("Total nodes: %d\n", total);

    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);
    Serial.printf("Routing table size: %d\n", table_size);

    for (int i = 0; i < table_size; i++) {
        Serial.printf("  [%d] %02X:%02X:%02X:%02X:%02X:%02X\n", i,
            routing_table[i].addr[0], routing_table[i].addr[1],
            routing_table[i].addr[2], routing_table[i].addr[3],
            routing_table[i].addr[4], routing_table[i].addr[5]);
    }
}
