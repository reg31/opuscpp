#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

struct curr_OpusEncoder;
struct curr_OpusDecoder;
curr_OpusEncoder *curr_opus_encoder_create(int Fs, int channels, int application, int *error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder *st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder *st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder *st, const std::int16_t *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept;
curr_OpusDecoder *curr_opus_decoder_create(int Fs, int channels, int *error) noexcept;
void curr_opus_decoder_destroy(curr_OpusDecoder *st) noexcept;

#include <opus.h>

namespace {

namespace fs = std::filesystem;
constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr double pi = 3.141592653589793238462643383279502884;

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
  std::string label;
  int channels = 0;
  std::vector<std::int16_t> samples;
};

struct options final {
  fs::path input{"tests/generated_audio/synthetic_music_like_stereo.wav"};
  fs::path listening_dir{};
  int bitrate = 24000;
  int application = OPUS_APPLICATION_AUDIO;
  int memory_instances = 256;
  double max_seconds = 0.0;
  bool memory_only = false;
  bool skip_memory = false;
};

struct totals final {
  double signal = 0.0;
  double error = 0.0;
  double asym_error = 0.0;
  double seg_snr_sum = 0.0;
  std::uint64_t seg_count = 0;
  double log_abs = 0.0;
  double log_ref = 0.0;
  double log_deg = 0.0;
  double log_ref2 = 0.0;
  double log_deg2 = 0.0;
  double log_cross = 0.0;
  std::uint64_t log_count = 0;
  double celt_err16_sum = 0.0;
  double celt_high_mse_sum = 0.0;
  double stereo_width_abs_error = 0.0;
  std::uint64_t celt_windows = 0;
  std::uint64_t celt_high_count = 0;
  std::uint64_t stereo_width_count = 0;
  std::uint64_t packets = 0;
  std::uint64_t packet_bytes = 0;
  std::uint64_t encode_ns = 0;
};

struct result final {
  std::string name;
  totals score{};
  std::vector<float> decoded;
};

[[nodiscard]] auto clampd(double v, double lo, double hi) noexcept -> double { return std::max(lo, std::min(hi, v)); }

[[nodiscard]] auto snr_db(const totals &v) noexcept -> double {
  if (v.error <= 1e-20) return 120.0;
  if (v.signal <= 1e-20) return -120.0;
  return 10.0 * std::log10(v.signal / v.error);
}

[[nodiscard]] auto seg_snr_db(const totals &v) noexcept -> double {
  return v.seg_count == 0 ? 0.0 : v.seg_snr_sum / static_cast<double>(v.seg_count);
}

[[nodiscard]] auto log_error(const totals &v) noexcept -> double {
  return v.log_count == 0 ? 0.0 : v.log_abs / static_cast<double>(v.log_count);
}

[[nodiscard]] auto log_corr(const totals &v) noexcept -> double {
  const auto n = static_cast<double>(v.log_count);
  if (n <= 1.0) return 0.0;
  const auto cov = v.log_cross - v.log_ref * v.log_deg / n;
  const auto rv = v.log_ref2 - v.log_ref * v.log_ref / n;
  const auto dv = v.log_deg2 - v.log_deg * v.log_deg / n;
  if (rv <= 1e-20 || dv <= 1e-20) return 0.0;
  return clampd(cov / std::sqrt(rv * dv), -1.0, 1.0);
}

[[nodiscard]] auto pesq_style(const totals &v) noexcept -> double {
  const auto asym = v.error <= 1e-20 ? 1.0 : std::max(1.0, v.asym_error / v.error);
  return clampd(1.0 + 0.075 * (seg_snr_db(v) + 10.0) - 0.10 * std::log2(asym) - 0.045 * log_error(v), 1.0, 4.5);
}

[[nodiscard]] auto visqol_style(const totals &v) noexcept -> double {
  const auto corr01 = 0.5 + 0.5 * log_corr(v);
  const auto distance = 1.0 / (1.0 + log_error(v));
  return clampd(1.0 + 4.0 * (0.70 * corr01 + 0.30 * distance), 1.0, 5.0);
}

[[nodiscard]] auto celt_masked_error(const totals &v) noexcept -> double {
  return v.celt_windows == 0 ? 0.0 : std::pow(v.celt_err16_sum / static_cast<double>(v.celt_windows), 1.0 / 16.0);
}

[[nodiscard]] auto celt_quality(const totals &v) noexcept -> double {
  const auto err = celt_masked_error(v);
  return clampd(100.0 * std::exp(-err / 80.0), 0.0, 100.0);
}

[[nodiscard]] auto celt_highband_error(const totals &v) noexcept -> double {
  return v.celt_high_count == 0 ? 0.0 : std::sqrt(v.celt_high_mse_sum / static_cast<double>(v.celt_high_count));
}

[[nodiscard]] auto stereo_width_error(const totals &v) noexcept -> double {
  return v.stereo_width_count == 0 ? 0.0 : v.stereo_width_abs_error / static_cast<double>(v.stereo_width_count);
}

[[nodiscard]] auto avg_packet_bytes(const totals &v) noexcept -> double {
  return v.packets == 0 ? 0.0 : static_cast<double>(v.packet_bytes) / static_cast<double>(v.packets);
}

[[nodiscard]] auto encode_ms(const totals &v) noexcept -> double {
  return static_cast<double>(v.encode_ns) / 1'000'000.0;
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

  int format = 0;
  int channels = 0;
  int rate = 0;
  int bits = 0;
  std::size_t data_offset = 0;
  std::size_t data_size = 0;
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
  return {.label = path.stem().string(), .channels = channels, .samples = std::move(samples)};
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

[[nodiscard]] auto make_official_decoder(int channels) -> state_handle {
  int error = OPUS_OK;
  auto *decoder = opus_decoder_create(sample_rate, channels, &error);
  if (decoder == nullptr || error != OPUS_OK) throw std::runtime_error("official decoder create failed");
  return {decoder, [](void *ptr) { opus_decoder_destroy(static_cast<OpusDecoder *>(ptr)); }};
}

[[nodiscard]] auto make_current_decoder(int channels) -> state_handle {
  int error = OPUS_OK;
  auto *decoder = curr_opus_decoder_create(sample_rate, channels, &error);
  if (decoder == nullptr || error != OPUS_OK) throw std::runtime_error("current decoder create failed");
  return {decoder, [](void *ptr) { curr_opus_decoder_destroy(static_cast<curr_OpusDecoder *>(ptr)); }};
}

[[nodiscard]] auto mono(std::span<const std::int16_t> x, std::size_t offset, int channels, std::size_t n) noexcept -> double {
  double v = 0.0;
  for (int c = 0; c < channels; ++c) v += static_cast<double>(x[offset + n * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]) / 32768.0;
  return v / channels;
}

[[nodiscard]] auto mono(std::span<const float> x, std::size_t offset, int channels, std::size_t n) noexcept -> double {
  double v = 0.0;
  for (int c = 0; c < channels; ++c) v += x[offset + n * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
  return v / channels;
}

template <typename Samples>
[[nodiscard]] auto goertzel(Samples x, std::size_t offset, int channels, double coeff, std::span<const double> window) noexcept -> double {
  double s1 = 0.0;
  double s2 = 0.0;
  for (std::size_t i = 0; i < window.size(); ++i) {
    const auto s0 = mono(x, offset, channels, i) * window[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2) / static_cast<double>(window.size());
}

struct celt_metric_tables final {
  static constexpr int window_size = 480;
  static constexpr int freq_bins = 200;
  std::array<double, window_size> window{};
  std::array<std::array<double, window_size>, freq_bins> cos_table{};
  std::array<std::array<double, window_size>, freq_bins> sin_table{};
  std::array<int, freq_bins> bin_band{};
};

[[nodiscard]] auto make_celt_metric_tables() -> celt_metric_tables {
  static constexpr std::array<int, 22> bands{0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 68, 80, 96, 120, 156, 200};
  auto tables = celt_metric_tables{};
  for (int i = 0; i < celt_metric_tables::window_size; ++i) {
    tables.window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos((2.0 * pi * static_cast<double>(i)) / (celt_metric_tables::window_size - 1));
  }
  for (int bin = 0; bin < celt_metric_tables::freq_bins; ++bin) {
    for (int i = 0; i < celt_metric_tables::window_size; ++i) {
      const auto phase = (2.0 * pi * static_cast<double>(bin) * static_cast<double>(i)) / celt_metric_tables::window_size;
      tables.cos_table[static_cast<std::size_t>(bin)][static_cast<std::size_t>(i)] = std::cos(phase);
      tables.sin_table[static_cast<std::size_t>(bin)][static_cast<std::size_t>(i)] = std::sin(phase);
    }
    auto band = 0;
    while (band + 1 < static_cast<int>(bands.size()) && bin >= bands[static_cast<std::size_t>(band + 1)]) ++band;
    tables.bin_band[static_cast<std::size_t>(bin)] = std::min(band, 20);
  }
  return tables;
}

void add_celt_perceptual_metrics(totals &out, std::span<const std::int16_t> ref, std::span<const float> deg, int channels) {
  static constexpr int nbands = 21;
  static constexpr int high_band_start = 17; // 8 kHz and above in the CELT band map.
  static const auto tables = make_celt_metric_tables();
  const auto frames = ref.size() / static_cast<std::size_t>(channels);
  if (frames < celt_metric_tables::window_size) return;

  auto previous_mask = std::array<std::array<double, 2>, nbands>{};
  for (std::size_t start = 0; start + celt_metric_tables::window_size <= frames; start += celt_metric_tables::window_size) {
    auto ref_energy = std::array<std::array<double, 2>, nbands>{};
    auto deg_energy = std::array<std::array<double, 2>, nbands>{};
    for (int bin = 0; bin < celt_metric_tables::freq_bins; ++bin) {
      const auto band = tables.bin_band[static_cast<std::size_t>(bin)];
      for (int ch = 0; ch < channels; ++ch) {
        double ref_re = 0.0, ref_im = 0.0, deg_re = 0.0, deg_im = 0.0;
        for (int i = 0; i < celt_metric_tables::window_size; ++i) {
          const auto idx = (start + static_cast<std::size_t>(i)) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch);
          const auto w = tables.window[static_cast<std::size_t>(i)];
          const auto r = (static_cast<double>(ref[idx]) / 32768.0) * w;
          const auto d = static_cast<double>(deg[idx]) * w;
          const auto c = tables.cos_table[static_cast<std::size_t>(bin)][static_cast<std::size_t>(i)];
          const auto s = tables.sin_table[static_cast<std::size_t>(bin)][static_cast<std::size_t>(i)];
          ref_re += r * c;
          ref_im -= r * s;
          deg_re += d * c;
          deg_im -= d * s;
        }
        ref_energy[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += ref_re * ref_re + ref_im * ref_im + 1e-12;
        deg_energy[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += deg_re * deg_re + deg_im * deg_im + 1e-12;
      }
    }

    auto mask = ref_energy;
    for (int band = 1; band < nbands; ++band) {
      for (int ch = 0; ch < channels; ++ch) mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += 0.1 * mask[static_cast<std::size_t>(band - 1)][static_cast<std::size_t>(ch)];
    }
    for (int band = nbands - 1; band-- > 0;) {
      for (int ch = 0; ch < channels; ++ch) mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += 0.03 * mask[static_cast<std::size_t>(band + 1)][static_cast<std::size_t>(ch)];
    }
    if (out.celt_windows != 0) {
      for (int band = 0; band < nbands; ++band) {
        for (int ch = 0; ch < channels; ++ch) mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += 0.5 * previous_mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
      }
    }
    if (channels == 2) {
      for (int band = 0; band < nbands; ++band) {
        const auto left = mask[static_cast<std::size_t>(band)][0];
        const auto right = mask[static_cast<std::size_t>(band)][1];
        mask[static_cast<std::size_t>(band)][0] += 0.01 * right;
        mask[static_cast<std::size_t>(band)][1] += 0.01 * left;
      }
    }
    previous_mask = mask;

    double frame_mse = 0.0;
    int frame_count = 0;
    for (int band = 0; band < nbands; ++band) {
      for (int ch = 0; ch < channels; ++ch) {
        const auto masked_ref = ref_energy[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] + 0.1 * mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
        const auto masked_deg = deg_energy[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] + 0.1 * mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
        const auto ratio = std::max(1e-12, masked_deg / std::max(masked_ref, 1e-12));
        const auto disturbance = ratio - std::log(ratio) - 1.0;
        frame_mse += disturbance * disturbance;
        ++frame_count;
        if (band >= high_band_start) {
          out.celt_high_mse_sum += disturbance * disturbance;
          ++out.celt_high_count;
        }
      }
    }
    frame_mse /= std::max(1, frame_count);
    out.celt_err16_sum += frame_mse * frame_mse * frame_mse * frame_mse;
    ++out.celt_windows;

    if (channels == 2) {
      double ref_mid = 0.0, ref_side = 0.0, deg_mid = 0.0, deg_side = 0.0;
      for (int i = 0; i < celt_metric_tables::window_size; ++i) {
        const auto idx = (start + static_cast<std::size_t>(i)) * 2;
        const auto rl = static_cast<double>(ref[idx]) / 32768.0;
        const auto rr = static_cast<double>(ref[idx + 1]) / 32768.0;
        const auto dl = static_cast<double>(deg[idx]);
        const auto dr = static_cast<double>(deg[idx + 1]);
        const auto rm = 0.5 * (rl + rr);
        const auto rs = 0.5 * (rl - rr);
        const auto dm = 0.5 * (dl + dr);
        const auto ds = 0.5 * (dl - dr);
        ref_mid += rm * rm;
        ref_side += rs * rs;
        deg_mid += dm * dm;
        deg_side += ds * ds;
      }
      const auto ref_width = std::sqrt(ref_side / std::max(ref_mid + ref_side, 1e-12));
      const auto deg_width = std::sqrt(deg_side / std::max(deg_mid + deg_side, 1e-12));
      out.stereo_width_abs_error += std::abs(ref_width - deg_width);
      ++out.stereo_width_count;
    }
  }
}

void add_metrics(totals &out, std::span<const std::int16_t> ref, std::span<const float> deg, int channels) {
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  for (std::size_t i = 0; i < ref.size(); ++i) {
    const auto r = static_cast<double>(ref[i]) / 32768.0;
    const auto d = static_cast<double>(deg[i]);
    const auto e = d - r;
    out.signal += r * r;
    out.error += e * e;
    out.asym_error += e * e * (std::abs(d) > std::abs(r) ? 1.30 : 1.0);
  }
  for (std::size_t frame = 0; frame * samples_per_frame < ref.size(); ++frame) {
    const auto offset = frame * samples_per_frame;
    double s = 0.0;
    double e = 0.0;
    for (std::size_t i = 0; i < samples_per_frame; ++i) {
      const auto r = static_cast<double>(ref[offset + i]) / 32768.0;
      const auto diff = static_cast<double>(deg[offset + i]) - r;
      s += r * r;
      e += diff * diff;
    }
    if (s > 1e-9) {
      out.seg_snr_sum += clampd(10.0 * std::log10(s / std::max(e, 1e-20)), -10.0, 35.0);
      ++out.seg_count;
    }
  }

  static constexpr std::array<double, 18> centers{80, 120, 180, 270, 400, 600, 900, 1300, 1800, 2500, 3400, 4500, 6000, 8000, 11000, 14000, 17000, 20000};
  auto coeffs = std::array<double, centers.size()>{};
  for (std::size_t i = 0; i < centers.size(); ++i) coeffs[i] = 2.0 * std::cos(2.0 * pi * centers[i] / sample_rate);
  auto window = std::array<double, frame_size>{};
  for (std::size_t i = 0; i < window.size(); ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / (window.size() - 1));

  const auto frames = ref.size() / samples_per_frame;
  const auto stride = std::max<std::size_t>(1, frames / 1200);
  for (std::size_t frame = 0; frame < frames; frame += stride) {
    const auto offset = frame * samples_per_frame;
    for (const auto coeff : coeffs) {
      const auto r = std::log1p(1000.0 * goertzel(std::span<const std::int16_t>{ref}, offset, channels, coeff, window));
      const auto d = std::log1p(1000.0 * goertzel(std::span<const float>{deg}, offset, channels, coeff, window));
      out.log_abs += std::abs(r - d);
      out.log_ref += r;
      out.log_deg += d;
      out.log_ref2 += r * r;
      out.log_deg2 += d * d;
      out.log_cross += r * d;
      ++out.log_count;
    }
  }
  add_celt_perceptual_metrics(out, ref, deg, channels);
}

[[nodiscard]] auto run_variant(std::string name, const clip_data &clip, const options &opt, bool official) -> result {
  auto encoder = official ? make_official_encoder(clip.channels, opt.bitrate, opt.application) : make_current_encoder(clip.channels, opt.bitrate, opt.application);
  auto decoder = make_official_decoder(clip.channels);
  auto decoded_frame = std::vector<float>(static_cast<std::size_t>(frame_size * clip.channels));
  auto decoded = std::vector<float>(clip.samples.size());
  auto packet = std::array<unsigned char, 1500>{};
  auto score = totals{};
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * clip.channels);
  const auto frames = clip.samples.size() / samples_per_frame;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto offset = frame * samples_per_frame;
    const auto start = std::chrono::steady_clock::now();
    const auto packet_size = official
      ? opus_encode(static_cast<OpusEncoder *>(encoder.ptr), clip.samples.data() + offset, frame_size, packet.data(), static_cast<int>(packet.size()))
      : curr_opus_encode(static_cast<curr_OpusEncoder *>(encoder.ptr), clip.samples.data() + offset, frame_size, packet.data(), static_cast<int>(packet.size()));
    score.encode_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    if (packet_size <= 0) throw std::runtime_error(name + " encode failed");
    const auto got = opus_decode_float(static_cast<OpusDecoder *>(decoder.ptr), packet.data(), packet_size, decoded_frame.data(), frame_size, 0);
    if (got != frame_size) throw std::runtime_error(name + " official decode failed");
    std::copy(decoded_frame.begin(), decoded_frame.end(), decoded.begin() + static_cast<std::ptrdiff_t>(offset));
    ++score.packets;
    score.packet_bytes += static_cast<std::uint64_t>(packet_size);
  }
  add_metrics(score, clip.samples, decoded, clip.channels);
  return {.name = std::move(name), .score = score, .decoded = std::move(decoded)};
}

void write_u16(std::ostream &out, std::uint16_t v) {
  const std::array<unsigned char, 2> b{static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
  out.write(reinterpret_cast<const char *>(b.data()), 2);
}

void write_u32(std::ostream &out, std::uint32_t v) {
  const std::array<unsigned char, 4> b{static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
  out.write(reinterpret_cast<const char *>(b.data()), 4);
}

template <typename Getter>
void write_wav(const fs::path &path, int channels, std::size_t samples, Getter get) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("failed to create " + path.string());
  const auto data_bytes = static_cast<std::uint32_t>(samples * sizeof(std::int16_t));
  out.write("RIFF", 4);
  write_u32(out, 36 + data_bytes);
  out.write("WAVEfmt ", 8);
  write_u32(out, 16);
  write_u16(out, 1);
  write_u16(out, static_cast<std::uint16_t>(channels));
  write_u32(out, sample_rate);
  write_u32(out, sample_rate * static_cast<std::uint32_t>(channels) * 2);
  write_u16(out, static_cast<std::uint16_t>(channels * 2));
  write_u16(out, 16);
  out.write("data", 4);
  write_u32(out, data_bytes);
  for (std::size_t i = 0; i < samples; ++i) write_u16(out, static_cast<std::uint16_t>(get(i)));
}

[[nodiscard]] auto safe_name(std::string value) -> std::string {
  for (auto &ch : value) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    if (!ok) ch = '_';
  }
  return value;
}

void write_listening(const options &opt, const clip_data &clip, const result &current, const result &official) {
  if (opt.listening_dir.empty()) return;
  const auto base = opt.listening_dir / (std::to_string(opt.bitrate / 1000) + "kbps") / safe_name(clip.label);
  write_wav(base.string() + "_reference.wav", clip.channels, clip.samples.size(), [&](std::size_t i) { return clip.samples[i]; });
  write_wav(base.string() + "_current.wav", clip.channels, current.decoded.size(), [&](std::size_t i) {
    return static_cast<std::int16_t>(std::clamp<long>(std::lrint(std::clamp(static_cast<double>(current.decoded[i]), -1.0, 1.0) * 32767.0), -32768, 32767));
  });
  write_wav(base.string() + "_official.wav", clip.channels, official.decoded.size(), [&](std::size_t i) {
    return static_cast<std::int16_t>(std::clamp<long>(std::lrint(std::clamp(static_cast<double>(official.decoded[i]), -1.0, 1.0) * 32767.0), -32768, 32767));
  });
}

void print_result(std::string_view label, const totals &v) {
  std::cout << label
            << " snr_db=" << std::fixed << std::setprecision(4) << snr_db(v)
            << " segmental_snr_db=" << seg_snr_db(v)
            << " pesq_style=" << pesq_style(v)
            << " visqol_style=" << visqol_style(v)
            << " logband_corr=" << log_corr(v)
            << " logband_error=" << log_error(v)
            << " celt_quality=" << celt_quality(v)
            << " celt_masked_error=" << celt_masked_error(v)
            << " celt_highband_error=" << celt_highband_error(v)
            << " stereo_width_error=" << stereo_width_error(v)
            << " avg_packet_bytes=" << avg_packet_bytes(v)
            << " encode_ms=" << encode_ms(v)
            << " packets=" << v.packets << '\n';
}

struct memory_snapshot final {
  std::uint64_t private_bytes = 0;
  std::uint64_t working_set = 0;
  std::uint64_t peak_working_set = 0;
};

[[nodiscard]] auto read_memory() noexcept -> memory_snapshot {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)) != 0) {
    return {static_cast<std::uint64_t>(counters.PrivateUsage), static_cast<std::uint64_t>(counters.WorkingSetSize), static_cast<std::uint64_t>(counters.PeakWorkingSetSize)};
  }
#endif
  return {};
}

[[nodiscard]] auto delta(std::uint64_t after, std::uint64_t before) noexcept -> std::uint64_t { return after >= before ? after - before : 0; }

template <typename Factory>
auto measure_group(std::string_view label, int count, Factory factory, int api_state_bytes = 0) -> std::vector<state_handle> {
  auto states = std::vector<state_handle>{};
  states.reserve(static_cast<std::size_t>(count));
  const auto before = read_memory();
  for (int i = 0; i < count; ++i) states.push_back(factory());
  const auto after = read_memory();
  const auto private_delta = delta(after.private_bytes, before.private_bytes);
  const auto working_delta = delta(after.working_set, before.working_set);
  std::cout << "memory label=" << label
            << " instances=" << count
            << " private_delta_bytes=" << private_delta
            << " private_per_instance=" << std::fixed << std::setprecision(1) << (static_cast<double>(private_delta) / count)
            << " working_set_delta_bytes=" << working_delta
            << " working_set_per_instance=" << (static_cast<double>(working_delta) / count);
  if (api_state_bytes > 0) std::cout << " official_api_state_bytes=" << api_state_bytes;
  std::cout << '\n';
  return states;
}

void run_memory(const options &opt) {
  std::cout << "\nmemory footprint summary\n";
  auto retained = std::vector<std::vector<state_handle>>{};
  retained.reserve(8);
  for (const auto channels : {1, 2}) {
    retained.push_back(measure_group("current_encoder_ch" + std::to_string(channels), opt.memory_instances, [&] { return make_current_encoder(channels, opt.bitrate, opt.application); }));
    retained.push_back(measure_group("official_encoder_ch" + std::to_string(channels), opt.memory_instances, [&] { return make_official_encoder(channels, opt.bitrate, opt.application); }, opus_encoder_get_size(channels)));
    retained.push_back(measure_group("current_decoder_ch" + std::to_string(channels), opt.memory_instances, [&] { return make_current_decoder(channels); }));
    retained.push_back(measure_group("official_decoder_ch" + std::to_string(channels), opt.memory_instances, [&] { return make_official_decoder(channels); }, opus_decoder_get_size(channels)));
  }
  const auto mem = read_memory();
  std::cout << "memory retained_groups=" << retained.size()
            << " final_private_bytes=" << mem.private_bytes
            << " final_working_set_bytes=" << mem.working_set
            << " peak_working_set_bytes=" << mem.peak_working_set << '\n';
}

[[nodiscard]] auto parse_application(std::string_view value) -> int {
  if (value == "audio") return OPUS_APPLICATION_AUDIO;
  if (value == "voip") return OPUS_APPLICATION_VOIP;
  if (value == "lowdelay") return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
  return std::stoi(std::string{value});
}

[[nodiscard]] auto parse_options(int argc, char **argv) -> options {
  auto opt = options{};
  for (int i = 1; i < argc; ++i) {
    const auto arg = std::string_view{argv[i]};
    auto value = [&]() -> std::string_view {
      if (++i >= argc) throw std::runtime_error("missing value for " + std::string{arg});
      return argv[i];
    };
    if (arg == "--input") opt.input = fs::path{value()};
    else if (arg == "--bitrate") opt.bitrate = std::stoi(std::string{value()});
    else if (arg == "--application") opt.application = parse_application(value());
    else if (arg == "--listening-dir") opt.listening_dir = fs::path{value()};
    else if (arg == "--memory-instances") opt.memory_instances = std::stoi(std::string{value()});
    else if (arg == "--max-seconds") opt.max_seconds = std::stod(std::string{value()});
    else if (arg == "--memory-only") opt.memory_only = true;
    else if (arg == "--skip-memory") opt.skip_memory = true;
    else if (!arg.starts_with("--")) opt.input = fs::path{arg};
    else throw std::runtime_error("unknown argument: " + std::string{arg});
  }
  if (opt.memory_instances < 1) throw std::runtime_error("--memory-instances must be positive");
  return opt;
}

void run_quality(const options &opt) {
  const auto clip = load_wav(opt.input, opt.max_seconds);
  std::cout << "perceptual validation bitrate=" << opt.bitrate
            << " input=" << opt.input.string()
            << " channels=" << clip.channels
            << " frames=" << (clip.samples.size() / static_cast<std::size_t>(frame_size * clip.channels)) << '\n';
  const auto current = run_variant("current", clip, opt, false);
  const auto official = run_variant("official", clip, opt, true);
  write_listening(opt, clip, current, official);
  print_result("  current ", current.score);
  print_result("  official", official.score);
  std::cout << "  delta current_minus_official"
            << " snr_db=" << std::fixed << std::setprecision(4) << (snr_db(current.score) - snr_db(official.score))
            << " pesq_style=" << (pesq_style(current.score) - pesq_style(official.score))
            << " visqol_style=" << (visqol_style(current.score) - visqol_style(official.score))
            << " logband_corr=" << (log_corr(current.score) - log_corr(official.score))
            << " celt_quality=" << (celt_quality(current.score) - celt_quality(official.score))
            << " celt_masked_error=" << (celt_masked_error(current.score) - celt_masked_error(official.score))
            << " celt_highband_error=" << (celt_highband_error(current.score) - celt_highband_error(official.score))
            << " stereo_width_error=" << (stereo_width_error(current.score) - stereo_width_error(official.score))
            << " packet_bytes=" << (avg_packet_bytes(current.score) - avg_packet_bytes(official.score))
            << " packet_bytes_pct=" << (100.0 * (avg_packet_bytes(current.score) - avg_packet_bytes(official.score)) / std::max(1e-9, avg_packet_bytes(official.score)))
            << " encode_speed_ratio_current_vs_official=" << (encode_ms(current.score) / std::max(1e-9, encode_ms(official.score))) << '\n';
  if (!opt.listening_dir.empty()) std::cout << "listening_wavs=" << opt.listening_dir.string() << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto opt = parse_options(argc, argv);
    if (!opt.memory_only) run_quality(opt);
    if (!opt.skip_memory) run_memory(opt);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "perceptual/memory validation failed: " << error.what() << '\n';
    return 1;
  }
}

