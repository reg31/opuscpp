#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {

uint32_t read_be32(std::istream& input, bool& ok) {
  std::array<unsigned char, 4> b{};
  input.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
  ok = input.gcount() == static_cast<std::streamsize>(b.size());
  if (!ok) {
    return {};
  }
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) |
         static_cast<uint32_t>(b[3]);
}

int16_t f32_to_s16(float x) {
  int v = static_cast<int>(x * 32768.0f);
  v = std::clamp(v, -32768, 32767);
  return static_cast<int16_t>(v);
}

std::optional<int> parse_int(std::string_view value) {
  int parsed = 0;
  const auto* first = value.data();
  const auto* last = value.data() + value.size();
  const auto result = std::from_chars(first, last, parsed);
  if (result.ec != std::errc{} || result.ptr != last) {
    return std::nullopt;
  }
  return parsed;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: conformance_decode <rate> <channels> <in.bit> <out.pcm> <check_range 0|1>\n";
    return 1;
  }

  const auto rate_arg = parse_int(argv[1]);
  const auto channels_arg = parse_int(argv[2]);
  const auto check_range_arg = parse_int(argv[5]);
  if (!rate_arg || !channels_arg || !check_range_arg) {
    std::cerr << "Invalid numeric argument.\n";
    return 1;
  }
  const int rate = *rate_arg;
  const int channels = *channels_arg;
  const char* in_path = argv[3];
  const char* out_path = argv[4];
  const int check_range = *check_range_arg;

  if ((rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) || (channels != 1 && channels != 2)) {
    std::cerr << "Invalid rate/channels.\n";
    return 1;
  }

  std::ifstream input{in_path, std::ios::binary};
  if (!input) {
    std::cerr << "Cannot open input: " << in_path << '\n';
    return 1;
  }

  std::ofstream output{out_path, std::ios::binary};
  if (!output) {
    std::cerr << "Cannot open output: " << out_path << '\n';
    return 1;
  }

  int err = 0;
  auto dec = make_opus_decoder(rate, channels, &err);
  if (!dec || err != OPUS_OK) {
    std::cerr << "opus_decoder_create failed: " << err << '\n';
    return 1;
  }

  const int max_frame = rate * 2; // 120 ms max
  std::vector<unsigned char> packet(1275);
  std::vector<float> out(max_frame * channels);
  std::vector<int16_t> out16(max_frame * channels);

  int frame_index = 0;
  while (true) {
    bool ok = false;
    uint32_t len = read_be32(input, ok);
    if (!ok) {
      break;
    }
    uint32_t enc_final = read_be32(input, ok);
    if (!ok) {
      break;
    }

    if (len > packet.size()) {
      packet.resize(len);
    }

    if (len > 0u) {
      input.read(reinterpret_cast<char*>(packet.data()), static_cast<std::streamsize>(len));
      if (input.gcount() != static_cast<std::streamsize>(len)) {
        break;
      }
    }

    int out_samples = 0;
    if (len == 0u) {
      int last = 0;
      if (opus_decoder_ctl(dec.get(), OPUS_GET_LAST_PACKET_DURATION(&last)) != OPUS_OK || last <= 0) {
        last = rate / 50;
      }
      out_samples = opus_decode_float(dec.get(), nullptr, 0, out.data(), std::min(last, max_frame), 0);
    } else {
      out_samples = opus_decode_float(dec.get(), packet.data(), static_cast<int>(len), out.data(), max_frame, 0);
    }

    if (out_samples < 0) {
      std::cerr << "Decode failed in frame " << frame_index << ": " << out_samples << '\n';
      return 3;
    }
    if (out_samples > max_frame) {
      std::cerr << "Decoder returned too many samples in frame " << frame_index << ": " << out_samples << " > " << max_frame << '\n';
      return 7;
    }

    if (check_range && len > 0u && enc_final != 0u) {
      uint32_t dec_final = 0;
      if (opus_decoder_ctl(dec.get(), OPUS_GET_FINAL_RANGE(&dec_final)) != OPUS_OK) {
        std::cerr << "OPUS_GET_FINAL_RANGE failed in frame " << frame_index << '\n';
        return 4;
      }
      if (dec_final != enc_final) {
        std::cerr << "Error: Range coder state mismatch in frame " << frame_index << ": " << dec_final << " vs " << enc_final << '\n';
        return 5;
      }
    }

    int samples_total = out_samples * channels;
    for (int i = 0; i < samples_total; ++i) {
      out16[i] = f32_to_s16(out[i]);
    }
    output.write(reinterpret_cast<const char*>(out16.data()), static_cast<std::streamsize>(samples_total * sizeof(int16_t)));
    if (!output) {
      std::cerr << "Write failed.\n";
      return 6;
    }

    frame_index++;
  }

  std::cerr << "Decoded " << frame_index << " frames.\n";
  return 0;
}
