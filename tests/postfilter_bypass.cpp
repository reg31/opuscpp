#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

void require(bool condition) {
  if (!condition)
    throw std::runtime_error("Postfilter bypass validation failed");
}

template <typename Sample>
void check(const std::vector<std::vector<unsigned char>>& packets, int rate, int channels, int duration) {
  int error = OPUS_OK;
  auto off = make_opus_decoder(rate, channels, &error);
  require(off && error == OPUS_OK);
  auto legacy = make_opus_decoder(rate, channels, &error);
  require(legacy && error == OPUS_OK);
  const int frame_size = duration * rate / 48000;
  std::vector<Sample> a(frame_size * channels), b(a.size());
  for (std::size_t frame = 0; frame < packets.size(); ++frame) {
    if (frame == 9) {
      require(opus_decoder_ctl(off.get(), OPUS_RESET_STATE) == OPUS_OK);
      require(opus_decoder_ctl(legacy.get(), OPUS_RESET_STATE) == OPUS_OK);
    }
    const int level = static_cast<int>(frame % 4);
    require(opus_decoder_ctl(off.get(), OPUSCPP_SET_DECODE_POSTFILTER(0)) == OPUS_OK);
    require(opus_decoder_ctl(legacy.get(), OPUSCPP_SET_DECODE_POSTFILTER(level)) == OPUS_OK);
    opus_int32 configured = -1;
    require(opus_decoder_ctl(legacy.get(), OPUSCPP_GET_DECODE_POSTFILTER(&configured)) == OPUS_OK);
    require(configured == 0);
    const bool lost = frame % 7 == 3;
    const bool fec = frame % 7 == 6;
    const std::span<const unsigned char> packet = lost ? std::span<const unsigned char>{} : packets[frame];
    const auto decode = [&](OpusDecoder* decoder, std::span<Sample> output) {
      if constexpr (std::is_same_v<Sample, opus_int16>)
        return opus_decode(decoder, packet, output, fec);
      else
        return opus_decode_float(decoder, packet.data(), static_cast<int>(packet.size()), output.data(), frame_size, fec);
    };
    const int na = decode(off.get(), a);
    const int nb = decode(legacy.get(), b);
    require(na > 0 && na == nb && a == b);
    opus_uint32 ra = 0, rb = 0;
    require(opus_decoder_ctl(off.get(), OPUS_GET_FINAL_RANGE(&ra)) == OPUS_OK);
    require(opus_decoder_ctl(legacy.get(), OPUS_GET_FINAL_RANGE(&rb)) == OPUS_OK);
    require(ra == rb);
  }
}

}

int main() {
  try {
    int cases = 0;
    constexpr std::array bitrates{16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000};
    for (int application : {OPUS_APPLICATION_VOIP, OPUS_APPLICATION_AUDIO, OPUS_APPLICATION_RESTRICTED_LOWDELAY}) {
      for (int channels : {1, 2}) {
        for (int duration : {120, 240, 480, 960, 1920, 2880, 5760}) {
          if (application == OPUS_APPLICATION_RESTRICTED_LOWDELAY && duration > 960)
            continue;
          int error = OPUS_OK;
          auto encoder = make_opus_encoder(48000, channels, application, &error);
          require(encoder && error == OPUS_OK);
          require(opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(1)) == OPUS_OK);
          require(opus_encoder_ctl(encoder.get(), OPUS_SET_PACKET_LOSS_PERC(15)) == OPUS_OK);
          std::vector<opus_int16> pcm(duration * channels);
          std::array<unsigned char, 8000> packet;
          std::vector<std::vector<unsigned char>> packets;
          for (int frame = 0; frame < 18; ++frame) {
            require(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrates[frame % bitrates.size()])) == OPUS_OK);
            for (std::size_t i = 0; i < pcm.size(); ++i)
              pcm[i] = frame == 7 ? 0 : static_cast<opus_int16>(5000 * std::sin(0.043 * (i + frame * pcm.size())));
            const int bytes = opus_encode(encoder.get(), pcm.data(), duration, packet.data(), packet.size());
            require(bytes > 0);
            packets.emplace_back(packet.begin(), packet.begin() + bytes);
          }
          for (int rate : {8000, 12000, 16000, 24000, 48000}) {
            for (int output_channels : {1, 2}) {
              check<opus_int16>(packets, rate, output_channels, duration);
              check<float>(packets, rate, output_channels, duration);
              cases += 2;
            }
          }
        }
      }
    }
    std::cout << cases << " retired postfilter compatibility sequences passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
