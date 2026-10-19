# Complete production metrics

Measured: `aea62e338b0748d193120d1675fdbde46f369f28`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 737 |
| Publication | 806 | 12 | 773 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 17 | 12 |
| segmental_snr_db | Higher | 13 | 10 |
| rms_error | Lower | 17 | 12 |
| mean_abs_error | Lower | 12 | 9 |
| pesq_style | Higher | 14 | 10 |
| visqol_style | Higher | 64 | 72 |
| logband_corr | Higher | 64 | 89 |
| logband_error | Lower | 62 | 67 |
| celt_quality | Higher | 131 | 146 |
| celt_masked_error | Lower | 131 | 146 |
| celt_highband_error | Lower | 188 | 184 |
| stereo_width_error | Lower | 24 | 16 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.93x to 1.15x | 1.14x to 1.76x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20480 | 20432 | Not exposed |
| official_encoder_ch1 | 31824 | 31888 | 31668 |
| current_decoder_ch1 | 8416 | 8320 | Not exposed |
| official_decoder_ch1 | 18432 | 18576 | 18468 |
| current_encoder_ch2 | 30656 | 30560 | Not exposed |
| official_encoder_ch2 | 48928 | 48720 | 48684 |
| current_decoder_ch2 | 17568 | 16224 | Not exposed |
| official_decoder_ch2 | 27104 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 505580 | 0 | 0 | 505580 |
| android | 485284 | 472 | 0 | 485756 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | FAIL |
| Standard interoperability comparison | FAIL |
| Scored recovery wins | 17/18 |
| Maximum recovery-error ratio | 1.514000 |
| Recovery-error ratio | 0.682841 |
| Packet-byte ratio | 0.996265 |
| Backup coverage: opuscpp | 15/18 |
| Backup coverage: official | 15/18 |
| Configurations | 36 |
| Direction rows | 72 |
| C1 | 17/18 |
| C2 | 18/18 |
| C3 | 18/18 |
| C4 | 15/18 |

[FEC direction fields](fec_interop_metrics.csv) · [Source comparisons](fec_source_metrics.json) · [Ratios and raw output](fec_run_metadata.json).

| DTX field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.309319 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.603645 |
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

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=18.0592 decode_ms=15.8845 checksum=340886119
mono-high bytes=115 encode_ms=20.0796 decode_ms=15.0857 checksum=2633358364
stereo-mid bytes=102 encode_ms=73.3034 decode_ms=31.1656 checksum=3625126390
stereo-high bytes=147 encode_ms=80.5314 decode_ms=30.4155 checksum=2476207253
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
