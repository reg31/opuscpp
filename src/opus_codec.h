#pragma once
#include <cstdint>
#include <memory>

using opus_int16 = std::int16_t;
using opus_int32 = std::int32_t;
using opus_uint32 = std::uint32_t;

static constexpr int OPUS_OK                  =  0;
static constexpr int OPUS_BAD_ARG             = -1;
static constexpr int OPUS_BUFFER_TOO_SMALL    = -2;
static constexpr int OPUS_INTERNAL_ERROR      = -3;
static constexpr int OPUS_INVALID_PACKET      = -4;
static constexpr int OPUS_UNIMPLEMENTED       = -5;
static constexpr int OPUS_INVALID_STATE       = -6;
static constexpr int OPUS_ALLOC_FAIL          = -7;

static constexpr int OPUS_APPLICATION_VOIP               = 2048;
static constexpr int OPUS_APPLICATION_AUDIO              = 2049;
static constexpr int OPUS_APPLICATION_RESTRICTED_LOWDELAY = 2051;

static constexpr int OPUS_AUTO        = -1000;
static constexpr int OPUS_BITRATE_MAX = -1;

static constexpr int OPUS_SET_BITRATE_REQUEST    = 4002;
static constexpr int OPUS_GET_BITRATE_REQUEST    = 4003;
static constexpr int OPUS_SET_VBR_REQUEST        = 4006;
static constexpr int OPUS_GET_VBR_REQUEST        = 4007;
static constexpr int OPUS_SET_COMPLEXITY_REQUEST = 4010;
static constexpr int OPUS_GET_COMPLEXITY_REQUEST = 4011;
static constexpr int OPUS_SET_VBR_CONSTRAINT_REQUEST = 4020;
static constexpr int OPUS_GET_VBR_CONSTRAINT_REQUEST = 4021;
static constexpr int OPUS_RESET_STATE            = 4028;
static constexpr int OPUS_GET_FINAL_RANGE_REQUEST = 4031;
static constexpr int OPUS_GET_LAST_PACKET_DURATION_REQUEST = 4039;

#define OPUS_SET_BITRATE(x)    OPUS_SET_BITRATE_REQUEST,    static_cast<int>(x)
#define OPUS_GET_BITRATE(x)    OPUS_GET_BITRATE_REQUEST,    (x)
#define OPUS_SET_COMPLEXITY(x) OPUS_SET_COMPLEXITY_REQUEST, static_cast<int>(x)
#define OPUS_GET_COMPLEXITY(x) OPUS_GET_COMPLEXITY_REQUEST, (x)
#define OPUS_SET_VBR(x)        OPUS_SET_VBR_REQUEST,        static_cast<int>(x)
#define OPUS_GET_VBR(x)        OPUS_GET_VBR_REQUEST,        (x)
#define OPUS_SET_VBR_CONSTRAINT(x) OPUS_SET_VBR_CONSTRAINT_REQUEST, static_cast<int>(x)
#define OPUS_GET_VBR_CONSTRAINT(x) OPUS_GET_VBR_CONSTRAINT_REQUEST, (x)
#define OPUS_GET_FINAL_RANGE(x) OPUS_GET_FINAL_RANGE_REQUEST, (x)
#define OPUS_GET_LAST_PACKET_DURATION(x) OPUS_GET_LAST_PACKET_DURATION_REQUEST, (x)

static constexpr int OPUS_FRAME_SIZE_2MS5 = 120;
static constexpr int OPUS_FRAME_SIZE_5MS  = 240;
static constexpr int OPUS_FRAME_SIZE_10MS = 480;
static constexpr int OPUS_FRAME_SIZE_20MS = 960;

struct OpusEncoder;
struct OpusDecoder;
OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
void         opus_encoder_destroy(OpusEncoder* st) noexcept;
int          opus_encoder_ctl(OpusEncoder* st, int request, ...) noexcept;
int          opus_encode(OpusEncoder* st, const int16_t* pcm, int frame_size,
                         unsigned char* data, int max_data_bytes) noexcept;
int          opus_encode_float(OpusEncoder* st, const float* pcm, int frame_size,
                               unsigned char* data, int max_data_bytes) noexcept;

OpusDecoder* opus_decoder_create(int Fs, int channels, int* error) noexcept;
void         opus_decoder_destroy(OpusDecoder* st) noexcept;
int          opus_decoder_ctl(OpusDecoder* st, int request, ...) noexcept;
int          opus_decode(OpusDecoder* st, const unsigned char* data, int len,
                         int16_t* pcm, int frame_size, int decode_fec) noexcept;
int          opus_decode_float(OpusDecoder* st, const unsigned char* data, int len,
                               float* pcm, int frame_size, int decode_fec) noexcept;

int         opus_packet_get_nb_samples(const unsigned char* data, int len, int Fs) noexcept;
const char* opus_strerror(int error) noexcept;

namespace std {
template <> struct default_delete<OpusEncoder> {
  constexpr default_delete() noexcept = default;
  void operator()(OpusEncoder* st) const noexcept { opus_encoder_destroy(st); }
};

template <> struct default_delete<OpusDecoder> {
  constexpr default_delete() noexcept = default;
  void operator()(OpusDecoder* st) const noexcept { opus_decoder_destroy(st); }
};
} // namespace std

[[nodiscard]] inline auto make_opus_encoder(int Fs, int channels, int application, int* error) noexcept
    -> std::unique_ptr<OpusEncoder> {
  return std::unique_ptr<OpusEncoder>{opus_encoder_create(Fs, channels, application, error)};
}

[[nodiscard]] inline auto make_opus_decoder(int Fs, int channels, int* error) noexcept
    -> std::unique_ptr<OpusDecoder> {
  return std::unique_ptr<OpusDecoder>{opus_decoder_create(Fs, channels, error)};
}
