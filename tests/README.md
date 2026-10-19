# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

## Voice conditioning release regression

[`voice_conditioning_release.cpp`](voice_conditioning_release.cpp) checks that provisional and normal cues use the same falling-score envelope, provisional state expires, rumble/DC protection still attacks promptly, and clean input recovers after contamination. Its 133 explicit checks remain active with `-DNDEBUG`; the previous coefficient selection fails 61 checks specific to the provisional fall. The harness includes the implementation and builds on its own:

```sh
g++ -std=c++23 -O2 -DNDEBUG -Isrc tests/voice_conditioning_release.cpp -o voice_conditioning_release_test
```

## NSQ quantization-level regression

[`nsq_quant_levels.cpp`](nsq_quant_levels.cpp) checks every clamped residual, all four offset cells and 15 Lambda boundary/extreme values in both zero-pulse modes: 7,495,800 comparisons of all six output fields. It checks lookup bounds before access and passes a null table pointer on the zero-pulse path. The harness includes the implementation and builds on its own:

```sh
g++ -std=c++23 -O2 -DNDEBUG -Isrc tests/nsq_quant_levels.cpp -o nsq_quant_levels_test
```

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

- clone official Opus (upstream `main`),
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
2. Build official Opus (upstream `main`) as a static library with intrinsics enabled at `-O2 -DNDEBUG`
   flags for the public benchmark comparison.
3. Build the `opuscpp` decoder harness.

## Optional local listening samples

Generate local listening samples:

```bash
python3 tests/generate_synthetic_wav.py --out tests/generated_audio
```

The generated files are ignored by git.

The mono "voice-like" fixture is a voiced formant synthesis: a glottal impulse train with an
accumulated-phase pitch contour (so the instantaneous pitch stays near the intended 120-140 Hz
range) shaped by three formant resonators, then gated by a syllable-rate envelope with breath
noise mixed at a controlled 30 dB SNR. It is a deterministic stress fixture, not recorded speech;
use a real 48 kHz mono WAV when a speech-representative check is needed. The
phase-modulated two-tone fixture is still generated as `synthetic_tonal_stress_mono.wav` for
tonal-stress testing.

## Optional WER validation for speech-to-text

`run_wer_validation.py` is a speech-to-text validation tool for VOIP tuning. It encodes and decodes
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

Decode official RFC 6716 vectors updated by RFC 8251 and compare output with reference PCM using `opus_compare`. Vector files are downloaded by `tests/scripts/setup_official_compare.py`.

To build the decoder harness:

```bash
c++ -std=c++23 -O2 -DNDEBUG -I src \
    tests/conformance_decode.cpp src/opus_codec.cpp \
    -o build/conformance_decode
```

Coverage: 24 RFC 8251 updated decode vectors, mono/stereo; final-range checks are supported by the harness.

## Encode interoperability validation

The encoder may emit any valid Opus packet; validation checks that the official decoder accepts generated packets. Harness files: `conformance_encode.cpp`, `official_encode_validation.cpp`, and `encode_conformance_shared.h`.

Coverage: 96 generated encode-interoperability cases.

## Focused test commands

Focused internal transient-budget command:

```sh
c++ -std=c++23 -O2 -DNDEBUG -DOPUSCPP_ENABLE_TEST_HOOKS -I src \
    tests/hybrid_transient_budget.cpp src/opus_codec.cpp -o build/hybrid_transient_budget
```

FEC source-quality command (using current and official gate executables):

```text
python tests/scripts/check_fec_source_quality.py current_gate.exe official_gate.exe --output fec_source_results.json
```

## Perceptual and memory harness

`perceptual_memory_validation.cpp` accepts generated or user-provided 16-bit PCM WAV input and reports quality proxies, effective bitrate, encode time and optional process memory. Quality is delay-aligned and uses per-channel scoring.

Both positive and negative quality deltas are retained. Source hashes, flags and scope are
recorded in [run metadata](metrics/run_metadata.json).

## Speed metrics vs official Opus with x86 intrinsics

Both builds use `-O2 -DNDEBUG`; official Opus enables x86 intrinsics. Measurements use Windows MinGW GCC 16.2 / AMD Ryzen 7 8840HS and nine 60-second stereo runs. Ratios above `1.00x` indicate faster processing.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.023x | 1.879x | 348x | 340x | 2430x | 1293x |
| 24&nbsp;kbps | 1.073x | 1.349x | 349x | 325x | 1558x | 1155x |
| 32&nbsp;kbps | 1.064x | 1.325x | 346x | 325x | 1511x | 1141x |
| 48&nbsp;kbps | 1.061x | 1.251x | 317x | 299x | 1209x | 966x |
| 64&nbsp;kbps | 1.066x | 1.291x | 276x | 259x | 1076x | 834x |
| 96&nbsp;kbps | 1.005x | 1.309x | 214x | 213x | 857x | 655x |
| 128&nbsp;kbps | 1.023x | 1.304x | 194x | 190x | 750x | 575x |
| 192&nbsp;kbps | 0.966x | 1.258x | 170x | 176x | 629x | 500x |
| 256&nbsp;kbps | 0.972x | 1.174x | 163x | 167x | 526x | 448x |

## Quality metrics vs official Opus

AUDIO quality proxies compare both encoders at complexity10 on the same six-second synthetic music-like input. Deltas are `opuscpp - official`; positive is better. Effective bitrates are measured payload rates from the same validation run; decoding uses the default unfiltered output.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.5179 | +0.3285 | +2.1316 | 16.684 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.1668 | +0.0482 | +0.3881 | 24.683 kbps | 25.229 kbps |
| 32&nbsp;kbps | +0.1913 | +0.0248 | +0.3034 | 32.748 kbps | 33.613 kbps |
| 48&nbsp;kbps | +0.3764 | +0.0331 | -0.0370 | 48.560 kbps | 48.560 kbps |
| 64&nbsp;kbps | +0.4378 | +0.0306 | +0.0007 | 64.613 kbps | 64.613 kbps |
| 96&nbsp;kbps | +0.3462 | +0.0143 | +0.0186 | 96.720 kbps | 96.697 kbps |
| 128&nbsp;kbps | +0.2200 | +0.0034 | +0.0096 | 128.827 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0739 | +0.0043 | +0.0026 | 193.040 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0352 | +0.0051 | +0.0124 | 256.908 kbps | 256.737 kbps |

## VOIP quality metrics vs official Opus

VOIP uses a separate synthetic mono speech-like validation sample because its mode-selection semantics differ from AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1892 | +0.0303 | +0.1861 | 15.375 kbps | 12.072 kbps |
| 24&nbsp;kbps | +0.1960 | +0.0140 | -0.2404 | 24.375 kbps | 24.020 kbps |
| 32&nbsp;kbps | +0.2418 | +0.0136 | +0.3676 | 32.440 kbps | 32.088 kbps |
| 48&nbsp;kbps | +0.3390 | +0.0072 | +0.2259 | 48.431 kbps | 48.409 kbps |
| 64&nbsp;kbps | +0.5078 | +0.0103 | +0.4822 | 64.465 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.8553 | +0.0120 | +0.4089 | 96.400 kbps | 96.499 kbps |
| 128&nbsp;kbps | +0.9274 | +0.0066 | +0.3926 | 128.400 kbps | 128.489 kbps |
| 192&nbsp;kbps | +1.0999 | +0.0058 | +0.4274 | 192.400 kbps | 192.472 kbps |
| 256&nbsp;kbps | +1.1016 | +0.0057 | +0.4513 | 256.400 kbps | 256.464 kbps |

The voiced/formant fixture uses phase-integrated pitch and breath noise at 30 dB SNR; its complete fields and fixture metadata are in [quality_official_full_precision.csv](metrics/quality_official_full_precision.csv) and [quality_run_metadata.json](metrics/quality_run_metadata.json).

### Broader content check

Coverage includes the broad ladder, stereo/content holdouts, four mono speech recordings, and the retained tonal-stress fixture.

| Set | Comparisons | Negative PESQ-style | Negative ViSQOL-style | Negative CELT proxy |
|---:|---:|---:|---:|---:|
| Broad ladder | 99 | 4 | 7 | 20 |
| Stereo/content holdouts | 30 | 5 | 7 | 8 |
| Mono speech recordings | 36 | 0 | 0 | 0 |
| Retained tonal stress | 9 | 0 | 0 | 0 |

All signed deltas and effective bitrates are in [quality_broad.csv](metrics/quality_broad.csv).

### Optional speech denoiser

The optional encoder denoiser targets sustained broadband noise in mono VOIP capture and is disabled by default.

The on/off comparison uses mono speech mixed with sustained 6 dB white noise and is scored against clean speech after codec-delay alignment.

The 15.5/20 kbps cases have PESQ-style gains +0.1742/+0.2060 and ViSQOL-style gains +0.1235/+0.1231. The boundary benchmark passes 25/25 rates.

Across 246 on/off comparisons covering 41 clean/noisy/content conditions at 6 rates, 13 have negative PESQ-style deltas, 28 negative ViSQOL-style deltas, and 30 negative CELT-proxy deltas. All 12 fields, including other losses, are retained in `metrics/voice_denoise_broad.csv`.


Encode overhead includes downstream coding work; timings are medians of nine 60-second runs after one warm-up.

Optional denoiser state: 68 bytes; state/bounds/reset coverage: 90 configurations.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1898 | +0.1179 | 6.1% |
| 24&nbsp;kbps | +0.2194 | +0.1258 | 10.7% |
| 32&nbsp;kbps | +0.2046 | +0.1205 | 7.4% |
| 48&nbsp;kbps | +0.2182 | +0.1221 | 2.1% |
| 64&nbsp;kbps | +0.2197 | +0.1245 | 4.1% |
| 96&nbsp;kbps | +0.2131 | +0.1568 | 7.0% |
| 128&nbsp;kbps | +0.2125 | +0.1572 | 6.4% |
| 192&nbsp;kbps | +0.2155 | +0.1584 | 4.2% |
| 256&nbsp;kbps | +0.2156 | +0.1593 | 5.5% |

Sources: `metrics/voice_denoise_quality_voip.csv`, `metrics/voice_denoise_timing.csv`, `metrics/voice_denoise_boundary.csv`, and `metrics/voice_denoise_provenance.json`.

The state/bounds/reset command covers five sample rates, six frame durations and complexities0,5,10:

```bash
c++ -std=c++23 -O2 -DNDEBUG tests/voice_denoise_state.cpp -o build/voice_denoise_state
build/voice_denoise_state
```

## Memory metrics

Memory uses 256 instances across three processes with FEC and denoising disabled; values are private allocation deltas. [Run metadata](metrics/run_metadata.json).

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 20,480 B | 31,824 B | -35.6% |
| Encoder stereo | 30,656 B | 48,880 B | -37.3% |
| Decoder mono | 8,448 B | 18,480 B | -54.3% |
| Decoder stereo | 17,568 B | 27,088 B | -35.1% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 499,312 B | 0 B | 499,312 B |
| Android arm64 Clang `-O2` | 481,828 B | 472 B | 482,300 B |
