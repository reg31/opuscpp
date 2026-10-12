# Complete production metrics

Measured: `fe620396de3daf196e62a5b68a13cb9aec93f059`; pinned official Opus: `503d81b138d76621aae4b12786e90de48aa8db3a`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 793 |
| Publication | 806 | 12 | 921 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 24 | 7 |
| segmental_snr_db | Higher | 21 | 6 |
| rms_error | Lower | 24 | 7 |
| mean_abs_error | Lower | 19 | 4 |
| pesq_style | Higher | 24 | 6 |
| visqol_style | Higher | 70 | 101 |
| logband_corr | Higher | 65 | 115 |
| logband_error | Lower | 70 | 94 |
| celt_quality | Higher | 119 | 170 |
| celt_masked_error | Lower | 119 | 170 |
| celt_highband_error | Lower | 211 | 228 |
| stereo_width_error | Lower | 27 | 13 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.94x to 1.04x | 1.13x to 1.78x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19760 | 19808 | Not exposed |
| official_encoder_ch1 | 31856 | 31920 | 31668 |
| current_decoder_ch1 | 8432 | 8304 | Not exposed |
| official_decoder_ch1 | 18480 | 18560 | 18468 |
| current_encoder_ch2 | 30000 | 29872 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17584 | 16224 | Not exposed |
| official_decoder_ch2 | 27104 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 426156 | 0 | 0 | 426156 |
| android | 420916 | 472 | 0 | 421388 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.990260 |
| Recovery-error ratio | 0.435776 |
| Packet-byte ratio | 0.986486 |
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
| reentry_nrmse_opuscpp | 0.316582 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.400718 |
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
mono-mid bytes=76 encode_ms=20.8714 decode_ms=20.8191 checksum=340886119
mono-high bytes=115 encode_ms=22.2531 decode_ms=18.526 checksum=2633358364
stereo-mid bytes=102 encode_ms=83.6352 decode_ms=29.1817 checksum=3294895434
stereo-high bytes=147 encode_ms=89.6401 decode_ms=30.844 checksum=1997488960
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
