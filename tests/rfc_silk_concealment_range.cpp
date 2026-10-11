#include "opus_codec.h"
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

template <typename Sample>
bool check(const std::vector<std::vector<unsigned char>>& packets, int channels, std::ofstream& output) {
  int error = OPUS_OK;
  auto decoder = make_opus_decoder(16000, channels, &error);
  if (!decoder || error != OPUS_OK)
    return false;
  std::array<Sample, 640> pcm{};
  const std::array<unsigned char, 1> dtx{static_cast<unsigned char>(packets[0][0] & 0xFC)};
  for (int frame = 0; frame < 8; ++frame) {
    const bool loss = frame == 4 || frame == 6;
    const auto* data = loss ? nullptr : frame == 5 ? dtx.data()
                                                   : packets[frame == 7 ? 4 : frame].data();
    const int bytes = loss ? 0 : frame == 5 ? 1
                                            : static_cast<int>(packets[frame == 7 ? 4 : frame].size());
    int samples = 0;
    if constexpr (std::is_same_v<Sample, float>) {
      samples = opus_decode_float(decoder.get(), data, bytes, pcm.data(), 320, 0);
    } else {
      samples = opus_decode(decoder.get(), data, bytes, pcm.data(), 320, 0);
    }
    opus_uint32 range = 0;
    int duration = 0;
    if (samples != 320 || opus_decoder_ctl(decoder.get(), OPUS_GET_FINAL_RANGE(&range)) != OPUS_OK ||
        opus_decoder_ctl(decoder.get(), OPUS_GET_LAST_PACKET_DURATION(&duration)) != OPUS_OK || duration != 320 ||
        ((loss || frame == 5) && range != 0))
      return false;
    for (int i = 0; i < samples * channels; ++i)
      if (!std::isfinite(static_cast<double>(pcm[i])))
        return false;
    output.write(reinterpret_cast<const char*>(pcm.data()), samples * channels * sizeof(Sample));
    output.write(reinterpret_cast<const char*>(&range), sizeof(range));
    output.write(reinterpret_cast<const char*>(&duration), sizeof(duration));
    std::cout << "api=" << (std::is_same_v<Sample, float> ? "float" : "pcm16") << " channels=" << channels << " frame=" << frame << " bytes=" << bytes << " returned=" << samples << " range=" << range << " duration=" << duration << '\n';
  }
  return static_cast<bool>(output);
}

int main(int argc, char** argv) {
  if (argc != 3)
    return 1;
  std::ifstream input(argv[1], std::ios::binary);
  std::ofstream output(argv[2], std::ios::binary);
  if (!input || !output)
    return 1;
  std::vector<std::vector<unsigned char>> packets;
  for (int i = 0; i < 5; ++i) {
    std::array<unsigned char, 4> raw{};
    input.read(reinterpret_cast<char*>(raw.data()), 4);
    const auto bytes = raw[0] | (unsigned{raw[1]} << 8) | (unsigned{raw[2]} << 16) | (unsigned{raw[3]} << 24);
    if (!input || bytes < 2 || bytes > 1500)
      return 1;
    packets.emplace_back(bytes);
    input.read(reinterpret_cast<char*>(packets.back().data()), bytes);
    if (!input || (packets.back()[0] & 0xE0) != 0x40)
      return 1;
  }
  for (int channels : {1, 2}) {
    if (!check<float>(packets, channels, output) || !check<opus_int16>(packets, channels, output))
      return 2;
  }
  std::cout << "rfc_silk_concealment_range=PASS calls=32\n";
  return 0;
}
