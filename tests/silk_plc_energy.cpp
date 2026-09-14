#include "../src/opus_codec.cpp"
#include <cstdio>

// Signed excitation with equal magnitudes must retain the reference's truncation asymmetry.
int main() {
  std::array<opus_int32, 160> excitation;
  std::fill_n(excitation.begin(), 80, 1280);
  std::fill_n(excitation.begin() + 80, 80, -1280);
  const std::array<opus_int32, 2> gains{823296, 823296};
  opus_int32 positive_energy = 0, negative_energy = 0;
  int positive_shift = -1, negative_shift = -1;
  silk_PLC_energy(&positive_energy, &positive_shift, &negative_energy, &negative_shift, excitation, gains, 80, 2);
  // The scaled samples are +62 and -63, respectively, before squaring.
  if (positive_shift != 0 || negative_shift != 0 || positive_energy != 307520 || negative_energy != 317520) {
    std::fprintf(stderr, "PLC energy mismatch: %d/%d and %d/%d\n",
                 positive_energy, positive_shift, negative_energy, negative_shift);
    return 1;
  }
  std::puts("silk_plc_energy=PASS");
}
