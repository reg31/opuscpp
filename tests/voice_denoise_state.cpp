#include "../src/opus_codec.cpp"
#include <cstdio>
#include <vector>

static int broadband_cases = 0;

static int check_case(int rate, int quarters, int complexity) {
  int error = OPUS_OK;
  auto encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &error), opus_encoder_destroy};
  auto decoder = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>{opus_decoder_create(rate, 1, &error), opus_decoder_destroy};
  if (!encoder || !decoder || error != OPUS_OK)
    return 1;
  if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(20000)) != OPUS_OK ||
      opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(complexity)) != OPUS_OK ||
      opus_encoder_ctl(encoder.get(), OPUSCPP_SET_VOICE_DENOISE(1)) != OPUS_OK)
    return 2;
  const int count = rate * quarters / 400;
  std::vector<float> input(count), decoded(count + 2, 42.f);
  std::array<unsigned char, 1502> packet;
  std::uint32_t random = 67168251;
  const int noise_frames = std::max(8, rate / (3 * count));
  for (int frame = 0; frame < noise_frames + 8; ++frame) {
    for (int i = 0; i < count; ++i) {
      random = random * 1664525u + 1013904223u;
      const float noise = (static_cast<int>(random >> 16) - 32768) * (.06f / 32768.f);
      const float speech = frame < noise_frames ? 0.f : .2f * std::sin(6.283185307179586 * 180 * (frame * count + i) / rate);
      input[i] = noise + speech;
    }
    packet.fill(0xa5);
    const int length = opus_encode_float(encoder.get(), input.data(), count, packet.data() + 1, 1500);
    if (length <= 0 || packet.front() != 0xa5 || packet.back() != 0xa5)
      return 3;
    const int samples = opus_decode_float(decoder.get(), packet.data() + 1, length, decoded.data() + 1, count, 0);
    if (samples != count || decoded.front() != 42.f || decoded.back() != 42.f)
      return 4;
    for (int i = 0; i < samples; ++i)
      if (!std::isfinite(decoded[i + 1]))
        return 5;
    const auto& state = *encoder->voice_denoise;
    if (!std::isfinite(state.noise_variance) || state.noise_variance < 0)
      return 6;
    for (float gain : state.gain)
      if (!std::isfinite(gain) || gain < 0 || gain > 1)
        return 7;
  }
  broadband_cases += encoder->voice_denoise->model == VoiceDenoiseModel::broadband;
  if (opus_encoder_ctl(encoder.get(), OPUS_RESET_STATE) != OPUS_OK)
    return 8;
  const auto& state = *encoder->voice_denoise;
  if (state.model != VoiceDenoiseModel::undecided || state.noise_variance != 0 || state.gain != std::array<float, 3>{1, 1, 1})
    return 9;
  if (opus_encoder_ctl(encoder.get(), OPUSCPP_SET_VOICE_DENOISE(0)) != OPUS_OK)
    return 10;
  int enabled = -1;
  if (opus_encoder_ctl(encoder.get(), OPUSCPP_GET_VOICE_DENOISE(&enabled)) != OPUS_OK || enabled != 0)
    return 11;
  return 0;
}

static int check_silence_reentry(int fec, bool tiny_packet) {
  int error = OPUS_OK;
  const auto make_encoder = [&] {
    return std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error), opus_encoder_destroy};
  };
  auto encoder = make_encoder();
  auto reference = make_encoder();
  if (!encoder || !reference || error != OPUS_OK)
    return 1;
  for (auto* state : {encoder.get(), reference.get()})
    if (opus_encoder_ctl(state, OPUS_SET_BITRATE(20000)) || opus_encoder_ctl(state, OPUS_SET_COMPLEXITY(10)) ||
        opus_encoder_ctl(state, OPUSCPP_SET_VOICE_DENOISE(1)) || opus_encoder_ctl(state, OPUS_SET_INBAND_FEC(fec)) ||
        opus_encoder_ctl(state, OPUS_SET_PACKET_LOSS_PERC(fec ? 10 : 0)))
      return 2;
  std::array<float, 960> input{};
  std::array<unsigned char, 1500> packet{}, expected{};
  std::uint32_t random = 67168251;
  for (int frame = 0; frame < 12; ++frame) {
    for (auto& sample : input) {
      random = random * 1664525U + 1013904223U;
      sample = (static_cast<int>(random >> 16) - 32768) * (.06f / 32768.f);
    }
    for (auto* state : {encoder.get(), reference.get()})
      if (opus_encode_float(state, input.data(), input.size(), packet.data(), packet.size()) <= 0)
        return 3;
  }
  if (encoder->voice_denoise->model != VoiceDenoiseModel::broadband || reference->voice_denoise->model != VoiceDenoiseModel::broadband)
    return 4;
  input.fill(0);
  for (auto* state : {encoder.get(), reference.get()}) {
    if (opus_encoder_ctl(state, OPUS_SET_BITRATE(tiny_packet ? 20000 : 1000)) || opus_encoder_ctl(state, OPUS_SET_COMPLEXITY(fec ? 5 : 0)) ||
        opus_encode_float(state, input.data(), input.size(), packet.data(), tiny_packet ? 2 : packet.size()) <= 0)
      return 5;
  }
  const auto& denoiser = *encoder->voice_denoise;
  const bool cleared = denoiser.model == VoiceDenoiseModel::undecided && denoiser.noise_variance == 0 && !denoiser.initialized &&
                       denoiser.gain == std::array<float, 3>{1, 1, 1} && denoiser.target_gain == std::array<float, 3>{1, 1, 1};
  reset_voice_denoise_state(*reference->voice_denoise);
  for (auto* state : {encoder.get(), reference.get()})
    if (opus_encoder_ctl(state, OPUS_SET_BITRATE(20000)) || opus_encoder_ctl(state, OPUS_SET_COMPLEXITY(10)))
      return 6;
  bool equal = true;
  for (int frame = 0; frame < 3; ++frame) {
    for (std::size_t i = 0; i < input.size(); ++i)
      input[i] = .035f * std::sin(6.283185307179586f * 180.f * static_cast<float>(frame * input.size() + i) / 48000.f);
    const int length = opus_encode_float(encoder.get(), input.data(), input.size(), packet.data(), packet.size());
    const int expected_length = opus_encode_float(reference.get(), input.data(), input.size(), expected.data(), expected.size());
    if (length <= 0 || expected_length <= 0)
      return 7;
    equal = equal && length == expected_length && std::equal(packet.begin(), packet.begin() + length, expected.begin());
    opus_uint32 range = 0, expected_range = 0;
    if (opus_encoder_ctl(encoder.get(), OPUS_GET_FINAL_RANGE(&range)) || opus_encoder_ctl(reference.get(), OPUS_GET_FINAL_RANGE(&expected_range)))
      return 8;
    equal = equal && range == expected_range;
  }
  if (!cleared || !equal) {
    std::fprintf(stderr, "Denoiser re-entry: fec=%d tiny_packet=%d cleared=%d packet_equal=%d\n", fec, tiny_packet, cleared, equal);
    return 9;
  }
  return 0;
}

int main() {
  int cases = 0;
  for (int rate : {8000, 12000, 16000, 24000, 48000}) {
    for (int quarters : {1, 2, 4, 8, 16, 24}) {
      for (int complexity : {0, 5, 10}) {
        const int error = check_case(rate, quarters, complexity);
        if (error != 0) {
          std::fprintf(stderr, "denoiser failure: rate=%d frame_quarters=%d complexity=%d check=%d\n", rate, quarters, complexity, error);
          return 1;
        }
        ++cases;
      }
    }
  }
  for (int fec : {0, 1, 2})
    for (bool tiny_packet : {false, true})
      if (check_silence_reentry(fec, tiny_packet) != 0)
        return 13;
  std::puts("Low-rate and small-packet silence/re-entry: 6 cases passed");
  if (broadband_cases == 0)
    return 12;
  std::printf("Broadband model exercised in %d configurations\n", broadband_cases);
  std::printf("Denoiser state, bounds and roundtrip: %d cases passed; state=%zu bytes\n", cases, sizeof(VoiceDenoiseState));
}
