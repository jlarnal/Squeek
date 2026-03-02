#include "mesh_delegate.h"
#include "web_server.h"
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
// Minimal WiFi wizard HTML — served inline (no LittleFS dependency)
// ---------------------------------------------------------------------------
static const char WIZARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Squeek Setup</title>
<style>
body{font-family:system-ui,sans-serif;max-width:400px;margin:2em auto;padding:0 1em;background:#1a1a2e;color:#e0e0e0}
h1{color:#00d4ff}input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box;border-radius:4px;border:1px solid #444;background:#0d0d1a;color:#e0e0e0}
button{background:#00d4ff;color:#000;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;font-size:1em;width:100%}
button:disabled{opacity:0.5}
#msg{margin-top:1em;padding:8px;border-radius:4px}
.ok{background:#1b3a2a;border:1px solid #2d6a3e}
.err{background:#3a1b1b;border:1px solid #6a2d2d}
.wait{background:#3a3a1b;border:1px solid #6a6a2d}
</style></head><body>
<h1>Squeek Setup</h1>
<p>Connect this mesh to your WiFi router.</p>
<form id="f">
<label>SSID<input id="s" name="ssid" required></label>
<label>Password<input id="p" name="pass" type="password"></label>
<button type="submit" id="btn">Connect</button>
</form>
<div id="msg"></div>
<script>
document.getElementById('f').onsubmit=function(e){
  e.preventDefault();
  var btn=document.getElementById('btn'),msg=document.getElementById('msg');
  btn.disabled=true; msg.className='wait'; msg.textContent='Connecting...';
  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:document.getElementById('s').value,pass:document.getElementById('p').value})
  }).then(function(r){return r.json()}).then(function(d){
    if(d.ok){msg.className='ok';msg.textContent='Connected! Rebooting...';}
    else{msg.className='err';msg.textContent='Failed: '+(d.error||'unknown');btn.disabled=false;}
  }).catch(function(){msg.className='err';msg.textContent='Network error';btn.disabled=false;});
};
</script></body></html>
)rawliteral";

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
    // Serve wizard page
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", WIZARD_HTML);
    });

    // Captive portal catch-all redirects
    s_server->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("/");
    });
    s_server->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->redirect("/");
    });

    // WiFi credential submission
    s_server->on("/api/wifi", HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // no-op for body handler
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index + len > 256) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"too large\"}"); return; }
            if (index + len < total) return;

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

            bool ok = s_delegateInstance && s_delegateInstance->onCredsSubmitted(ssid.c_str(), pass.c_str());
            if (ok) {
                req->send(200, "application/json", "{\"ok\":true}");
            } else {
                req->send(200, "application/json", "{\"ok\":false,\"error\":\"connection failed\"}");
            }
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

bool Delegate::onCredsSubmitted(const char* ssid, const char* pass) {
    ESP_LOGI(TAG, "Attempting connection to router: %s", ssid);

    // Temporarily switch to STA+AP to test the connection
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid, pass);

    // Wait up to 15 seconds for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "Router connection successful (IP=%s)", WiFi.localIP().toString().c_str());
        WiFi.disconnect(true);

        // Save credentials to NVS
        SqWebServer::saveWifiCreds(ssid, pass);

        // Spawn deferred reboot task (2s delay to let HTTP response flush)
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            rtc_state_t* rtc = RtcState::get();
            rtc->next_role = (uint8_t)RoleId::PEER;
            rtc->delegate_active = 1;  // signal peer to push creds
            RtcState::save();
            esp_restart();
        }, "dlg_reboot", 2048, nullptr, 2, nullptr);

        return true;
    }

    ESP_LOGW(TAG, "Router connection failed (status=%d)", WiFi.status());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);  // back to AP-only
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
