#include "web_server.h"
#include "credential_table.h"
#include "storage_manager.h"
#include "property_value.h"
#include "peer_table.h"
#include "orchestrator.h"
#include "tone_library.h"
#include "nvs_config_registry.h"
#include "nvs_config.h"
#include "power_manager.h"
#include "mesh_conductor.h"
#include "ftm_scheduler.h"
#include "rtc_state.h"

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mesh.h>
#include <nvs_flash.h>
#include <mdns.h>
#include <esp_sntp.h>
#include <time.h>

static const char* TAG = "webserver";

// ---------------------------------------------------------------------------
// WiFi credential helpers — delegate to CredentialTable slot 0
// ---------------------------------------------------------------------------

bool SqWebServer::loadWifiCreds(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    return CredentialTable::get(0, ssid, ssidLen, pass, passLen);
}

bool SqWebServer::saveWifiCreds(const char* ssid, const char* pass) {
    int8_t slot = CredentialTable::add(ssid, pass);
    if (slot < 0) {
        ESP_LOGE(TAG, "Failed to save WiFi creds — table full");
        return false;
    }
    ESP_LOGI(TAG, "WiFi credentials saved to slot %d (SSID=%s)", slot, ssid);

    // Reset delegate state — creds obtained successfully
    rtc_state_t* rtc = RtcState::get();
    rtc->delegate_attempts = 0;
    memset(rtc->ticket_delegate_mac, 0, 6);
    rtc->ticket_remaining_s = 0;
    RtcState::save();

    return true;
}

bool SqWebServer::clearWifiCreds() {
    // CredentialTable doesn't support clearing individual slots yet,
    // but the legacy API only cleared the single wifiSsid/wifiPass keys.
    // For backward compat, erase the legacy keys if they exist.
    if (!NvsConfig::isOpen) return false;
    nvs_erase_key(NvsConfig::handle, "wifiSsid");
    nvs_erase_key(NvsConfig::handle, "wifiPass");
    nvs_commit(NvsConfig::handle);
    ESP_LOGI(TAG, "Legacy WiFi credentials cleared from NVS");
    return true;
}

bool SqWebServer::hasWifiCreds() {
    return CredentialTable::hasAny();
}


// ---- file-scope state -----------------------------------------------------
static AsyncWebServer* s_server  = nullptr;
static AsyncWebSocket*  s_ws     = nullptr;
static bool             s_running  = false;
static bool             s_staMode  = false;
static TaskHandle_t     s_dnsTask  = nullptr;
static volatile bool    s_dnsStop  = false;

// ---------------------------------------------------------------------------
// Get the AP interface IP — works with ESP-mesh (WiFi.softAPIP() returns
// 0.0.0.0 when the mesh manages the AP netif).
// ---------------------------------------------------------------------------
static uint32_t getApIpAddr() {
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK
        && ip_info.ip.addr != 0) {
        return ip_info.ip.addr;
    }
    // Fallback: try the mesh SoftAP netif
    ap_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK
        && ip_info.ip.addr != 0) {
        return ip_info.ip.addr;
    }
    return 0;
}

// Fixed BSSID for the mesh AP — defined here but NOT applied yet.
// Must be applied before esp_mesh_start() (Phase 5 wiring task).
static const uint8_t SQUEEK_FIXED_BSSID[6] = { 0x52, 0x51, 0x45, 0x45, 0x4B, 0x01 };
// Spells "SQEEK\x01" in ASCII

// ---------------------------------------------------------------------------
// DNS captive portal task
// ---------------------------------------------------------------------------
static void dnsTask(void* /*param*/) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed");
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    // Non-blocking with short timeout so we can check the stop flag
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS captive portal running on :53");

    uint8_t buf[512];

    while (!s_dnsStop) {
        struct sockaddr_in client = {};
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&client, &clen);
        if (n < 12) continue;  // too short or timeout

        // Get the AP IP to redirect to
        uint32_t addr = getApIpAddr();
        if (addr == 0) continue;  // no IP yet, skip
        uint8_t ip[4];
        ip[0] = (addr >>  0) & 0xFF;
        ip[1] = (addr >>  8) & 0xFF;
        ip[2] = (addr >> 16) & 0xFF;
        ip[3] = (addr >> 24) & 0xFF;

        // Build minimal DNS response in-place
        // Header: set QR=1 (response), AA=1, RCODE=0
        buf[2] = 0x84;  // QR=1, AA=1
        buf[3] = 0x00;  // RCODE=0 (no error)
        // ANCOUNT = 1
        buf[6] = 0x00;
        buf[7] = 0x01;
        // NSCOUNT = 0, ARCOUNT = 0
        buf[8] = 0; buf[9] = 0;
        buf[10] = 0; buf[11] = 0;

        // Find end of question section (skip QNAME + QTYPE + QCLASS)
        int pos = 12;
        while (pos < n && buf[pos] != 0) {
            pos += buf[pos] + 1;  // skip label
        }
        if (pos >= n) continue;
        pos += 1;  // skip null terminator of QNAME
        pos += 4;  // skip QTYPE (2) + QCLASS (2)

        // Make sure we have room for the answer (16 bytes)
        if (pos + 16 > (int)sizeof(buf)) continue;

        // Append answer record
        buf[pos++] = 0xC0;  // name pointer
        buf[pos++] = 0x0C;  // offset to QNAME in query
        buf[pos++] = 0x00;  // TYPE A
        buf[pos++] = 0x01;
        buf[pos++] = 0x00;  // CLASS IN
        buf[pos++] = 0x01;
        buf[pos++] = 0x00;  // TTL = 60 seconds
        buf[pos++] = 0x00;
        buf[pos++] = 0x00;
        buf[pos++] = 0x3C;
        buf[pos++] = 0x00;  // RDLENGTH = 4
        buf[pos++] = 0x04;
        buf[pos++] = ip[0]; // RDATA = IP address
        buf[pos++] = ip[1];
        buf[pos++] = ip[2];
        buf[pos++] = ip[3];

        sendto(sock, buf, pos, 0,
               (struct sockaddr*)&client, clen);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS captive portal stopped");
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// WebSocket event handler
// ---------------------------------------------------------------------------
void SqWebServer::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                          int type, void* arg, uint8_t* data, size_t len) {
    switch ((AwsEventType)type) {
        case WS_EVT_CONNECT:
            ESP_LOGI(TAG, "WS client #%u connected from %s",
                     client->id(), client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            ESP_LOGI(TAG, "WS client #%u disconnected", client->id());
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                // Null-terminate for logging
                char tmp[128];
                size_t cpLen = (len < sizeof(tmp) - 1) ? len : sizeof(tmp) - 1;
                memcpy(tmp, data, cpLen);
                tmp[cpLen] = '\0';
                ESP_LOGI(TAG, "WS client #%u data: %s", client->id(), tmp);
            }
            break;
        }
        case WS_EVT_ERROR:
            ESP_LOGE(TAG, "WS client #%u error", client->id());
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Helper: format MAC as string
// ---------------------------------------------------------------------------
static void macToStr(const uint8_t* mac, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void SqWebServer::registerRoutes() {
    // Root → dashboard.html (fallback to index.html then inline stub)
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (StorageManager::serveFile(request, "/dashboard.html")) return;
        if (StorageManager::serveFile(request, "/index.html")) return;
        request->send(200, "text/html",
            "<!DOCTYPE html><html><body>"
            "<h1>Squeek</h1><p>No UI uploaded yet. Use OTA to upload.</p>"
            "</body></html>");
    });

    // ----- GET /api/status -----
    s_server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char macStr[18];
        macToStr(mac, macStr);
        doc["mac"]          = macStr;
        doc["battery_mv"]   = PowerManager::batteryMv();
        doc["peer_count"]   = PeerTable::peerCount();
        doc["alive_peers"]  = PeerTable::alivePeerCount();
        doc["dimension"]    = PeerTable::getDimension();
        doc["uptime_s"]     = millis() / 1000;
        doc["free_heap"]    = esp_get_free_heap_size();
        doc["orch_mode"]    = (uint8_t)Orchestrator::getMode();
        doc["build"]        = __DATE__ " " __TIME__;
        char buf[300];
        serializeJson(doc, buf, sizeof(buf));
        request->send(200, "application/json", buf);
    });

    // ----- GET /api/peers -----
    s_server->on("/api/peers", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        uint8_t count = PeerTable::peerCount();
        uint8_t ownMac[6];
        esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
        for (uint8_t i = 0; i < count; i++) {
            PeerEntry* e = PeerTable::getEntryByIndex(i);
            if (!e) continue;
            JsonObject o = arr.add<JsonObject>();
            o["idx"] = i;
            char macStr[18];
            macToStr(e->mac, macStr);
            o["mac"]        = macStr;
            o["battery_mv"] = e->battery_mv;
            o["flags"]      = e->flags;
            JsonArray pos   = o["pos"].to<JsonArray>();
            pos.add(e->position[0]);
            pos.add(e->position[1]);
            pos.add(e->position[2]);
            o["confidence"] = e->confidence;
            o["is_gateway"] = (memcmp(e->mac, ownMac, 6) == 0 && MeshConductor::isGateway());
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ----- GET /api/distances -----
    s_server->on("/api/distances", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        uint8_t count = PeerTable::peerCount();
        for (uint8_t a = 0; a < count; a++) {
            for (uint8_t b = a + 1; b < count; b++) {
                float d = PeerTable::getDistance(a, b);
                if (d >= 0) {
                    JsonObject o = arr.add<JsonObject>();
                    o["a"] = a;
                    o["b"] = b;
                    o["d"] = d;
                }
            }
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ----- GET /api/tones -----
    s_server->on("/api/tones", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        uint8_t count = ToneLibrary::count();
        for (uint8_t i = 0; i < count; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["idx"]  = i;
            o["name"] = ToneLibrary::nameByIndex(i);
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ----- GET /api/orch -----
    s_server->on("/api/orch", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        doc["mode"]         = (uint8_t)Orchestrator::getMode();
        doc["travel_order"] = (uint8_t)Orchestrator::getTravelOrder();
        JsonArray seq = doc["sequence"].to<JsonArray>();
        const SeqStep* steps = Orchestrator::sequenceSteps();
        uint8_t count = Orchestrator::sequenceCount();
        for (uint8_t i = 0; i < count; i++) {
            JsonObject s = seq.add<JsonObject>();
            s["node"]  = steps[i].node_index;
            s["tone"]  = steps[i].tone_index;
            s["delay"] = steps[i].delay_ms;
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ----- GET /api/config -----
    s_server->on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        configBuildJson(doc, nullptr, 0);
        // Add _meta array
        JsonArray meta = doc["_meta"].to<JsonArray>();
        uint8_t fc = configFieldCount();
        for (uint8_t i = 0; i < fc; i++) {
            const ConfigField* f = configFieldByIndex(i);
            if (!f) continue;
            JsonObject m = meta.add<JsonObject>();
            m["key"]  = f->key;
            m["desc"] = f->description;
            m["type"] = (f->type == CFG_BOOL) ? "bool" :
                        (f->type == CFG_FLOAT) ? "float" : "u32";
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ----- GET /api/storage -----
    s_server->on("/api/storage", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        doc["total"] = StorageManager::totalBytes();
        doc["used"]  = StorageManager::usedBytes();
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // ----- POST /api/orch -----
    s_server->on("/api/orch", HTTP_POST, [](AsyncWebServerRequest* request) {
        // Handled by body callback
    }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            request->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        if (doc["mode"].is<uint8_t>()) {
            Orchestrator::setMode((OrchMode)doc["mode"].as<uint8_t>());
        }
        if (doc["travel_order"].is<uint8_t>()) {
            Orchestrator::setTravelOrder((TravelOrder)doc["travel_order"].as<uint8_t>());
        }
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // ----- POST /api/config -----
    s_server->on("/api/config", HTTP_POST, [](AsyncWebServerRequest* request) {
        // Handled by body callback
    }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            request->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        uint8_t applied = configApplyJson(doc.as<JsonObjectConst>());
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"applied\":%u}", applied);
        request->send(200, "application/json", buf);
    });

    // ----- POST /api/sequence -----
    s_server->on("/api/sequence", HTTP_POST, [](AsyncWebServerRequest* request) {
        // Handled by body callback
    }, nullptr, [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len)) {
            request->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        Orchestrator::clearSequence();
        JsonArray steps = doc["steps"].as<JsonArray>();
        for (JsonObject s : steps) {
            Orchestrator::addSequenceStep(
                s["node"].as<uint8_t>(),
                s["tone"].as<uint8_t>(),
                s["delay"].as<uint16_t>()
            );
        }
        if (doc["save"].as<bool>()) {
            Orchestrator::saveSequence();
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"steps\":%u}", Orchestrator::sequenceCount());
        request->send(200, "application/json", buf);
    });

    // ----- POST /api/sweep -----
    s_server->on("/api/sweep", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!MeshConductor::isGateway()) {
            request->send(403, "application/json", "{\"error\":\"gateway only\"}");
            return;
        }
        FtmScheduler::enqueueFullSweep();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // ----- POST /api/reboot -----
    s_server->on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", "{\"ok\":true}");
        // Delay to let the response flush before restarting
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    });

    // Catch-all: try to serve from LittleFS, else 404
    s_server->onNotFound([](AsyncWebServerRequest* request) {
        if (!StorageManager::serveFile(request, request->url().c_str())) {
            request->send(404, "text/plain", "404 — Not Found");
        }
    });
}

// ---------------------------------------------------------------------------
// DNS start/stop
// ---------------------------------------------------------------------------
void SqWebServer::startDNS() {
    if (s_dnsTask) return;
    s_dnsStop = false;
    xTaskCreate(dnsTask, "dns53", 3072, nullptr, 2, &s_dnsTask);
}

void SqWebServer::stopDNS() {
    if (!s_dnsTask) return;
    s_dnsStop = true;
    // Give the task time to see the flag and exit (recv timeout is 1s)
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_dnsTask = nullptr;
}

// ---------------------------------------------------------------------------
// IP event — start mDNS + NTP when the root gets a STA IP from the router
// ---------------------------------------------------------------------------
static void onStaGotIp(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    ESP_LOGI(TAG, "Got router IP: " IPSTR, IP2STR(&event->ip_info.ip));

    if (s_staMode) return;  // already initialized

    // mDNS
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("squeek");
        mdns_instance_name_set("Squeek Mesh Controller");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS started: squeek.local");
    }
    // NTP
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "NTP sync started");
    s_staMode = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SqWebServer::start() {
    if (s_running) {
        ESP_LOGW(TAG, "already running");
        return;
    }

    // Ensure filesystem is mounted
    StorageManager::init();

    // Create server and websocket
    s_server = new AsyncWebServer(80);
    s_ws     = new AsyncWebSocket("/ws");

    s_ws->onEvent([](AsyncWebSocket* server, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len) {
        SqWebServer::onWsEvent(server, client, (int)type, arg, data, len);
    });

    s_server->addHandler(s_ws);

    registerRoutes();
    s_server->begin();

    // DNS captive portal is NOT started here — it belongs to Delegate only.

    s_running = true;

    // mDNS + NTP start when we get a STA IP from the router (async via IP event)
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onStaGotIp, NULL);

    // Log the best reachable address
    esp_netif_ip_info_t log_ip = {};
    esp_netif_t* log_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (log_sta && esp_netif_get_ip_info(log_sta, &log_ip) == ESP_OK && log_ip.ip.addr != 0) {
        ESP_LOGI(TAG, "Web server started — http://" IPSTR "/", IP2STR(&log_ip.ip));
    } else {
        uint32_t addr = getApIpAddr();
        if (addr) {
            ESP_LOGI(TAG, "Web server started — http://%u.%u.%u.%u/ (AP, waiting for router DHCP)",
                     (addr >> 0) & 0xFF, (addr >> 8) & 0xFF,
                     (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
        } else {
            ESP_LOGI(TAG, "Web server started (no IP yet)");
        }
    }
}

void SqWebServer::stop() {
    if (!s_running) return;

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &onStaGotIp);

    if (s_staMode) {
        mdns_service_remove_all();
        mdns_free();
        esp_sntp_stop();
        s_staMode = false;
        ESP_LOGI(TAG, "mDNS + NTP stopped");
    }

    stopDNS();  // no-op if DNS wasn't started (Delegate manages its own)

    if (s_ws) {
        s_ws->closeAll();
    }

    if (s_server) {
        s_server->end();
        delete s_server;
        s_server = nullptr;
    }

    if (s_ws) {
        delete s_ws;
        s_ws = nullptr;
    }

    s_running = false;
    ESP_LOGI(TAG, "Web server stopped");
}

bool SqWebServer::isRunning() {
    return s_running;
}

void SqWebServer::broadcast(const char* json) {
    if (s_ws && s_ws->count() > 0) {
        s_ws->textAll(json);
    }
}

void SqWebServer::broadcastPeers() {
    if (!s_ws || s_ws->count() == 0) return;
    broadcast("{\"type\":\"peer_update\"}");
}

void SqWebServer::broadcastOrchState() {
    if (!s_ws || s_ws->count() == 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"orch_update\",\"mode\":%u,\"travel_order\":%u}",
             (uint8_t)Orchestrator::getMode(), (uint8_t)Orchestrator::getTravelOrder());
    broadcast(buf);
}
