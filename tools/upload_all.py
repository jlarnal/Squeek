#!/usr/bin/env python3
"""Build once, flash many — parallel ESP32 upload tool.

All chip types, flash offsets, and file paths are derived automatically
from platformio.ini and PlatformIO build outputs — zero hardcoded constants.
"""

import argparse
import configparser
import csv
import json
import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
CONFIG_PATH = SCRIPT_DIR / "upload_all.config.json"

# Lock for interleaved line output so port prefixes don't collide
_print_lock = threading.Lock()


def load_config():
    if not CONFIG_PATH.exists():
        sys.exit(f"ERROR: Config file not found: {CONFIG_PATH}")
    with open(CONFIG_PATH) as f:
        cfg = json.load(f)
    for key in ("platformio", "esptool"):
        p = cfg.get(key)
        if not p:
            sys.exit(f"ERROR: '{key}' not set in {CONFIG_PATH}")
        cfg[key] = str(Path(os.path.expandvars(p)).expanduser())
        if not Path(cfg[key]).exists():
            sys.exit(f"ERROR: {key} path does not exist: {cfg[key]}")
    cfg.setdefault("baud", 460800)
    return cfg


def _read_ini():
    """Read and return the parsed platformio.ini ConfigParser."""
    ini_path = PROJECT_DIR / "platformio.ini"
    if not ini_path.exists():
        sys.exit(f"ERROR: platformio.ini not found at {ini_path}")
    cp = configparser.ConfigParser()
    cp.read(str(ini_path))
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


def resolve_chip(cfg, env_name):
    """Derive the esptool chip name from the board's MCU via `pio boards`."""
    cp = _read_ini()
    env_section = f"env:{env_name}"
    board = None
    if cp.has_option(env_section, "board"):
        board = cp.get(env_section, "board").strip()
    elif cp.has_option("env", "board"):
        board = cp.get("env", "board").strip()
    if not board:
        sys.exit(f"ERROR: No 'board' found in platformio.ini for env '{env_name}'")

    cmd = [cfg["platformio"], "boards", board, "--json-output"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True,
                                cwd=str(PROJECT_DIR))
        data = json.loads(result.stdout)
        # pio boards <exact> returns a list with one entry
        if isinstance(data, list):
            data = data[0]
        mcu = data.get("mcu", "")
        if not mcu:
            sys.exit(f"ERROR: No MCU field in board '{board}' JSON")
        chip = mcu.lower()
        print(f">> CHIP: {chip} (from board '{board}')")
        return chip
    except subprocess.CalledProcessError as e:
        sys.exit(f"ERROR: Failed to query board '{board}': {e.stderr}")
    except (json.JSONDecodeError, KeyError, IndexError) as e:
        sys.exit(f"ERROR: Failed to parse board JSON for '{board}': {e}")


def _parse_flash_offset(filepath):
    """Extract the flash offset from a PlatformIO *-flash_args file.

    Skips lines starting with '--'. Returns the offset string (e.g. '0x0').
    """
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


def get_storage_offset(env_name):
    """Return the offset (int) of the 'storage' partition from partitions CSV."""
    csv_path = _resolve_partitions_csv(env_name)
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#"):
                continue
            name = row[0].strip()
            if name == "storage":
                return int(row[3].strip(), 0)
    sys.exit("ERROR: No 'storage' partition found in partitions CSV")


def resolve_flash_files(build_dir, full, upload_image, env_name):
    """Build the list of (offset, filepath) pairs for write_flash.

    Offsets come from PlatformIO's *-flash_args files (derived from ESP-IDF).
    Binary paths use PIO's canonical names in the build root (bootloader.bin,
    partitions.bin, firmware.bin) since ESP-IDF's flash_args reference internal
    sub-build paths that PIO doesn't preserve.
    """
    pairs = []

    if full:
        bl_offset = _parse_flash_offset(build_dir / "bootloader-flash_args")
        pairs.append((bl_offset, str(build_dir / "bootloader.bin")))

        pt_offset = _parse_flash_offset(build_dir / "partition-table-flash_args")
        pairs.append((pt_offset, str(build_dir / "partitions.bin")))

    app_offset = _parse_flash_offset(build_dir / "app-flash_args")
    pairs.append((app_offset, str(build_dir / "firmware.bin")))

    if upload_image:
        fs_offset = get_storage_offset(env_name)
        fs_image = build_dir / "littlefs.bin"
        if not fs_image.exists():
            sys.exit(f"ERROR: Filesystem image not found: {fs_image}\n"
                     f"       Use --filesystem to build it first.")
        pairs.append((hex(fs_offset), str(fs_image)))

    # Validate all files exist
    for offset, filepath in pairs:
        if not Path(filepath).exists():
            sys.exit(f"ERROR: Flash file not found: {filepath} (at offset {offset})")

    return pairs


def build(cfg, env_arg):
    cmd = [cfg["platformio"], "run"]
    if env_arg:
        cmd += ["-e", env_arg]
    print(f">> BUILD: {' '.join(cmd)}")
    rc = subprocess.run(cmd, cwd=str(PROJECT_DIR)).returncode
    if rc != 0:
        sys.exit(f"ERROR: Build failed (exit {rc})")


def build_filesystem(cfg, env_arg):
    """Build the LittleFS image from ./data using PlatformIO."""
    cmd = [cfg["platformio"], "run", "--target", "buildfs"]
    if env_arg:
        cmd += ["-e", env_arg]
    print(f">> BUILDFS: {' '.join(cmd)}")
    rc = subprocess.run(cmd, cwd=str(PROJECT_DIR)).returncode
    if rc != 0:
        sys.exit(f"ERROR: Filesystem build failed (exit {rc})")


def _stream_binary_pipe(pipe, prefix):
    """Read from a binary pipe, decode with replace, print with port prefix.

    Handles \\r-only lines (esptool progress bars) by splitting on both
    \\r and \\n so each update gets its own prefixed line.
    """
    buf = b""
    while True:
        chunk = pipe.read(256)
        if not chunk:
            break
        buf += chunk
        # Split on any line ending (\r\n, \n, or bare \r)
        while b"\r" in buf or b"\n" in buf:
            # Find earliest line break
            cr = buf.find(b"\r")
            lf = buf.find(b"\n")
            if cr >= 0 and (lf < 0 or cr < lf):
                line = buf[:cr]
                # Consume \r\n as one break
                if cr + 1 < len(buf) and buf[cr + 1:cr + 2] == b"\n":
                    buf = buf[cr + 2:]
                else:
                    buf = buf[cr + 1:]
            else:
                line = buf[:lf]
                buf = buf[lf + 1:]
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                with _print_lock:
                    print(f"  [{prefix}] {text}", flush=True)
    # Flush remainder
    text = buf.decode("utf-8", errors="replace").strip()
    if text:
        with _print_lock:
            print(f"  [{prefix}] {text}", flush=True)


def _run_esptool(cfg, port, chip, esptool_args):
    """Run an esptool command, stream output. Returns (port, success, error)."""
    cmd = [
        cfg["esptool"],
        "--chip", chip,
        "--port", port,
        "--baud", str(cfg["baud"]),
    ] + esptool_args

    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        _stream_binary_pipe(proc.stdout, port)
        rc = proc.wait(timeout=120)
        if rc == 0:
            return (port, True, "OK")
        else:
            return (port, False, f"exit code {rc}")
    except subprocess.TimeoutExpired:
        proc.kill()
        return (port, False, "Timed out after 120s")
    except Exception as e:
        return (port, False, str(e))


def erase_port(cfg, port, chip):
    """Full chip erase on a single port."""
    return _run_esptool(cfg, port, chip, ["erase_flash"])


def flash_port(cfg, port, chip, flash_pairs):
    """Flash all offset+file pairs to a single port in one write_flash call."""
    esptool_args = ["write_flash"]
    for offset, filepath in flash_pairs:
        esptool_args += [offset, filepath]
    return _run_esptool(cfg, port, chip, esptool_args)


def _run_parallel(desc, func, ports, *args):
    """Run func(port, *args) in parallel across ports, print results, return counts."""
    print(f"\n>> {desc} {len(ports)} port(s): {', '.join(ports)}")
    results = []
    with ThreadPoolExecutor(max_workers=len(ports)) as pool:
        futures = {
            pool.submit(func, port, *args): port
            for port in ports
        }
        for future in as_completed(futures):
            port, ok, msg = future.result()
            status = "SUCCESS" if ok else "FAILED"
            print(f"  [{status}] {port}: {msg}")
            results.append((port, ok, msg))
    passed = sum(1 for _, ok, _ in results if ok)
    failed = len(results) - passed
    return passed, failed


def main():
    parser = argparse.ArgumentParser(
        description="Build once, flash many ESP32 boards in parallel."
    )
    parser.add_argument(
        "-v", "--environment", dest="env", default=None, metavar="ENV",
        help="PlatformIO environment (default: from platformio.ini default_envs)"
    )
    parser.add_argument(
        "-f", "--full", action="store_true",
        help="Full flash: build FS + build app + erase + flash everything"
    )
    parser.add_argument(
        "-n", "--nobuild", action="store_true",
        help="Skip build step, flash existing firmware"
    )
    parser.add_argument(
        "-e", "--erase", action="store_true",
        help="Erase entire flash before flashing"
    )
    parser.add_argument(
        "-fs", "--filesystem", action="store_true",
        help="Rebuild LittleFS image from ./data folder"
    )
    parser.add_argument(
        "-ui", "--upload-image", action="store_true",
        help="Upload LittleFS image to targets"
    )
    parser.add_argument(
        "ports", nargs="*", metavar="COMx",
        help="Serial ports to flash"
    )
    args = parser.parse_args()

    # Show help if no ports given
    if not args.ports:
        parser.print_help()
        sys.exit(1)

    # --full implies everything
    if args.full:
        args.erase = True
        args.filesystem = True
        args.upload_image = True

    cfg = load_config()
    env_name = resolve_env(args.env)
    build_dir = PROJECT_DIR / ".pio" / "build" / env_name

    # ── Build filesystem image ──
    if args.filesystem:
        data_dir = PROJECT_DIR / "data"
        if not data_dir.is_dir():
            sys.exit(f"ERROR: Data folder not found: {data_dir}")
        build_filesystem(cfg, args.env)

    # ── Build firmware ──
    if not args.nobuild:
        build(cfg, args.env)
    else:
        print(">> BUILD: skipped (--nobuild)")

    # ── Resolve chip type and flash files ──
    chip = resolve_chip(cfg, env_name)
    flash_pairs = resolve_flash_files(build_dir, args.full, args.upload_image, env_name)

    print(f">> FLASH PLAN ({len(flash_pairs)} file(s)):")
    for offset, filepath in flash_pairs:
        print(f"     {offset} -> {Path(filepath).name}")

    total_passed = 0
    total_failed = 0

    # ── Erase flash ──
    if args.erase:
        p, f = _run_parallel("ERASING",
                             lambda port: erase_port(cfg, port, chip),
                             args.ports)
        total_passed += p
        total_failed += f
        if f:
            sys.exit(f"ERROR: Erase failed on {f} port(s), aborting.")

    # ── Flash all files in one write_flash call ──
    p, f = _run_parallel("FLASHING",
                         lambda port: flash_port(cfg, port, chip, flash_pairs),
                         args.ports)
    total_passed += p
    total_failed += f

    # ── Summary ──
    print(f"\n>> DONE: {total_passed} succeeded, {total_failed} failed")
    if total_failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
