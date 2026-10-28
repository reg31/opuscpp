#include <algorithm>
#include <array>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _MSC_VER
#include <float.h>
#endif

struct curr_OpusEncoder;
struct curr_OpusDecoder;
curr_OpusEncoder* curr_opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder* st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder* st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes) noexcept;
curr_OpusDecoder* curr_opus_decoder_create(int Fs, int channels, int* error) noexcept;
void curr_opus_decoder_destroy(curr_OpusDecoder* st) noexcept;
int curr_opus_decoder_ctl(curr_OpusDecoder* st, int request, ...) noexcept;
int curr_opus_decode(curr_OpusDecoder* st, const unsigned char* data, int len, std::int16_t* pcm, int frame_size, int decode_fec) noexcept;

#include "official_opus_abi.h"

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr double benchmark_seconds = 60.0;
constexpr int benchmark_repetitions = 9;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::array bitrates{16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000};
constexpr int opuscpp_get_decode_postfilter_request = 5101;
constexpr int opuscpp_get_voice_denoise_request = 5103;

template <typename T, void (*Destroy)(T*)> struct handle_deleter final {
  void operator()(T* ptr) const noexcept {
    if (ptr != nullptr) {
      Destroy(ptr);
    }
  }
};

template <typename T, void (*Destroy)(T*)> using handle = std::unique_ptr<T, handle_deleter<T, Destroy>>;

struct control_result final {
  int status;
  int value;
};

struct control_record final {
  std::string_view lane;
  std::uint64_t instance;
  std::string_view name;
  control_result result;
};

struct environment_record final {
  std::string_view lane;
  std::uint64_t instance;
  int round;
  int exceptions;
  int error;
#ifdef _MSC_VER
  unsigned int control;
  unsigned int status;
#endif
};

std::vector<control_record> control_records;
std::vector<environment_record> environment_records;

auto read_control(auto* state, auto control, int request) -> control_result {
  control_result result{-1, -1};
  result.status = control(state, request, &result.value);
  return result;
}

void require_control(control_result result, int expected, std::string_view label) {
  if (result.status != OPUS_OK || result.value != expected) {
    throw std::runtime_error(std::string{label} + " setup mismatch");
  }
}

void record_control(std::string_view lane, std::uint64_t instance, std::string_view name, control_result result) {
  control_records.push_back({lane, instance, name, result});
}

void record_environment(std::string_view label, std::uint64_t instance) {
  std::fenv_t environment{};
  std::fegetenv(&environment);
  const int round = std::fegetround();
  const int exceptions = std::fetestexcept(FE_ALL_EXCEPT);
  const int saved_errno = errno;
#ifdef _MSC_VER
  unsigned int control = 0;
  _controlfp_s(&control, 0, 0);
  const unsigned int status = _statusfp();
#endif
  environment_records.push_back({label, instance, round, exceptions, saved_errno
#ifdef _MSC_VER
                                 , control, status
#endif
  });
  std::fesetenv(&environment);
  errno = saved_errno;
}

void emit_setup_records() {
  for (const auto& record : control_records) {
    std::cerr << "benchmark_setup lane=" << record.lane << " instance=" << record.instance << ' ' << record.name << '='
              << record.result.value << ':' << record.result.status << '\n';
  }
  for (const auto& record : environment_records) {
    std::cerr << "benchmark_environment lane=" << record.lane << " instance=" << record.instance << " round=" << record.round
              << " exceptions=" << record.exceptions << " errno=" << record.error;
#ifdef _MSC_VER
    std::cerr << " control=" << record.control << " status=" << record.status;
#endif
    std::cerr << '\n';
  }
}

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

auto make_current_encoder(int channels, int bitrate, std::string_view lane, std::uint64_t instance)
    -> handle<curr_OpusEncoder, curr_opus_encoder_destroy> {
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
  const auto actual_bitrate = read_control(enc.get(), curr_opus_encoder_ctl, OPUS_GET_BITRATE_REQUEST);
  const auto actual_complexity = read_control(enc.get(), curr_opus_encoder_ctl, OPUS_GET_COMPLEXITY_REQUEST);
  const auto actual_vbr = read_control(enc.get(), curr_opus_encoder_ctl, OPUS_GET_VBR_REQUEST);
  const auto actual_fec = read_control(enc.get(), curr_opus_encoder_ctl, OPUS_GET_INBAND_FEC_REQUEST);
  const auto actual_dtx = read_control(enc.get(), curr_opus_encoder_ctl, OPUS_GET_DTX_REQUEST);
  const auto actual_denoise = read_control(enc.get(), curr_opus_encoder_ctl, opuscpp_get_voice_denoise_request);
  record_control(lane, instance, "bitrate", actual_bitrate);
  record_control(lane, instance, "complexity", actual_complexity);
  record_control(lane, instance, "vbr", actual_vbr);
  record_control(lane, instance, "fec", actual_fec);
  record_control(lane, instance, "dtx", actual_dtx);
  record_control(lane, instance, "denoise", actual_denoise);
  require_control(actual_bitrate, bitrate, "current bitrate");
  require_control(actual_complexity, 10, "current complexity");
  require_control(actual_vbr, 1, "current VBR");
  require_control(actual_fec, 0, "current FEC");
  require_control(actual_dtx, 0, "current DTX");
  require_control(actual_denoise, 0, "current voice denoise");
  return enc;
}

auto make_official_encoder(int channels, int bitrate, std::string_view lane, std::uint64_t instance)
    -> handle<OpusEncoder, opus_encoder_destroy> {
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
  const auto actual_bitrate = read_control(enc.get(), opus_encoder_ctl, OPUS_GET_BITRATE_REQUEST);
  const auto actual_complexity = read_control(enc.get(), opus_encoder_ctl, OPUS_GET_COMPLEXITY_REQUEST);
  const auto actual_vbr = read_control(enc.get(), opus_encoder_ctl, OPUS_GET_VBR_REQUEST);
  const auto actual_fec = read_control(enc.get(), opus_encoder_ctl, OPUS_GET_INBAND_FEC_REQUEST);
  const auto actual_dtx = read_control(enc.get(), opus_encoder_ctl, OPUS_GET_DTX_REQUEST);
  record_control(lane, instance, "bitrate", actual_bitrate);
  record_control(lane, instance, "complexity", actual_complexity);
  record_control(lane, instance, "vbr", actual_vbr);
  record_control(lane, instance, "fec", actual_fec);
  record_control(lane, instance, "dtx", actual_dtx);
  require_control(actual_bitrate, bitrate, "official bitrate");
  require_control(actual_complexity, 10, "official complexity");
  require_control(actual_vbr, 1, "official VBR");
  require_control(actual_fec, 0, "official FEC");
  require_control(actual_dtx, 0, "official DTX");
  return enc;
}

auto make_current_decoder(int channels, std::string_view lane, std::uint64_t instance)
    -> handle<curr_OpusDecoder, curr_opus_decoder_destroy> {
  int err = OPUS_OK;
  auto dec = handle<curr_OpusDecoder, curr_opus_decoder_destroy>{curr_opus_decoder_create(sample_rate, channels, &err)};
  if (!dec || err != OPUS_OK) {
    throw std::runtime_error("current decoder create failed");
  }
  const auto actual_postfilter = read_control(dec.get(), curr_opus_decoder_ctl, opuscpp_get_decode_postfilter_request);
  record_control(lane, instance, "postfilter", actual_postfilter);
  require_control(actual_postfilter, 0, "current decoder postfilter");
  return dec;
}

auto make_official_decoder(int channels, std::string_view, std::uint64_t) -> handle<OpusDecoder, opus_decoder_destroy> {
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
auto encode_stream(Encoder* enc, EncodeFn encode, std::span<const std::int16_t> pcm, int channels, std::string_view lane,
                   std::uint64_t instance) -> packet_stream {
  packet_stream out{};
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  std::array<unsigned char, 1500> packet{};
  const auto frames = pcm.size() / samples_per_frame;
  out.packets.reserve(frames);
  record_environment(lane, instance);
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
auto measure_encode(Encoder* enc, EncodeFn encode, std::span<const std::int16_t> pcm, int channels, std::string_view lane,
                    std::uint64_t instance) -> double {
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  std::array<unsigned char, 1500> packet{};
  const auto frames = pcm.size() / samples_per_frame;
  record_environment(lane, instance);
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
auto decode_stream(Decoder* dec, DecodeFn decode, const packet_stream& stream, int channels, std::string_view lane,
                   std::uint64_t instance) -> double {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_size * channels));
  record_environment(lane, instance);
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

}

int main() {
  try {
    constexpr int channels = 2;
    const auto pcm = make_music_like_pcm(channels, benchmark_seconds);
    control_records.reserve(1024);
    environment_records.reserve(400);
    std::uint64_t instance = 0;
    std::vector<benchmark_case> cases;
    cases.reserve(bitrates.size());
    for (const auto bitrate : bitrates) {
      const auto current_instance = instance++;
      const auto official_instance = instance++;
      auto current_enc = make_current_encoder(channels, bitrate, "current packet encoder", current_instance);
      auto official_enc = make_official_encoder(channels, bitrate, "official packet encoder", official_instance);
      auto current_packets = encode_stream(current_enc.get(), curr_opus_encode, pcm, channels, "current packet encoder", current_instance);
      auto official_packets = encode_stream(official_enc.get(), opus_encode, pcm, channels, "official packet encoder", official_instance);
      cases.push_back({bitrate, std::move(current_packets), std::move(official_packets), {}, {}, {}, {}});
      cases.back().current_encode_runs.reserve(benchmark_repetitions);
      cases.back().official_encode_runs.reserve(benchmark_repetitions);
      cases.back().current_decode_runs.reserve(benchmark_repetitions);
      cases.back().official_decode_runs.reserve(benchmark_repetitions);
    }

    for (int run = 0; run < benchmark_repetitions; ++run) {
      for (std::size_t step = 0; step < cases.size(); ++step) {
        const auto index = run == 0 ? step : run == 1 ? cases.size() - 1 - step
                                                      : (step + cases.size() / 2) % cases.size();
        auto& test = cases[index];
        const auto measure_current = [&] {
          const auto encoder_instance = instance++;
          auto enc = make_current_encoder(channels, test.bitrate, "current timed encoder", encoder_instance);
          test.current_encode_runs.push_back(
              measure_encode(enc.get(), curr_opus_encode, pcm, channels, "current timed encoder", encoder_instance));
          const auto decoder_instance = instance++;
          auto dec = make_current_decoder(channels, "current timed decoder", decoder_instance);
          test.current_decode_runs.push_back(
              decode_stream(dec.get(), curr_opus_decode, test.official_packets, channels, "current timed decoder", decoder_instance));
        };
        const auto measure_official = [&] {
          const auto encoder_instance = instance++;
          auto enc = make_official_encoder(channels, test.bitrate, "official timed encoder", encoder_instance);
          test.official_encode_runs.push_back(
              measure_encode(enc.get(), opus_encode, pcm, channels, "official timed encoder", encoder_instance));
          const auto decoder_instance = instance++;
          auto dec = make_official_decoder(channels, "official timed decoder", decoder_instance);
          test.official_decode_runs.push_back(
              decode_stream(dec.get(), opus_decode, test.official_packets, channels, "official timed decoder", decoder_instance));
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
    emit_setup_records();
    return 0;
  } catch (const std::exception& ex) {
    emit_setup_records();
    std::cerr << "benchmark_vs_official failed: " << ex.what() << '\n';
    return 1;
  }
}
