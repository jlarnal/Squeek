# Debug CLI Redesign: Blocking Menu to Always-On Serial CLI

## Context

The current debug menu blocks in `setup()` with a marquee animation waiting for ENTER. All submenus are blocking. This prevents runtime diagnostics while the mesh is running.

## Design Decisions

- **Always-on CLI** -- serial is always a command prompt, no activation gate
- **Text commands** with inline arguments (e.g., `sleep 10`, not numbered menus)
- **Background stays running** -- mesh, FTM, heartbeats all continue during debug commands
- **No exit/reboot on leaving** -- just stop typing; re-enter anytime
- **Quiet mode** -- toggleable suppression of all background serial output (ESP_LOG + subsystem prints), CLI output always passes through
- **Dedicated FreeRTOS task** -- only spawned when `DEBUG_MENU_ENABLED` is defined

## Architecture

### 1. FreeRTOS Serial Task

`debug_cli_init()` is called from `setup()` (guarded by `DEBUG_MENU_ENABLED`). It spawns `debugCliTask` with 4KB stack at `tskIDLE_PRIORITY + 1`.

The task runs a line-buffer loop:
- Reads characters from Serial into a 128-byte buffer
- On `\n` or `\r`, parses the first token as the command name, remainder as args
- Looks up the command in a static dispatch table
- Calls the handler with the args string
- Prints `> ` prompt after each command completes

### 2. Command Table

Static array of `{ name, handler_fn, description }`:

| Command     | Args         | Description                              |
|-------------|--------------|------------------------------------------|
| `help`      | --           | List all commands with descriptions      |
| `led`       | --           | Blink status LED + RGB R/G/B test        |
| `battery`   | --           | Read battery voltage and status          |
| `wifi`      | --           | Scan nearby APs                          |
| `mesh`      | --           | Join mesh, show peers, then stop         |
| `elect`     | --           | Force gateway re-election                |
| `rtc`       | --           | RTC memory write/readback test           |
| `sleep`     | `[seconds]`  | Light sleep (default 5s)                 |
| `peers`     | --           | Show PeerTable (gateway only)            |
| `ftm`       | --           | FTM single-shot to first peer            |
| `sweep`     | --           | FTM full sweep, print distance matrix    |
| `solve`     | --           | Run MDS position solver                  |
| `broadcast` | --           | Broadcast positions to all nodes         |
| `quiet`     | --           | Toggle background output suppression     |
| `status`    | --           | Print mesh state, role, battery, peers   |
| `reboot`    | --           | `esp_restart()`                          |

The `help` command iterates the table and prints each entry.

### 3. SqLog -- Custom Print Wrapper

A global `SqLog` object wrapping `Serial` with a quiet-mode gate:

- Provides `printf()`, `println()`, `print()` matching the `Serial` interface
- Checks a `static bool s_quiet` flag before forwarding to `Serial`
- When quiet=true: output is silently dropped
- When quiet=false: output forwards to `Serial`

All subsystem code migrates from `Serial.printf` to `SqLog.printf`:
- `mesh_conductor.cpp`, `mesh_node.cpp`, `mesh_gateway.cpp`
- `peer_table.cpp`, `ftm_manager.cpp`, `ftm_scheduler.cpp`, `position_solver.cpp`

CLI command handlers continue using `Serial.printf` directly -- their output always appears regardless of quiet mode.

ESP-IDF logging is also suppressed via `esp_log_set_vprintf()` with a custom handler that checks the same quiet flag.

### 4. Removed: Marquee Animation & Debug Timeout

The marquee animation, Kitt scanner, and `debugTimeout_ms` NVS parameter are all removed. The `NVS_DEFAULT_DEBUG_TIMEOUT_MS` define is dropped from `bsp.hpp`.

### 5. Loop Changes

- Remove `Serial.printf("Battery: %lu mV\n", ...)` from `loop()` (available via `battery` command)
- `SQ_POWER_DELAY(5000)` stays -- the serial task runs independently on its own FreeRTOS task
- No blocking waits in `setup()` or `loop()`

## File Changes

| File | Change |
|------|--------|
| `include/debug_menu.h` | Rename to `include/debug_cli.h`, expose `debug_cli_init()` + `debugQuiet()` |
| `src/debug_menu.cpp` | Rename to `src/debug_cli.cpp` -- rewrite with task, command table, handlers |
| `include/sq_log.h` | **New** -- `SqLog` print wrapper class + quiet flag |
| `src/main.cpp` | Replace `debug_menu()` with `debug_cli_init()`, remove battery print |
| `src/mesh_conductor.cpp` | `Serial.printf` -> `SqLog.printf` |
| `src/mesh_node.cpp` | `Serial.printf` -> `SqLog.printf` |
| `src/mesh_gateway.cpp` | `Serial.printf` -> `SqLog.printf` |
| `src/peer_table.cpp` | `Serial.printf` -> `SqLog.printf` |
| `src/ftm_manager.cpp` | `Serial.printf` -> `SqLog.printf` |
| `src/ftm_scheduler.cpp` | `Serial.printf` -> `SqLog.printf` |
| `src/position_solver.cpp` | `Serial.printf` -> `SqLog.printf` |
| `include/bsp.hpp` | Remove `NVS_DEFAULT_DEBUG_TIMEOUT_MS` |
| `include/nvs_config.h` | Remove `debugTimeout_ms` property |
| `src/nvs_config.cpp` | Remove `debugTimeout_ms` definitions and load/restore calls |
| `src/CMakeLists.txt` | `debug_menu.cpp` -> `debug_cli.cpp` |
