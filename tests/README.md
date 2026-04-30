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
2. Build official Opus 1.6.1 as a static library with intrinsics disabled if you want a matched portable-C comparison.
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
| 16 kbps | 1.21x | 41.37 | 42.24 | +23.2% |
| 24 kbps | 1.37x | 61.15 | 62.52 | +5.2% |
| 32 kbps | 1.33x | 81.20 | 83.25 | +9.8% |
| 48 kbps | 1.25x | 121.30 | 121.28 | +7.2% |
| 64 kbps | 1.30x | 161.40 | 161.39 | -4.1% |
| 96 kbps | 1.50x | 241.00 | 241.56 | +2.4% |
| 128 kbps | 1.56x | 321.00 | 321.75 | -0.3% |
| 192 kbps | 1.54x | 481.00 | 482.13 | +0.4% |
| 256 kbps | 1.49x | 641.00 | 642.12 | +1.4% |

Source CSVs:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

## Quality metrics vs official Opus

Quality proxy metrics were measured on the validation corpus used during development. Deltas are `opuscpp - official`.

| Bitrate | Current PESQ-style | Official PESQ-style | PESQ delta | Current ViSQOL-style | Official ViSQOL-style | ViSQOL delta | Current CELT proxy | Official CELT proxy | CELT delta |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 kbps | 1.5557 | 1.5560 | -0.0003 | 4.7626 | 4.7780 | -0.0153 | 14.8028 | 13.4990 | +1.3037 |
| 24 kbps | 1.4544 | 1.4629 | -0.0085 | 4.7537 | 4.7259 | +0.0279 | 25.4104 | 3.3945 | +22.0159 |
| 32 kbps | 1.4342 | 1.4347 | -0.0005 | 4.7688 | 4.7449 | +0.0239 | 24.1309 | 7.2448 | +16.8861 |
| 48 kbps | 1.4345 | 1.4343 | +0.0001 | 4.7802 | 4.7773 | +0.0029 | 19.3872 | 19.0003 | +0.3869 |
| 64 kbps | 1.4355 | 1.4344 | +0.0011 | 4.7843 | 4.7871 | -0.0028 | 18.8411 | 18.6503 | +0.1908 |
| 96 kbps | 1.4344 | 1.4348 | -0.0004 | 4.7944 | 4.7955 | -0.0011 | 18.4624 | 18.6757 | -0.2133 |
| 128 kbps | 1.4349 | 1.4344 | +0.0005 | 4.7978 | 4.7969 | +0.0009 | 18.4787 | 18.7170 | -0.2383 |
| 192 kbps | 1.4346 | 1.4345 | +0.0001 | 4.7991 | 4.8000 | -0.0009 | 18.3900 | 18.6836 | -0.2936 |
| 256 kbps | 1.4346 | 1.4345 | +0.0001 | 4.8002 | 4.8019 | -0.0017 | 18.3728 | 18.5904 | -0.2176 |

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
| Encoder mono | 17,200 B | 31,632 B | -45.6% |
| Encoder stereo | 32,448 B | 48,880 B | -33.6% |
| Decoder mono | 14,192 B | 18,272 B | -22.3% |
| Decoder stereo | 21,376 B | 27,408 B | -22.0% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured text+data |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 227,064 B | 0 B | 227,064 B |
| Android arm64 Clang `-O2` | 252,219 B | 800 B | 253,019 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC C++23 | Builds with zero warnings in measured configuration. |
| Android arm64 Clang C++23 | Builds with zero warnings in measured configuration. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use `tests/run_smoke.py` for a quick local check. |
