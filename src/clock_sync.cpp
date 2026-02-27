#include "clock_sync.h"
#include "mesh_conductor.h"
#include "nvs_config.h"
#include "sq_log.h"
#include <Arduino.h>
#include <freertos/timers.h>

static int32_t       s_offset    = 0;
static bool          s_synced    = false;
static TimerHandle_t s_syncTimer = nullptr;

static void syncTimerCb(TimerHandle_t) {
    ClockSyncMsg msg;
    msg.type       = MSG_TYPE_CLOCK_SYNC;
    msg.gateway_ms = millis();
    MeshConductor::broadcastToAll(&msg, sizeof(msg));
}

void ClockSync::init() {
    if (!MeshConductor::isGateway()) {
        s_synced = false;
        return;
    }

    s_synced = true;  // gateway is always synced
    s_offset = 0;

    uint32_t interval_s = (uint32_t)NvsConfigManager::clockSyncInterval_s;
    if (interval_s == 0) interval_s = 10;

    if (s_syncTimer == nullptr) {
        s_syncTimer = xTimerCreate("csync", pdMS_TO_TICKS(interval_s * 1000),
                                    pdTRUE, nullptr, syncTimerCb);
    } else {
        xTimerChangePeriod(s_syncTimer, pdMS_TO_TICKS(interval_s * 1000), 0);
    }
    xTimerStart(s_syncTimer, 0);

    // Send one immediately
    syncTimerCb(nullptr);

    SqLog.printf("[csync] Gateway clock sync started (interval=%lus)\n", interval_s);
}

void ClockSync::stop() {
    if (s_syncTimer) {
        xTimerStop(s_syncTimer, 0);
    }
    s_synced = false;
}

void ClockSync::onSyncReceived(uint32_t gateway_ms) {
    s_offset = (int32_t)(gateway_ms - millis());
    s_synced = true;
}

uint32_t ClockSync::meshTime() {
    if (MeshConductor::isGateway()) return millis();
    return millis() + s_offset;
}

bool ClockSync::isSynced() {
    if (MeshConductor::isGateway()) return true;
    return s_synced;
}
