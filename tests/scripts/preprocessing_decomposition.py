#!/usr/bin/env python3
"""Attribute encoder spectral distortion to preprocessing vs core coding.

Two independent measurements are produced per fixture/bitrate, all against the
same reference (the original waveform), so they are directly comparable:

  total   = metric( decode(encode(original)), original )        # production path
  bypass  = metric( decode(encode_raw(original)), original )    # custom conditioning disabled

``total - bypass`` is the net effect of the encoder's custom input conditioning
(high-pass, low-band blend, DC reject, error-balance gain); ``bypass`` is the
core codec fed the untouched samples. This is a causal split on one reference,
not a comparison against a different codec's preprocessed signal.

Two more numbers are descriptive only and are *not* additive and *not*
comparable to official's totals, because they use different references:

  pre     = metric( preprocessed, original )                    # conditioning alone
  coding  = metric( decode(encode(original)), preprocessed )    # codec error around preprocessed

``preprocessed`` is dumped as raw float32 frames from the encoder (build the
codec object with ``-DOPUSCPP_ENABLE_PREPROCESS_DUMP`` and set
``OPUSCPP_DUMP_PREPROCESS``). Official Opus does not expose its preprocessed
signal, so no equivalent official split is possible.
"""
from __future__ import annotations

import argparse
import array
import os
import pathlib
import re
import subprocess
import sys
import wave

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

ALIAS_NAMES = [
    "OpusEncoder",
    "OpusDecoder",
    "opus_encoder_create",
    "opus_encoder_destroy",
    "opus_encoder_ctl",
    "opus_encode",
    "opus_encode_float",
    "opus_decoder_create",
    "opus_decoder_destroy",
    "opus_decoder_ctl",
    "opus_decode",
    "opus_decode_float",
    "opus_packet_get_nb_samples",
    "opus_strerror",
]

CODEC_ENV_KEYS = ("OPUSCPP_DUMP_PREPROCESS", "OPUSCPP_BYPASS_PREPROCESS")


def alias_macros(prefix: str = "curr") -> list[str]:
    return [f"-D{name}={prefix}_{name}" for name in ALIAS_NAMES]


def clean_env(**overrides: str) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if key not in CODEC_ENV_KEYS}
    env.update(overrides)
    return env


def run(cmd: list[str], env: dict[str, str] | None = None) -> str:
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(cmd)}\n{result.stderr}")
    return result.stdout


def build_harness(cxx: str, work_dir: pathlib.Path, official_lib: pathlib.Path) -> pathlib.Path:
    work_dir.mkdir(parents=True, exist_ok=True)
    obj = work_dir / "curr_preprocess.o"
    exe = work_dir / ("preprocessing_decomposition.exe" if os.name == "nt" else "preprocessing_decomposition")
    run([cxx, "-std=c++23", "-O2", "-DNDEBUG", "-I", str(REPO_ROOT / "src"), "-DOPUSCPP_ENABLE_PREPROCESS_DUMP",
         *alias_macros(), "-c", str(REPO_ROOT / "src" / "opus_codec.cpp"), "-o", str(obj)])
    cmd = [cxx, "-std=c++23", "-O2", "-DNDEBUG", "-I", str(REPO_ROOT / "tests"),
           str(REPO_ROOT / "tests" / "perceptual_memory_validation.cpp"), str(obj), str(official_lib)]
    if os.name != "nt":
        cmd.append("-lm")
    run(cmd + ["-o", str(exe)])
    return exe


def parse_scores(text: str) -> dict[str, dict[str, float]]:
    scores: dict[str, dict[str, float]] = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not (stripped.startswith("current ") or stripped.startswith("official ")):
            continue
        key = stripped.split()[0]
        scores[key] = {name: float(value) for name, value in re.findall(r"([a-z_]+)=(\S+)", stripped)}
    return scores


def read_wav(path: pathlib.Path) -> tuple[int, int, int]:
    with wave.open(str(path), "rb") as wav:
        return wav.getframerate(), wav.getnchannels(), wav.getnframes()


def write_raw_as_wav(raw: pathlib.Path, wav_out: pathlib.Path, frames: int, frame_size: int) -> None:
    values = array.array("f")
    with raw.open("rb") as handle:
        values.frombytes(handle.read(frames * frame_size * 4))
    samples = array.array("h")
    for value in values[: frames * frame_size]:
        samples.append(max(-32768, min(32767, int(round(value * 32767.0)))))
    with wave.open(str(wav_out), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(48000)
        out.writeframes(samples.tobytes())


def spectral_contributions(current: dict[str, float], official: dict[str, float]) -> tuple[float, float]:
    corr = 1.4 * (current["logband_corr"] - official["logband_corr"])
    distance = 1.2 * (1.0 / (1.0 + current["logband_error"]) - 1.0 / (1.0 + official["logband_error"]))
    return corr, distance


def analyse(exe: pathlib.Path, work_dir: pathlib.Path, fixture: pathlib.Path, bitrates: list[int], frame_size: int = 960) -> list[dict]:
    _rate, channels, total_frames = read_wav(fixture)
    if channels != 1:
        raise RuntimeError(f"{fixture} is not mono")
    frames = total_frames // frame_size
    raw = work_dir / "preprocessed.raw"
    pre_wav = work_dir / "preprocessed.wav"
    rows: list[dict] = []
    for bitrate in bitrates:
        base = [str(exe), "--bitrate", str(bitrate), "--application", "voip", "--skip-memory"]
        total = parse_scores(run(base + ["--input", str(fixture)], env=clean_env(OPUSCPP_DUMP_PREPROCESS=str(raw))))
        if not raw.exists():
            raise RuntimeError("preprocess dump missing; harness was not built with OPUSCPP_ENABLE_PREPROCESS_DUMP")
        bypass = parse_scores(run(base + ["--input", str(fixture)], env=clean_env(OPUSCPP_BYPASS_PREPROCESS="1")))
        write_raw_as_wav(raw, pre_wav, frames, frame_size)
        coding = parse_scores(run(base + ["--input", str(fixture), "--reference", str(pre_wav)]))
        pre = parse_scores(run([str(exe), "--input", str(pre_wav), "--reference", str(fixture), "--identity", "--skip-memory"]))
        raw.unlink()
        corr, distance = spectral_contributions(total["current"], total["official"])
        rows.append({
            "fixture": fixture.stem,
            "bitrate": bitrate,
            "total_visqol_delta": total["current"]["visqol_style"] - total["official"]["visqol_style"],
            "corr_contrib": corr,
            "spec_contrib": distance,
            "current_total_logband_error": total["current"]["logband_error"],
            "official_total_logband_error": total["official"]["logband_error"],
            "bypassed_logband_error": bypass["current"]["logband_error"],
            "conditioning_effect_logband_error": total["current"]["logband_error"] - bypass["current"]["logband_error"],
            "conditioning_effect_visqol": total["current"]["visqol_style"] - bypass["current"]["visqol_style"],
            "bypass_visqol_delta": bypass["current"]["visqol_style"] - bypass["official"]["visqol_style"],
            "preprocessing_only_logband_error": pre["current"]["logband_error"],
            "coding_relative_logband_error": coding["current"]["logband_error"],
        })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--harness", type=pathlib.Path)
    parser.add_argument("--official-lib", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "official_opus_o2_intrinsics_mingw" / "libopus.a")
    parser.add_argument("--work-dir", type=pathlib.Path, default=REPO_ROOT / "build" / "_invest")
    parser.add_argument("--fixtures", nargs="+", type=pathlib.Path)
    parser.add_argument("--bitrates", nargs="+", type=int, default=[16000, 24000, 32000, 48000, 64000])
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()

    fixtures = args.fixtures or [
        REPO_ROOT / "build" / "official_compare_report" / "generated_audio" / "synthetic_voice_like_mono.wav",
        *sorted((REPO_ROOT / "build" / "tier_quality" / "selector_holdout").glob("*.wav")),
    ]
    harness = args.harness or build_harness(args.cxx, args.work_dir, args.official_lib)
    rows: list[dict] = []
    for fixture in fixtures:
        if fixture.exists():
            rows.extend(analyse(harness, args.work_dir, fixture, args.bitrates))

    header = ("fixture", "bitrate", "total_visqol_delta", "corr_contrib", "spec_contrib",
              "current_total_logband_error", "official_total_logband_error",
              "bypassed_logband_error", "conditioning_effect_logband_error", "conditioning_effect_visqol",
              "bypass_visqol_delta", "preprocessing_only_logband_error", "coding_relative_logband_error")
    print(",".join(header))
    for row in rows:
        print(",".join(f"{row[key]:+.5f}" if isinstance(row[key], float) else str(row[key]) for key in header))
    if args.out:
        args.out.write_text(",".join(header) + "\n" + "\n".join(
            ",".join(f"{row[key]:+.5f}" if isinstance(row[key], float) else str(row[key]) for key in header) for row in rows
        ) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
