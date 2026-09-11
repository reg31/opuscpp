# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API,
derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is
designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`,
and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an
alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff:
standard Opus compatibility, faster measured decoding, and content-dependent quality trade-offs. Memory figures use the current tracked snapshot. The headline benchmark comparison
uses both `opuscpp` and official Opus built from the upstream `main` branch (commit `503d81b`) with `-O2 -DNDEBUG`, with x86 intrinsics enabled for official Opus, because that is the practical upstream baseline for many desktop builds. The
project targets standard Opus packets. Existing code using the supported Opus API can use this
implementation without packet-format changes as long as it stays within the supported CTL subset
described in `src/README.md`. Custom Opus is intentionally unsupported.

In short: `opuscpp` is a portable C++23 alternative to official Opus for C++ users:
source-embeddable, standards-compatible, and faster to both encode and decode in the tracked O2 benchmark against an official build using platform intrinsics. At complexity 10, encoding is 1.23x-1.70x faster than official across the tracked bitrates.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

Quality and speed comparisons below use the current complexity-10 encoder directly against official Opus. Some spectral and speech-proxy losses remain. Memory figures use the current tracked snapshot.

- Portable C++23 source embedding: `src/opus_codec.cpp` + `src/opus_codec.h`; no separate DLL or static library.
- Standard Opus packets and the documented single-stream API/CTL subset.
- Complexity-10 encoding is faster than official Opus at 9/9 measured bitrates (1.23x to 1.70x).
- Decode is faster than official Opus with x86 intrinsics in 9/9 measured bitrates (1.20x to 1.80x).
- Transient detection runs for every fullband CELT frame (matching official Opus gating), improving percussive content.
- Quality is mixed: AUDIO improves the PESQ-style proxy in 9/9 and the ViSQOL-style proxy in 8/9 tracked bitrates, but every AUDIO and VOIP row still loses at least one measured spectral or speech-proxy field. No universal quality advantage is claimed.
- Effective bitrate, optional processing, FEC/DTX, memory and speed have dedicated benchmark coverage.
- Updated RFC decode vectors: 24/24 passed; encode interoperability: 96/96 passed.
- API, FEC, DTX, long-frame and 290,909 malformed-packet checks passed; trapping UBSan found no issue in the exercised cases.
- Optional DTX: zero false DTX packets on the tracked active-content set, 61.4% lower re-entry error and 52.2% lower gain error at 16/24&nbsp;kbps.
- Optional FEC: lower missing-frame error in all 18 tracked loss scenarios; 52.8% lower combined recovery error, protection in 18 scenarios versus 15, and 0.4% fewer bytes.
- 22.5% to 46.6% lower measured private allocation footprint across the listed encoder/decoder configurations.
- Host object: `310,160 B` (text + data + BSS).
- No assembly, SIMD intrinsics, PGO or LTO requirement; warning-free MinGW GCC and Android arm64 Clang builds.

## Pros and cons

| Pros | Cons |
|---|---|
| Source embedding: include the header and compile one implementation file. | An alternative, not a replacement for every official Opus use case. |
| Encoding is 1.23x-1.70x of official Opus speed in the measured workload. | Results describe this machine and workload, not every platform or packet mix. |
| Faster decode in 9/9 measured bitrates (1.20x to 1.80x). | Official Opus supports a broader feature surface and ecosystem. |
| 22.5% to 46.6% lower measured private allocation footprint. | Aligned quality proxies show both gains and losses; optional filtering is not a universal improvement. |
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
- CTLs: bitrate, VBR, constrained VBR, in-band FEC, expected packet loss, guarded DTX, complexity, reset, final range, last packet duration, plus `OPUSCPP_SET_VOICE_DENOISE(x)`, see the [measured speech-processing quality and cost](https://github.com/reg31/opuscpp/tree/main/tests#optional-speech-denoiser).

Unsupported families include custom Opus, multistream helpers, repacketizer helpers, projection
APIs, and unsupported CTLs not listed in `src/README.md`.

## Published benchmark snapshot vs official Opus

Quality and effective-bitrate columns use the current encoder at complexity 10; [quality metadata](tests/metrics/quality_run_metadata.json) records the source and matched settings. Speed uses the same production source at complexity 10; [speed metadata](tests/metrics/speed_run_metadata.json) records the isolated run. Memory is the tracked snapshot identified by [run metadata](tests/metrics/run_metadata.json).
Quality scoring removes each encoder's delay, flushes the tail, and scores stereo channels
independently. Both quality gains and losses are retained.

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`. The official Opus
baseline (upstream `main`, commit `503d81b`) is also built with `-O2 -DNDEBUG`, with x86 runtime-dispatched intrinsics enabled (`SSE`,
`SSE2`, `SSE4.1`, `AVX2`), on Windows MinGW GCC 16.2 / AMD Ryzen 7 8840HS. Encode and decode speed
are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster. The
published speed ratios are medians of nine repository 60-second stereo synthetic benchmark runs.
Both implementations alternate within one process pinned to one logical CPU at above-normal priority;
no other test workloads run concurrently.
Speed columns use the 60-second benchmark; quality and both effective-bitrate columns use the
six-second AUDIO validation. Payload rates in the speed CSV can therefore differ. Quality uses the default unfiltered
decoder output; the quality values are synthetic objective proxy scores, not a replacement for
official PESQ/ViSQOL tooling or listening tests.

| Bitrate | Encode speed vs official intrinsics | Decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.697x | 1.802x | +0.0006 | -0.0029 | 16.000 kbps | 17.065 kbps |
| 24&nbsp;kbps | 1.382x | 1.406x | +0.2622 | +0.0748 | 24.000 kbps | 25.229 kbps |
| 32&nbsp;kbps | 1.340x | 1.389x | +0.3234 | +0.0848 | 32.000 kbps | 33.613 kbps |
| 48&nbsp;kbps | 1.278x | 1.308x | +0.1185 | +0.0144 | 48.000 kbps | 48.560 kbps |
| 64&nbsp;kbps | 1.269x | 1.275x | +0.0209 | +0.0057 | 64.000 kbps | 64.613 kbps |
| 96&nbsp;kbps | 1.490x | 1.205x | +0.0876 | +0.0108 | 96.000 kbps | 96.697 kbps |
| 128&nbsp;kbps | 1.412x | 1.202x | +0.1909 | +0.0043 | 128.000 kbps | 128.759 kbps |
| 192&nbsp;kbps | 1.286x | 1.212x | +0.0761 | +0.0029 | 192.000 kbps | 192.900 kbps |
| 256&nbsp;kbps | 1.230x | 1.201x | +0.0407 | +0.0016 | 256.000 kbps | 256.737 kbps |


VOIP mono speech-like quality spot check:

| Bitrate | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1254 | -0.0000 | 15.999 kbps | 16.249 kbps |
| 24&nbsp;kbps | +0.1451 | -0.0051 | 24.000 kbps | 24.145 kbps |
| 32&nbsp;kbps | +0.1449 | -0.0039 | 32.000 kbps | 32.185 kbps |
| 48&nbsp;kbps | +0.1482 | -0.0071 | 48.000 kbps | 48.245 kbps |
| 64&nbsp;kbps | +0.1409 | -0.0089 | 63.985 kbps | 64.496 kbps |
| 96&nbsp;kbps | +0.0019 | +0.0050 | 96.000 kbps | 96.621 kbps |
| 128&nbsp;kbps | +0.0039 | +0.0008 | 128.000 kbps | 128.551 kbps |
| 192&nbsp;kbps | +0.0007 | -0.0004 | 192.000 kbps | 192.424 kbps |
| 256&nbsp;kbps | +0.0008 | -0.0003 | 256.000 kbps | 256.415 kbps |

Mode-selection check at 32&nbsp;kbps mono: for the synthetic spoken-voice sample, AUDIO mode selected
CELT for 95.7% of frames and hybrid for 4.3%. For the sustained harmonic/music sample, it selected
CELT for every frame. Restricted-lowdelay also remained CELT-only, as required.

## Memory snapshot

In this run, `opuscpp` uses less encoder and decoder state than official Opus
in every listed mono and stereo configuration. The optional encoder FEC state is allocated lazily
and is not included in this default-FEC-off, denoiser-off snapshot. These are process-private
allocation deltas measured with 256 instances per run (median of three fresh runs), not exact `sizeof` values; allocator/page rounding
can slightly change the measurement.

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 16,928 B | 31,712 B | -46.6% |
| Encoder stereo | 32,192 B | 49,072 B | -34.4% |
| Decoder mono | 14,176 B | 18,288 B | -22.5% |
| Decoder stereo | 21,184 B | 27,328 B | -22.5% |

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
  claim. The test encodes generated cases with `opuscpp`, decodes the packets with official Opus,
  and checks that the official decoder accepts the output for the supported scenarios.

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
