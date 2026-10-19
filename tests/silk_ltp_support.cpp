#include "../src/opus_codec.cpp"
#include <cstdio>

static int check_support(int fs_khz, int subframes, int changed_sample, bool joint_support) {
  constexpr int history = 256;
  constexpr float amplitude = 8.0f;
  const int samples = 5 * fs_khz;
  std::array<float, 1024> residual{};
  std::fill_n(residual.begin(), history + samples, amplitude);
  auto* target = residual.data() + history;
  int energy_units = samples;
  int cross_units = samples;
  if (joint_support) {
    std::fill_n(target + samples, 5, amplitude);
    energy_units += 5;
  } else if (changed_sample == samples - 1) {
    target[changed_sample] = 2.0f * amplitude;
    energy_units += 3;
    cross_units += 1;
  } else if (changed_sample >= 0) {
    target[changed_sample] = amplitude;
    if (changed_sample < samples + 5)
      energy_units += 1;
  }
  std::array<float, 100> covariance{};
  std::array<float, 20> cross{};
  std::array<int, 4> lags{};
  lags.fill(4 * fs_khz);
  silk_find_LTP_FLP(covariance.data(), cross.data(), residual.data() + history, lags.data(), samples, subframes);
  const float inverse_energy = 1.0f / static_cast<float>(64 * energy_units);
  const float expected_covariance = static_cast<float>(64 * samples) * inverse_energy;
  const float expected_cross = static_cast<float>(64 * cross_units) * inverse_energy;
  for (int i = 0; i < 25; ++i) {
    if (covariance[i] != expected_covariance) {
      std::fprintf(stderr, "fs=%d subframes=%d sample=%d joint=%d covariance[%d]=%.9g expected=%.9g\n",
                   fs_khz, subframes, changed_sample, joint_support, i, covariance[i], expected_covariance);
      return 1;
    }
  }
  for (int i = 0; i < 5; ++i) {
    if (cross[i] != expected_cross) {
      std::fprintf(stderr, "fs=%d subframes=%d sample=%d joint=%d cross[%d]=%.9g expected=%.9g\n",
                   fs_khz, subframes, changed_sample, joint_support, i, cross[i], expected_cross);
      return 1;
    }
  }
  return 0;
}

int main() {
  int failures = 0;
  int cases = 0;
  for (const int fs_khz : {8, 12, 16}) {
    const int samples = 5 * fs_khz;
    for (const int subframes : {2, 4}) {
      for (const int sample : {-1, samples - 1, samples, samples + 1, samples + 2, samples + 3, samples + 4, samples + 5}) {
        failures += check_support(fs_khz, subframes, sample, false);
        ++cases;
      }
      failures += check_support(fs_khz, subframes, -1, true);
      ++cases;
    }
  }
  std::printf("silk_ltp_support cases=%d failures=%d %s\n", cases, failures, failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
