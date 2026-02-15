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

    return true;
}
