# Complete production metrics

Measured: `9bf0bc5d28678924b702844b01b5e9697529d92c`; pinned official Opus: `51bd2e649f4516d103ab9a7f22a768595621e847`.

## Current objective

| Criterion | Fresh result | Status |
|---|---|---|
| Every individual encoding median strictly faster than OFF | 14/17 | FAIL |
| Smaller 16-to-256 kbps absolute encoding-time increase | 263.8342 ms vs OFF 276.5921 ms | PASS |
| Every published quality delta nonnegative | 703 negative fields | FAIL |
| Each normal image at most 350,000 B, including text+data+bss | Host 522,844 B; Android 489,856 B | FAIL |

The combined goal is unmet. Slower encoding medians: voice_david 64 kbps (0.977006x), Hazel FEC60 24 kbps (0.802396x), David FEC60 24 kbps (0.783743x). These are maintained elapsed-time medians, not process-CPU measurements. All raw repetitions, orders and adverse tails are retained. Against the prior complete full (Source `609707f89544`), 168 current and 0 OFF numeric quality fields changed. The quality account retains every prior-relative decline and introduced/worsened below-OFF field; no metric-majority claim is made. The growth comparison uses the same 60-second stereo AUDIO workload and full-precision raw medians.

 Runtime factory getter evidence covers the quality image, all 17 headline timing paths, and the current-only optional/denoiser timing paths for their exposed properties. The current benchmark-v2 source records bitrate, complexity, VBR/CVBR, FEC/loss, DTX and effective current denoiser before frame1; private official-side guards cover paths that execute an official encoder. Application request4001 and signal getters remain explicitly UNKNOWN because the current public API does not expose them; no value is inferred.

## Quality

| Suite | Cases | Fields | Below official |
|---|---:|---:|---:|
| Matrix | 498 | 12 | 672 |
| Publication | 806 | 12 | 703 |

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
| celt_highband_error | Lower | 182 | 192 |
| stereo_width_error | Lower | 26 | 17 |

[Matrix](production_quality_matrix.csv) · [Publication](production_quality_published.csv) · [Headline controls](quality_official_full_precision.csv) · [Denoising](voice_denoise_broad.csv) · [Boundary rates](voice_denoise_boundary.csv).

## Speed and processing costs

| Workload | Encode ratios | Decode ratios |
|---|---:|---:|
| AUDIO | 1.02x to 1.09x | 1.20x to 1.95x |

[Durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv) · [Encode](encode_speed_vs_official.csv) · [Decode](decode_speed_vs_official.csv) · [Speech and FEC60 data](voice_speed_vs_official.csv).

[PCM16 processing](postfilter_pcm16_path.csv) · [Postfilter quality](postfilter_quality_voip.csv) · [Denoiser timing](voice_denoise_timing.csv) · [Raw timing samples](run_metadata.json).

## Memory and object size

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 20464 | 20656 | Not exposed |
| official_encoder_ch1 | 31824 | 31888 | 31668 |
| current_decoder_ch1 | 8448 | 8336 | Not exposed |
| official_decoder_ch1 | 18480 | 18560 | 18468 |
| current_encoder_ch2 | 30656 | 30560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 17568 | 16224 | Not exposed |
| official_decoder_ch2 | 27104 | 27264 | 27236 |

[Memory data](memory_vs_official.csv).

| State | Bytes |
|---|---:|
| Optional denoiser | 76 |

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 522844 | 0 | 0 | 522844 |
| android | 489392 | 464 | 0 | 489856 |

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
mono-mid bytes=76 encode_ms=18.2118 decode_ms=15.628 checksum=340886119
mono-high bytes=115 encode_ms=19.99 decode_ms=14.6549 checksum=2633358364
stereo-mid bytes=103 encode_ms=55.6412 decode_ms=29.309 checksum=4179984942
stereo-high bytes=147 encode_ms=50.4589 decode_ms=29.9643 checksum=3681257371
```

[Metric coverage](readme_metric_coverage.json) · [Artifact hashes](benchmark_metric_inventory.json).
