#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "property_value.h"
#include "bsp.hpp"

// NVS keys
inline constexpr char NVS_KEY_SHASH[]  = "sHash";
inline constexpr char NVS_KEY_LEDSEN[] = "ledsEn";
inline constexpr char NVS_KEY_EW_BAT[] = "ewBat";
inline constexpr char NVS_KEY_EW_ADJ[] = "ewAdj";
inline constexpr char NVS_KEY_EW_TEN[] = "ewTen";
inline constexpr char NVS_KEY_EW_LBP[] = "ewLbp";
inline constexpr char NVS_KEY_DBGTMO[] = "dbgTmo";
inline constexpr char NVS_KEY_CLR_INIT[] = "clrInit";
inline constexpr char NVS_KEY_CLR_RDY[]  = "clrRdy";
inline constexpr char NVS_KEY_CLR_GW[]   = "clrGw";
inline constexpr char NVS_KEY_CLR_PEER[] = "clrPeer";
inline constexpr char NVS_KEY_CLR_DISC[] = "clrDisc";

// --- Default values (sourced from BSP defines for single-point maintenance) ---

inline constexpr bool     DEFAULT_LEDS_ENABLED       = NVS_DEFAULT_LEDS_ENABLED;
inline constexpr float    DEFAULT_ELECT_W_BATTERY    = NVS_DEFAULT_ELECT_W_BATTERY;
inline constexpr float    DEFAULT_ELECT_W_ADJACENCY  = NVS_DEFAULT_ELECT_W_ADJACENCY;
inline constexpr float    DEFAULT_ELECT_W_TENURE     = NVS_DEFAULT_ELECT_W_TENURE;
inline constexpr float    DEFAULT_ELECT_W_LOWBAT_PEN = NVS_DEFAULT_ELECT_W_LOWBAT_PEN;
inline constexpr uint32_t DEFAULT_DEBUG_TIMEOUT_MS   = NVS_DEFAULT_DEBUG_TIMEOUT_MS;
inline constexpr uint32_t DEFAULT_CLR_INIT           = NVS_DEFAULT_CLR_INIT;
inline constexpr uint32_t DEFAULT_CLR_READY          = NVS_DEFAULT_CLR_READY;
inline constexpr uint32_t DEFAULT_CLR_GATEWAY        = NVS_DEFAULT_CLR_GATEWAY;
inline constexpr uint32_t DEFAULT_CLR_PEER           = NVS_DEFAULT_CLR_PEER;
inline constexpr uint32_t DEFAULT_CLR_DISCONNECTED   = NVS_DEFAULT_CLR_DISCONNECTED;

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

    constexpr uint64_t fnvFloat(uint64_t hash, float v) {
        uint32_t bits = __builtin_bit_cast(uint32_t, v);
        hash = fnvByte(hash, (uint8_t)(bits >> 24));
        hash = fnvByte(hash, (uint8_t)(bits >> 16));
        hash = fnvByte(hash, (uint8_t)(bits >> 8));
        hash = fnvByte(hash, (uint8_t)(bits));
        return hash;
    }

    constexpr uint64_t fnvU32(uint64_t hash, uint32_t v) {
        hash = fnvByte(hash, (uint8_t)(v >> 24));
        hash = fnvByte(hash, (uint8_t)(v >> 16));
        hash = fnvByte(hash, (uint8_t)(v >> 8));
        hash = fnvByte(hash, (uint8_t)(v));
        return hash;
    }
}

namespace detail {
    constexpr uint64_t computeSettingsHash() {
        uint64_t h = FNV_OFFSET;
        h = fnvBool(h,  DEFAULT_LEDS_ENABLED);
        h = fnvFloat(h, DEFAULT_ELECT_W_BATTERY);
        h = fnvFloat(h, DEFAULT_ELECT_W_ADJACENCY);
        h = fnvFloat(h, DEFAULT_ELECT_W_TENURE);
        h = fnvFloat(h, DEFAULT_ELECT_W_LOWBAT_PEN);
        h = fnvU32(h,  DEFAULT_DEBUG_TIMEOUT_MS);
        h = fnvU32(h, DEFAULT_CLR_INIT);
        h = fnvU32(h, DEFAULT_CLR_READY);
        h = fnvU32(h, DEFAULT_CLR_GATEWAY);
        h = fnvU32(h, DEFAULT_CLR_PEER);
        h = fnvU32(h, DEFAULT_CLR_DISCONNECTED);
        return h;
    }
}

inline constexpr uint64_t SETTINGS_HASH = detail::computeSettingsHash();

// --- NvsConfigManager ---

class NvsConfigManager {
    NvsConfigManager() = delete;
private:
    /// FNV-1a hash of all members' compile-time defaults.
    /// Detects when firmware defaults have changed since last NVS write.
    static PropertyValue<NVS_KEY_SHASH, uint64_t, NvsConfigManager> settingHash;

public:
    static void begin();
    static void reloadFromNvs();

    /// Resets ALL NVS-backed members to their compile-time defaults.
    /// @param safeKey must equal 0xBEEFF00D or the call aborts immediately.
    /// @return true if reset was performed, false if safeKey was wrong.
    static bool restoreFactoryDefault(uint32_t safeKey);

    /// Whether LEDs (status + RGB) are enabled.
    static PropertyValue<NVS_KEY_LEDSEN, bool, NvsConfigManager>    ledsEnabled;

    // Election weight factors (tunable via NVS, see mesh_conductor.cpp)
    static PropertyValue<NVS_KEY_EW_BAT, float, NvsConfigManager>   electWBattery;
    static PropertyValue<NVS_KEY_EW_ADJ, float, NvsConfigManager>   electWAdjacency;
    static PropertyValue<NVS_KEY_EW_TEN, float, NvsConfigManager>   electWTenure;
    static PropertyValue<NVS_KEY_EW_LBP, float, NvsConfigManager>   electWLowbatPenalty;

    /// Debug menu marquee timeout in ms (0 = infinite).
    static PropertyValue<NVS_KEY_DBGTMO, uint32_t, NvsConfigManager> debugTimeout_ms;

    // Mesh status LED colors (packed as 0x00RRGGBB)
    static PropertyValue<NVS_KEY_CLR_INIT, uint32_t, NvsConfigManager> colorInit;
    static PropertyValue<NVS_KEY_CLR_RDY,  uint32_t, NvsConfigManager> colorReady;
    static PropertyValue<NVS_KEY_CLR_GW,   uint32_t, NvsConfigManager> colorGateway;
    static PropertyValue<NVS_KEY_CLR_PEER, uint32_t, NvsConfigManager> colorPeer;
    static PropertyValue<NVS_KEY_CLR_DISC, uint32_t, NvsConfigManager> colorDisconnected;
};

#endif // NVS_CONFIG_H
