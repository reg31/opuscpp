#include "opus_codec.h"

#include <array>
#include <iostream>

namespace {

[[nodiscard]] auto expect_eq(int actual, int expected, const char* message) -> bool {
  if (actual != expected) {
    std::cout << "FAIL " << message << " actual=" << actual << " expected=" << expected << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  auto ok = true;

  constexpr std::array<unsigned char, 4> code0_20ms{0x78, 1, 2, 3};
  constexpr std::array<unsigned char, 7> code1_20ms{0x79, 1, 2, 3, 4, 5, 6};
  constexpr std::array<unsigned char, 7> code2_20ms{0x7A, 2, 1, 2, 3, 4, 5};
  constexpr std::array<unsigned char, 8> code3_cbr_20ms{0x7B, 0x03, 1, 2, 3, 4, 5, 6};
  constexpr std::array<unsigned char, 10> code3_vbr_20ms{0x7B, 0x83, 1, 2, 3, 4, 5, 6, 7, 8};
  constexpr std::array<unsigned char, 2> code3_120ms{0x83, 48};
  constexpr std::array<unsigned char, 2> code3_too_long{0x7B, 7};

  ok &= expect_eq(opus_packet_get_nb_samples(code0_20ms.data(), static_cast<int>(code0_20ms.size()), 48000), 960, "code0 one 20ms frame");
  ok &= expect_eq(opus_packet_get_nb_samples(code1_20ms.data(), static_cast<int>(code1_20ms.size()), 48000), 1920,
                  "code1 two 20ms CBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code2_20ms.data(), static_cast<int>(code2_20ms.size()), 48000), 1920,
                  "code2 two 20ms VBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_cbr_20ms.data(), static_cast<int>(code3_cbr_20ms.size()), 48000), 2880,
                  "code3 three 20ms CBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_vbr_20ms.data(), static_cast<int>(code3_vbr_20ms.size()), 48000), 2880,
                  "code3 three 20ms VBR frames");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_120ms.data(), static_cast<int>(code3_120ms.size()), 48000), 5760,
                  "code3 accepts 120ms aggregate");
  ok &= expect_eq(opus_packet_get_nb_samples(code3_too_long.data(), static_cast<int>(code3_too_long.size()), 48000), OPUS_INVALID_PACKET,
                  "code3 rejects packets above 120ms");
  ok &= expect_eq(opus_packet_get_nb_samples(nullptr, 0, 48000), OPUS_BAD_ARG, "null/empty packet rejected");

  if (!ok) {
    return 1;
  }
  std::cout << "packet_duration_behavior=PASS\n";
  return 0;
}
