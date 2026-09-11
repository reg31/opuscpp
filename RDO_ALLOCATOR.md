# RDO Allocator Design

Goal: raise the **search-off** encode quality so the reconstruction-based quality search can be
dropped (recovering ~1.8x encode speed) without losing the quality it currently buys. This is a
**quality** project, not a speed one.

## 1. Hard constraint: the allocator is decoder-synced

`clt_compute_allocation` (opus_codec.cpp ~9248) and `interp_bits2pulses` are run by **both** the
encoder and the decoder from the bitstream. The decoder re-derives `pulses[]`, `ebits[]`,
`fine_priority[]` and `balance` from the transmitted side information. The encoder therefore
**cannot** choose an arbitrary pulse allocation; its only degrees of freedom are the parameters
that are actually signaled and consumed by the shared allocator:

- `alloc_trim` (0..10): the LF/HF tilt applied through `trim_offset[band]`.
- `offsets[band]` (`dynalloc_analysis`): per-band boosts.
- `cap[band]`: per-band bit caps (derived from the budget).
- `intensity`, `dual_stereo`: stereo coupling decisions.

So "RDO allocator" here means: **choose the signaled parameters (primarily `alloc_trim` and
`offsets`) that minimise an analytically estimated distortion**, instead of the current fixed
heuristics (`alloc_trim_analysis`, `dynalloc_analysis`). The pulse assignment and bitstream
syntax are untouched, so decoder compatibility is preserved by construction.

## 2. Distortion model

For band `b` with `N_b` coefficients, unit-normalised shape, unquantised energy `E_b`, and `K_b`
PVQ pulses, model the expected shape-quantisation MSE as

    D_b(K) = E_b * g(N_b, K)

with `g` the PVQ gain-shape residual. A practical monotone form (high-rate behaviour, cheap):

    g(N, K) = 1 / (1 + K / N)          # 0 at K=0 -> 1, decreasing in K

(or the sharper `g(N,K) = (N / (N + 2K))`). Only monotonicity and rough curvature matter for
ranking allocations; the absolute scale cancels in the comparison.

Audibility weight `w_b`: reuse the masking floor already computed by `dynalloc_analysis`
(`noise_floor[b]`, `celt_noise_floor_base[b]`, `eMeans[b]`, `bandLogE[b]`). A band is audible
when it is well above its masking floor:

    depth_b  = max(0, bandLogE[b] - (celt_noise_floor_base[b] + 9 - lsb_depth))
    w_b      = clamp(1 - 2^(-depth_b), 0, 1)      # ~1 for audible, ~0 for fully masked

Total estimated distortion:

    D(params) = SUM_b w_b * D_b(K_b(params))

`K_b(params)` is obtained by running the existing `clt_compute_allocation`/`interp_bits2pulses`
with the candidate parameters (cheap: O(nbEBands)).

## 3. Search

Two bounded variants, in increasing cost:

**RDO-trim (primary).** Enumerate the 11 legal `alloc_trim` values (or a window around the
heuristic), keep `offsets` from `dynalloc_analysis`, run the allocator for each, and pick the
`alloc_trim` minimising `D`. This directly attacks the current heuristic's weakest link (a
single global tilt chosen by a hand-tuned formula) at ~11x allocator cost (allocator only, no
PVQ, no decode).

**RDO-boost (secondary).** Given the chosen trim, do a small Lagrangian move of boosts between
the lowest- and highest-marginal-benefit bands (one or two swaps) to reduce `D`, subject to the
same total bits. Must be expressed as signaled `offsets`.

## 4. Implementation plan

1. Extract a helper `estimate_allocation_distortion(...)` that runs the encoder-side allocator
   for a given `(alloc_trim, offsets)` and returns `D`. Reuse `celt_pvq_v_entry`/the pulse cache
   for `g`.
2. Add `rdo_alloc_trim(...)` computing the trim by the §3 search; call it where
   `alloc_trim_analysis` is currently called, behind a build/runtime flag so both can be
   compared.
3. Validate: search-OFF AUDIO `celt_quality` and PESQ/ViSQOL must be **>= the search-ON
   baseline** across the 9 rates; RFC/interop/Android unchanged (bitstream is still valid, just a
   different valid choice). If green, disable the search.
4. If the trim alone is insufficient, add the §3 boost step.

## 5. Acceptance test (from SPEED_MASTERPLAN.md)

Run `tests/scripts/setup_official_compare.py --report-out full_report.md` with the search OFF and
the RDO allocator ON, and require the AUDIO CELT delta row to be **>= the search-ON row**
(48k >= -0.34, 96k >= +0.03, etc.). Speed target: the search-off speed (~1.8x at 16k–48k).

## 6. Risks

- `alloc_trim` interacts with `dynalloc` boosts and the VBR budget; a wrong trim can starve the
  HF or waste bits on masked bands.
- The distortion model must rank two allocations consistently; if it does not, the search picks
  worse allocations than the heuristic (a regression, as happened with the earlier spectral and
  NMR proxies).
- Verdict to watch: if RDO-trim cannot beat the existing `alloc_trim_analysis`, the whole
  approach is closed and the search stays.
