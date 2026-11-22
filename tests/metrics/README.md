# Complete production metrics

Measured: `374a3682988b3e7a16cc938e2901870baaa7b011`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 745 |
| Publication | 806 | 12 | 775 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 29 | 16 |
| segmental_snr_db | Higher | 25 | 16 |
| rms_error | Lower | 29 | 16 |
| mean_abs_error | Lower | 24 | 15 |
| pesq_style | Higher | 25 | 16 |
| visqol_style | Higher | 64 | 72 |
| logband_corr | Higher | 61 | 78 |
| logband_error | Lower | 64 | 73 |
| celt_quality | Higher | 113 | 123 |
| celt_masked_error | Lower | 113 | 123 |
| celt_highband_error | Lower | 171 | 213 |
| stereo_width_error | Lower | 27 | 14 |

Nominal S17 scalar music EOF quality passed 108/108 fields across 9 rates. Each 60-second stereo clip uses one 20 ms drain. [Music EOF quality and payload rates](quality_run_metadata.json).

[Matrix](production_quality_matrix.csv) Â· [Publication](production_quality_published.csv) Â· [Headline controls](quality_official_full_precision.csv) Â· [Denoising](voice_denoise_broad.csv) Â· [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.97x to 1.05x | 1.04x to 1.44x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) Â· [Encode](encode_speed_vs_official.csv) Â· [Decode](decode_speed_vs_official.csv) Â· [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) Â· [Postfilter quality](postfilter_quality_voip.csv) Â· [Denoiser timing](voice_denoise_timing.csv) Â· [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20464 | 20832 | Not exposed |
| official_encoder_ch1 | 31824 | 31904 | 31668 |
| current_decoder_ch1 | 8496 | 8320 | Not exposed |
| official_decoder_ch1 | 18432 | 18544 | 18468 |
| current_encoder_ch2 | 30656 | 30544 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17520 | 16208 | Not exposed |
| official_decoder_ch2 | 27136 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 305212 | 0 | 19712 | 324924 |
| android | 294115 | 152 | 19664 | 313931 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.886834 |
| Recovery-error ratio | 0.390692 |
| Packet-byte ratio | 0.988523 |
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
| reentry_nrmse_opuscpp | 0.311453 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.609972 |
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
mono-mid bytes=76 encode_ms=20.5974 decode_ms=17.7369 checksum=340886119
mono-high bytes=115 encode_ms=21.8223 decode_ms=17.9983 checksum=2633358364
stereo-mid bytes=103 encode_ms=43.8928 decode_ms=32.6901 checksum=1794710616
stereo-high bytes=147 encode_ms=48.1949 decode_ms=35.1713 checksum=2844469021
```

[Metric coverage](readme_metric_coverage.json) Â· [Artifact hashes](benchmark_metric_inventory.json).

## Scalar encoder CPU

Opuscpp `374a368` had the lower encode CPU median in all 17 cases against scalar libopus `51bd2e649f45` with SIMD and dispatch disabled. Medians use 15 matched runs per codec and are shown in milliseconds per original clip, rounded to three decimals.

| Workload | Requested kbps | Clip seconds | opuscpp CPU ms | libopus scalar CPU ms |
|---|---:|---:|---:|---:|
| AUDIO | 16 | 60 | 234.375 | 320.312 |
| AUDIO | 24 | 60 | 247.768 | 344.866 |
| AUDIO | 32 | 60 | 258.681 | 305.556 |
| AUDIO | 48 | 60 | 285.156 | 310.547 |
| AUDIO | 64 | 60 | 248.047 | 306.641 |
| AUDIO | 96 | 60 | 281.250 | 377.232 |
| AUDIO | 128 | 60 | 326.562 | 426.562 |
| AUDIO | 192 | 60 | 463.542 | 604.167 |
| AUDIO | 256 | 60 | 388.021 | 492.188 |
| Hazel VOIP | 16 | 13.46 | 84.821 | 133.929 |
| Hazel VOIP | 32 | 13.46 | 87.054 | 135.417 |
| Hazel VOIP | 64 | 13.46 | 84.310 | 119.141 |
| Hazel FEC60 | 24 | 13.44 | 117.188 | 159.180 |
| David VOIP | 16 | 11.8 | 74.219 | 113.932 |
| David VOIP | 32 | 11.8 | 75.149 | 120.536 |
| David VOIP | 64 | 11.8 | 96.788 | 137.153 |
| David FEC60 | 24 | 11.76 | 103.299 | 137.153 |

From 16 to 256 kbps, absolute CPU growth per 60-second AUDIO clip was **153.646 ms** for opuscpp and **171.875 ms** for scalar libopus. Relative growth was 65.56% and 53.66%, respectively.
