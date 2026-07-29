#include "opus_codec.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = sample_rate / 50;

void require(bool condition, const std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("DTX behavior check failed at line " + std::to_string(location.line()));
  }
}

void fill_tone(std::array<opus_int16, frame_size>& pcm, double& phase, double frequency, double amplitude) {
  constexpr double tau = 6.28318530717958647692;
  const double step = tau * frequency / sample_rate;
  for (auto& sample : pcm) {
    sample = static_cast<opus_int16>(std::lrint(amplitude * std::sin(phase)));
    phase += step;
    if (phase >= tau) {
      phase -= tau;
    }
  }
}

} // namespace

int main() {
  int error = OPUS_OK;
  auto encoder = make_opus_encoder(sample_rate, 1, OPUS_APPLICATION_VOIP, &error);
  auto decoder = make_opus_decoder(sample_rate, 1, &error);
  require(encoder && decoder && error == OPUS_OK);

  opus_int32 enabled = -1;
  require(opus_encoder_ctl(encoder.get(), OPUS_GET_DTX(&enabled)) == OPUS_OK && enabled == 0);
  require(opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(2)) == OPUS_BAD_ARG);
  require(opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(1)) == OPUS_OK);
  require(opus_encoder_ctl(encoder.get(), OPUS_GET_DTX(&enabled)) == OPUS_OK && enabled == 1);

  std::array<opus_int16, frame_size> input{};
  std::array<opus_int16, frame_size> output{};
  std::array<unsigned char, 1276> packet{};
  double phase = 0;
  for (int frame = 0; frame < 15; ++frame) {
    fill_tone(input, phase, 180.0, 9000.0);
    require(opus_encode(encoder.get(), input.data(), frame_size, packet.data(), packet.size()) > 1);
  }

  input.fill(0);
  int dtx_packets = 0;
  int refresh_packets = 0;
  for (int frame = 0; frame < 40; ++frame) {
    const int length = opus_encode(encoder.get(), input.data(), frame_size, packet.data(), packet.size());
    require(length > 0);
    dtx_packets += length == 1;
    refresh_packets += frame >= 10 && length > 1;
    require(opus_decode(decoder.get(), packet.data(), length, output.data(), frame_size, 0) == frame_size);
  }
  require(dtx_packets >= 28);
  require(refresh_packets >= 1);

  opus_int32 in_dtx = 0;
  require(opus_encoder_ctl(encoder.get(), OPUS_GET_IN_DTX(&in_dtx)) == OPUS_OK && in_dtx == 1);
  fill_tone(input, phase, 180.0, 9000.0);
  require(opus_encode(encoder.get(), input.data(), frame_size, packet.data(), packet.size()) > 1);
  require(opus_encoder_ctl(encoder.get(), OPUS_GET_IN_DTX(&in_dtx)) == OPUS_OK && in_dtx == 0);

  require(opus_encoder_ctl(encoder.get(), OPUS_RESET_STATE) == OPUS_OK);
  phase = 0;
  for (int frame = 0; frame < 15; ++frame) {
    fill_tone(input, phase, 180.0, 9000.0);
    require(opus_encode(encoder.get(), input.data(), frame_size, packet.data(), packet.size()) > 1);
  }
  int tonal_dtx_packets = 0;
  for (int frame = 0; frame < 40; ++frame) {
    fill_tone(input, phase, 330.0, 220.0);
    tonal_dtx_packets += opus_encode(encoder.get(), input.data(), frame_size, packet.data(), packet.size()) == 1;
  }
  require(tonal_dtx_packets == 0);

  auto narrow_encoder = make_opus_encoder(8000, 1, OPUS_APPLICATION_VOIP, &error);
  auto wide_decoder = make_opus_decoder(sample_rate, 1, &error);
  require(narrow_encoder && wide_decoder && error == OPUS_OK);
  require(opus_encoder_ctl(narrow_encoder.get(), OPUS_SET_BITRATE(12000)) == OPUS_OK);
  require(opus_encoder_ctl(narrow_encoder.get(), OPUS_SET_DTX(1)) == OPUS_OK);

  std::array<opus_int16, 160> digital_silence;
  digital_silence.fill(3);
  double decoded_energy = 0;
  int digital_silence_packets = 0;
  for (int frame = 0; frame < 100; ++frame) {
    const int length = opus_encode(narrow_encoder.get(), digital_silence.data(), digital_silence.size(), packet.data(), packet.size());
    require(length > 0);
    digital_silence_packets += length == 1;
    require(opus_decode(wide_decoder.get(), packet.data(), length, output.data(), output.size(), 0) == frame_size);
    for (const auto sample : output) {
      decoded_energy += static_cast<double>(sample) * sample;
    }
  }
  require(digital_silence_packets >= 70);
  require(std::sqrt(decoded_energy / (100 * output.size())) < 100.0);
  std::cout << "dtx_behavior=PASS silence_packets=" << dtx_packets << " tonal_packets=" << tonal_dtx_packets
            << " digital_silence_packets=" << digital_silence_packets << '\n';
}
