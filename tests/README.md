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
| VBR budget behavior | Passed |
| Guarded DTX behavior, refresh, and quiet-tonal protection | Passed |
| DTX active-content and re-entry comparison vs official Opus | Passed |

`dtx_vs_official.cpp` exercises voice, 20-LSB quiet voice, far-field and noisy speech, two
speakers, speech mixed with music, and fricative speech at 16/24&nbsp;kbps. The current deterministic
run records zero false DTX packets for both encoders across 1,680 active frames. Against the original
signal after a silence interval, `opuscpp` has lower aggregate wake-up NRMSE (`0.7254` vs `0.7588`)
and gain error (`1.4588` vs `1.6082` dB), while both suppress the same 406 silence frames. That is
approximately 4.4% less re-entry error and 9.3% less gain error than official Opus.

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
are from Windows MinGW GCC 16.1 on an AMD Ryzen 7 8845HS, using medians of three repository
60-second stereo synthetic music-like benchmark runs. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. This keeps the optimization level matched while comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.277x | 1.814x | 689x | 303x | 2025x | 1116x |
| 24&nbsp;kbps | 1.646x | 1.351x | 522x | 317x | 1507x | 1116x |
| 32&nbsp;kbps | 1.629x | 1.324x | 522x | 320x | 1459x | 1101x |
| 48&nbsp;kbps | 1.603x | 1.246x | 466x | 291x | 1185x | 952x |
| 64&nbsp;kbps | 1.619x | 1.234x | 419x | 259x | 1020x | 826x |
| 96&nbsp;kbps | 1.704x | 1.241x | 355x | 208x | 804x | 648x |
| 128&nbsp;kbps | 1.956x | 1.094x | 372x | 190x | 623x | 570x |
| 192&nbsp;kbps | 1.697x | 1.185x | 288x | 169x | 581x | 490x |
| 256&nbsp;kbps | 1.675x | 1.126x | 275x | 164x | 496x | 440x |


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
The harness uses the public decoder default: unfiltered output. The CELT proxy excludes the first
unprimed 10 ms of codec startup and scores the remaining steady-state windows.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0073 | +0.0013 | +0.4863 | 16.0 kbps | 17.1 kbps |
| 24&nbsp;kbps | +0.0017 | +0.0267 | +1.0525 | 24.0 kbps | 25.2 kbps |
| 32&nbsp;kbps | +0.0089 | +0.0424 | +0.6391 | 32.0 kbps | 33.6 kbps |
| 48&nbsp;kbps | +0.0012 | +0.0126 | +0.0019 | 48.0 kbps | 48.6 kbps |
| 64&nbsp;kbps | +0.0011 | +0.0050 | +0.0017 | 64.0 kbps | 64.6 kbps |
| 96&nbsp;kbps | +0.0003 | +0.0042 | +0.0009 | 96.0 kbps | 96.7 kbps |
| 128&nbsp;kbps | +0.0010 | +0.0015 | +0.0026 | 128.0 kbps | 128.8 kbps |
| 192&nbsp;kbps | +0.0007 | +0.0005 | +0.0029 | 192.0 kbps | 192.9 kbps |
| 256&nbsp;kbps | +0.0008 | +0.0001 | +0.0045 | 256.0 kbps | 256.7 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0083 | +0.0010 | +0.0609 | 16.0 kbps | 16.3 kbps |
| 24&nbsp;kbps | +0.0019 | +0.0050 | +0.0841 | 24.0 kbps | 24.1 kbps |
| 32&nbsp;kbps | +0.0005 | +0.0017 | +0.0447 | 32.0 kbps | 32.2 kbps |
| 48&nbsp;kbps | +0.0042 | +0.0017 | +0.0293 | 48.0 kbps | 48.2 kbps |
| 64&nbsp;kbps | +0.0075 | +0.0008 | +0.0228 | 64.0 kbps | 64.5 kbps |
| 96&nbsp;kbps | +0.0019 | +0.0002 | +0.0127 | 96.0 kbps | 96.6 kbps |
| 128&nbsp;kbps | +0.0022 | +0.0009 | +0.0132 | 128.0 kbps | 128.5 kbps |
| 192&nbsp;kbps | +0.0009 | +0.0001 | +0.0114 | 192.0 kbps | 192.4 kbps |
| 256&nbsp;kbps | +0.0007 | +0.0002 | +0.0050 | 256.0 kbps | 256.4 kbps |

### Optional speech postfilter A/B

Adaptive postfilter mode (`3`) is useful for speech, not a universal quality switch. It runs only
where the focused A/B gate found a worthwhile benefit. On tracked mono VOIP at 16/32&nbsp;kbps,
PESQ-style improves by `+0.0258`/`+0.0255` and ViSQOL-style by `+0.0053`/`+0.0017`; continuous
mono decode overhead measured about 7%/37% while active. Other tested mono rates and CELT-only
stereo bypass the filter, while hybrid stereo VOIP at 24/32&nbsp;kbps retains positive PESQ- and
ViSQOL-style deltas. Auto mode now applies to both PCM16 and float decoding. The public default and
headline metrics remain unfiltered.

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
| Host MinGW GCC `-O2` | 295,488 B | 0 B | 295,488 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.1 C++23 | Build check passed in the latest full report. |
| Android arm64 Clang C++23 | Build check passed in the latest full report. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |
