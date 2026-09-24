# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API,
derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is
designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`,
and ship no separate DLL or static library.

For C++ users who want a source-embeddable Opus implementation, `opuscpp` is positioned as an
alternative to official Opus rather than an outright replacement. It aims at a practical tradeoff:
standard Opus compatibility, portable source embedding, and content-dependent quality trade-offs. The headline benchmark comparison
uses the current `opuscpp` source and official Opus from upstream `main`, both built with `-O2 -DNDEBUG`, with x86 intrinsics enabled for official Opus, because that is the practical upstream baseline for many desktop builds. The
project targets standard Opus packets. Existing code using the supported Opus API can use this
implementation without packet-format changes as long as it stays within the supported CTL subset
described in `src/README.md`. Custom Opus is intentionally unsupported.

`opuscpp` is a portable C++23 alternative to official Opus for C++ users: it is source-embeddable, follows the standard Opus packet format, and exposes a focused single-stream API.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

Quality and speed comparisons below use the current complexity-10 encoder directly against official Opus. Some spectral and speech-proxy losses remain.

- Portable C++23 source embedding: `src/opus_codec.cpp` + `src/opus_codec.h`; no separate DLL or static library.
- Standard Opus packets and the documented single-stream API/CTL subset.
- Complexity-10 encoding is faster than official Opus at 9/9 measured AUDIO bitrates (1.05x to 1.26x).
- Decode is faster than official Opus with x86 intrinsics in 9/9 measured AUDIO bitrates (1.20x to 1.87x).
- Transient detection runs for every fullband CELT frame (matching official Opus gating), improving percussive content.
- Quality is mixed: AUDIO improves the PESQ-style proxy in 6/9 and the ViSQOL-style proxy in 6/9 tracked bitrates; VOIP improves them in 9/9 and 8/9. The full 498-case matrix has 849 below-official fields out of 5,976; all 12 metrics and signed deltas are retained.
- Effective bitrate, optional processing, FEC/DTX, memory and speed have dedicated benchmark coverage.
- Updated RFC decode vectors: 24/24 passed; encode interoperability: 96/96 passed in the source-bound compatibility refresh.
- RFC decode, encode interoperability, API/lookahead and focused source-bound regressions were rerun against this source; complete command outputs are in [compatibility validation](tests/metrics/README.md#fresh-compatibility-checks).
- Optional DTX: zero false DTX packets on the tracked active-content set; aggregate re-entry error is 44.1% lower and aggregate gain error is 12.7% higher at 16/24&nbsp;kbps. [DTX measurements](tests/metrics/dtx_metrics.csv).
- Optional FEC: FEC records 18/18 recovery wins and aggregate recovery ratio 0.504119; packet-byte ratio 0.993345 passes the <=1 gate. Source criteria are C1 18/18, C2 18/18, C3 18/18, C4 18/18; all four source criteria pass. Strict source gate: PASS; standard interop gate: PASS. The packet-byte acceptance check passes (ratio 0.993345).
- 2.4% to 23.3% lower measured private allocation footprint across the listed encoder/decoder configurations.
- Host object: `355,656 B` (text + data + BSS).
- No assembly, SIMD intrinsics, PGO or LTO requirement; MinGW GCC and Android arm64 Clang C++23 build checks were rerun in the full refresh.

## Pros and cons

| Pros | Cons |
|---|---|
| Source embedding: include the header and compile one implementation file. | An alternative, not a replacement for every official Opus use case. |
| Encoding is 1.05x to 1.26x of official Opus speed in the measured workload. | Results describe this machine and workload, not every platform or packet mix. |
| Faster decode in 9/9 measured AUDIO bitrates (1.20x to 1.87x). | Official Opus supports a broader feature surface and ecosystem. |
| 2.4% to 23.3% lower measured private allocation footprint. | Aligned quality proxies show both gains and losses; optional filtering is not a universal improvement. |
| Pure portable C++23, without ASM or SIMD intrinsics. | Requires a C++23-capable compiler. |

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

The full benchmark tables below were refreshed on 2026-09-24 for source `6d931a2` (SHA-256 `083a13104d07430b3382115ef4a90c21dfaf209e0f0e93bc5cdf8abe73a477c1`). The 498-case matrix has 849 below-official fields; all 12 metrics and adverse deltas are retained. FEC records 18/18 recovery wins and aggregate recovery ratio 0.504119; packet-byte ratio 0.993345 passes the <=1 gate. Source criteria are C1 18/18, C2 18/18, C3 18/18, C4 18/18; all four source criteria pass. Strict source gate: PASS; standard interop gate: PASS. Full values and bindings are in [the benchmark inventory](tests/metrics/README.md).

Measurements refreshed independently by Codex on 2026-09-24 for production `6d931a2`, against official Opus `503d81b` with intrinsics. [Complete metric inventory and results](tests/metrics/README.md) includes every measured field, signed loss, optional-processing cost and raw timing sample. Historical commit comparisons retain their original dates.

Quality and effective-bitrate columns use the current encoder at complexity 10; [quality metadata](tests/metrics/quality_run_metadata.json) records the source and matched settings. Speed uses the same production source at complexity 10; [speed metadata](tests/metrics/speed_run_metadata.json) records the isolated run. Memory is the tracked snapshot identified by [run metadata](tests/metrics/run_metadata.json).
Quality scoring removes each encoder's delay, flushes the tail, and scores stereo channels
independently. Both quality gains and losses are retained.

Measurements below use `opuscpp` compiled globally with `-O2 -DNDEBUG`. The official Opus
baseline (upstream `main`) is also built with `-O2 -DNDEBUG`, with x86 runtime-dispatched intrinsics enabled (`SSE`,
`SSE2`, `SSE4.1`, `AVX2`), on Windows MinGW GCC 16.2 / AMD Ryzen 7 8840HS. Encode and decode speed
are multiplicative ratios versus official Opus; values above `1.00x` mean `opuscpp` is faster. The
published AUDIO speed ratios are medians of nine repository 60-second stereo synthetic benchmark runs.
Both implementations alternate within one process pinned to one logical CPU at above-normal priority;
no other test workloads run concurrently.
AUDIO speed columns use the 60-second benchmark; quality and both effective-bitrate columns use the
six-second AUDIO validation. Payload rates in the speed CSV can therefore differ. Quality uses the default unfiltered
decoder output; the quality values are synthetic objective proxy scores, not a replacement for
official PESQ/ViSQOL tooling or listening tests.

| Bitrate | AUDIO encode speed vs official intrinsics | AUDIO decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 1.133x | 1.875x | +0.0002 | -0.0071 | 16.451 kbps | 17.065 kbps |
| 24&nbsp;kbps | 1.180x | 1.425x | -0.0236 | +0.0215 | 24.480 kbps | 25.229 kbps |
| 32&nbsp;kbps | 1.165x | 1.373x | -0.0368 | -0.0164 | 32.507 kbps | 33.613 kbps |
| 48&nbsp;kbps | 1.160x | 1.320x | -0.0348 | -0.0077 | 48.560 kbps | 48.560 kbps |
| 64&nbsp;kbps | 1.211x | 1.301x | +0.1505 | +0.0136 | 64.613 kbps | 64.613 kbps |
| 96&nbsp;kbps | 1.260x | 1.235x | +0.2812 | +0.0152 | 96.720 kbps | 96.697 kbps |
| 128&nbsp;kbps | 1.195x | 1.201x | +0.1528 | +0.0061 | 128.827 kbps | 128.759 kbps |
| 192&nbsp;kbps | 1.104x | 1.221x | +0.0705 | +0.0048 | 193.028 kbps | 192.900 kbps |
| 256&nbsp;kbps | 1.053x | 1.212x | +0.0355 | +0.0039 | 256.576 kbps | 256.737 kbps |



VOIP mono speech-like quality spot check (phase-integrated voiced/formant fixture with 30 dB breath-noise SNR):

| Bitrate | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1482 | +0.0049 | 12.367 kbps | 12.072 kbps |
| 24&nbsp;kbps | +0.2090 | +0.0138 | 24.011 kbps | 24.020 kbps |
| 32&nbsp;kbps | +0.2713 | +0.0157 | 32.113 kbps | 32.088 kbps |
| 48&nbsp;kbps | +0.1432 | -0.0050 | 48.352 kbps | 48.409 kbps |
| 64&nbsp;kbps | +0.2931 | +0.0016 | 64.481 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.7011 | +0.0081 | 96.400 kbps | 96.499 kbps |
| 128&nbsp;kbps | +0.7738 | +0.0033 | 128.400 kbps | 128.489 kbps |
| 192&nbsp;kbps | +1.0978 | +0.0048 | 192.400 kbps | 192.472 kbps |
| 256&nbsp;kbps | +1.1009 | +0.0055 | 256.400 kbps | 256.464 kbps |

Mode-selection check at 32&nbsp;kbps mono: for the separate detector synthetic test signal, AUDIO mode selected
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
| Encoder mono | 31,072 B | 31,824 B | -2.4% |
| Encoder stereo | 46,960 B | 48,864 B | -3.9% |
| Decoder mono | 14,080 B | 18,352 B | -23.3% |
| Decoder stereo | 21,200 B | 27,376 B | -22.6% |

## Conformance

The implementation is standard Opus compatible. The measured conformance gates are:

- RFC decode conformance: 24/24 mono+stereo RFC 8251 updated vector checks passed against the pinned official reference.
- Encode interoperability validation: 96/96 generated encode cases produced packets accepted by the
  official Opus decoder.
- API behavior validation: decoder channel-remap, packet/frame-duration rejection, encoder-lookahead, VBR-budget, and guarded-DTX checks passed
  in the source-bound compatibility refresh.
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

This project is derived from Opus 1.6.1 (tracked upstream commit [503d81b138d7](https://github.com/xiph/opus/commit/503d81b138d76621aae4b12786e90de48aa8db3a)) and retains the upstream Opus license text in `LICENSE`.

## Real-world use: Melo

`opuscpp` powers the voice path in Melo, an ultra-lightweight translator built for fast, natural
conversations across languages. The goal is the same as this codec: no bloat, no lag, just clear
human connection.

Available on [Android](https://play.google.com/store/apps/details?id=dands.technologies.melo),
[Windows](https://storage.googleapis.com/dnstech-release/executables/Melo.exe),
[iOS](https://apps.apple.com/us/app/melo/id1037721120), and
[macOS](https://apps.apple.com/us/app/melo/id1037721124?mt=12).
