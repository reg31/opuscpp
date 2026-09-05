#include "opus_codec.h"

#include <algorithm>
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

  const auto incomplete_stereo = std::span{stereo_pcm}.first(stereo_pcm.size() - 1);
  const auto incomplete_bytes = opus_encode(stereo_encoder.get(), incomplete_stereo, std::span{stereo_packet});
  ok &= expect(incomplete_bytes == OPUS_BAD_ARG, "span encoder rejects incomplete stereo frame");
  const auto stereo_bytes = opus_encode(stereo_encoder.get(), std::span{stereo_pcm}, std::span{stereo_packet});
  ok &= expect(stereo_bytes > 0, "encode stereo packet");
  const auto stereo_payload = std::span{stereo_packet}.first(stereo_bytes);
  const auto mono_samples = opus_decode(mono_decoder.get(), stereo_payload, std::span{mono_out});
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
  const auto mono_payload = std::span{mono_packet}.first(mono_bytes);
  const auto stereo_samples = opus_decode(stereo_decoder.get(), mono_payload, std::span{stereo_out});
  ok &= expect(stereo_samples == 320, "stereo decoder accepts mono packet");
  const auto stereo_nonzero = count_nonzero(stereo_out);
  ok &= expect(stereo_nonzero > 0, "mono packet duplicate has signal");
  ok &= expect(stereo_abs_difference(stereo_out) == 0, "mono packet duplicated equally to stereo");
  ok &= expect(opus_decode(stereo_decoder.get(), nullptr, 0, stereo_out.data(), 320, 0) == 320, "packet loss produces one concealed frame");

  for (int channels : {1, 2}) {
    auto transition_encoder = make_opus_encoder(16000, channels, OPUS_APPLICATION_VOIP, &err);
    auto float_decoder = make_opus_decoder(16000, channels, &err);
    auto pcm16_decoder = make_opus_decoder(16000, channels, &err);
    if (!transition_encoder || !float_decoder || !pcm16_decoder || err != OPUS_OK) return 1;
    std::array<opus_int16, 640> input{}, integer_output{};
    std::array<float, 640> float_output{};
    std::array<unsigned char, 1500> packet{};
    bool saw_silk = false, saw_celt_after_silk = false;
    for (int frame = 0; frame < 24; ++frame) {
      if (frame == 0 || frame == 12) {
        ok &= expect(opus_encoder_ctl(transition_encoder.get(), OPUS_SET_BITRATE(frame == 0 ? 12000 : 128000)) == OPUS_OK, "configure transition bitrate");
      }
      for (int sample = 0; sample < 320; ++sample) {
        for (int channel = 0; channel < channels; ++channel) {
          input[sample * channels + channel] = static_cast<opus_int16>(5200.f * std::sin((sample + 320 * frame) * (.09f + .006f * channel)));
        }
      }
      const int bytes = opus_encode(transition_encoder.get(), input.data(), 320, packet.data(), static_cast<int>(packet.size()));
      if (bytes <= 0) return 1;
      const bool celt_packet = (packet[0] & 0x80) != 0;
      saw_silk |= !celt_packet && (packet[0] & 0x60) != 0x60;
      saw_celt_after_silk |= saw_silk && celt_packet;
      if (opus_decode_float(float_decoder.get(), packet.data(), bytes, float_output.data(), 320, 0) != 320 ||
          opus_decode(pcm16_decoder.get(), packet.data(), bytes, integer_output.data(), 320, 0) != 320) return 1;
      opus_uint32 float_range = 0, integer_range = 0;
      opus_decoder_ctl(float_decoder.get(), OPUS_GET_FINAL_RANGE(&float_range));
      opus_decoder_ctl(pcm16_decoder.get(), OPUS_GET_FINAL_RANGE(&integer_range));
      ok &= expect(float_range == integer_range, "PCM16 consumes transition redundancy like float decoding");
      for (int sample = 0; sample < 320 * channels; ++sample) {
        const int rounded = static_cast<int>(std::lrint(std::clamp(float_output[sample] * 32768.f, -32768.f, 32767.f)));
        if (std::abs(rounded - integer_output[sample]) > 1) {
          std::cout << "FAIL PCM16 transition waveform channels=" << channels << " frame=" << frame << '\n';
          return 1;
        }
      }
    }
    ok &= expect(saw_silk && saw_celt_after_silk, "transition regression covers SILK then CELT");
  }

  if (!ok) {
    return 1;
  }
  std::cout << "decoder_channel_remap=PASS stereo_bytes=" << stereo_bytes << " mono_bytes=" << mono_bytes
            << " mono_nonzero=" << count_nonzero(mono_out) << " stereo_nonzero=" << stereo_nonzero << '\n';
  return 0;
}
