#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
#include "../src/opus_codec.cpp"

struct settings {
  int rate = 48000, channels = 2, frame_ms = 20, complexity = 9, application = OPUS_APPLICATION_AUDIO;
  int dtx = 0, fec = 0, loss = 0, constrained = 1, vbr = 1;
};

bool check(settings config, int pattern, bool should_activate) {
  int status = OPUS_OK;
  auto enc = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(config.rate, config.channels, config.application, &status), opus_encoder_destroy};
  auto dec = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>{opus_decoder_create(config.rate, config.channels, &status), opus_decoder_destroy};
  if (!enc || !dec || status != OPUS_OK)
    return false;
  if (opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(64000)) || opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY(config.complexity)) ||
      opus_encoder_ctl(enc.get(), OPUS_SET_DTX(config.dtx)) || opus_encoder_ctl(enc.get(), OPUS_SET_INBAND_FEC(config.fec)) ||
      opus_encoder_ctl(enc.get(), OPUS_SET_PACKET_LOSS_PERC(config.loss)) || opus_encoder_ctl(enc.get(), OPUS_SET_VBR_CONSTRAINT(config.constrained)) ||
      opus_encoder_ctl(enc.get(), OPUS_SET_VBR(config.vbr)))
    return false;
  const int framesize = config.rate * config.frame_ms / 1000;
  std::vector<opus_int16> input(framesize * config.channels), output(input.size());
  std::array<unsigned char, 1500> packet{};
  std::vector<unsigned char> first_packet;
  bool activated = false;
  unsigned random = 1;
  const auto tone = [](double t) {
    return .18 * std::sin(6.283185307179586 * 233.08 * t) + .10 * std::sin(6.283185307179586 * 698.46 * t);
  };
  for (int frame = 0; frame <= 150; ++frame) {
    if (frame == 150 && opus_encoder_ctl(enc.get(), OPUS_RESET_STATE) != OPUS_OK)
      return false;
    for (int i = 0; i < framesize; ++i) {
      const double t = ((frame == 150 ? 0 : frame) * framesize + i) / static_cast<double>(config.rate);
      double left = tone(t);
      double right = .18 * std::sin(6.283185307179586 * 311.13 * t) + .10 * std::sin(6.283185307179586 * 1046.5 * t);
      if (pattern == 1)
        right = left;
      if (pattern == 2)
        right = -left;
      if (pattern == 3)
        right = tone(t - 73. / config.rate);
      if (pattern == 4 && frame < 5) {
        random = random * 1664525u + 1013904223u;
        left = static_cast<int>(random >> 16) / 65535. - .5;
        random = random * 1664525u + 1013904223u;
        right = static_cast<int>(random >> 16) / 65535. - .5;
      }
      if (pattern == 5)
        left = right = 0;
      input[i * config.channels] = static_cast<opus_int16>(std::lrint(32767 * left));
      if (config.channels == 2)
        input[i * 2 + 1] = static_cast<opus_int16>(std::lrint(32767 * right));
    }
    const int bytes = opus_encode(enc.get(), input.data(), framesize, packet.data(), packet.size());
    if (bytes <= 0 || opus_decode(dec.get(), packet.data(), bytes, output.data(), framesize, 0) != framesize)
      return false;
    if (frame == 0)
      first_packet.assign(packet.begin(), packet.begin() + bytes);
    if (frame == 150) {
      if (encoder_celt_state(enc.get())->stereo_policy_celt || enc->stereo_recovery_frames != 0)
        return false;
      if (pattern != 4 && (bytes != static_cast<int>(first_packet.size()) || !std::equal(first_packet.begin(), first_packet.end(), packet.begin())))
        return false;
    } else {
      activated |= encoder_celt_state(enc.get())->stereo_policy_celt;
    }
  }
  if (activated != should_activate) {
    std::cerr << "activation mismatch rate=" << config.rate << " channels=" << config.channels << " frame_ms=" << config.frame_ms << " complexity=" << config.complexity << " application=" << config.application << " pattern=" << pattern << '\n';
    return false;
  }
  return true;
}

int main() {
  if (!check({}, 0, true))
    return 1;
  for (int pattern = 1; pattern <= 5; ++pattern)
    if (!check({}, pattern, false))
      return 1;
  for (const auto config : std::array{settings{.complexity = 0}, settings{.complexity = 4}, settings{.complexity = 5}, settings{.complexity = 8}, settings{.rate = 24000}, settings{.frame_ms = 10},
                                      settings{.frame_ms = 40}, settings{.dtx = 1}, settings{.fec = 1, .loss = 10}, settings{.loss = 10}, settings{.constrained = 0},
                                      settings{.vbr = 0}, settings{.channels = 1}, settings{.application = OPUS_APPLICATION_VOIP}, settings{.application = OPUS_APPLICATION_RESTRICTED_LOWDELAY}}) {
    if (!check(config, 0, false))
      return 1;
  }
  std::cout << "stereo_policy_guard=PASS (activation, copies, unstable startup, protected settings, reset, roundtrip)\n";
}
