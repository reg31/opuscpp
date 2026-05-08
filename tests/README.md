# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

## Quick start

### Option 1 - Run the full conformance and benchmark report in one command (recommended)

The commands below download the current `opuscpp` test bundle and run the full official-comparison/report flow automatically.
You can run them from any folder: they create an `opuscpp-report` workspace in your current folder, run the report workflow there, and keep the downloaded checkout and generated artifacts so you can inspect them afterward.
The final Markdown report is saved at `./opuscpp-report/full_report.md`.

macOS / Linux:

```bash
/bin/sh -c "$(curl -fsSL https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_full_report.sh)"
```

Windows PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_full_report.ps1' -OutFile run_full_report.ps1; ./run_full_report.ps1"
```

These one-liners expect Python 3, a C++23 compiler, `git`, and `cmake` to already be installed and available on your PATH.
On Windows, add `-Cleanup` if you want the helper workspace removed at the end.

### Option 2 - Manual prerequisites

Install the following yourself:

- Python 3
- A C++23 compiler (`g++`, `clang++`, or equivalent)
- `git`
- `cmake`
- either `ninja` or `make`

## Official comparison setup

### Option 1 - Run the setup script (recommended)

This script can:

- clone official Opus 1.6.1,
- build it as a static comparison build with intrinsics disabled,
- build the `opuscpp` decoder conformance harness,
- and download the RFC vector bundles into `tests/external/testvectors`.

macOS / Linux:

```bash
python3 tests/scripts/setup_official_compare.py --cxx c++
```

Windows:

```powershell
py tests\scripts\setup_official_compare.py --cxx g++
```

By default it downloads the RFC 6716 vector bundle. To fetch both the RFC 6716 and RFC 8251 bundles:

```bash
python3 tests/scripts/setup_official_compare.py --download-vectors both
```

### Option 2 - Manual setup

If you prefer to do it yourself, the equivalent manual steps are:

1. Obtain the official Opus RFC test vector set.
2. Build official Opus 1.6.1 as a static library with intrinsics disabled and matching `-O2 -DNDEBUG` flags for a portable-C comparison.
3. Build the `opuscpp` decoder harness.

## Optional quick local smoke test

From the repository root:

```bash
python3 tests/run_smoke.py --cxx c++
```

Or use the tiny wrappers:

macOS / Linux:

```bash
sh tests/scripts/run_smoke.sh c++
```

Windows PowerShell:

```powershell
./tests/scripts/run_smoke.ps1 -Cxx g++
```

Permalinks for these wrapper scripts:

- [run_smoke.ps1](https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_smoke.ps1)
- [run_smoke.sh](https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_smoke.sh)

When run from a local checkout, these wrappers call `tests/run_smoke.py` from the repository root. When run via the raw permalinks above, they download the current repository snapshot into `./opuscpp-smoke` and run the same smoke harness from there.

The smoke test compiles `src/opus_codec.cpp`, generates PCM in-process, encodes and decodes mono/stereo frames at 16/24/32/48/96/128/192/256 kbps, and checks packet duration/round-trip success.

If Python 3 or your C++23 compiler is missing, the smoke script will fail early and show the missing command.

Generate optional local listening samples:

```bash
python3 tests/generate_synthetic_wav.py --out tests/generated_audio
```

The generated files are ignored by git.

## RFC decode conformance

`RFC decode conformance` means the standard Opus decoder-vector check: decode the official RFC 6716 test vectors and the RFC 8251 update vectors, then compare the output against the reference PCM with the official `opus_compare` acceptance criteria.

The RFC vector files are not committed to this repository. If you used `tests/scripts/setup_official_compare.py`, you already have the recommended directory layout and build outputs. To run full decode conformance manually, build the decoder harness with:

```bash
c++ -std=c++23 -O2 -I src \
    tests/conformance_decode.cpp src/opus_codec.cpp \
    -o build/conformance_decode
```

4. Run each vector through `conformance_decode` and compare the generated PCM with the reference decoded PCM using the official `opus_compare` tool from the Opus source tree.

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC decode vectors | 24/24 passed |
| Mono/stereo coverage | Passed |
| Final range check mode | Supported by harness |

## Encode oracle validation

`Encode oracle validation` is the project's encoder regression gate, not a separate IETF RFC test. Opus encoders are not required to emit identical packets, so byte-for-byte packet comparison would be the wrong test. Instead, the harness encodes the same generated validation cases with `opuscpp` and with official Opus 1.6.1, decodes both paths with the official decoder, and compares the decoded audio. The relevant files are:

- `conformance_encode.cpp`
- `official_encode_oracle.cpp`
- `encode_conformance_shared.h`

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC 8251 encode oracle cases | 96/96 passed |
| Total encode oracle validation cases | 96/96 passed |

## Perceptual and memory harness

`perceptual_memory_validation.cpp` compares this implementation with official Opus on generated or user-provided 16-bit PCM WAV input. It reports:

- SNR and segmental SNR.
- PESQ-style proxy score.
- ViSQOL-style proxy score.
- CELT-style high-band proxy score.
- Average packet bytes.
- Encode time.
- Optional process memory measurements.

These proxy scores are useful for regression tracking, but they are not substitutes for official PESQ/ViSQOL tooling or listening tests.

## Speed metrics vs official Opus

Portable comparison setup: `opuscpp` is compiled globally with `-O2 -DNDEBUG`, with selected source-level attributes for hot integer paths and cold/size-sensitive paths. Official Opus 1.6.1 is built with matching `-O2 -DNDEBUG` flags and intrinsics disabled. Encode and decode speed are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster.

| Bitrate | Encode speed | Current avg bytes | Official avg bytes | Decode speed |
|---:|---:|---:|---:|---:|
| 16 kbps | 1.29x | 41.37 | 42.24 | 1.54x |
| 24 kbps | 1.37x | 61.15 | 62.52 | 1.17x |
| 32 kbps | 1.37x | 81.20 | 83.25 | 1.17x |
| 48 kbps | 1.22x | 121.30 | 121.28 | 1.12x |
| 64 kbps | 1.30x | 161.40 | 161.39 | 1.09x |
| 96 kbps | 1.42x | 241.00 | 241.56 | 1.05x |
| 128 kbps | 1.50x | 321.00 | 321.75 | 1.05x |
| 192 kbps | 1.37x | 481.00 | 482.13 | 1.02x |
| 256 kbps | 1.32x | 641.00 | 642.12 | 1.01x |

Source CSVs:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

## Speed metrics vs official Opus with x86 intrinsics

This is the more practical Windows desktop comparison: official Opus 1.6.1 is built at `-O2` with x86 runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` remains the same pure C++23 build with no assembly and no SIMD intrinsics. Measurements are from Windows MinGW GCC 16.1 on an AMD Ryzen 7 8845HS, using the repository's 8-second stereo synthetic music-like benchmark. A value above `1.00x` means `opuscpp` is faster than the optimized official build. It is shown as a separate practical reference point because many desktop official-Opus builds enable these paths.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16 kbps | 0.98x | 1.48x | 328x | 335x | 1878x | 1269x |
| 24 kbps | 1.03x | 1.14x | 306x | 297x | 1203x | 1059x |
| 32 kbps | 1.05x | 1.10x | 299x | 283x | 1162x | 1056x |
| 48 kbps | 0.96x | 1.07x | 282x | 293x | 1041x | 971x |
| 64 kbps | 1.02x | 1.03x | 270x | 266x | 878x | 849x |
| 96 kbps | 1.06x | 1.02x | 220x | 208x | 693x | 676x |
| 128 kbps | 1.12x | 0.99x | 225x | 200x | 592x | 598x |
| 192 kbps | 1.05x | 1.01x | 187x | 178x | 505x | 502x |
| 256 kbps | 1.09x | 1.00x | 185x | 169x | 460x | 460x |

Source CSV:

- `metrics/speed_vs_official_intrinsics_60s.csv`

## Quality metrics vs official Opus

Quality proxy metrics were measured on the validation corpus used during development. Deltas are `opuscpp - official`; positive is better for the proxy quality columns and negative means smaller packets.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | Packet bytes vs official |
|---:|---:|---:|---:|---:|
| 16 kbps | +0.0003 | -0.0162 | +1.3037 | -2.8% |
| 24 kbps | -0.0071 | +0.0385 | +22.0159 | -2.9% |
| 32 kbps | +0.0006 | +0.0348 | +16.8861 | -3.3% |
| 48 kbps | +0.0005 | +0.0109 | +0.3921 | +0.0% |
| 64 kbps | +0.0001 | +0.0026 | +0.2358 | +0.0% |
| 96 kbps | -0.0001 | +0.0012 | -0.1403 | -0.3% |
| 128 kbps | +0.0004 | +0.0013 | -0.2914 | -0.3% |
| 192 kbps | +0.0002 | -0.0002 | -0.2242 | -0.3% |
| 256 kbps | +0.0003 | -0.0004 | -0.1322 | -0.1% |

Source CSV:

- `metrics/quality_vs_official.csv`

## Detector mode-balance spot check

The lightweight detector added for this snapshot distinguishes spoken pitch from sustained harmonic/music pitch using pitch stability, envelope stability, zero-crossing rate, and low-order tonal markers before updating the voice estimate.

Representative AUDIO-mode results at 32 kbps mono:

| Material class | SILK | Hybrid | CELT |
|---|---:|---:|---:|
| Speech-like | 0.0% | 8.0% | 92.0% |
| Harmonic music | 0.0% | 0.0% | 100.0% |

## Memory metrics

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,224 B | 31,824 B | -49.0% |
| Encoder stereo | 32,448 B | 48,944 B | -33.7% |
| Decoder mono | 14,192 B | 18,352 B | -22.7% |
| Decoder stereo | 21,344 B | 27,312 B | -21.9% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured text+data |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 254,580 B | 0 B | 254,580 B |
| Android arm64 Clang `-O2` | 252,845 B | 800 B | 253,645 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.1 C++23 | Builds with zero warnings in measured configuration. |
| Android arm64 Clang C++23 | Builds with zero warnings in measured configuration. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use `tests/run_smoke.py` or `tests/scripts/run_smoke.sh` for a quick local check. |
