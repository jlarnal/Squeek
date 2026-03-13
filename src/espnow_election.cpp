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
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <string.h>

// --- File-scope election state ---

static ElectionCandidate s_candidates[ELECTION_MAX_CANDIDATES];
static uint8_t           s_candidateCount = 0;
static uint8_t           s_ownMac[6];
static uint16_t          s_ownTenure  = 0;
static uint8_t           s_ownTarget  = 1;       // default: ch1 (routerless)
static bool              s_hasBroadcast = false;  // true after first broadcast sent
static SemaphoreHandle_t s_doneSema   = nullptr;  // signalled when silence timer expires

static TimerHandle_t     s_broadcastTimer = nullptr;
static TimerHandle_t     s_silenceTimer   = nullptr;

// --- Forward declarations ---
static void addCandidate(const uint8_t* mac, uint16_t tenure, uint8_t channel);
static void broadcastSelf();
static void broadcastTimerCb(TimerHandle_t timer);
static void silenceTimerCb(TimerHandle_t timer);
static void espnowRecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len);
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

// --- Phase 2: ESP-NOW election ---

static void broadcastSelf() {
    // Frame: mac[6] + tenure_score[2] + target_channel[1]
    uint8_t frame[ELECTION_FRAME_SIZE];
    memcpy(&frame[0], s_ownMac, 6);
    frame[6] = (uint8_t)(s_ownTenure & 0xFF);
    frame[7] = (uint8_t)(s_ownTenure >> 8);
    frame[8] = s_ownTarget;

    // Send twice for redundancy
    esp_now_send(NULL, frame, ELECTION_FRAME_SIZE);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_now_send(NULL, frame, ELECTION_FRAME_SIZE);

    s_hasBroadcast = true;
    SqLog.printf("[election] Broadcast sent (tenure=%u, ch=%u)\n", s_ownTenure, s_ownTarget);
}

static void broadcastTimerCb(TimerHandle_t timer) {
    (void)timer;
    broadcastSelf();
    // Reset silence timer (we just made noise)
    if (s_silenceTimer) {
        xTimerReset(s_silenceTimer, 0);
    }
}

static void silenceTimerCb(TimerHandle_t timer) {
    (void)timer;
    SqLog.println("[election] Silence timer expired — election complete");
    if (s_doneSema) {
        xSemaphoreGive(s_doneSema);
    }
}

static void espnowRecvCb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
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

    // Reset silence timer (we heard someone)
    if (s_silenceTimer) {
        xTimerReset(s_silenceTimer, 0);
    }

    // If we haven't broadcast yet, restart random timer (defer to avoid collision)
    if (!s_hasBroadcast && s_broadcastTimer) {
        uint32_t newDelay = esp_random() % ELECTION_BCAST_MAX_MS;
        if (newDelay < 100) newDelay = 100;  // floor at 100ms
        xTimerChangePeriod(s_broadcastTimer, pdMS_TO_TICKS(newDelay), 0);
    }
    // If we already broadcast, don't touch broadcast timer
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

// --- Phase 3: Resolve winner ---

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
    s_hasBroadcast   = false;
    memset(s_candidates, 0, sizeof(s_candidates));

    // --- Phase 1: Scan & Score ---
    scanAndScore();

    // --- Phase 2: ESP-NOW election ---

    // Lock WiFi to channel 1 for election
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    SqLog.println("[election] WiFi locked to ch1 for election");

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
    peer.channel = 1;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    // Register receive callback
    esp_now_register_recv_cb(espnowRecvCb);

    // Create semaphore for "election done" signal
    s_doneSema = xSemaphoreCreateBinary();

    // Arm random broadcast timer
    uint32_t bcastDelay = esp_random() % ELECTION_BCAST_MAX_MS;
    if (bcastDelay < 100) bcastDelay = 100;  // floor
    s_broadcastTimer = xTimerCreate("elBcast", pdMS_TO_TICKS(bcastDelay),
                                     pdFALSE, nullptr, broadcastTimerCb);

    // Arm silence timer (3500ms)
    s_silenceTimer = xTimerCreate("elSilence", pdMS_TO_TICKS(ELECTION_SILENCE_MS),
                                   pdFALSE, nullptr, silenceTimerCb);

    // Start both timers
    xTimerStart(s_broadcastTimer, 0);
    xTimerStart(s_silenceTimer, 0);

    SqLog.printf("[election] Timers armed: broadcast=%lums, silence=%ums\n",
                 bcastDelay, ELECTION_SILENCE_MS);

    // Block until silence timer expires (max ~6.5s: 3s broadcast + 3.5s silence)
    // Hard ceiling of 15s to prevent infinite hang
    if (xSemaphoreTake(s_doneSema, pdMS_TO_TICKS(15000)) == pdFALSE) {
        SqLog.println("[election] Hard timeout — forcing election end");
    }

    // --- Cleanup ---
    if (s_broadcastTimer) {
        xTimerStop(s_broadcastTimer, 0);
        xTimerDelete(s_broadcastTimer, 0);
        s_broadcastTimer = nullptr;
    }
    if (s_silenceTimer) {
        xTimerStop(s_silenceTimer, 0);
        xTimerDelete(s_silenceTimer, 0);
        s_silenceTimer = nullptr;
    }
    vSemaphoreDelete(s_doneSema);
    s_doneSema = nullptr;

    esp_now_unregister_recv_cb();
    esp_now_deinit();
    SqLog.println("[election] ESP-NOW torn down");

    // --- Phase 3: Resolve winner ---
    return resolveWinner();
}
