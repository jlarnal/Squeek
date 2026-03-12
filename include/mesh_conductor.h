#ifndef MESH_CONDUCTOR_H
#define MESH_CONDUCTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

// --- Message types for mesh data exchange ---

enum MeshMsgType : uint8_t {
    MSG_TYPE_HEARTBEAT   = 0x10,   // peer → gateway
    MSG_TYPE_FTM_WAKE    = 0x20,   // gateway → pair
    MSG_TYPE_FTM_READY   = 0x21,   // node → gateway
    MSG_TYPE_FTM_GO      = 0x22,   // gateway → initiator
    MSG_TYPE_FTM_RESULT  = 0x23,   // initiator → gateway
    MSG_TYPE_FTM_CANCEL  = 0x24,   // gateway → pair (abort)
    MSG_TYPE_POS_UPDATE  = 0x30,   // gateway → all
    MSG_TYPE_PEER_SYNC   = 0x31,   // gateway → all (peer table broadcast)
    MSG_TYPE_CONFIG_REQ  = 0x50,   // any node → target node
    MSG_TYPE_CONFIG_RESP = 0x51,   // target node → requester
    MSG_TYPE_PLAY_CMD    = 0x70,   // gateway → node: play tone
    MSG_TYPE_ORCH_MODE   = 0x71,   // gateway → all: mode changed
    MSG_TYPE_CLOCK_SYNC  = 0x72,   // gateway → all: time sync
    // Phase 5: Setup Delegate
    MSG_TYPE_WIFI_CREDS      = 0x80,  // delegate → gateway, gateway → peers
    MSG_TYPE_WIFI_CREDS_ACK  = 0x81,  // receiver → sender
    MSG_TYPE_MERGE_CHECK     = 0x82,  // delegate → broadcast: split-mesh healing
    MSG_TYPE_SETUP_DELEGATE  = 0x83,  // gateway → peer: designate as delegate
    MSG_TYPE_DELEGATE_RESULT = 0x84,  // peer → gateway: delegation outcome
    MSG_TYPE_SCAN_REQUEST    = 0x85,  // gateway → all: "scan WiFi and report"
    MSG_TYPE_SCAN_RESULT     = 0x86,  // peer → gateway: scan result (ssid count)
    MSG_TYPE_DELEGATE_TICKET = 0x87,  // old gateway → new gateway: delegate tracking
    MSG_TYPE_DELEGATE_TICKET_ACK = 0x88,  // new gateway → old gateway
    // Credential exchange
    MSG_TYPE_CRED_OFFER  = 0x90,  // gateway → peer: all known credentials
    MSG_TYPE_CRED_REPLY  = 0x91,  // peer → gateway: merged creds + tenure score
};

// --- Heartbeat message (peer → gateway) ---

struct __attribute__((packed)) HeartbeatMsg {
    uint8_t  type;           // MSG_TYPE_HEARTBEAT
    uint8_t  mac[6];         // STA MAC
    uint16_t battery_mv;
    uint8_t  flags;          // awake/sleeping/low-battery
    uint8_t  softap_mac[6];  // SoftAP MAC (for FTM targeting)
    int8_t   router_rssi;    // best known router RSSI (-128 = none)
    uint16_t tenure_score;   // computed tenure fitness score
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

// --- Phase 5: Setup Delegate messages ---

struct __attribute__((packed)) WifiCredsMsg {
    uint8_t type;           // MSG_TYPE_WIFI_CREDS
    char    ssid[33];       // null-terminated, max 32 chars
    char    password[65];   // null-terminated, max 64 chars
};

struct __attribute__((packed)) WifiCredsAckMsg {
    uint8_t type;           // MSG_TYPE_WIFI_CREDS_ACK
};

struct __attribute__((packed)) MergeCheckMsg {
    uint8_t type;            // MSG_TYPE_MERGE_CHECK
    uint8_t root_table_size; // routing table size of the sender
};

struct __attribute__((packed)) SetupDelegateMsg {
    uint8_t type;            // MSG_TYPE_SETUP_DELEGATE
    uint8_t gateway_mac[6];  // gateway MAC — last 2 bytes used for SSID discriminator
};

struct __attribute__((packed)) DelegateResultMsg {
    uint8_t type;            // MSG_TYPE_DELEGATE_RESULT
    uint8_t success;         // nonzero = creds obtained
};

struct __attribute__((packed)) ScanRequestMsg {
    uint8_t type;            // MSG_TYPE_SCAN_REQUEST
};

struct __attribute__((packed)) ScanResultMsg {
    uint8_t type;            // MSG_TYPE_SCAN_RESULT
    uint8_t mac[6];          // STA MAC of reporting peer
    uint8_t ssid_count;      // number of unique SSIDs found
};

struct __attribute__((packed)) DelegateTicketMsg {
    uint8_t  type;           // MSG_TYPE_DELEGATE_TICKET
    uint8_t  delegate_mac[6];
    uint16_t remaining_s;    // monotonic countdown, floors at zero
};

// --- Credential exchange messages ---

struct __attribute__((packed)) CredOfferMsg {
    uint8_t type;           // MSG_TYPE_CRED_OFFER
    uint8_t count;          // number of cred entries following
    // followed by count × CredWireEntry (from credential_table.h)
    // total payload written by CredentialTable::toBuffer()
};

struct __attribute__((packed)) CredReplyMsg {
    uint8_t  type;           // MSG_TYPE_CRED_REPLY
    uint8_t  count;          // number of cred entries following
    uint16_t tenure_score;   // peer's computed tenure score
    // followed by count × CredReplyEntry (from credential_table.h)
};

// --- Tenure score computation (RAM-only, never persisted) ---
uint16_t computeTenureScore(int8_t best_rssi_dBm);

// --- Role identifier ---

enum class RoleId : uint8_t { PEER = 0, GATEWAY = 1, DELEGATE = 2 };

// --- IMeshRole abstract interface ---

class IMeshRole {
public:
    virtual ~IMeshRole() = default;
    virtual void begin() = 0;
    virtual void end() = 0;
    virtual void onPeerJoined(const uint8_t* mac) = 0;
    virtual void onPeerLeft(const uint8_t* mac) = 0;
    virtual RoleId roleId() const = 0;
    virtual void printStatus() = 0;
};

// --- Gateway role ---

class Gateway : public IMeshRole {
public:
    void begin() override;
    void end() override;
    void onPeerJoined(const uint8_t* mac) override;
    void onPeerLeft(const uint8_t* mac) override;
    RoleId roleId() const override { return RoleId::GATEWAY; }
    void printStatus() override;
    void startDelegation();           // button-triggered: scan contest or self-delegate
    void onScanResult(const uint8_t* mac, uint8_t ssid_count);
    void trackPeerTenure(uint16_t tenure);  // track best peer tenure from heartbeats
    bool hasDelegateTicket() const;   // true if a delegate is out (remaining_s > 0)
    void installTicket(const uint8_t* delegateMac, uint16_t remaining_s);
    void transferTicket(const uint8_t* newGwMac);  // send ticket to new GW before stepping down
    void clearTicket();               // delegate returned — clear ticket
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
    RoleId roleId() const override { return RoleId::PEER; }
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
    static void setRole(IMeshRole* role);
    static void printStatus();

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

    // Gateway step-down (waive root)
    static void stepDown();                              // gateway only

    // Remote config
    static bool sendConfigReq(const uint8_t* sta_mac, const char* json, uint8_t reqId);
    static bool waitConfigResp(char* outBuf, size_t bufSize, uint32_t timeout_ms);

    // Cred push ACK tracking
    static bool isCredAckReceived();

    // BOOT button handler — routes to delegate based on current role
    static void onBootButton();

    // Battery rotation
    static void requestStepDown();

    // RSSI tracking
    static int8_t bestRouterRssi();
    static void   setBestRouterRssi(int8_t rssi);

private:
    MeshConductor() = delete;
};

#endif // MESH_CONDUCTOR_H
