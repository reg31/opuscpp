#define main perceptual_validation_main
#include "perceptual_memory_validation.cpp"
#undef main

int main() {
  try {
    clip_data stereo{.label = "stereo scoring", .channels = 2, .samples = {}};
    stereo.samples.resize(4 * frame_size * 2);
    std::vector<float> identity(stereo.samples.size()), collapsed(stereo.samples.size()), swapped(stereo.samples.size());
    for (int i = 0; i < 4 * frame_size; ++i) {
      stereo.samples[2 * i] = static_cast<std::int16_t>(9000 * std::sin(2 * pi * 400 * i / sample_rate));
      stereo.samples[2 * i + 1] = static_cast<std::int16_t>(6000 * std::sin(2 * pi * 900 * i / sample_rate));
      identity[2 * i] = stereo.samples[2 * i] / 32768.f;
      identity[2 * i + 1] = stereo.samples[2 * i + 1] / 32768.f;
      collapsed[2 * i] = collapsed[2 * i + 1] = .5f * (identity[2 * i] + identity[2 * i + 1]);
      swapped[2 * i] = identity[2 * i + 1];
      swapped[2 * i + 1] = identity[2 * i];
    }
    totals identity_score{}, collapsed_score{}, swapped_score{};
    add_metrics(identity_score, stereo.samples, identity, 2);
    add_metrics(collapsed_score, stereo.samples, collapsed, 2);
    add_metrics(swapped_score, stereo.samples, swapped, 2);
    if (visqol_style(identity_score) - visqol_style(collapsed_score) < .01 ||
        visqol_style(identity_score) - visqol_style(swapped_score) < .01) {
      throw std::runtime_error("stereo spectral score failed to detect channel collapse or swapping");
    }
    options opt;
    opt.bitrate = 256000;
    for (int channels : {1, 2}) {
      clip_data clip{.label = "alignment", .channels = channels, .samples = {}};
      clip.samples.resize(static_cast<std::size_t>(6 * frame_size * channels));
      for (int i = 0; i < 6 * frame_size; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
          const double t = static_cast<double>(i) / sample_rate;
          const double signal = 7000 * std::sin(2 * pi * ((310 + 150 * ch) * t + 7000 * t * t)) +
                                2500 * std::sin(2 * pi * 1900 * t);
          clip.samples[static_cast<std::size_t>(i * channels + ch)] = static_cast<std::int16_t>(signal);
        }
      }
      for (int application : {OPUS_APPLICATION_VOIP, OPUS_APPLICATION_AUDIO, OPUS_APPLICATION_RESTRICTED_LOWDELAY}) {
        opt.application = application;
        for (bool pcm16 : {false, true}) {
          opt.pcm16 = pcm16;
          for (bool official : {false, true}) {
            const auto output = run_variant("alignment", clip, clip.samples, opt, official);
            if (output.decoded.size() != clip.samples.size() || output.score.sample_count != clip.samples.size() ||
                output.score.packets != 6) {
              throw std::runtime_error("incorrect aligned duration or flush frames included in packet statistics");
            }
            double signal_energy = 0, error_energy = 0;
            const auto tail_start = clip.samples.size() - static_cast<std::size_t>(120 * channels);
            for (std::size_t i = tail_start; i < clip.samples.size(); ++i) {
              const double reference = static_cast<double>(clip.samples[i]) / 32768.0;
              const double error = output.decoded[i] - reference;
              signal_energy += reference * reference;
              error_energy += error * error;
            }
            const double tail_snr = 10 * std::log10(signal_energy / std::max(error_energy, 1e-20));
            if (tail_snr < 10) {
              throw std::runtime_error("misaligned or missing decoded tail: snr=" + std::to_string(tail_snr));
            }
          }
        }
      }
    }
    std::cout << "perceptual_alignment=PASS (stereo negative controls; 24 roundtrips, three applications, float/PCM16)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "perceptual_alignment=FAIL: " << error.what() << '\n';
    return 1;
  }
}
