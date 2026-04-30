# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API, derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`, and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff: full standards compatibility, substantially lower memory use, and about 2x faster encoding than official Opus in our measured configurations, while keeping decode and quality metrics close to upstream. The project targets standard Opus packets. Existing code using the supported Opus API can use this implementation without packet-format changes as long as it stays within the supported CTL subset described in `src/README.md`. Custom Opus is intentionally unsupported.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

- Pure C++23 single-translation-unit codec: `src/opus_codec.cpp` + `src/opus_codec.h`.
- Standard Opus packet compatibility for encode/decode.
- About 2x faster encoding than official Opus in the published benchmark set.
- RFC decode conformance: 24/24 vectors passed.
- Encode oracle conformance: 96/96 cases passed.
- No assembly, no SIMD intrinsics, no PGO, no LTO requirement.
- Tested with MinGW GCC and Android arm64 Clang.
- Lightweight speech/music detector keeps speech-biased content in hybrid while moving sustained harmonic/music content toward CELT.
- Lower memory footprint than official Opus in the measured configurations.

## Quick start

Copy or vendor the `src` directory, then add the implementation file to your target:

```bash
c++ -std=c++23 -O2 -I path/to/opuscpp/src \
    your_app.cpp path/to/opuscpp/src/opus_codec.cpp \
    -o your_app
```

Use the normal supported Opus-style API:

```cpp
#include "opus_codec.h"

int err = OPUS_OK;
OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));
opus_encoder_ctl(enc, OPUS_SET_VBR(1));

unsigned char packet[1500];
int bytes = opus_encode(enc, pcm_i16, 960, packet, sizeof(packet));

opus_encoder_destroy(enc);
```

No prebuilt DLL or static library is required; this repository is intended to be embedded from source.
The repository intentionally does not ship a top-level `CMakeLists.txt`; consumers are expected to add `src/opus_codec.cpp` to their own build.

## Supported API surface

See `src/README.md` for the supported functions, constants, and CTLs. The short version:

- Encoder: create/destroy/ctl, `opus_encode`, `opus_encode_float`.
- Decoder: create/destroy/ctl, `opus_decode`, `opus_decode_float`.
- Utility: `opus_packet_get_nb_samples`, `opus_strerror`.
- CTLs: bitrate, VBR, constrained VBR, complexity, reset, final range, last packet duration.

Unsupported families include custom Opus, multistream helpers, repacketizer helpers, projection APIs, and unsupported CTLs not listed in `src/README.md`.

## Current benchmark snapshot vs official Opus

Measurements below are from a matched `-O2` official Opus build with intrinsics disabled. Positive decode values mean this implementation decoded faster; encode speed is shown as a multiplicative speedup over official Opus. Quality metrics are synthetic objective proxy scores from the validation harness, not a replacement for official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed | Decode vs official | PESQ-style delta | ViSQOL-style delta | Packet bytes vs official |
|---:|---:|---:|---:|---:|---:|
| 16 kbps | 1.39x | +36.8% | +0.0207 | -0.0040 | +2.0% |
| 24 kbps | 1.79x | +17.7% | +0.0139 | +0.0030 | +0.8% |
| 32 kbps | 1.78x | +8.6% | +0.0087 | +0.0081 | +1.9% |
| 48 kbps | 1.64x | +7.2% | +0.0065 | +0.0007 | +1.7% |
| 64 kbps | 2.02x | +7.9% | -0.0005 | +0.0002 | +0.0% |
| 96 kbps | 2.29x | +6.9% | -0.0001 | +0.0000 | +0.0% |
| 128 kbps | 2.06x | +11.1% | -0.0002 | +0.0011 | +0.0% |
| 192 kbps | 2.40x | +3.8% | -0.0002 | -0.0004 | +0.0% |
| 256 kbps | 2.32x | +7.5% | +0.0002 | +0.0002 | +0.0% |

Detector validation on representative material: speech-like content stays about 99% hybrid, sustained harmonic/music material moves mostly to CELT, and restricted-lowdelay remains CELT-only as expected.

## Memory snapshot

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,864 B | 31,648 B | -46.7% |
| Encoder stereo | 32,448 B | 48,880 B | -33.6% |
| Decoder mono | 14,112 B | 18,336 B | -23.0% |
| Decoder stereo | 21,376 B | 27,408 B | -22.0% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo vectors passed.
- Encode oracle conformance: 96/96 validation cases passed.
- Android arm64 Clang build: zero warnings in the measured configuration.
- MinGW GCC build: zero warnings in the measured configuration.

The test harnesses and detailed metrics are in `tests/`.

## License

This project is derived from Opus 1.6.1 and retains the upstream Opus license text in `LICENSE`.
