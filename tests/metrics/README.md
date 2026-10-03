# Complete production metrics

Codex independently refreshed production `35c5039379dfe9d5404637e9c8e5c0ddc9b0a0d6` on 2026-10-03 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. Fresh checks on source `35c5039` include 24/24 RFC 8251 decode vectors and 96/96 encode-interoperability cases; API and selected regression results are in [compatibility validation](compatibility_validation.json).

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 805 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 14 | 5 |
| segmental_snr_db | Higher | 26 | 10 |
| rms_error | Lower | 14 | 5 |
| mean_abs_error | Lower | 15 | 7 |
| pesq_style | Higher | 29 | 10 |
| visqol_style | Higher | 66 | 94 |
| logband_corr | Higher | 65 | 119 |
| logband_error | Lower | 64 | 81 |
| celt_quality | Higher | 115 | 199 |
| celt_masked_error | Lower | 115 | 199 |
| celt_highband_error | Lower | 254 | 306 |
| stereo_width_error | Lower | 28 | 17 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.13x to 1.27x; decode: 1.17x to 1.72x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 19792 | 19808 | Not exposed |
| official_encoder_ch1 | 31872 | 31872 | 31668 |
| current_decoder_ch1 | 11840 | 10752 | Not exposed |
| official_decoder_ch1 | 18416 | 18560 | 18468 |
| current_encoder_ch2 | 30000 | 29904 | Not exposed |
| official_encoder_ch2 | 48912 | 48736 | 48684 |
| current_decoder_ch2 | 17552 | 17664 | Not exposed |
| official_decoder_ch2 | 27424 | 27280 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes from the fresh state/bounds test. Temporary denoiser stack usage was not measured in this refresh. Compiler-reported stack reservations in named historical checkpoints are not current peak-memory measurements.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 380272 | 0 | 0 | 380272 |
| android | 374148 | 472 | 0 | 374620 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC: 18/18 strict scored ratios; maximum 0.977027. C1 recovered-source fidelity, C2 following-frame fidelity, C3 source transition error and C4 recovered fidelity versus own PLC each pass 18/18. The expanded 36-configuration aggregate recovery ratio is 0.504119, packet-byte ratio 0.993345; backup coverage is 18/18 versus official 15/18. FEC is retained as regression coverage, with no further optimization in this round.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV):

| Field | Value |
|---|---:|
| dtx_comparison | FAIL |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.426114 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.852367 |
| reentry_gain_db_official | 1.646307 |
| silence_dtx_opuscpp | 406 |
| silence_dtx_official | 406 |
| steady_noise_dtx_opuscpp | 120 |
| steady_noise_dtx_official | 0 |

[All DTX false-positive and re-entry measurements](dtx_metrics.csv); unchanged aggregate gate: FAIL ([acceptance record](dtx_acceptance.json)).

## Fresh compatibility and API checks

This full refresh is bound to source `35c5039379dfe9d5404637e9c8e5c0ddc9b0a0d6` and official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. RFC 8251 decode: 24/24; encode interoperability: 96/96. API behavior and selected source-bound regressions are recorded in [compatibility validation](compatibility_validation.json). No sanitizer was run.

The LPC reconstruction diagnostic remains FAIL at a SILK-primary frame63 transition: the packet carries 18 bytes of trailing CELT redundancy and triggers one CELT N240 call, while the test compares the full decoded frame against NSQ-only samples. Pinned official Opus and A7 decode the frame byte-identically; the comparator differs across samples 286–319 (first -3925 vs -3926; 34 samples, maximum absolute error 640). The supported VOICE/VAD127 run retained all16 fixture configurations and assertions; independent bit-helper, seed-wrap, Schur, SILK LPC-order and CELT-IIR assertions passed, but the aggregate diagnostic exits1 at the transition comparator. See the [transition record](lpc_analysis_known_failure.json).

## Mode selection

```text
speech_like,silk_pct=0.0,hybrid_pct=88.0,celt_pct=12.0
harmonic_music,silk_pct=0.0,hybrid_pct=5.7,celt_pct=94.3
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=21.3301 decode_ms=20.7156 checksum=340886119
mono-high bytes=115 encode_ms=22.1044 decode_ms=18.227 checksum=2633358364
stereo-mid bytes=102 encode_ms=89.9631 decode_ms=29.7436 checksum=3294895434
stereo-high bytes=147 encode_ms=95.1128 decode_ms=31.1933 checksum=1997488960
```

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Broader compatibility/conformance, startup and latency/lookahead records retain their original source commits and validation dates; this refresh does not claim a new complete compatibility campaign. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.
