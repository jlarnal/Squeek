#!/usr/bin/env python3
"""Resample MP3 files in a folder to a target sample rate using ffmpeg."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile


def get_sample_rate(filepath: str) -> int | None:
    """Return the sample rate of an audio file via ffprobe, or None on error."""
    try:
        result = subprocess.run(
            [
                "ffprobe", "-v", "quiet", "-print_format", "json",
                "-show_streams", filepath,
            ],
            capture_output=True, text=True, check=True,
        )
        info = json.loads(result.stdout)
        for stream in info.get("streams", []):
            if stream.get("codec_type") == "audio":
                return int(stream["sample_rate"])
    except (subprocess.CalledProcessError, KeyError, ValueError, FileNotFoundError):
        return None
    return None


def resample(src: str, dst: str, rate: int) -> bool:
    """Resample src to dst at the given rate. Returns True on success."""
    # When overwriting, write to a temp file first then replace atomically.
    overwrite = os.path.abspath(src) == os.path.abspath(dst)
    if overwrite:
        fd, tmp = tempfile.mkstemp(suffix=".mp3", dir=os.path.dirname(src))
        os.close(fd)
        target = tmp
    else:
        target = dst

    try:
        subprocess.run(
            [
                "ffmpeg", "-y", "-i", src,
                "-ar", str(rate),
                "-codec:a", "libmp3lame", "-q:a", "2",
                target,
            ],
            capture_output=True, text=True, check=True,
        )
    except subprocess.CalledProcessError as e:
        if overwrite and os.path.exists(tmp):
            os.remove(tmp)
        print(f"  ERROR: ffmpeg failed — {e.stderr.strip().splitlines()[-1]}")
        return False

    if overwrite:
        shutil.move(tmp, src)

    return True


def main():
    parser = argparse.ArgumentParser(description="Resample MP3 files to a target sample rate.")
    parser.add_argument("-t", "--target", required=True, help="Source folder containing MP3 files")
    parser.add_argument("-f", "--freq", type=int, default=32000, help="Target sample rate in Hz (default: 32000)")
    parser.add_argument("-d", "--dest", default=None, help="Destination folder (default: overwrite in place)")
    args = parser.parse_args()

    src_dir = os.path.abspath(args.target)
    if not os.path.isdir(src_dir):
        sys.exit(f"Error: '{args.target}' is not a directory")

    if args.dest:
        dst_dir = os.path.abspath(args.dest)
        os.makedirs(dst_dir, exist_ok=True)
    else:
        dst_dir = None

    # Collect MP3 files (non-recursive)
    mp3s = sorted(f for f in os.listdir(src_dir) if f.lower().endswith(".mp3"))
    if not mp3s:
        sys.exit(f"No MP3 files found in '{src_dir}'")

    print(f"Found {len(mp3s)} MP3(s) — target rate: {args.freq} Hz")

    skipped = resampled = errors = 0
    for name in mp3s:
        src_path = os.path.join(src_dir, name)
        rate = get_sample_rate(src_path)
        if rate is None:
            print(f"  SKIP  {name} (could not read sample rate)")
            errors += 1
            continue

        if rate == args.freq:
            if dst_dir:
                # Copy as-is to dest
                shutil.copy2(src_path, os.path.join(dst_dir, name))
            print(f"  OK    {name} (already {rate} Hz)")
            skipped += 1
            continue

        dst_path = os.path.join(dst_dir, name) if dst_dir else src_path
        print(f"  CONV  {name} ({rate} -> {args.freq} Hz)", end="", flush=True)
        if resample(src_path, dst_path, args.freq):
            print(" — done")
            resampled += 1
        else:
            errors += 1

    print(f"\nDone: {resampled} resampled, {skipped} already OK, {errors} errors")


if __name__ == "__main__":
    main()
