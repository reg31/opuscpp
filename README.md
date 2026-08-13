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
  run (1.78x to 2.51x).
- Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current
  run (1.22x to 1.88x).
- AUDIO and VOIP PESQ-style, ViSQOL-style, and CELT-style proxy deltas are positive at all nine
  measured bitrates.
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
  corpus, with 61.4% lower re-entry error when sound returns after silence and 52.2% lower gain error
  (volume mismatch) in the current 16/24&nbsp;kbps comparison.
- Optional in-band FEC interoperates in both directions with official Opus. Across 18 tracked
  nominal, quiet, and noisy loss scenarios, its recovered audio is closer to the original every
  time. Average missing-frame reconstruction error is 52.8% lower, protection is present in all 18
  scenarios instead of 15 for official Opus, and packets are 0.4% smaller.
- An optional speech-oriented decoder postfilter is available through an `opuscpp` CTL; it remains
  off by default because it is not a universal music-quality improvement.
- Lower memory footprint than official Opus in the measured configurations (21.9% to 46.5%
  lower private state in the current memory snapshot).
- Host MinGW GCC `-O2` measured object image in the current snapshot: `288,788 B` total.

## Pros and cons

| Pros | Cons |
|---|---|
| Much simpler for C++ source embedding: include the header and compile one implementation file. | Not an outright replacement for every official Opus use case. |
| Encode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.78x to 2.51x). | Supports a documented subset of the full Opus CTL/API surface. |
| Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates in the current run (1.22x to 1.88x). | Official Opus remains extremely mature and supports a broader feature surface. |
| Lower encoder and decoder memory use in the measured configurations (21.9% to 46.5% lower private state in the current memory snapshot). | Official Opus remains the safer default if you need the broadest ecosystem compatibility and feature coverage. |
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
- CTLs: bitrate, VBR, constrained VBR, in-band FEC, expected packet loss, guarded DTX, complexity, reset, final range, last packet duration, plus an opt-in `opuscpp` decoder postfilter CTL.

Unsupported families include custom Opus, multistream helpers, repacketizer helpers, projection
APIs, and unsupported CTLs not listed in `src/README.md`.

## Current benchmark snapshot vs official Opus

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`. The official Opus 1.6.1
baseline is also built with `-O2 -DNDEBUG`, with x86 runtime-dispatched intrinsics enabled (`SSE`,
`SSE2`, `SSE4.1`, `AVX2`), on Windows MinGW GCC 16.2 / AMD Ryzen 7 8845HS. Encode and decode speed
are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster. The
current speed ratios are medians of nine repository 60-second stereo synthetic benchmark runs.
Quality and effective-bitrate columns come from the same validation run using the default unfiltered
decoder output; the quality values are synthetic objective proxy scores, not a replacement for
official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.514x | 1.884x | +0.0059 | +0.0024 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | 1.907x | 1.348x | +0.0148 | +0.0332 | 24.000 kbps | 25.220 kbps |
| 32&nbsp;kbps | 1.865x | 1.367x | +0.0211 | +0.0404 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | 1.797x | 1.296x | +0.0011 | +0.0148 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | 1.784x | 1.274x | +0.0075 | +0.0055 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | 1.867x | 1.270x | +0.0002 | +0.0035 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | 2.081x | 1.259x | +0.0010 | +0.0015 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | 1.858x | 1.249x | +0.0007 | +0.0005 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | 1.777x | 1.221x | +0.0008 | +0.0001 | 256.000 kbps | 256.736 kbps |


VOIP mono speech-like quality spot check:

| Bitrate | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.0083 | +0.0010 | 15.999 kbps | 16.255 kbps |
| 24&nbsp;kbps | +0.0097 | +0.0057 | 24.000 kbps | 24.148 kbps |
| 32&nbsp;kbps | +0.0092 | +0.0069 | 32.000 kbps | 32.184 kbps |
| 48&nbsp;kbps | +0.0123 | +0.0073 | 48.000 kbps | 48.244 kbps |
| 64&nbsp;kbps | +0.0128 | +0.0052 | 63.988 kbps | 64.501 kbps |
| 96&nbsp;kbps | +0.0019 | +0.0002 | 96.000 kbps | 96.595 kbps |
| 128&nbsp;kbps | +0.0022 | +0.0009 | 128.000 kbps | 128.503 kbps |
| 192&nbsp;kbps | +0.0009 | +0.0001 | 192.000 kbps | 192.421 kbps |
| 256&nbsp;kbps | +0.0007 | +0.0002 | 256.000 kbps | 256.415 kbps |

The optional adaptive speech postfilter is not included in these default-output figures. On the
tracked mono VOIP sample, mode `3` adds `+0.0249` PESQ-style and `+0.0017` ViSQOL-style at
32&nbsp;kbps, and `+0.0137` to `+0.0138` PESQ-style at 96-256&nbsp;kbps while retaining positive
ViSQOL-style deltas. Its current PCM16 path adds effectively 0% to 7.6% end-to-end decode time across
the tracked ladder and decodes 60 seconds in 0.051 to 0.080 seconds on the measured system. It remains
opt-in because mono music can still lose fidelity.

Mode-selection check at 32&nbsp;kbps mono: for the synthetic spoken-voice sample, AUDIO mode selected
CELT for 95.7% of frames and hybrid for 4.3%. For the sustained harmonic/music sample, it selected
CELT for every frame. Restricted-lowdelay also remained CELT-only, as required.

## Memory snapshot

In the published memory snapshot, `opuscpp` uses less encoder and decoder state than official Opus
in every listed mono and stereo configuration. The optional encoder FEC state is allocated lazily
and is not included in this default-FEC-off snapshot.

| State | opuscpp | official Opus | Difference |
|---|---:|---:|---:|
| Encoder mono | 16,928 B | 31,648 B | -46.5% |
| Encoder stereo | 32,176 B | 48,912 B | -34.2% |
| Decoder mono | 14,128 B | 18,384 B | -23.2% |
| Decoder stereo | 21,360 B | 27,344 B | -21.9% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo RFC 8251 updated vector checks passed in the current
  run.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the
  official Opus decoder.
- API behavior validation: decoder channel-remap, packet/frame-duration rejection, VBR-budget, and guarded-DTX checks passed in the current
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
