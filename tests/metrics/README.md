# Complete production metrics

Codex independently refreshed production `99c2bcbdecbbdbff94d9e5a2a81803e15c271b2b` on 2026-10-02 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. Fresh compatibility checks were run for this source; raw results and bindings are linked below.

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

AUDIO encode ratios: 1.08x to 1.29x; decode: 1.22x to 1.83x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

The encoder and decoder rows are from the complete source `99c2bcb` refresh, measured together with 256 instances per group across three fresh processes and FEC/denoising disabled. Private/working-set page deltas are not exact object sizes.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 23824 | 23824 | Not exposed |
| official_encoder_ch1 | 31824 | 31904 | 31668 |
| current_decoder_ch1 | 14176 | 13120 | Not exposed |
| official_decoder_ch1 | 18400 | 18544 | 18468 |
| current_encoder_ch2 | 36080 | 35968 | Not exposed |
| official_encoder_ch2 | 48880 | 48720 | 48684 |
| current_decoder_ch2 | 21344 | 21360 | Not exposed |
| official_decoder_ch2 | 27376 | 27248 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 358184 | 0 | 0 | 358184 |
| android | 358716 | 472 | 0 | 359188 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC: 18/18 strict scored ratios; maximum 0.977027. C1 recovered-source fidelity, C2 following-frame fidelity, C3 source transition error and C4 recovered fidelity versus own PLC each pass 18/18. FEC effective setting is 1. The scored recovery aggregate contains 18 10/20 ms cases: 18/18 recovery wins, aggregate recovery ratio 0.504119, and packet-byte ratio 0.993345. The separate interoperability run covers 36 configurations and emits 72 direction rows; 40/60 ms cases remain in the raw report but are excluded from that scored aggregate. Source criteria C1-C4 each pass 18/18. See [FEC acceptance](acceptance.json).

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX acceptance FAIL under the unchanged criteria: false positives remain zero, aggregate re-entry NRMSE is 44.1% lower, but aggregate gain error is 12.5% higher; steady-noise DTX counts are 120 versus 0 for official. No waiver was applied. See [DTX acceptance](dtx_acceptance.json) and [runner recovery provenance](measurement_runner_recovery.json). All per-case DTX rows are retained below and in the CSV.

DTX aggregate fields (individual material losses remain in the CSV):

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

## Mode selection

```text
speech_like,silk_pct=0.0,hybrid_pct=88.0,celt_pct=12.0
harmonic_music,silk_pct=0.0,hybrid_pct=5.7,celt_pct=94.3
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=21.7555 decode_ms=23.1211 checksum=340886119
mono-high bytes=115 encode_ms=23.6736 decode_ms=19.0117 checksum=2633358364
stereo-mid bytes=102 encode_ms=89.1458 decode_ms=29.2035 checksum=3294895434
stereo-high bytes=147 encode_ms=94.8076 decode_ms=30.9023 checksum=1997488960
```

## Fresh compatibility checks

Fresh source-bound results for `99c2bcb` include 24/24 RFC 8251 decoder vectors, 96 encode-interoperability cases, API/lookahead behavior and four focused current-source regressions. No sanitizer was run. Full source, fixture, binary and command bindings are in [fresh compatibility metadata](compatibility_validation.json).

## Current selected checks

- `voice_conditioning_release`: PASS (133 checks).
- `voice_denoise_state`: PASS (90 cases).
- `transient_invariant`: PASS (4 checks).
- `voip_quiet_start_latch`: PASS (26 checks).

Raw outputs and the 99c2bcb source binding are in [selected regression metadata](selected_regressions.json).

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Named historical compatibility, startup and latency/lookahead checkpoints retain their original source commits and validation dates. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.
