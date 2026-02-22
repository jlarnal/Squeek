#include "debug_cli.h"
#include "bsp.hpp"
#include "nvs_config.h"
#include "nvs_config_registry.h"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_conductor.h"
#include "rtc_mesh_map.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "position_solver.h"
#include "sq_log.h"
#include "audio_engine.h"
#include "audio_tweeter.h"
#include "tone_library.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <WiFi.h>
#include <string.h>

// --- Command handler prototypes ---
static void cmd_help(const char* args);
static void cmd_led(const char* args);
static void cmd_battery(const char* args);
static void cmd_wifi(const char* args);
static void cmd_mesh(const char* args);
static void cmd_elect(const char* args);
static void cmd_rtc(const char* args);
static void cmd_sleep(const char* args);
static void cmd_peers(const char* args);
static void cmd_ftm(const char* args);
static void cmd_sweep(const char* args);
static void cmd_solve(const char* args);
static void cmd_broadcast(const char* args);
static void cmd_quiet(const char* args);
static void cmd_tone(const char* args);
static void cmd_config(const char* args);
static void cmd_mode(const char* args);
static void cmd_status(const char* args);
static void cmd_reboot(const char* args);

// --- Command table ---
struct CliCommand {
    const char* name;
    void (*handler)(const char* args);
    const char* description;
};

static const CliCommand s_commands[] = {
    { "help",      cmd_help,      "List all commands" },
    { "led",       cmd_led,       "Blink status LED + RGB R/G/B test" },
    { "battery",   cmd_battery,   "Read battery voltage and status" },
    { "wifi",      cmd_wifi,      "Scan nearby APs" },
    { "mesh",      cmd_mesh,      "Join mesh, show peers, then stop" },
    { "elect",     cmd_elect,     "Force gateway re-election" },
    { "rtc",       cmd_rtc,       "RTC memory write/readback test" },
    { "sleep",     cmd_sleep,     "Light sleep [seconds] (default 5)" },
    { "peers",     cmd_peers,     "Show PeerTable (synced from gateway)" },
    { "tone",      cmd_tone,      "Interactive tone player (numpad)" },
    { "config",    cmd_config,    "Get/set NVS config locally or on peers" },
    { "mode",      cmd_mode,      "Set role: 'mode gateway' or 'mode peer'" },
    { "ftm",       cmd_ftm,       "FTM single-shot to first peer" },
    { "sweep",     cmd_sweep,     "FTM full sweep, print distance matrix" },
    { "solve",     cmd_solve,     "Run MDS position solver" },
    { "broadcast", cmd_broadcast, "Broadcast positions to all nodes" },
    { "quiet",     cmd_quiet,     "Toggle background output suppression" },
    { "status",    cmd_status,    "Print mesh state, role, battery, peers" },
    { "reboot",    cmd_reboot,    "Reboot (esp_restart)" },
};
static constexpr int CMD_COUNT = sizeof(s_commands) / sizeof(s_commands[0]);

// --- Command implementations ---

static void cmd_help(const char* args) {
    (void)args;
    Serial.println("Available commands:");
    for (int i = 0; i < CMD_COUNT; i++) {
        Serial.printf("  %-10s  %s\n", s_commands[i].name, s_commands[i].description);
    }
}

static void cmd_led(const char* args) {
    (void)args;
    auto saved = LedDriver::saveState();

    Serial.println("LED test: status LED blink...");
    LedDriver::statusFlash(200, 200, 3);

    Serial.println("RGB: Red...");
    LedDriver::rgbSet(255, 0, 0);
    delay(500);

    Serial.println("RGB: Green...");
    LedDriver::rgbSet(0, 255, 0);
    delay(500);

    Serial.println("RGB: Blue...");
    LedDriver::rgbSet(0, 0, 255);
    delay(500);

    LedDriver::restoreState(saved);
    Serial.println("LED test done.");
}

static void cmd_battery(const char* args) {
    (void)args;
    PowerManager::init();
    uint32_t raw = PowerManager::batteryRaw();
    uint32_t mv  = PowerManager::batteryMv();
    Serial.printf("Battery RAW: %lu\n", raw);
    Serial.printf("Battery mV:  %lu\n", mv);
    Serial.printf("Low: %s  Critical: %s\n",
        PowerManager::isLowBattery() ? "YES" : "no",
        PowerManager::isCriticalBattery() ? "YES" : "no");
}

static void cmd_wifi(const char* args) {
    (void)args;
    Serial.println("Scanning WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks();
    if (n == 0) {
        Serial.println("No networks found.");
    } else {
        Serial.printf("Found %d networks:\n", n);
        for (int i = 0; i < n; i++) {
            Serial.printf("  [%d] %-32s  RSSI:%d  CH:%d\n",
                i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
        }
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
}

static void cmd_mesh(const char* args) {
    (void)args;
    Serial.println("Initializing mesh...");
    RtcMap::init();
    MeshConductor::init();
    MeshConductor::start();

    Serial.println("Waiting for mesh (30s timeout)...");
    unsigned long deadline = millis() + 30000;
    while (!MeshConductor::isConnected() && millis() < deadline) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (MeshConductor::isConnected()) {
        Serial.println("Mesh connected!");
    } else {
        Serial.println("Mesh timeout -- may still be forming.");
    }
    MeshConductor::printStatus();
    RtcMap::print();

    MeshConductor::stop();
}

static void cmd_elect(const char* args) {
    (void)args;
    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected. Run 'mesh' first.");
        return;
    }
    Serial.println("Forcing re-election (will reboot)...");
    Serial.flush();
    MeshConductor::forceReelection();
}

static void cmd_rtc(const char* args) {
    (void)args;
    Serial.println("RTC memory test...");
    RtcMap::init();

    rtc_mesh_map_t* map  = RtcMap::get();
    map->own_short_id    = 42;
    map->mesh_generation = 12345;
    map->peer_count      = 1;
    memset(map->peers[0].mac, 0xAA, 6);
    map->peers[0].short_id = 1;
    map->peers[0].flags    = PEER_FLAG_ALIVE;
    RtcMap::save();

    Serial.println("Written test data:");
    RtcMap::print();

    if (RtcMap::isValid()) {
        Serial.println("PASS: checksum valid after save.");
    } else {
        Serial.println("FAIL: checksum invalid!");
    }

    RtcMap::clear();
    if (!RtcMap::isValid()) {
        Serial.println("PASS: map invalid after clear.");
    } else {
        Serial.println("FAIL: map still valid after clear!");
    }

    RtcMap::init();
    Serial.println("Re-initialized:");
    RtcMap::print();
}

static void cmd_sleep(const char* args) {
    uint32_t secs = 5;
    if (args && *args) {
        int val = atoi(args);
        if (val > 0) secs = (uint32_t)val;
    }

    Serial.printf("Sleeping for %lu seconds...\n", secs);
    Serial.flush();
    PowerManager::init();
    PowerManager::lightSleep(secs);
    Serial.println("Woke up from light sleep!");
}

static void cmd_peers(const char* args) {
    (void)args;
    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected. Run 'mesh' first.");
        return;
    }
    if (MeshConductor::isGateway()) {
        PeerTable::print();
    } else {
        MeshConductor::printPeerShadow();
    }
}

// Numpad key-to-tone mapping (index 0-9, nullptr = unassigned)
static const struct { const char* name; const char* label; } s_padSlots[10] = {
    { nullptr,      "stop"       },  // 0
    { "chirp",      "chirp"      },  // 1
    { "chirp_down", "chirp down" },  // 2
    { "squeak",     "squeak"     },  // 3
    { "warble",     "warble"     },  // 4
    { "alert",      "alert"      },  // 5
    { "fade_chirp", "fade chirp" },  // 6
    { nullptr,      "---"        },  // 7
    { nullptr,      "---"        },  // 8
    { nullptr,      "---"        },  // 9
};

static void tonePadDraw(const char* status) {
    Serial.println("Tone Player (press key, '.' to quit)");
    Serial.println("\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90");  // ┌───────┬───────┬───────┐
    // Row: 7 8 9
    Serial.printf("\xe2\x94\x82 7     \xe2\x94\x82 8     \xe2\x94\x82 9     \xe2\x94\x82\n");
    Serial.printf("\xe2\x94\x82 %-5s \xe2\x94\x82 %-5s \xe2\x94\x82 %-5s \xe2\x94\x82\n", s_padSlots[7].label, s_padSlots[8].label, s_padSlots[9].label);
    Serial.println("\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");  // ├───────┼───────┼───────┤
    // Row: 4 5 6
    Serial.printf("\xe2\x94\x82 4     \xe2\x94\x82 5     \xe2\x94\x82 6     \xe2\x94\x82\n");
    Serial.printf("\xe2\x94\x82 %-5s \xe2\x94\x82 %-5s \xe2\x94\x82 %-5s \xe2\x94\x82\n", s_padSlots[4].label, s_padSlots[5].label, s_padSlots[6].label);
    Serial.println("\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");  // ├───────┼───────┼───────┤
    // Row: 1 2 3
    Serial.printf("\xe2\x94\x82 1     \xe2\x94\x82 2     \xe2\x94\x82 3     \xe2\x94\x82\n");
    Serial.printf("\xe2\x94\x82 %-5s \xe2\x94\x82 %-5s \xe2\x94\x82 %-5s \xe2\x94\x82\n", s_padSlots[1].label, s_padSlots[2].label, s_padSlots[3].label);
    Serial.println("\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4");  // ├───────┴───────┼───────┤
    Serial.println("\xe2\x94\x82     0 = stop  \xe2\x94\x82 . quit\xe2\x94\x82");
    Serial.println("\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98");  // └───────────────┴───────┘
    if (status && *status) {
        Serial.printf("[%s]\n", status);
    }
}

static void cmd_tone(const char* args) {
    (void)args;
    tonePadDraw(nullptr);

    for (;;) {
        if (!Serial.available()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        char c = Serial.read();

        if (c == '.' || c == 127) {
            // Exit tone mode
            AudioEngine::stop();
            Serial.println("Tone player closed.");
            return;
        }

        if (c >= '0' && c <= '9') {
            int idx = c - '0';
            if (idx == 0) {
                AudioEngine::stop();
            } else if (s_padSlots[idx].name) {
                const ToneSequence* seq = ToneLibrary::get(s_padSlots[idx].name);
                if (seq) AudioEngine::play(seq);
            }
        }
        // Ignore other keys silently
    }
}

static uint8_t s_configReqId = 0;

static void configDumpLocal() {
    JsonDocument doc;
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
        own_mac[0], own_mac[1], own_mac[2],
        own_mac[3], own_mac[4], own_mac[5]);
    doc["mac"] = macStr;
    configBuildJson(doc, nullptr, 0);
    serializeJsonPretty(doc, Serial);
    Serial.println();
}

static void configSetLocal(const char* args) {
    // Parse key=val pairs from args
    char buf[128];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    JsonDocument doc;
    char* token = strtok(buf, " ");
    while (token) {
        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            const char* key = token;
            const char* val = eq + 1;
            const ConfigField* f = configLookup(key);
            if (!f) {
                Serial.printf("Unknown field: %s\n", key);
            } else if (f->type == CFG_BOOL) {
                doc[key] = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
            } else if (f->type == CFG_FLOAT) {
                doc[key] = (float)atof(val);
            } else {
                doc[key] = (uint32_t)strtoul(val, nullptr, 0);
            }
        } else {
            Serial.printf("Invalid pair (expected key=val): %s\n", token);
        }
        token = strtok(nullptr, " ");
    }

    uint8_t applied = configApplyJson(doc.as<JsonObjectConst>());
    Serial.printf("Applied %u field(s) locally.\n", applied);
    configDumpLocal();
}

static void configRemoteGetSet(bool isSet, const char* target, const char* rest) {
    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected.");
        return;
    }

    // Build JSON request
    JsonDocument reqDoc;
    if (isSet) {
        reqDoc["action"] = "set";
        // Parse key=val pairs from rest
        if (rest && *rest) {
            char buf[128];
            strncpy(buf, rest, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char* token = strtok(buf, " ");
            while (token) {
                char* eq = strchr(token, '=');
                if (eq) {
                    *eq = '\0';
                    const char* key = token;
                    const char* val = eq + 1;
                    const ConfigField* f = configLookup(key);
                    if (!f) {
                        Serial.printf("Unknown field: %s\n", key);
                    } else if (f->type == CFG_BOOL) {
                        reqDoc[key] = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
                    } else if (f->type == CFG_FLOAT) {
                        reqDoc[key] = (float)atof(val);
                    } else {
                        reqDoc[key] = (uint32_t)strtoul(val, nullptr, 0);
                    }
                }
                token = strtok(nullptr, " ");
            }
        }
    } else {
        reqDoc["action"] = "get";
        // Parse optional field names from rest
        if (rest && *rest) {
            JsonArray arr = reqDoc["fields"].to<JsonArray>();
            char buf[128];
            strncpy(buf, rest, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char* token = strtok(buf, " ");
            while (token) {
                arr.add(token);
                token = strtok(nullptr, " ");
            }
        }
    }

    char reqJson[256];
    serializeJson(reqDoc, reqJson, sizeof(reqJson));

    // Determine target peer(s)
    bool allPeers = (strcmp(target, "*") == 0);

    if (allPeers) {
        // Include local node in * operations
        if (isSet) {
            configApplyJson(reqDoc.as<JsonObjectConst>());
        }
        // Show local config (filtered by requested fields for get, full dump for set)
        {
            JsonDocument localDoc;
            uint8_t own_mac[6];
            esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
            char macStr[18];
            snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                own_mac[0], own_mac[1], own_mac[2],
                own_mac[3], own_mac[4], own_mac[5]);
            localDoc["mac"] = macStr;
            if (!isSet && rest && *rest) {
                // Parse field names for filtered get
                char fbuf[128];
                strncpy(fbuf, rest, sizeof(fbuf) - 1);
                fbuf[sizeof(fbuf) - 1] = '\0';
                const char* fptrs[16];
                uint8_t fc = 0;
                char* tok = strtok(fbuf, " ");
                while (tok && fc < 16) { fptrs[fc++] = tok; tok = strtok(nullptr, " "); }
                configBuildJson(localDoc, fptrs, fc);
            } else {
                configBuildJson(localDoc, nullptr, 0);
            }
            Serial.print("[local] ");
            serializeJson(localDoc, Serial);
            Serial.println();
        }

        uint8_t count = MeshConductor::isGateway() ? PeerTable::peerCount() : MeshConductor::peerShadowCount();
        uint8_t own_mac[6];
        esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

        for (uint8_t i = 0; i < count; i++) {
            PeerEntry* e = MeshConductor::isGateway() ? PeerTable::getEntryByIndex(i) : nullptr;
            if (!e) continue;
            if (memcmp(e->mac, own_mac, 6) == 0) continue;
            if (e->flags & PEER_STATUS_DEAD) continue;

            uint8_t reqId = ++s_configReqId;
            Serial.printf("[%u] Requesting %02X:%02X... ", i, e->mac[4], e->mac[5]);
            if (MeshConductor::sendConfigReq(e->mac, reqJson, reqId)) {
                char resp[480];
                if (MeshConductor::waitConfigResp(resp, sizeof(resp), 5000)) {
                    Serial.println(resp);
                } else {
                    Serial.println("TIMEOUT");
                }
            } else {
                Serial.println("SEND FAILED");
            }
        }
    } else {
        int slot = atoi(target);
        PeerEntry* e = MeshConductor::isGateway() ? PeerTable::getEntryByIndex((uint8_t)slot) : nullptr;
        if (!e) {
            Serial.printf("Peer slot %d not found.\n", slot);
            return;
        }
        if (e->flags & PEER_STATUS_DEAD) {
            Serial.printf("Peer slot %d is dead.\n", slot);
            return;
        }

        uint8_t reqId = ++s_configReqId;
        Serial.printf("Requesting slot %d (%02X:%02X:%02X:%02X:%02X:%02X)...\n",
            slot, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);

        if (MeshConductor::sendConfigReq(e->mac, reqJson, reqId)) {
            char resp[480];
            if (MeshConductor::waitConfigResp(resp, sizeof(resp), 5000)) {
                Serial.println(resp);
            } else {
                Serial.println("TIMEOUT — no response from peer.");
            }
        } else {
            Serial.println("Failed to send config request.");
        }
    }
}

static void cmd_config(const char* args) {
    if (!args || !*args) {
        configDumpLocal();
        return;
    }

    // Tokenize: first word is subcommand
    char buf[128];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* subcmd = buf;
    char* rest = nullptr;
    for (int i = 0; buf[i]; i++) {
        if (buf[i] == ' ') {
            buf[i] = '\0';
            rest = &buf[i + 1];
            break;
        }
    }

    if (strcasecmp(subcmd, "list") == 0) {
        configListFields(Serial);
        return;
    }

    if (strcasecmp(subcmd, "get") == 0) {
        if (!rest || !*rest) {
            Serial.println("Usage: config get <slot|*> [field1 field2...]");
            return;
        }
        // Extract target (first token of rest)
        char* target = rest;
        char* fields = nullptr;
        for (int i = 0; rest[i]; i++) {
            if (rest[i] == ' ') {
                rest[i] = '\0';
                fields = &rest[i + 1];
                break;
            }
        }
        configRemoteGetSet(false, target, fields);
        return;
    }

    if (strcasecmp(subcmd, "set") == 0) {
        if (!rest || !*rest) {
            Serial.println("Usage: config set <slot|*|local> key=val [key=val...]");
            return;
        }
        // Extract target
        char* target = rest;
        char* pairs = nullptr;
        for (int i = 0; rest[i]; i++) {
            if (rest[i] == ' ') {
                rest[i] = '\0';
                pairs = &rest[i + 1];
                break;
            }
        }

        if (strcasecmp(target, "local") == 0) {
            if (pairs && *pairs) {
                configSetLocal(pairs);
            } else {
                Serial.println("Usage: config set local key=val [key=val...]");
            }
            return;
        }

        if (!pairs || !*pairs) {
            Serial.println("Usage: config set <slot|*> key=val [key=val...]");
            return;
        }
        configRemoteGetSet(true, target, pairs);
        return;
    }

    Serial.println("Usage: config [list|get <slot|*> [fields...]|set <slot|*|local> key=val...]");
}

static void cmd_mode(const char* args) {
    if (!args || !*args) {
        Serial.println("Usage: mode gateway | mode peer");
        return;
    }

    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected.");
        return;
    }

    if (strcasecmp(args, "gateway") == 0) {
        if (MeshConductor::isGateway()) {
            Serial.println("Already gateway.");
            return;
        }
        Serial.println("Requesting gateway role...");
        Serial.flush();
        NominateMsg msg;
        msg.type = MSG_TYPE_NOMINATE;
        esp_read_mac(msg.mac, ESP_MAC_WIFI_STA);
        // Send to logical gateway (may differ from ESP-IDF root after role transfer)
        const uint8_t* gw = MeshConductor::gatewayMac();
        static const uint8_t zero[6] = {0};
        if (memcmp(gw, zero, 6) != 0) {
            MeshConductor::sendToNode(gw, &msg, sizeof(msg));
        } else {
            MeshConductor::sendToRoot(&msg, sizeof(msg));
        }
    } else if (strcasecmp(args, "peer") == 0) {
        if (!MeshConductor::isGateway()) {
            Serial.println("Already a peer node.");
            return;
        }
        Serial.println("Stepping down from gateway...");
        Serial.flush();
        MeshConductor::stepDown();
    } else {
        Serial.println("Usage: mode gateway | mode peer");
    }
}

static void cmd_ftm(const char* args) {
    (void)args;
    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected. Run 'mesh' first.");
        return;
    }

    Serial.println("FTM single-shot test");

    if (MeshConductor::isGateway() && PeerTable::peerCount() >= 2) {
        PeerEntry* peer = PeerTable::getEntryByIndex(1);
        if (peer && !(peer->flags & PEER_STATUS_DEAD)) {
            Serial.printf("Ranging to peer slot 1: %02X:%02X:%02X:%02X:%02X:%02X (SoftAP: %02X:%02X:%02X:%02X:%02X:%02X)\n",
                peer->mac[0], peer->mac[1], peer->mac[2],
                peer->mac[3], peer->mac[4], peer->mac[5],
                peer->softap_mac[0], peer->softap_mac[1], peer->softap_mac[2],
                peer->softap_mac[3], peer->softap_mac[4], peer->softap_mac[5]);

            float dist = FtmManager::initiateSession(peer->softap_mac, MESH_CHANNEL, (uint8_t)(uint32_t)NvsConfigManager::ftmSamplesPerPair);
            if (dist >= 0) {
                Serial.printf("SUCCESS: distance = %.1f cm (%.2f m)\n", dist, dist / 100.0f);
            } else {
                Serial.println("FAILED: FTM session did not succeed");
            }
            return;
        }
    }

    Serial.printf("No peer available for FTM. PeerTable has %d entries (need >= 2).\n",
        PeerTable::peerCount());
    if (!MeshConductor::isGateway()) {
        Serial.println("(Not gateway -- FTM ranging only runs on gateway)");
    } else if (PeerTable::peerCount() >= 2) {
        PeerEntry* p = PeerTable::getEntryByIndex(1);
        if (p) {
            Serial.printf("Slot 1 SoftAP: %02X:%02X:%02X:%02X:%02X:%02X flags=0x%02X\n",
                p->softap_mac[0], p->softap_mac[1], p->softap_mac[2],
                p->softap_mac[3], p->softap_mac[4], p->softap_mac[5], p->flags);
        }
    } else {
        Serial.println("(Peer must have sent a heartbeat so its SoftAP MAC is in PeerTable)");
    }
}

static void cmd_sweep(const char* args) {
    (void)args;
    if (!MeshConductor::isGateway()) {
        Serial.println("Not gateway -- FTM sweep only runs on gateway.");
        return;
    }
    if (PeerTable::peerCount() < 2) {
        Serial.println("Need at least 2 nodes for sweep.");
        return;
    }

    Serial.println("Starting full FTM sweep...");
    FtmScheduler::enqueueFullSweep();

    unsigned long deadline = millis() + 120000;
    while (FtmScheduler::isActive() && millis() < deadline) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println();

    if (FtmScheduler::isActive()) {
        Serial.println("Sweep timed out (still active).");
    } else {
        Serial.println("Sweep complete.");
    }

    uint8_t n = PeerTable::peerCount();
    Serial.println("Distance matrix (cm):");
    Serial.print("      ");
    for (uint8_t j = 0; j < n; j++) Serial.printf(" %5u", j);
    Serial.println();
    for (uint8_t i = 0; i < n; i++) {
        Serial.printf("  [%u] ", i);
        for (uint8_t j = 0; j < n; j++) {
            float d = PeerTable::getDistance(i, j);
            if (i == j) Serial.print("    - ");
            else if (d < 0) Serial.print("    ? ");
            else Serial.printf("%5.0f ", d);
        }
        Serial.println();
    }
}

static void cmd_solve(const char* args) {
    (void)args;
    if (!MeshConductor::isGateway()) {
        Serial.println("Not gateway -- solver only runs on gateway.");
        return;
    }

    Serial.println("Running MDS position solver...");
    PositionSolver::solve();

    uint8_t n = PeerTable::peerCount();
    uint8_t dim = PeerTable::getDimension();
    Serial.printf("Positions (%uD):\n", dim);
    for (uint8_t i = 0; i < n; i++) {
        PeerEntry* e = PeerTable::getEntryByIndex(i);
        if (e) {
            Serial.printf("  [%u] %02X:%02X  pos=(%.0f, %.0f, %.0f) cm  conf=%.2f\n",
                i, e->mac[4], e->mac[5],
                e->position[0], e->position[1], e->position[2], e->confidence);
        }
    }
}

static void cmd_broadcast(const char* args) {
    (void)args;
    if (!MeshConductor::isGateway()) {
        Serial.println("Not gateway.");
        return;
    }
    FtmScheduler::broadcastPositions();
    Serial.println("Positions broadcast sent.");
}

static void cmd_quiet(const char* args) {
    (void)args;
    bool newState = !SqLogClass::isQuiet();
    SqLogClass::setQuiet(newState);
    Serial.printf("Quiet mode: %s\n", newState ? "ON (background output suppressed)" : "OFF");
}

static void cmd_status(const char* args) {
    (void)args;
    Serial.printf("Squeek v%s\n", SQUEEK_VERSION);
    Serial.printf("Battery: %lu mV\n", PowerManager::batteryMv());
    Serial.printf("Mesh connected: %s\n", MeshConductor::isConnected() ? "yes" : "no");
    Serial.printf("Role: %s\n", MeshConductor::isGateway() ? "GATEWAY" : "NODE");
    if (MeshConductor::isConnected()) {
        MeshConductor::printStatus();
    }
}

static void cmd_reboot(const char* args) {
    (void)args;
    Serial.println("Rebooting...");
    Serial.flush();
    esp_restart();
}

// --- CLI History ---

static constexpr uint8_t HIST_MAX = 3;
static char   s_history[HIST_MAX][128];
static uint8_t s_histCount  = 0;   // total entries stored (0..HIST_MAX)
static uint8_t s_histWrite  = 0;   // next write slot (circular)

static void histPush(const char* line) {
    // Skip if duplicate of most recent entry
    if (s_histCount > 0) {
        uint8_t last = (s_histWrite + HIST_MAX - 1) % HIST_MAX;
        if (strcmp(s_history[last], line) == 0) return;
    }
    strncpy(s_history[s_histWrite], line, 127);
    s_history[s_histWrite][127] = '\0';
    s_histWrite = (s_histWrite + 1) % HIST_MAX;
    if (s_histCount < HIST_MAX) s_histCount++;
}

// Erase current line on terminal, replace with new content
static void lineReplace(char* lineBuf, uint8_t& linePos, const char* newLine) {
    // Move cursor to start, overwrite with spaces, move back
    Serial.print('\r');
    Serial.print("> ");
    for (uint8_t i = 0; i < linePos; i++) Serial.print(' ');
    // Write new content
    linePos = (uint8_t)strlen(newLine);
    memcpy(lineBuf, newLine, linePos);
    lineBuf[linePos] = '\0';
    Serial.print('\r');
    Serial.print("> ");
    Serial.print(lineBuf);
}

// --- CLI Task ---

static void debugCliTask(void* pvParameters) {
    (void)pvParameters;
    char lineBuf[128];
    uint8_t linePos = 0;

    // Tab-cycle history: -1 = not browsing, 0 = most recent, etc.
    int8_t browseIdx = -1;

    Serial.println("Squeek CLI ready. Type 'help' for commands. Tab = history.");
    Serial.print("> ");

    for (;;) {
        if (!Serial.available()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char c = Serial.read();

        // Tab on empty line: cycle through history
        if (c == '\t') {
            if (s_histCount == 0) continue;
            browseIdx++;
            if (browseIdx >= (int8_t)s_histCount) {
                // Wrapped past oldest — clear line
                browseIdx = -1;
                lineReplace(lineBuf, linePos, "");
            } else {
                uint8_t slot = (s_histWrite + HIST_MAX - 1 - browseIdx) % HIST_MAX;
                lineReplace(lineBuf, linePos, s_history[slot]);
            }
            continue;
        }

        // Any non-Tab key resets browse state
        if (browseIdx >= 0 && c != '\n' && c != '\r') {
            browseIdx = -1;
        }

        if (c == '\n' || c == '\r') {
            browseIdx = -1;
            if (linePos == 0) {
                Serial.print("\n> ");
                continue;
            }
            Serial.println();
            lineBuf[linePos] = '\0';

            // Save full line before parsing mutates it
            char savedLine[128];
            memcpy(savedLine, lineBuf, linePos + 1);

            // Parse command and args
            char* cmd = lineBuf;
            char* args = nullptr;
            for (uint8_t i = 0; i < linePos; i++) {
                if (lineBuf[i] == ' ') {
                    lineBuf[i] = '\0';
                    args = &lineBuf[i + 1];
                    break;
                }
            }

            // Lookup and dispatch
            bool found = false;
            for (int i = 0; i < CMD_COUNT; i++) {
                if (strcasecmp(cmd, s_commands[i].name) == 0) {
                    s_commands[i].handler(args);
                    found = true;
                    break;
                }
            }
            if (!found) {
                Serial.printf("Unknown command: '%s'. Type 'help'.\n", cmd);
            } else {
                histPush(savedLine);
            }

            linePos = 0;
            Serial.print("> ");
        } else if (c == '\b' || c == 127) {
            if (linePos > 0) {
                linePos--;
                Serial.print("\b \b");
            }
        } else if (c >= 0x20 && linePos < sizeof(lineBuf) - 1) {
            lineBuf[linePos++] = c;
            Serial.print(c);
        }
    }
}

// --- Public API ---

void debug_cli_init() {
    SqLogClass::init();
    xTaskCreate(debugCliTask, "cli", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
}
