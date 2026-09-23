# Complete production metrics

Codex independently refreshed production `5d531c6430d82e1dcf910fe9f880b8047274a709` on 2026-09-22 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. No extra compatibility check results are claimed here.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 1,129 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 43 | 42 |
| segmental_snr_db | Higher | 42 | 42 |
| rms_error | Lower | 43 | 42 |
| mean_abs_error | Lower | 37 | 42 |
| pesq_style | Higher | 40 | 41 |
| visqol_style | Higher | 109 | 113 |
| logband_corr | Higher | 103 | 150 |
| logband_error | Lower | 115 | 129 |
| celt_quality | Higher | 157 | 204 |
| celt_masked_error | Lower | 157 | 204 |
| celt_highband_error | Lower | 240 | 289 |
| stereo_width_error | Lower | 43 | 29 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.26x to 1.79x; decode: 1.19x to 1.81x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

| Speech input | Mode | Frame ms | Rate bps | Current encode ms | Official encode ms | Encode ratio | Current decode ms | Official decode ms | Decode ratio |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hazel | voice | 20.0 | 16000 | 180.041700 | 123.767200 | 0.687436 | 16.629800 | 20.013900 | 1.203496 |
| hazel | voice | 20.0 | 32000 | 160.587300 | 130.632100 | 0.813465 | 18.397300 | 21.471900 | 1.167122 |
| hazel | voice | 20.0 | 64000 | 46.426300 | 117.492500 | 2.530732 | 19.545700 | 22.884600 | 1.170825 |
| hazel | fec60 | 60.0 | 24000 | 151.222800 | 99.764000 | 0.659715 | 8.268000 | 10.116400 | 1.223561 |
| david | voice | 20.0 | 16000 | 159.787400 | 116.596300 | 0.729696 | 15.225200 | 18.054100 | 1.185804 |
| david | voice | 20.0 | 32000 | 129.856400 | 109.815000 | 0.845665 | 13.877000 | 18.850700 | 1.358413 |
| david | voice | 20.0 | 64000 | 41.560400 | 98.937400 | 2.380569 | 16.974800 | 19.783800 | 1.165481 |
| david | fec60 | 60.0 | 24000 | 127.115900 | 84.185300 | 0.662272 | 7.103700 | 8.123400 | 1.143545 |

[Full speech durations, frame counts and payload rates](voice_speed_vs_official.csv). `fec60` uses 60 ms packets with FEC; `voice` uses 20 ms packets without FEC. Values below 1 are slower and remain in the table.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 16960 | 16912 | Not exposed |
| official_encoder_ch1 | 31840 | 31888 | 31668 |
| current_decoder_ch1 | 14128 | 13056 | Not exposed |
| official_decoder_ch1 | 18320 | 18560 | 18468 |
| current_encoder_ch2 | 32704 | 32560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 21312 | 21312 | Not exposed |
| official_decoder_ch2 | 27280 | 27248 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 327780 | 0 | 0 | 327780 |
| android | 329380 | 472 | 0 | 329852 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC-only snapshot `32f44fc`: effective FEC=1. The 18 scored 10/20 ms cases have aggregate recovery-error ratio 0.482074 (51.8% lower) and packet-byte ratio 0.996061 (0.4% fewer bytes). Recovery is better in 17/18 cases; the profile-0 stereo 20 ms, 48 kbps VBR case has ratio 1.04615, so the scored acceptance check remains failed. Backup coverage is 18/18 versus official 15/18. Source-fidelity criteria C1-C4 each pass 18/18. The full interoperability run covers 36 configurations and 72 direction rows; only the 18 10/20 ms cases enter the scored aggregate. [Startup loss and regression checks](fec_startup_checkpoint.json). Quality, speed, memory and object-size tables retain their original full-run provenance; they were not refreshed for this FEC-only change.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV):

| Field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.375470 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.551069 |
| reentry_gain_db_official | 1.646307 |
| silence_dtx_opuscpp | 406 |
| silence_dtx_official | 406 |
| steady_noise_dtx_opuscpp | 120 |
| steady_noise_dtx_official | 0 |

[All DTX false-positive and re-entry measurements](dtx_metrics.csv).

## Mode selection

```text
speech_like,silk_pct=0.0,hybrid_pct=4.3,celt_pct=95.7
harmonic_music,silk_pct=0.0,hybrid_pct=0.0,celt_pct=100.0
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=20.7975 decode_ms=20.54 checksum=340886119
mono-high bytes=115 encode_ms=22.2117 decode_ms=18.1456 checksum=2633358364
stereo-mid bytes=102 encode_ms=86.4728 decode_ms=28.4399 checksum=3294895434
stereo-high bytes=147 encode_ms=92.9415 decode_ms=29.9699 checksum=1997488960
```

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Except for the FEC startup checkpoint above, broader compatibility/conformance, startup and latency/lookahead records retain their original source commits and validation dates; this refresh does not claim a new complete compatibility campaign. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.
