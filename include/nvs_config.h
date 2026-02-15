#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "property_value.h"

// NVS keys
inline constexpr char NVS_KEY_SHASH[]  = "sHash";
inline constexpr char NVS_KEY_LEDSEN[] = "ledsEn";

// --- Default values (extend this list as members grow) ---

inline constexpr bool DEFAULT_LEDS_ENABLED = true;

// --- Compile-time settings hash (FNV-1a) ---
//     Changes automatically when any default value above is modified.

namespace detail {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;

    constexpr uint64_t fnvByte(uint64_t hash, uint8_t byte) {
        return (hash ^ byte) * FNV_PRIME;
    }

    constexpr uint64_t fnvBool(uint64_t hash, bool v) {
        return fnvByte(hash, v ? 1 : 0);
    }
}

inline constexpr uint64_t SETTINGS_HASH =
    detail::fnvBool(detail::FNV_OFFSET, DEFAULT_LEDS_ENABLED);

// --- NvsConfigManager ---

class NvsConfigManager {
    NvsConfigManager() = delete;

public:
    static void begin();
    static void reloadFromNvs();

    /// FNV-1a hash of all members' compile-time defaults.
    /// Detects when firmware defaults have changed since last NVS write.
    static PropertyValue<NVS_KEY_SHASH, uint64_t, NvsConfigManager> settingHash;

    /// Whether LEDs (status + RGB) are enabled.
    static PropertyValue<NVS_KEY_LEDSEN, bool, NvsConfigManager>    ledsEnabled;
};

#endif // NVS_CONFIG_H
