#ifndef MESH_CONDUCTOR_H
#define MESH_CONDUCTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

// --- Message types for mesh data exchange ---

enum MeshMsgType : uint8_t {
    MSG_TYPE_ELECTION    = 0x01,
    MSG_TYPE_HEARTBEAT   = 0x10,   // peer → gateway
    MSG_TYPE_FTM_WAKE    = 0x20,   // gateway → pair
    MSG_TYPE_FTM_READY   = 0x21,   // node → gateway
    MSG_TYPE_FTM_GO      = 0x22,   // gateway → initiator
    MSG_TYPE_FTM_RESULT  = 0x23,   // initiator → gateway
    MSG_TYPE_FTM_CANCEL  = 0x24,   // gateway → pair (abort)
    MSG_TYPE_POS_UPDATE  = 0x30,   // gateway → all
    MSG_TYPE_PEER_SYNC   = 0x31,   // gateway → all (peer table broadcast)
    MSG_TYPE_NOMINATE    = 0x40,   // peer → gateway (request gateway role)
    MSG_TYPE_CONFIG_REQ  = 0x50,   // any node → target node
    MSG_TYPE_CONFIG_RESP = 0x51,   // target node → requester
    MSG_TYPE_ROLE_CHANGE = 0x60,   // gateway → all (new gateway MAC)
    MSG_TYPE_PLAY_CMD    = 0x70,   // gateway → node: play tone
    MSG_TYPE_ORCH_MODE   = 0x71,   // gateway → all: mode changed
    MSG_TYPE_CLOCK_SYNC  = 0x72,   // gateway → all: time sync
};

// --- Election score broadcast packet ---

struct __attribute__((packed)) ElectionScore {
    uint8_t  type;              // MSG_TYPE_ELECTION
    uint8_t  mac[6];
    uint16_t battery_mv;
    uint8_t  peer_count;        // number of peers this node can see
    uint16_t gateway_tenure;    // times this node has been gateway (from NVS)
    double   score;             // pre-computed score (double for overflow safety)
};

// --- Heartbeat message (peer → gateway) ---

struct __attribute__((packed)) HeartbeatMsg {
    uint8_t  type;           // MSG_TYPE_HEARTBEAT
    uint8_t  mac[6];         // STA MAC
    uint16_t battery_mv;
    uint8_t  flags;          // awake/sleeping/low-battery
    uint8_t  softap_mac[6];  // SoftAP MAC (for FTM targeting)
};

// --- FTM protocol messages ---

struct __attribute__((packed)) FtmWakeMsg {
    uint8_t  type;           // MSG_TYPE_FTM_WAKE
    uint8_t  initiator[6];   // STA MAC of initiator
    uint8_t  responder[6];   // STA MAC of responder
    uint8_t  responder_ap[6]; // SoftAP MAC of responder (FTM target)
};

struct __attribute__((packed)) FtmReadyMsg {
    uint8_t  type;           // MSG_TYPE_FTM_READY
    uint8_t  mac[6];         // STA MAC of node reporting ready
};

struct __attribute__((packed)) FtmGoMsg {
    uint8_t  type;           // MSG_TYPE_FTM_GO
    uint8_t  target_ap[6];   // SoftAP MAC of responder to range against
    uint8_t  samples;        // number of FTM frames per burst
};

struct __attribute__((packed)) FtmResultMsg {
    uint8_t  type;           // MSG_TYPE_FTM_RESULT
    uint8_t  initiator[6];   // STA MAC of initiator
    uint8_t  responder[6];   // STA MAC of responder
    float    distance_cm;    // measured distance in cm (-1 = failed)
    uint8_t  status;         // 0 = ok, 1 = timeout, 2 = refused
};

struct __attribute__((packed)) FtmCancelMsg {
    uint8_t  type;           // MSG_TYPE_FTM_CANCEL
};

struct __attribute__((packed)) PosUpdateEntry {
    uint8_t  mac[6];
    float    x, y, z;        // position in cm
    float    confidence;
};

struct __attribute__((packed)) PosUpdateMsg {
    uint8_t  type;           // MSG_TYPE_POS_UPDATE
    uint8_t  dimension;      // 1=distance, 2=2D, 3=3D
    uint8_t  count;          // number of entries following
    // followed by count × PosUpdateEntry
};

// --- Peer sync message (gateway → all) ---

struct __attribute__((packed)) PeerSyncEntry {
    uint8_t  mac[6];
    uint8_t  softap_mac[6];
    uint16_t battery_mv;
    uint8_t  flags;
};
// 15 bytes per entry

struct __attribute__((packed)) PeerSyncMsg {
    uint8_t type;    // MSG_TYPE_PEER_SYNC
    uint8_t count;
    // followed by count × PeerSyncEntry
};
// 2 + 16×15 = 242 bytes max (fits 256-byte rx_buf)

// --- Nominate message (peer → gateway) ---

struct __attribute__((packed)) NominateMsg {
    uint8_t type;    // MSG_TYPE_NOMINATE
    uint8_t mac[6];  // STA MAC of node requesting gateway role
};

// --- Role change message (gateway → all) ---

struct __attribute__((packed)) RoleChangeMsg {
    uint8_t type;        // MSG_TYPE_ROLE_CHANGE
    uint8_t new_gw[6];   // STA MAC of new gateway
};

// --- Phase 4: Orchestrator messages ---

struct __attribute__((packed)) PlayCmdMsg {
    uint8_t  type;           // MSG_TYPE_PLAY_CMD
    uint8_t  tone_index;     // ToneLibrary index
};

struct __attribute__((packed)) OrchModeMsg {
    uint8_t  type;           // MSG_TYPE_ORCH_MODE
    uint8_t  mode;           // OrchMode enum value
};

struct __attribute__((packed)) ClockSyncMsg {
    uint8_t  type;           // MSG_TYPE_CLOCK_SYNC
    uint32_t gateway_ms;     // gateway's millis()
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
    static double computeScore();
    static void runElection();

    // Messaging
    static esp_err_t sendToRoot(const void* data, uint16_t len);
    static esp_err_t sendToNode(const uint8_t* sta_mac, const void* data, uint16_t len);
    static esp_err_t broadcastToAll(const void* data, uint16_t len);

    // Peer shadow (non-gateway nodes)
    static void printPeerShadow();
    static uint8_t peerShadowCount();
    static const PeerSyncEntry* peerShadowEntries();

    // Gateway MAC tracking (for heartbeat routing)
    static const uint8_t* gatewayMac();
    static void setGatewayMac(const uint8_t* mac);

    // Role nomination
    static void nominateNode(const uint8_t* sta_mac);  // gateway only
    static void stepDown();                              // gateway only

    // Remote config
    static bool sendConfigReq(const uint8_t* sta_mac, const char* json, uint8_t reqId);
    static bool waitConfigResp(char* outBuf, size_t bufSize, uint32_t timeout_ms);

    // Debug
    static void forceReelection();

private:
    MeshConductor() = delete;
};

#endif // MESH_CONDUCTOR_H
