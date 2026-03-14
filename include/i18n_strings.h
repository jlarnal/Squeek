#ifndef I18N_STRINGS_H
#define I18N_STRINGS_H

#include <stdint.h>

/// Supported languages (index into null-terminated PropStrings arrays).
enum class Lang : uint8_t {
    EN = 0,   // English (default / fallback)
    FR,       // French
    ES,       // Spanish
    DE,       // German
};

/// Null-terminated arrays of localized name and description strings.
struct PropStrings {
    const char* const* name;
    const char* const* description;
};

/// Safely index into a null-terminated string array.
/// Returns arr[lang] if in bounds, or arr[0] (English) as fallback.
inline const char* i18nGet(const char* const* arr, Lang lang) {
    const uint8_t idx = static_cast<uint8_t>(lang);
    for (uint8_t i = 0; i <= idx; i++) {
        if (arr[i] == nullptr) return arr[0];
    }
    return arr[idx];
}

// --- PropStrings for each NVS property ---

extern const PropStrings I18N_SETTINGS_HASH;
extern const PropStrings I18N_LEDS_ENABLED;
extern const PropStrings I18N_COLOR_INIT;
extern const PropStrings I18N_COLOR_READY;
extern const PropStrings I18N_COLOR_GATEWAY;
extern const PropStrings I18N_COLOR_PEER;
extern const PropStrings I18N_COLOR_DISCONNECTED;
extern const PropStrings I18N_HEARTBEAT_INTERVAL;
extern const PropStrings I18N_HEARTBEAT_STALE;
extern const PropStrings I18N_REELECTION_COOLDOWN;
extern const PropStrings I18N_BATTERY_HYSTERESIS;
extern const PropStrings I18N_FTM_ANCHORS;
extern const PropStrings I18N_FTM_SAMPLES;
extern const PropStrings I18N_FTM_TIMEOUT;
extern const PropStrings I18N_FTM_KALMAN_NOISE;
extern const PropStrings I18N_FTM_OFFSET;
extern const PropStrings I18N_ORCH_MODE;
extern const PropStrings I18N_ORCH_TRAVEL_DELAY;
extern const PropStrings I18N_ORCH_RANDOM_MIN;
extern const PropStrings I18N_ORCH_RANDOM_MAX;
extern const PropStrings I18N_ORCH_TONE_INDEX;
extern const PropStrings I18N_CLOCK_SYNC_INTERVAL;
extern const PropStrings I18N_WEB_ENABLED;
extern const PropStrings I18N_FAST_SCAN_DELAY;
extern const PropStrings I18N_DELEGATE_TIMEOUT;
extern const PropStrings I18N_RSSI_DECAY_K;
extern const PropStrings I18N_BATTERY_IN_TENURE;
extern const PropStrings I18N_ELECTION_SLOT;
extern const PropStrings I18N_ELECTION_ANNOUNCE;

#endif // I18N_STRINGS_H
