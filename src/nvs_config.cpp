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

// Static member definitions with defaults
PropertyValue<NVS_KEY_SHASH, uint64_t, NvsConfigManager> NvsConfigManager::settingHash(SETTINGS_HASH);
PropertyValue<NVS_KEY_LEDSEN, bool, NvsConfigManager>     NvsConfigManager::ledsEnabled(DEFAULT_LEDS_ENABLED);
PropertyValue<NVS_KEY_EW_BAT, float, NvsConfigManager>    NvsConfigManager::electWBattery(DEFAULT_ELECT_W_BATTERY);
PropertyValue<NVS_KEY_EW_ADJ, float, NvsConfigManager>    NvsConfigManager::electWAdjacency(DEFAULT_ELECT_W_ADJACENCY);
PropertyValue<NVS_KEY_EW_TEN, float, NvsConfigManager>    NvsConfigManager::electWTenure(DEFAULT_ELECT_W_TENURE);
PropertyValue<NVS_KEY_EW_LBP, float, NvsConfigManager>    NvsConfigManager::electWLowbatPenalty(DEFAULT_ELECT_W_LOWBAT_PEN);
PropertyValue<NVS_KEY_CLR_INIT, uint32_t, NvsConfigManager> NvsConfigManager::colorInit(DEFAULT_CLR_INIT);
PropertyValue<NVS_KEY_CLR_RDY,  uint32_t, NvsConfigManager> NvsConfigManager::colorReady(DEFAULT_CLR_READY);
PropertyValue<NVS_KEY_CLR_GW,   uint32_t, NvsConfigManager> NvsConfigManager::colorGateway(DEFAULT_CLR_GATEWAY);
PropertyValue<NVS_KEY_CLR_PEER, uint32_t, NvsConfigManager> NvsConfigManager::colorPeer(DEFAULT_CLR_PEER);
PropertyValue<NVS_KEY_CLR_DISC, uint32_t, NvsConfigManager> NvsConfigManager::colorDisconnected(DEFAULT_CLR_DISCONNECTED);

// Phase 2: Heartbeat & re-election
PropertyValue<NVS_KEY_HB_INT,   uint32_t, NvsConfigManager> NvsConfigManager::heartbeatInterval_s(DEFAULT_HB_INTERVAL_S);
PropertyValue<NVS_KEY_HB_STALE, uint32_t, NvsConfigManager> NvsConfigManager::heartbeatStaleMultiplier(DEFAULT_HB_STALE_MULT);
PropertyValue<NVS_KEY_REEL_DMV, uint32_t, NvsConfigManager> NvsConfigManager::reelectionBatteryDelta_mv(DEFAULT_REELECT_DELTA_MV);

// Phase 2: FTM
PropertyValue<NVS_KEY_FTM_STALE, uint32_t, NvsConfigManager> NvsConfigManager::ftmStaleness_s(DEFAULT_FTM_STALE_S);
PropertyValue<NVS_KEY_FTM_ANCH,  uint32_t, NvsConfigManager> NvsConfigManager::ftmNewNodeAnchors(DEFAULT_FTM_NEW_ANCHORS);
PropertyValue<NVS_KEY_FTM_SAMP,  uint32_t, NvsConfigManager> NvsConfigManager::ftmSamplesPerPair(DEFAULT_FTM_SAMPLES);
PropertyValue<NVS_KEY_FTM_TMO,   uint32_t, NvsConfigManager> NvsConfigManager::ftmPairTimeout_ms(DEFAULT_FTM_PAIR_TMO_MS);
PropertyValue<NVS_KEY_FTM_SWP,   uint32_t, NvsConfigManager> NvsConfigManager::ftmSweepInterval_s(DEFAULT_FTM_SWEEP_INT_S);
PropertyValue<NVS_KEY_FTM_KPN,   float,    NvsConfigManager> NvsConfigManager::ftmKalmanProcessNoise(DEFAULT_FTM_KALMAN_PN);
PropertyValue<NVS_KEY_FTM_OFS,   uint32_t, NvsConfigManager> NvsConfigManager::ftmResponderOffset_cm(DEFAULT_FTM_RESP_OFS_CM);

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
    electWBattery.loadInitial(nvsGetFloat(NVS_KEY_EW_BAT, DEFAULT_ELECT_W_BATTERY));
    electWAdjacency.loadInitial(nvsGetFloat(NVS_KEY_EW_ADJ, DEFAULT_ELECT_W_ADJACENCY));
    electWTenure.loadInitial(nvsGetFloat(NVS_KEY_EW_TEN, DEFAULT_ELECT_W_TENURE));
    electWLowbatPenalty.loadInitial(nvsGetFloat(NVS_KEY_EW_LBP, DEFAULT_ELECT_W_LOWBAT_PEN));
    colorInit.loadInitial(nvsGetU32(NVS_KEY_CLR_INIT, DEFAULT_CLR_INIT));
    colorReady.loadInitial(nvsGetU32(NVS_KEY_CLR_RDY, DEFAULT_CLR_READY));
    colorGateway.loadInitial(nvsGetU32(NVS_KEY_CLR_GW, DEFAULT_CLR_GATEWAY));
    colorPeer.loadInitial(nvsGetU32(NVS_KEY_CLR_PEER, DEFAULT_CLR_PEER));
    colorDisconnected.loadInitial(nvsGetU32(NVS_KEY_CLR_DISC, DEFAULT_CLR_DISCONNECTED));

    // Phase 2
    heartbeatInterval_s.loadInitial(nvsGetU32(NVS_KEY_HB_INT, DEFAULT_HB_INTERVAL_S));
    heartbeatStaleMultiplier.loadInitial(nvsGetU32(NVS_KEY_HB_STALE, DEFAULT_HB_STALE_MULT));
    reelectionBatteryDelta_mv.loadInitial(nvsGetU32(NVS_KEY_REEL_DMV, DEFAULT_REELECT_DELTA_MV));
    ftmStaleness_s.loadInitial(nvsGetU32(NVS_KEY_FTM_STALE, DEFAULT_FTM_STALE_S));
    ftmNewNodeAnchors.loadInitial(nvsGetU32(NVS_KEY_FTM_ANCH, DEFAULT_FTM_NEW_ANCHORS));
    ftmSamplesPerPair.loadInitial(nvsGetU32(NVS_KEY_FTM_SAMP, DEFAULT_FTM_SAMPLES));
    ftmPairTimeout_ms.loadInitial(nvsGetU32(NVS_KEY_FTM_TMO, DEFAULT_FTM_PAIR_TMO_MS));
    ftmSweepInterval_s.loadInitial(nvsGetU32(NVS_KEY_FTM_SWP, DEFAULT_FTM_SWEEP_INT_S));
    ftmKalmanProcessNoise.loadInitial(nvsGetFloat(NVS_KEY_FTM_KPN, DEFAULT_FTM_KALMAN_PN));
    ftmResponderOffset_cm.loadInitial(nvsGetU32(NVS_KEY_FTM_OFS, DEFAULT_FTM_RESP_OFS_CM));

    ESP_LOGI(TAG, "Config loaded from NVS");
}

bool NvsConfigManager::restoreFactoryDefault(uint32_t safeKey)
{
    if (safeKey != 0xBEEFF00D)
        return false;

    ESP_LOGW(TAG, "Restoring all settings to factory defaults");

    settingHash         = SETTINGS_HASH;
    ledsEnabled         = DEFAULT_LEDS_ENABLED;
    electWBattery       = DEFAULT_ELECT_W_BATTERY;
    electWAdjacency     = DEFAULT_ELECT_W_ADJACENCY;
    electWTenure        = DEFAULT_ELECT_W_TENURE;
    electWLowbatPenalty  = DEFAULT_ELECT_W_LOWBAT_PEN;
    colorInit          = DEFAULT_CLR_INIT;
    colorReady         = DEFAULT_CLR_READY;
    colorGateway       = DEFAULT_CLR_GATEWAY;
    colorPeer          = DEFAULT_CLR_PEER;
    colorDisconnected  = DEFAULT_CLR_DISCONNECTED;

    // Phase 2
    heartbeatInterval_s       = (uint32_t)DEFAULT_HB_INTERVAL_S;
    heartbeatStaleMultiplier  = (uint32_t)DEFAULT_HB_STALE_MULT;
    reelectionBatteryDelta_mv = (uint32_t)DEFAULT_REELECT_DELTA_MV;
    ftmStaleness_s            = (uint32_t)DEFAULT_FTM_STALE_S;
    ftmNewNodeAnchors         = (uint32_t)DEFAULT_FTM_NEW_ANCHORS;
    ftmSamplesPerPair         = (uint32_t)DEFAULT_FTM_SAMPLES;
    ftmPairTimeout_ms         = (uint32_t)DEFAULT_FTM_PAIR_TMO_MS;
    ftmSweepInterval_s        = (uint32_t)DEFAULT_FTM_SWEEP_INT_S;
    ftmKalmanProcessNoise     = DEFAULT_FTM_KALMAN_PN;
    ftmResponderOffset_cm     = (uint32_t)DEFAULT_FTM_RESP_OFS_CM;

    return true;
}
