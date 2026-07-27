#include "opus_codec.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

[[nodiscard]] auto expect(bool condition, const char* message) -> bool {
  if (!condition) {
    std::cout << "FAIL " << message << '\n';
  }
  return condition;
}

[[nodiscard]] auto count_nonzero(std::span<const opus_int16> samples) noexcept -> int {
  auto count = 0;
  for (const auto sample : samples) {
    if (sample != 0) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] auto stereo_abs_difference(std::span<const opus_int16> pcm) noexcept -> int {
  auto sum = 0;
  for (std::size_t frame = 0; 2 * frame + 1 < pcm.size(); ++frame) {
    sum += std::abs(static_cast<int>(pcm[2 * frame]) - static_cast<int>(pcm[2 * frame + 1]));
  }
  return sum;
}

void fill_mono(std::span<opus_int16> pcm) noexcept {
  for (std::size_t frame = 0; frame < pcm.size(); ++frame) {
    pcm[frame] = static_cast<opus_int16>(4200.0f * std::sin(static_cast<float>(frame) * 0.045f));
  }
}

void fill_stereo(std::span<opus_int16> pcm) noexcept {
  const auto frames = pcm.size() / 2;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    pcm[2 * frame] = static_cast<opus_int16>(5200.0f * std::sin(static_cast<float>(frame) * 0.041f));
    pcm[2 * frame + 1] = static_cast<opus_int16>(3600.0f * std::sin(static_cast<float>(frame) * 0.057f + 0.4f));
  }
}

} // namespace

int main() {
  constexpr auto sample_rate = 48000;
  constexpr auto frame_samples = 960;
  auto ok = true;

  auto err = OPUS_OK;
  auto stereo_encoder = make_opus_encoder(sample_rate, 2, OPUS_APPLICATION_AUDIO, &err);
  ok &= expect(stereo_encoder != nullptr && err == OPUS_OK, "create stereo encoder");
  auto mono_decoder = make_opus_decoder(sample_rate, 1, &err);
  ok &= expect(mono_decoder != nullptr && err == OPUS_OK, "create mono decoder");

  std::array<opus_int16, frame_samples * 2> stereo_pcm{};
  std::array<unsigned char, 1500> stereo_packet{};
  std::array<opus_int16, frame_samples> mono_out{};
  fill_stereo(stereo_pcm);

  const auto stereo_bytes =
      opus_encode(stereo_encoder.get(), stereo_pcm.data(), frame_samples, stereo_packet.data(), static_cast<int>(stereo_packet.size()));
  ok &= expect(stereo_bytes > 0, "encode stereo packet");
  const auto mono_samples = opus_decode(mono_decoder.get(), stereo_packet.data(), stereo_bytes, mono_out.data(), frame_samples, 0);
  ok &= expect(mono_samples == frame_samples, "mono decoder accepts stereo packet");
  ok &= expect(count_nonzero(mono_out) > 0, "stereo packet downmix has signal");

  auto mono_encoder = make_opus_encoder(16000, 1, OPUS_APPLICATION_VOIP, &err);
  ok &= expect(mono_encoder != nullptr && err == OPUS_OK, "create mono encoder");
  auto stereo_decoder = make_opus_decoder(16000, 2, &err);
  ok &= expect(stereo_decoder != nullptr && err == OPUS_OK, "create stereo decoder");

  std::array<opus_int16, 320> mono_pcm{};
  std::array<unsigned char, 1500> mono_packet{};
  std::array<opus_int16, 320 * 2> stereo_out{};
  fill_mono(mono_pcm);

  const auto mono_bytes = opus_encode(mono_encoder.get(), mono_pcm.data(), 320, mono_packet.data(), static_cast<int>(mono_packet.size()));
  ok &= expect(mono_bytes > 0, "encode mono packet");
  const auto stereo_samples = opus_decode(stereo_decoder.get(), mono_packet.data(), mono_bytes, stereo_out.data(), 320, 0);
  ok &= expect(stereo_samples == 320, "stereo decoder accepts mono packet");
  const auto stereo_nonzero = count_nonzero(stereo_out);
  ok &= expect(stereo_nonzero > 0, "mono packet duplicate has signal");
  ok &= expect(stereo_abs_difference(stereo_out) == 0, "mono packet duplicated equally to stereo");
  ok &= expect(opus_decode(stereo_decoder.get(), nullptr, 0, stereo_out.data(), 320, 0) == 320,
               "packet loss produces one concealed frame");

  if (!ok) {
    return 1;
  }
  std::cout << "decoder_channel_remap=PASS stereo_bytes=" << stereo_bytes << " mono_bytes=" << mono_bytes
            << " mono_nonzero=" << count_nonzero(mono_out) << " stereo_nonzero=" << stereo_nonzero << '\n';
  return 0;
}
