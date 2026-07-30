# Source API

`src/` contains only the embeddable library:

- `opus_codec.h` - public API declarations and constants.
- `opus_codec.cpp` - self-contained C++23 implementation.

Add `opus_codec.cpp` to your build and include `opus_codec.h`. There is no required DLL, shared library, static library, runtime data file, assembly object, or platform-specific dependency.
This repository intentionally omits a packaged build-system wrapper; the expected integration model is to compile `opus_codec.cpp` directly inside your project.

## Compatibility goal

The supported API mirrors the commonly used single-stream Opus API. It produces and consumes standard Opus packets, so decoder/encoder interoperability does not require custom packet handling. Existing user code can usually keep the same include-and-call pattern as official Opus, provided it stays within the supported CTL subset documented here.

Custom Opus is unsupported.

## Public constants

Error/status constants:

- `OPUS_OK`
- `OPUS_BAD_ARG`
- `OPUS_BUFFER_TOO_SMALL`
- `OPUS_INTERNAL_ERROR`
- `OPUS_INVALID_PACKET`
- `OPUS_UNIMPLEMENTED`
- `OPUS_INVALID_STATE`
- `OPUS_ALLOC_FAIL`

Applications:

- `OPUS_APPLICATION_VOIP`
- `OPUS_APPLICATION_AUDIO`
- `OPUS_APPLICATION_RESTRICTED_LOWDELAY`

Bitrate helpers:

- `OPUS_AUTO`
- `OPUS_BITRATE_MAX`

Frame-size constants:

- `OPUS_FRAME_SIZE_2MS5`
- `OPUS_FRAME_SIZE_5MS`
- `OPUS_FRAME_SIZE_10MS`
- `OPUS_FRAME_SIZE_20MS`

## Public functions

Encoder:

```cpp
OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
void opus_encoder_destroy(OpusEncoder* st) noexcept;
int opus_encoder_ctl(OpusEncoder* st, int request, ...) noexcept;
int opus_encode(OpusEncoder* st, const int16_t* pcm, int frame_size,
                unsigned char* data, int max_data_bytes) noexcept;
int opus_encode_float(OpusEncoder* st, const float* pcm, int frame_size,
                      unsigned char* data, int max_data_bytes) noexcept;
```

Decoder:

```cpp
OpusDecoder* opus_decoder_create(int Fs, int channels, int* error) noexcept;
void opus_decoder_destroy(OpusDecoder* st) noexcept;
int opus_decoder_ctl(OpusDecoder* st, int request, ...) noexcept;
int opus_decode(OpusDecoder* st, const unsigned char* data, int len,
                int16_t* pcm, int frame_size, int decode_fec) noexcept;
int opus_decode_float(OpusDecoder* st, const unsigned char* data, int len,
                      float* pcm, int frame_size, int decode_fec) noexcept;
```

Utility:

```cpp
int opus_packet_get_nb_samples(const unsigned char* data, int len, int Fs) noexcept;
const char* opus_strerror(int error) noexcept;
```

C++ RAII helpers:

```cpp
std::unique_ptr<OpusEncoder> make_opus_encoder(int Fs, int channels, int application, int* error) noexcept;
std::unique_ptr<OpusDecoder> make_opus_decoder(int Fs, int channels, int* error) noexcept;
```

## Supported encoder CTLs

| CTL | Macro | Notes |
|---|---|---|
| `OPUS_SET_BITRATE_REQUEST` | `OPUS_SET_BITRATE(x)` | Supports explicit bps, `OPUS_AUTO`, and `OPUS_BITRATE_MAX`. |
| `OPUS_GET_BITRATE_REQUEST` | `OPUS_GET_BITRATE(&x)` | Returns current effective bitrate. |
| `OPUS_SET_VBR_REQUEST` | `OPUS_SET_VBR(x)` | `1` for VBR, `0` for CBR-style packet padding; default is VBR on. |
| `OPUS_GET_VBR_REQUEST` | `OPUS_GET_VBR(&x)` | Returns VBR setting. |
| `OPUS_SET_DTX_REQUEST` | `OPUS_SET_DTX(x)` | Enables guarded discontinuous transmission; default is off. |
| `OPUS_GET_DTX_REQUEST` | `OPUS_GET_DTX(&x)` | Returns the DTX setting. |
| `OPUS_GET_IN_DTX_REQUEST` | `OPUS_GET_IN_DTX(&x)` | Reports whether the encoder is currently suppressing inactive frames. |
| `OPUS_SET_VBR_CONSTRAINT_REQUEST` | `OPUS_SET_VBR_CONSTRAINT(x)` | Enables/disables constrained VBR; default is constrained VBR on. |
| `OPUS_GET_VBR_CONSTRAINT_REQUEST` | `OPUS_GET_VBR_CONSTRAINT(&x)` | Returns constrained-VBR setting. |
| `OPUS_SET_COMPLEXITY_REQUEST` | `OPUS_SET_COMPLEXITY(x)` | Accepts `0..10`; higher values enable more encoder analysis. |
| `OPUS_GET_COMPLEXITY_REQUEST` | `OPUS_GET_COMPLEXITY(&x)` | Returns effective complexity. |
| `OPUS_GET_FINAL_RANGE_REQUEST` | `OPUS_GET_FINAL_RANGE(&x)` | Final entropy range for validation/debug. |
| `OPUS_RESET_STATE` | `OPUS_RESET_STATE` | Resets encoder state while preserving encoder configuration CTLs. |

DTX reuses the existing speech/music analysis, protects quiet or tonal content, and emits standard
one-byte Opus DTX packets after sustained inactivity. It periodically sends a refresh packet and
suppresses tiny digital-silence residue before encoding so it cannot excite decoder comfort noise.

## Supported decoder CTLs

| CTL | Macro | Notes |
|---|---|---|
| `OPUS_GET_LAST_PACKET_DURATION_REQUEST` | `OPUS_GET_LAST_PACKET_DURATION(&x)` | Returns last decoded packet duration. |
| `OPUS_GET_FINAL_RANGE_REQUEST` | `OPUS_GET_FINAL_RANGE(&x)` | Final entropy range for validation/debug. |
| `OPUSCPP_SET_DECODE_POSTFILTER_REQUEST` | `OPUSCPP_SET_DECODE_POSTFILTER(x)` | Speech-oriented `opuscpp` output postfilter: `0` off, `1` light, `2` stronger, `3` adaptive; default is off for RFC-compatible decoder output. |
| `OPUSCPP_GET_DECODE_POSTFILTER_REQUEST` | `OPUSCPP_GET_DECODE_POSTFILTER(&x)` | Returns the output-postfilter level. |
| `OPUS_RESET_STATE` | `OPUS_RESET_STATE` | Resets decoder state. |

The decoder postfilter is intentionally `opuscpp`-specific and does not change packet syntax or
encoder interoperability. The default level is `0`, so normal decoder output remains RFC-compatible.
It gently suppresses high-frequency quantization noise, harshness, and ringing, mainly at low and
mid bitrates. Adaptive mode (`3`) uses bitrate, packet mode, channel count, and speech activity, and
bypasses combinations where its measured benefit did not justify the decode cost. The trade-off is
extra decoder work and potentially softer treble while filtering is active.

| Level | Recommended use |
|---:|---|
| `0` (off) | General audio, music, validation, speech-to-text, and applications requiring the plainest decoder output. This is the default. |
| `1` (light) | Mild harshness or ringing where preserving treble is more important than maximum smoothing. |
| `2` (stronger) | Clearly noisy low-bitrate speech where stronger smoothing is preferred despite a greater risk of dulling treble. |
| `3` (adaptive) | Recommended for speech below 40 kbps. It filters only packet combinations with a measured benefit and otherwise bypasses the extra pass. It is not recommended for music, where it usually has no effect and may soften tonal detail when active. |

Published quality metrics use the default unfiltered output.

## Unsupported API areas

Unsupported requests return `OPUS_UNIMPLEMENTED` or `OPUS_BAD_ARG` depending on the call. Notable unsupported areas:

- Custom Opus (`opus_custom_*`).
- Multistream/projection APIs.
- Repacketizer APIs.
- FEC/bandwidth/signal/gain/LSB-depth CTLs not listed above.
- Decoder-side FEC output beyond accepting the standard decode argument shape.

This is deliberate: the implementation focuses on the common single-stream Opus API and keeps unused feature surfaces out of the public contract.
