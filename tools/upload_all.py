#!/usr/bin/env python3
"""Build once, flash many — parallel ESP32 upload tool."""

import argparse
import configparser
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
CHIP = "esp32c6"

# Lock for interleaved line output so port prefixes don't collide
_print_lock = threading.Lock()


def load_config():
    if not CONFIG_PATH.exists():
        sys.exit(f"ERROR: Config file not found: {CONFIG_PATH}")
    with open(CONFIG_PATH) as f:
        cfg = json.load(f)
    for key in ("platformio", "python"):
        p = cfg.get(key)
        if not p:
            sys.exit(f"ERROR: '{key}' not set in {CONFIG_PATH}")
        if not Path(p).exists():
            sys.exit(f"ERROR: {key} path does not exist: {p}")
    cfg.setdefault("baud", 460800)
    return cfg


def resolve_env(env_arg):
    """Return the PlatformIO environment name."""
    if env_arg:
        return env_arg
    ini_path = PROJECT_DIR / "platformio.ini"
    if not ini_path.exists():
        sys.exit(f"ERROR: platformio.ini not found at {ini_path}")
    cp = configparser.ConfigParser()
    cp.read(str(ini_path))
    default = cp.get("platformio", "default_envs", fallback=None)
    if not default:
        sys.exit("ERROR: No -env given and no default_envs in platformio.ini")
    return default.strip()


def build(cfg, env_arg):
    cmd = [cfg["platformio"], "run"]
    if env_arg:
        cmd += ["-e", env_arg]
    print(f">> BUILD: {' '.join(cmd)}")
    rc = subprocess.run(cmd, cwd=str(PROJECT_DIR)).returncode
    if rc != 0:
        sys.exit(f"ERROR: Build failed (exit {rc})")


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


def flash_port(cfg, port, firmware, bootloader, partitions, full):
    """Flash a single port (streams output live). Returns (port, success, error)."""
    cmd = [
        cfg["python"], "-X", "utf8", "-m", "esptool",
        "--chip", CHIP,
        "--port", port,
        "--baud", str(cfg["baud"]),
        "write_flash",
    ]
    if full:
        cmd += ["0x0", str(bootloader), "0x9000", str(partitions)]
    cmd += ["0x10000", str(firmware)]

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


def main():
    parser = argparse.ArgumentParser(
        description="Build once, flash many ESP32 boards in parallel."
    )
    parser.add_argument(
        "-env", dest="env", default=None,
        help="PlatformIO environment (default: from platformio.ini default_envs)"
    )
    parser.add_argument(
        "-full", action="store_true",
        help="Flash bootloader + partitions + app (not just app)"
    )
    parser.add_argument(
        "-nobuild", action="store_true",
        help="Skip build step, flash existing firmware"
    )
    parser.add_argument(
        "ports", nargs="+", metavar="COMx",
        help="Serial ports to flash"
    )
    args = parser.parse_args()

    cfg = load_config()
    env_name = resolve_env(args.env)

    # Build
    if not args.nobuild:
        build(cfg, args.env)
    else:
        print(">> BUILD: skipped (-nobuild)")

    # Resolve firmware paths
    build_dir = PROJECT_DIR / ".pio" / "build" / env_name
    firmware = build_dir / "firmware.bin"
    if not firmware.exists():
        sys.exit(f"ERROR: Firmware not found: {firmware}")

    bootloader = partitions = None
    if args.full:
        bootloader = build_dir / "bootloader.bin"
        partitions = build_dir / "partitions.bin"
        for p in (bootloader, partitions):
            if not p.exists():
                sys.exit(f"ERROR: Required for -full but not found: {p}")

    # Flash in parallel
    print(f"\n>> FLASHING {len(args.ports)} port(s): {', '.join(args.ports)}")
    results = []
    with ThreadPoolExecutor(max_workers=len(args.ports)) as pool:
        futures = {
            pool.submit(
                flash_port, cfg, port, firmware, bootloader, partitions, args.full
            ): port
            for port in args.ports
        }
        for future in as_completed(futures):
            port, ok, msg = future.result()
            status = "SUCCESS" if ok else "FAILED"
            print(f"  [{status}] {port}: {msg}")
            results.append((port, ok, msg))

    # Summary
    passed = sum(1 for _, ok, _ in results if ok)
    failed = len(results) - passed
    print(f"\n>> DONE: {passed} succeeded, {failed} failed")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
