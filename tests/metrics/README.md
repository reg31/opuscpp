# Complete production metrics

Codex independently refreshed production `5cf9f1cd572a342b7cf13727d64f5d4bc754c6d8` on 2026-10-04 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. Fresh checks on source `5cf9f1c` include 24/24 RFC 8251 decode vectors and 96/96 encode-interoperability cases; API and selected regression results are in [compatibility validation](compatibility_validation.json).

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 808 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 14 | 5 |
| segmental_snr_db | Higher | 24 | 10 |
| rms_error | Lower | 14 | 5 |
| mean_abs_error | Lower | 15 | 7 |
| pesq_style | Higher | 27 | 10 |
| visqol_style | Higher | 66 | 94 |
| logband_corr | Higher | 70 | 119 |
| logband_error | Lower | 62 | 81 |
| celt_quality | Higher | 117 | 199 |
| celt_masked_error | Lower | 117 | 199 |
| celt_highband_error | Lower | 254 | 305 |
| stereo_width_error | Lower | 28 | 17 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.10x to 1.30x; decode: 1.17x to 1.69x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. The dedicated 11-rate denoiser overhead spans -0.279689% to +5.033792%; any small negative overhead remains explicit. The nine-rate feature summary selects only the tracked headline BITRATES and spans -0.3% to 5.0%. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19760 | 19792 | Not exposed |
| official_encoder_ch1 | 31824 | 31872 | 31668 |
| current_decoder_ch1 | 11888 | 10752 | Not exposed |
| official_decoder_ch1 | 18336 | 18544 | 18468 |
| current_encoder_ch2 | 30000 | 29920 | Not exposed |
| official_encoder_ch2 | 48880 | 48720 | 48684 |
| current_decoder_ch2 | 17712 | 17696 | Not exposed |
| official_decoder_ch2 | 27344 | 27280 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes from the fresh state/bounds test. Temporary denoiser stack usage was not measured in this refresh. Compiler-reported stack reservations in named historical checkpoints are not current peak-memory measurements.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 380464 | 0 | 0 | 380464 |
| android | 374688 | 472 | 0 | 375160 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC strict source gate: PASS ; separate scored recovery comparison: 18/18 ratios at or below official, maximum 0.977027. Standard interop gate: PASS; source-quality counts are C1 18/18, C2 18/18, C3 18/18, C4 18/18. For the scored 18-case 10/20ms subset, the FEC summary reports recovery-error ratio 0.504119, packet-byte ratio 0.993345 and backup coverage 18/18 for opuscpp versus 15/18 for official Opus. The expanded run has 36 configurations and 72 direction rows; strict and standard scopes are separate. FEC is retained as regression coverage, with no further optimization in this round.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

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

[All DTX false-positive and re-entry measurements](dtx_metrics.csv); unchanged aggregate gate: FAIL ([acceptance record](dtx_acceptance.json)).

## Fresh compatibility and API checks

This full refresh is bound to source `5cf9f1cd572a342b7cf13727d64f5d4bc754c6d8` and official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. RFC 8251 decode: 24/24; encode interoperability: 96/96. API behavior and selected source-bound regressions are recorded in [compatibility validation](compatibility_validation.json). No sanitizer was run.

## Mode selection

```text
speech_like,silk_pct=0.0,hybrid_pct=88.0,celt_pct=12.0
harmonic_music,silk_pct=0.0,hybrid_pct=5.7,celt_pct=94.3
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=22.0005 decode_ms=21.6384 checksum=340886119
mono-high bytes=115 encode_ms=23.7505 decode_ms=19.3316 checksum=2633358364
stereo-mid bytes=102 encode_ms=91.6174 decode_ms=30.1764 checksum=3294895434
stereo-high bytes=147 encode_ms=98.5636 decode_ms=32.0096 checksum=1997488960
```

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.

[README-to-raw metric coverage](readme_metric_coverage.json) maps each current summary and table to its fresh results; historical sections and unavailable measurements are listed separately. [Complete raw/export hash inventory](benchmark_metric_inventory.json) records the hashes of every generated metric artifact.
