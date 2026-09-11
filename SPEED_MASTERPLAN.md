# opuscpp Speed Masterplan

Status: draft for review. Scope: **encoder** speed (decoder already beats official 1.2–1.9×).
Constraints (non-negotiable): quality must not regress vs the shipped baseline; build stays
`-std=c++23 -O2 -DNDEBUG`; no explicit SIMD intrinsics (rely on auto-vectorization); must keep
compiling for Android arm64 Clang; RFC/decode/interop conformance stays green.

All line numbers refer to `src/opus_codec.cpp` on `0f3e774`.

---

## 1. Measured baseline

Published encode speedup vs official (official built `-O2` + intrinsics):

| Bitrate | 16k | 24k | 32k | 48k | 64k | 96k | 128k | 192k | 256k |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Encode | 1.273 | 1.074 | 1.047 | **0.986** | 1.006 | 1.063 | 1.176 | 1.074 | 0.980 |
| Decode | 1.894 | 1.470 | 1.437 | 1.361 | 1.351 | 1.317 | 1.340 | 1.292 | 1.230 |

New measurement (this audit), 48 kbps stereo AUDIO, `synthetic_music_like_stereo.wav`, 300 frames:

| Config | encode_ms | speedup vs official |
|---|---:|---:|
| opuscpp search **ON** (shipped) | ~22.15 | **0.985×** |
| opuscpp search **OFF** (temp `#if 0`) | 11.09 | **1.968×** |
| official | 21.83 | 1.000× |

**Headline: the reconstruction-based quality search accounts for ~50% of encoder wall time**
in every configuration where it is active (11.06 of 22.15 ms). This is the single dominant
speed opportunity.

Microbench (`tests/quant_all_bands_microbench.cpp`, 2000 iters, LM=3): `quant_all_bands`
encode ≈ 8 µs/frame, decode ≈ 8 µs/frame — so the PVQ search and PVQ dequant cost about the
same, and both are small relative to the full per-frame encode (~37 µs/frame with the search
disabled).

---

## 2. Hot-path map (encoder)

```
opus_encode_float (3871) / opus_encode (3845)
  -> encode_native (2813)
    -> opus_encode_frame_native (3232)
      -> celt_encode_with_ec (6436)            [per-frame CELT entry]
        -> celt_encode_with_history (6334)     [quality search wrapper]
          -> celt_encode_candidate (5925)      [the real work; called 1-3x/frame]
```

Stages inside `celt_encode_candidate` (line):

| Stage | Line |
|---|---|
| preemphasis input | 6040 (`celt_preemphasise_input` 5702) |
| tone / transient detect | 6041–6065 |
| pitch prefilter | 6067 (`celt_encode_prefilter` 5790) |
| forward MDCT | 6082 (`compute_mdcts` 5101 → `clt_mdct_forward_c` 8169) |
| band energy + normalise | 6083 (`compute_band_energies_and_normalise` 4123) |
| dynalloc | 6095 (`dynalloc_analysis` 5311) |
| coarse energy | 6173 (`quant_coarse_energy` 9006) |
| TF | 6175 (`process_tf_changes` 5136) |
| spread / stereo / trim / VBR | 6179–6285 |
| allocation | 6289 (`clt_compute_allocation` 9242) |
| fine energy | 6295 (`process_fine_energy` 9028) |
| **PVQ quant** | 6297 (`quant_all_bands` 4789 → `quant_band` 4605 → `quant_partition` 4504 → `alg_quant` 9503 → `op_pvq_search_c` 9412) |

**Entirely scalar.** No `<immintrin.h>`/`<arm_neon.h>`, no `__m128`, no `#pragma`, no
`restrict`, no explicit vectorization. 65 `inline` markers exist but on scalar helpers.

---

## 3. Candidate inventory (ranked by value/effort)

### WS-A — Kill the per-frame history shadow-decode  ★ highest value
**Where:** `quality_prepare_history` (6735) called every main packet at 6088 sets
`work.active`; `celt_encode_candidate` then calls `quality_probe_real_decoder` (6774) whenever
`active` and a checkpoint exists — i.e. a **full `celt_decode_with_ec` every frame** to keep
`quality_history` (leaky-band trajectories `reference_bands`/`decoded_bands`, 443–447).

**Why it's removable:** the accept decision at 6413–6418 is dominated by `score[0]` (raw
time-domain MSE, 6918). The history only feeds the secondary perceptual guards
`score[2..4]` (leaky band-energy ratios, 6947–6954) and `quality_advance_filters` (6863).
Per-frame decode is paid for the guards, not the decision.

**Plan:**
- A1. Accept on `score[0]` + `score[1]` only (drop `score[2..4]` guards); make the probe run
  *only* on eligible frames. Removes ~1 decode/frame for ~all frames.
- A2. If guards are needed, replace the leaky-band history with a cheap proxy computed from
  the encoder's own reconstruction instead of a decode (needs a synthesis-free approximation
  — partly explored, see §5).
- A3. Fall back: keep the history but only update it every N frames (reduces, not removes).

**Expected:** recover most of the ~50% at 48 kbps (target ≥ 1.5× at 48k, ≥ 1.9× at 16k).
**Risk:** quality regression where the guards vetoed a bad challenger. Gate on the full
quality suite.

### WS-B — Cheaper challenger
**Where:** challenger call in `celt_encode_candidate` (6147–6157), a second full encode, plus
a second probe decode, on every `quality_search_interval`-th frame (interval = 12, 57).
**Plan:** B1 keep interval 12 (already shipped); B2 share more of the ordinary's analysis
(`celt_analysis_checkpoint`, 5908) so the challenger skips recomputation; B3 bound the
challenger to the bands that differ.
**Expected:** ~0.1–0.2× encode. Secondary to WS-A.

### WS-C — Auto-vectorization of hot loops (no intrinsics)
**Where:** `op_pvq_search_c` inner argmax (9453–9475); `celt_inner_prod_c` (1565);
`xcorr_kernel_c` (1539); `compute_band_energies_and_normalise` (4132–4145); MDCT
`clt_mdct_forward_transform` (8128–8166) and FFT butterflies (7722–7842).
**Plan:** restructure loops to be branch-free and alias-clear so GCC/Clang at `-O2`
auto-vectorize (e.g. compute `(Rxy)²/(Ryy)` over a contiguous vector then reduce argmax;
`restrict`-style local-pointer patterns; hoist per-iteration loads). Guard against
`-Wconversion` regressions.
**Expected:** 10–25% on the base encode. Medium effort, cross-platform safe.

### WS-D — Micro-optimisations / wasted work
- D1. `compute_band_energies_and_normalise` (4138–4139): per-band `std::log(double)` →
  `log2f`/precomputed constant; normalise loop as one contiguous pass.
- D2. `quality_frame_work` (415) embeds a full `quality_celt_snapshot`
  (`quality_celt_decoder_storage_max` = 4552 floats ≈ 18 KB, 405) and is stack-constructed
  **every frame** in the non-eligible path (6349). Make it a reusable member of the encoder
  (or a `thread_local`), not a per-frame stack object.
- D3. `celt_state_checkpoint` (5901) / `celt_analysis_checkpoint` (5908) are ~10 KB each and
  copied via `save`/`restore` per eligible frame (6390–6401). Avoid the full-state memcpy by
  diffing only fields the challenger can change, or by using the existing analysis image.
- D4. SSE-absent `celt_inner_prod_c` double-counting in stereo analysis paths.

**Expected:** 3–10% combined. Low risk, independent.

### WS-E — Compiler/build (informational, likely out of scope)
`-O3 -ftree-vectorize` or LTO would help the scalar code, but the public comparison fixes
opuscpp at `-O2` (script 351), so this is not shippable under current rules unless the
comparison policy changes. Keep for local headroom analysis only.

---

## 4. Validation protocol (every workstream)

1. Correctness/conformance: RFC decode, encode interop (96/96), Android arm64 Clang build.
2. Quality: tracked `synthetic_music_like_*` + 16 signals in `diverse/`, at all bitrates;
   **no negative `celt_quality` delta** beyond baseline. VOIP 16k delta is already +1.81.
3. Speed: `tests/perceptual_memory_validation.cpp` `encode_speed_ratio_current_vs_official`,
   and the report's 9-point sweep (`--report-out full_report.md`).
4. Memory: keep the current −30% encoder advantage.
5. Full report regenerated and committed with the change.

## 5. Known-dead ends (do not re-litigate)
- Spectral-only replacement of the metric: bounded by Pearson ≈ 0.33 (overlap-dominated).
- Cross-term spectral proxy: partial IMDCT is O(overlap·N), more expensive than full synthesis.
- Exact reuse + synthesis: quality-neutral but slower at 48k (0.79×).
- Smarter challenger heuristics (low-band bias, correlation exponent): inert or worse.

## 6. Recommended order
1. **WS-A first** — validate A1 on the tracker; it is the only path to a step-change.
2. **WS-D2/D3** regardless — cheap, safe, complementary.
3. **WS-C** next for broad-base gains.
4. **WS-B** last (small, and depends on WS-A's final shape).

---

## 7. Progress log

### Done — WS-D1, WS-D2, WS-C (bit-exact)
Landed as one commit:
- D1: `compute_band_energies_and_normalise` (4138) now uses `std::log2` directly instead of
  `1.442695040888963387 * std::log(double)`.
- D2: `celt_encode_candidate` resets only the logical fields of `quality_frame_work` instead of
  `*quality_work = {}` (was memset-ing the ~26 KB struct, incl. the 18 KB decoder snapshot,
  every frame).
- C: `op_pvq_search_c` (9459) argmax rewritten as a vectorizable `candidate_num/den` precompute
  pass plus a scalar reduction (the old form was the only hot loop GCC reported as
  "not vectorized: loop nest containing two or more consecutive inner loops").

Evidence:
- **Bit-exact**: FNV-1a hashes of encoded packets identical on mono VOIP 24k, stereo AUDIO 48k,
  stereo AUDIO 96k (`tests`-style harness). `celt_quality` at 48k unchanged to 7 digits
  (99.50859017); `quant_all_bands_microbench` checksums unchanged.
- **Speed** (3-rep median, `benchmark_vs_official`, encode speedup vs official):

  | Rate | base | D+C |
  |---|---:|---:|
  | 16k | 1.2768 | 1.2968 |
  | 24k | 1.0733 | 1.0802 |
  | 32k | 1.0507 | 1.0715 |
  | 48k | 0.9795 | 0.9980 |
  | 64k | 1.0133 | 1.0226 |
  | 96k | 1.0948 | 1.1254 |
  | 128k | 1.1568 | 1.1630 |
  | 192k | 1.0612 | 1.0739 |
  | 256k | 0.9810 | 0.9790 |

  Net: consistent ~1–3% encode gain, zero quality/conformance change.
- Warning-free at `-Wall -Wextra -Wpedantic`.

**Note:** GCC `-O2` already auto-vectorizes 153 loops here, including the inner-product helpers,
MDCT, FFT and the PVQ `K>N/2` branch — so WS-C headroom is genuinely small without intrinsics.

### Remaining
WS-A (drop the per-frame history shadow-decode) is untouched and remains the ~50% prize.

### Attempted - WS-A must not be done naively
Implemented WS-A two ways and both failed; the per-frame probe is **load-bearing**, not pure overhead.

What the probe actually feeds (all coupled):
1. the guards `score[2..4]` (leaky band-energy history);
2. the **`packet_selection_ready` sync gate** (3824: it reads the *decoder* state
   `decoder_celt_state(...)->start == 0 && last_frame_type == 1`, which only the per-frame probe
   updates);
3. the decoder overlap/energy continuity that the eligible-frame decode needs.

Attempts:
- **Counter eligibility + probe removed** (search from frame 0): quality preserved
  (99.50859017) but **9-22% slower** across the sweep - because the shipped search does *not*
  start until `packet_selection_ready` (~frame 185 on the 60 s bench), so forcing frame-0
  eligibility adds far more challenger work than the probe removal saves (controlled A/B,
  3 reps: 48k 0.980 -> 0.844, ratio 0.86).
- **`packet_selection_ready` kept + probe removed**: the gate never opens, the search never
  runs, quality collapses to search-off (98.51 vs 99.51 at 48k).

Conclusion: deleting the probe is only worth it together with **replacing the decoder-derived
`packet_selection_ready` with an encoder-side equivalent** (the encoder knows its own
frame-type/mode transition), so the probe can run *only* on eligible frames while the guards
are either dropped (quality-neutral per the sweep above) or fed from encoder band energies.
That refactor is the real WS-A; the naive version is a regression.

### Attempted - encoder-side packet_selection_ready (still not enough)
Did exactly that: replaced
`previous_packet_celt && ready && decoder_start==0 && decoder_last_frame_type==1`
with `previous_packet_celt && ready && encoder_celt_state(st)->start == 0`.
- **In isolation it is correct and quality-neutral** (48k 99.50859017, 96k 99.94694972 - matches
  baseline exactly), so the gate can indeed be made decoder-independent.
- But with the probe also removed it *still* fails: the eligible path fires (frames 2, 14, 26,
  ...) yet always rejects, so quality falls to search-off (98.51). Cause: the per-frame probe
  also keeps the **decoder's overlap/energy state continuous**; without it the eligible-frame
  decode runs on a stale decoder state, the time-MSE is meaningless, and the challenger never
  wins.

**Final conclusion: WS-A is not viable.** The metric *is* a continuous decode - the per-frame
probe is what makes the eligible-frame decode valid, not merely gate/guard maintenance. The
~50% measured for "the search" is the price of a continuously-running reference decoder and
cannot be removed without changing what the metric measures. The encoder-side
`packet_selection_ready` refactor is safe but has no performance payoff, so it is not shipped
on its own.

### Research + prototype - masked-NMR metric (fresh idea, validated but not a speed win)
Surveyed how modern codecs avoid the live decoder: perceptual coders score candidates with the
**noise-to-mask ratio (NMR = error / masking-threshold)**, and the mask is computed from the
*input spectrum only* (no decode). FastEnc (HE-AAC v2) does this directly in the MDCT domain
with fixed conservative assumptions (SMR 29 dB, fixed spreading slopes) and is ~15x faster than
the classic iterative model. Analysis-by-Synthesis (what opuscpp does) is the known-expensive
branch. Note the stock Opus CELT encoder does **not** shadow-decode at all - it uses the PVQ
gain-shape ratio and `l1_metric`; the shadow decode is an opuscpp extra.

Prototype (built, measured, reverted):
- Added a FastEnc-style masked-NMR scorer: `nmr = SUM_b max(0, err_b/mask_b - 1)`,
  `mask_b = max(exp2(-depth_b), power_b * exp2(-29dB/6.02))`.
- **The metric works and is decision-equivalent**: on the tracked signal and all 16 diverse
  signals the NMR accept produced **bit-identical** `celt_quality` and `celt_masked_error` to
  the shipped time-MSE accept (`dCq = dCm = 0.000000` everywhere).
- Full removal prototype (encoder-side `packet_selection_ready` + no per-frame probe + NMR
  accept): **9-20% slower** (48k 0.80x) and quality fell to search-off at 48k/96k.

**Why it does not win:** the NMR needs the *exact* encoder-side reconstruction, and turning on
`resynth=1 / process_end=end` on every frame costs about the same as the shadow decode it
replaces. The metric is right; the reconstruction is now the bottleneck. A truly cheap version
would need an *analytic* PVQ distortion estimate (gain-shape residual `||x||^2 - <x,y>^2/||y||^2`,
already computed inside `op_pvq_search`) so no reconstruction/decode is needed at all - but
that is exactly the quantity the ordinary allocation already optimises, so it is unlikely to
change any decision.

**Net:** the perceptual-metric direction is sound and validated, but no version so far beats the
shipped code on speed without regressing quality. Recommend treating WS-A as closed unless a
genuinely decode-free distortion estimate can be devised.

### Reframe - "good decision first" instead of search (research + probes)
Shift from "verify two allocations" to "make the allocator's first answer good enough to skip
the search". Research: perceptual coders allocate by minimising NMR (`error/mask`) with the mask
from the input spectrum; RDO allocators (AAC/MP3, VVC RDOQ) solve
`min SUM w_b D_b(n_b) s.t. SUM R_b <= budget` using analytic distortion models, no decode.
Probes run this session:

1. **Energy-refresh lever (found).** The search's only real side effect is
   `pending_energy_refresh` (`allocation_history_changed` is set but never read - dead). Forcing
   the refresh every frame, with no search, gave: 32k 98.871->**99.274**, 48k 99.509->**99.605**
   on the tracked music signal (beats the search's 99.51). BUT on the 16-signal 60 s corpus it is
   **neutral-to-negative** (mostly 0; `mus_chord` -0.053 celt / +0.043 masked). So the lever is
   real but **content-dependent** - not a clean win as an unconditional toggle.
2. **Masked-NMR metric** (above): valid and decision-equivalent, needs the reconstruction.
3. **RDO allocator** (strongest untested direction): replace the static-prototype interpolation
   in `clt_compute_allocation` (9248) with an analytic PVQ distortion model
   `D_b(K) = E_b * (1 - <x,y>^2/(||x||^2 ||y||^2))` (computable inside `op_pvq_search`, no decode)
   plus the existing pulse-bit cache, solved greedily/Lagrangian with audibility weights `w_b`.
   This changes the *decision*, not the metric, and is the only direction that can win on both
   axes. Not yet prototyped (large, touches bitstream-critical allocation).

**Status: no shippable solution found.** Every probe either regresses quality, is content-
dependent, or needs the decode it was meant to replace. The one open direction with real upside
is the RDO allocator above; everything else is documented as closed.

### Correction - the search is cheap, so the quality-for-speed trade is bad
The earlier "search = ~50% of encode" (from a temp `#if 0` of `quality_history_enable`) was a
measurement artifact. Direct measurement on the tracked signal, harness `encode_ms`:
- interval 12: `current_ms = 22.42`; interval off: `20.58` -> search costs **~1.8 ms (~8%)**,
  and the official reference itself swung 20.9 -> 23.8 (noise ~3 ms), so it is within a few
  percent.
- Benchmark `benchmark_vs_official`: interval 12 -> 24 gives only **~2-3%** encode speedup
  (48k 0.984 -> 1.006), consistent with the search being ~2-8%.

Quality cost of dropping it (interval 24+ == off in effect): 48k CELT 99.51 -> 98.51 (-1.0),
96k -0.79; SNR/PESQ essentially unchanged (+1.57 / +3.6, above official).

**Verdict:** ~2-8% encode speed for ~1.0 CELT is not a worthwhile trade, so this is not shipped.
More importantly it corrects the premise: the search is not where encoder time goes. If speed is
the goal, the lever is the **base encode** (the other ~92%), not the search. The RDO allocator
(A) is still the only known direction that changes the decision and can win on both axes.

### Old version comparison (major clue)
`C:\Users\regis\Documents\New project\old` is a previous snapshot (`opus_codec.cpp` 14314 lines,
identical header) with **no quality-search machinery at all**. Built both with the same flags
and harnessed against official Opus:

| | 16k | 24k | 32k | 48k | 64k | 96k | 128k | 192k | 256k |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| OLD encode speedup | 2.58 | 1.92 | 1.88 | **1.82** | 1.80 | 1.89 | 2.10 | 1.89 | 1.81 |
| CUR encode speedup | 1.29 | 1.08 | 1.06 | **0.98** | 1.02 | 1.10 | 1.17 | 1.07 | 0.98 |

Quality (CELT delta vs official, tracked stereo signal):

| | 16k | 24k | 48k | 96k | 256k |
|---:|---:|---:|---:|---:|---:|
| OLD | +1.59 | +0.159 | **-1.475** | -0.624 | -0.124 |
| CUR | +1.59 | +0.058 | **-0.340** | +0.033 | -0.006 |

**So the old version is ~1.8-2.6x faster but ~0.5-1.1 CELT worse at mid/high rates** (16k/24k
comparable). The current build already spent ~1.8x speed buying that quality. Since the search
is only ~8%, **~1.7x was spent on the other added features** visible in the diff: per-frame
`measure_frame_activity` predictability/shape/difference-energy metrics, the stereo-policy
coherence tracking, VOIP voice denoise, and the decoder-path additions.

Implication: there is a **real, measured ~1.7x of base-encode speed sitting in the current
tree**, recoverable by finding which of these features earns its keep and gating the rest. This
is now the top speed lead - far bigger than the search (8%) or SIMD (probably 10-25%). Needs a
working profiler (mingw `-pg` produces an empty flat profile here) or a feature-toggle bisection.









### Correction 2 - the search really is ~1.8x (earlier 8% was wrong)
Toggle bisection against the 60 s benchmark signal (env-gated prepare_quality_history):

| rate | search ON cur_ms | search OFF cur_ms | speedx ON | speedx OFF |
|---:|---:|---:|---:|---:|
| 16k | 192.8 | 100.0 | 1.26 | **2.46** |
| 24k | 249.3 | 147.1 | 1.07 | **1.84** |
| 48k | 296.1 | 168.1 | 0.97 | **1.74** |

Search-off speed matches the old version almost exactly (16k 2.46 vs 2.58; 48k 1.74 vs 1.82),
which confirms **the entire old-vs-new encode gap is the quality search**. The earlier "~8%"
was an artifact of the 6 s harness signal, where packet_selection_ready engages only near the
end; on a 60 s stream the search costs ~1.8-1.9x at every rate.

Also ruled out by direct toggle: the per-frame analysis-checkpoint save (nalysis->image +
req + packet memcpy) is **negligible** (no measurable change when disabled).

So the real trade is: **~1.8x encode speed for the search's quality** (48k CELT -0.34 -> -1.48,
i.e. ~1.14; 16k/24k roughly unchanged). That is the decision to make - and the earlier metric
work (spectral, NMR) was trying to keep the search's quality at lower cost; all decode-free
metrics failed because the reconstruction costs as much as the decode.


### Answer - the search is nearly quality-inert, so removing it is the win
Measured search on/off (env-gated prepare_quality_history) with the quality harness:

Tracked synthetic 60 s, all metrics:
| rate | searchON celt | searchOFF celt | SNR/PESQ |
|---:|---:|---:|---:|
| 24k | -0.3323 | -0.3315 | identical |
| 48k | -0.7975 | -0.8603 | identical |
| 96k | -0.6048 | -0.6571 | identical |

So the search buys ~0.05 CELT and nothing on SNR/PESQ. Across the 16-signal 60 s corpus it is
**identical for 12/16 signals** and within +-0.05 CELT for the rest (turning xtra_depth off
statically was sometimes *better*, e.g. mus_chord -1.186 vs search -1.193). The earlier
"old = -1.48 vs new = -0.34" gap is therefore **the other added features (stereo policy etc.),
not the search** - the search had been credited for quality it doesn't deliver.

**Conclusion: the "we need other quality means before removing the search" premise is
unnecessary - the search's quality contribution is ~0 while it costs ~1.8x.** Removing the
search (drop quality_*, celt_encode_with_history, the per-frame probe) is a near-free 1.8x
encode speedup. That is the answer: remove it, do not replace it.


### Correction 3 - the search is NOT quality-inert; removing it regresses AUDIO
The full official report on the search-free build exposed the error in the "quality-inert"
conclusion. AUDIO celt deltas (search-free vs official) vs the search-on baseline:

| | 16k | 24k | 32k | 48k | 64k | 96k | 128k | 192k | 256k |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| search ON | +1.59 | +0.06 | -0.16 | -0.34 | -0.05 | +0.03 | +0.02 | -0.00 | -0.01 |
| search OFF | +1.59 | +0.12 | -0.63 | **-1.34** | -0.85 | -0.76 | -0.25 | -0.20 | -0.13 |

So the search buys ~1.0 CELT at 48k on the report's synthetic-music AUDIO signal and keeps
32k-128k above/near official; without it those rates fall below official. (VOIP is unaffected,
as before.) The earlier "inert" result came from the diverse corpus, which is not representative
of the report signal - so the conclusion was over-generalised.

**Decision: the search removal (8cd269) was reverted (1f6b1c7).** The 1.8x speed is real but
not worth dropping 32k-128k AUDIO below official. The search stays. Net shipped work remains the
bit-exact D+C speedup (5885387) and the earlier throttle/scratch fixes ( f3e774).


### Correction 4 - "learn the rule" also fails; the search cannot be replaced statically
Instrumented the search (OPUSCPP_RULE) on the report signal + the 16-signal 60 s corpus: 123
eligible frames, 40 challenger-wins (~33%). Then tried to predict the winner from decode-free
features:
- **Static features** (bitrate, intensity, lastCodedBands): best thresholded rule = 0.675 accuracy,
  i.e. identical to always-reject. The features don't carry the decision.
- **Decode-derived score[2]** (leaky band-energy trajectory guard) ratio: predicts at **0.886** -
  but it needs the decode we are trying to remove.

Meaning: the search's accept/reject needs information only the shadow decode produces. There is
no static-feature rule, no allocation/refresh setting, and no decode-free metric (spectral, NMR)
that reproduces it - every avenue is now closed with direct measurement.

**Final: the per-frame search is the only mechanism that yields its quality; it stays.** 1.8x
speed and search-on quality are mutually exclusive given the current architecture. Shipped work
remains 5885387 (D+C bit-exact speedup) and  f3e774 (throttle + PVQ scratch). The tree is at
dd12d79 (search restored, all gates green).
