#include "setup_delegate.h"
#include "web_server.h"
#include "mesh_conductor.h"
#include "storage_manager.h"
#include "bsp.hpp"
#include "sq_log.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mesh.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "delegate";

// ---------------------------------------------------------------------------
// File-scope state
// ---------------------------------------------------------------------------
static bool             s_active   = false;
static AsyncWebServer*  s_server   = nullptr;
static uint8_t          s_gwMac[6] = {};
static TaskHandle_t     s_pushTask = nullptr;
static volatile bool    s_pushStop = false;

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
    if(d.ok){msg.className='ok';msg.textContent='Connected! Rejoining mesh...';}
    else{msg.className='err';msg.textContent='Failed: '+(d.error||'unknown');btn.disabled=false;}
  }).catch(function(){msg.className='err';msg.textContent='Network error';btn.disabled=false;});
};
</script></body></html>
)rawliteral";

// ---------------------------------------------------------------------------
// Credential push task — resends MSG_TYPE_WIFI_CREDS until ACK'd
// ---------------------------------------------------------------------------
static void credPushTask(void* /*param*/) {
    char ssid[33], pass[65];
    if (!SqWebServer::loadWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        SqLog.println("[delegate] No creds to push");
        vTaskDelete(nullptr);
        return;
    }

    WifiCredsMsg msg = {};
    msg.type = MSG_TYPE_WIFI_CREDS;
    strncpy(msg.ssid, ssid, 32);
    strncpy(msg.password, pass, 64);

    for (int i = 0; i < 10 && !s_pushStop; i++) {
        SqLog.printf("[delegate] Pushing WiFi creds to mesh (attempt %d)\n", i + 1);
        MeshConductor::sendToRoot(&msg, sizeof(msg));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // Also broadcast merge check
    mesh_addr_t rt[MESH_MAX_NODES];
    int rtSize = 0;
    esp_mesh_get_routing_table(rt, sizeof(rt), &rtSize);
    MergeCheckMsg mc = { .type = MSG_TYPE_MERGE_CHECK, .root_table_size = (uint8_t)rtSize };
    MeshConductor::broadcastToAll(&mc, sizeof(mc));
    SqLog.println("[delegate] Merge check broadcast sent");

    s_pushTask = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// SoftAP management
// ---------------------------------------------------------------------------
void SetupDelegate::startSoftAP(const uint8_t gatewayMac[6]) {
    // Build SSID: Squeek_Config_XXYY
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "Squeek_Config_%02X%02X",
             gatewayMac[4], gatewayMac[5]);

    // Stop mesh to free WiFi for standalone AP.
    // Don't deinit mesh — that's needed for rejoin later.
    // Don't create new netifs — mesh already created AP+STA netifs.
    esp_mesh_stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Reconfigure the existing AP with our SSID (no password, open)
    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = SOFTAP_MAX_CONNECTIONS;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    // Wait for AP to be ready and get IP
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_get_ip_info(ap_netif, &ip_info);
    }
    ESP_LOGI(TAG, "SoftAP started: %s (IP=" IPSTR ")", ssid, IP2STR(&ip_info.ip));
}

void SetupDelegate::stopSoftAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(200));
}

// ---------------------------------------------------------------------------
// WiFi wizard routes
// ---------------------------------------------------------------------------
void SetupDelegate::registerWizardRoutes() {
    // Serve wizard page
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", WIZARD_HTML);
    });

    // Captive portal catch-all → redirect to wizard
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
            // Only process when all data received
            if (index + len < total) return;

            // Parse JSON
            char buf[257];
            size_t cpLen = (total < 256) ? total : 256;
            memcpy(buf, data, cpLen);
            buf[cpLen] = '\0';

            // Simple JSON extraction (avoid ArduinoJson dependency here)
            // Use ArduinoJson since it's already a project dependency
            // But we need to handle this in the body callback which accumulates data
            // For simplicity, parse the full body
            String body = String(buf);
            // Quick parse: find "ssid":"..." and "pass":"..."
            int si = body.indexOf("\"ssid\"");
            int pi = body.indexOf("\"pass\"");
            if (si < 0) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}"); return; }

            // Extract values between quotes after the colon
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

            bool ok = SetupDelegate::onCredsSubmitted(ssid.c_str(), pass.c_str());
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
// Public API
// ---------------------------------------------------------------------------
void SetupDelegate::begin(const uint8_t gatewayMac[6]) {
    if (s_active) return;

    memcpy(s_gwMac, gatewayMac, 6);
    ESP_LOGI(TAG, "Entering Setup Delegate mode");

    startSoftAP(gatewayMac);

    // Start web server with wizard
    s_server = new AsyncWebServer(80);
    registerWizardRoutes();
    s_server->begin();

    // Start DNS captive portal (reuse SqWebServer's DNS)
    SqWebServer::startDNS();

    s_active = true;
    ESP_LOGI(TAG, "Setup Delegate active — waiting for WiFi credentials");
}

void SetupDelegate::end() {
    if (!s_active) return;

    ESP_LOGI(TAG, "Leaving Setup Delegate mode");

    SqWebServer::stopDNS();

    if (s_server) {
        s_server->end();
        delete s_server;
        s_server = nullptr;
    }

    stopSoftAP();

    // Rejoin mesh (mesh was stopped but not deinited, so just restart)
    ESP_LOGI(TAG, "Rejoining mesh...");
    MeshConductor::start();

    // Start credential push task
    s_pushStop = false;
    xTaskCreate(credPushTask, "credpush", 3072, nullptr, 2, &s_pushTask);

    s_active = false;
    ESP_LOGI(TAG, "Setup Delegate ended, mesh rejoin initiated");
}

bool SetupDelegate::isActive() {
    return s_active;
}

bool SetupDelegate::onCredsSubmitted(const char* ssid, const char* pass) {
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
        WiFi.disconnect(true);  // disconnect — we'll reconnect via mesh router config

        // Save credentials to NVS
        SqWebServer::saveWifiCreds(ssid, pass);

        // Schedule mesh rejoin (can't call end() from HTTP handler context)
        // Use a short timer to call end() after response is sent
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(2000));  // let HTTP response flush
            SetupDelegate::end();
            vTaskDelete(nullptr);
        }, "dlg_end", 2048, nullptr, 2, nullptr);

        return true;
    }

    ESP_LOGW(TAG, "Router connection failed (status=%d)", WiFi.status());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);  // back to AP-only
    return false;
}
