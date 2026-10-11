# opuscpp

`opuscpp` is a pure portable C++23 implementation of the standard Opus single-stream codec API,
derived from [Xiph's official Opus project](https://github.com/xiph/opus) version 1.6.1. It is
designed for source embedding: add `src/opus_codec.cpp` to your build, include `src/opus_codec.h`,
and ship no separate DLL or static library.

`opuscpp` is a portable C++23 implementation of the standard Opus single-stream API, derived from Xiph Opus 1.6.1. It is source-embeddable, uses standard Opus packets, and supports the documented single-stream API/CTL subset in `src/README.md`. Custom Opus is unsupported.

Minimal integration looks like:

```cpp
#include "opus_codec.h"
```

## Highlights

- Portable C++23 source embedding: `src/opus_codec.cpp` and `src/opus_codec.h`; no separate library is required.
- Standard Opus packet format and a focused single-stream API/CTL subset.
- Optional in-band FEC, guarded DTX and a mono-VOIP denoiser, each disabled by default.
- No assembly or SIMD-intrinsic dependency.

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

Both builds use `-O2 -DNDEBUG`; official Opus enables x86 intrinsics. Measurements use Windows MinGW GCC 16.2 / AMD Ryzen 7 8840HS, nine 60-second stereo AUDIO runs. Ratios above `1.00x` indicate faster processing. [Complete metrics](tests/metrics/README.md).

| Bitrate | AUDIO encode speed vs official intrinsics | AUDIO decode speed vs official intrinsics | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | 0.991x | 1.778x | +0.0022 | -0.0045 | 17.032 kbps | 17.065 kbps |
| 24&nbsp;kbps | 1.000x | 1.352x | +0.0233 | +0.0182 | 25.247 kbps | 25.229 kbps |
| 32&nbsp;kbps | 1.017x | 1.280x | +0.0524 | -0.0039 | 33.464 kbps | 33.613 kbps |
| 48&nbsp;kbps | 1.054x | 1.181x | +0.1992 | +0.0260 | 48.560 kbps | 48.560 kbps |
| 64&nbsp;kbps | 1.065x | 1.236x | +0.2926 | +0.0234 | 64.613 kbps | 64.613 kbps |
| 96&nbsp;kbps | 1.107x | 1.124x | +0.2818 | +0.0160 | 96.720 kbps | 96.697 kbps |
| 128&nbsp;kbps | 1.029x | 1.181x | +0.1593 | +0.0052 | 128.827 kbps | 128.759 kbps |
| 192&nbsp;kbps | 1.007x | 1.217x | +0.0693 | +0.0048 | 193.033 kbps | 192.900 kbps |
| 256&nbsp;kbps | 0.970x | 1.175x | +0.0342 | +0.0029 | 256.528 kbps | 256.737 kbps |

VOIP mono speech-like quality spot check (phase-integrated voiced/formant fixture with 30 dB breath-noise SNR):

| Bitrate | PESQ-style delta | ViSQOL-style delta | opuscpp effective bitrate | official Opus effective bitrate |
|---:|---:|---:|---:|---:|
| 16&nbsp;kbps | +0.1323 | +0.0034 | 12.263 kbps | 12.072 kbps |
| 24&nbsp;kbps | +0.2588 | +0.0205 | 24.005 kbps | 24.020 kbps |
| 32&nbsp;kbps | +0.3299 | +0.0219 | 32.116 kbps | 32.088 kbps |
| 48&nbsp;kbps | +0.1979 | -0.0003 | 48.349 kbps | 48.409 kbps |
| 64&nbsp;kbps | +0.3576 | +0.0041 | 64.477 kbps | 64.515 kbps |
| 96&nbsp;kbps | +0.7636 | +0.0102 | 96.400 kbps | 96.499 kbps |
| 128&nbsp;kbps | +0.8311 | +0.0049 | 128.400 kbps | 128.489 kbps |
| 192&nbsp;kbps | +1.0997 | +0.0054 | 192.400 kbps | 192.472 kbps |
| 256&nbsp;kbps | +1.1014 | +0.0056 | 256.400 kbps | 256.464 kbps |


## Memory snapshot

Memory uses 256 instances per group across three fresh processes with FEC and denoising disabled; values are process-private allocation deltas.

| State | opuscpp | official Opus | Difference |
|---:|---:|---:|---:|
| Encoder mono | 19,776 B | 31,856 B | -37.9% |
| Encoder stereo | 30,016 B | 48,880 B | -38.6% |
| Decoder mono | 8,432 B | 18,464 B | -54.3% |
| Decoder stereo | 17,600 B | 27,088 B | -35.0% |

## Compatibility coverage

[Build and run instructions](tests/README.md) · [Detailed metrics](tests/metrics/README.md).

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
