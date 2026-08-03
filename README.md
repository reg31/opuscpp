# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API,
derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is
designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`,
and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an
alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff:
full standards compatibility, substantially lower memory use, faster encode/decode in the current
measured configurations, and quality metrics close to upstream. The headline benchmark comparison
uses both `opuscpp` and official Opus 1.6.1 built with `-O2 -DNDEBUG`, with x86 intrinsics enabled for official Opus, because that is the practical upstream baseline for many desktop builds. The
project targets standard Opus packets. Existing code using the supported Opus API can use this
implementation without packet-format changes as long as it stays within the supported CTL subset
described in `src/README.md`. Custom Opus is intentionally unsupported.

In short: `opuscpp` is a portable C++23 alternative to official Opus for C++ users:
source-embeddable, lower-memory, standards-compatible, and faster to encode and decode in the
tracked O2 benchmark, even against an official Opus build using platform intrinsics.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

- Pure C++23 single-translation-unit codec: `src/opus_codec.cpp` + `src/opus_codec.h`.
- Standard Opus packet compatibility for encode/decode.
- Encode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current
  run (1.52x to 2.37x).
- Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current
  run (1.13x to 1.87x).
- AUDIO and VOIP quality proxy deltas are tracked separately; AUDIO stays close to official Opus
  with stronger CELT-oriented proxy scores at 24/32&nbsp;kbps in the current harness.
- Effective bitrate tracks official Opus closely in the measured set while staying slightly lower at
  several low/mid rates.
- RFC decode conformance: 24/24 RFC 8251 updated vector checks passed in the current run.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the
  official Opus decoder.
- No assembly, no SIMD intrinsics, no PGO, no LTO requirement.
- Tested with MinGW GCC and Android arm64 Clang.
- Lightweight speech/music detector moves sustained harmonic/music content toward CELT and is
  tracked by a mode-balance harness.
- Optional guarded DTX matches official Opus's zero false-DTX result on the tracked active-content
  corpus, with 4.4% lower re-entry error when sound returns after silence and 6.4% lower gain error
  (volume mismatch) in the current 16/24&nbsp;kbps comparison.
- An optional speech-oriented decoder postfilter is available through an `opuscpp` CTL; it remains
  off by default because it is not a universal music-quality improvement.
- Lower memory footprint than official Opus in the measured configurations (21.8% to 49.0%
  lower private state in the current memory snapshot).
- Host MinGW GCC `-O2` measured object image in the current snapshot: `310,540 B` total.

## Pros and cons

| Pros | Cons |
|---|---|
| Much simpler for C++ source embedding: include the header and compile one implementation file. | Not an outright replacement for every official Opus use case. |
| Encode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.52x to 2.37x). | Supports a documented subset of the full Opus CTL/API surface. |
| Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.13x to 1.87x). | A few CELT-heavy decode points are close to parity and official Opus remains extremely mature. |
| Lower encoder and decoder memory use in the measured configurations (21.8% to 49.0% lower private state in the current memory snapshot). | Official Opus remains the safer default if you need the broadest ecosystem compatibility and feature coverage. |
| Pure portable C++23, with no ASM, SIMD intrinsics, PGO, or separate library packaging required. | Quality metrics are close proxy measurements, not a substitute for listening tests or official PESQ/ViSQOL tooling. |

## Quick start

Put `src/opus_codec.h` and `src/opus_codec.cpp` in your project. Include `opus_codec.h` where you
use the API, and compile `opus_codec.cpp` as part of your normal application build.

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

No prebuilt DLL or static library is required; this repository is intended to be embedded from
source. The repository intentionally does not ship a top-level `CMakeLists.txt`; consumers are
expected to add `src/opus_codec.cpp` to their own build.

## Supported API surface

See `src/README.md` for the supported functions, constants, and CTLs. The short version:

- Encoder: create/destroy/ctl, `opus_encode`, `opus_encode_float`.
- Decoder: create/destroy/ctl, `opus_decode`, `opus_decode_float`.
- Utility: `opus_packet_get_nb_samples`, `opus_strerror`.
- CTLs: bitrate, VBR, constrained VBR, guarded DTX, complexity, reset, final range, last packet duration, plus an opt-in `opuscpp` decoder postfilter CTL.

Unsupported families include custom Opus, multistream helpers, repacketizer helpers, projection
APIs, and unsupported CTLs not listed in `src/README.md`.

## Current benchmark snapshot vs official Opus

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`. The official Opus 1.6.1
baseline is also built with `-O2 -DNDEBUG`, with x86 runtime-dispatched intrinsics enabled (`SSE`,
`SSE2`, `SSE4.1`, `AVX2`), on Windows MinGW GCC 16.1 / AMD Ryzen 7 8845HS. Encode and decode speed
are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster. The
current speed ratios are medians of three repository 60-second stereo synthetic benchmark runs;
effective-bitrate columns use the same portable O2 performance profile. Quality metrics use the
default unfiltered decoder output and are synthetic objective proxy scores, not a replacement for
official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.374x | 1.874x | +0.0059 | +0.0024 | 16.000 kbps | 16.462 kbps |
| 24&nbsp;kbps | 1.600x | 1.338x | +0.0017 | +0.0267 | 24.000 kbps | 24.476 kbps |
| 32&nbsp;kbps | 1.649x | 1.423x | +0.0207 | +0.0391 | 32.000 kbps | 32.512 kbps |
| 48&nbsp;kbps | 1.689x | 1.290x | +0.0006 | +0.0142 | 48.000 kbps | 48.402 kbps |
| 64&nbsp;kbps | 1.789x | 1.523x | +0.0010 | +0.0051 | 64.000 kbps | 64.402 kbps |
| 96&nbsp;kbps | 2.005x | 1.190x | +0.0002 | +0.0035 | 96.000 kbps | 96.404 kbps |
| 128&nbsp;kbps | 1.858x | 1.376x | +0.0010 | +0.0015 | 128.000 kbps | 128.405 kbps |
| 192&nbsp;kbps | 1.831x | 1.215x | +0.0007 | +0.0005 | 192.000 kbps | 192.416 kbps |
| 256&nbsp;kbps | 1.525x | 1.133x | +0.0008 | +0.0001 | 256.000 kbps | 256.436 kbps |


VOIP mono speech-like quality spot check:

| Bitrate | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0083 | +0.0010 | 15.999 kbps | 16.255 kbps |
| 24&nbsp;kbps | +0.0021 | +0.0025 | 24.000 kbps | 24.148 kbps |
| 32&nbsp;kbps | +0.0013 | +0.0000 | 32.000 kbps | 32.184 kbps |
| 48&nbsp;kbps | +0.0042 | +0.0017 | 48.000 kbps | 48.244 kbps |
| 64&nbsp;kbps | +0.0075 | +0.0008 | 63.969 kbps | 64.501 kbps |
| 96&nbsp;kbps | +0.0019 | +0.0002 | 96.000 kbps | 96.595 kbps |
| 128&nbsp;kbps | +0.0022 | +0.0009 | 128.000 kbps | 128.503 kbps |
| 192&nbsp;kbps | +0.0009 | +0.0001 | 192.000 kbps | 192.421 kbps |
| 256&nbsp;kbps | +0.0007 | +0.0002 | 256.000 kbps | 256.415 kbps |

The optional adaptive speech postfilter is not included in these default-output figures. It bypasses
packet modes and rates where its measured benefit did not justify the decode cost. On the tracked
mono VOIP sample, the retained 16/32&nbsp;kbps cases add about `+0.0255` PESQ-style and `+0.0017`
to `+0.0053` ViSQOL-style; it remains opt-in because mono music can still lose fidelity.

Mode-selection check at 32&nbsp;kbps mono: for the synthetic spoken-voice sample, AUDIO mode selected
CELT for 91.3% of frames and hybrid for 8.7%. For the sustained harmonic/music sample, it selected
CELT for every frame. Restricted-lowdelay also remained CELT-only, as required.

## Memory snapshot

In the published memory snapshot, `opuscpp` uses less encoder and decoder state than official Opus
in every listed mono and stereo configuration.

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,256 B | 31,856 B | -49.0% |
| Encoder stereo | 32,320 B | 49,072 B | -34.1% |
| Decoder mono | 14,128 B | 18,384 B | -23.2% |
| Decoder stereo | 21,360 B | 27,312 B | -21.8% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo RFC 8251 updated vector checks passed in the current
  run.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the
  official Opus decoder.
- API behavior validation: decoder channel-remap, packet-duration, VBR-budget, and guarded-DTX checks passed in the current
  run.
- Android arm64 Clang build: C++23 build check passed in the measured configuration.
- MinGW GCC build: C++23 build check passed in the measured configuration.

Terminology used here:

- **RFC decode conformance** means decoding the official IETF Opus test-vector bitstreams from RFC
  6716 and the RFC 8251 update set, then passing the official `opus_compare` acceptance test against
  the reference PCM.
- **Encode interoperability validation** is the encoder regression gate. Opus encoders are allowed
  to produce different valid packets, so byte-for-byte packet identity is not the right public
  claim. The test encodes generated cases with `opuscpp`, decodes the packets with official Opus
  1.6.1, and checks that the official decoder accepts the output for the supported scenarios.

The test harnesses and detailed metrics are in `tests/`.

## License

This project is derived from Opus 1.6.1 and retains the upstream Opus license text in `LICENSE`.

## Real-world use: Melo

`opuscpp` powers the voice path in Melo, an ultra-lightweight translator built for fast, natural
conversations across languages. The goal is the same as this codec: no bloat, no lag, just clear
human connection.

Available on [Android](https://play.google.com/store/apps/details?id=dands.technologies.melo),
[Windows](https://storage.googleapis.com/dnstech-release/executables/Melo.exe),
[iOS](https://apps.apple.com/us/app/melo/id1037721120), and
[macOS](https://apps.apple.com/us/app/melo/id1037721124?mt=12).
