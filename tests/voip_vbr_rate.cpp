#include "opus_codec.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr int seconds = 12;
constexpr double pi = 3.14159265358979323846;

auto make_voice() -> std::vector<opus_int16> {
  std::vector<opus_int16> pcm(static_cast<std::size_t>(sample_rate * seconds));
  std::uint32_t noise_state = 1;
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const bool talking = std::fmod(t, 3.0) < 2.2;
    const double pitch = 120.0 + 25.0 * std::sin(2.0 * pi * 1.3 * t);
    const double phase = 2.0 * pi * pitch * t;
    const double syllable = .5 + .5 * std::sin(2.0 * pi * 4.0 * t);
    noise_state = noise_state * 1664525u + 1013904223u;
    const double noise = static_cast<double>(static_cast<std::int32_t>(noise_state)) / 2147483648.0;
    double v = 30.0 * noise;
    if (talking) {
      v += 6000.0 * syllable * (.6 * std::sin(phase) + .25 * std::sin(2.0 * phase) + .15 * std::sin(3.0 * phase)) + 700.0 * noise;
    }
    pcm[i] = static_cast<opus_int16>(std::lround(v));
  }
  return pcm;
}

}

int main() {
  const auto pcm = make_voice();
  int failures = 0;
  for (const int bitrate : {16000, 32000, 64000}) {
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_VOIP, &error);
    if (encoder == nullptr || error != OPUS_OK || opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10)) != OPUS_OK || opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK) {
      std::printf("encoder setup failed at %d\n", bitrate);
      return 1;
    }
    std::array<unsigned char, 1500> packet{};
    long long bytes = 0;
    int frames = 0;
    for (std::size_t offset = 0; offset + frame_size <= pcm.size(); offset += frame_size, ++frames) {
      const int length = opus_encode(encoder, pcm.data() + offset, frame_size, packet.data(), static_cast<int>(packet.size()));
      if (length <= 0) {
        std::printf("encode failed at %d frame %d\n", bitrate, frames);
        return 1;
      }
      bytes += length;
    }
    opus_encoder_destroy(encoder);
    const double ratio = static_cast<double>(bytes) * 8.0 * sample_rate / (static_cast<double>(frames) * frame_size) / bitrate;
    const bool ok = ratio >= .97 && ratio <= 1.01;
    failures += !ok;
    std::printf("voip_vbr_rate bitrate=%d ratio=%.4f %s\n", bitrate, ratio, ok ? "PASS" : "FAIL");
  }
  return failures != 0;
}
