#include "debug_menu.h"
#include "bsp.hpp"
#include "led_driver.h"
#include "power_manager.h"
#include "mesh_manager.h"
#include "rtc_mesh_map.h"
#include <Arduino.h>
#include <WiFi.h>

static const char* BANNER = "  Squeek v" SQUEEK_VERSION "  ";

// Returns true if user pressed ENTER (enter menu), false to skip
static bool marquee_animation() {
    const int width = 30;
    const int banner_len = strlen(BANNER);
    unsigned long deadline = millis() + (DEBUG_MENU_TIMEOUT_S * 1000UL);

    int dot_pos = 0;
    int dot_dir = 1;
    int frame = 0;

    Serial.println();
    Serial.println("Press ENTER for debug menu...");
    Serial.println();

    while (millis() < deadline) {
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
            if (i == dot_pos) Serial.print('o');
            else if (i == dot_pos - 1 || i == dot_pos + 1) Serial.print('.');
            else Serial.print(' ');
        }
        Serial.print(']');

        dot_pos += dot_dir;
        if (dot_pos >= 19) dot_dir = -1;
        if (dot_pos <= 0) dot_dir = 1;
        frame++;

        if (Serial.available()) {
            char c = Serial.read();
            Serial.println();
            if (c == '\n' || c == '\r') return true;
            Serial.println("Skipping to normal boot...");
            return false;
        }

        delay(100);
    }

    Serial.println("\nTimeout - normal boot.");
    return false;
}

static void menu_led_test() {
    Serial.println("LED test: status LED blink...");
    LedDriver::init();
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

    LedDriver::rgbOff();
    Serial.println("LED test done.");
}

static void menu_battery() {
    power_init();
    uint32_t raw = power_battery_raw();
    uint32_t mv = power_battery_mv();
    Serial.printf("Battery RAW: %lu\n", raw);
    Serial.printf("Battery mV:  %lu\n", mv);
    Serial.printf("Low: %s  Critical: %s\n",
        power_is_low_battery() ? "YES" : "no",
        power_is_critical_battery() ? "YES" : "no");
}

static void menu_wifi_scan() {
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

static void menu_mesh_join() {
    Serial.println("Initializing mesh...");
    rtc_map_init();
    mesh_init();
    mesh_start();

    Serial.println("Waiting for mesh (30s timeout)...");
    unsigned long deadline = millis() + 30000;
    while (!mesh_is_connected() && millis() < deadline) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (mesh_is_connected()) {
        Serial.println("Mesh connected!");
    } else {
        Serial.println("Mesh timeout — may still be forming.");
    }
    mesh_print_status();
    rtc_map_print();
}

static void menu_gateway_elect() {
    if (!mesh_is_connected()) {
        Serial.println("Mesh not connected. Run [4] first.");
        return;
    }
    mesh_force_reelection();
    Serial.println("Waiting for re-election (15s)...");
    delay(15000);
    mesh_print_status();
}

static void menu_rtc_test() {
    Serial.println("RTC memory test...");
    rtc_map_init();

    // Write test pattern
    rtc_mesh_map_t* map = rtc_map_get();
    map->own_short_id = 42;
    map->mesh_generation = 12345;
    map->peer_count = 1;
    memset(map->peers[0].mac, 0xAA, 6);
    map->peers[0].short_id = 1;
    map->peers[0].flags = PEER_FLAG_ALIVE;
    rtc_map_save();

    Serial.println("Written test data:");
    rtc_map_print();

    // Validate
    if (rtc_map_is_valid()) {
        Serial.println("PASS: checksum valid after save.");
    } else {
        Serial.println("FAIL: checksum invalid!");
    }

    // Clear and verify
    rtc_map_clear();
    if (!rtc_map_is_valid()) {
        Serial.println("PASS: map invalid after clear.");
    } else {
        Serial.println("FAIL: map still valid after clear!");
    }

    // Re-init
    rtc_map_init();
    Serial.println("Re-initialized:");
    rtc_map_print();
}

static void menu_light_sleep() {
    Serial.println("Enter sleep seconds (default 5): ");
    unsigned long start = millis();
    String input = "";

    while (millis() - start < 5000) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') break;
            input += c;
        }
    }

    uint32_t secs = 5;
    if (input.length() > 0) {
        secs = input.toInt();
        if (secs == 0) secs = 5;
    }

    Serial.printf("Sleeping for %lu seconds...\n", secs);
    Serial.flush();
    power_init();
    power_enter_light_sleep(secs);
    Serial.println("Woke up from light sleep!");
}

void debug_menu() {
    delay(500);

    if (!marquee_animation()) return;

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
        Serial.println("[0] Exit            - Normal boot");
        Serial.print("> ");

        // Wait for input
        while (!Serial.available()) {
            delay(10);
        }

        char choice = Serial.read();
        // Consume trailing newline
        while (Serial.available()) Serial.read();
        Serial.println(choice);

        switch (choice) {
        case '1': menu_led_test(); break;
        case '2': menu_battery(); break;
        case '3': menu_wifi_scan(); break;
        case '4': menu_mesh_join(); break;
        case '5': menu_gateway_elect(); break;
        case '6': menu_rtc_test(); break;
        case '7': menu_light_sleep(); break;
        case '0': running = false; break;
        default:
            Serial.println("Invalid choice.");
            break;
        }
    }

    Serial.println("Exiting debug menu, normal boot...");
}
