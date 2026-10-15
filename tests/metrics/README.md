# Complete production metrics

Measured: `1bb64d9bb81227599f4526633d9b2c20883718f0`; pinned official Opus: `503d81b138d76621aae4b12786e90de48aa8db3a`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 898 |
| Publication | 806 | 12 | 982 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 26 | 8 |
| segmental_snr_db | Higher | 25 | 11 |
| rms_error | Lower | 26 | 8 |
| mean_abs_error | Lower | 27 | 9 |
| pesq_style | Higher | 27 | 11 |
| visqol_style | Higher | 91 | 114 |
| logband_corr | Higher | 87 | 128 |
| logband_error | Lower | 85 | 105 |
| celt_quality | Higher | 128 | 171 |
| celt_masked_error | Lower | 128 | 171 |
| celt_highband_error | Lower | 212 | 223 |
| stereo_width_error | Lower | 36 | 23 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.87x to 0.99x | 1.16x to 1.88x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19792 | 19840 | Not exposed |
| official_encoder_ch1 | 31872 | 31936 | 31668 |
| current_decoder_ch1 | 8432 | 8336 | Not exposed |
| official_decoder_ch1 | 18448 | 18576 | 18468 |
| current_encoder_ch2 | 29888 | 29856 | Not exposed |
| official_encoder_ch2 | 48944 | 48736 | 48684 |
| current_decoder_ch2 | 17600 | 16224 | Not exposed |
| official_decoder_ch2 | 27120 | 27280 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 439660 | 0 | 0 | 439660 |
| android | 435424 | 472 | 0 | 435896 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.969006 |
| Recovery-error ratio | 0.437983 |
| Packet-byte ratio | 0.986893 |
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
| reentry_nrmse_opuscpp | 0.323748 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.399521 |
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
mono-mid bytes=76 encode_ms=24.7543 decode_ms=21.7433 checksum=340886119
mono-high bytes=115 encode_ms=25.8591 decode_ms=19.1808 checksum=2633358364
stereo-mid bytes=103 encode_ms=95.8594 decode_ms=33.9359 checksum=1470193116
stereo-high bytes=147 encode_ms=109.071 decode_ms=31.8209 checksum=2703015114
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
