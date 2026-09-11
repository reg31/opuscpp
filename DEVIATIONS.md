# Encoder deviations from official Opus — correction list

Ours = `src/opus_codec.cpp`; Official = `tests/external/official_opus/celt/`. Ordered by likely
quality impact. "Reduce?" = could this lower quality vs official.

## Tier 1 — major official adaptations missing (very likely quality-reducing)

1. **`tf_analysis` / `tf_select` absent.** Official runs a per-band L1-metric Viterbi search over
   time/frequency resolutions (`celt_encoder.c:663-822` analysis, `824-862` encode, gated
   `2242`). Ours set every band to a single per-frame constant; no `tf_select`, no
   `importance[]`, no `lambda`. **DONE.** Ported `l1_metric` + `tf_analysis`, added dynalloc
   `importance[]`, threaded `tf_select` through `process_tf_changes`, and gated the new path by
   the official `enable_tf_analysis` condition (`effectiveBytes >= 15*C && !hybrid &&
   complexity >= 2 && toneishness < .98`), keeping the original constant-fill for every other
   case. Full report: AUDIO celt **48k −0.34→−0.01 (+0.33)**, **16k 1.58→1.76 (+0.18)**,
   24k +0.04, 64k +0.07, 32k/96k/128k/192k/256k neutral-positive; VOIP byte-identical
   (0.000 delta); RFC 24/24, interop, Android arm64 pass; warning-free. No regressions.
   Root cause of the earlier mono crash: the failed attempt had replaced the non-`tf_analysis`
   `tf_res` fill with opuscpp-specific `hybrid` branches, changing the mono hybrid bitstream;
   restoring the original fill for that branch fixed it.

2. **`spreading_decision` + tapset replaced.** Official (`bands.c:470-561`) measures per-band |x|
   CDF, weights by `spread_weight[]`, keeps recursive `tonal_average`/`hf_average` with
   hysteresis, and emits SPREAD_NONE/LIGHT/NORMAL/AGGRESSIVE plus a tapset. Ours (`6187-6198`)
   defaults SPREAD_NONE and only bumps to SPREAD_NORMAL for one narrow condition; LIGHT/AGGRESSIVE
   never used; tapset hardcoded 0 (`5598,5620,5809`). SPREAD_NONE also disables `exp_rotation`
   and changes folding. **Reduce? no — TESTED, reverted.** Porting official `spreading_decision`
   (with `spread_weight[]` + `tonal_average`/`hf_average` state) measured AUDIO 16k celt
   **−0.47 worse** (1.590→1.118) and ~neutral at 24k/32k/48k/64k. Our tuned heuristic is a
   beneficial deviation for the low-rate pipeline; keep it.

3. **`transient_analysis` / `tf_estimate` replaced.** Official (`celt_encoder.c:267-469`,
   `patch_transient_decision` `473-507`) does masking-based detection, suppresses LF-tone-induced
   transients, marks weak transients, and derives `tf_estimate` (feeds trim/VBR/prefilter). Ours
   (`5684-5699`) uses `max|x[i]-x[i-4]|*(len/4) > thr*sum` only, no tone suppression, no
   patch pass, plus extra `isTransient=1` overrides (`6077-6088`). **Reduce? yes.**

4. **Coarse-energy two-pass intra/inter decision absent.** Official (`quant_bands.c:260-358`)
   trial-codes intra and inter into a backup coder and keeps the cheaper (`badness`, `intra_bias`
   scaled by `loss_rate`). Ours (`9018-9037`) runs exactly one pass; no `loss_rate`/`two_pass`.
   **Reduce? yes.**

5. **Stereo `theta_rdo` absent.** Official (`bands.c:1618-1622,1808-1895`) at complexity>=8,
   stereo non-dual, trial-encodes both angle roundings with `resynth=1` and keeps the lower
   weighted distortion. **DONE.** Ported `compute_channel_weights`, `theta_round` in
   `compute_theta` + `band_ctx`, `resynth = !encode || theta_rdo`, `complexity` through
   `quant_all_bands`, the two-pass save/restore trial block, and the faithful
   `alg_quant(..., gain, resynth)` reconstruction (unpack `iy`, reverse `exp_rotation`).
   Two blockers found and fixed: (a) the encode call passed `collapse_masks = nullptr`, but
   `resynth`-on-encode writes/folds `collapse_masks[i*C]` → null write (`0xC0000005`); fixed by
   passing a real buffer. (b) opuscpp's `celt_adjust_alloc_trim` adds **+2 for 80000..112000**,
   with no official equivalent; official's theta_rdo assumes official's allocation, so in that
   band the trial was miscalibrated and **96k celt regressed −0.42**. Gated `theta_rdo` off in
   exactly that band (`!(C==2 && bitrate>=80000 && bitrate<112000)`); 96k returns to +0.030.
   Landed: AUDIO 32k +0.018, 64k +0.013, 96k/128k neutral, 24k −0.007 and 48k −0.011 (tiny,
   near the report noise floor); VOIP byte-identical; RFC 24/24, interop, Android pass;
   warning-free.

## Tier 2 — dropped terms in shared analyses (likely quality-reducing)

6. **`compute_vbr` terms lost.** Official (`celt_encoder.c:1604-1716`): activity (`1630-1633`),
   transient boost using real `tf_estimate` (`1652-1653`), tonality + `pitch_change`
   (`1655-1669`), surround (`1675-1680`), `tf_estimate<.2` gate (`1703`), constrained factor
   `0.67`. Ours (`5642-5676`): `target += -0.044*target` (assumes `tf_estimate=0`), no
   tonality/activity/surround, constrained factor `0.67+0.07*content_vbr`, temporal-VBR applied
   unconditionally. **DONE `e3f86f4`** — restored `(tf_estimate-.044)*target` and the
   `tf_estimate<.2` VBR guard; neutral on the tracked (non-transient) signal, correct on
   transients. Remaining: activity/tonality/surround need a float analysis struct we don't have.
   **Tested the tonality term with `toneishness` as a proxy — neutral** (and the proxy has the
   wrong polarity: official's analysis tonality is ~0.5/bipolar, `toneishness` is ~0 for most
   content). So the remainder is **blocked on porting official's `run_analysis` (MLP)**; until
   then it is faithfully skipped (official only applies these `if (analysis->valid)`).

7. **`alloc_trim_analysis` terms lost + extra.** Official (`865-955`) subtracts `2*tf_estimate`
   (`933`), `surround_trim` (`932`), and the float `tonality_slope` term (`934-939`). Ours
   (`5170-5222`) omits all three and adds `if (equiv_rate>=64000) trim += 1.f` (`5216-5218`,
   BENEFICIAL — tested). **Reduce? no change** — `2*tf_estimate` already matches; the
   tonality/surround terms need the analysis struct (see #6; official gates them on
   `analysis->valid`, so skipping is faithful for a build without the MLP); the stereo
   correlation uses `celt_inner_prod_c` vs official's `celt_inner_prod_norm_shift`
   (`NORM_SHIFT=24`, fixed-point shifts are identity in float → **equivalent**).

8. **`dynalloc_analysis` adaptations lost + constants changed.** Official (`1049-1273`) computes
   `mask`/`sig`/`spread_weight[]` (`1082-1117`) and `importance[]` (`1182-1191`), tone boost
   `2/1/1/0.5` (`1205-1222`), `leak_boost` (`1226-1230`), and net `×1` for low bands i<8
   (`1193-1204`). Ours (`5310-5429`) omits `importance`/`spread_weight`/`leak_boost`, tone boost
   `4.5/2.5/1.5/0.5` (`5265-5282`, ~2x), extra `apply_low_rate_lf_dynalloc_boost`
   (`5284-5308`), extra high-rate `follower[0]` boost (`5399-5401`), and uses `2-corr^2` for low
   bands (`5378-5390`, `×2` for mono). **Reduce? yes.**

9. **`run_prefilter` terms lost + shortcut.** Official (`1404-1602`) kills gain on large pitch
   change if `tf_estimate>.98` (`1502-1508`), halves/zeros gain by `loss_rate` (`1482-1487`),
   uses `prefilter_tapset` (`1542-1555`), scales by `max_pitch_ratio` (`1492-1497`). Ours
   (`5499-5640`) omits all, hardcodes tapset 0, and adds a "reuse previous period/gain" shortcut
   (`5546-5549`). **Reduce? yes.**

10. **`secondMdct` long-window energy (`bandLogE2`) not produced.** Official (`2076-2088`) feeds
    dynalloc a long-window baseline at `shortBlocks && complexity>=8`; ours (`6100`) always uses
    the short-block energy. **Reduce? unclear.**

11. **`patch_transient_decision` second pass absent.** Official (`473-507`) re-examines band
    energies to catch time-domain-missed transients. Ours: none. **Reduce? no — TESTED, reverted.**
    Ported the function plus the full official block (recompute `shortBlocks`/`compute_mdcts`/
    band energies, lift `bandLogE2` by `.5*LM`, set `tf_estimate=.2`) gated by the official
    `complexity>=5 && !isTransient && !hybrid`. Result: AUDIO 16k celt **1.76→0.95 (−0.81)**
    and mono VOIP segfaults. Our combined `compute_band_energies_and_normalise` is not equivalent
    to official's separate `compute_band_energies`+`amp2Log2`, and the mid-frame `compute_mdcts`
    aliasing needs auditing. Reverted.

## Tier 3 — extra non-official heuristics in ours (direction uncertain)

12. Extra alloc-trim post-adjust: `celt_adjust_alloc_trim` (`5831-5858`),
    `celt_balance_lowrate_stereo_trim` (`5860-5889`), and the RDO-trim (`9328-9339`). Official has
    none. **Reduce? unclear.**

13. Extra VBR target boosts after `compute_vbr` (`6257-6273`). Official: none. **Reduce? unclear.**

14. Extra HF-tonal / energy-feedback state (`high_z_tonal_Q7` `2376-2601`, `2915-2922`;
    `celt_energy_feedback_bypass_*` gating `6172-6177`). Official applies the energy-error bias
    unconditionally. **Reduce? unclear.**

## Faithful (no material divergence)
`stereo_analysis`; `interp_bits2pulses`/`clt_compute_allocation` (all constants match);
`quant_partition`/`quant_band`/`quant_band_stereo` core; `compute_theta` core; `exp_rotation`;
`op_pvq_search`/`alg_quant`; `compute_mdcts`/preemphasis; anti-collapse reserve;
`process_fine_energy`; intensity hysteresis; `tf_select_table`.

## Recommended correction order (highest quality / lowest risk first)
1. [done] #3 transient/`tf_estimate` (`05dd0b9`) — enables #6/#7/#9 below.
2. [done] #4 coarse-energy two-pass (`ca234d1`) — AUDIO 24k +0.57, 32k +0.40.
3. [tested: keep deviation] #2 `spreading_decision` — regressed AUDIO 16k −0.47; reverted.
4. #1 `tf_analysis`/`tf_select` (needs `importance[]` from #8).
5. #8 dynalloc `importance`/`spread_weight` + tone-boost constants.
6. #6 `compute_vbr` tf/tonality/surround terms (needs `tf_estimate`).
7. #7 alloc-trim terms (keep the `+1`).
8. #5 stereo `theta_rdo` (stereo only).
9. #9 prefilter terms; #11 patch-transient; #10 secondMdct.
