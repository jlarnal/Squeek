#include "mesh_conductor.h"
#include "espnow_election.h"
#include "credential_table.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "nvs_config_registry.h"
#include "bsp.hpp"
#include "rtc_state.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "sq_log.h"
#include "orchestrator.h"
#include "clock_sync.h"
#include "web_server.h"
#include "mesh_delegate.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <driver/gpio.h>

static const char* TAG = "mesh";

// --- File-scope state ---

static IMeshRole*  s_role           = nullptr;
static bool        s_connected      = false;
static bool        s_started        = false;
static bool        s_roleAssigned   = false;
static uint8_t     s_meshId[6]      = { 0x53, 0x51, 0x45, 0x45, 0x4B, 0x00 }; // "SQUEEK"
// Role objects are now heap-allocated via MeshConductor::setRole()
// (no more static Gateway/MeshNode — reboot switches role)

// Peer shadow (non-gateway nodes receive this from gateway)
static PeerSyncEntry s_peerShadow[MESH_MAX_NODES];
static uint8_t       s_peerShadowCount = 0;

// Gateway MAC — all nodes track this for heartbeat routing
static uint8_t       s_gatewayMac[6] = {0};

// Bug 3: Cred push ACK flag — set when ACK received, checked in push loop
static volatile bool s_credAckReceived = false;

// Mesh state
static uint8_t     s_parentRetries  = 0;
static int8_t      s_bestRouterRssi = -128;     // best RSSI to a known router (from boot scan)

// ESP-NOW election result (computed in init(), consumed in start())
static ElectionResult s_electionResult = {};
static bool           s_electionRan    = false;
static bool           s_scanResultPending = false;
static bool        s_hasRouterCreds = false;     // true when real WiFi creds loaded

// Pre-scan state (brief mesh start before election to detect existing mesh)
static bool        s_prescanActive  = false;     // true during init() mesh prescan
static bool        s_prescanFound   = false;     // prescan found an existing mesh
static uint8_t     s_prescanChannel = 0;         // channel of the found mesh
static SemaphoreHandle_t s_prescanSema = nullptr;

static void assignRoleFromMeshState();  // forward decl

// Channel lock — stop continuous scanning after mesh forms
static void lockChannel() {
    uint8_t primary = 0;
    wifi_second_chan_t secondary;
    if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK && primary > 0) {
        mesh_cfg_t cfg;
        esp_mesh_get_config(&cfg);
        if (cfg.channel != primary) {
            cfg.channel = primary;
            esp_mesh_set_config(&cfg);
            SqLog.printf("[mesh] Channel locked to %d\n", primary);
        }
    }
}

// BOOT button — routes to delegate (if gateway)
static volatile uint32_t s_bootBtnLastEdge = 0;
static volatile uint8_t  s_bootBtnEdges    = 0;

static void IRAM_ATTR bootButtonISR(void* arg) {
    (void)arg;
    uint32_t now = xTaskGetTickCountFromISR();
    uint32_t elapsed = (now - s_bootBtnLastEdge) * portTICK_PERIOD_MS;
    if (elapsed < BOOT_BUTTON_DEBOUNCE_MS) return;  // debounce

    uint8_t edges = s_bootBtnEdges + 1;
    s_bootBtnEdges = edges;
    s_bootBtnLastEdge = now;

    if (edges >= 2) {
        // Two edges = one press-release cycle
        s_bootBtnEdges = 0;
        // Defer to timer service context (can't call mesh APIs from ISR)
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTimerPendFunctionCallFromISR(
            [](void* p1, uint32_t p2) {
                (void)p1; (void)p2;
                MeshConductor::onBootButton();
            },
            nullptr, 0, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// Config response wait mechanism
static SemaphoreHandle_t s_configRespSema = nullptr;
static char              s_configRespBuf[480];
static uint8_t           s_configRespReqId = 0;

// --- Tenure score computation (RAM-only, never persisted to NVS) ---

uint16_t computeTenureScore(int8_t best_rssi_dBm) {
    uint16_t score = 128;

    // RSSI component: 0..~80 (only if a known router is detected)
    if (best_rssi_dBm > -128) {
        int16_t rssi_contrib = (int16_t)LOWEST_TOLERATED_RSSI + (int16_t)best_rssi_dBm;
        if (rssi_contrib < 0)   rssi_contrib = 0;
        if (rssi_contrib > 80)  rssi_contrib = 80;
        score += (uint16_t)rssi_contrib;
    }

    // Battery component: 0..100
    // When disabled (USB-powered boards read false low), assume full battery (100).
    if ((bool)NvsConfigManager::batteryInTenure) {
        int32_t bat = PowerManager::batteryMv();
        int32_t bat_contrib = 0;
        if (bat > BATTERY_LOW_MV) {
            bat_contrib = (bat - BATTERY_LOW_MV) * 100 / (4200 - BATTERY_LOW_MV);
            if (bat_contrib > 100) bat_contrib = 100;
        }
        score += (uint16_t)bat_contrib;
    } else {
        score += 100;  // assume full battery
    }

    // Uptime stability component: 0..50
    uint32_t uptime_u16 = (uint32_t)(millis() >> 16);  // ~65s granularity
    uint32_t up_contrib = uptime_u16;
    if (up_contrib > 65535) up_contrib = 65535;
    up_contrib = up_contrib * 50 / 65535;
    score += (uint16_t)up_contrib;

    return score;
}

// --- RTC map update (carried over from mesh_manager) ---

static void updateRtcState() {
    rtc_state_t* map = RtcState::get();
    map->own_role = s_role ? static_cast<uint8_t>(s_role->roleId()) : 0;
    map->mesh_channel = MESH_CHANNEL;

    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, sizeof(routing_table), &table_size);

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

    RtcState::save();
}

// --- Scan delegate: send result to gateway after mesh join ---

static void checkAndReportScanResult() {
    if (!s_scanResultPending) return;
    s_scanResultPending = false;

    rtc_state_t* rtc = RtcState::get();
    if (rtc->scan_result == 1) {
        CredVerifiedMsg msg = {};
        msg.type = MSG_TYPE_CRED_VERIFIED;
        strncpy(msg.ssid, rtc->scan_ssid, 32);
        msg.channel = rtc->scan_channel;
        msg.rssi = rtc->scan_rssi;
        SqLog.printf("[scan] Reporting CRED_VERIFIED: %s ch%u rssi=%d\n",
                     msg.ssid, msg.channel, msg.rssi);
        MeshConductor::sendToRoot(&msg, sizeof(msg));
    } else {
        CredRejectedMsg msg = {};
        msg.type = MSG_TYPE_CRED_REJECTED;
        strncpy(msg.ssid, rtc->scan_ssid, 32);
        SqLog.printf("[scan] Reporting CRED_REJECTED: %s\n", msg.ssid);
        MeshConductor::sendToRoot(&msg, sizeof(msg));
    }

    memset(rtc->scan_ssid, 0, sizeof(rtc->scan_ssid));
    memset(rtc->scan_pass, 0, sizeof(rtc->scan_pass));
    rtc->scan_result = 0;
    RtcState::save();
}

// --- Root-observation role assignment ---
// Whoever is ESP-MESH root IS the Gateway. No election overlay.

static void assignRoleFromMeshState() {
    bool amRoot = esp_mesh_is_root();
    RoleId target = amRoot ? RoleId::GATEWAY : RoleId::PEER;

    // Already in the correct role — nothing to do
    if (s_role && s_role->roleId() == target) {
        s_roleAssigned = true;
        return;
    }

    // Role mismatch (role was pre-assigned by fast-path boot but mesh state differs) — reboot
    if (s_role) {
        SqLog.printf("[mesh] Role mismatch — rebooting into %s\n",
            amRoot ? "GATEWAY" : "PEER");
        rtc_state_t* rtc = RtcState::get();
        rtc->next_role = (uint8_t)target;
        RtcState::save();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }

    // First assignment — instantiate role
    s_roleAssigned = true;
    if (amRoot) {
        s_connected = true;  // Root is "connected" even without a router
        SqLog.printf("[mesh] Role assigned: GATEWAY (tenure=%u)\n",
                     computeTenureScore(s_bestRouterRssi));
        uint8_t own_mac[6];
        esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
        memcpy(s_gatewayMac, own_mac, 6);
        // Don't lockChannel() here — routerless root must keep scanning
        // so ESP-MESH can detect dual-root situations. Channel gets locked
        // on PARENT_CONNECTED (routed) or CHANNEL_SWITCH events.
    } else {
        SqLog.println("[mesh] Role assigned: NODE (not root)");
    }

    s_role = amRoot
        ? static_cast<IMeshRole*>(new Gateway())
        : static_cast<IMeshRole*>(new MeshNode());
    s_role->begin();
    updateRtcState();

    // Enable FTM Responder on the mesh SoftAP so peers can range to us.
    // Done here (not in MESH_EVENT_STARTED) because the AP interface isn't
    // fully configured by ESP-MESH until the node has joined the topology.
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
}


// --- Mesh data receive task ---

static void meshRxTask(void* pvParameters) {
    mesh_addr_t from;
    mesh_data_t data;
    uint8_t rx_buf[512];
    data.data = rx_buf;
    data.size = sizeof(rx_buf);
    int flag = 0;

    while (s_started) {
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (data.size >= 1) {
            uint8_t msgType = rx_buf[0];

            if (msgType == MSG_TYPE_HEARTBEAT && data.size >= 15) {
                HeartbeatMsg* hb = (HeartbeatMsg*)rx_buf;
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    PeerTable::updateFromHeartbeat(hb->mac, hb->battery_mv,
                                                    hb->flags, hb->softap_mac);
                    // Track best peer tenure for RSSI re-evaluation
                    // (older nodes send 15-byte heartbeats without tenure fields)
                    if (data.size >= sizeof(HeartbeatMsg)) {
                        static_cast<Gateway*>(s_role)->trackPeerTenure(hb->tenure_score);
                    }
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
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    FtmScheduler::onFtmReady(ready->mac);
                }
            }
            else if (msgType == MSG_TYPE_FTM_RESULT && data.size >= sizeof(FtmResultMsg)) {
                FtmResultMsg* result = (FtmResultMsg*)rx_buf;
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
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
            else if (msgType == MSG_TYPE_CONFIG_REQ && data.size >= 3) {
                uint8_t reqId = rx_buf[1];
                const char* json = (const char*)&rx_buf[2];
                // Ensure null-terminated
                rx_buf[data.size] = '\0';

                JsonDocument reqDoc;
                DeserializationError jsonErr = deserializeJson(reqDoc, json);
                if (jsonErr) {
                    SqLog.printf("[mesh] CONFIG_REQ: JSON parse error: %s\n", jsonErr.c_str());
                } else {
                    const char* action = reqDoc["action"] | "get";
                    JsonDocument respDoc;

                    // Add own MAC to response
                    uint8_t own_mac[6];
                    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
                    char macStr[18];
                    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                        own_mac[0], own_mac[1], own_mac[2],
                        own_mac[3], own_mac[4], own_mac[5]);
                    respDoc["mac"] = macStr;

                    if (strcmp(action, "set") == 0) {
                        uint8_t applied = configApplyJson(reqDoc.as<JsonObjectConst>());
                        SqLog.printf("[mesh] CONFIG_REQ set: applied %u fields\n", applied);
                        // Respond with new values of all fields that were set
                        for (JsonPairConst kv : reqDoc.as<JsonObjectConst>()) {
                            const char* key = kv.key().c_str();
                            if (strcmp(key, "action") == 0) continue;
                            const ConfigField* f = configLookup(key);
                            if (f) configBuildJson(respDoc, (const char**)&key, 1);
                        }
                    } else {
                        // "get"
                        if (reqDoc["fields"].is<JsonArray>()) {
                            JsonArray arr = reqDoc["fields"];
                            const char* fields[20];
                            uint8_t cnt = 0;
                            for (JsonVariant v : arr) {
                                if (cnt < 20) fields[cnt++] = v.as<const char*>();
                            }
                            configBuildJson(respDoc, fields, cnt);
                        } else {
                            configBuildJson(respDoc, nullptr, 0);
                        }
                    }

                    // Serialize and send response
                    char respJson[460];
                    size_t jsonLen = serializeJson(respDoc, respJson, sizeof(respJson));

                    uint8_t respBuf[464];
                    respBuf[0] = MSG_TYPE_CONFIG_RESP;
                    respBuf[1] = reqId;
                    memcpy(&respBuf[2], respJson, jsonLen + 1);  // include null

                    MeshConductor::sendToNode(from.addr, respBuf, 2 + jsonLen + 1);
                }
            }
            else if (msgType == MSG_TYPE_CONFIG_RESP && data.size >= 3) {
                uint8_t reqId = rx_buf[1];
                if (reqId == s_configRespReqId && s_configRespSema) {
                    size_t payloadLen = data.size - 2;
                    if (payloadLen >= sizeof(s_configRespBuf))
                        payloadLen = sizeof(s_configRespBuf) - 1;
                    memcpy(s_configRespBuf, &rx_buf[2], payloadLen);
                    s_configRespBuf[payloadLen] = '\0';
                    xSemaphoreGive(s_configRespSema);
                }
            }
            // Phase 4: Orchestrator messages
            else if (msgType == MSG_TYPE_PLAY_CMD && data.size >= sizeof(PlayCmdMsg)) {
                PlayCmdMsg* play = (PlayCmdMsg*)rx_buf;
                Orchestrator::onPlayCmd(play->tone_index);
            }
            else if (msgType == MSG_TYPE_ORCH_MODE && data.size >= sizeof(OrchModeMsg)) {
                OrchModeMsg* om = (OrchModeMsg*)rx_buf;
                Orchestrator::onModeChange(om->mode);
            }
            else if (msgType == MSG_TYPE_CLOCK_SYNC && data.size >= sizeof(ClockSyncMsg)) {
                ClockSyncMsg* cs = (ClockSyncMsg*)rx_buf;
                ClockSync::onSyncReceived(cs->gateway_ms);
            }
            // Phase 5: Setup Delegate messages
            else if (msgType == MSG_TYPE_WIFI_CREDS && data.size >= sizeof(WifiCredsMsg)) {
                WifiCredsMsg* wc = (WifiCredsMsg*)rx_buf;
                wc->ssid[32] = '\0';      // safety null-terminate
                wc->password[64] = '\0';

                // Add to credential table (deduplicates by SSID)
                int8_t slot = CredentialTable::add(wc->ssid, wc->password);
                bool credsChanged = (slot >= 0);
                if (credsChanged) {
                    SqLog.printf("[mesh] Saved WiFi credentials to slot %d (SSID=%s)\n",
                                 slot, wc->ssid);
                } else {
                    SqLog.printf("[mesh] WiFi credentials unchanged (SSID=%s)\n", wc->ssid);
                }

                if (esp_mesh_is_root() && s_role && s_role->roleId() == RoleId::GATEWAY) {
                    Gateway* gw = static_cast<Gateway*>(s_role);
                    if (gw->hasDelegateTicket()) {
                        // Delegate return — creds already verified. Broadcast + reboot.
                        if (credsChanged) {
                            SqLog.println("[mesh] Broadcasting WiFi credentials to all peers");
                            MeshConductor::broadcastToAll(wc, sizeof(WifiCredsMsg));
                        }
                        gw->clearTicket();
                        if (credsChanged) {
                            // Warn peers to reboot with us
                            RebootWarnMsg rw = {};
                            rw.type = MSG_TYPE_REBOOT_WARN;
                            rw.delay_ms = 2000;
                            MeshConductor::broadcastToAll(&rw, sizeof(rw));
                            SqLog.println("[mesh] Router creds saved — everyone rebooting in 2s");
                            TimerHandle_t t = xTimerCreate("gwReboot", pdMS_TO_TICKS(2000),
                                pdFALSE, nullptr, [](TimerHandle_t timer) {
                                    xTimerDelete(timer, 0);
                                    esp_restart();
                                });
                            if (t) xTimerStart(t, 0);
                        }
                    } else {
                        // Fresh wifi set from a peer — dispatch scan delegate to verify
                        SqLog.printf("[mesh] WiFi creds from peer — dispatching scan delegate for \"%s\"\n", wc->ssid);
                        gw->startScanDelegate(wc->ssid, wc->password);
                    }
                }

                // Non-root: update local mesh config with router creds
                // (survive root loss, can find router independently on next boot)
                if (!esp_mesh_is_root() && credsChanged) {
                    esp_mesh_set_self_organized(false, false);

                    mesh_cfg_t meshCfg;
                    esp_mesh_get_config(&meshCfg);
                    memset(meshCfg.router.ssid, 0, sizeof(meshCfg.router.ssid));
                    memcpy(meshCfg.router.ssid, wc->ssid, strlen(wc->ssid));
                    meshCfg.router.ssid_len = strlen(wc->ssid);
                    memset(meshCfg.router.password, 0, sizeof(meshCfg.router.password));
                    memcpy(meshCfg.router.password, wc->password, strlen(wc->password));
                    esp_mesh_set_config(&meshCfg);
                    s_hasRouterCreds = true;
                    SqLog.println("[mesh] Updated local mesh config with router creds");

                    // Re-enable — peer stays connected to parent
                    esp_mesh_set_self_organized(true, false);
                }

                // Send ACK back to sender (not sendToRoot — we ARE root)
                WifiCredsAckMsg ack = { .type = MSG_TYPE_WIFI_CREDS_ACK };
                MeshConductor::sendToNode(from.addr, &ack, sizeof(ack));
            }
            else if (msgType == MSG_TYPE_WIFI_CREDS_ACK) {
                SqLog.println("[mesh] WiFi credentials ACK received");
                s_credAckReceived = true;
            }
            else if (msgType == MSG_TYPE_MERGE_CHECK && data.size >= sizeof(MergeCheckMsg)) {
                MergeCheckMsg* mc = (MergeCheckMsg*)rx_buf;
                if (esp_mesh_is_root()) {
                    mesh_addr_t rt[MESH_MAX_NODES];
                    int rtSize = 0;
                    esp_mesh_get_routing_table(rt, sizeof(rt), &rtSize);
                    if (rtSize < mc->root_table_size) {
                        SqLog.printf("[mesh] Merge check: yielding root (my %d < sender %d)\n",
                                     rtSize, mc->root_table_size);
                        esp_mesh_set_self_organized(true, true);  // rescan
                    }
                }
            }
            else if (msgType == MSG_TYPE_SETUP_DELEGATE && data.size >= sizeof(SetupDelegateMsg)) {
                SqLog.println("[mesh] Designated as Setup Delegate — rebooting");
                rtc_state_t* rtc = RtcState::get();
                rtc->next_role = (uint8_t)RoleId::DELEGATE;
                RtcState::save();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            else if (msgType == MSG_TYPE_DELEGATE_RESULT && data.size >= sizeof(DelegateResultMsg)) {
                DelegateResultMsg* dr = (DelegateResultMsg*)rx_buf;
                SqLog.printf("[mesh] Delegate result: %s\n", dr->success ? "creds obtained" : "failed");
                // Clear delegate ticket — delegate has returned
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    static_cast<Gateway*>(s_role)->clearTicket();
                }
            }
            else if (msgType == MSG_TYPE_DELEGATE_TICKET && data.size >= sizeof(DelegateTicketMsg)) {
                DelegateTicketMsg* dt = (DelegateTicketMsg*)rx_buf;
                SqLog.printf("[mesh] Received delegate ticket: %02X:..:%02X (%us)\n",
                    dt->delegate_mac[0], dt->delegate_mac[5], dt->remaining_s);
                // Store in RTC — new gateway will pick it up after reboot
                rtc_state_t* rtc = RtcState::get();
                memcpy(rtc->ticket_delegate_mac, dt->delegate_mac, 6);
                rtc->ticket_remaining_s = dt->remaining_s;
                RtcState::save();
                // If already running as gateway, install immediately
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    static_cast<Gateway*>(s_role)->installTicket(dt->delegate_mac, dt->remaining_s);
                }
                // Send ACK
                DelegateTicketMsg ack = {};  // reuse struct, only type matters
                ack.type = MSG_TYPE_DELEGATE_TICKET_ACK;
                MeshConductor::sendToRoot(&ack, sizeof(ack));
            }
            else if (msgType == MSG_TYPE_DELEGATE_TICKET_ACK) {
                SqLog.println("[mesh] Delegate ticket ACK received");
            }
            else if (msgType == MSG_TYPE_SCAN_REQUEST) {
                // Peer: run WiFi scan and report SSID count back to gateway
                SqLog.println("[mesh] Scan request received — scanning WiFi");
                wifi_scan_config_t scanCfg = {};
                scanCfg.show_hidden = false;
                scanCfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
                scanCfg.scan_time.active.min = 100;
                scanCfg.scan_time.active.max = 300;
                esp_err_t err = esp_wifi_scan_start(&scanCfg, true);  // blocking
                uint16_t apCount = 0;
                if (err == ESP_OK) {
                    esp_wifi_scan_get_ap_num(&apCount);
                    esp_wifi_clear_ap_list();  // free scan memory
                } else {
                    SqLog.printf("[mesh] Scan failed: %s\n", esp_err_to_name(err));
                }
                SqLog.printf("[mesh] Scan complete: %d APs found\n", apCount);

                uint8_t ownMac[6];
                esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
                ScanResultMsg res = {};
                res.type = MSG_TYPE_SCAN_RESULT;
                memcpy(res.mac, ownMac, 6);
                res.ssid_count = (apCount > 255) ? 255 : (uint8_t)apCount;
                MeshConductor::sendToRoot(&res, sizeof(res));
            }
            else if (msgType == MSG_TYPE_SCAN_RESULT && data.size >= sizeof(ScanResultMsg)) {
                ScanResultMsg* sr = (ScanResultMsg*)rx_buf;
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    Gateway* gw = static_cast<Gateway*>(s_role);
                    gw->onScanResult(sr->mac, sr->ssid_count);
                }
            }
            // --- Scan delegate protocol ---
            else if (msgType == MSG_TYPE_SCAN_DELEGATE && data.size >= sizeof(ScanDelegateMsg)) {
                ScanDelegateMsg* sd = (ScanDelegateMsg*)rx_buf;
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    // Gateway received from peer's wifi set — dispatch scan delegate
                    SqLog.printf("[mesh] Peer requests scan for \"%s\" — dispatching delegate\n", sd->ssid);
                    static_cast<Gateway*>(s_role)->startScanDelegate(sd->ssid, sd->password);
                } else {
                    // Peer received from gateway — reboot as scan delegate
                    SqLog.printf("[mesh] Designated as Scan Delegate for \"%s\" — rebooting\n", sd->ssid);
                    rtc_state_t* rtc = RtcState::get();
                    rtc->next_role = (uint8_t)RoleId::SCAN_DELEGATE;
                    strncpy(rtc->scan_ssid, sd->ssid, 32);
                    rtc->scan_ssid[32] = '\0';
                    strncpy(rtc->scan_pass, sd->password, 64);
                    rtc->scan_pass[64] = '\0';
                    rtc->scan_result = 0;
                    RtcState::save();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            }
            else if (msgType == MSG_TYPE_CRED_VERIFIED && data.size >= sizeof(CredVerifiedMsg)) {
                CredVerifiedMsg* cv = (CredVerifiedMsg*)rx_buf;
                SqLog.printf("[mesh] CRED_VERIFIED: \"%s\" on ch%u (RSSI %d)\n",
                             cv->ssid, cv->channel, cv->rssi);

                // Retrieve password from RTC (not NVS — creds weren't committed yet)
                rtc_state_t* rtc = RtcState::get();
                char pass[65] = {};
                // The gateway stored the password in scan_pass when dispatching
                // (startScanDelegate wrote it to RTC for lone-gateway path).
                // For multi-node, the password was in the ScanDelegateMsg we sent.
                // We need to recover it — check RTC first, then CredentialTable fallback.
                if (rtc->scan_pass[0] != '\0' && strcmp(rtc->scan_ssid, cv->ssid) == 0) {
                    strncpy(pass, rtc->scan_pass, 64);
                } else {
                    // Fallback: check if creds happen to be in CredentialTable already
                    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
                        const CredEntry* e = CredentialTable::getSlot(i);
                        if (e && strcmp(e->ssid, cv->ssid) == 0) {
                            strncpy(pass, e->pass, 64);
                            break;
                        }
                    }
                }

                // NOW commit verified creds to NVS
                CredentialTable::add(cv->ssid, pass);
                SqLog.printf("[mesh] Verified creds saved to NVS: SSID=%s\n", cv->ssid);

                // Broadcast creds to all peers so they save to NVS
                WifiCredsMsg wc = {};
                wc.type = MSG_TYPE_WIFI_CREDS;
                strncpy(wc.ssid, cv->ssid, 32);
                strncpy(wc.password, pass, 64);
                MeshConductor::broadcastToAll(&wc, sizeof(wc));

                // Warn peers to reboot with us (fixed 2s delay)
                RebootWarnMsg rw = {};
                rw.type = MSG_TYPE_REBOOT_WARN;
                rw.delay_ms = 2000;
                MeshConductor::broadcastToAll(&rw, sizeof(rw));

                // Clear RTC scan fields
                memset(rtc->scan_ssid, 0, sizeof(rtc->scan_ssid));
                memset(rtc->scan_pass, 0, sizeof(rtc->scan_pass));
                rtc->scan_result = 0;
                RtcState::save();

                Serial.printf("Credentials verified! Everyone rebooting in 2s to connect to \"%s\" on ch%u...\n",
                              cv->ssid, cv->channel);
                TimerHandle_t t = xTimerCreate("credReboot", pdMS_TO_TICKS(2000),
                    pdFALSE, nullptr, [](TimerHandle_t timer) {
                        xTimerDelete(timer, 0);
                        esp_restart();
                    });
                if (t) xTimerStart(t, 0);
            }
            else if (msgType == MSG_TYPE_CRED_REJECTED && data.size >= sizeof(CredRejectedMsg)) {
                CredRejectedMsg* cr = (CredRejectedMsg*)rx_buf;
                SqLog.printf("[mesh] CRED_REJECTED: \"%s\" not found by scan delegate\n", cr->ssid);
                Serial.printf("WARNING: SSID \"%s\" not found in scan — credentials may be wrong.\n", cr->ssid);
                Serial.println("Credentials remain stored. Use 'wifi clear' to remove, or retry.");
            }
            else if (msgType == MSG_TYPE_REBOOT_WARN && data.size >= sizeof(RebootWarnMsg)) {
                RebootWarnMsg* rw = (RebootWarnMsg*)rx_buf;
                SqLog.printf("[mesh] Gateway reboot warning — rebooting in %ums\n", rw->delay_ms);
                TimerHandle_t t = xTimerCreate("peerReboot", pdMS_TO_TICKS(rw->delay_ms),
                    pdFALSE, nullptr, [](TimerHandle_t timer) {
                        xTimerDelete(timer, 0);
                        esp_restart();
                    });
                if (t) xTimerStart(t, 0);
            }
            // --- Force gateway (manual gateway designation) ---
            else if (msgType == MSG_TYPE_FORCE_GATEWAY && data.size >= sizeof(ForceGatewayMsg)) {
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    Gateway* gw = static_cast<Gateway*>(s_role);
                    if (gw->hasDelegateTicket()) {
                        SqLog.println("[mesh] FORCE_GATEWAY rejected — delegate active");
                    } else {
                        SqLog.printf("[mesh] FORCE_GATEWAY from %02X:%02X:%02X:%02X:%02X:%02X — yielding\n",
                            from.addr[0], from.addr[1], from.addr[2],
                            from.addr[3], from.addr[4], from.addr[5]);
                        // Broadcast reboot warning (best-effort notification)
                        RebootWarnMsg rw = {};
                        rw.type = MSG_TYPE_REBOOT_WARN;
                        rw.delay_ms = 500;
                        MeshConductor::broadcastToAll(&rw, sizeof(rw));
                        // Confirm to requesting peer
                        ForceGatewayGoMsg go = {};
                        go.type = MSG_TYPE_FORCE_GATEWAY_GO;
                        MeshConductor::sendToNode(from.addr, &go, sizeof(go));
                        // Deferred reboot — let messages flush
                        TimerHandle_t t = xTimerCreate("forceGwReboot", pdMS_TO_TICKS(500),
                            pdFALSE, nullptr, [](TimerHandle_t timer) {
                                xTimerDelete(timer, 0);
                                esp_restart();
                            });
                        if (t) xTimerStart(t, 0);
                    }
                }
            }
            else if (msgType == MSG_TYPE_FORCE_GATEWAY_GO && data.size >= sizeof(ForceGatewayGoMsg)) {
                SqLog.println("[mesh] FORCE_GATEWAY_GO — rebooting as forced gateway");
                rtc_state_t* rtc = RtcState::get();
                rtc->force_gateway = 1;
                RtcState::save();
                esp_restart();
            }
            // --- Credential exchange protocol ---
            else if (msgType == MSG_TYPE_CRED_OFFER && data.size >= 2) {
                // Peer receives cred offer from gateway
                SqLog.println("[mesh] CRED_OFFER received from gateway");
                uint8_t offerCount = rx_buf[1];
                // Import credentials (skip type+count header, pass rest to fromBuffer)
                // The buffer after type byte is: count + CredWireEntry[]
                uint8_t added = CredentialTable::fromBuffer(&rx_buf[1], data.size - 1);
                SqLog.printf("[mesh] Imported %u new credentials from gateway (%u offered)\n",
                             added, offerCount);

                // Run a quick scan to measure RSSI to known routers
                wifi_scan_config_t scanCfg = {};
                scanCfg.show_hidden = false;
                scanCfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
                scanCfg.scan_time.active.min = 100;
                scanCfg.scan_time.active.max = 300;
                esp_err_t scanErr = esp_wifi_scan_start(&scanCfg, true);

                int8_t myBestRssi = -128;
                if (scanErr == ESP_OK) {
                    uint16_t apCount = 0;
                    esp_wifi_scan_get_ap_num(&apCount);
                    if (apCount > 0) {
                        uint16_t maxAps = (apCount > 20) ? 20 : apCount;
                        wifi_ap_record_t* aps = (wifi_ap_record_t*)malloc(maxAps * sizeof(wifi_ap_record_t));
                        if (aps) {
                            esp_wifi_scan_get_ap_records(&maxAps, aps);
                            ScanMatch match = CredentialTable::matchScan(aps, maxAps);
                            if (match.slot >= 0) {
                                myBestRssi = match.rssi;
                                s_bestRouterRssi = myBestRssi;
                            }
                            free(aps);
                        } else {
                            esp_wifi_clear_ap_list();
                        }
                    } else {
                        esp_wifi_clear_ap_list();
                    }
                }

                // Build CRED_REPLY: our merged creds + per-AP RSSI + tenure score
                uint16_t myTenure = computeTenureScore(myBestRssi);

                // Serialize: type(1) + count(1) + tenure(2) + CredWireEntry[] payload
                // Heap-allocate — meshRxTask stack is only 4KB with rx_buf[512] already on it
                uint8_t* replyBuf = (uint8_t*)malloc(1024);
                if (replyBuf) {
                    replyBuf[0] = MSG_TYPE_CRED_REPLY;
                    replyBuf[2] = (uint8_t)(myTenure & 0xFF);
                    replyBuf[3] = (uint8_t)(myTenure >> 8);

                    uint16_t credLen = CredentialTable::toBuffer(&replyBuf[4], 1024 - 4);
                    replyBuf[1] = replyBuf[4];  // count byte from toBuffer
                    uint16_t totalLen = 4 + credLen;

                    MeshConductor::sendToNode(from.addr, replyBuf, totalLen);
                    SqLog.printf("[mesh] Sent CRED_REPLY (tenure=%u, rssi=%d)\n",
                                 myTenure, myBestRssi);
                    free(replyBuf);
                }
            }
            else if (msgType == MSG_TYPE_CRED_REPLY && data.size >= 4) {
                // Gateway receives cred reply from peer
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
                    uint8_t credCount = rx_buf[1];
                    uint16_t peerTenure = (uint16_t)rx_buf[2] | ((uint16_t)rx_buf[3] << 8);
                    SqLog.printf("[mesh] CRED_REPLY from peer: tenure=%u, creds=%u\n",
                                 peerTenure, credCount);

                    // Import peer's credentials (skip type+count+tenure header)
                    // The buffer at offset 4 is toBuffer format: count(1) + entries
                    if (data.size > 4) {
                        uint8_t added = CredentialTable::fromBuffer(&rx_buf[4], data.size - 4);
                        if (added > 0) {
                            SqLog.printf("[mesh] Learned %u new credentials from peer\n", added);
                            // Recompute our RSSI if we gained new cred knowledge
                            // (deferred to next heartbeat re-evaluation)
                        }
                    }

                    // Compare tenure scores
                    uint16_t myTenure = computeTenureScore(s_bestRouterRssi);
                    SqLog.printf("[mesh] Tenure comparison: mine=%u, peer=%u\n",
                                 myTenure, peerTenure);
                    if (peerTenure > myTenure) {
                        SqLog.println("[mesh] Peer has higher tenure — waiving root");
                        MeshConductor::requestStepDown();
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
    // During prescan, only process FIND_NETWORK — skip Rx task creation,
    // state changes, role assignment, etc.
    if (s_prescanActive) {
        if (event_id == MESH_EVENT_FIND_NETWORK) {
            mesh_event_find_network_t* net = (mesh_event_find_network_t*)event_data;
            SqLog.printf("[mesh] Pre-scan: found mesh on ch%d\n", net->channel);
            s_prescanChannel = (uint8_t)net->channel;
            if (s_prescanSema) xSemaphoreGive(s_prescanSema);
        }
        return;
    }

    switch (event_id) {
    case MESH_EVENT_STARTED:
        SqLog.println("[mesh] Mesh started");
        s_started = true;

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
        SqLog.println("[mesh] Parent connected");
        s_connected = true;
        s_parentRetries = 0;
        lockChannel();  // Stop continuous scanning — mesh channel is known
        if (esp_mesh_is_root()) {
            SqLog.println("[mesh] I am ROOT");
            // Root is connected to the router — start DHCP to get a STA IP
            esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta) {
                esp_netif_dhcpc_start(sta);
            }
        }
        updateRtcState();

        // Send heartbeat immediately so the gateway adds us to PeerTable
        // before the election completes (election can take 3s settle + 15s timeout)
        if (!esp_mesh_is_root()) {
            HeartbeatMsg hb;
            hb.type = MSG_TYPE_HEARTBEAT;
            esp_read_mac(hb.mac, ESP_MAC_WIFI_STA);
            hb.battery_mv = (uint16_t)PowerManager::batteryMv();
            hb.flags = 0;
            if (esp_mesh_get_type() == MESH_LEAF) {
                hb.flags |= PEER_STATUS_LEAF;
            }
            esp_read_mac(hb.softap_mac, ESP_MAC_WIFI_SOFTAP);
            // Use logical gateway MAC if known, else fall back to ESP-IDF root
            static const uint8_t zero[6] = {0};
            if (memcmp(s_gatewayMac, zero, 6) != 0) {
                MeshConductor::sendToNode(s_gatewayMac, &hb, sizeof(hb));
            } else {
                MeshConductor::sendToRoot(&hb, sizeof(hb));
            }
        }

        // Assign role based on mesh state (root = gateway)
        if (!s_roleAssigned) {
            assignRoleFromMeshState();
        }

        // Scan delegate: report result now that we're connected
        checkAndReportScanResult();
        break;
    }

    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t* disc = (mesh_event_disconnected_t*)event_data;

        // Root losing its upstream router is not a mesh disconnect — the mesh
        // is still alive with children connected. Only non-root nodes treat
        // parent disconnect as a real mesh disconnection.
        if (esp_mesh_is_root()) {
            // Suppress spam when routerless — no creds means nothing to connect to
            if (s_hasRouterCreds) {
                SqLog.printf("[mesh] Root lost router connection (reason=%d)\n", disc->reason);
            }
            break;
        }

        SqLog.printf("[mesh] Parent disconnected reason=%d\n", disc->reason);

        s_connected = false;
        updateRtcState();
        // reason=8 (ASSOC_LEAVE) is a voluntary mesh parent switch — the mesh
        // stack will reconnect automatically. Don't treat it as gateway loss.
        if (disc->reason == WIFI_REASON_ASSOC_LEAVE) {
            SqLog.println("[mesh] Parent switch in progress — waiting for reconnect");
            break;
        }
        if (s_role && s_role->roleId() == RoleId::PEER) {
            static_cast<MeshNode*>(s_role)->onGatewayLost();
        }
        break;
    }

    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t* child = (mesh_event_child_connected_t*)event_data;
        SqLog.printf("[mesh] Child connected: %02X:%02X:%02X:%02X:%02X:%02X\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5]);
        if (s_role) s_role->onPeerJoined(child->mac);
        updateRtcState();
        break;
    }

    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t* child = (mesh_event_child_disconnected_t*)event_data;
        SqLog.printf("[mesh] Child disconnected: %02X:%02X:%02X:%02X:%02X:%02X reason=%u\n",
            child->mac[0], child->mac[1], child->mac[2],
            child->mac[3], child->mac[4], child->mac[5], child->reason);
        if (s_role) s_role->onPeerLeft(child->mac);
        updateRtcState();
        break;
    }

    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
        updateRtcState();
        break;

    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t* root = (mesh_event_root_address_t*)event_data;
        SqLog.printf("[mesh] Root address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            root->addr[0], root->addr[1], root->addr[2],
            root->addr[3], root->addr[4], root->addr[5]);
        rtc_state_t* map = RtcState::get();
        memcpy(map->gateway_mac, root->addr, 6);
        updateRtcState();
        break;
    }

    case MESH_EVENT_NO_PARENT_FOUND:
        s_parentRetries++;
        SqLog.printf("[mesh] No parent found (attempt %u)\n", s_parentRetries);

        // Root with real creds that can't reach router: reboot after retries
        if (esp_mesh_is_root() && s_hasRouterCreds && s_parentRetries >= MESH_MAX_RETRIES) {
            SqLog.println("[mesh] Root can't reach router — rebooting");
            MeshConductor::stop();
            SQ_LIGHT_SLEEP(MESH_REELECT_SLEEP_MS);
            esp_restart();
        }
        break;

    case MESH_EVENT_ROOT_SWITCH_REQ: {
        SqLog.println("[mesh] Root switch requested — reassigning role from mesh state");
        s_roleAssigned = false;
        assignRoleFromMeshState();
        break;
    }

    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t* net = (mesh_event_network_state_t*)event_data;
        SqLog.printf("[mesh] Network state: is_rootless=%d\n", net->is_rootless);
        break;
    }

    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t* cs = (mesh_event_channel_switch_t*)event_data;
        SqLog.printf("[mesh] Channel switch to %d\n", cs->channel);
        lockChannel();  // Update locked channel after root-driven migration
        break;
    }

    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t* net = (mesh_event_find_network_t*)event_data;
        SqLog.printf("[mesh] Found network on channel %d\n", net->channel);
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

    CredentialTable::init();
    SqLog.printf("[mesh] Credential table: %u slot(s)\n", CredentialTable::count());

    // Config response semaphore
    if (!s_configRespSema)
        s_configRespSema = xSemaphoreCreateBinary();

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

    // Initialize mesh early — needed for prescan before election.
    // esp_mesh_init() just registers hooks; it doesn't start mesh tasks
    // or take over WiFi, so the scan-delegate WiFi scan and ESP-NOW
    // election still work fine after this.
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                &meshEventHandler, NULL));

    // ESP-NOW election runs here — BEFORE esp_mesh_start() which takes over WiFi internals
    bool isDelegateReturn = RtcState::isValid() && RtcState::get()->delegate_active;
    // Scan delegate: scan_ssid is populated + scan_result==0 means scan hasn't run yet
    bool isScanDelegate   = RtcState::isValid() &&
                            RtcState::get()->scan_ssid[0] != '\0' &&
                            RtcState::get()->scan_result == 0;
    bool isForceGateway = RtcState::isValid() && RtcState::get()->force_gateway;
    bool skipElection = isDelegateReturn || isScanDelegate || isForceGateway;

    // Scan delegate: run all-channel scan NOW while WiFi is free (pre-mesh)
    if (isScanDelegate) {
        rtc_state_t* rtc = RtcState::get();
        SqLog.printf("[scan] Pre-mesh scan for \"%s\"...\n", rtc->scan_ssid);

        wifi_scan_config_t scanCfg = {};
        scanCfg.show_hidden = false;
        scanCfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
        scanCfg.scan_time.active.min = 120;
        scanCfg.scan_time.active.max = 300;
        esp_err_t scanErr = esp_wifi_scan_start(&scanCfg, true);

        rtc->scan_result  = 2;  // default: not found
        rtc->scan_channel = 0;
        rtc->scan_rssi    = -128;

        if (scanErr == ESP_OK) {
            uint16_t apCount = 0;
            esp_wifi_scan_get_ap_num(&apCount);
            uint16_t maxAps = (apCount > 30) ? 30 : apCount;
            wifi_ap_record_t* aps = maxAps ? (wifi_ap_record_t*)malloc(maxAps * sizeof(wifi_ap_record_t)) : nullptr;
            if (aps) {
                esp_wifi_scan_get_ap_records(&maxAps, aps);
                for (uint16_t i = 0; i < maxAps; i++) {
                    if (strcmp((const char*)aps[i].ssid, rtc->scan_ssid) == 0) {
                        if (aps[i].rssi > rtc->scan_rssi) {
                            rtc->scan_result  = 1;
                            rtc->scan_channel = aps[i].primary;
                            rtc->scan_rssi    = aps[i].rssi;
                        }
                    }
                }
                free(aps);
            }
            esp_wifi_clear_ap_list();
        } else {
            SqLog.printf("[scan] Scan failed: %s\n", esp_err_to_name(scanErr));
        }

        if (rtc->scan_result == 1) {
            SqLog.printf("[scan] FOUND \"%s\" on ch%u (RSSI %d)\n",
                         rtc->scan_ssid, rtc->scan_channel, rtc->scan_rssi);
        } else {
            SqLog.printf("[scan] NOT FOUND \"%s\"\n", rtc->scan_ssid);
        }
        RtcState::save();
    }

    // Note: s_role may be set later by boot path; start() handles that case
    if (!skipElection) {
        // Load creds early to set s_hasRouterCreds for election context
        char ssid[33] = {}, pass[65] = {};
        bool credsInNvs = SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass));
        bool suppressCreds = isDelegateReturn;
        s_hasRouterCreds = credsInNvs && !suppressCreds;

        // --- Pre-scan: briefly start mesh to detect an existing SQUEEK network ---
        // If a mesh is already running, we skip the election and join as peer.
        // If not, we stop and fall through to the ESP-NOW election as usual.
        {
            s_prescanSema = xSemaphoreCreateBinary();
            s_prescanActive = true;

            mesh_cfg_t pre = MESH_INIT_CONFIG_DEFAULT();
            memcpy((uint8_t*)&pre.mesh_id, s_meshId, 6);
            if (s_hasRouterCreds) {
                memcpy(pre.router.ssid, ssid, strlen(ssid));
                pre.router.ssid_len = strlen(ssid);
                memcpy(pre.router.password, pass, strlen(pass));
            }
            pre.channel = 0;  // scan all channels
            pre.mesh_ap.max_connection = 6;
            pre.crypto_funcs = NULL;

            esp_mesh_set_config(&pre);
            esp_mesh_set_max_layer(MESH_MAX_LAYER);
            esp_mesh_set_self_organized(true, true);
            esp_mesh_start();

            SqLog.println("[mesh] Pre-scan: searching for existing mesh...");

            s_prescanFound = (xSemaphoreTake(s_prescanSema,
                                pdMS_TO_TICKS(MESH_PRESCAN_TIMEOUT_MS)) == pdTRUE);

            esp_mesh_stop();
            vTaskDelay(pdMS_TO_TICKS(100));

            s_prescanActive = false;
            vSemaphoreDelete(s_prescanSema);
            s_prescanSema = nullptr;

            // esp_mesh_stop() on ESP-IDF 5.5.2 tears down mesh init state.
            // Re-init so start() can reconfigure and start fresh.
            esp_mesh_deinit();
            ESP_ERROR_CHECK(esp_mesh_init());

            if (s_prescanFound) {
                SqLog.printf("[mesh] Pre-scan: mesh found on ch%u — skipping election\n",
                             s_prescanChannel);
            } else {
                SqLog.println("[mesh] Pre-scan: no mesh found — proceeding to election");
            }
        }

        if (!s_prescanFound) {
            s_electionResult = EspNowElection::run();
            s_electionRan = true;
        }
    }

    // BOOT button (GPIO9) — gateway: delegate, disconnected: force root
    gpio_config_t btn_cfg = {};
    btn_cfg.pin_bit_mask = (1ULL << BOOT_BUTTON_PIN);
    btn_cfg.mode = GPIO_MODE_INPUT;
    btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_cfg.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&btn_cfg);
    gpio_install_isr_service(0);  // OK if already installed (ESP_ERR_INVALID_STATE)
    gpio_isr_handler_add(BOOT_BUTTON_PIN, bootButtonISR, nullptr);
    SqLog.println("[mesh] BOOT button (GPIO9) ready");
}

void MeshConductor::start() {
    static bool s_meshStarting = false;
    if (s_started || s_meshStarting) {
        Serial.println("[mesh] Already started, ignoring duplicate start()");
        return;
    }
    s_meshStarting = true;

    // --- Load credentials (may already be loaded by init() for election) ---
    char ssid[33] = {}, pass[65] = {};
    bool credsInNvs = SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass));
    bool suppressCreds = RtcState::isValid() && RtcState::get()->delegate_active;
    s_hasRouterCreds = credsInNvs && !suppressCreds;

    // --- Use election result from init(), or skip if it didn't run ---
    ElectionResult election = {};
    if (s_electionRan) {
        election = s_electionResult;
    } else if (s_prescanFound) {
        SqLog.println("[mesh] No election ran (prescan found existing mesh)");
    } else {
        SqLog.println("[mesh] No election ran (delegate return or pre-assigned role)");
        esp_read_mac(election.winner_mac, ESP_MAC_WIFI_STA);
        election.i_am_winner    = false;
        election.target_channel = 0;  // scan all
        election.candidate_count = 0;
    }

    // --- Configure mesh ---
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t*)&cfg.mesh_id, s_meshId, 6);

    // Router config
    memset(&cfg.router, 0, sizeof(cfg.router));
    if (s_hasRouterCreds) {
        memcpy(cfg.router.ssid, ssid, strlen(ssid));
        cfg.router.ssid_len = strlen(ssid);
        memcpy(cfg.router.password, pass, strlen(pass));
        SqLog.printf("[mesh] Router config set: SSID=%s\n", ssid);
    } else if (suppressCreds) {
        SqLog.println("[mesh] Suppressing router creds (delegate return)");
    }

    // Channel: prescan > election > legacy fallback
    bool isDelegateReturn = RtcState::isValid() && RtcState::get()->delegate_active;
    bool isScanDelegateReturn = RtcState::isValid() && RtcState::get()->scan_result != 0;
    if (s_prescanFound) {
        cfg.channel = s_prescanChannel;
        SqLog.printf("[mesh] Channel from prescan: %u\n", cfg.channel);
    } else if (s_electionRan && election.candidate_count > 0) {
        cfg.channel = election.target_channel;
        SqLog.printf("[mesh] Channel from election: %u\n", cfg.channel);
    } else if (isDelegateReturn || isScanDelegateReturn) {
        cfg.channel = 0;  // scan all for existing mesh
    } else if (s_hasRouterCreds) {
        cfg.channel = 0;  // scan for router
    } else {
        cfg.channel = 1;  // routerless fallback
    }

    // Mesh AP settings
    cfg.mesh_ap.max_connection = 6;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));
    cfg.crypto_funcs = NULL;

    esp_err_t err = esp_mesh_set_config(&cfg);
    if (err == ESP_ERR_MESH_ARGUMENT) {
        const char* ph = "SQUEEK_MESH";
        memcpy(cfg.router.ssid, ph, strlen(ph));
        cfg.router.ssid_len = strlen(ph);
        memset(cfg.router.password, 0, sizeof(cfg.router.password));
        ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    }

    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    // Election winner becomes root before mesh starts
    if (s_electionRan && election.i_am_winner) {
        SqLog.println("[mesh] Election winner — setting MESH_ROOT before start");
        esp_mesh_set_type(MESH_ROOT);
    }

    // Force gateway: button-triggered manual gateway designation
    bool isForceGateway = RtcState::isValid() && RtcState::get()->force_gateway;
    if (isForceGateway) {
        SqLog.println("[mesh] Force-gateway flag — setting MESH_ROOT before start");
        esp_mesh_set_type(MESH_ROOT);
        RtcState::get()->force_gateway = 0;  // consume the flag
        RtcState::save();
    }

    // Reset state
    s_roleAssigned = (s_role != nullptr);
    s_parentRetries = 0;

    ESP_ERROR_CHECK(esp_mesh_start());
    SqLog.println("[mesh] Mesh starting...");

    // Suppress noisy ESP-MESH/WiFi internal logs (reason=201 spam,
    // mesh_schedule.c window warnings, etc.)
    esp_log_level_set("mesh", ESP_LOG_ERROR);
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    // Election winner / force-gateway: assign GATEWAY immediately (don't wait for
    // PARENT_CONNECTED, which only fires if/when the router is reachable)
    if ((s_electionRan && election.i_am_winner) || isForceGateway) {
        if (!s_roleAssigned) {
            assignRoleFromMeshState();
        }
    }

    // Check if we booted as scan delegate and need to report results
    if (RtcState::isValid() && RtcState::get()->scan_result != 0) {
        s_scanResultPending = true;
    }

    // Scan delegate self-scan: if we became gateway, handle result locally
    if (s_scanResultPending && s_role && s_role->roleId() == RoleId::GATEWAY) {
        s_scanResultPending = false;
        rtc_state_t* rtc = RtcState::get();
        if (rtc->scan_result == 1) {
            SqLog.printf("[scan] Self-scan verified: %s ch%u — broadcasting and rebooting\n",
                         rtc->scan_ssid, rtc->scan_channel);
            // Commit verified creds to NVS
            CredentialTable::add(rtc->scan_ssid, rtc->scan_pass);
            // Broadcast creds + reboot warning to peers
            WifiCredsMsg wc = {};
            wc.type = MSG_TYPE_WIFI_CREDS;
            strncpy(wc.ssid, rtc->scan_ssid, 32);
            strncpy(wc.password, rtc->scan_pass, 64);
            MeshConductor::broadcastToAll(&wc, sizeof(wc));
            RebootWarnMsg rw = {};
            rw.type = MSG_TYPE_REBOOT_WARN;
            rw.delay_ms = 2000;
            MeshConductor::broadcastToAll(&rw, sizeof(rw));
            memset(rtc->scan_ssid, 0, sizeof(rtc->scan_ssid));
            memset(rtc->scan_pass, 0, sizeof(rtc->scan_pass));
            rtc->scan_result = 0;
            RtcState::save();
            TimerHandle_t t = xTimerCreate("scanReboot", pdMS_TO_TICKS(2000),
                pdFALSE, nullptr, [](TimerHandle_t timer) {
                    xTimerDelete(timer, 0);
                    esp_restart();
                });
            if (t) xTimerStart(t, 0);
        } else {
            SqLog.printf("[scan] Self-scan: \"%s\" not found\n", rtc->scan_ssid);
            memset(rtc->scan_ssid, 0, sizeof(rtc->scan_ssid));
            memset(rtc->scan_pass, 0, sizeof(rtc->scan_pass));
            rtc->scan_result = 0;
            RtcState::save();
        }
    }
}

void MeshConductor::onBootButton() {
    // Multi-purpose: depends on current state
    if (s_role && s_role->roleId() == RoleId::GATEWAY) {
        SqLog.println("[mesh] BOOT button — gateway: starting delegation");
        static_cast<Gateway*>(s_role)->startDelegation();
    } else if (s_connected && s_role && s_role->roleId() == RoleId::PEER) {
        // Connected peer — request to become gateway
        SqLog.println("[mesh] BOOT button — requesting force-gateway");
        ForceGatewayMsg msg = {};
        msg.type = MSG_TYPE_FORCE_GATEWAY;
        sendToRoot(&msg, sizeof(msg));
    } else if (!s_connected && !esp_mesh_is_root()) {
        // Disconnected node — force self-election as root
        SqLog.println("[mesh] BOOT button — forcing root self-election");
        esp_err_t err = esp_mesh_set_type(MESH_ROOT);
        if (err != ESP_OK) {
            SqLog.printf("[mesh] set_type(ROOT) failed: %s\n", esp_err_to_name(err));
            return;
        }
        if (!s_roleAssigned) {
            assignRoleFromMeshState();
        }
    } else {
        SqLog.println("[mesh] BOOT button — ignored");
    }
}

void MeshConductor::stop() {
    if (s_role) {
        s_role->end();
        delete s_role;
        s_role = nullptr;
    }
    esp_mesh_stop();
    s_started = false;
    s_connected = false;
    s_roleAssigned = false;
}

bool MeshConductor::isCredAckReceived() {
    return s_credAckReceived;
}

bool MeshConductor::isConnected() {
    return s_connected;
}

bool MeshConductor::isGateway() {
    return s_role && s_role->roleId() == RoleId::GATEWAY;
}

IMeshRole* MeshConductor::role() {
    return s_role;
}

void MeshConductor::setRole(IMeshRole* role) {
    if (s_role) {
        s_role->end();
        delete s_role;
    }
    s_role = role;
    if (s_role) {
        s_roleAssigned = true;
        s_role->begin();
    }
}

void MeshConductor::printStatus() {
    Serial.println("=== Mesh Status ===");
    Serial.printf("Started: %s\n", s_started ? "yes" : "no");
    Serial.printf("Connected: %s\n", s_connected ? "yes" : "no");
    Serial.printf("Is Root: %s\n", esp_mesh_is_root() ? "yes" : "no");
    Serial.printf("Role assigned: %s\n", s_roleAssigned ? "yes" : "no");
    const char* roleName = !s_role ? "none"
        : s_role->roleId() == RoleId::GATEWAY ? "GATEWAY"
        : s_role->roleId() == RoleId::DELEGATE ? "DELEGATE"
        : s_role->roleId() == RoleId::SCAN_DELEGATE ? "SCAN_DELEGATE" : "NODE";
    Serial.printf("Role: %s\n", roleName);
    Serial.printf("Layer: %d\n", esp_mesh_get_layer());
    {
        // esp_wifi_get_channel() returns garbage on root (STA not associated).
        // Use the mesh config channel instead — reliable for all roles.
        mesh_cfg_t meshCfg;
        esp_mesh_get_config(&meshCfg);
        Serial.printf("Channel: %u\n", meshCfg.channel);
    }
    Serial.printf("Tenure score: %u (rssi=%d)\n",
        computeTenureScore(s_bestRouterRssi), s_bestRouterRssi);
    Serial.printf("Credentials: %u slot(s)\n", CredentialTable::count());

    int total = esp_mesh_get_total_node_num();
    Serial.printf("Total nodes: %d\n", total);

    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, sizeof(routing_table), &table_size);
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

void MeshConductor::stepDown() {
    if (!s_role || s_role->roleId() != RoleId::GATEWAY) {
        Serial.println("Not gateway — cannot step down.");
        return;
    }

    // Broadcast delegate ticket to all peers before waiving
    // (we don't know who ESP-MESH will elect as new root)
    Gateway* gw = static_cast<Gateway*>(s_role);
    if (gw->hasDelegateTicket()) {
        // Broadcast ticket to all peers so the new gateway picks it up
        rtc_state_t* rtc = RtcState::get();
        DelegateTicketMsg dt = {};
        dt.type = MSG_TYPE_DELEGATE_TICKET;
        memcpy(dt.delegate_mac, rtc->ticket_delegate_mac, 6);
        dt.remaining_s = rtc->ticket_remaining_s;
        broadcastToAll(&dt, sizeof(dt));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    SqLog.println("[mesh] Waiving root — ESP-MESH will re-elect");
    esp_err_t err = esp_mesh_waive_root(nullptr, MESH_VOTE_REASON_ROOT_INITIATED);
    if (err != ESP_OK) {
        SqLog.printf("[mesh] waive_root failed: %s\n", esp_err_to_name(err));
    }
    // MESH_EVENT_ROOT_SWITCH_REQ will fire → assignRoleFromMeshState() → reboot as PEER
}

// One-shot task to perform role transfer outside timer callback context
static void stepDownTask(void* arg) {
    (void)arg;
    MeshConductor::stepDown();
    vTaskDelete(nullptr);
}

void MeshConductor::requestStepDown() {
    SqLog.println("[mesh] Scheduling step-down (deferred to task context)...");
    // stepDown() does heavy work (broadcast, role switch, logging) that
    // overflows the FreeRTOS timer service task stack.  Spawn a one-shot
    // task with enough stack to handle it safely.
    xTaskCreate(stepDownTask, "stepdown", 4096, nullptr, 5, nullptr);
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
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    mesh_data_t mdata;
    mdata.data = (uint8_t*)data;
    mdata.size = len;
    mdata.proto = MESH_PROTO_BIN;
    mdata.tos = MESH_TOS_P2P;

    esp_err_t last_err = ESP_OK;

    if (esp_mesh_is_root()) {
        // ESP-IDF root: use routing table for complete mesh coverage
        mesh_addr_t routing_table[MESH_MAX_NODES];
        int table_size = 0;
        esp_mesh_get_routing_table(routing_table, sizeof(routing_table), &table_size);

        for (int i = 0; i < table_size; i++) {
            if (memcmp(routing_table[i].addr, own_mac, 6) == 0) continue;
            esp_err_t err = esp_mesh_send(&routing_table[i], &mdata, MESH_DATA_P2P, NULL, 0);
            if (err != ESP_OK) last_err = err;
        }
    } else {
        // Non-root gateway (after role transfer): use PeerTable MACs
        uint8_t count = PeerTable::peerCount();
        for (uint8_t i = 0; i < count; i++) {
            PeerEntry* e = PeerTable::getEntryByIndex(i);
            if (!e) continue;
            if (memcmp(e->mac, own_mac, 6) == 0) continue;
            if (e->flags & PEER_STATUS_DEAD) continue;
            mesh_addr_t addr;
            memcpy(addr.addr, e->mac, 6);
            esp_err_t err = esp_mesh_send(&addr, &mdata, MESH_DATA_P2P, NULL, 0);
            if (err != ESP_OK) last_err = err;
        }
    }
    return last_err;
}

// --- Remote config helpers ---

bool MeshConductor::sendConfigReq(const uint8_t* sta_mac, const char* json, uint8_t reqId) {
    size_t jsonLen = strlen(json);
    if (jsonLen + 3 > 512) return false;  // too large

    uint8_t buf[512];
    buf[0] = MSG_TYPE_CONFIG_REQ;
    buf[1] = reqId;
    memcpy(&buf[2], json, jsonLen + 1);  // include null terminator

    s_configRespReqId = reqId;
    s_configRespBuf[0] = '\0';
    // Drain any stale semaphore
    if (s_configRespSema)
        xSemaphoreTake(s_configRespSema, 0);

    esp_err_t err = sendToNode(sta_mac, buf, 2 + jsonLen + 1);
    return err == ESP_OK;
}

bool MeshConductor::waitConfigResp(char* outBuf, size_t bufSize, uint32_t timeout_ms) {
    if (!s_configRespSema) return false;
    if (xSemaphoreTake(s_configRespSema, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        strncpy(outBuf, s_configRespBuf, bufSize - 1);
        outBuf[bufSize - 1] = '\0';
        return true;
    }
    return false;
}

// --- Gateway MAC tracking ---

const uint8_t* MeshConductor::gatewayMac() {
    return s_gatewayMac;
}

void MeshConductor::setGatewayMac(const uint8_t* mac) {
    memcpy(s_gatewayMac, mac, 6);
}

const PeerSyncEntry* MeshConductor::peerShadowEntries() {
    return s_peerShadow;
}

int8_t MeshConductor::bestRouterRssi() {
    return s_bestRouterRssi;
}

void MeshConductor::setBestRouterRssi(int8_t rssi) {
    s_bestRouterRssi = rssi;
}
