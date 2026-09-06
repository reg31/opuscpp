from __future__ import annotations

import argparse
import concurrent.futures
from pathlib import Path
import re
import subprocess


metrics_pattern = re.compile(
    r"current .*?pesq_style=(?P<pesq>\S+).*?visqol_style=(?P<visqol>\S+)"
)
boundary_bitrates = (
    15000,
    15500,
    15900,
    16000,
    16100,
    16500,
    17000,
    18000,
    20000,
    22000,
    23000,
    23500,
    23900,
    24000,
    24100,
    24500,
    25000,
    28000,
    32000,
    48000,
    64000,
    96000,
    128000,
    192000,
    256000,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check that encoder voice denoising does not reduce tracked speech-quality metrics.")
    parser.add_argument("--harness", type=Path, required=True, help="Built perceptual_memory_validation executable.")
    parser.add_argument("--input", type=Path, required=True, help="Noisy mono speech WAV input.")
    parser.add_argument("--reference", type=Path, required=True, help="Clean mono speech WAV reference.")
    parser.add_argument("--bitrates", type=int, nargs="+", default=boundary_bitrates)
    return parser.parse_args()


def measure(arguments: argparse.Namespace, bitrate: int, denoise: bool) -> tuple[int, bool, float, float]:
    command = [
        str(arguments.harness),
        "--input",
        str(arguments.input),
        "--reference",
        str(arguments.reference),
        "--bitrate",
        str(bitrate),
        "--application",
        "voip",
        "--max-seconds",
        "6",
        "--skip-memory",
    ]
    if denoise:
        command.append("--current-voice-denoise")
    output = subprocess.run(command, check=True, text=True, capture_output=True).stdout
    match = metrics_pattern.search(output)
    if match is None:
        raise RuntimeError(output)
    return bitrate, denoise, float(match["pesq"]), float(match["visqol"])


def main() -> None:
    arguments = parse_arguments()
    jobs = [(bitrate, denoise) for bitrate in arguments.bitrates for denoise in (False, True)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, len(jobs))) as pool:
        results = list(pool.map(lambda job: measure(arguments, *job), jobs))
    indexed = {(bitrate, denoise): (pesq, visqol) for bitrate, denoise, pesq, visqol in results}
    failures = []
    print("bitrate,pesq_delta,visqol_delta")
    for bitrate in arguments.bitrates:
        off = indexed[bitrate, False]
        on = indexed[bitrate, True]
        pesq_delta = on[0] - off[0]
        visqol_delta = on[1] - off[1]
        print(f"{bitrate},{pesq_delta:+.8f},{visqol_delta:+.8f}")
        if pesq_delta < 0 or visqol_delta < 0:
            failures.append(bitrate)
    if failures:
        raise SystemExit("voice denoise quality regression at: " + ", ".join(map(str, failures)))


if __name__ == "__main__":
    main()
