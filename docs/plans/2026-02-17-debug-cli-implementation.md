# Debug CLI Redesign Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the blocking debug menu with an always-on, non-blocking serial CLI that runs as a FreeRTOS task alongside the mesh.

**Architecture:** A dedicated FreeRTOS task reads serial input line-by-line and dispatches text commands (like `help`, `battery`, `ftm`) via a static command table. A `SqLog` print wrapper gates all background serial output through a quiet-mode flag. ESP_LOG output is also gated via `esp_log_set_vprintf()`.

**Tech Stack:** ESP-IDF (FreeRTOS, esp_log), Arduino Serial, C++

**Build command:** `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

---

### Task 1: Create SqLog print wrapper

**Files:**
- Create: `include/sq_log.h`

**Step 1: Write sq_log.h**

This is a header-only utility. The class wraps `Serial` and checks a static quiet flag before forwarding output. CLI handlers use `Serial` directly to bypass the gate.

```cpp
#ifndef SQ_LOG_H
#define SQ_LOG_H

#include <Arduino.h>
#include <esp_log.h>
#include <cstdarg>

class SqLogClass {
public:
    static void init() {
        s_defaultVprintf = esp_log_set_vprintf(quietVprintf);
    }

    static void setQuiet(bool q) { s_quiet = q; }
    static bool isQuiet() { return s_quiet; }

    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (s_quiet) return;
        va_list args;
        va_start(args, fmt);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.print(buf);
    }

    void print(const char* s) {
        if (s_quiet) return;
        Serial.print(s);
    }

    void print(char c) {
        if (s_quiet) return;
        Serial.print(c);
    }

    void println(const char* s = "") {
        if (s_quiet) return;
        Serial.println(s);
    }

    void println(int v) {
        if (s_quiet) return;
        Serial.println(v);
    }

    void flush() {
        Serial.flush();
    }

private:
    static bool s_quiet;
    static vprintf_like_t s_defaultVprintf;

    static int quietVprintf(const char* fmt, va_list args) {
        if (s_quiet) return 0;
        if (s_defaultVprintf) return s_defaultVprintf(fmt, args);
        return vprintf(fmt, args);
    }
};

inline bool SqLogClass::s_quiet = false;
inline vprintf_like_t SqLogClass::s_defaultVprintf = nullptr;

inline SqLogClass SqLog;

#endif // SQ_LOG_H
```

**Step 2: Commit**

```bash
git add include/sq_log.h
git commit -m "feat: add SqLog print wrapper with quiet-mode gate"
```

---

### Task 2: Remove debugTimeout_ms NVS parameter

**Files:**
- Modify: `include/bsp.hpp:56` — remove `NVS_DEFAULT_DEBUG_TIMEOUT_MS`
- Modify: `include/nvs_config.h:14,42,104,155` — remove NVS key, default, hash entry, property
- Modify: `src/nvs_config.cpp:22,133,168` — remove static def, loadInitial, factory restore

**Step 1: Edit bsp.hpp**

Remove line 56:
```
#define NVS_DEFAULT_DEBUG_TIMEOUT_MS    15000
```

**Step 2: Edit nvs_config.h**

Remove these lines:
- Line 14: `inline constexpr char NVS_KEY_DBGTMO[] = "dbgTmo";`
- Line 42: `inline constexpr uint32_t DEFAULT_DEBUG_TIMEOUT_MS   = NVS_DEFAULT_DEBUG_TIMEOUT_MS;`
- Line 104: `h = fnvU32(h,  DEFAULT_DEBUG_TIMEOUT_MS);`
- Lines 154-155: the `debugTimeout_ms` property declaration and its comment

**Step 3: Edit nvs_config.cpp**

Remove these lines:
- Line 22: `PropertyValue<NVS_KEY_DBGTMO, uint32_t, NvsConfigManager> NvsConfigManager::debugTimeout_ms(DEFAULT_DEBUG_TIMEOUT_MS);`
- Line 133: `debugTimeout_ms.loadInitial(nvsGetU32(NVS_KEY_DBGTMO, DEFAULT_DEBUG_TIMEOUT_MS));`
- Line 168: `debugTimeout_ms      = DEFAULT_DEBUG_TIMEOUT_MS;`

**Step 4: Build to verify no compile errors**

Run: `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

**Step 5: Commit**

```bash
git add include/bsp.hpp include/nvs_config.h src/nvs_config.cpp
git commit -m "chore: remove debugTimeout_ms NVS parameter (marquee removed)"
```

---

### Task 3: Migrate subsystem Serial.printf to SqLog.printf

**Files:**
- Modify: `src/mesh_conductor.cpp` — all `Serial.printf` and `Serial.println` calls
- Modify: `src/mesh_node.cpp` — all `Serial.printf` and `Serial.println` calls
- Modify: `src/mesh_gateway.cpp` — all `Serial.printf` and `Serial.println` calls
- Modify: `src/peer_table.cpp` — all `Serial.printf` and `Serial.println` calls
- Modify: `src/ftm_manager.cpp` — all `Serial.printf` and `Serial.println` calls
- Modify: `src/ftm_scheduler.cpp` — all `Serial.printf` and `Serial.println` calls
- Modify: `src/position_solver.cpp` — all `Serial.printf` and `Serial.println` calls

**Step 1: Add `#include "sq_log.h"` to each file**

Add `#include "sq_log.h"` after the existing includes in each of the 7 files listed above.

**Step 2: Replace Serial calls with SqLog calls**

In each file, perform these replacements:
- `Serial.printf(` → `SqLog.printf(`
- `Serial.println(` → `SqLog.println(`
- `Serial.print(` → `SqLog.print(`
- `Serial.flush()` → `SqLog.flush()`

**Important:** Do NOT touch `Serial.begin(115200)` in `main.cpp` — that stays.

The specific files and approximate number of replacements:
- `src/mesh_conductor.cpp`: ~40 Serial calls (lines 149, 152, 195, 220-227, 231, 241-242, 245, 251, 261, 282, 344, 416, 421, 461-462, 482, 490, 497, 501, 514, 523-525, 531, 567, 572, 593, 597, 617, 705, 741-767, 771)
- `src/mesh_node.cpp`: ~6 Serial calls (lines 30, 50, 57-58, 62-63, 67, 79)
- `src/mesh_gateway.cpp`: ~5 Serial calls (lines 21, 41, 53-54, 62-63, 73-75)
- `src/peer_table.cpp`: ~10 Serial calls (lines 72, 80, 89, 95-96, 126-127, 145-146, 207-219)
- `src/ftm_manager.cpp`: ~10 Serial calls (lines 85-86, 98, 104, 129, 134, 148-151, 155, 163, 185, 200-202)
- `src/ftm_scheduler.cpp`: ~15 Serial calls (lines 158-159, 165-166, 193-194, 202, 260, 269, 289-290, 296, 312, 324, 327, 345, 355-356, 358-359, 396, 404-414)
- `src/position_solver.cpp`: ~8 Serial calls (lines 99, 105, 110, 117, 121, 145, 337, 344)

**Step 3: Build to verify**

Run: `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

**Step 4: Commit**

```bash
git add src/mesh_conductor.cpp src/mesh_node.cpp src/mesh_gateway.cpp src/peer_table.cpp src/ftm_manager.cpp src/ftm_scheduler.cpp src/position_solver.cpp
git commit -m "refactor: migrate subsystem Serial output to SqLog for quiet-mode support"
```

---

### Task 4: Rewrite debug menu as non-blocking CLI

**Files:**
- Delete: `include/debug_menu.h` (replaced by new file)
- Delete: `src/debug_menu.cpp` (replaced by new file)
- Create: `include/debug_cli.h`
- Create: `src/debug_cli.cpp`
- Modify: `src/CMakeLists.txt:9` — change `debug_menu.cpp` to `debug_cli.cpp`

**Step 1: Create include/debug_cli.h**

```cpp
#ifndef DEBUG_CLI_H
#define DEBUG_CLI_H

/// Spawn the CLI FreeRTOS task. Call once from setup().
void debug_cli_init();

#endif // DEBUG_CLI_H
```

**Step 2: Create src/debug_cli.cpp**

The file contains:
1. A command entry struct: `{ const char* name; void (*handler)(const char* args); const char* description; }`
2. All command handler functions (ported from debug_menu.cpp but non-blocking)
3. The static command table
4. The `debugCliTask` function (line-buffer loop reading from Serial)
5. `debug_cli_init()` which calls `SqLogClass::init()` and spawns the task

Command handlers to port from `src/debug_menu.cpp`:
- `cmd_help` — iterates command table, prints name + description via `Serial.printf`
- `cmd_led` — same as `menu_led_test()` (lines 92-113)
- `cmd_battery` — same as `menu_battery()` (lines 115-123)
- `cmd_wifi` — same as `menu_wifi_scan()` (lines 125-143)
- `cmd_mesh` — same as `menu_mesh_join()` (lines 145-170)
- `cmd_elect` — same as `menu_gateway_elect()` (lines 172-179)
- `cmd_rtc` — same as `menu_rtc_test()` (lines 181-218)
- `cmd_sleep` — parse args for seconds (default 5), no sub-prompt: `sleep 10`
- `cmd_peers` — same as `menu_peer_table()` (lines 283-294)
- `cmd_ftm` — same as `menu_ftm_single()` (lines 296-329)
- `cmd_sweep` — same as `menu_ftm_sweep()` (lines 331-375)
- `cmd_solve` — same as `menu_mds_solve()` (lines 377-399)
- `cmd_broadcast` — same as `menu_broadcast_pos()` (lines 401-409)
- `cmd_quiet` — toggle `SqLogClass::setQuiet(!SqLogClass::isQuiet())`
- `cmd_status` — print mesh state, role, battery, peer count (composite of MeshConductor::printStatus + battery)
- `cmd_reboot` — `Serial.println("Rebooting..."); Serial.flush(); esp_restart();`

All command handlers use `Serial.printf` directly (not SqLog) so their output is always visible.

The CLI task:
```cpp
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
            // Backspace
            if (linePos > 0) {
                linePos--;
                Serial.print("\b \b");
            }
        } else if (linePos < sizeof(lineBuf) - 1) {
            lineBuf[linePos++] = c;
            Serial.print(c); // echo
        }
    }
}
```

**Step 3: Update CMakeLists.txt**

Change line 9 from `"debug_menu.cpp"` to `"debug_cli.cpp"`.

**Step 4: Delete old files**

Delete `include/debug_menu.h` and `src/debug_menu.cpp`.

**Step 5: Build**

Run: `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

**Step 6: Commit**

```bash
git rm include/debug_menu.h src/debug_menu.cpp
git add include/debug_cli.h src/debug_cli.cpp src/CMakeLists.txt
git commit -m "feat: replace blocking debug menu with always-on serial CLI"
```

---

### Task 5: Update main.cpp

**Files:**
- Modify: `src/main.cpp`

**Step 1: Edit main.cpp**

Changes:
1. Replace `#include "debug_menu.h"` with `#include "debug_cli.h"` (line 12)
2. Replace `debug_menu();` with `debug_cli_init();` (line 27)
3. Remove `Serial.printf("Battery: %lu mV\n", PowerManager::batteryMv());` (line 48)
4. Add `#include "sq_log.h"` to includes

The `loop()` battery print is removed because it's now available via the `battery` CLI command, and it would interleave with CLI output.

Keep the LED blink logic and `RtcMap::save()` and `SQ_POWER_DELAY(5000)` as-is.

**Step 2: Build**

Run: `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "chore: wire up debug_cli_init in main.cpp, remove battery print"
```

---

### Task 6: Final build verification and cleanup

**Step 1: Full clean build**

Run: `C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run -t clean && C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run`

Verify:
- Compiles with 0 errors
- No warnings about unused variables from removed debug timeout code
- RAM and flash usage reported

**Step 2: Verify file list is clean**

Run: `git status`

Ensure no stray files. All changes committed.

**Step 3: Update CLAUDE.md**

Update the "Current Status" section to note the CLI redesign is complete. Remove references to the marquee/debug timeout.
