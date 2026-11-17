# bench17 baseline evidence: bitrate allowance and minimum deltas

Pin: upstream libopus `f1afc4222aff7c5456d27581ec618ae64158fbe1`, scalar build (`OPUS_DISABLE_INTRINSICS`, no RTCD), GCC 16.2 `-O2 -DNDEBUG`.
Harness: `tests/bench/bench17.py quality`, nominal target requested for both codecs, official decoder for both.

## Determinism
Official, 3 independent invocations per config: max-min spread 0 for every metric, bitrate, packet count and final range in all 17 configs. Our encoder is deterministic by construction. Run-to-run variance therefore puts no lower bound on minimum deltas.

## Official bitrate vs target (VBR, constrained default)
- AUDIO (CELT): official overshoots by about +400 bps in every config (+2.9% at 16 kbps, +0.17% at 256 kbps). This is one byte per 20 ms packet: CELT's VBR target leaves out the TOC byte.
- VOIP (SILK/hybrid): official undershoots by 10-17% (e.g. hazel_voice_16000 14,372 bps, david_voice_64000 54,105 bps).

Window effect: in AUDIO the ceiling is min(1.01 x target, official actual), so we must stay at or below official's spend. In VOIP we may spend up to 1.01 x target, which is well above official. The current candidate counts the TOC byte in CELT VBR, and all AUDIO configs land at 1.0001-1.0035 x target.

## Sensitivity of each metric to +1% bits (official, per config)
Raw data: `build/bench/plus1pct.json` (official and ours at 1.00x and 1.01x of target, all 17 configs).
Maximum over configs of |effect of +1% bits| on official:

| metric | max effect | unit |
|---|---:|---|
| snr_db | 0.232 | dB |
| seg_snr_db | 0.184 | dB |
| pesq_style | 0.014 | score |
| visqol_style | 0.0035 | score |
| logband_corr | 0.0006 | corr |
| celt_quality | 0.20 | score (0-100) |
| rms_error | 2.6 | % relative |
| mean_abs_error | 2.3 | % relative |
| logband_error | 2.5 | % relative |
| celt_masked_error | 34.8 | % relative |
| celt_highband_error | 137 | % relative |
| stereo_width_error | 2.3 | % relative |

The CELT perceptual metrics (celt_masked_error is a 16th-power mean dominated by the worst windows; celt_highband_error is tiny in absolute terms at high rates) move by tens of percent from a 1% rate change. Their per-config values are mostly chaotic, not a monotone function of bits.

## Candidate minimum-delta rules
- BASE: absolute for scores (snr/seg_snr 0.05 dB, pesq 0.01, visqol 0.005, logband_corr 0.001, celt_quality 0.05); 1% relative for error metrics. This fails the "+1% bits must move a metric by less than half its min delta" check for every metric.
- PER-CONFIG: min_delta(metric, config) = max(BASE, 2 x |official +1%-bits effect in that config|). A relative error-metric delta of 100% or more makes the pair saturated (official within min delta of the 0 bound), so we only have to match official there.

Failing pairs on the current candidate (`build/bench/results/20261117_174909_quality.json`): BASE 28, PER-CONFIG 41.
