# Complete production metrics

Codex independently refreshed production `32c3f3163501f679e85f1d03d890c6a5e178d8d5` on 2026-10-02 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field and the raw outputs for current compatibility checks are retained below or in linked files. Historical per-commit comparisons preserve their original provenance. The preserved `fec_s7_candidate_validation.json` is a separate historical candidate record and does not supply any current table values.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 805 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 14 | 5 |
| segmental_snr_db | Higher | 26 | 10 |
| rms_error | Lower | 14 | 5 |
| mean_abs_error | Lower | 15 | 7 |
| pesq_style | Higher | 29 | 10 |
| visqol_style | Higher | 66 | 94 |
| logband_corr | Higher | 65 | 119 |
| logband_error | Lower | 64 | 81 |
| celt_quality | Higher | 115 | 199 |
| celt_masked_error | Lower | 115 | 199 |
| celt_highband_error | Lower | 254 | 306 |
| stereo_width_error | Lower | 28 | 17 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.07x to 1.34x; decode: 1.21x to 1.93x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

The current encoder retained-allocation rows are a focused cafa9b08 remeasurement using the same 256-instance/3-process `--memory-only --bitrate16000` harness. Decoder rows and the full quality/timing measurements remain bound to the32c3f31 refresh; they were not remeasured for cafa9b08. Private/working-set page deltas are not exact object sizes. See [retained encoder memory evidence](retained_memory_ring_update.json).

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 27952 | 27776 | Not exposed |
| official_encoder_ch1 | 31824 | 31904 | 31668 |
| current_decoder_ch1 | 14112 | 13104 | Not exposed |
| official_decoder_ch1 | 18336 | 18528 | 18468 |
| current_encoder_ch2 | 43616 | 43440 | Not exposed |
| official_encoder_ch2 | 48864 | 48704 | 48684 |
| current_decoder_ch2 | 21296 | 21360 | Not exposed |
| official_decoder_ch2 | 27264 | 27248 | 27236 |

[Retained-state memory CSV](memory_vs_official.csv) contains the refreshed per-instance allocation/page values; it is distinct from the peak working-memory measurement below. A separate working-memory probe measures requested-live codec allocation payload plus native-thread stack-touched high-water; it excludes shared ROM, allocator bookkeeping, caller PCM/packet buffers, process working set and the 4 MiB thread-stack reservation. Encode allocation payload did not change during measured calls. The probe used MinGW GCC 16.2, `-std=c++23 -O2 -DNDEBUG -fstack-usage`, and 32 calls per configuration. It is bound to source commit `32c3f3163501f679e85f1d03d890c6a5e178d8d5` (Git-normalized source SHA-256 `1b59a733584d79d2712ba010e4b43430be292362d04a65414de46ceec8ce5281`); the frozen source-file SHA-256 is `5bf8709e9c821d7b04eaaa0f9d92db3668e629575aa8777197c8fbc233043ed9`, and baseline source is `c60faf726d69408feb8d4cf65f2ea3c30d89955f50f66a3ff26c5e1d26f47f2a`. Identity passed 77,760 frames over 1,620 cases with exact packet, final-range, decoded-PCM and FEC-PCM outputs. Retained allocation payload is unchanged; object text/read-only/unwind grows 1,608 B (+0.45%) and shared allocation caps use 672 B.

| Physical C | Application | API | Frame | Baseline peak working B | Final peak working B | Reduction |
|---:|---|---|---:|---:|---:|---:|
| 1 | VOIP | PCM16 | 20 ms | 120528 | 91152 | 24.37% |
| 1 | VOIP | Float | 20 ms | 112768 | 83392 | 26.05% |
| 1 | AUDIO | PCM16 | 20 ms | 121072 | 91696 | 24.26% |
| 1 | AUDIO | Float | 20 ms | 113312 | 83936 | 25.92% |
| 1 | LOWDELAY | PCM16 | 20 ms | 110088 | 80712 | 26.68% |
| 1 | LOWDELAY | Float | 20 ms | 102328 | 72952 | 28.71% |
| 2 | VOIP | PCM16 | 20 ms | 136872 | 113256 | 17.25% |
| 2 | VOIP | Float | 20 ms | 129112 | 105496 | 18.29% |
| 2 | AUDIO | PCM16 | 20 ms | 137144 | 113528 | 17.22% |
| 2 | AUDIO | Float | 20 ms | 129384 | 105768 | 18.25% |
| 2 | LOWDELAY | PCM16 | 20 ms | 115272 | 91656 | 20.49% |
| 2 | LOWDELAY | Float | 20 ms | 107512 | 83896 | 21.97% |

At 20 ms, mono savings are 24.26%–28.71%; stereo savings are 17.22%–21.97%, with VOIP/AUDIO below 20%. The 120 ms cases are 48 B higher (0.027%–0.047%), rather than lower. All 24 C/application/duration/API records and measured timing medians are in [working-memory evidence](working_memory_vs_baseline.csv). Focused timing results are mixed and do not support a general speed claim. The CSV records focused timing only for cases that were measured. Full source/object bindings and measurement definitions are in [working-memory report](working_memory_report.json); the five alternating timing pairs and three same-baseline controls are preserved in [raw timing rounds](working_memory_timing_rounds.json). The source-bound denoiser state check reports 68 optional state bytes across 90 configurations. Compiler-reported NSQ stack reservations are historical function reservations, not this measurement.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 356816 | 0 | 0 | 356816 |
| android | 356832 | 472 | 0 | 357304 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC effective setting is 1. FEC records 18/18 recovery wins and aggregate recovery ratio 0.504119; packet-byte ratio 0.993345 passes the <=1 gate. Source criteria are C1 18/18, C2 18/18, C3 18/18, C4 18/18; all four source criteria pass. Strict source gate: PASS; standard interop gate: PASS. The scored recovery aggregate contains 18 10/20 ms cases, while the interoperability run covers 36 configurations and emits 72 direction rows; 40/60 ms cases remain in the raw report but are excluded from that aggregate.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV). Aggregate re-entry NRMSE is 44.1% lower; aggregate gain error is 12.5% higher. DTX comparison: FAIL. The original acceptance criteria are unchanged; all measurements are retained. See [DTX acceptance](dtx_acceptance.json) and [runner recovery provenance](measurement_runner_recovery.json).

| Field | Value |
|---|---:|
| dtx_comparison | FAIL |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.426114 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.852367 |
| reentry_gain_db_official | 1.646307 |
| silence_dtx_opuscpp | 406 |
| silence_dtx_official | 406 |
| steady_noise_dtx_opuscpp | 120 |
| steady_noise_dtx_official | 0 |

[All DTX false-positive and re-entry measurements](dtx_metrics.csv).

## Fresh compatibility checks

RFC 8251 decoder vectors: 24/24; encode interoperability: 96/96. Current API/lookahead lines and source-bound voice-conditioning, denoiser-state, transient and quiet-start results are preserved in [compatibility validation](compatibility_validation.json), together with raw command output. No sanitizer was run. Broader historical conformance comparisons, per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` retain their original source/date labels.

## Mode selection

```text
speech_like,silk_pct=0.0,hybrid_pct=88.0,celt_pct=12.0
harmonic_music,silk_pct=0.0,hybrid_pct=5.7,celt_pct=94.3
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=22.182 decode_ms=21.4883 checksum=340886119
mono-high bytes=115 encode_ms=23.8042 decode_ms=21.6102 checksum=2633358364
stereo-mid bytes=102 encode_ms=107.399 decode_ms=34.9897 checksum=3294895434
stereo-high bytes=147 encode_ms=94.7839 decode_ms=31.5463 checksum=1997488960
```

## Current selected checks

[Source-bound results and commands](selected_regressions.json) retain every output.

- `voice_conditioning_release`: passed.
- `voice_denoise_state`: passed.
- `transient_invariant`: passed.
- `voip_quiet_start_latch`: passed.
