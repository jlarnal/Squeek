#!/usr/bin/env python3
"""
Squeek Tone Simulator — Tkinter GUI that synthesizes tones matching
the ESP32 AudioEngine (200 Hz ISR tick, linear freq/duty interpolation).

Tone definitions are parsed directly from src/tone_data.inc (the same file
the C++ firmware #includes), so there is a single source of truth.

The .inc file uses a three-level structure:
  1. ToneSegment arrays   — raw waveform data
  2. ToneSequence arrays   — variation tables (one per group)
  3. ToneGroup table       — public-facing group names

All audio buffers are precomputed at startup for every waveform × variation
combination, so playback is instant on click.

Usage:  python tools/tone_sim.py [path/to/tone_data.inc]
        Defaults to src/tone_data.inc relative to the project root.
"""

import random
import re
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Tuple

import numpy as np
import sounddevice as sd
import tkinter as tk
from tkinter import ttk

# ---------------------------------------------------------------------------
# Data structures (mirrors include/tone_library.h)
# ---------------------------------------------------------------------------

SAMPLE_RATE = 44100
TICK_HZ = 200          # ISR tick rate on firmware
TICK_S = 1.0 / TICK_HZ # 5 ms


@dataclass
class ToneSegment:
    freq_start_hz: int
    freq_end_hz: int
    duty_start: int       # 0-255
    duty_end: int         # 0-255
    duration_ms: int


@dataclass
class ToneSequence:
    segments: List[ToneSegment]
    repeats: int = 0      # 0 = play once, 255 = loop forever


@dataclass
class ToneGroup:
    name: str
    variations: List[ToneSequence] = field(default_factory=list)
    var_names: List[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# .inc parser — reads the C source shared with the firmware
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_INC = SCRIPT_DIR.parent / "src" / "tone_data.inc"

# Matches: static const ToneSegment s_xyz_segs[] = { ... };
_RE_SEG_ARRAY = re.compile(
    r"static\s+const\s+ToneSegment\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
# Matches one brace-enclosed tuple: { 1000, 4000, 200, 200, 150 }
_RE_SEG_TUPLE = re.compile(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")

# Matches: static const ToneSequence s_xxx_vars[] = { ... };
_RE_VAR_ARRAY = re.compile(
    r"static\s+const\s+ToneSequence\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
# Matches one variation entry: { s_xxx_segs, count, repeats }
_RE_VAR_ENTRY = re.compile(r"\{\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")

# Matches one group entry: { "name", s_xxx_vars, count }
_RE_GROUP_ENTRY = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\d+)\s*\}'
)


def load_tones(path: Path) -> List[ToneGroup]:
    """Parse tone definitions from a C .inc file."""
    text = path.read_text(encoding="utf-8")

    # 1) Parse all segment arrays into a dict keyed by C identifier
    seg_arrays: Dict[str, List[ToneSegment]] = {}
    for m in _RE_SEG_ARRAY.finditer(text):
        ident = m.group(1)
        body = m.group(2)
        segs = [
            ToneSegment(int(t[0]), int(t[1]), int(t[2]), int(t[3]), int(t[4]))
            for t in _RE_SEG_TUPLE.findall(body)
        ]
        seg_arrays[ident] = segs

    # 2) Parse variation arrays into a dict keyed by C identifier
    var_arrays: Dict[str, List[tuple]] = {}  # ident -> [(seg_ident, count, repeats)]
    for m in _RE_VAR_ARRAY.finditer(text):
        ident = m.group(1)
        body = m.group(2)
        entries = []
        for vm in _RE_VAR_ENTRY.finditer(body):
            entries.append((vm.group(1), int(vm.group(2)), int(vm.group(3))))
        var_arrays[ident] = entries

    # 3) Parse the s_groups[] table
    groups: List[ToneGroup] = []
    for m in _RE_GROUP_ENTRY.finditer(text):
        name = m.group(1)
        var_ident = m.group(2)
        _count = int(m.group(3))

        group = ToneGroup(name=name)
        if var_ident in var_arrays:
            for seg_ident, seg_count, repeats in var_arrays[var_ident]:
                if seg_ident in seg_arrays:
                    group.variations.append(
                        ToneSequence(seg_arrays[seg_ident], repeats)
                    )
                    # Derive a readable name from the segment array identifier
                    # e.g. "s_scratch_fast_segs" -> "scratch_fast"
                    vname = seg_ident
                    if vname.startswith("s_"):
                        vname = vname[2:]
                    if vname.endswith("_segs"):
                        vname = vname[:-5]
                    group.var_names.append(vname)
                else:
                    print(f"Warning: segment array '{seg_ident}' not found "
                          f"for group '{name}'", file=sys.stderr)
        else:
            print(f"Warning: variation array '{var_ident}' not found "
                  f"for group '{name}'", file=sys.stderr)

        if group.variations:
            groups.append(group)

    return groups

# ---------------------------------------------------------------------------
# Waveform generators
# ---------------------------------------------------------------------------

def _wave_sine(phi: np.ndarray) -> np.ndarray:
    return np.sin(phi)


def _wave_square(phi: np.ndarray) -> np.ndarray:
    """Square wave via sign of sine — matches LEDC PWM output."""
    return np.sign(np.sin(phi))


def _wave_triangle(phi: np.ndarray) -> np.ndarray:
    """Triangle wave: linear ramps between -1 and +1."""
    return 2.0 * np.abs(2.0 * (phi / (2.0 * np.pi) % 1.0) - 1.0) - 1.0


WAVEFORMS = {
    "Square (LEDC actual)": _wave_square,
    "Sine (filtered)":      _wave_sine,
    "Triangle":             _wave_triangle,
}

# ---------------------------------------------------------------------------
# Synthesis — matches firmware AudioEngine ISR math exactly
# ---------------------------------------------------------------------------

def synthesize_segment(
    seg: ToneSegment, phase: float, wave_fn: Callable
) -> Tuple[np.ndarray, float]:
    """Render one ToneSegment to audio samples.

    Uses the same 200 Hz tick quantization and integer linear interpolation
    as the firmware ISR.  Returns (samples, carry_phase).
    """
    seg_ticks = (seg.duration_ms * TICK_HZ) // 1000
    if seg_ticks == 0:
        return np.array([], dtype=np.float32), phase

    samples_per_tick = int(TICK_S * SAMPLE_RATE)
    out = np.empty(seg_ticks * samples_per_tick, dtype=np.float32)

    pos = 0
    for tick in range(seg_ticks):
        # --- integer math matching audio_engine.cpp ---
        if seg.freq_start_hz == 0 and seg.freq_end_hz == 0:
            freq = 0
            amp = 0.0
        else:
            freq = ((seg.freq_start_hz * (seg_ticks - tick))
                    + (seg.freq_end_hz * tick)) // seg_ticks
            duty = ((seg.duty_start * (seg_ticks - tick))
                    + (seg.duty_end * tick)) // seg_ticks
            amp = duty / 255.0

        n = samples_per_tick
        if freq == 0 or amp == 0.0:
            out[pos:pos + n] = 0.0
        else:
            t = np.arange(n, dtype=np.float64) / SAMPLE_RATE
            phi = 2.0 * np.pi * freq * t + phase
            chunk = (amp * wave_fn(phi)).astype(np.float32)
            phase = (phase + 2.0 * np.pi * freq * n / SAMPLE_RATE) % (2.0 * np.pi)
            out[pos:pos + n] = chunk
        pos += n

    return out[:pos], phase


def synthesize_sequence(seq: ToneSequence, wave_fn: Callable) -> np.ndarray:
    """Render a full ToneSequence (with repeats) to audio samples."""
    plays = 1 + seq.repeats if seq.repeats < 255 else 4   # cap infinite loops
    parts: list[np.ndarray] = []
    phase = 0.0
    for _ in range(plays):
        for seg in seq.segments:
            samples, phase = synthesize_segment(seg, phase, wave_fn)
            parts.append(samples)
    if not parts:
        return np.array([], dtype=np.float32)
    return np.concatenate(parts)


# ---------------------------------------------------------------------------
# Audio cache — precomputed buffers for every (waveform, group, variation)
# ---------------------------------------------------------------------------

# cache[(wave_name, group_idx, var_idx)] -> np.ndarray
AudioCache = Dict[Tuple[str, int, int], np.ndarray]


def precompute_cache(
    groups: List[ToneGroup],
    progress_cb: Callable[[int, int], None] | None = None,
) -> AudioCache:
    """Precompute all audio buffers for every waveform × variation."""
    cache: AudioCache = {}
    total = len(WAVEFORMS) * sum(len(g.variations) for g in groups)
    done = 0
    for wave_name, wave_fn in WAVEFORMS.items():
        for gi, group in enumerate(groups):
            for vi, seq in enumerate(group.variations):
                cache[(wave_name, gi, vi)] = synthesize_sequence(seq, wave_fn)
                done += 1
                if progress_cb:
                    progress_cb(done, total)
    return cache


# ---------------------------------------------------------------------------
# Playback helpers
# ---------------------------------------------------------------------------

_play_lock = threading.Lock()
_stop_flag = threading.Event()


def play_audio(audio: np.ndarray) -> None:
    """Play a precomputed audio buffer in the calling thread."""
    _stop_flag.clear()
    if audio.size == 0:
        return
    with _play_lock:
        try:
            sd.play(audio, samplerate=SAMPLE_RATE)
            stream = sd.get_stream()
            while stream and stream.active and not _stop_flag.is_set():
                _stop_flag.wait(timeout=0.05)
            if _stop_flag.is_set():
                sd.stop()
        except sd.PortAudioError:
            pass


def stop_playback() -> None:
    """Stop any in-progress playback immediately."""
    _stop_flag.set()
    sd.stop()


# ---------------------------------------------------------------------------
# Tkinter GUI
# ---------------------------------------------------------------------------

CARD_COLUMNS = 3       # cards per row in the grid
VAR_COLUMNS = 4        # variation buttons per row inside a card


def show_progress(root: tk.Tk, groups: List[ToneGroup]) -> AudioCache:
    """Show a progress window while precomputing all audio buffers."""
    splash = tk.Toplevel(root)
    splash.title("Precomputing audio...")
    splash.resizable(False, False)
    splash.transient(root)
    splash.grab_set()

    total = len(WAVEFORMS) * sum(len(g.variations) for g in groups)

    frame = ttk.Frame(splash, padding=20)
    frame.pack()
    label = ttk.Label(frame, text="Rendering tones...", font=("Segoe UI", 10))
    label.pack(pady=(0, 8))
    progress = ttk.Progressbar(frame, length=320, maximum=total)
    progress.pack()
    detail = ttk.Label(frame, text=f"0 / {total}", font=("Segoe UI", 9))
    detail.pack(pady=(4, 0))

    # Center the splash on screen
    splash.update_idletasks()
    w, h = splash.winfo_width(), splash.winfo_height()
    x = (splash.winfo_screenwidth() - w) // 2
    y = (splash.winfo_screenheight() - h) // 2
    splash.geometry(f"+{x}+{y}")

    cache: AudioCache = {}
    done = 0

    # Run synchronously on main thread — 198 buffers takes ~80ms.
    # Periodic root.update() keeps the progress bar painting.
    for wave_name, wave_fn in WAVEFORMS.items():
        for gi, group in enumerate(groups):
            for vi, seq in enumerate(group.variations):
                cache[(wave_name, gi, vi)] = synthesize_sequence(seq, wave_fn)
                done += 1
                progress["value"] = done
                detail.config(text=f"{done} / {total}")
                if done % 10 == 0:
                    root.update()

    splash.destroy()
    return cache


def build_gui(groups: List[ToneGroup]) -> None:
    root = tk.Tk()
    root.title("Squeek Tone Simulator")
    root.withdraw()  # hide until ready

    # --- precompute all audio ---
    cache = show_progress(root, groups)

    # --- state ---
    wave_names = list(WAVEFORMS.keys())
    current_wave = tk.StringVar(value=wave_names[0])

    def get_audio(gi: int, vi: int) -> np.ndarray:
        return cache[(current_wave.get(), gi, vi)]

    def on_play_var(gi: int, vi: int) -> None:
        stop_playback()
        audio = get_audio(gi, vi)
        threading.Thread(target=play_audio, args=(audio,), daemon=True).start()

    def on_play_random(gi: int, var_count: int) -> None:
        vi = random.randint(0, var_count - 1)
        on_play_var(gi, vi)

    # --- styles ---
    style = ttk.Style()
    style.configure("Header.TLabel", font=("Segoe UI", 13, "bold"))
    style.configure("Card.TLabelframe.Label", font=("Segoe UI", 10, "bold"))
    style.configure("Random.TButton", font=("Segoe UI", 10, "bold"), padding=5)
    style.configure("Var.TButton", font=("Segoe UI", 8), padding=3)
    style.configure("Stop.TButton", font=("Segoe UI", 11, "bold"), padding=6)

    # --- main layout ---
    outer = ttk.Frame(root, padding=12)
    outer.pack(fill="both", expand=True)

    # Title row
    ttk.Label(outer, text="Squeek Tone Simulator", style="Header.TLabel") \
        .grid(row=0, column=0, columnspan=CARD_COLUMNS, pady=(0, 4))

    # Waveform selector row
    wave_frame = ttk.Frame(outer)
    wave_frame.grid(row=1, column=0, columnspan=CARD_COLUMNS, pady=(0, 10))
    ttk.Label(wave_frame, text="Waveform:", font=("Segoe UI", 9)).pack(side="left")
    wave_combo = ttk.Combobox(
        wave_frame, textvariable=current_wave, values=wave_names,
        state="readonly", width=22,
    )
    wave_combo.pack(side="left", padx=(6, 0))

    # --- group cards in a grid ---
    for gi, group in enumerate(groups):
        card_row = 2 + gi // CARD_COLUMNS
        card_col = gi % CARD_COLUMNS

        card = ttk.LabelFrame(outer, text=f"  {group.name}  ", style="Card.TLabelframe")
        card.grid(row=card_row, column=card_col, padx=4, pady=4, sticky="nsew")

        # "Play random" button spanning the top of the card
        rand_btn = ttk.Button(
            card,
            text=f"Play random  ({len(group.variations)} variations)",
            style="Random.TButton",
            command=lambda g=gi, n=len(group.variations): on_play_random(g, n),
        )
        rand_btn.grid(row=0, column=0, columnspan=VAR_COLUMNS,
                      sticky="ew", padx=4, pady=(4, 6))

        # Variation buttons in a sub-grid
        for vi, vname in enumerate(group.var_names):
            seg_count = len(group.variations[vi].segments)
            total_ms = sum(s.duration_ms for s in group.variations[vi].segments)
            label = f"{vname}\n{seg_count}seg {total_ms}ms"

            vbtn = ttk.Button(
                card, text=label, style="Var.TButton",
                command=lambda g=gi, v=vi: on_play_var(g, v),
            )
            vr = 1 + vi // VAR_COLUMNS
            vc = vi % VAR_COLUMNS
            vbtn.grid(row=vr, column=vc, sticky="ew", padx=2, pady=2)

        for c in range(VAR_COLUMNS):
            card.columnconfigure(c, weight=1)

    # Stop button spanning the bottom
    stop_row = 2 + (len(groups) - 1) // CARD_COLUMNS + 1
    stop_btn = ttk.Button(
        outer, text="Stop", style="Stop.TButton", command=stop_playback
    )
    stop_btn.grid(row=stop_row, column=0, columnspan=CARD_COLUMNS,
                  sticky="ew", pady=(8, 0))

    for c in range(CARD_COLUMNS):
        outer.columnconfigure(c, weight=1)

    root.deiconify()  # show now that everything is built
    root.mainloop()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    inc_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_INC
    if not inc_path.exists():
        print(f"Error: {inc_path} not found", file=sys.stderr)
        sys.exit(1)
    groups = load_tones(inc_path)
    if not groups:
        print(f"Error: no tone groups found in {inc_path}", file=sys.stderr)
        sys.exit(1)
    total_vars = sum(len(g.variations) for g in groups)
    print(f"Loaded {len(groups)} groups ({total_vars} variations) from {inc_path}")
    build_gui(groups)
