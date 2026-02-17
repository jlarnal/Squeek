#include "debug_menu.h"
#include "bsp.hpp"
#include "nvs_config.h"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_conductor.h"
#include "rtc_mesh_map.h"
#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>

static const char* BANNER = "  Squeek v" SQUEEK_VERSION "  Press ENTER for debug menu, any other key to skip...  ";

inline static void flushSerialInput()
{
    while (Serial.available()) {
        Serial.read();
        vTaskDelay(1);
    }
}

// Returns true if user pressed ENTER (enter menu), false to skip
static bool marquee_animation()
{
    const int width      = 30;
    const int banner_len = strlen(BANNER);

    int dot_pos = 0;
    int dot_dir = 1;
    int frame   = 0;

    flushSerialInput();
    Serial.println();
    uint32_t startTime = millis();
    uint32_t timeout   = NvsConfigManager::debugTimeout_ms;
    while (timeout == 0 || (millis() - startTime) < timeout) {
        // Scrolling banner
        int offset = frame % (banner_len + width);
        Serial.print("\r  ");
        for (int i = 0; i < width; i++) {
            int idx = i - (width - offset);
            if (idx >= 0 && idx < banner_len) {
                Serial.print(BANNER[idx]);
            } else {
                Serial.print(' ');
            }
        }

        // Kitt scanner
        Serial.print("  [");
        for (int i = 0; i < 20; i++) {
            if (i == dot_pos)
                Serial.print('o');
            else if (i == dot_pos - 1 || i == dot_pos + 1)
                Serial.print('.');
            else
                Serial.print(' ');
        }
        Serial.print(']');
        Serial.flush();

        dot_pos += dot_dir;
        if (dot_pos >= 19)
            dot_dir = -1;
        if (dot_pos <= 0)
            dot_dir = 1;
        frame++;

        if (Serial.available()) {
            char c = Serial.read();
            Serial.println();
            if (c == '\n' || c == '\r') {
                flushSerialInput();
                return true;
            }
            Serial.println("Skipping to normal boot...");
            flushSerialInput();
            return false;
        }

        delay(100);
    }

    Serial.println("\nTimeout - normal boot.");
    return false;
}

static void menu_led_test()
{
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

static void menu_battery()
{
    PowerManager::init();
    uint32_t raw = PowerManager::batteryRaw();
    uint32_t mv  = PowerManager::batteryMv();
    Serial.printf("Battery RAW: %lu\n", raw);
    Serial.printf("Battery mV:  %lu\n", mv);
    Serial.printf("Low: %s  Critical: %s\n", PowerManager::isLowBattery() ? "YES" : "no", PowerManager::isCriticalBattery() ? "YES" : "no");
}

static void menu_wifi_scan()
{
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
            Serial.printf("  [%d] %-32s  RSSI:%d  CH:%d\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
        }
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
}

static void menu_mesh_join()
{
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
        Serial.println("Mesh timeout — may still be forming.");
    }
    MeshConductor::printStatus();
    RtcMap::print();

    // Stop mesh so background scanning doesn't spam the debug menu
    MeshConductor::stop();
}

static void menu_gateway_elect()
{
    if (!MeshConductor::isConnected()) {
        Serial.println("Mesh not connected. Run [4] first.");
        return;
    }
    MeshConductor::forceReelection();
}

static void menu_rtc_test()
{
    Serial.println("RTC memory test...");
    RtcMap::init();

    // Write test pattern
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

    // Validate
    if (RtcMap::isValid()) {
        Serial.println("PASS: checksum valid after save.");
    } else {
        Serial.println("FAIL: checksum invalid!");
    }

    // Clear and verify
    RtcMap::clear();
    if (!RtcMap::isValid()) {
        Serial.println("PASS: map invalid after clear.");
    } else {
        Serial.println("FAIL: map still valid after clear!");
    }

    // Re-init
    RtcMap::init();
    Serial.println("Re-initialized:");
    RtcMap::print();
}

static void menu_light_sleep()
{
    Serial.println("Enter sleep seconds (default 5): ");
    unsigned long start = millis();
    String input        = "";

    while (millis() - start < 5000) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r')
                break;
            input += c;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t secs = 5;
    if (input.length() > 0) {
        secs = input.toInt();
        if (secs == 0)
            secs = 5;
    }

    Serial.printf("Sleeping for %lu seconds...\n", secs);
    Serial.flush();
    PowerManager::init();
    PowerManager::lightSleep(secs);
    Serial.println("Woke up from light sleep!");
}

static void menu_debug_timeout()
{
    Serial.printf("Current debug timeout: %lu ms (0 = infinite)\n",
                  (unsigned long)(uint32_t)NvsConfigManager::debugTimeout_ms);
    Serial.print("Enter new value in ms (750-60000): ");

    unsigned long start = millis();
    String input        = "";
    while (millis() - start < 10000) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r')
                break;
            input += c;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (input.length() == 0) {
        Serial.println("\nNo input, cancelled.");
        return;
    }

    long val = input.toInt();
    if (val < 750 || val > 60000) {
        Serial.printf("\nOut of range: %ld (must be 750-60000)\n", val);
        return;
    }

    NvsConfigManager::debugTimeout_ms = (uint32_t)val;
    Serial.printf("\nDebug timeout set to %lu ms\n",val);
}

void debug_menu()
{

    if (!marquee_animation())
        return;


    bool running = true;
    while (running) {
        Serial.println();
        Serial.println("=== Squeek Debug Menu ===");
        Serial.println("[1] LED test        - Blink status LED + RGB R/G/B");
        Serial.println("[2] Battery ADC     - Read voltage");
        Serial.println("[3] WiFi scan       - List nearby APs");
        Serial.println("[4] Mesh join       - Form mesh, show peers");
        Serial.println("[5] Gateway elect   - Force re-election");
        Serial.println("[6] RTC memory      - Write/readback test");
        Serial.println("[7] Light sleep     - Sleep N seconds");
        Serial.println("[8] Debug timeout   - Set marquee timeout");
        Serial.println("[0] Exit            - Normal boot");
        Serial.print("> ");

        // Wait for input
        while (!Serial.available()) {
            delay(250);
        }

        char choice = Serial.read();
        // Consume trailing newline
        flushSerialInput();
        Serial.println(choice);

        switch (choice) {
            case '1':
                menu_led_test();
                break;
            case '2':
                menu_battery();
                break;
            case '3':
                menu_wifi_scan();
                break;
            case '4':
                menu_mesh_join();
                break;
            case '5':
                menu_gateway_elect();
                break;
            case '6':
                menu_rtc_test();
                break;
            case '7':
                menu_light_sleep();
                break;
            case '8':
                menu_debug_timeout();
                break;
            case '0':
                running = false;
                break;
            default:
                Serial.println("Invalid choice.");
                break;
        }
    }

    Serial.println("Rebooting for clean start...");
    Serial.flush();
    esp_restart();
}
