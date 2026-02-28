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
inline constexpr char NVS_KEY_CLR_INIT[] = "clrInit";
inline constexpr char NVS_KEY_CLR_RDY[]  = "clrRdy";
inline constexpr char NVS_KEY_CLR_GW[]   = "clrGw";
inline constexpr char NVS_KEY_CLR_PEER[] = "clrPeer";
inline constexpr char NVS_KEY_CLR_DISC[] = "clrDisc";

// Phase 2: Heartbeat & re-election
inline constexpr char NVS_KEY_HB_INT[]    = "hbInt";
inline constexpr char NVS_KEY_HB_STALE[]  = "hbStale";
inline constexpr char NVS_KEY_REEL_DMV[]  = "reelDmv";
inline constexpr char NVS_KEY_REEL_CD[]   = "reelCd";
inline constexpr char NVS_KEY_REEL_DTH[]  = "reelDth";

// Phase 4: Orchestrator
inline constexpr char NVS_KEY_ORCH_MODE[]  = "orchMode";
inline constexpr char NVS_KEY_ORCH_TRVD[]  = "orchTrvD";
inline constexpr char NVS_KEY_ORCH_RMIN[]  = "orchRMin";
inline constexpr char NVS_KEY_ORCH_RMAX[]  = "orchRMax";
inline constexpr char NVS_KEY_ORCH_TONE[]  = "orchTone";
inline constexpr char NVS_KEY_CSYNC_INT[]  = "csyncInt";

// Phase 2: FTM
inline constexpr char NVS_KEY_FTM_STALE[] = "ftmStale";
inline constexpr char NVS_KEY_FTM_ANCH[]  = "ftmAnch";
inline constexpr char NVS_KEY_FTM_SAMP[]  = "ftmSamp";
inline constexpr char NVS_KEY_FTM_TMO[]   = "ftmTmo";
inline constexpr char NVS_KEY_FTM_SWP[]   = "ftmSwp";
inline constexpr char NVS_KEY_FTM_KPN[]   = "ftmKpn";
inline constexpr char NVS_KEY_FTM_OFS[]   = "ftmOfs";

// --- Default values (sourced from BSP defines for single-point maintenance) ---

inline constexpr bool     DEFAULT_LEDS_ENABLED       = NVS_DEFAULT_LEDS_ENABLED;
inline constexpr float    DEFAULT_ELECT_W_BATTERY    = NVS_DEFAULT_ELECT_W_BATTERY;
inline constexpr float    DEFAULT_ELECT_W_ADJACENCY  = NVS_DEFAULT_ELECT_W_ADJACENCY;
inline constexpr float    DEFAULT_ELECT_W_TENURE     = NVS_DEFAULT_ELECT_W_TENURE;
inline constexpr float    DEFAULT_ELECT_W_LOWBAT_PEN = NVS_DEFAULT_ELECT_W_LOWBAT_PEN;
inline constexpr uint32_t DEFAULT_CLR_INIT           = NVS_DEFAULT_CLR_INIT;
inline constexpr uint32_t DEFAULT_CLR_READY          = NVS_DEFAULT_CLR_READY;
inline constexpr uint32_t DEFAULT_CLR_GATEWAY        = NVS_DEFAULT_CLR_GATEWAY;
inline constexpr uint32_t DEFAULT_CLR_PEER           = NVS_DEFAULT_CLR_PEER;
inline constexpr uint32_t DEFAULT_CLR_DISCONNECTED   = NVS_DEFAULT_CLR_DISCONNECTED;

// Phase 2: Heartbeat & re-election defaults
inline constexpr uint32_t DEFAULT_HB_INTERVAL_S      = NVS_DEFAULT_HB_INTERVAL_S;
inline constexpr uint8_t  DEFAULT_HB_STALE_MULT      = NVS_DEFAULT_HB_STALE_MULT;
inline constexpr uint16_t DEFAULT_REELECT_DELTA_MV   = NVS_DEFAULT_REELECT_DELTA_MV;
inline constexpr uint16_t DEFAULT_REELECT_COOLDOWN_S = NVS_DEFAULT_REELECT_COOLDOWN_S;
inline constexpr uint16_t DEFAULT_REELECT_DETHRONE_MV = NVS_DEFAULT_REELECT_DETHRONE_MV;

// Phase 4: Orchestrator defaults
inline constexpr uint32_t DEFAULT_ORCH_MODE           = NVS_DEFAULT_ORCH_MODE;
inline constexpr uint32_t DEFAULT_ORCH_TRAVEL_DELAY   = NVS_DEFAULT_ORCH_TRAVEL_DELAY;
inline constexpr uint32_t DEFAULT_ORCH_RANDOM_MIN     = NVS_DEFAULT_ORCH_RANDOM_MIN;
inline constexpr uint32_t DEFAULT_ORCH_RANDOM_MAX     = NVS_DEFAULT_ORCH_RANDOM_MAX;
inline constexpr uint32_t DEFAULT_ORCH_TONE_INDEX     = NVS_DEFAULT_ORCH_TONE_INDEX;
inline constexpr uint32_t DEFAULT_CSYNC_INTERVAL_S    = NVS_DEFAULT_CSYNC_INTERVAL_S;

// Phase 2: FTM defaults
inline constexpr uint32_t DEFAULT_FTM_STALE_S        = NVS_DEFAULT_FTM_STALE_S;
inline constexpr uint8_t  DEFAULT_FTM_NEW_ANCHORS    = NVS_DEFAULT_FTM_NEW_ANCHORS;
inline constexpr uint8_t  DEFAULT_FTM_SAMPLES        = NVS_DEFAULT_FTM_SAMPLES;
inline constexpr uint32_t DEFAULT_FTM_PAIR_TMO_MS    = NVS_DEFAULT_FTM_PAIR_TMO_MS;
inline constexpr uint32_t DEFAULT_FTM_SWEEP_INT_S    = NVS_DEFAULT_FTM_SWEEP_INT_S;
inline constexpr float    DEFAULT_FTM_KALMAN_PN      = NVS_DEFAULT_FTM_KALMAN_PN;
inline constexpr int16_t  DEFAULT_FTM_RESP_OFS_CM    = NVS_DEFAULT_FTM_RESP_OFS_CM;

// --- Compile-time settings hash (FNV-1a) ---
//     Changes automatically when any default value above is modified.

namespace nvs_detail {
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

namespace nvs_detail {
    constexpr uint64_t computeSettingsHash() {
        uint64_t h = FNV_OFFSET;
        h = fnvBool(h,  DEFAULT_LEDS_ENABLED);
        h = fnvFloat(h, DEFAULT_ELECT_W_BATTERY);
        h = fnvFloat(h, DEFAULT_ELECT_W_ADJACENCY);
        h = fnvFloat(h, DEFAULT_ELECT_W_TENURE);
        h = fnvFloat(h, DEFAULT_ELECT_W_LOWBAT_PEN);
        h = fnvU32(h, DEFAULT_CLR_INIT);
        h = fnvU32(h, DEFAULT_CLR_READY);
        h = fnvU32(h, DEFAULT_CLR_GATEWAY);
        h = fnvU32(h, DEFAULT_CLR_PEER);
        h = fnvU32(h, DEFAULT_CLR_DISCONNECTED);
        // Phase 2
        h = fnvU32(h, DEFAULT_HB_INTERVAL_S);
        h = fnvByte(h, DEFAULT_HB_STALE_MULT);
        h = fnvU32(h, (uint32_t)DEFAULT_REELECT_DELTA_MV);
        h = fnvU32(h, (uint32_t)DEFAULT_REELECT_COOLDOWN_S);
        h = fnvU32(h, (uint32_t)DEFAULT_REELECT_DETHRONE_MV);
        h = fnvU32(h, DEFAULT_FTM_STALE_S);
        h = fnvByte(h, DEFAULT_FTM_NEW_ANCHORS);
        h = fnvByte(h, DEFAULT_FTM_SAMPLES);
        h = fnvU32(h, DEFAULT_FTM_PAIR_TMO_MS);
        h = fnvU32(h, DEFAULT_FTM_SWEEP_INT_S);
        h = fnvFloat(h, DEFAULT_FTM_KALMAN_PN);
        h = fnvU32(h, (uint32_t)(uint16_t)DEFAULT_FTM_RESP_OFS_CM);
        // Phase 4
        h = fnvU32(h, DEFAULT_ORCH_MODE);
        h = fnvU32(h, DEFAULT_ORCH_TRAVEL_DELAY);
        h = fnvU32(h, DEFAULT_ORCH_RANDOM_MIN);
        h = fnvU32(h, DEFAULT_ORCH_RANDOM_MAX);
        h = fnvU32(h, DEFAULT_ORCH_TONE_INDEX);
        h = fnvU32(h, DEFAULT_CSYNC_INTERVAL_S);
        return h;
    }
}

inline constexpr uint64_t SETTINGS_HASH = nvs_detail::computeSettingsHash();

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

    // Mesh status LED colors (packed as 0x00RRGGBB)
    static PropertyValue<NVS_KEY_CLR_INIT, uint32_t, NvsConfigManager> colorInit;
    static PropertyValue<NVS_KEY_CLR_RDY,  uint32_t, NvsConfigManager> colorReady;
    static PropertyValue<NVS_KEY_CLR_GW,   uint32_t, NvsConfigManager> colorGateway;
    static PropertyValue<NVS_KEY_CLR_PEER, uint32_t, NvsConfigManager> colorPeer;
    static PropertyValue<NVS_KEY_CLR_DISC, uint32_t, NvsConfigManager> colorDisconnected;

    // Phase 2: Heartbeat & re-election
    static PropertyValue<NVS_KEY_HB_INT,   uint32_t, NvsConfigManager> heartbeatInterval_s;
    static PropertyValue<NVS_KEY_HB_STALE, uint32_t, NvsConfigManager> heartbeatStaleMultiplier;
    static PropertyValue<NVS_KEY_REEL_DMV, uint32_t, NvsConfigManager> reelectionBatteryDelta_mv;
    static PropertyValue<NVS_KEY_REEL_CD,  uint16_t, NvsConfigManager> reelectionCooldown_s;
    static PropertyValue<NVS_KEY_REEL_DTH, uint16_t, NvsConfigManager> reelectionDethrone_mv;

    // Phase 2: FTM
    static PropertyValue<NVS_KEY_FTM_STALE, uint32_t, NvsConfigManager> ftmStaleness_s;
    static PropertyValue<NVS_KEY_FTM_ANCH,  uint32_t, NvsConfigManager> ftmNewNodeAnchors;
    static PropertyValue<NVS_KEY_FTM_SAMP,  uint32_t, NvsConfigManager> ftmSamplesPerPair;
    static PropertyValue<NVS_KEY_FTM_TMO,   uint32_t, NvsConfigManager> ftmPairTimeout_ms;
    static PropertyValue<NVS_KEY_FTM_SWP,   uint32_t, NvsConfigManager> ftmSweepInterval_s;
    static PropertyValue<NVS_KEY_FTM_KPN,   float,    NvsConfigManager> ftmKalmanProcessNoise;
    static PropertyValue<NVS_KEY_FTM_OFS,   uint32_t, NvsConfigManager> ftmResponderOffset_cm;

    // Phase 4: Orchestrator
    static PropertyValue<NVS_KEY_ORCH_MODE, uint32_t, NvsConfigManager> orchMode;
    static PropertyValue<NVS_KEY_ORCH_TRVD, uint32_t, NvsConfigManager> orchTravelDelay_ms;
    static PropertyValue<NVS_KEY_ORCH_RMIN, uint32_t, NvsConfigManager> orchRandomMin_ms;
    static PropertyValue<NVS_KEY_ORCH_RMAX, uint32_t, NvsConfigManager> orchRandomMax_ms;
    static PropertyValue<NVS_KEY_ORCH_TONE, uint32_t, NvsConfigManager> orchToneIndex;
    static PropertyValue<NVS_KEY_CSYNC_INT, uint32_t, NvsConfigManager> clockSyncInterval_s;
};

#endif // NVS_CONFIG_H
