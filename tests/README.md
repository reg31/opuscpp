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
- build it as a static comparison build with intrinsics enabled,
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
2. Build official Opus 1.6.1 as a static library with intrinsics enabled and matching `-O2 -DNDEBUG` flags for the public benchmark comparison.
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

Then run each vector through `conformance_decode` and compare the generated PCM with the reference decoded PCM using the official `opus_compare` tool from the Opus source tree.

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC decode vectors | 24/24 passed |
| Mono/stereo coverage | Passed |
| Final range check mode | Supported by harness |

## Encode interoperability validation

`Encode interoperability validation` is the project's encoder regression gate, not a separate IETF RFC test. Opus encoders are not required to emit identical packets, so byte-for-byte packet comparison would be the wrong test. Instead, the harness encodes generated validation cases with `opuscpp` and verifies that official Opus 1.6.1 accepts and decodes those packets for the supported scenarios. The relevant files are:

- `conformance_encode.cpp`
- `official_encode_oracle.cpp`
- `encode_conformance_shared.h`

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC 8251 encode interoperability cases | 96/96 passed |
| Total encode interoperability cases | 96/96 passed |

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

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus 1.6.1 is built with `-O2 -DNDEBUG` and x86 runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` remains the same pure C++23 build with no assembly and no SIMD intrinsics. Measurements are from Windows MinGW GCC 16.1 on an AMD Ryzen 7 8845HS, using the repository's 8-second stereo synthetic music-like benchmark. A value above `1.00x` means `opuscpp` is faster than the optimized official build. This keeps optimization level and `NDEBUG` matched while comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.594x | 1.486x | 504x | 316x | 1785x | 1202x |
| 24&nbsp;kbps | 1.733x | 1.139x | 495x | 286x | 1176x | 1032x |
| 32&nbsp;kbps | 1.723x | 1.111x | 473x | 275x | 1138x | 1024x |
| 48&nbsp;kbps | 1.548x | 1.078x | 422x | 273x | 992x | 920x |
| 64&nbsp;kbps | 1.527x | 1.048x | 387x | 254x | 843x | 804x |
| 96&nbsp;kbps | 1.540x | 1.016x | 311x | 202x | 587x | 578x |
| 128&nbsp;kbps | 1.527x | 1.016x | 281x | 184x | 538x | 530x |
| 192&nbsp;kbps | 1.436x | 1.003x | 231x | 161x | 477x | 475x |
| 256&nbsp;kbps | 1.410x | 1.004x | 228x | 162x | 436x | 434x |

The source CSV for the published intrinsics speed table is tracked under `tests/metrics/`; local refresh runs may also write temporary Markdown reports under `build/` or the working directory.

Source CSV:

- `metrics/speed_vs_official_intrinsics_60s.csv`
- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

## Quality metrics vs official Opus

Quality proxy metrics were measured on the validation corpus used during development. Deltas are `opuscpp - official`; positive is better for the proxy quality columns and negative means smaller packets.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | Packet bytes vs official |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0000 | -0.0185 | +1.3037 | -2.8% |
| 24&nbsp;kbps | -0.0072 | +0.0411 | +22.0159 | -2.9% |
| 32&nbsp;kbps | +0.0007 | +0.0351 | +16.8861 | -3.3% |
| 48&nbsp;kbps | +0.0010 | +0.0131 | +0.3921 | +0.0% |
| 64&nbsp;kbps | +0.0010 | +0.0057 | +0.2358 | +0.0% |
| 96&nbsp;kbps | +0.0002 | +0.0015 | -0.1403 | -0.3% |
| 128&nbsp;kbps | +0.0006 | +0.0022 | -0.2914 | -0.3% |
| 192&nbsp;kbps | +0.0003 | -0.0001 | -0.2242 | -0.3% |
| 256&nbsp;kbps | +0.0005 | -0.0005 | -0.1322 | -0.1% |

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
| Host MinGW GCC `-O2` | 253,156 B | 0 B | 253,156 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.1 C++23 | Build check passed in the latest full report. |
| Android arm64 Clang C++23 | Build check passed in the latest full report. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use `tests/run_smoke.py` or `tests/scripts/run_smoke.sh` for a quick local check. |
