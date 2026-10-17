# Complete production metrics

Measured: `766d6a6507446c18f97ca01410a195e299089384`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 741 |
| Publication | 806 | 12 | 803 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 20 | 9 |
| segmental_snr_db | Higher | 16 | 11 |
| rms_error | Lower | 20 | 9 |
| mean_abs_error | Lower | 17 | 7 |
| pesq_style | Higher | 18 | 11 |
| visqol_style | Higher | 68 | 91 |
| logband_corr | Higher | 72 | 109 |
| logband_error | Lower | 63 | 81 |
| celt_quality | Higher | 109 | 123 |
| celt_masked_error | Lower | 109 | 123 |
| celt_highband_error | Lower | 207 | 215 |
| stereo_width_error | Lower | 22 | 14 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.94x to 1.11x | 1.21x to 1.97x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20496 | 20480 | Not exposed |
| official_encoder_ch1 | 31824 | 31904 | 31668 |
| current_decoder_ch1 | 8464 | 8352 | Not exposed |
| official_decoder_ch1 | 18464 | 18560 | 18468 |
| current_encoder_ch2 | 30656 | 30560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17520 | 16224 | Not exposed |
| official_decoder_ch2 | 27136 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 485464 | 0 | 0 | 485464 |
| android | 472144 | 472 | 0 | 472616 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.903303 |
| Recovery-error ratio | 0.408094 |
| Packet-byte ratio | 0.994975 |
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
| reentry_nrmse_opuscpp | 0.296349 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.451628 |
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
| speech_like | 0.0 | 85.3 | 14.7 |
| harmonic_music | 0.0 | 1.0 | 99.0 |

[Mode data](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=17.6823 decode_ms=16.7645 checksum=340886119
mono-high bytes=115 encode_ms=21.6793 decode_ms=15.9237 checksum=2633358364
stereo-mid bytes=102 encode_ms=74.2185 decode_ms=29.8857 checksum=3625126390
stereo-high bytes=147 encode_ms=81.7216 decode_ms=31.1132 checksum=2476207253
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
