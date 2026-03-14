#include "mesh_conductor.h"
#include "credential_table.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "position_solver.h"
#include "power_manager.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include "sq_log.h"
#include "orchestrator.h"
#include "clock_sync.h"
#include "web_server.h"
#include "mesh_delegate.h"
#include "rtc_state.h"
#include <Arduino.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
#include <esp_mac.h>

// Gateway self-heartbeat timer — updates own battery in PeerTable
static TimerHandle_t s_gwHeartbeatTimer = nullptr;
// Scan contest: collect results from peers
static TimerHandle_t s_scanContestTimer = nullptr;

struct ScanContestEntry { uint8_t mac[6]; uint8_t ssid_count; };
static ScanContestEntry s_scanResults[MESH_MAX_NODES];
static uint8_t s_scanResultCount = 0;
static bool s_scanContestActive = false;

// Delegate ticket — tracks an active delegate out in the field
static TimerHandle_t s_ticketTimer = nullptr;
static uint8_t  s_ticketMac[6] = {0};
static uint16_t s_ticketRemaining = 0;

// RSSI re-evaluation state — gateway tracks best peer tenure from heartbeats
static uint16_t s_bestPeerTenure  = 0;
static uint32_t s_hbTickCounter   = 0;   // counts heartbeat timer ticks

static void ticketCountdownCb(TimerHandle_t t) {
    (void)t;
    if (s_ticketRemaining > 0) {
        s_ticketRemaining--;
        // Persist to RTC every 10s (avoid flash wear)
        if (s_ticketRemaining % 10 == 0) {
            rtc_state_t* rtc = RtcState::get();
            rtc->ticket_remaining_s = s_ticketRemaining;
            RtcState::save();
        }
        if (s_ticketRemaining == 0) {
            SqLog.println("[gateway] Delegate ticket expired");
            memset(s_ticketMac, 0, 6);
            rtc_state_t* rtc = RtcState::get();
            memset(rtc->ticket_delegate_mac, 0, 6);
            rtc->ticket_remaining_s = 0;
            RtcState::save();
            
        }
    }
}

static void gwHeartbeatCb(TimerHandle_t t) {
    (void)t;
    PeerTable::updateSelf((uint16_t)PowerManager::batteryMv());

    // Periodic RSSI re-evaluation: every N*(1+k) heartbeat ticks
    s_hbTickCounter++;
    uint8_t alive = PeerTable::alivePeerCount();
    if (alive == 0) return;  // no peers to compare against

    // k is Q4.4 fixed-point: 0x08 = 0.5, 0x10 = 1.0
    uint8_t kRaw = (uint8_t)(uint16_t)NvsConfigManager::rssiDecayK;
    // interval = N * (1 + k/16) = N + N*k/16
    uint32_t interval = (uint32_t)alive + ((uint32_t)alive * kRaw) / 16;
    if (interval < 2) interval = 2;  // minimum 2 ticks

    if (s_hbTickCounter >= interval) {
        s_hbTickCounter = 0;

        // Compare our tenure against best peer tenure seen in recent heartbeats
        uint16_t myTenure = computeTenureScore(MeshConductor::bestRouterRssi());
        if (s_bestPeerTenure > myTenure) {
            SqLog.printf("[gateway] Re-eval: peer tenure %u > mine %u — waiving root\n",
                         s_bestPeerTenure, myTenure);
            MeshConductor::requestStepDown();
        }
        // Reset for next evaluation window
        s_bestPeerTenure = 0;
    }
}

// Scan contest timeout — pick winner and dispatch delegate
static void scanContestTimeoutCb(TimerHandle_t t) {
    (void)t;
    s_scanContestActive = false;

    if (s_scanResultCount == 0) {
        SqLog.println("[gateway] Scan contest: no responses — aborting");
        return;
    }

    // Pick peer with highest ssid_count
    uint8_t bestIdx = 0;
    for (uint8_t i = 1; i < s_scanResultCount; i++) {
        if (s_scanResults[i].ssid_count > s_scanResults[bestIdx].ssid_count) {
            bestIdx = i;
        }
    }

    const uint8_t* winner = s_scanResults[bestIdx].mac;
    SqLog.printf("[gateway] Scan contest winner: %02X:%02X:%02X:%02X:%02X:%02X (%d SSIDs)\n",
        winner[0], winner[1], winner[2], winner[3], winner[4], winner[5],
        s_scanResults[bestIdx].ssid_count);

    // Designate winner as delegate
    uint8_t ownMac[6];
    esp_read_mac(ownMac, ESP_MAC_WIFI_STA);

    SetupDelegateMsg msg = {};
    msg.type = MSG_TYPE_SETUP_DELEGATE;
    memcpy(msg.gateway_mac, ownMac, 6);
    MeshConductor::sendToNode(winner, &msg, sizeof(msg));

    // Start delegate ticket countdown
    memcpy(s_ticketMac, winner, 6);
    s_ticketRemaining = NVS_DEFAULT_DELEGATE_TMO;
    rtc_state_t* rtc = RtcState::get();
    memcpy(rtc->ticket_delegate_mac, winner, 6);
    rtc->ticket_remaining_s = s_ticketRemaining;
    RtcState::save();

    SqLog.printf("[gateway] Delegate ticket started: %us\n", s_ticketRemaining);
}

void Gateway::begin() {
    m_peerCount = 0;
    SqLog.println("[gateway] Gateway role active");

    // Clear delegate flag — we're running as gateway now
    RtcState::get()->delegate_active = 0;
    RtcState::save();

    // Initialize Phase 2 subsystems
    PeerTable::init();
    FtmManager::init();
    PositionSolver::init();
    FtmScheduler::init();
    ClockSync::init();

    // Start self-heartbeat timer (update own battery in PeerTable)
    uint32_t hbInterval = (uint32_t)NvsConfigManager::heartbeatInterval_s;
    if (s_gwHeartbeatTimer == nullptr) {
        s_gwHeartbeatTimer = xTimerCreate("gwHb", pdMS_TO_TICKS(hbInterval * 1000),
                                           pdTRUE, nullptr, gwHeartbeatCb);
    } else {
        xTimerChangePeriod(s_gwHeartbeatTimer, pdMS_TO_TICKS(hbInterval * 1000), 0);
    }
    xTimerStart(s_gwHeartbeatTimer, 0);

    // Restore delegate ticket from RTC (survives gateway handoff)
    {
        rtc_state_t* rtc = RtcState::get();
        static const uint8_t zero[6] = {0};
        if (rtc->ticket_remaining_s > 0 && memcmp(rtc->ticket_delegate_mac, zero, 6) != 0) {
            memcpy(s_ticketMac, rtc->ticket_delegate_mac, 6);
            s_ticketRemaining = rtc->ticket_remaining_s;
            SqLog.printf("[gateway] Restored delegate ticket: %02X:%02X:%02X:%02X:%02X:%02X (%us remaining)\n",
                s_ticketMac[0], s_ticketMac[1], s_ticketMac[2],
                s_ticketMac[3], s_ticketMac[4], s_ticketMac[5], s_ticketRemaining);
        }
    }

    // Start ticket countdown timer (always runs, ticks only when remaining > 0)
    if (s_ticketTimer == nullptr) {
        s_ticketTimer = xTimerCreate("ticket", pdMS_TO_TICKS(1000),
                                       pdTRUE, nullptr, ticketCountdownCb);
    }
    xTimerStart(s_ticketTimer, 0);

    // Phase 5: Web UI
    if (SqWebServer::hasWifiCreds()) {
        SqWebServer::start();
    } else {
        // No WiFi creds — user must press BOOT button to trigger delegation
        SqLog.println("[gateway] No WiFi creds — press BOOT button to start delegation");
    }
}

void Gateway::end() {
    SqLog.println("[gateway] Gateway role stopping");

    // Phase 5: Web UI — stop before mesh teardown
    SqWebServer::stop();

    if (s_gwHeartbeatTimer) {
        xTimerStop(s_gwHeartbeatTimer, 0);
    }
    if (s_scanContestTimer) {
        xTimerStop(s_scanContestTimer, 0);
        s_scanContestActive = false;
    }
    if (s_ticketTimer) {
        xTimerStop(s_ticketTimer, 0);
    }

    Orchestrator::setMode(ORCH_OFF);
    ClockSync::stop();

    FtmScheduler::shutdown();
    PeerTable::shutdown();
}

void Gateway::onPeerJoined(const uint8_t* mac) {
    m_peerCount++;
    SqLog.printf("[gateway] Peer joined (%u total): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Peer will send heartbeat shortly — PeerTable entry created on first heartbeat.
    // If we want immediate FTM, queue the new node once it appears in PeerTable.

    // Credential exchange: send all known creds to the new peer
    // Heap-allocate — this runs on sys_evt task which has limited stack
    if (CredentialTable::hasAny()) {
        uint8_t* offerBuf = (uint8_t*)malloc(1024);
        if (offerBuf) {
            offerBuf[0] = MSG_TYPE_CRED_OFFER;
            uint16_t credLen = CredentialTable::toBuffer(&offerBuf[1], 1024 - 1);
            MeshConductor::sendToNode(mac, offerBuf, 1 + credLen);
            SqLog.printf("[gateway] Sent CRED_OFFER to new peer (%u creds)\n",
                         CredentialTable::count());
            free(offerBuf);
        }
    }

    // Notify dashboard clients
    if (SqWebServer::isRunning()) {
        SqWebServer::broadcast("{\"type\":\"peer_join\"}");
    }
}

void Gateway::onPeerLeft(const uint8_t* mac) {
    if (m_peerCount > 0) m_peerCount--;
    SqLog.printf("[gateway] Peer left (%u remaining): %02X:%02X:%02X:%02X:%02X:%02X\n",
        m_peerCount, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Mark peer as dead in PeerTable
    PeerEntry* e = PeerTable::getEntry(mac);
    if (e) {
        e->flags = PEER_STATUS_DEAD;
    }

    // Notify dashboard clients
    if (SqWebServer::isRunning()) {
        SqWebServer::broadcast("{\"type\":\"peer_leave\"}");
    }
}

void Gateway::printStatus() {
    Serial.println("--- Gateway Status ---");

    // Show router-assigned STA IP if connected
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        Serial.printf("Router IP: " IPSTR "\n", IP2STR(&ip.ip));
    } else {
        Serial.printf("Router IP: not connected\n");
    }

    Serial.printf("Peers: %u\n", m_peerCount);
    Serial.printf("Scan contest: %s\n", s_scanContestActive ? "active" : "idle");
    if (s_ticketRemaining > 0) {
        Serial.printf("Delegate ticket: %02X:%02X:%02X:%02X:%02X:%02X (%us remaining)\n",
            s_ticketMac[0], s_ticketMac[1], s_ticketMac[2],
            s_ticketMac[3], s_ticketMac[4], s_ticketMac[5], s_ticketRemaining);
    } else {
        Serial.println("Delegate ticket: none");
    }
    PeerTable::print();
}

void Gateway::startDelegation() {
    if (s_scanContestActive) {
        SqLog.println("[gateway] Scan contest already in progress");
        return;
    }
    if (s_ticketRemaining > 0) {
        SqLog.printf("[gateway] Delegate already active (%us remaining) — ignoring\n", s_ticketRemaining);
        return;
    }

    int meshNodes = esp_mesh_get_total_node_num();
    if (meshNodes <= 1) {
        // Lone gateway — self-delegate
        SqLog.println("[gateway] No peers — self-delegating");
        rtc_state_t* rtc = RtcState::get();
        rtc->next_role = (uint8_t)RoleId::DELEGATE;
        RtcState::save();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }

    // Broadcast scan request to all peers
    SqLog.println("[gateway] Starting scan contest — requesting WiFi scans from peers");
    s_scanResultCount = 0;
    s_scanContestActive = true;

    ScanRequestMsg req = {};
    req.type = MSG_TYPE_SCAN_REQUEST;
    MeshConductor::broadcastToAll(&req, sizeof(req));

    // Start collection timeout (10s default)
    if (s_scanContestTimer == nullptr) {
        s_scanContestTimer = xTimerCreate("scanCtst", pdMS_TO_TICKS(10000),
                                           pdFALSE, nullptr, scanContestTimeoutCb);
    } else {
        xTimerChangePeriod(s_scanContestTimer, pdMS_TO_TICKS(10000), 0);
    }
    xTimerStart(s_scanContestTimer, 0);
}

void Gateway::trackPeerTenure(uint16_t tenure) {
    if (tenure > s_bestPeerTenure) {
        s_bestPeerTenure = tenure;
    }
}

void Gateway::onScanResult(const uint8_t* mac, uint8_t ssid_count) {
    if (!s_scanContestActive) return;

    if (s_scanResultCount < MESH_MAX_NODES) {
        memcpy(s_scanResults[s_scanResultCount].mac, mac, 6);
        s_scanResults[s_scanResultCount].ssid_count = ssid_count;
        s_scanResultCount++;
        SqLog.printf("[gateway] Scan result from %02X:%02X:%02X:%02X:%02X:%02X: %d SSIDs\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ssid_count);
    }

    // Check if all peers have responded
    int expected = esp_mesh_get_total_node_num() - 1;  // minus ourselves
    if ((int)s_scanResultCount >= expected) {
        // All in — stop timer and pick winner now
        if (s_scanContestTimer) xTimerStop(s_scanContestTimer, 0);
        scanContestTimeoutCb(nullptr);
    }
}

bool Gateway::hasDelegateTicket() const {
    return s_ticketRemaining > 0;
}

void Gateway::installTicket(const uint8_t* delegateMac, uint16_t remaining_s) {
    if (remaining_s == 0) return;
    memcpy(s_ticketMac, delegateMac, 6);
    s_ticketRemaining = remaining_s;

    rtc_state_t* rtc = RtcState::get();
    memcpy(rtc->ticket_delegate_mac, delegateMac, 6);
    rtc->ticket_remaining_s = remaining_s;
    RtcState::save();

    SqLog.printf("[gateway] Delegate ticket installed: %02X:%02X:%02X:%02X:%02X:%02X (%us)\n",
        delegateMac[0], delegateMac[1], delegateMac[2],
        delegateMac[3], delegateMac[4], delegateMac[5], remaining_s);
}

void Gateway::transferTicket(const uint8_t* newGwMac) {
    if (s_ticketRemaining == 0) return;

    DelegateTicketMsg msg = {};
    msg.type = MSG_TYPE_DELEGATE_TICKET;
    memcpy(msg.delegate_mac, s_ticketMac, 6);
    msg.remaining_s = s_ticketRemaining;
    MeshConductor::sendToNode(newGwMac, &msg, sizeof(msg));

    SqLog.printf("[gateway] Delegate ticket transferred to new GW (%us remaining)\n",
        s_ticketRemaining);
}

void Gateway::clearTicket() {
    if (s_ticketRemaining == 0) return;
    SqLog.println("[gateway] Delegate ticket cleared");
    s_ticketRemaining = 0;
    memset(s_ticketMac, 0, 6);

    rtc_state_t* rtc = RtcState::get();
    memset(rtc->ticket_delegate_mac, 0, 6);
    rtc->ticket_remaining_s = 0;
    RtcState::save();
}

void Gateway::startScanDelegate(const char* ssid, const char* pass) {
    int meshNodes = esp_mesh_get_total_node_num();
    if (meshNodes <= 1) {
        // Lone gateway — self-scan
        SqLog.println("[gateway] No peers — self-scan-delegating");
        rtc_state_t* rtc = RtcState::get();
        rtc->next_role = (uint8_t)RoleId::SCAN_DELEGATE;
        strncpy(rtc->scan_ssid, ssid, 32);
        rtc->scan_ssid[32] = '\0';
        strncpy(rtc->scan_pass, pass, 64);
        rtc->scan_pass[64] = '\0';
        rtc->scan_result = 0;
        RtcState::save();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }

    // Stash creds in RTC so CRED_VERIFIED handler can recover the password
    rtc_state_t* rtc = RtcState::get();
    strncpy(rtc->scan_ssid, ssid, 32);
    rtc->scan_ssid[32] = '\0';
    strncpy(rtc->scan_pass, pass, 64);
    rtc->scan_pass[64] = '\0';
    rtc->scan_result = 0;
    RtcState::save();

    // Pick first available peer from routing table
    mesh_addr_t routing_table[MESH_MAX_NODES];
    int table_size = 0;
    esp_mesh_get_routing_table(routing_table, sizeof(routing_table), &table_size);

    uint8_t ownMac[6];
    esp_read_mac(ownMac, ESP_MAC_WIFI_STA);

    for (int i = 0; i < table_size; i++) {
        if (memcmp(routing_table[i].addr, ownMac, 6) != 0) {
            ScanDelegateMsg msg = {};
            msg.type = MSG_TYPE_SCAN_DELEGATE;
            strncpy(msg.ssid, ssid, 32);
            msg.ssid[32] = '\0';
            strncpy(msg.password, pass, 64);
            msg.password[64] = '\0';
            MeshConductor::sendToNode(routing_table[i].addr, &msg, sizeof(msg));
            SqLog.printf("[gateway] Scan delegate dispatched to %02X:%02X:%02X:%02X:%02X:%02X for \"%s\"\n",
                routing_table[i].addr[0], routing_table[i].addr[1], routing_table[i].addr[2],
                routing_table[i].addr[3], routing_table[i].addr[4], routing_table[i].addr[5], ssid);
            return;
        }
    }

    // Fallback: no reachable peers — self-scan (rtc already populated above)
    SqLog.println("[gateway] No peers in routing table — self-scan-delegating");
    rtc->next_role = (uint8_t)RoleId::SCAN_DELEGATE;
    RtcState::save();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
