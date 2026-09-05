# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API,
derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is
designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`,
and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an
alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff:
standard Opus compatibility, substantially lower measured memory use, faster encode/decode in the current
measured configurations, and content-dependent quality trade-offs. The headline benchmark comparison
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

- Portable C++23 source embedding: `src/opus_codec.cpp` + `src/opus_codec.h`; no separate DLL or static library.
- Standard Opus packets and the documented single-stream API/CTL subset.
- Encode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates (1.74x to 2.38x).
- Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates (1.24x to 1.87x).
- Quality is mixed: the aligned AUDIO sample improves both main proxies at 24/32&nbsp;kbps, but other rates/content lose metrics. No universal quality advantage is claimed.
- Effective bitrate, optional processing, FEC/DTX, memory and speed below were refreshed on 2026-09-05.
- Updated RFC decode vectors: 24/24 passed; encode interoperability: 96/96 passed.
- API, FEC, DTX, long-frame and 290,909 malformed-packet checks passed; trapping UBSan found no issue in the exercised cases.
- Optional DTX: zero false DTX packets on the tracked active-content set, 61.4% lower re-entry error and 52.2% lower gain error at 16/24&nbsp;kbps.
- Optional FEC: lower missing-frame error in all 18 tracked loss scenarios; 52.8% lower combined recovery error, protection in 18 scenarios versus 15, and 0.4% fewer bytes.
- 22.7% to 46.7% lower measured private allocation footprint across the listed encoder/decoder configurations.
- Host object: `299,536 B`; Android arm64 object: `312,436 B` (text + data + BSS).
- No assembly, SIMD intrinsics, PGO or LTO requirement; warning-free MinGW GCC and Android arm64 Clang builds.

## Pros and cons

| Pros | Cons |
|---|---|
| Source embedding: include the header and compile one implementation file. | An alternative, not a replacement for every official Opus use case. |
| Faster encode in 9/9 measured bitrates (1.74x to 2.38x). | Supports a documented subset of the full Opus CTL/API surface. |
| Faster decode in 9/9 measured bitrates (1.24x to 1.87x). | Results describe this machine and workload, not every platform or packet mix. |
| 22.7% to 46.7% lower measured private allocation footprint. | Official Opus supports a broader feature surface and ecosystem. |
| Pure portable C++23, without ASM or SIMD intrinsics. | Aligned quality proxies show both gains and losses; optional filtering is not a universal improvement. |

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
const int bytes = opus_encode(enc, std::span{pcm_i16}, std::span{packet});

opus_encoder_destroy(enc);
```

No prebuilt DLL or static library is required; this repository is intended to be embedded from
source. The repository intentionally does not ship a top-level `CMakeLists.txt`; consumers are
expected to add `src/opus_codec.cpp` to their own build.

## Supported API surface

See `src/README.md` for the supported functions, constants, and CTLs. The short version:

- Encoder: create/destroy/ctl, `opus_encode`, `opus_encode_float`, and a zero-copy C++23 `std::span` overload for `opus_encode`.
- Decoder: create/destroy/ctl, `opus_decode`, `opus_decode_float`, and a zero-copy C++23 `std::span` overload for `opus_decode`.
- Utility: `opus_packet_get_nb_samples`, `opus_strerror`.
- CTLs: bitrate, VBR, constrained VBR, in-band FEC, expected packet loss, guarded DTX, complexity, reset, final range, last packet duration, plus `OPUSCPP_SET_VOICE_DENOISE(x)` and `OPUSCPP_SET_DECODE_POSTFILTER(x)`, see the [measured speech-processing quality and cost](https://github.com/reg31/opuscpp/tree/main/tests#optional-speech-denoiser).

Unsupported families include custom Opus, multistream helpers, repacketizer helpers, projection
APIs, and unsupported CTLs not listed in `src/README.md`.

## Published benchmark snapshot vs official Opus

Measured on 2026-09-05 from codec revision `34a5b76`. All numerical tables and claims below use
this fresh run; [run metadata](tests/metrics/run_metadata.json) records source hashes and settings.
Quality scoring removes each encoder's delay, flushes the tail, and scores stereo channels
independently. These replace the historical unaligned scores. The codec itself was not changed by
this refresh, so a difference from an old score is not by itself a new codec regression.

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`. The official Opus 1.6.1
baseline is also built with `-O2 -DNDEBUG`, with x86 runtime-dispatched intrinsics enabled (`SSE`,
`SSE2`, `SSE4.1`, `AVX2`), on Windows MinGW GCC 16.2 / AMD Ryzen 7 8845HS. Encode and decode speed
are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster. The
published speed ratios are medians of nine repository 60-second stereo synthetic benchmark runs.
Quality and effective-bitrate columns come from the same validation run using the default unfiltered
decoder output; the quality values are synthetic objective proxy scores, not a replacement for
official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 2.384x | 1.865x | +0.0006 | -0.0032 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | 1.882x | 1.411x | +0.1931 | +0.0753 | 24.000 kbps | 25.220 kbps |
| 32&nbsp;kbps | 1.823x | 1.396x | +0.2572 | +0.0791 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | 1.735x | 1.348x | -0.0237 | +0.0028 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | 1.757x | 1.313x | -0.1168 | -0.0042 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | 1.843x | 1.300x | -0.0475 | +0.0058 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | 2.040x | 1.311x | -0.0037 | -0.0021 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | 1.853x | 1.299x | +0.0067 | +0.0003 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | 1.761x | 1.237x | +0.0244 | -0.0011 | 256.000 kbps | 256.736 kbps |


VOIP mono speech-like quality spot check:

| Bitrate | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1240 | -0.0002 | 15.999 kbps | 16.255 kbps |
| 24&nbsp;kbps | +0.1472 | -0.0038 | 24.000 kbps | 24.148 kbps |
| 32&nbsp;kbps | +0.1464 | -0.0026 | 32.000 kbps | 32.184 kbps |
| 48&nbsp;kbps | +0.1446 | -0.0012 | 48.000 kbps | 48.244 kbps |
| 64&nbsp;kbps | +0.1378 | -0.0050 | 63.988 kbps | 64.501 kbps |
| 96&nbsp;kbps | +0.0000 | -0.0003 | 96.000 kbps | 96.595 kbps |
| 128&nbsp;kbps | +0.0024 | -0.0040 | 128.000 kbps | 128.503 kbps |
| 192&nbsp;kbps | +0.0005 | -0.0009 | 192.000 kbps | 192.421 kbps |
| 256&nbsp;kbps | +0.0012 | -0.0001 | 256.000 kbps | 256.415 kbps |

The optional adaptive speech postfilter is excluded from the default-output figures. On the tracked
mono VOIP sample, mode `3` adds `+0.1380` PESQ-style at 32&nbsp;kbps, but
ViSQOL-style changes by `-0.0114` and the CELT proxy also decreases. It is
a smoothing trade-off, not an automatic quality improvement. Its PCM16 path adds 3.2% to
9.8% end-to-end decode time across the tracked ladder and decodes 60 seconds in
0.046 to 0.075 seconds on this system. Keep it off unless that smoothing is preferred;
see the [optional-processing results](tests/README.md#optional-speech-postfilter).

Mode-selection check at 32&nbsp;kbps mono: for the synthetic spoken-voice sample, AUDIO mode selected
CELT for 95.7% of frames and hybrid for 4.3%. For the sustained harmonic/music sample, it selected
CELT for every frame. Restricted-lowdelay also remained CELT-only, as required.

## Memory snapshot

In this run, `opuscpp` uses less encoder and decoder state than official Opus
in every listed mono and stereo configuration. The optional encoder FEC state is allocated lazily
and is not included in this default-FEC-off, denoiser-off snapshot. These are process-private
allocation deltas averaged over 256 instances, not exact `sizeof` values; allocator/page rounding
can slightly change the measurement.

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 16,928 B | 31,744 B | -46.7% |
| Encoder stereo | 32,192 B | 49,072 B | -34.4% |
| Decoder mono | 14,064 B | 18,400 B | -23.6% |
| Decoder stereo | 21,200 B | 27,408 B | -22.7% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo RFC 8251 updated vector checks passed in this run.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the
  official Opus decoder.
- API behavior validation: decoder channel-remap, packet/frame-duration rejection, encoder-lookahead, VBR-budget, and guarded-DTX checks passed
  in the current run.
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
