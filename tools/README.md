# Squeek Serial Monitor

Multi-port serial monitor for debugging ESP32 boards side by side.
Each port gets its own native console window with full scrollback.

## Quick start

```bash
pip install -r requirements.txt

# Single board
python multi_monitor.py COM8

# Two boards (or more)
python multi_monitor.py COM8 COM9
```

## How it works

### Single-port mode (`python multi_monitor.py COM8`)

Opens a plain serial terminal in the current console at 115200 baud.
A reader thread pipes raw serial bytes to stdout; the main thread reads
keyboard input via `msvcrt.getch()` (Windows) and sends completed lines
to the serial port with `\r\n` termination.

### Multi-port mode (`python multi_monitor.py COM8 COM9 [COM10 ...]`)

The launcher process:

1. Starts a **TCP relay server** on `localhost` (random port).
2. Spawns one console window per port (`CREATE_NEW_CONSOLE` on Windows).
3. Passes `--relay <port>` to each child (internal flag, not user-facing).
4. Polls children every 300 ms — if **any** window closes, all others are
   terminated automatically. Ctrl+C in the launcher also closes everything.

Each child window connects to the relay server and behaves identically to
single-port mode, with the addition of Common Input Mode.

## Features

### Smart autoscroll (Windows)

Serial output is buffered when you scroll up in the console. When you
scroll back to the bottom, buffered output is flushed and normal streaming
resumes. This uses `GetConsoleScreenBufferInfo` via ctypes to detect the
viewport position relative to the cursor. Buffer is capped at 512 KB.

### Common Input Mode (multi-port only)

Press **F2** in any child window to toggle Common Input Mode.

| State | Behavior |
|-------|----------|
| OFF (default) | Typed lines go only to that window's serial port |
| ON | Typed lines go to the local port **and** are relayed to all other windows, which forward them to their serial ports |

Visual feedback:
- Window title shows `COM8 — Squeek Monitor [COMMON]` when active.
- A status line prints on toggle: `--- COMMON INPUT MODE ON (F2 to toggle) ---`

The relay is a simple TCP line protocol: when a child sends a `\n`-terminated
line to the server, the server broadcasts it to every other connected child.

### Keyboard reference

| Key | Action |
|-----|--------|
| Enter | Send current line to serial port (+ relay if common mode) |
| Backspace | Delete last character |
| F2 | Toggle Common Input Mode (multi-port only) |
| Ctrl+C | Quit (closes all windows in multi-port mode) |

## Architecture

```
 Launcher process
 +-------------------------------------------+
 |  RelayServer (localhost TCP)               |
 |    - accepts child connections             |
 |    - broadcasts lines between children     |
 |  Polls children, kills all on any exit     |
 +-------------------------------------------+
        |               |
   TCP socket      TCP socket
        |               |
 +-------------+  +-------------+
 | Child: COM8 |  | Child: COM9 |
 | serial_reader thread          |
 |   ser.read -> stdout.buffer   |
 |   (smart autoscroll on Win32) |
 | relay_reader thread           |
 |   relay recv -> ser.write     |
 | main thread (input loop)      |
 |   msvcrt.getch -> ser.write   |
 |   + relay send if common mode |
 +-------------+  +-------------+
```

## Dependencies

- **pyserial** (`>=3.5`) — serial port I/O
- **msvcrt** / **ctypes** — Windows console input and scroll detection (stdlib)

## Flashing workflow

Close the monitor, flash boards with PlatformIO, restart the monitor.
The monitor does not manage port release or flashing.
