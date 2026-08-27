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
  constexpr std::array<unsigned char, 1> celt_20ms{0x98};
  constexpr std::array<unsigned char, 1> silk_60ms{0x18};
  constexpr int large_sample_rate = 2147483200;

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
  ok &= expect_eq(opus_packet_get_nb_samples(celt_20ms.data(), static_cast<int>(celt_20ms.size()), large_sample_rate), 42949664,
                  "large sample rate accepts 20ms CELT packet without overflow");
  ok &= expect_eq(opus_packet_get_nb_samples(silk_60ms.data(), static_cast<int>(silk_60ms.size()), large_sample_rate), 128848992,
                  "large sample rate accepts 60ms SILK packet without overflow");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_120ms.data(), static_cast<int>(code3_120ms.size()), large_sample_rate), 257697984,
                  "large sample rate accepts 120ms packet without overflow");
  ok &= expect_eq(opus_packet_get_nb_samples(nullptr, 0, 48000), OPUS_BAD_ARG, "null/empty packet rejected");

  int encoder_error = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &encoder_error);
  std::array<opus_int16, 1> encoder_pcm{};
  std::array<float, 1> encoder_pcm_float{};
  std::array<unsigned char, 16> encoder_packet{};
  constexpr int overflowing_frame_size = 1073741864;
  opus_int32 lookahead = -1;
  opus_int32 voice_denoise = -1;
  opus_int32* null_lookahead = nullptr;
  ok &= expect_eq(encoder_error, OPUS_OK, "create encoder");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)), OPUS_OK, "get VOIP lookahead");
  ok &= expect_eq(lookahead, 52, "8 kHz VOIP lookahead");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(null_lookahead)), OPUS_BAD_ARG, "reject null lookahead output");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUSCPP_GET_VOICE_DENOISE(&voice_denoise)), OPUS_OK, "default voice denoise get");
  ok &= expect_eq(voice_denoise, 0, "default voice denoise off");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUSCPP_SET_VOICE_DENOISE(2)), OPUS_BAD_ARG, "invalid voice denoise setting");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUSCPP_SET_VOICE_DENOISE(1)), OPUS_OK, "enable voice denoise");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUS_RESET_STATE), OPUS_OK, "reset encoder with voice denoise");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUSCPP_GET_VOICE_DENOISE(&voice_denoise)), OPUS_OK, "voice denoise survives reset get");
  ok &= expect_eq(voice_denoise, 1, "voice denoise setting survives reset");
  ok &= expect_eq(opus_encoder_ctl(encoder, OPUSCPP_SET_VOICE_DENOISE(0)), OPUS_OK, "disable voice denoise");
  ok &= expect_eq(opus_encode(encoder, encoder_pcm.data(), overflowing_frame_size, encoder_packet.data(), encoder_packet.size()),
                  OPUS_BAD_ARG, "reject overflowing PCM16 frame size");
  ok &=
      expect_eq(opus_encode_float(encoder, encoder_pcm_float.data(), overflowing_frame_size, encoder_packet.data(), encoder_packet.size()),
                OPUS_BAD_ARG, "reject overflowing float frame size");
  opus_encoder_destroy(encoder);

  int audio_error = OPUS_OK;
  OpusEncoder* audio_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &audio_error);
  lookahead = -1;
  ok &= expect_eq(audio_error, OPUS_OK, "create audio encoder");
  ok &= expect_eq(opus_encoder_ctl(audio_encoder, OPUS_GET_LOOKAHEAD(&lookahead)), OPUS_OK, "get AUDIO lookahead");
  ok &= expect_eq(lookahead, 312, "48 kHz AUDIO lookahead");
  ok &= expect_eq(opus_encoder_ctl(audio_encoder, OPUSCPP_SET_VOICE_DENOISE(1)), OPUS_OK, "accept AUDIO voice denoise");
  voice_denoise = -1;
  ok &= expect_eq(opus_encoder_ctl(audio_encoder, OPUSCPP_GET_VOICE_DENOISE(&voice_denoise)), OPUS_OK, "get bypassed AUDIO voice denoise");
  ok &= expect_eq(voice_denoise, 0, "AUDIO voice denoise remains off");
  opus_encoder_destroy(audio_encoder);

  int stereo_voip_error = OPUS_OK;
  OpusEncoder* stereo_voip_encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_VOIP, &stereo_voip_error);
  ok &= expect_eq(stereo_voip_error, OPUS_OK, "create stereo VOIP encoder");
  ok &= expect_eq(opus_encoder_ctl(stereo_voip_encoder, OPUSCPP_SET_VOICE_DENOISE(1)), OPUS_OK, "accept stereo voice denoise");
  voice_denoise = -1;
  ok &= expect_eq(opus_encoder_ctl(stereo_voip_encoder, OPUSCPP_GET_VOICE_DENOISE(&voice_denoise)), OPUS_OK,
                  "get bypassed stereo voice denoise");
  ok &= expect_eq(voice_denoise, 0, "stereo voice denoise remains off");
  opus_encoder_destroy(stereo_voip_encoder);

  int lowdelay_error = OPUS_OK;
  OpusEncoder* lowdelay_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &lowdelay_error);
  lookahead = -1;
  ok &= expect_eq(lowdelay_error, OPUS_OK, "create low-delay encoder");
  ok &= expect_eq(opus_encoder_ctl(lowdelay_encoder, OPUS_GET_LOOKAHEAD(&lookahead)), OPUS_OK, "get low-delay lookahead");
  ok &= expect_eq(lookahead, 120, "48 kHz restricted-low-delay lookahead");
  opus_encoder_destroy(lowdelay_encoder);

  int long_encoder_error = OPUS_OK;
  OpusEncoder* long_encoder = opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP, &long_encoder_error);
  std::array<opus_int16, 320> long_pcm{};
  std::array<unsigned char, 1276> long_packet{};
  ok &= expect_eq(long_encoder_error, OPUS_OK, "create long-frame encoder");
  ok &= expect_eq(opus_encoder_ctl(long_encoder, OPUS_SET_BITRATE(12000)), OPUS_OK, "configure long-frame encoder");
  const int long_packet_bytes = opus_encode(long_encoder, long_pcm.data(), 320, long_packet.data(), static_cast<int>(long_packet.size()));
  ok &= long_packet_bytes > 0;
  if (long_packet_bytes <= 0) {
    std::cout << "FAIL encode one 40ms SILK frame actual=" << long_packet_bytes << '\n';
  } else {
    ok &= expect_eq(long_packet[0] & 3, 0, "single-frame 40ms SILK packet");
    ok &= expect_eq(opus_packet_get_nb_samples(long_packet.data(), long_packet_bytes, 8000), 320, "encoded 40ms SILK duration");
    int long_decoder_error = OPUS_OK;
    OpusDecoder* long_decoder = opus_decoder_create(8000, 1, &long_decoder_error);
    std::array<opus_int16, 320> long_output{};
    ok &= expect_eq(long_decoder_error, OPUS_OK, "create long-frame decoder");
    ok &= expect_eq(
        opus_decode(long_decoder, long_packet.data(), long_packet_bytes, long_output.data(), static_cast<int>(long_output.size()), 0), 320,
        "decode one 40ms SILK frame");
    opus_decoder_destroy(long_decoder);
  }
  opus_encoder_destroy(long_encoder);

  int error = OPUS_OK;
  OpusDecoder* decoder = opus_decoder_create(48000, 2, &error);
  std::array<opus_int16, 5760 * 2> pcm{};
  std::array<float, 5760 * 2> pcm_float{};
  opus_int32 postfilter_level = -1;
  ok &= expect_eq(error, OPUS_OK, "create decoder");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_GET_DECODE_POSTFILTER(&postfilter_level)), OPUS_OK, "default postfilter get");
  ok &= expect_eq(postfilter_level, 0, "default postfilter off");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_SET_DECODE_POSTFILTER(2)), OPUS_OK, "set postfilter");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_GET_DECODE_POSTFILTER(&postfilter_level)), OPUS_OK, "configured postfilter get");
  ok &= expect_eq(postfilter_level, 2, "configured postfilter level");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_SET_DECODE_POSTFILTER(3)), OPUS_OK, "set adaptive postfilter");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_SET_DECODE_POSTFILTER(4)), OPUS_BAD_ARG, "invalid postfilter level");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUS_RESET_STATE), OPUS_OK, "reset decoder");
  ok &= expect_eq(opus_decoder_ctl(decoder, OPUSCPP_GET_DECODE_POSTFILTER(&postfilter_level)), OPUS_OK, "postfilter survives reset get");
  ok &= expect_eq(postfilter_level, 3, "postfilter setting survives reset");
  ok &= expect_eq(opus_decode(decoder, celt_code3_too_long.data(), static_cast<int>(celt_code3_too_long.size()), pcm.data(), 5760, 0),
                  OPUS_INVALID_PACKET, "invalid CELT-shaped packet is rejected before decode");
  ok &= expect_eq(
      opus_decode_float(decoder, celt_code3_too_long.data(), static_cast<int>(celt_code3_too_long.size()), pcm_float.data(), 5760, 0),
      OPUS_INVALID_PACKET, "invalid CELT-shaped float packet is rejected before decode");
  opus_decoder_destroy(decoder);

  if (!ok) {
    return 1;
  }
  std::cout << "packet_duration_behavior=PASS\n";
  return 0;
}
