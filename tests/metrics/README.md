# Complete production metrics

Measured: `28b4579b101759d2eba9275aff3f46deb3cacd1a`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 682 |
| Publication | 806 | 12 | 711 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 11 | 8 |
| segmental_snr_db | Higher | 12 | 10 |
| rms_error | Lower | 11 | 8 |
| mean_abs_error | Lower | 8 | 6 |
| pesq_style | Higher | 13 | 9 |
| visqol_style | Higher | 57 | 63 |
| logband_corr | Higher | 57 | 70 |
| logband_error | Lower | 58 | 56 |
| celt_quality | Higher | 126 | 139 |
| celt_masked_error | Lower | 126 | 139 |
| celt_highband_error | Lower | 180 | 188 |
| stereo_width_error | Lower | 23 | 15 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.91x to 1.09x | 1.15x to 1.97x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20480 | 20448 | Not exposed |
| official_encoder_ch1 | 31824 | 31872 | 31668 |
| current_decoder_ch1 | 8416 | 8304 | Not exposed |
| official_decoder_ch1 | 18432 | 18560 | 18468 |
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
| host | 507756 | 0 | 0 | 507756 |
| android | 488336 | 472 | 0 | 488808 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | FAIL |
| Standard interoperability comparison | FAIL |
| Scored recovery wins | 17/18 |
| Maximum recovery-error ratio | 1.514000 |
| Recovery-error ratio | 0.677122 |
| Packet-byte ratio | 0.995382 |
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
| reentry_nrmse_opuscpp | 0.310477 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.598115 |
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
mono-mid bytes=76 encode_ms=19.1227 decode_ms=16.5552 checksum=340886119
mono-high bytes=115 encode_ms=20.9531 decode_ms=15.6105 checksum=2633358364
stereo-mid bytes=102 encode_ms=80.0178 decode_ms=40.4919 checksum=3625126390
stereo-high bytes=147 encode_ms=103.435 decode_ms=41.2254 checksum=2476207253
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
