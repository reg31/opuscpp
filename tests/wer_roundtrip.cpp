#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct wav_data final {
  int sample_rate = 0;
  int channels = 0;
  std::vector<opus_int16> samples;
};

[[nodiscard]] auto read_u16(std::istream& input) -> std::uint16_t {
  std::array<unsigned char, 2> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

[[nodiscard]] auto read_u32(std::istream& input) -> std::uint32_t {
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<std::uint32_t>(bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24));
}

auto write_u16(std::ostream& output, std::uint16_t value) -> void {
  const std::array<unsigned char, 2> bytes{static_cast<unsigned char>(value & 0xFF), static_cast<unsigned char>((value >> 8) & 0xFF)};
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

auto write_u32(std::ostream& output, std::uint32_t value) -> void {
  const std::array<unsigned char, 4> bytes{static_cast<unsigned char>(value & 0xFF), static_cast<unsigned char>((value >> 8) & 0xFF),
                                          static_cast<unsigned char>((value >> 16) & 0xFF), static_cast<unsigned char>((value >> 24) & 0xFF)};
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] auto read_wav(const std::string& path) -> wav_data {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open input wav: " + path);
  }

  std::array<char, 4> tag{};
  input.read(tag.data(), 4);
  if (std::string_view{tag.data(), tag.size()} != "RIFF") {
    throw std::runtime_error("not a RIFF wav: " + path);
  }
  (void)read_u32(input);
  input.read(tag.data(), 4);
  if (std::string_view{tag.data(), tag.size()} != "WAVE") {
    throw std::runtime_error("not a WAVE file: " + path);
  }

  auto result = wav_data{};
  auto audio_format = std::uint16_t{};
  auto bits_per_sample = std::uint16_t{};
  std::vector<unsigned char> pcm_bytes;

  while (input && (!pcm_bytes.size() || result.sample_rate == 0)) {
    input.read(tag.data(), 4);
    if (!input) {
      break;
    }
    const auto chunk_size = read_u32(input);
    const auto chunk_start = input.tellg();
    const auto chunk_name = std::string_view{tag.data(), tag.size()};
    if (chunk_name == "fmt ") {
      audio_format = read_u16(input);
      result.channels = read_u16(input);
      result.sample_rate = static_cast<int>(read_u32(input));
      (void)read_u32(input);
      (void)read_u16(input);
      bits_per_sample = read_u16(input);
    } else if (chunk_name == "data") {
      pcm_bytes.resize(chunk_size);
      input.read(reinterpret_cast<char*>(pcm_bytes.data()), static_cast<std::streamsize>(pcm_bytes.size()));
    }
    input.seekg(chunk_start + static_cast<std::streamoff>(chunk_size + (chunk_size & 1U)));
  }

  if (audio_format != 1 || bits_per_sample != 16 || result.sample_rate != 48000 || (result.channels != 1 && result.channels != 2)) {
    throw std::runtime_error("WER roundtrip expects 48 kHz mono/stereo PCM16 WAV");
  }
  if (pcm_bytes.size() % sizeof(opus_int16) != 0) {
    throw std::runtime_error("invalid PCM data size");
  }
  result.samples.resize(pcm_bytes.size() / sizeof(opus_int16));
  std::memcpy(result.samples.data(), pcm_bytes.data(), pcm_bytes.size());
  return result;
}

auto write_wav(const std::string& path, const wav_data& wav) -> void {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("cannot open output wav: " + path);
  }
  const auto data_bytes = static_cast<std::uint32_t>(wav.samples.size() * sizeof(opus_int16));
  output.write("RIFF", 4);
  write_u32(output, 36U + data_bytes);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, 1);
  write_u16(output, static_cast<std::uint16_t>(wav.channels));
  write_u32(output, static_cast<std::uint32_t>(wav.sample_rate));
  write_u32(output, static_cast<std::uint32_t>(wav.sample_rate * wav.channels * sizeof(opus_int16)));
  write_u16(output, static_cast<std::uint16_t>(wav.channels * sizeof(opus_int16)));
  write_u16(output, 16);
  output.write("data", 4);
  write_u32(output, data_bytes);
  output.write(reinterpret_cast<const char*>(wav.samples.data()), static_cast<std::streamsize>(data_bytes));
}

[[nodiscard]] auto parse_application(std::string_view value) -> int {
  if (value == "voip") {
    return OPUS_APPLICATION_VOIP;
  }
  if (value == "audio") {
    return OPUS_APPLICATION_AUDIO;
  }
  if (value == "lowdelay") {
    return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
  }
  return std::stoi(std::string{value});
}

[[nodiscard]] auto roundtrip(const wav_data& input, int bitrate, int application, int frame_size, int vbr, int complexity) -> wav_data {
  auto error = OPUS_OK;
  auto encoder = make_opus_encoder(input.sample_rate, input.channels, application, &error);
  if (!encoder || error != OPUS_OK) {
    throw std::runtime_error("opus_encoder_create failed");
  }
  auto decoder = make_opus_decoder(input.sample_rate, input.channels, &error);
  if (!decoder || error != OPUS_OK) {
    throw std::runtime_error("opus_decoder_create failed");
  }
  if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
      opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(vbr)) != OPUS_OK ||
      opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(complexity)) != OPUS_OK) {
    throw std::runtime_error("encoder ctl failed");
  }

  const auto input_frames = input.samples.size() / static_cast<std::size_t>(input.channels);
  const auto total_frames = ((input_frames + frame_size - 1) / frame_size) * frame_size;
  auto padded = input.samples;
  padded.resize(total_frames * static_cast<std::size_t>(input.channels));

  auto output = wav_data{.sample_rate = input.sample_rate, .channels = input.channels, .samples = {}};
  output.samples.reserve(padded.size());

  std::array<unsigned char, 1500> packet{};
  std::vector<opus_int16> decoded(static_cast<std::size_t>(frame_size * input.channels));
  for (std::size_t offset = 0; offset < padded.size(); offset += static_cast<std::size_t>(frame_size * input.channels)) {
    const int packet_size = opus_encode(encoder.get(), padded.data() + offset, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (packet_size < 0) {
      throw std::runtime_error(std::string{"opus_encode failed: "} + opus_strerror(packet_size));
    }
    const int decoded_frames = opus_decode(decoder.get(), packet.data(), packet_size, decoded.data(), frame_size, 0);
    if (decoded_frames < 0) {
      throw std::runtime_error(std::string{"opus_decode failed: "} + opus_strerror(decoded_frames));
    }
    const auto decoded_samples = static_cast<std::size_t>(decoded_frames * input.channels);
    output.samples.insert(output.samples.end(), decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(decoded_samples));
  }
  output.samples.resize(input_frames * static_cast<std::size_t>(input.channels));
  return output;
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 5 || argc > 8) {
      std::cerr << "usage: wer_roundtrip <input.wav> <output.wav> <bitrate> <voip|audio|lowdelay> [frame_size=960] [vbr=1] "
                   "[complexity=10]\n";
      return 2;
    }
    const auto input = read_wav(argv[1]);
    const auto bitrate = std::stoi(argv[3]);
    const auto application = parse_application(argv[4]);
    const auto frame_size = argc > 5 ? std::stoi(argv[5]) : OPUS_FRAME_SIZE_20MS;
    const auto vbr = argc > 6 ? std::stoi(argv[6]) : 1;
    const auto complexity = argc > 7 ? std::stoi(argv[7]) : 10;
    write_wav(argv[2], roundtrip(input, bitrate, application, frame_size, vbr, complexity));
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "wer_roundtrip: " << e.what() << '\n';
    return 1;
  }
}
