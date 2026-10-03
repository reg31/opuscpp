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

## NSQ candidate helper inlining checkpoint (2026-09-15)

The standard `inline` hint on `silk_quantize_candidate_pair` lets the measured GCC 16.2.0/O2
build eliminate its out-of-line helper. Arithmetic, API and encoder state are unchanged.
All nine real-voice/FEC/AUDIO packet streams are byte-identical, and all 498 quality rows,
5,976 fields, official references and packet metadata match the FEC allocation checkpoint.
Expanded strict FEC, source criteria and source-bound public checks pass.

## FEC rate allocation checkpoint (2026-09-15)

SILK now apportions normal-frame capacity after charging already-written redundancy bits.
A 60 ms packet could previously spend 1,514 bits before a first-frame ceiling of only 688,
forcing repeated quantization and zero-pulse fallback. Stereo mid/side reservations now use
the remaining capacity and analyzed channel rates. Final packet ceilings are unchanged.
The checkpoint also restores packet-loss input to equivalent-rate selection, separates FEC
availability from bandwidth quality, removes custom LBRR Lambda scaling, and carries LBRR
history across consecutive internal frames.

- Strict FEC: 18/18; source criteria C1-C4: 18/18 each. The public FEC test now also covers
  stereo 40/60 ms at 32 kbps CBR and 48 kbps VBR, including quiet/startup transitions.
- All 498 quality configurations, 5,976 metrics, official references and packet metadata are
  unchanged. **1,154 below-official quality fields remain; full parity is not complete.**
- 41 integration commands and 21 additional public commands pass, including startup/reset,
  VBR limits, interoperability, DTX, denoiser, PLC and NSQ feedback checks.
- On Hazel, the rejected loss-aware intermediate caused 2,075 normal quantizer trials and
  205 zero fallbacks; corrected accounting reduces them to 915 and 5. Production had 883
  and 1, with nine fewer SILK frames. Counts distinguish normal trials, replays and LBRR.

Exact source/object identities, source criteria, extended results and workload counts are in [the checkpoint record](metrics/fec_rate_allocation_checkpoint.json). Earlier benchmark tables retain their original measured revisions.

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

Fresh checks were run against source `35c5039` and official Opus `503d81b`. RFC decode result: 24/24; encode interoperability: 96/96. API behavior and selected source-bound regression outputs are recorded in [fresh compatibility metadata](metrics/compatibility_validation.json). No sanitizer was run.

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
| VBR budget behavior (CELT-run budgets; all-mode packet bounds and decoded duration) | Passed |
| Hybrid transient bit budget | Passed |
| Guarded DTX behavior, refresh, and quiet-tonal protection | Passed |
| DTX active-content and re-entry comparison vs official Opus | FAIL (unchanged criteria) |
| In-band FEC encode/decode interoperability vs official Opus | Passed |
| SILK NSQ reconstruction oracle | FAIL: the frame63 CELT-redundancy tail makes the NSQ-only expectation inapplicable; official/A7 PCM matches ([record](metrics/lpc_analysis_known_failure.json)) |
| Independent bit helpers, seed wrap, Schur, SILK LPC orders and CELT IIR assertions | Passed in one supplemental scratch continuation; reconstruction remains FAIL and executable exits 1 |
| Separate CELT energy boundaries and guarded stereo-policy tests | Passed |
| Trapping UBSan: API, long frames and 291,755 malformed packets | Historical; not rerun |

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
signal after silence, `opuscpp` has lower aggregate wake-up NRMSE (`0.4261` vs `0.7622`)
and gain error (`1.8524` vs `1.6463` dB); silence-frame suppression is 406 versus 406.
Aggregate re-entry NRMSE is 44.1% lower and aggregate gain error is 12.5% higher. The separate steady-noise-only case suppresses 120 frames with `opuscpp` versus 0 with official Opus. Individual per-material gain errors remain mixed; all current values are in `metrics/dtx_metrics.csv`. DTX comparison: FAIL. The original acceptance criteria are unchanged; all measurements are retained. Per-case rows and both exit records are in [DTX acceptance metadata](metrics/dtx_acceptance.json).

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

Current full-refresh FEC uses effective setting 1. FEC records 18/18 recovery wins and aggregate recovery ratio 0.504119; packet-byte ratio 0.993345 passes the <=1 gate. Source criteria are C1 18/18, C2 18/18, C3 18/18, C4 18/18; all four source criteria pass. Strict source gate: PASS; standard interop gate: PASS. The standard score covers 18/36 configurations; the complete harness output and all 72 direction rows are retained in [FEC metadata](metrics/fec_run_metadata.json).

`fec_source_quality.cpp` separately compares recovered-frame fidelity, the following frame and boundary transition against the original source. Current C1/C2/C3/C4 counts are 18/18, 18/18, 18/18, 18/18; C1-C4 pass for every recorded case. Full current/official case values are in [the source-quality JSON](metrics/fec_source_metrics.json).
the same `curr_`-renamed codec object as `fec_vs_official.cpp`; build the reference binary with
`-DUSE_OFFICIAL_ENCODER` and the official library. Then run:

```text
python tests/scripts/check_fec_source_quality.py current_gate.exe official_gate.exe --output fec_source_results.json
```

The checkpoint also passes packet budgets, 240 API/reset cases, 96 encoder conformance cases,
packet-duration/channel-remap checks, SILK reconstruction, postfilter and denoiser checks.
The later VOIP startup checkpoint below removes the quiet classifier and startup mode overrides;
its behavioral regression supersedes the original classifier-state check.

The full 498-configuration quality matrix has 1288 below-official fields, compared with 1580 at
parent commit `a4a1fde`: 360 fixed, 68 newly negative and 260 worsened existing deficits. All AUDIO
fields are unchanged; these differences are VOIP. This is an FEC-qualified checkpoint, not full
quality/performance parity. The separate CELT transient/history and LPC experiments remain
outside this checkpoint for individual review. The main speed/memory tables below have a separate current production refresh; named historical checkpoints retain their original provenance.

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

The full benchmark tables below were refreshed on 2026-10-03 for source `35c5039` (SHA-256 `a7d976360e2749b38faaa110f8e855e81fd5b935d1e982daa0bc9e1e61bb5d75`) against official Opus `503d81b`.
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
| 16&nbsp;kbps | 1.261x | 1.724x | 435x | 345x | 2226x | 1291x |
| 24&nbsp;kbps | 1.254x | 1.357x | 402x | 321x | 1545x | 1139x |
| 32&nbsp;kbps | 1.251x | 1.347x | 405x | 324x | 1496x | 1111x |
| 48&nbsp;kbps | 1.273x | 1.315x | 376x | 295x | 1257x | 956x |
| 64&nbsp;kbps | 1.271x | 1.233x | 337x | 265x | 1044x | 847x |
| 96&nbsp;kbps | 1.262x | 1.172x | 272x | 216x | 773x | 660x |
| 128&nbsp;kbps | 1.204x | 1.190x | 234x | 194x | 685x | 575x |
| 192&nbsp;kbps | 1.135x | 1.200x | 197x | 174x | 588x | 490x |
| 256&nbsp;kbps | 1.127x | 1.191x | 182x | 161x | 525x | 441x |

The isolated production speed run is recorded in [speed_run_metadata.json](metrics/speed_run_metadata.json). Encoding is faster at 9/9 measured AUDIO rates (1.13x to 1.27x); decoding at 9/9 (1.17x to 1.72x).

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
| 16&nbsp;kbps | +0.0024 | -0.0035 | +1.6979 | 16.432 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.0403 | +0.0333 | +0.4578 | 24.480 kbps | 25.229 kbps |
| 32&nbsp;kbps | +0.0719 | +0.0130 | +0.1952 | 32.507 kbps | 33.613 kbps |
| 48&nbsp;kbps | +0.1996 | +0.0274 | -0.0294 | 48.560 kbps | 48.560 kbps |
| 64&nbsp;kbps | +0.2810 | +0.0246 | -0.0094 | 64.613 kbps | 64.613 kbps |
| 96&nbsp;kbps | +0.2812 | +0.0152 | +0.0091 | 96.720 kbps | 96.697 kbps |
| 128&nbsp;kbps | +0.1528 | +0.0061 | -0.0364 | 128.827 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0705 | +0.0048 | +0.0033 | 193.028 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0355 | +0.0039 | +0.0064 | 256.576 kbps | 256.737 kbps |

## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1482 | +0.0049 | +0.2362 | 12.367 kbps | 12.072 kbps |
| 24&nbsp;kbps | +0.2090 | +0.0138 | +0.0641 | 24.011 kbps | 24.020 kbps |
| 32&nbsp;kbps | +0.2713 | +0.0157 | +0.4659 | 32.113 kbps | 32.088 kbps |
| 48&nbsp;kbps | +0.1432 | -0.0050 | +0.1056 | 48.352 kbps | 48.409 kbps |
| 64&nbsp;kbps | +0.2931 | +0.0016 | +0.0031 | 64.481 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.7011 | +0.0081 | +0.3507 | 96.400 kbps | 96.499 kbps |
| 128&nbsp;kbps | +0.7738 | +0.0033 | +0.3840 | 128.400 kbps | 128.489 kbps |
| 192&nbsp;kbps | +1.0978 | +0.0048 | +0.4258 | 192.400 kbps | 192.472 kbps |
| 256&nbsp;kbps | +1.1009 | +0.0055 | +0.4504 | 256.400 kbps | 256.464 kbps |

The voiced/formant fixture uses phase-integrated pitch and breath noise at a controlled 30 dB SNR. Its ViSQOL-style delta improves at 8/9 rates; losses remain. The former phase-modulated fixture is retained as a separate tonal-stress case, not relabelled as speech. Do not compare new VOIP scores directly with the former fixture. The complete 12-field comparisons and complexity-9 control are in [quality_official_full_precision.csv](metrics/quality_official_full_precision.csv), with [configuration and fixture hashes](metrics/quality_run_metadata.json). Rounded zero in a table does not imply exact equality.

### Broader content check

Fresh comparisons cover the broad ladder, stereo/content holdouts, four mono speech recordings, and the retained tonal-stress fixture. These are short-clip diagnostics, not a representative listening survey.

| Set | Comparisons | Negative PESQ-style | Negative ViSQOL-style | Negative CELT proxy |
|---:|---:|---:|---:|---:|
| Broad ladder | 99 | 6 | 14 | 25 |
| Stereo/content holdouts | 30 | 4 | 8 | 7 |
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

The 15.5/20 kbps cases have PESQ-style gains +0.1345/+0.1341 and ViSQOL-style gains +0.0922/+0.0905. The boundary benchmark passes 25/25 rates.

Across 246 on/off comparisons covering 41 clean/noisy/content conditions at 6 rates, 2 have negative PESQ-style deltas, 0 negative ViSQOL-style deltas, and 4 negative CELT-proxy deltas. All 12 fields, including other losses, are retained in `metrics/voice_denoise_broad.csv`.


End-to-end encode overhead is **1.4% to 4.8%** on the tracked noisy recording. This includes changed downstream coding work, not just filter arithmetic. Timing runs in isolation, pinned to one logical CPU at above-normal priority; enabled/bypass order rotates. Values are medians of nine 60-second runs after one warm-up.

The optional denoiser state is 68 bytes; the fresh state/bounds/reset check passed 90 configurations. Temporary stack high-water was not measured in this refresh.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1151 | +0.0964 | 4.8% |
| 24&nbsp;kbps | +0.1820 | +0.1082 | 1.4% |
| 32&nbsp;kbps | +0.2048 | +0.1174 | 3.4% |
| 48&nbsp;kbps | +0.2195 | +0.1223 | 4.2% |
| 64&nbsp;kbps | +0.2140 | +0.1327 | 4.1% |
| 96&nbsp;kbps | +0.2128 | +0.1558 | 3.7% |
| 128&nbsp;kbps | +0.2118 | +0.1581 | 4.2% |
| 192&nbsp;kbps | +0.2148 | +0.1591 | 3.9% |
| 256&nbsp;kbps | +0.2157 | +0.1594 | 3.6% |

Sources: `metrics/voice_denoise_quality_voip.csv`, `metrics/voice_denoise_timing.csv`, `metrics/voice_denoise_boundary.csv`, and `metrics/voice_denoise_provenance.json`. The previous-version CSV is historical, not a current acceptance result.

The focused state/bounds/reset test covers five sample rates, six frame durations, and complexities 0, 5 and 10:

```bash
c++ -std=c++23 -O2 -DNDEBUG tests/voice_denoise_state.cpp -o build/voice_denoise_state
build/voice_denoise_state
```

## Memory metrics

This table is from the full refresh for source `35c5039`: all encoder and decoder rows used the same 256-instance, three-process memory-only run with FEC and denoising disabled. Process-private deltas are not exact structure sizes; allocator/page rounding can affect results. See [run metadata](metrics/run_metadata.json).
| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 19,792 B | 31,872 B | -37.9% |
| Encoder stereo | 30,000 B | 48,912 B | -38.7% |
| Decoder mono | 11,840 B | 18,416 B | -35.7% |
| Decoder stereo | 17,552 B | 27,424 B | -36.0% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 380,272 B | 0 B | 380,272 B |
| Android arm64 Clang `-O2` | 374,148 B | 472 B | 374,620 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC 16.2 C++23 | Fresh `-std=c++23 -O2 -DNDEBUG` compile passed; command and compiler output are retained in run metadata. |
| Android arm64 Clang C++23 | Fresh `-std=c++23 -O2 -DNDEBUG` compile passed; command and compiler output are retained in run metadata. |
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

## SILK concealment-energy checkpoint

PLC now truncates scaled excitation when comparing subframe energies, matching official Opus.
Rounding those samples could select a different noise-history segment even when the decoder
states and input excitation were identical. That divergence also affected FEC after a concealed
prefix of a 40 ms packet. On the captured reproducer, both prefix concealment and FEC recovery
now match official PCM exactly; previously recovered-frame NRMSE was 0.451981.

`silk_plc_energy.cpp` is a direct regression: compile it as a standalone C++23 test. The previous
implementation fails; the fix passes. All 5976 loss-free quality fields are unchanged. Strict FEC,
all four source criteria, packet budgets and ordinary integration checks pass. See
[concealment checkpoint metadata](metrics/silk_plc_energy_checkpoint.json).

## VOIP startup checkpoint

The encoder no longer uses a quiet-start classification or the first few frames to keep a
filtering/gain/mode decision. The exact 64 kbps quiet-to-SILK override, one-shot speech-mode
force and low-rate startup flags are removed. Low-rate processing uses current signal cues;
noise confidence continues updating and can decay after clean input. Tone evidence reuses the
existing LPC detector. The obsolete quiet classifier and its state are deleted.

`voip_quiet_start_latch.cpp` now checks behavior through the public API: silence, quiet speech,
low tones and noise precede the same common input. All 24 cases converge to the same steady
mode at 16/48/64 kbps through both input APIs, and reset produces byte-identical packets. The
previous implementation fails 12 cases. It links `src/opus_codec.cpp`; test hooks are not required.
The separate David/low-pitch corpus check covers six histories, three rates and both APIs: all
72 mode traces agree after the first 60 common frames. SILK reconstruction testing now seeds
current speech evidence to select SILK instead of relying on the removed startup override.

Strict FEC and all four source criteria pass 18/18. API/reset, packet budgets, conformance,
duration/channel-remap, postfilter, denoiser and reconstruction checks pass. The full quality
matrix improves from 1233 to 1158 below-official fields: 191 fixed, 116 newly negative and
172 worsened existing deficits. Every AUDIO quality field is unchanged; remaining VOIP and
AUDIO deficits remain open.

Median process-private bytes per instance (three fresh runs of 256 instances):

| State | Current | Official |
|---|---:|---:|
| Mono encoder | 16864 | 31824 |
| Stereo encoder | 32576 | 49072 |
| Mono decoder | 14160 | 18304 |
| Stereo decoder | 21232 | 27392 |

## Hybrid bitrate allocation checkpoint

Hybrid encoding no longer forces a 24 kbps SILK target for requested rates from 28 to
36 kbps. It uses the existing hybrid rate allocation. The removed target boost was absent
from the hard-ceiling calculation, causing additional gain-search retries. The independent
stereo LTP scaling rule retains its original rate limits.

On the 13.46-second Hazel mono fixture at 32 kbps, requested SILK bitrate falls from 27000
to 23400 bps. NSQ calls fall from 1008 to 878 for the same 618 SILK frames; first-trial
overshoots fall from 324 to 193. Packet size remains 53840 bytes. Mono 16/24/48 kbps
control packet streams are unchanged. These are measured work counts, not timing ratios.

The complete 498-configuration matrix changes 99 of 5976 quality fields, all at 32 kbps:
four below-official fields are fixed, no new negative fields appear, and six existing
negative fields worsen. There are 1154 remaining below-official fields. All official
reference fields and packet-size metadata are unchanged.

Strict FEC remains 18/18, with aggregate recovery-error ratio 0.469410 and packet-byte ratio
0.996469. All four source-quality criteria pass 18/18. Startup/reset, API, conformance,
VBR, packet duration/channel remapping, interoperability, LPC/PLC, DTX, denoiser and
postfilter checks pass. Full quality and speed parity remain unfinished.

Exact source identities, measurements and remaining quality regressions are recorded in
[hybrid rate checkpoint metadata](metrics/hybrid_rate_checkpoint.json).

## SILK shaping-feedback checkpoint

Delayed-decision NSQ computes the shaping feedback of four independent candidate states
together. Per-lane integer operations retain their original order. The temporary history
tracks winner-state replacements and is copied back at the subframe boundary.

All 5976 quality fields, official references and packet metadata are unchanged across the
498-configuration matrix. Strict FEC and all four source criteria pass 18/18. Current-source
NSQ captures match pulses, Seed and named state; forced-zero replay state also matches.
Startup/reset, API/conformance, VBR, interoperability, LPC/PLC, DTX, denoiser and postfilter
checks pass. The standalone `nsq_shaping_feedback.cpp` regression checks 19200 lane results
against the scalar helper, including independent full-range histories and untouched tails.

The compiler-reported frame reservation for `silk_NSQ<true, false>` increases from 10112
to 10592 bytes (+480). Other NSQ template instantiations retain their previous frame sizes.
These are per-function reservations, not total nested stack usage. There are no new heap
allocations or persistent encoder-state fields.



Full quality parity remains unfinished, with 1154 below-official quality fields.
Full measurements and identities are in [feedback checkpoint metadata](metrics/nsq_feedback_checkpoint.json).

## FEC startup mode regression

The `32f44fc` update removes the fixed startup wait before eligible FEC mode selection. The existing startup/reset harness now has 26 checks; its two new int16/float first-packet checks fail on the previous source and pass with the fix, including reset after populated history. In two aligned packet-1 loss controls, source error falls from 0.347366240 to 0.205424276 (mono 32 kbps) and from 0.782491949 to 0.243185224 (stereo 48 kbps). The later packet-8 self-reference comparison still has one deficit, reported above. [Startup measurements](metrics/fec_startup_checkpoint.json).
