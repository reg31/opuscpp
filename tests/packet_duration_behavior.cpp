#include "opus_codec.h"

#include <array>
#include <iostream>

namespace {

[[nodiscard]] auto expect_eq(int actual, int expected, const char* message) -> bool {
  if (actual != expected) {
    std::cout << "FAIL " << message << " actual=" << actual << " expected=" << expected << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  auto ok = true;

  constexpr std::array<unsigned char, 4> code0_20ms{0x78, 1, 2, 3};
  constexpr std::array<unsigned char, 7> code1_20ms{0x79, 1, 2, 3, 4, 5, 6};
  constexpr std::array<unsigned char, 7> code2_20ms{0x7A, 2, 1, 2, 3, 4, 5};
  constexpr std::array<unsigned char, 8> code3_cbr_20ms{0x7B, 0x03, 1, 2, 3, 4, 5, 6};
  constexpr std::array<unsigned char, 10> code3_vbr_20ms{0x7B, 0x83, 1, 2, 3, 4, 5, 6, 7, 8};
  constexpr std::array<unsigned char, 2> code3_120ms{0x83, 48};
  constexpr std::array<unsigned char, 2> code3_too_long{0x7B, 7};
  constexpr std::array<unsigned char, 2> celt_code3_too_long{0x83, 63};

  ok &= expect_eq(opus_packet_get_nb_samples(code0_20ms.data(), static_cast<int>(code0_20ms.size()), 48000), 960, "code0 one 20ms frame");
  ok &= expect_eq(opus_packet_get_nb_samples(code1_20ms.data(), static_cast<int>(code1_20ms.size()), 48000), 1920,
                  "code1 two 20ms CBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code2_20ms.data(), static_cast<int>(code2_20ms.size()), 48000), 1920,
                  "code2 two 20ms VBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_cbr_20ms.data(), static_cast<int>(code3_cbr_20ms.size()), 48000), 2880,
                  "code3 three 20ms CBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_vbr_20ms.data(), static_cast<int>(code3_vbr_20ms.size()), 48000), 2880,
                  "code3 three 20ms VBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_120ms.data(), static_cast<int>(code3_120ms.size()), 48000), 5760,
                  "code3 accepts 120ms aggregate");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_too_long.data(), static_cast<int>(code3_too_long.size()), 48000), OPUS_INVALID_PACKET,
                  "code3 rejects packets above 120ms");
  ok &= expect_eq(opus_packet_get_nb_samples(celt_code3_too_long.data(), static_cast<int>(celt_code3_too_long.size()), 48000),
                  OPUS_INVALID_PACKET, "CELT fast-path shaped packet rejects packets above 120ms");
  ok &= expect_eq(opus_packet_get_nb_samples(nullptr, 0, 48000), OPUS_BAD_ARG, "null/empty packet rejected");

  int encoder_error = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &encoder_error);
  std::array<opus_int16, 1> encoder_pcm{};
  std::array<float, 1> encoder_pcm_float{};
  std::array<unsigned char, 16> encoder_packet{};
  constexpr int overflowing_frame_size = 1073741864;
  ok &= expect_eq(encoder_error, OPUS_OK, "create encoder");
  ok &= expect_eq(opus_encode(encoder, encoder_pcm.data(), overflowing_frame_size, encoder_packet.data(), encoder_packet.size()),
                  OPUS_BAD_ARG, "reject overflowing PCM16 frame size");
  ok &= expect_eq(opus_encode_float(encoder, encoder_pcm_float.data(), overflowing_frame_size, encoder_packet.data(), encoder_packet.size()),
                  OPUS_BAD_ARG, "reject overflowing float frame size");
  opus_encoder_destroy(encoder);

  int error = OPUS_OK;
  OpusDecoder* decoder = opus_decoder_create(48000, 2, &error);
  std::array<opus_int16, 5760 * 2> pcm{};
  std::array<float, 5760 * 2> pcm_float{};
  opus_int32 postfilter_level = -1;
  ok &= expect_eq(error, OPUS_OK, "create decoder");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_GET_DECODE_POSTFILTER(&postfilter_level)), OPUS_OK,
                  "default postfilter get");
  ok &= expect_eq(postfilter_level, 0, "default postfilter off");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_SET_DECODE_POSTFILTER(2)), OPUS_OK, "set postfilter");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_GET_DECODE_POSTFILTER(&postfilter_level)), OPUS_OK,
                  "configured postfilter get");
  ok &= expect_eq(postfilter_level, 2, "configured postfilter level");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_SET_DECODE_POSTFILTER(3)), OPUS_OK, "set adaptive postfilter");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_SET_DECODE_POSTFILTER(4)), OPUS_BAD_ARG, "invalid postfilter level");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUS_RESET_STATE), OPUS_OK, "reset decoder");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_GET_DECODE_POSTFILTER(&postfilter_level)), OPUS_OK,
                  "postfilter survives reset get");
  ok &= expect_eq(postfilter_level, 3, "postfilter setting survives reset");
  ok &= expect_eq(opus_decode(decoder, celt_code3_too_long.data(), static_cast<int>(celt_code3_too_long.size()), pcm.data(), 5760, 0),
                  OPUS_INVALID_PACKET, "invalid CELT-shaped packet is rejected before decode");
  ok &= expect_eq(opus_decode_float(decoder, celt_code3_too_long.data(), static_cast<int>(celt_code3_too_long.size()),
                                    pcm_float.data(), 5760, 0),
                  OPUS_INVALID_PACKET, "invalid CELT-shaped float packet is rejected before decode");
  opus_decoder_destroy(decoder);

  if (!ok) {
    return 1;
  }
  std::cout << "packet_duration_behavior=PASS\n";
  return 0;
}
