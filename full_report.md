# Official Comparison Report

This report summarizes the latest local benchmark refresh.
The full official-comparison setup lives in `tests/scripts/setup_official_compare.py`, and the committed source tables live in `tests/metrics`.

## RFC decode conformance

- Official Opus checkout prepared locally.
- Official `opus_demo` / `opus_compare` build prepared locally.
- `opuscpp` decoder conformance harness built locally.
- Published repository result: 24/24 RFC decode vectors passed.

## Encode oracle conformance

- Fresh local result: 144/144 encode oracle cases passed.

## Perceptual and memory harness

- Published repository snapshot includes perceptual proxy metrics, packet-byte deltas, encode timing, and memory figures.
- Source harness: `tests/perceptual_memory_validation.cpp`.

## Speed metrics vs official Opus

| Bitrate | Encode speedup | Decode speedup |
|---:|---:|---:|
| 16 kbps | 1.285610x | 1.380349x |
| 24 kbps | 1.426038x | 1.118988x |
| 32 kbps | 1.308942x | 1.099954x |
| 48 kbps | 1.229498x | 1.041681x |
| 64 kbps | 1.248527x | 1.045234x |
| 96 kbps | 1.473938x | 1.020750x |
| 128 kbps | 1.600741x | 0.983899x |
| 192 kbps | 1.501439x | 1.011969x |
| 256 kbps | 1.500865x | 0.936989x |

## Quality metrics vs official Opus

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT delta |
|---:|---:|---:|---:|
| 16 kbps | -0.0003 | -0.0153 | 1.3037 |
| 24 kbps | -0.0085 | 0.0279 | 22.0159 |
| 32 kbps | -0.0005 | 0.0239 | 16.8861 |
| 48 kbps | 0.0001 | 0.0029 | 0.3869 |
| 64 kbps | 0.0011 | -0.0028 | 0.1908 |
| 96 kbps | -0.0004 | -0.0011 | -0.2133 |
| 128 kbps | 0.0005 | 0.0009 | -0.2383 |
| 192 kbps | 0.0001 | -0.0009 | -0.2936 |
| 256 kbps | 0.0001 | -0.0017 | -0.2176 |

## Detector mode-balance spot check

| Material class | SILK | Hybrid | CELT |
|---|---:|---:|---:|
| Speech-like | 0.0% | 8.0% | 92.0% |
| Harmonic music | 0.0% | 0.0% | 100.0% |

## Memory metrics

| State | Difference |
|---|---:|
| Encoder mono | -45.7% |
| Encoder stereo | -33.6% |
| Decoder mono | -22.3% |
| Decoder stereo | -22.0% |

## Binary size

| Build | Text | Data | Total measured text+data |
|---|---:|---:|---:|
| Host MinGW GCC `-O2` | 227,592 B | 0 B | 227,592 B |
| Android arm64 Clang `-O2` | 252,627 B | 800 B | 253,427 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC C++23 | Builds with zero warnings in measured configuration. |
| Android arm64 Clang C++23 | Builds with zero warnings in measured configuration. |
