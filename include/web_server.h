#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stddef.h>

class AsyncWebSocket;
class AsyncWebSocketClient;

class SqWebServer {
public:
    SqWebServer() = delete;

    static void start();    // called from Gateway::begin()
    static void stop();     // called from Gateway::end()
    static bool isRunning();

    // WebSocket broadcast JSON to all connected clients
    static void broadcast(const char* json);

    // WiFi credential management (raw NVS string API)
    static bool loadWifiCreds(char* ssid, size_t ssidLen, char* pass, size_t passLen);
    static bool saveWifiCreds(const char* ssid, const char* pass);
    static bool clearWifiCreds();
    static bool hasWifiCreds();

    // DNS captive portal (public so SetupDelegate can reuse)
    static void startDNS();
    static void stopDNS();

private:
    static void registerRoutes();
    static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                          int type, void* arg, uint8_t* data, size_t len);
};

#endif // WEB_SERVER_H
