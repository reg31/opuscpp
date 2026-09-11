# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

The production speed and headline AUDIO/VOIP quality tables are refreshed directly against official Opus (upstream `main`, commit `503d81b`) at complexity 10. Memory and binary-size figures are the current tracked snapshots. Both positive and negative quality deltas are retained, and the remaining deficits are listed in the quality sections.

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

The speed table uses the current production source at complexity 10; memory and binary size are the current tracked snapshots. The AUDIO and VOIP quality tables use fresh direct comparisons against official Opus; optional-processing and broader-corpus results retain their explicitly stated scope.
Default output, optional processing and their input references remain separate comparisons.
Historical optimization/validation comparisons are explicitly labelled.
Both positive and negative quality deltas are retained. Source hashes, flags and scope are
recorded in [run metadata](metrics/run_metadata.json).

## Speed metrics vs official Opus with x86 intrinsics

This is the public benchmark comparison: official Opus (upstream `main`, commit `503d81b`) is built with `-O2 -DNDEBUG` and x86
runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`). `opuscpp` uses the same pure C++23 `-O2 -DNDEBUG` profile, with no assembly and no SIMD intrinsics. Measurements
are from Windows MinGW GCC 16.2 on an AMD Ryzen 7 8845HS, using medians of nine repository
60-second stereo synthetic music-like benchmark runs. A value above `1.00x` means `opuscpp` is faster than the optimized
official build. Each repetition changes the bitrate sweep order and alternates which implementation
runs first to reduce CPU boost and thermal-order bias. This keeps the optimization level matched while
comparing against the optimized official desktop path most users would actually get.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | opuscpp encode real-time | Official encode real-time | opuscpp decode real-time | Official decode real-time |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.162x | 1.859x | 210x | 181x | 1244x | 669x |
| 24&nbsp;kbps | 0.976x | 1.468x | 161x | 165x | 837x | 570x |
| 32&nbsp;kbps | 0.944x | 1.372x | 156x | 165x | 797x | 581x |
| 48&nbsp;kbps | 0.855x | 1.319x | 129x | 151x | 664x | 504x |
| 64&nbsp;kbps | 0.873x | 1.318x | 117x | 134x | 567x | 430x |
| 96&nbsp;kbps | 0.998x | 1.310x | 109x | 109x | 443x | 338x |
| 128&nbsp;kbps | 0.972x | 1.321x | 95x | 98x | 388x | 294x |
| 192&nbsp;kbps | 0.869x | 1.278x | 78x | 89x | 323x | 253x |
| 256&nbsp;kbps | 0.804x | 1.234x | 69x | 86x | 282x | 228x |


The isolated production speed run is recorded in [speed_run_metadata.json](metrics/speed_run_metadata.json). In this default-VBR benchmark the encoder is within 0.80x-1.00x of official across the tracked bitrates (1.16x at 16&nbsp;kbps). The optional complexity-10 allocation/history search is gated on constrained VBR and is not exercised by this default-VBR workload; when active it adds encoder work, and default complexity 9 never runs it.

The full-report script refreshes the tracked source CSVs under `tests/metrics/` and writes the
generated Markdown report under `build/` or the requested working-directory path.

Source CSV:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

A supplemental real-time-factor snapshot is also tracked in
`metrics/speed_vs_official_intrinsics_60s.csv`.

## Quality metrics vs official Opus

AUDIO quality proxy metrics use the current encoder and official Opus, both at complexity 10, on the same six-second synthetic music-like input. Deltas are `opuscpp - official`; positive is better for the proxy quality columns.
Effective bitrate columns show measured payload bitrate for the same validation run.
The harness uses the public decoder default: unfiltered output. The CELT proxy excludes the first
unprimed 10 ms of codec startup and scores the remaining steady-state windows.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0006 | -0.0029 | +1.7605 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.2621 | +0.0748 | +0.5315 | 24.000 kbps | 25.229 kbps |
| 32&nbsp;kbps | +0.3236 | +0.0849 | +0.2059 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | +0.1185 | +0.0144 | -0.0252 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | +0.0211 | +0.0056 | +0.0384 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | +0.0876 | +0.0107 | +0.0327 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | +0.1915 | +0.0041 | +0.0190 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0762 | +0.0029 | +0.0074 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0408 | +0.0016 | +0.0084 | 256.000 kbps | 256.737 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1254 | -0.0000 | +0.9883 | 15.999 kbps | 16.249 kbps |
| 24&nbsp;kbps | +0.1451 | -0.0051 | +0.4338 | 24.000 kbps | 24.145 kbps |
| 32&nbsp;kbps | +0.1449 | -0.0039 | +0.4285 | 32.000 kbps | 32.185 kbps |
| 48&nbsp;kbps | +0.1482 | -0.0071 | +0.3884 | 48.000 kbps | 48.245 kbps |
| 64&nbsp;kbps | +0.1409 | -0.0089 | +0.0373 | 63.985 kbps | 64.496 kbps |
| 96&nbsp;kbps | +0.0019 | +0.0050 | +0.0514 | 96.000 kbps | 96.621 kbps |
| 128&nbsp;kbps | +0.0040 | +0.0008 | +0.0313 | 128.000 kbps | 128.551 kbps |
| 192&nbsp;kbps | +0.0007 | -0.0004 | -0.0024 | 192.000 kbps | 192.424 kbps |
| 256&nbsp;kbps | +0.0008 | -0.0003 | +0.0025 | 256.000 kbps | 256.415 kbps |

The aligned VOIP sample has negative ViSQOL-style deltas at seven of nine rates; 96 and 128 kbps are positive. All 18 AUDIO/VOIP rows still have at least one adverse quality field. The complete 12-field comparisons, including the separate complexity-9 control, are retained in [quality_official_full_precision.csv](metrics/quality_official_full_precision.csv), with [source and configuration metadata](metrics/quality_run_metadata.json).

### Broader content check

This supplementary snapshot predates the allocation/history changes and has not been refreshed; it is not a current acceptance result. The set contains 99 source/settings comparisons across speech, quiet/noisy speech,
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

These denoiser measurements are part of the published full-report snapshot.
The table compares denoising on versus off on the same mono speech recording mixed with sustained
6 dB white noise. Decoded output is scored against clean speech after codec-delay alignment.
These are internal quality proxies, not certified PESQ or official ViSQOL scores.

The previous 15.5/20 kbps failures are fixed: PESQ-style deltas are now **+0.1175/+0.1064**
and ViSQOL-style deltas **+0.0876/+0.0775** versus denoising off. All **25** rates in
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
end-to-end overhead versus denoising off is **4.0% to 22.8%** on this recording.
This includes downstream SILK work changed by filtering, not just the denoiser's arithmetic.

Timing runs without concurrent test workloads. Enabled/bypass order rotates in one process,
pinned to one logical CPU at above-normal priority. Values are medians of nine 60-second runs
after one warm-up; the six-second recording is repeated. The cache uses **7.5 KiB of temporary
stack**, not additional per-stream heap memory; longer frames retain filter recomputation.
Earlier exact-output checks cover **836 configurations / 195,008 packets** and **22,680 kernel
PCM/state comparisons**. This full refresh reruns the public 90-configuration state/bounds/reset
test with sanitizers, and both host and Android builds have zero warnings.
The optional filter state is **68 bytes**. Default-off packets remain
identical in the 198-configuration comparison.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0993 | +0.0785 | 11.3% |
| 24&nbsp;kbps | +0.1686 | +0.0954 | 13.1% |
| 32&nbsp;kbps | +0.2022 | +0.1181 | 22.8% |
| 48&nbsp;kbps | +0.2169 | +0.1223 | 21.3% |
| 64&nbsp;kbps | +0.2223 | +0.1245 | 21.4% |
| 96&nbsp;kbps | +0.2148 | +0.1566 | 5.2% |
| 128&nbsp;kbps | +0.2154 | +0.1590 | 4.9% |
| 192&nbsp;kbps | +0.2181 | +0.1591 | 4.0% |
| 256&nbsp;kbps | +0.2182 | +0.1597 | 4.3% |

Sources: `metrics/voice_denoise_quality_voip.csv`, `metrics/voice_denoise_timing.csv`,
and `metrics/voice_denoise_provenance.json`.

The focused state/bounds/reset test exercises five sample rates, six frame durations,
and complexities 0, 5 and 10. Build it as one translation unit:

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
| Encoder mono | 16,992 B | 31,712 B | -46.4% |
| Encoder stereo | 32,320 B | 48,880 B | -33.9% |
| Decoder mono | 14,096 B | 18,288 B | -22.9% |
| Decoder stereo | 21,248 B | 27,360 B | -22.3% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 336,804 B | 0 B | 336,804 B |
| Android arm64 Clang `-O2` | 337,340 B | 472 B | 337,812 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.2 C++23 | Warning-free build in this run (`-Wall -Wextra -Wpedantic`). |
| Android arm64 Clang C++23 | Warning-free build in this run (`-Wall -Wextra -Wpedantic`). |
| Linux C++23 compiler | Intended to build with a standard C++23 toolchain; use the full report script for local validation. |


## Allocation/history integration

At complexity 10, the encoder can compare two fullband CELT allocations against the same reconstructed decoder history, using the ordinary packet's byte budget. The comparison currently covers eligible 48 kHz PCM16 mono VOIP and stereo AUDIO, 10/20 ms frames, constrained VBR, and no DTX/FEC. Complexity 9 remains the default and does not run this search.

Quality acceptance uses the direct official comparisons above, not gains against an earlier opuscpp encoder. The remaining adverse fields are not rounded away or claimed as wins. This search has not achieved an all-metric advantage.

The search shares compatible pre-emphasis, pitch/prefilter, transform and band analysis. Identical or budget-ineligible allocation proposals skip the remaining alternative quantization; energy-refresh and release cases keep their required paths. Identical packets and candidates that fail the cheap waveform-error checks are rejected before full spectral scoring.

Full candidate pairs share source filtering, band energies and stereo power through an approximately 1.4 KiB frame-local cache. The observer filter banks remain in double precision, matching the scoring arithmetic and avoiding float/double conversion at every sample. This adds 256 bytes to an allocated history object and preserves packets and measured quality in the validation corpus. The full timing sweep shows 2-10% higher encode throughput after normalization to the official control; see [observer timing](metrics/quality_observer_speed.csv). It is a partial recovery, not a resolution of the remaining encoding slowdown. Inactive alternatives advance only the selected output's filter history. Hybrid encoding skips shadow decoding only when that history will be replaced before selection. Ordinary encoding bypasses the large search wrapper.

These reductions do not eliminate the cost of the second candidate and reconstructed-output checks, which remain gated on constrained VBR. The default-VBR production timings above are 0.80x-1.00x of official across 48-256 kbps (the search does not run in that workload). The budget-ineligible cleanup removes proven duplicate work, but the full synthetic benchmark did not establish a separate speed gain from that one-line change.

All 36 headline quality configurations retain identical measured values and tested packet hashes. Windows and Android builds pass; 288 exact scorer checks and the trapping-UBSan history test also pass.

Integration checks passed across 96 configurations and 7,680 packets: mono/stereo, 10/20 ms, complexity 0/9/10, VBR/CBR, DTX/FEC, PCM16/float, and reset. Packet decoding and final ranges agree with official Opus. The cross-decoder PCM comparison is not bit-exact: the largest observed difference was two int16 units. Windows GCC and Android arm64 Clang compile without warnings.

The focused history test also passes trapping undefined-behavior checks. Run it without separately compiling the implementation, because the test includes it:

It also verifies that scoring uses the output channel layout: a perfectly matching stereo output scores zero even when the packet codes mono, and an error confined to the final right-channel sample is counted correctly.

The encoder tracker follows the decoder's redundancy-retention rule. The 2,592-packet reversal matrix reports zero model-state or output-filter mismatches. The public-control test detects the former mismatch when enabling FEC while changing bitrate, and also checks that advance-only filtering preserves the exact full-scoring endpoints across successive frames.

~~~sh
c++ -std=c++23 -O2 -DNDEBUG -I src tests/encoder_quality_history.cpp -o encoder_quality_history
./encoder_quality_history
~~~

`encoder_reference_continuity.cpp` checks 48 mono/stereo sequences with changing frame sizes and PCM16/float calls. It verifies the actual input tail, packet ranges, decoder state, and one-frame scoring warmup after an unsupported reference window. The stale-tail case fails on the unfixed implementation; the repaired path passes trapping undefined-behavior checks.

~~~sh
c++ -std=c++23 -O2 -DNDEBUG -I src tests/encoder_reference_continuity.cpp -o encoder_reference_continuity
./encoder_reference_continuity
~~~
