#include "storage_manager.h"

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "storage";
static bool s_mounted = false;

// ---------------------------------------------------------------------------
// MIME type helper
// ---------------------------------------------------------------------------
static const char* mimeTypeFor(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    if (strcasecmp(dot, ".html") == 0) return "text/html";
    if (strcasecmp(dot, ".js")   == 0) return "application/javascript";
    if (strcasecmp(dot, ".css")  == 0) return "text/css";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".mp3")  == 0) return "audio/mpeg";
    if (strcasecmp(dot, ".gz")   == 0) return "application/gzip";

    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// init — mount LittleFS on the "storage" partition
// ---------------------------------------------------------------------------
bool StorageManager::init() {
    if (s_mounted) return true;

    if (!LittleFS.begin(true, "/littlefs", 10, "storage")) {
        ESP_LOGE(TAG, "LittleFS mount failed");
        return false;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "LittleFS mounted — total %u B, used %u B",
             (unsigned)LittleFS.totalBytes(), (unsigned)LittleFS.usedBytes());
    return true;
}

// ---------------------------------------------------------------------------
bool StorageManager::isReady() {
    return s_mounted;
}

// ---------------------------------------------------------------------------
size_t StorageManager::totalBytes() {
    return s_mounted ? LittleFS.totalBytes() : 0;
}

size_t StorageManager::usedBytes() {
    return s_mounted ? LittleFS.usedBytes() : 0;
}

// ---------------------------------------------------------------------------
// serveFile — gzip-transparent file serving
// ---------------------------------------------------------------------------
bool StorageManager::serveFile(AsyncWebServerRequest* request, const char* path) {
    if (!s_mounted || !request || !path) return false;

    // MIME type from the original (non-.gz) extension
    const char* mime = mimeTypeFor(path);

    // LittleFS paths are relative to mount point (e.g. "/index.html")
    String gzPath = String(path) + ".gz";

    // Prefer gzip version
    if (LittleFS.exists(gzPath.c_str())) {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, gzPath, mime);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
        return true;
    }

    // Fallback to uncompressed
    if (LittleFS.exists(path)) {
        request->send(LittleFS, path, mime);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
bool StorageManager::exists(const char* path) {
    return s_mounted && LittleFS.exists(path);
}

bool StorageManager::remove(const char* path) {
    return s_mounted && LittleFS.remove(path);
}
