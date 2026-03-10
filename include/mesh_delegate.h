#ifndef MESH_DELEGATE_H
#define MESH_DELEGATE_H

#include "mesh_conductor.h"

class Delegate : public IMeshRole {
public:
    void begin() override;
    void end() override;
    void onPeerJoined(const uint8_t* mac) override;
    void onPeerLeft(const uint8_t* mac) override;
    RoleId roleId() const override { return RoleId::DELEGATE; }
    void printStatus() override;

    bool onCredsSubmitted(const char* ssid, const char* pass);

    static bool hasClient();
};

#endif // MESH_DELEGATE_H
