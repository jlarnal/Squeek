#!/usr/bin/env python3
"""Build once, flash many — parallel ESP32 upload tool.

All flash offsets and file paths are derived automatically from platformio.ini
and PlatformIO build outputs — zero hardcoded constants.  Flashing and erasing
use EspTool_Multi.exe (C# interleaved round-robin, no esptool dependency).

Usage:
    upload_all.py -b <targets> -f <targets> -e <targets> -p <ports> [-v ENV]

Targets:
    -b/--build   app | fs | all         Build specified targets
    -f/--flash   boot | app | fs | all  Flash specified targets
    -e/--erase   boot | app | fs | nvs | all   Erase partitions before flash

Ports:
    -p/--ports   COM8 COM10@115200      Target ports (optional per-port baud)

Examples:
    upload_all.py -b app -f app -p COM8 COM9
    upload_all.py -b all -f all -e all -p COM8 COM9 COM10
    upload_all.py -f fs -p COM8
    upload_all.py -b fs
    upload_all.py -b app -f boot app -e nvs -p COM8 COM10@115200 COM12
"""

import argparse
import atexit
import configparser
import csv
import ctypes
import json
import os
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
CONFIG_PATH = SCRIPT_DIR / "upload_all.config.json"

ESPTOOL_MULTI = SCRIPT_DIR / "EspTool_Multi.exe"

# Partition table is always 0x1000 bytes on ESP32
PARTITION_TABLE_SIZE = 0x1000


# ---------------------------------------------------------------------------
# Logging — tee stdout/stderr to timestamped log file
# ---------------------------------------------------------------------------

class _TeeWriter:
    """Duplicates writes to both the original stream and a log file.

    Log lines are prefixed with elapsed milliseconds since start.
    """
    _t0 = time.perf_counter()

    def __init__(self, original, log_file):
        self._original = original
        self._log = log_file
        self._at_line_start = True

    def write(self, text):
        self._original.write(text)
        for ch in text:
            if self._at_line_start and ch not in ("\r", "\n"):
                ms = (time.perf_counter() - _TeeWriter._t0) * 1000
                self._log.write(f"[{ms:10.1f}ms] ")
                self._at_line_start = False
            self._log.write(ch)
            if ch == "\n":
                self._at_line_start = True

    def flush(self):
        self._original.flush()
        self._log.flush()

    def __getattr__(self, name):
        return getattr(self._original, name)


def _setup_logging():
    """Tee stdout/stderr to a timestamped log file in tools/logs/."""
    log_dir = SCRIPT_DIR / "logs"
    log_dir.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = log_dir / f"upload_{stamp}.log"
    log_file = open(log_path, "w", encoding="utf-8")
    original_stdout = sys.stdout
    original_stderr = sys.stderr
    sys.stdout = _TeeWriter(original_stdout, log_file)
    sys.stderr = _TeeWriter(original_stderr, log_file)

    def _restore_and_close():
        sys.stdout = original_stdout
        sys.stderr = original_stderr
        log_file.close()

    atexit.register(_restore_and_close)
    return log_path


# ---------------------------------------------------------------------------
# Win32 Job Object — guarantees child processes die with the parent
# ---------------------------------------------------------------------------
_job = None

if sys.platform == "win32":
    _kernel32 = ctypes.windll.kernel32

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

    _job = _kernel32.CreateJobObjectW(None, None)
    if _job:
        _info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        _info.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
        _kernel32.SetInformationJobObject(
            _job, 9, ctypes.byref(_info), ctypes.sizeof(_info),
        )
        atexit.register(_kernel32.CloseHandle, _job)


# ---------------------------------------------------------------------------
# Config & PlatformIO helpers
# ---------------------------------------------------------------------------

def load_config():
    if not CONFIG_PATH.exists():
        sys.exit(f"ERROR: Config file not found: {CONFIG_PATH}")
    with open(CONFIG_PATH) as f:
        cfg = json.load(f)
    p = cfg.get("platformio")
    if not p:
        sys.exit(f"ERROR: 'platformio' not set in {CONFIG_PATH}")
    cfg["platformio"] = str(Path(os.path.expandvars(p)).expanduser())
    if not Path(cfg["platformio"]).exists():
        sys.exit(f"ERROR: platformio path does not exist: {cfg['platformio']}")
    cfg.setdefault("baud", 460800)
    return cfg


_ini_cache = None

def _read_ini():
    """Read and return the parsed platformio.ini ConfigParser (cached)."""
    global _ini_cache
    if _ini_cache is not None:
        return _ini_cache
    ini_path = PROJECT_DIR / "platformio.ini"
    if not ini_path.exists():
        sys.exit(f"ERROR: platformio.ini not found at {ini_path}")
    cp = configparser.ConfigParser()
    cp.read(str(ini_path))
    _ini_cache = cp
    return cp


def resolve_env(env_arg):
    """Return the PlatformIO environment name."""
    if env_arg:
        return env_arg
    cp = _read_ini()
    default = cp.get("platformio", "default_envs", fallback=None)
    if not default:
        sys.exit("ERROR: No --environment given and no default_envs in platformio.ini")
    return default.strip()


def _parse_flash_offset(filepath):
    """Extract the flash offset from a PlatformIO *-flash_args file."""
    if not filepath.exists():
        sys.exit(f"ERROR: Flash args file not found: {filepath}")
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("--"):
                continue
            parts = line.split()
            if len(parts) == 2:
                return parts[0]
    sys.exit(f"ERROR: No offset found in {filepath}")


# ---------------------------------------------------------------------------
# Partition resolution
# ---------------------------------------------------------------------------

def _resolve_partitions_csv(env_name):
    """Resolve the partitions CSV path from platformio.ini for the given env."""
    cp = _read_ini()
    env_section = f"env:{env_name}"
    partitions_file = None
    if cp.has_option(env_section, "board_build.partitions"):
        partitions_file = cp.get(env_section, "board_build.partitions").strip()
    elif cp.has_option("env", "board_build.partitions"):
        partitions_file = cp.get("env", "board_build.partitions").strip()
    if not partitions_file:
        sys.exit(f"ERROR: No board_build.partitions found in platformio.ini "
                 f"for env '{env_name}' or [env] base")
    csv_path = PROJECT_DIR / partitions_file
    if not csv_path.exists():
        sys.exit(f"ERROR: Partition table not found: {csv_path}")
    return csv_path


def resolve_partition_map(env_name):
    """Read partitions CSV. Return dict mapping target names to (offset, size).

    Maps: app (first 'app' type), fs ('storage'), nvs ('nvs').
    """
    csv_path = _resolve_partitions_csv(env_name)
    result = {}
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#"):
                continue
            name = row[0].strip()
            ptype = row[1].strip()
            offset = int(row[3].strip(), 0)
            size = int(row[4].strip(), 0)

            if ptype == "app" and "app" not in result:
                result["app"] = (offset, size)
            if name == "storage":
                result["fs"] = (offset, size)
            if name == "nvs":
                result["nvs"] = (offset, size)
    return result


def resolve_flash_layout(build_dir, env_name):
    """Build full flash layout for all targets.

    Returns dict: target -> {
        'files': [(offset_str, filepath), ...],   # for write_flash
        'erase': (offset_int, size_int),           # for erase_region
    }
    """
    partitions = resolve_partition_map(env_name)
    layout = {}

    # boot: bootloader + partition table (always a pair)
    bl_offset_str = _parse_flash_offset(build_dir / "bootloader-flash_args")
    pt_offset_str = _parse_flash_offset(build_dir / "partition-table-flash_args")
    bl_offset = int(bl_offset_str, 0)
    pt_offset = int(pt_offset_str, 0)
    layout['boot'] = {
        'files': [
            (bl_offset_str, str(build_dir / "bootloader.bin")),
            (pt_offset_str, str(build_dir / "partitions.bin")),
        ],
        'erase': (bl_offset, pt_offset + PARTITION_TABLE_SIZE - bl_offset),
    }

    # app: firmware
    app_offset_str = _parse_flash_offset(build_dir / "app-flash_args")
    app_part = partitions.get("app")
    layout['app'] = {
        'files': [(app_offset_str, str(build_dir / "firmware.bin"))],
        'erase': app_part if app_part else None,
    }

    # fs: LittleFS image
    fs_part = partitions.get("fs")
    if fs_part:
        layout['fs'] = {
            'files': [(hex(fs_part[0]), str(build_dir / "littlefs.bin"))],
            'erase': fs_part,
        }

    # nvs: erase only, nothing to flash or build
    nvs_part = partitions.get("nvs")
    if nvs_part:
        layout['nvs'] = {
            'erase': nvs_part,
        }

    return layout


# ---------------------------------------------------------------------------
# Port parsing
# ---------------------------------------------------------------------------

def parse_port_specs(port_args, default_baud):
    """Parse port specs like 'COM8' or 'COM10@115200'.

    Returns (baud_groups, all_ports) where baud_groups is {baud: [ports]}.
    """
    groups = {}
    all_ports = []
    for spec in port_args:
        if '@' in spec:
            port, baud_str = spec.rsplit('@', 1)
            try:
                baud = int(baud_str)
            except ValueError:
                sys.exit(f"ERROR: Invalid baud rate in '{spec}'")
        else:
            port = spec
            baud = default_baud
        groups.setdefault(baud, []).append(port)
        all_ports.append(port)
    return groups, all_ports


# ---------------------------------------------------------------------------
# PlatformIO build
# ---------------------------------------------------------------------------

def build_app(cfg, env_arg):
    cmd = [cfg["platformio"], "run"]
    if env_arg:
        cmd += ["-e", env_arg]
    print(f">> BUILD APP: {' '.join(cmd)}")
    rc = subprocess.run(cmd, cwd=str(PROJECT_DIR)).returncode
    if rc != 0:
        sys.exit(f"ERROR: Build failed (exit {rc})")


def build_filesystem(cfg, env_arg):
    """Build the LittleFS image from ./data using PlatformIO."""
    data_dir = PROJECT_DIR / "data"
    if not data_dir.is_dir():
        sys.exit(f"ERROR: Data folder not found: {data_dir}")
    cmd = [cfg["platformio"], "run", "--target", "buildfs"]
    if env_arg:
        cmd += ["-e", env_arg]
    print(f">> BUILD FS: {' '.join(cmd)}")
    rc = subprocess.run(cmd, cwd=str(PROJECT_DIR)).returncode
    if rc != 0:
        sys.exit(f"ERROR: Filesystem build failed (exit {rc})")


# ---------------------------------------------------------------------------
# EspTool_Multi interface
# ---------------------------------------------------------------------------

def _stream_output(pipe):
    """Stream subprocess output, preserving \\r progress updates.

    EspTool_Multi formats its own per-port prefixes ([COM8] FLASH 45% ...),
    so we pass lines through without adding our own prefix.
    """
    buf = b""
    while True:
        chunk = pipe.read1(256)
        if not chunk:
            break
        buf += chunk
        while b"\r" in buf or b"\n" in buf:
            cr = buf.find(b"\r")
            lf = buf.find(b"\n")
            if cr >= 0 and (lf < 0 or cr < lf):
                line = buf[:cr]
                if cr + 1 < len(buf) and buf[cr + 1:cr + 2] == b"\n":
                    buf = buf[cr + 2:]
                else:
                    buf = buf[cr + 1:]
            else:
                line = buf[:lf]
                buf = buf[lf + 1:]
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                print(f"  {text}", flush=True)
    text = buf.decode("utf-8", errors="replace").strip()
    if text:
        print(f"  {text}", flush=True)


def _run_multi(ports, baud, args_list, timeout=300):
    """Run EspTool_Multi with the given arguments.

    Returns (passed, failed) counts.
    Stdout is streamed in a daemon thread so the main thread can enforce
    the timeout even if the process stalls without closing its pipe.
    """
    if not ESPTOOL_MULTI.exists():
        sys.exit(f"ERROR: EspTool_Multi.exe not found at {ESPTOOL_MULTI}")

    cmd = [str(ESPTOOL_MULTI), "-p", ",".join(ports), "-b", str(baud)] + args_list

    print(f"   CMD: {' '.join(cmd)}")

    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        if _job and sys.platform == "win32":
            _kernel32.AssignProcessToJobObject(_job, int(proc._handle))

        reader = threading.Thread(
            target=_stream_output, args=(proc.stdout,), daemon=True
        )
        reader.start()
        reader.join(timeout=timeout)

        if reader.is_alive():
            proc.kill()
            proc.stdout.close()
            reader.join(timeout=5)
            proc.wait()
            print(f"  [FAILED] EspTool_Multi timed out after {timeout}s")
            return 0, len(ports)

        rc = proc.wait(timeout=10)

        if rc == 0:
            return len(ports), 0
        elif rc == 2:
            print("  [FAILED] Chip type mismatch across ports")
            return 0, len(ports)
        else:
            print(f"  [PARTIAL] EspTool_Multi exited with code {rc}")
            return 0, len(ports)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.stdout.close()
        proc.wait()
        print(f"  [FAILED] EspTool_Multi timed out after {timeout}s")
        return 0, len(ports)
    except Exception as e:
        try:
            proc.kill()
            proc.stdout.close()
            proc.wait()
        except Exception:
            pass
        print(f"  [FAILED] EspTool_Multi error: {e}")
        return 0, len(ports)


def run_erase_flash(baud_groups):
    """Full chip erase across all port groups."""
    print(f"\n>> ERASE FLASH (full chip)")
    total_p, total_f = 0, 0
    for baud, ports in baud_groups.items():
        p, f = _run_multi(ports, baud, ["erase_flash"])
        total_p += p
        total_f += f
    return total_p, total_f


def run_erase_region(baud_groups, offset, size, label=""):
    """Erase a specific flash region across all port groups."""
    desc = f" ({label})" if label else ""
    print(f"\n>> ERASE REGION{desc}: offset={hex(offset)}, size={hex(size)}")
    total_p, total_f = 0, 0
    for baud, ports in baud_groups.items():
        p, f = _run_multi(ports, baud, ["erase_region", hex(offset), hex(size)])
        total_p += p
        total_f += f
    return total_p, total_f


def run_write_flash(baud_groups, flash_pairs):
    """Write flash segments across all port groups."""
    print(f"\n>> WRITE FLASH ({len(flash_pairs)} segment(s))")
    for offset, filepath in flash_pairs:
        print(f"     {offset} -> {Path(filepath).name}")

    args = ["write_flash"]
    for offset, filepath in flash_pairs:
        args += [offset, filepath]

    total_p, total_f = 0, 0
    for baud, ports in baud_groups.items():
        p, f = _run_multi(ports, baud, args)
        total_p += p
        total_f += f
    return total_p, total_f


# ---------------------------------------------------------------------------
# Target expansion & validation
# ---------------------------------------------------------------------------

BUILD_ALL = {'app', 'fs'}
FLASH_ALL = {'boot', 'app', 'fs'}

def expand_targets(targets, all_set):
    """Expand 'all' into its components and deduplicate."""
    if targets is None:
        return set()
    result = set()
    for t in targets:
        if t == 'all':
            result.update(all_set)
        else:
            result.add(t)
    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Build once, flash many ESP32 boards in parallel.",
        epilog="Examples:\n"
               "  upload_all.py -b app -f app -p COM8 COM9\n"
               "  upload_all.py -b all -f all -e all -p COM8 COM9\n"
               "  upload_all.py -f fs -p COM8\n"
               "  upload_all.py -b fs\n"
               "  upload_all.py -b app -f boot app -e nvs -p COM8 COM10@115200\n",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "-b", "--build", nargs="+",
        choices=["app", "fs", "all"],
        help="Build targets: app, fs, all"
    )
    parser.add_argument(
        "-f", "--flash", nargs="+",
        choices=["boot", "app", "fs", "all"],
        help="Flash targets: boot (bootloader+partitions), app, fs, all"
    )
    parser.add_argument(
        "-e", "--erase", nargs="+",
        choices=["boot", "app", "fs", "nvs", "all"],
        help="Erase targets: boot, app, fs, nvs, all (full chip)"
    )
    parser.add_argument(
        "-p", "--ports", nargs="+", metavar="PORT",
        help="Target ports (e.g. COM8 COM10@115200)"
    )
    parser.add_argument(
        "-v", "--environment", dest="env", default=None, metavar="ENV",
        help="PlatformIO environment (default: from platformio.ini)"
    )
    args = parser.parse_args()

    # ── Validate ──
    if not args.build and not args.flash and not args.erase:
        parser.print_help()
        sys.exit(1)

    needs_ports = args.flash or args.erase
    if needs_ports and not args.ports:
        sys.exit("ERROR: -p/--ports required when using -f/--flash or -e/--erase")

    if args.ports and not needs_ports:
        sys.exit("ERROR: -p/--ports given but no -f/--flash or -e/--erase specified")

    # ── Expand 'all' ──
    build_targets = expand_targets(args.build, BUILD_ALL)
    flash_targets = expand_targets(args.flash, FLASH_ALL)
    erase_all = args.erase and 'all' in args.erase
    erase_targets = set() if erase_all else expand_targets(args.erase, set())

    # ── Setup ──
    log_path = _setup_logging()
    print(f">> LOG: {log_path}")

    cfg = load_config()
    env_name = resolve_env(args.env)
    build_dir = PROJECT_DIR / ".pio" / "build" / env_name

    # ── Parse ports ──
    baud_groups = None
    all_ports = []
    if args.ports:
        baud_groups, all_ports = parse_port_specs(args.ports, cfg["baud"])
        print(f">> PORTS: {', '.join(args.ports)}")

    total_passed = 0
    total_failed = 0

    # ── Phase 1: Build ──
    if 'app' in build_targets:
        build_app(cfg, args.env)
    if 'fs' in build_targets:
        build_filesystem(cfg, args.env)

    # ── Resolve flash layout (needed for erase and flash) ──
    layout = None
    if flash_targets or erase_targets or erase_all:
        layout = resolve_flash_layout(build_dir, env_name)

        # Validate flash files exist
        for target in flash_targets:
            if target not in layout:
                sys.exit(f"ERROR: No flash layout for target '{target}'")
            for offset, filepath in layout[target].get('files', []):
                if not Path(filepath).exists():
                    sys.exit(f"ERROR: File not found: {filepath} (target '{target}')")

    # ── Phase 2: Erase ──
    if erase_all:
        p, f = run_erase_flash(baud_groups)
        total_passed += p
        total_failed += f
        if f:
            sys.exit(f"ERROR: Full erase failed on {f} port(s), aborting.")
    elif erase_targets:
        for target in erase_targets:
            if target not in layout or not layout[target].get('erase'):
                sys.exit(f"ERROR: No partition info for erase target '{target}'")
            offset, size = layout[target]['erase']
            p, f = run_erase_region(baud_groups, offset, size, label=target)
            total_passed += p
            total_failed += f
            if f:
                sys.exit(f"ERROR: Erase '{target}' failed on {f} port(s), aborting.")

    # ── Phase 3: Flash ──
    if flash_targets:
        # Collect all (offset, file) pairs from requested targets
        flash_pairs = []
        for target in ['boot', 'app', 'fs']:  # deterministic order
            if target in flash_targets:
                flash_pairs.extend(layout[target]['files'])

        p, f = run_write_flash(baud_groups, flash_pairs)
        total_passed += p
        total_failed += f

    # ── Summary ──
    if total_passed or total_failed:
        print(f"\n>> DONE: {total_passed} succeeded, {total_failed} failed")
        if total_failed:
            sys.exit(1)
    else:
        print("\n>> DONE: build only")


if __name__ == "__main__":
    main()
