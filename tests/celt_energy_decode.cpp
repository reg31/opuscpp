#include <array>
#include <cmath>
#include <iostream>

#include "../src/opus_codec.cpp"

int main() {
  for (int lm : {2, 3}) {
    const int blocks = 1 << lm;
    const int frame_samples = celt_short_mdct_size * blocks;
    for (int channels : {1, 2}) {
      for (int depth : {0, 8, 16, 32}) {
        std::array<float, celt_max_channels * celt_max_frame_samples> spectrum{};
        std::array<unsigned char, celt_max_channels * celt_default_nb_ebands> masks{};
        std::array<float, celt_max_channels * celt_default_nb_ebands> energy{}, previous{};
        std::array<int, celt_default_nb_ebands> pulses{};
        pulses[0] = depth * blocks;
        for (int channel = 0; channel < channels; ++channel) {
          spectrum[channel * frame_samples] = 1;
          masks[channel] = 1;
        }
        anti_collapse(spectrum.data(), masks.data(), lm, channels, frame_samples, 0, 1, energy.data(), previous.data(), previous.data(), pulses.data(), 1);
        const float expected = .5f * std::exp2(-.125f * depth) / std::sqrt(static_cast<float>(blocks));
        for (int channel = 0; channel < channels; ++channel) {
          const auto* band = spectrum.data() + channel * frame_samples;
          float norm = band[0] * band[0];
          for (int block = 1; block < blocks; ++block) {
            const float actual = std::abs(band[block] / band[0]);
            if (std::abs(actual - expected) > 2e-7f) {
              std::cerr << "anti-collapse cap mismatch LM=" << lm << " depth=" << depth << " actual=" << actual << " expected=" << expected << '\n';
              return 1;
            }
            norm += band[block] * band[block];
          }
          if (std::abs(norm - 1.f) > 1e-6f)
            return 1;
        }
      }
    }
  }
  std::array<unsigned char, 2> packet{};
  std::array<float, 2 * celt_default_nb_ebands> energy{}, old_energy{}, error{};
  ec_enc enc;
  ec_enc_init(&enc, packet.data(), packet.size());
  for (int bit = 0; bit < 12; ++bit)
    ec_enc_bit_logp(&enc, 0, 1);
  ec_enc_bit_logp(&enc, 0, 3);
  process_coarse_energy<true>(17, 19, energy.data(), old_energy.data(), 16, ec_tell(&enc), e_prob_model[6].data(), error.data(), &enc, 1, 3, 0, 16);
  ec_enc_done(&enc);
  int status = OPUS_OK;
  auto decoder = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>{opus_decoder_create(48000, 1, &status), opus_decoder_destroy};
  if (!decoder || status != OPUS_OK)
    return 1;
  auto* celt = decoder_celt_state(decoder.get());
  celt->stream_channels = 1;
  celt->start = 17;
  celt->end = 19;
  ec_dec dec;
  ec_dec_init(&dec, packet.data(), packet.size());
  for (int bit = 0; bit < 12; ++bit)
    if (ec_dec_bit_logp(&dec, 1) != 0)
      return 1;
  std::array<float, celt_max_frame_samples> pcm{};
  const int decoded = celt_decode_with_ec(celt, packet.data(), packet.size(), pcm.data(), 960, &dec);
  const auto views = make_celt_decoder_views(celt, 960);
  if (decoded != 960 || dec.rng != enc.rng || views.oldBandE[17] != old_energy[17] || views.oldBandE[18] != old_energy[18]) {
    std::cerr << "false transient flag consumed an unavailable intra-energy symbol\n";
    return 1;
  }
  std::cout << "celt_energy_decode=PASS (anti-collapse cap, unit energy, false-transient bit boundary)\n";
}
