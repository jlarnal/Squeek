#!/usr/bin/env python3
"""Serial monitor — one console window per port.

Single port:    python multi_monitor.py COM8
Multiple ports: python multi_monitor.py COM8 COM9 [COM10 ...]
                (spawns a separate console window for each port)

Baud rate:      python multi_monitor.py 9600 COM1 COM2
                (first numeric arg sets default baud for all ports)
Per-port baud:  python multi_monitor.py COM1@57600 COM2@9600
                (PORT@BAUD overrides the default for that port)

In multi-port mode, press F2 in any window to toggle COMMON INPUT MODE.
When active, lines you type are sent to ALL ports simultaneously.

Autoscroll: output pauses when you scroll up, resumes when you scroll
back to the bottom (Windows only).
"""

from __future__ import annotations

import glob as globmod
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from datetime import datetime

import serial

DEFAULT_BAUD = 115200
F2_SCANCODE = 0x3C
MAX_PENDING = 512 * 1024  # max buffered bytes while scrolled up
DEFAULT_RECONNECT_INTERVAL = 2.0  # seconds between reconnect attempts


# ---------------------------------------------------------------------------
# Config file loader
# ---------------------------------------------------------------------------

def _load_config() -> dict:
    """Load multi_monitor.config.json from the same directory as the script."""
    cfg_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "multi_monitor.config.json")
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


_config = _load_config()
RECONNECT_INTERVAL = _config.get("reconnect_interval_ms", 2000) / 1000.0


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

    # -- Job Object support ---------------------------------------------------
    # A Job Object with KILL_ON_JOB_CLOSE guarantees all child processes are
    # terminated when the launcher exits — regardless of how it exits (clean
    # return, Ctrl+C, X button, Task Manager kill, crash).

    class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_int64),
            ("PerJobUserTimeLimit", ctypes.c_int64),
            ("LimitFlags", ctypes.c_uint32),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", ctypes.c_uint32),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", ctypes.c_uint32),
            ("SchedulingClass", ctypes.c_uint32),
        ]

    class _IO_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_uint64),
            ("WriteOperationCount", ctypes.c_uint64),
            ("OtherOperationCount", ctypes.c_uint64),
            ("ReadTransferCount", ctypes.c_uint64),
            ("WriteTransferCount", ctypes.c_uint64),
            ("OtherTransferCount", ctypes.c_uint64),
        ]

    class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _JOBOBJECT_BASIC_LIMIT_INFORMATION),
            ("IoInfo", _IO_COUNTERS),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    _kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    _kernel32.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _kernel32.AssignProcessToJobObject.restype = ctypes.c_bool
    _kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    def _create_job_object():
        """Create a Win32 Job Object that kills all children on close."""
        job = _kernel32.CreateJobObjectW(None, None)
        if not job:
            return None
        info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        info.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
        _kernel32.SetInformationJobObject(
            job, 9,  # JobObjectExtendedLimitInformation
            ctypes.byref(info), ctypes.sizeof(info),
        )
        return job


# ---------------------------------------------------------------------------
# ELF & toolchain auto-detection
# ---------------------------------------------------------------------------

def find_elf_file() -> str | None:
    """Walk up from CWD and script dir looking for .pio/build/*/firmware.elf."""
    roots = {os.path.abspath(os.getcwd())}
    roots.add(os.path.abspath(os.path.dirname(__file__)))
    for start in list(roots):
        d = start
        for _ in range(6):
            matches = globmod.glob(os.path.join(d, ".pio", "build", "*", "firmware.elf"))
            if matches:
                return max(matches, key=os.path.getmtime)
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
    return None


def detect_arch(elf_path: str) -> str | None:
    """Read ELF e_machine to determine architecture."""
    try:
        with open(elf_path, "rb") as f:
            f.seek(18)
            e_machine = struct.unpack("<H", f.read(2))[0]
        if e_machine == 243:
            return "riscv32"
        if e_machine == 94:
            return "xtensa"
    except OSError:
        pass
    return None


def find_addr2line(arch: str) -> str | None:
    """Find the Espressif toolchain addr2line for the given architecture."""
    search_dirs = []
    idf_tools = os.environ.get("IDF_TOOLS_PATH")
    if idf_tools:
        search_dirs.append(idf_tools)
    if sys.platform == "win32":
        search_dirs.append(r"C:\Espressif\tools")
    search_dirs.append(os.path.expanduser(os.path.join("~", ".espressif", "tools")))
    pio_packages = os.path.expanduser(os.path.join("~", ".platformio", "packages"))

    prefix = "riscv32-esp-elf" if arch == "riscv32" else "xtensa-esp-elf"
    suffix = ".exe" if sys.platform == "win32" else ""

    # Search Espressif tool directories
    for search_dir in search_dirs:
        pattern = os.path.join(
            search_dir, prefix, "*", prefix, "bin", f"{prefix}-addr2line{suffix}"
        )
        matches = globmod.glob(pattern)
        if matches:
            return matches[0]

    # Search PlatformIO packages
    if arch == "riscv32":
        pio_pattern = os.path.join(
            pio_packages, "toolchain-riscv32-esp*", "bin",
            f"riscv32-esp-elf-addr2line{suffix}"
        )
    else:
        pio_pattern = os.path.join(
            pio_packages, "toolchain-xtensa-esp*", "bin",
            f"xtensa-esp-elf-addr2line{suffix}"
        )
    matches = globmod.glob(pio_pattern)
    if matches:
        return matches[0]

    return None


# ---------------------------------------------------------------------------
# Exception decoder
# ---------------------------------------------------------------------------

# Addresses in this range are typically code pointers (flash-mapped + IRAM)
_CODE_ADDR_RE = re.compile(r"0x(4[0-2][0-9a-fA-F]{6})")

class ExceptionDecoder:
    """Monitors serial output for ESP32 crash dumps and decodes addresses."""

    def __init__(self, addr2line: str, elf_path: str, arch: str) -> None:
        self._addr2line = addr2line
        self._elf = elf_path
        self._arch = arch
        self._in_crash = False
        self._key_addrs: list[str] = []   # MEPC, RA, PC, Backtrace
        self._stack_addrs: list[str] = []  # addresses found in stack dump

    def feed_line(self, line: str) -> str | None:
        """Feed one serial line. Returns decoded output on crash end, else None."""
        if "Guru Meditation Error" in line:
            self._in_crash = True
            self._key_addrs.clear()
            self._stack_addrs.clear()
            return None

        if not self._in_crash:
            return None

        stripped = line.strip()

        # RISC-V register dump: "MEPC    : 0x... RA      : 0x..."
        if stripped.startswith("MEPC"):
            for label in ("MEPC", "RA"):
                m = re.search(rf"{label}\s*:\s*(0x[0-9a-fA-F]+)", stripped)
                if m:
                    self._key_addrs.append(m.group(1))

        # Xtensa register dump: "PC      : 0x..."
        if stripped.startswith("PC") and ":" in stripped:
            m = re.search(r"PC\s*:\s*(0x[0-9a-fA-F]+)", stripped)
            if m:
                self._key_addrs.append(m.group(1))

        # Xtensa Backtrace line: "Backtrace: 0xPC:0xSP 0xPC:0xSP ..."
        if stripped.startswith("Backtrace:"):
            for m in re.finditer(r"0x([0-9a-fA-F]+):0x[0-9a-fA-F]+", stripped):
                self._key_addrs.append("0x" + m.group(1))

        # Stack memory dump lines: "408xxxxx: 0x... 0x... ..."
        if re.match(r"[0-9a-fA-F]{8}:", stripped):
            for m in _CODE_ADDR_RE.finditer(stripped):
                addr = "0x" + m.group(1)
                if addr not in self._key_addrs and addr not in self._stack_addrs:
                    self._stack_addrs.append(addr)

        # End of crash dump
        if "Rebooting..." in line:
            self._in_crash = False
            return self._decode()

        return None

    def _decode(self) -> str:
        """Run addr2line on all collected addresses."""
        addrs = self._key_addrs + self._stack_addrs
        if not addrs:
            return ""
        try:
            result = subprocess.run(
                [self._addr2line, "-pfiaCe", self._elf] + addrs,
                capture_output=True, text=True, timeout=10,
            )
            lines = [l for l in result.stdout.splitlines() if "??" not in l and l.strip()]
        except Exception:
            return "\n--- Exception decoder: addr2line failed ---\n"

        if not lines:
            return "\n--- Exception decoder: no symbols resolved ---\n"

        out = ["\n--- Decoded exception ---"]
        for l in lines:
            out.append("  " + l)
        out.append("--- End decoded exception ---\n")
        return "\n".join(out)


# ---------------------------------------------------------------------------
# Session logger
# ---------------------------------------------------------------------------

class SessionLogger:
    """Writes timestamped lines to a per-port log file.

    Format (timestamps on):
        12345.678 [COM9 <<] Battery: 1636 mV
        12350.201 [COM9 >>] reboot
    Format (timestamps off):
        [COM9 <<] Battery: 1636 mV
    """

    def __init__(self, port_name: str, log_dir: str, timestamps: bool = True) -> None:
        os.makedirs(log_dir, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self._path = os.path.join(log_dir, f"squeek_{port_name}_{stamp}.log")
        self._f = open(self._path, "w", encoding="utf-8")
        self._port = port_name
        self._timestamps = timestamps
        self._t0 = time.perf_counter()

    @property
    def path(self) -> str:
        return self._path

    def log_rx(self, text: str) -> None:
        """Log a line received from the serial port."""
        self._write("<<", text)

    def log_tx(self, text: str) -> None:
        """Log a line sent by the user."""
        self._write(">>", text)

    _ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

    def _write(self, direction: str, text: str) -> None:
        line = self._ANSI_RE.sub("", text).rstrip("\r\n")
        if self._timestamps:
            elapsed_ms = (time.perf_counter() - self._t0) * 1000.0
            self._f.write(f"{elapsed_ms:12.3f} [{self._port} {direction}] {line}\n")
        else:
            self._f.write(f"[{self._port} {direction}] {line}\n")
        self._f.flush()

    def close(self) -> None:
        self._f.close()


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
        try:
            while True:
                client, _ = self._sock.accept()
                with self._lock:
                    self._clients.append(client)
                threading.Thread(target=self._handle, args=(client,), daemon=True).start()
        except OSError:
            pass  # server socket closed by close()

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

    def close(self) -> None:
        """Shut down all client connections and the server socket."""
        with self._lock:
            for c in self._clients:
                try:
                    c.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    c.close()
                except Exception:
                    pass
            self._clients.clear()
        try:
            self._sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Single-port monitor (runs inside its own console window)
# ---------------------------------------------------------------------------

def single_port_monitor(
    port_name: str,
    baud: int = DEFAULT_BAUD,
    relay_port: int | None = None,
    elf_path: str | None = None,
    log_dir: str | None = None,
    no_timestamp: bool = False,
) -> None:
    common_mode = False
    relay_sock: socket.socket | None = None
    ser: serial.Serial | None = None
    disconnected = threading.Event()
    stop = threading.Event()

    def update_title() -> None:
        if sys.platform == "win32":
            if disconnected.is_set():
                suffix = " [DISCONNECTED]"
            elif common_mode:
                suffix = " [COMMON]"
            else:
                suffix = ""
            os.system(f"title {port_name} \u2014 Squeek Monitor{suffix}")

    def try_open() -> serial.Serial | None:
        """Try to open the serial port. Returns Serial or None."""
        try:
            return serial.Serial(port_name, baud, timeout=0.1)
        except serial.SerialException:
            return None

    def wait_for_reconnect() -> serial.Serial | None:
        """Block until port reopens or stop is requested."""
        disconnected.set()
        update_title()
        print(f"\n--- {port_name} disconnected — reconnecting... ---")
        if logger:
            logger.log_rx(f"--- disconnected, waiting to reconnect ---")
        while not stop.is_set():
            s = try_open()
            if s is not None:
                disconnected.clear()
                update_title()
                print(f"--- {port_name} reconnected ---\n")
                if logger:
                    logger.log_rx(f"--- reconnected ---")
                return s
            stop.wait(RECONNECT_INTERVAL)
        return None

    # Register Win32 console control handler for clean shutdown on
    # Ctrl+C, Ctrl+Break, and console window close (X button).
    if sys.platform == "win32":
        _HANDLER_ROUTINE = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_ulong)

        def _ctrl_handler(ctrl_type: int) -> bool:
            if ctrl_type in (0, 1, 2):  # CTRL_C, CTRL_BREAK, CTRL_CLOSE
                stop.set()
                return True
            return False

        # prevent GC of the callback
        _console_handler = _HANDLER_ROUTINE(_ctrl_handler)
        _kernel32.SetConsoleCtrlHandler(_console_handler, True)

    update_title()

    # Open serial port (wait if not available at startup)
    ser = try_open()
    if ser is None:
        disconnected.set()
        update_title()
        print(f"--- {port_name} not available — waiting to connect... ---")
        while not stop.is_set():
            ser = try_open()
            if ser is not None:
                disconnected.clear()
                update_title()
                print(f"--- {port_name} connected ---")
                break
            if sys.platform == "win32" and msvcrt.kbhit():
                if msvcrt.getch() == b"\x03":
                    print(f"\n--- {port_name} closed ---")
                    return
            stop.wait(RECONNECT_INTERVAL)
        if ser is None:
            return

    # Connect to relay (multi-port mode)
    if relay_port is not None:
        try:
            relay_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            relay_sock.connect(("127.0.0.1", relay_port))
        except Exception:
            relay_sock = None

    # Set up exception decoder
    decoder: ExceptionDecoder | None = None
    if elf_path:
        arch = detect_arch(elf_path)
        a2l = find_addr2line(arch) if arch else None
        if arch and a2l:
            decoder = ExceptionDecoder(a2l, elf_path, arch)
            elf_name = os.path.basename(elf_path)
            print(f"--- Exception decoder active ({elf_name}, {arch}) ---")
        else:
            missing = "arch unknown" if not arch else "addr2line not found"
            print(f"--- Exception decoder: {missing} ---")

    # Set up session logger
    logger: SessionLogger | None = None
    if log_dir:
        try:
            logger = SessionLogger(port_name, log_dir, timestamps=not no_timestamp)
            print(f"--- Logging to {logger.path} ---")
        except OSError as e:
            print(f"--- Logging failed: {e} ---")

    print(f"--- {port_name} @ {baud} baud ---")
    if relay_sock:
        print("--- F2: toggle COMMON INPUT mode ---")
    print("--- Ctrl+C to quit ---\n")

    # -- Serial reader thread (with smart autoscroll + reconnect) ------------

    def serial_reader() -> None:
        nonlocal ser
        smart_scroll = sys.platform == "win32"
        pending = bytearray()
        line_buf = b""
        while not stop.is_set():
            try:
                data = ser.read(ser.in_waiting or 1)
            except (serial.SerialException, OSError):
                try:
                    ser.close()
                except Exception:
                    pass
                new_ser = wait_for_reconnect()
                if new_ser is None:
                    break
                ser = new_ser
                line_buf = b""
                continue
            except Exception:
                break

            if data:
                line_buf += data
                # Extract complete lines
                while b"\n" in line_buf:
                    line_bytes, line_buf = line_buf.split(b"\n", 1)
                    full_line = line_bytes + b"\n"
                    pending.extend(full_line)
                    # Log and feed to decoder
                    text = line_bytes.decode("utf-8", errors="replace")
                    if logger:
                        logger.log_rx(text)
                    if decoder:
                        try:
                            result = decoder.feed_line(text)
                            if result:
                                pending.extend(result.encode("utf-8", errors="replace"))
                                if logger:
                                    for dl in result.splitlines():
                                        if dl.strip():
                                            logger.log_rx(dl)
                        except Exception:
                            pass
            else:
                # Read timeout — flush incomplete line for display
                if line_buf:
                    pending.extend(line_buf)
                    line_buf = b""

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
                    if not disconnected.is_set():
                        try:
                            ser.write(line + b"\n")
                        except (serial.SerialException, OSError):
                            pass
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
        if logger:
            logger.log_tx(line)
        if not disconnected.is_set():
            try:
                ser.write((line + "\r\n").encode())
                ser.flush()
            except (serial.SerialException, OSError):
                pass
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
            while not stop.is_set():
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
            while not stop.is_set():
                line = input()
                send_line(line)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        if relay_sock:
            relay_sock.close()
        try:
            ser.close()
        except Exception:
            pass
        if logger:
            logger.close()
        print(f"\n--- {port_name} closed ---")


# ---------------------------------------------------------------------------
# Multi-port launcher (spawns one console window per port)
# ---------------------------------------------------------------------------

def spawn_windows(
    port_bauds: list[tuple[str, int]],
    elf_path: str | None = None,
    log_dir: str | None = None,
    no_timestamp: bool = False,
) -> None:
    # On Windows, a Job Object with KILL_ON_JOB_CLOSE guarantees all child
    # processes are terminated when the launcher exits — for any reason.
    job = _create_job_object() if sys.platform == "win32" else None

    relay = RelayServer()
    threading.Thread(target=relay.serve_forever, daemon=True).start()
    print(f"Relay server on localhost:{relay.port}")

    script = os.path.abspath(__file__)
    procs: list[tuple[str, subprocess.Popen]] = []

    for port, baud in port_bauds:
        # Pass PORT@BAUD so the child parses it back
        port_arg = f"{port}@{baud}" if baud != DEFAULT_BAUD else port
        args = [sys.executable, script, port_arg, "--relay", str(relay.port)]
        if elf_path:
            args.extend(["--elf", elf_path])
        if log_dir:
            args.extend(["--log-dir", log_dir])
        if no_timestamp:
            args.append("--no-timestamp")
        if sys.platform == "win32":
            proc = subprocess.Popen(args, creationflags=subprocess.CREATE_NEW_CONSOLE)
            if job:
                _kernel32.AssignProcessToJobObject(job, int(proc._handle))
        else:
            proc = subprocess.Popen(args, start_new_session=True)
        procs.append((port, proc))
        print(f"  Launched {port} @ {baud} (PID {proc.pid})")

    print(f"\n{len(procs)} monitor(s) running.")
    print("Close any window or Ctrl+C here to quit all.")

    try:
        while all(proc.poll() is None for _, proc in procs):
            time.sleep(0.3)
    except KeyboardInterrupt:
        pass
    finally:
        print("\nClosing all monitors...")
        if job:
            # Closing the job handle kills all children instantly
            _kernel32.CloseHandle(job)
        else:
            # Non-Windows: send SIGTERM
            for _, proc in procs:
                if proc.poll() is None:
                    proc.terminate()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _boot_boards(ports: list[str]) -> None:
    """Reboot all boards via esptool chip_id before monitoring.

    Reads the esptool path from upload_all.config.json (same dir as this script).
    Uses --chip auto so no platformio.ini parsing is needed.
    """
    config_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "upload_all.config.json")
    if not os.path.exists(config_path):
        print(f"--- Boot: config not found: {config_path} ---")
        return
    with open(config_path) as f:
        cfg = json.load(f)
    esptool = cfg.get("esptool", "")
    if not esptool:
        print("--- Boot: no 'esptool' in config ---")
        return
    esptool = str(os.path.expandvars(esptool))
    if not os.path.exists(esptool):
        print(f"--- Boot: esptool not found: {esptool} ---")
        return

    print(f"--- Rebooting {len(ports)} board(s) via esptool... ---")
    # Retry loop: ports may still be held by the OS after a prior esptool exit
    for attempt in range(3):
        procs = []
        failed_ports = []
        for port in ports:
            cmd = [esptool, "--chip", "auto", "--port", port, "chip_id"]
            procs.append((port, subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                                 stderr=subprocess.STDOUT)))
        for port, proc in procs:
            try:
                proc.wait(timeout=8)
                if proc.returncode == 0:
                    print(f"  [{port}] ok")
                else:
                    print(f"  [{port}] failed ({proc.returncode})")
                    failed_ports.append(port)
            except subprocess.TimeoutExpired:
                proc.kill()
                print(f"  [{port}] timeout")
                failed_ports.append(port)
        if not failed_ports:
            break
        if attempt < 2:
            print(f"  Retrying {', '.join(failed_ports)} in 2s...")
            time.sleep(2)
            ports = failed_ports
    print("--- Boot done ---\n")


def _parse_port(token: str, default_baud: int) -> tuple[str, int]:
    """Parse 'COM1' or 'COM1@9600' into (port_name, baud_rate)."""
    if "@" in token:
        port, baud_str = token.rsplit("@", 1)
        return port, int(baud_str)
    return token, default_baud


def main() -> None:
    if len(sys.argv) < 2 or any(a in ("-h", "--help") for a in sys.argv[1:]):
        print(f"Usage: {sys.argv[0]} [BAUD] <PORT[@BAUD]> [PORT[@BAUD] ...]")
        print(f"  Single port:    {sys.argv[0]} COM8")
        print(f"  Custom baud:    {sys.argv[0]} 9600 COM8 COM9")
        print(f"  Per-port baud:  {sys.argv[0]} COM8@57600 COM9")
        print()
        print("Options:")
        print("  -b, --boot         Reboot all boards via esptool before monitoring")
        print("  -n, --no-log       Disable session file logging")
        print("  -nt, --no-timestamp Disable elapsed-ms timestamps in log lines")
        print("  --elf <path>       Override ELF file for exception decoding")
        print("  --log-dir <dir>    Override log output directory")
        print("  -h, --help         Show this help message")
        sys.exit(0 if "--help" in sys.argv or "-h" in sys.argv else 1)

    # Parse args (--relay is internal, set by launcher)
    relay_port: int | None = None
    elf_path: str | None = None
    log_dir: str | None = None
    no_log = False
    no_timestamp = False
    boot = False
    positional: list[str] = []
    i = 1
    while i < len(sys.argv):
        if sys.argv[i] == "--relay" and i + 1 < len(sys.argv):
            relay_port = int(sys.argv[i + 1])
            i += 2
        elif sys.argv[i] == "--elf" and i + 1 < len(sys.argv):
            elf_path = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == "--log-dir" and i + 1 < len(sys.argv):
            log_dir = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] in ("-b", "--boot"):
            boot = True
            i += 1
        elif sys.argv[i] in ("-n", "--no-log", "--nolog"):
            no_log = True
            i += 1
        elif sys.argv[i] in ("-nt", "--no-timestamp"):
            no_timestamp = True
            i += 1
        else:
            positional.append(sys.argv[i])
            i += 1

    # If first positional arg is a bare integer, it's the default baud rate
    default_baud = DEFAULT_BAUD
    if positional and positional[0].isdigit():
        default_baud = int(positional.pop(0))

    # Parse PORT[@BAUD] tokens
    port_bauds: list[tuple[str, int]] = []
    for token in positional:
        port_bauds.append(_parse_port(token, default_baud))

    # Auto-detect ELF if not specified
    if not elf_path:
        elf_path = find_elf_file()

    # Default log directory: logs/ next to this script
    if not no_log and not log_dir:
        log_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")

    if no_log:
        log_dir = None

    # Reboot all boards before monitoring
    if boot and port_bauds:
        _boot_boards([port for port, _ in port_bauds])

    if len(port_bauds) == 1:
        port, baud = port_bauds[0]
        single_port_monitor(port, baud, relay_port, elf_path, log_dir, no_timestamp)
    elif len(port_bauds) > 1:
        spawn_windows(port_bauds, elf_path, log_dir, no_timestamp)
    else:
        print("Error: no port specified.")
        sys.exit(1)


if __name__ == "__main__":
    main()
