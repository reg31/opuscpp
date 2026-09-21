// Regression test: an explicit unconstrained-VBR hybrid VOIP stream must not grow with caller buffer.
//
// Scenario: mono VOIP at 16 kbps (the voip_mono_silk_budget_boost window), VBR=1 and
// OPUS_SET_VBR_CONSTRAINT(0), actual hybrid packets. Large caller max_data_bytes must not be converted
// into a requested CELT rate: packet sequences must stay capacity-invariant while both buffers are
// verified nonbinding (natural packets well below the smallest buffer, measured across all runs).
// Every returned packet is size-checked, every packet is decoded with a finite-output check, and every
// ctl return is checked. Covers 10/20 ms frames and the float API, plus 40/60 ms multiframe packets,
// without imposing a new strict average-bitrate contract. The automatic-bitrate case is a labeled
// capacity-invariance compatibility control only: automatic mode selection may legitimately choose
// non-hybrid frames, so its observed mode counts are reported instead of a hybrid requirement.
//
// Source under test is the codec translation unit itself (internal-test style):
//   production:            tests/unconstrained_capacity_vbr.cpp -> #include "../src/opus_codec.cpp"
//   private candidate run: -DOPUSCPP_UNCONSTRAINED_CAPACITY_SRC='"C/opus_codec.cpp"'
//
// Build and run standalone (there is no CMake registration here):
//   g++ -std=c++23 -O2 -DNDEBUG -Isrc tests/unconstrained_capacity_vbr.cpp -o unconstrained_capacity_vbr_test
//   ./unconstrained_capacity_vbr_test
#ifndef OPUSCPP_UNCONSTRAINED_CAPACITY_SRC
#define OPUSCPP_UNCONSTRAINED_CAPACITY_SRC "../src/opus_codec.cpp"
#endif
#include OPUSCPP_UNCONSTRAINED_CAPACITY_SRC

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

constexpr int sample_rate = 48000;
constexpr int channels = 1;
constexpr int bitrate = 16000;
constexpr int complexity = 10;
constexpr double pi = 3.14159265358979323846;
constexpr int total_seconds = 2;
constexpr int total_samples = sample_rate * total_seconds;
constexpr int smallest_capacity = 512;

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what, int value = 0) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("unconstrained_capacity_vbr FAIL: %s value=%d\n", what, value);
  }
}

void check_ctl(int status, const char* what) {
  check(status == OPUS_OK, what, status);
}

[[nodiscard]] auto make_pcm16() -> std::vector<std::int16_t> {
  auto pcm = std::vector<std::int16_t>(total_samples);
  std::uint32_t noise = 0x12345678u;
  for (int i = 0; i < total_samples; ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const double envelope = 0.55 + 0.30 * std::sin(2.0 * pi * 3.1 * t);
    noise = noise * 1664525u + 1013904223u;
    const double dither = (static_cast<double>((noise >> 8) & 0xffff) / 32768.0 - 1.0) * 0.06;
    const double base = envelope * (0.42 * std::sin(2.0 * pi * 210.0 * t) + 0.20 * std::sin(2.0 * pi * 900.0 * t) +
                                    0.16 * std::sin(2.0 * pi * 4200.0 * t) + 0.12 * std::sin(2.0 * pi * 6800.0 * t) + dither);
    pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::lrint(std::clamp(base, -0.95, 0.95) * 32767.0));
  }
  return pcm;
}

using packet_vector = std::vector<std::vector<unsigned char>>;

struct decode_check final {
  bool all_decoded = true;
  bool all_finite = true;
  bool all_hybrid = false;
  int hybrid_count = 0;
  int celt_count = 0;
  int silk_count = 0;
  int max_packet_size = 0;
};

[[nodiscard]] auto encode_stream(bool use_float, int frame_size, int max_data_bytes, bool automatic_bitrate, int vbr_constraint,
                                 const std::vector<std::int16_t>& pcm16, const std::vector<float>& pcm_f) -> packet_vector {
  int error = OPUS_OK;
  std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)> encoder(opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error),
                                                               [](OpusEncoder* st) {
                                                                 opus_encoder_destroy(st);
                                                               });
  if (!encoder || error != OPUS_OK) {
    throw std::runtime_error("encoder create failed");
  }
  check_ctl(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(automatic_bitrate ? OPUS_AUTO : bitrate)), "set bitrate failed");
  check_ctl(opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(complexity)), "set complexity failed");
  check_ctl(opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(1)), "set VBR failed");
  check_ctl(opus_encoder_ctl(encoder.get(), OPUS_SET_VBR_CONSTRAINT(vbr_constraint)), "set VBR constraint failed");
  int encoder_lookahead = 0;
  check_ctl(opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&encoder_lookahead)), "get lookahead failed");
  check(encoder_lookahead > 0, "encoder lookahead must be positive", encoder_lookahead);
  const int frame_count = total_samples / frame_size;
  auto packets = packet_vector(static_cast<std::size_t>(frame_count));
  std::array<unsigned char, 1500> packet{};
  for (int frame = 0; frame < frame_count; ++frame) {
    const std::size_t offset = static_cast<std::size_t>(frame) * static_cast<std::size_t>(frame_size);
    const int packet_size = use_float
                                ? opus_encode_float(encoder.get(), pcm_f.data() + offset, frame_size, packet.data(), max_data_bytes)
                                : opus_encode(encoder.get(), pcm16.data() + offset, frame_size, packet.data(), max_data_bytes);
    check(packet_size > 0 && packet_size <= max_data_bytes && packet_size <= static_cast<int>(packet.size()),
          "encode returned an invalid packet size", packet_size);
    if (packet_size <= 0 || packet_size > max_data_bytes || packet_size > static_cast<int>(packet.size())) {
      throw std::runtime_error("encode returned an invalid packet size");
    }
    packets[static_cast<std::size_t>(frame)].assign(packet.begin(), packet.begin() + packet_size);
  }
  return packets;
}

[[nodiscard]] auto decode_and_check(bool use_float, int frame_size, const packet_vector& packets) -> decode_check {
  int error = OPUS_OK;
  std::unique_ptr<OpusDecoder, void (*)(OpusDecoder*)> decoder(opus_decoder_create(sample_rate, channels, &error),
                                                               [](OpusDecoder* st) {
                                                                 opus_decoder_destroy(st);
                                                               });
  if (!decoder || error != OPUS_OK) {
    throw std::runtime_error("decoder create failed");
  }
  std::vector<float> decoded_f(static_cast<std::size_t>(frame_size));
  std::vector<std::int16_t> decoded16(static_cast<std::size_t>(frame_size));
  decode_check out{};
  for (const auto& packet : packets) {
    const int got = use_float ? opus_decode_float(decoder.get(), packet.data(), static_cast<int>(packet.size()), decoded_f.data(), frame_size, 0)
                              : opus_decode(decoder.get(), packet.data(), static_cast<int>(packet.size()), decoded16.data(), frame_size, 0);
    if (got != frame_size) {
      out.all_decoded = false;
    } else if (use_float) {
      for (int i = 0; i < frame_size; ++i) {
        if (!std::isfinite(decoded_f[static_cast<std::size_t>(i)])) {
          out.all_finite = false;
        }
      }
    }
    if (!packet.empty()) {
      const int config = packet[0] >> 3;
      if (config >= 12 && config < 16) {
        ++out.hybrid_count;
      } else if (config < 12) {
        ++out.silk_count;
      } else {
        ++out.celt_count;
      }
      out.max_packet_size = std::max(out.max_packet_size, static_cast<int>(packet.size()));
    }
  }
  out.all_hybrid = out.hybrid_count == static_cast<int>(packets.size());
  return out;
}

[[nodiscard]] auto make_float_pcm(const std::vector<std::int16_t>& pcm16) -> std::vector<float> {
  auto pcm_f = std::vector<float>(pcm16.size());
  for (std::size_t i = 0; i < pcm16.size(); ++i) {
    pcm_f[i] = static_cast<float>(pcm16[i]) / 32768.0f;
  }
  return pcm_f;
}

void check_case(bool use_float, int frame_size) {
  const auto pcm16 = make_pcm16();
  const auto pcm_f = make_float_pcm(pcm16);
  const auto run = [&](int capacity) {
    return encode_stream(use_float, frame_size, capacity, false, 0, pcm16, pcm_f);
  };
  const auto small = run(smallest_capacity);
  const auto large = run(1276);
  const auto huge = run(1500);
  check(small == large && large == huge, "packet sequences must be capacity-invariant");
  int max_packet = 0;
  for (const auto* stream : {&small, &large, &huge}) {
    const auto decoded = decode_and_check(use_float, frame_size, *stream);
    check(decoded.all_decoded, "every packet must decode to a full frame");
    check(decoded.all_finite, "float decoder output must be finite");
    check(decoded.all_hybrid, "stream must use actual hybrid packets");
    max_packet = std::max(max_packet, decoded.max_packet_size);
  }
  check(max_packet < smallest_capacity, "buffers must be verified nonbinding (max packet below the smallest buffer)", max_packet);
  std::printf("unconstrained_capacity_vbr case: %s frame=%d packets=%d max_packet=%d\n", use_float ? "float" : "int16", frame_size,
              static_cast<int>(small.size()), max_packet);
}

// Compatibility control only: automatic mode selection may legitimately choose non-hybrid frames, so
// hybrid coverage is not required here; the observed mode counts are printed. AUTO supplies no
// governor budget under either constraint flag, so both VBR_CONSTRAINT values are checked; the two
// settings are not required to produce the same packets as each other.
void check_automatic_capacity_control(int vbr_constraint) {
  constexpr int frame_size = 960;
  const auto pcm16 = make_pcm16();
  const auto pcm_f = make_float_pcm(pcm16);
  const auto run = [&](int capacity) {
    return encode_stream(false, frame_size, capacity, true, vbr_constraint, pcm16, pcm_f);
  };
  const auto small = run(smallest_capacity);
  const auto large = run(1276);
  const auto huge = run(1500);
  check(small == large && large == huge, "automatic bitrate: packet sequences must be capacity-invariant");
  const auto decoded = decode_and_check(false, frame_size, small);
  check(decoded.all_decoded, "automatic bitrate: every packet must decode to a full frame");
  check(decoded.max_packet_size < smallest_capacity, "automatic bitrate: buffers must be nonbinding", decoded.max_packet_size);
  std::printf("unconstrained_capacity_vbr case: int16 frame=%d auto-bitrate constraint=%d packets=%d max_packet=%d hybrid=%d celt=%d silk=%d\n",
              frame_size, vbr_constraint, static_cast<int>(small.size()), decoded.max_packet_size, decoded.hybrid_count, decoded.celt_count,
              decoded.silk_count);
}

} // namespace

int main() {
  try {
    check_case(false, 960);  // 20 ms int16
    check_case(true, 960);   // 20 ms float
    check_case(false, 480);  // 10 ms int16
    check_case(true, 480);   // 10 ms float
    check_case(false, 1920); // 40 ms multiframe int16
    check_case(false, 2880); // 60 ms multiframe int16
    check_automatic_capacity_control(0);
    check_automatic_capacity_control(1);
    std::printf("unconstrained_capacity_vbr %s checks=%d failures=%d\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::printf("unconstrained_capacity_vbr FAIL: %s\n", error.what());
    return 1;
  }
}
