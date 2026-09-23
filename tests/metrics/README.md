# Complete production metrics

Codex independently refreshed production `f944dc7565ee5925ffb95ebd3fcb208171efb1cd` on 2026-09-23 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. No extra compatibility check results are claimed here.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 1,038 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 38 | 42 |
| segmental_snr_db | Higher | 39 | 42 |
| rms_error | Lower | 38 | 42 |
| mean_abs_error | Lower | 33 | 42 |
| pesq_style | Higher | 37 | 41 |
| visqol_style | Higher | 93 | 99 |
| logband_corr | Higher | 91 | 134 |
| logband_error | Lower | 94 | 95 |
| celt_quality | Higher | 143 | 177 |
| celt_masked_error | Lower | 143 | 177 |
| celt_highband_error | Lower | 246 | 309 |
| stereo_width_error | Lower | 43 | 29 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.24x to 1.78x; decode: 1.19x to 1.85x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

All real-speech AUDIO/VOIP timing rows are retained in [voice_speed_vs_official.csv](voice_speed_vs_official.csv) and raw run metadata; no Hazel/David VOIP timing columns are added to the summary tables.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 16960 | 16896 | Not exposed |
| official_encoder_ch1 | 31840 | 31904 | 31668 |
| current_decoder_ch1 | 14128 | 13056 | Not exposed |
| official_decoder_ch1 | 18336 | 18560 | 18468 |
| current_encoder_ch2 | 32704 | 32560 | Not exposed |
| official_encoder_ch2 | 48880 | 48736 | 48684 |
| current_decoder_ch2 | 21296 | 21312 | Not exposed |
| official_decoder_ch2 | 27296 | 27248 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 327588 | 0 | 0 | 327588 |
| android | 329340 | 472 | 0 | 329812 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC effective setting is 1. The standard 18-case recovery comparison records 18/18 wins and aggregate recovery ratio 0.509621, but packet-byte ratio 1.00102 fails the <=1 gate. Source criteria C1/C2/C3/C4 are 18/18, 18/18, 17/18, 18/18. C3 fails for mono 10 ms, 24 kb/s, profile-1 VBR: transition_fec 0.420358036 vs official 0.271244925. Aggregate wins are not a full FEC pass. The full interoperability run covers 36 configurations and emits 72 direction rows; 40/60 ms cases are excluded from the scored aggregate.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV):

| Field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.375632 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.531290 |
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
mono-mid bytes=76 encode_ms=24.5792 decode_ms=26.2237 checksum=340886119
mono-high bytes=115 encode_ms=28.6323 decode_ms=22.7278 checksum=2633358364
stereo-mid bytes=102 encode_ms=105.246 decode_ms=35.1229 checksum=3294895434
stereo-high bytes=147 encode_ms=112.824 decode_ms=32.164 checksum=1997488960
```

## Unavailable and historical measurements

No sanitizer run was made. Broader compatibility/conformance, startup and latency/lookahead records retain their original source commits and validation dates; this refresh does not claim a new complete compatibility campaign. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.
