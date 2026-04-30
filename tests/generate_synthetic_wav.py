#!/usr/bin/env python3
"""Generate small synthetic WAV samples for local listening tests.

These files are generated from math/noise at runtime so the repository does not need
to carry audio assets.
"""
from __future__ import annotations

import argparse
import math
import pathlib
import random
import struct
import wave

SAMPLE_RATE = 48_000


def write_wav(path: pathlib.Path, channels: int, samples: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(struct.pack("<" + "h" * len(samples), *samples))


def synth_voice_like(seconds: float, channels: int) -> list[int]:
    frames = int(seconds * SAMPLE_RATE)
    out: list[int] = []
    rng = random.Random(1)
    for i in range(frames):
        t = i / SAMPLE_RATE
        f0 = 120.0 + 20.0 * math.sin(2.0 * math.pi * 1.7 * t)
        env = 0.3 + 0.7 * max(0.0, math.sin(2.0 * math.pi * 3.2 * t))
        v = env * (0.70 * math.sin(2.0 * math.pi * f0 * t) + 0.25 * math.sin(2.0 * math.pi * 2.0 * f0 * t))
        v += 0.025 * (rng.random() * 2.0 - 1.0)
        sample = max(-32768, min(32767, round(v * 21000)))
        for c in range(channels):
            out.append(sample if channels == 1 else round(sample * (0.95 if c == 0 else 0.80)))
    return out


def synth_music_like(seconds: float, channels: int) -> list[int]:
    frames = int(seconds * SAMPLE_RATE)
    out: list[int] = []
    for i in range(frames):
        t = i / SAMPLE_RATE
        env = 0.65 + 0.35 * math.sin(2.0 * math.pi * 0.7 * t)
        left = env * (0.45 * math.sin(2.0 * math.pi * 196.0 * t) + 0.35 * math.sin(2.0 * math.pi * 293.66 * t) + 0.20 * math.sin(2.0 * math.pi * 587.33 * t))
        right = env * (0.45 * math.sin(2.0 * math.pi * 246.94 * t) + 0.35 * math.sin(2.0 * math.pi * 369.99 * t) + 0.20 * math.sin(2.0 * math.pi * 739.99 * t))
        if channels == 1:
            out.append(max(-32768, min(32767, round((left + right) * 0.5 * 22000))))
        else:
            out.extend([max(-32768, min(32767, round(left * 22000))), max(-32768, min(32767, round(right * 22000)))])
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="tests/generated_audio", help="Output directory")
    parser.add_argument("--seconds", type=float, default=5.0)
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    write_wav(out / "synthetic_voice_like_mono.wav", 1, synth_voice_like(args.seconds, 1))
    write_wav(out / "synthetic_music_like_stereo.wav", 2, synth_music_like(args.seconds, 2))
    print(f"wrote synthetic WAVs under {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
