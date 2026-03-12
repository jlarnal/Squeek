#!/usr/bin/env python3
"""Gzip web assets from web/ into data/ for LittleFS upload.

Usage:  python tools/gzip_web.py
        (run from project root)

Files are gzipped into data/ with flat names (no subdirectories).
StorageManager serves *.gz transparently with Content-Encoding: gzip.
"""
import gzip
import os
import shutil
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(PROJECT_ROOT, "data")

# Source file -> LittleFS name mapping
# (source path relative to web/, target filename in data/)
ASSETS = {
    "dashboard/dashboard.html": "dashboard.html",
    "dashboard/dashboard.css":  "dashboard.css",
    "dashboard/dashboard.js":   "dashboard.js",
    "wizard/wizard.html":       "wizard.html",
}

def main():
    web_dir = os.path.join(PROJECT_ROOT, "web")
    os.makedirs(DATA_DIR, exist_ok=True)

    # Remove old .gz files that we manage
    managed = {v + ".gz" for v in ASSETS.values()}
    for f in os.listdir(DATA_DIR):
        if f in managed:
            os.remove(os.path.join(DATA_DIR, f))

    compressed = 0
    for src_rel, dst_name in ASSETS.items():
        src = os.path.join(web_dir, src_rel)
        if not os.path.isfile(src):
            print(f"  SKIP  {src_rel} (not found)")
            continue
        dst = os.path.join(DATA_DIR, dst_name + ".gz")
        with open(src, "rb") as f_in:
            raw = f_in.read()
        with gzip.open(dst, "wb", compresslevel=9) as f_out:
            f_out.write(raw)
        ratio = os.path.getsize(dst) / len(raw) * 100 if raw else 0
        print(f"  {dst_name:30s} {len(raw):>6d} -> {os.path.getsize(dst):>6d}  ({ratio:.0f}%)")
        compressed += 1

    print(f"\n  {compressed} files gzipped into {DATA_DIR}")

if __name__ == "__main__":
    main()
