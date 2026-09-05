#define main perceptual_validation_main
#include "perceptual_memory_validation.cpp"
#undef main

namespace {

using packets = std::vector<std::vector<unsigned char>>;
constexpr std::array rates{16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000};

auto repeat_minute(const fs::path& path) -> clip_data {
  auto clip = load_wav(path, 6);
  if (clip.channels != 1 || clip.samples.empty())
    throw std::runtime_error("Expected nonempty mono input");
  const auto original = clip.samples;
  clip.samples.resize(sample_rate * 60);
  for (std::size_t i = 0; i < clip.samples.size(); ++i)
    clip.samples[i] = original[i % original.size()];
  return clip;
}

auto encode_minute(const clip_data& clip, int bitrate, bool denoise, packets* saved = nullptr) -> double {
  auto encoder = make_current_encoder(1, bitrate, OPUS_APPLICATION_VOIP, denoise);
  std::array<unsigned char, 1500> packet;
  if (saved)
    saved->reserve(clip.samples.size() / frame_size);
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i + frame_size <= clip.samples.size(); i += frame_size) {
    const int bytes = curr_opus_encode(static_cast<curr_OpusEncoder*>(encoder.get()), clip.samples.data() + i, frame_size, packet.data(), packet.size());
    if (bytes <= 0)
      throw std::runtime_error("Optional processing encode failed");
    if (saved)
      saved->emplace_back(packet.begin(), packet.begin() + bytes);
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

auto decode_minute(const packets& input, int level) -> double {
  auto decoder = make_current_decoder(1, level);
  std::array<std::int16_t, frame_size> pcm;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& packet : input) {
    if (curr_opus_decode(static_cast<curr_OpusDecoder*>(decoder.get()), packet.data(), static_cast<int>(packet.size()), pcm.data(), frame_size, 0) != frame_size)
      throw std::runtime_error("Optional processing decode failed");
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

auto median(std::vector<double> values) -> double {
  std::ranges::sort(values);
  return values[values.size() / 2];
}

struct measurement {
  packets speech;
  std::array<std::vector<double>, 4> decode;
  std::array<std::vector<double>, 2> encode;
};

}

int main(int argc, char** argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("Usage: optional_processing_benchmark speech.wav noisy_speech.wav");
    const auto speech = repeat_minute(argv[1]);
    const auto noisy = repeat_minute(argv[2]);
    std::array<measurement, rates.size()> measurements;
    for (std::size_t i = 0; i < rates.size(); ++i)
      encode_minute(speech, rates[i], false, &measurements[i].speech);
    for (int rep = 0; rep < 10; ++rep) {
      for (std::size_t step = 0; step < rates.size(); ++step) {
        const auto i = (step + rep * 4) % rates.size();
        auto& item = measurements[i];
        for (int trial = 0; trial < 4; ++trial) {
          const int level = (trial + rep) % 4;
          const auto elapsed = decode_minute(item.speech, level);
          if (rep)
            item.decode[level].push_back(elapsed);
        }
        for (int trial = 0; trial < 2; ++trial) {
          const int enabled = (trial + rep) % 2;
          const auto elapsed = encode_minute(noisy, rates[i], enabled != 0);
          if (rep)
            item.encode[enabled].push_back(elapsed);
        }
      }
    }
    std::cout << "bitrate,off_ms,light_ms,strong_ms,auto_ms,denoise_off_ms,denoise_on_ms\n"
              << std::fixed << std::setprecision(6);
    for (std::size_t i = 0; i < rates.size(); ++i) {
      std::cout << rates[i];
      for (const auto& values : measurements[i].decode)
        std::cout << ',' << median(values);
      for (const auto& values : measurements[i].encode)
        std::cout << ',' << median(values);
      std::cout << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
