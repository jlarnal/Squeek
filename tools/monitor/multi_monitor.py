#!/usr/bin/env python3
"""Serial monitor — one console window per port.

Single port:    python multi_monitor.py COM8
Multiple ports: python multi_monitor.py COM8 COM9 [COM10 ...]
                (spawns a separate console window for each port)

In multi-port mode, press F2 in any window to toggle COMMON INPUT MODE.
When active, lines you type are sent to ALL ports simultaneously.

Autoscroll: output pauses when you scroll up, resumes when you scroll
back to the bottom (Windows only).
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import threading
import time

import serial

BAUD = 115200
F2_SCANCODE = 0x3C
MAX_PENDING = 512 * 1024  # max buffered bytes while scrolled up


# ---------------------------------------------------------------------------
# Win32 console helpers (autoscroll detection)
# ---------------------------------------------------------------------------

if sys.platform == "win32":
    import ctypes
    import msvcrt

    class _COORD(ctypes.Structure):
        _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

    class _SMALL_RECT(ctypes.Structure):
        _fields_ = [
            ("Left", ctypes.c_short), ("Top", ctypes.c_short),
            ("Right", ctypes.c_short), ("Bottom", ctypes.c_short),
        ]

    class _CSBI(ctypes.Structure):
        _fields_ = [
            ("dwSize", _COORD),
            ("dwCursorPosition", _COORD),
            ("wAttributes", ctypes.c_ushort),
            ("srWindow", _SMALL_RECT),
            ("dwMaximumWindowSize", _COORD),
        ]

    _kernel32 = ctypes.windll.kernel32
    _h_stdout = _kernel32.GetStdHandle(-11)

    def _viewport_at_bottom() -> bool:
        """True if the console viewport includes the cursor row."""
        csbi = _CSBI()
        if not _kernel32.GetConsoleScreenBufferInfo(_h_stdout, ctypes.byref(csbi)):
            return True  # assume bottom on API failure
        return csbi.srWindow.Bottom >= csbi.dwCursorPosition.Y


# ---------------------------------------------------------------------------
# TCP relay server (runs inside the launcher process)
# ---------------------------------------------------------------------------

class RelayServer:
    """Accepts connections from child monitors. When one sends a line,
    broadcasts it to every *other* connected child."""

    def __init__(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(16)
        self.port: int = self._sock.getsockname()[1]
        self._clients: list[socket.socket] = []
        self._lock = threading.Lock()

    def serve_forever(self) -> None:
        while True:
            client, _ = self._sock.accept()
            with self._lock:
                self._clients.append(client)
            threading.Thread(target=self._handle, args=(client,), daemon=True).start()

    def _handle(self, client: socket.socket) -> None:
        buf = b""
        try:
            while True:
                data = client.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self._broadcast(line + b"\n", exclude=client)
        except Exception:
            pass
        finally:
            with self._lock:
                if client in self._clients:
                    self._clients.remove(client)
            client.close()

    def _broadcast(self, data: bytes, exclude: socket.socket) -> None:
        with self._lock:
            for c in list(self._clients):
                if c is not exclude:
                    try:
                        c.sendall(data)
                    except Exception:
                        self._clients.remove(c)


# ---------------------------------------------------------------------------
# Single-port monitor (runs inside its own console window)
# ---------------------------------------------------------------------------

def single_port_monitor(port_name: str, relay_port: int | None = None) -> None:
    common_mode = False
    relay_sock: socket.socket | None = None

    def update_title() -> None:
        if sys.platform == "win32":
            suffix = " [COMMON]" if common_mode else ""
            os.system(f"title {port_name} \u2014 Squeek Monitor{suffix}")

    update_title()

    # Open serial port
    try:
        ser = serial.Serial(port_name, BAUD, timeout=0.1)
    except serial.SerialException as e:
        print(f"Failed to open {port_name}: {e}")
        input("Press Enter to exit...")
        return

    # Connect to relay (multi-port mode)
    if relay_port is not None:
        try:
            relay_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            relay_sock.connect(("127.0.0.1", relay_port))
        except Exception:
            relay_sock = None

    print(f"--- {port_name} @ {BAUD} baud ---")
    if relay_sock:
        print("--- F2: toggle COMMON INPUT mode ---")
    print("--- Ctrl+C to quit ---\n")

    stop = threading.Event()

    # -- Serial reader thread (with smart autoscroll) ------------------------

    def serial_reader() -> None:
        smart_scroll = sys.platform == "win32"
        pending = bytearray()
        while not stop.is_set():
            try:
                data = ser.read(1024)
            except serial.SerialException:
                print(f"\n--- {port_name} disconnected ---")
                break
            except Exception:
                break

            if data:
                pending.extend(data)
                if len(pending) > MAX_PENDING:
                    pending = pending[-MAX_PENDING:]

            # Flush to console only when viewport is at the bottom
            if pending and (not smart_scroll or _viewport_at_bottom()):
                sys.stdout.buffer.write(bytes(pending))
                sys.stdout.buffer.flush()
                pending.clear()

    # -- Relay reader thread -------------------------------------------------

    def relay_reader() -> None:
        assert relay_sock is not None
        buf = b""
        try:
            while not stop.is_set():
                data = relay_sock.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if ser.is_open:
                        ser.write(line + b"\n")
        except Exception:
            pass

    threading.Thread(target=serial_reader, daemon=True).start()
    if relay_sock:
        threading.Thread(target=relay_reader, daemon=True).start()

    # -- Input loop ----------------------------------------------------------

    def toggle_common() -> None:
        nonlocal common_mode
        common_mode = not common_mode
        update_title()
        state = "ON" if common_mode else "OFF"
        print(f"\r--- COMMON INPUT MODE {state} (F2 to toggle) ---")

    def send_line(line: str) -> None:
        if ser.is_open:
            ser.write((line + "\r\n").encode())
            ser.flush()
        if common_mode and relay_sock:
            try:
                relay_sock.sendall((line + "\n").encode())
            except Exception:
                pass

    try:
        if sys.platform == "win32":
            # Drain any stale input left by os.system("title ...")
            while msvcrt.kbhit():
                msvcrt.getch()
            line_buf = ""
            while ser.is_open and not stop.is_set():
                if not msvcrt.kbhit():
                    time.sleep(0.01)
                    continue

                b = msvcrt.getch()

                # Special key prefix (function keys, arrows, etc.)
                if b in (b"\x00", b"\xe0"):
                    scan = msvcrt.getch()[0]
                    if scan == F2_SCANCODE and relay_sock:
                        toggle_common()
                    continue

                code = b[0]

                if code == 13:  # Enter
                    sys.stdout.buffer.write(b"\r\n")
                    sys.stdout.buffer.flush()
                    send_line(line_buf)
                    line_buf = ""
                elif code == 8:  # Backspace
                    if line_buf:
                        line_buf = line_buf[:-1]
                        sys.stdout.buffer.write(b"\b \b")
                        sys.stdout.buffer.flush()
                elif code == 3:  # Ctrl+C
                    raise KeyboardInterrupt
                elif 32 <= code < 127:
                    line_buf += chr(code)
                    sys.stdout.buffer.write(b)
                    sys.stdout.buffer.flush()
        else:
            # Non-Windows fallback (no F2 / smart scroll)
            while ser.is_open and not stop.is_set():
                line = input()
                send_line(line)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        if relay_sock:
            relay_sock.close()
        ser.close()
        print(f"\n--- {port_name} closed ---")


# ---------------------------------------------------------------------------
# Multi-port launcher (spawns one console window per port)
# ---------------------------------------------------------------------------

def spawn_windows(ports: list[str]) -> None:
    relay = RelayServer()
    threading.Thread(target=relay.serve_forever, daemon=True).start()
    print(f"Relay server on localhost:{relay.port}")

    script = os.path.abspath(__file__)
    procs: list[tuple[str, subprocess.Popen]] = []

    for port in ports:
        args = [sys.executable, script, port, "--relay", str(relay.port)]
        if sys.platform == "win32":
            proc = subprocess.Popen(args, creationflags=subprocess.CREATE_NEW_CONSOLE)
        else:
            proc = subprocess.Popen(args, start_new_session=True)
        procs.append((port, proc))
        print(f"  Launched {port} (PID {proc.pid})")

    print(f"\n{len(procs)} monitor(s) running.")
    print("Close any window or Ctrl+C here to quit all.")

    try:
        # Poll until any child exits — then tear down everything
        while all(proc.poll() is None for _, proc in procs):
            time.sleep(0.3)
    except KeyboardInterrupt:
        pass
    finally:
        print("\nClosing all monitors...")
        for _, proc in procs:
            if proc.poll() is None:
                proc.terminate()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <PORT> [PORT2 ...]")
        print(f"  Single port:  {sys.argv[0]} COM8")
        print(f"  Multi port:   {sys.argv[0]} COM8 COM9")
        sys.exit(1)

    # Parse args (--relay is internal, set by launcher)
    relay_port: int | None = None
    ports: list[str] = []
    i = 1
    while i < len(sys.argv):
        if sys.argv[i] == "--relay" and i + 1 < len(sys.argv):
            relay_port = int(sys.argv[i + 1])
            i += 2
        else:
            ports.append(sys.argv[i])
            i += 1

    if len(ports) == 1:
        single_port_monitor(ports[0], relay_port)
    elif len(ports) > 1:
        spawn_windows(ports)
    else:
        print("Error: no port specified.")
        sys.exit(1)


if __name__ == "__main__":
    main()
