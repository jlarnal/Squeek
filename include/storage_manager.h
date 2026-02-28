#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <stdint.h>
#include <stddef.h>

class AsyncWebServerRequest;  // forward decl

class StorageManager {
public:
    StorageManager() = delete;

    static bool init();          // mount LittleFS, format on first use
    static bool isReady();

    // Storage stats
    static size_t totalBytes();
    static size_t usedBytes();

    // Serve a file from LittleFS to an HTTP request (gzip-transparent)
    // Returns true if file was found and response sent, false if not found
    static bool serveFile(AsyncWebServerRequest* request, const char* path);

    // File operations for sample management
    static bool exists(const char* path);
    static bool remove(const char* path);
};

#endif // STORAGE_MANAGER_H
