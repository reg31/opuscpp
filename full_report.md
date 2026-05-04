# Official Comparison Report

This report summarizes the latest local benchmark refresh.
The full official-comparison setup lives in `tests/scripts/setup_official_compare.py`, and the committed source tables live in `tests/metrics`.

## RFC decode conformance

- Official Opus checkout prepared locally.
- Official `opus_demo` / `opus_compare` build prepared locally.
- `opuscpp` decoder conformance harness built locally.
- Published repository result: 24/24 RFC decode vectors passed.

## Encode oracle conformance

- Fresh local result: 96/96 RFC 8251 encode oracle cases passed; the broader documented gate remains 144/144 across RFC 6716 + RFC 8251 cases.

## Perceptual and memory harness

- Published repository snapshot includes perceptual proxy metrics, packet-byte deltas, encode timing, and memory figures.
- Source harness: `tests/perceptual_memory_validation.cpp`.

## Speed metrics vs official Opus

`opuscpp` is compiled globally with `-O2 -DNDEBUG`, with selected GCC function-level `O3`/`Os` attributes in the source. Official Opus 1.6.1 is built in Release mode (`-O3 -DNDEBUG`) with intrinsics disabled, so these numbers compare the intended `opuscpp` source-embedded optimization profile against official Opus' normal portable Release build.

| Bitrate | Encode speedup | Decode speedup |
|---:|---:|---:|
| 16 kbps | 1.356050x | 1.201277x |
| 24 kbps | 1.344907x | 0.984250x |
| 32 kbps | 1.350392x | 0.983717x |
| 48 kbps | 1.227675x | 0.980783x |
| 64 kbps | 1.313250x | 0.981243x |
| 96 kbps | 1.410520x | 0.934501x |
| 128 kbps | 1.511734x | 0.895410x |
| 192 kbps | 1.468819x | 0.919786x |
| 256 kbps | 1.471334x | 0.951877x |

## Quality metrics vs official Opus

| Bitrate | PESQ-style delta | ViSQOL-style delta | CELT delta |
|---:|---:|---:|---:|
| 16 kbps | -0.0003 | -0.0153 | 1.3037 |
| 24 kbps | -0.0071 | 0.0386 | 22.0159 |
| 32 kbps | 0.0004 | 0.0339 | 16.8861 |
| 48 kbps | 0.0002 | 0.0111 | 0.3869 |
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
| Host MinGW GCC `-O2` | 228,248 B | 0 B | 228,248 B |
| Android arm64 Clang `-O2` | 253,335 B | 800 B | 254,135 B |

## Toolchains checked

| Toolchain | Status |
|---|---|
| MinGW GCC C++23 | Builds with zero warnings in measured configuration. |
| Android arm64 Clang C++23 | Builds with zero warnings in measured configuration. |
