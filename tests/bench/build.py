"""Build the codec object (curr_ prefix) and benchmark/gate executables against the pinned official libopus."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CXX = "g++"
FLAGS = ["-std=c++23", "-O2", "-DNDEBUG"]
API = ["OpusEncoder", "OpusDecoder", "opus_encoder_create", "opus_encoder_destroy", "opus_encoder_ctl", "opus_encode",
       "opus_encode_float", "opus_decoder_create", "opus_decoder_destroy", "opus_decoder_ctl", "opus_decode",
       "opus_decode_float", "opus_packet_get_nb_samples", "opus_strerror"]
PREFIX = [f"-D{name}=curr_{name}" for name in API]
GATES = {"fec_source_quality": "tests/fec_source_quality.cpp", "bench17": "tests/bench/bench17.cpp"}


def run(cmd):
    print(" ".join(map(str, cmd)), flush=True)
    subprocess.run(list(map(str, cmd)), check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--src", type=Path, default=ROOT / "src", help="directory holding opus_codec.cpp")
    p.add_argument("--out", type=Path, default=ROOT / "build" / "bench")
    p.add_argument("--official", type=Path, default=ROOT / "build" / "official_scalar" / "libopus.a")
    p.add_argument("targets", nargs="*", default=["obj", "size", *GATES])
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    obj = a.out / "curr.o"
    if "obj" in a.targets:
        run([CXX, *FLAGS, f"-I{a.src}", *PREFIX, "-c", a.src / "opus_codec.cpp", "-o", obj])
    if "size" in a.targets:
        pub = a.out / "public.o"
        run([CXX, *FLAGS, f"-I{a.src}", "-c", a.src / "opus_codec.cpp", "-o", pub])
        run(["strip", "--strip-unneeded", "-o", a.out / "public.stripped.o", pub])
        print(f"stripped_bytes={(a.out / 'public.stripped.o').stat().st_size}")
    for name, source in GATES.items():
        if name not in a.targets:
            continue
        lanes = (("", []),) if name == "bench17" else (("_current", []), ("_official", ["-DUSE_OFFICIAL_ENCODER"]))
        for lane, extra in lanes:
            run([CXX, *FLAGS, *extra, f"-I{a.src}", f"-I{ROOT / 'tests'}", ROOT / source, obj, a.official,
                 "-o", a.out / f"{name}{lane}.exe"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
