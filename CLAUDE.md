## Build Commands

```bash
C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run
```

Read the full "./FSD.md" file included in this project's root dir.

## Current Status

**Phase 1 — Mesh & Blink: COMPLETE** (committed and pushed)

Phase 1 delivers: WiFi mesh formation, weighted gateway election (battery + adjacency - tenure + MAC tiebreak), routerless self-promotion with MAC-jittered timers, NVS-backed config with PropertyValue<> auto-persistence, LED driver with save/restore, battery ADC, RTC slow-memory mesh map.

**Phase 2 — FTM Localization: IMPLEMENTED** (compiles clean, needs hardware verification)

Phase 2 delivers: PeerTable (IRAM working map with 16-node capacity), heartbeat protocol (MSG_TYPE_HEARTBEAT, 30s default, NVS-tunable), battery-aware re-election on stable mesh, FtmManager (FTM initiator with 2σ outlier rejection), FtmScheduler (priority queue, wake/ready/go state machine, anchor+incremental scheduling), PositionSolver (classical MDS via power iteration + per-node diagonal Kalman filter, 1D/2D/3D adaptive), 10 new NVS parameters, 7 new mesh message types.

**Phase 3 — Audio Engine: VERIFIED** (hardware-tested on real boards)

Phase 3 delivers: LEDC PWM + GPTimer tone engine (Mozzi removed — incompatible with ESP32-C6 single-core RISC-V). PiezoDriver (push-pull complementary LEDC on GPIO22/GPIO23), AudioEngine (GPTimer ISR at 200 Hz, fixed-point envelope interpolation), ToneLibrary (6 built-in tones: chirp, chirp_down, squeak, warble, alert, fade_chirp), IAudioOutput interface for future I2S DAC.

**Debug CLI: COMPLETE** — Always-on serial CLI (FreeRTOS task, non-blocking). 18 text commands: help, led, battery, wifi, mesh, elect, rtc, sleep, peers, tone, config, mode, ftm, sweep, solve, broadcast, quiet, status, reboot. Tab-cycles last 3 successful commands as history. Interactive `tone` command with ASCII numpad (keys 1-6 play tones, 0 stops, `.` quits). SqLog wrapper gates background output through quiet-mode flag (also suppresses ESP_LOG via esp_log_set_vprintf).

**Next: Phase 4 — Orchestrator** — Coordinated sound across the flotilla (traveling sound, random pop-up, triggered sequences).

## Tools

- **Multi-port serial monitor**: `python tools/monitor/multi_monitor.py COM8 COM9`
  - Built-in ESP32 exception decoder (auto-detects firmware.elf + addr2line)
  - Session logging with timestamps to `tools/monitor/logs/` (on by default, `--no-log` to disable)
  - `--elf <path>` to override ELF, `--log-dir <path>` to override log directory
