# Complete production metrics

Codex independently refreshed production `6d931a266893dc8eea678feca2b57d15dbb16e12` on 2026-09-24 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field and the raw outputs for current compatibility checks are retained below or in linked files. Historical per-commit comparisons preserve their original provenance. The preserved `fec_s7_candidate_validation.json` is a separate historical candidate record and does not supply any current table values.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 849 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 19 | 17 |
| segmental_snr_db | Higher | 33 | 26 |
| rms_error | Lower | 19 | 17 |
| mean_abs_error | Lower | 19 | 17 |
| pesq_style | Higher | 35 | 26 |
| visqol_style | Higher | 70 | 104 |
| logband_corr | Higher | 68 | 129 |
| logband_error | Lower | 67 | 93 |
| celt_quality | Higher | 116 | 199 |
| celt_masked_error | Lower | 116 | 199 |
| celt_highband_error | Lower | 255 | 313 |
| stereo_width_error | Lower | 32 | 27 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.05x to 1.26x; decode: 1.20x to 1.87x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

All real-speech AUDIO/VOIP timing rows are retained in [voice_speed_vs_official.csv](voice_speed_vs_official.csv) and raw run metadata; no Hazel/David VOIP timing columns are added to the summary tables.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 31072 | 30976 | Not exposed |
| official_encoder_ch1 | 31824 | 31872 | 31668 |
| current_decoder_ch1 | 14080 | 13088 | Not exposed |
| official_decoder_ch1 | 18352 | 18528 | 18468 |
| current_encoder_ch2 | 46960 | 46672 | Not exposed |
| official_encoder_ch2 | 48864 | 48704 | 48684 |
| current_decoder_ch2 | 21200 | 21360 | Not exposed |
| official_decoder_ch2 | 27376 | 27264 | 27236 |

[Memory CSV](memory_vs_official.csv). The source-bound denoiser state check reports 68 optional state bytes across 90 configurations; this refresh does not claim a stack high-water measurement. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 355656 | 0 | 0 | 355656 |
| android | 365704 | 472 | 0 | 366176 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC effective setting is 1. FEC records 18/18 recovery wins and aggregate recovery ratio 0.504119; packet-byte ratio 0.993345 passes the <=1 gate. Source criteria are C1 18/18, C2 18/18, C3 18/18, C4 18/18; all four source criteria pass. Strict source gate: PASS; standard interop gate: PASS. The scored recovery aggregate contains 18 10/20 ms cases, while the interoperability run covers 36 configurations and emits 72 direction rows; 40/60 ms cases remain in the raw report but are excluded from that aggregate.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV). Aggregate re-entry NRMSE is 44.1% lower; aggregate gain error is 12.7% higher. DTX comparison: FAIL. The original acceptance criteria are unchanged; all measurements are retained. See [DTX acceptance](dtx_acceptance.json) and [runner recovery provenance](measurement_runner_recovery.json).

| Field | Value |
|---|---:|
| dtx_comparison | FAIL |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.426177 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.855549 |
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
mono-mid bytes=76 encode_ms=20.6565 decode_ms=20.4063 checksum=340886119
mono-high bytes=115 encode_ms=21.8641 decode_ms=18.0244 checksum=2633358364
stereo-mid bytes=102 encode_ms=84.9015 decode_ms=28.2041 checksum=3294895434
stereo-high bytes=147 encode_ms=90.8429 decode_ms=32.1345 checksum=1997488960
```

## Current selected checks

[Source-bound results and commands](selected_regressions.json) retain every output.

- `voice_conditioning_release`: passed.
- `voice_denoise_state`: passed.
- `transient_invariant`: passed.
- `voip_quiet_start_latch`: passed.
