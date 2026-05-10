# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API, derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`, and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff: full standards compatibility, substantially lower memory use, faster encoding than official Opus in the current measured configurations, and quality metrics close to upstream. The headline benchmark comparison uses official Opus 1.6.1 built with `-O2 -DNDEBUG` and x86 intrinsics enabled, because that is the practical baseline for many desktop builds. The project targets standard Opus packets. Existing code using the supported Opus API can use this implementation without packet-format changes as long as it stays within the supported CTL subset described in `src/README.md`. Custom Opus is intentionally unsupported.

In short: `opuscpp` is a portable C++23 alternative to official Opus for C++ users: source-embeddable, lower-memory, standards-compatible, and faster to encode and decode in the tracked benchmark, even against an official Opus build using platform intrinsics.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

- Pure C++23 single-translation-unit codec: `src/opus_codec.cpp` + `src/opus_codec.h`.
- Standard Opus packet compatibility for encode/decode.
- Encode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.413x to 1.811x).
- Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.031x to 1.490x).
- Quality proxy deltas stay close to official Opus, with stronger CELT-oriented proxy scores at 24/32&nbsp;kbps in the current harness.
- Effective bitrate tracks official Opus closely in the measured set while staying slightly lower at several low/mid rates.
- RFC decode conformance: 24/24 RFC 6716/RFC 8251 vector checks passed.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the official Opus decoder.
- No assembly, no SIMD intrinsics, no PGO, no LTO requirement.
- Tested with MinGW GCC and Android arm64 Clang.
- Lightweight speech/music detector moves sustained harmonic/music content toward CELT and is tracked by a mode-balance harness.
- Lower memory footprint than official Opus in the measured configurations (`-21.9%` to `-49.0%` private state in the current memory snapshot).
- Host MinGW GCC `-O2` binary text in the current measured snapshot: `264,168 B`.

## Pros and cons

| Pros | Cons |
|---|---|
| Much simpler for C++ source embedding: include the header and compile one implementation file. | Not an outright replacement for every official Opus use case. |
| Encode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.413x to 1.811x). | Supports a documented subset of the full Opus CTL/API surface. |
| Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.031x to 1.490x). | A few high-rate decode points are close to parity and official Opus remains extremely mature. |
| Lower encoder and decoder memory use in the measured configurations (`-21.9%` to `-49.0%` private state in the current memory snapshot). | Official Opus remains the safer default if you need the broadest ecosystem compatibility and feature coverage. |
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

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`. The official Opus 1.6.1 baseline is also built with `-O2 -DNDEBUG`, with x86 runtime-dispatched intrinsics enabled (`SSE`, `SSE2`, `SSE4.1`, `AVX2`), on Windows MinGW GCC 16.1 / AMD Ryzen 7 8845HS. Encode and decode speed are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster. The current speed ratios and effective-bitrate columns come from the repository's 8-second stereo synthetic benchmark; quality metrics are synthetic objective proxy scores from the validation harness, not a replacement for official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.651x | 1.490x | +0.0000 | -0.0185 | 16.5 kbps | 16.9 kbps |
| 24&nbsp;kbps | 1.724x | 1.127x | -0.0072 | +0.0411 | 24.5 kbps | 25.0 kbps |
| 32&nbsp;kbps | 1.811x | 1.142x | +0.0007 | +0.0351 | 32.5 kbps | 33.3 kbps |
| 48&nbsp;kbps | 1.424x | 1.118x | +0.0010 | +0.0131 | 48.5 kbps | 48.5 kbps |
| 64&nbsp;kbps | 1.531x | 1.079x | +0.0010 | +0.0057 | 64.6 kbps | 64.6 kbps |
| 96&nbsp;kbps | 1.586x | 1.044x | +0.0002 | +0.0015 | 96.4 kbps | 96.6 kbps |
| 128&nbsp;kbps | 1.522x | 1.031x | +0.0006 | +0.0022 | 128.4 kbps | 128.7 kbps |
| 192&nbsp;kbps | 1.451x | 1.040x | +0.0003 | -0.0001 | 192.4 kbps | 192.9 kbps |
| 256&nbsp;kbps | 1.413x | 1.049x | +0.0005 | -0.0005 | 256.4 kbps | 256.8 kbps |

Detector validation on representative material: at 32&nbsp;kbps mono, the current AUDIO policy routes speech-like synthetic material mostly to CELT and sustained harmonic/music material entirely to CELT; restricted-lowdelay remains CELT-only as expected.

## Memory snapshot

In the published memory snapshot, `opuscpp` uses less encoder and decoder state than official Opus in every listed mono and stereo configuration.

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,224 B | 31,824 B | -49.0% |
| Encoder stereo | 32,448 B | 48,944 B | -33.7% |
| Decoder mono | 14,192 B | 18,352 B | -22.7% |
| Decoder stereo | 21,344 B | 27,312 B | -21.9% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo RFC 6716/RFC 8251 vector checks passed.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the official Opus decoder.
- Android arm64 Clang build: C++23 build check passed in the measured configuration.
- MinGW GCC build: C++23 build check passed in the measured configuration.

Terminology used here:

- **RFC decode conformance** means decoding the official IETF Opus test-vector bitstreams from RFC 6716 and the RFC 8251 update set, then passing the official `opus_compare` acceptance test against the reference PCM.
- **Encode interoperability validation** is the encoder regression gate. Opus encoders are allowed to produce different valid packets, so byte-for-byte packet identity is not the right public claim. The test encodes generated cases with `opuscpp`, decodes the packets with official Opus 1.6.1, and checks that the official decoder accepts the output for the supported scenarios.

The test harnesses and detailed metrics are in `tests/`.

## License

This project is derived from Opus 1.6.1 and retains the upstream Opus license text in `LICENSE`.

## Real-world use: Melo

`opuscpp` powers the voice path in Melo, an ultra-lightweight translator built for fast, natural conversations across languages. The goal is the same as this codec: no bloat, no lag, just clear human connection.

Available on [Android](https://play.google.com/store/apps/details?id=dands.technologies.melo), [Windows](https://storage.googleapis.com/dnstech-release/executables/Melo.exe), [iOS](https://apps.apple.com/us/app/melo/id1037721120), and [macOS](https://apps.apple.com/us/app/melo/id1037721124?mt=12).
