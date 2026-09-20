# Complete production metrics

Codex independently refreshed production `65a99c0c343232935dcd65c5194c7cde84ef6221` on 2026-09-20 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. 8 selected behavioral/regression harnesses passed on this source; broader compatibility results remain historical.

## Quality: every metric

The parity matrix contains 498 cases and 5,976 comparisons; 1,134 fields are below official. The separate 806-run publication suite includes headline complexity 9/10 controls, broader content, postfilter and denoiser comparisons. Overlapping cases in these suites are not independent observations.

| Metric | Better direction | Below official / 498 parity cases | Below official / 806 publication cases |
|---|---|---:|---:|
| snr_db | Higher | 44 | 42 |
| segmental_snr_db | Higher | 42 | 42 |
| rms_error | Lower | 44 | 42 |
| mean_abs_error | Lower | 38 | 42 |
| pesq_style | Higher | 40 | 41 |
| visqol_style | Higher | 110 | 113 |
| logband_corr | Higher | 103 | 150 |
| logband_error | Lower | 116 | 129 |
| celt_quality | Higher | 157 | 204 |
| celt_masked_error | Lower | 157 | 204 |
| celt_highband_error | Lower | 240 | 289 |
| stereo_width_error | Lower | 43 | 29 |

Full current/official values and both raw and direction-adjusted deltas: [498-case matrix](production_quality_matrix.csv), [806-run suite](production_quality_published.csv), [headline controls](quality_official_full_precision.csv). These tables also retain packet counts, average packet bytes and effective payload rates. All 12 denoiser on/off fields are in [broader denoising](voice_denoise_broad.csv) and [boundary rates](voice_denoise_boundary.csv). Proxy scores are not certified PESQ/ViSQOL or a listening test.

## Speed, payload rate and processing costs

Both codecs alternate within each run; nine repetitions, one logical CPU, above-normal priority. Decode timings use identical official packets.

AUDIO encode ratios: 1.27x to 1.77x; decode: 1.19x to 1.82x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

| Speech input | Mode | Frame ms | Rate bps | Current encode ms | Official encode ms | Encode ratio | Current decode ms | Official decode ms | Decode ratio |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hazel | voice | 20.0 | 16000 | 126.435100 | 85.610500 | 0.677110 | 11.469400 | 13.539200 | 1.180463 |
| hazel | voice | 20.0 | 32000 | 120.071800 | 87.689300 | 0.730307 | 12.549800 | 14.579500 | 1.161732 |
| hazel | voice | 20.0 | 64000 | 30.988800 | 78.986400 | 2.548869 | 13.321300 | 15.748400 | 1.182197 |
| hazel | fec60 | 60.0 | 24000 | 156.290600 | 96.136600 | 0.615114 | 7.923300 | 9.378300 | 1.183636 |
| david | voice | 20.0 | 16000 | 108.432000 | 74.457000 | 0.686670 | 10.084500 | 11.875600 | 1.177609 |
| david | voice | 20.0 | 32000 | 112.432600 | 76.215300 | 0.677875 | 10.972500 | 12.717000 | 1.158988 |
| david | voice | 20.0 | 64000 | 27.519900 | 66.010500 | 2.398646 | 11.483600 | 13.443100 | 1.170635 |
| david | fec60 | 60.0 | 24000 | 135.010100 | 82.890400 | 0.613957 | 7.032900 | 8.045500 | 1.143980 |

[Full speech durations, frame counts and payload rates](voice_speed_vs_official.csv). `fec60` uses 60 ms packets with FEC; `voice` uses 20 ms packets without FEC. Values below 1 are slower and remain in the table.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 16832 | 16864 | Not exposed |
| official_encoder_ch1 | 31888 | 31904 | 31668 |
| current_decoder_ch1 | 14128 | 13104 | Not exposed |
| official_decoder_ch1 | 18352 | 18576 | 18468 |
| current_encoder_ch2 | 32576 | 32496 | Not exposed |
| official_encoder_ch2 | 48880 | 48720 | 48684 |
| current_decoder_ch2 | 21264 | 21312 | Not exposed |
| official_decoder_ch2 | 27392 | 27280 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 324888 | 0 | 0 | 324888 |
| android | 327684 | 472 | 0 | 328156 |

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
mono-mid bytes=76 encode_ms=21.3811 decode_ms=21.9111 checksum=340886119
mono-high bytes=115 encode_ms=23.6668 decode_ms=18.7651 checksum=2633358364
stereo-mid bytes=102 encode_ms=92.191 decode_ms=29.4364 checksum=3294895434
stereo-high bytes=147 encode_ms=97.9409 decode_ms=31.6701 checksum=1997488960
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
- `voice_conditioning_release`: passed.
