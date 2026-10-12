# Complete production metrics

Measured: `d378640dee7303f7c903e3f4075273353dd0d638`; pinned official Opus: `503d81b138d76621aae4b12786e90de48aa8db3a`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 868 |
| Publication | 806 | 12 | 1009 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 23 | 8 |
| segmental_snr_db | Higher | 22 | 9 |
| rms_error | Lower | 23 | 8 |
| mean_abs_error | Lower | 25 | 8 |
| pesq_style | Higher | 26 | 9 |
| visqol_style | Higher | 82 | 111 |
| logband_corr | Higher | 76 | 121 |
| logband_error | Lower | 81 | 101 |
| celt_quality | Higher | 134 | 189 |
| celt_masked_error | Lower | 134 | 189 |
| celt_highband_error | Lower | 218 | 241 |
| stereo_width_error | Lower | 24 | 15 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.93x to 1.04x | 1.17x to 1.76x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19760 | 19792 | Not exposed |
| official_encoder_ch1 | 31856 | 31904 | 31668 |
| current_decoder_ch1 | 8432 | 8304 | Not exposed |
| official_decoder_ch1 | 18464 | 18576 | 18468 |
| current_encoder_ch2 | 29888 | 29856 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17712 | 16256 | Not exposed |
| official_decoder_ch2 | 27088 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 429844 | 0 | 0 | 429844 |
| android | 426596 | 472 | 0 | 427068 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.990260 |
| Recovery-error ratio | 0.434577 |
| Packet-byte ratio | 0.986689 |
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
| reentry_nrmse_opuscpp | 0.323476 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.407260 |
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
mono-mid bytes=76 encode_ms=20.9373 decode_ms=20.6265 checksum=340886119
mono-high bytes=115 encode_ms=22.2594 decode_ms=18.5576 checksum=2633358364
stereo-mid bytes=103 encode_ms=86.4801 decode_ms=30.6697 checksum=4168508527
stereo-high bytes=147 encode_ms=95.9031 decode_ms=32.1596 checksum=3221589587
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
