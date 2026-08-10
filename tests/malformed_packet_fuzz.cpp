#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

constexpr auto sample_rate = 48000;
constexpr auto frame_size = 960;
constexpr auto max_frame_size = 5760;
constexpr auto max_packet_bytes = 1276;
constexpr auto guard_samples = 32;
constexpr auto guard_i16 = static_cast<opus_int16>(0x5A5A);
constexpr auto guard_f32_bits = std::uint32_t{0x42B4B4B4};

struct random_source {
  std::uint64_t state = 0xD1B54A32D192ED03ULL;

  [[nodiscard]] auto next() noexcept -> std::uint32_t {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return static_cast<std::uint32_t>((state * 0x2545F4914F6CDD1DULL) >> 32);
  }
};

template <typename Sample> [[nodiscard]] auto guarded_output(int channels, Sample guard) -> std::vector<Sample> {
  return std::vector<Sample>(2 * guard_samples + max_frame_size * channels, guard);
}

template <typename Sample> [[nodiscard]] auto guards_intact(const std::vector<Sample>& output, Sample guard) noexcept -> bool {
  return std::all_of(output.begin(), output.begin() + guard_samples,
                     [guard](Sample value) {
                       return value == guard;
                     }) &&
         std::all_of(output.end() - guard_samples, output.end(), [guard](Sample value) {
           return value == guard;
         });
}

[[nodiscard]] auto make_seed_packets() -> std::vector<std::vector<unsigned char>> {
  constexpr std::array bitrates{6000, 12000, 16000, 24000, 32000, 48000, 64000, 96000, 128000, 256000};
  constexpr std::array applications{OPUS_APPLICATION_VOIP, OPUS_APPLICATION_AUDIO, OPUS_APPLICATION_RESTRICTED_LOWDELAY};
  auto packets = std::vector<std::vector<unsigned char>>{};
  auto pcm = std::array<opus_int16, frame_size * 2>{};
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<opus_int16>(((i * 109 + i * i * 7) & 0x7FFF) - 16384);
  }
  for (const int channels : {1, 2}) {
    for (const int application : applications) {
      int error = OPUS_OK;
      auto encoder = std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)>{opus_encoder_create(sample_rate, channels, application, &error),
                                                                          opus_encoder_destroy};
      if (!encoder || error != OPUS_OK) {
        throw std::runtime_error("encoder creation failed");
      }
      for (const int bitrate : bitrates) {
        if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)) != OPUS_OK) {
          throw std::runtime_error("bitrate setup failed");
        }
        auto packet = std::vector<unsigned char>(max_packet_bytes);
        const int bytes = opus_encode(encoder.get(), pcm.data(), frame_size, packet.data(), static_cast<int>(packet.size()));
        if (bytes <= 0) {
          throw std::runtime_error("seed encoding failed");
        }
        packet.resize(static_cast<std::size_t>(bytes));
        packets.push_back(std::move(packet));
      }
    }
  }
  return packets;
}

class decoder_fuzzer {
public:
  explicit decoder_fuzzer(int channels)
      : decoder_{nullptr, opus_decoder_destroy}, i16_output_{guarded_output(channels, guard_i16)},
        f32_output_{guarded_output(channels, std::bit_cast<float>(guard_f32_bits))} {
    int error = OPUS_OK;
    decoder_.reset(opus_decoder_create(sample_rate, channels, &error));
    if (!decoder_ || error != OPUS_OK) {
      throw std::runtime_error("decoder creation failed");
    }
  }

  auto test(std::span<const unsigned char> packet, bool decode_float) -> bool {
    std::fill(i16_output_.begin(), i16_output_.end(), guard_i16);
    const int result =
        opus_decode(decoder_.get(), packet.data(), static_cast<int>(packet.size()), i16_output_.data() + guard_samples, max_frame_size, 0);
    if (result > max_frame_size || !guards_intact(i16_output_, guard_i16)) {
      return false;
    }
    if (!decode_float) {
      return true;
    }
    const auto float_guard = std::bit_cast<float>(guard_f32_bits);
    std::fill(f32_output_.begin(), f32_output_.end(), float_guard);
    const int float_result = opus_decode_float(decoder_.get(), packet.data(), static_cast<int>(packet.size()),
                                               f32_output_.data() + guard_samples, max_frame_size, 0);
    return float_result <= max_frame_size && guards_intact(f32_output_, float_guard);
  }

private:
  std::unique_ptr<OpusDecoder, void (*)(OpusDecoder*)> decoder_;
  std::vector<opus_int16> i16_output_;
  std::vector<float> f32_output_;
};

[[nodiscard]] auto run_fuzz() -> bool {
  auto rng = random_source{};
  const auto seeds = make_seed_packets();
  auto mono = decoder_fuzzer{1};
  auto stereo = decoder_fuzzer{2};
  std::array<unsigned char, max_packet_bytes> packet{};
  std::uint64_t cases = 0;

  auto test = [&](std::span<const unsigned char> bytes) {
    const bool float_path = (cases & 7U) == 0;
    ++cases;
    return mono.test(bytes, float_path) && stereo.test(bytes, float_path) &&
           opus_packet_get_nb_samples(bytes.data(), static_cast<int>(bytes.size()), sample_rate) <= max_frame_size;
  };

  for (const auto& seed : seeds) {
    for (std::size_t length = 1; length <= seed.size(); ++length) {
      if (!test(std::span{seed}.first(length))) {
        return false;
      }
    }
    for (std::size_t index = 0; index < seed.size(); ++index) {
      packet[index] = seed[index];
    }
    for (std::size_t index = 0; index < seed.size(); ++index) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        packet[index] ^= static_cast<unsigned char>(1U << bit);
        if (!test(std::span{packet}.first(seed.size()))) {
          return false;
        }
        packet[index] ^= static_cast<unsigned char>(1U << bit);
      }
    }
  }

  for (int iteration = 0; iteration < 200000; ++iteration) {
    const auto length = static_cast<std::size_t>(1 + rng.next() % max_packet_bytes);
    for (auto& byte : std::span{packet}.first(length)) {
      byte = static_cast<unsigned char>(rng.next());
    }
    if (!test(std::span{packet}.first(length))) {
      return false;
    }
  }

  std::cout << "malformed_packet_cases=" << cases << " seeds=" << seeds.size() << '\n';
  return true;
}

} // namespace

int main() {
  try {
    if (!run_fuzz()) {
      std::cerr << "malformed_packet_fuzz=FAIL\n";
      return 1;
    }
    std::cout << "malformed_packet_fuzz=PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "malformed_packet_fuzz=FAIL error=" << error.what() << '\n';
    return 1;
  }
}
