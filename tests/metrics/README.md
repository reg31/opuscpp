# Complete production metrics

The [measured Opuscpp revision](https://github.com/reg31/opuscpp/commit/916753c04d94530056a46bf8e3d75d5d3af91770) is compared with [pinned official Opus](https://github.com/xiph/opus/commit/51bd2e649f4516d103ab9a7f22a768595621e847).

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 650 |
| Publication | 806 | 12 | 652 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 24 | 12 |
| segmental_snr_db | Higher | 21 | 14 |
| rms_error | Lower | 24 | 12 |
| mean_abs_error | Lower | 22 | 13 |
| pesq_style | Higher | 20 | 13 |
| visqol_style | Higher | 49 | 58 |
| logband_corr | Higher | 51 | 73 |
| logband_error | Lower | 47 | 53 |
| celt_quality | Higher | 103 | 109 |
| celt_masked_error | Lower | 103 | 109 |
| celt_highband_error | Lower | 161 | 173 |
| stereo_width_error | Lower | 25 | 13 |

Nominal S17 scalar music EOF quality met the frozen tolerance in 107/108 fields across 9 rates. Music16 high-band error retained its pre-audit miss. Each 60-second stereo clip uses one 20 ms drain. [Music EOF quality and payload rates](quality_run_metadata.json).

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.95x to 1.07x | 1.04x to 1.44x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20448 | 20768 | Not exposed |
| official_encoder_ch1 | 31824 | 31952 | 31668 |
| current_decoder_ch1 | 8432 | 8320 | Not exposed |
| official_decoder_ch1 | 18496 | 18576 | 18468 |
| current_encoder_ch2 | 30528 | 30464 | Not exposed |
| official_encoder_ch2 | 48928 | 48736 | 48684 |
| current_decoder_ch2 | 17536 | 16224 | Not exposed |
| official_decoder_ch2 | 27088 | 27248 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 392992 | 0 | 19712 | 412704 |
| android | 386955 | 152 | 19664 | 406771 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.876003 |
| Recovery-error ratio | 0.414133 |
| Packet-byte ratio | 0.988727 |
| Backup coverage: opuscpp | 18/18 |
| Backup coverage: official | 15/18 |
| Configurations | 36 |
| Direction rows | 72 |
| C1 | 18/18 |
| C2 | 18/18 |
| C3 | 18/18 |
| C4 | 18/18 |

[FEC direction fields](fec_interop_metrics.csv) · [Source comparisons](fec_source_metrics.json) · [Ratios and raw output](fec_run_metadata.json).

| DTX field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.311091 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.612919 |
| reentry_gain_db_official | 1.646307 |
| silence_dtx_opuscpp | 406 |
| silence_dtx_official | 406 |
| steady_noise_dtx_opuscpp | 120 |
| steady_noise_dtx_official | 0 |

[DTX fields](dtx_metrics.csv).

## Compatibility

| Measurement | Result |
|---|---:|
| RFC 8251 decode configurations | 24/24 |
| Encode interoperability cases | 96/96 |

[API and compatibility data](compatibility_validation.json).

## Mode selection

| Signal | silk_pct | hybrid_pct | celt_pct |
|---|---:|---:|---:|
| speech_like | 0.0 | 88.0 | 12.0 |
| harmonic_music | 0.0 | 5.7 | 94.3 |

[Mode data](mode_balance_metrics.json).

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
