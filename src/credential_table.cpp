#include "credential_table.h"
#include "nvs_config.h"
#include <nvs.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "cred";

static CredEntry s_slots[CRED_TABLE_SLOTS];

// NVS key helpers — keys like "c0s", "c0p" (max 15 chars, well within limit)
static void slotSsidKey(uint8_t slot, char* out) {
    out[0] = 'c'; out[1] = '0' + slot; out[2] = 's'; out[3] = '\0';
}
static void slotPassKey(uint8_t slot, char* out) {
    out[0] = 'c'; out[1] = '0' + slot; out[2] = 'p'; out[3] = '\0';
}

static void loadSlot(uint8_t slot) {
    if (!NvsConfig::isOpen) return;
    char sk[4], pk[4];
    slotSsidKey(slot, sk);
    slotPassKey(slot, pk);

    size_t sLen = sizeof(s_slots[slot].ssid);
    esp_err_t err = nvs_get_str(NvsConfig::handle, sk, s_slots[slot].ssid, &sLen);
    if (err != ESP_OK || sLen <= 1) {
        s_slots[slot].populated = false;
        s_slots[slot].ssid[0] = '\0';
        s_slots[slot].pass[0] = '\0';
        return;
    }
    s_slots[slot].populated = true;

    size_t pLen = sizeof(s_slots[slot].pass);
    err = nvs_get_str(NvsConfig::handle, pk, s_slots[slot].pass, &pLen);
    if (err != ESP_OK) {
        s_slots[slot].pass[0] = '\0';  // open network
    }
}

static void saveSlot(uint8_t slot) {
    if (!NvsConfig::isOpen) return;
    char sk[4], pk[4];
    slotSsidKey(slot, sk);
    slotPassKey(slot, pk);

    if (s_slots[slot].populated) {
        nvs_set_str(NvsConfig::handle, sk, s_slots[slot].ssid);
        nvs_set_str(NvsConfig::handle, pk, s_slots[slot].pass);
    } else {
        nvs_erase_key(NvsConfig::handle, sk);
        nvs_erase_key(NvsConfig::handle, pk);
    }
    nvs_commit(NvsConfig::handle);
}

void CredentialTable::init() {
    // Load all slots from NVS
    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
        loadSlot(i);
    }

    // Migrate legacy wifiSsid/wifiPass to slot 0 if slot 0 is empty
    if (!s_slots[0].populated && NvsConfig::isOpen) {
        size_t sLen = sizeof(s_slots[0].ssid);
        esp_err_t err = nvs_get_str(NvsConfig::handle, "wifiSsid", s_slots[0].ssid, &sLen);
        if (err == ESP_OK && sLen > 1) {
            s_slots[0].populated = true;
            size_t pLen = sizeof(s_slots[0].pass);
            err = nvs_get_str(NvsConfig::handle, "wifiPass", s_slots[0].pass, &pLen);
            if (err != ESP_OK) s_slots[0].pass[0] = '\0';
            saveSlot(0);
            ESP_LOGI(TAG, "Migrated legacy WiFi creds to slot 0 (SSID=%s)", s_slots[0].ssid);
        }
    }

    uint8_t n = count();
    if (n > 0) {
        ESP_LOGI(TAG, "Credential table: %u slot(s) populated", n);
    }
}

int8_t CredentialTable::add(const char* ssid, const char* pass) {
    if (!ssid || ssid[0] == '\0') return -1;

    // Deduplicate: update password if SSID already exists
    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
        if (s_slots[i].populated && strcmp(s_slots[i].ssid, ssid) == 0) {
            strncpy(s_slots[i].pass, pass ? pass : "", CRED_PASS_MAX);
            s_slots[i].pass[CRED_PASS_MAX] = '\0';
            saveSlot(i);
            ESP_LOGI(TAG, "Updated creds in slot %u (SSID=%s)", i, ssid);
            return (int8_t)i;
        }
    }

    // Find first empty slot
    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
        if (!s_slots[i].populated) {
            strncpy(s_slots[i].ssid, ssid, CRED_SSID_MAX);
            s_slots[i].ssid[CRED_SSID_MAX] = '\0';
            strncpy(s_slots[i].pass, pass ? pass : "", CRED_PASS_MAX);
            s_slots[i].pass[CRED_PASS_MAX] = '\0';
            s_slots[i].populated = true;
            saveSlot(i);
            ESP_LOGI(TAG, "Added creds to slot %u (SSID=%s)", i, ssid);
            return (int8_t)i;
        }
    }

    ESP_LOGW(TAG, "Credential table full — cannot add SSID=%s", ssid);
    return -1;
}

bool CredentialTable::get(uint8_t slot, char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    if (slot >= CRED_TABLE_SLOTS || !s_slots[slot].populated) return false;
    if (ssid) strncpy(ssid, s_slots[slot].ssid, ssidLen);
    if (pass) strncpy(pass, s_slots[slot].pass, passLen);
    return true;
}

uint8_t CredentialTable::count() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
        if (s_slots[i].populated) n++;
    }
    return n;
}

bool CredentialTable::hasAny() {
    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
        if (s_slots[i].populated) return true;
    }
    return false;
}

ScanMatch CredentialTable::matchScan(const wifi_ap_record_t* records, uint16_t count) {
    ScanMatch best = { .slot = -1, .rssi = -128, .ssid = {0} };

    for (uint16_t r = 0; r < count; r++) {
        for (uint8_t s = 0; s < CRED_TABLE_SLOTS; s++) {
            if (!s_slots[s].populated) continue;
            if (strcmp((const char*)records[r].ssid, s_slots[s].ssid) == 0) {
                if (records[r].rssi > best.rssi) {
                    best.slot = (int8_t)s;
                    best.rssi = records[r].rssi;
                    strncpy(best.ssid, s_slots[s].ssid, CRED_SSID_MAX);
                    best.ssid[CRED_SSID_MAX] = '\0';
                }
            }
        }
    }
    return best;
}

uint16_t CredentialTable::toBuffer(uint8_t* buf, uint16_t maxLen) {
    uint16_t pos = 0;
    // First byte: count of entries
    if (pos >= maxLen) return 0;
    uint8_t n = count();
    buf[pos++] = n;

    for (uint8_t i = 0; i < CRED_TABLE_SLOTS && pos < maxLen; i++) {
        if (!s_slots[i].populated) continue;
        uint16_t entrySize = sizeof(CredWireEntry);
        if (pos + entrySize > maxLen) break;

        CredWireEntry* e = (CredWireEntry*)&buf[pos];
        e->ssid_len = (uint8_t)strlen(s_slots[i].ssid);
        memset(e->ssid, 0, CRED_SSID_MAX);
        memcpy(e->ssid, s_slots[i].ssid, e->ssid_len);
        e->pass_len = (uint8_t)strlen(s_slots[i].pass);
        memset(e->pass, 0, CRED_PASS_MAX);
        memcpy(e->pass, s_slots[i].pass, e->pass_len);
        pos += entrySize;
    }
    return pos;
}

uint8_t CredentialTable::fromBuffer(const uint8_t* buf, uint16_t len) {
    if (len < 1) return 0;
    uint8_t n = buf[0];
    uint16_t pos = 1;
    uint8_t added = 0;

    for (uint8_t i = 0; i < n && pos + sizeof(CredWireEntry) <= len; i++) {
        const CredWireEntry* e = (const CredWireEntry*)&buf[pos];
        pos += sizeof(CredWireEntry);

        // Extract null-terminated strings
        char ssid[CRED_SSID_MAX + 1] = {0};
        char pass[CRED_PASS_MAX + 1] = {0};
        uint8_t sLen = (e->ssid_len > CRED_SSID_MAX) ? CRED_SSID_MAX : e->ssid_len;
        uint8_t pLen = (e->pass_len > CRED_PASS_MAX) ? CRED_PASS_MAX : e->pass_len;
        memcpy(ssid, e->ssid, sLen);
        memcpy(pass, e->pass, pLen);

        if (ssid[0] != '\0') {
            int8_t slot = add(ssid, pass);
            if (slot >= 0) added++;
        }
    }
    return added;
}

void CredentialTable::clear() {
    for (uint8_t i = 0; i < CRED_TABLE_SLOTS; i++) {
        s_slots[i].populated = false;
        s_slots[i].ssid[0] = '\0';
        s_slots[i].pass[0] = '\0';
        saveSlot(i);  // erases NVS keys + commits
    }
    // Also erase legacy keys so migration doesn't resurrect them
    if (NvsConfig::isOpen) {
        nvs_erase_key(NvsConfig::handle, "wifiSsid");
        nvs_erase_key(NvsConfig::handle, "wifiPass");
        nvs_commit(NvsConfig::handle);
    }
    ESP_LOGI(TAG, "All credential slots cleared");
}

const CredEntry* CredentialTable::getSlot(uint8_t slot) {
    if (slot >= CRED_TABLE_SLOTS) return nullptr;
    return s_slots[slot].populated ? &s_slots[slot] : nullptr;
}
