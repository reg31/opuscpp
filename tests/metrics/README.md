# Complete production metrics

Measured: `6d5a83e7fb888c4e031082c35ee4258709332458`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 809 |
| Publication | 806 | 12 | 836 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 23 | 10 |
| segmental_snr_db | Higher | 20 | 9 |
| rms_error | Lower | 23 | 10 |
| mean_abs_error | Lower | 24 | 11 |
| pesq_style | Higher | 21 | 8 |
| visqol_style | Higher | 77 | 93 |
| logband_corr | Higher | 75 | 108 |
| logband_error | Lower | 70 | 84 |
| celt_quality | Higher | 116 | 137 |
| celt_masked_error | Lower | 116 | 137 |
| celt_highband_error | Lower | 214 | 211 |
| stereo_width_error | Lower | 30 | 18 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.91x to 1.06x | 1.22x to 1.93x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19808 | 19808 | Not exposed |
| official_encoder_ch1 | 31824 | 31904 | 31668 |
| current_decoder_ch1 | 8432 | 8320 | Not exposed |
| official_decoder_ch1 | 18448 | 18560 | 18468 |
| current_encoder_ch2 | 29888 | 29856 | Not exposed |
| official_encoder_ch2 | 48944 | 48736 | 48684 |
| current_decoder_ch2 | 17600 | 16224 | Not exposed |
| official_decoder_ch2 | 27120 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 458744 | 0 | 0 | 458744 |
| android | 458128 | 472 | 0 | 458600 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.903303 |
| Recovery-error ratio | 0.416643 |
| Packet-byte ratio | 0.995654 |
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
| reentry_nrmse_opuscpp | 0.295772 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.373986 |
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
mono-mid bytes=76 encode_ms=18.4083 decode_ms=15.9541 checksum=340886119
mono-high bytes=115 encode_ms=21.2735 decode_ms=14.9893 checksum=2633358364
stereo-mid bytes=103 encode_ms=75.2727 decode_ms=28.9501 checksum=3799820484
stereo-high bytes=147 encode_ms=82.6381 decode_ms=29.8753 checksum=2476207253
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
