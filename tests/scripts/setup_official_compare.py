#!/usr/bin/env python3
"""Prepare local assets for official-opus comparison and RFC decode conformance."""
from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request

OFFICIAL_OPUS_REPO = "https://github.com/xiph/opus.git"
OFFICIAL_OPUS_TAG = "v1.6.1"
RFC6716_VECTORS_URL = "https://opus-codec.org/static/testvectors/opus_testvectors.tar.gz"
RFC8251_VECTORS_URL = "https://opus-codec.org/static/testvectors/opus_testvectors-rfc8251.tar.gz"


def run(cmd: list[str], cwd: pathlib.Path | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)


def load_csv_rows(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def emit_metrics_report(repo_root: pathlib.Path, output_dir: pathlib.Path) -> pathlib.Path:
    metrics_dir = repo_root / "tests" / "metrics"
    report_dir = output_dir
    report_dir.mkdir(parents=True, exist_ok=True)

    sections = {
        "encode_speed_vs_official": load_csv_rows(metrics_dir / "encode_speed_vs_official.csv"),
        "decode_speed_vs_official": load_csv_rows(metrics_dir / "decode_speed_vs_official.csv"),
        "quality_vs_official": load_csv_rows(metrics_dir / "quality_vs_official.csv"),
        "memory_vs_official": load_csv_rows(metrics_dir / "memory_vs_official.csv"),
    }

    json_path = report_dir / "published_metrics_summary.json"
    json_path.write_text(json.dumps(sections, indent=2), encoding="utf-8")

    md_path = report_dir / "published_metrics_summary.md"
    memory_rows = sections["memory_vs_official"]
    memory_summary: list[tuple[str, str]] = []
    memory_map = {row["label"]: row for row in memory_rows if "label" in row}
    memory_pairs = [
        ("Encoder mono", "current_encoder_ch1", "official_encoder_ch1"),
        ("Encoder stereo", "current_encoder_ch2", "official_encoder_ch2"),
        ("Decoder mono", "current_decoder_ch1", "official_decoder_ch1"),
        ("Decoder stereo", "current_decoder_ch2", "official_decoder_ch2"),
    ]
    for title, current_key, official_key in memory_pairs:
        current = memory_map.get(current_key)
        official = memory_map.get(official_key)
        if not current or not official:
            continue
        current_bytes = float(current["private_per_instance"])
        official_bytes = float(official["private_per_instance"])
        delta_pct = 100.0 * (current_bytes - official_bytes) / official_bytes
        memory_summary.append((title, f"{delta_pct:.1f}%"))

    lines: list[str] = [
        "# Published Metrics Summary",
        "",
        "This report was generated from the committed CSV files in `tests/metrics`.",
        "It summarizes the published benchmark snapshot for this repository state.",
        "",
        "## Decode speed vs official Opus",
        "",
        "| Bitrate | Decode vs official |",
        "|---:|---:|",
    ]
    for row in sections["decode_speed_vs_official"]:
        lines.append(f"| {row.get('bitrate', '')} kbps | {row.get('current_faster_pct', '')}% |")

    lines += [
        "",
        "## Encode speed vs official Opus",
        "",
        "| Bitrate | Encode speedup |",
        "|---:|---:|",
    ]
    for row in sections["encode_speed_vs_official"]:
        lines.append(f"| {row.get('bitrate', '')} kbps | {row.get('encode_speedx', '')}x |")

    lines += [
        "",
        "## Quality deltas vs official Opus",
        "",
        "| Bitrate | PESQ-style delta | ViSQOL-style delta |",
        "|---:|---:|---:|",
    ]
    for row in sections["quality_vs_official"]:
        lines.append(
            f"| {row.get('bitrate', '')} kbps | {row.get('pesq_delta', '')} | {row.get('visqol_delta', '')} |"
        )

    lines += [
        "",
        "## Memory vs official Opus",
        "",
        "| State | Difference |",
        "|---|---:|",
    ]
    for title, delta in memory_summary:
        lines.append(f"| {title} | {delta} |")

    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return md_path


def download(url: str, destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        print(f"Using existing archive: {destination}")
        return
    print(f"Downloading {url} -> {destination}")
    with urllib.request.urlopen(url) as response, destination.open("wb") as out:
        shutil.copyfileobj(response, out)


def extract_tarball(archive: pathlib.Path, out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    marker = out_dir / ".extract_complete"
    if marker.exists():
        print(f"Using existing extraction: {out_dir}")
        return
    print(f"Extracting {archive} -> {out_dir}")
    with tarfile.open(archive, "r:gz") as tf:
        tf.extractall(out_dir)
    marker.write_text("ok\n", encoding="utf-8")


def ensure_official_clone(repo_dir: pathlib.Path) -> None:
    if (repo_dir / ".git").exists():
        print(f"Using existing official-opus checkout: {repo_dir}")
        run(["git", "fetch", "--tags", "--depth", "1", "origin", OFFICIAL_OPUS_TAG], cwd=repo_dir)
    else:
        repo_dir.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--branch", OFFICIAL_OPUS_TAG, "--depth", "1", OFFICIAL_OPUS_REPO, str(repo_dir)])
    run(["git", "checkout", OFFICIAL_OPUS_TAG], cwd=repo_dir)


def configure_and_build_official(repo_dir: pathlib.Path, build_dir: pathlib.Path, generator: str | None) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "cmake",
        "-S",
        str(repo_dir),
        "-B",
        str(build_dir),
        "-DBUILD_SHARED_LIBS=OFF",
        "-DOPUS_DISABLE_INTRINSICS=ON",
        "-DOPUS_BUILD_PROGRAMS=ON",
        "-DOPUS_BUILD_TESTING=ON",
    ]
    if generator:
        cmd.extend(["-G", generator])
    run(cmd)
    run(["cmake", "--build", str(build_dir), "--config", "Release", "--target", "opus_demo", "opus_compare"])


def build_conformance_harness(repo_root: pathlib.Path, build_dir: pathlib.Path, cxx: str) -> pathlib.Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    exe = build_dir / f"conformance_decode{suffix}"
    cmd = [
        cxx,
        "-std=c++23",
        "-O2",
        "-DNDEBUG",
        "-I",
        str(repo_root / "src"),
        str(repo_root / "tests" / "conformance_decode.cpp"),
        str(repo_root / "src" / "opus_codec.cpp"),
        "-o",
        str(exe),
    ]
    if os.name != "nt":
        cmd.append("-lm")
    run(cmd)
    return exe


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Set up official Opus comparison assets and the opuscpp RFC decode harness."
    )
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++23 compiler to use for our harness.")
    parser.add_argument(
        "--generator",
        default="Ninja" if shutil.which("ninja") else None,
        help="Optional CMake generator for the official-opus build.",
    )
    parser.add_argument(
        "--download-vectors",
        choices=["none", "rfc6716", "rfc8251", "both"],
        default="rfc6716",
        help="Download RFC vector bundles into tests/external/testvectors.",
    )
    args = parser.parse_args()

    repo_root = pathlib.Path(__file__).resolve().parents[2]
    external_root = repo_root / "tests" / "external"
    official_repo_dir = external_root / "official_opus"
    official_build_dir = repo_root / "build" / "official_opus"
    harness_build_dir = repo_root / "build" / "conformance_decode"
    report_dir = repo_root / "build" / "official_compare_report"
    vector_root = external_root / "testvectors"

    if args.download_vectors in ("rfc6716", "both"):
        archive = vector_root / "downloads" / "opus_testvectors.tar.gz"
        out_dir = vector_root / "rfc6716"
        download(RFC6716_VECTORS_URL, archive)
        extract_tarball(archive, out_dir)
        print(f"Prepared RFC 6716 vectors in: {out_dir}")
    if args.download_vectors in ("rfc8251", "both"):
        archive = vector_root / "downloads" / "opus_testvectors-rfc8251.tar.gz"
        out_dir = vector_root / "rfc8251"
        download(RFC8251_VECTORS_URL, archive)
        extract_tarball(archive, out_dir)
        print(f"Prepared RFC 8251 vectors in: {out_dir}")

    ensure_official_clone(official_repo_dir)
    print(f"Prepared official Opus checkout: {official_repo_dir}")
    configure_and_build_official(official_repo_dir, official_build_dir, args.generator)
    print(f"Built official Opus comparison tools in: {official_build_dir}")
    harness_path = build_conformance_harness(repo_root, harness_build_dir, args.cxx)
    print(f"Built opuscpp decoder conformance harness: {harness_path}")
    metrics_report = emit_metrics_report(repo_root, report_dir)

    print()
    print("Setup complete.")
    print(f"- Official Opus repo : {official_repo_dir}")
    print(f"- Official build dir : {official_build_dir}")
    print(f"- opuscpp harness    : {harness_path}")
    print(f"- Vector root        : {vector_root}")
    print(f"- Metrics report     : {metrics_report}")
    print()
    print("Published metrics snapshot:")
    for row in load_csv_rows(repo_root / "tests" / "metrics" / "decode_speed_vs_official.csv"):
        print(f"  decode {row.get('bitrate', '')} kbps: {row.get('current_faster_pct', '')}% vs official")
    for row in load_csv_rows(repo_root / "tests" / "metrics" / "encode_speed_vs_official.csv"):
        print(f"  encode {row.get('bitrate', '')} kbps: {row.get('encode_speedx', '')}x official")
    print()
    print("Next steps:")
    print("1. Pick a vector bundle under tests/external/testvectors.")
    print("2. Run the built harness against the .bit files you want to check.")
    print("3. Compare the produced PCM with official opus_compare from the official build directory.")
    print()
    print("Example (adjust paths for the vector you want):")
    print(
        f'  "{harness_path}" 48000 2 <vector.bit> <decoded.pcm> 1'
    )
    print(
        f'  "{official_build_dir / ("opus_compare" + (".exe" if os.name == "nt" else ""))}" -s -r 48000 <reference.dec> <decoded.pcm>'
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
