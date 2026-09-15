#include "../src/opus_codec.cpp"
#include <cstdio>

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
      for (auto& coefficient : coefficients) coefficient = static_cast<opus_int16>(random());
      for (int trial = 0; trial < 200; ++trial) {
        opus_int32 scalar[4][24];
        opus_int32 lanes[24][4];
        opus_int32 diff[4];
        for (int lane = 0; lane < 4; ++lane) {
          diff[lane] = trial % 5 == 0 ? (lane % 2 ? INT32_MIN : INT32_MAX) : random();
          for (int tap = 0; tap < 24; ++tap) {
            const auto value = trial % 7 == 0 && tap == 0 ? (lane % 2 ? INT32_MIN : INT32_MAX) : random();
            scalar[lane][tap] = value;
            lanes[tap][lane] = value;
          }
        }
        const auto result = warped
            ? silk_nsq_noise_shape_feedback_four<true>(diff[0], diff[1], diff[2], diff[3], lanes, coefficients.data(), order, warping_q16)
            : silk_nsq_noise_shape_feedback_four<false>(diff[0], diff[1], diff[2], diff[3], lanes, coefficients.data(), order, warping_q16);
        for (int lane = 0; lane < 4; ++lane) {
          const auto reference = warped
              ? silk_nsq_noise_shape_feedback<true>(diff[lane], scalar[lane], coefficients.data(), order, warping_q16)
              : silk_nsq_noise_shape_feedback<false>(diff[lane], scalar[lane], coefficients.data(), order);
          bool matches = result[static_cast<std::size_t>(lane)] == reference;
          for (int tap = 0; tap < 24; ++tap) matches = matches && lanes[tap][lane] == scalar[lane][tap];
          ++checks;
          if (!matches && ++failures <= 5)
            std::fprintf(stderr, "feedback mismatch: order=%d warped=%d trial=%d lane=%d\n", order, warped, trial, lane);
        }
      }
    }
  }
  std::printf("nsq_shaping_feedback checks=%d failures=%d\n", checks, failures);
  return failures ? 1 : 0;
}
