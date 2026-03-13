# ESP-NOW Bootstrap Election — Implementation Plan

## Goal

Replace the MAC-based bootstrap timer in `MeshConductor::start()` with a pre-mesh ESP-NOW election phase. All nodes broadcast their tenure score on channel 1 before `esp_mesh_start()`. The highest-scoring node becomes root, and all nodes converge on the winner's target channel from the start — eliminating split-brain and the 60-120s credentialed bootstrap wait.

## Architecture

```
  boot
   │
   ├── delegate_active / next_role set?  ──YES──► skip election, existing flow
   │
   NO
   │
   ▼
  MeshConductor::init()          ← WiFi STA already started here
   │
   ▼
  EspNowElection::run()          ← NEW: blocking call (~5-14s)
   │  1. All-channel AP scan (~2s)
   │  2. Compute tenure score + target_channel
   │  3. Lock to ch1, esp_now_init()
   │  4. Random broadcast timer (0-3s) + 3.5s silence timer
   │  5. Collect candidates
   │  6. esp_now_deinit()
   │  7. Return ElectionResult { winner_mac, target_channel, i_am_winner }
   │
   ▼
  MeshConductor::start()         ← MODIFIED: use ElectionResult
   │  - cfg.channel = result.target_channel
   │  - if i_am_winner: esp_mesh_set_type(MESH_ROOT)
   │  - NO bootstrap timer
   │
   ▼
  esp_mesh_start()
```

## Tech Stack

- **Platform:** ESP32-C6 SuperMini, PlatformIO (Arduino + ESP-IDF dual framework)
- **Language:** C++ (static class pattern, file-scope state)
- **Key APIs:** `esp_now_init/deinit`, `esp_now_send`, `esp_now_register_recv_cb`, `esp_now_add_peer`, `esp_wifi_scan_start`, `esp_wifi_set_channel`, FreeRTOS timers
- **Build:** `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

---

## File Structure

| File | Action | Purpose |
|------|--------|---------|
| `include/espnow_election.h` | **CREATE** | Public API: `ElectionResult` struct, `EspNowElection` static class |
| `src/espnow_election.cpp` | **CREATE** | Election logic: scan, ESP-NOW send/recv, timers, candidate tracking |
| `src/mesh_conductor.cpp` | **MODIFY** | Call election before `esp_mesh_start()`, apply result, remove old bootstrap timer |
| `include/mesh_conductor.h` | **MODIFY** | Remove `computeTenureScore` declaration (moves to espnow_election.h) |
| `src/CMakeLists.txt` | **MODIFY** | Add `espnow_election.cpp` |
| `src/main.cpp` | **MODIFY** | Pass election skip flag for delegate/fast-path boot |

---

## Task 1: Create `include/espnow_election.h`

**File:** `G:\sources\Esp32\Squeek\include\espnow_election.h`

This header defines the public API. The election module is a static class (matching the codebase pattern).

```cpp
#ifndef ESPNOW_ELECTION_H
#define ESPNOW_ELECTION_H

#include <stdint.h>
#include <stdbool.h>

#define ELECTION_MAX_CANDIDATES  16   // == MESH_MAX_NODES
#define ELECTION_FRAME_SIZE       9   // mac[6] + tenure_score[2] + target_channel[1]
#define ELECTION_SILENCE_MS    3500   // silence timer duration
#define ELECTION_BCAST_MAX_MS  3000   // random broadcast delay max

struct ElectionCandidate {
    uint8_t  mac[6];
    uint16_t tenure_score;
    uint8_t  target_channel;
};

struct ElectionResult {
    uint8_t  winner_mac[6];
    uint8_t  target_channel;      // channel the mesh should use
    bool     i_am_winner;         // true if this node won
    uint8_t  candidate_count;     // how many nodes participated
};

class EspNowElection {
public:
    /// Run the full election sequence (blocking, ~5-14s).
    /// Call AFTER MeshConductor::init() (WiFi STA is up) and BEFORE esp_mesh_start().
    /// @return election result with winner info and target channel
    static ElectionResult run();

private:
    EspNowElection() = delete;
};

#endif // ESPNOW_ELECTION_H
```

**Build verification:** `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`
**Commit:** `feat(election): add EspNowElection header with ElectionResult struct`

---

## Task 2: Create `src/espnow_election.cpp` — scaffold + scan phase

**File:** `G:\sources\Esp32\Squeek\src\espnow_election.cpp`

This task implements the scan phase (Phase 1 from the spec) and stubs out the ESP-NOW phase. The file compiles but the election just returns "I win" until Task 3 wires up ESP-NOW.

**Also update:** `G:\sources\Esp32\Squeek\src\CMakeLists.txt` — add `"espnow_election.cpp"` to the SRCS list.

```cpp
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

static const char* TAG = "election";

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

// --- Scan + score (Phase 1 from spec) ---

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

// --- ESP-NOW election (Phase 2 from spec) ---

static void broadcastSelf() {
    // Frame: mac[6] + tenure_score[2] + target_channel[1]
    uint8_t frame[ELECTION_FRAME_SIZE];
    memcpy(&frame[0], s_ownMac, 6);
    frame[6] = (uint8_t)(s_ownTenure & 0xFF);
    frame[7] = (uint8_t)(s_ownTenure >> 8);
    frame[8] = s_ownTarget;

    // Send twice for redundancy (as specified)
    esp_now_send(NULL, frame, ELECTION_FRAME_SIZE);  // NULL = broadcast peer
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
    // If we already broadcast, don't touch broadcast timer (spec rule)
}

static void addCandidate(const uint8_t* mac, uint16_t tenure, uint8_t channel) {
    // Update existing candidate if same MAC
    for (uint8_t i = 0; i < s_candidateCount; i++) {
        if (memcmp(s_candidates[i].mac, mac, 6) == 0) {
            // Take the higher tenure (in case of duplicate frames)
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

// --- Phase 3: resolve winner ---

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
            // Tiebreak: lower MAC wins
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
        // Fallback: act as lone node
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
    // Add a hard ceiling of 15s to prevent infinite hang
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
```

**CMakeLists.txt change** — add `"espnow_election.cpp"` after `"mesh_delegate.cpp"`:

```
    "mesh_delegate.cpp"
    "espnow_election.cpp"
    "stealth_manager.cpp"
```

**Build verification:** `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`
**Commit:** `feat(election): implement EspNowElection scan, broadcast, and candidate tracking`

---

## Task 3: Integrate election into `MeshConductor::start()` and remove old bootstrap timer

**File:** `G:\sources\Esp32\Squeek\src\mesh_conductor.cpp`

This task modifies `start()` to call `EspNowElection::run()` before `esp_mesh_start()`, and removes all old bootstrap timer code.

### 3a. Add include

At the top of `mesh_conductor.cpp`, add:

```cpp
#include "espnow_election.h"
```

### 3b. Remove old bootstrap timer code

Delete these functions entirely (lines 56-138 in current file):
- `s_bootstrapTimer` variable declaration (line 56)
- `forward decl for bootstrapTimerCb` (line 58)
- `bootstrapTimerCb()` function (lines 77-94)
- `macBasedBootstrapDelay()` function (lines 100-117)
- `scheduleBootstrapTimer()` function (lines 125-138)

### 3c. Remove bootstrap timer references from event handler

In `meshEventHandler`, remove the bootstrap timer cancellation code:

**In `MESH_EVENT_PARENT_CONNECTED` (around lines 746-750):**
Remove:
```cpp
        if (s_bootstrapTimer) {
            xTimerStop(s_bootstrapTimer, 0);
            xTimerDelete(s_bootstrapTimer, 0);
            s_bootstrapTimer = nullptr;
        }
```

**In `MESH_EVENT_NO_PARENT_FOUND` (around line 860):**
Remove the comment: `// Bootstrap timer is already running from start() — nothing to schedule here.`

**In `MESH_EVENT_FIND_NETWORK` (around lines 895-900):**
Remove:
```cpp
        if (s_bootstrapTimer) {
            SqLog.println("[mesh] Cancelling bootstrap — found existing network");
            xTimerStop(s_bootstrapTimer, 0);
            xTimerDelete(s_bootstrapTimer, 0);
            s_bootstrapTimer = nullptr;
        }
```

### 3d. Remove bootstrap timer from `onBootButton()`

In `MeshConductor::onBootButton()` (around lines 1050-1054), remove:
```cpp
        if (s_bootstrapTimer) {
            xTimerStop(s_bootstrapTimer, 0);
            xTimerDelete(s_bootstrapTimer, 0);
            s_bootstrapTimer = nullptr;
        }
```

The BOOT button force-root path still works — it just calls `esp_mesh_set_type(MESH_ROOT)` directly, which is fine post-election.

### 3e. Remove bootstrap timer from `MeshConductor::stop()`

In `MeshConductor::stop()` (around lines 1069-1073), remove:
```cpp
    if (s_bootstrapTimer) {
        xTimerStop(s_bootstrapTimer, 0);
        xTimerDelete(s_bootstrapTimer, 0);
        s_bootstrapTimer = nullptr;
    }
```

### 3f. Rewrite `MeshConductor::start()` to use election

Replace the current `start()` function (lines 965-1040) with the new version that calls the election:

```cpp
void MeshConductor::start() {
    static bool s_meshStarting = false;
    if (s_started || s_meshStarting) {
        Serial.println("[mesh] Already started, ignoring duplicate start()");
        return;
    }
    s_meshStarting = true;

    // --- Load credentials ---
    char ssid[33] = {}, pass[65] = {};
    bool credsInNvs = SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass));
    bool suppressCreds = RtcState::isValid() && RtcState::get()->delegate_active;
    s_hasRouterCreds = credsInNvs && !suppressCreds;

    // --- Determine if election should run ---
    bool isDelegateReturn = RtcState::isValid() && RtcState::get()->delegate_active;
    bool skipElection = isDelegateReturn || (s_role != nullptr);  // pre-assigned role = skip

    // --- Run ESP-NOW election (or skip) ---
    ElectionResult election = {};
    if (!skipElection) {
        election = EspNowElection::run();
    } else {
        SqLog.println("[mesh] Skipping election (delegate return or pre-assigned role)");
        // Default: no winner info, use legacy channel logic
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

    // Channel selection:
    //  - Election ran: use winner's target_channel
    //  - Delegate return (skipped election): channel=0 (scan all)
    //  - Pre-assigned role (skipped election): channel=0 (scan all)
    if (!skipElection && election.candidate_count > 0) {
        cfg.channel = election.target_channel;
        SqLog.printf("[mesh] Channel from election: %u\n", cfg.channel);
    } else if (isDelegateReturn) {
        cfg.channel = 0;
    } else if (s_hasRouterCreds) {
        cfg.channel = 0;  // scan for router
    } else {
        cfg.channel = 1;  // routerless fallback (shouldn't happen — election ran)
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

    // Apply election result: winner becomes root
    if (!skipElection && election.i_am_winner) {
        SqLog.println("[mesh] Election winner — setting MESH_ROOT before start");
        esp_mesh_set_type(MESH_ROOT);
    }

    // Reset state
    s_roleAssigned = (s_role != nullptr);
    s_parentRetries = 0;

    ESP_ERROR_CHECK(esp_mesh_start());
    SqLog.println("[mesh] Mesh starting...");

    // Suppress noisy ESP-MESH internal logs when routerless
    if (!s_hasRouterCreds) {
        esp_log_level_set("mesh", ESP_LOG_WARN);
        esp_log_level_set("wifi", ESP_LOG_WARN);
    }

    // If we are the winner and routerless, assign role immediately
    // (PARENT_CONNECTED won't fire for routerless root)
    if (!skipElection && election.i_am_winner && !s_hasRouterCreds) {
        if (!s_roleAssigned) {
            assignRoleFromMeshState();
        }
    }
}
```

### 3g. Keep `computeTenureScore()` accessible

The `computeTenureScore()` function stays in `mesh_conductor.cpp` and is declared in `mesh_conductor.h` (line 190: `uint16_t computeTenureScore(int8_t best_rssi_dBm);`). The election module calls it via the existing declaration. **No change needed here.**

**Build verification:** `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`
**Commit:** `feat(election): integrate ESP-NOW election into MeshConductor, remove bootstrap timer`

---

## Task 4: Handle edge case — election winner with router creds in routerless-only mesh

**File:** `G:\sources\Esp32\Squeek\src\mesh_conductor.cpp`

When the winner has router creds (target_channel != 1) but is the only node, `PARENT_CONNECTED` fires when the router responds. The existing `assignRoleFromMeshState()` path handles this. No code change needed — just verify the flow works.

However, when the winner has router creds and **other nodes exist** (mixed fleet), the losers need to be on the winner's target channel. The election already sets `cfg.channel = winner.target_channel` for all nodes, so they will all scan on the router's channel. This is already handled by the Task 3 `start()` rewrite.

**Verification:** Confirm by reading the flow:
1. Election runs on ch1 (all nodes hear each other).
2. Winner's target_channel (e.g., ch6 for a known router) is communicated to all.
3. All nodes set `cfg.channel = 6` before `esp_mesh_start()`.
4. The winner (root) connects to the router; losers join the mesh tree.

No code change. Just a flow verification note.

**Commit:** (no commit — this is a verification checkpoint)

---

## Task 5: Verify `MESH_EVENT_NO_PARENT_FOUND` behavior post-election

**File:** `G:\sources\Esp32\Squeek\src\mesh_conductor.cpp`

After the election, the bootstrap timer no longer exists. But `NO_PARENT_FOUND` can still fire for credentialed roots that can't reach their router. The existing handler (lines 856-868) already reboots after `MESH_MAX_RETRIES` — this is correct behavior post-election. No change needed.

For routerless meshes, `NO_PARENT_FOUND` fires for non-root nodes that haven't found the elected root yet. ESP-MESH's self-organized mode handles retries. No change needed.

**Commit:** (no commit — this is a verification checkpoint)

---

## Task 6: Clean up dead code and update comments

**File:** `G:\sources\Esp32\Squeek\src\mesh_conductor.cpp`

### 6a. Remove stale comments

Update the comment block around `MESH_EVENT_NO_PARENT_FOUND`:

```cpp
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
```

### 6b. Update file header comments

Remove references to "bootstrap timer" and "MAC-based bootstrap delay" from any code comments in `mesh_conductor.cpp`. Replace with references to ESP-NOW election.

**Build verification:** `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`
**Commit:** `refactor(election): remove dead bootstrap timer code and update comments`

---

## Summary of all changes

| # | File | What changes |
|---|------|-------------|
| 1 | `include/espnow_election.h` | **NEW** — `ElectionCandidate`, `ElectionResult`, `EspNowElection::run()` |
| 2 | `src/espnow_election.cpp` | **NEW** — scan phase, ESP-NOW broadcast/recv, timers, winner resolution |
| 3 | `src/CMakeLists.txt` | Add `"espnow_election.cpp"` to SRCS |
| 4 | `src/mesh_conductor.cpp` | Remove `s_bootstrapTimer`, `bootstrapTimerCb`, `macBasedBootstrapDelay`, `scheduleBootstrapTimer`; rewrite `start()` to call `EspNowElection::run()`; clean up event handler timer references; add `#include "espnow_election.h"` |

Files NOT changed:
- `include/mesh_conductor.h` — `computeTenureScore()` declaration stays (used by both modules)
- `src/main.cpp` — boot flow unchanged (delegate/fast-path still sets `next_role` in RTC; `start()` internally decides whether to run election)
- `include/credential_table.h` / `src/credential_table.cpp` — unchanged, `matchScan()` reused as-is

## Commit sequence

1. `feat(election): add EspNowElection header with ElectionResult struct`
2. `feat(election): implement EspNowElection scan, broadcast, and candidate tracking`
3. `feat(election): integrate ESP-NOW election into MeshConductor, remove bootstrap timer`
4. `refactor(election): remove dead bootstrap timer code and update comments`

Each commit compiles cleanly. No commit depends on untested runtime behavior — all are structural code changes verified by build.
