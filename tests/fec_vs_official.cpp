#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
constexpr int packet_count = 12;
constexpr int dropped_packet = 8;
constexpr double pi = 3.14159265358979323846;

using packet_stream = std::vector<std::vector<unsigned char>>;

[[nodiscard]] auto make_voice(int channels, int frame_size, int profile) -> std::vector<std::int16_t> {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(packet_count * frame_size * channels));
  std::uint32_t noise_state = 1;
  constexpr std::array signal_levels{7600.0, 2600.0, 5600.0, 2600.0};
  constexpr std::array noise_levels{900.0, 450.0, 2200.0, 0.0};
  for (int frame = 0; frame < packet_count; ++frame) {
    for (int sample = 0; sample < frame_size; ++sample) {
      const auto index = frame * frame_size + sample;
      const double time = static_cast<double>(index) / sample_rate;
      const double pitch = 128.0 + 9.0 * std::sin(2.0 * pi * 1.7 * time);
      const double phase = 2.0 * pi * pitch * time;
      const double envelope = .55 + .35 * std::sin(2.0 * pi * 2.3 * time);
      noise_state = noise_state * 1664525u + 1013904223u;
      const double noise = static_cast<double>(static_cast<std::int32_t>(noise_state)) / 2147483648.0;
      const auto left = profile == 3 && frame < 6 ? std::int16_t{0} : static_cast<std::int16_t>(std::lround(signal_levels[static_cast<std::size_t>(profile)] * envelope * (.75 * std::sin(phase) + .20 * std::sin(2.0 * phase)) + noise_levels[static_cast<std::size_t>(profile)] * noise));
      pcm[static_cast<std::size_t>(index * channels)] = left;
      if (channels == 2) {
        pcm[static_cast<std::size_t>(index * channels + 1)] = profile == 3 && frame < 6 ? std::int16_t{0} : static_cast<std::int16_t>(std::lround(.88 * left + 420.0 * std::sin(2.0 * pi * 211.0 * time)));
      }
    }
  }
  return pcm;
}

template <typename Encoder, auto Create, auto Destroy, auto Control, auto Encode>
[[nodiscard]] auto encode_fec(const std::vector<std::int16_t>& pcm, int channels, int bitrate, int frame_size, bool vbr, bool switch_bitrate = false) -> packet_stream {
  int error = OPUS_OK;
  std::unique_ptr<Encoder, void (*)(Encoder*)> encoder{Create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error), Destroy};
  int fec = -1;
  int loss = -1;
  if (!encoder || error != OPUS_OK || Control(encoder.get(), OPUS_GET_INBAND_FEC_REQUEST, &fec) != OPUS_OK || fec != 0 ||
      Control(encoder.get(), OPUS_GET_PACKET_LOSS_PERC_REQUEST, &loss) != OPUS_OK || loss != 0 ||
      Control(encoder.get(), OPUS_SET_BITRATE_REQUEST, switch_bitrate ? 256000 : bitrate) != OPUS_OK ||
      Control(encoder.get(), OPUS_SET_COMPLEXITY_REQUEST, 10) != OPUS_OK ||
      Control(encoder.get(), OPUS_SET_VBR_REQUEST, static_cast<int>(vbr)) != OPUS_OK ||
      Control(encoder.get(), OPUS_SET_INBAND_FEC_REQUEST, 2) != OPUS_OK ||
      Control(encoder.get(), OPUS_SET_PACKET_LOSS_PERC_REQUEST, 15) != OPUS_OK) {
    throw std::runtime_error("FEC encoder setup failed");
  }
  fec = 0;
  loss = 0;
  if (Control(encoder.get(), OPUS_GET_INBAND_FEC_REQUEST, &fec) != OPUS_OK || fec != 2 ||
      Control(encoder.get(), OPUS_GET_PACKET_LOSS_PERC_REQUEST, &loss) != OPUS_OK || loss != 15 ||
      Control(encoder.get(), OPUS_SET_INBAND_FEC_REQUEST, 1) != OPUS_OK) {
    throw std::runtime_error("FEC encoder CTL round-trip failed");
  }
  packet_stream packets;
  packets.reserve(packet_count);
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> validator{opus_decoder_create(sample_rate, channels, &error), opus_decoder_destroy};
  if (!validator || error != OPUS_OK)
    throw std::runtime_error("FEC packet validator setup failed");
  std::vector<std::int16_t> decoded(static_cast<std::size_t>(frame_size * channels));
  std::array<unsigned char, 1276> packet{};
  for (int frame = 0; frame < packet_count; ++frame) {
    if (switch_bitrate && frame == 6 && Control(encoder.get(), OPUS_SET_BITRATE_REQUEST, bitrate) != OPUS_OK)
      throw std::runtime_error("FEC bitrate transition failed");
    const auto* input = pcm.data() + static_cast<std::size_t>(frame * frame_size * channels);
    const int length = Encode(encoder.get(), input, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (length <= 0) {
      throw std::runtime_error("FEC encode failed");
    }
    std::uint32_t encoded_range = 0, decoded_range = 0;
    if (opus_decode(validator.get(), packet.data(), length, decoded.data(), frame_size, 0) != frame_size ||
        Control(encoder.get(), OPUS_GET_FINAL_RANGE_REQUEST, &encoded_range) != OPUS_OK ||
        opus_decoder_ctl(validator.get(), OPUS_GET_FINAL_RANGE_REQUEST, &decoded_range) != OPUS_OK || encoded_range != decoded_range) {
      throw std::runtime_error("FEC normal-packet entropy range mismatch at frame " + std::to_string(frame));
    }
    packets.emplace_back(packet.begin(), packet.begin() + length);
  }
  return packets;
}

struct recovery_result {
  std::vector<std::int16_t> fec;
  std::vector<std::int16_t> current;
};

struct fec_quality_result {
  double recovery_error;
  double plc_error;
  std::size_t packet_bytes;
  bool has_fec;
};

template <typename Decoder, auto Create, auto Destroy, auto Decode>
[[nodiscard]] auto recover(const packet_stream& packets, int channels, int frame_size) -> recovery_result {
  int error = OPUS_OK;
  std::unique_ptr<Decoder, void (*)(Decoder*)> decoder{Create(sample_rate, channels, &error), Destroy};
  if (!decoder || error != OPUS_OK) {
    throw std::runtime_error("FEC decoder setup failed");
  }
  std::vector<std::int16_t> scratch(static_cast<std::size_t>(frame_size * channels));
  for (int frame = 0; frame < dropped_packet; ++frame) {
    const auto& packet = packets[static_cast<std::size_t>(frame)];
    const int decoded = Decode(decoder.get(), packet.data(), static_cast<int>(packet.size()), scratch.data(), frame_size, 0);
    if (decoded != frame_size) {
      std::cerr << "decode_before_fec frame=" << frame << " expected=" << frame_size << " actual=" << decoded
                << " packet_bytes=" << packet.size() << '\n';
      throw std::runtime_error("normal decode before FEC failed");
    }
  }
  recovery_result result{scratch, scratch};
  const auto& recovery_packet = packets[static_cast<std::size_t>(dropped_packet + 1)];
  if (Decode(decoder.get(), recovery_packet.data(), static_cast<int>(recovery_packet.size()), result.fec.data(), frame_size, 1) !=
          frame_size ||
      Decode(decoder.get(), recovery_packet.data(), static_cast<int>(recovery_packet.size()), result.current.data(), frame_size, 0) !=
          frame_size) {
    throw std::runtime_error("FEC recovery decode failed");
  }
  return result;
}

template <typename Decoder, auto Create, auto Destroy, auto Decode>
[[nodiscard]] auto conceal(const packet_stream& packets, int channels, int frame_size) -> std::vector<std::int16_t> {
  int error = OPUS_OK;
  std::unique_ptr<Decoder, void (*)(Decoder*)> decoder{Create(sample_rate, channels, &error), Destroy};
  if (!decoder || error != OPUS_OK) {
    throw std::runtime_error("PLC decoder setup failed");
  }
  std::vector<std::int16_t> output(static_cast<std::size_t>(frame_size * channels));
  for (int frame = 0; frame < dropped_packet; ++frame) {
    const auto& packet = packets[static_cast<std::size_t>(frame)];
    if (Decode(decoder.get(), packet.data(), static_cast<int>(packet.size()), output.data(), frame_size, 0) != frame_size) {
      throw std::runtime_error("normal decode before PLC failed");
    }
  }
  if (Decode(decoder.get(), nullptr, 0, output.data(), frame_size, 0) != frame_size) {
    throw std::runtime_error("PLC decode failed");
  }
  return output;
}

template <typename Decoder, auto Create, auto Destroy, auto Decode>
[[nodiscard]] auto decode_target(const packet_stream& packets, int channels, int frame_size) -> std::vector<std::int16_t> {
  int error = OPUS_OK;
  std::unique_ptr<Decoder, void (*)(Decoder*)> decoder{Create(sample_rate, channels, &error), Destroy};
  if (!decoder || error != OPUS_OK) {
    throw std::runtime_error("FEC reference decoder setup failed");
  }
  std::vector<std::int16_t> output(static_cast<std::size_t>(frame_size * channels));
  for (int frame = 0; frame <= dropped_packet; ++frame) {
    const auto& packet = packets[static_cast<std::size_t>(frame)];
    if (Decode(decoder.get(), packet.data(), static_cast<int>(packet.size()), output.data(), frame_size, 0) != frame_size) {
      throw std::runtime_error("normal FEC reference decode failed");
    }
  }
  return output;
}

[[nodiscard]] auto normalized_error(const std::vector<std::int16_t>& lhs, const std::vector<std::int16_t>& rhs,
                                    std::size_t offset = 0) noexcept -> double {
  double error = 0.0;
  double reference = 1.0;
  for (std::size_t index = offset; index < lhs.size(); ++index) {
    const double delta = static_cast<double>(lhs[index]) - rhs[index];
    error += delta * delta;
    reference += static_cast<double>(rhs[index]) * rhs[index];
  }
  return std::sqrt(error / reference);
}

template <typename Encoder, auto EncoderCreate, auto EncoderDestroy, auto EncoderControl, auto Encode>
[[nodiscard]] auto validate_direction(int channels, int bitrate, int duration_ms, bool vbr, int profile, bool current_encoder)
    -> fec_quality_result {
  const int frame_size = sample_rate * duration_ms / 1000;
  const auto pcm = make_voice(channels, frame_size, profile);
  const auto packets = encode_fec<Encoder, EncoderCreate, EncoderDestroy, EncoderControl, Encode>(pcm, channels, bitrate, frame_size, vbr);
  std::uint32_t packet_hash = 2166136261u;
  std::size_t packet_bytes = 0;
  for (const auto& packet : packets) {
    packet_bytes += packet.size();
    for (const auto byte : packet) {
      packet_hash = (packet_hash ^ byte) * 16777619u;
    }
  }
  const auto current =
      recover<curr_OpusDecoder, curr_opus_decoder_create, curr_opus_decoder_destroy, curr_opus_decode>(packets, channels, frame_size);
  const auto official = recover<OpusDecoder, opus_decoder_create, opus_decoder_destroy, opus_decode>(packets, channels, frame_size);
  const auto plc = conceal<OpusDecoder, opus_decoder_create, opus_decoder_destroy, opus_decode>(packets, channels, frame_size);
  const auto current_plc =
      conceal<curr_OpusDecoder, curr_opus_decoder_create, curr_opus_decoder_destroy, curr_opus_decode>(packets, channels, frame_size);
  const auto reference = decode_target<OpusDecoder, opus_decoder_create, opus_decoder_destroy, opus_decode>(packets, channels, frame_size);
  const auto fec_samples = static_cast<std::size_t>(std::min(frame_size, sample_rate / 50) * channels);
  const double decoder_error = normalized_error(current.fec, official.fec, current.fec.size() - fec_samples);
  const double fec_plc_difference = normalized_error(official.fec, plc);
  const double current_fec_plc_difference = normalized_error(current.fec, current_plc);
  const double continuation_error = normalized_error(current.current, official.current);
  const double recovery_error = normalized_error(official.fec, reference);
  const double plc_error = normalized_error(plc, reference);
  const bool has_fec = fec_plc_difference >= .01;
  std::cout << "fec_interop=" << (current_encoder ? "opuscpp_to_official" : "official_to_opuscpp") << " channels=" << channels
            << " duration_ms=" << duration_ms << " vbr=" << vbr << " profile=" << profile << " decoder_error=" << decoder_error
            << " continuation_error=" << continuation_error << " fec_plc_difference=" << fec_plc_difference
            << " recovery_error=" << recovery_error << " plc_error=" << plc_error << " packet_bytes=" << packet_bytes
            << " packet_hash=" << packet_hash << '\n';
  if ((has_fec ? decoder_error : current_fec_plc_difference) > .12 || continuation_error > .12) {
    throw std::runtime_error("FEC interoperability quality check failed");
  }
  if (current_encoder && has_fec && recovery_error >= plc_error) {
    throw std::runtime_error("FEC recovery did not improve on PLC");
  }
  return {recovery_error, plc_error, packet_bytes, has_fec};
}

} // namespace

int main() {
  try {
    struct test_case {
      int channels;
      int duration_ms;
      int bitrate;
      bool vbr;
    };
    constexpr std::array cases{test_case{1, 10, 24000, true}, test_case{1, 20, 16000, true}, test_case{1, 20, 24000, true},
                               test_case{1, 20, 32000, true}, test_case{1, 40, 24000, true}, test_case{1, 60, 24000, true},
                               test_case{2, 20, 32000, false}, test_case{2, 20, 48000, true}};
    double opuscpp_error = 0;
    double official_error = 0;
    std::size_t opuscpp_bytes = 0;
    std::size_t official_bytes = 0;
    int opuscpp_coverage = 0;
    int official_coverage = 0;
    int opuscpp_wins = 0;
    int quality_cases = 0;
    for (const auto& test : cases) {
      const int frame_size = sample_rate * test.duration_ms / 1000;
      auto quiet = make_voice(test.channels, frame_size, 3);
      static_cast<void>(encode_fec<curr_OpusEncoder, curr_opus_encoder_create, curr_opus_encoder_destroy, curr_opus_encoder_ctl, curr_opus_encode>(quiet, test.channels, test.bitrate, frame_size, test.vbr));
      std::reverse(quiet.begin(), quiet.end());
      static_cast<void>(encode_fec<curr_OpusEncoder, curr_opus_encoder_create, curr_opus_encoder_destroy, curr_opus_encoder_ctl, curr_opus_encode>(quiet, test.channels, test.bitrate, frame_size, test.vbr, true));
    }
    std::cout << "fec_quiet_packet_validation=PASS (silent startup, speech/silence and bitrate transitions, official decoder entropy ranges)\n";
    for (int profile = 0; profile < 3; ++profile) {
      for (const auto& test : cases) {
        const auto opuscpp =
            validate_direction<curr_OpusEncoder, curr_opus_encoder_create, curr_opus_encoder_destroy, curr_opus_encoder_ctl,
                               curr_opus_encode>(test.channels, test.bitrate, test.duration_ms, test.vbr, profile, true);
        const auto official = validate_direction<OpusEncoder, opus_encoder_create, opus_encoder_destroy, opus_encoder_ctl, opus_encode>(
            test.channels, test.bitrate, test.duration_ms, test.vbr, profile, false);
        if (test.duration_ms <= 20) {
          opuscpp_error += opuscpp.recovery_error;
          official_error += official.recovery_error;
          opuscpp_bytes += opuscpp.packet_bytes;
          official_bytes += official.packet_bytes;
          opuscpp_coverage += opuscpp.has_fec;
          official_coverage += official.has_fec;
          opuscpp_wins += opuscpp.recovery_error < official.recovery_error;
          ++quality_cases;
          std::cout << "fec_quality_ratio_opuscpp_vs_official=" << opuscpp.recovery_error / official.recovery_error << '\n';
        }
      }
    }
    std::cout << "fec_summary recovery_error_ratio=" << opuscpp_error / official_error << " wins=" << opuscpp_wins << '/' << quality_cases
              << " coverage=" << opuscpp_coverage << '/' << quality_cases << " official_coverage=" << official_coverage << '/'
              << quality_cases << " packet_byte_ratio=" << static_cast<double>(opuscpp_bytes) / official_bytes << '\n';
    if (opuscpp_error >= official_error || opuscpp_wins != quality_cases || opuscpp_coverage != quality_cases ||
        opuscpp_bytes > official_bytes) {
      throw std::runtime_error("opuscpp FEC did not outperform official Opus");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
