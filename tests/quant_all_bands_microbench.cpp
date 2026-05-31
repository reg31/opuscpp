#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

#include "../src/opus_codec.cpp"

namespace {

constexpr auto bench_iterations = 2000;
constexpr auto checksum_scale = 1000003u;

struct QuantCase {
  const char *name;
  int channels;
  int bits_per_bin_q3;
};

struct BenchResult {
  double encode_ms{};
  double decode_ms{};
  std::uint32_t checksum{};
  int bytes{};
};

[[nodiscard]] auto mix(std::uint32_t hash, std::uint32_t value) noexcept -> std::uint32_t {
  return hash * checksum_scale + value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
}

[[nodiscard]] auto checksum_bytes(const unsigned char *data, int size, std::uint32_t hash) noexcept -> std::uint32_t {
  for (int i = 0; i < size; ++i) hash = mix(hash, data[i]);
  return hash;
}

[[nodiscard]] auto checksum_norms(const std::vector<celt_norm> &data, std::uint32_t hash) noexcept -> std::uint32_t {
  for (const auto value : data) {
    hash = mix(hash, static_cast<std::uint32_t>(std::lrint(value * 8192.0f)) & 0xffffu);
  }
  return hash;
}

void check_cwrs_roundtrip() {
  std::array<int, 176> pulses{};
  for (int n = 2; n <= 48; ++n) {
    for (int k = 0; k <= 32; ++k) {
      const auto total = celt_pvq_u_total(n, k);
      const std::array<opus_uint32, 7> samples{
          0U, total / 7U, total / 3U, total / 2U, (total * 5U) / 7U, total > 1U ? total - 2U : 0U, total - 1U};
      for (const auto index : samples) {
        fill_n_items(pulses.data(), pulses.size(), 0);
        auto writer = celt_pvq_dense_writer{pulses.data()};
        celt_pvq_unrank(n, k, index, writer);
        const auto roundtrip = icwrs(n, pulses.data());
        if (roundtrip != index) {
          std::cerr << "CWRS roundtrip failed: n=" << n << " k=" << k << " index=" << index << " got=" << roundtrip << '\n';
          std::exit(2);
        }
      }
    }
  }
}

void prepare_case(const CeltModeInternal *mode, const QuantCase &test, int LM, std::vector<celt_norm> &source,
                  std::vector<celt_ener> &band_energy, std::vector<int> &pulses, std::vector<int> &tf_res) {
  const int N = (1 << LM) * mode->shortMdctSize;
  source.assign(static_cast<std::size_t>(test.channels * N), 0.0f);
  for (int c = 0; c < test.channels; ++c) {
    for (int i = 0; i < N; ++i) {
      const float phase = static_cast<float>((i + 3) * (c + 1));
      source[static_cast<std::size_t>(c * N + i)] = 0.42f * std::sin(phase * 0.017f) + 0.21f * std::cos(phase * 0.071f);
    }
  }

  band_energy.assign(static_cast<std::size_t>(test.channels * mode->nbEBands), 1.0f);
  for (int c = 0; c < test.channels; ++c) {
    for (int b = 0; b < mode->nbEBands; ++b) {
      band_energy[static_cast<std::size_t>(c * mode->nbEBands + b)] = 1.0f + 0.05f * static_cast<float>((b + c) % 7);
    }
  }

  pulses.assign(static_cast<std::size_t>(mode->nbEBands), 0);
  for (int b = 0; b < mode->nbEBands; ++b) {
    const int width = (mode->eBands[b + 1] - mode->eBands[b]) << LM;
    pulses[static_cast<std::size_t>(b)] = std::min(760, 24 + width * test.channels * test.bits_per_bin_q3);
  }
  tf_res.assign(static_cast<std::size_t>(mode->nbEBands), 0);
}

[[nodiscard]] OPUS_NOINLINE auto quant_encode_once(const CeltModeInternal *mode, const QuantCase &test, int LM,
                                                   const std::vector<celt_norm> &source, const std::vector<celt_ener> &band_energy,
                                                   std::vector<int> &pulses, std::vector<int> &tf_res,
                                                   unsigned char *packet, int packet_bytes, opus_uint32 seed) -> int {
  const int N = (1 << LM) * mode->shortMdctSize;
  auto work = source;
  std::array<unsigned char, 2 * 64> collapse_masks{};
  ec_enc enc{};
  ec_enc_init(&enc, packet, static_cast<opus_uint32>(packet_bytes));
  const auto total_bits = static_cast<opus_int32>(packet_bytes * (8 << 3) - 1);
  quant_all_bands(1, mode, 0, mode->effEBands, work.data(), test.channels == 2 ? work.data() + N : nullptr, collapse_masks.data(),
                  band_energy.data(), pulses.data(), 0, 2, 0, mode->effEBands, tf_res.data(), total_bits, 0, &enc, LM, mode->effEBands,
                  &seed, 0);
  ec_enc_done(&enc);
  return (ec_tell(&enc) + 7) >> 3;
}

[[nodiscard]] OPUS_NOINLINE auto quant_decode_once(const CeltModeInternal *mode, const QuantCase &test, int LM,
                                                   const unsigned char *packet, int packet_bytes, std::vector<int> &pulses,
                                                   std::vector<int> &tf_res, opus_uint32 seed, std::uint32_t &checksum) -> int {
  const int N = (1 << LM) * mode->shortMdctSize;
  std::vector<celt_norm> work(static_cast<std::size_t>(test.channels * N), 0.0f);
  std::array<unsigned char, 2 * 64> collapse_masks{};
  ec_dec dec{};
  ec_dec_init(&dec, const_cast<unsigned char *>(packet), static_cast<opus_uint32>(packet_bytes));
  const auto total_bits = static_cast<opus_int32>(packet_bytes * (8 << 3) - 1);
  quant_all_bands(0, mode, 0, mode->effEBands, work.data(), test.channels == 2 ? work.data() + N : nullptr, collapse_masks.data(),
                  nullptr, pulses.data(), 0, 2, 0, mode->effEBands, tf_res.data(), total_bits, 0, &dec, LM, mode->effEBands, &seed, 0);
  checksum = checksum_norms(work, checksum);
  return dec.error;
}

[[nodiscard]] auto run_case(const QuantCase &test) -> BenchResult {
  const auto *mode = default_custom_mode();
  constexpr int LM = 3;
  std::vector<celt_norm> source;
  std::vector<celt_ener> band_energy;
  std::vector<int> pulses;
  std::vector<int> tf_res;
  prepare_case(mode, test, LM, source, band_energy, pulses, tf_res);

  std::array<unsigned char, 4096> packet{};
  const int packet_bytes = static_cast<int>(packet.size());
  auto result = BenchResult{};
  result.bytes = quant_encode_once(mode, test, LM, source, band_energy, pulses, tf_res, packet.data(), packet_bytes, 0x12345678u);
  result.checksum = checksum_bytes(packet.data(), result.bytes, 0);

  const auto encode_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < bench_iterations; ++i) {
    result.bytes = quant_encode_once(mode, test, LM, source, band_energy, pulses, tf_res, packet.data(), packet_bytes, 0x12345678u + static_cast<opus_uint32>(i));
    result.checksum = checksum_bytes(packet.data(), result.bytes, result.checksum);
  }
  const auto encode_end = std::chrono::steady_clock::now();

  const auto decode_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < bench_iterations; ++i) {
    const int error = quant_decode_once(mode, test, LM, packet.data(), result.bytes, pulses, tf_res, 0x12345678u + static_cast<opus_uint32>(i), result.checksum);
    if (error != 0) {
      std::cerr << "decode error in " << test.name << '\n';
      std::exit(3);
    }
  }
  const auto decode_end = std::chrono::steady_clock::now();

  result.encode_ms = std::chrono::duration<double, std::milli>(encode_end - encode_begin).count();
  result.decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_begin).count();
  return result;
}

} // namespace

int main() {
  check_cwrs_roundtrip();
  const std::array cases{
      QuantCase{"mono-mid", 1, 6},
      QuantCase{"mono-high", 1, 12},
      QuantCase{"stereo-mid", 2, 5},
      QuantCase{"stereo-high", 2, 10},
  };

  for (const auto &test : cases) {
    const auto result = run_case(test);
    std::cout << test.name << " bytes=" << result.bytes << " encode_ms=" << result.encode_ms << " decode_ms=" << result.decode_ms
              << " checksum=" << result.checksum << '\n';
  }
}
