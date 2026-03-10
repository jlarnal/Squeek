#include "mesh_conductor.h"
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
static uint16_t    s_gwTenure       = 0;        // cached from NVS
static bool        s_hasRouterCreds = false;     // true when real WiFi creds loaded

// Routerless bootstrap: one-shot timer to self-elect as root
static TimerHandle_t s_bootstrapTimer = nullptr;

static void assignRoleFromMeshState();  // forward decl for bootstrapTimerCb

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

// Routerless bootstrap: self-elect as root after deterministic timeout
// Uses MAC-based delay so the lowest-MAC node wins the race every time.
static void bootstrapTimerCb(TimerHandle_t timer) {
    xTimerDelete(timer, 0);
    s_bootstrapTimer = nullptr;

    if (s_connected || esp_mesh_is_root()) return;  // already resolved

    SqLog.println("[mesh] Bootstrap timeout — self-electing as root");
    esp_err_t err = esp_mesh_set_type(MESH_ROOT);
    if (err != ESP_OK) {
        SqLog.printf("[mesh] set_type(ROOT) failed: %s\n", esp_err_to_name(err));
        return;
    }

    // In routerless mode, PARENT_CONNECTED won't fire, so assign role directly
    if (!s_roleAssigned) {
        assignRoleFromMeshState();
    }
}

// Compute a deterministic bootstrap delay from the node's MAC address.
// Lower MAC hash → shorter delay → that node becomes root first.
// Range: 5-30s, spread by FNV-1a hash of MAC.
static uint32_t macBasedBootstrapDelay() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // FNV-1a hash of MAC → uniform distribution
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }

    // Map to 5-30s range
    return 5000 + (h % 25001);
}

// Schedule the bootstrap timer (if appropriate).
// Called once from start(), before NO_PARENT_FOUND ever fires.
static void scheduleBootstrapTimer() {
    if (s_hasRouterCreds || s_bootstrapTimer) return;  // not routerless, or already scheduled

    bool isDelegateReturn = RtcState::isValid() && RtcState::get()->delegate_active;
    if (isDelegateReturn) return;  // returning delegate must rejoin, not self-promote

    uint32_t delay = macBasedBootstrapDelay();
    SqLog.printf("[mesh] Bootstrap timer: self-election in %lu ms (MAC-based)\n", delay);
    s_bootstrapTimer = xTimerCreate("bootstrap", pdMS_TO_TICKS(delay),
                                     pdFALSE, nullptr, bootstrapTimerCb);
    if (s_bootstrapTimer) xTimerStart(s_bootstrapTimer, 0);
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
        s_gwTenure++;
        nvsWriteTenure();
        SqLog.println("[mesh] Role assigned: GATEWAY (I am root)");
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

            if (msgType == MSG_TYPE_HEARTBEAT && data.size >= sizeof(HeartbeatMsg)) {
                HeartbeatMsg* hb = (HeartbeatMsg*)rx_buf;
                if (s_role && s_role->roleId() == RoleId::GATEWAY) {
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

                // Only save if creds are new or changed
                char curSsid[33] = {}, curPass[65] = {};
                bool haveCreds = SqWebServer::loadWifiCreds(curSsid, sizeof(curSsid), curPass, sizeof(curPass));
                bool credsChanged = !haveCreds || strcmp(curSsid, wc->ssid) != 0 || strcmp(curPass, wc->password) != 0;
                if (credsChanged) {
                    SqWebServer::saveWifiCreds(wc->ssid, wc->password);
                    SqLog.printf("[mesh] Saved WiFi credentials (SSID=%s)\n", wc->ssid);
                } else {
                    SqLog.printf("[mesh] WiFi credentials unchanged (SSID=%s)\n", wc->ssid);
                }

                if (esp_mesh_is_root()) {
                    // Start web server if not running
                    if (!SqWebServer::isRunning()) {
                        SqLog.println("[mesh] Starting web server with new credentials");
                        SqWebServer::start();
                    }
                    // Broadcast new/changed creds to all peers
                    if (credsChanged) {
                        SqLog.println("[mesh] Broadcasting WiFi credentials to all peers");
                        MeshConductor::broadcastToAll(wc, sizeof(WifiCredsMsg));
                    }

                    // Creds are in NVS + broadcast to peers. Reboot to apply —
                    // esp_mesh_set_config() is unreliable at runtime, and we can't
                    // call esp_mesh_stop() from meshRxTask (kills our own stack).
                    // Defer the reboot via timer so meshRxTask can exit cleanly.
                    if (credsChanged) {
                        SqLog.println("[mesh] Router creds saved — rebooting in 2s to apply");
                        TimerHandle_t t = xTimerCreate("gwReboot", pdMS_TO_TICKS(2000),
                            pdFALSE, nullptr, [](TimerHandle_t timer) {
                                xTimerDelete(timer, 0);
                                esp_restart();
                            });
                        if (t) xTimerStart(t, 0);
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
        if (s_bootstrapTimer) {
            xTimerStop(s_bootstrapTimer, 0);
            xTimerDelete(s_bootstrapTimer, 0);
            s_bootstrapTimer = nullptr;
        }
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

        // Bootstrap timer is already running from start() — nothing to schedule here.

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
        // Cancel bootstrap timer — no need to self-elect, we found an existing mesh
        if (s_bootstrapTimer) {
            SqLog.println("[mesh] Cancelling bootstrap — found existing network");
            xTimerStop(s_bootstrapTimer, 0);
            xTimerDelete(s_bootstrapTimer, 0);
            s_bootstrapTimer = nullptr;
        }
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

    // Initialize mesh
    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                                &meshEventHandler, NULL));

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

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t*)&cfg.mesh_id, s_meshId, 6);

    // Router config: populate with real creds if available, else placeholder
    memset(&cfg.router, 0, sizeof(cfg.router));
    {
        char ssid[33] = {}, pass[65] = {};
        bool credsInNvs = SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass));
        bool suppressCreds = RtcState::isValid() && RtcState::get()->delegate_active;
        s_hasRouterCreds = credsInNvs && !suppressCreds;
        if (s_hasRouterCreds) {
            memcpy(cfg.router.ssid, ssid, strlen(ssid));
            cfg.router.ssid_len = strlen(ssid);
            memcpy(cfg.router.password, pass, strlen(pass));
            SqLog.printf("[mesh] Router config set: SSID=%s (auto-channel)\n", ssid);
        } else if (suppressCreds) {
            SqLog.println("[mesh] Suppressing router creds (delegate return — rejoin mesh first)");
        }
    }

    // Channel selection:
    //  - Router creds active: channel=0 (scan for router)
    //  - Delegate return (creds suppressed): channel=0 (scan all channels for existing mesh)
    //  - Routerless bootstrap: channel=1 (fixed so all nodes converge)
    bool isDelegateReturn = RtcState::isValid() && RtcState::get()->delegate_active;
    cfg.channel = (s_hasRouterCreds || isDelegateReturn) ? 0 : 1;

    // Mesh AP settings (no password for Phase 1)
    cfg.mesh_ap.max_connection = 6;
    memset(cfg.mesh_ap.password, 0, sizeof(cfg.mesh_ap.password));

    // No encryption for Phase 1
    cfg.crypto_funcs = NULL;

    esp_err_t err = esp_mesh_set_config(&cfg);
    if (err == ESP_ERR_MESH_ARGUMENT) {
        // SSID check failed — use placeholder (routerless mesh)
        const char* ph = "SQUEEK_MESH";
        memcpy(cfg.router.ssid, ph, strlen(ph));
        cfg.router.ssid_len = strlen(ph);
        memset(cfg.router.password, 0, sizeof(cfg.router.password));
        ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    }

    // Configure mesh topology
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    // Reset state — don't reset s_role (may have been set by boot path)
    s_roleAssigned = (s_role != nullptr);  // if role pre-assigned, skip assignment
    s_parentRetries = 0;

    ESP_ERROR_CHECK(esp_mesh_start());
    SqLog.println("[mesh] Mesh starting...");

    // Suppress noisy ESP-MESH internal logs when routerless (reason=201 spam)
    if (!s_hasRouterCreds) {
        esp_log_level_set("mesh", ESP_LOG_WARN);
        esp_log_level_set("wifi", ESP_LOG_WARN);
    }

    // Start bootstrap timer immediately — don't wait for NO_PARENT_FOUND (60 scans, ~3 min).
    // MAC-based delay ensures deterministic root election order.
    scheduleBootstrapTimer();
}

void MeshConductor::onBootButton() {
    // Multi-purpose: depends on current state
    if (s_role && s_role->roleId() == RoleId::GATEWAY) {
        SqLog.println("[mesh] BOOT button — gateway: starting delegation");
        static_cast<Gateway*>(s_role)->startDelegation();
    } else if (!s_connected && !esp_mesh_is_root()) {
        // Disconnected node — force self-election as root
        SqLog.println("[mesh] BOOT button — forcing root self-election");
        if (s_bootstrapTimer) {
            xTimerStop(s_bootstrapTimer, 0);
            xTimerDelete(s_bootstrapTimer, 0);
            s_bootstrapTimer = nullptr;
        }
        esp_err_t err = esp_mesh_set_type(MESH_ROOT);
        if (err != ESP_OK) {
            SqLog.printf("[mesh] set_type(ROOT) failed: %s\n", esp_err_to_name(err));
            return;
        }
        if (!s_roleAssigned) {
            assignRoleFromMeshState();
        }
    } else {
        SqLog.println("[mesh] BOOT button — ignored (connected peer)");
    }
}

void MeshConductor::stop() {
    if (s_bootstrapTimer) {
        xTimerStop(s_bootstrapTimer, 0);
        xTimerDelete(s_bootstrapTimer, 0);
        s_bootstrapTimer = nullptr;
    }
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
        : s_role->roleId() == RoleId::DELEGATE ? "DELEGATE" : "NODE";
    Serial.printf("Role: %s\n", roleName);
    Serial.printf("Layer: %d\n", esp_mesh_get_layer());
    {
        uint8_t primary = 0;
        wifi_second_chan_t secondary;
        esp_wifi_get_channel(&primary, &secondary);
        Serial.printf("Channel: %u\n", primary);
    }
    Serial.printf("Gateway tenure: %u\n", s_gwTenure);

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
