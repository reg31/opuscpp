# Complete production metrics

Measured: `487a5a3f6bfb1bd87779997f793f77618708eaff`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Current objective

| Criterion | Fresh result | Status |
|---|---|---|
| Every individual encoding median strictly faster than OFF | 15/17 | FAIL |
| Smaller 16-to-256 kbps absolute encoding-time increase | 180.9660 ms vs OFF 189.5949 ms | PASS |
| Every published quality delta nonnegative | 713 negative fields | FAIL |
| Each normal image at most 350,000 B, including text+data+bss | Host 519,724 B; Android 503,604 B | FAIL |

The combined goal is unmet. Slower encoding medians: Hazel FEC60 24 kbps (0.861331x), David FEC60 24 kbps (0.775637x). These are maintained elapsed-time medians, not process-CPU measurements. All raw repetitions, orders and adverse tails are retained. All 12 quality values in both namespaces are unchanged from the prior complete full (Source `8ab28e50ed3c`); this is not a quality-parity claim. The growth comparison uses the same 60-second stereo AUDIO workload and full-precision raw medians.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 674 |
| Publication | 806 | 12 | 713 |

| Metric | Better direction | Below official / 498 matrix cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 10 | 7 |
| segmental_snr_db | Higher | 12 | 10 |
| rms_error | Lower | 10 | 7 |
| mean_abs_error | Lower | 10 | 8 |
| pesq_style | Higher | 14 | 9 |
| visqol_style | Higher | 55 | 61 |
| logband_corr | Higher | 55 | 67 |
| logband_error | Lower | 54 | 55 |
| celt_quality | Higher | 123 | 140 |
| celt_masked_error | Lower | 123 | 140 |
| celt_highband_error | Lower | 182 | 192 |
| stereo_width_error | Lower | 26 | 17 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 1.01x to 1.08x | 1.19x to 1.94x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20528 | 20496 | Not exposed |
| official_encoder_ch1 | 31808 | 31904 | 31668 |
| current_decoder_ch1 | 8448 | 8304 | Not exposed |
| official_decoder_ch1 | 18496 | 18576 | 18468 |
| current_encoder_ch2 | 30640 | 30560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17536 | 16208 | Not exposed |
| official_decoder_ch2 | 27120 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 519724 | 0 | 0 | 519724 |
| android | 503132 | 472 | 0 | 503604 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | FAIL |
| Standard interoperability comparison | FAIL |
| Scored recovery wins | 17/18 |
| Maximum recovery-error ratio | 1.514000 |
| Recovery-error ratio | 0.62938 |
| Packet-byte ratio | 0.997487 |
| Backup coverage: opuscpp | 16/18 |
| Backup coverage: official | 15/18 |
| Configurations | 36 |
| Direction rows | 72 |
| C1 | 18/18 |
| C2 | 18/18 |
| C3 | 18/18 |
| C4 | 16/18 |

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
mono-mid bytes=76 encode_ms=18.3975 decode_ms=15.8911 checksum=340886119
mono-high bytes=115 encode_ms=20.3271 decode_ms=14.958 checksum=2633358364
stereo-mid bytes=103 encode_ms=61.9991 decode_ms=31.2014 checksum=4179984942
stereo-high bytes=147 encode_ms=54.8113 decode_ms=31.5502 checksum=3681257371
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
