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

## Optional WER validation for speech-to-text

`run_wer_validation.py` is a speech-to-text oriented gate for VOIP tuning. It encodes and decodes
48 kHz PCM16 speech samples, optionally attenuates quiet-talker cases, adds deterministic noise at
several SNR levels, runs an ASR command on the decoded WAVs, then reports WER/CER against the
reference transcript.

Create a manifest like `tests/wer_manifest.example.json` with exact transcripts, then run:

```bash
python3 tests/scripts/run_wer_validation.py \
    --manifest tests/wer_manifest.json \
    --asr-command "python3 my_asr.py {wav}" \
    --bitrate 16000,24000,32000,48000 \
    --application voip \
    --gain-db 0,-12,-24 \
    --snr-db clean,20,10,5,0 \
    --max-average-wer 0.12 \
    --max-case-wer 0.30
```

The ASR command can be Azure, Whisper, Android Speech, or any local recognizer; it only needs to
print the recognized text to stdout. Use `OPUSCPP_ASR_COMMAND` instead of `--asr-command` if you
prefer environment configuration. Reports are written under `build/wer_validation/`.
`--gain-db` is applied before encoding and before optional noise injection; use it to validate quiet
voice robustness without needing separate low-volume source files.

For regression gating, pass `--baseline tests/metrics/wer_results.json --max-wer-regression 0.02`.
Add `--update-baseline` only after listening/ASR review confirms the new result is better.

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
| Overflow-safe encoder frame and packet-duration validation | Passed |
| VBR budget behavior | Passed |
| Guarded DTX behavior, refresh, and quiet-tonal protection | Passed |
| DTX active-content and re-entry comparison vs official Opus | Passed |

`dtx_vs_official.cpp` exercises voice, 20-LSB quiet voice, far-field and noisy speech, two
speakers, speech mixed with music, and fricative speech at 16/24&nbsp;kbps. The current deterministic
run records zero false DTX packets for both encoders across 1,680 active frames. Against the original
signal after a silence interval, `opuscpp` has lower aggregate wake-up NRMSE (`0.6692` vs `0.7588`)
and gain error (`0.5756` vs `1.6082` dB), while both suppress the same 406 silence frames. That is
approximately 11.8% less re-entry error and 64.2% less gain error than official Opus.

In everyday terms, re-entry is the moment speech or music returns after DTX stopped sending during
silence; lower error means a cleaner restart. Gain error measures whether that returning sound is
temporarily too loud or too quiet. This comparison is run automatically by the full-report scripts
above, with detailed output saved under `build/official_compare_report/api_behavior/`.

## Perceptual and memory harness

`perceptual_memory_validation.cpp` compares this implementation with official Opus on generated or
user-provided 16-bit PCM WAV input. It reports:

- SNR, segmental SNR, RMS error, and mean absolute error.
- PESQ-style proxy score.
- ViSQOL-style proxy score.
- CELT-style masked spectral proxy score, with a roughly -60 dBFS audibility floor so spectral nulls do not dominate.
- Average payload bytes, reported publicly as effective bitrate.
- Encode time.
- Optional process memory measurements.

These proxy scores are useful for regression tracking, but they are not substitutes for official
PESQ/ViSQOL tooling or listening tests.

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus 1.6.1 is built with `-O2 -DNDEBUG` and x86
runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` uses the same pure C++23 `-O2 -DNDEBUG` profile, with no assembly and no SIMD intrinsics. Measurements
are from Windows MinGW GCC 16.1 on an AMD Ryzen 7 8845HS, using medians of nine repository
60-second stereo synthetic music-like benchmark runs. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. Each repetition changes the bitrate sweep order and alternates which implementation
runs first to reduce CPU boost and thermal-order bias. This keeps the optimization level matched while
comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.297x | 1.791x | 634x | 276x | 1705x | 952x |
| 24&nbsp;kbps | 1.785x | 1.276x | 476x | 267x | 1094x | 857x |
| 32&nbsp;kbps | 1.943x | 1.258x | 482x | 248x | 1052x | 837x |
| 48&nbsp;kbps | 1.722x | 1.287x | 419x | 243x | 944x | 733x |
| 64&nbsp;kbps | 1.791x | 1.322x | 378x | 211x | 849x | 643x |
| 96&nbsp;kbps | 1.882x | 1.211x | 316x | 168x | 612x | 505x |
| 128&nbsp;kbps | 1.992x | 1.285x | 311x | 156x | 568x | 442x |
| 192&nbsp;kbps | 1.818x | 1.228x | 259x | 143x | 467x | 380x |
| 256&nbsp;kbps | 1.768x | 1.227x | 233x | 132x | 407x | 331x |


The full-report script refreshes the tracked source CSVs under `tests/metrics/` and writes the
generated Markdown report under `build/` or the requested working-directory path.

Source CSV:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

A supplemental real-time-factor snapshot is also tracked in
`metrics/speed_vs_official_intrinsics_60s.csv`.

## Quality metrics vs official Opus

AUDIO quality proxy metrics were measured on the synthetic music-like validation corpus used during
development. Deltas are `opuscpp - official`; positive is better for the proxy quality columns.
Effective bitrate columns show measured payload bitrate for the same validation run.
The harness uses the public decoder default: unfiltered output. The CELT proxy excludes the first
unprimed 10 ms of codec startup and scores the remaining steady-state windows.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0059 | +0.0024 | +0.4116 | 16.0 kbps | 17.1 kbps |
| 24&nbsp;kbps | +0.0148 | +0.0332 | +1.0422 | 24.0 kbps | 25.2 kbps |
| 32&nbsp;kbps | +0.0211 | +0.0404 | +0.6725 | 32.0 kbps | 33.6 kbps |
| 48&nbsp;kbps | +0.0006 | +0.0142 | +0.0032 | 48.0 kbps | 48.6 kbps |
| 64&nbsp;kbps | +0.0075 | +0.0055 | +0.0262 | 64.0 kbps | 64.6 kbps |
| 96&nbsp;kbps | +0.0002 | +0.0035 | +0.0013 | 96.0 kbps | 96.7 kbps |
| 128&nbsp;kbps | +0.0010 | +0.0015 | +0.0026 | 128.0 kbps | 128.8 kbps |
| 192&nbsp;kbps | +0.0007 | +0.0005 | +0.0029 | 192.0 kbps | 192.9 kbps |
| 256&nbsp;kbps | +0.0008 | +0.0001 | +0.0045 | 256.0 kbps | 256.7 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0083 | +0.0010 | +0.0609 | 16.0 kbps | 16.3 kbps |
| 24&nbsp;kbps | +0.0097 | +0.0057 | +0.1416 | 24.0 kbps | 24.1 kbps |
| 32&nbsp;kbps | +0.0092 | +0.0069 | +0.0678 | 32.0 kbps | 32.2 kbps |
| 48&nbsp;kbps | +0.0123 | +0.0073 | +0.0436 | 48.0 kbps | 48.2 kbps |
| 64&nbsp;kbps | +0.0128 | +0.0039 | +0.0277 | 64.0 kbps | 64.5 kbps |
| 96&nbsp;kbps | +0.0019 | +0.0002 | +0.0127 | 96.0 kbps | 96.6 kbps |
| 128&nbsp;kbps | +0.0022 | +0.0009 | +0.0132 | 128.0 kbps | 128.5 kbps |
| 192&nbsp;kbps | +0.0009 | +0.0001 | +0.0114 | 192.0 kbps | 192.4 kbps |
| 256&nbsp;kbps | +0.0007 | +0.0002 | +0.0050 | 256.0 kbps | 256.4 kbps |

All tracked AUDIO and VOIP PESQ-style, ViSQOL-style, and CELT-style proxy deltas are positive in the
current validation run.

### Optional speech postfilter A/B

Adaptive postfilter mode (`3`) is useful for speech, not a universal quality switch. It runs only
where the focused A/B gate found a worthwhile benefit. On tracked mono VOIP at 16/32&nbsp;kbps,
PESQ-style improves by `+0.0258`/`+0.0255` and ViSQOL-style by `+0.0053`/`+0.0017`; continuous
mono decode overhead measured between 7% and 37% while active. Other tested mono rates and CELT-only
stereo bypass the filter, while hybrid stereo VOIP at 24/32&nbsp;kbps retains positive PESQ- and
ViSQOL-style deltas. Auto mode applies to both PCM16 and float decoding. The public default and
headline metrics remain unfiltered.

## Memory metrics

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,976 B | 31,696 B | -46.4% |
| Encoder stereo | 32,192 B | 49,072 B | -34.4% |
| Decoder mono | 14,096 B | 18,288 B | -22.9% |
| Decoder stereo | 21,184 B | 27,440 B | -22.8% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 280,280 B | 0 B | 280,280 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.1 C++23 | Build check passed in the latest full report. |
| Android arm64 Clang C++23 | Build check passed in the latest full report. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |
