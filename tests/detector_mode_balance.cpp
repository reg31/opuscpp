#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

struct curr_OpusEncoder;
curr_OpusEncoder *curr_opus_encoder_create(int Fs, int channels, int application, int *error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder *st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder *st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder *st, const std::int16_t *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept;
extern "C" int opuscpp_detector_bench_run(const float *pcm, int frame_count, int frame_size, int channels, int include_stereo_width, int iterations, int *accumulator) noexcept;

#include <opus.h>

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = 960;
constexpr double pi = 3.141592653589793238462643383279502884;

auto make_voice_like_pcm(double seconds) -> std::vector<std::int16_t> {
  const auto frames = static_cast<int>(seconds * sample_rate);
  auto out = std::vector<std::int16_t>(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    const auto t = static_cast<double>(i) / sample_rate;
    const auto f0 = 120.0 + 20.0 * std::sin(2.0 * pi * 1.7 * t);
    const auto env = 0.3 + 0.7 * std::max(0.0, std::sin(2.0 * pi * 3.2 * t));
    auto v = env * (0.70 * std::sin(2.0 * pi * f0 * t) + 0.25 * std::sin(2.0 * pi * 2.0 * f0 * t));
    v += 0.01 * std::sin(2.0 * pi * 900.0 * t);
    out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::clamp<int>(std::lround(v * 21000.0), -32768, 32767));
  }
  return out;
}

auto make_music_like_pcm(double seconds) -> std::vector<std::int16_t> {
  const auto frames = static_cast<int>(seconds * sample_rate);
  auto out = std::vector<std::int16_t>(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    const auto t = static_cast<double>(i) / sample_rate;
    const auto env = 0.65 + 0.35 * std::sin(2.0 * pi * 0.7 * t);
    const auto v = env * (0.45 * std::sin(2.0 * pi * 196.0 * t) + 0.35 * std::sin(2.0 * pi * 293.66 * t) + 0.20 * std::sin(2.0 * pi * 587.33 * t));
    out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::clamp<int>(std::lround(v * 22000.0), -32768, 32767));
  }
  return out;
}

auto make_stereo_from_mono(const std::vector<std::int16_t> &mono) -> std::vector<std::int16_t> {
  auto out = std::vector<std::int16_t>(mono.size() * 2);
  for (std::size_t i = 0; i < mono.size(); ++i) {
    out[2 * i] = mono[i];
    out[2 * i + 1] = static_cast<std::int16_t>(std::clamp<int>(static_cast<int>(std::lround(mono[i] * 0.82)), -32768, 32767));
  }
  return out;
}

auto pcm_to_float(const std::vector<std::int16_t> &pcm) -> std::vector<float> {
  auto out = std::vector<float>(pcm.size());
  for (std::size_t i = 0; i < pcm.size(); ++i) out[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
  return out;
}

auto make_current_encoder(int bitrate) -> curr_OpusEncoder * {
  int err = OPUS_OK;
  auto *enc = curr_opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_AUDIO, &err);
  if (!enc || err != OPUS_OK) throw std::runtime_error("encoder create failed");
  if (curr_opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate)) != OPUS_OK) throw std::runtime_error("bitrate failed");
  if (curr_opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPUS_OK) throw std::runtime_error("complexity failed");
  if (curr_opus_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPUS_OK) throw std::runtime_error("VBR failed");
  return enc;
}

auto packet_mode(const unsigned char *data) -> int {
  if (data[0] & 0x80) return 1002;
  if ((data[0] & 0x60) == 0x60) return 1001;
  return 1000;
}

void run_case(const char *label, const std::vector<std::int16_t> &pcm, int bitrate) {
  std::unique_ptr<curr_OpusEncoder, decltype(&curr_opus_encoder_destroy)> enc(make_current_encoder(bitrate), curr_opus_encoder_destroy);
  std::array<unsigned char, 1500> packet{};
  std::uint64_t silk = 0;
  std::uint64_t hybrid = 0;
  std::uint64_t celt = 0;
  const auto frames = pcm.size() / static_cast<std::size_t>(frame_size);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto *in = pcm.data() + frame * static_cast<std::size_t>(frame_size);
    const int len = curr_opus_encode(enc.get(), in, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (len <= 0) throw std::runtime_error("encode failed");
    switch (packet_mode(packet.data())) {
      case 1000: ++silk; break;
      case 1001: ++hybrid; break;
      case 1002: ++celt; break;
    }
  }
  const auto total = std::max<std::uint64_t>(1, silk + hybrid + celt);
  const auto non_celt = silk + hybrid;
  std::cout << label << ",bitrate=" << bitrate
            << ",silk_pct=" << std::fixed << std::setprecision(1) << (100.0 * silk / total)
            << ",hybrid_pct=" << (100.0 * hybrid / total)
            << ",celt_pct=" << (100.0 * celt / total)
            << ",non_celt_pct=" << (100.0 * non_celt / total) << '\n';
}

void run_detector_cost_case(const char *label, const std::vector<std::int16_t> &pcm, int channels) {
  const auto float_pcm = pcm_to_float(pcm);
  const int frame_count = static_cast<int>(pcm.size() / (static_cast<std::size_t>(frame_size) * channels));
  constexpr int iterations = 400;
  int accumulator = 0;
  const auto start = std::chrono::steady_clock::now();
  const int ret = opuscpp_detector_bench_run(float_pcm.data(), frame_count, frame_size, channels, 1, iterations, &accumulator);
  const auto end = std::chrono::steady_clock::now();
  if (ret != 0) throw std::runtime_error("detector bench hook failed");
  const auto elapsed_us = std::chrono::duration<double, std::micro>(end - start).count();
  const auto calls = static_cast<double>(frame_count) * iterations;
  std::cout << label << ",channels=" << channels
            << ",detector_us_per_frame=" << std::fixed << std::setprecision(3) << (elapsed_us / std::max(1.0, calls))
            << ",frames=" << frame_count
            << ",iterations=" << iterations
            << ",accumulator=" << accumulator << '\n';
}

} // namespace

int main() {
  try {
    const auto speech = make_voice_like_pcm(6.0);
    const auto music = make_music_like_pcm(6.0);
    const auto speech_stereo = make_stereo_from_mono(speech);
    const auto music_stereo = make_stereo_from_mono(music);
    for (const int bitrate : {24000, 32000, 48000}) {
      run_case("speech_like", speech, bitrate);
      run_case("harmonic_music", music, bitrate);
    }
    run_detector_cost_case("detector_cost_speech_mono", speech, 1);
    run_detector_cost_case("detector_cost_music_mono", music, 1);
    run_detector_cost_case("detector_cost_speech_stereo", speech_stereo, 2);
    run_detector_cost_case("detector_cost_music_stereo", music_stereo, 2);
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "detector_mode_balance failed: " << ex.what() << '\n';
    return 1;
  }
}
