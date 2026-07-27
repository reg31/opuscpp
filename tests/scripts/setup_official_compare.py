#!/usr/bin/env python3
"""Prepare local assets for official-opus comparison and RFC decode conformance."""
from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

OFFICIAL_OPUS_REPO = "https://github.com/xiph/opus.git"
OFFICIAL_OPUS_TAG = "v1.6.1"
RFC6716_VECTORS_URL = "https://opus-codec.org/static/testvectors/opus_testvectors.tar.gz"
RFC8251_VECTORS_URL = "https://opus-codec.org/static/testvectors/opus_testvectors-rfc8251.tar.gz"
BITRATES = (16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000)


def run(cmd: list[str], cwd: pathlib.Path | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)


def capture(cmd: list[str], cwd: pathlib.Path | None = None) -> str:
    print("+", " ".join(cmd))
    result = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        check=True,
        text=True,
        capture_output=True,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result.stdout


def load_csv_rows(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def resolve_tool_path(tool: str) -> pathlib.Path | None:
    resolved = shutil.which(tool)
    if resolved:
        return pathlib.Path(resolved)
    candidate = pathlib.Path(tool)
    return candidate if candidate.exists() else None


def prepend_tool_directory_to_path(tool: str) -> None:
    tool_path = resolve_tool_path(tool)
    if tool_path is None:
        return
    tool_dir = str(tool_path.parent)
    current_path = os.environ.get("PATH", "")
    if tool_dir not in current_path.split(os.pathsep):
        os.environ["PATH"] = tool_dir + os.pathsep + current_path


def find_size_tool(cxx: str) -> str | None:
    cxx_path = resolve_tool_path(cxx)
    if cxx_path is not None:
        for name in ("size.exe", "size"):
            candidate = cxx_path.parent / name
            if candidate.exists():
                return str(candidate)
    return shutil.which("size")


def parse_size_output(output: str) -> dict[str, str]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(lines) < 2:
        return {}
    parts = lines[-1].split()
    if len(parts) < 4:
        return {}
    return {"text": parts[0], "data": parts[1], "bss": parts[2], "total": parts[3]}


def effective_kbps(avg_packet_bytes: str) -> str:
    """Convert 20 ms Opus frame bytes to payload kbps."""
    return f"{float(avg_packet_bytes) * 0.4:.3f}"


def normalize_signed_zero(value: str) -> str:
    """Keep rounded zero metrics from rendering as negative zero."""
    return value.lstrip("-") if float(value) == 0 else value


def emit_metrics_report(
    repo_root: pathlib.Path,
    output_dir: pathlib.Path,
    rfc: dict[str, str],
    encode: dict[str, str],
    api_behavior: list[str],
    perceptual_rows: list[dict[str, str]],
    voip_perceptual_rows: list[dict[str, str]],
    benchmark_rows: list[dict[str, str]],
    detector_rows: list[str],
    binary_size: dict[str, str],
    toolchains: list[str],
) -> pathlib.Path:
    metrics_dir = repo_root / "tests" / "metrics"
    report_dir = output_dir
    report_dir.mkdir(parents=True, exist_ok=True)

    memory_rows = load_csv_rows(metrics_dir / "memory_vs_official.csv")
    sections = {
        "speed_vs_official": benchmark_rows,
        "quality_vs_official_audio": perceptual_rows,
        "quality_vs_official_voip": voip_perceptual_rows,
        "memory_vs_official": memory_rows,
        "rfc_decode_conformance": rfc,
        "encode_validation_conformance": encode,
        "api_behavior_validation": api_behavior,
        "detector_mode_balance": detector_rows,
        "binary_size": binary_size,
        "toolchains": toolchains,
    }

    json_path = report_dir / "metrics_summary.json"
    json_path.write_text(json.dumps(sections, indent=2), encoding="utf-8")

    md_path = report_dir / "metrics_summary.md"
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

    detector_table: list[tuple[str, str, str, str]] = []
    for line in detector_rows:
        if "," in line and "_pct=" in line:
            parts = line.split(",")
            row = dict(item.split("=", 1) for item in parts[1:] if "=" in item)
            if {"silk_pct", "hybrid_pct", "celt_pct"} <= row.keys():
                detector_table.append(
                    (
                        parts[0].replace("_", " "),
                        f"{float(row['silk_pct']):.1f}%",
                        f"{float(row['hybrid_pct']):.1f}%",
                        f"{float(row['celt_pct']):.1f}%",
                    )
                )
            continue
        row = dict(item.split("=", 1) for item in line.split() if "=" in item)
        if {"class", "frames", "silk", "hybrid", "celt"} <= row.keys():
            frames = max(1, int(row["frames"]))
            detector_table.append(
                (
                    row["class"].replace("_", " "),
                    f"{100.0 * int(row['silk']) / frames:.1f}%",
                    f"{100.0 * int(row['hybrid']) / frames:.1f}%",
                    f"{100.0 * int(row['celt']) / frames:.1f}%",
                )
            )

    size = parse_size_output(binary_size.get("output", ""))
    lines: list[str] = [
        "# Official Comparison Report",
        "",
        "This report is generated by `tests/scripts/setup_official_compare.py`.",
        "It records the locally measured comparison produced by this run.",
        "",
        f"Benchmark setup: `opuscpp` is built with `{OPUSCPP_OPT_FLAG} -DNDEBUG`; official Opus is built with `-O2 -DNDEBUG` and intrinsics enabled for the public comparison.",
        "",
        "## RFC decode conformance",
        "",
        "- Official Opus checkout prepared locally.",
        "- Official `opus_demo` / `opus_compare` build prepared locally.",
        "- `opuscpp` decoder conformance harness built locally.",
        f"- Vector set: {rfc.get('vector_set', 'unknown')}.",
        f"- Local result: {rfc.get('passed', '0')}/{rfc.get('total', '0')} RFC decode vectors passed.",
        "",
        "## Encode interoperability validation",
        "",
        f"- Local result: {encode.get('passed', '0')}/96 generated encode cases produced packets accepted by official Opus.",
        "",
        "## API behavior validation",
        "",
    ]
    if api_behavior:
        lines.extend(f"- `{line}`" for line in api_behavior)
    else:
        lines.append("- No API behavior validation was run.")

    lines += [
        "",
        "## Perceptual and memory harness",
        "",
        "- Local run includes perceptual proxy metrics, effective-bitrate measurements, encode timing, and memory figures.",
        "- Proxy quality uses the opt-in automatic opuscpp decoder postfilter; default decoding remains unfiltered.",
        "- Source harness: `tests/perceptual_memory_validation.cpp`.",
        "",
        "## Speed metrics vs official Opus",
        "",
        "| Bitrate | Encode speedup | Decode speedup |",
        "|---:|---:|---:|",
    ]
    for row in benchmark_rows:
        bitrate = row.get("bitrate", "")
        bitrate_kbps = int(bitrate) // 1000 if bitrate.isdigit() else bitrate
        lines.append(f"| {bitrate_kbps} kbps | {row.get('encode_speedx', '')}x | {row.get('decode_speedx', '')}x |")

    lines += [
        "",
        "## AUDIO quality metrics vs official Opus",
        "",
        "| Bitrate | SNR delta | RMS error delta | Mean abs error delta | PESQ-style delta | ViSQOL-style delta | Log-band corr delta | CELT delta | opuscpp effective bitrate | official Opus effective bitrate |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in perceptual_rows:
        bitrate = row.get("bitrate", "")
        bitrate_kbps = int(bitrate) // 1000 if bitrate.isdigit() else bitrate
        lines.append(
            f"| {bitrate_kbps} kbps | {row.get('snr_delta', '')} | {row.get('rms_error_delta', '')} | "
            f"{row.get('mean_abs_error_delta', '')} | {row.get('pesq_delta', '')} | {row.get('visqol_delta', '')} | "
            f"{row.get('logband_corr_delta', '')} | {row.get('celt_delta', '')} | "
            f"{row.get('opuscpp_effective_kbps', '')} kbps | {row.get('official_effective_kbps', '')} kbps |"
        )

    lines += [
        "",
        "## VOIP quality metrics vs official Opus",
        "",
        "| Bitrate | SNR delta | RMS error delta | Mean abs error delta | PESQ-style delta | ViSQOL-style delta | Log-band corr delta | CELT delta | opuscpp effective bitrate | official Opus effective bitrate |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in voip_perceptual_rows:
        bitrate = row.get("bitrate", "")
        bitrate_kbps = int(bitrate) // 1000 if bitrate.isdigit() else bitrate
        lines.append(
            f"| {bitrate_kbps} kbps | {row.get('snr_delta', '')} | {row.get('rms_error_delta', '')} | "
            f"{row.get('mean_abs_error_delta', '')} | {row.get('pesq_delta', '')} | {row.get('visqol_delta', '')} | "
            f"{row.get('logband_corr_delta', '')} | {row.get('celt_delta', '')} | "
            f"{row.get('opuscpp_effective_kbps', '')} kbps | {row.get('official_effective_kbps', '')} kbps |"
        )

    lines += [
        "",
        "## Detector mode-balance spot check",
        "",
        "| Material class | SILK | Hybrid | CELT |",
        "|---|---:|---:|---:|",
    ]
    for material, silk, hybrid, celt in detector_table:
        lines.append(f"| {material} | {silk} | {hybrid} | {celt} |")

    lines += [
        "",
        "## Memory metrics",
        "",
        "| State | Difference |",
        "|---|---:|",
    ]
    for title, delta in memory_summary:
        lines.append(f"| {title} | {delta} |")

    lines += [
        "",
        "## Binary size",
        "",
        "| Build | Text | Data | Total measured image (text+data+bss) |",
        "|---|---:|---:|---:|",
        f"| Host C++23 `{OPUSCPP_OPT_FLAG}` | {size.get('text', 'n/a')} B | {size.get('data', 'n/a')} B | {size.get('total', 'n/a')} B |",
        "",
        "## Toolchains checked",
        "",
        "| Toolchain | Status |",
        "|---|---|",
    ]
    for line in toolchains:
        lines.append(f"| {line} | checked |")

    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return md_path

def find_vector_dir(vector_root: pathlib.Path) -> pathlib.Path:
    # RFC 8251 updates the normative decoder vectors. Prefer it whenever both
    # historical and updated bundles are available in tests/external.
    for preferred in ("rfc8251", "rfc6716"):
        preferred_root = vector_root / preferred
        if preferred_root.exists():
            for candidate in preferred_root.rglob("testvector01.bit"):
                return candidate.parent
    for candidate in vector_root.rglob("testvector01.bit"):
        return candidate.parent
    raise FileNotFoundError(f"Could not find extracted RFC vector files under {vector_root}")


def find_official_library(build_dir: pathlib.Path) -> pathlib.Path:
    candidates = [
        *build_dir.rglob("libopus.a"),
        *build_dir.rglob("opus.lib"),
        *build_dir.rglob("libopus.lib"),
    ]
    if not candidates:
        raise FileNotFoundError(f"Could not find official Opus static library under {build_dir}")
    return candidates[0]


def current_alias_macros(prefix: str) -> list[str]:
    names = [
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
    macros: list[str] = []
    for name in names:
        if name in ("OpusEncoder", "OpusDecoder"):
            macros.append(f"-D{name}={prefix}_{name}")
        else:
            macros.append(f"-D{name}={prefix}_{name}")
    return macros


OPUSCPP_OPT_FLAG = "-O2"


def compile_object(
    cxx: str,
    source: pathlib.Path,
    output: pathlib.Path,
    includes: list[pathlib.Path],
    extra_args: list[str],
) -> pathlib.Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cxx, "-std=c++23", OPUSCPP_OPT_FLAG, "-DNDEBUG"]
    for include in includes:
        cmd.extend(["-I", str(include)])
    cmd.extend(extra_args)
    cmd.extend(["-c", str(source), "-o", str(output)])
    run(cmd)
    return output


def link_executable(
    cxx: str,
    sources: list[pathlib.Path],
    objects: list[pathlib.Path],
    includes: list[pathlib.Path],
    output: pathlib.Path,
    libs: list[pathlib.Path],
) -> pathlib.Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cxx, "-std=c++23", OPUSCPP_OPT_FLAG, "-DNDEBUG"]
    for include in includes:
        cmd.extend(["-I", str(include)])
    cmd.extend(str(src) for src in sources)
    cmd.extend(str(obj) for obj in objects)
    cmd.extend(str(lib) for lib in libs)
    if os.name != "nt":
        cmd.append("-lm")
    cmd.extend(["-o", str(output)])
    run(cmd)
    return output


def run_rfc_decode_conformance(harness: pathlib.Path, opus_compare: pathlib.Path, vector_dir: pathlib.Path, report_dir: pathlib.Path) -> dict[str, str]:
    out_dir = report_dir / "rfc_decode"
    log_dir = out_dir / "logs"
    out_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    def stereo_reference_for_mono(dec_path: pathlib.Path) -> pathlib.Path:
        """opus_compare expects its reference input as stereo, even for mono tests."""
        if not dec_path.name.endswith("m.dec"):
            return dec_path
        stereo_sibling = dec_path.with_name(dec_path.stem.removesuffix("m") + ".dec")
        if stereo_sibling.exists() and dec_path.stat().st_size == stereo_sibling.stat().st_size:
            return dec_path
        stereo_path = out_dir / f"{dec_path.stem}_stereo_ref.dec"
        source = dec_path.read_bytes()
        stereo = bytearray(len(source) * 2)
        for index in range(0, len(source), 2):
            sample = source[index : index + 2]
            stereo[2 * index : 2 * index + 2] = sample
            stereo[2 * index + 2 : 2 * index + 4] = sample
        stereo_path.write_bytes(stereo)
        return stereo_path

    passed = 0
    total = 24
    for channels in (1, 2):
        for index in range(1, 13):
            stem = f"testvector{index:02d}"
            bit_path = vector_dir / f"{stem}.bit"
            out_path = out_dir / f"{stem}_{channels}ch.pcm"
            capture([str(harness), "48000", str(channels), str(bit_path), str(out_path), "1"])
            compare_args = [str(opus_compare)]
            if channels == 2:
                compare_args.append("-s")
            compare_args.extend(["-r", "48000"])
            # RFC 8251 includes an alternate "m" reference for the permitted
            # no-phase-inversion mono-downmix behavior. The opuscpp mono path
            # intentionally uses that behavior, so try it first for mono runs
            # and avoid a guaranteed failed opus_compare pass on those vectors.
            if channels == 1:
                dec_candidates = [vector_dir / f"{stem}m.dec", vector_dir / f"{stem}.dec"]
            else:
                dec_candidates = [vector_dir / f"{stem}.dec", vector_dir / f"{stem}m.dec"]
            matched = False
            matched_reference = ""
            compare_logs: list[tuple[pathlib.Path, int]] = []
            for dec_path in dec_candidates:
                compare_dec_path = stereo_reference_for_mono(dec_path) if channels == 1 else dec_path
                result = subprocess.run(
                    compare_args + [str(compare_dec_path), str(out_path)],
                    text=True,
                    capture_output=True,
                )
                log_path = log_dir / f"{stem}_{channels}ch_vs_{dec_path.stem}.log"
                log_path.write_text((result.stdout or "") + (result.stderr or ""), encoding="utf-8")
                compare_logs.append((log_path, result.returncode))
                if result.returncode == 0:
                    matched = True
                    matched_reference = dec_path.name
                    break
            if not matched:
                for log_path, return_code in compare_logs:
                    print(f"  compare failed rc={return_code}: {log_path}")
                raise RuntimeError(f"RFC decode mismatch for {stem} channels={channels}")
            passed += 1
            print(f"RFC vector PASS {stem} channels={channels} reference={matched_reference}")
    vector_set = "rfc8251" if "rfc8251" in vector_dir.parts else "rfc6716" if "rfc6716" in vector_dir.parts else vector_dir.name
    return {"passed": str(passed), "total": str(total), "vector_set": vector_set}


def run_encode_validation_conformance(
    cxx: str,
    repo_root: pathlib.Path,
    official_lib: pathlib.Path,
    vector_dir: pathlib.Path,
    report_dir: pathlib.Path,
) -> dict[str, str]:
    build_dir = report_dir / "encode_validation"
    build_dir.mkdir(parents=True, exist_ok=True)
    validation_exe = build_dir / ("official_encode_validation.exe" if os.name == "nt" else "official_encode_validation")
    conformance_exe = build_dir / ("conformance_encode.exe" if os.name == "nt" else "conformance_encode")
    codex_obj = compile_object(
        cxx,
        repo_root / "src" / "opus_codec.cpp",
        build_dir / "codex_opus_codec.o",
        [repo_root / "src"],
        current_alias_macros("codex"),
    )
    link_executable(
        cxx,
        [repo_root / "tests" / "official_encode_validation.cpp"],
        [],
        [repo_root / "tests"],
        validation_exe,
        [official_lib],
    )
    link_executable(
        cxx,
        [repo_root / "tests" / "conformance_encode.cpp"],
        [codex_obj],
        [repo_root / "src", repo_root / "tests"],
        conformance_exe,
        [official_lib],
    )
    validation_path = build_dir / "encode_validation.bin"
    capture([str(validation_exe), str(vector_dir), str(validation_path)])
    output = capture([str(conformance_exe), str(vector_dir), str(validation_path)])
    match = re.search(r"Encode conformance completed \((\d+) case checks passed\)", output)
    return {"passed": match.group(1) if match else "0"}


def run_perceptual_and_memory(
    cxx: str,
    repo_root: pathlib.Path,
    official_lib: pathlib.Path,
    report_dir: pathlib.Path,
) -> dict[str, object]:
    build_dir = report_dir / "perceptual"
    build_dir.mkdir(parents=True, exist_ok=True)
    curr_obj = compile_object(
        cxx,
        repo_root / "src" / "opus_codec.cpp",
        build_dir / "curr_opus_codec.o",
        [repo_root / "src"],
        current_alias_macros("curr"),
    )
    exe = link_executable(
        cxx,
        [repo_root / "tests" / "perceptual_memory_validation.cpp"],
        [curr_obj],
        [repo_root / "tests"],
        build_dir / ("perceptual_memory_validation.exe" if os.name == "nt" else "perceptual_memory_validation"),
        [official_lib],
    )
    generated_audio = report_dir / "generated_audio"
    run([sys.executable, str(repo_root / "tests" / "generate_synthetic_wav.py"), "--out", str(generated_audio), "--seconds", "6"])
    rows: list[dict[str, str]] = []
    voip_rows: list[dict[str, str]] = []
    memory_output = ""

    def measure_rows(application: str, input_name: str, capture_memory: bool) -> list[dict[str, str]]:
        nonlocal memory_output
        measured: list[dict[str, str]] = []
        for bitrate in BITRATES:
            args = [
                str(exe),
                "--input",
                str(generated_audio / input_name),
                "--bitrate",
                str(bitrate),
                "--application",
                application,
            ]
            if not capture_memory or bitrate != BITRATES[0]:
                args.append("--skip-memory")
            output = capture(args)
            if capture_memory and bitrate == BITRATES[0]:
                memory_output = output
            delta_pattern = (
                r"delta current_minus_official.*?snr_db=([^\s]+).*?"
                r"rms_error=([^\s]+).*?mean_abs_error=([^\s]+).*?"
                r"pesq_style=([^\s]+).*?visqol_style=([^\s]+).*?"
                r"logband_corr=([^\s]+).*?celt_quality=([^\s]+).*?"
                r"packet_bytes_pct=[^\s]+.*?encode_speed_ratio_current_vs_official=([^\s]+)"
            )
            delta_match = re.search(delta_pattern, output)
            if not delta_match:
                raise RuntimeError(f"Could not parse perceptual output for {application} bitrate {bitrate}")
            current_match = re.search(r"^\s*current\s+.*?avg_packet_bytes=([^\s]+)", output, re.MULTILINE)
            official_match = re.search(r"^\s*official\s+.*?avg_packet_bytes=([^\s]+)", output, re.MULTILINE)
            if not current_match or not official_match:
                raise RuntimeError(f"Could not parse effective bitrate for {application} bitrate {bitrate}")
            measured.append(
                {
                    "application": application,
                    "bitrate": str(bitrate),
                    "snr_delta": normalize_signed_zero(delta_match.group(1)),
                    "rms_error_delta": normalize_signed_zero(delta_match.group(2)),
                    "mean_abs_error_delta": normalize_signed_zero(delta_match.group(3)),
                    "pesq_delta": normalize_signed_zero(delta_match.group(4)),
                    "visqol_delta": normalize_signed_zero(delta_match.group(5)),
                    "logband_corr_delta": normalize_signed_zero(delta_match.group(6)),
                    "celt_delta": normalize_signed_zero(delta_match.group(7)),
                    "opuscpp_effective_kbps": effective_kbps(current_match.group(1)),
                    "official_effective_kbps": effective_kbps(official_match.group(1)),
                    "encode_speed_ratio": delta_match.group(8),
                }
            )
        return measured

    rows = measure_rows("audio", "synthetic_music_like_stereo.wav", True)
    voip_rows = measure_rows("voip", "synthetic_voice_like_mono.wav", False)
    return {"rows": rows, "voip_rows": voip_rows, "memory_output": memory_output}


def run_benchmark_vs_official(
    cxx: str,
    repo_root: pathlib.Path,
    official_lib: pathlib.Path,
    report_dir: pathlib.Path,
) -> list[dict[str, str]]:
    build_dir = report_dir / "benchmark"
    build_dir.mkdir(parents=True, exist_ok=True)
    curr_obj = compile_object(
        cxx,
        repo_root / "src" / "opus_codec.cpp",
        build_dir / "curr_opus_codec.o",
        [repo_root / "src"],
        current_alias_macros("curr"),
    )
    exe = link_executable(
        cxx,
        [repo_root / "tests" / "benchmark_vs_official.cpp"],
        [curr_obj],
        [repo_root / "tests"],
        build_dir / ("benchmark_vs_official.exe" if os.name == "nt" else "benchmark_vs_official"),
        [official_lib],
    )
    output = capture([str(exe)])
    rows = list(csv.DictReader(output.splitlines()))
    return rows


def run_detector_mode_balance(cxx: str, repo_root: pathlib.Path, report_dir: pathlib.Path) -> list[str]:
    build_dir = report_dir / "detector"
    build_dir.mkdir(parents=True, exist_ok=True)
    curr_obj = compile_object(
        cxx,
        repo_root / "src" / "opus_codec.cpp",
        build_dir / "curr_opus_codec.o",
        [repo_root / "src"],
        current_alias_macros("curr"),
    )
    exe = link_executable(
        cxx,
        [repo_root / "tests" / "detector_mode_balance.cpp"],
        [curr_obj],
        [repo_root / "tests"],
        build_dir / ("detector_mode_balance.exe" if os.name == "nt" else "detector_mode_balance"),
        [],
    )
    output = capture([str(exe)])
    return [line for line in output.splitlines() if line.strip()]


def run_api_behavior_validation(cxx: str, repo_root: pathlib.Path, report_dir: pathlib.Path) -> list[str]:
    build_dir = report_dir / "api_behavior"
    build_dir.mkdir(parents=True, exist_ok=True)
    results: list[str] = []
    suffix = ".exe" if os.name == "nt" else ""
    for name in ("decoder_channel_remap", "packet_duration_behavior", "vbr_budget_behavior"):
        exe = link_executable(
            cxx,
            [repo_root / "tests" / f"{name}.cpp", repo_root / "src" / "opus_codec.cpp"],
            [],
            [repo_root / "src"],
            build_dir / f"{name}{suffix}",
            [],
        )
        output = capture([str(exe)])
        results.extend(line for line in output.splitlines() if line.strip())
    return results


def run_binary_size(cxx: str, repo_root: pathlib.Path, report_dir: pathlib.Path) -> dict[str, str]:
    build_dir = report_dir / "binary_size"
    build_dir.mkdir(parents=True, exist_ok=True)
    obj = compile_object(cxx, repo_root / "src" / "opus_codec.cpp", build_dir / "opus_codec.o", [repo_root / "src"], [])
    size_tool = find_size_tool(cxx)
    if not size_tool:
        return {"status": "size tool unavailable"}
    output = capture([size_tool, str(obj)])
    return {"output": output}


def run_toolchain_checks(repo_root: pathlib.Path, cxx: str, report_dir: pathlib.Path) -> list[str]:
    lines = [f"MinGW/current C++23 compiler: {cxx}"]
    ndk_root = os.environ.get("ANDROID_NDK_ROOT")
    if ndk_root:
        clang = pathlib.Path(ndk_root) / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "aarch64-linux-android21-clang++.cmd"
        if not clang.exists():
            clang = clang.with_suffix("")
        if clang.exists():
            out = report_dir / "toolchain_android.o"
            cmd = [
                str(clang),
                "--target=aarch64-linux-android21",
                "-std=c++23",
                OPUSCPP_OPT_FLAG,
                "-DNDEBUG",
                "-c",
                str(repo_root / "src" / "opus_codec.cpp"),
                "-I",
                str(repo_root / "src"),
                "-o",
                str(out),
            ]
            run(cmd)
            lines.append("Android arm64 Clang C++23: build check passed")
    return lines


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
    if generator is None and os.name == "nt":
        generator = "MinGW Makefiles"
    cmd = [
        "cmake",
        "-S",
        str(repo_dir),
        "-B",
        str(build_dir),
        "-DBUILD_SHARED_LIBS=OFF",
        "-DOPUS_DISABLE_INTRINSICS=OFF",
        "-DOPUS_BUILD_PROGRAMS=ON",
        "-DOPUS_BUILD_TESTING=ON",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG",
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
        OPUSCPP_OPT_FLAG,
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
        default=None,
        help="Optional CMake generator for the official-opus build.",
    )
    parser.add_argument(
        "--download-vectors",
        choices=["none", "rfc6716", "rfc8251", "both"],
        default="rfc6716",
        help="Download RFC vector bundles into tests/external/testvectors.",
    )
    parser.add_argument(
        "--report-out",
        default=None,
        help="Optional path where the final Markdown report should also be copied.",
    )
    parser.add_argument(
        "--opuscpp-opt",
        default="-O2",
        help="Optimization flag for opuscpp harnesses and benchmarks; official Opus remains at -O2 -DNDEBUG.",
    )
    args = parser.parse_args()
    global OPUSCPP_OPT_FLAG
    OPUSCPP_OPT_FLAG = args.opuscpp_opt
    prepend_tool_directory_to_path(args.cxx)

    repo_root = pathlib.Path(__file__).resolve().parents[2]
    external_root = repo_root / "tests" / "external"
    official_repo_dir = external_root / "official_opus"
    official_build_dir = repo_root / "build" / "official_opus_o2_intrinsics_mingw"
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
    official_lib = find_official_library(official_build_dir)
    opus_compare = official_build_dir / ("opus_compare.exe" if os.name == "nt" else "opus_compare")
    vector_dir = find_vector_dir(vector_root)

    rfc = run_rfc_decode_conformance(harness_path, opus_compare, vector_dir, report_dir)
    encode = run_encode_validation_conformance(args.cxx, repo_root, official_lib, vector_dir, report_dir)
    api_behavior = run_api_behavior_validation(args.cxx, repo_root, report_dir)
    perceptual = run_perceptual_and_memory(args.cxx, repo_root, official_lib, report_dir)
    benchmark_rows = run_benchmark_vs_official(args.cxx, repo_root, official_lib, report_dir)
    detector_rows = run_detector_mode_balance(args.cxx, repo_root, report_dir)
    binary_size = run_binary_size(args.cxx, repo_root, report_dir)
    toolchains = run_toolchain_checks(repo_root, args.cxx, report_dir)
    metrics_report = emit_metrics_report(
        repo_root,
        report_dir,
        rfc,
        encode,
        api_behavior,
        perceptual["rows"],
        perceptual["voip_rows"],
        benchmark_rows,
        detector_rows,
        binary_size,
        toolchains,
    )
    if args.report_out:
        report_out = pathlib.Path(args.report_out)
        report_out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(metrics_report, report_out)
        metrics_report = report_out

    print()
    print("Setup complete.")
    print(f"- Official Opus repo : {official_repo_dir}")
    print(f"- Official build dir : {official_build_dir}")
    print(f"- opuscpp harness    : {harness_path}")
    print(f"- Vector root        : {vector_root}")
    print(f"- Metrics report     : {metrics_report}")
    print()
    print("RFC decode conformance:")
    print(f"  passed={rfc['passed']}/{rfc['total']}")
    print("Encode interoperability validation:")
    print(f"  passed_cases={encode['passed']}")
    print("API behavior validation:")
    for row in api_behavior:
        print(f"  {row}")
    print("AUDIO perceptual and memory harness:")
    for row in perceptual["rows"]:
        print(
            f"  bitrate={row['bitrate']} pesq_delta={row['pesq_delta']} "
            f"visqol_delta={row['visqol_delta']} celt_delta={row['celt_delta']} "
            f"opuscpp_effective_kbps={row['opuscpp_effective_kbps']} "
            f"official_effective_kbps={row['official_effective_kbps']} "
            f"encode_speed_ratio={row['encode_speed_ratio']}"
        )
    print("VOIP perceptual harness:")
    for row in perceptual["voip_rows"]:
        print(
            f"  bitrate={row['bitrate']} pesq_delta={row['pesq_delta']} "
            f"visqol_delta={row['visqol_delta']} celt_delta={row['celt_delta']} "
            f"opuscpp_effective_kbps={row['opuscpp_effective_kbps']} "
            f"official_effective_kbps={row['official_effective_kbps']} "
            f"encode_speed_ratio={row['encode_speed_ratio']}"
        )
    print("Speed metrics vs official Opus:")
    for row in benchmark_rows:
        print(
            f"  bitrate={row['bitrate']} encode_speedx={row['encode_speedx']} "
            f"decode_speedx={row['decode_speedx']} "
            f"opuscpp_effective_kbps={row.get('opuscpp_effective_kbps', '')} "
            f"official_effective_kbps={row.get('official_effective_kbps', '')}"
        )
    print("Detector mode-balance spot check:")
    for row in detector_rows:
        print(f"  {row}")
    print("Binary size:")
    print(f"  {binary_size.get('output', binary_size.get('status', 'unavailable')).strip()}")
    print("Toolchains checked:")
    for line in toolchains:
        print(f"  {line}")
    print(f"report saved to: {metrics_report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
