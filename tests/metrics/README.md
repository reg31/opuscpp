# Complete production metrics

Codex independently refreshed production `c019647331f67b772fbad9ea1e597a12ac4a89e9` on 2026-10-02 against official Opus `503d81b138d76621aae4b12786e90de48aa8db3a`. Both builds use `-O2 -DNDEBUG`; official enables x86 intrinsics. Every measured field is retained below or in the linked full tables. Named per-commit comparisons in the parent README remain historical. The current source-bound compatibility checks completed; their test-only recovery provenance is recorded below.

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

AUDIO encode ratios: 1.07x to 1.28x; decode: 1.18x to 1.81x. [All durations, ratios and payload rates](speed_vs_official_intrinsics_60s.csv), [encode](encode_speed_vs_official.csv), [decode](decode_speed_vs_official.csv). Real-time factor is audio duration divided by processing time.

[Optional PCM16 processing](postfilter_pcm16_path.csv) retains every requested/applied level, duration and overhead; [postfilter quality](postfilter_quality_voip.csv). [Denoiser timing](voice_denoise_timing.csv) includes all 11 rates, bypass/enabled durations and overhead, with nine measured runs after warm-up. Raw timing samples are in [run metadata](run_metadata.json).

## Memory and object size

Median of three fresh processes, 256 instances each. FEC and denoising disabled for state allocation measurements. Private/working-set page deltas are not exact object sizes or peak stack usage.

| State | Private bytes/instance | Working-set bytes/instance | Official API bytes |
|---|---:|---:|---:|
| current_encoder_ch1 | 22592 | 22592 | Not exposed |
| official_encoder_ch1 | 31824 | 31920 | 31668 |
| current_decoder_ch1 | 14256 | 13120 | Not exposed |
| official_decoder_ch1 | 18320 | 18528 | 18468 |
| current_encoder_ch2 | 33616 | 33504 | Not exposed |
| official_encoder_ch2 | 48880 | 48704 | 48684 |
| current_decoder_ch2 | 21168 | 21248 | Not exposed |
| official_decoder_ch2 | 27280 | 27248 | 27236 |

[Memory CSV](memory_vs_official.csv). Denoiser optional state: 68 bytes; temporary stack cache: 7,680 bytes. Compiler-reported NSQ stack reservations in earlier checkpoint sections are historical function reservations, not newly measured peak memory.

| Object | Text bytes | Data bytes | BSS bytes | Total bytes |
|---|---:|---:|---:|---:|
| host | 359728 | 0 | 0 | 359728 |
| android | 356404 | 472 | 0 | 356876 |

[Binary-size CSV](binary_size.csv).

## FEC and DTX

FEC: 18/18 strict scored ratios; maximum 0.977027. C1 recovered-source fidelity, C2 following-frame fidelity, C3 source transition error and C4 recovered fidelity versus own PLC each pass 18/18. The scored recovery aggregate contains 18 10/20 ms cases: recovery ratio 0.504119, packet-byte ratio 0.993345, and backup coverage 18/18 versus official 15/18. The separate interoperability run covers 36 configurations and emits 72 direction rows; 40/60 ms cases remain in the raw report but are excluded from the scored aggregate. See [FEC acceptance](acceptance.json).

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

[All DTX false-positive and re-entry measurements](dtx_metrics.csv); the unchanged aggregate gate is FAIL ([acceptance record](dtx_acceptance.json)).

## Fresh compatibility and test-only recovery

The c019647 full refresh completed all seven phases. Saved current-source results include 24/24 RFC 8251 decode vectors, 96 encode-interoperability checks, API behavior checks, and four selected regressions. The final two selected checks ran after test-only maintenance commit `767d7c9`; codec source SHA-256 remains `f8dcafb38d21f2ec90ec20143f48eda79b64e25340c7fa407b9ad0bb830ddf3f`. No quality, memory, timing, speech-timing, or other metric phase was rerun for the test repair. Full outputs are in [compatibility validation](compatibility_validation.json), with the explicit repair binding in [recovery metadata](compatibility_recovery_767d7c9.json). The original stale-test compile failure is retained in the run logs and marked as superseded by the test-only repair.

Supplemental source-bound checkpoints remain separate from the full-refresh measurements: [retained storage](encoder_retained_storage_checkpoint.json), [CELT native-budget evidence](celt_native_budget_checkpoint.json), and [multiframe packet-budget regression](multiframe_budget_checkpoint.json). Their embedded source revisions and scopes remain attached; they do not replace the c019647 metric tables.

Current selected checks, with raw outputs in [compatibility validation](compatibility_validation.json):

- `voice_conditioning_release`: PASS (133 checks).
- `voice_denoise_state`: PASS (90 cases).
- `transient_invariant`: PASS (4 checks).
- `voip_quiet_start_latch`: PASS (26 checks).

## Mode selection

```text
speech_like,silk_pct=0.0,hybrid_pct=88.0,celt_pct=12.0
harmonic_music,silk_pct=0.0,hybrid_pct=5.7,celt_pct=94.3
```

Measured at 32 kbps with AUDIO application; all SILK, hybrid and CELT percentages are retained in [mode metadata](mode_balance_metrics.json).

## CELT microbenchmark

```text
mono-mid bytes=76 encode_ms=21.8944 decode_ms=21.3281 checksum=340886119
mono-high bytes=115 encode_ms=22.819 decode_ms=19.2188 checksum=2633358364
stereo-mid bytes=102 encode_ms=102.443 decode_ms=31.1517 checksum=3294895434
stereo-high bytes=147 encode_ms=94.4811 decode_ms=30.8276 checksum=1997488960
```

## Unavailable and historical measurements

WER/CER: not measured; no configured ASR engine/transcript manifest. No result is inferred from the quality proxies. No sanitizer run was made. Named historical compatibility, startup and latency/lookahead checkpoints retain their original source commits and dates. The c019647 compatibility checks are current; the test-only repair and its unchanged codec source are bound above. Per-commit optimization timings, NSQ work counters, `quality_history_*.csv`, observer/reference-reuse experiments and `voice_denoise_vs_previous.csv` remain historical. Current measurements do not overwrite their provenance.
