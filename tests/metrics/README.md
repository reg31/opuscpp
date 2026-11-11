# Complete production metrics

Measured: `9270eacdb9b1817cf77f6e1c3e43d17de1f332a7`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Current objective

| Criterion | Fresh result | Status |
|---|---|---|
| Every individual encoding median strictly faster than OFF | 14/17 | FAIL |
| Smaller 16-to-256 kbps absolute encoding-time increase | 275.1214 ms vs OFF 286.2695 ms | PASS |
| Every published quality delta nonnegative | 710 negative fields | FAIL |
| Each normal image at most 350,000 B, including text+data+bss | Host 530,940 B; Android 496,524 B | FAIL |

The combined goal is unmet. Slower encoding medians: AUDIO 128 kbps (0.965450x), Hazel FEC60 24 kbps (0.858250x), David FEC60 24 kbps (0.818246x). These are maintained elapsed-time medians, not process-CPU measurements. All raw repetitions, orders and adverse tails are retained. Against the prior complete full (library `9bf0bc5d2867`, Source `85cd34120e30`), 360 current and 0 OFF numeric quality fields changed. The quality account retains every prior-relative decline and introduced/worsened below-OFF field; no metric-majority claim is made. The growth comparison uses the same 60-second stereo AUDIO workload and full-precision raw medians.

 Runtime factory getter evidence covers the quality image, all 17 headline timing paths, and the current-only optional/denoiser timing paths for their exposed properties. The current benchmark-v2 source records bitrate, complexity, VBR/CVBR, FEC/loss, DTX and effective current denoiser before frame1; private official-side guards cover paths that execute an official encoder. Application request4001 and signal getters remain explicitly UNKNOWN because the current public API does not expose them; no value is inferred.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 675 |
| Publication | 806 | 12 | 710 |

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
| celt_quality | Higher | 122 | 135 |
| celt_masked_error | Lower | 122 | 135 |
| celt_highband_error | Lower | 185 | 199 |
| stereo_width_error | Lower | 26 | 17 |

[Matrix](production_quality_matrix.csv) Â· [Publication](production_quality_published.csv) Â· [Headline controls](quality_official_full_precision.csv) Â· [Denoising](voice_denoise_broad.csv) Â· [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 0.97x to 1.10x | 1.22x to 2.01x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) Â· [Encode](encode_speed_vs_official.csv) Â· [Decode](decode_speed_vs_official.csv) Â· [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) Â· [Postfilter quality](postfilter_quality_voip.csv) Â· [Denoiser timing](voice_denoise_timing.csv) Â· [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20464 | 20656 | Not exposed |
| official_encoder_ch1 | 31824 | 31872 | 31668 |
| current_decoder_ch1 | 8464 | 8304 | Not exposed |
| official_decoder_ch1 | 18448 | 18544 | 18468 |
| current_encoder_ch2 | 30656 | 30560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17584 | 16224 | Not exposed |
| official_decoder_ch2 | 27120 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 530940 | 0 | 0 | 530940 |
| android | 496060 | 464 | 0 | 496524 |

[Object-size data](binary_size.csv).

## FEC and DTX

| FEC metric | Value |
|---|---:|
| Strict source comparison | FAIL |
| Standard interoperability comparison | FAIL |
| Scored recovery wins | 18/18 |
| Maximum recovery-error ratio | 0.987481 |
| Recovery-error ratio | 0.566567 |
| Packet-byte ratio | 1.00197 |
| Backup coverage: opuscpp | 17/18 |
| Backup coverage: official | 15/18 |
| Configurations | 36 |
| Direction rows | 72 |
| C1 | 18/18 |
| C2 | 18/18 |
| C3 | 18/18 |
| C4 | 17/18 |

[FEC direction fields](fec_interop_metrics.csv) Â· [Source comparisons](fec_source_metrics.json) Â· [Ratios and raw output](fec_run_metadata.json).

| DTX field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.309710 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 0.603404 |
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
mono-mid bytes=76 encode_ms=19.4023 decode_ms=17.5885 checksum=340886119
mono-high bytes=115 encode_ms=21.9385 decode_ms=16.9941 checksum=2633358364
stereo-mid bytes=103 encode_ms=64.3835 decode_ms=37.8927 checksum=4179984942
stereo-high bytes=147 encode_ms=59.777 decode_ms=34.3313 checksum=3681257371
```

[Metric coverage](readme_metric_coverage.json) Â· [Artifact hashes](benchmark_metric_inventory.json).
