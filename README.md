# opuscpp

`opuscpp` is a pure portable C++23 implementation of the Opus codec API, derived from Opus 1.6.1. It is designed for source embedding: add the source file to your build, include the header, and ship no separate DLL or static library.

The project targets standard Opus packets. Existing code that uses the supported Opus API can use this implementation without packet-format changes. Custom Opus is intentionally unsupported.

## Highlights

- Pure C++23 single-translation-unit codec: `src/opus_codec.cpp` + `src/opus_codec.h`.
- Standard Opus packet compatibility for encode/decode.
- RFC decode conformance: 24/24 vectors passed.
- Encode oracle conformance: 96/96 cases passed.
- No assembly, no SIMD intrinsics, no PGO, no LTO requirement.
- Tested with MinGW GCC and Android arm64 Clang.
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

## Supported API surface

See `src/README.md` for the supported functions, constants, and CTLs. The short version:

- Encoder: create/destroy/ctl, `opus_encode`, `opus_encode_float`.
- Decoder: create/destroy/ctl, `opus_decode`, `opus_decode_float`.
- Utility: `opus_packet_get_nb_samples`, `opus_strerror`.
- CTLs: bitrate, VBR, constrained VBR, complexity, reset, final range, last packet duration.

Unsupported families include custom Opus, multistream helpers, repacketizer helpers, projection APIs, and unsupported CTLs not listed in `src/README.md`.

## Current benchmark snapshot vs official Opus

Measurements below are from a matched `-O2` official Opus build with intrinsics disabled. Positive decode values mean this implementation decoded faster; encode speed is shown as a multiplicative speedup over official Opus. Quality metrics are synthetic objective scores from the validation harness, not a replacement for listening tests.

| Bitrate | Encode speed | Decode vs official | PESQ-style delta | ViSQOL-style delta | Packet bytes vs official |
|---:|---:|---:|---:|---:|---:|
| 16 kbps | 2.40x | -1.5% | +0.0466 | -0.0349 | +0.9% |
| 24 kbps | 3.73x | -9.0% | +0.0302 | -0.0135 | +5.6% |
| 32 kbps | 4.87x | -5.4% | +0.0228 | +0.0041 | +7.0% |
| 48 kbps | 3.40x | -8.0% | +0.0176 | +0.0000 | +3.2% |
| 96 kbps | 2.10x | +24.6% | +0.0010 | -0.0012 | +0.0% |
| 128 kbps | 2.01x | +24.5% | +0.0009 | -0.0005 | +0.0% |
| 192 kbps | 2.14x | +12.4% | +0.0008 | +0.0000 | -0.0% |
| 256 kbps | 2.03x | -5.5% | +0.0009 | +0.0001 | +0.0% |

Interpretation: encode is consistently much faster in this benchmark. Decode is parity-class overall but 24/32/48 kbps remain the known tuning focus because mode balance differs from official Opus on some content.

## Memory snapshot

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,848 B | 31,648 B | -46.8% |
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
