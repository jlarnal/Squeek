#include "orchestrator.h"
#include "clock_sync.h"
#include "mesh_conductor.h"
#include "peer_table.h"
#include "audio_engine.h"
#include "tone_library.h"
#include "nvs_config.h"
#include "sq_log.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/timers.h>
#include <nvs_flash.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <string.h>

static const char* TAG = "Orch";

// --- Task events ---
enum OrchEvt : uint8_t {
    EVT_MODE_CHANGE = 1,
    EVT_STOP        = 2,
    EVT_SCHED_FIRE  = 3,
};

// --- File-scope state ---
static TaskHandle_t   s_taskHandle   = nullptr;
static QueueHandle_t  s_queue        = nullptr;
static OrchMode       s_mode         = ORCH_OFF;
static TravelOrder    s_travelOrder  = TRAVEL_NEAREST;

// Travel state
static uint8_t s_travelPath[MESH_MAX_NODES];
static uint8_t s_travelLen  = 0;
static uint8_t s_travelIdx  = 0;
static uint32_t s_lastStepMs = 0;

// Random state
static uint32_t s_nextRandomMs = 0;
static uint32_t s_lastRandomMs = 0;

// Sequence state
static SeqStep s_seqSteps[32];
static uint8_t s_seqCount   = 0;
static uint8_t s_seqIdx     = 0;
static uint32_t s_lastSeqMs = 0;

// Schedule state
static TimerHandle_t s_schedTimer   = nullptr;
static OrchMode      s_schedMode    = ORCH_OFF;

static const char* NVS_BLOB_KEY = "orchSeq";
static const char* NVS_NAMESPACE = "sqcfg";

// --- Helpers ---

static const char* modeName(OrchMode m) {
    switch (m) {
        case ORCH_OFF:       return "Off";
        case ORCH_TRAVEL:    return "Travel";
        case ORCH_RANDOM:    return "Random";
        case ORCH_SEQUENCE:  return "Sequence";
        case ORCH_SCHEDULED: return "Scheduled";
        default:             return "Unknown";
    }
}

static const char* travelOrderName(TravelOrder t) {
    switch (t) {
        case TRAVEL_NEAREST: return "nearest";
        case TRAVEL_AXIS:    return "axis";
        case TRAVEL_RANDOM:  return "random";
        default:             return "unknown";
    }
}

static void sendPlayCmd(uint8_t peerIdx, uint8_t toneIdx) {
    PeerEntry* pe = PeerTable::getEntryByIndex(peerIdx);
    if (!pe || !(pe->flags & PEER_STATUS_ALIVE)) return;

    // Check if target is self
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
    if (memcmp(own_mac, pe->mac, 6) == 0) {
        // Play locally
        const ToneSequence* seq = ToneLibrary::getByIndex(toneIdx);
        if (seq) AudioEngine::play(seq);
        return;
    }

    PlayCmdMsg msg;
    msg.type       = MSG_TYPE_PLAY_CMD;
    msg.tone_index = toneIdx;
    MeshConductor::sendToNode(pe->mac, &msg, sizeof(msg));
}

static uint32_t randomRange(uint32_t minVal, uint32_t maxVal) {
    if (minVal >= maxVal) return minVal;
    return minVal + (esp_random() % (maxVal - minVal + 1));
}

// --- Travel path builders ---

static void buildTravelNearest() {
    uint8_t count = PeerTable::peerCount();
    if (count == 0) { s_travelLen = 0; return; }

    bool visited[MESH_MAX_NODES] = {};
    s_travelLen = 0;

    // Start from node 0
    uint8_t current = 0;
    for (uint8_t step = 0; step < count; step++) {
        // Find first alive unvisited if current is dead
        PeerEntry* ce = PeerTable::getEntryByIndex(current);
        if (!ce || !(ce->flags & PEER_STATUS_ALIVE)) {
            // Find any alive unvisited
            bool found = false;
            for (uint8_t i = 0; i < count; i++) {
                if (visited[i]) continue;
                PeerEntry* pe = PeerTable::getEntryByIndex(i);
                if (pe && (pe->flags & PEER_STATUS_ALIVE)) {
                    current = i;
                    found = true;
                    break;
                }
            }
            if (!found) break;
        }

        s_travelPath[s_travelLen++] = current;
        visited[current] = true;

        // Find nearest unvisited
        float bestDist = 1e9f;
        int8_t bestIdx = -1;
        for (uint8_t i = 0; i < count; i++) {
            if (visited[i]) continue;
            PeerEntry* pe = PeerTable::getEntryByIndex(i);
            if (!pe || !(pe->flags & PEER_STATUS_ALIVE)) continue;
            float d = PeerTable::getDistance(current, i);
            if (d >= 0 && d < bestDist) {
                bestDist = d;
                bestIdx = i;
            }
        }

        if (bestIdx >= 0) {
            current = bestIdx;
        } else {
            // No distances known — fall back to next alive index
            for (uint8_t i = 0; i < count; i++) {
                if (!visited[i]) {
                    PeerEntry* pe = PeerTable::getEntryByIndex(i);
                    if (pe && (pe->flags & PEER_STATUS_ALIVE)) {
                        current = i;
                        break;
                    }
                }
            }
        }
    }
}

static void buildTravelAxis() {
    uint8_t count = PeerTable::peerCount();
    if (count == 0) { s_travelLen = 0; return; }

    // Collect alive indices
    uint8_t alive[MESH_MAX_NODES];
    float   xpos[MESH_MAX_NODES];
    uint8_t n = 0;
    for (uint8_t i = 0; i < count; i++) {
        PeerEntry* pe = PeerTable::getEntryByIndex(i);
        if (pe && (pe->flags & PEER_STATUS_ALIVE)) {
            alive[n] = i;
            xpos[n]  = pe->position[0];
            n++;
        }
    }

    // Simple insertion sort by X position
    for (uint8_t i = 1; i < n; i++) {
        for (uint8_t j = i; j > 0 && xpos[j] < xpos[j - 1]; j--) {
            float tmpX = xpos[j]; xpos[j] = xpos[j-1]; xpos[j-1] = tmpX;
            uint8_t tmpA = alive[j]; alive[j] = alive[j-1]; alive[j-1] = tmpA;
        }
    }

    s_travelLen = n;
    memcpy(s_travelPath, alive, n);
}

static void buildTravelRandom() {
    uint8_t count = PeerTable::peerCount();
    if (count == 0) { s_travelLen = 0; return; }

    // Collect alive indices
    uint8_t n = 0;
    for (uint8_t i = 0; i < count; i++) {
        PeerEntry* pe = PeerTable::getEntryByIndex(i);
        if (pe && (pe->flags & PEER_STATUS_ALIVE)) {
            s_travelPath[n++] = i;
        }
    }
    s_travelLen = n;

    // Fisher-Yates shuffle
    for (uint8_t i = n - 1; i > 0; i--) {
        uint8_t j = esp_random() % (i + 1);
        uint8_t tmp = s_travelPath[i];
        s_travelPath[i] = s_travelPath[j];
        s_travelPath[j] = tmp;
    }
}

static void buildTravelPath() {
    switch (s_travelOrder) {
        case TRAVEL_NEAREST: buildTravelNearest(); break;
        case TRAVEL_AXIS:    buildTravelAxis();    break;
        case TRAVEL_RANDOM:  buildTravelRandom();  break;
    }
    s_travelIdx  = 0;
    s_lastStepMs = millis();
    SqLog.printf("[orch] Travel path built (%s): %u nodes\n",
                 travelOrderName(s_travelOrder), s_travelLen);
}

// --- Mode stepping ---

static void stepTravel() {
    if (s_travelLen == 0) return;
    uint32_t now = millis();
    uint32_t delay = (uint32_t)NvsConfigManager::orchTravelDelay_ms;
    if (now - s_lastStepMs < delay) return;

    uint8_t toneIdx = (uint32_t)NvsConfigManager::orchToneIndex;
    sendPlayCmd(s_travelPath[s_travelIdx], toneIdx);

    s_travelIdx = (s_travelIdx + 1) % s_travelLen;
    s_lastStepMs = now;
}

static void stepRandom() {
    uint32_t now = millis();
    if (now - s_lastRandomMs < s_nextRandomMs) return;

    uint8_t count = PeerTable::peerCount();
    if (count == 0) return;

    // Pick random alive node
    uint8_t alive[MESH_MAX_NODES];
    uint8_t n = 0;
    for (uint8_t i = 0; i < count; i++) {
        PeerEntry* pe = PeerTable::getEntryByIndex(i);
        if (pe && (pe->flags & PEER_STATUS_ALIVE))
            alive[n++] = i;
    }
    if (n == 0) return;

    uint8_t pick = alive[esp_random() % n];
    uint8_t toneIdx = (uint32_t)NvsConfigManager::orchToneIndex;
    sendPlayCmd(pick, toneIdx);

    uint32_t minMs = (uint32_t)NvsConfigManager::orchRandomMin_ms;
    uint32_t maxMs = (uint32_t)NvsConfigManager::orchRandomMax_ms;
    s_nextRandomMs = randomRange(minMs, maxMs);
    s_lastRandomMs = now;
}

static void stepSequence() {
    if (s_seqCount == 0) return;
    uint32_t now = millis();

    const SeqStep& step = s_seqSteps[s_seqIdx];
    if (now - s_lastSeqMs < step.delay_ms && s_lastSeqMs != 0) return;

    sendPlayCmd(step.node_index, step.tone_index);

    s_seqIdx = (s_seqIdx + 1) % s_seqCount;
    s_lastSeqMs = now;
}

// --- Scheduled trigger ---

static void schedTimerCb(TimerHandle_t) {
    if (s_queue) {
        uint8_t evt = EVT_SCHED_FIRE;
        xQueueSend(s_queue, &evt, 0);
    }
}

// --- NVS sequence persistence ---

void Orchestrator::loadSequence() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = 0;
    if (nvs_get_blob(h, NVS_BLOB_KEY, nullptr, &len) != ESP_OK || len < 1) {
        nvs_close(h);
        return;
    }

    uint8_t buf[129];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (nvs_get_blob(h, NVS_BLOB_KEY, buf, &len) == ESP_OK && len >= 1) {
        s_seqCount = buf[0];
        if (s_seqCount > 32) s_seqCount = 32;
        size_t expected = 1 + s_seqCount * sizeof(SeqStep);
        if (len >= expected) {
            memcpy(s_seqSteps, &buf[1], s_seqCount * sizeof(SeqStep));
            SqLog.printf("[orch] Loaded %u sequence steps from NVS\n", s_seqCount);
        } else {
            s_seqCount = 0;
        }
    }
    nvs_close(h);
}

void Orchestrator::saveSequence() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t buf[129];
    buf[0] = s_seqCount;
    memcpy(&buf[1], s_seqSteps, s_seqCount * sizeof(SeqStep));
    size_t len = 1 + s_seqCount * sizeof(SeqStep);

    nvs_set_blob(h, NVS_BLOB_KEY, buf, len);
    nvs_commit(h);
    nvs_close(h);
    SqLog.printf("[orch] Saved %u sequence steps to NVS\n", s_seqCount);
}

void Orchestrator::addSequenceStep(uint8_t node_idx, uint8_t tone_idx, uint16_t delay_ms) {
    if (s_seqCount >= 32) return;
    s_seqSteps[s_seqCount].node_index = node_idx;
    s_seqSteps[s_seqCount].tone_index = tone_idx;
    s_seqSteps[s_seqCount].delay_ms   = delay_ms;
    s_seqCount++;
}

void Orchestrator::clearSequence() {
    s_seqCount = 0;
    s_seqIdx   = 0;
}

uint8_t Orchestrator::sequenceCount() {
    return s_seqCount;
}

const SeqStep* Orchestrator::sequenceSteps() {
    return s_seqSteps;
}

// --- FreeRTOS task ---

void Orchestrator::orchTask(void*) {
    uint8_t evt;

    for (;;) {
        TickType_t timeout;
        if (s_mode == ORCH_OFF) {
            timeout = portMAX_DELAY;
        } else {
            timeout = pdMS_TO_TICKS(50);  // 50ms tick for stepping
        }

        if (xQueueReceive(s_queue, &evt, timeout) == pdTRUE) {
            switch (evt) {
                case EVT_MODE_CHANGE:
                    // Mode already set by setMode(), just reset state
                    switch (s_mode) {
                        case ORCH_TRAVEL:
                            buildTravelPath();
                            break;
                        case ORCH_RANDOM:
                            s_lastRandomMs = millis();
                            s_nextRandomMs = randomRange(
                                (uint32_t)NvsConfigManager::orchRandomMin_ms,
                                (uint32_t)NvsConfigManager::orchRandomMax_ms);
                            break;
                        case ORCH_SEQUENCE:
                            s_seqIdx    = 0;
                            s_lastSeqMs = 0;
                            break;
                        default:
                            break;
                    }
                    break;

                case EVT_STOP:
                    s_mode = ORCH_OFF;
                    break;

                case EVT_SCHED_FIRE:
                    SqLog.printf("[orch] Scheduled trigger fired -> %s\n", modeName(s_schedMode));
                    s_mode = s_schedMode;
                    // Post mode change to self
                    {
                        uint8_t mc = EVT_MODE_CHANGE;
                        xQueueSend(s_queue, &mc, 0);
                    }
                    break;
            }
        }

        // Step the active mode (gateway only)
        if (MeshConductor::isGateway()) {
            switch (s_mode) {
                case ORCH_TRAVEL:   stepTravel();   break;
                case ORCH_RANDOM:   stepRandom();   break;
                case ORCH_SEQUENCE: stepSequence(); break;
                default: break;
            }
        }
    }
}

// --- Public API ---

void Orchestrator::init() {
    s_queue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(orchTask, "orch", 4096, nullptr, tskIDLE_PRIORITY + 2, &s_taskHandle);

    ClockSync::init();
    loadSequence();

    SqLog.println("[orch] Orchestrator initialized");
}

void Orchestrator::stop() {
    s_mode = ORCH_OFF;
    if (s_queue) {
        uint8_t evt = EVT_STOP;
        xQueueSend(s_queue, &evt, 0);
    }
    ClockSync::stop();
}

void Orchestrator::setMode(OrchMode mode) {
    s_mode = mode;
    NvsConfigManager::orchMode = (uint32_t)mode;

    // Broadcast mode change to all peers
    if (MeshConductor::isGateway()) {
        OrchModeMsg msg;
        msg.type = MSG_TYPE_ORCH_MODE;
        msg.mode = mode;
        MeshConductor::broadcastToAll(&msg, sizeof(msg));
    }

    // Notify task
    if (s_queue) {
        uint8_t evt = EVT_MODE_CHANGE;
        xQueueSend(s_queue, &evt, 0);
    }

    SqLog.printf("[orch] Mode set to %s\n", modeName(mode));
}

OrchMode Orchestrator::getMode() {
    return s_mode;
}

void Orchestrator::setTravelOrder(TravelOrder order) {
    s_travelOrder = order;
}

TravelOrder Orchestrator::getTravelOrder() {
    return s_travelOrder;
}

void Orchestrator::onPlayCmd(uint8_t tone_index) {
    const ToneSequence* seq = ToneLibrary::getByIndex(tone_index);
    if (seq) AudioEngine::play(seq);
}

void Orchestrator::onModeChange(uint8_t mode) {
    s_mode = (OrchMode)mode;
    SqLog.printf("[orch] Mode changed to %s (from gateway)\n", modeName(s_mode));
}

void Orchestrator::scheduleRelative(uint32_t delay_ms, OrchMode mode) {
    s_schedMode = mode;

    if (s_schedTimer == nullptr) {
        s_schedTimer = xTimerCreate("orchSched", pdMS_TO_TICKS(delay_ms),
                                     pdFALSE, nullptr, schedTimerCb);
    } else {
        xTimerChangePeriod(s_schedTimer, pdMS_TO_TICKS(delay_ms), 0);
    }
    xTimerStart(s_schedTimer, 0);
    SqLog.printf("[orch] Scheduled %s in %lu ms\n", modeName(mode), delay_ms);
}

void Orchestrator::cancelSchedule() {
    if (s_schedTimer) {
        xTimerStop(s_schedTimer, 0);
    }
    SqLog.println("[orch] Schedule cancelled");
}

void Orchestrator::printStatus(Print& out) {
    out.printf("Orchestrator mode: %s\n", modeName(s_mode));
    if (s_mode == ORCH_TRAVEL) {
        out.printf("  Travel order: %s, path len: %u, current: %u\n",
                   travelOrderName(s_travelOrder), s_travelLen, s_travelIdx);
    }
    out.printf("  Tone index: %lu (%s)\n",
               (uint32_t)NvsConfigManager::orchToneIndex,
               ToneLibrary::nameByIndex((uint32_t)NvsConfigManager::orchToneIndex) ?: "?");
    out.printf("  Travel delay: %lu ms\n", (uint32_t)NvsConfigManager::orchTravelDelay_ms);
    out.printf("  Random: %lu-%lu ms\n",
               (uint32_t)NvsConfigManager::orchRandomMin_ms,
               (uint32_t)NvsConfigManager::orchRandomMax_ms);
    out.printf("  Sequence steps: %u\n", s_seqCount);
    out.printf("  Clock synced: %s\n", ClockSync::isSynced() ? "yes" : "no");
}
