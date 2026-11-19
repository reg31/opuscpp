#include "../src/opus_codec.cpp"
#include <cstdio>

static opus_int32 reference_feedback(opus_int32 diff, opus_int32* data, const opus_int16* coefficients, int order, int warping_q16) {
  const auto smlawb = [](opus_int32 a, opus_int32 b, opus_int32 c) {
    return wrap_add(a, silk_mul_wb(b, c));
  };
  opus_int32 tmp2 = smlawb(diff, data[0], warping_q16);
  opus_int32 tmp1 = smlawb(data[0], wrap_subtract(data[1], tmp2), warping_q16);
  data[0] = tmp2;
  opus_int32 out = order >> 1;
  out = smlawb(out, tmp2, coefficients[0]);
  for (int j = 2; j < order; j += 2) {
    tmp2 = smlawb(data[j - 1], wrap_subtract(data[j], tmp1), warping_q16);
    data[j - 1] = tmp1;
    out = smlawb(out, tmp1, coefficients[j - 1]);
    tmp1 = smlawb(data[j], wrap_subtract(data[j + 1], tmp2), warping_q16);
    data[j] = tmp2;
    out = smlawb(out, tmp2, coefficients[j]);
  }
  data[order - 1] = tmp1;
  out = smlawb(out, tmp1, coefficients[order - 1]);
  return wrap_shift_left(out, 1);
}

int main() {
  std::uint32_t random_state = 123456789u;
  const auto random = [&]() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return std::bit_cast<opus_int32>(random_state);
  };
  int checks = 0;
  int failures = 0;
  for (int order = 2; order <= 24; order += 2) {
    for (bool warped : {false, true}) {
      const int warping_q16 = warped ? 24326 : 0;
      std::array<opus_int16, 24> coefficients;
      for (auto& coefficient : coefficients)
        coefficient = static_cast<opus_int16>(random());
      for (int trial = 0; trial < 200; ++trial) {
        opus_int32 actual[26], expected[26];
        for (int tap = 0; tap < 26; ++tap)
          actual[tap] = expected[tap] = trial % 7 == 0 && tap == 0 ? INT32_MAX : random();
        const opus_int32 diff = trial % 5 == 0 ? INT32_MIN : random();
        const auto result = silk_nsq_noise_shape_feedback(diff, actual, coefficients.data(), order, warping_q16);
        const auto reference = reference_feedback(diff, expected, coefficients.data(), order, warping_q16);
        bool matches = result == reference;
        for (int tap = 0; tap < 26; ++tap)
          matches = matches && actual[tap] == expected[tap];
        ++checks;
        if (!matches && ++failures <= 5)
          std::fprintf(stderr, "feedback mismatch: order=%d warped=%d trial=%d\n", order, warped, trial);
      }
    }
  }
  std::printf("nsq_shaping_feedback checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
