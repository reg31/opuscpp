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
curr_OpusEncoder *curr_opus_encoder_create(int Fs, int channels, int application, int *error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder *st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder *st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder *st, const std::int16_t *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept;
curr_OpusDecoder *curr_opus_decoder_create(int Fs, int channels, int *error) noexcept;
void curr_opus_decoder_destroy(curr_OpusDecoder *st) noexcept;
int curr_opus_decode(curr_OpusDecoder *st, const unsigned char *data, int len, std::int16_t *pcm, int frame_size, int decode_fec) noexcept;

#include <opus.h>

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::array bitrates{16000, 24000, 32000, 48000, 64000, 96000, 128000, 192000, 256000};

template <typename T, void (*Destroy)(T *)>
struct handle final {
  T *ptr = nullptr;
  handle() = default;
  explicit handle(T *value) noexcept : ptr(value) {}
  handle(const handle &) = delete;
  auto operator=(const handle &) -> handle & = delete;
  handle(handle &&other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
  auto operator=(handle &&other) noexcept -> handle & {
    if (this != &other) {
      reset();
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }
  ~handle() { reset(); }
  void reset() noexcept {
    if (ptr) Destroy(ptr);
    ptr = nullptr;
  }
};

auto make_music_like_pcm(int channels, double seconds) -> std::vector<std::int16_t> {
  const auto frames = static_cast<int>(seconds * sample_rate);
  auto out = std::vector<std::int16_t>(static_cast<std::size_t>(frames * channels));
  for (int i = 0; i < frames; ++i) {
    const auto t = static_cast<double>(i) / sample_rate;
    const auto env = 0.65 + 0.35 * std::sin(2.0 * pi * 0.7 * t);
    const auto left = env * (0.45 * std::sin(2.0 * pi * 196.0 * t) + 0.35 * std::sin(2.0 * pi * 293.66 * t) + 0.20 * std::sin(2.0 * pi * 587.33 * t));
    const auto right = env * (0.45 * std::sin(2.0 * pi * 246.94 * t) + 0.35 * std::sin(2.0 * pi * 369.99 * t) + 0.20 * std::sin(2.0 * pi * 739.99 * t));
    if (channels == 1) {
      out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::clamp<int>(std::lround((left + right) * 0.5 * 22000.0), -32768, 32767));
    } else {
      out[static_cast<std::size_t>(i * 2 + 0)] = static_cast<std::int16_t>(std::clamp<int>(std::lround(left * 22000.0), -32768, 32767));
      out[static_cast<std::size_t>(i * 2 + 1)] = static_cast<std::int16_t>(std::clamp<int>(std::lround(right * 22000.0), -32768, 32767));
    }
  }
  return out;
}

auto make_current_encoder(int channels, int bitrate) -> handle<curr_OpusEncoder, curr_opus_encoder_destroy> {
  int err = OPUS_OK;
  auto *enc = curr_opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
  if (!enc || err != OPUS_OK) throw std::runtime_error("current encoder create failed");
  if (curr_opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) throw std::runtime_error("current bitrate failed");
  if (curr_opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) throw std::runtime_error("current complexity failed");
  if (curr_opus_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPUS_OK) throw std::runtime_error("current VBR failed");
  return handle<curr_OpusEncoder, curr_opus_encoder_destroy>{enc};
}

auto make_official_encoder(int channels, int bitrate) -> handle<OpusEncoder, opus_encoder_destroy> {
  int err = OPUS_OK;
  auto *enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
  if (!enc || err != OPUS_OK) throw std::runtime_error("official encoder create failed");
  if (opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) throw std::runtime_error("official bitrate failed");
  if (opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) throw std::runtime_error("official complexity failed");
  if (opus_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPUS_OK) throw std::runtime_error("official VBR failed");
  return handle<OpusEncoder, opus_encoder_destroy>{enc};
}

auto make_current_decoder(int channels) -> handle<curr_OpusDecoder, curr_opus_decoder_destroy> {
  int err = OPUS_OK;
  auto *dec = curr_opus_decoder_create(sample_rate, channels, &err);
  if (!dec || err != OPUS_OK) throw std::runtime_error("current decoder create failed");
  return handle<curr_OpusDecoder, curr_opus_decoder_destroy>{dec};
}

auto make_official_decoder(int channels) -> handle<OpusDecoder, opus_decoder_destroy> {
  int err = OPUS_OK;
  auto *dec = opus_decoder_create(sample_rate, channels, &err);
  if (!dec || err != OPUS_OK) throw std::runtime_error("official decoder create failed");
  return handle<OpusDecoder, opus_decoder_destroy>{dec};
}

struct packet_stream final {
  std::vector<std::vector<unsigned char>> packets;
  std::uint64_t bytes = 0;
  double encode_ms = 0.0;
};

template <typename Encoder, typename EncodeFn>
auto encode_stream(Encoder *enc, EncodeFn encode, std::span<const std::int16_t> pcm, int channels) -> packet_stream {
  packet_stream out{};
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  std::array<unsigned char, 1500> packet{};
  const auto frames = pcm.size() / samples_per_frame;
  out.packets.reserve(frames);
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto *in = pcm.data() + frame * samples_per_frame;
    const int len = encode(enc, in, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (len <= 0) throw std::runtime_error("encode failed");
    out.packets.emplace_back(packet.begin(), packet.begin() + len);
    out.bytes += static_cast<std::uint64_t>(len);
  }
  out.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return out;
}

template <typename Decoder, typename DecodeFn>
auto decode_stream(Decoder *dec, DecodeFn decode, const packet_stream &stream, int channels) -> double {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frame_size * channels));
  const auto start = std::chrono::steady_clock::now();
  for (const auto &packet : stream.packets) {
    const int got = decode(dec, packet.data(), static_cast<int>(packet.size()), pcm.data(), frame_size, 0);
    if (got != frame_size) throw std::runtime_error("decode failed");
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main() {
  try {
    constexpr int channels = 2;
    const auto pcm = make_music_like_pcm(channels, 8.0);
    std::cout << "bitrate,encode_speedx,current_encode_ms,official_encode_ms,current_avg_bytes,official_avg_bytes,packet_delta_pct,decode_faster_pct,current_decode_ms,official_decode_ms\n";
    for (const auto bitrate : bitrates) {
      auto current_enc = make_current_encoder(channels, bitrate);
      auto official_enc = make_official_encoder(channels, bitrate);
      const auto current_packets = encode_stream(current_enc.ptr, curr_opus_encode, pcm, channels);
      const auto official_packets = encode_stream(official_enc.ptr, opus_encode, pcm, channels);

      auto current_dec = make_current_decoder(channels);
      auto official_dec = make_official_decoder(channels);
      const auto current_decode_ms = decode_stream(current_dec.ptr, curr_opus_decode, official_packets, channels);
      const auto official_decode_ms = decode_stream(official_dec.ptr, opus_decode, official_packets, channels);

      const auto frame_count = static_cast<double>(official_packets.packets.size());
      const auto current_avg_bytes = static_cast<double>(current_packets.bytes) / frame_count;
      const auto official_avg_bytes = static_cast<double>(official_packets.bytes) / frame_count;
      const auto encode_speedx = official_packets.encode_ms / std::max(1e-9, current_packets.encode_ms);
      const auto decode_faster_pct = 100.0 * (official_decode_ms - current_decode_ms) / std::max(1e-9, official_decode_ms);
      const auto packet_delta_pct = 100.0 * (current_avg_bytes - official_avg_bytes) / std::max(1e-9, official_avg_bytes);

      std::cout << bitrate << ','
                << std::fixed << std::setprecision(6) << encode_speedx << ','
                << current_packets.encode_ms << ','
                << official_packets.encode_ms << ','
                << current_avg_bytes << ','
                << official_avg_bytes << ','
                << packet_delta_pct << ','
                << decode_faster_pct << ','
                << current_decode_ms << ','
                << official_decode_ms << '\n';
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark_vs_official failed: " << ex.what() << '\n';
    return 1;
  }
}
