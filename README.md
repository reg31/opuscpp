# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API, derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`, and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff: full standards compatibility, substantially lower memory use, faster encoding than official Opus in our measured configurations, and quality metrics close to upstream. `opuscpp` is generally faster to encode than official Opus in the measured configurations, while decode performance is competitive with the portable official build (`0.91x` to `1.21x` in the published benchmark set) and close to the optimized x86-intrinsics build. The project targets standard Opus packets. Existing code using the supported Opus API can use this implementation without packet-format changes as long as it stays within the supported CTL subset described in `src/README.md`. Custom Opus is intentionally unsupported.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

- Pure C++23 single-translation-unit codec: `src/opus_codec.cpp` + `src/opus_codec.h`.
- Standard Opus packet compatibility for encode/decode.
- Faster encoding than official Opus in the published benchmark set (`1.17x` to `1.50x` at complexity 10).
- Competitive decode speed versus official portable Release (`0.91x` to `1.21x` in the current table).
- Quality proxy deltas stay close to official Opus, with stronger CELT-oriented proxy scores at 24/32 kbps in the current harness.
- Packet sizes are close to or smaller than official Opus in the measured set (`-2.5%` to `+0.0%`).
- RFC decode conformance: 24/24 RFC 6716/RFC 8251 vector checks passed.
- Encode oracle validation: 96/96 encode regression cases passed against the official Opus reference path.
- No assembly, no SIMD intrinsics, no PGO, no LTO requirement.
- Tested with MinGW GCC and Android arm64 Clang.
- Lightweight speech/music detector moves sustained harmonic/music content toward CELT and is tracked by a mode-balance harness.
- Lower memory footprint than official Opus in the measured configurations (`-22.0%` to `-45.7%` in the current memory snapshot).

## Pros and cons

| Pros | Cons |
|---|---|
| Much simpler for C++ source embedding: include the header and compile one implementation file. | Not an outright replacement for every official Opus use case. |
| Faster encoding in the published benchmark set (`1.17x` to `1.50x` at complexity 10). | Supports a documented subset of the full Opus CTL/API surface. |
| Decode is competitive with official portable Release (`0.91x` to `1.21x`) and sometimes faster at low/mid rates. | Decode is not uniformly faster, especially on high-rate CELT-heavy packets. |
| Lower encoder and decoder memory use in the measured configurations (`-22.0%` to `-45.7%` in the current memory snapshot). | Official Opus remains the more mature default if you need the broadest ecosystem compatibility and feature coverage. |
| Pure portable C++23, with no ASM, SIMD intrinsics, PGO, or separate library packaging required. | Quality metrics are close proxy measurements, not a substitute for listening tests or official PESQ/ViSQOL tooling. |

## Quick start

Put `src/opus_codec.h` and `src/opus_codec.cpp` in your project. Include `opus_codec.h` where you use the API, and compile `opus_codec.cpp` as part of your normal application build.

Use the normal supported Opus-style API:

```cpp
#include "opus_codec.h"

int err = OPUS_OK;
OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000));

opus_int16 pcm_i16[960 * 2] = {};
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

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`, with selected GCC function-level attributes inside the source for hot integer paths (`O3`) and cold/size-sensitive paths (`Os`). This is intentional: users do not need to compile the whole translation unit at global `-O3` to get the measured source-level tuning. The comparison target is official Opus 1.6.1 built in Release mode (`-O3 -DNDEBUG`) with intrinsics disabled, with the public encoder complexity set to 10. Encode and decode speed are multiplicative ratios versus official Opus; values above `1.00x` mean this implementation is faster. Quality metrics are synthetic objective proxy scores from the validation harness, not a replacement for official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed | Decode speed | PESQ-style delta | ViSQOL-style delta | Packet bytes vs official |
|---:|---:|---:|---:|---:|---:|
| 16 kbps | 1.36x | 1.21x | -0.0003 | -0.0153 | -2.1% |
| 24 kbps | 1.36x | 0.99x | -0.0071 | +0.0386 | -2.2% |
| 32 kbps | 1.40x | 0.98x | +0.0004 | +0.0339 | -2.5% |
| 48 kbps | 1.17x | 1.03x | +0.0002 | +0.0111 | +0.0% |
| 64 kbps | 1.49x | 0.98x | +0.0011 | -0.0028 | +0.0% |
| 96 kbps | 1.38x | 0.92x | -0.0004 | -0.0011 | -0.2% |
| 128 kbps | 1.40x | 0.91x | +0.0005 | +0.0009 | -0.2% |
| 192 kbps | 1.50x | 0.94x | +0.0001 | -0.0009 | -0.2% |
| 256 kbps | 1.46x | 0.96x | +0.0001 | -0.0017 | -0.2% |

Detector validation on representative material: at 32 kbps mono, the current AUDIO policy routes speech-like synthetic material mostly to CELT and sustained harmonic/music material entirely to CELT; restricted-lowdelay remains CELT-only as expected.

## Memory snapshot

In the published memory snapshot, `opuscpp` uses less encoder and decoder state than official Opus in every listed mono and stereo configuration.

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 17,184 B | 31,648 B | -45.7% |
| Encoder stereo | 32,448 B | 48,880 B | -33.6% |
| Decoder mono | 14,192 B | 18,272 B | -22.3% |
| Decoder stereo | 21,376 B | 27,408 B | -22.0% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo RFC 6716/RFC 8251 vector checks passed.
- Encode oracle validation: 96/96 encode regression cases passed against the official Opus reference path.
- Android arm64 Clang build: zero warnings in the measured configuration.
- MinGW GCC build: zero warnings in the measured configuration.

Terminology used here:

- **RFC decode conformance** means decoding the official IETF Opus test-vector bitstreams from RFC 6716 and the RFC 8251 update set, then passing the official `opus_compare` acceptance test against the reference PCM.
- **Encode oracle validation** is not an IETF term. Opus encoders are allowed to produce different valid packets, so this project checks encoding by comparing `opuscpp` against an official Opus 1.6.1 oracle path on the same generated inputs and CTL settings: encode, decode with official Opus, then compare the decoded audio with the oracle output.

The test harnesses and detailed metrics are in `tests/`.

## License

This project is derived from Opus 1.6.1 and retains the upstream Opus license text in `LICENSE`.

## Real-world use: Melo

`opuscpp` powers the voice path in Melo, an ultra-lightweight translator built for fast, natural conversations across languages. The goal is the same as this codec: no bloat, no lag, just clear human connection.

Available on [Android](https://play.google.com/store/apps/details?id=dands.technologies.melo), [Windows](https://storage.googleapis.com/dnstech-release/executables/Melo.exe), [iOS](https://apps.apple.com/us/app/melo/id1037721120), and [macOS](https://apps.apple.com/us/app/melo/id1037721124?mt=12).
