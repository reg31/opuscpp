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
use a real 48 kHz mono WAV when a speech-representative check is needed. The earlier
phase-modulated two-tone fixture is still generated as `synthetic_tonal_stress_mono.wav` for
historical comparison.

## Optional WER validation for speech-to-text

No WER/CER or external ASR endpoint was run for the published benchmark refresh.

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

## Fresh compatibility and API checks

Fresh checks were run against source `0310bad` and official Opus `503d81b`. RFC decode result: 24/24; encode interoperability: 96/96. API behavior and selected source-bound regression outputs are recorded in [fresh compatibility metadata](metrics/compatibility_validation.json). No sanitizer was run.

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
`opuscpp` and verifies that official Opus accepts and decodes those packets for the supported
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
| Decoder channel remap | Passed in fresh source-bound check |
| Packet-duration helper behavior | Passed in fresh source-bound check |
| Overflow-safe encoder frame and packet-duration validation | Passed in fresh source-bound check |
| Encoder lookahead and restricted-low-delay behavior | Passed in fresh source-bound check |
| VBR budget behavior (CELT-run budgets; all-mode packet bounds and decoded duration) | Passed in fresh source-bound check |
| Hybrid transient bit budget | Not run by the full-refresh API harness |
| Guarded DTX behavior, refresh, and quiet-tonal protection | Passed in fresh source-bound check |
| DTX active-content and re-entry comparison vs official Opus | FAIL (unchanged criteria) |
| In-band FEC encode/decode interoperability vs official Opus | Passed in fresh source-bound check |
| Public packet decoder acceptance/comparison | PASS in fresh source-bound check; 16 configurations/3,200 frames; exact public returns/final ranges and RFC 6716 opus_compare; details in `metrics/lpc_analysis_known_failure.json` |
| Independent bit helpers, seed wrap, Schur, SILK LPC orders and CELT PLC | Passed in fresh source-bound check; details in `metrics/compatibility_validation.json` |
| Separate CELT energy boundaries and guarded stereo-policy tests | Passed in fresh source-bound check |
| Trapping UBSan: API, long frames and malformed packets | Not run in this refresh; sanitizer run excluded by scope |

`hybrid_transient_budget.cpp` pins the hybrid CELT bit target's transient response: the
`tf_estimate` term must move the target around the 0.25 pivot, and a strong transient
(`tf_estimate > 0.7`) must clear the 50-bit floor while a weak one stays below it. The function under
test is internal, so build with the test hook enabled:

```sh
c++ -std=c++23 -O2 -DNDEBUG -DOPUSCPP_ENABLE_TEST_HOOKS -I src \
    tests/hybrid_transient_budget.cpp src/opus_codec.cpp -o build/hybrid_transient_budget
```

`dtx_vs_official.cpp` exercises voice, 20-LSB quiet voice, far-field and noisy speech, two
speakers, speech mixed with music, and fricative speech at 16/24&nbsp;kbps. The current deterministic run records 0 false DTX packets for opuscpp versus 0 for official Opus across 1,680 active frames. Against the original signal after silence, wake-up NRMSE is 0.4416 versus 0.7622, and gain error is 1.6560 versus 1.6463 dB; silence-frame suppression is 406 versus 406. Aggregate re-entry NRMSE is 42.1% lower and gain error is 0.6% higher. The steady-noise-only case suppresses 120 frames with opuscpp versus 0 with official Opus. Individual per-material gain errors remain mixed; all current values are in `metrics/dtx_metrics.csv`. DTX comparison: FAIL. The original acceptance criteria are unchanged; all measurements are retained. Per-case rows and both exit records are in [DTX acceptance metadata](metrics/dtx_acceptance.json).

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

Fresh FEC results for the scored 18-case 10/20ms subset: 18/18 recovery wins; aggregate recovery ratio 0.499237; packet-byte ratio 0.996333. Source-quality criteria: C1 18/18, C2 18/18, C3 17/18, C4 18/18. Strict source gate: FAIL; standard interop gate: PASS. Standard-summary backup coverage is 18/18 for opuscpp versus 15/18 for official Opus. The expanded run covers 36 configurations and 72 direction rows; current values are in [FEC metadata](metrics/fec_run_metadata.json).

`fec_source_quality.cpp` separately compares recovered-frame fidelity, the following frame and boundary transition against the original source. Current source-quality counts are C1 18/18, C2 18/18, C3 17/18, C4 18/18; all recorded cases are retained. Full current/official case values are in [the source-quality JSON](metrics/fec_source_metrics.json).
To reproduce the current source-quality check, use the same `curr_`-renamed codec object as `fec_vs_official.cpp`; build the reference binary with
`-DUSE_OFFICIAL_ENCODER` and the official library. Then run:

```text
python tests/scripts/check_fec_source_quality.py current_gate.exe official_gate.exe --output fec_source_results.json
```

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

The full benchmark tables below were refreshed on 2026-10-10 for source `0310bad` (SHA-256 `5d3bd4f5ee68427a3fc5b082bb3888ef02e1bdfe6d6c34c5ce57d3f2585032de`) against official Opus `503d81b`.
Both positive and negative quality deltas are retained. Source hashes, flags and scope are
recorded in [run metadata](metrics/run_metadata.json).

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus (upstream `main`) is built with `-O2 -DNDEBUG` and x86
runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` uses the same pure C++23 `-O2 -DNDEBUG` profile, with no assembly and no SIMD intrinsics. Measurements
are from Windows MinGW GCC 16.2 on an AMD Ryzen 7 8840HS, using medians of nine repository
60-second stereo synthetic music-like benchmark runs. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. Each repetition changes the bitrate sweep order and alternates which implementation
runs first to reduce CPU boost and thermal-order bias. This keeps the optimization level matched while
comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 0.955x | 1.756x | 339x | 355x | 2302x | 1310x |
| 24&nbsp;kbps | 0.959x | 1.321x | 311x | 325x | 1521x | 1151x |
| 32&nbsp;kbps | 0.954x | 1.296x | 312x | 327x | 1480x | 1142x |
| 48&nbsp;kbps | 0.997x | 1.284x | 297x | 298x | 1243x | 968x |
| 64&nbsp;kbps | 1.026x | 1.209x | 271x | 264x | 1027x | 849x |
| 96&nbsp;kbps | 1.067x | 1.146x | 230x | 216x | 769x | 672x |
| 128&nbsp;kbps | 1.033x | 1.169x | 201x | 194x | 688x | 589x |
| 192&nbsp;kbps | 0.973x | 1.185x | 171x | 176x | 581x | 491x |
| 256&nbsp;kbps | 0.950x | 1.173x | 161x | 169x | 521x | 444x |

The isolated production speed run is recorded in [speed_run_metadata.json](metrics/speed_run_metadata.json). Encoding is faster at 3/9 measured AUDIO rates (0.95x to 1.07x); decoding at 9/9 (1.15x to 1.76x).

The full-report script refreshes the tracked source CSVs under `tests/metrics/` and writes the
generated Markdown report under `build/` or the requested working-directory path.

Source CSV:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

The same speed run, including raw encode/decode durations used for real-time factors, is tracked in
`metrics/speed_vs_official_intrinsics_60s.csv`.

## Quality metrics vs official Opus

AUDIO quality proxy metrics use the current encoder and official Opus, both at complexity 10, on the same six-second synthetic music-like input. Deltas are `opuscpp - official`; positive is better for the proxy quality columns.
Effective bitrate columns show measured payload bitrate for the same validation run.
The harness uses the public decoder default: unfiltered output. The CELT proxy excludes the first
unprimed 10 ms of codec startup and scores the remaining steady-state windows.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0022 | -0.0045 | +0.3945 | 17.032 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.0233 | +0.0182 | +0.3837 | 25.247 kbps | 25.229 kbps |
| 32&nbsp;kbps | +0.0524 | -0.0039 | -1.2356 | 33.464 kbps | 33.613 kbps |
| 48&nbsp;kbps | +0.1992 | +0.0260 | -0.0366 | 48.560 kbps | 48.560 kbps |
| 64&nbsp;kbps | +0.2926 | +0.0234 | -0.0090 | 64.613 kbps | 64.613 kbps |
| 96&nbsp;kbps | +0.2818 | +0.0160 | +0.0246 | 96.720 kbps | 96.697 kbps |
| 128&nbsp;kbps | +0.1593 | +0.0052 | -0.0363 | 128.827 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0693 | +0.0048 | +0.0051 | 193.033 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0342 | +0.0029 | +0.0072 | 256.528 kbps | 256.737 kbps |

## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1323 | +0.0034 | +0.1180 | 12.263 kbps | 12.072 kbps |
| 24&nbsp;kbps | +0.2588 | +0.0205 | +0.0572 | 24.005 kbps | 24.020 kbps |
| 32&nbsp;kbps | +0.3299 | +0.0219 | +0.4903 | 32.116 kbps | 32.088 kbps |
| 48&nbsp;kbps | +0.1979 | -0.0003 | +0.1062 | 48.349 kbps | 48.409 kbps |
| 64&nbsp;kbps | +0.3576 | +0.0041 | +0.0031 | 64.477 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.7636 | +0.0102 | +0.3973 | 96.400 kbps | 96.499 kbps |
| 128&nbsp;kbps | +0.8311 | +0.0049 | +0.3870 | 128.400 kbps | 128.489 kbps |
| 192&nbsp;kbps | +1.0997 | +0.0054 | +0.4274 | 192.400 kbps | 192.472 kbps |
| 256&nbsp;kbps | +1.1014 | +0.0056 | +0.4505 | 256.400 kbps | 256.464 kbps |

The voiced/formant fixture uses phase-integrated pitch and breath noise at a controlled 30 dB SNR. Its ViSQOL-style delta improves at 8/9 rates; losses remain. The former phase-modulated fixture is retained as a separate tonal-stress case, not relabelled as speech. Do not compare new VOIP scores directly with the former fixture. The complete 12-field comparisons and complexity-9 control are in [quality_official_full_precision.csv](metrics/quality_official_full_precision.csv), with [configuration and fixture hashes](metrics/quality_run_metadata.json). Rounded zero in a table does not imply exact equality.

### Broader content check

Fresh comparisons cover the broad ladder, stereo/content holdouts, four mono speech recordings, and the retained tonal-stress fixture. These are short-clip diagnostics, not a representative listening survey.

| Set | Comparisons | Negative PESQ-style | Negative ViSQOL-style | Negative CELT proxy |
|---:|---:|---:|---:|---:|
| Broad ladder | 99 | 3 | 9 | 27 |
| Stereo/content holdouts | 30 | 4 | 6 | 10 |
| Mono speech recordings | 36 | 0 | 0 | 0 |
| Retained tonal stress | 9 | 0 | 0 | 0 |

All signed deltas and effective bitrates are in `metrics/quality_broad.csv`. These results do not establish a universal quality advantage.

### Optional speech denoiser

The optional encoder denoiser targets sustained broadband noise in mono VOIP capture.
After confirming a near-flat noise spectrum, it keeps the background-noise estimate separate
from speech activity. Current-frame band energy sets the attenuation: suppression ramps in,
but gain recovers immediately at speech onsets so consonants are not faded in late.
Capture-noise reduction runs before the codec's existing signal shaping. Uncertain material
retains the conservative path. No FFT, extra look-ahead, or per-frame heap allocation is added.
It remains disabled by default and has no effect on stereo or non-VOIP applications.

These measurements compare denoising on versus off on the same mono recording mixed with sustained 6 dB white noise, scored against clean speech after codec-delay alignment. They are internal proxies, not certified PESQ or official ViSQOL.

The 15.5/20 kbps cases have PESQ-style gains +0.1742/+0.2060 and ViSQOL-style gains +0.1235/+0.1231. The boundary benchmark passes 25/25 rates.

Across 246 on/off comparisons covering 41 clean/noisy/content conditions at 6 rates, 13 have negative PESQ-style deltas, 28 negative ViSQOL-style deltas, and 30 negative CELT-proxy deltas. All 12 fields, including other losses, are retained in `metrics/voice_denoise_broad.csv`.


End-to-end encode overhead is **3.5% to 9.5%** on the tracked noisy recording. This includes changed downstream coding work, not just filter arithmetic. Timing runs in isolation, pinned to one logical CPU at above-normal priority; enabled/bypass order rotates. Values are medians of nine 60-second runs after one warm-up.

The optional denoiser state is 68 bytes; the fresh state/bounds/reset check passed 90 configurations. Temporary stack high-water was not measured in this refresh.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1883 | +0.1311 | 3.7% |
| 24&nbsp;kbps | +0.2252 | +0.1349 | 4.3% |
| 32&nbsp;kbps | +0.2068 | +0.1171 | 4.8% |
| 48&nbsp;kbps | +0.2194 | +0.1213 | 5.2% |
| 64&nbsp;kbps | +0.2139 | +0.1332 | 3.5% |
| 96&nbsp;kbps | +0.2122 | +0.1558 | 7.1% |
| 128&nbsp;kbps | +0.2116 | +0.1584 | 5.7% |
| 192&nbsp;kbps | +0.2149 | +0.1590 | 9.5% |
| 256&nbsp;kbps | +0.2157 | +0.1594 | 7.1% |

Sources: `metrics/voice_denoise_quality_voip.csv`, `metrics/voice_denoise_timing.csv`, `metrics/voice_denoise_boundary.csv`, and `metrics/voice_denoise_provenance.json`. The previous-version CSV is historical, not a current acceptance result.

The focused state/bounds/reset test covers five sample rates, six frame durations, and complexities 0, 5 and 10:

```bash
c++ -std=c++23 -O2 -DNDEBUG tests/voice_denoise_state.cpp -o build/voice_denoise_state
build/voice_denoise_state
```

## Memory metrics

This table is from the full refresh for source `0310bad`: all encoder and decoder rows used the same 256-instance, three-process memory-only run with FEC and denoising disabled. Process-private deltas are not exact structure sizes; allocator/page rounding can affect results. See [run metadata](metrics/run_metadata.json).
| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 19,760 B | 31,856 B | -38.0% |
| Encoder stereo | 29,888 B | 48,880 B | -38.9% |
| Decoder mono | 8,432 B | 18,464 B | -54.3% |
| Decoder stereo | 17,712 B | 27,088 B | -34.6% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 399,852 B | 0 B | 399,852 B |
| Android arm64 Clang `-O2` | 397,440 B | 472 B | 397,912 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.2 C++23 | Fresh `-std=c++23 -O2 -DNDEBUG` compile passed; command and compiler output are retained in run metadata. |
| Android arm64 Clang C++23 | Fresh `-std=c++23 -O2 -DNDEBUG` compile passed; command and compiler output are retained in run metadata. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |
