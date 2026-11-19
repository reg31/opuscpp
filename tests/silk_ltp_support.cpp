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
  std::array<float, 4 * silk_ltp_matrix_values> covariance{};
  std::array<float, 20> cross{};
  std::array<int, 4> lags{};
  lags.fill(4 * fs_khz);
  silk_find_LTP_FLP(covariance.data(), cross.data(), residual.data() + history, lags.data(), samples, subframes);
  const float inverse_energy = 1.0f / static_cast<float>(64 * energy_units);
  const float expected_covariance = static_cast<float>(64 * samples) * inverse_energy;
  const float expected_cross = static_cast<float>(64 * cross_units) * inverse_energy;
  for (int i = 0; i < silk_ltp_matrix_values; ++i) {
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

static int check_packed_order() {
  constexpr int length = 40;
  constexpr int pitch = 48;
  std::array<float, 160> signal;
  for (int i = 0; i < static_cast<int>(signal.size()); ++i)
    signal[i] = static_cast<float>((i * i + 3 * i) % 23 - 11);
  const auto* residual = signal.data() + 56;
  const auto* delayed = residual - pitch + 2;
  const std::array lags{pitch, pitch, 0, 0};
  std::array<float, 2 * 15> matrix;
  std::array<float, 2 * 5> cross;
  silk_find_LTP_FLP(matrix.data(), cross.data(), residual, lags.data(), length, 2);
  for (int subframe = 0; subframe < 2; ++subframe) {
    const int start = subframe * length;
    float target_energy = 0.0f;
    for (int i = 0; i < length + 5; ++i)
      target_energy += residual[start + i] * residual[start + i];
    auto dot = [&](int row, int column) {
      float product = 0.0f;
      for (int i = start; i < start + length; ++i)
        product += delayed[i - row] * delayed[i - column];
      return product;
    };
    const float scale = 1.0f / std::max(target_energy, 0.015f * (dot(0, 0) + dot(4, 4)) + 1.0f);
    int slot = subframe * 15;
    for (int row = 0; row < 5; ++row) {
      for (int column = row; column < 5; ++column) {
        if (matrix[slot++] != dot(row, column) * scale) {
          std::fprintf(stderr, "packed matrix mismatch in subframe %d at (%d,%d)\n", subframe, row, column);
          return 1;
        }
      }
      float product = 0.0f;
      for (int i = start; i < start + length; ++i)
        product += delayed[i - row] * residual[i];
      if (cross[subframe * 5 + row] != product * scale) {
        std::fprintf(stderr, "cross vector mismatch in subframe %d at %d\n", subframe, row);
        return 1;
      }
    }
  }
  return 0;
}

int main() {
  int failures = check_packed_order();
  int cases = 1;
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
