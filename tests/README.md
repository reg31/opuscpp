# Tests and Metrics

This directory contains portable test harnesses and benchmark documentation for `opuscpp`.

No audio assets are required by the smoke tests. Synthetic samples can be generated locally with `generate_synthetic_wav.py` if you want listening material.

## Quick smoke test

From the repository root:

```bash
python3 tests/run_smoke.py --cxx c++
```

The smoke test compiles `src/opus_codec.cpp`, generates PCM in-process, encodes and decodes mono/stereo frames at 16/24/32/48/96/128/192/256 kbps, and checks packet duration/round-trip success.

Generate optional local listening samples:

```bash
python3 tests/generate_synthetic_wav.py --out tests/generated_audio
```

The generated files are ignored by git.

## RFC decode conformance

The RFC vector files are not committed to this repository. To run full decode conformance:

1. Obtain the official Opus RFC test vector set.
2. Build official Opus 1.6.1 as a static library with intrinsics disabled if you want matched portable-C comparison.
3. Build the decoder harness:

```bash
c++ -std=c++23 -O2 -I src \
    tests/conformance_decode.cpp src/opus_codec.cpp \
    -o build/conformance_decode
```

4. Run each vector through `conformance_decode` and compare the generated PCM with the reference decoded PCM using the official `opus_compare` tool from the Opus source tree.

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| RFC decode vectors | 24/24 passed |
| Mono/stereo coverage | Passed |
| Final range check mode | Supported by harness |

## Encode oracle conformance

Encode conformance is checked by encoding the same validation cases with `opuscpp`, decoding with official Opus, and comparing against an official-oracle path. The relevant files are:

- `conformance_encode.cpp`
- `official_encode_oracle.cpp`
- `encode_conformance_shared.h`

Measured result for this repository snapshot:

| Suite | Result |
|---|---:|
| Encode oracle cases | 96/96 passed |

## Perceptual and memory harness

`perceptual_memory_validation.cpp` compares this implementation with official Opus on generated or user-provided 16-bit PCM WAV input. It reports:

- SNR and segmental SNR.
- PESQ-style proxy score.
- ViSQOL-style proxy score.
- CELT-style high-band proxy score.
- Average packet bytes.
- Encode time.
- Optional process memory measurements.

These proxy scores are useful for regression tracking, but they are not substitutes for PESQ/ViSQOL official tooling or listening tests.

## Speed metrics vs official Opus

Matched setup: official Opus built at `-O2`, intrinsics disabled. Positive decode percent means `opuscpp` is faster; negative means slower. Encode speed is a multiplicative speedup over official Opus.

| Bitrate | Encode speed | Current avg bytes | Official avg bytes | Decode vs official |
|---:|---:|---:|---:|---:|
| 16 kbps | 2.40x | 39.99 | 39.63 | -1.5% |
| 24 kbps | 3.73x | 60.91 | 57.66 | -9.0% |
| 32 kbps | 4.87x | 81.02 | 75.72 | -5.4% |
| 48 kbps | 3.40x | 121.21 | 113.15 | -8.0% |
| 64 kbps | 2.16x | 160.74 | 160.74 | +5.4% |
| 96 kbps | 2.10x | 240.66 | 240.61 | +24.6% |
| 128 kbps | 2.01x | 320.54 | 320.48 | +24.5% |
| 192 kbps | 2.14x | 480.30 | 480.22 | +12.4% |
| 256 kbps | 2.03x | 640.04 | 639.95 | -5.5% |

Source CSVs:

- `metrics/encode_speed_vs_official.csv`
- `metrics/decode_speed_vs_official.csv`

## Quality metrics vs official Opus

Quality proxy metrics were measured on the validation corpus used during development. Deltas are `opuscpp - official`.

| Bitrate | Current PESQ-style | Official PESQ-style | PESQ delta | Current ViSQOL-style | Official ViSQOL-style | ViSQOL delta | Current CELT proxy | Official CELT proxy | CELT delta |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 kbps | 1.5068 | 1.4601 | +0.0466 | 4.5805 | 4.6154 | -0.0349 | 1.2828 | 0.4768 | +0.8060 |
| 24 kbps | 1.4867 | 1.4566 | +0.0302 | 4.6132 | 4.6267 | -0.0135 | 87.8095 | 0.0882 | +87.7214 |
| 32 kbps | 1.4795 | 1.4567 | +0.0228 | 4.6337 | 4.6296 | +0.0041 | 87.4363 | 0.0882 | +87.3481 |
| 48 kbps | 1.4744 | 1.4568 | +0.0176 | 4.6361 | 4.6361 | +0.0000 | 90.4999 | 96.9481 | -6.4482 |
| 64 kbps | 1.4582 | 1.4575 | +0.0008 | 4.6371 | 4.6366 | +0.0005 | 91.1172 | 96.8792 | -5.7620 |
| 96 kbps | 1.4588 | 1.4578 | +0.0010 | 4.6392 | 4.6404 | -0.0012 | 92.6484 | 96.8992 | -4.2508 |
| 128 kbps | 1.4589 | 1.4580 | +0.0009 | 4.6386 | 4.6391 | -0.0005 | 93.2536 | 96.9260 | -3.6724 |
| 192 kbps | 1.4591 | 1.4583 | +0.0009 | 4.6406 | 4.6406 | +0.0000 | 96.3118 | 96.9277 | -0.6159 |
| 256 kbps | 1.4592 | 1.4583 | +0.0010 | 4.6408 | 4.6407 | +0.0001 | 96.8805 | 96.9322 | -0.0517 |

Source CSV:

- `metrics/quality_vs_official.csv`

## Memory metrics

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,848 B | 31,648 B | -46.8% |
| Encoder stereo | 32,448 B | 48,880 B | -33.6% |
| Decoder mono | 14,112 B | 18,336 B | -23.0% |
| Decoder stereo | 21,376 B | 27,408 B | -22.0% |

## Binary size

| Build | Text | Data | Total measured text+data |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 226,380 B | 0 B | 226,380 B |
| Android arm64 Clang `-O2` | 251,815 B | 800 B | 252,615 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC C++23 | Builds with zero warnings in measured configuration. |
| Android arm64 Clang C++23 | Builds with zero warnings in measured configuration. |
| Linux/macOS | Intended to build with any C++23 compiler; use `tests/run_smoke.py` for a quick local check. |
