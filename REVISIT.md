# Experiments to revisit (the `celt_quality` metric misled us)

`celt_quality` (the harness's band-error column) is dominated by **band 0 / low-band
(band-error-in-DC)** and does **not** track perceptual quality on tonal/transient content. For
`sweep` at 96k it read **−16 while snr was +3.4, pesq +0.10, visqol +0.009**. The stereo-mode
fix (`d192a92`) proved this: `celt_quality` said "regression", the perceptual metrics said
"large win".

**Rule going forward:** judge quality by `snr_delta` / `pesq_delta` / `visqol_delta`
(and RFC/interop), **not** `celt_quality`. Re-test everything previously rejected *solely*
because `celt_quality` went negative.

## Rejected-on-celt_quality experiments to re-run

1. **Cluster A — official `spreading_decision`** (replaced our 0/2 heuristic with the official
   CDF + `spread_weight[]` + `tonal_average`/`hf_average` state). Rejected for AUDIO 16k
   `celt = 1.59 → 1.12`. **RE-VISITED (`cb49b23`):** with perceptual metrics the tracked AUDIO
   deltas are **neutral** at every rate (16k snr −0.003, pesq +0.0004) — confirming the old
   rejection was a `celt_quality` artifact. **BUT** mono VOIP still segfaults (`0xC0000005`).
   Net: no quality benefit + a crash → **skip** (not worth root-causing).
2. **#11 `patch_transient_decision`** (second transient pass). Rejected for AUDIO 16k
   `celt −0.81` + a mono crash. **Re-run** with the perceptual metrics after fixing the
   `compute_band_energies`/`amp2Log2` equivalence and the mono fault.
3. **#10 `secondMdct`** (long-window `bandLogE2` at `shortBlocks && complexity>=8`). Rejected
   for `celt −1.31 @48k`. **RE-VISITED:** perceptual deltas **unchanged** (24k pesq 0.262,
   48k 0.118, 96k 0.088; transient corpus neutral). Rejection was an artifact, but no
   perceptual benefit either → **skip**.
4. **#7 alloc-trim terms** (`2*tf_estimate` already matches; tonality/surround/no-`+1`). Never
   cleanly measured with perceptual metrics.
5. **RDO-trim allocator** (`OPUSCPP_RDOTRIM`, off by default). Rejected as a search
   replacement on `celt`; re-evaluate purely as a quality tweak with pesq/visqol.
6. **`compute_vbr` extra terms** (#6 remainder: activity/tonality/surround) — only the tf term
   landed; the rest are untested.

## Mode routing (tracked as its own task)

7. **Route signals to full-band CELT at 48k+** (matching official). Landed: stereo AUDIO at
   `bitrate_bps >= 48000` (`d192a92` + 48k+ scope). Remaining to verify/route:
   - 24k stereo AUDIO: official uses **hybrid**, we use CELT. Decide whether to match.
   - Mono CELT / hybrid boundaries across rates — confirm ours equals official via TOC dumps
     (`OPUSCPP_TOC` harness hook, add if needed).

## Notes
- Tooling: `OPUSCPP_BANDS=1` (per-band error, current vs official).
- Do **not** re-reject a change on `celt_quality` alone again.
