# ESP-NOW Bootstrap Election — Design Spec

## Problem

When multiple nodes boot simultaneously without a mesh root present, the current MAC-based bootstrap timer can produce split-brain: two nodes self-elect within seconds of each other on different channels, forming independent meshes. Additionally, credentialed nodes with bad/unreachable router creds had no bootstrap fallback at all until recently (60-120s timer), and the time spread was too narrow to prevent collisions.

## Solution

Replace the bootstrap timer with an ESP-NOW election phase that runs before `esp_mesh_start()`. Nodes broadcast their tenure score on a shared channel; the highest score wins and all nodes converge on the winner's target channel from the start.

## Trigger

On boot, after WiFi STA init, when no predefined role exists in RTC:
- `delegate_active` set → boot as delegate, skip election
- `next_role` set → boot into that role, skip election
- Valid fast-path reboot (same mesh) → rejoin, skip election

All other cases enter the election.

## Phase 1 — Scan & Score (~2s)

1. Init WiFi in STA mode
2. All-channel AP scan (~2s)
3. Cross-reference results against `CredentialTable::matchScan()` → best RSSI + channel
4. Compute tenure score: RSSI component (0-80) + battery component (0-100, or fixed 100 if `batTen=false`) + uptime component (0 at fresh boot)
5. Determine `target_channel`:
   - If a known router was found in scan: that router's channel
   - Otherwise: channel 1 (routerless fallback)

## Phase 2 — Election (3-10s typical)

1. Lock WiFi to channel 1
2. `esp_now_init()`, register broadcast peer `FF:FF:FF:FF:FF:FF` (no encryption)
3. Arm random broadcast timer: `esp_random() % 3000` ms
4. Arm 3500ms silence timer

### On random broadcast timer fire:
- Broadcast `{MAC[6], tenure_score[2], target_channel[1]}` — send twice for redundancy
- Reset 3500ms silence timer

### On receive:
- Record sender's {MAC, tenure_score, target_channel}
- Reset 3500ms silence timer
- If this node has **NOT** broadcast yet → restart random broadcast timer (`esp_random() % 3000` ms)
- If this node **HAS** already broadcast → do not touch broadcast timer

### On 3500ms silence timer expiry:
- Election is over — proceed to Phase 3

### Lone node case:
- No ESP-NOW responses arrive
- Random broadcast timer fires (0-3s), node broadcasts, arms 3500ms silence
- Silence expires → node is the only candidate, self-promotes

## Phase 3 — Result

1. Every node knows all candidates (including itself)
2. Winner = highest `tenure_score`; tiebreak = lowest MAC address
3. Winner calls `esp_mesh_set_type(MESH_ROOT)` before mesh start
4. **All nodes** (winner and losers) configure `cfg.channel = winner's target_channel`
5. `esp_now_deinit()`
6. `esp_mesh_start()`
7. No bootstrap timer needed — election already decided

## Broadcast Frame

```
| mac[6] | tenure_score[2] | target_channel[1] |  = 9 bytes total
```

- `mac`: sender's STA MAC
- `tenure_score`: little-endian uint16_t
- `target_channel`: uint8_t (1-14, or 1 for routerless)

## `wifi set` CLI Validation

The `wifi set <ssid> [pass]` CLI command now scans before saving:
- Rejects if SSID not found in scan (case-sensitive match)
- Shows channel and RSSI on success
- Already implemented in this branch

## Boot Flow (revised)

1. Load creds from NVS → set `s_hasRouterCreds`
2. Check RTC for predefined role → if set, skip to step 7
3. Init WiFi STA, all-channel AP scan (~2s)
4. Compute tenure score + target_channel from scan results
5. **ESP-NOW election** on ch1 (3-10s)
6. Tear down ESP-NOW
7. Configure ESP-MESH: `cfg.channel = winner's target_channel`
8. Winner: `esp_mesh_set_type(MESH_ROOT)`
9. `esp_mesh_start()`

## Files

| File | Changes |
|------|---------|
| **NEW: `include/espnow_election.h`** | Public API: `init()`, `run()` returning winner info |
| **NEW: `src/espnow_election.cpp`** | Election logic: ESP-NOW send/recv, timers, candidate tracking |
| `src/mesh_conductor.cpp` | Call election before `esp_mesh_start()`, apply winner's channel, remove bootstrap timer code |
| `src/CMakeLists.txt` | Add `espnow_election.cpp` |

## Constraints

- ESP-NOW and ESP-MESH cannot coexist — election must complete and `esp_now_deinit()` before `esp_mesh_start()`
- All nodes must be on ch1 during the election for ESP-NOW delivery
- Maximum candidates tracked: 16 (MESH_MAX_NODES)
- Frame size (9 bytes) is well within ESP-NOW's 250-byte limit
