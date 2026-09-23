#include "opus_codec.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr int frame_size = 960;
constexpr double pi = 3.14159265358979323846;
using packets = std::vector<std::vector<unsigned char>>;

std::vector<opus_int16> make_input(int frames, int kind) {
  std::vector<opus_int16> result(frames * frame_size);
  unsigned random = 7;
  for (int f = 0; f < frames; ++f)
    for (int i = 0; i < frame_size; ++i) {
      const double t = (f * frame_size + i) / 48000.;
      random = random * 1664525u + 1013904223u;
      double sample = 0;
      if (kind == 0 || kind == 2)
        sample = f < 6 ? .35 * std::sin(2 * pi * 220 * t) + .20 * std::sin(2 * pi * 1100 * t) + .30 * std::sin(2 * pi * 3300 * t)
                       : .3 * std::sin(2 * pi * 700 * t);
      if (kind == 2)
        sample *= .02;
      if (kind == 3)
        sample = .15 * std::sin(2 * pi * 100 * t);
      if (kind == 4)
        sample = .12 * (static_cast<int>(random >> 16) - 32768) / 32768.;
      result[f * frame_size + i] = static_cast<opus_int16>(std::lround(sample * 32767));
    }
  return result;
}

packets encode(int bitrate, bool float_api, const std::vector<opus_int16>& common, int prefix, bool reset, int fec = 0) {
  int error = OPUS_OK;
  auto encoder = make_opus_encoder(48000, 1, OPUS_APPLICATION_VOIP, &error);
  if (!encoder || error || opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)) ||
      opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(10)) || opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(1)))
    return {};
  if (fec != 0 && (opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(fec)) ||
                   opus_encoder_ctl(encoder.get(), OPUS_SET_PACKET_LOSS_PERC(15))))
    return {};
  std::array<unsigned char, 1500> packet{};
  std::array<float, frame_size> converted{};
  const auto frame = [&](const opus_int16* input) {
    if (float_api) {
      for (int i = 0; i < frame_size; ++i)
        converted[i] = input[i] / 32768.f;
      return opus_encode_float(encoder.get(), converted.data(), frame_size, packet.data(), packet.size());
    }
    return opus_encode(encoder.get(), input, frame_size, packet.data(), packet.size());
  };
  if (prefix) {
    const auto history = make_input(50, prefix);
    for (int f = 0; f < 50; ++f)
      if (frame(history.data() + f * frame_size) <= 0)
        return {};
  }
  if (reset && opus_encoder_ctl(encoder.get(), OPUS_RESET_STATE))
    return {};
  packets result;
  for (std::size_t pos = 0; pos < common.size(); pos += frame_size) {
    const int size = frame(common.data() + pos);
    if (size <= 0)
      return {};
    result.emplace_back(packet.begin(), packet.begin() + size);
  }
  return result;
}

int mode(const std::vector<unsigned char>& packet) {
  const int configuration = packet[0] >> 3;
  return configuration < 12 ? 0 : configuration < 16 ? 1
                                                     : 2;
}
}

int main() {
  const auto common = make_input(200, 0);
  int checks = 0, failures = 0;
  for (int bitrate : {16000, 48000, 64000})
    for (bool float_api : {false, true}) {
      const auto baseline = encode(bitrate, float_api, common, 0, false);
      if (baseline.size() != 200)
        return 1;
      for (int prefix : {1, 2, 3, 4}) {
        const auto candidate = encode(bitrate, float_api, common, prefix, false);
        const auto after_reset = encode(bitrate, float_api, common, prefix, true);
        if (candidate.size() != baseline.size() || after_reset != baseline)
          return 2;
        for (std::size_t f = 150; f < baseline.size(); ++f) {
          if (mode(candidate[f]) != mode(baseline[f])) {
            std::fprintf(stderr, "VOIP startup mode persists: bitrate=%d float=%d prefix=%d frame=%zu modes=%d/%d\n",
                         bitrate, float_api, prefix, f, mode(candidate[f]), mode(baseline[f]));
            ++failures;
            break;
          }
        }
        ++checks;
      }
    }
  for (bool float_api : {false, true}) {
    const auto fec_stream = encode(32000, float_api, common, 0, false, 1);
    const auto fec_reset = encode(32000, float_api, common, 3, true, 1);
    if (fec_stream.size() != 200 || fec_reset != fec_stream) {
      std::fprintf(stderr, "FEC1 startup reset mismatch: float=%d\n", float_api);
      ++failures;
    }
    if (fec_stream.empty() || mode(fec_stream[0]) == 2) {
      std::fprintf(stderr, "FEC1 startup first packet is CELT: float=%d\n", float_api);
      ++failures;
    }
    ++checks;
  }
  std::printf("voip_startup_behavior checks=%d failures=%d (mode convergence and byte-identical reset)\n", checks, failures);
  return failures ? 3 : 0;
}
