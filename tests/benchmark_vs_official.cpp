#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

struct curr_OpusEncoder;
struct curr_OpusDecoder;
curr_OpusEncoder* curr_opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder* st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder* st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes) noexcept;
curr_OpusDecoder* curr_opus_decoder_create(int Fs, int channels, int* error) noexcept;
void curr_opus_decoder_destroy(curr_OpusDecoder* st) noexcept;
int curr_opus_decode(curr_OpusDecoder* st, const unsigned char* data, int len, std::int16_t* pcm, int frame_size, int decode_fec) noexcept;

#include "official_opus_abi.h"

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr double benchmark_seconds = 60.0;
constexpr int benchmark_repetitions = 9;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::array bitrates{16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000};

template <typename T, void (*Destroy)(T*)> struct handle_deleter final {
  void operator()(T* ptr) const noexcept {
    if (ptr != nullptr) {
      Destroy(ptr);
    }
  }
};

template <typename T, void (*Destroy)(T*)> using handle = std::unique_ptr<T, handle_deleter<T, Destroy>>;

auto make_music_like_pcm(int channels, double seconds) -> std::vector<std::int16_t> {
  const auto frames = static_cast<int>(seconds * sample_rate);
  auto out = std::vector<std::int16_t>(static_cast<std::size_t>(frames * channels));
  for (int i = 0; i < frames; ++i) {
    const auto t = static_cast<double>(i) / sample_rate;
    const auto env = 0.65 + 0.35 * std::sin(2.0 * pi * 0.7 * t);
    const auto left =
        env * (0.45 * std::sin(2.0 * pi * 196.0 * t) + 0.35 * std::sin(2.0 * pi * 293.66 * t) + 0.20 * std::sin(2.0 * pi * 587.33 * t));
    const auto right =
        env * (0.45 * std::sin(2.0 * pi * 246.94 * t) + 0.35 * std::sin(2.0 * pi * 369.99 * t) + 0.20 * std::sin(2.0 * pi * 739.99 * t));
    if (channels == 1) {
      out[static_cast<std::size_t>(i)] =
          static_cast<std::int16_t>(std::clamp<int>(std::lround((left + right) * 0.5 * 22000.0), -32768, 32767));
    } else {
      out[static_cast<std::size_t>(i * 2 + 0)] = static_cast<std::int16_t>(std::clamp<int>(std::lround(left * 22000.0), -32768, 32767));
      out[static_cast<std::size_t>(i * 2 + 1)] = static_cast<std::int16_t>(std::clamp<int>(std::lround(right * 22000.0), -32768, 32767));
    }
  }
  return out;
}

auto make_current_encoder(int channels, int bitrate) -> handle<curr_OpusEncoder, curr_opus_encoder_destroy> {
  int err = OPUS_OK;
  auto enc =
      handle<curr_OpusEncoder, curr_opus_encoder_destroy>{curr_opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err)};
  if (!enc || err != OPUS_OK) {
    throw std::runtime_error("current encoder create failed");
  }
  if (curr_opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE_REQUEST, bitrate) != OPUS_OK) {
    throw std::runtime_error("current bitrate failed");
  }
  if (curr_opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY_REQUEST, 10) != OPUS_OK) {
    throw std::runtime_error("current complexity failed");
  }
  if (curr_opus_encoder_ctl(enc.get(), OPUS_SET_VBR_REQUEST, 1) != OPUS_OK) {
    throw std::runtime_error("current VBR failed");
  }
  return enc;
}

auto make_official_encoder(int channels, int bitrate) -> handle<OpusEncoder, opus_encoder_destroy> {
  int err = OPUS_OK;
  auto enc = handle<OpusEncoder, opus_encoder_destroy>{opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err)};
  if (!enc || err != OPUS_OK) {
    throw std::runtime_error("official encoder create failed");
  }
  if (opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE_REQUEST, bitrate) != OPUS_OK) {
    throw std::runtime_error("official bitrate failed");
  }
  if (opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY_REQUEST, 10) != OPUS_OK) {
    throw std::runtime_error("official complexity failed");
  }
  if (opus_encoder_ctl(enc.get(), OPUS_SET_VBR_REQUEST, 1) != OPUS_OK) {
    throw std::runtime_error("official VBR failed");
  }
  return enc;
}

auto make_current_decoder(int channels) -> handle<curr_OpusDecoder, curr_opus_decoder_destroy> {
  int err = OPUS_OK;
  auto dec = handle<curr_OpusDecoder, curr_opus_decoder_destroy>{curr_opus_decoder_create(sample_rate, channels, &err)};
  if (!dec || err != OPUS_OK) {
    throw std::runtime_error("current decoder create failed");
  }
  return dec;
}

auto make_official_decoder(int channels) -> handle<OpusDecoder, opus_decoder_destroy> {
  int err = OPUS_OK;
  auto dec = handle<OpusDecoder, opus_decoder_destroy>{opus_decoder_create(sample_rate, channels, &err)};
  if (!dec || err != OPUS_OK) {
    throw std::runtime_error("official decoder create failed");
  }
  return dec;
}

struct packet_stream final {
  std::vector<std::vector<unsigned char>> packets;
  std::uint64_t bytes = 0;
};

struct benchmark_case final {
  int bitrate;
  packet_stream current_packets;
  packet_stream official_packets;
  std::vector<double> current_encode_runs;
  std::vector<double> official_encode_runs;
  std::vector<double> current_decode_runs;
  std::vector<double> official_decode_runs;
};

template <typename Encoder, typename EncodeFn>
auto encode_stream(Encoder* enc, EncodeFn encode, std::span<const std::int16_t> pcm, int channels) -> packet_stream {
  packet_stream out{};
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  std::array<unsigned char, 1500> packet{};
  const auto frames = pcm.size() / samples_per_frame;
  out.packets.reserve(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto* in = pcm.data() + frame * samples_per_frame;
    const int len = encode(enc, in, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (len <= 0) {
      throw std::runtime_error("encode failed");
    }
    out.packets.emplace_back(packet.begin(), packet.begin() + len);
    out.bytes += static_cast<std::uint64_t>(len);
  }
  return out;
}

template <typename Encoder, typename EncodeFn>
auto measure_encode(Encoder* enc, EncodeFn encode, std::span<const std::int16_t> pcm, int channels) -> double {
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  std::array<unsigned char, 1500> packet{};
  const auto frames = pcm.size() / samples_per_frame;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto* in = pcm.data() + frame * samples_per_frame;
    if (encode(enc, in, frame_size, packet.data(), static_cast<int>(packet.size())) <= 0) {
      throw std::runtime_error("encode failed");
    }
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

template <typename Decoder, typename DecodeFn>
auto decode_stream(Decoder* dec, DecodeFn decode, const packet_stream& stream, int channels) -> double {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_size * channels));
  const auto start = std::chrono::steady_clock::now();
  for (const auto& packet : stream.packets) {
    const int got = decode(dec, packet.data(), static_cast<int>(packet.size()), pcm.data(), frame_size, 0);
    if (got != frame_size) {
      throw std::runtime_error("decode failed");
    }
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

auto median_ms(std::vector<double> values) -> double {
  std::ranges::sort(values);
  return values[values.size() / 2];
}

} // namespace

int main() {
  try {
    constexpr int channels = 2;
    const auto pcm = make_music_like_pcm(channels, benchmark_seconds);
    std::vector<benchmark_case> cases;
    cases.reserve(bitrates.size());
    for (const auto bitrate : bitrates) {
      auto current_enc = make_current_encoder(channels, bitrate);
      auto official_enc = make_official_encoder(channels, bitrate);
      auto current_packets = encode_stream(current_enc.get(), curr_opus_encode, pcm, channels);
      auto official_packets = encode_stream(official_enc.get(), opus_encode, pcm, channels);
      cases.push_back({bitrate, std::move(current_packets), std::move(official_packets), {}, {}, {}, {}});
      cases.back().current_encode_runs.reserve(benchmark_repetitions);
      cases.back().official_encode_runs.reserve(benchmark_repetitions);
      cases.back().current_decode_runs.reserve(benchmark_repetitions);
      cases.back().official_decode_runs.reserve(benchmark_repetitions);
    }

    // Sweep in different orders so CPU boost and thermal drift do not favour
    // the same bitrate in every repetition.
    for (int run = 0; run < benchmark_repetitions; ++run) {
      for (std::size_t step = 0; step < cases.size(); ++step) {
        const auto index = run == 0 ? step : run == 1 ? cases.size() - 1 - step : (step + cases.size() / 2) % cases.size();
        auto& test = cases[index];
        const auto measure_current = [&] {
          auto enc = make_current_encoder(channels, test.bitrate);
          test.current_encode_runs.push_back(measure_encode(enc.get(), curr_opus_encode, pcm, channels));
          auto dec = make_current_decoder(channels);
          test.current_decode_runs.push_back(decode_stream(dec.get(), curr_opus_decode, test.official_packets, channels));
        };
        const auto measure_official = [&] {
          auto enc = make_official_encoder(channels, test.bitrate);
          test.official_encode_runs.push_back(measure_encode(enc.get(), opus_encode, pcm, channels));
          auto dec = make_official_decoder(channels);
          test.official_decode_runs.push_back(decode_stream(dec.get(), opus_decode, test.official_packets, channels));
        };
        if (((run + step) & 1) == 0) {
          measure_current();
          measure_official();
        } else {
          measure_official();
          measure_current();
        }
      }
    }

    std::cout << "bitrate,encode_speedx,current_encode_ms,official_encode_ms,opuscpp_effective_kbps,official_effective_kbps,decode_speedx,"
                 "current_decode_ms,official_decode_ms\n";
    for (auto& test : cases) {
      const auto current_encode_ms = median_ms(std::move(test.current_encode_runs));
      const auto official_encode_ms = median_ms(std::move(test.official_encode_runs));
      const auto current_decode_ms = median_ms(std::move(test.current_decode_runs));
      const auto official_decode_ms = median_ms(std::move(test.official_decode_runs));
      const auto encode_speedx = official_encode_ms / std::max(1e-9, current_encode_ms);
      const auto decode_speedx = official_decode_ms / std::max(1e-9, current_decode_ms);

      const auto frame_count = static_cast<double>(test.official_packets.packets.size());
      const auto current_avg_bytes = static_cast<double>(test.current_packets.bytes) / frame_count;
      const auto official_avg_bytes = static_cast<double>(test.official_packets.bytes) / frame_count;
      const auto current_effective_kbps = current_avg_bytes * 0.4;
      const auto official_effective_kbps = official_avg_bytes * 0.4;

      std::cout << test.bitrate << ',' << std::fixed << std::setprecision(6) << encode_speedx << ',' << current_encode_ms << ','
                << official_encode_ms << ',' << current_effective_kbps << ',' << official_effective_kbps << ',' << decode_speedx << ','
                << current_decode_ms << ',' << official_decode_ms << '\n';
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "benchmark_vs_official failed: " << ex.what() << '\n';
    return 1;
  }
}
