# Complete production metrics

Measured: `3b0ae85624ef08b710160bd710353aa3cde9951b`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 677 |
| Publication | 806 | 12 | 719 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 11 | 9 |
| segmental_snr_db | Higher | 12 | 10 |
| rms_error | Lower | 11 | 9 |
| mean_abs_error | Lower | 10 | 9 |
| pesq_style | Higher | 14 | 9 |
| visqol_style | Higher | 55 | 62 |
| logband_corr | Higher | 57 | 69 |
| logband_error | Lower | 54 | 55 |
| celt_quality | Higher | 122 | 139 |
| celt_masked_error | Lower | 122 | 139 |
| celt_highband_error | Lower | 182 | 192 |
| stereo_width_error | Lower | 27 | 17 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.99x to 1.08x | 1.17x to 2.00x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20480 | 20448 | Not exposed |
| official_encoder_ch1 | 31824 | 31872 | 31668 |
| current_decoder_ch1 | 8448 | 8320 | Not exposed |
| official_decoder_ch1 | 18496 | 18560 | 18468 |
| current_encoder_ch2 | 30640 | 30560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17568 | 16224 | Not exposed |
| official_decoder_ch2 | 27104 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 512892 | 0 | 0 | 512892 |
| android | 490428 | 472 | 0 | 490900 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | FAIL |
| Standard interoperability comparison | FAIL |
| Scored recovery wins | 10/18 |
| Maximum recovery-error ratio | 1.514000 |
| Recovery-error ratio | 0.888704 |
| Packet-byte ratio | 1.0074 |
| Backup coverage: opuscpp | 15/18 |
| Backup coverage: official | 15/18 |
| Configurations | 36 |
| Direction rows | 72 |
| C1 | 17/18 |
| C2 | 18/18 |
| C3 | 14/18 |
| C4 | 15/18 |

[FEC direction fields](fec_interop_metrics.csv) · [Source comparisons](fec_source_metrics.json) · [Ratios and raw output](fec_run_metadata.json).

| DTX field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.310510 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.596741 |
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
mono-mid bytes=76 encode_ms=18.2689 decode_ms=16.0549 checksum=340886119
mono-high bytes=115 encode_ms=20.885 decode_ms=15.9251 checksum=2633358364
stereo-mid bytes=103 encode_ms=64.4587 decode_ms=34.4595 checksum=4179984942
stereo-high bytes=147 encode_ms=57.6275 decode_ms=34.6737 checksum=3681257371
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
