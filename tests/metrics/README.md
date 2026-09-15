# Complete production metrics

Codex independently refreshed production `d7248b421de6956aad1b83fddd098898f47e6673` on 2026-09-15 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. 8 selected behavioral/regression harnesses passed on this source; broader compatibility results remain historical.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 1,154 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 44 | 42 |
| segmental_snr_db | Higher | 42 | 42 |
| rms_error | Lower | 44 | 42 |
| mean_abs_error | Lower | 38 | 42 |
| pesq_style | Higher | 40 | 41 |
| visqol_style | Higher | 116 | 113 |
| logband_corr | Higher | 109 | 150 |
| logband_error | Lower | 120 | 129 |
| celt_quality | Higher | 159 | 204 |
| celt_masked_error | Lower | 159 | 204 |
| celt_highband_error | Lower | 240 | 289 |
| stereo_width_error | Lower | 43 | 29 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.27x to 1.79x; decode: 1.19x to 1.77x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

| Speech input | Mode | Frame ms | Rate bps | Current encode ms | Official encode ms | Encode ratio | Current decode ms | Official decode ms | Decode ratio |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hazel | voice | 20.0 | 16000 | 123.794700 | 85.581900 | 0.691321 | 11.437500 | 13.605600 | 1.189561 |
| hazel | voice | 20.0 | 32000 | 118.890600 | 87.537200 | 0.736284 | 12.491600 | 14.595900 | 1.168457 |
| hazel | voice | 20.0 | 64000 | 31.223000 | 78.885400 | 2.526516 | 13.229500 | 15.665600 | 1.184142 |
| hazel | fec60 | 60.0 | 24000 | 156.809600 | 95.052300 | 0.606164 | 7.958000 | 9.260300 | 1.163647 |
| david | voice | 20.0 | 16000 | 109.230800 | 74.260200 | 0.679847 | 10.111600 | 11.937400 | 1.180565 |
| david | voice | 20.0 | 32000 | 112.199600 | 76.363600 | 0.680605 | 10.971700 | 12.726300 | 1.159921 |
| david | voice | 20.0 | 64000 | 27.894000 | 66.016000 | 2.366674 | 11.458600 | 13.420000 | 1.171173 |
| david | fec60 | 60.0 | 24000 | 135.297500 | 82.600600 | 0.610511 | 7.013700 | 8.054900 | 1.148452 |

[Full speech durations, frame counts and payload rates](voice_speed_vs_official.csv). `fec60` uses 60 ms packets with FEC; `voice` uses 20 ms packets without FEC. Values below 1 are slower and remain in the table.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 16832 | 16768 | Not exposed |
| official_encoder_ch1 | 31872 | 31696 | 31668 |
| current_decoder_ch1 | 14192 | 13088 | Not exposed |
| official_decoder_ch1 | 18304 | 18496 | 18468 |
| current_encoder_ch2 | 32576 | 32512 | Not exposed |
| official_encoder_ch2 | 49072 | 48752 | 48684 |
| current_decoder_ch2 | 21232 | 21328 | Not exposed |
| official_decoder_ch2 | 27392 | 27280 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 324760 | 0 | 0 | 324760 |
| android | 327588 | 472 | 0 | 328060 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC: 18/18 strict scored ratios; maximum 0.953507. C1 recovered-source fidelity, C2 following-frame fidelity, C3 source transition error and C4 recovered fidelity versus own PLC each pass 18/18. The expanded 36-configuration aggregate recovery ratio is 0.416949, packet-byte ratio 0.997419; backup coverage is 18/18 versus official 15/18. FEC is retained as regression coverage, with no further optimization in this round.

[Every FEC direction field](fec_interop_metrics.csv) includes decoder/continuation errors, recovered-to-PLC difference, recovery/PLC error, packet bytes and hashes. [Every source comparison](fec_source_metrics.json) includes fidelity_fec, next_fec, transition_fec, fidelity_ref, next_ref, transition_ref, fidelity_plc, self_consistency, step_fec and step_ref, for both codecs. [Strict ratios and complete output](fec_run_metadata.json).

DTX aggregate fields (individual material losses remain in the CSV):

| Field | Value |
|---|---:|
| dtx_comparison | PASS |
| false_positive_opuscpp | 0 |
| false_positive_official | 0 |
| reentry_nrmse_opuscpp | 0.376146 |
| reentry_nrmse_official | 0.762239 |
| reentry_gain_db_opuscpp | 1.549985 |
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
mono-mid bytes=76 encode_ms=21.4865 decode_ms=21.1869 checksum=340886119
mono-high bytes=115 encode_ms=24.9614 decode_ms=20.9401 checksum=2633358364
stereo-mid bytes=102 encode_ms=90.0466 decode_ms=29.3295 checksum=3294895434
stereo-high bytes=147 encode_ms=97.6792 decode_ms=31.0477 checksum=1997488960
```

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Broader compatibility/conformance and latency/lookahead records retain their historical provenance. Current selected checks are listed below. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.

## Current selected checks

[Source-bound results and commands](selected_regressions.json) retain every output.

- `voip_quiet_start_latch`: passed.
- `vbr_budget_behavior`: passed.
- `packet_duration_behavior`: passed.
- `decoder_channel_remap`: passed.
- `dtx_behavior`: passed.
- `voice_denoise_state`: passed.
- `hybrid_transient_budget`: passed.
- `nsq_quant_levels`: passed.
