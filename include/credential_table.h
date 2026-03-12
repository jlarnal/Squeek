#ifndef CREDENTIAL_TABLE_H
#define CREDENTIAL_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <esp_wifi_types.h>

#define CRED_TABLE_SLOTS  8
#define CRED_SSID_MAX     32
#define CRED_PASS_MAX     64

struct CredEntry {
    char ssid[CRED_SSID_MAX + 1];   // null-terminated
    char pass[CRED_PASS_MAX + 1];   // null-terminated
    bool populated;
};

// Result of matchScan()
struct ScanMatch {
    int8_t  slot;       // -1 if no match
    int8_t  rssi;       // best RSSI among matching APs
    char    ssid[CRED_SSID_MAX + 1];
};

// Serialized credential entry for mesh exchange
struct __attribute__((packed)) CredWireEntry {
    uint8_t ssid_len;
    char    ssid[CRED_SSID_MAX];
    uint8_t pass_len;
    char    pass[CRED_PASS_MAX];
};

// Extended entry in CRED_REPLY (includes peer's RSSI observation)
struct __attribute__((packed)) CredReplyEntry {
    uint8_t ssid_len;
    char    ssid[CRED_SSID_MAX];
    uint8_t pass_len;
    char    pass[CRED_PASS_MAX];
    int8_t  rssi;       // peer's measured RSSI to this AP (-128 = not detected)
    uint8_t detected;   // 1 if peer can see this AP
};

class CredentialTable {
    CredentialTable() = delete;
public:
    static void init();

    // Add or update a credential. Returns slot index, or -1 if full.
    static int8_t add(const char* ssid, const char* pass);

    // Get credential by slot index. Returns false if slot empty.
    static bool get(uint8_t slot, char* ssid, size_t ssidLen, char* pass, size_t passLen);

    // Number of populated slots
    static uint8_t count();

    // Check if any creds exist
    static bool hasAny();

    // Cross-reference scan results against stored SSIDs.
    // Returns best match (highest RSSI among known routers).
    static ScanMatch matchScan(const wifi_ap_record_t* records, uint16_t count);

    // Serialization for mesh exchange
    static uint16_t toBuffer(uint8_t* buf, uint16_t maxLen);
    static uint8_t  fromBuffer(const uint8_t* buf, uint16_t len);

    // Direct slot access (for internal use)
    static const CredEntry* getSlot(uint8_t slot);
};

#endif // CREDENTIAL_TABLE_H
