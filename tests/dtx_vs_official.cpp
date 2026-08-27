#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

struct curr_OpusEncoder;
curr_OpusEncoder* curr_opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder* st) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder* st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes) noexcept;

#include "official_opus_abi.h"

namespace {

constexpr int sample_rate = 48000;
constexpr int frame_size = sample_rate / 50;
constexpr int warmup_frames = 15;
constexpr int active_frames = 120;
constexpr double pi = 3.14159265358979323846;

enum class material {
  voice,
  quiet_voice,
  far_field,
  noisy_voice,
  two_speakers,
  speech_music,
  fricative_voice,
  steady_noise,
};

constexpr std::array materials{
    material::voice,
    material::quiet_voice,
    material::far_field,
    material::noisy_voice,
    material::two_speakers,
    material::speech_music,
    material::fricative_voice,
};

[[nodiscard]] constexpr auto material_name(material value) noexcept -> std::string_view {
  switch (value) {
  case material::voice:
    return "voice";
  case material::quiet_voice:
    return "quiet_voice";
  case material::far_field:
    return "far_field";
  case material::noisy_voice:
    return "noisy_voice";
  case material::two_speakers:
    return "two_speakers";
  case material::speech_music:
    return "speech_music";
  case material::fricative_voice:
    return "fricative_voice";
  case material::steady_noise:
    return "steady_noise";
  }
  return "unknown";
}

[[nodiscard]] auto noise_sample(std::uint32_t index) noexcept -> double {
  auto value = index * 747796405u + 2891336453u;
  value = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
  value = (value >> 22u) ^ value;
  return static_cast<double>(static_cast<std::int32_t>(value)) / 2147483648.0;
}

[[nodiscard]] auto speech_wave(double t, double pitch_offset = 0.0) noexcept -> double {
  const double phase = 2.0 * pi * ((126.0 + pitch_offset) * t + 2.4 * std::sin(2.0 * pi * .73 * t));
  const double envelope = .45 + .40 * std::pow(.5 + .5 * std::sin(2.0 * pi * 2.1 * t), 2.0);
  return envelope * (.72 * std::sin(phase) + .20 * std::sin(2.0 * phase) + .08 * std::sin(3.0 * phase));
}

[[nodiscard]] auto material_sample(material value, std::uint32_t sample_index) noexcept -> double {
  const double t = static_cast<double>(sample_index) / sample_rate;
  const double voice = speech_wave(t);
  const double noise = noise_sample(sample_index);
  switch (value) {
  case material::voice:
    return 7000.0 * voice;
  case material::quiet_voice:
    return 20.0 * voice;
  case material::far_field:
    return 850.0 * voice + 220.0 * noise;
  case material::noisy_voice:
    return 3500.0 * voice + 3000.0 * noise;
  case material::two_speakers:
    return 3600.0 * voice + 3000.0 * speech_wave(t, 61.0);
  case material::speech_music:
    return 4200.0 * voice + 1200.0 * std::sin(2.0 * pi * 329.63 * t) + 900.0 * std::sin(2.0 * pi * 493.88 * t);
  case material::fricative_voice: {
    const bool fricative = (sample_index / static_cast<std::uint32_t>(frame_size)) % 10u >= 8u;
    return fricative ? 2800.0 * (noise - noise_sample(sample_index - 1u)) : 5000.0 * voice;
  }
  case material::steady_noise:
    return 1800.0 * noise;
  }
  return 0.0;
}

[[nodiscard]] auto make_material(material value, int frames) -> std::vector<std::int16_t> {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames * frame_size));
  for (std::size_t index = 0; index < pcm.size(); ++index) {
    const auto sample = std::lround(material_sample(value, static_cast<std::uint32_t>(index)));
    pcm[index] = static_cast<std::int16_t>(std::clamp<long>(sample, -32768, 32767));
  }
  return pcm;
}

struct packet_stream {
  std::vector<std::vector<unsigned char>> packets;
};

template <typename Encoder, auto Create, auto Destroy, auto Control, auto Encode>
[[nodiscard]] auto encode(std::span<const std::int16_t> pcm, int bitrate, bool dtx) -> packet_stream {
  int error = OPUS_OK;
  std::unique_ptr<Encoder, void (*)(Encoder*)> encoder{Create(sample_rate, 1, OPUS_APPLICATION_VOIP, &error), [](Encoder* value) {
                                                         Destroy(value);
                                                       }};
  if (!encoder || error != OPUS_OK || Control(encoder.get(), OPUS_SET_BITRATE_REQUEST, bitrate) != OPUS_OK ||
      Control(encoder.get(), OPUS_SET_COMPLEXITY_REQUEST, 10) != OPUS_OK || Control(encoder.get(), OPUS_SET_VBR_REQUEST, 1) != OPUS_OK ||
      Control(encoder.get(), OPUS_SET_DTX_REQUEST, dtx ? 1 : 0) != OPUS_OK) {
    throw std::runtime_error("encoder setup failed");
  }

  packet_stream output;
  std::array<unsigned char, 1276> packet{};
  const auto frames = pcm.size() / frame_size;
  output.packets.reserve(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const int length = Encode(encoder.get(), pcm.data() + frame * frame_size, frame_size, packet.data(), packet.size());
    if (length <= 0) {
      throw std::runtime_error("encode failed");
    }
    output.packets.emplace_back(packet.begin(), packet.begin() + length);
  }
  return output;
}

[[nodiscard]] auto encode_current(std::span<const std::int16_t> pcm, int bitrate, bool dtx) -> packet_stream {
  return encode<curr_OpusEncoder, curr_opus_encoder_create, curr_opus_encoder_destroy, curr_opus_encoder_ctl, curr_opus_encode>(
      pcm, bitrate, dtx);
}

[[nodiscard]] auto encode_official(std::span<const std::int16_t> pcm, int bitrate, bool dtx) -> packet_stream {
  return encode<OpusEncoder, opus_encoder_create, opus_encoder_destroy, opus_encoder_ctl, opus_encode>(pcm, bitrate, dtx);
}

[[nodiscard]] auto decode_official(const packet_stream& stream) -> std::vector<std::int16_t> {
  int error = OPUS_OK;
  std::unique_ptr<OpusDecoder, void (*)(OpusDecoder*)> decoder{opus_decoder_create(sample_rate, 1, &error), [](OpusDecoder* value) {
                                                                 opus_decoder_destroy(value);
                                                               }};
  if (!decoder || error != OPUS_OK) {
    throw std::runtime_error("decoder setup failed");
  }

  std::vector<std::int16_t> output(stream.packets.size() * frame_size);
  for (std::size_t frame = 0; frame < stream.packets.size(); ++frame) {
    const auto& packet = stream.packets[frame];
    if (opus_decode(decoder.get(), packet.data(), static_cast<int>(packet.size()), output.data() + frame * frame_size, frame_size, 0) !=
        frame_size) {
      throw std::runtime_error("decode failed");
    }
  }
  return output;
}

[[nodiscard]] auto count_dtx(const packet_stream& stream, int first_frame, int frame_count) noexcept -> int {
  const auto first = stream.packets.begin() + first_frame;
  return static_cast<int>(std::count_if(first, first + frame_count, [](const auto& packet) {
    return packet.size() == 1;
  }));
}

struct reentry_score {
  double normalized_error = 0.0;
  double gain_error_db = 0.0;
};

[[nodiscard]] auto score_reentry(std::span<const std::int16_t> decoded, std::span<const std::int16_t> source, int first_frame,
                                 int frames) noexcept -> reentry_score {
  constexpr std::size_t codec_lookahead = 312; // Standard 48 kHz Opus encoder lookahead.
  const auto begin = static_cast<std::size_t>(first_frame * frame_size);
  const auto end = begin + static_cast<std::size_t>(frames * frame_size);
  double source_energy = 0.0;
  double decoded_energy = 0.0;
  double error = 0.0;
  for (std::size_t index = begin; index < end; ++index) {
    const double expected = source[index];
    const double actual = decoded[index + codec_lookahead];
    source_energy += expected * expected;
    decoded_energy += actual * actual;
    const double delta = actual - expected;
    error += delta * delta;
  }
  constexpr double floor = 1e-12;
  return {std::sqrt(error / std::max(source_energy, floor)),
          std::abs(10.0 * std::log10(std::max(decoded_energy, floor) / std::max(source_energy, floor)))};
}

[[nodiscard]] auto make_reentry_clip(material value) -> std::vector<std::int16_t> {
  constexpr int speech_frames = 20;
  constexpr int silence_frames = 40;
  constexpr int wake_frames = 20;
  auto output = make_material(value, speech_frames + silence_frames + wake_frames);
  std::fill(output.begin() + speech_frames * frame_size, output.begin() + (speech_frames + silence_frames) * frame_size, 0);
  return output;
}

} // namespace

int main() {
  try {
    constexpr std::array bitrates{16000, 24000};
    int current_false_dtx = 0;
    int official_false_dtx = 0;
    double current_reentry_error = 0.0;
    double official_reentry_error = 0.0;
    double current_gain_error = 0.0;
    double official_gain_error = 0.0;
    int current_silence_dtx = 0;
    int official_silence_dtx = 0;
    int current_noise_dtx = 0;
    int official_noise_dtx = 0;
    int reentry_cases = 0;

    std::cout << std::fixed << std::setprecision(6);
    for (const int bitrate : bitrates) {
      for (const auto value : materials) {
        const auto active = make_material(value, warmup_frames + active_frames);
        const auto current_packets = encode_current(active, bitrate, true);
        const auto official_packets = encode_official(active, bitrate, true);
        const int current_false = count_dtx(current_packets, warmup_frames, active_frames);
        const int official_false = count_dtx(official_packets, warmup_frames, active_frames);
        current_false_dtx += current_false;
        official_false_dtx += official_false;
        std::cout << "dtx_false_positive bitrate=" << bitrate << " material=" << material_name(value) << " opuscpp=" << current_false
                  << " official=" << official_false << '\n';

        const auto reentry = make_reentry_clip(value);
        const auto current_reentry_packets = encode_current(reentry, bitrate, true);
        const auto official_reentry_packets = encode_official(reentry, bitrate, true);
        constexpr int silence_frame = 20;
        constexpr int silence_frames = 40;
        current_silence_dtx += count_dtx(current_reentry_packets, silence_frame, silence_frames);
        official_silence_dtx += count_dtx(official_reentry_packets, silence_frame, silence_frames);
        const auto current_dtx = decode_official(current_reentry_packets);
        const auto official_dtx = decode_official(official_reentry_packets);
        constexpr int wake_frame = 20 + 40;
        const auto current_score = score_reentry(current_dtx, reentry, wake_frame, 8);
        const auto official_score = score_reentry(official_dtx, reentry, wake_frame, 8);
        current_reentry_error += current_score.normalized_error;
        official_reentry_error += official_score.normalized_error;
        current_gain_error += current_score.gain_error_db;
        official_gain_error += official_score.gain_error_db;
        ++reentry_cases;
        std::cout << "dtx_reentry bitrate=" << bitrate << " material=" << material_name(value)
                  << " opuscpp_nrmse=" << current_score.normalized_error << " official_nrmse=" << official_score.normalized_error
                  << " opuscpp_gain_db=" << current_score.gain_error_db << " official_gain_db=" << official_score.gain_error_db << '\n';
      }
    }

    for (const int bitrate : bitrates) {
      const auto noise = make_material(material::steady_noise, warmup_frames + active_frames);
      const auto current_packets = encode_current(noise, bitrate, true);
      const auto official_packets = encode_official(noise, bitrate, true);
      current_noise_dtx += count_dtx(current_packets, warmup_frames, active_frames);
      official_noise_dtx += count_dtx(official_packets, warmup_frames, active_frames);
    }

    current_reentry_error /= reentry_cases;
    official_reentry_error /= reentry_cases;
    current_gain_error /= reentry_cases;
    official_gain_error /= reentry_cases;
    if (current_false_dtx > official_false_dtx || current_reentry_error > official_reentry_error ||
        current_gain_error > official_gain_error || current_silence_dtx < official_silence_dtx || current_noise_dtx <= official_noise_dtx) {
      throw std::runtime_error("DTX comparison regressed against official Opus");
    }
    std::cout << "dtx_comparison=PASS false_positive_opuscpp=" << current_false_dtx << " false_positive_official=" << official_false_dtx
              << " reentry_nrmse_opuscpp=" << current_reentry_error << " reentry_nrmse_official=" << official_reentry_error
              << " reentry_gain_db_opuscpp=" << current_gain_error << " reentry_gain_db_official=" << official_gain_error
              << " silence_dtx_opuscpp=" << current_silence_dtx << " silence_dtx_official=" << official_silence_dtx
              << " steady_noise_dtx_opuscpp=" << current_noise_dtx << " steady_noise_dtx_official=" << official_noise_dtx << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
