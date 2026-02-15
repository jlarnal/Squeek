#include "mesh_conductor.h"
#include "bsp.hpp"
#include "rtc_mesh_map.h"
#include "power_manager.h"
#include "nvs_config.h"
#include <Arduino.h>
#include <string.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char* TAG = "mesh";

// --- File-scope state ---

static IMeshRole*  s_role           = nullptr;
static bool        s_connected      = false;
static bool        s_started        = false;
static bool        s_electionDone   = false;
static uint8_t     s_meshId[6]      = { 0x53, 0x51, 0x45, 0x45, 0x4B, 0x00 }; // "SQUEEK"
static Gateway     s_gateway;
static MeshNode    s_meshNode;

// Election state
static uint8_t     s_parentRetries  = 0;
static TimerHandle_t s_electTimer   = nullptr;
static ElectionScore s_scores[MESH_MAX_NODES];
static uint8_t     s_scoreCount     = 0;
static uint16_t    s_gwTenure       = 0;        // cached from NVS

// --- NVS tenure helpers ---

static void nvsReadTenure() {
    nvs_handle_t h;
    if (nvs_open("squeek", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "gw_tenure", &s_gwTenure);
        nvs_close(h);
    }
}

static void nvsWriteTenure() {
    nvs_handle_t h;
    if (nvs_open("squeek", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "gw_tenure", s_gwTenure);
        nvs_commit(h);
        nvs_close(h);
    }
}

// --- RTC map update (carried over from mesh_manager) ---

static void updateRtcMap() {
    rtc_mesh_map_t* map = RtcMap::get();
    map->own_role = (s_role && s_role->isGateway()) ? 1 : 0;
    map->mesh_channel = MESH_CHANNEL;

    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);

    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    uint8_t count = 0;
    for (int i = 0; i < table_size && count < MESH_MAX_NODES; i++) {
        if (memcmp(routing_table[i].addr, own_mac, 6) == 0) continue;
        memcpy(map->peers[count].mac, routing_table[i].addr, 6);
        map->peers[count].short_id = count + 1;
        map->peers[count].flags = PEER_FLAG_ALIVE;
        count++;
    }
    map->peer_count = count;
    map->mesh_generation++;

    if (esp_mesh_is_root()) {
        memcpy(map->gateway_mac, own_mac, 6);
    }

    RtcMap::save();
}

// --- Election logic ---

double MeshConductor::computeScore() {
    uint16_t battery = (uint16_t)PowerManager::batteryMv();
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    // Peer count from routing table
    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);
    uint8_t peers = (table_size > 1) ? (uint8_t)(table_size - 1) : 0;

    // MAC tiebreaker: last 2 bytes, scaled small so it never outweighs real factors
    double mac_tb = (double)(((uint16_t)own_mac[4] << 8) | own_mac[5]) / 65536.0;

    double score = (double)battery    * (float)NvsConfigManager::electWBattery
                 + (double)peers      * (float)NvsConfigManager::electWAdjacency
                 - (double)s_gwTenure * (float)NvsConfigManager::electWTenure
                 + mac_tb;

    // Below battery floor: heavy penalty, but not disqualifying
    if (battery < ELECT_BATTERY_FLOOR_MV)
        score *= (float)NvsConfigManager::electWLowbatPenalty;

    return score;
}

static void buildOwnScore(ElectionScore* out) {
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);

    out->type          = MSG_TYPE_ELECTION;
    memcpy(out->mac, own_mac, 6);
    out->battery_mv    = (uint16_t)PowerManager::batteryMv();
    out->peer_count    = (table_size > 1) ? (uint8_t)(table_size - 1) : 0;
    out->gateway_tenure = s_gwTenure;
    out->score         = MeshConductor::computeScore();
}

static void assignRole(const uint8_t* winnerMac) {
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    if (memcmp(own_mac, winnerMac, 6) == 0) {
        // We are the winner — become Gateway
        s_gwTenure++;
        nvsWriteTenure();
        s_role = &s_gateway;
        Serial.println("[mesh] Role assigned: GATEWAY");
    } else {
        s_role = &s_meshNode;
        Serial.printf("[mesh] Role assigned: NODE (gateway=%02X:%02X:%02X:%02X:%02X:%02X)\n",
            winnerMac[0], winnerMac[1], winnerMac[2],
            winnerMac[3], winnerMac[4], winnerMac[5]);
    }
    s_electionDone = true;
    s_role->begin();
}

static const uint8_t* pickWinner() {
    if (s_scoreCount == 0) return nullptr;

    // Highest score wins (low-battery penalty is already baked in)
    uint8_t best = 0;
    for (uint8_t i = 1; i < s_scoreCount; i++) {
        if (s_scores[i].score > s_scores[best].score) {
            best = i;
        } else if (s_scores[i].score == s_scores[best].score) {
            // Exact tie: highest MAC wins
            if (memcmp(s_scores[i].mac, s_scores[best].mac, 6) > 0)
                best = i;
        }
    }
    return s_scores[best].mac;
}

// Called by the election timer or when single-node fallback triggers
static void electionTimerCallback(TimerHandle_t xTimer) {
    (void)xTimer;

    if (s_electionDone) return;

    // Add own score if not already present
    bool selfPresent = false;
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
    for (uint8_t i = 0; i < s_scoreCount; i++) {
        if (memcmp(s_scores[i].mac, own_mac, 6) == 0) {
            selfPresent = true;
            break;
        }
    }
    if (!selfPresent && s_scoreCount < MESH_MAX_NODES) {
        buildOwnScore(&s_scores[s_scoreCount++]);
    }

    Serial.printf("[mesh] Election: %d candidates\n", s_scoreCount);
    for (uint8_t i = 0; i < s_scoreCount; i++) {
        Serial.printf("[mesh]   %02X:%02X:%02X:%02X:%02X:%02X  bat=%umV peers=%u tenure=%u score=%.1f\n",
            s_scores[i].mac[0], s_scores[i].mac[1], s_scores[i].mac[2],
            s_scores[i].mac[3], s_scores[i].mac[4], s_scores[i].mac[5],
            s_scores[i].battery_mv, s_scores[i].peer_count,
            s_scores[i].gateway_tenure, s_scores[i].score);
    }

    const uint8_t* winner = pickWinner();
    if (winner) {
        Serial.printf("[mesh] Election winner: %02X:%02X:%02X:%02X:%02X:%02X\n",
            winner[0], winner[1], winner[2], winner[3], winner[4], winner[5]);

        // Check if ESP-IDF root matches election winner
        if (esp_mesh_is_root() && memcmp(own_mac, winner, 6) != 0) {
            // We are root but not the winner — waive root to winner
            mesh_vote_t vote;
            vote.percentage = 0.8f;
            vote.is_rc_specified = true;
            memcpy(vote.config.rc_addr.addr, winner, 6);
            Serial.println("[mesh] Waiving root to election winner...");
            esp_mesh_waive_root(&vote, MESH_VOTE_REASON_ROOT_INITIATED);
            // Role will be assigned after root migration completes
            // For now, assign as node; if migration fails, timeout will reassign
            assignRole(winner);
        } else {
            assignRole(winner);
        }
    } else {
        // Fallback: current root stays as gateway
        Serial.println("[mesh] Election fallback: current root keeps gateway");
        if (esp_mesh_is_root()) {
            assignRole(own_mac);
        } else {
            // We're not root and got no scores — become node
            s_role = &s_meshNode;
            s_electionDone = true;
            s_role->begin();
        }
    }
}

void MeshConductor::runElection() {
    if (s_electionDone) return;

    s_scoreCount = 0;

    // Broadcast own score to root (or to all if we are root)
    ElectionScore myScore;
    buildOwnScore(&myScore);

    // Store own score locally
    if (s_scoreCount < MESH_MAX_NODES) {
        s_scores[s_scoreCount++] = myScore;
    }

    // Check total nodes in mesh
    int totalNodes = esp_mesh_get_total_node_num();

    if (totalNodes <= 1) {
        // Single node — instant self-election
        Serial.println("[mesh] Single node — self-electing as Gateway");
        uint8_t own_mac[6];
        esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
        assignRole(own_mac);
        return;
    }

    // Send score to mesh root
    mesh_data_t data;
    data.data = (uint8_t*)&myScore;
    data.size = sizeof(myScore);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;

    if (esp_mesh_is_root()) {
        // We are root: broadcast request for scores to all children
        // Children will send their scores up; we collect them
        // For now, send our own score as a broadcast so children know election is underway
        mesh_addr_t bcast;
        memset(&bcast, 0xFF, sizeof(bcast));  // broadcast
        esp_mesh_send(&bcast, &data, MESH_DATA_P2P, NULL, 0);
    } else {
        // Send score to root
        esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
    }

    // Start election timeout timer
    if (s_electTimer == nullptr) {
        s_electTimer = xTimerCreate("elect", pdMS_TO_TICKS(ELECT_TIMEOUT_MS),
                                     pdFALSE, nullptr, electionTimerCallback);
    }
    xTimerStart(s_electTimer, 0);
}

// --- Mesh data receive task ---

static void meshRxTask(void* pvParameters) {
    mesh_addr_t from;
    mesh_data_t data;
    uint8_t rx_buf[256];
    data.data = rx_buf;
    data.size = sizeof(rx_buf);
    int flag = 0;

    while (s_started) {
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (data.size >= 1 && rx_buf[0] == MSG_TYPE_ELECTION) {
            if (data.size >= sizeof(ElectionScore) && !s_electionDone) {
                ElectionScore* incoming = (ElectionScore*)rx_buf;

                // Check for duplicate
                bool dup = false;
                for (uint8_t i = 0; i < s_scoreCount; i++) {
                    if (memcmp(s_scores[i].mac, incoming->mac, 6) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup && s_scoreCount < MESH_MAX_NODES) {
                    s_scores[s_scoreCount++] = *incoming;
                    Serial.printf("[mesh] Received election score from %02X:%02X:%02X:%02X:%02X:%02X score=%.1f\n",
                        incoming->mac[0], incoming->mac[1], incoming->mac[2],
                        incoming->mac[3], incoming->mac[4], incoming->mac[5],
                        incoming->score);

                    // If we are root, check if we have all scores
                    if (esp_mesh_is_root()) {
                        int totalNodes = esp_mesh_get_total_node_num();
                        if ((int)s_scoreCount >= totalNodes) {
                            // All scores collected — broadcast results and decide
                            // Send all scores to every node
                            for (uint8_t i = 0; i < s_scoreCount; i++) {
                                mesh_data_t bcast_data;
                                bcast_data.data = (uint8_t*)&s_scores[i];
                                bcast_data.size = sizeof(ElectionScore);
                                bcast_data.proto = MESH_PROTO_BIN;
                                bcast_data.tos = MESH_TOS_P2P;
                                mesh_addr_t bcast;
                                memset(&bcast, 0xFF, sizeof(bcast));
                                esp_mesh_send(&bcast, &bcast_data, MESH_DATA_P2P, NULL, 0);
                            }
                            // Cancel timeout and decide now
                            if (s_electTimer) xTimerStop(s_electTimer, 0);
                            electionTimerCallback(nullptr);
                        }
                    } else {
                        // Non-root: check if we have enough scores to decide
                        int totalNodes = esp_mesh_get_total_node_num();
                        if ((int)s_scoreCount >= totalNodes && !s_electionDone) {
                            if (s_electTimer) xTimerStop(s_electTimer, 0);
                            electionTimerCallback(nullptr);
                        }
                    }
                }
            }
        }

        // Reset buffer for next receive
        data.size = sizeof(rx_buf);
    }

    vTaskDelete(nullptr);
}

// --- Event handler ---

static void meshEventHandler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    switch (event_id) {
    case MESH_EVENT_STARTED:
        Serial.println("[mesh] Mesh started");
        s_started = true;
        // Start RX task
        xTaskCreateUniversal(meshRxTask, "meshRx", 4096, nullptr,
                             tskIDLE_PRIORITY + 2, nullptr, tskNO_AFFINITY);
        break;

    case MESH_EVENT_STOPPED:
        Serial.println("[mesh] Mesh stopped");
        s_started = false;
        s_connected = false;
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        Serial.println("[mesh] Parent connected");
        s_connected = true;
        s_parentRetries = 0;
        if (esp_mesh_is_root()) {
            Serial.println("[mesh] I am ROOT");
        }
        updateRtcMap();

        // Start election after settle delay
        if (!s_electionDone) {
            if (s_electTimer == nullptr) {
                s_electTimer = xTimerCreate("elect", pdMS_TO_TICKS(ELECT_SETTLE_MS),
                                             pdFALSE, nullptr, [](TimerHandle_t t) {
                    (void)t;
                    MeshConductor::runElection();
                });
            }
            // Use settle delay for initial trigger, then election timeout takes over
            xTimerChangePeriod(s_electTimer, pdMS_TO_TICKS(ELECT_SETTLE_MS), 0);
            xTimerStart(s_electTimer, 0);
        }
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED:
        Serial.println("[mesh] Parent disconnected");
        s_connected = false;
        updateRtcMap();
        if (s_role && !s_role->isGateway()) {
            ((MeshNode*)s_role)->onGatewayLost();
        }
        break;

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t* child = (mesh_event_child_connected_t*)event_data;
        Serial.printf("[mesh] Child connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5]);
        if (s_role) s_role->onPeerJoined(child->mac);
        updateRtcMap();
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t* child = (mesh_event_child_disconnected_t*)event_data;
        Serial.printf("[mesh] Child disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5]);
        if (s_role) s_role->onPeerLeft(child->mac);
        updateRtcMap();
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
        updateRtcMap();
        break;

    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t* root = (mesh_event_root_address_t*)event_data;
        Serial.printf("[mesh] Root address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            root->addr[0], root->addr[1], root->addr[2],
            root->addr[3], root->addr[4], root->addr[5]);
        rtc_mesh_map_t* map = RtcMap::get();
        memcpy(map->gateway_mac, root->addr, 6);
        updateRtcMap();
        break;
    }

    case MESH_EVENT_NO_PARENT_FOUND:
        s_parentRetries++;
        Serial.printf("[mesh] No parent found (retry %u/%u)\n", s_parentRetries, MESH_MAX_RETRIES);
        if (s_parentRetries < MESH_MAX_RETRIES) {
            // Retry after delay
            vTaskDelay(pdMS_TO_TICKS(MESH_RETRY_DELAY_MS));
            esp_mesh_connect();
        } else {
            Serial.println("[mesh] Max retries reached — sleep and reboot");
            MeshConductor::stop();
            SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
            esp_restart();
        }
        break;

    case MESH_EVENT_ROOT_SWITCH_REQ:
        Serial.println("[mesh] Root switch requested — accepting");
        break;

    default:
        Serial.printf("[mesh] Event %ld\n", event_id);
        break;
    }
}

// --- MeshConductor public API ---

void MeshConductor::init() {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvsReadTenure();
    Serial.printf("[mesh] Gateway tenure from NVS: %u\n", s_gwTenure);

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_mesh_netifs(NULL, NULL);

    // Initialize WiFi
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize mesh
    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                &meshEventHandler, NULL));
}

void MeshConductor::start() {
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.channel = MESH_CHANNEL;
    memcpy((uint8_t*)&cfg.mesh_id, s_meshId, 6);

    // No external router — self-contained mesh
    memset(&cfg.router, 0, sizeof(cfg.router));

    // Mesh AP settings (no password for Phase 1)
    cfg.mesh_ap.max_connection = 6;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));

    // No encryption for Phase 1
    cfg.crypto_funcs = NULL;

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    // Configure mesh topology — NO fix_root, allow dynamic root changes
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(0.8));
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(false));

    // Reset election state
    s_electionDone = false;
    s_role = nullptr;
    s_scoreCount = 0;
    s_parentRetries = 0;

    ESP_ERROR_CHECK(esp_mesh_start());
    Serial.println("[mesh] Mesh starting...");
}

void MeshConductor::stop() {
    if (s_role) {
        s_role->end();
        s_role = nullptr;
    }
    if (s_electTimer) {
        xTimerStop(s_electTimer, 0);
    }
    esp_mesh_stop();
    s_started = false;
    s_connected = false;
    s_electionDone = false;
}

bool MeshConductor::isConnected() {
    return s_connected;
}

bool MeshConductor::isGateway() {
    return s_role && s_role->isGateway();
}

IMeshRole* MeshConductor::role() {
    return s_role;
}

void MeshConductor::printStatus() {
    Serial.println("=== Mesh Status ===");
    Serial.printf("Started: %s\n", s_started ? "yes" : "no");
    Serial.printf("Connected: %s\n", s_connected ? "yes" : "no");
    Serial.printf("Is Root: %s\n", esp_mesh_is_root() ? "yes" : "no");
    Serial.printf("Election done: %s\n", s_electionDone ? "yes" : "no");
    Serial.printf("Role: %s\n", s_role ? (s_role->isGateway() ? "GATEWAY" : "NODE") : "none");
    Serial.printf("Layer: %d\n", esp_mesh_get_layer());
    Serial.printf("Gateway tenure: %u\n", s_gwTenure);

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

    if (s_role) {
        s_role->printStatus();
    }
}

void MeshConductor::forceReelection() {
    Serial.println("[mesh] Forcing re-election — sleep and reboot...");
    if (s_role) {
        s_role->end();
        s_role = nullptr;
    }
    stop();
    SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
    esp_restart();
}
