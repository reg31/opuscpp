#include "../src/opus_codec.cpp"
#include <cstdio>
#include <limits>

template <bool KnownZero>
static silk_nsq_candidate_pair ref_pair(opus_int32 residual_q10, int Lambda_Q10, int offset_Q10) {
  if constexpr (KnownZero)
    return {offset_Q10, offset_Q10, 0, 0, 0, 0};
  opus_int32 q1_Q10 = residual_q10 - offset_Q10;
  opus_int32 q1_Q0 = q1_Q10 >> 10;
  if (Lambda_Q10 > 2048) {
    const auto rdo_offset = Lambda_Q10 / 2 - 512;
    if (q1_Q10 > rdo_offset) {
      q1_Q0 = (q1_Q10 - rdo_offset) >> 10;
    } else if (q1_Q10 < -rdo_offset)
      q1_Q0 = (q1_Q10 + rdo_offset) >> 10;
    else
      q1_Q0 = q1_Q10 < 0 ? -1 : 0;
  }
  opus_int32 q2_Q10;
  if (q1_Q0 > 0) {
    q1_Q10 = q1_Q0 * 1024 - 80 + offset_Q10;
    q2_Q10 = q1_Q10 + 1024;
  } else if (q1_Q0 == 0) {
    q1_Q10 = offset_Q10;
    q2_Q10 = q1_Q10 + (1024 - 80);
  } else if (q1_Q0 == -1) {
    q2_Q10 = offset_Q10;
    q1_Q10 = q2_Q10 - (1024 - 80);
  } else {
    q1_Q10 = q1_Q0 * 1024 + 80 + offset_Q10;
    q2_Q10 = q1_Q10 + 1024;
  }
  const auto lambda_i16 = static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10));
  const auto rd1_bias = static_cast<opus_int32>(static_cast<opus_int16>(q1_Q10 < 0 ? -q1_Q10 : q1_Q10)) * lambda_i16;
  const auto rd2_bias = static_cast<opus_int32>(static_cast<opus_int16>(q2_Q10 < 0 ? -q2_Q10 : q2_Q10)) * lambda_i16;
  return {q1_Q10, q2_Q10, silk_square_i16(residual_q10 - q1_Q10), silk_square_i16(residual_q10 - q2_Q10), rd1_bias, rd2_bias};
}

static_assert(sizeof(silk_nsq_quant_level_pair) == 8 && sizeof(silk_nsq_quant_levels) == 1984);
int main() {
  const int lambdas[] = {0, 1, 512, 1024, 2047, 2048, 2049, 3072, 4096, 8192, 16384, 32767, 32768, 65535, std::numeric_limits<int>::max()};
  long cases = 0, failures = 0, max_index = 0;
  for (int row = 0; row < 2; ++row)
    for (int col = 0; col < 2; ++col) {
      const int offset = silk_Quantization_Offsets_Q10[row][col];
      for (int Lambda : lambdas)
        for (int residual = -31744; residual <= 30720; ++residual)
          for (int kz = 0; kz < 2; ++kz) {
            if (!kz) {
              opus_int32 q1 = residual - offset;
              opus_int32 qb = q1 >> 10;
              if (Lambda > 2048) {
                const auto rdo = Lambda / 2 - 512;
                if (q1 > rdo)
                  qb = (q1 - rdo) >> 10;
                else if (q1 < -rdo)
                  qb = (q1 + rdo) >> 10;
                else
                  qb = q1 < 0 ? -1 : 0;
              }
              const long idx = qb + 32;
              if (idx < 0 || idx > 61) {
                std::printf("INDEX_FAIL %ld\n", idx);
                return 2;
              }
              max_index = std::max(max_index, idx);
            }
            const silk_nsq_candidate_pair a = kz ? silk_quantize_candidate_pair<true>(residual, Lambda, offset, nullptr)
                                                 : silk_quantize_candidate_pair<false>(residual, Lambda, offset, silk_nsq_quant_levels[row][col].data());
            const silk_nsq_candidate_pair b = kz ? ref_pair<true>(residual, Lambda, offset) : ref_pair<false>(residual, Lambda, offset);
            ++cases;
            if (a.q1_Q10 != b.q1_Q10 || a.q2_Q10 != b.q2_Q10 || a.dist1_Q20 != b.dist1_Q20 || a.dist2_Q20 != b.dist2_Q20 ||
                a.rate1_Q20 != b.rate1_Q20 || a.rate2_Q20 != b.rate2_Q20) {
              if (failures < 5)
                std::printf("FAIL row=%d col=%d lambda=%d residual=%d kz=%d\n", row, col, Lambda, residual, kz);
              ++failures;
            }
          }
    }
  std::printf("ORACLE cases=%ld failures=%ld max_index_seen=%ld table_bytes=%zu\n", cases, failures, max_index, sizeof(silk_nsq_quant_levels));
  return failures == 0 ? 0 : 1;
}
