#ifndef FTM_MANAGER_H
#define FTM_MANAGER_H

#include <stdint.h>
#include <esp_err.h>

// FTM session result callback
using FtmResultCb = void(*)(const uint8_t* responder_mac, float distance_cm, uint8_t status);

class FtmManager {
public:
    static void init();

    /// Initiate an FTM ranging session to a target SoftAP.
    /// @param target_ap_mac  6-byte SoftAP BSSID of the responder
    /// @param channel        WiFi channel the responder is on
    /// @param samples        number of FTM frames per burst
    /// @return distance in cm on success, -1.0f on failure
    static float initiateSession(const uint8_t* target_ap_mac, uint8_t channel, uint8_t samples);

    /// Set FTM responder offset for calibration (called once at init).
    static void setResponderOffset(int16_t offset_cm);

    /// Handle FTM_WAKE message — prepare for upcoming session
    static void onFtmWake(const uint8_t* initiator_mac, const uint8_t* responder_mac,
                          const uint8_t* responder_ap_mac);

    /// Handle FTM_GO message — start ranging to the given target
    static void onFtmGo(const uint8_t* target_ap_mac, uint8_t samples);

    /// Check if an FTM session is currently in progress
    static bool isBusy();

private:
    FtmManager() = delete;
};

#endif // FTM_MANAGER_H
