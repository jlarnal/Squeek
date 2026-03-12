#include "nvs_config.h"
#include <nvs_flash.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG           = "NvsConfig";
static const char* NVS_NAMESPACE = "sqcfg";

// NVS handle storage
namespace NvsConfig {
nvs_handle_t handle = 0;
bool isOpen         = false;
}

// Static member definitions with defaults + friendly names
PropertyValue<NVS_KEY_SHASH, uint64_t, NvsConfigManager> NvsConfigManager::settingHash(SETTINGS_HASH, "Settings Hash");
PropertyValue<NVS_KEY_LEDSEN, bool, NvsConfigManager>     NvsConfigManager::ledsEnabled(DEFAULT_LEDS_ENABLED, "LEDs Enabled");
PropertyValue<NVS_KEY_CLR_INIT, uint32_t, NvsConfigManager> NvsConfigManager::colorInit(DEFAULT_CLR_INIT, "Color: Init");
PropertyValue<NVS_KEY_CLR_RDY,  uint32_t, NvsConfigManager> NvsConfigManager::colorReady(DEFAULT_CLR_READY, "Color: Ready");
PropertyValue<NVS_KEY_CLR_GW,   uint32_t, NvsConfigManager> NvsConfigManager::colorGateway(DEFAULT_CLR_GATEWAY, "Color: Gateway");
PropertyValue<NVS_KEY_CLR_PEER, uint32_t, NvsConfigManager> NvsConfigManager::colorPeer(DEFAULT_CLR_PEER, "Color: Peer");
PropertyValue<NVS_KEY_CLR_DISC, uint32_t, NvsConfigManager> NvsConfigManager::colorDisconnected(DEFAULT_CLR_DISCONNECTED, "Color: Disconnected");

// Phase 2: Heartbeat & battery rotation
PropertyValue<NVS_KEY_HB_INT,   uint32_t, NvsConfigManager> NvsConfigManager::heartbeatInterval_s(DEFAULT_HB_INTERVAL_S, "Heartbeat Interval (s)");
PropertyValue<NVS_KEY_HB_STALE, uint32_t, NvsConfigManager> NvsConfigManager::heartbeatStaleMultiplier(DEFAULT_HB_STALE_MULT, "Heartbeat Stale Multiplier");
PropertyValue<NVS_KEY_REEL_CD,  uint16_t, NvsConfigManager> NvsConfigManager::reelectionCooldown_s(DEFAULT_REELECT_COOLDOWN_S, "Re-election Cooldown (s)");
PropertyValue<NVS_KEY_BAT_HYST, uint16_t, NvsConfigManager> NvsConfigManager::batteryHysteresis_mv(DEFAULT_BATTERY_HYST_MV, "Battery Hysteresis (mV)");

// Phase 2: FTM
PropertyValue<NVS_KEY_FTM_ANCH,  uint32_t, NvsConfigManager> NvsConfigManager::ftmNewNodeAnchors(DEFAULT_FTM_NEW_ANCHORS, "FTM New-Node Anchors");
PropertyValue<NVS_KEY_FTM_SAMP,  uint32_t, NvsConfigManager> NvsConfigManager::ftmSamplesPerPair(DEFAULT_FTM_SAMPLES, "FTM Samples/Pair");
PropertyValue<NVS_KEY_FTM_TMO,   uint32_t, NvsConfigManager> NvsConfigManager::ftmPairTimeout_ms(DEFAULT_FTM_PAIR_TMO_MS, "FTM Pair Timeout (ms)");
PropertyValue<NVS_KEY_FTM_KPN,   float,    NvsConfigManager> NvsConfigManager::ftmKalmanProcessNoise(DEFAULT_FTM_KALMAN_PN, "FTM Kalman Process Noise");
PropertyValue<NVS_KEY_FTM_OFS,   uint32_t, NvsConfigManager> NvsConfigManager::ftmResponderOffset_cm(DEFAULT_FTM_RESP_OFS_CM, "FTM Responder Offset (cm)");

// Phase 5: Web UI
PropertyValue<NVS_KEY_WEB_EN, bool, NvsConfigManager> NvsConfigManager::webEnabled(DEFAULT_WEB_ENABLED, "Web UI Enabled");

// Fast-path boot
PropertyValue<NVS_KEY_FAST_SCAN, uint16_t, NvsConfigManager>
    NvsConfigManager::fastScanDelay_s{DEFAULT_FAST_SCAN, "Fast-Scan Delay (s)"};

// Delegate
PropertyValue<NVS_KEY_DLG_TMO, uint16_t, NvsConfigManager>
    NvsConfigManager::delegateTimeout_s{DEFAULT_DELEGATE_TMO, "Delegate Timeout (s)"};

// RSSI re-evaluation
PropertyValue<NVS_KEY_RSSI_DK, uint16_t, NvsConfigManager>
    NvsConfigManager::rssiDecayK{DEFAULT_RSSI_DEC_K, "RSSI Decay K"};

// Battery in tenure
PropertyValue<NVS_KEY_BAT_TEN, bool, NvsConfigManager>
    NvsConfigManager::batteryInTenure{DEFAULT_BAT_TENURE, "Battery in Tenure"};

// Phase 4: Orchestrator
PropertyValue<NVS_KEY_ORCH_MODE, uint32_t, NvsConfigManager> NvsConfigManager::orchMode(DEFAULT_ORCH_MODE, "Orchestrator Mode");
PropertyValue<NVS_KEY_ORCH_TRVD, uint32_t, NvsConfigManager> NvsConfigManager::orchTravelDelay_ms(DEFAULT_ORCH_TRAVEL_DELAY, "Travel Delay (ms)");
PropertyValue<NVS_KEY_ORCH_RMIN, uint32_t, NvsConfigManager> NvsConfigManager::orchRandomMin_ms(DEFAULT_ORCH_RANDOM_MIN, "Random Min (ms)");
PropertyValue<NVS_KEY_ORCH_RMAX, uint32_t, NvsConfigManager> NvsConfigManager::orchRandomMax_ms(DEFAULT_ORCH_RANDOM_MAX, "Random Max (ms)");
PropertyValue<NVS_KEY_ORCH_TONE, uint32_t, NvsConfigManager> NvsConfigManager::orchToneIndex(DEFAULT_ORCH_TONE_INDEX, "Orchestrator Tone Index");
PropertyValue<NVS_KEY_CSYNC_INT, uint32_t, NvsConfigManager> NvsConfigManager::clockSyncInterval_s(DEFAULT_CSYNC_INTERVAL_S, "Clock Sync Interval (s)");

// NVS read helpers

static bool nvsGetBool(const char* key, bool defaultValue)
{
    uint8_t v     = 0;
    esp_err_t err = nvs_get_u8(NvsConfig::handle, key, &v);
    if (err == ESP_OK)
        return v != 0;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "nvs_get_u8(%s) failed: %s", key, esp_err_to_name(err));
    return defaultValue;
}

static uint64_t nvsGetU64(const char* key, uint64_t defaultValue)
{
    uint64_t v    = 0;
    esp_err_t err = nvs_get_u64(NvsConfig::handle, key, &v);
    if (err == ESP_OK)
        return v;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "nvs_get_u64(%s) failed: %s", key, esp_err_to_name(err));
    return defaultValue;
}

static float nvsGetFloat(const char* key, float defaultValue)
{
    uint32_t bits = 0;
    esp_err_t err = nvs_get_u32(NvsConfig::handle, key, &bits);
    if (err == ESP_OK) {
        float v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "nvs_get_u32(%s) failed: %s", key, esp_err_to_name(err));
    return defaultValue;
}

static uint8_t nvsGetU8(const char* key, uint8_t defaultValue)
{
    uint8_t v     = 0;
    esp_err_t err = nvs_get_u8(NvsConfig::handle, key, &v);
    if (err == ESP_OK)
        return v;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "nvs_get_u8(%s) failed: %s", key, esp_err_to_name(err));
    return defaultValue;
}

static uint16_t nvsGetU16(const char* key, uint16_t defaultValue)
{
    uint16_t v    = 0;
    esp_err_t err = nvs_get_u16(NvsConfig::handle, key, &v);
    if (err == ESP_OK)
        return v;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "nvs_get_u16(%s) failed: %s", key, esp_err_to_name(err));
    return defaultValue;
}

static uint32_t nvsGetU32(const char* key, uint32_t defaultValue)
{
    uint32_t v    = 0;
    esp_err_t err = nvs_get_u32(NvsConfig::handle, key, &v);
    if (err == ESP_OK)
        return v;
    if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "nvs_get_u32(%s) failed: %s", key, esp_err_to_name(err));
    return defaultValue;
}

void NvsConfigManager::begin()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated or new version, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &NvsConfig::handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(\"%s\") failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }
    NvsConfig::isOpen = true;

    // Check stored settings hash against compile-time hash
    uint64_t storedHash = nvsGetU64(NVS_KEY_SHASH, 0);
    if (storedHash != SETTINGS_HASH) {
        ESP_LOGW(TAG, "Defaults changed (stored=%llX, compiled=%llX) — restoring factory defaults", storedHash, SETTINGS_HASH);
        restoreFactoryDefault(0xBEEFF00D);
    } else {
        reloadFromNvs();
    }
}

void NvsConfigManager::reloadFromNvs()
{
    if (!NvsConfig::isOpen)
        return;

    settingHash.loadInitial(nvsGetU64(NVS_KEY_SHASH, SETTINGS_HASH));
    ledsEnabled.loadInitial(nvsGetBool(NVS_KEY_LEDSEN, DEFAULT_LEDS_ENABLED));
    colorInit.loadInitial(nvsGetU32(NVS_KEY_CLR_INIT, DEFAULT_CLR_INIT));
    colorReady.loadInitial(nvsGetU32(NVS_KEY_CLR_RDY, DEFAULT_CLR_READY));
    colorGateway.loadInitial(nvsGetU32(NVS_KEY_CLR_GW, DEFAULT_CLR_GATEWAY));
    colorPeer.loadInitial(nvsGetU32(NVS_KEY_CLR_PEER, DEFAULT_CLR_PEER));
    colorDisconnected.loadInitial(nvsGetU32(NVS_KEY_CLR_DISC, DEFAULT_CLR_DISCONNECTED));

    // Phase 2
    heartbeatInterval_s.loadInitial(nvsGetU32(NVS_KEY_HB_INT, DEFAULT_HB_INTERVAL_S));
    heartbeatStaleMultiplier.loadInitial(nvsGetU32(NVS_KEY_HB_STALE, DEFAULT_HB_STALE_MULT));
    reelectionCooldown_s.loadInitial(nvsGetU16(NVS_KEY_REEL_CD, DEFAULT_REELECT_COOLDOWN_S));
    batteryHysteresis_mv.loadInitial(nvsGetU16(NVS_KEY_BAT_HYST, DEFAULT_BATTERY_HYST_MV));
    ftmNewNodeAnchors.loadInitial(nvsGetU32(NVS_KEY_FTM_ANCH, DEFAULT_FTM_NEW_ANCHORS));
    ftmSamplesPerPair.loadInitial(nvsGetU32(NVS_KEY_FTM_SAMP, DEFAULT_FTM_SAMPLES));
    ftmPairTimeout_ms.loadInitial(nvsGetU32(NVS_KEY_FTM_TMO, DEFAULT_FTM_PAIR_TMO_MS));
    ftmKalmanProcessNoise.loadInitial(nvsGetFloat(NVS_KEY_FTM_KPN, DEFAULT_FTM_KALMAN_PN));
    ftmResponderOffset_cm.loadInitial(nvsGetU32(NVS_KEY_FTM_OFS, DEFAULT_FTM_RESP_OFS_CM));

    // Phase 4
    orchMode.loadInitial(nvsGetU32(NVS_KEY_ORCH_MODE, DEFAULT_ORCH_MODE));
    orchTravelDelay_ms.loadInitial(nvsGetU32(NVS_KEY_ORCH_TRVD, DEFAULT_ORCH_TRAVEL_DELAY));
    orchRandomMin_ms.loadInitial(nvsGetU32(NVS_KEY_ORCH_RMIN, DEFAULT_ORCH_RANDOM_MIN));
    orchRandomMax_ms.loadInitial(nvsGetU32(NVS_KEY_ORCH_RMAX, DEFAULT_ORCH_RANDOM_MAX));
    orchToneIndex.loadInitial(nvsGetU32(NVS_KEY_ORCH_TONE, DEFAULT_ORCH_TONE_INDEX));
    clockSyncInterval_s.loadInitial(nvsGetU32(NVS_KEY_CSYNC_INT, DEFAULT_CSYNC_INTERVAL_S));

    // Phase 5
    webEnabled.loadInitial(nvsGetBool(NVS_KEY_WEB_EN, DEFAULT_WEB_ENABLED));

    // Fast-path boot
    fastScanDelay_s.loadInitial(nvsGetU16(NVS_KEY_FAST_SCAN, DEFAULT_FAST_SCAN));
    delegateTimeout_s.loadInitial(nvsGetU16(NVS_KEY_DLG_TMO, DEFAULT_DELEGATE_TMO));
    rssiDecayK.loadInitial(nvsGetU16(NVS_KEY_RSSI_DK, DEFAULT_RSSI_DEC_K));
    batteryInTenure.loadInitial(nvsGetBool(NVS_KEY_BAT_TEN, DEFAULT_BAT_TENURE));

    ESP_LOGI(TAG, "Config loaded from NVS");
}

bool NvsConfigManager::restoreFactoryDefault(uint32_t safeKey)
{
    if (safeKey != 0xBEEFF00D)
        return false;

    ESP_LOGW(TAG, "Restoring all settings to factory defaults");

    settingHash         = SETTINGS_HASH;
    ledsEnabled         = DEFAULT_LEDS_ENABLED;
    colorInit          = DEFAULT_CLR_INIT;
    colorReady         = DEFAULT_CLR_READY;
    colorGateway       = DEFAULT_CLR_GATEWAY;
    colorPeer          = DEFAULT_CLR_PEER;
    colorDisconnected  = DEFAULT_CLR_DISCONNECTED;

    // Phase 2
    heartbeatInterval_s       = (uint32_t)DEFAULT_HB_INTERVAL_S;
    heartbeatStaleMultiplier  = (uint32_t)DEFAULT_HB_STALE_MULT;
    reelectionCooldown_s      = DEFAULT_REELECT_COOLDOWN_S;
    batteryHysteresis_mv      = DEFAULT_BATTERY_HYST_MV;
    ftmNewNodeAnchors         = (uint32_t)DEFAULT_FTM_NEW_ANCHORS;
    ftmSamplesPerPair         = (uint32_t)DEFAULT_FTM_SAMPLES;
    ftmPairTimeout_ms         = (uint32_t)DEFAULT_FTM_PAIR_TMO_MS;
    ftmKalmanProcessNoise     = DEFAULT_FTM_KALMAN_PN;
    ftmResponderOffset_cm     = (uint32_t)DEFAULT_FTM_RESP_OFS_CM;

    // Phase 4
    orchMode              = (uint32_t)DEFAULT_ORCH_MODE;
    orchTravelDelay_ms    = (uint32_t)DEFAULT_ORCH_TRAVEL_DELAY;
    orchRandomMin_ms      = (uint32_t)DEFAULT_ORCH_RANDOM_MIN;
    orchRandomMax_ms      = (uint32_t)DEFAULT_ORCH_RANDOM_MAX;
    orchToneIndex         = (uint32_t)DEFAULT_ORCH_TONE_INDEX;
    clockSyncInterval_s   = (uint32_t)DEFAULT_CSYNC_INTERVAL_S;

    // Phase 5
    webEnabled            = DEFAULT_WEB_ENABLED;

    // Fast-path boot
    fastScanDelay_s       = DEFAULT_FAST_SCAN;
    delegateTimeout_s     = DEFAULT_DELEGATE_TMO;
    rssiDecayK            = (uint16_t)DEFAULT_RSSI_DEC_K;
    batteryInTenure       = DEFAULT_BAT_TENURE;

    return true;
}
