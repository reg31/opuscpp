# Complete production metrics

Measured: `96ef72d19446079c8647178f32591f00fe149d64`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 658 |
| Publication | 806 | 12 | 666 |

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
| celt_quality | Higher | 103 | 110 |
| celt_masked_error | Lower | 103 | 110 |
| celt_highband_error | Lower | 169 | 185 |
| stereo_width_error | Lower | 25 | 13 |

Nominal S17 scalar music EOF quality met the frozen tolerance in 107/108 fields across 9 rates. Music16 high-band error retained its pre-audit miss. Each 60-second stereo clip uses one 20 ms drain. [Music EOF quality and payload rates](quality_run_metadata.json).

[Matrix](production_quality_matrix.csv) Â· [Publication](production_quality_published.csv) Â· [Headline controls](quality_official_full_precision.csv) Â· [Denoising](voice_denoise_broad.csv) Â· [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.86x to 1.00x | 0.94x to 1.36x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) Â· [Encode](encode_speed_vs_official.csv) Â· [Decode](decode_speed_vs_official.csv) Â· [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) Â· [Postfilter quality](postfilter_quality_voip.csv) Â· [Denoiser timing](voice_denoise_timing.csv) Â· [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20464 | 20800 | Not exposed |
| official_encoder_ch1 | 31824 | 31920 | 31668 |
| current_decoder_ch1 | 8432 | 8288 | Not exposed |
| official_decoder_ch1 | 18496 | 18576 | 18468 |
| current_encoder_ch2 | 30528 | 30480 | Not exposed |
| official_encoder_ch2 | 48928 | 48736 | 48684 |
| current_decoder_ch2 | 17536 | 16224 | Not exposed |
| official_decoder_ch2 | 27088 | 27248 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 384648 | 0 | 19712 | 404360 |
| android | 379387 | 184 | 19664 | 399235 |

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

[FEC direction fields](fec_interop_metrics.csv) Â· [Source comparisons](fec_source_metrics.json) Â· [Ratios and raw output](fec_run_metadata.json).

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

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=19.8315 decode_ms=17.3536 checksum=340886119
mono-high bytes=115 encode_ms=21.6967 decode_ms=16.9115 checksum=2633358364
stereo-mid bytes=103 encode_ms=42.0616 decode_ms=33.8559 checksum=1794710616
stereo-high bytes=147 encode_ms=48.4875 decode_ms=35.1997 checksum=2844469021
```

[Metric coverage](readme_metric_coverage.json) Â· [Artifact hashes](benchmark_metric_inventory.json).
