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


