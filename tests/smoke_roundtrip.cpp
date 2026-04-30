#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr int kFrameSize = 960;
constexpr double kPi = 3.141592653589793238462643383279502884;

std::vector<std::int16_t> make_signal(int channels, int frames) {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(channels * frames));
  std::uint32_t noise = 1;
  for (int i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    noise = noise * 1664525u + 1013904223u;
    const double n = static_cast<int>((noise >> 16) & 0xffff) / 32768.0 - 1.0;
    const double envelope = 0.55 + 0.45 * std::sin(2.0 * kPi * 2.0 * t);
    const double base = envelope * (0.55 * std::sin(2.0 * kPi * 220.0 * t) + 0.25 * std::sin(2.0 * kPi * 440.0 * t)) + 0.03 * n;
    for (int c = 0; c < channels; ++c) {
      const double pan = channels == 1 ? 1.0 : (c == 0 ? 0.90 : 0.75);
      pcm[static_cast<std::size_t>(i * channels + c)] = static_cast<std::int16_t>(std::clamp<int>(static_cast<int>(base * pan * 28000.0), -32768, 32767));
    }
  }
  return pcm;
}

bool run_case(int channels, int bitrate) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(kSampleRate, channels, OPUS_APPLICATION_AUDIO, &err);
  OpusDecoder *dec = opus_decoder_create(kSampleRate, channels, &err);
  if (!enc || !dec || err != OPUS_OK) return false;

  opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
  opus_encoder_ctl(enc, OPUS_SET_VBR(1));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));

  constexpr int kFrames = 8;
  auto pcm = make_signal(channels, kFrameSize * kFrames);
  std::vector<std::int16_t> decoded(static_cast<std::size_t>(channels * kFrameSize));
  std::array<unsigned char, 1500> packet{};
  std::uint64_t checksum = 0;

  for (int f = 0; f < kFrames; ++f) {
    const auto *in = pcm.data() + static_cast<std::size_t>(f * kFrameSize * channels);
    const int len = opus_encode(enc, in, kFrameSize, packet.data(), static_cast<int>(packet.size()));
    if (len <= 0 || len > static_cast<int>(packet.size())) return false;
    const int samples = opus_packet_get_nb_samples(packet.data(), len, kSampleRate);
    if (samples != kFrameSize) return false;
    const int got = opus_decode(dec, packet.data(), len, decoded.data(), kFrameSize, 0);
    if (got != kFrameSize) return false;
    checksum += packet[0] + (static_cast<std::uint64_t>(packet[static_cast<std::size_t>(len - 1)]) << 8) + static_cast<std::uint64_t>(len) * 257u;
  }

  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);
  std::cout << "channels=" << channels << " bitrate=" << bitrate << " checksum=" << checksum << "\n";
  return checksum != 0;
}
} // namespace

int main() {
  for (int channels : {1, 2}) {
    for (int bitrate : {16000, 24000, 32000, 48000, 96000, 128000, 192000, 256000}) {
      if (!run_case(channels, bitrate)) {
        std::cerr << "smoke failed for channels=" << channels << " bitrate=" << bitrate << "\n";
        return 1;
      }
    }
  }
  return 0;
}
