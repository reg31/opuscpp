#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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
int curr_opus_decode_float(curr_OpusDecoder *st, const unsigned char *data, int len, float *pcm, int frame_size, int decode_fec) noexcept;

#include <opus.h>

namespace {

namespace fs = std::filesystem;
constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr int max_packet_bytes = 1275;

struct state_handle final {
  void *ptr = nullptr;
  void (*destroy)(void *) = nullptr;
  state_handle() = default;
  state_handle(void *value, void (*deleter)(void *)) noexcept : ptr(value), destroy(deleter) {}
  state_handle(const state_handle &) = delete;
  auto operator=(const state_handle &) -> state_handle & = delete;
  state_handle(state_handle &&other) noexcept : ptr(other.ptr), destroy(other.destroy) {
    other.ptr = nullptr;
    other.destroy = nullptr;
  }
  auto operator=(state_handle &&other) noexcept -> state_handle & {
    if (this != &other) {
      reset();
      ptr = other.ptr;
      destroy = other.destroy;
      other.ptr = nullptr;
      other.destroy = nullptr;
    }
    return *this;
  }
  ~state_handle() { reset(); }
  void reset() noexcept {
    if (ptr != nullptr && destroy != nullptr) destroy(ptr);
    ptr = nullptr;
    destroy = nullptr;
  }
};

struct clip_data final {
  int channels = 0;
  std::vector<std::int16_t> samples;
};

struct packet_stream final {
  std::string name;
  std::vector<std::vector<unsigned char>> packets;
};

struct metric_accum final {
  double signal = 0.0;
  double error = 0.0;
  double abs_error = 0.0;
  std::uint64_t samples = 0;
};

struct plc_metrics final {
  metric_accum all;
  metric_accum lost;
  metric_accum recovery;
  double boundary_jump_error = 0.0;
  std::uint64_t boundary_count = 0;
};

[[nodiscard]] auto clampd(double v, double lo, double hi) noexcept -> double {
  return std::max(lo, std::min(hi, v));
}

[[nodiscard]] auto snr_db(const metric_accum &m) noexcept -> double {
  if (m.samples == 0) return 0.0;
  if (m.error <= 1e-20) return 120.0;
  if (m.signal <= 1e-20) return -120.0;
  return 10.0 * std::log10(m.signal / m.error);
}

[[nodiscard]] auto mae(const metric_accum &m) noexcept -> double {
  return m.samples == 0 ? 0.0 : m.abs_error / static_cast<double>(m.samples);
}

[[nodiscard]] auto plc_quality(const plc_metrics &m) noexcept -> double {
  const auto lost = snr_db(m.lost);
  const auto recovery = snr_db(m.recovery);
  const auto click = m.boundary_count == 0 ? 0.0 : m.boundary_jump_error / static_cast<double>(m.boundary_count);
  return clampd(1.0 + 0.070 * (lost + 10.0) + 0.030 * (recovery + 10.0) - 2.0 * click, 1.0, 5.0);
}

[[nodiscard]] auto read_u16(std::span<const unsigned char> bytes, std::size_t offset) -> std::uint16_t {
  if (offset + 2 > bytes.size()) throw std::runtime_error("unexpected EOF");
  return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

[[nodiscard]] auto read_u32(std::span<const unsigned char> bytes, std::size_t offset) -> std::uint32_t {
  if (offset + 4 > bytes.size()) throw std::runtime_error("unexpected EOF");
  return static_cast<std::uint32_t>(bytes[offset])
       | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
       | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
       | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

[[nodiscard]] auto read_file(const fs::path &path) -> std::vector<unsigned char> {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  input.seekg(0, std::ios::beg);
  if (size < 0) throw std::runtime_error("failed to size " + path.string());
  auto bytes = std::vector<unsigned char>(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) throw std::runtime_error("failed to read " + path.string());
  return bytes;
}

[[nodiscard]] auto resample_48k(std::vector<std::int16_t> input, int channels, int rate) -> std::vector<std::int16_t> {
  if (rate == sample_rate) return input;
  const auto in_frames = input.size() / static_cast<std::size_t>(channels);
  const auto out_frames = static_cast<std::size_t>(std::llround(static_cast<double>(in_frames) * sample_rate / rate));
  auto out = std::vector<std::int16_t>(out_frames * static_cast<std::size_t>(channels));
  const auto step = static_cast<double>(rate) / sample_rate;
  for (std::size_t frame = 0; frame < out_frames; ++frame) {
    const auto pos = static_cast<double>(frame) * step;
    const auto base = std::min<std::size_t>(static_cast<std::size_t>(pos), in_frames - 1);
    const auto next = std::min<std::size_t>(base + 1, in_frames - 1);
    const auto frac = pos - static_cast<double>(base);
    for (int channel = 0; channel < channels; ++channel) {
      const auto a = static_cast<double>(input[base * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)]);
      const auto b = static_cast<double>(input[next * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)]);
      out[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)] =
          static_cast<std::int16_t>(std::clamp<long long>(std::llround(a + (b - a) * frac), -32768, 32767));
    }
  }
  return out;
}

[[nodiscard]] auto load_wav(const fs::path &path, double max_seconds) -> clip_data {
  const auto storage = read_file(path);
  const auto bytes = std::span<const unsigned char>{storage};
  if (bytes.size() < 44 || std::string_view{reinterpret_cast<const char *>(bytes.data()), 4} != "RIFF"
      || std::string_view{reinterpret_cast<const char *>(bytes.data() + 8), 4} != "WAVE") {
    throw std::runtime_error("not a WAV file: " + path.string());
  }
  int format = 0, channels = 0, rate = 0, bits = 0;
  std::size_t data_offset = 0, data_size = 0;
  for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
    const auto id = std::string_view{reinterpret_cast<const char *>(bytes.data() + offset), 4};
    const auto size = static_cast<std::size_t>(read_u32(bytes, offset + 4));
    const auto payload = offset + 8;
    if (payload + size > bytes.size()) throw std::runtime_error("bad WAV chunk size");
    if (id == "fmt ") {
      format = read_u16(bytes, payload);
      channels = read_u16(bytes, payload + 2);
      rate = static_cast<int>(read_u32(bytes, payload + 4));
      bits = read_u16(bytes, payload + 14);
    } else if (id == "data") {
      data_offset = payload;
      data_size = size;
    }
    offset = payload + size + (size & 1U);
  }
  if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || data_offset == 0) {
    throw std::runtime_error("only 16-bit PCM mono/stereo WAV is supported");
  }
  auto samples = std::vector<std::int16_t>(data_size / sizeof(std::int16_t));
  for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<std::int16_t>(read_u16(bytes, data_offset + i * 2));
  samples = resample_48k(std::move(samples), channels, rate);
  auto usable = samples.size() - samples.size() % static_cast<std::size_t>(frame_size * channels);
  if (max_seconds > 0.0) {
    usable = std::min(usable, static_cast<std::size_t>(max_seconds * sample_rate) * static_cast<std::size_t>(channels));
    usable -= usable % static_cast<std::size_t>(frame_size * channels);
  }
  samples.resize(usable);
  return {.channels = channels, .samples = std::move(samples)};
}

[[nodiscard]] auto make_current_encoder(int channels, int bitrate, int application) -> state_handle {
  int error = OPUS_OK;
  auto *encoder = curr_opus_encoder_create(sample_rate, channels, application, &error);
  if (encoder == nullptr || error != OPUS_OK) throw std::runtime_error("current encoder create failed");
  if (curr_opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) throw std::runtime_error("current bitrate failed");
  if (curr_opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) throw std::runtime_error("current complexity failed");
  if (curr_opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK) throw std::runtime_error("current VBR failed");
  return {encoder, [](void *ptr) { curr_opus_encoder_destroy(static_cast<curr_OpusEncoder *>(ptr)); }};
}

[[nodiscard]] auto make_official_encoder(int channels, int bitrate, int application) -> state_handle {
  int error = OPUS_OK;
  auto *encoder = opus_encoder_create(sample_rate, channels, application, &error);
  if (encoder == nullptr || error != OPUS_OK) throw std::runtime_error("official encoder create failed");
  if (opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) throw std::runtime_error("official bitrate failed");
  if (opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) throw std::runtime_error("official complexity failed");
  if (opus_encoder_ctl(encoder, OPUS_SET_VBR(1)) != OPUS_OK) throw std::runtime_error("official VBR failed");
  return {encoder, [](void *ptr) { opus_encoder_destroy(static_cast<OpusEncoder *>(ptr)); }};
}

[[nodiscard]] auto make_current_decoder(int channels) -> state_handle {
  int error = OPUS_OK;
  auto *decoder = curr_opus_decoder_create(sample_rate, channels, &error);
  if (decoder == nullptr || error != OPUS_OK) throw std::runtime_error("current decoder create failed");
  return {decoder, [](void *ptr) { curr_opus_decoder_destroy(static_cast<curr_OpusDecoder *>(ptr)); }};
}

[[nodiscard]] auto make_official_decoder(int channels) -> state_handle {
  int error = OPUS_OK;
  auto *decoder = opus_decoder_create(sample_rate, channels, &error);
  if (decoder == nullptr || error != OPUS_OK) throw std::runtime_error("official decoder create failed");
  return {decoder, [](void *ptr) { opus_decoder_destroy(static_cast<OpusDecoder *>(ptr)); }};
}

[[nodiscard]] auto encode_packets(const clip_data &clip, int bitrate, int application, bool official) -> packet_stream {
  auto encoder = official ? make_official_encoder(clip.channels, bitrate, application) : make_current_encoder(clip.channels, bitrate, application);
  auto packets = std::vector<std::vector<unsigned char>>{};
  packets.reserve(clip.samples.size() / static_cast<std::size_t>(frame_size * clip.channels));
  auto buffer = std::array<unsigned char, max_packet_bytes>{};
  for (std::size_t offset = 0; offset < clip.samples.size(); offset += static_cast<std::size_t>(frame_size * clip.channels)) {
    const auto *pcm = clip.samples.data() + offset;
    const int bytes = official
        ? opus_encode(static_cast<OpusEncoder *>(encoder.ptr), pcm, frame_size, buffer.data(), max_packet_bytes)
        : curr_opus_encode(static_cast<curr_OpusEncoder *>(encoder.ptr), pcm, frame_size, buffer.data(), max_packet_bytes);
    if (bytes < 0) throw std::runtime_error("encode failed");
    packets.emplace_back(buffer.begin(), buffer.begin() + bytes);
  }
  return {.name = official ? "official_packets" : "current_packets", .packets = std::move(packets)};
}

[[nodiscard]] auto is_lost_frame(std::string_view pattern, std::size_t frame) -> bool {
  if (pattern == "every10") return frame % 10 == 9;
  if (pattern == "every5") return frame % 5 == 4;
  if (pattern == "burst2") return frame % 50 == 20 || frame % 50 == 21;
  if (pattern == "burst5") return frame % 80 >= 30 && frame % 80 < 35;
  if (pattern == "burst10") return frame % 120 >= 40 && frame % 120 < 50;
  throw std::runtime_error("unknown loss pattern");
}

void add_metric(metric_accum &m, double ref, double deg) noexcept {
  const auto err = ref - deg;
  m.signal += ref * ref;
  m.error += err * err;
  m.abs_error += std::abs(err);
  ++m.samples;
}

void add_frame_metrics(plc_metrics &metrics, const clip_data &clip, std::span<const float> decoded, std::span<const unsigned char> lost) {
  const auto channels = clip.channels;
  const auto frames = lost.size();
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const bool is_lost = lost[frame] != 0;
    const bool is_recovery = frame > 0 && lost[frame - 1] != 0 && !is_lost;
    for (int i = 0; i < frame_size; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        const auto index = (frame * static_cast<std::size_t>(frame_size) + static_cast<std::size_t>(i)) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch);
        const auto ref = static_cast<double>(clip.samples[index]) / 32768.0;
        const auto deg = static_cast<double>(decoded[index]);
        add_metric(metrics.all, ref, deg);
        if (is_lost) add_metric(metrics.lost, ref, deg);
        if (is_recovery) add_metric(metrics.recovery, ref, deg);
      }
    }
    if (frame > 0 && lost[frame - 1] != lost[frame]) {
      for (int ch = 0; ch < channels; ++ch) {
        const auto prev = (frame * static_cast<std::size_t>(frame_size) - 1) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch);
        const auto now = frame * static_cast<std::size_t>(frame_size) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch);
        const auto ref_jump = static_cast<double>(clip.samples[now] - clip.samples[prev]) / 32768.0;
        const auto deg_jump = static_cast<double>(decoded[now] - decoded[prev]);
        metrics.boundary_jump_error += std::abs(ref_jump - deg_jump);
        ++metrics.boundary_count;
      }
    }
  }
}

[[nodiscard]] auto decode_with_loss(const clip_data &clip, const packet_stream &stream, std::string_view pattern, bool official_decoder) -> plc_metrics {
  auto decoder = official_decoder ? make_official_decoder(clip.channels) : make_current_decoder(clip.channels);
  auto decoded = std::vector<float>(clip.samples.size());
  auto lost = std::vector<unsigned char>(stream.packets.size());
  for (std::size_t frame = 0; frame < stream.packets.size(); ++frame) {
    lost[frame] = is_lost_frame(pattern, frame) ? 1U : 0U;
    auto *out = decoded.data() + frame * static_cast<std::size_t>(frame_size * clip.channels);
    const int ret = lost[frame] != 0
        ? (official_decoder
               ? opus_decode_float(static_cast<OpusDecoder *>(decoder.ptr), nullptr, 0, out, frame_size, 0)
               : curr_opus_decode_float(static_cast<curr_OpusDecoder *>(decoder.ptr), nullptr, 0, out, frame_size, 0))
        : (official_decoder
               ? opus_decode_float(static_cast<OpusDecoder *>(decoder.ptr), stream.packets[frame].data(), static_cast<int>(stream.packets[frame].size()), out, frame_size, 0)
               : curr_opus_decode_float(static_cast<curr_OpusDecoder *>(decoder.ptr), stream.packets[frame].data(), static_cast<int>(stream.packets[frame].size()), out, frame_size, 0));
    if (ret != frame_size) throw std::runtime_error("decode failed");
  }
  auto metrics = plc_metrics{};
  add_frame_metrics(metrics, clip, decoded, lost);
  return metrics;
}

void print_metrics(std::string_view packet_source, std::string_view decoder, std::string_view pattern, const plc_metrics &m) {
  const auto boundary = m.boundary_count == 0 ? 0.0 : m.boundary_jump_error / static_cast<double>(m.boundary_count);
  std::cout << "  " << packet_source << " " << decoder << " pattern=" << pattern
            << " plc_quality=" << plc_quality(m)
            << " all_snr_db=" << snr_db(m.all)
            << " lost_snr_db=" << snr_db(m.lost)
            << " recovery_snr_db=" << snr_db(m.recovery)
            << " lost_mae=" << mae(m.lost)
            << " recovery_mae=" << mae(m.recovery)
            << " boundary_jump_error=" << boundary << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    auto input = fs::path{"tests/generated_audio/synthetic_voice_like_mono.wav"};
    int bitrate = 24000;
    int application = OPUS_APPLICATION_AUDIO;
    double max_seconds = 0.0;
    for (int i = 1; i < argc; ++i) {
      const auto arg = std::string_view{argv[i]};
      if (arg == "--input" && i + 1 < argc) input = argv[++i];
      else if (arg == "--bitrate" && i + 1 < argc) bitrate = std::stoi(argv[++i]);
      else if (arg == "--application" && i + 1 < argc) application = std::stoi(argv[++i]);
      else if (arg == "--max-seconds" && i + 1 < argc) max_seconds = std::stod(argv[++i]);
      else throw std::runtime_error("usage: plc_validation --input file.wav --bitrate bps [--application app] [--max-seconds seconds]");
    }

    const auto clip = load_wav(input, max_seconds);
    const auto official_packets = encode_packets(clip, bitrate, application, true);
    const auto current_packets = encode_packets(clip, bitrate, application, false);
    const std::array<std::string_view, 5> patterns{"every10", "every5", "burst2", "burst5", "burst10"};
    std::cout << "PLC validation bitrate=" << bitrate << " channels=" << clip.channels
              << " frames=" << official_packets.packets.size() << " application=" << application << '\n';
    for (const auto pattern : patterns) {
      const auto official_official = decode_with_loss(clip, official_packets, pattern, true);
      const auto official_current = decode_with_loss(clip, official_packets, pattern, false);
      const auto current_current = decode_with_loss(clip, current_packets, pattern, false);
      print_metrics("official_packets", "official_decoder", pattern, official_official);
      print_metrics("official_packets", "current_decoder", pattern, official_current);
      print_metrics("current_packets", "current_decoder", pattern, current_current);
      std::cout << "  delta official_packets current_minus_official pattern=" << pattern
                << " plc_quality=" << (plc_quality(official_current) - plc_quality(official_official))
                << " lost_snr_db=" << (snr_db(official_current.lost) - snr_db(official_official.lost))
                << " recovery_snr_db=" << (snr_db(official_current.recovery) - snr_db(official_official.recovery))
                << " lost_mae=" << (mae(official_current.lost) - mae(official_official.lost)) << '\n';
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}

