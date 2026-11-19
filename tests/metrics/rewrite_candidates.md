# Whole-section rewrite candidates — audit of `96f7485` vs scalar official (Step 1)

Base `96f7485f06459e4e0270ed15f3573614807b45ae`, Source SHA-256 `6003cde4a8a1b625d0504d838dcbf614a062396ecaf83ff5ac3c19fe6d6ef0a7`.
Stripped whole objects: **Windows 509,923 B, Android 524,200 B**. Cap 399,999 B: need **−109,924 B Windows, −124,201 B Android**.
Official reference: the single-stream relocatable closure (128 members) from `work/official_single_stream_size_20261118/{host,android}.official_single_stream.map`; executable text 293,952 B Windows / 279,260 B Android. Our executable functions total 430,928 B Windows / 409,968 B Android, so the gap is executable code (non-executable bytes are already smaller than official).

Method: per-function sizes from the unstripped normal objects (`funcs.py`: COFF symbol address deltas, ELF `nm -S`), mapped into mutually exclusive sections by name (`sections.py`), against official members mapped by source file. Inlining differs per target (Windows inlines the SILK decoder into `opus_decode_frame`; clang inlines SILK analysis/NLSF helpers into `silk_encode_frame_FLP` and the analysis into `encode_native`), so the rewrite boundaries below are drawn to absorb those differences. Raw data: `functions.json`, `sections.json`.

## Section excess (ours − official, executable bytes)

| Section | Windows ours / official | Windows excess | Android ours / official | Android excess |
|---|---:|---:|---:|---:|
| SILK NSQ | 44,336 / 12,128 | **+32,208** | 39,308 / 11,928 | **+27,380** |
| CELT encoder frame (encode_with_ec, prefilter, tone, transient, dynalloc, tf, vbr, energy-stage) | 66,836 / 35,328 | **+31,508** | 61,204 / 34,288 | **+26,916** |
| CELT/shared DSP (FFT, MDCT, pitch search, LPC, range coder) | 46,166 / 23,136 | +23,030 | 43,280 / 23,388 | +19,892 |
| SILK encoder frame analysis/control | 56,736 / 38,416 | +18,320 | 64,764 / 34,220 | **+30,544** |
| CELT bands/PVQ/cwrs | 41,200 / 27,072 | +14,128 | 31,808 / 23,124 | +8,684 |
| Opus encoder top/routing/packet (incl. denoiser) | 54,128 / 41,088 | +13,040 | 56,432 / 36,340 | +20,092 |
| Opus analysis/tonality/classifier | 21,616 / 15,808 | +5,808 | 4,520 / 15,840 | −11,320 (inlined into encode_native) |
| SILK encoder API/control/entropy/NLSF/stereo/VAD | 28,790 / 29,872 | −1,082 | 42,576 / 30,940 | +11,636 |
| SILK shared DSP/tables | 14,323 / 9,344 | +4,979 | 16,644 / 9,160 | +7,484 |
| Decoders (Opus + SILK + CELT, incl. PLC) | 36,083 / 44,800 | −8,717 | 36,192 / 45,420 | −9,228 |
| Energy/allocation, resampler, unclassified | — | +3,754 | — | −1,372 |

## Ordered rewrite list (whole sections; each replaces the complete old implementation)

| # | Section boundary (delete + rewrite from scratch) | Target size (W / A) | Expected saving (W / A) | Quality/speed risk |
|---:|---|---:|---:|---|
| 1 | **SILK NSQ subsystem**: all `silk_NSQ<...>` instantiations (6 bodies, 13.7K+7.3K+6.8K+4.5K+3.1K+2.5K W), `silk_nsq_*` helpers, scale/restore/copy-history, noise-shape feedback variants, `silk_NSQ_prepare_FLP`, `silk_NSQ_wrapper_FLP<>`. Rewrite as one plain NSQ kernel and one delayed-decision kernel with runtime parameters (LBRR/primary/replay, fixed pulses, deferred history), official-style. | ~15K / ~14K | **~−29K / −25K** | Output changes; SILK hot path (voice margins 30–40%); FEC/LBRR/replay contracts. |
| 2 | **CELT encoder frame**: `celt_encode_with_ec` and its frame-only analysis: prefilter (`run_prefilter_active_for_channels`, `celt_encode_prefilter`, fit terms, passthrough), transient, tone detect, dynalloc, tf_analysis, alloc-trim, VBR target, energy-stage retry/restore, fine-phase cost pair, low-rate stereo trim balance, haar metric, preemphasis, compute_mdcts. | ~40K / ~38K | **~−27K / −23K** | Output changes; many tuned quality decisions live here (energy-stage retry, trim preview, fine-phase RDO, hybrid band-17 compensation, transient threshold). |
| 3 | **SILK encoder frame + control**: `silk_encode_frame_FLP`, `silk_find_pred_coefs_FLP`, pitch analysis core, Burg, LPC analysis filter, find_LPC/LTP/pitch_lags, inner products, LBRR generation/cost, and on the same boundary `silk_Encode` control, NLSF processing/encode, VAD, stereo LR→MS/predictor, gain/index/pulse encoding. Keep NSQ (item 1) and shared decoder primitives outside. | ~70K / ~72K | **~−15K / −35K** | Output changes; FEC/LBRR, 60 ms, stereo, CVBR contract test. |
| 4 | **DSP: FFT/MDCT/pitch search**: Stockham SoA + tensor480 stages + kiss butterflies + PFA + `pitch_fft512` → one FFT; five `clt_mdct_*` variants → one forward + one backward; `pitch_search`/`remove_doubling_impl`. | ~26K / ~25K | **~−20K / −18K** | Float rounding changes (decoder MDCT/FFT too → conformance must hold); CELT speed-critical. |
| 5 | **Opus encoder top + analysis**: `encode_native`, `opus_encode_frame_native`, multi/single-frame packet paths, `write_packet_frames`, mode/bandwidth/FEC decisions, rate servo, DTX, `classical_leak_*` analysis and classifier, voice denoiser. | ~60K / ~52K | **~−16K / −9K** | Output changes; mode decisions drive most metrics. |
| 6 | **CELT bands/PVQ**: `quant_all_bands`, `quant_partition`, `quant_band(_stereo)`, `compute_theta`, `alg_quant`, rotations. | ~30K / ~25K | **~−11K / −7K** | Output changes at high rates (theta RDO). |
| | **Total if all six land at target** | | **~−118K / −117K** | Windows ≈392K (passes); Android ≈407K (still ~7K over → Android needs items 1–6 to beat targets, or SILK shared DSP/tables rewrite as item 7, +7.5K excess). |

Notes:
- Order follows the biggest measured excess on either target (handover work rule), with item 1 first because its excess is pure duplication (six template bodies of one algorithm) and its boundary is the cleanest.
- Savings are targets, not forecasts; each rewrite is credited only on measured whole stripped files.
- Each rewrite lands only through all gates (nominal + matched quality, FEC, conformance, tests, speed). A rewrite that loses quality pairs is reworked, not landed.
