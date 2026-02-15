# Squeek — Functional Specification Document

## Context

Squeek is a pet toy and prank device built from a flotilla of identical ESP32-C6 SuperMini boards. The nodes form a self-healing WiFi mesh that uses FTM (Fine Timing Measurement) to determine their relative 3D positions without manual configuration. A smartphone or laptop controls the flotilla through a web UI served by an automatically elected gateway node. The spatial awareness enables sounds to "travel" across the physical space — a cat chases a squeak that runs from node to node following the real room layout.

---

## 1. Overview

**Product:** Squeek — a distributed, self-locating sound mesh
**Target hardware:** ESP32-C6 SuperMini (ESP32-C6FH4, 4MB flash, WiFi 6, FTM, RISC-V)
**Framework:** PlatformIO, dual Arduino + ESP-IDF
**Nodes:** Identical firmware, scalable from 2 to N nodes

### Board Hardware (per SuperMini)
- ESP32-C6 SoC with WiFi 6 + FTM
- 4MB flash
- USB-C connector with built-in LiPo charger IC
- Charge state LED (managed by charger IC)
- WS2812 RGB LED on GPIO8
- Simple LED on GPIO15 via 1K resistor. Defaults to blink in debug, defaults to off in release.
- LiPo battery (user-supplied)
- Piezo buzzer driven push-pull via two GPIOs (user-soldered)
- Optional: voltage divider on GPIO2 or GPIO3 for battery ADC monitoring (user-soldered)

### Imported Arduino libraries
 -  `adafruit/Adafruit NeoPixel@^1.12` for obvious purposes
 -  `https://github.com/me-no-dev/AsyncTCP.git#master` for inteactive React/Preact web pages.
 -	`https://github.com/me-no-dev/ESPAsyncWebServer.git#master` for webservices. 
 -	`paulstoffregen/Time @ ^1.6.1  ` to allow for NTP. 
 -	`sensorium/Mozzi@^2.0.2	` for sound synthesis. 

### Pin Mapping (defined in `include/bsp.hpp`)
| Symbol | GPIO | Purpose |
|--------|------|---------|
| `LED_BUILTIN` | GPIO15 | Status LED (via 1K resistor) |
| `RBG_BUILTIN` | GPIO8 | WS2812 RGB LED |
| TBD | GPIO2/3 | Battery voltage ADC (via voltage divider) |
| TBD | 2x GPIO | Piezo buzzer (push-pull, opposed phases) |


**All app-centric preprocessor defines are made in `include/bsp.hpp`.**



### Primary Use Cases
- **Pet toy** for cats, ferrets, and small predators — sounds that travel, pop up randomly, or run in triggered sequences to stimulate hunting instincts
- **Prank device** — scheduled sound triggers with stealth mode that hides the mesh from discovery

### Key Differentiator
Nodes use WiFi FTM to build a 3D spatial map of the flotilla without any manual configuration. This drives intelligent sound routing — a "chase" sequence follows the actual physical layout, not an arbitrary order.

---

## 2. Functional Requirements

### FR1 — Self-Healing WiFi Mesh
- Nodes discover each other automatically on power-up
- Mesh reforms when nodes join, leave, or lose connectivity
- No manual pairing or configuration required
- All nodes run identical firmware

### FR2 — Gateway Election
- One node is elected as gateway (serves web UI, coordinates playback, schedules FTM)
- If the gateway goes offline, another node is elected automatically
- Gateway provides a WiFi SoftAP for the controller (smartphone/laptop) to connect
- Election strategy: simplest viable (e.g. highest MAC wins), re-election on gateway timeout

### FR3 — FTM Self-Localization
- Nodes perform pairwise FTM ranging to estimate inter-node distances
- Gateway coordinates FTM round scheduling (non-overlapping pairs per round)
- Multiple samples per pair, averaged for precision
- Relative 3D Cartesian positions computed from distance matrix (MDS or iterative trilateration)
- Position map updates periodically; frequency configurable (battery vs. accuracy trade-off)
- Incremental updates: if a node moves, only its edges re-measured

### FR4 — Sound Playback
- **Tone synthesis** via Mozzi — procedural chirps, squeaks, warbles, melodies
- **Sample playback** — compressed audio clips (MP3) decoded via libhelix-mp3, stored in LittleFS
- **Audio output layer is modular:**
  - Phase 1: piezo buzzer, push-pull via two GPIOs (doubled voltage swing)
  - Future: I2S DAC companion board

### FR5 — Play Modes
- **Traveling sound** — sound hops across nodes following the physical 3D layout (spatial path computed by gateway)
- **Random pop-up** — random nodes emit sounds at random intervals
- **Triggered sequences** — user-defined patterns of (node, sound, delay) tuples launched from web UI
- **Scheduled triggers** — time-delayed or clock-based activation (for pranks)

### FR6 — Web UI (served by gateway)
- Upload and manage sound samples
- Visualize node topology map in 3D (from FTM data)
- Design and trigger play sequences visually
- Configure play modes, scheduling, and FTM frequency
- Battery levels per node
- Stealth mode toggle

### FR7 — Stealth Mode
- Nodes stop advertising the WiFi SoftAP
- Web UI becomes inaccessible
- Only pre-scheduled or pre-configured sequences remain active
- Exit stealth via physical reset or a pre-set timeout

### FR8 — Visual Feedback
- WS2812 RGB LED for node status (mesh state, playback activity, low battery)
- GPIO15 LED as simple heartbeat indicator
- **LEDs must be kept brief** to conserve battery — flash and off, no sustained illumination

### FR9 — Battery Monitoring
- ADC reads battery voltage via high-impedance voltage divider on GPIO2 or GPIO3
- Low-battery threshold triggers brief WS2812 warning color
- Critical-battery triggers graceful mesh departure and deep sleep
- Battery levels reported to gateway and visible in web UI

### FR10 — Node-Local Mesh Map
Each node maintains a local map of the mesh it belongs to, stored in two tiers:

**RTC Slow Memory (~8KB, survives light + deep sleep):**
- Own node ID, MAC, current role (gateway/peer)
- Gateway MAC, mesh channel, credentials
- Peer table: MAC + short ID for each known node (compact)
- Own 3D position (3 floats = 12 bytes)
- Last FTM epoch timestamp
- Mesh generation counter (detect stale data on wake)

**IRAM (full working map, when awake):**
- All peer 3D positions
- Full or partial distance matrix
- Peer status (awake/sleeping/dead/battery level)
- Current play sequence state + own role in it
- Routing hints (which peers to relay through)

---

## 3. Architecture

### System Topology

```
┌──────────────────────────────────────────────────────┐
│                   Controller                         │
│           (smartphone/laptop browser)                │
│                       │                              │
│                 WiFi SoftAP                          │
│                       ▼                              │
│   ┌──────────── Gateway Node ────────────────┐       │
│   │ Web Server │ Coordinator │ FTM Scheduler │       │
│   └──────────────────┬───────────────────────┘       │
│            WiFi Mesh │                               │
│         ┌────────────┼────────────┐                  │
│         ▼            ▼            ▼                  │
│    ┌─────────┐  ┌─────────┐     ┌─────────┐          │
│    │ Node A  │  │ Node B  │ ... │ Node X  │          │
│    │     ◄───────── FTM ─────────────►    │          │
│    └─────────┘  └─────────┘     └─────────┘          │
└──────────────────────────────────────────────────────┘
```

### Node Operating Modes

| Mode | WiFi | Power | Wake Trigger | Specific behavior
|------|------|-------|-------------|-----|
| **Deep Sleep** | Off | Minimal (~10uA) | Timer or GPIO only | Broadcasts its low battery states to MESH and goes back to sleep.
| **Light Sleep** | Maintained (DTIM beacon) | Low (~1-2mA) | WiFi packet from gateway, timer | Awaits for events to become idle/active again.
| **Idle** | Active, mesh participant | Medium | Immediate — already awake |  -na-
| **Active Play** | Active, low latency | Full | N/A — already in play mode | -na-

**Default state: Light Sleep.** Nodes wake briefly for mesh maintenance beacons, then return to sleep. Gateway wakes them via WiFi when needed for FTM rounds or play sequences.

### Firmware Layers (all nodes, identical firmware)

| Layer | Responsibility |
|-------|---------------|
| **BSP** | `include/bsp.hpp` — pin definitions, peripherals, board constants |
| **Power Manager** | Sleep mode control, battery ADC, wake scheduling, LED brightness/duration limits |
| **Mesh Manager** | WiFi mesh join/heal, gateway election, message routing, node wake via DTIM |
| **Localization Engine** | FTM round scheduling, distance matrix, 3D position solver (trilateration/MDS) |
| **Audio Engine** | Modular: Mozzi synthesis + MP3 decode → abstract output (piezo driver / I2S driver) |
| **Storage** | LittleFS: samples, node config, sequences, position cache |
| **Orchestrator** | Play modes, sequence execution, scheduling, stealth mode |
| **Gateway Services** | Web server, SoftAP, REST API, UI assets (active on elected gateway only) |

### Node Lifecycle State Machine

```
BOOT → MESH_JOINING → LIGHT_SLEEP ←──────────────────┐
                           │                           │
                     [wake: beacon/timer]               │
                           ▼                           │
                      MESH_ACTIVE                      │
                       │       │                       │
              [FTM scheduled]  [play cmd]              │
                       ▼       ▼                       │
                 FTM_RANGING  ACTIVE_PLAY              │
                       │       │                       │
                  [done]   [sequence done]              │
                       └───────┴───────────────────────┘

GATEWAY_ELECTED (parallel role on one node):
  MESH_ACTIVE + SERVING_UI + FTM_COORDINATOR

STEALTH: like LIGHT_SLEEP but AP hidden,
         only pre-loaded schedules run

LOW_BATTERY → DEEP_SLEEP (timer-only wake for periodic check)
```

---

## 4. Software Stack

| Component | Library / API | Purpose |
|-----------|--------------|---------|
| Build system | PlatformIO + pioarduino platform | Dual Arduino + ESP-IDF |
| WiFi Mesh | ESP-IDF WiFi Mesh (`esp_mesh`) | Self-healing mesh network |
| FTM | ESP-IDF FTM API (`esp_wifi_ftm`) | Pairwise ranging for localization |
| Audio synthesis | Mozzi | Procedural tone generation (chirps, squeaks, warbles) |
| MP3 decode | chmorgan/esp-libhelix-mp3 | Compressed sample playback |
| File system | joltwallet/littlefs | Sample storage, config, sequences |
| LED | WS2812 driver (via GPIO8) | Visual status feedback |
| Web server | ESP-IDF HTTP server or ESPAsyncWebServer | REST API + static UI assets |
| DSP | espressif/esp-dsp | Signal processing for FTM/audio if needed |
| JSON | espressif/json_generator + json_parser | API serialization |

---

## 5. Implementation Phases

### Phase 1 — Mesh & Blink
**Goal:** Two or more nodes form a self-healing mesh and prove it works.

- WiFi mesh formation with auto-join
- Gateway election (highest MAC wins, re-election on timeout)
- Brief WS2812 heartbeat flash (then off — save battery)
- Battery voltage ADC reading + serial logging
- Light sleep between heartbeats
- RTC mesh map: own ID, gateway MAC, peer table
- **Deliverable:** Scatter nodes, they find each other. Kill the gateway, another takes over.

### Phase 2 — FTM Localization
**Goal:** Nodes know where they are in 3D space.

- FTM initiator/responder implementation
- Gateway-coordinated round-robin pair scheduling
- Distance matrix construction from averaged RTT samples
- 3D position solver (MDS or iterative trilateration)
- Position data broadcast to all nodes + stored in IRAM mesh map
- Serial/log output of 3D coordinate map
- **Deliverable:** Nodes report their 3D positions. Move one, positions update.

### Phase 3 — Audio Engine
**Goal:** Every node can make sound.

- Mozzi integration with push-pull piezo output (two GPIOs from `bsp.hpp`)
- Procedural tone library: chirps, squeaks, warbles
- MP3 sample decode via libhelix → piezo output
- LittleFS sample storage (upload via serial for now)
- Modular audio output interface (piezo driver now, I2S driver later)
- **Deliverable:** Node plays a chirp on command via serial.

### Phase 4 — Orchestrator & Play Modes
**Goal:** Coordinated sound across the flotilla.

- Traveling sound: gateway computes spatial path from 3D positions, sequences nodes
- Random pop-up: gateway assigns random triggers to random nodes
- Triggered sequences: user defines (node, sound, delay) tuples
- Time-synchronized playback using mesh clock sync
- **Deliverable:** Trigger "chase mode" — sound runs across nodes following physical layout.

### Phase 5 — Web UI
**Goal:** Browser-based control from a phone.

- Gateway serves SoftAP + captive portal style web UI
- Embedded web assets (HTML/JS/CSS in LittleFS or PROGMEM)
- REST API: node list, position map, sound library, trigger play, upload samples
- Visual 3D topology map showing node positions (from FTM data)
- Sequence designer: build play patterns visually
- Schedule configuration
- Battery levels per node
- **Deliverable:** Connect phone to Squeek AP, open browser, see the map, trigger a chase.

### Phase 6 — Stealth & Polish
**Goal:** Prank-ready, power-optimized, robust.

- Stealth mode: hide AP, disable web UI, run only pre-loaded schedules
- Stealth exit: physical reset or pre-set timeout
- Power tuning: optimize sleep intervals, LED durations, FTM frequency
- Low-battery graceful shutdown with mesh notification
- OTA firmware update via web UI
- **Deliverable:** Set a schedule, enable stealth, hide the nodes, wait for chaos.

---

## 6. Key Files

| File | Purpose |
|------|---------|
| `include/bsp.hpp` | Board support — pin definitions, hardware constants |
| `src/main.cpp` | Application entry point (currently skeleton) |
| `platformio.ini` | Build config, dual framework, board definition |
| `sdkconfig.defaults` | ESP-IDF defaults (FreeRTOS tick, flash, Arduino autostart) |
| `sdkconfig.esp32c6-supermini` | Board-specific SDK config |

---

## 7. Open Questions

1. **Piezo GPIO assignment** — Which two GPIOs for push-pull piezo? Needs to be added to `bsp.hpp`.
2. **Battery ADC GPIO** — GPIO2 or GPIO3? Depends on what's free after piezo assignment.
3. **ESP-IDF WiFi Mesh vs ESP-NOW** — WiFi Mesh (`esp_mesh`) provides routing but is heavier. ESP-NOW is lighter but no mesh routing. Need to evaluate which fits better with FTM and light sleep.
4. **Mozzi on ESP32-C6** — Needs a compilation test early in Phase 3. Fallback: direct LEDC PWM synthesis.
5. **FTM accuracy in practice** — Real-world testing needed in Phase 2 to calibrate expectations for 3D positioning.
6. **Web UI framework** — Vanilla JS for minimal size, or a lightweight framework? Storage budget is limited (4MB flash shared with firmware + samples).
7. **Max sample storage** — How much flash to allocate for uploaded MP3 samples after firmware + UI assets?

---

## Appendix A — Debug Menu

A compile-time debug menu for in-situ hardware and firmware testing via the serial monitor. Each implementation phase adds its own test entries so that new subsystems can be verified on real hardware as they land.

### A.1 Activation Mechanism

- `#define DEBUG_MENU_ENABLED` declared in `include/bsp.hpp`
- When defined, `setup()` calls `debug_menu()` before any normal initialization
- When undefined, the debug menu code is exclided from compilation entirely (`#ifdef` guard)

### A.2 Startup Behavior

1. On boot, the debug menu prints an animated scrolling marquee to Serial (project name, version, kitt-scanner dot moving on a row).
2. The marquee loops indefinitely, waiting for **any keypress** or **Line feed** (i.e '\n') on Serial. The latter enters the menu, any other key skips it — this gives the developer time to open the serial monitor after flashing.
3. Once a key is received, the marquee stops and the numbered menu is displayed.
4. A configurable timeout (default 30 seconds) auto-skips to normal boot if no key is pressed — avoids bricking a battery-only node left in debug mode.

### A.3 Menu Interaction

- Simple mnemonic menu printed to Serial, one entry per line
- User types a number + Enter to select.
- After a test completes, the menu re-displays (loop until the user picks "Exit" or the inactivity timeout expires).
- Menu entries grow with each phase — each phase adds its test **items**.
- Entries are organized by phase/category with clear header lines.

### A.4 Menu Entries by Phase

#### Phase 1 — Mesh & Blink

| # | Entry | Description |
|---|-------|-------------|
| 1 | LED test | Blink GPIO15, flash WS2812 in R/G/B sequence |
| 2 | Battery ADC | Read and print raw ADC value + computed voltage |
| 3 | WiFi scan | Scan and list nearby APs (verify radio works) |
| 4 | Mesh join | Attempt mesh formation, print peer count + gateway MAC |
| 5 | Gateway election | Force re-election, print result |
| 6 | RTC memory | Write/read-back test of RTC slow memory mesh map |
| 7 | Light sleep | Enter light sleep for N seconds, wake, print confirmation |

#### Phase 2 — FTM Localization

| # | Entry | Description |
|---|-------|-------------|
| 8 | FTM single-shot | Initiate one FTM exchange with a specific peer, print RTT/distance |
| 9 | FTM full sweep | Run pairwise FTM with all visible peers, print distance matrix |
| 10 | Position solver | Run MDS/trilateration on current distance matrix, print 3D coords |
| 11 | Position broadcast | Send computed positions to mesh, confirm receipt |

#### Phase 3 — Audio Engine

| # | Entry | Description |
|---|-------|-------------|
| 12 | Piezo test tone | Drive push-pull piezo with a fixed-frequency square wave |
| 13 | Mozzi chirp | Play a procedural chirp through Mozzi |
| 14 | Mozzi squeak | Play a procedural squeak |
| 15 | MP3 playback | Decode + play first MP3 sample from LittleFS |
| 16 | LittleFS listing | List all files in LittleFS with sizes |

#### Phase 4 — Orchestrator

| # | Entry | Description |
|---|-------|-------------|
| 17 | Traveling sound test | Trigger a traveling sound across 2–3 nodes (gateway only) |
| 18 | Random pop-up | Trigger one random pop-up event |
| 19 | Clock sync check | Print local clock vs mesh reference clock delta |

#### Phase 5 — Web UI

| # | Entry | Description |
|---|-------|-------------|
| 20 | Start SoftAP | Bring up AP, print SSID + IP |
| 21 | HTTP smoke test | Start web server, print URL, wait for one request |
| 22 | REST API dump | Print JSON of `/nodes` and `/topology` endpoints |

#### Phase 6 — Stealth & Polish

| # | Entry | Description |
|---|-------|-------------|
| 23 | Stealth toggle | Enter/exit stealth mode, confirm AP visibility change |
| 24 | OTA dry-run | Check OTA manifest endpoint without applying |
| 25 | Power profile | Run a timed cycle of sleep/wake/radio and log current estimates |

#### Always Present

| # | Entry | Description |
|---|-------|-------------|
| 0 | Exit | Skip debug menu, proceed to normal `setup()` flow |

### A.5 Implementation Notes

- The debug menu lives in its own source file (`src/debug_menu.cpp` / `include/debug_menu.h`)
- Each phase's menu entries are gated by per-feature `#ifdef`s so the menu compiles even before later phases are implemented
- Serial baud rate: 115200 (matches `platformio.ini` `monitor_speed`)
- The marquee animation uses only basic ASCII (no Unicode) for maximum terminal compatibility
- **Sleep is incompatible with debugging.** When `DEBUG_MENU_ENABLED` is defined, all sleep modes (light sleep, deep sleep) must be disabled. A sleeping node kills Serial output, JTAG, and makes interactive debugging impossible. Sleep integration is only tested and enabled in release builds (i.e. when `DEBUG_MENU_ENABLED` is not defined).
- **Power macros replace raw sleep/delay calls.** All power-saving sleeps and related timeouts must use the following macros (defined in `include/bsp.hpp`), never raw `esp_light_sleep_start()`, `esp_deep_sleep()`, or `delay()` for power-saving purposes:
  - `SQ_LIGHT_SLEEP(duration_ms)` — enters light sleep in release; becomes `delay(duration_ms)` when `DEBUG_MENU_ENABLED` is defined (keeps Serial and JTAG alive).
  - `SQ_DEEP_SLEEP(duration_ms)` — enters deep sleep in release; becomes `delay(duration_ms)` + a Serial warning when `DEBUG_MENU_ENABLED` is defined (prevents bricking the debug session).
  - `SQ_POWER_DELAY(duration_ms)` — a power-budget delay (e.g., idle interval between mesh beacons). Same `delay()` in both modes, but exists as a distinct macro so power-tuning passes can find and adjust these values without touching functional delays.

  This keeps sleep policy in one place and avoids littering the codebase with `#ifdef DEBUG_MENU_ENABLED` guards around every sleep call.
