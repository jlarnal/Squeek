#include "mesh_delegate.h"
#include "web_server.h"
#include "storage_manager.h"
#include "mesh_conductor.h"
#include "bsp.hpp"
#include "sq_log.h"
#include "rtc_state.h"
#include "nvs_config.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_mesh.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>

static const char* TAG = "delegate";

// ---------------------------------------------------------------------------
// File-scope state
// ---------------------------------------------------------------------------
static AsyncWebServer*  s_server   = nullptr;
static TimerHandle_t    s_watchdog = nullptr;
static Delegate*        s_delegateInstance = nullptr;

// ---------------------------------------------------------------------------
// WiFi scan results — populated once in begin() before SoftAP starts
// ---------------------------------------------------------------------------
static constexpr int MAX_SCAN_RESULTS = 20;

struct ScanEntry {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;  // wifi_auth_mode_t
};

static ScanEntry s_scanResults[MAX_SCAN_RESULTS];
static int       s_scanCount = 0;

// ---------------------------------------------------------------------------
// Connection state machine — runs in background task, polled via /api/status
// ---------------------------------------------------------------------------
enum ConnState : uint8_t { CONN_IDLE = 0, CONN_BUSY, CONN_OK, CONN_FAIL };
static volatile ConnState s_connState = CONN_IDLE;
static char s_pendingSsid[33];
static char s_pendingPass[65];

static void connectTask(void*) {
    ESP_LOGI(TAG, "Attempting connection to router: %s", s_pendingSsid);

    // Create STA netif if needed (AP netif already exists from startSoftAP)
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t sta_cfg = {};
    strncpy((char*)sta_cfg.sta.ssid, s_pendingSsid, sizeof(sta_cfg.sta.ssid) - 1);
    if (s_pendingPass[0]) {
        strncpy((char*)sta_cfg.sta.password, s_pendingPass, sizeof(sta_cfg.sta.password) - 1);
    }
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    // Poll for IP assignment (proves DHCP + full L3 connectivity)
    esp_netif_t* sta_nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = false;
    for (int i = 0; i < 30; i++) {   // 15 seconds max
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_netif_ip_info_t ip = {};
        if (sta_nif && esp_netif_get_ip_info(sta_nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            connected = true;
            ESP_LOGI(TAG, "Router connection successful (IP=" IPSTR ")", IP2STR(&ip.ip));
            break;
        }
    }

    if (connected) {
        esp_wifi_disconnect();
        SqWebServer::saveWifiCreds(s_pendingSsid, s_pendingPass);
        s_connState = CONN_OK;

        // Let the page poll one more time to see CONN_OK, then clean up
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Stop web server + AP to avoid rts error spam during shutdown
        SqWebServer::stopDNS();
        if (s_server) { s_server->end(); }
        esp_wifi_stop();

        rtc_state_t* rtc = RtcState::get();
        rtc->next_role = (uint8_t)RoleId::PEER;
        rtc->delegate_active = 1;  // signal peer to push creds
        RtcState::save();
        esp_restart();
    } else {
        ESP_LOGW(TAG, "Router connection failed");
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_AP);
        s_connState = CONN_FAIL;
    }

    vTaskDelete(nullptr);
}

static void doWiFiScan() {
    ESP_LOGI(TAG, "Starting WiFi scan (STA mode)...");

    // Brief STA mode for scanning
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_scan_config_t scanCfg = {};
    scanCfg.show_hidden = false;
    scanCfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    scanCfg.scan_time.active.min = 240;   // ms per channel
    scanCfg.scan_time.active.max = 480;   // ms per channel
    esp_err_t err = esp_wifi_scan_start(&scanCfg, true);  // blocking
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        esp_wifi_stop();
        return;
    }

    uint16_t apCount = 0;
    esp_wifi_scan_get_ap_num(&apCount);
    if (apCount == 0) {
        ESP_LOGI(TAG, "WiFi scan: 0 networks found");
        esp_wifi_stop();
        return;
    }

    // Allocate temp buffer for all results
    uint16_t fetchCount = (apCount > 64) ? 64 : apCount;
    wifi_ap_record_t* records = (wifi_ap_record_t*)malloc(fetchCount * sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "WiFi scan: malloc failed");
        esp_wifi_scan_get_ap_records(&fetchCount, nullptr);  // free internal buffer
        esp_wifi_stop();
        return;
    }

    esp_wifi_scan_get_ap_records(&fetchCount, records);
    esp_wifi_stop();

    // Sort by RSSI descending (records are already sorted by ESP-IDF, but ensure it)
    // Deduplicate by SSID, keep strongest
    s_scanCount = 0;
    for (uint16_t i = 0; i < fetchCount && s_scanCount < MAX_SCAN_RESULTS; i++) {
        if (records[i].ssid[0] == '\0') continue;  // skip hidden

        // Check for duplicate SSID
        bool dup = false;
        for (int j = 0; j < s_scanCount; j++) {
            if (strcmp(s_scanResults[j].ssid, (const char*)records[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        strncpy(s_scanResults[s_scanCount].ssid, (const char*)records[i].ssid, 32);
        s_scanResults[s_scanCount].ssid[32] = '\0';
        s_scanResults[s_scanCount].rssi = records[i].rssi;
        s_scanResults[s_scanCount].auth = (uint8_t)records[i].authmode;
        s_scanCount++;
    }

    free(records);
    ESP_LOGI(TAG, "WiFi scan: %d unique networks", s_scanCount);
}

// ---------------------------------------------------------------------------
// Watchdog callback — reboots as peer if no creds received in time
// ---------------------------------------------------------------------------
static void watchdogCb(TimerHandle_t t) {
    (void)t;
    ESP_LOGW(TAG, "Delegate timeout (%ds) — no creds received, rebooting as peer", (uint16_t)NvsConfigManager::delegateTimeout_s);
    rtc_state_t* rtc = RtcState::get();
    rtc->next_role = (uint8_t)RoleId::PEER;
    rtc->delegate_active = 0;
    RtcState::save();
    esp_restart();
}

// ---------------------------------------------------------------------------
// SoftAP management (file-scope, uses own MAC for SSID)
// ---------------------------------------------------------------------------
static void startSoftAP() {
    // Get own MAC for SSID suffix
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Build SSID: Squeek_Config_XXYY
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "Squeek_Config_%02X%02X", mac[4], mac[5]);

    // Stop mesh and WiFi completely so we can swap netifs before restarting.
    esp_mesh_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_stop();

    // Mesh netifs have DHCP_SERVER stripped — destroy and replace with a standard
    // AP netif that has DHCP built in.
    esp_netif_t* nif;
    if ((nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")))  esp_netif_destroy(nif);
    if ((nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))) esp_netif_destroy(nif);
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();

    // Configure and start WiFi in AP-only mode
    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = SOFTAP_MAX_CONNECTIONS;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    if (ap_netif) {
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_ip_info_t ip_info = {};
        IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(ap_netif, &ip_info);

        esp_err_t err = esp_netif_dhcps_start(ap_netif);
        ESP_LOGI(TAG, "SoftAP started: %s (IP=" IPSTR ") dhcp=%s",
                 ssid, IP2STR(&ip_info.ip),
                 err == ESP_OK ? "ok" : esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "SoftAP started: %s (failed to create AP netif)", ssid);
    }
}

// ---------------------------------------------------------------------------
// WiFi wizard routes
// ---------------------------------------------------------------------------
static void registerWizardRoutes() {
    // Serve wizard page from LittleFS (gzip-transparent via StorageManager)
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!StorageManager::serveFile(req, "/wizard.html")) {
            req->send(500, "text/plain", "wizard.html not found on filesystem");
        }
    });

    // WiFi scan results as JSON
    s_server->on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Build JSON array: [{"ssid":"...","rssi":-45,"auth":3}, ...]
        String json = "[";
        for (int i = 0; i < s_scanCount; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"";
            // Escape any quotes in SSID
            for (const char* p = s_scanResults[i].ssid; *p; p++) {
                if (*p == '"') json += "\\\"";
                else if (*p == '\\') json += "\\\\";
                else json += *p;
            }
            json += "\",\"rssi\":";
            json += String((int)s_scanResults[i].rssi);
            json += ",\"auth\":";
            json += String((int)s_scanResults[i].auth);
            json += "}";
        }
        json += "]";
        req->send(200, "application/json", json);
    });

    // Connection status polling endpoint
    s_server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        const char* s = "idle";
        switch (s_connState) {
            case CONN_BUSY: s = "connecting"; break;
            case CONN_OK:   s = "connected";  break;
            case CONN_FAIL: s = "failed";     break;
            default:        s = "idle";        break;
        }
        char json[48];
        snprintf(json, sizeof(json), "{\"status\":\"%s\"}", s);
        req->send(200, "application/json", json);
    });

    // Captive portal catch-all redirects
    s_server->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("/");
    });
    s_server->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("/");
    });

    // WiFi credential submission — returns immediately, spawns background task
    s_server->on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // no-op for body handler
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index + len > 256) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"too large\"}"); return; }
            if (index + len < total) return;

            if (s_connState == CONN_BUSY) {
                req->send(409, "application/json", "{\"ok\":false,\"error\":\"already connecting\"}");
                return;
            }

            char buf[257];
            size_t cpLen = (total < 256) ? total : 256;
            memcpy(buf, data, cpLen);
            buf[cpLen] = '\0';

            String body = String(buf);
            int si = body.indexOf("\"ssid\"");
            int pi = body.indexOf("\"pass\"");
            if (si < 0) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}"); return; }

            auto extractVal = [](const String& s, int keyPos) -> String {
                int colon = s.indexOf(':', keyPos);
                if (colon < 0) return "";
                int q1 = s.indexOf('"', colon + 1);
                if (q1 < 0) return "";
                int q2 = s.indexOf('"', q1 + 1);
                if (q2 < 0) return "";
                return s.substring(q1 + 1, q2);
            };

            String ssid = extractVal(body, si);
            String pass = (pi >= 0) ? extractVal(body, pi) : "";

            if (ssid.length() == 0 || ssid.length() > 32) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid ssid\"}");
                return;
            }

            // Stash creds and spawn background connection task
            strncpy(s_pendingSsid, ssid.c_str(), sizeof(s_pendingSsid) - 1);
            s_pendingSsid[sizeof(s_pendingSsid) - 1] = '\0';
            strncpy(s_pendingPass, pass.c_str(), sizeof(s_pendingPass) - 1);
            s_pendingPass[sizeof(s_pendingPass) - 1] = '\0';

            s_connState = CONN_BUSY;
            xTaskCreate(connectTask, "dlg_conn", 4096, nullptr, 3, nullptr);

            req->send(200, "application/json", "{\"ok\":true,\"status\":\"connecting\"}");
        }
    );

    // Catch-all for captive portal
    s_server->onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("/");
    });
}

// ---------------------------------------------------------------------------
// Delegate IMeshRole implementation
// ---------------------------------------------------------------------------

void Delegate::begin() {
    s_delegateInstance = this;

    // Mark RTC state
    rtc_state_t* rtc = RtcState::get();
    rtc->delegate_active = 1;
    rtc->own_role = (uint8_t)RoleId::DELEGATE;
    RtcState::save();

    ESP_LOGI(TAG, "Entering Delegate role");

    // Scan for WiFi networks before starting SoftAP (needs STA mode)
    doWiFiScan();

    // Mount LittleFS for wizard.html.gz
    StorageManager::init();

    // Start SoftAP
    startSoftAP();

    // Start web server with wizard
    s_server = new AsyncWebServer(80);
    registerWizardRoutes();
    s_server->begin();

    // Start DNS captive portal
    SqWebServer::startDNS();

    // Start watchdog timer (clamp 60-600s)
    uint16_t tmo = NvsConfigManager::delegateTimeout_s;
    if (tmo < 60)  tmo = 60;
    if (tmo > 600) tmo = 600;

    s_watchdog = xTimerCreate("dlg_wd", pdMS_TO_TICKS((uint32_t)tmo * 1000),
                              pdFALSE, nullptr, watchdogCb);
    if (s_watchdog) {
        xTimerStart(s_watchdog, 0);
        ESP_LOGI(TAG, "Watchdog armed: %u s", tmo);
    }

    ESP_LOGI(TAG, "Delegate active — waiting for WiFi credentials");
}

void Delegate::end() {
    s_delegateInstance = nullptr;

    // Stop + delete watchdog
    if (s_watchdog) {
        xTimerStop(s_watchdog, 0);
        xTimerDelete(s_watchdog, 0);
        s_watchdog = nullptr;
    }

    // Stop DNS
    SqWebServer::stopDNS();

    // Tear down web server
    if (s_server) {
        s_server->end();
        delete s_server;
        s_server = nullptr;
    }

    // Tear down SoftAP
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    ESP_LOGI(TAG, "Delegate role ended");
}

bool Delegate::onCredsSubmitted(const char* /*ssid*/, const char* /*pass*/) {
    // Legacy — connection now handled by connectTask via /api/wifi POST
    return false;
}

void Delegate::onPeerJoined(const uint8_t* mac) {
    (void)mac;  // no mesh active in delegate mode
}

void Delegate::onPeerLeft(const uint8_t* mac) {
    (void)mac;  // no mesh active in delegate mode
}

void Delegate::printStatus() {
    Serial.printf("  Role: DELEGATE\n");
    if (s_watchdog) {
        TickType_t remaining = xTimerGetExpiryTime(s_watchdog) - xTaskGetTickCount();
        Serial.printf("  Watchdog: %lu s remaining\n",
                      (unsigned long)(remaining / configTICK_RATE_HZ));
    } else {
        Serial.printf("  Watchdog: inactive\n");
    }
}
