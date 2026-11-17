#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#include <time.h>
#endif

struct curr_OpusEncoder;
struct curr_OpusDecoder;
curr_OpusEncoder* curr_opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder* st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder* st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes) noexcept;

#include "official_opus_abi.h"

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr double pi = 3.141592653589793238462643383279502884;

struct totals final {
  double signal = 0.0;
  double error = 0.0;
  double abs_error = 0.0;
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
  std::array<double, 21> celt_band_dist{};
  std::array<double, 21> celt_shape_corr{};
  std::array<int, 21> celt_shape_count{};
  std::array<double, 21> celt_energy_logratio{};
  std::array<int, 21> celt_energy_logratio_count{};
  double stereo_width_abs_error = 0.0;
  std::uint64_t celt_windows = 0;
  std::uint64_t celt_high_count = 0;
  std::uint64_t stereo_width_count = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t packets = 0;
  std::uint64_t packet_bytes = 0;
  std::uint64_t encode_ns = 0;
};
[[nodiscard]] auto clampd(double v, double lo, double hi) noexcept -> double {
  return std::max(lo, std::min(hi, v));
}
[[nodiscard]] auto snr_db(const totals& v) noexcept -> double {
  if (v.error <= 1e-20) {
    return 120.0;
  }
  if (v.signal <= 1e-20) {
    return -120.0;
  }
  return 10.0 * std::log10(v.signal / v.error);
}

[[nodiscard]] auto seg_snr_db(const totals& v) noexcept -> double {
  return v.seg_count == 0 ? 0.0 : v.seg_snr_sum / static_cast<double>(v.seg_count);
}

[[nodiscard]] auto rms_error(const totals& v) noexcept -> double {
  return v.sample_count == 0 ? 0.0 : std::sqrt(v.error / static_cast<double>(v.sample_count));
}

[[nodiscard]] auto mean_abs_error(const totals& v) noexcept -> double {
  return v.sample_count == 0 ? 0.0 : v.abs_error / static_cast<double>(v.sample_count);
}

[[nodiscard]] auto log_error(const totals& v) noexcept -> double {
  return v.log_count == 0 ? 0.0 : v.log_abs / static_cast<double>(v.log_count);
}

[[nodiscard]] auto log_corr(const totals& v) noexcept -> double {
  const auto n = static_cast<double>(v.log_count);
  if (n <= 1.0) {
    return 0.0;
  }
  const auto cov = v.log_cross - v.log_ref * v.log_deg / n;
  const auto rv = v.log_ref2 - v.log_ref * v.log_ref / n;
  const auto dv = v.log_deg2 - v.log_deg * v.log_deg / n;
  if (rv <= 1e-20 || dv <= 1e-20) {
    return 0.0;
  }
  return clampd(cov / std::sqrt(rv * dv), -1.0, 1.0);
}

[[nodiscard]] auto pesq_style(const totals& v) noexcept -> double {
  const auto asym = v.error <= 1e-20 ? 1.0 : std::max(1.0, v.asym_error / v.error);
  return clampd(1.0 + 0.075 * (seg_snr_db(v) + 10.0) - 0.10 * std::log2(asym) - 0.045 * log_error(v), 1.0, 4.5);
}

[[nodiscard]] auto visqol_style(const totals& v) noexcept -> double {
  const auto corr01 = 0.5 + 0.5 * log_corr(v);
  const auto distance = 1.0 / (1.0 + log_error(v));
  return clampd(1.0 + 4.0 * (0.70 * corr01 + 0.30 * distance), 1.0, 5.0);
}

[[nodiscard]] auto celt_masked_error(const totals& v) noexcept -> double {
  return v.celt_windows == 0 ? 0.0 : std::pow(v.celt_err16_sum / static_cast<double>(v.celt_windows), 1.0 / 16.0);
}

[[nodiscard]] auto celt_quality(const totals& v) noexcept -> double {
  const auto err = celt_masked_error(v);
  return clampd(100.0 * std::exp(-err / 80.0), 0.0, 100.0);
}

[[nodiscard]] auto celt_highband_error(const totals& v) noexcept -> double {
  return v.celt_high_count == 0 ? 0.0 : std::sqrt(v.celt_high_mse_sum / static_cast<double>(v.celt_high_count));
}

[[nodiscard]] auto stereo_width_error(const totals& v) noexcept -> double {
  return v.stereo_width_count == 0 ? 0.0 : v.stereo_width_abs_error / static_cast<double>(v.stereo_width_count);
}
template <typename Samples>
[[nodiscard]] auto goertzel(Samples x, std::size_t offset, int channels, double coeff, std::span<const double> window) noexcept -> double {
  constexpr double scale = std::is_same_v<typename Samples::value_type, std::int16_t> ? 1.0 / 32768.0 : 1.0;
  double s1 = 0.0;
  double s2 = 0.0;
  for (std::size_t i = 0; i < window.size(); ++i) {
    const auto sample = static_cast<double>(x[offset + i * static_cast<std::size_t>(channels)]) * scale;
    const auto s0 = sample * window[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2) / static_cast<double>(window.size());
}

struct celt_metric_tables final {
  static constexpr int window_size = 480;
  static constexpr int freq_bins = 200;
  static constexpr double absolute_mask_amplitude = 1.0 / 1024.0;
  static constexpr double absolute_mask_magnitude = absolute_mask_amplitude * (window_size - 1) / 4.0;
  static constexpr double absolute_mask_energy = absolute_mask_magnitude * absolute_mask_magnitude;
  std::array<double, window_size> window{};
  std::array<std::array<double, window_size>, freq_bins> cos_table{};
  std::array<std::array<double, window_size>, freq_bins> sin_table{};
  std::array<int, freq_bins> bin_band{};
};

[[nodiscard]] auto make_celt_metric_tables() -> celt_metric_tables {
  static constexpr std::array<int, 22> bands{0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 68, 80, 96, 120, 156, 200};
  auto tables = celt_metric_tables{};
  for (int i = 0; i < celt_metric_tables::window_size; ++i) {
    tables.window[static_cast<std::size_t>(i)] =
        0.5 - 0.5 * std::cos((2.0 * pi * static_cast<double>(i)) / (celt_metric_tables::window_size - 1));
  }
  for (int bin = 0; bin < celt_metric_tables::freq_bins; ++bin) {
    for (int i = 0; i < celt_metric_tables::window_size; ++i) {
      const auto phase = (2.0 * pi * static_cast<double>(bin) * static_cast<double>(i)) / celt_metric_tables::window_size;
      tables.cos_table[static_cast<std::size_t>(bin)][static_cast<std::size_t>(i)] = std::cos(phase);
      tables.sin_table[static_cast<std::size_t>(bin)][static_cast<std::size_t>(i)] = std::sin(phase);
    }
    auto band = 0;
    while (band + 1 < static_cast<int>(bands.size()) && bin >= bands[static_cast<std::size_t>(band + 1)]) {
      ++band;
    }
    tables.bin_band[static_cast<std::size_t>(bin)] = std::min(band, 20);
  }
  return tables;
}

void add_celt_perceptual_metrics(totals& out, std::span<const std::int16_t> ref, std::span<const float> deg, int channels) {
  static constexpr int nbands = 21;
  static constexpr int high_band_start = 17;
  static const auto tables = make_celt_metric_tables();
  const auto frames = ref.size() / static_cast<std::size_t>(channels);
  if (frames < celt_metric_tables::window_size) {
    return;
  }

  auto previous_mask = std::array<std::array<double, 2>, nbands>{};
  for (std::size_t start = celt_metric_tables::window_size; start + celt_metric_tables::window_size <= frames;
       start += celt_metric_tables::window_size) {
    auto ref_energy = std::array<std::array<double, 2>, nbands>{};
    auto deg_energy = std::array<std::array<double, 2>, nbands>{};
    auto shape_cross = std::array<std::array<double, 2>, nbands>{};
    auto shape_refsq = std::array<std::array<double, 2>, nbands>{};
    auto shape_degsq = std::array<std::array<double, 2>, nbands>{};
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
        const double ref_mag = std::sqrt(ref_re * ref_re + ref_im * ref_im);
        const double deg_mag = std::sqrt(deg_re * deg_re + deg_im * deg_im);
        shape_cross[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += ref_mag * deg_mag;
        shape_refsq[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += ref_mag * ref_mag;
        shape_degsq[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] += deg_mag * deg_mag;
      }
    }

    auto mask = ref_energy;
    for (int band = 1; band < nbands; ++band) {
      for (int ch = 0; ch < channels; ++ch) {
        mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +=
            0.1 * mask[static_cast<std::size_t>(band - 1)][static_cast<std::size_t>(ch)];
      }
    }
    for (int band = nbands - 1; band-- > 0;) {
      for (int ch = 0; ch < channels; ++ch) {
        mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +=
            0.03 * mask[static_cast<std::size_t>(band + 1)][static_cast<std::size_t>(ch)];
      }
    }
    if (out.celt_windows != 0) {
      for (int band = 0; band < nbands; ++band) {
        for (int ch = 0; ch < channels; ++ch) {
          mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +=
              0.5 * previous_mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
        }
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
        const auto masked_ref = ref_energy[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +
                                0.1 * mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +
                                celt_metric_tables::absolute_mask_energy;
        const auto masked_deg = deg_energy[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +
                                0.1 * mask[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)] +
                                celt_metric_tables::absolute_mask_energy;
        const auto ratio = std::max(1e-12, masked_deg / std::max(masked_ref, 1e-12));
        const auto disturbance = ratio - std::log(ratio) - 1.0;
        frame_mse += disturbance * disturbance;
        out.celt_band_dist[static_cast<std::size_t>(band)] += disturbance * disturbance;
        const auto sc = shape_cross[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
        const auto sr = shape_refsq[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
        const auto sd = shape_degsq[static_cast<std::size_t>(band)][static_cast<std::size_t>(ch)];
        if (sr > celt_metric_tables::absolute_mask_energy) {
          out.celt_shape_corr[static_cast<std::size_t>(band)] += sc / std::sqrt(sr * sd + 1e-30);
          ++out.celt_shape_count[static_cast<std::size_t>(band)];
          out.celt_energy_logratio[static_cast<std::size_t>(band)] += 0.5 * std::log(std::max(1e-30, sd) / std::max(1e-30, sr));
          ++out.celt_energy_logratio_count[static_cast<std::size_t>(band)];
        }
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

void add_metrics(totals& out, std::span<const std::int16_t> ref, std::span<const float> deg, int channels) {
  const auto samples_per_frame = static_cast<std::size_t>(frame_size * channels);
  for (std::size_t i = 0; i < ref.size(); ++i) {
    const auto r = static_cast<double>(ref[i]) / 32768.0;
    const auto d = static_cast<double>(deg[i]);
    const auto e = d - r;
    out.signal += r * r;
    out.error += e * e;
    out.abs_error += std::abs(e);
    out.asym_error += e * e * (std::abs(d) > std::abs(r) ? 1.30 : 1.0);
    ++out.sample_count;
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

  static constexpr std::array<double, 18> centers{80, 120, 180, 270, 400, 600, 900, 1300, 1800,
                                                  2500, 3400, 4500, 6000, 8000, 11000, 14000, 17000, 20000};
  auto coeffs = std::array<double, centers.size()>{};
  for (std::size_t i = 0; i < centers.size(); ++i) {
    coeffs[i] = 2.0 * std::cos(2.0 * pi * centers[i] / sample_rate);
  }
  auto window = std::array<double, frame_size>{};
  for (std::size_t i = 0; i < window.size(); ++i) {
    window[i] = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / (window.size() - 1));
  }

  const auto frames = ref.size() / samples_per_frame;
  const auto stride = std::max<std::size_t>(1, frames / 1200);
  for (std::size_t frame = 0; frame < frames; frame += stride) {
    for (int channel = 0; channel < channels; ++channel) {
      const auto offset = frame * samples_per_frame + static_cast<std::size_t>(channel);
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
  }
  add_celt_perceptual_metrics(out, ref, deg, channels);
}

struct codec_api final {
  void* (*create)(int channels, int application, int* error);
  void (*destroy)(void* st);
  int (*ctl_set)(void* st, int request, int value);
  int (*ctl_get)(void* st, int request, int* value);
  int (*encode)(void* st, const std::int16_t* pcm, int frame, unsigned char* data, int max_bytes);
};

const codec_api official_api{
    [](int channels, int application, int* error) -> void* {
      return opus_encoder_create(sample_rate, channels, application, error);
    },
    [](void* st) {
      opus_encoder_destroy(static_cast<OpusEncoder*>(st));
    },
    [](void* st, int request, int value) {
      return opus_encoder_ctl(static_cast<OpusEncoder*>(st), request, value);
    },
    [](void* st, int request, int* value) {
      return opus_encoder_ctl(static_cast<OpusEncoder*>(st), request, value);
    },
    [](void* st, const std::int16_t* pcm, int frame, unsigned char* data, int max_bytes) {
      return opus_encode(static_cast<OpusEncoder*>(st), pcm, frame, data, max_bytes);
    }};

const codec_api current_api{
    [](int channels, int application, int* error) -> void* {
      return curr_opus_encoder_create(sample_rate, channels, application, error);
    },
    [](void* st) {
      curr_opus_encoder_destroy(static_cast<curr_OpusEncoder*>(st));
    },
    [](void* st, int request, int value) {
      return curr_opus_encoder_ctl(static_cast<curr_OpusEncoder*>(st), request, value);
    },
    [](void* st, int request, int* value) {
      return curr_opus_encoder_ctl(static_cast<curr_OpusEncoder*>(st), request, value);
    },
    [](void* st, const std::int16_t* pcm, int frame, unsigned char* data, int max_bytes) {
      return curr_opus_encode(static_cast<curr_OpusEncoder*>(st), pcm, frame, data, max_bytes);
    }};

struct config final {
  std::string mode;
  std::string codec;
  int application = OPUS_APPLICATION_AUDIO;
  int channels = 2;
  int bitrate = 0;
  int complexity = 10;
  int vbr = 1;
  int fec = 0;
  int loss = 0;
  int frame = 960;
  std::string pcm;
  int batch = 1;
  int cpu = -1;
};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

void expect_get(const codec_api& api, void* st, int request, int expected, const char* name) {
  int observed = -1;
  if (api.ctl_get(st, request, &observed) != OPUS_OK || observed != expected) {
    fail(std::string{"encoder option mismatch: "} + name + " expected " + std::to_string(expected) + " got " + std::to_string(observed));
  }
}

[[nodiscard]] auto make_encoder(const codec_api& api, const config& cfg) -> void* {
  int error = OPUS_OK;
  void* st = api.create(cfg.channels, cfg.application, &error);
  if (st == nullptr || error != OPUS_OK) {
    fail("encoder create failed");
  }
  const std::array<std::pair<int, int>, 5> setters{{{OPUS_SET_BITRATE_REQUEST, cfg.bitrate},
                                                    {OPUS_SET_COMPLEXITY_REQUEST, cfg.complexity},
                                                    {OPUS_SET_VBR_REQUEST, cfg.vbr},
                                                    {OPUS_SET_INBAND_FEC_REQUEST, cfg.fec},
                                                    {OPUS_SET_PACKET_LOSS_PERC_REQUEST, cfg.loss}}};
  for (const auto& [request, value] : setters) {
    if (api.ctl_set(st, request, value) != OPUS_OK) {
      fail("encoder ctl failed: " + std::to_string(request));
    }
  }
  expect_get(api, st, OPUS_GET_BITRATE_REQUEST, cfg.bitrate, "bitrate");
  expect_get(api, st, OPUS_GET_COMPLEXITY_REQUEST, cfg.complexity, "complexity");
  expect_get(api, st, OPUS_GET_VBR_REQUEST, cfg.vbr, "vbr");
  expect_get(api, st, OPUS_GET_VBR_CONSTRAINT_REQUEST, 1, "vbr_constraint");
  expect_get(api, st, OPUS_GET_INBAND_FEC_REQUEST, cfg.fec, "fec");
  expect_get(api, st, OPUS_GET_PACKET_LOSS_PERC_REQUEST, cfg.loss, "loss");
  expect_get(api, st, OPUS_GET_DTX_REQUEST, 0, "dtx");
  return st;
}

[[nodiscard]] auto load_pcm(const config& cfg) -> std::vector<std::int16_t> {
  std::ifstream in(cfg.pcm, std::ios::binary | std::ios::ate);
  if (!in) {
    fail("cannot open " + cfg.pcm);
  }
  const auto bytes = static_cast<std::size_t>(in.tellg());
  in.seekg(0);
  std::vector<std::int16_t> samples(bytes / sizeof(std::int16_t));
  in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(std::int16_t)));
  const auto unit = static_cast<std::size_t>(cfg.frame * cfg.channels);
  samples.resize(samples.size() - samples.size() % unit);
  if (samples.empty()) {
    fail("input has no complete frames");
  }
  return samples;
}

[[nodiscard]] auto thread_cpu_seconds() -> double {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
    fail("GetThreadTimes failed");
  }
  const auto to_100ns = [](const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return static_cast<double>(to_100ns(kernel) + to_100ns(user)) * 1e-7;
#else
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

void pin_and_prioritize(int cpu) {
#if defined(_WIN32)
  if (cpu >= 0) {
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu;
    if (!SetProcessAffinityMask(GetCurrentProcess(), mask) || !SetThreadAffinityMask(GetCurrentThread(), mask)) {
      fail("affinity failed");
    }
  }
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#else
  if (cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
      fail("affinity failed");
    }
  }
#endif
}

void run_time(const config& cfg) {
  const auto& api = cfg.codec == "official" ? official_api : current_api;
  const auto pcm = load_pcm(cfg);
  const auto unit = static_cast<std::size_t>(cfg.frame * cfg.channels);
  const auto frames = pcm.size() / unit;
  pin_and_prioritize(cfg.cpu);
  std::vector<void*> encoders;
  for (int i = 0; i < cfg.batch; ++i) {
    encoders.push_back(make_encoder(api, cfg));
  }
  std::array<unsigned char, 1500> packet{};
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  const double start = thread_cpu_seconds();
  for (void* st : encoders) {
    for (std::size_t f = 0; f < frames; ++f) {
      const int n = api.encode(st, pcm.data() + f * unit, cfg.frame, packet.data(), static_cast<int>(packet.size()));
      if (n <= 0) {
        fail("encode failed");
      }
      bytes += static_cast<std::uint64_t>(n);
      ++packets;
    }
  }
  const double cpu_ms = (thread_cpu_seconds() - start) * 1000.0;
  for (void* st : encoders) {
    api.destroy(st);
  }
  const double clip_seconds = static_cast<double>(frames) * cfg.frame / sample_rate;
  const double per_clip_bytes = static_cast<double>(bytes) / cfg.batch;
  std::printf("{\"cpu_ms_total\": %.6f, \"cpu_ms_per_clip\": %.6f, \"batch\": %d, \"packets\": %llu, \"bytes\": %llu, \"actual_bitrate_bps\": %.3f}\n",
              cpu_ms, cpu_ms / cfg.batch, cfg.batch, static_cast<unsigned long long>(packets), static_cast<unsigned long long>(bytes),
              per_clip_bytes * 8.0 / clip_seconds);
}

void run_quality(const config& cfg) {
  const auto& api = cfg.codec == "official" ? official_api : current_api;
  const auto pcm = load_pcm(cfg);
  const auto channels = static_cast<std::size_t>(cfg.channels);
  const auto unit = static_cast<std::size_t>(cfg.frame) * channels;
  const auto frames = pcm.size() / unit;
  void* enc = make_encoder(api, cfg);
  std::int32_t lookahead = 0;
  if (api.ctl_get(enc, OPUS_GET_LOOKAHEAD_REQUEST, &lookahead) != OPUS_OK || lookahead < 0 || lookahead > sample_rate) {
    fail("invalid lookahead");
  }
  int error = OPUS_OK;
  OpusDecoder* dec = opus_decoder_create(sample_rate, cfg.channels, &error);
  if (dec == nullptr || error != OPUS_OK) {
    fail("decoder create failed");
  }
  const auto flush_frames = static_cast<std::size_t>((lookahead + cfg.frame - 1) / cfg.frame);
  std::vector<float> decoded((frames + flush_frames) * unit);
  const std::vector<std::int16_t> silence(unit);
  std::array<unsigned char, 1500> packet{};
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  std::uint32_t final_range = 0;
  for (std::size_t f = 0; f < frames + flush_frames; ++f) {
    const auto* input = f < frames ? pcm.data() + f * unit : silence.data();
    const int n = api.encode(enc, input, cfg.frame, packet.data(), static_cast<int>(packet.size()));
    if (n <= 0) {
      fail("encode failed");
    }
    if (api.ctl_get(enc, OPUS_GET_FINAL_RANGE_REQUEST, reinterpret_cast<int*>(&final_range)) != OPUS_OK) {
      fail("final range failed");
    }
    if (opus_decode_float(dec, packet.data(), n, decoded.data() + f * unit, cfg.frame, 0) != cfg.frame) {
      fail("decode failed");
    }
    if (f < frames) {
      ++packets;
      bytes += static_cast<std::uint64_t>(n);
    }
  }
  api.destroy(enc);
  opus_decoder_destroy(dec);
  decoded.erase(decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(lookahead * cfg.channels));
  decoded.resize(pcm.size());
  auto score = totals{};
  add_metrics(score, pcm, decoded, cfg.channels);
  const double clip_seconds = static_cast<double>(frames) * cfg.frame / sample_rate;
  std::printf("{\"actual_bitrate_bps\": %.3f, \"packets\": %llu, \"bytes\": %llu, \"final_range\": %u", static_cast<double>(bytes) * 8.0 / clip_seconds,
              static_cast<unsigned long long>(packets), static_cast<unsigned long long>(bytes), final_range);
  const std::array<std::pair<const char*, double>, 12> metrics{{{"snr_db", snr_db(score)},
                                                                {"seg_snr_db", seg_snr_db(score)},
                                                                {"rms_error", rms_error(score)},
                                                                {"mean_abs_error", mean_abs_error(score)},
                                                                {"pesq_style", pesq_style(score)},
                                                                {"visqol_style", visqol_style(score)},
                                                                {"logband_corr", log_corr(score)},
                                                                {"logband_error", log_error(score)},
                                                                {"celt_quality", celt_quality(score)},
                                                                {"celt_masked_error", celt_masked_error(score)},
                                                                {"celt_highband_error", celt_highband_error(score)},
                                                                {"stereo_width_error", stereo_width_error(score)}}};
  for (const auto& [name, value] : metrics) {
    std::printf(", \"%s\": %.17g", name, value);
  }
  std::printf("}\n");
}

[[nodiscard]] auto parse(int argc, char** argv) -> config {
  if (argc < 2) {
    fail("usage: bench17 time|quality --codec current|official --app audio|voip --channels N --bitrate B --complexity C --vbr 0|1 --fec 0|1 --loss P --frame F --pcm PATH [--batch K] [--cpu N]");
  }
  config cfg;
  cfg.mode = argv[1];
  if (cfg.mode != "time" && cfg.mode != "quality") {
    fail("mode must be time or quality");
  }
  std::map<std::string, std::string> opts;
  for (int i = 2; i + 1 < argc; i += 2) {
    opts[argv[i]] = argv[i + 1];
  }
  const auto need = [&](const char* key) -> const std::string& {
    const auto it = opts.find(key);
    if (it == opts.end()) {
      fail(std::string{"missing "} + key);
    }
    return it->second;
  };
  cfg.codec = need("--codec");
  if (cfg.codec != "current" && cfg.codec != "official") {
    fail("codec must be current or official");
  }
  const auto& app = need("--app");
  if (app != "audio" && app != "voip") {
    fail("app must be audio or voip");
  }
  cfg.application = app == "voip" ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
  cfg.channels = std::stoi(need("--channels"));
  cfg.bitrate = std::stoi(need("--bitrate"));
  cfg.complexity = std::stoi(need("--complexity"));
  cfg.vbr = std::stoi(need("--vbr"));
  cfg.fec = std::stoi(need("--fec"));
  cfg.loss = std::stoi(need("--loss"));
  cfg.frame = std::stoi(need("--frame"));
  cfg.pcm = need("--pcm");
  if (opts.count("--batch")) {
    cfg.batch = std::stoi(opts["--batch"]);
  }
  if (opts.count("--cpu")) {
    cfg.cpu = std::stoi(opts["--cpu"]);
  }
  if (cfg.batch < 1 || (cfg.channels != 1 && cfg.channels != 2) || cfg.frame <= 0) {
    fail("bad batch/channels/frame");
  }
  return cfg;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto cfg = parse(argc, argv);
    if (cfg.mode == "time") {
      run_time(cfg);
    } else {
      run_quality(cfg);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bench17 error: %s\n", e.what());
    return 1;
  }
  return 0;
}
