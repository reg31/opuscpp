# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

## Quick start

### Option 1 - Run the full conformance and benchmark report in one command (recommended)

The commands below download the current `opuscpp` test bundle and run the full
official-comparison/report flow automatically. You can run them from any folder: they create an
`opuscpp-report` workspace in your current folder, run the report workflow there, and keep the
downloaded checkout and generated artifacts so you can inspect them afterward. The final Markdown
report is saved at `./opuscpp-report/full_report.md`.

macOS / Linux:

```bash
/bin/sh -c "$(curl -fsSL https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_full_report.sh)"
```

Windows PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/reg31/opuscpp/main/tests/scripts/run_full_report.ps1' -OutFile run_full_report.ps1; ./run_full_report.ps1"
```

These one-liners expect Python 3, a C++23 compiler, `git`, and `cmake` to already be installed and
available on your PATH. On Windows, add `-Cleanup` if you want the helper workspace removed at the
end.

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
2. Build official Opus 1.6.1 as a static library with intrinsics enabled at `-O2 -DNDEBUG`
   flags for the public benchmark comparison.
3. Build the `opuscpp` decoder harness.

## Optional local listening samples

Generate local listening samples:

```bash
python3 tests/generate_synthetic_wav.py --out tests/generated_audio
```

The generated files are ignored by git.

## RFC decode conformance

`RFC decode conformance` means the standard Opus decoder-vector check: decode the official RFC 6716
test vectors and the RFC 8251 update vectors, then compare the output against the reference PCM with
the official `opus_compare` acceptance criteria.

The RFC vector files are not committed to this repository. If you used
`tests/scripts/setup_official_compare.py`, you already have the recommended directory layout and
build outputs. To run full decode conformance manually, build the decoder harness with:

```bash
c++ -std=c++23 -O2 -DNDEBUG -I src \
    tests/conformance_decode.cpp src/opus_codec.cpp \
    -o build/conformance_decode
```

Then run each vector through `conformance_decode` and compare the generated PCM with the reference
decoded PCM using the official `opus_compare` tool from the Opus source tree.

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC 8251 updated decode vectors | 24/24 passed |
| Mono/stereo coverage | Passed |
| Final range check mode | Supported by harness |

## Encode interoperability validation

`Encode interoperability validation` is the project's encoder regression gate, not a separate IETF
RFC test. Opus encoders are not required to emit identical packets, so byte-for-byte packet
comparison would be the wrong test. Instead, the harness encodes generated validation cases with
`opuscpp` and verifies that official Opus 1.6.1 accepts and decodes those packets for the supported
scenarios. The relevant files are:

- `conformance_encode.cpp`
- `official_encode_validation.cpp`
- `encode_conformance_shared.h`

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC 8251 encode interoperability cases | 96/96 passed |
| Total encode interoperability cases | 96/96 passed |

## API behavior validation

Additional API-level validation checks exercise supported behavior that is not covered directly by
the RFC decode vectors:

| Check | Result |
|---|---:|
| Decoder channel remap | Passed |
| Packet-duration helper behavior | Passed |

## Perceptual and memory harness

`perceptual_memory_validation.cpp` compares this implementation with official Opus on generated or
user-provided 16-bit PCM WAV input. It reports:

- SNR, segmental SNR, RMS error, and mean absolute error.
- PESQ-style proxy score.
- ViSQOL-style proxy score.
- CELT-style high-band proxy score.
- Average payload bytes, reported publicly as effective bitrate.
- Encode time.
- Optional process memory measurements.

These proxy scores are useful for regression tracking, but they are not substitutes for official
PESQ/ViSQOL tooling or listening tests.

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus 1.6.1 is built with `-O2 -DNDEBUG` and x86
runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` uses the same pure C++23 `-O2 -DNDEBUG` profile, with no assembly and no SIMD intrinsics. Measurements
are from Windows MinGW GCC 16.1 on an AMD Ryzen 7 8845HS, using the repository's 8-second stereo
synthetic music-like benchmark. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. This keeps the optimization level matched while comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.364x | 1.691x | 786x | 333x | 2144x | 1268x |
| 24&nbsp;kbps | 1.760x | 1.299x | 525x | 298x | 1421x | 1094x |
| 32&nbsp;kbps | 1.724x | 1.256x | 512x | 297x | 1359x | 1081x |
| 48&nbsp;kbps | 1.541x | 1.225x | 460x | 299x | 1145x | 935x |
| 64&nbsp;kbps | 1.529x | 1.174x | 409x | 268x | 990x | 843x |
| 96&nbsp;kbps | 1.694x | 1.164x | 350x | 207x | 779x | 669x |
| 128&nbsp;kbps | 1.864x | 1.139x | 367x | 197x | 671x | 589x |
| 192&nbsp;kbps | 1.690x | 1.094x | 301x | 178x | 552x | 504x |
| 256&nbsp;kbps | 1.622x | 1.059x | 256x | 158x | 455x | 429x |



The source CSV for the published intrinsics speed table is tracked under `tests/metrics/`; local
refresh runs may also write temporary Markdown reports under `build/` or the working directory.

Source CSV:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

A supplemental real-time-factor snapshot is also tracked in
`metrics/speed_vs_official_intrinsics_60s.csv`.

## Quality metrics vs official Opus

AUDIO quality proxy metrics were measured on the synthetic music-like validation corpus used during
development. Deltas are `opuscpp - official`; positive is better for the proxy quality columns.
Effective bitrate columns show measured payload bitrate for the same validation run.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0039 | +0.0001 | +16.3393 | 16.0 kbps | 17.1 kbps |
| 24&nbsp;kbps | +0.0051 | +0.0430 | +24.6337 | 24.0 kbps | 25.2 kbps |
| 32&nbsp;kbps | +0.0013 | +0.0365 | +16.5267 | 32.0 kbps | 33.6 kbps |
| 48&nbsp;kbps | +0.0012 | +0.0126 | +0.6082 | 48.0 kbps | 48.6 kbps |
| 64&nbsp;kbps | +0.0007 | -0.0016 | +0.2112 | 64.0 kbps | 64.6 kbps |
| 96&nbsp;kbps | +0.0004 | +0.0040 | -0.0296 | 96.0 kbps | 96.7 kbps |
| 128&nbsp;kbps | +0.0010 | +0.0015 | -0.2095 | 128.0 kbps | 128.8 kbps |
| 192&nbsp;kbps | +0.0007 | +0.0005 | -0.1654 | 192.0 kbps | 192.9 kbps |
| 256&nbsp;kbps | +0.0008 | +0.0001 | +0.1183 | 256.0 kbps | 256.7 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0115 | +0.0076 | -0.0534 | 15.8 kbps | 16.3 kbps |
| 24&nbsp;kbps | +0.0109 | +0.0095 | -1.3103 | 23.9 kbps | 24.1 kbps |
| 32&nbsp;kbps | +0.0104 | +0.0084 | -0.5399 | 32.0 kbps | 32.2 kbps |
| 48&nbsp;kbps | +0.0139 | +0.0102 | -1.0647 | 47.8 kbps | 48.2 kbps |
| 64&nbsp;kbps | +0.0097 | +0.0062 | +0.1277 | 64.0 kbps | 64.5 kbps |
| 96&nbsp;kbps | -0.0001 | +0.0000 | -0.0849 | 96.0 kbps | 96.6 kbps |
| 128&nbsp;kbps | +0.0000 | +0.0012 | -0.1227 | 128.0 kbps | 128.5 kbps |
| 192&nbsp;kbps | +0.0003 | +0.0000 | +0.0400 | 192.0 kbps | 192.4 kbps |
| 256&nbsp;kbps | -0.0001 | +0.0001 | +0.0010 | 256.0 kbps | 256.4 kbps |

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

| Build | Text | Data | Total measured image (text+data+bss) |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 261,844 B | 8 B | 279,932 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.1 C++23 | Build check passed in the latest full report. |
| Android arm64 Clang C++23 | Build check passed in the latest full report. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |
