#include "web_server.h"
#include "storage_manager.h"

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <WiFi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <esp_netif.h>
#include <esp_mesh.h>

static const char* TAG = "webserver";

// ---- file-scope state -----------------------------------------------------
static AsyncWebServer* s_server  = nullptr;
static AsyncWebSocket*  s_ws     = nullptr;
static bool             s_running = false;
static TaskHandle_t     s_dnsTask = nullptr;
static volatile bool    s_dnsStop = false;

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
void SqWebServer::registerRoutes() {
    // Root → index.html
    s_server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!StorageManager::serveFile(request, "/index.html")) {
            request->send(200, "text/html",
                "<!DOCTYPE html><html><body>"
                "<h1>Squeek</h1><p>No UI uploaded yet. Use OTA to upload.</p>"
                "</body></html>");
        }
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
    startDNS();

    s_running = true;

    uint32_t addr = getApIpAddr();
    ESP_LOGI(TAG, "Web server started — http://%u.%u.%u.%u/",
             (addr >> 0) & 0xFF, (addr >> 8) & 0xFF,
             (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
}

void SqWebServer::stop() {
    if (!s_running) return;

    stopDNS();

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
