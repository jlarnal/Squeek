#ifndef MESH_CONDUCTOR_H
#define MESH_CONDUCTOR_H

#include <stdint.h>
#include <stdbool.h>

// --- Message types for mesh data exchange ---

enum MeshMsgType : uint8_t {
    MSG_TYPE_ELECTION = 0x01,
};

// --- Election score broadcast packet ---

struct __attribute__((packed)) ElectionScore {
    uint8_t  type;              // MSG_TYPE_ELECTION
    uint8_t  mac[6];
    uint16_t battery_mv;
    uint8_t  peer_count;        // number of peers this node can see
    uint16_t gateway_tenure;    // times this node has been gateway (from NVS)
    uint32_t score;             // pre-computed score
};

// --- IMeshRole abstract interface ---

class IMeshRole {
public:
    virtual ~IMeshRole() = default;
    virtual void begin() = 0;
    virtual void end() = 0;
    virtual void onPeerJoined(const uint8_t* mac) = 0;
    virtual void onPeerLeft(const uint8_t* mac) = 0;
    virtual bool isGateway() const = 0;
    virtual void printStatus() = 0;
};

// --- Gateway role ---

class Gateway : public IMeshRole {
public:
    void begin() override;
    void end() override;
    void onPeerJoined(const uint8_t* mac) override;
    void onPeerLeft(const uint8_t* mac) override;
    bool isGateway() const override { return true; }
    void printStatus() override;
private:
    uint8_t m_peerCount = 0;
};

// --- MeshNode role ---

class MeshNode : public IMeshRole {
public:
    void begin() override;
    void end() override;
    void onPeerJoined(const uint8_t* mac) override;
    void onPeerLeft(const uint8_t* mac) override;
    bool isGateway() const override { return false; }
    void printStatus() override;
    void onGatewayLost();
private:
    bool m_gatewayAlive = true;
};

// --- MeshConductor static orchestrator ---

class MeshConductor {
public:
    static void init();
    static void start();
    static void stop();
    static bool isConnected();
    static bool isGateway();
    static IMeshRole* role();
    static void printStatus();

    // Election
    static uint32_t computeScore();
    static void runElection();

    // Debug
    static void forceReelection();

private:
    MeshConductor() = delete;
};

#endif // MESH_CONDUCTOR_H
