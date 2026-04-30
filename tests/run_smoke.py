#!/usr/bin/env python3
"""Build and run the portable Opus C++ smoke test.

The smoke test generates synthetic PCM in-process; it does not need any audio files.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="C++23 compiler, e.g. g++ or clang++")
    parser.add_argument("--build-dir", default=None, help="Build directory; defaults to build/smoke")
    parser.add_argument("--keep-build", action="store_true", help="Keep the build directory after the run")
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parents[1]
    suffix = ".exe" if os.name == "nt" else ""
    tmp = pathlib.Path(args.build_dir) if args.build_dir else repo / "build" / "smoke"
    tmp.mkdir(parents=True, exist_ok=True)
    exe = tmp / ("smoke_roundtrip" + suffix)

    cmd = [
        args.cxx,
        "-std=c++23",
        "-O2",
        "-DNDEBUG",
        "-I",
        str(repo / "src"),
        str(repo / "tests" / "smoke_roundtrip.cpp"),
        str(repo / "src" / "opus_codec.cpp"),
        "-o",
        str(exe),
    ]
    if os.name != "nt":
        cmd.append("-lm")

    print("Compiling smoke test harness...", flush=True)
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    print("Running smoke test...", flush=True)
    subprocess.run([str(exe)], check=True)
    print("Smoke test passed: mono and stereo completed at 16/24/32/48/96/128/192/256 kbps.", flush=True)
    if not args.keep_build and args.build_dir:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
