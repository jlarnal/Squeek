## Build Commands

```bash
C:/Users/arnal/.platformio/penv/Scripts/platformio.exe run
```

Read the full "./FSD.md" file included in this project's root dir.

## Current Status

**Phase 1 — Mesh & Blink: COMPLETE** (committed and pushed)

Phase 1 delivers: WiFi mesh formation, weighted gateway election (battery + adjacency - tenure + MAC tiebreak), routerless self-promotion with MAC-jittered timers, NVS-backed config with PropertyValue<> auto-persistence, LED driver with save/restore, battery ADC, RTC slow-memory mesh map, debug menu with esp_restart() clean exit.

**Phase 2 — FTM Localization: IMPLEMENTED** (compiles clean, needs hardware verification)

Phase 2 delivers: PeerTable (IRAM working map with 16-node capacity), heartbeat protocol (MSG_TYPE_HEARTBEAT, 30s default, NVS-tunable), battery-aware re-election on stable mesh, FtmManager (FTM initiator with 2σ outlier rejection), FtmScheduler (priority queue, wake/ready/go state machine, anchor+incremental scheduling), PositionSolver (classical MDS via power iteration + per-node diagonal Kalman filter, 1D/2D/3D adaptive), 10 new NVS parameters, 7 new mesh message types, debug menu entries 9-D for hardware verification.

**Next: Hardware verification** — Flash both boards and run through Phase 2a-2d verification steps (see plan). Critical test: debug menu [A] FTM single-shot to verify FTM works while mesh is active.

## Tools

- **Multi-port serial monitor**: `python tools/monitor/multi_monitor.py COM8 COM9`
  - Built-in ESP32 exception decoder (auto-detects firmware.elf + addr2line)
  - Session logging with timestamps to `tools/monitor/logs/` (on by default, `--no-log` to disable)
  - `--elf <path>` to override ELF, `--log-dir <path>` to override log directory
