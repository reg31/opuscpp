#include "../src/opus_codec.cpp"
#include <cstdio>

int main() {
  std::array<opus_uint16, 160> excitation;
  std::array<opus_int8, 160> high{};
  std::fill_n(excitation.begin(), 80, 20);
  std::fill_n(excitation.begin() + 80, 80, 65516);
  std::fill_n(high.begin() + 80, 80, -1);
  const std::array<opus_int32, 2> gains{823296, 823296};
  opus_int32 positive_energy = 0, negative_energy = 0;
  int positive_shift = -1, negative_shift = -1;
  silk_PLC_energy(&positive_energy, &positive_shift, &negative_energy, &negative_shift, excitation, high, gains, 80, 2);
  if (positive_shift != 0 || negative_shift != 0 || positive_energy != 307520 || negative_energy != 317520) {
    std::fprintf(stderr, "PLC energy mismatch: %d/%d and %d/%d\n",
                 positive_energy, positive_shift, negative_energy, negative_shift);
    return 1;
  }
  std::puts("silk_plc_energy=PASS");
}
