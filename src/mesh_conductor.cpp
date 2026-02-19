#include "mesh_conductor.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "bsp.hpp"
#include "rtc_mesh_map.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "sq_log.h"
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

// Peer shadow (non-gateway nodes receive this from gateway)
static PeerSyncEntry s_peerShadow[MESH_MAX_NODES];
static uint8_t       s_peerShadowCount = 0;

// Election state
static uint8_t     s_parentRetries  = 0;
static TimerHandle_t s_electTimer   = nullptr;
static TimerHandle_t s_settleTimer  = nullptr;
static TimerHandle_t s_promoteTimer = nullptr;
static TaskHandle_t  s_electTaskHandle = nullptr;
static ElectionScore s_scores[MESH_MAX_NODES];
static uint8_t     s_scoreCount     = 0;
static uint16_t    s_gwTenure       = 0;        // cached from NVS

// Task-notification bits for the election task
static constexpr uint32_t ELECT_NOTIFY_RUN     = (1u << 0);
static constexpr uint32_t ELECT_NOTIFY_TIMEOUT = (1u << 1);

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

    IMeshRole* newRole;
    if (memcmp(own_mac, winnerMac, 6) == 0) {
        s_gwTenure++;
        nvsWriteTenure();
        newRole = &s_gateway;
        SqLog.println("[mesh] Role assigned: GATEWAY");
    } else {
        newRole = &s_meshNode;
        SqLog.printf("[mesh] Role assigned: NODE (gateway=%02X:%02X:%02X:%02X:%02X:%02X)\n",
            winnerMac[0], winnerMac[1], winnerMac[2],
            winnerMac[3], winnerMac[4], winnerMac[5]);
    }

    // Skip transition if already in the correct role
    if (s_role == newRole) {
        s_electionDone = true;
        return;
    }
    if (s_role) s_role->end();
    s_electionDone = true;
    s_role = newRole;
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

    // Non-root that timed out without collecting all scores → accept peer role
    if (!esp_mesh_is_root()) {
        int totalNodes = esp_mesh_get_total_node_num();
        if ((int)s_scoreCount < totalNodes) {
            SqLog.println("[mesh] Election timeout (non-root) — accepting peer role");
            if (s_role != &s_meshNode) {
                if (s_role) s_role->end();
                s_role = &s_meshNode;
                s_role->begin();
            }
            s_electionDone = true;
            return;
        }
    }

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

    SqLog.printf("[mesh] Election: %d candidates\n", s_scoreCount);
    for (uint8_t i = 0; i < s_scoreCount; i++) {
        SqLog.printf("[mesh]   %02X:%02X:%02X:%02X:%02X:%02X  bat=%umV peers=%u tenure=%u score=%.1f\n",
            s_scores[i].mac[0], s_scores[i].mac[1], s_scores[i].mac[2],
            s_scores[i].mac[3], s_scores[i].mac[4], s_scores[i].mac[5],
            s_scores[i].battery_mv, s_scores[i].peer_count,
            s_scores[i].gateway_tenure, s_scores[i].score);
    }

    const uint8_t* winner = pickWinner();
    if (winner) {
        SqLog.printf("[mesh] Election winner: %02X:%02X:%02X:%02X:%02X:%02X\n",
            winner[0], winner[1], winner[2], winner[3], winner[4], winner[5]);

        // Check if ESP-IDF root matches election winner
        if (esp_mesh_is_root() && memcmp(own_mac, winner, 6) != 0) {
            // We are root but not the winner — waive root to winner
            mesh_vote_t vote;
            vote.percentage = 0.8f;
            vote.is_rc_specified = true;
            memcpy(vote.config.rc_addr.addr, winner, 6);
            SqLog.println("[mesh] Waiving root to election winner...");
            esp_mesh_waive_root(&vote, MESH_VOTE_REASON_ROOT_INITIATED);
            // Role will be assigned after root migration completes
            // For now, assign as node; if migration fails, timeout will reassign
            assignRole(winner);
        } else {
            assignRole(winner);
        }
    } else {
        // Fallback: current root stays as gateway
        SqLog.println("[mesh] Election fallback: current root keeps gateway");
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
        SqLog.println("[mesh] Single node — self-electing as Gateway");
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

    // Stop settle timer (election is now active) and start election timeout
    if (s_settleTimer) xTimerStop(s_settleTimer, 0);
    xTimerChangePeriod(s_electTimer, pdMS_TO_TICKS(ELECT_TIMEOUT_MS), 0);
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
                    SqLog.printf("[mesh] Received election score from %02X:%02X:%02X:%02X:%02X:%02X score=%.1f\n",
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

        // --- Phase 2 message dispatch ---

        if (data.size >= 1) {
            uint8_t msgType = rx_buf[0];

            if (msgType == MSG_TYPE_HEARTBEAT && data.size >= sizeof(HeartbeatMsg)) {
                HeartbeatMsg* hb = (HeartbeatMsg*)rx_buf;
                if (s_role && s_role->isGateway()) {
                    PeerTable::updateFromHeartbeat(hb->mac, hb->battery_mv,
                                                    hb->flags, hb->softap_mac);
                }
            }
            else if (msgType == MSG_TYPE_FTM_WAKE && data.size >= sizeof(FtmWakeMsg)) {
                FtmWakeMsg* wake = (FtmWakeMsg*)rx_buf;
                FtmManager::onFtmWake(wake->initiator, wake->responder, wake->responder_ap);
            }
            else if (msgType == MSG_TYPE_FTM_GO && data.size >= sizeof(FtmGoMsg)) {
                FtmGoMsg* go = (FtmGoMsg*)rx_buf;
                FtmManager::onFtmGo(go->target_ap, go->samples);
            }
            else if (msgType == MSG_TYPE_FTM_READY && data.size >= sizeof(FtmReadyMsg)) {
                FtmReadyMsg* ready = (FtmReadyMsg*)rx_buf;
                if (s_role && s_role->isGateway()) {
                    FtmScheduler::onFtmReady(ready->mac);
                }
            }
            else if (msgType == MSG_TYPE_FTM_RESULT && data.size >= sizeof(FtmResultMsg)) {
                FtmResultMsg* result = (FtmResultMsg*)rx_buf;
                if (s_role && s_role->isGateway()) {
                    FtmScheduler::onFtmResult(result->initiator, result->responder,
                                               result->distance_cm, result->status);
                }
            }
            else if (msgType == MSG_TYPE_FTM_CANCEL) {
                // Cancel any in-progress FTM session
                SqLog.println("[mesh] FTM_CANCEL received");
            }
            else if (msgType == MSG_TYPE_POS_UPDATE && data.size >= sizeof(PosUpdateMsg)) {
                PosUpdateMsg* pos = (PosUpdateMsg*)rx_buf;
                PosUpdateEntry* entries = (PosUpdateEntry*)(rx_buf + sizeof(PosUpdateMsg));
                SqLog.printf("[mesh] POS_UPDATE: %u nodes, %uD\n", pos->count, pos->dimension);
                // Nodes could store their own position from this
            }
            else if (msgType == MSG_TYPE_PEER_SYNC && data.size >= sizeof(PeerSyncMsg)) {
                PeerSyncMsg* sync = (PeerSyncMsg*)rx_buf;
                uint8_t count = sync->count;
                if (count > MESH_MAX_NODES) count = MESH_MAX_NODES;
                uint16_t expected = sizeof(PeerSyncMsg) + count * sizeof(PeerSyncEntry);
                if (data.size >= expected) {
                    PeerSyncEntry* entries = (PeerSyncEntry*)(rx_buf + sizeof(PeerSyncMsg));
                    memcpy(s_peerShadow, entries, count * sizeof(PeerSyncEntry));
                    s_peerShadowCount = count;
                    SqLog.printf("[mesh] PEER_SYNC received: %u entries\n", count);
                }
            }
            else if (msgType == MSG_TYPE_NOMINATE && data.size >= sizeof(NominateMsg)) {
                NominateMsg* nom = (NominateMsg*)rx_buf;
                if (s_role && s_role->isGateway()) {
                    SqLog.printf("[mesh] NOMINATE received from %02X:%02X:%02X:%02X:%02X:%02X\n",
                        nom->mac[0], nom->mac[1], nom->mac[2],
                        nom->mac[3], nom->mac[4], nom->mac[5]);
                    MeshConductor::nominateNode(nom->mac);
                }
            }
        }

        // Reset buffer for next receive
        data.size = sizeof(rx_buf);
    }

    vTaskDelete(nullptr);
}

// --- Election task: runs heavy election logic with a proper stack ---

static void electTask(void* pvParameters) {
    (void)pvParameters;
    uint32_t bits;
    for (;;) {
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) == pdTRUE) {
            if (bits & ELECT_NOTIFY_RUN)
                MeshConductor::runElection();
            if (bits & ELECT_NOTIFY_TIMEOUT)
                electionTimerCallback(nullptr);
        }
    }
}

// --- Settle timer: debounced election trigger ---

static void startSettleTimer() {
    if (s_electTimer) xTimerStop(s_electTimer, 0);
    xTimerChangePeriod(s_settleTimer, pdMS_TO_TICKS(ELECT_SETTLE_MS), 0);
}

// --- Promote timer callback (routerless root self-connect) ---

static void promoteTimerCb(TimerHandle_t t) {
    (void)t;
    if (s_connected || esp_mesh_is_root()) return;

    SqLog.println("[mesh] Self-promoting to root (no existing mesh)");
    esp_mesh_set_type(MESH_ROOT);
    esp_mesh_set_self_organized(true, false);  // re-enable so children can join

    // Routerless root has no upstream parent, so PARENT_CONNECTED won't fire.
    // Manually mark connected and kick off the election.
    s_connected = true;
    s_parentRetries = 0;
    updateRtcMap();

    if (!s_electionDone) {
        startSettleTimer();
    }
}

// --- Event handler ---

static void meshEventHandler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    switch (event_id) {
    case MESH_EVENT_STARTED:
        SqLog.println("[mesh] Mesh started");
        s_started = true;

        // Enable FTM Responder on the mesh SoftAP so peers can range to us
        {
            wifi_config_t ap_cfg = {};
            esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
            ap_cfg.ap.ftm_responder = true;
            esp_err_t ftm_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
            if (ftm_err == ESP_OK) {
                SqLog.println("[mesh] FTM Responder enabled on SoftAP");
            } else {
                SqLog.printf("[mesh] WARNING: Failed to enable FTM Responder: %s\n",
                    esp_err_to_name(ftm_err));
            }
        }

        // Start RX task
        xTaskCreateUniversal(meshRxTask, "meshRx", 4096, nullptr,
                             tskIDLE_PRIORITY + 2, nullptr, tskNO_AFFINITY);
        break;

    case MESH_EVENT_STOPPED:
        SqLog.println("[mesh] Mesh stopped");
        s_started = false;
        s_connected = false;
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        if (s_promoteTimer) xTimerStop(s_promoteTimer, 0);
        SqLog.println("[mesh] Parent connected");
        s_connected = true;
        s_parentRetries = 0;
        if (esp_mesh_is_root()) {
            SqLog.println("[mesh] I am ROOT");
        }
        updateRtcMap();

        // Send heartbeat immediately so the gateway adds us to PeerTable
        // before the election completes (election can take 3s settle + 15s timeout)
        if (!esp_mesh_is_root()) {
            HeartbeatMsg hb;
            hb.type = MSG_TYPE_HEARTBEAT;
            esp_read_mac(hb.mac, ESP_MAC_WIFI_STA);
            hb.battery_mv = (uint16_t)PowerManager::batteryMv();
            hb.flags = 0;
            esp_read_mac(hb.softap_mac, ESP_MAC_WIFI_SOFTAP);
            MeshConductor::sendToRoot(&hb, sizeof(hb));
        }

        // Start election after settle delay
        if (!s_electionDone) {
            startSettleTimer();
        }
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED:
        SqLog.println("[mesh] Parent disconnected");
        s_connected = false;
        updateRtcMap();
        if (s_role && !s_role->isGateway()) {
            ((MeshNode*)s_role)->onGatewayLost();
        }
        break;

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t* child = (mesh_event_child_connected_t*)event_data;
        SqLog.printf("[mesh] Child connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5]);
        if (s_role) s_role->onPeerJoined(child->mac);
        updateRtcMap();

        // Re-run election so the new child can participate
        if (s_electionDone && esp_mesh_is_root()) {
            SqLog.println("[mesh] Child joined after election — re-electing");
            s_electionDone = false;
            s_scoreCount = 0;
            startSettleTimer();
        }
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t* child = (mesh_event_child_disconnected_t*)event_data;
        SqLog.printf("[mesh] Child disconnected: %02X:%02X:%02X:%02X:%02X:%02X\n",
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
        SqLog.printf("[mesh] Root address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            root->addr[0], root->addr[1], root->addr[2],
            root->addr[3], root->addr[4], root->addr[5]);
        rtc_mesh_map_t* map = RtcMap::get();
        memcpy(map->gateway_mac, root->addr, 6);
        updateRtcMap();
        break;
    }

    case MESH_EVENT_NO_PARENT_FOUND:
        s_parentRetries++;
        SqLog.printf("[mesh] No parent found (attempt %u)\n", s_parentRetries);
        if (!esp_mesh_is_root()) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            uint16_t jitter = ((mac[4] << 8) | mac[5]) % 2000;
            SqLog.printf("[mesh] Scheduling root promotion in %u ms\n", jitter);
            if (s_promoteTimer == nullptr) {
                s_promoteTimer = xTimerCreate("promote",
                    pdMS_TO_TICKS(jitter > 0 ? jitter : 1),
                    pdFALSE, nullptr, promoteTimerCb);
            } else {
                xTimerChangePeriod(s_promoteTimer,
                    pdMS_TO_TICKS(jitter > 0 ? jitter : 1), 0);
            }
            xTimerStart(s_promoteTimer, 0);
        } else {
            if (s_parentRetries >= MESH_MAX_RETRIES) {
                SqLog.println("[mesh] Root with no children — rebooting");
                MeshConductor::stop();
                SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
                esp_restart();
            }
        }
        break;

    case MESH_EVENT_ROOT_SWITCH_REQ: {
        SqLog.println("[mesh] Root switch requested — accepting, becoming gateway");
        uint8_t own_mac[6];
        esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
        assignRole(own_mac);
        break;
    }

    default:
        SqLog.printf("[mesh] Event %ld\n", event_id);
        break;
    }
}

// --- MeshConductor public API ---

void MeshConductor::init() {
    static bool s_meshInited = false;
    if (s_meshInited) return;
    s_meshInited = true;

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    nvsReadTenure();
    SqLog.printf("[mesh] Gateway tenure from NVS: %u\n", s_gwTenure);

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_create_default_wifi_mesh_netifs(NULL, NULL);

    // Initialize WiFi
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize mesh
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));   // disable RSSI-based root voting; Squeek election takes over

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                &meshEventHandler, NULL));
}

void MeshConductor::start() {
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.channel = MESH_CHANNEL;
    memcpy((uint8_t*)&cfg.mesh_id, s_meshId, 6);

    // Routerless mesh — fix_root(true) disables router-RSSI election,
    // so router config can be zeroed.  If esp_mesh_set_config() still
    // rejects an empty SSID even with fix_root, fall back to a placeholder.
    memset(&cfg.router, 0, sizeof(cfg.router));

    // Mesh AP settings (no password for Phase 1)
    cfg.mesh_ap.max_connection = 6;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));

    // No encryption for Phase 1
    cfg.crypto_funcs = NULL;

    esp_err_t err = esp_mesh_set_config(&cfg);
    if (err == ESP_ERR_MESH_ARGUMENT) {
        // fix_root didn't relax the SSID check — use placeholder
        const char* ph = "SQUEEK_MESH";
        memcpy(cfg.router.ssid, ph, strlen(ph));
        cfg.router.ssid_len = strlen(ph);
        ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    }

    // Configure mesh topology
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    // Create election task (runs heavy election logic off the timer stack)
    if (s_electTaskHandle == nullptr) {
        xTaskCreateUniversal(electTask, "elect", 4096, nullptr,
                             tskIDLE_PRIORITY + 1, &s_electTaskHandle, tskNO_AFFINITY);
    }

    // Create election timers — callbacks are lightweight trampolines that
    // notify the election task (timer service stack is too small for election logic)
    if (s_settleTimer == nullptr) {
        s_settleTimer = xTimerCreate("settle", pdMS_TO_TICKS(ELECT_SETTLE_MS),
                                      pdFALSE, nullptr, [](TimerHandle_t t) {
            (void)t;
            if (s_electTaskHandle)
                xTaskNotify(s_electTaskHandle, ELECT_NOTIFY_RUN, eSetBits);
        });
    }
    if (s_electTimer == nullptr) {
        s_electTimer = xTimerCreate("electTO", pdMS_TO_TICKS(ELECT_TIMEOUT_MS),
                                     pdFALSE, nullptr, [](TimerHandle_t t) {
            (void)t;
            if (s_electTaskHandle)
                xTaskNotify(s_electTaskHandle, ELECT_NOTIFY_TIMEOUT, eSetBits);
        });
    }

    // Reset election state
    s_electionDone = false;
    s_role = nullptr;
    s_scoreCount = 0;
    s_parentRetries = 0;

    ESP_ERROR_CHECK(esp_mesh_start());
    SqLog.println("[mesh] Mesh starting...");
}

void MeshConductor::stop() {
    if (s_role) {
        s_role->end();
        s_role = nullptr;
    }
    if (s_settleTimer) {
        xTimerStop(s_settleTimer, 0);
    }
    if (s_promoteTimer) {
        xTimerStop(s_promoteTimer, 0);
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
    SqLog.println("=== Mesh Status ===");
    SqLog.printf("Started: %s\n", s_started ? "yes" : "no");
    SqLog.printf("Connected: %s\n", s_connected ? "yes" : "no");
    SqLog.printf("Is Root: %s\n", esp_mesh_is_root() ? "yes" : "no");
    SqLog.printf("Election done: %s\n", s_electionDone ? "yes" : "no");
    SqLog.printf("Role: %s\n", s_role ? (s_role->isGateway() ? "GATEWAY" : "NODE") : "none");
    SqLog.printf("Layer: %d\n", esp_mesh_get_layer());
    SqLog.printf("Gateway tenure: %u\n", s_gwTenure);

    int total = esp_mesh_get_total_node_num();
    SqLog.printf("Total nodes: %d\n", total);

    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);
    SqLog.printf("Routing table size: %d\n", table_size);

    for (int i = 0; i < table_size; i++) {
        SqLog.printf("  [%d] %02X:%02X:%02X:%02X:%02X:%02X\n", i,
            routing_table[i].addr[0], routing_table[i].addr[1],
            routing_table[i].addr[2], routing_table[i].addr[3],
            routing_table[i].addr[4], routing_table[i].addr[5]);
    }

    if (s_role) {
        s_role->printStatus();
    }
}

void MeshConductor::printPeerShadow() {
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    Serial.println("=== Peer Table (synced from gateway) ===");
    Serial.printf("Entries: %u\n", s_peerShadowCount);

    for (uint8_t i = 0; i < s_peerShadowCount; i++) {
        PeerSyncEntry* e = &s_peerShadow[i];
        const char* status = (e->flags & PEER_STATUS_DEAD) ? "DEAD " :
                             (e->flags & PEER_STATUS_SLEEPING) ? "SLEEP" : "ALIVE";
        bool isGw   = (i == 0);
        bool isSelf = (memcmp(e->mac, own_mac, 6) == 0);
        const char* suffix = (isGw && isSelf) ? " <-- Gateway, this" :
                             isGw             ? " <-- Gateway" :
                             isSelf           ? " <-- this" : "";
        Serial.printf("  [%u] %02X:%02X:%02X:%02X:%02X:%02X  bat=%umV  %s%s\n",
            i, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
            e->battery_mv, status, suffix);
    }
}

uint8_t MeshConductor::peerShadowCount() {
    return s_peerShadowCount;
}

void MeshConductor::nominateNode(const uint8_t* sta_mac) {
    if (!s_role || !s_role->isGateway()) {
        SqLog.println("[mesh] nominateNode: not gateway, ignoring");
        return;
    }

    SqLog.printf("[mesh] Waiving root to nominee %02X:%02X:%02X:%02X:%02X:%02X\n",
        sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

    mesh_vote_t vote;
    vote.percentage = 0.8f;
    vote.is_rc_specified = true;
    memcpy(vote.config.rc_addr.addr, sta_mac, 6);
    esp_mesh_waive_root(&vote, MESH_VOTE_REASON_ROOT_INITIATED);

    assignRole(sta_mac);  // step down to node
}

void MeshConductor::stepDown() {
    if (!s_role || !s_role->isGateway()) {
        Serial.println("Not gateway — cannot step down.");
        return;
    }

    // Find best alive candidate (highest battery_mv, skip self at slot 0)
    uint8_t bestIdx = 0;
    uint16_t bestBat = 0;
    uint8_t count = PeerTable::peerCount();
    for (uint8_t i = 1; i < count; i++) {
        PeerEntry* e = PeerTable::getEntryByIndex(i);
        if (!e || (e->flags & PEER_STATUS_DEAD)) continue;
        if (e->battery_mv > bestBat) {
            bestBat = e->battery_mv;
            bestIdx = i;
        }
    }

    if (bestIdx == 0) {
        Serial.println("No alive peers to hand off gateway role.");
        return;
    }

    PeerEntry* candidate = PeerTable::getEntryByIndex(bestIdx);
    Serial.printf("Stepping down, nominating %02X:%02X:%02X:%02X:%02X:%02X (%u mV)\n",
        candidate->mac[0], candidate->mac[1], candidate->mac[2],
        candidate->mac[3], candidate->mac[4], candidate->mac[5], candidate->battery_mv);

    nominateNode(candidate->mac);
}

void MeshConductor::forceReelection() {
    SqLog.println("[mesh] Forcing re-election — sleep and reboot...");
    if (s_role) {
        s_role->end();
        s_role = nullptr;
    }
    stop();
    SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
    esp_restart();
}

// --- Messaging helpers ---

esp_err_t MeshConductor::sendToRoot(const void* data, uint16_t len) {
    mesh_data_t mdata;
    mdata.data = (uint8_t*)data;
    mdata.size = len;
    mdata.proto = MESH_PROTO_BIN;
    mdata.tos = MESH_TOS_P2P;
    return esp_mesh_send(NULL, &mdata, MESH_DATA_TODS, NULL, 0);
}

esp_err_t MeshConductor::sendToNode(const uint8_t* sta_mac, const void* data, uint16_t len) {
    mesh_data_t mdata;
    mdata.data = (uint8_t*)data;
    mdata.size = len;
    mdata.proto = MESH_PROTO_BIN;
    mdata.tos = MESH_TOS_P2P;

    mesh_addr_t addr;
    memcpy(addr.addr, sta_mac, 6);
    return esp_mesh_send(&addr, &mdata, MESH_DATA_P2P, NULL, 0);
}

esp_err_t MeshConductor::broadcastToAll(const void* data, uint16_t len) {
    // Send to all nodes in the routing table
    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, MESH_MAX_NODES, &table_size);

    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    mesh_data_t mdata;
    mdata.data = (uint8_t*)data;
    mdata.size = len;
    mdata.proto = MESH_PROTO_BIN;
    mdata.tos = MESH_TOS_P2P;

    esp_err_t last_err = ESP_OK;
    for (int i = 0; i < table_size; i++) {
        if (memcmp(routing_table[i].addr, own_mac, 6) == 0) continue;
        esp_err_t err = esp_mesh_send(&routing_table[i], &mdata, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK) last_err = err;
    }
    return last_err;
}
