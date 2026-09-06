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

No WER/CER or external ASR endpoint was run for the 2026-09-06 refresh.

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
signal after silence, `opuscpp` has lower aggregate wake-up NRMSE (`0.2930` vs `0.7588`)
and gain error (`0.7681` vs `1.6082` dB), while both suppress 406 silence frames.
That is approximately 61.4% less re-entry error and 52.2% less gain error. The separate
steady-noise-only case differs: `opuscpp` suppresses 120 frames versus 0 for official Opus.

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

The current metric tables below were refreshed on 2026-09-06 against codec revision `7538c9b`.
Default output, optional processing and their input references remain separate comparisons.
Historical optimization/validation comparisons are explicitly labelled.
Both positive and negative quality deltas are retained. Source hashes, flags and scope are
recorded in [run metadata](metrics/run_metadata.json).

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
| 16&nbsp;kbps | 2.362x | 1.872x | 848x | 359x | 2456x | 1312x |
| 24&nbsp;kbps | 1.892x | 1.422x | 617x | 326x | 1641x | 1154x |
| 32&nbsp;kbps | 1.833x | 1.393x | 607x | 331x | 1588x | 1141x |
| 48&nbsp;kbps | 1.744x | 1.362x | 526x | 302x | 1324x | 972x |
| 64&nbsp;kbps | 1.772x | 1.330x | 472x | 266x | 1133x | 852x |
| 96&nbsp;kbps | 1.840x | 1.312x | 398x | 216x | 882x | 673x |
| 128&nbsp;kbps | 2.060x | 1.315x | 404x | 196x | 771x | 587x |
| 192&nbsp;kbps | 1.855x | 1.313x | 330x | 178x | 650x | 495x |
| 256&nbsp;kbps | 1.771x | 1.246x | 302x | 170x | 557x | 447x |


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
|---:|---:|---:|---:|---:|
| Broad ladder | 99 | 32 | 45 | 58 |
| Stereo/content holdouts | 30 | 26 | 23 | 22 |

All signed deltas and effective bitrates are in `metrics/quality_broad.csv`. The main synthetic
table must not be generalized to a universal quality advantage.

### Optional speech denoiser

The optional encoder denoiser targets sustained broadband noise in mono VOIP capture.
After confirming a near-flat noise spectrum, it keeps the background-noise estimate separate
from speech activity. Current-frame band energy sets the attenuation: suppression ramps in,
but gain recovers immediately at speech onsets so consonants are not faded in late.
Capture-noise reduction runs before the codec's existing signal shaping. Uncertain material
retains the conservative path. No FFT, extra look-ahead, or per-frame heap allocation is added.
It remains disabled by default and has no effect on stereo or non-VOIP applications.

These denoiser measurements were refreshed on **2026-09-06** as part of the full report.
The table compares denoising on versus off on the same mono speech recording mixed with sustained
6 dB white noise. Decoded output is scored against clean speech after codec-delay alignment.
These are internal quality proxies, not certified PESQ or official ViSQOL scores.

The previous 15.5/20 kbps failures are fixed: PESQ-style deltas are now **+0.1174/+0.1063**
and ViSQOL-style deltas **+0.0875/+0.0775** versus denoising off. All **25** rates in
`metrics/voice_denoise_boundary.csv` pass the unchanged non-negative quality gate; no
bitrate-specific bypass or score tolerance was introduced.

Across **246 comparisons** (29 tracked conditions and 12 additional voice/noise scenarios,
each at six rates), no tested quality component regressed versus the previous denoiser.
This is still not a universal improvement over bypass: two additional voice/noise cases
retain their previous small PESQ-style losses (about -0.00005 at 15.5 kbps and -0.00293 at
16 kbps). Some other diagnostic deltas versus bypass also remain negative. Both the bypass
comparison and the previous-version comparison are retained, without clamping, in
`metrics/voice_denoise_broad.csv` and `metrics/voice_denoise_vs_previous.csv`.
Do not infer a speech-recognition improvement from these proxy scores.

The active broadband filter's earlier exact-output optimization reduced its isolated kernel time
by about 20%; that historical comparison is recorded separately in the provenance. The fresh
end-to-end overhead versus denoising off is **4.4% to 26.1%** on this recording.
This includes downstream SILK work changed by filtering, not just the denoiser's arithmetic.

Timing runs without concurrent test workloads. Enabled/bypass order rotates in one process,
pinned to one logical CPU at above-normal priority. Values are medians of nine 60-second runs
after one warm-up; the six-second recording is repeated. The cache uses **7.5 KiB of temporary
stack**, not additional per-stream heap memory; longer frames retain filter recomputation.
Earlier exact-output checks cover **836 configurations / 195,008 packets** and **22,680 kernel
PCM/state comparisons**. This full refresh reruns the public 90-configuration state/bounds/reset
test with sanitizers, and both host and Android builds have zero warnings.
The optional filter state shrinks from **72 to 68 bytes**. Default-off packets remain
identical in the 198-configuration comparison.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0993 | +0.0785 | 11.4% |
| 24&nbsp;kbps | +0.1684 | +0.0952 | 14.5% |
| 32&nbsp;kbps | +0.2037 | +0.1213 | 21.0% |
| 48&nbsp;kbps | +0.2102 | +0.1162 | 26.1% |
| 64&nbsp;kbps | +0.2275 | +0.1327 | 23.9% |
| 96&nbsp;kbps | +0.2143 | +0.1577 | 7.6% |
| 128&nbsp;kbps | +0.2143 | +0.1582 | 7.3% |
| 192&nbsp;kbps | +0.2176 | +0.1593 | 5.0% |
| 256&nbsp;kbps | +0.2182 | +0.1598 | 4.4% |

Sources: `metrics/voice_denoise_quality_voip.csv`, `metrics/voice_denoise_timing.csv`,
and `metrics/voice_denoise_provenance.json`.

The focused state/bounds/reset test exercises five sample rates, six frame durations,
and complexities 0, 5 and 10. Build it as one translation unit:

```bash
c++ -std=c++23 -O2 -DNDEBUG tests/voice_denoise_state.cpp -o build/voice_denoise_state
build/voice_denoise_state
```

### Optional speech postfilter

Adaptive postfilter mode (`3`) is a speech-smoothing option, not an automatic quality guarantee.
The current PCM16 comparison finds +0.1380 PESQ-style at 32&nbsp;kbps, but
-0.0114 ViSQOL-style and a lower CELT proxy. Default unfiltered output
remains the baseline; audition optional smoothing rather than assuming every metric improves.

Auto adds 1.7% to 10.3% end-to-end PCM16 decode time and decodes 60 seconds in
0.046 to 0.075 seconds. Even unchanged samples can involve optional-path
decision/state work. These timings use actual public modes, not a modified zero-gain decoder.
Each value is the median of nine isolated decodes after warm-up; mode order rotates.

| Bitrate | PESQ-style gain from auto | ViSQOL-style gain from auto | PCM16 auto decode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0000 | +0.0000 | 4.7% |
| 24&nbsp;kbps | +0.0008 | +0.0001 | 3.6% |
| 32&nbsp;kbps | +0.1380 | -0.0114 | 10.3% |
| 48&nbsp;kbps | +0.0000 | +0.0000 | 5.1% |
| 64&nbsp;kbps | +0.0000 | +0.0000 | 1.7% |
| 96&nbsp;kbps | +0.0547 | -0.0071 | 5.3% |
| 128&nbsp;kbps | +0.0537 | -0.0091 | 5.0% |
| 192&nbsp;kbps | +0.0623 | -0.0116 | 4.2% |
| 256&nbsp;kbps | +0.0610 | -0.0135 | 3.7% |

Source CSVs:

- `metrics/postfilter_quality_voip.csv`
- `metrics/postfilter_pcm16_path.csv`

PCM16 time to decode 60 seconds (not time per frame):

| Bitrate | Off | Light | Stronger | Adaptive |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 49.898 ms | 53.539 ms | 53.943 ms | 52.221 ms |
| 24&nbsp;kbps | 56.222 ms | 59.872 ms | 59.906 ms | 58.228 ms |
| 32&nbsp;kbps | 59.373 ms | 63.755 ms | 63.749 ms | 65.498 ms |
| 48&nbsp;kbps | 64.761 ms | 68.687 ms | 68.653 ms | 68.096 ms |
| 64&nbsp;kbps | 67.713 ms | 71.788 ms | 72.591 ms | 68.889 ms |
| 96&nbsp;kbps | 43.584 ms | 45.854 ms | 45.985 ms | 45.912 ms |
| 128&nbsp;kbps | 53.503 ms | 56.124 ms | 56.291 ms | 56.157 ms |
| 192&nbsp;kbps | 61.821 ms | 63.900 ms | 65.647 ms | 64.420 ms |
| 256&nbsp;kbps | 72.265 ms | 73.858 ms | 75.108 ms | 74.909 ms |

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
snapshot. These are median process-private allocation deltas from three fresh runs of 256 instances, not exact
structure sizes or peak stack usage; allocator/page rounding contributes to small run-to-run differences.

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 17,248 B | 31,712 B | -45.6% |
| Encoder stereo | 32,192 B | 49,072 B | -34.4% |
| Decoder mono | 14,176 B | 18,288 B | -22.5% |
| Decoder stereo | 21,168 B | 27,456 B | -22.9% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 302,188 B | 0 B | 302,188 B |
| Android arm64 Clang `-O2` | 313,336 B | 472 B | 313,808 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.2 C++23 | Warning-free build in this run (`-Wall -Wextra -Wpedantic`). |
| Android arm64 Clang C++23 | Warning-free build in this run (`-Wall -Wextra -Wpedantic`). |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |
