#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "../src/opus_codec.cpp"

namespace {

constexpr auto bench_iterations = 2000;
constexpr auto checksum_scale = 1000003u;

struct QuantCase {
  const char* name;
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

[[nodiscard]] auto checksum_bytes(const unsigned char* data, int size, std::uint32_t hash) noexcept -> std::uint32_t {
  for (int i = 0; i < size; ++i) {
    hash = mix(hash, data[i]);
  }
  return hash;
}

[[nodiscard]] auto checksum_norms(const std::vector<celt_norm>& data, std::uint32_t hash) noexcept -> std::uint32_t {
  for (const auto value : data) {
    hash = mix(hash, static_cast<std::uint32_t>(std::lrint(value * 8192.0f)) & 0xffffu);
  }
  return hash;
}

[[nodiscard]] auto rank_cwrs(const std::span<const opus_int16> pulses) noexcept -> opus_uint32 {
  int position = static_cast<int>(pulses.size()) - 1;
  int accumulated = std::abs(pulses[static_cast<std::size_t>(position)]);
  auto index = accumulated != 0 && pulses[static_cast<std::size_t>(position)] < 0 ? 1U : 0U;
  while (position-- > 0) {
    const auto pulse = pulses[static_cast<std::size_t>(position)];
    const int magnitude = std::abs(pulse);
    index += celt_pvq_u_entry(static_cast<int>(pulses.size()) - position, accumulated);
    accumulated += magnitude;
    if (magnitude != 0 && pulse < 0) {
      index += celt_pvq_u_entry(static_cast<int>(pulses.size()) - position, accumulated + 1);
    }
  }
  return index;
}

void check_cwrs_roundtrip() {
  const auto has_entry = [](int dimensions, int pulses) {
    const auto row = static_cast<unsigned>(std::min(dimensions, pulses));
    return row < celt_pvq_table_row_count && std::max(dimensions, pulses) <= celt_pvq_fast_row_max_columns[row];
  };
  const auto* mode = celt_mode();
  std::array<opus_int16, celt_max_band_samples> decoded{};
  for (int lm = -1; lm <= celt_max_lm; ++lm) {
    for (int band = 0; band < celt_default_nb_ebands; ++band) {
      const int n = ((mode->eBands[band + 1] - mode->eBands[band]) << (lm + 1)) >> 1;
      if (n < 2)
        continue;

      const int cache_index = mode->cache_index[(lm + 1) * celt_default_nb_ebands + band];
      if (cache_index < 0 || n > celt_max_band_samples)
        throw std::runtime_error("CWRS invalid cached dimension: n=" + std::to_string(n));
      const int max_q = mode->cache_bits[cache_index];
      for (int q = 1; q <= max_q; ++q) {
        const int k = q < 8 ? q : (8 + (q & 7)) << ((q >> 3) - 1);
        if (!has_entry(n, k) || !has_entry(n, k + 1))
          throw std::runtime_error("CWRS packed bounds failed: n=" + std::to_string(n) + " k=" + std::to_string(k));

        const auto total = static_cast<std::uint64_t>(celt_pvq_u_entry(n, k)) + celt_pvq_u_entry(n, k + 1);
        if (total < 2 || total > std::numeric_limits<opus_uint32>::max())
          throw std::runtime_error("CWRS cardinality failed: n=" + std::to_string(n) + " k=" + std::to_string(k));

        const std::array<std::uint64_t, 7> samples{0, total / 7, total / 3, total / 2, 5 * total / 7, total - 2, total - 1};
        for (const auto index : samples) {
          zero_n_items(decoded.data(), decoded.size());
          celt_pvq_unrank(n, k, static_cast<opus_uint32>(index), decoded.data());
          const auto vector = std::span{decoded}.first(static_cast<std::size_t>(n));
          const int pulse_count = std::accumulate(vector.begin(), vector.end(), 0, [](int sum, opus_int16 pulse) {
            return sum + std::abs(pulse);
          });
          if (pulse_count != k || rank_cwrs(vector) != index)
            throw std::runtime_error("CWRS roundtrip failed: n=" + std::to_string(n) + " k=" + std::to_string(k) +
                                     " index=" + std::to_string(index));
        }
      }
    }
  }
}

void prepare_case(const CeltModeInternal* mode, const QuantCase& test, int LM, std::vector<celt_norm>& source,
                  std::vector<celt_ener>& band_energy, std::vector<int>& pulses, std::vector<int>& tf_res) {
  const int N = (1 << LM) * celt_default_overlap;
  source.assign(static_cast<std::size_t>(test.channels * N), 0.0f);
  for (int c = 0; c < test.channels; ++c) {
    for (int i = 0; i < N; ++i) {
      const float phase = static_cast<float>((i + 3) * (c + 1));
      source[static_cast<std::size_t>(c * N + i)] = 0.42f * std::sin(phase * 0.017f) + 0.21f * std::cos(phase * 0.071f);
    }
  }

  band_energy.assign(static_cast<std::size_t>(test.channels * celt_default_nb_ebands), 1.0f);
  for (int c = 0; c < test.channels; ++c) {
    for (int b = 0; b < celt_default_nb_ebands; ++b) {
      band_energy[static_cast<std::size_t>(c * celt_default_nb_ebands + b)] = 1.0f + 0.05f * static_cast<float>((b + c) % 7);
    }
  }

  pulses.assign(static_cast<std::size_t>(celt_default_nb_ebands), 0);
  for (int b = 0; b < celt_default_nb_ebands; ++b) {
    const int width = (mode->eBands[b + 1] - mode->eBands[b]) << LM;
    pulses[static_cast<std::size_t>(b)] = std::min(760, 24 + width * test.channels * test.bits_per_bin_q3);
  }
  tf_res.assign(static_cast<std::size_t>(celt_default_nb_ebands), 0);
}

[[nodiscard]] auto quant_encode_once(const QuantCase& test, int LM, const std::vector<celt_norm>& source,
                                     const std::vector<celt_ener>& band_energy, std::vector<int>& pulses, std::vector<int>& tf_res,
                                     unsigned char* packet, int packet_bytes) -> int {
  const int N = (1 << LM) * celt_default_overlap;
  auto work = source;
  ec_enc enc{};
  ec_enc_init(&enc, packet, static_cast<opus_uint32>(packet_bytes));
  const auto total_bits = static_cast<opus_int32>(packet_bytes * (8 << 3) - 1);
  quant_all_bands(1, 0, celt_default_nb_ebands, work.data(), test.channels == 2 ? work.data() + N : nullptr, nullptr,
                  band_energy.data(), pulses.data(), 0, 2, 0, celt_default_nb_ebands, tf_res.data(), total_bits, 0, &enc, LM,
                  celt_default_nb_ebands, nullptr, 0);
  ec_enc_done(&enc);
  return (ec_tell(&enc) + 7) >> 3;
}

[[nodiscard]] auto quant_decode_once(const QuantCase& test, int LM, const unsigned char* packet, int packet_bytes, std::vector<int>& pulses,
                                     std::vector<int>& tf_res, opus_uint32 seed, std::uint32_t& checksum) -> int {
  const int N = (1 << LM) * celt_default_overlap;
  std::vector<celt_norm> work(static_cast<std::size_t>(test.channels * N), 0.0f);
  std::array<unsigned char, 2 * 64> collapse_masks{};
  ec_dec dec{};
  ec_dec_init(&dec, const_cast<unsigned char*>(packet), static_cast<opus_uint32>(packet_bytes));
  const auto total_bits = static_cast<opus_int32>(packet_bytes * (8 << 3) - 1);
  quant_all_bands(0, 0, celt_default_nb_ebands, work.data(), test.channels == 2 ? work.data() + N : nullptr, collapse_masks.data(), nullptr,
                  pulses.data(), 0, 2, 0, celt_default_nb_ebands, tf_res.data(), total_bits, 0, &dec, LM, celt_default_nb_ebands, &seed, 0);
  checksum = checksum_norms(work, checksum);
  return dec.error;
}

[[nodiscard]] auto run_case(const QuantCase& test) -> BenchResult {
  const auto* mode = celt_mode();
  constexpr int LM = 3;
  std::vector<celt_norm> source;
  std::vector<celt_ener> band_energy;
  std::vector<int> pulses;
  std::vector<int> tf_res;
  prepare_case(mode, test, LM, source, band_energy, pulses, tf_res);

  std::array<unsigned char, 1275> packet{};
  const int packet_bytes = static_cast<int>(packet.size());
  auto result = BenchResult{};
  result.bytes = quant_encode_once(test, LM, source, band_energy, pulses, tf_res, packet.data(), packet_bytes);
  result.checksum = checksum_bytes(packet.data(), result.bytes, 0);

  const auto encode_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < bench_iterations; ++i) {
    result.bytes = quant_encode_once(test, LM, source, band_energy, pulses, tf_res, packet.data(), packet_bytes);
    result.checksum = checksum_bytes(packet.data(), result.bytes, result.checksum);
  }
  const auto encode_end = std::chrono::steady_clock::now();

  const auto decode_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < bench_iterations; ++i) {
    const int error = quant_decode_once(test, LM, packet.data(), result.bytes, pulses, tf_res, 0x12345678u + static_cast<opus_uint32>(i),
                                        result.checksum);
    if (error != 0) {
      throw std::runtime_error(std::string{"decode error in "} + test.name);
    }
  }
  const auto decode_end = std::chrono::steady_clock::now();

  result.encode_ms = std::chrono::duration<double, std::milli>(encode_end - encode_begin).count();
  result.decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_begin).count();
  return result;
}

}

int main() {
  try {
    fill_celt_pvq_u_fast_rows();
    check_cwrs_roundtrip();
    const std::array cases{
        QuantCase{"mono-mid", 1, 6},
        QuantCase{"mono-high", 1, 12},
        QuantCase{"stereo-mid", 2, 5},
        QuantCase{"stereo-high", 2, 10},
    };

    for (const auto& test : cases) {
      const auto result = run_case(test);
      std::cout << test.name << " bytes=" << result.bytes << " encode_ms=" << result.encode_ms << " decode_ms=" << result.decode_ms
                << " checksum=" << result.checksum << '\n';
    }
  } catch (const std::exception& ex) {
    std::cerr << "quant_all_bands_microbench failed: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
