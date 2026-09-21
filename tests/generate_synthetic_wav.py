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


def synth_voice_like(seconds: float, channels: int, breath_snr_db: float = 30.0) -> list[int]:
    frames = int(seconds * SAMPLE_RATE)
    out: list[int] = []
    rng = random.Random(1)

    formants = ((700.0, 80.0), (1220.0, 90.0), (2600.0, 130.0))
    resonators: list[tuple[float, float, float]] = []
    for freq, bandwidth in formants:
        radius = math.exp(-math.pi * bandwidth / SAMPLE_RATE)
        angle = 2.0 * math.pi * freq / SAMPLE_RATE
        resonators.append((2.0 * radius * math.cos(angle), -radius * radius, 1.0 - radius))
    states = [[0.0, 0.0] for _ in resonators]

    phase = 0.0
    radiated_previous = 0.0
    voiced: list[float] = []
    for i in range(frames):
        t = i / SAMPLE_RATE
        f0 = 120.0 + 18.0 * math.sin(2.0 * math.pi * 1.3 * t) + 4.0 * math.sin(2.0 * math.pi * 0.37 * t)
        phase += f0 / SAMPLE_RATE
        excitation = 0.0
        if phase >= 1.0:
            phase -= 1.0
            excitation = 1.0
        value = excitation
        for index, (a1, a2, gain) in enumerate(resonators):
            state = states[index]
            value = gain * value + a1 * state[0] + a2 * state[1]
            state[1] = state[0]
            state[0] = value
        radiated = value - radiated_previous
        radiated_previous = value
        envelope = 0.25 + 0.75 * max(0.0, math.sin(2.0 * math.pi * 2.6 * t)) ** 0.7
        voiced.append(envelope * radiated)

    voiced_rms = math.sqrt(sum(value * value for value in voiced) / len(voiced)) or 1.0
    voiced = [value / voiced_rms for value in voiced]

    breath = 0.0
    noise: list[float] = []
    for _ in range(frames):
        breath = 0.85 * breath + 0.15 * (rng.random() * 2.0 - 1.0)
        noise.append(breath)
    noise_rms = math.sqrt(sum(value * value for value in noise) / len(noise)) or 1.0
    noise_scale = 10.0 ** (-breath_snr_db / 20.0) / noise_rms

    mixed = [value + noise_scale * tone for value, tone in zip(voiced, noise)]
    peak = max(abs(value) for value in mixed) or 1.0
    scale = 21000.0 / peak
    for value in mixed:
        sample = max(-32768, min(32767, round(value * scale)))
        for c in range(channels):
            out.append(sample if channels == 1 else round(sample * (0.95 if c == 0 else 0.80)))
    return out


def synth_tonal_stress(seconds: float, channels: int) -> list[int]:
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


def add_white_noise(samples: list[int], snr_db: float) -> list[int]:
    rng = random.Random(2)
    noise = [rng.random() * 2.0 - 1.0 for _ in samples]
    signal_rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
    noise_rms = math.sqrt(sum(sample * sample for sample in noise) / len(noise))
    scale = signal_rms / (noise_rms * 10.0 ** (snr_db / 20.0))
    return [max(-32768, min(32767, round(sample + scale * background))) for sample, background in zip(samples, noise)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="tests/generated_audio", help="Output directory")
    parser.add_argument("--seconds", type=float, default=5.0)
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    voice = synth_voice_like(args.seconds, 1)
    write_wav(out / "synthetic_voice_like_mono.wav", 1, voice)
    write_wav(out / "synthetic_voice_like_mono_noisy.wav", 1, add_white_noise(voice, 6.0))
    write_wav(out / "synthetic_tonal_stress_mono.wav", 1, synth_tonal_stress(args.seconds, 1))
    write_wav(out / "synthetic_music_like_stereo.wav", 2, synth_music_like(args.seconds, 2))
    print(f"wrote synthetic WAVs under {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
