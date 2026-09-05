#include <array>
#include <iostream>
#include <limits>

#include "../src/opus_codec.cpp"

static bool check_silk_reconstruction() {
  for (const int complexity : {0, 5, 9, 10}) {
    for (const int bitrate : {16000, 24000, 32000, 48000}) {
      int error = OPUS_OK;
      const auto encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error), opus_encoder_destroy};
      const auto decoder = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>{opus_decoder_create(16000, 1, &error), opus_decoder_destroy};
      if (!encoder || !decoder || error || opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)) || opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(complexity)))
        return false;
      encoder->audio_preprocess_mode = audio_preprocess_speech;
      std::array<opus_int16, 320> input, output;
      std::array<opus_int16, 13> tail{};
      std::array<unsigned char, 1500> packet;
      opus_uint32 random = 1;
      for (int frame = 0; frame < 200; ++frame) {
        for (int i = 0; i < 320; ++i) {
          const double t = (frame * 320 + i) / 16000.;
          random = random * 1664525U + 1013904223U;
          const double noise = (static_cast<int>(random >> 16) - 32768) * .005;
          const double phase = 2 * 3.141592653589793 * (140 * t + 3 * std::sin(2 * t));
          input[i] = static_cast<opus_int16>((.6 + .4 * std::sin(11 * t)) * (7000 * std::sin(phase) + 2500 * std::sin(2 * phase) + 1500 * std::sin(3 * phase)) + noise);
        }
        const int bytes = opus_encode(encoder.get(), input.data(), 320, packet.data(), packet.size());
        if (bytes <= 0 || opus_decode(decoder.get(), packet.data(), bytes, output.data(), 320, 0) != 320)
          return false;
        const auto& state = silk_encoder_channel_states(static_cast<silk_encoder*>(encoder_silk_state(encoder.get())))[0].sCmn;
        const auto& decoded_state = static_cast<silk_decoder*>(decoder_silk_state(decoder.get()))->channel_state;
        if (state.fs_kHz != 16 || encoder->mode != opus_mode_silk_only || decoded_state.resampler_state.inputDelay + 1 != static_cast<int>(tail.size()))
          return false;
        const auto* samples = state.sNSQ.xq + state.ltp_mem_length - state.frame_length;
        if (frame >= 10) {
          for (std::size_t i = 0; i < output.size(); ++i) {
            if (output[i] != (i < tail.size() ? tail[i] : samples[i - tail.size()]))
              return false;
          }
        }
        std::copy_n(samples + state.frame_length - tail.size(), tail.size(), tail.data());
      }
    }
  }
  return true;
}

int main() {
  if (!check_silk_reconstruction()) {
    std::cerr << "SILK encoder/decoder reconstruction mismatch\n";
    return 1;
  }
  const auto check_fraction = [](opus_uint32 bits) {
    opus_int32 leading = 0, fraction = 0;
    silk_CLZ_FRAC(std::bit_cast<opus_int32>(bits), &leading, &fraction);
    int expected_leading = 32;
    for (auto value = bits; value != 0; value >>= 1)
      --expected_leading;
    const auto expected_fraction = ((static_cast<std::uint64_t>(bits) << expected_leading) >> 24) & 127;
    return leading == expected_leading && static_cast<std::uint64_t>(fraction) == expected_fraction;
  };
  opus_uint32 mask = 0, random = 1;
  if (!check_fraction(0))
    return 1;
  for (unsigned bits = 0; bits < 32; ++bits) {
    if (low_bits_mask(bits) != mask || !check_fraction(opus_uint32{1} << bits) || !check_fraction(mask))
      return 1;
    mask = (mask << 1) | 1;
  }
  for (int i = 0; i < 200000; ++i) {
    random = random * 1664525U + 1013904223U;
    if (!check_fraction(random))
      return 1;
  }
  int status = OPUS_OK;
  auto encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &status), opus_encoder_destroy};
  if (!encoder || status != OPUS_OK || opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(16000)) != OPUS_OK)
    return 1;
  std::array<opus_int16, 960> speech{};
  std::array<unsigned char, 1500> packet{};
  for (int i = 0; i < 960; ++i)
    speech[i] = static_cast<opus_int16>((i % 50 - 25) * 200);
  if (opus_encode(encoder.get(), speech.data(), 960, packet.data(), packet.size()) <= 0 || encoder->mode == opus_mode_celt_only)
    return 1;
  auto& state = silk_encoder_channel_states(static_cast<silk_encoder*>(encoder_silk_state(encoder.get())))[0].sCmn;
  state.frameCounter = std::numeric_limits<decltype(state.frameCounter)>::max();
  for (const auto expected : {opus_uint32{0}, opus_uint32{1}}) {
    if (opus_encode(encoder.get(), speech.data(), 960, packet.data(), packet.size()) <= 0 || state.frameCounter != expected)
      return 1;
  }
  for (int order : {0, 2, 4, 6, 8, 10, 12, 16, 24}) {
    std::array<float, 25> correlation;
    std::array<float, 24> reflection;
    correlation.fill(std::numeric_limits<float>::quiet_NaN());
    reflection.fill(1.f);
    correlation[0] = .75f;
    std::fill_n(correlation.begin() + 1, order, 0.f);
    if (silk_schur_FLP(reflection.data(), correlation.data(), order) != .75f)
      return 1;
    for (int i = 0; i < 24; ++i) {
      if (reflection[i] != (i < order ? 0.f : 1.f))
        return 1;
    }
  }
  std::array<float, 96> input;
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<float>(static_cast<int>(i * i % 101) - 50) * .125f;
  }
  for (int order : {6, 8, 10, 12, 16}) {
    std::array<float, 16> coefficients;
    coefficients.fill(std::numeric_limits<float>::quiet_NaN());
    for (int i = 0; i < order; ++i) {
      coefficients[i] = static_cast<float>(i - 3) * .015625f;
    }
    for (int length : {order, order + 1, static_cast<int>(input.size())}) {
      std::array<float, 96> actual;
      actual.fill(std::numeric_limits<float>::quiet_NaN());
      silk_LPC_analysis_filter_FLP(actual.data(), coefficients.data(), input.data(), length, order);
      for (int i = 0; i < length; ++i) {
        float expected = 0;
        if (i >= order) {
          float prediction = 0;
          for (int tap = 0; tap < order; ++tap) {
            prediction += input[i - tap - 1] * coefficients[tap];
          }
          expected = input[i] - prediction;
        }
        if (actual[i] != expected) {
          std::cerr << "LPC filter mismatch order=" << order << " length=" << length << " sample=" << i << '\n';
          return 1;
        }
      }
    }
  }
  for (int samples : {240, 360, 600, 1080}) {
    std::array<float, celt_max_frame_samples + celt_default_overlap> pcm{};
    std::array<float, celt_lpc_order> denominator{}, memory{};
    denominator[0] = -.5f;
    memory[0] = .25f;
    for (int i = 0; i < samples; ++i)
      pcm[i] = static_cast<float>(i % 17 - 8) * .125f;
    const auto original = pcm;
    celt_iir(pcm.data(), denominator.data(), pcm.data(), samples, memory.data());
    float previous = .25f;
    for (int i = 0; i < samples; ++i) {
      const float expected = original[i] + .5f * previous;
      if (pcm[i] != expected) {
        std::cerr << "CELT IIR mismatch length=" << samples << " sample=" << i << '\n';
        return 1;
      }
      previous = expected;
    }
  }
  std::cout << "lpc_analysis_filter=PASS (SILK encoder/decoder reconstruction; bit helpers; seed wrap; Schur initialization; SILK orders 6/8/10/12/16; CELT PLC including overlap)\n";
}
