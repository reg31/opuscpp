# Complete production metrics

These benchmarks compare Opuscpp with official Opus.

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

In the retained scalar S17 checkpoint, music EOF quality met the frozen tolerance in 107/108 fields across 9 rates. Music16 high-band error retained its pre-audit miss. Each 60-second stereo clip uses one 20 ms drain. [Music EOF quality and payload rates](quality_run_metadata.json).

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 1.15x to 1.25x | 1.24x to 2.04x |
| Speech and FEC60 | 1.05x to 1.34x | 1.48x to 1.90x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20480 | 20784 | Not exposed |
| official_encoder_ch1 | 31808 | 31872 | 31668 |
| current_decoder_ch1 | 8384 | 8304 | Not exposed |
| official_decoder_ch1 | 18480 | 18592 | 18468 |
| current_encoder_ch2 | 30512 | 30464 | Not exposed |
| official_encoder_ch2 | 48880 | 48720 | 48684 |
| current_decoder_ch2 | 17664 | 16256 | Not exposed |
| official_decoder_ch2 | 27104 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 425988 | 0 | 20672 | 446660 |
| android | 393951 | 152 | 20624 | 414727 |

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
| RFC 8251 decode conformance | PASS (24/24) |
| Encode interoperability cases | 96/96 |

In the legacy RFC 6716 comparison, opuscpp and pinned official Opus both scored
14/24 and missed the same 10 cases.

[API and compatibility data](compatibility_validation.json).

## Mode selection

| Signal | silk_pct | hybrid_pct | celt_pct |
|---|---:|---:|---:|
| speech_like | 0.0 | 88.0 | 12.0 |
| harmonic_music | 0.0 | 5.7 | 94.3 |

[Mode data](mode_balance_metrics.json).

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
