#include "../src/opus_codec.cpp"
#include <cstdio>
#include <vector>

// Keep the encoder predictor identical to the decoder across stereo-to-mono changes.
int main() {
  int checks = 0;
  for (int lm = 0; lm <= 3; ++lm)
    for (int profile = 0; profile < 3; ++profile) {
      int error = 0;
      auto encoder = make_opus_encoder(48000, 2, OPUS_APPLICATION_AUDIO, &error);
      auto decoder = make_opus_decoder(48000, 2, &error);
      if (!encoder || !decoder || error) return 1;
      auto* enc = encoder_celt_state(encoder.get());
      auto* dec = decoder_celt_state(decoder.get());
      const int n = 120 << lm;
      auto ev = make_celt_encoder_views(enc);
      auto dv = make_celt_decoder_views(dec, n);
      enc->stream_channels = dec->stream_channels = 1;
      enc->end = dec->end = celt_default_nb_ebands;
      enc->complexity = 0;
      enc->bitrate = 128000;
      enc->vbr = 0;
      enc->prediction_disabled = false;
      enc->delayedIntra = 0;
      for (int band = 0; band < celt_default_nb_ebands; ++band) {
        const float left = profile == 0 ? -9.f : profile == 1 ? 0.f : -static_cast<float>(band % 8);
        const float right = profile == 0 ? 0.f : profile == 1 ? -9.f : -static_cast<float>((band + 4) % 8);
        ev.oldBandE[band] = dv.oldBandE[band] = left;
        ev.oldBandE[band + celt_default_nb_ebands] = dv.oldBandE[band + celt_default_nb_ebands] = right;
      }
      std::vector<float> input(2 * n), output(2 * n);
      for (int i = 0; i < n; ++i) {
        input[2 * i] = 1638.4f * std::sin(i * .09f);
        input[2 * i + 1] = 6553.6f * std::sin(i * .14f);
      }
      std::array<unsigned char, 1275> packet;
      const int bytes = celt_encode_with_ec(enc, input.data(), n, packet.data(), packet.size(), nullptr);
      if (bytes <= 0 || celt_decode_with_ec(dec, packet.data(), bytes, output.data(), n, nullptr, nullptr) != n) return 2;
      for (int band = 0; band < 2 * celt_default_nb_ebands; ++band) {
        if (!std::isfinite(ev.oldBandE[band]) || !std::isfinite(dv.oldBandE[band]) || std::abs(ev.oldBandE[band] - dv.oldBandE[band]) > 1e-6f) {
          std::fprintf(stderr, "energy state mismatch lm=%d profile=%d band=%d encoder=%g decoder=%g\n", lm, profile, band, ev.oldBandE[band], dv.oldBandE[band]);
          return 3;
        }
      }
      ++checks;
    }
  std::printf("energy_state_checks=%d PASS\n", checks);
}
