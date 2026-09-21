#include "opus_codec.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#if !defined(OPUSCPP_ENABLE_TEST_HOOKS)
#error "hybrid_transient_budget requires -DOPUSCPP_ENABLE_TEST_HOOKS"
#endif

int opuscpp_test_hybrid_target(int base_target, int LM, int silk_offset, float tf_estimate) noexcept;

namespace {

void expect_equal(int got, int want, const std::string& what) {
  if (got != want) {
    throw std::runtime_error(what + ": got " + std::to_string(got) + " want " + std::to_string(want));
  }
}

void expect_true(bool value, const std::string& what) {
  if (!value) {
    throw std::runtime_error(what);
  }
}

}

int main() {
  constexpr int lm = 3;
  constexpr int base = 1000;
  constexpr int offset_low = 12 << 3;
  constexpr int offset_high = 18 << 3;
  constexpr int strong_floor = 50 << 3;

  expect_equal(opuscpp_test_hybrid_target(base, lm, 0, 0.0f), base + offset_low - 100, "tf=0 offset");
  expect_equal(opuscpp_test_hybrid_target(base, lm, 0, 0.25f), base + offset_low, "tf=pivot");
  expect_equal(opuscpp_test_hybrid_target(base, lm, 0, 0.75f), base + offset_low + 200, "tf=0.75 term");
  expect_equal(opuscpp_test_hybrid_target(base, lm, 0, 0.50f), base + offset_low + 100, "tf=0.5 term");
  expect_equal(opuscpp_test_hybrid_target(base, lm, 150, 0.0f), base - offset_high - 100, "silk>100 offset");
  expect_equal(opuscpp_test_hybrid_target(base, 2, 0, 0.0f), base + (12 << 3 >> 1) - 100, "LM scaling");

  const int strong = opuscpp_test_hybrid_target(10, lm, 0, 0.80f);
  expect_true(strong == strong_floor, "strong-transient floor");
  expect_true(opuscpp_test_hybrid_target(base, lm, 0, 0.80f) > base + offset_low, "strong term grows target");
  expect_true(opuscpp_test_hybrid_target(10, lm, 0, 0.70f) < strong_floor, "floor is strict at 0.7");
  expect_equal(opuscpp_test_hybrid_target(10, lm, 0, 0.75f), strong_floor, "floor applies above 0.7");
  expect_true(opuscpp_test_hybrid_target(10, lm, 0, 0.10f) < strong_floor, "weak transient stays below floor");

  std::cout << "hybrid_transient_budget=PASS\n";
  return 0;
}
