# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

The current benchmark tables were refreshed independently by Codex on 2026-09-15 for production `d7248b4`. Both codecs use complexity 10 and `-O2 -DNDEBUG`, with official Opus intrinsics enabled. [Every metric and full results](metrics/README.md) are retained. Named per-commit checkpoint sections are historical comparisons; their measurements are not relabelled as new runs. Broader compatibility/conformance results remain historical; 8 selected current checks are recorded in [regression metadata](metrics/selected_regressions.json).


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

Seven alternating rounds, one inner repetition, verified processor affinity and priority:

| Input | Mode | kbps | Previous encode ms | New encode ms | Encode change | Decode change |
|---|---|---:|---:|---:|---:|---:|
| david | voip | 16 | 123.4179 | 120.0324 | -2.74% | -0.24% |
| david | voip | 32 | 127.0713 | 123.5603 | -2.76% | -0.20% |
| david | voip | 64 | 27.9689 | 27.9678 | -0.00% | +0.39% |
| david_60ms | fec60 | 24 | 157.6282 | 152.2068 | -3.44% | +0.15% |
| hazel | voip | 16 | 140.3955 | 136.9864 | -2.43% | +0.39% |
| hazel | voip | 32 | 135.0412 | 131.3401 | -2.74% | -0.50% |
| hazel | voip | 64 | 31.4791 | 31.5561 | +0.24% | -0.81% |
| hazel_60ms | fec60 | 24 | 182.8446 | 178.0399 | -2.63% | -0.36% |
| synthetic_music_12s | audio | 32 | 20.8715 | 20.7444 | -0.61% | -0.25% |

VOIP16/32 encoding improves 2.4-2.8%; FEC60 improves 2.6-3.4%. Control variation is shown
explicitly. This is a compiler-specific measurement, not a universal inlining guarantee.
**1,154 below-official quality fields remain; full speed/quality parity is unfinished.**
Exact identities and all raw timing rows are in
[the checkpoint record](metrics/nsq_candidate_inline_checkpoint.json).

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

FEC-on 24 kbps mono, 60 ms packets; median of seven alternating runs with verified processor
affinity and priority. Inputs contain 13.44 s (Hazel) and 11.76 s (David) of complete frames.

| Input | Previous encode time | New encode time | Time change |
|---|---:|---:|---:|
| Hazel | 187.4754 ms | 182.1265 ms | -2.85% |
| David | 165.1605 ms | 157.1302 ms | -4.86% |

Exact source/object identities, source criteria, extended results, workload counts and all raw
timing rows are in [the checkpoint record](metrics/fec_rate_allocation_checkpoint.json).
Earlier benchmark tables retain their original measured revisions.

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
signal after silence, `opuscpp` has lower aggregate wake-up NRMSE (`0.3761` vs `0.7622`)
and gain error (`1.5500` vs `1.6463` dB), while both suppress 406 silence frames.
That is 50.7% less re-entry error and 5.9% less aggregate gain error. The separate steady-noise-only case suppresses 120 frames with `opuscpp` versus 0 with official Opus. Individual per-material gain errors remain mixed; all current values are in `metrics/dtx_metrics.csv`.

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
same stream. The current benchmark passes all 18 scored strict 10/20 ms cases (maximum ratio 0.953507). The expanded 36-configuration run has aggregate recovery error ratio 0.416949 (58.3% lower), backup coverage 18/18 versus official 15/18, and packet-byte ratio 0.997419. This aggregate covers the expanded run and is not directly comparable with older matrices. All ratios, 72 direction rows and source-quality criteria are in [current FEC metadata](metrics/fec_run_metadata.json).

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

The main speed, memory, binary-size, AUDIO/VOIP quality, optional-processing and broader-corpus tables use the current production refresh. Historical per-commit comparisons retain their recorded source. The headline encoder comparisons use complexity 10.
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
| 16&nbsp;kbps | 1.768x | 1.770x | 619x | 350x | 2303x | 1301x |
| 24&nbsp;kbps | 1.790x | 1.397x | 569x | 318x | 1573x | 1126x |
| 32&nbsp;kbps | 1.738x | 1.353x | 556x | 320x | 1513x | 1118x |
| 48&nbsp;kbps | 1.552x | 1.343x | 458x | 295x | 1262x | 940x |
| 64&nbsp;kbps | 1.539x | 1.272x | 401x | 261x | 1060x | 834x |
| 96&nbsp;kbps | 1.533x | 1.195x | 326x | 212x | 785x | 657x |
| 128&nbsp;kbps | 1.454x | 1.199x | 280x | 192x | 686x | 572x |
| 192&nbsp;kbps | 1.319x | 1.230x | 230x | 174x | 602x | 489x |
| 256&nbsp;kbps | 1.270x | 1.208x | 211x | 166x | 530x | 439x |


The isolated production speed run is recorded in [speed_run_metadata.json](metrics/speed_run_metadata.json). Encoding is faster at 9/9 measured AUDIO rates (1.27x to 1.79x); decoding at 9/9 (1.19x to 1.77x). The separate real-speech VOIP timings, including slower cases, are in [the complete results](metrics/README.md).

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
| 16&nbsp;kbps | +0.0003 | -0.0014 | +1.6768 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | +0.3781 | +0.0963 | +0.6840 | 24.000 kbps | 25.229 kbps |
| 32&nbsp;kbps | +0.5416 | +0.0929 | +0.1636 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | +0.1634 | +0.0127 | -0.0395 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | +0.1394 | +0.0047 | -0.0519 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | +0.2902 | +0.0125 | +0.0260 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | +0.1902 | +0.0042 | -0.0459 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | +0.0709 | +0.0028 | +0.0064 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | +0.0341 | +0.0020 | +0.0077 | 256.000 kbps | 256.737 kbps |


## VOIP quality metrics vs official Opus

VOIP quality proxy metrics are measured separately on the synthetic mono speech-like validation
sample because VOIP deliberately uses different mode-selection semantics than AUDIO.

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT proxy delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1703 | +0.0202 | +0.3070 | 15.997 kbps | 12.072 kbps |
| 24&nbsp;kbps | -0.3443 | +0.0017 | +0.2560 | 23.997 kbps | 24.020 kbps |
| 32&nbsp;kbps | -0.4126 | +0.0025 | +0.4631 | 31.997 kbps | 32.088 kbps |
| 48&nbsp;kbps | -0.5072 | +0.0001 | +0.1186 | 47.997 kbps | 48.409 kbps |
| 64&nbsp;kbps | +0.2648 | -0.0049 | +0.0068 | 64.000 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.6546 | +0.0061 | +0.3496 | 96.000 kbps | 96.499 kbps |
| 128&nbsp;kbps | +0.7434 | +0.0029 | +0.3504 | 128.000 kbps | 128.489 kbps |
| 192&nbsp;kbps | +1.0911 | +0.0047 | +0.4131 | 192.000 kbps | 192.472 kbps |
| 256&nbsp;kbps | +1.1005 | +0.0052 | +0.4503 | 256.000 kbps | 256.464 kbps |

The voiced/formant fixture uses phase-integrated pitch and breath noise at a controlled 30 dB SNR. Its ViSQOL-style delta improves at 8/9 rates; losses remain. The former phase-modulated fixture is retained as a separate tonal-stress case, not relabelled as speech. Do not compare new VOIP scores directly with the former fixture. The complete 12-field comparisons and complexity-9 control are in [quality_official_full_precision.csv](metrics/quality_official_full_precision.csv), with [configuration and fixture hashes](metrics/quality_run_metadata.json). Rounded zero in a table does not imply exact equality.

### Broader content check

Fresh comparisons cover the broad ladder, stereo/content holdouts, four mono speech recordings, and the retained tonal-stress fixture. These are short-clip diagnostics, not a representative listening survey.

| Set | Comparisons | Negative PESQ-style | Negative ViSQOL-style | Negative CELT proxy |
|---:|---:|---:|---:|---:|
| Broad ladder | 99 | 22 | 36 | 35 |
| Stereo/content holdouts | 30 | 1 | 9 | 8 |
| Mono speech recordings | 36 | 0 | 0 | 4 |
| Retained tonal stress | 9 | 0 | 1 | 0 |

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

The 15.5/20 kbps cases have PESQ-style gains +0.1211/+0.1140 and ViSQOL-style gains +0.0918/+0.0794. The boundary benchmark passes 25/25 rates.

Across 246 on/off comparisons covering 41 clean/noisy/content conditions at six rates, 1 has a negative PESQ-style delta, 0 negative ViSQOL-style deltas, and 4 negative CELT-proxy deltas. All 12 fields, including other losses, are retained in `metrics/voice_denoise_broad.csv`.

End-to-end encode overhead is **5.1% to 13.2%** on the tracked noisy recording. This includes changed downstream coding work, not just filter arithmetic. Timing runs in isolation, pinned to one logical CPU at above-normal priority; enabled/bypass order rotates. Values are medians of nine 60-second runs after one warm-up.

The optional state is 68 bytes. A 7.5 KiB temporary stack cache avoids repeating filter work for frames of up to 960 samples; longer frames recompute. The 90-configuration state/bounds/reset harness passed in this refresh. Both toolchains build; unused tone-analysis warnings remain.

| Bitrate | PESQ-style gain | ViSQOL-style gain | Encode overhead |
|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0965 | +0.0779 | 13.2% |
| 24&nbsp;kbps | +0.1637 | +0.1057 | 8.3% |
| 32&nbsp;kbps | +0.1993 | +0.1229 | 8.5% |
| 48&nbsp;kbps | +0.2082 | +0.1264 | 8.2% |
| 64&nbsp;kbps | +0.1955 | +0.1500 | 7.0% |
| 96&nbsp;kbps | +0.2127 | +0.1551 | 5.4% |
| 128&nbsp;kbps | +0.2123 | +0.1574 | 5.5% |
| 192&nbsp;kbps | +0.2152 | +0.1593 | 5.1% |
| 256&nbsp;kbps | +0.2158 | +0.1593 | 5.4% |

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
| Encoder mono | 16,832 B | 31,872 B | -47.2% |
| Encoder stereo | 32,576 B | 49,072 B | -33.6% |
| Decoder mono | 14,192 B | 18,304 B | -22.5% |
| Decoder stereo | 21,232 B | 27,392 B | -22.5% |

Source CSV:

- `metrics/memory_vs_official.csv`

## Binary size

| Build | Text | Data | Total measured image (text+data+bss) |
|---:|---:|---:|---:|
| Host MinGW GCC `-O2` | 324,760 B | 0 B | 324,760 B |
| Android arm64 Clang `-O2` | 327,588 B | 472 B | 328,060 B |

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

The corrected VOIP decisions change mode use. David now predominantly uses hybrid at
16-48 kbps, as official Opus does, and its earlier speed advantage at these rates disappears.
Both codecs predominantly use hybrid on Hazel at these rates too. The remaining encode
slowdown must therefore be addressed within that path. The table gives fresh nine-repeat
ratios (official time/current time); below 1 means slower. Inputs are the existing 11.8-second
David and 13.46-second Hazel PCM; complexity 10, -O2 -DNDEBUG, with official intrinsics enabled.
There was no concurrent codec workload or explicit affinity/priority pinning.

| kbps | David encode | David decode | Hazel encode | Hazel decode |
|---:|---:|---:|---:|---:|
| 16 | 0.560x | 1.178x | 0.574x | 1.175x |
| 24 | 0.566x | 1.161x | 0.622x | 1.175x |
| 32 | 0.546x | 1.176x | 0.565x | 1.180x |
| 48 | 0.581x | 1.160x | 0.632x | 1.170x |
| 64 | 2.342x | 1.154x | 2.519x | 1.165x |
| 96 | 1.045x | 1.194x | 1.090x | 1.247x |
| 128 | 1.053x | 1.181x | 1.069x | 1.186x |
| 192 | 1.069x | 1.189x | 1.085x | 1.171x |
| 256 | 1.100x | 1.148x | 1.134x | 1.171x |

Median process-private bytes per instance (three fresh runs of 256 instances):

| State | Current | Official |
|---|---:|---:|
| Mono encoder | 16864 | 31824 |
| Stereo encoder | 32576 | 49072 |
| Mono decoder | 14160 | 18304 |
| Stereo decoder | 21232 | 27392 |

Exact source identities, full timing rows and check results are in [startup checkpoint metadata](metrics/voip_startup_checkpoint.json). Full quality/speed parity remains unfinished.

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

Nine alternating runs used logical CPU 0 and AboveNormal priority, verified inside
each private benchmark. VOIP inputs are the same Hazel/David PCM fixtures; the AUDIO
control uses 12 seconds of the existing synthetic music fixture. Control timing
variation is included below. Percentages describe encoding time, so negative is faster.

| Input / application | kbps | Baseline ms | Candidate ms | Change |
|---|---:|---:|---:|---:|
| synthetic_music_12s / audio | 32 | 22.15 | 22.18 | +0.1% |
| david / voip | 16 | 137.35 | 139.77 | +1.8% |
| david / voip | 32 | 149.22 | 141.20 | -5.4% |
| david / voip | 64 | 29.31 | 29.25 | -0.2% |
| hazel / voip | 16 | 155.58 | 155.76 | +0.1% |
| hazel / voip | 32 | 162.89 | 148.68 | -8.7% |
| hazel / voip | 64 | 32.80 | 32.84 | +0.1% |

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

Nine rounds rotated the baseline, first probe and final candidate. The private benchmark
verified logical-CPU-0 affinity and AboveNormal priority before timing. The selected
candidate reduced encoding time by 5.5-6.5% at 16/32 kbps; control timings are included below.
This is a measured code-layout/data-flow improvement, not a claim that every loop uses SIMD.

| Input | kbps | Baseline encode ms | Candidate encode ms | Time change |
|---|---:|---:|---:|---:|
| voip_hazel | 16 | 148.86 | 139.23 | -6.5% |
| voip_hazel | 32 | 142.02 | 134.23 | -5.5% |
| voip_hazel | 64 | 31.24 | 31.18 | -0.2% |
| voip_david | 16 | 130.34 | 123.05 | -5.6% |
| voip_david | 32 | 134.09 | 126.69 | -5.5% |
| voip_david | 64 | 27.80 | 27.78 | -0.1% |
| audio | 32 | 20.97 | 20.86 | -0.5% |

Full quality/speed parity remains unfinished, with 1154 below-official quality fields.
Full measurements and identities are in [feedback checkpoint metadata](metrics/nsq_feedback_checkpoint.json).
