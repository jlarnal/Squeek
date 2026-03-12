## FSD is Canon

The file `./FSD.md` is the authoritative specification for this project. **The FSD must be updated BEFORE implementing any behavioral, architectural, or protocol change.** The FSD leads — code follows. Bug fixes and internal refactors that don't change externally visible behavior are exempt, but anything that alters what the system does (new features, changed flows, new message types, timing changes, API changes) must be reflected in the FSD first. Do not:
- Remove, rename, or change the semantics of features described in the FSD
- Alter message types, protocol flows, or role definitions unless the user explicitly asks
- Simplify away mechanisms the FSD specifies (FTM scheduling, RTC state fields, play modes, etc.)
- Refactor code in a way that drops FSD-specified capabilities, even if they appear unused

Bug fixes and implementation improvements are fine — just don't break what the FSD says the system should do. When in doubt, quote the relevant FSD section and ask.

## Build Commands

```bash
C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run
```

Read the full "./FSD.md" file included in this project's root dir.

## Current Status

**Phase 1 — Mesh & Blink: COMPLETE** (committed and pushed)

Phase 1 delivers: WiFi mesh formation, root-observation role assignment (ESP-MESH root = Gateway, no election overlay, native ESP-MESH election via `fix_root(false)`), NVS-backed config with PropertyValue<> auto-persistence, LED driver with save/restore, battery ADC, RtcState manager (RTC_NOINIT_ATTR + CRC32, fast-path boot on soft reset).

**Phase 2 — FTM Localization: IMPLEMENTED** (compiles clean, needs hardware verification)

Phase 2 delivers: PeerTable (IRAM working map with 16-node capacity), heartbeat protocol (MSG_TYPE_HEARTBEAT, 30s default, NVS-tunable), battery-aware step-down on stable mesh, FtmManager (FTM initiator with 2σ outlier rejection), FtmScheduler (priority queue, wake/ready/go state machine, anchor+incremental scheduling), PositionSolver (classical MDS via power iteration + per-node diagonal Kalman filter, 1D/2D/3D adaptive), 10 new NVS parameters, 7 new mesh message types.

**Phase 3 — Audio Engine: VERIFIED** (hardware-tested on real boards)

Phase 3 delivers: LEDC PWM + GPTimer tone engine (Mozzi removed — incompatible with ESP32-C6 single-core RISC-V). PiezoDriver (push-pull complementary LEDC on GPIO22/GPIO23), AudioEngine (GPTimer ISR at 200 Hz, fixed-point envelope interpolation), ToneLibrary (6 built-in tones: chirp, chirp_down, squeak, warble, alert, fade_chirp), IAudioOutput interface for future I2S DAC.

**Phase 4 — Orchestrator: IMPLEMENTED** (compiles clean, needs hardware verification)

Phase 4 delivers: ClockSync (gateway millis() broadcast, peer offset tracking), Orchestrator FreeRTOS task (4KB stack, event-driven queue), 4 play modes (travel with nearest/axis/random paths, random popup, NVS-persisted sequences up to 32 steps, scheduled triggers), 3 new mesh message types (PLAY_CMD, ORCH_MODE, CLOCK_SYNC), 6 new NVS parameters, gateway role transfer safety, CLI `orch` command with 12 sub-commands.

**Debug CLI: COMPLETE** — Always-on serial CLI (FreeRTOS task, non-blocking). 21 text commands: help, led, battery, wifi, mesh, elect, rtc, sleep, peers, tone, config, mode, ftm, sweep, solve, broadcast, quiet, status, orch, temp, reboot. Tab-cycles last 3 successful commands as history. Interactive `tone` command with ASCII numpad (keys 1-6 play tones, 0 stops, `.` quits). SqLog wrapper gates background output through quiet-mode flag (also suppresses ESP_LOG via esp_log_set_vprintf).

**Phase 5A — Web Infrastructure: COMPLETE** (LittleFS, SqWebServer, StorageManager, DNS captive portal)

**Phase 5B — Setup Delegate & STA Connectivity: VERIFIED** (hardware-tested, committed 1f5120a)

Phase 5B delivers: Delegate role (IMeshRole), WiFi wizard SoftAP (Squeek_Config_XXYY), credential propagation (delegate→gateway→peers), deferred delegation (30s timer, routing table), exhaustion guard (2 attempts), MAC-jittered gateway-loss backoff, fast-path boot, channel=0 cross-channel discovery, Squeek_Config_* scan filter, 4x LED blink on client connect. RSSI-aware tenure score (replaces blind NVS counter), CredentialTable (8-slot multi-cred NVS), credential exchange protocol (CRED_OFFER/CRED_REPLY), periodic gateway re-evaluation via heartbeat tenure scores.

**Next: Phase 5C — Dashboard** — Browser-based control from a phone (REST API, 3D topology map, sequence designer).

## Tools

- **Multi-port serial monitor**: `python tools/monitor/multi_monitor.py COM8 COM9`
  - Built-in ESP32 exception decoder (auto-detects firmware.elf + addr2line)
  - Session logging with timestamps to `tools/monitor/logs/` (on by default, `--no-log` to disable)
  - `--elf <path>` to override ELF, `--log-dir <path>` to override log directory
