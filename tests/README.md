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

No WER/CER or external ASR endpoint was run for the 2026-09-05 refresh.

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
    --postfilter 3 \
    --max-average-wer 0.12 \
    --max-case-wer 0.30
```

The ASR command can be Azure, Whisper, Android Speech, or any local recognizer; it only needs to
print the recognized text to stdout. Use `OPUSCPP_ASR_COMMAND` instead of `--asr-command` if you
prefer environment configuration. Reports are written under `build/wer_validation/`.
`--gain-db` is applied before encoding and before optional noise injection; use it to validate quiet
voice robustness without needing separate low-volume source files.
Use `--postfilter 0` for the public decoder default or `--postfilter 3` to validate adaptive speech
postfilter output.

For regression gating, pass `--baseline tests/metrics/wer_results.json --max-wer-regression 0.02`.
Add `--update-baseline` only after listening/ASR review confirms the new result is better.

## RFC decode conformance

`RFC decode conformance` means the standard Opus decoder-vector check: decode the official RFC 6716
test vectors as updated by RFC 8251, then compare the output against the reference PCM with
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
| Encoder lookahead and restricted-low-delay behavior | Passed |
| VBR budget behavior | Passed |
| Guarded DTX behavior, refresh, and quiet-tonal protection | Passed |
| DTX active-content and re-entry comparison vs official Opus | Passed |
| In-band FEC encode/decode interoperability vs official Opus | Passed |
| LPC orders, CELT energy boundaries and guarded stereo-policy checks | Passed |
| Trapping UBSan: API, long frames and 290,909 malformed packets | Passed |

`dtx_vs_official.cpp` exercises voice, 20-LSB quiet voice, far-field and noisy speech, two
speakers, speech mixed with music, and fricative speech at 16/24&nbsp;kbps. The current deterministic
run records zero false DTX packets for both encoders across 1,680 active frames. Against the original
signal after a silence interval, `opuscpp` has lower aggregate wake-up NRMSE (`0.2930` vs `0.7588`)
and gain error (`0.7681` vs `1.6082` dB), while both suppress the same 406 silence frames. That is
approximately 61.4% less re-entry error and 52.2% less gain error than official Opus.

In everyday terms, re-entry is the moment speech or music returns after DTX stopped sending during
silence; lower error means a cleaner restart. Gain error measures whether that returning sound is
temporarily too loud or too quiet. This comparison is run automatically by the full-report scripts
above, with detailed output saved under `build/official_compare_report/api_behavior/`.

`fec_vs_official.cpp` enables FEC and a 15% expected-loss setting, drops one packet, recovers it
from the following packet, and then decodes that following packet normally. It checks nominal,
quiet, and noisy speech at mono 10/20/40/60 ms and stereo 20 ms, including VBR and CBR, in both
directions: `opuscpp` encoder to official decoder and official encoder to `opuscpp` decoder. In this
tracked recovery matrix, each carried `opuscpp` FEC frame must improve on ordinary packet-loss concealment.

Every normally decoded packet also checks that the encoder and official decoder finish with the same
entropy-coder state. This catches malformed payloads even when both decoders produce the same wrong
audio. Additional packet checks cover silent startup, speech-to-silence changes, and bitrate changes
while FEC is enabled.

Recovery error compares the reconstructed missing audio with normal, loss-free decoding of the
same encoded stream; lower means less damage from the lost packet. It does not measure total error
against the original recording. The aggregate score combines the tracked 10/20 ms scenarios before
comparing the two encoders. In the current run, `opuscpp` reconstructs audio more accurately in all
18 scenarios and reduces the combined error by 52.8%. It supplies recoverable backup audio in all 18
scenarios, compared with 15 for official Opus, while using 0.4% fewer packet bytes.

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

The harness now queries each encoder's `OPUS_GET_LOOKAHEAD`, flushes its delayed tail with silence,
and removes the delay before scoring or exporting listening WAVs. Flush packets are excluded from
the payload-bitrate and encode-time statistics. `perceptual_alignment.cpp` checks alignment and tail
recovery across both codecs, three applications, mono/stereo, and float/PCM16; the full-report script runs it automatically.
Spectral scores now compare each channel independently, with negative controls for stereo collapse
and channel swapping. The earlier mono downmix hid these errors. `--complexity 0..10` selects the
same encoder complexity for both codecs; the default is `10`. Raw quality output retains eight decimal places.

All numerical sections below were refreshed on 2026-09-05 against codec revision `34a5b76`.
The codec source is unchanged by this refresh. Correcting the earlier unaligned/downmixed
measurement exposes previously hidden losses as well as larger gains; it is not evidence that
this documentation update changed the sound. Negative results are retained.
Source hashes, flags and scope are recorded in [run metadata](metrics/run_metadata.json).

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus 1.6.1 is built with `-O2 -DNDEBUG` and x86
runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` uses the same pure C++23 `-O2 -DNDEBUG` profile, with no assembly and no SIMD intrinsics. Measurements
are from Windows MinGW GCC 16.2 on an AMD Ryzen 7 8845HS, using medians of nine repository
60-second stereo synthetic music-like benchmark runs. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. Each repetition changes the bitrate sweep order and alternates which implementation
runs first to reduce CPU boost and thermal-order bias. This keeps the optimization level matched while
comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.384x | 1.865x | 849x | 356x | 2442x | 1309x |
| 24&nbsp;kbps | 1.882x | 1.411x | 611x | 325x | 1620x | 1148x |
| 32&nbsp;kbps | 1.823x | 1.396x | 595x | 327x | 1582x | 1133x |
| 48&nbsp;kbps | 1.735x | 1.348x | 522x | 301x | 1314x | 974x |
| 64&nbsp;kbps | 1.757x | 1.313x | 469x | 267x | 1119x | 852x |
| 96&nbsp;kbps | 1.843x | 1.300x | 398x | 216x | 873x | 672x |
| 128&nbsp;kbps | 2.040x | 1.311x | 399x | 195x | 767x | 585x |
| 192&nbsp;kbps | 1.853x | 1.299x | 326x | 176x | 648x | 499x |
| 256&nbsp;kbps | 1.761x | 1.237x | 299x | 170x | 550x | 444x |


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
| 16&nbsp;kbps | +0.0006 | -0.0032 | +1.5898 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.1931 | +0.0753 | +0.1586 | 24.000 kbps | 25.220 kbps |
| 32&nbsp;kbps | +0.2572 | +0.0791 | -0.6145 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | -0.0237 | +0.0028 | -1.4751 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | -0.1168 | -0.0042 | -0.9377 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | -0.0475 | +0.0058 | -0.6239 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | -0.0037 | -0.0021 | -0.2709 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0067 | +0.0003 | -0.2310 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0244 | -0.0011 | -0.1239 | 256.000 kbps | 256.736 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1240 | -0.0002 | +1.8146 | 15.999 kbps | 16.255 kbps |
| 24&nbsp;kbps | +0.1472 | -0.0038 | +0.3763 | 24.000 kbps | 24.148 kbps |
| 32&nbsp;kbps | +0.1464 | -0.0026 | +0.4079 | 32.000 kbps | 32.184 kbps |
| 48&nbsp;kbps | +0.1446 | -0.0012 | +0.1011 | 48.000 kbps | 48.244 kbps |
| 64&nbsp;kbps | +0.1378 | -0.0050 | +0.0486 | 63.988 kbps | 64.501 kbps |
| 96&nbsp;kbps | +0.0000 | -0.0003 | +0.0850 | 96.000 kbps | 96.595 kbps |
| 128&nbsp;kbps | +0.0024 | -0.0040 | +0.0033 | 128.000 kbps | 128.503 kbps |
| 192&nbsp;kbps | +0.0005 | -0.0009 | -0.0009 | 192.000 kbps | 192.421 kbps |
| 256&nbsp;kbps | +0.0012 | -0.0001 | -0.0009 | 256.000 kbps | 256.415 kbps |

The aligned VOIP sample has negative ViSQOL-style deltas throughout this ladder; the PESQ-style
advantage at lower rates is not an all-metric win.

### Broader content check

The supplementary set contains 99 source/settings comparisons across speech, quiet/noisy speech,
music, tones and transients, plus 30 additional stereo/content holdout comparisons. They are
short-clip diagnostics, not a representative listening survey or 129 independent recordings.

| Set | Comparisons | Negative PESQ-style | Negative ViSQOL-style | Negative CELT proxy |
|---|---:|---:|---:|---:|
| Broad ladder | 99 | 32 | 45 | 58 |
| Stereo/content holdouts | 30 | 26 | 23 | 22 |

All signed deltas and effective bitrates are in `metrics/quality_broad.csv`. The main synthetic
table must not be generalized to a universal quality advantage.

### Optional speech denoiser

The optional encoder denoiser is intended for mono VOIP capture with sustained broadband noise. It
waits for consistent noise evidence, attenuates the affected frequency regions smoothly, and releases
gradually when the evidence disappears. This avoids applying a permanent low-pass filter to ordinary
speech. It is disabled by default and has no effect on non-VOIP applications or stereo input. It is
not intended for audio that has already been denoised.

The table compares denoising enabled and disabled on the same tracked mono speech sample mixed
with sustained 6 dB white noise, scoring the decoded output against clean speech after codec-delay
alignment. Across this ladder, PESQ-style gain is +0.0011 to +0.0084 and ViSQOL-style gain is
+0.0113 to +0.0310. Encode overhead is 3.0% to 5.3%, measured separately as the median of nine
60-second runs after warm-up, using the same noisy recording repeated.

This is not a universal denoising win. A wider 72-comparison check over 18 noise/content
conditions at 16/24/32/48&nbsp;kbps found one PESQ-style loss (-0.0024 on 12 dB white noise at
24&nbsp;kbps), no ViSQOL-style losses, and a tiny negative CELT-proxy delta on one babble case.
Clean/quiet speech, tonal and transient controls in that set remain unchanged. Full deltas are
in `metrics/voice_denoise_broad.csv`; do not interpret one noisy-speech table as a general guarantee.
The separate 20-rate boundary gate also fails at 15.5 and 20 kbps on 6 dB white noise (PESQ-style
losses of about 0.0024 and 0.0020). Its failures are retained in `metrics/voice_denoise_boundary.csv`;
the script still returns failure for them.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0011 | +0.0113 | 3.0% |
| 24&nbsp;kbps | +0.0051 | +0.0240 | 3.7% |
| 32&nbsp;kbps | +0.0042 | +0.0240 | 3.8% |
| 48&nbsp;kbps | +0.0072 | +0.0186 | 3.4% |
| 64&nbsp;kbps | +0.0071 | +0.0261 | 3.3% |
| 96&nbsp;kbps | +0.0084 | +0.0303 | 5.3% |
| 128&nbsp;kbps | +0.0083 | +0.0304 | 4.5% |
| 192&nbsp;kbps | +0.0083 | +0.0310 | 4.3% |
| 256&nbsp;kbps | +0.0084 | +0.0307 | 4.8% |

Source CSV: `metrics/voice_denoise_quality_voip.csv`.

### Optional speech postfilter

Adaptive postfilter mode (`3`) is a speech-smoothing option, not an automatic quality guarantee.
The corrected PCM16 comparison finds +0.1380 PESQ-style at 32&nbsp;kbps, but -0.0114 ViSQOL-style
and a lower CELT proxy. At 96-256&nbsp;kbps PESQ-style improves while ViSQOL-style decreases.
Default unfiltered output therefore remains the recommended baseline; audition optional smoothing.

Auto adds 3.2% to 9.8% end-to-end PCM16 decode time in this run and decodes 60 seconds in
0.046 to 0.075 seconds. Even where the samples are unchanged, entering the optional path and
updating its decisions/state can cost time. These timings use the actual public modes, not a
modified zero-gain decoder. Each mode is the median of nine isolated 60-second decodes after
warm-up; the quality sample is repeated to fill the minute and mode order rotates.

| Bitrate | PESQ-style gain from auto | ViSQOL-style gain from auto | PCM16 auto decode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0000 | +0.0000 | 5.0% |
| 24&nbsp;kbps | +0.0008 | +0.0001 | 5.6% |
| 32&nbsp;kbps | +0.1380 | -0.0114 | 9.8% |
| 48&nbsp;kbps | +0.0000 | +0.0000 | 3.5% |
| 64&nbsp;kbps | +0.0000 | +0.0000 | 3.2% |
| 96&nbsp;kbps | +0.0547 | -0.0071 | 5.9% |
| 128&nbsp;kbps | +0.0537 | -0.0091 | 5.8% |
| 192&nbsp;kbps | +0.0623 | -0.0116 | 4.6% |
| 256&nbsp;kbps | +0.0610 | -0.0135 | 3.6% |

Source CSVs:

- `metrics/postfilter_quality_voip.csv`
- `metrics/postfilter_pcm16_path.csv`

PCM16 time to decode 60 seconds (not time per frame):

| Bitrate | Off | Light | Stronger | Adaptive |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 49.936 ms | 53.749 ms | 54.033 ms | 52.454 ms |
| 24&nbsp;kbps | 56.509 ms | 61.171 ms | 61.053 ms | 59.673 ms |
| 32&nbsp;kbps | 60.004 ms | 63.917 ms | 64.022 ms | 65.879 ms |
| 48&nbsp;kbps | 64.688 ms | 69.003 ms | 68.583 ms | 66.972 ms |
| 64&nbsp;kbps | 68.256 ms | 71.902 ms | 72.590 ms | 70.464 ms |
| 96&nbsp;kbps | 43.673 ms | 45.495 ms | 46.288 ms | 46.230 ms |
| 128&nbsp;kbps | 53.893 ms | 56.320 ms | 56.595 ms | 57.015 ms |
| 192&nbsp;kbps | 62.200 ms | 63.675 ms | 64.954 ms | 65.053 ms |
| 256&nbsp;kbps | 72.624 ms | 74.463 ms | 74.984 ms | 75.233 ms |

Reproduce optional timing after building the comparison object/library:

```bash
c++ -std=c++23 -O2 -DNDEBUG -I tests tests/optional_processing_benchmark.cpp \
    build/official_compare_report/perceptual/curr_opus_codec.o \
    build/official_opus_o2_intrinsics_mingw/libopus.a -o build/optional_processing_benchmark
build/optional_processing_benchmark speech.wav noisy_speech.wav
```

Both inputs must be 48 kHz mono PCM16 WAV. The postfilter uses `speech.wav`; denoiser timing uses
`noisy_speech.wav`. Use the same inputs/settings as the quality check for a comparable result.
The full-report wrapper refreshes core comparisons; these optional-input measurements are separate.


## Memory metrics

The optional encoder FEC state is allocated lazily and is not included in this default-FEC-off
snapshot. These are process-private allocation deltas averaged over 256 instances, not exact
structure sizes or peak stack usage; allocator/page rounding contributes to small run-to-run differences.

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 16,928 B | 31,744 B | -46.7% |
| Encoder stereo | 32,192 B | 49,072 B | -34.4% |
| Decoder mono | 14,064 B | 18,400 B | -23.6% |
| Decoder stereo | 21,200 B | 27,408 B | -22.7% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 299,536 B | 0 B | 299,536 B |
| Android arm64 Clang `-O2` | 311,964 B | 472 B | 312,436 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.2 C++23 | Warning-free build in this run (`-Wall -Wextra -Wpedantic`). |
| Android arm64 Clang C++23 | Warning-free build in this run (`-Wall -Wextra -Wpedantic`). |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |
