#include "nvs_config_registry.h"
#include "nvs_config.h"
#include <Arduino.h>

// --- Registry table ---

static const ConfigField s_fields[] = {
    { NVS_KEY_LEDSEN,    "LEDs enabled",                CFG_BOOL  },
    { NVS_KEY_EW_BAT,   "Election weight: battery",    CFG_FLOAT },
    { NVS_KEY_EW_ADJ,   "Election weight: adjacency",  CFG_FLOAT },
    { NVS_KEY_EW_TEN,   "Election weight: tenure",     CFG_FLOAT },
    { NVS_KEY_EW_LBP,   "Election weight: low-bat penalty", CFG_FLOAT },
    { NVS_KEY_CLR_INIT,  "Color: init (0xRRGGBB)",     CFG_U32   },
    { NVS_KEY_CLR_RDY,   "Color: ready",               CFG_U32   },
    { NVS_KEY_CLR_GW,    "Color: gateway",              CFG_U32   },
    { NVS_KEY_CLR_PEER,  "Color: peer",                 CFG_U32   },
    { NVS_KEY_CLR_DISC,  "Color: disconnected",         CFG_U32   },
    { NVS_KEY_HB_INT,    "Heartbeat interval (s)",      CFG_U32   },
    { NVS_KEY_HB_STALE,  "Heartbeat stale multiplier",  CFG_U32   },
    { NVS_KEY_REEL_DMV,  "Re-election battery delta (mV)", CFG_U32 },
    { NVS_KEY_FTM_STALE, "FTM staleness (s)",           CFG_U32   },
    { NVS_KEY_FTM_ANCH,  "FTM new-node anchors",        CFG_U32   },
    { NVS_KEY_FTM_SAMP,  "FTM samples per pair",        CFG_U32   },
    { NVS_KEY_FTM_TMO,   "FTM pair timeout (ms)",       CFG_U32   },
    { NVS_KEY_FTM_SWP,   "FTM sweep interval (s)",      CFG_U32   },
    { NVS_KEY_FTM_KPN,   "FTM Kalman process noise",    CFG_FLOAT },
    { NVS_KEY_FTM_OFS,   "FTM responder offset (cm)",   CFG_U32   },
    { NVS_KEY_ORCH_MODE, "Orchestrator mode",            CFG_U32   },
    { NVS_KEY_ORCH_TRVD, "Orch travel delay (ms)",       CFG_U32   },
    { NVS_KEY_ORCH_RMIN, "Orch random min (ms)",         CFG_U32   },
    { NVS_KEY_ORCH_RMAX, "Orch random max (ms)",         CFG_U32   },
    { NVS_KEY_ORCH_TONE, "Orch tone index",              CFG_U32   },
    { NVS_KEY_CSYNC_INT, "Clock sync interval (s)",      CFG_U32   },
};
static constexpr uint8_t FIELD_COUNT = sizeof(s_fields) / sizeof(s_fields[0]);

// --- Getter helper: read current value into JsonDocument ---

static void getField(JsonDocument& doc, const ConfigField& f) {
    if (strcmp(f.key, NVS_KEY_LEDSEN) == 0)     { doc[f.key] = (bool)NvsConfigManager::ledsEnabled; return; }
    if (strcmp(f.key, NVS_KEY_EW_BAT) == 0)     { doc[f.key] = (float)NvsConfigManager::electWBattery; return; }
    if (strcmp(f.key, NVS_KEY_EW_ADJ) == 0)     { doc[f.key] = (float)NvsConfigManager::electWAdjacency; return; }
    if (strcmp(f.key, NVS_KEY_EW_TEN) == 0)     { doc[f.key] = (float)NvsConfigManager::electWTenure; return; }
    if (strcmp(f.key, NVS_KEY_EW_LBP) == 0)     { doc[f.key] = (float)NvsConfigManager::electWLowbatPenalty; return; }
    if (strcmp(f.key, NVS_KEY_CLR_INIT) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::colorInit; return; }
    if (strcmp(f.key, NVS_KEY_CLR_RDY) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::colorReady; return; }
    if (strcmp(f.key, NVS_KEY_CLR_GW) == 0)     { doc[f.key] = (uint32_t)NvsConfigManager::colorGateway; return; }
    if (strcmp(f.key, NVS_KEY_CLR_PEER) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::colorPeer; return; }
    if (strcmp(f.key, NVS_KEY_CLR_DISC) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::colorDisconnected; return; }
    if (strcmp(f.key, NVS_KEY_HB_INT) == 0)     { doc[f.key] = (uint32_t)NvsConfigManager::heartbeatInterval_s; return; }
    if (strcmp(f.key, NVS_KEY_HB_STALE) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::heartbeatStaleMultiplier; return; }
    if (strcmp(f.key, NVS_KEY_REEL_DMV) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::reelectionBatteryDelta_mv; return; }
    if (strcmp(f.key, NVS_KEY_FTM_STALE) == 0)  { doc[f.key] = (uint32_t)NvsConfigManager::ftmStaleness_s; return; }
    if (strcmp(f.key, NVS_KEY_FTM_ANCH) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::ftmNewNodeAnchors; return; }
    if (strcmp(f.key, NVS_KEY_FTM_SAMP) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::ftmSamplesPerPair; return; }
    if (strcmp(f.key, NVS_KEY_FTM_TMO) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::ftmPairTimeout_ms; return; }
    if (strcmp(f.key, NVS_KEY_FTM_SWP) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::ftmSweepInterval_s; return; }
    if (strcmp(f.key, NVS_KEY_FTM_KPN) == 0)    { doc[f.key] = (float)NvsConfigManager::ftmKalmanProcessNoise; return; }
    if (strcmp(f.key, NVS_KEY_FTM_OFS) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::ftmResponderOffset_cm; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_MODE) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchMode; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_TRVD) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchTravelDelay_ms; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_RMIN) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchRandomMin_ms; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_RMAX) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchRandomMax_ms; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_TONE) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchToneIndex; return; }
    if (strcmp(f.key, NVS_KEY_CSYNC_INT) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::clockSyncInterval_s; return; }
}

// --- Setter helper: apply a JSON value to the matching PropertyValue ---

static bool setField(const char* key, JsonVariantConst val) {
    if (strcmp(key, NVS_KEY_LEDSEN) == 0)     { NvsConfigManager::ledsEnabled = val.as<bool>(); return true; }
    if (strcmp(key, NVS_KEY_EW_BAT) == 0)     { NvsConfigManager::electWBattery = val.as<float>(); return true; }
    if (strcmp(key, NVS_KEY_EW_ADJ) == 0)     { NvsConfigManager::electWAdjacency = val.as<float>(); return true; }
    if (strcmp(key, NVS_KEY_EW_TEN) == 0)     { NvsConfigManager::electWTenure = val.as<float>(); return true; }
    if (strcmp(key, NVS_KEY_EW_LBP) == 0)     { NvsConfigManager::electWLowbatPenalty = val.as<float>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_INIT) == 0)   { NvsConfigManager::colorInit = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_RDY) == 0)    { NvsConfigManager::colorReady = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_GW) == 0)     { NvsConfigManager::colorGateway = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_PEER) == 0)   { NvsConfigManager::colorPeer = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_DISC) == 0)   { NvsConfigManager::colorDisconnected = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_HB_INT) == 0)     { NvsConfigManager::heartbeatInterval_s = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_HB_STALE) == 0)   { NvsConfigManager::heartbeatStaleMultiplier = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_REEL_DMV) == 0)   { NvsConfigManager::reelectionBatteryDelta_mv = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_STALE) == 0)  { NvsConfigManager::ftmStaleness_s = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_ANCH) == 0)   { NvsConfigManager::ftmNewNodeAnchors = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_SAMP) == 0)   { NvsConfigManager::ftmSamplesPerPair = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_TMO) == 0)    { NvsConfigManager::ftmPairTimeout_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_SWP) == 0)    { NvsConfigManager::ftmSweepInterval_s = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_KPN) == 0)    { NvsConfigManager::ftmKalmanProcessNoise = val.as<float>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_OFS) == 0)    { NvsConfigManager::ftmResponderOffset_cm = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_MODE) == 0) { NvsConfigManager::orchMode = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_TRVD) == 0) { NvsConfigManager::orchTravelDelay_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_RMIN) == 0) { NvsConfigManager::orchRandomMin_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_RMAX) == 0) { NvsConfigManager::orchRandomMax_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_TONE) == 0) { NvsConfigManager::orchToneIndex = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CSYNC_INT) == 0) { NvsConfigManager::clockSyncInterval_s = val.as<uint32_t>(); return true; }
    return false;
}

// --- Public API ---

const ConfigField* configLookup(const char* key) {
    for (uint8_t i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(s_fields[i].key, key) == 0)
            return &s_fields[i];
    }
    return nullptr;
}

uint8_t configFieldCount() {
    return FIELD_COUNT;
}

const ConfigField* configFieldByIndex(uint8_t idx) {
    if (idx >= FIELD_COUNT) return nullptr;
    return &s_fields[idx];
}

void configBuildJson(JsonDocument& doc, const char** fields, uint8_t count) {
    if (count == 0) {
        // All fields
        for (uint8_t i = 0; i < FIELD_COUNT; i++) {
            getField(doc, s_fields[i]);
        }
    } else {
        for (uint8_t i = 0; i < count; i++) {
            const ConfigField* f = configLookup(fields[i]);
            if (f) getField(doc, *f);
        }
    }
}

uint8_t configApplyJson(const JsonObjectConst& obj) {
    uint8_t applied = 0;
    for (JsonPairConst kv : obj) {
        const char* key = kv.key().c_str();
        if (strcmp(key, "action") == 0) continue;  // skip action field
        if (strcmp(key, "mac") == 0) continue;      // skip mac field
        if (setField(key, kv.value()))
            applied++;
    }
    return applied;
}

void configListFields(Print& out) {
    out.println("NVS Config Fields:");
    for (uint8_t i = 0; i < FIELD_COUNT; i++) {
        const char* typeStr = (s_fields[i].type == CFG_BOOL) ? "bool" :
                              (s_fields[i].type == CFG_FLOAT) ? "float" : "u32";
        out.printf("  %-10s [%-5s]  %s\n", s_fields[i].key, typeStr, s_fields[i].description);
    }
}
