#include "official_opus_abi.h"

void* lpc_official_decoder_create(int rate, int channels, int* error) {
  return opus_decoder_create(rate, channels, error);
}

void lpc_official_decoder_destroy(void* decoder) {
  opus_decoder_destroy(static_cast<OpusDecoder*>(decoder));
}

int lpc_official_decode(void* decoder, const unsigned char* packet, int bytes, std::int16_t* output, int capacity) {
  return opus_decode(static_cast<OpusDecoder*>(decoder), packet, bytes, output, capacity, 0);
}

int lpc_official_final_range(void* decoder, std::uint32_t* range) {
  return opus_decoder_ctl(static_cast<OpusDecoder*>(decoder), OPUS_GET_FINAL_RANGE_REQUEST, range);
}
