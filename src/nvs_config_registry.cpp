#include "nvs_config_registry.h"
#include "nvs_config.h"
#include <Arduino.h>

// --- Registry table ---

static const ConfigField s_fields[] = {
    { NVS_KEY_LEDSEN,    &I18N_LEDS_ENABLED,         CFG_BOOL  },
    { NVS_KEY_CLR_INIT,  &I18N_COLOR_INIT,            CFG_U32   },
    { NVS_KEY_CLR_RDY,   &I18N_COLOR_READY,           CFG_U32   },
    { NVS_KEY_CLR_GW,    &I18N_COLOR_GATEWAY,         CFG_U32   },
    { NVS_KEY_CLR_PEER,  &I18N_COLOR_PEER,            CFG_U32   },
    { NVS_KEY_CLR_DISC,  &I18N_COLOR_DISCONNECTED,    CFG_U32   },
    { NVS_KEY_HB_INT,    &I18N_HEARTBEAT_INTERVAL,    CFG_U32   },
    { NVS_KEY_HB_STALE,  &I18N_HEARTBEAT_STALE,       CFG_U32   },
    { NVS_KEY_REEL_CD,   &I18N_REELECTION_COOLDOWN,   CFG_U32   },
    { NVS_KEY_BAT_HYST,  &I18N_BATTERY_HYSTERESIS,    CFG_U32   },
    { NVS_KEY_FTM_ANCH,  &I18N_FTM_ANCHORS,           CFG_U32   },
    { NVS_KEY_FTM_SAMP,  &I18N_FTM_SAMPLES,           CFG_U32   },
    { NVS_KEY_FTM_TMO,   &I18N_FTM_TIMEOUT,           CFG_U32   },
    { NVS_KEY_FTM_KPN,   &I18N_FTM_KALMAN_NOISE,      CFG_FLOAT },
    { NVS_KEY_FTM_OFS,   &I18N_FTM_OFFSET,            CFG_U32   },
    { NVS_KEY_ORCH_MODE, &I18N_ORCH_MODE,             CFG_U32   },
    { NVS_KEY_ORCH_TRVD, &I18N_ORCH_TRAVEL_DELAY,    CFG_U32   },
    { NVS_KEY_ORCH_RMIN, &I18N_ORCH_RANDOM_MIN,      CFG_U32   },
    { NVS_KEY_ORCH_RMAX, &I18N_ORCH_RANDOM_MAX,      CFG_U32   },
    { NVS_KEY_ORCH_TONE, &I18N_ORCH_TONE_INDEX,      CFG_U32   },
    { NVS_KEY_CSYNC_INT, &I18N_CLOCK_SYNC_INTERVAL,  CFG_U32   },
    { NVS_KEY_WEB_EN,    &I18N_WEB_ENABLED,           CFG_BOOL  },
    { NVS_KEY_RSSI_DK,   &I18N_RSSI_DECAY_K,         CFG_U32   },
    { NVS_KEY_BAT_TEN,   &I18N_BATTERY_IN_TENURE,    CFG_BOOL  },
    { NVS_KEY_EL_SLOT,   &I18N_ELECTION_SLOT,        CFG_U32   },
    { NVS_KEY_EL_ANN,    &I18N_ELECTION_ANNOUNCE,    CFG_U32   },
};
static constexpr uint8_t FIELD_COUNT = sizeof(s_fields) / sizeof(s_fields[0]);

// --- Getter helper: read current value into JsonDocument ---

static void getField(JsonDocument& doc, const ConfigField& f) {
    if (strcmp(f.key, NVS_KEY_LEDSEN) == 0)     { doc[f.key] = (bool)NvsConfigManager::ledsEnabled; return; }
    if (strcmp(f.key, NVS_KEY_CLR_INIT) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::colorInit; return; }
    if (strcmp(f.key, NVS_KEY_CLR_RDY) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::colorReady; return; }
    if (strcmp(f.key, NVS_KEY_CLR_GW) == 0)     { doc[f.key] = (uint32_t)NvsConfigManager::colorGateway; return; }
    if (strcmp(f.key, NVS_KEY_CLR_PEER) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::colorPeer; return; }
    if (strcmp(f.key, NVS_KEY_CLR_DISC) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::colorDisconnected; return; }
    if (strcmp(f.key, NVS_KEY_HB_INT) == 0)     { doc[f.key] = (uint32_t)NvsConfigManager::heartbeatInterval_s; return; }
    if (strcmp(f.key, NVS_KEY_HB_STALE) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::heartbeatStaleMultiplier; return; }
    if (strcmp(f.key, NVS_KEY_REEL_CD) == 0)    { doc[f.key] = (uint32_t)(uint16_t)NvsConfigManager::reelectionCooldown_s; return; }
    if (strcmp(f.key, NVS_KEY_BAT_HYST) == 0)   { doc[f.key] = (uint32_t)(uint16_t)NvsConfigManager::batteryHysteresis_mv; return; }
    if (strcmp(f.key, NVS_KEY_FTM_ANCH) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::ftmNewNodeAnchors; return; }
    if (strcmp(f.key, NVS_KEY_FTM_SAMP) == 0)   { doc[f.key] = (uint32_t)NvsConfigManager::ftmSamplesPerPair; return; }
    if (strcmp(f.key, NVS_KEY_FTM_TMO) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::ftmPairTimeout_ms; return; }
    if (strcmp(f.key, NVS_KEY_FTM_KPN) == 0)    { doc[f.key] = (float)NvsConfigManager::ftmKalmanProcessNoise; return; }
    if (strcmp(f.key, NVS_KEY_FTM_OFS) == 0)    { doc[f.key] = (uint32_t)NvsConfigManager::ftmResponderOffset_cm; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_MODE) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchMode; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_TRVD) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchTravelDelay_ms; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_RMIN) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchRandomMin_ms; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_RMAX) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchRandomMax_ms; return; }
    if (strcmp(f.key, NVS_KEY_ORCH_TONE) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::orchToneIndex; return; }
    if (strcmp(f.key, NVS_KEY_CSYNC_INT) == 0) { doc[f.key] = (uint32_t)NvsConfigManager::clockSyncInterval_s; return; }
    if (strcmp(f.key, NVS_KEY_WEB_EN) == 0)   { doc[f.key] = (bool)NvsConfigManager::webEnabled; return; }
    if (strcmp(f.key, NVS_KEY_RSSI_DK) == 0) { doc[f.key] = (uint32_t)(uint16_t)NvsConfigManager::rssiDecayK; return; }
    if (strcmp(f.key, NVS_KEY_BAT_TEN) == 0) { doc[f.key] = (bool)NvsConfigManager::batteryInTenure; return; }
    if (strcmp(f.key, NVS_KEY_EL_SLOT) == 0) { doc[f.key] = (uint32_t)(uint16_t)NvsConfigManager::electionSlot_ms; return; }
    if (strcmp(f.key, NVS_KEY_EL_ANN) == 0)  { doc[f.key] = (uint32_t)(uint16_t)NvsConfigManager::electionAnnounce_ms; return; }
}

// --- Setter helper: apply a JSON value to the matching PropertyValue ---

static bool setField(const char* key, JsonVariantConst val) {
    if (strcmp(key, NVS_KEY_LEDSEN) == 0)     { NvsConfigManager::ledsEnabled = val.as<bool>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_INIT) == 0)   { NvsConfigManager::colorInit = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_RDY) == 0)    { NvsConfigManager::colorReady = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_GW) == 0)     { NvsConfigManager::colorGateway = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_PEER) == 0)   { NvsConfigManager::colorPeer = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CLR_DISC) == 0)   { NvsConfigManager::colorDisconnected = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_HB_INT) == 0)     { NvsConfigManager::heartbeatInterval_s = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_HB_STALE) == 0)   { NvsConfigManager::heartbeatStaleMultiplier = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_REEL_CD) == 0)  {
        uint16_t v = val.as<uint16_t>();
        if (v < 30) v = 30;
        NvsConfigManager::reelectionCooldown_s = v;
        return true;
    }
    if (strcmp(key, NVS_KEY_BAT_HYST) == 0) {
        uint16_t v = val.as<uint16_t>();
        if (v < 50)   v = 50;
        if (v > 1000)  v = 1000;
        NvsConfigManager::batteryHysteresis_mv = v;
        return true;
    }
    if (strcmp(key, NVS_KEY_FTM_ANCH) == 0)   { NvsConfigManager::ftmNewNodeAnchors = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_SAMP) == 0)   { NvsConfigManager::ftmSamplesPerPair = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_TMO) == 0)    { NvsConfigManager::ftmPairTimeout_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_KPN) == 0)    { NvsConfigManager::ftmKalmanProcessNoise = val.as<float>(); return true; }
    if (strcmp(key, NVS_KEY_FTM_OFS) == 0)    { NvsConfigManager::ftmResponderOffset_cm = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_MODE) == 0) { NvsConfigManager::orchMode = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_TRVD) == 0) { NvsConfigManager::orchTravelDelay_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_RMIN) == 0) { NvsConfigManager::orchRandomMin_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_RMAX) == 0) { NvsConfigManager::orchRandomMax_ms = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_ORCH_TONE) == 0) { NvsConfigManager::orchToneIndex = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_CSYNC_INT) == 0) { NvsConfigManager::clockSyncInterval_s = val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_WEB_EN) == 0)   { NvsConfigManager::webEnabled = val.as<bool>(); return true; }
    if (strcmp(key, NVS_KEY_RSSI_DK) == 0) { NvsConfigManager::rssiDecayK = (uint16_t)val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_BAT_TEN) == 0) { NvsConfigManager::batteryInTenure = val.as<bool>(); return true; }
    if (strcmp(key, NVS_KEY_EL_SLOT) == 0) { NvsConfigManager::electionSlot_ms = (uint16_t)val.as<uint32_t>(); return true; }
    if (strcmp(key, NVS_KEY_EL_ANN) == 0)  { NvsConfigManager::electionAnnounce_ms = (uint16_t)val.as<uint32_t>(); return true; }
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

void configListFields(Print& out, Lang lang) {
    out.println("NVS Config Fields:");
    for (uint8_t i = 0; i < FIELD_COUNT; i++) {
        const char* typeStr = (s_fields[i].type == CFG_BOOL) ? "bool" :
                              (s_fields[i].type == CFG_FLOAT) ? "float" : "u32";
        const char* desc = i18nGet(s_fields[i].strings->description, lang);
        out.printf("  %-10s [%-5s]  %s\n", s_fields[i].key, typeStr, desc);
    }
}
