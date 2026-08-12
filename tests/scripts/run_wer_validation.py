#!/usr/bin/env python3
"""WER-style validation gate for speech-to-text oriented Opus tuning.

The script intentionally does not depend on a particular ASR provider. Pass an
ASR command with a ``{wav}`` placeholder; the command must print the recognized
text to stdout. This lets local runs use Azure, Whisper, Android Speech, or any
other recognizer without adding a cloud dependency to opuscpp.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import pathlib
import random
import re
import shutil
import struct
import subprocess
import sys
import wave
from dataclasses import dataclass
from typing import Iterable


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
NUMBER_WORDS = {
    "zero": 0,
    "one": 1,
    "two": 2,
    "three": 3,
    "four": 4,
    "five": 5,
    "six": 6,
    "seven": 7,
    "eight": 8,
    "nine": 9,
    "ten": 10,
    "eleven": 11,
    "twelve": 12,
    "thirteen": 13,
    "fourteen": 14,
    "fifteen": 15,
    "sixteen": 16,
    "seventeen": 17,
    "eighteen": 18,
    "nineteen": 19,
    "twenty": 20,
    "thirty": 30,
    "forty": 40,
    "fifty": 50,
    "sixty": 60,
    "seventy": 70,
    "eighty": 80,
    "ninety": 90,
}


def canonicalize_tokens(tokens: list[str]) -> list[str]:
    canonical: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in {"it's", "it"} and index + 1 < len(tokens) and tokens[index + 1] == "is":
            canonical.append("it's")
            index += 2
            continue
        if token in {"kilobit", "kilobits", "kbit", "kbits", "kb", "kbps"}:
            if index + 2 < len(tokens) and tokens[index + 1] == "per" and tokens[index + 2] == "second":
                index += 3
            else:
                index += 1
            canonical.append("kbps")
            continue
        first = NUMBER_WORDS.get(token)
        if first is not None:
            if index + 1 < len(tokens):
                second = NUMBER_WORDS.get(tokens[index + 1])
                if second is not None and first >= 20 and first % 10 == 0 and 0 < second < 10:
                    canonical.append(str(first + second))
                    index += 2
                    continue
            canonical.append(str(first))
            index += 1
            continue
        canonical.append(token)
        index += 1
    return canonical


@dataclass(frozen=True)
class Sample:
    sample_id: str
    wav: pathlib.Path
    transcript: str


@dataclass(frozen=True)
class Score:
    sample_id: str
    gain_db: str
    snr_db: str
    bitrate: int
    application: str
    postfilter: int
    reference: str
    hypothesis: str
    wer: float
    cer: float
    decoded_wav: str


def normalize_text(text: str) -> str:
    text = text.lower()
    text = re.sub(r"[^a-z0-9']+", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    return " ".join(canonicalize_tokens(text.split()))


def edit_distance(lhs: list[str] | str, rhs: list[str] | str) -> int:
    previous = list(range(len(rhs) + 1))
    for i, left in enumerate(lhs, 1):
        current = [i] + [0] * len(rhs)
        for j, right in enumerate(rhs, 1):
            current[j] = min(
                previous[j] + 1,
                current[j - 1] + 1,
                previous[j - 1] + (0 if left == right else 1),
            )
        previous = current
    return previous[-1]


def word_error_rate(reference: str, hypothesis: str) -> float:
    ref_words = normalize_text(reference).split()
    hyp_words = normalize_text(hypothesis).split()
    if not ref_words:
        return 0.0 if not hyp_words else 1.0
    return edit_distance(ref_words, hyp_words) / len(ref_words)


def char_error_rate(reference: str, hypothesis: str) -> float:
    ref = normalize_text(reference).replace(" ", "")
    hyp = normalize_text(hypothesis).replace(" ", "")
    if not ref:
        return 0.0 if not hyp else 1.0
    return edit_distance(ref, hyp) / len(ref)


def load_manifest(path: pathlib.Path) -> list[Sample]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    base = path.parent
    samples: list[Sample] = []
    for item in manifest.get("samples", []):
        wav = pathlib.Path(item["wav"])
        if not wav.is_absolute():
            wav = (base / wav).resolve()
        samples.append(Sample(str(item["id"]), wav, str(item["transcript"])))
    if not samples:
        raise ValueError(f"manifest contains no samples: {path}")
    return samples


def read_pcm16_wav(path: pathlib.Path) -> tuple[int, int, list[int]]:
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        rate = wav.getframerate()
        sample_width = wav.getsampwidth()
        frames = wav.readframes(wav.getnframes())
    if channels not in (1, 2) or rate != 48000 or sample_width != 2:
        raise ValueError(f"{path} must be 48 kHz mono/stereo PCM16 WAV")
    count = len(frames) // 2
    return rate, channels, list(struct.unpack(f"<{count}h", frames))


def write_pcm16_wav(path: pathlib.Path, rate: int, channels: int, samples: Iterable[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    values = [max(-32768, min(32767, int(v))) for v in samples]
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(struct.pack(f"<{len(values)}h", *values))


def make_conditioned_wav(source: pathlib.Path, output: pathlib.Path, gain_db: str, snr_db: str, seed: int) -> pathlib.Path:
    rate, channels, samples = read_pcm16_wav(source)
    gain = 10.0 ** (float(gain_db) / 20.0)
    gained = [sample * gain for sample in samples]
    if snr_db.lower() in {"clean", "none", "inf", "infinite"}:
        write_pcm16_wav(output, rate, channels, gained)
        return output

    target_snr = float(snr_db)
    signal_rms = math.sqrt(sum(float(v) * float(v) for v in gained) / max(1, len(gained)))
    if signal_rms <= 1e-9:
        noise_scale = 1.0
    else:
        noise_scale = signal_rms / (10.0 ** (target_snr / 20.0))
    rng = random.Random(seed)
    noisy = [sample + rng.gauss(0.0, noise_scale) for sample in gained]
    write_pcm16_wav(output, rate, channels, noisy)
    return output


def build_roundtrip(cxx: str, output_dir: pathlib.Path) -> pathlib.Path:
    exe = output_dir / ("wer_roundtrip.exe" if os.name == "nt" else "wer_roundtrip")
    source = REPO_ROOT / "tests" / "wer_roundtrip.cpp"
    codec = REPO_ROOT / "src" / "opus_codec.cpp"
    cmd = [
        cxx,
        "-std=c++23",
        "-O2",
        "-DNDEBUG",
        "-I",
        str(REPO_ROOT / "src"),
        str(source),
        str(codec),
        "-o",
        str(exe),
    ]
    subprocess.run(cmd, check=True)
    return exe


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_asr_cache(path: pathlib.Path | None) -> dict[str, str]:
    if path is None or not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return {str(key): str(value) for key, value in data.items()}


def save_asr_cache(path: pathlib.Path | None, cache: dict[str, str]) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cache, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def asr_cache_key(asr_command: str, wav: pathlib.Path, sample: Sample, bitrate: int, gain_db: str, snr_db: str, postfilter: int) -> str:
    return json.dumps(
        {
            "asr_command": asr_command,
            "bitrate": bitrate,
            "gain_db": gain_db,
            "sample_id": sample.sample_id,
            "sha256": file_sha256(wav),
            "snr_db": snr_db,
            "postfilter": postfilter,
        },
        sort_keys=True,
    )


def run_asr(
    asr_command: str,
    wav: pathlib.Path,
    sample: Sample,
    bitrate: int,
    gain_db: str,
    snr_db: str,
    postfilter: int,
    asr_cache: dict[str, str] | None,
    asr_cache_path: pathlib.Path | None,
) -> str:
    key = asr_cache_key(asr_command, wav, sample, bitrate, gain_db, snr_db, postfilter) if asr_cache is not None else ""
    if asr_cache is not None and key in asr_cache:
        return asr_cache[key]
    command = asr_command.format(
        wav=str(wav),
        sample_id=sample.sample_id,
        bitrate=bitrate,
        gain_db=gain_db,
        snr_db=snr_db,
    )
    result = subprocess.run(command, shell=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(f"ASR command failed for {wav}\ncommand: {command}\n{result.stderr}")
    transcript = result.stdout.strip()
    if asr_cache is not None:
        asr_cache[key] = transcript
        save_asr_cache(asr_cache_path, asr_cache)
    return transcript


def parse_csv_ints(value: str) -> list[int]:
    return [int(item.strip()) for item in value.split(",") if item.strip()]


def parse_csv_strings(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def load_baseline(path: pathlib.Path | None) -> dict[tuple[str, str, str, int, str, int], float]:
    if path is None or not path.exists():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    rows = data.get("rows", data if isinstance(data, list) else [])
    baseline: dict[tuple[str, str, str, int, str, int], float] = {}
    for row in rows:
        gain_db = str(row.get("gain_db", "0"))
        baseline[(str(row["sample_id"]), gain_db, str(row["snr_db"]), int(row["bitrate"]), str(row["application"]),
                  int(row.get("postfilter", 0)))] = float(row["wer"])
    return baseline


def write_reports(scores: list[Score], output_dir: pathlib.Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = [score.__dict__ for score in scores]
    (output_dir / "wer_results.json").write_text(json.dumps({"rows": rows}, indent=2), encoding="utf-8")

    with (output_dir / "wer_results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()) if rows else [])
        if rows:
            writer.writeheader()
            writer.writerows(rows)

    average_wer = sum(score.wer for score in scores) / max(1, len(scores))
    average_cer = sum(score.cer for score in scores) / max(1, len(scores))
    lines = [
        "# WER Validation Report",
        "",
        f"- Cases: {len(scores)}",
        f"- Average WER: {average_wer:.4f}",
        f"- Average CER: {average_cer:.4f}",
        "",
        "| Sample | Gain | SNR | Bitrate | Application | Postfilter | WER | CER | Hypothesis |",
        "|---|---:|---:|---:|---|---:|---:|---:|---|",
    ]
    for score in scores:
        hyp = score.hypothesis.replace("|", "\\|")
        lines.append(
            f"| {score.sample_id} | {score.gain_db} dB | {score.snr_db} | {score.bitrate} | {score.application} | {score.postfilter} | "
            f"{score.wer:.4f} | {score.cer:.4f} | {hyp} |"
        )
    (output_dir / "wer_report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run WER-style validation on opuscpp encoded speech.")
    parser.add_argument("--manifest", required=True, type=pathlib.Path, help="JSON manifest with WAV paths and reference transcripts.")
    parser.add_argument("--asr-command", default=os.environ.get("OPUSCPP_ASR_COMMAND", ""), help="Command that prints transcript; use {wav}.")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++23 compiler.")
    parser.add_argument("--output-dir", default=REPO_ROOT / "build" / "wer_validation", type=pathlib.Path)
    parser.add_argument("--bitrate", default="16000,24000,32000,48000", help="Comma-separated bitrates.")
    parser.add_argument("--application", default="voip", help="Comma-separated applications: voip,audio,lowdelay.")
    parser.add_argument("--gain-db", default="0", help="Comma-separated input gain values before encode, e.g. 0,-12,-24 for quiet talkers.")
    parser.add_argument("--snr-db", default="clean,20,10,5,0", help="Comma-separated SNR values plus clean.")
    parser.add_argument("--frame-size", default=960, type=int)
    parser.add_argument("--vbr", default=1, type=int)
    parser.add_argument("--complexity", default=10, type=int)
    parser.add_argument("--postfilter", default=0, choices=range(4), type=int)
    parser.add_argument("--max-average-wer", default=None, type=float)
    parser.add_argument("--max-case-wer", default=None, type=float)
    parser.add_argument("--asr-cache", default=None, type=pathlib.Path, help="Optional JSON cache for ASR transcripts keyed by decoded WAV hash.")
    parser.add_argument("--baseline", default=None, type=pathlib.Path, help="Optional previous wer_results.json for regression gating.")
    parser.add_argument("--max-wer-regression", default=0.02, type=float)
    parser.add_argument("--update-baseline", action="store_true", help="Write current results to --baseline after a passing run.")
    args = parser.parse_args()

    if not args.asr_command:
        print("No ASR command configured. Pass --asr-command or set OPUSCPP_ASR_COMMAND.", file=sys.stderr)
        return 2

    samples = load_manifest(args.manifest)
    bitrates = parse_csv_ints(args.bitrate)
    applications = parse_csv_strings(args.application)
    gain_values = parse_csv_strings(args.gain_db)
    snr_values = parse_csv_strings(args.snr_db)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    roundtrip_exe = build_roundtrip(args.cxx, args.output_dir)
    baseline = load_baseline(args.baseline)
    asr_cache = load_asr_cache(args.asr_cache) if args.asr_cache is not None else None

    scores: list[Score] = []
    failures: list[str] = []
    for sample in samples:
        for gain_db in gain_values:
            for snr_db in snr_values:
                input_wav = make_conditioned_wav(
                    sample.wav,
                    args.output_dir / "inputs" / f"{sample.sample_id}_gain{gain_db}db_{snr_db}snr.wav",
                    gain_db,
                    snr_db,
                    0x5154,
                )
                for application in applications:
                    for bitrate in bitrates:
                        decoded_wav = (
                            args.output_dir
                            / "decoded"
                            / f"{sample.sample_id}_gain{gain_db}db_{snr_db}snr_{application}_{bitrate}.wav"
                        )
                        decoded_wav.parent.mkdir(parents=True, exist_ok=True)
                        subprocess.run(
                            [
                                str(roundtrip_exe),
                                str(input_wav),
                                str(decoded_wav),
                                str(bitrate),
                                application,
                                str(args.frame_size),
                                str(args.vbr),
                                str(args.complexity),
                                str(args.postfilter),
                            ],
                            check=True,
                        )
                        hypothesis = run_asr(args.asr_command, decoded_wav, sample, bitrate, gain_db, snr_db, args.postfilter, asr_cache,
                                             args.asr_cache)
                        score = Score(
                            sample_id=sample.sample_id,
                            gain_db=gain_db,
                            snr_db=snr_db,
                            bitrate=bitrate,
                            application=application,
                            postfilter=args.postfilter,
                            reference=sample.transcript,
                            hypothesis=hypothesis,
                            wer=word_error_rate(sample.transcript, hypothesis),
                            cer=char_error_rate(sample.transcript, hypothesis),
                            decoded_wav=str(decoded_wav),
                        )
                        scores.append(score)
                        if args.max_case_wer is not None and score.wer > args.max_case_wer:
                            failures.append(
                                f"{sample.sample_id}/{gain_db}dB/{snr_db}/{application}/{bitrate}: "
                                f"WER {score.wer:.4f} > {args.max_case_wer:.4f}"
                            )
                        previous = baseline.get((sample.sample_id, gain_db, snr_db, bitrate, application, args.postfilter))
                        if previous is not None and score.wer - previous > args.max_wer_regression:
                            failures.append(
                                f"{sample.sample_id}/{gain_db}dB/{snr_db}/{application}/{bitrate}: "
                                f"WER regression {score.wer - previous:.4f} > {args.max_wer_regression:.4f}"
                            )

    write_reports(scores, args.output_dir)
    average_wer = sum(score.wer for score in scores) / max(1, len(scores))
    if args.max_average_wer is not None and average_wer > args.max_average_wer:
        failures.append(f"average WER {average_wer:.4f} > {args.max_average_wer:.4f}")

    if failures:
        print("WER validation FAILED")
        for failure in failures:
            print(f"- {failure}")
        print(f"Report: {args.output_dir / 'wer_report.md'}")
        return 1

    if args.update_baseline and args.baseline is not None:
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.output_dir / "wer_results.json", args.baseline)

    print(f"WER validation PASS average_wer={average_wer:.4f} report={args.output_dir / 'wer_report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
