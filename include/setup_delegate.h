#ifndef SETUP_DELEGATE_H
#define SETUP_DELEGATE_H

#include <stdint.h>

class SetupDelegate {
public:
    SetupDelegate() = delete;

    /// Enter Setup Delegate mode: leave mesh, start Squeek_Config_XXYY SoftAP
    static void begin(const uint8_t gatewayMac[6]);

    /// Leave delegate mode: tear down SoftAP, rejoin mesh, push creds + merge check
    static void end();

    /// Is this node currently in Setup Delegate mode?
    static bool isActive();

    /// Called by WiFi wizard when user submits credentials.
    /// Attempts router connection; on success saves creds, calls end().
    /// Returns true if connection succeeded.
    static bool onCredsSubmitted(const char* ssid, const char* pass);

private:
    static void startSoftAP(const uint8_t gatewayMac[6]);
    static void stopSoftAP();
    static void registerWizardRoutes();
    static void pushCredsToMesh();
};

#endif // SETUP_DELEGATE_H
