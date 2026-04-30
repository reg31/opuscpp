# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

No audio assets are required by the smoke tests. Synthetic samples can be generated locally with `generate_synthetic_wav.py` if you want listening material.

## Quick start

### Option 1 - Run the smoke test in one command (recommended)

The commands below download the current `opuscpp` test bundle and run the portable smoke test automatically.

macOS / Linux:

```bash
/bin/sh -c "$(curl -fsSL https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_smoke.sh)"
```

Windows PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_smoke.ps1' -OutFile run_smoke.ps1; ./run_smoke.ps1"
```

These one-liners expect Python 3 and a C++23 compiler to already be installed and available on your PATH.

### Option 2 - Check prerequisites first

If you want to verify your local toolchain before running the smoke test, use the environment check helper below.

## Prerequisites

### Option 1 - Run the environment check script (recommended)

Use the helper script for your platform. It verifies the basic tools needed for the portable smoke test (Python 3 and a C++23 compiler), and it can also check the extra tools commonly used to reproduce official Opus comparison builds (`git`, `cmake`, and either `ninja` or `make`).

macOS / Linux:

```bash
python3 tests/scripts/check_env.py
python3 tests/scripts/check_env.py --official
```

Windows:

```powershell
py tests\scripts\check_env.py
py tests\scripts\check_env.py --official
```

Notes:

- The basic smoke test needs Python and a C++23 compiler.
- CMake is not required for the basic smoke test in this repository.
- CMake is only needed if you want to reproduce the official-opus comparison builds.

### Option 2 - Manual prerequisites

Install the following yourself:

- Python 3
- A C++23 compiler (`g++`, `clang++`, or equivalent)
- Optional for official-opus comparison runs: `git`, `cmake`, and either `ninja` or `make`

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
2. Build official Opus 1.6.1 as a static library with intrinsics disabled if you want a matched portable-C comparison.
3. Build the `opuscpp` decoder harness.

## Quick smoke test from a local checkout

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

When run from a local checkout, these wrappers call `tests/run_smoke.py` from the repository root. When run via the raw permalinks above, they download the current repository snapshot to a temporary directory and run the same smoke harness from there.

The smoke test compiles `src/opus_codec.cpp`, generates PCM in-process, encodes and decodes mono/stereo frames at 16/24/32/48/96/128/192/256 kbps, and checks packet duration/round-trip success.

Generate optional local listening samples:

```bash
python3 tests/generate_synthetic_wav.py --out tests/generated_audio
```

The generated files are ignored by git.

## RFC decode conformance

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

## Encode oracle conformance

Encode conformance is checked by encoding the same validation cases with `opuscpp`, decoding with official Opus, and comparing against an official-oracle path. The relevant files are:

- `conformance_encode.cpp`
- `official_encode_oracle.cpp`
- `encode_conformance_shared.h`

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| Encode oracle cases | 96/96 passed |

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

Matched setup: official Opus built at `-O2`, intrinsics disabled. Positive decode percent means `opuscpp` is faster; encode speed is a multiplicative speedup over official Opus.

| Bitrate | Encode speed | Current avg bytes | Official avg bytes | Decode vs official |
|---:|---:|---:|---:|---:|
| 16 kbps | 1.39x | 41.81 | 40.99 | +36.8% |
| 24 kbps | 1.79x | 61.45 | 60.96 | +17.7% |
| 32 kbps | 1.78x | 82.52 | 80.95 | +8.6% |
| 48 kbps | 1.64x | 122.85 | 120.76 | +7.2% |
| 64 kbps | 2.02x | 160.70 | 160.68 | +7.9% |
| 96 kbps | 2.29x | 240.52 | 240.52 | +6.9% |
| 128 kbps | 2.06x | 320.36 | 320.36 | +11.1% |
| 192 kbps | 2.40x | 480.04 | 480.04 | +3.8% |
| 256 kbps | 2.32x | 639.72 | 639.72 | +7.5% |

Source CSVs:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

## Quality metrics vs official Opus

Quality proxy metrics were measured on the validation corpus used during development. Deltas are `opuscpp - official`.

| Bitrate | Current PESQ-style | Official PESQ-style | PESQ delta | Current ViSQOL-style | Official ViSQOL-style | ViSQOL delta | Current CELT proxy | Official CELT proxy | CELT delta |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 kbps | 1.4694 | 1.4487 | +0.0207 | 4.6367 | 4.6407 | -0.0040 | 0.6784 | 0.2182 | +0.4602 |
| 24 kbps | 1.4506 | 1.4367 | +0.0139 | 4.6480 | 4.6450 | +0.0030 | 85.8556 | 0.0315 | +85.8241 |
| 32 kbps | 1.4452 | 1.4365 | +0.0087 | 4.6545 | 4.6464 | +0.0081 | 88.8000 | 0.0315 | +88.7685 |
| 48 kbps | 1.4425 | 1.4360 | +0.0065 | 4.6541 | 4.6534 | +0.0007 | 90.5330 | 96.6402 | -6.1072 |
| 64 kbps | 1.4355 | 1.4360 | -0.0005 | 4.6546 | 4.6544 | +0.0002 | 89.8912 | 96.5798 | -6.6886 |
| 96 kbps | 1.4360 | 1.4361 | -0.0001 | 4.6572 | 4.6572 | +0.0000 | 91.6229 | 96.6386 | -5.0157 |
| 128 kbps | 1.4361 | 1.4363 | -0.0002 | 4.6591 | 4.6580 | +0.0011 | 92.3059 | 96.6286 | -4.3227 |
| 192 kbps | 1.4364 | 1.4366 | -0.0002 | 4.6597 | 4.6601 | -0.0004 | 95.7972 | 96.6216 | -0.8244 |
| 256 kbps | 1.4365 | 1.4363 | +0.0002 | 4.6600 | 4.6598 | +0.0002 | 96.5173 | 96.6373 | -0.1200 |

Source CSV:

- `metrics/quality_vs_official.csv`

## Detector mode-balance spot check

The lightweight detector added for this snapshot distinguishes spoken pitch from sustained harmonic/music pitch using pitch stability, envelope stability, zero-crossing rate, and low-order tonal markers before updating the voice estimate.

Representative AUDIO-mode results at 32 kbps mono:

| Material class | Hybrid | CELT |
|---|---:|---:|
| Speech-like | 99.2% | 0.8% |
| Acoustic harmonic music | 39.2% | 60.8% |
| Synthetic sustained harmonic | 0.0% | 100.0% |
| Full-song music | 27.3% | 72.7% |

## Memory metrics

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,864 B | 31,648 B | -46.7% |
| Encoder stereo | 32,448 B | 48,880 B | -33.6% |
| Decoder mono | 14,112 B | 18,336 B | -23.0% |
| Decoder stereo | 21,376 B | 27,408 B | -22.0% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured text+data |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 227,216 B | 0 B | 227,216 B |
| Android arm64 Clang `-O2` | 252,387 B | 800 B | 253,187 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC C++23 | Builds with zero warnings in measured configuration. |
| Android arm64 Clang C++23 | Builds with zero warnings in measured configuration. |
| Linux/macOS | Intended to build with any C++23 compiler; use `tests/run_smoke.py` for a quick local check. |
