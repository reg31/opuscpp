#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include "../src/opus_codec.cpp"

int main() {
  auto encoder = make_opus_encoder(48000, 1, OPUS_APPLICATION_VOIP, nullptr);
  auto decoder = make_opus_decoder(48000, 1, nullptr);
  if (!encoder || !decoder || opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)) ||
      opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(10)) || opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(24000)) ||
      opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(1)) || opus_encoder_ctl(encoder.get(), OPUS_SET_PACKET_LOSS_PERC(20)))
    return 1;
  std::uint32_t random = 17;
  for (int i = 0; i < 288000; ++i)
    random = random * 1664525u + 1013904223u;
  std::array<float, 960> input{}, output{};
  std::array<unsigned char, 7656> packet{};
  bool pending = false;
  for (int frame = 0; frame <= 44; ++frame) {
    const int samples = frame % 2 == 0 ? 480 : 960;
    for (int sample = 0; sample < samples; ++sample) {
      random = random * 1664525u + 1013904223u;
      const double phase = 6.283185307179586 * (frame * samples + sample) / 48000;
      input[sample] = static_cast<float>(.2 * std::sin(173 * phase) + .02 * (static_cast<int>(random >> 17) - 16384) / 16384.0);
    }
    const int size = opus_encode_float(encoder.get(), input.data(), samples, packet.data(), static_cast<int>(packet.size()));
    if (size <= 0 || opus_packet_get_nb_samples(packet.data(), size, 48000) != samples ||
        opus_decode_float(decoder.get(), packet.data(), size, output.data(), samples, 0) != samples)
      return 2;
    if (frame == 43) {
      auto* silk = static_cast<silk_encoder*>(encoder_silk_state(encoder.get()));
      const auto* states = silk_encoder_channel_states(silk);
      pending = silk->lbrr != nullptr && silk->lbrr->channels[0].flags[0] != 0 && states[0].sCmn.nb_subfr == 4;
    }
  }
  if (!pending)
    return 3;
  std::cout << "PASSED\n";
  return 0;
}
