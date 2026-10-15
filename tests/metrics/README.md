# Complete production metrics

Measured: `b0fdbb904059273693b019bc64bced7e1cc6efb4`; pinned official Opus: `503d81b138d76621aae4b12786e90de48aa8db3a`.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 851 |
| Publication | 806 | 12 | 897 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 23 | 9 |
| segmental_snr_db | Higher | 23 | 10 |
| rms_error | Lower | 23 | 9 |
| mean_abs_error | Lower | 24 | 9 |
| pesq_style | Higher | 25 | 9 |
| visqol_style | Higher | 81 | 91 |
| logband_corr | Higher | 80 | 101 |
| logband_error | Lower | 81 | 88 |
| celt_quality | Higher | 121 | 158 |
| celt_masked_error | Lower | 121 | 158 |
| celt_highband_error | Lower | 220 | 236 |
| stereo_width_error | Lower | 29 | 19 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.90x to 1.01x | 1.10x to 1.85x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19792 | 19840 | Not exposed |
| official_encoder_ch1 | 31872 | 31904 | 31668 |
| current_decoder_ch1 | 8432 | 8336 | Not exposed |
| official_decoder_ch1 | 18448 | 18560 | 18468 |
| current_encoder_ch2 | 29888 | 29856 | Not exposed |
| official_encoder_ch2 | 48944 | 48736 | 48684 |
| current_decoder_ch2 | 17600 | 16224 | Not exposed |
| official_decoder_ch2 | 27136 | 27280 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 443516 | 0 | 0 | 443516 |
| android | 442280 | 472 | 0 | 442752 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | PASS |
| Standard interoperability comparison | PASS |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.883341 |
| Recovery-error ratio | 0.445586 |
| Packet-byte ratio | 0.997555 |
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
| reentry_nrmse_opuscpp | 0.297378 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.389987 |
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
mono-mid bytes=76 encode_ms=24.8836 decode_ms=22.1672 checksum=340886119
mono-high bytes=115 encode_ms=27.8825 decode_ms=21.0611 checksum=2633358364
stereo-mid bytes=103 encode_ms=105.441 decode_ms=37.4968 checksum=58924144
stereo-high bytes=147 encode_ms=116.423 decode_ms=37.8364 checksum=2993918497
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
