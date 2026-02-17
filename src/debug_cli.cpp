#include "debug_cli.h"
#include "bsp.hpp"
#include "nvs_config.h"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_conductor.h"
#include "rtc_mesh_map.h"
#include "peer_table.h"
#include "ftm_manager.h"
#include "ftm_scheduler.h"
#include "position_solver.h"
#include "sq_log.h"
#include <Arduino.h>
#include <esp_system.h>
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
    { "peers",     cmd_peers,     "Show PeerTable (gateway only)" },
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
    if (!MeshConductor::isGateway()) {
        Serial.println("Not gateway -- PeerTable only runs on gateway.");
        return;
    }
    PeerTable::print();
}

static void cmd_ftm(const char* args) {
    (void)args;
    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected. Run 'mesh' first.");
        return;
    }

    Serial.println("FTM single-shot test");
    Serial.println("Initializing FTM manager...");
    FtmManager::init();

    if (MeshConductor::isGateway() && PeerTable::peerCount() >= 2) {
        PeerEntry* peer = PeerTable::getEntryByIndex(1);
        if (peer && !(peer->flags & PEER_STATUS_DEAD)) {
            Serial.printf("Ranging to peer slot 1: %02X:%02X:%02X:%02X:%02X:%02X (SoftAP: %02X:%02X:%02X:%02X:%02X:%02X)\n",
                peer->mac[0], peer->mac[1], peer->mac[2],
                peer->mac[3], peer->mac[4], peer->mac[5],
                peer->softap_mac[0], peer->softap_mac[1], peer->softap_mac[2],
                peer->softap_mac[3], peer->softap_mac[4], peer->softap_mac[5]);

            float dist = FtmManager::initiateSession(peer->softap_mac, MESH_CHANNEL, 8);
            if (dist >= 0) {
                Serial.printf("SUCCESS: distance = %.1f cm (%.2f m)\n", dist, dist / 100.0f);
            } else {
                Serial.println("FAILED: FTM session did not succeed");
            }
            return;
        }
    }

    Serial.println("No peer available for FTM. Need at least 1 peer with known SoftAP MAC.");
    Serial.println("(Peer must have sent a heartbeat so its SoftAP MAC is in PeerTable)");
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

// --- CLI Task ---

static void debugCliTask(void* pvParameters) {
    (void)pvParameters;
    char lineBuf[128];
    uint8_t linePos = 0;

    Serial.println("Squeek CLI ready. Type 'help' for commands.");
    Serial.print("> ");

    for (;;) {
        if (!Serial.available()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char c = Serial.read();

        if (c == '\n' || c == '\r') {
            if (linePos == 0) {
                Serial.print("\n> ");
                continue;
            }
            Serial.println();
            lineBuf[linePos] = '\0';

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
            }

            linePos = 0;
            Serial.print("> ");
        } else if (c == '\b' || c == 127) {
            if (linePos > 0) {
                linePos--;
                Serial.print("\b \b");
            }
        } else if (linePos < sizeof(lineBuf) - 1) {
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
