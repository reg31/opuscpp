#!/usr/bin/env python3
"""Check local prerequisites for opuscpp test workflows."""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import sys


def tool_line(label: str, command: str, required: bool = True) -> tuple[bool, str]:
    path = shutil.which(command)
    if path:
        return True, f"[ok]   {label}: {path}"
    status = "miss" if required else "info"
    return (not required), f"[{status}] {label}: not found ({command})"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check local prerequisites for opuscpp smoke tests and official-opus comparisons."
    )
    parser.add_argument(
        "--official",
        action="store_true",
        help="Also check tools commonly needed to reproduce official Opus comparison builds.",
    )
    parser.add_argument(
        "--cxx",
        default=os.environ.get("CXX", "c++"),
        help="Compiler command to check (default: CXX or c++).",
    )
    args = parser.parse_args()

    print(f"Platform : {platform.system()} {platform.release()}")
    print(f"Python   : {sys.version.split()[0]}")
    print()
    print("Required for the portable smoke test:")

    checks = [
        tool_line("Python launcher", sys.executable, required=True),
        tool_line("C++23 compiler", args.cxx, required=True),
    ]

    if args.official:
        print()
        print("Additional tools commonly used for official-opus comparison runs:")
        checks.extend(
            [
                tool_line("git", "git", required=True),
                tool_line("cmake", "cmake", required=True),
                tool_line("ninja", "ninja", required=False),
                tool_line("make", "make", required=False),
            ]
        )

    ok = True
    for passed, line in checks:
        print(line)
        ok = ok and passed

    print()
    if ok:
        print("Environment check passed.")
    else:
        print("Environment check failed: install the missing required tools above.")

    print()
    print("Notes:")
    print("- CMake is not required for the basic smoke test in this repository.")
    print("- CMake is only needed if you want to reproduce the official-opus comparison builds.")
    print("- After the check passes, run the smoke test with:")
    if os.name == "nt":
        print(r"  py tests\run_smoke.py --cxx g++")
    else:
        print("  python3 tests/run_smoke.py --cxx c++")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
