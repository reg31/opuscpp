# Complete production metrics

Codex independently refreshed production `1379e10724e422adf722da45e843b669faed56e1` on 2026-09-23 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field and the raw outputs for current compatibility checks are retained below or in linked files. Historical per-commit comparisons preserve their original provenance. The preserved `fec_s7_candidate_validation.json` is a separate historical candidate record and does not supply any current table values.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 828 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 18 | 28 |
| segmental_snr_db | Higher | 29 | 31 |
| rms_error | Lower | 18 | 28 |
| mean_abs_error | Lower | 20 | 29 |
| pesq_style | Higher | 30 | 31 |
| visqol_style | Higher | 70 | 108 |
| logband_corr | Higher | 69 | 142 |
| logband_error | Lower | 72 | 100 |
| celt_quality | Higher | 124 | 189 |
| celt_masked_error | Lower | 124 | 189 |
| celt_highband_error | Lower | 229 | 304 |
| stereo_width_error | Lower | 25 | 14 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.16x to 1.46x; decode: 1.18x to 1.81x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

All real-speech AUDIO/VOIP timing rows are retained in [voice_speed_vs_official.csv](voice_speed_vs_official.csv) and raw run metadata; no Hazel/David VOIP timing columns are added to the summary tables.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 22336 | 22272 | Not exposed |
| official_encoder_ch1 | 31728 | 31856 | 31668 |
| current_decoder_ch1 | 14208 | 13136 | Not exposed |
| official_decoder_ch1 | 18336 | 18528 | 18468 |
| current_encoder_ch2 | 37984 | 37904 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 21408 | 21344 | Not exposed |
| official_decoder_ch2 | 27264 | 27248 | 27236 |

[Memory CSV](memory_vs_official.csv). The source-bound denoiser state check reports 68 optional state bytes across 90 configurations; this refresh does not claim a stack high-water measurement. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 334128 | 0 | 0 | 334128 |
| android | 335864 | 472 | 0 | 336336 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC effective setting is 1. FEC records 18/18 recovery wins and aggregate recovery ratio 0.504119; packet-byte ratio 0.993345 passes the <=1 gate. Source criteria are C1 18/18, C2 18/18, C3 18/18, C4 18/18; all four source criteria pass. Strict source gate: PASS; standard interop gate: PASS. The scored recovery aggregate contains 18 10/20 ms cases, while the interoperability run covers 36 configurations and emits 72 direction rows; 40/60 ms cases remain in the raw report but are excluded from that aggregate.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV):

| Field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.372135 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.410035 |
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
speech_like,silk_pct=0.0,hybrid_pct=4.3,celt_pct=95.7
harmonic_music,silk_pct=0.0,hybrid_pct=0.0,celt_pct=100.0
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=21.5385 decode_ms=21.1958 checksum=340886119
mono-high bytes=115 encode_ms=22.6606 decode_ms=18.5594 checksum=2633358364
stereo-mid bytes=102 encode_ms=87.6369 decode_ms=31.808 checksum=3294895434
stereo-high bytes=147 encode_ms=95.6949 decode_ms=31.1064 checksum=1997488960
```

## Current selected checks

[Source-bound results and commands](selected_regressions.json) retain every output.

- `voice_conditioning_release`: passed.
- `voice_denoise_state`: passed.
- `transient_invariant`: passed.
- `voip_quiet_start_latch`: passed.
