#include "espnow_election.h"
#include "credential_table.h"
#include "mesh_conductor.h"
#include "bsp.hpp"
#include "nvs_config.h"
#include "power_manager.h"
#include "sq_log.h"

#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <string.h>

// --- File-scope election state ---

static ElectionCandidate s_candidates[ELECTION_MAX_CANDIDATES];
static uint8_t           s_candidateCount = 0;
static uint8_t           s_ownMac[6];
static uint16_t          s_ownTenure  = 0;
static uint8_t           s_ownTarget  = 1;       // default: ch1 (routerless)

static const uint8_t     s_broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- Forward declarations ---
static void addCandidate(const uint8_t* mac, uint16_t tenure, uint8_t channel);
static void broadcastSelf();
static void espnowRecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
static void espnowSendCb(const wifi_tx_info_t* info, esp_now_send_status_t status);
static ElectionResult resolveWinner();

// --- Phase 1: Scan & Score ---

static void scanAndScore() {
    // All-channel AP scan (~2s)
    wifi_scan_config_t scanCfg = {};
    scanCfg.show_hidden = false;
    scanCfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    scanCfg.scan_time.active.min = 120;
    scanCfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scanCfg, true);  // blocking
    if (err != ESP_OK) {
        SqLog.printf("[election] Scan failed: %s — using defaults\n", esp_err_to_name(err));
        s_ownTarget = 1;
        s_ownTenure = computeTenureScore(-128);
        return;
    }

    uint16_t apCount = 0;
    esp_wifi_scan_get_ap_num(&apCount);

    int8_t bestRssi = -128;

    if (apCount > 0) {
        uint16_t maxAps = (apCount > 30) ? 30 : apCount;
        wifi_ap_record_t* aps = (wifi_ap_record_t*)malloc(maxAps * sizeof(wifi_ap_record_t));
        if (aps) {
            esp_wifi_scan_get_ap_records(&maxAps, aps);

            ScanMatch match = CredentialTable::matchScan(aps, maxAps);
            if (match.slot >= 0) {
                bestRssi = match.rssi;
                // Find the channel of the best-matching AP
                for (uint16_t i = 0; i < maxAps; i++) {
                    if (strcmp((const char*)aps[i].ssid, match.ssid) == 0 &&
                        aps[i].rssi == match.rssi) {
                        s_ownTarget = aps[i].primary;
                        break;
                    }
                }
                SqLog.printf("[election] Router found: SSID=%s ch=%u rssi=%d\n",
                             match.ssid, s_ownTarget, bestRssi);
            }
            free(aps);
        } else {
            esp_wifi_clear_ap_list();
        }
    } else {
        esp_wifi_clear_ap_list();
    }

    // Store best RSSI for MeshConductor to use later
    MeshConductor::setBestRouterRssi(bestRssi);

    // If no known router found, routerless fallback = ch1
    if (bestRssi == -128) {
        s_ownTarget = 1;
    }

    s_ownTenure = computeTenureScore(bestRssi);
    SqLog.printf("[election] Tenure=%u target_ch=%u\n", s_ownTenure, s_ownTarget);
}

// --- Broadcast helpers ---

static void broadcastSelf() {
    // Frame: mac[6] + tenure_score[2] + target_channel[1]
    uint8_t frame[ELECTION_FRAME_SIZE];
    memcpy(&frame[0], s_ownMac, 6);
    frame[6] = (uint8_t)(s_ownTenure & 0xFF);
    frame[7] = (uint8_t)(s_ownTenure >> 8);
    frame[8] = s_ownTarget;

    // Send twice for redundancy (back-to-back is fine for ESP-NOW)
    esp_err_t e1 = esp_now_send(s_broadcastAddr, frame, ELECTION_FRAME_SIZE);
    esp_err_t e2 = esp_now_send(s_broadcastAddr, frame, ELECTION_FRAME_SIZE);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        SqLog.printf("[election] Send error: %s / %s\n",
                     esp_err_to_name(e1), esp_err_to_name(e2));
    }
}

static void espnowRecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    (void)info;
    if (len < ELECTION_FRAME_SIZE) return;

    uint8_t  mac[6];
    memcpy(mac, data, 6);
    uint16_t tenure = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    uint8_t  channel = data[8];

    // Ignore our own broadcasts (received via loopback)
    if (memcmp(mac, s_ownMac, 6) == 0) return;

    SqLog.printf("[election] Received: %02X:%02X:..:%02X tenure=%u ch=%u\n",
                 mac[0], mac[1], mac[5], tenure, channel);

    addCandidate(mac, tenure, channel);
}

static void espnowSendCb(const wifi_tx_info_t* info, esp_now_send_status_t status) {
    (void)info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        SqLog.printf("[election] ESP-NOW send FAILED (status=%d)\n", status);
    }
}

static void addCandidate(const uint8_t* mac, uint16_t tenure, uint8_t channel) {
    // Update existing candidate if same MAC
    for (uint8_t i = 0; i < s_candidateCount; i++) {
        if (memcmp(s_candidates[i].mac, mac, 6) == 0) {
            if (tenure > s_candidates[i].tenure_score) {
                s_candidates[i].tenure_score = tenure;
                s_candidates[i].target_channel = channel;
            }
            return;
        }
    }
    // New candidate
    if (s_candidateCount < ELECTION_MAX_CANDIDATES) {
        memcpy(s_candidates[s_candidateCount].mac, mac, 6);
        s_candidates[s_candidateCount].tenure_score = tenure;
        s_candidates[s_candidateCount].target_channel = channel;
        s_candidateCount++;
    }
}

// --- Resolve winner ---

static ElectionResult resolveWinner() {
    ElectionResult result = {};

    // Add self as a candidate
    addCandidate(s_ownMac, s_ownTenure, s_ownTarget);

    // Find winner: highest tenure_score, tiebreak = lowest MAC
    uint8_t winnerIdx = 0;
    for (uint8_t i = 1; i < s_candidateCount; i++) {
        bool better = false;
        if (s_candidates[i].tenure_score > s_candidates[winnerIdx].tenure_score) {
            better = true;
        } else if (s_candidates[i].tenure_score == s_candidates[winnerIdx].tenure_score) {
            if (memcmp(s_candidates[i].mac, s_candidates[winnerIdx].mac, 6) < 0) {
                better = true;
            }
        }
        if (better) winnerIdx = i;
    }

    memcpy(result.winner_mac, s_candidates[winnerIdx].mac, 6);
    result.target_channel   = s_candidates[winnerIdx].target_channel;
    result.i_am_winner      = (memcmp(s_candidates[winnerIdx].mac, s_ownMac, 6) == 0);
    result.candidate_count  = s_candidateCount;

    SqLog.printf("[election] Winner: %02X:%02X:%02X:%02X:%02X:%02X tenure=%u ch=%u (%s)\n",
        result.winner_mac[0], result.winner_mac[1], result.winner_mac[2],
        result.winner_mac[3], result.winner_mac[4], result.winner_mac[5],
        s_candidates[winnerIdx].tenure_score,
        result.target_channel,
        result.i_am_winner ? "ME" : "other");

    return result;
}

// --- Public API ---

ElectionResult EspNowElection::run() {
    SqLog.println("[election] === Starting ESP-NOW bootstrap election ===");

    // Get own MAC
    esp_read_mac(s_ownMac, ESP_MAC_WIFI_STA);

    // Reset state
    s_candidateCount = 0;
    s_ownTarget      = 1;
    s_ownTenure      = 0;
    memset(s_candidates, 0, sizeof(s_candidates));

    // --- Phase 1: Scan & Score ---
    scanAndScore();

    // --- Phase 2: ESP-NOW slotted election ---

    // Read NVS-configurable timing
    uint16_t slotMs = (uint16_t)NvsConfigManager::electionSlot_ms;
    uint16_t annMs  = (uint16_t)NvsConfigManager::electionAnnounce_ms;
    if (slotMs < 5)  slotMs = 5;    // sanity floor
    if (annMs  < 100) annMs = 100;

    // Compute broadcast slot from MAC LSB: MAC(6)[0] * slotMs + jitter[0..slotMs-1]
    uint8_t macLsb = s_ownMac[5];   // last byte of 6-byte MAC array = LSB
    uint32_t slotBase  = (uint32_t)macLsb * slotMs;
    uint32_t jitter    = esp_random() % slotMs;
    uint32_t mySlotMs  = slotBase + jitter;
    uint32_t windowMs  = 256u * slotMs;        // total broadcast window
    uint32_t totalMs   = windowMs + annMs;      // full election duration
    uint32_t hardLimit = totalMs + 1000;        // safety ceiling

    SqLog.printf("[election] Slot: LSB=0x%02X base=%ums jitter=%ums (window=%ums, announce=%ums)\n",
                 macLsb, slotBase, jitter, windowMs, annMs);

    // Ensure scan resources are fully released before changing channel
    esp_wifi_clear_ap_list();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Lock WiFi to channel 1 for election — verify it actually took
    esp_err_t chErr = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    uint8_t actualCh = 0;
    wifi_second_chan_t secCh;
    esp_wifi_get_channel(&actualCh, &secCh);
    if (chErr != ESP_OK || actualCh != 1) {
        SqLog.printf("[election] Channel set problem: err=%s actual_ch=%u — retrying\n",
                     esp_err_to_name(chErr), actualCh);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        esp_wifi_get_channel(&actualCh, &secCh);
    }
    SqLog.printf("[election] WiFi on ch%u for election\n", actualCh);

    // Init ESP-NOW
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        SqLog.printf("[election] esp_now_init failed: %s — self-promoting\n",
                     esp_err_to_name(err));
        addCandidate(s_ownMac, s_ownTenure, s_ownTarget);
        return resolveWinner();
    }

    // Register broadcast peer (FF:FF:FF:FF:FF:FF)
    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 1;       // must match WiFi channel set above
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        SqLog.printf("[election] esp_now_add_peer failed: %s\n", esp_err_to_name(err));
    }
    if (!esp_now_is_peer_exist(s_broadcastAddr)) {
        SqLog.println("[election] WARNING: broadcast peer not found after add!");
    }

    // Register callbacks
    esp_now_register_recv_cb(espnowRecvCb);
    esp_now_register_send_cb(espnowSendCb);

    // --- Phase 2A: Slotted candidate broadcast ---
    // Wait for our deterministic slot, then broadcast once.
    // All nodes listen for the entire window.
    uint32_t t0 = (uint32_t)millis();

    // Sleep until our slot
    if (mySlotMs > 0) {
        vTaskDelay(pdMS_TO_TICKS(mySlotMs));
    }

    broadcastSelf();
    SqLog.printf("[election] Broadcast at slot %ums (tenure=%u, ch=%u)\n",
                 mySlotMs, s_ownTenure, s_ownTarget);

    // Wait for the rest of the broadcast window
    uint32_t elapsed = (uint32_t)millis() - t0;
    if (elapsed < windowMs) {
        vTaskDelay(pdMS_TO_TICKS(windowMs - elapsed));
    }

    // --- Phase 2B: Winner announcement ---
    // Resolve winner from candidates heard so far, then the winner
    // hammers its announcement 10× during the announce window.
    // Non-winners listen — if they hear a better candidate they
    // update their candidate table and re-resolve at the end.
    ElectionResult preliminary = resolveWinner();
    SqLog.printf("[election] Preliminary: %s — entering announcement phase (%ums)\n",
                 preliminary.i_am_winner ? "I won" : "deferred", annMs);

    uint32_t annStart = (uint32_t)millis();
    if (preliminary.i_am_winner) {
        // Announce 10 times, evenly spaced across the announce window
        uint32_t spacing = annMs / 10;
        for (int i = 0; i < 10; i++) {
            broadcastSelf();
            uint32_t annElapsed = (uint32_t)millis() - annStart;
            uint32_t nextAt = (uint32_t)(i + 1) * spacing;
            if (annElapsed < nextAt && i < 9) {
                vTaskDelay(pdMS_TO_TICKS(nextAt - annElapsed));
            }
        }
        SqLog.println("[election] Winner announcements sent (10x)");
    } else {
        // Non-winner: just listen for the duration
        vTaskDelay(pdMS_TO_TICKS(annMs));
    }

    // --- Cleanup ---
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    SqLog.println("[election] ESP-NOW torn down");

    // --- Final resolution (may differ from preliminary if announcement was heard) ---
    return resolveWinner();
}
