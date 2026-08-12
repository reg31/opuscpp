#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

[[nodiscard]] constexpr auto credit_capped_packet_bytes(int target_bits) noexcept -> int {
  return (target_bits + target_bits / 5) / 8;
}

[[nodiscard]] auto make_test_pcm(int channels, int samples) -> std::vector<opus_int16> {
  auto pcm = std::vector<opus_int16>(static_cast<std::size_t>(samples * channels));
  for (int i = 0; i < samples; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    const double envelope = 0.55 + 0.35 * std::sin(2.0 * 3.141592653589793 * 2.7 * t);
    const double base =
        envelope * (0.55 * std::sin(2.0 * 3.141592653589793 * 220.0 * t) + 0.20 * std::sin(2.0 * 3.141592653589793 * 880.0 * t) +
                    0.08 * std::sin(2.0 * 3.141592653589793 * 1720.0 * t));
    for (int channel = 0; channel < channels; ++channel) {
      const double width = channels == 2 ? (channel == 0 ? 1.0 : 0.72) : 1.0;
      pcm[static_cast<std::size_t>(i * channels + channel)] =
          static_cast<opus_int16>(std::lrint(std::clamp(base * width, -0.95, 0.95) * 32767.0));
    }
  }
  return pcm;
}

void require_ok(int value, const char* what) {
  if (value != OPUS_OK) {
    throw std::runtime_error(what);
  }
}

void check_case(int application, int channels, int bitrate, int frame_size) {
  constexpr int sample_rate = 48000;
  constexpr int seconds = 8;
  constexpr int total_samples = sample_rate * seconds;
  auto pcm = make_test_pcm(channels, total_samples);
  int error = OPUS_OK;
  auto encoder = make_opus_encoder(sample_rate, channels, application, &error);
  if (!encoder || error != OPUS_OK) {
    throw std::runtime_error("encoder create failed");
  }
  require_ok(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)), "set bitrate failed");
  require_ok(opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(1)), "set VBR failed");
  require_ok(opus_encoder_ctl(encoder.get(), OPUS_SET_VBR_CONSTRAINT(1)), "set constrained VBR failed");

  std::array<unsigned char, 4000> packet{};
  const int frame_count = total_samples / frame_size;
  long long total_bytes = 0;
  for (int frame = 0; frame < frame_count; ++frame) {
    const auto offset = static_cast<std::size_t>(frame * frame_size * channels);
    const int packet_size = opus_encode(encoder.get(), pcm.data() + offset, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (packet_size < 0) {
      throw std::runtime_error("encode failed");
    }
    const auto cumulative_target_bits = static_cast<long long>(bitrate) * (frame + 1) * frame_size / sample_rate;
    const auto frame_target_bits = cumulative_target_bits - static_cast<long long>(bitrate) * frame * frame_size / sample_rate;
    if (packet_size > credit_capped_packet_bytes(static_cast<int>(frame_target_bits))) {
      throw std::runtime_error("VBR credit cap exceeded");
    }
    total_bytes += packet_size;
    if (total_bytes * 8 > cumulative_target_bits) {
      throw std::runtime_error("VBR cumulative budget exceeded");
    }
  }

  const long long target_total_bits = static_cast<long long>(bitrate) * frame_count * frame_size / sample_rate;
  if (total_bytes * 8 > target_total_bits) {
    throw std::runtime_error("VBR average budget exceeded");
  }
}

} // namespace

int main() {
  for (const int application : std::array{OPUS_APPLICATION_AUDIO, OPUS_APPLICATION_VOIP}) {
    for (const int channels : std::array{1, 2}) {
      for (const int frame_size : std::array{480, 960, 1920, 2880}) {
        for (const int bitrate : std::array{15999, 16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000}) {
          check_case(application, channels, bitrate * channels, frame_size);
        }
      }
    }
  }
  std::cout << "vbr_budget_behavior=PASS\n";
  return 0;
}
