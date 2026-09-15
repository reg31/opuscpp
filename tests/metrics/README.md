# Complete production metrics

Codex independently refreshed production `fcf29787f607e706e1cad80f51080057199240d1` on 2026-09-15 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. No extra compatibility check results are claimed here.

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

AUDIO encode ratios: 1.19x to 1.80x; decode: 1.16x to 1.88x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

| Speech input | Mode | Frame ms | Rate bps | Current encode ms | Official encode ms | Encode ratio | Current decode ms | Official decode ms | Decode ratio |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| hazel | voice | 20.0 | 16000 | 210.350700 | 132.954000 | 0.632059 | 17.975500 | 20.845600 | 1.159667 |
| hazel | voice | 20.0 | 32000 | 202.576800 | 135.226200 | 0.667531 | 19.100100 | 22.284000 | 1.166695 |
| hazel | voice | 20.0 | 64000 | 49.155000 | 122.311900 | 2.488290 | 20.701300 | 23.874700 | 1.153295 |
| hazel | fec60 | 60.0 | 24000 | 188.000500 | 102.142600 | 0.543310 | 8.522800 | 9.847600 | 1.155442 |
| david | voice | 20.0 | 16000 | 185.711500 | 115.916600 | 0.624176 | 15.568000 | 18.005900 | 1.156597 |
| david | voice | 20.0 | 32000 | 166.397800 | 118.969800 | 0.714972 | 16.553400 | 19.480700 | 1.176840 |
| david | voice | 20.0 | 64000 | 42.893000 | 102.773400 | 2.396041 | 17.445000 | 20.665400 | 1.184603 |
| david | fec60 | 60.0 | 24000 | 162.733600 | 90.137100 | 0.553894 | 7.520400 | 8.843000 | 1.175868 |

[Full speech durations, frame counts and payload rates](voice_speed_vs_official.csv). `fec60` uses 60 ms packets with FEC; `voice` uses 20 ms packets without FEC. Values below 1 are slower and remain in the table.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 16800 | 16832 | Not exposed |
| official_encoder_ch1 | 31872 | 31856 | 31668 |
| current_decoder_ch1 | 14192 | 13120 | Not exposed |
| official_decoder_ch1 | 18304 | 18560 | 18468 |
| current_encoder_ch2 | 32576 | 32512 | Not exposed |
| official_encoder_ch2 | 49072 | 48752 | 48684 |
| current_decoder_ch2 | 21232 | 21328 | Not exposed |
| official_decoder_ch2 | 27392 | 27280 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 322176 | 0 | 0 | 322176 |
| android | 325768 | 472 | 0 | 326240 |

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
mono-mid bytes=76 encode_ms=22.9056 decode_ms=21.6342 checksum=340886119
mono-high bytes=115 encode_ms=23.4975 decode_ms=19.1914 checksum=2633358364
stereo-mid bytes=102 encode_ms=93.0645 decode_ms=29.9994 checksum=3294895434
stereo-high bytes=147 encode_ms=99.5511 decode_ms=31.8379 checksum=1997488960
```

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Compatibility, conformance, startup, latency/lookahead and toolchain checks retain their existing production validation records; additional checks were stopped as requested. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.
