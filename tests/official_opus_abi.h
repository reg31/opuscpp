#pragma once

#include <cstdint>

// Minimal official Opus C ABI used by comparison-only tests.
// This keeps opuscpp's test sources independent from third-party headers while
// still linking against an explicitly built official library.

struct OpusEncoder;
struct OpusDecoder;

inline constexpr int OPUS_OK = 0;

inline constexpr int OPUS_APPLICATION_VOIP = 2048;
inline constexpr int OPUS_APPLICATION_AUDIO = 2049;
inline constexpr int OPUS_APPLICATION_RESTRICTED_LOWDELAY = 2051;

inline constexpr int OPUS_SET_BITRATE_REQUEST = 4002;
inline constexpr int OPUS_SET_VBR_REQUEST = 4006;
inline constexpr int OPUS_SET_COMPLEXITY_REQUEST = 4010;
inline constexpr int OPUS_SET_INBAND_FEC_REQUEST = 4012;
inline constexpr int OPUS_GET_INBAND_FEC_REQUEST = 4013;
inline constexpr int OPUS_SET_PACKET_LOSS_PERC_REQUEST = 4014;
inline constexpr int OPUS_GET_PACKET_LOSS_PERC_REQUEST = 4015;
inline constexpr int OPUS_SET_DTX_REQUEST = 4016;
inline constexpr int OPUS_GET_FINAL_RANGE_REQUEST = 4031;

extern "C" {
OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error);
void opus_encoder_destroy(OpusEncoder* st);
int opus_encoder_ctl(OpusEncoder* st, int request, ...);
int opus_encode(OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes);
int opus_encoder_get_size(int channels);

OpusDecoder* opus_decoder_create(int Fs, int channels, int* error);
void opus_decoder_destroy(OpusDecoder* st);
int opus_decoder_ctl(OpusDecoder* st, int request, ...);
int opus_decode(OpusDecoder* st, const unsigned char* data, int len, std::int16_t* pcm, int frame_size, int decode_fec);
int opus_decode_float(OpusDecoder* st, const unsigned char* data, int len, float* pcm, int frame_size, int decode_fec);
int opus_decoder_get_size(int channels);
}
