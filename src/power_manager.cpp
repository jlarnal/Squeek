#include "power_manager.h"
#include "bsp.hpp"
#include <Arduino.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static bool cali_available = false;

void PowerManager::init() {
    if (adc_handle != NULL) return;   // already initialized
    // Configure ADC oneshot unit
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    // Configure channel
    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg));

    // Try curve fitting calibration
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
        cali_available = true;
    }
}

uint32_t PowerManager::batteryRaw() {
    int raw = 0;
    adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw);
    return (uint32_t)raw;
}

uint32_t PowerManager::batteryMv() {
    int raw = 0;
    adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw);

    if (cali_available) {
        int voltage_mv = 0;
        adc_cali_raw_to_voltage(cali_handle, raw, &voltage_mv);
        return (uint32_t)(voltage_mv * VDIV_RATIO);
    }

    // Fallback: approximate conversion (12-bit, 0-3.3V with 12dB atten)
    uint32_t adc_mv = (uint32_t)(raw * 3300.0f / 4095.0f);
    return (uint32_t)(adc_mv * VDIV_RATIO);
}

bool PowerManager::isLowBattery() {
    return batteryMv() < BATTERY_LOW_MV;
}

bool PowerManager::isCriticalBattery() {
    return batteryMv() < BATTERY_CRITICAL_MV;
}

void PowerManager::lightSleep(uint32_t seconds) {
    SQ_LIGHT_SLEEP(seconds * 1000UL);
}

void PowerManager::deepSleep(uint32_t seconds) {
    SQ_DEEP_SLEEP(seconds * 1000UL);
}
