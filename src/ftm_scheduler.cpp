#include "ftm_scheduler.h"
#include "peer_table.h"
#include "mesh_conductor.h"
#include "ftm_manager.h"
#include "position_solver.h"
#include "nvs_config.h"
#include "bsp.hpp"
#include <Arduino.h>
#include <esp_mac.h>
#include <string.h>

static const char* TAG = "ftmsched";

// --- File-scope state ---

#define FTM_QUEUE_MAX  ((MESH_MAX_NODES * (MESH_MAX_NODES - 1)) / 2)

static FtmQueueItem   s_queue[FTM_QUEUE_MAX];
static uint8_t        s_queueHead = 0;
static uint8_t        s_queueTail = 0;
static uint8_t        s_queueCount = 0;

static FtmPairState   s_pairState = FTM_PAIR_IDLE;
static uint8_t        s_currentA = 0;
static uint8_t        s_currentB = 0;
static bool           s_readyA = false;
static bool           s_readyB = false;
static uint32_t       s_pairStartMs = 0;

static TimerHandle_t  s_processTimer = nullptr;
static TimerHandle_t  s_sweepTimer   = nullptr;
static bool           s_active       = false;

// Edge staleness tracking: timestamp of last measurement per pair
static uint32_t       s_lastMeasured[MESH_MAX_NODES][MESH_MAX_NODES];

// --- Queue helpers ---

static bool queuePush(const FtmQueueItem& item) {
    if (s_queueCount >= FTM_QUEUE_MAX) return false;

    // Insert sorted by priority (lower = higher priority)
    // Simple insertion into array — fine for 120 max items
    uint8_t insertAt = s_queueCount;
    for (uint8_t i = 0; i < s_queueCount; i++) {
        uint8_t idx = (s_queueHead + i) % FTM_QUEUE_MAX;
        if (item.priority < s_queue[idx].priority) {
            insertAt = i;
            break;
        }
    }

    // Shift items to make room
    for (uint8_t i = s_queueCount; i > insertAt; i--) {
        uint8_t dst = (s_queueHead + i) % FTM_QUEUE_MAX;
        uint8_t src = (s_queueHead + i - 1) % FTM_QUEUE_MAX;
        s_queue[dst] = s_queue[src];
    }
    uint8_t pos = (s_queueHead + insertAt) % FTM_QUEUE_MAX;
    s_queue[pos] = item;
    s_queueCount++;
    return true;
}

static bool queuePop(FtmQueueItem* out) {
    if (s_queueCount == 0) return false;
    *out = s_queue[s_queueHead];
    s_queueHead = (s_queueHead + 1) % FTM_QUEUE_MAX;
    s_queueCount--;
    return true;
}

static bool isDuplicatePair(uint8_t a, uint8_t b) {
    for (uint8_t i = 0; i < s_queueCount; i++) {
        uint8_t idx = (s_queueHead + i) % FTM_QUEUE_MAX;
        if ((s_queue[idx].nodeA_idx == a && s_queue[idx].nodeB_idx == b) ||
            (s_queue[idx].nodeA_idx == b && s_queue[idx].nodeB_idx == a)) {
            return true;
        }
    }
    return false;
}

// --- Pair state machine ---

static void startNextPair();

static void sendWakeMessages(uint8_t idxA, uint8_t idxB) {
    PeerEntry* entA = PeerTable::getEntryByIndex(idxA);
    PeerEntry* entB = PeerTable::getEntryByIndex(idxB);
    if (!entA || !entB) return;

    FtmWakeMsg wake;
    wake.type = MSG_TYPE_FTM_WAKE;
    memcpy(wake.initiator, entA->mac, 6);
    memcpy(wake.responder, entB->mac, 6);
    memcpy(wake.responder_ap, entB->softap_mac, 6);

    // Send to both nodes (or handle locally if one is gateway)
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    if (memcmp(entA->mac, own_mac, 6) == 0) {
        // Gateway is initiator — mark self as ready
        s_readyA = true;
    } else {
        MeshConductor::sendToNode(entA->mac, &wake, sizeof(wake));
    }

    if (memcmp(entB->mac, own_mac, 6) == 0) {
        // Gateway is responder — mark self as ready (passive)
        s_readyB = true;
    } else {
        MeshConductor::sendToNode(entB->mac, &wake, sizeof(wake));
    }
}

static void sendGoMessage(uint8_t initiatorIdx, const uint8_t* responder_ap_mac) {
    PeerEntry* initiator = PeerTable::getEntryByIndex(initiatorIdx);
    if (!initiator) return;

    FtmGoMsg go;
    go.type = MSG_TYPE_FTM_GO;
    memcpy(go.target_ap, responder_ap_mac, 6);
    go.samples = (uint8_t)(uint32_t)NvsConfigManager::ftmSamplesPerPair;

    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    if (memcmp(initiator->mac, own_mac, 6) == 0) {
        // Gateway is the initiator — invoke FTM directly
        FtmManager::onFtmGo(go.target_ap, go.samples);
    } else {
        MeshConductor::sendToNode(initiator->mac, &go, sizeof(go));
    }
}

static void processTimerCb(TimerHandle_t t) {
    (void)t;

    uint32_t timeout = (uint32_t)NvsConfigManager::ftmPairTimeout_ms;

    switch (s_pairState) {
    case FTM_PAIR_IDLE:
        startNextPair();
        break;

    case FTM_PAIR_WAITING_READY:
        if (s_readyA && s_readyB) {
            // Both ready — send GO to initiator
            PeerEntry* responder = PeerTable::getEntryByIndex(s_currentB);
            if (responder) {
                s_pairState = FTM_PAIR_GO_SENT;
                sendGoMessage(s_currentA, responder->softap_mac);
                s_pairState = FTM_PAIR_WAITING_RESULT;
            }
        } else if ((millis() - s_pairStartMs) > timeout) {
            Serial.printf("[ftmsched] Pair (%u,%u) timed out waiting for READY\n",
                s_currentA, s_currentB);
            s_pairState = FTM_PAIR_IDLE;
        }
        break;

    case FTM_PAIR_WAITING_RESULT:
        if ((millis() - s_pairStartMs) > timeout * 2) {
            Serial.printf("[ftmsched] Pair (%u,%u) timed out waiting for RESULT\n",
                s_currentA, s_currentB);
            s_pairState = FTM_PAIR_IDLE;
        }
        break;

    default:
        break;
    }
}

static void startNextPair() {
    FtmQueueItem item;
    while (queuePop(&item)) {
        // Verify both peers are still alive
        PeerEntry* a = PeerTable::getEntryByIndex(item.nodeA_idx);
        PeerEntry* b = PeerTable::getEntryByIndex(item.nodeB_idx);
        if (!a || !b) continue;
        if ((a->flags & PEER_STATUS_DEAD) || (b->flags & PEER_STATUS_DEAD)) continue;

        s_currentA = item.nodeA_idx;
        s_currentB = item.nodeB_idx;
        s_readyA = false;
        s_readyB = false;
        s_pairStartMs = millis();
        s_pairState = FTM_PAIR_WAITING_READY;

        Serial.printf("[ftmsched] Starting pair (%u,%u) prio=%u\n",
            s_currentA, s_currentB, item.priority);

        sendWakeMessages(s_currentA, s_currentB);
        return;
    }

    // Queue empty — measurements complete
    if (s_active) {
        Serial.println("[ftmsched] All pairs measured, triggering solve");
        s_active = false;
        FtmScheduler::triggerSolve();
    }
}

static void sweepTimerCb(TimerHandle_t t) {
    (void)t;

    // Check for stale edges and re-queue them
    uint32_t stale_s = (uint32_t)NvsConfigManager::ftmStaleness_s;
    uint32_t now = millis();
    uint8_t count = PeerTable::peerCount();

    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            uint32_t age_ms = now - s_lastMeasured[i][j];
            if (age_ms > stale_s * 1000) {
                FtmScheduler::enqueuePair(i, j, FTM_PRIO_STALE);
            }
        }
    }

    // Also do full sweep if configured
    uint32_t sweep_s = (uint32_t)NvsConfigManager::ftmSweepInterval_s;
    if (sweep_s > 0) {
        // Full sweep is already handled by stale edge detection
        // Only force if all edges somehow stayed fresh
    }
}

// --- Public API ---

void FtmScheduler::init() {
    s_queueHead = 0;
    s_queueTail = 0;
    s_queueCount = 0;
    s_pairState = FTM_PAIR_IDLE;
    s_active = false;
    memset(s_lastMeasured, 0, sizeof(s_lastMeasured));

    // Process timer: checks pair state machine every 500ms
    if (s_processTimer == nullptr) {
        s_processTimer = xTimerCreate("ftmProc", pdMS_TO_TICKS(500),
                                       pdTRUE, nullptr, processTimerCb);
    }
    xTimerStart(s_processTimer, 0);

    // Sweep timer: checks staleness every ftmSweepInterval_s
    uint32_t sweep_s = (uint32_t)NvsConfigManager::ftmSweepInterval_s;
    if (sweep_s > 0) {
        if (s_sweepTimer == nullptr) {
            s_sweepTimer = xTimerCreate("ftmSwp", pdMS_TO_TICKS(sweep_s * 1000),
                                         pdTRUE, nullptr, sweepTimerCb);
        }
        xTimerStart(s_sweepTimer, 0);
    }

    Serial.println("[ftmsched] Initialized");
}

void FtmScheduler::shutdown() {
    if (s_processTimer) xTimerStop(s_processTimer, 0);
    if (s_sweepTimer) xTimerStop(s_sweepTimer, 0);
    s_active = false;
    s_queueCount = 0;
    s_pairState = FTM_PAIR_IDLE;
    Serial.println("[ftmsched] Shutdown");
}

void FtmScheduler::enqueuePair(uint8_t nodeA_idx, uint8_t nodeB_idx, FtmPriority prio) {
    if (nodeA_idx == nodeB_idx) return;
    if (isDuplicatePair(nodeA_idx, nodeB_idx)) return;

    FtmQueueItem item;
    item.nodeA_idx = nodeA_idx;
    item.nodeB_idx = nodeB_idx;
    item.priority = prio;
    item.queued_ms = millis();

    if (queuePush(item)) {
        s_active = true;
    }
}

void FtmScheduler::enqueueFullSweep() {
    uint8_t count = PeerTable::peerCount();
    Serial.printf("[ftmsched] Full sweep: %u nodes, %u pairs\n",
        count, (count * (count - 1)) / 2);

    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            enqueuePair(i, j, FTM_PRIO_SWEEP);
        }
    }
}

void FtmScheduler::enqueueNewNode(uint8_t node_idx) {
    uint8_t anchors = (uint8_t)(uint32_t)NvsConfigManager::ftmNewNodeAnchors;
    uint8_t count = PeerTable::peerCount();
    uint8_t queued = 0;

    for (uint8_t i = 0; i < count && queued < anchors; i++) {
        if (i == node_idx) continue;
        PeerEntry* e = PeerTable::getEntryByIndex(i);
        if (!e || (e->flags & PEER_STATUS_DEAD)) continue;
        enqueuePair(node_idx, i, FTM_PRIO_NEW_NODE);
        queued++;
    }

    Serial.printf("[ftmsched] Queued %u anchor pairs for new node %u\n", queued, node_idx);
}

void FtmScheduler::onFtmReady(const uint8_t* mac) {
    if (s_pairState != FTM_PAIR_WAITING_READY) return;

    PeerEntry* a = PeerTable::getEntryByIndex(s_currentA);
    PeerEntry* b = PeerTable::getEntryByIndex(s_currentB);
    if (!a || !b) return;

    if (memcmp(mac, a->mac, 6) == 0) {
        s_readyA = true;
        Serial.printf("[ftmsched] Node A (slot %u) ready\n", s_currentA);
    }
    if (memcmp(mac, b->mac, 6) == 0) {
        s_readyB = true;
        Serial.printf("[ftmsched] Node B (slot %u) ready\n", s_currentB);
    }

    // Check if both ready — immediate transition
    if (s_readyA && s_readyB) {
        PeerEntry* responder = PeerTable::getEntryByIndex(s_currentB);
        if (responder) {
            s_pairState = FTM_PAIR_GO_SENT;
            sendGoMessage(s_currentA, responder->softap_mac);
            s_pairState = FTM_PAIR_WAITING_RESULT;
        }
    }
}

void FtmScheduler::onFtmResult(const uint8_t* initiator, const uint8_t* responder,
                                 float distance_cm, uint8_t status) {
    if (s_pairState != FTM_PAIR_WAITING_RESULT) {
        Serial.println("[ftmsched] Unexpected FTM result (not waiting)");
        return;
    }

    if (status == 0 && distance_cm >= 0) {
        // Store distance in peer table
        PeerTable::setDistance(s_currentA, s_currentB, distance_cm);
        s_lastMeasured[s_currentA][s_currentB] = millis();
        s_lastMeasured[s_currentB][s_currentA] = millis();

        Serial.printf("[ftmsched] Pair (%u,%u) distance=%.1f cm\n",
            s_currentA, s_currentB, distance_cm);
    } else {
        Serial.printf("[ftmsched] Pair (%u,%u) FAILED status=%u\n",
            s_currentA, s_currentB, status);
    }

    // Move to next pair
    s_pairState = FTM_PAIR_IDLE;
}

void FtmScheduler::triggerSolve() {
    PositionSolver::solve();
}

void FtmScheduler::broadcastPositions() {
    uint8_t count = PeerTable::peerCount();
    uint8_t dim = PeerTable::getDimension();

    // Build position update message
    uint16_t msgSize = sizeof(PosUpdateMsg) + count * sizeof(PosUpdateEntry);
    uint8_t buf[sizeof(PosUpdateMsg) + MESH_MAX_NODES * sizeof(PosUpdateEntry)];

    PosUpdateMsg* msg = (PosUpdateMsg*)buf;
    msg->type = MSG_TYPE_POS_UPDATE;
    msg->dimension = dim;
    msg->count = count;

    PosUpdateEntry* entries = (PosUpdateEntry*)(buf + sizeof(PosUpdateMsg));
    for (uint8_t i = 0; i < count; i++) {
        PeerEntry* e = PeerTable::getEntryByIndex(i);
        if (e) {
            memcpy(entries[i].mac, e->mac, 6);
            entries[i].x = e->position[0];
            entries[i].y = e->position[1];
            entries[i].z = e->position[2];
            entries[i].confidence = e->confidence;
        }
    }

    MeshConductor::broadcastToAll(buf, msgSize);
    Serial.printf("[ftmsched] Broadcast %u positions (%uD)\n", count, dim);
}

bool FtmScheduler::isActive() {
    return s_active || s_pairState != FTM_PAIR_IDLE;
}

void FtmScheduler::print() {
    Serial.println("=== FTM Scheduler ===");
    Serial.printf("Queue: %u items, State: %u, Active: %s\n",
        s_queueCount, s_pairState, s_active ? "yes" : "no");
    if (s_pairState != FTM_PAIR_IDLE) {
        Serial.printf("Current pair: (%u,%u) readyA=%d readyB=%d\n",
            s_currentA, s_currentB, s_readyA, s_readyB);
    }
    for (uint8_t i = 0; i < s_queueCount; i++) {
        uint8_t idx = (s_queueHead + i) % FTM_QUEUE_MAX;
        Serial.printf("  [%u] pair=(%u,%u) prio=%u\n",
            i, s_queue[idx].nodeA_idx, s_queue[idx].nodeB_idx, s_queue[idx].priority);
    }
}
