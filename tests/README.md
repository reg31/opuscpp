# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

The sections below are separate measurement snapshots. The FEC and full-matrix quality checkpoint is identified in `metrics/fec_validated_checkpoint.json`; older speed, memory, optional-processing and quality tables retain their original measured revisions. Both encoders use complexity 10 and -O2 -DNDEBUG where specified, with official Opus intrinsics enabled. No historical table is relabelled as a measurement of the new checkpoint.

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
| Decoder channel remap | Passed |
| Packet-duration helper behavior | Passed |
| Overflow-safe encoder frame and packet-duration validation | Passed |
| Encoder lookahead and restricted-low-delay behavior | Passed |
| VBR budget behavior | Passed |
| Hybrid transient bit budget | Passed |
| Guarded DTX behavior, refresh, and quiet-tonal protection | Passed |
| DTX active-content and re-entry comparison vs official Opus | Passed |
| In-band FEC encode/decode interoperability vs official Opus | Passed |
| LPC orders, CELT energy boundaries and guarded stereo-policy checks | Passed |
| Trapping UBSan: API, long frames and 291,755 malformed packets | Passed |

`hybrid_transient_budget.cpp` pins the hybrid CELT bit target's transient response: the
`tf_estimate` term must move the target around the 0.25 pivot, and a strong transient
(`tf_estimate > 0.7`) must clear the 50-bit floor while a weak one stays below it. The function under
test is internal, so build with the test hook enabled:

```sh
c++ -std=c++23 -O2 -DNDEBUG -DOPUSCPP_ENABLE_TEST_HOOKS -I src \
    tests/hybrid_transient_budget.cpp src/opus_codec.cpp -o build/hybrid_transient_budget
```

`dtx_vs_official.cpp` exercises voice, 20-LSB quiet voice, far-field and noisy speech, two
speakers, speech mixed with music, and fricative speech at 16/24&nbsp;kbps. The current deterministic
run records zero false DTX packets for both encoders across 1,680 active frames. Against the original
signal after silence, `opuscpp` has lower aggregate wake-up NRMSE (`0.2855` vs `0.7622`)
and gain error (`0.7705` vs `1.6463` dB), while both suppress 406 silence frames.
That is 62.6% less re-entry error and 53.2% less gain error. The separate steady-noise-only case suppresses 120 frames with `opuscpp` versus 0 with official Opus.

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

Recovery error compares reconstructed missing audio with normal, loss-free decoding of the
same stream. The reviewed FEC checkpoint passes all 18 strict 10/20 ms cases: aggregate recovery
error ratio 0.461509 (53.8% lower than official), maximum per-case ratio 0.984130, backup
coverage 18/18 versus official 15/18, and packet-byte ratio 0.996333. The source identity and raw
criteria results are recorded in [FEC checkpoint metadata](metrics/fec_validated_checkpoint.json).

`fec_source_quality.cpp` separately compares recovered-frame fidelity, the following frame and
the boundary transition against the original source. All three errors are no greater than official
Opus on 18/18 cases, and recovered audio improves on this encoder's PLC on 18/18. Build it against
the same `curr_`-renamed codec object as `fec_vs_official.cpp`; build the reference binary with
`-DUSE_OFFICIAL_ENCODER` and the official library. Then run:

```text
python tests/scripts/check_fec_source_quality.py current_gate.exe official_gate.exe --output fec_source_results.json
```

The checkpoint also passes packet budgets, 240 API/reset cases, 96 encoder conformance cases,
packet-duration/channel-remap checks, SILK reconstruction, postfilter and denoiser checks.
`voip_quiet_start_latch.cpp` (compile with `-DOPUSCPP_ENABLE_TEST_HOOKS`) checks that silence
does not classify as quiet speech and that later loud input releases a quiet classification.
Complete startup independence is still open: the exact 64 kbps mode override and other mode
decisions require further review.

The full 498-configuration quality matrix has 1288 below-official fields, compared with 1580 at
parent commit `a4a1fde`: 360 fixed, 68 newly negative and 260 worsened existing deficits. All AUDIO
fields are unchanged; these differences are VOIP. This is an FEC-qualified checkpoint, not full
quality/performance parity. The separate CELT transient/history and LPC experiments remain
outside this checkpoint for individual review. Historical speed/memory tables below retain their
original measurement provenance and do not measure this new checkpoint.

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

The historical speed, memory, binary-size, AUDIO/VOIP quality, optional-processing and broader-corpus figures retain their recorded measurement source. The headline encoder comparisons use complexity 10.
Default output, optional processing and their input references remain separate comparisons.
Historical optimization/validation comparisons are explicitly labelled.
Both positive and negative quality deltas are retained. Source hashes, flags and scope are
recorded in [run metadata](metrics/run_metadata.json).

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus (upstream `main`) is built with `-O2 -DNDEBUG` and x86
runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` uses the same pure C++23 `-O2 -DNDEBUG` profile, with no assembly and no SIMD intrinsics. Measurements
are from Windows MinGW GCC 16.2 on an AMD Ryzen 7 8845HS, using medians of nine repository
60-second stereo synthetic music-like benchmark runs. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. Each repetition changes the bitrate sweep order and alternates which implementation
runs first to reduce CPU boost and thermal-order bias. This keeps the optimization level matched while
comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.780x | 1.826x | 617x | 347x | 2318x | 1269x |
| 24&nbsp;kbps | 1.667x | 1.411x | 531x | 318x | 1552x | 1100x |
| 32&nbsp;kbps | 1.612x | 1.399x | 515x | 319x | 1517x | 1084x |
| 48&nbsp;kbps | 1.519x | 1.338x | 439x | 289x | 1259x | 941x |
| 64&nbsp;kbps | 1.502x | 1.270x | 382x | 254x | 1052x | 828x |
| 96&nbsp;kbps | 1.480x | 1.198x | 312x | 211x | 781x | 652x |
| 128&nbsp;kbps | 1.414x | 1.199x | 271x | 191x | 684x | 571x |
| 192&nbsp;kbps | 1.287x | 1.214x | 224x | 174x | 594x | 490x |
| 256&nbsp;kbps | 1.229x | 1.193x | 204x | 166x | 526x | 441x |


The isolated production speed run is recorded in [speed_run_metadata.json](metrics/speed_run_metadata.json). Encoding is faster at 9/9 measured rates (1.23x to 1.78x); decoding is faster at 9/9 (1.19x to 1.83x).

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
| 16&nbsp;kbps | +0.0003 | -0.0012 | +1.8616 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.3784 | +0.0959 | -0.5008 | 24.000 kbps | 25.229 kbps |
| 32&nbsp;kbps | +0.5420 | +0.0929 | -0.6139 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | +0.1637 | +0.0132 | -0.6977 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | +0.1396 | +0.0052 | -0.9065 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | +0.2907 | +0.0127 | -0.4201 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | +0.1907 | +0.0042 | -0.2874 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0714 | +0.0030 | -0.1777 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0350 | +0.0022 | -0.0825 | 256.000 kbps | 256.737 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0894 | +0.0167 | +0.2670 | 15.997 kbps | 12.072 kbps |
| 24&nbsp;kbps | +0.1276 | +0.0013 | -0.4318 | 24.000 kbps | 24.020 kbps |
| 32&nbsp;kbps | +0.1958 | +0.0094 | +0.1623 | 32.000 kbps | 32.088 kbps |
| 48&nbsp;kbps | +0.0635 | -0.0127 | -0.1740 | 48.000 kbps | 48.409 kbps |
| 64&nbsp;kbps | -0.6057 | -0.0031 | +0.4079 | 63.963 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.0117 | +0.0028 | +0.0249 | 96.000 kbps | 96.499 kbps |
| 128&nbsp;kbps | -0.0089 | -0.0018 | -0.0092 | 128.000 kbps | 128.489 kbps |
| 192&nbsp;kbps | -0.0008 | -0.0004 | -0.0074 | 192.000 kbps | 192.472 kbps |
| 256&nbsp;kbps | +0.0012 | -0.0004 | -0.0008 | 256.000 kbps | 256.464 kbps |

The voiced/formant fixture uses phase-integrated pitch and breath noise at a controlled 30 dB SNR. Its ViSQOL-style delta improves at 4/9 rates; losses remain. The former phase-modulated fixture is retained as a separate tonal-stress case, not relabelled as speech. Do not compare new VOIP scores directly with the former fixture. The complete 12-field comparisons and complexity-9 control are in [quality_official_full_precision.csv](metrics/quality_official_full_precision.csv), with [configuration and fixture hashes](metrics/quality_run_metadata.json). Rounded zero in a table does not imply exact equality.

### Broader content check

Fresh comparisons cover the broad ladder, stereo/content holdouts, four mono speech recordings, and the retained tonal-stress fixture. These are short-clip diagnostics, not a representative listening survey.

| Set | Comparisons | Negative PESQ-style | Negative ViSQOL-style | Negative CELT proxy |
|---|---:|---:|---:|---:|
| Broad ladder | 99 | 15 | 38 | 64 |
| Stereo/content holdouts | 30 | 2 | 9 | 18 |
| Mono speech recordings | 36 | 1 | 8 | 16 |
| Retained tonal stress | 9 | 0 | 7 | 1 |

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

The 15.5/20 kbps cases have PESQ-style gains +0.1224/+0.1082 and ViSQOL-style gains +0.0917/+0.0811. The unchanged boundary gate passes 25/25 rates.

Across 246 on/off comparisons covering 41 clean/noisy/content conditions at six rates, 2 have negative PESQ-style deltas, 0 negative ViSQOL-style deltas, and 4 negative CELT-proxy deltas. Other diagnostic losses may also occur; retain the signed results in `metrics/voice_denoise_broad.csv`. Do not infer a speech-recognition improvement from these scores.

End-to-end encode overhead is **5.4% to 23.2%** on the tracked noisy recording. This includes changed downstream coding work, not just filter arithmetic. Timing runs in isolation, pinned to one logical CPU at above-normal priority; enabled/bypass order rotates. Values are medians of nine 60-second runs after one warm-up.

The optional state is 68 bytes. A 7.5 KiB temporary stack cache avoids repeating filter work for frames of up to 960 samples; longer frames recompute. The 90-configuration state/bounds/reset test passes with trapping sanitizers. Both toolchains build; unused tone-analysis warnings remain.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0876 | +0.0751 | 13.9% |
| 24&nbsp;kbps | +0.1712 | +0.0980 | 15.3% |
| 32&nbsp;kbps | +0.1997 | +0.1129 | 23.2% |
| 48&nbsp;kbps | +0.2160 | +0.1230 | 21.2% |
| 64&nbsp;kbps | +0.2200 | +0.1228 | 19.7% |
| 96&nbsp;kbps | +0.2106 | +0.1553 | 6.3% |
| 128&nbsp;kbps | +0.2107 | +0.1581 | 6.1% |
| 192&nbsp;kbps | +0.2136 | +0.1594 | 5.5% |
| 256&nbsp;kbps | +0.2142 | +0.1594 | 5.4% |

Sources: `metrics/voice_denoise_quality_voip.csv`, `metrics/voice_denoise_timing.csv`, `metrics/voice_denoise_boundary.csv`, and `metrics/voice_denoise_provenance.json`. The previous-version CSV is historical, not a current acceptance result.

The focused state/bounds/reset test covers five sample rates, six frame durations, and complexities 0, 5 and 10:

```bash
c++ -std=c++23 -O2 -DNDEBUG tests/voice_denoise_state.cpp -o build/voice_denoise_state
build/voice_denoise_state
```

## Memory metrics

The optional encoder FEC state is allocated lazily and is not included in this default-FEC-off
snapshot. These are median process-private allocation deltas from three fresh runs of 256 instances, not exact
structure sizes or peak stack usage; allocator/page rounding contributes to small run-to-run differences.

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 17,248 B | 31,712 B | -45.6% |
| Encoder stereo | 32,320 B | 48,880 B | -33.9% |
| Decoder mono | 14,176 B | 18,288 B | -22.5% |
| Decoder stereo | 21,248 B | 27,360 B | -22.3% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 309,308 B | 0 B | 309,308 B |
| Android arm64 Clang `-O2` | 316,156 B | 472 B | 316,628 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.2 C++23 | Build passes with `-Wall -Wextra -Wpedantic`; unused tone-analysis warnings remain. |
| Android arm64 Clang C++23 | Build passes with `-Wall -Wextra -Wpedantic`; unused tone-analysis warnings remain. |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |

## CELT empty-channel energy checkpoint

`src/opus_codec.cpp` (Git blob `db9040ab8c4422f3a89cde606a0ca06394a85160`) makes one change to CELT coarse-energy
coding. A channel that has no energy shape at all is no longer held above its real (empty) level by the coarse-energy
decay limiter, and when exactly one coded channel is shaped, that channel is coded as mid/side intensity from the first
band with independent dual stereo disabled. The baseline commit for every comparison in this section is `0d4d0fb`;
the change is isolated and independent of the other uncommitted work in the tree.

Measured results over the 498-case quality matrix (all tracked finite fields): 1594 -> 1580 negative fields versus
official Opus, 14 fields fixed, 0 newly negative, 33 existing deficits improved and 4 existing deficits worsened;
96 fields change across 8 stereo rows, and every mono row is identical. Strict FEC passes 18/18. Source-referenced
criteria and the complete failing-key list, the four worsened fields, harness and library identities and the explicit
limitations are recorded in `metrics/celt_empty_channel_checkpoint.json`. Ordinary checks pass: 96 encode-conformance
cases, VBR budget, and four changed-packet interop cases x 40 frames with final-range agreement. The regression test
`tests/celt_empty_channel.cpp` fails four one-sided stereo cases on `0d4d0fb` and passes with this change.

Build and run it directly against a codec source:

```
g++ -std=c++23 -O2 -DNDEBUG -I src tests/celt_empty_channel.cpp src/opus_codec.cpp -o celt_empty_channel.exe
celt_empty_channel.exe
```

## CELT stereo-to-mono predictor checkpoint

The encoder now merges the previous channel energies before coding a mono stream, matching
the decoder's predictor. `celt_mono_energy_history.cpp` covers four frame lengths and three
asymmetric histories: the previous code fails with mismatched predictor state; the fix passes
all 12 cases. Compile the test directly; it includes the codec source.

The isolated change on `535fed4` leaves all 5976 measured quality fields and strict FEC output
identical. All four source FEC criteria pass on 18/18; packet budgets, 240 API/reset cases,
96 conformance cases, packet-duration/channel-remap and changed-packet interoperability pass.
The broader quality and startup limitations of the FEC checkpoint still apply. Exact identities
are recorded in [predictor checkpoint metadata](metrics/celt_mono_energy_history_checkpoint.json).

## CELT transient-analysis checkpoint

The transient detector now uses the complete 128-entry official table and floating-point
normalization, removing fixed-point scaling that suppressed real attacks. The estimate calculation
also preserves the reference expression's floating-point types. The differential regression in
`celt_transient_analysis.cpp` compares all outputs exactly over 160 cases, including 44 detected
attacks: the previous implementation fails 64 cases; the corrected implementation passes all 160.
Compile `official_transient_analysis.c` as C using the configuration, include directories and flags
of the official CELT encoder, then link that object and the official library with the C++ test.

The full quality matrix improves from 1288 to 1233 below-official fields: 237 fixed, 182 newly
negative and 525 worsened existing deficits. This corrects an upstream parity bug and yields a net
quality improvement; individual regressions remain open. Strict FEC and all four source criteria
still pass 18/18, along with the packet-budget and ordinary integration checks.

Sequential nine-repeat VOIP measurements still show a Hazel encoding deficit at 16–48 kbps
(0.567–0.620x official speed); David encoding is faster at all nine measured rates (1.089–2.673x).
Decode ratios exceed 1x on both inputs. These use the existing 13.46-second Hazel and 11.8-second
David PCM, with both codecs at complexity 10 and -O2 -DNDEBUG; no explicit affinity/priority pinning.
See [transient checkpoint metadata](metrics/celt_transient_checkpoint.json) for exact source
identities, complete timing rows and quality counts. Full metric parity remains unfinished.
