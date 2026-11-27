#ifndef OPUSCPP_INTERNAL_SIMD_H
#define OPUSCPP_INTERNAL_SIMD_H

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#define OPUSCPP_SIMD_NEON 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#elif defined(__x86_64__) || defined(_M_X64)
#define OPUSCPP_SIMD_AVX2 1
#include <immintrin.h>
#endif

#if defined(OPUSCPP_SIMD_NEON) || defined(OPUSCPP_SIMD_AVX2)
#define OPUSCPP_HAS_SIMD 1
namespace opuscpp_simd {

#if defined(OPUSCPP_SIMD_NEON)
inline int32x4_t lpc_terms(int16x8_t h, uint16x8_t l, int16x8_t c) noexcept {
  const auto u = vreinterpretq_u16_s16(c);
  const auto hi = vcombine_u16(vshrn_n_u32(vmull_u16(vget_low_u16(l), vget_low_u16(u)), 16),
                               vshrn_n_u32(vmull_u16(vget_high_u16(l), vget_high_u16(u)), 16));
  const auto floor = vreinterpretq_s16_u16(vsubq_u16(hi, vandq_u16(l, vreinterpretq_u16_s16(vshrq_n_s16(c, 15)))));
  const auto upper = vaddq_s32(vmull_s16(vget_low_s16(h), vget_low_s16(c)),
                               vmull_s16(vget_high_s16(h), vget_high_s16(c)));
  return vpadalq_s16(upper, floor);
}

inline int32x4_t lpc_terms(const std::int16_t* high, const std::uint16_t* low, const std::int16_t* taps) noexcept {
  return lpc_terms(vld1q_s16(high), vld1q_u16(low), vld1q_s16(taps));
}

inline std::int32_t sum_words(int32x4_t sum) noexcept {
  return std::bit_cast<std::int32_t>(vaddvq_u32(vreinterpretq_u32_s32(sum)));
}
#else
inline std::int32_t sum_words(__m128i sum) noexcept {
  sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
  sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(2, 3, 0, 1)));
  return _mm_cvtsi128_si32(sum);
}
inline __m256i lpc_terms(__m256i h, __m256i l, __m256i c) noexcept {
  const auto floor = _mm256_sub_epi16(_mm256_mulhi_epu16(l, c), _mm256_and_si256(l, _mm256_srai_epi16(c, 15)));
  return _mm256_add_epi32(_mm256_madd_epi16(h, c), _mm256_madd_epi16(floor, _mm256_set1_epi16(1)));
}

inline std::int32_t sum_words(__m256i sum) noexcept {
  return sum_words(_mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1)));
}
#endif

[[nodiscard]] inline std::int32_t lpc16(const std::int16_t* high, const std::uint16_t* low,
                                        const std::int16_t* taps) noexcept {
#if defined(OPUSCPP_SIMD_NEON)
  const auto total = vaddq_s32(lpc_terms(high, low, taps), lpc_terms(high + 8, low + 8, taps + 8));
  return sum_words(total);
#else
  const auto h = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(high));
  const auto l = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(low));
  const auto c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(taps));
  return sum_words(lpc_terms(h, l, c));
#endif
}
inline void resampler_up2_hq(std::int32_t* state, std::int16_t* out, const std::int16_t* in, int length,
                             const std::array<std::int16_t, 3>& early,
                             const std::array<std::int16_t, 3>& late) noexcept {
#if defined(OPUSCPP_SIMD_NEON)
  using Pair = int32x2_t;
  const auto pair = [](int a, int b) {
    return vset_lane_s32(b, vdup_n_s32(a), 1);
  };
  const auto extend = [](int v) {
    return vdup_n_s32(v);
  };
  const auto add = [](Pair a, Pair b) {
    return vadd_s32(a, b);
  };
  const auto sub = [](Pair a, Pair b) {
    return vsub_s32(a, b);
  };
  const auto mul = [](Pair a, Pair b) {
    return vshrn_n_s64(vmull_s32(a, b), 16);
  };
  const auto low = [](Pair a) {
    return vget_lane_s32(a, 0);
  };
  const auto high = [](Pair a) {
    return vget_lane_s32(a, 1);
  };
#else
  using Pair = __m128i;
  const auto pair = [](int a, int b) {
    return _mm_setr_epi32(a, 0, b, 0);
  };
  const auto extend = [](int v) {
    return _mm_set1_epi32(v);
  };
  const auto add = [](Pair a, Pair b) {
    return _mm_add_epi32(a, b);
  };
  const auto sub = [](Pair a, Pair b) {
    return _mm_sub_epi32(a, b);
  };
  const auto mul = [](Pair a, Pair b) {
    return _mm_srli_epi64(_mm_mul_epi32(a, b), 16);
  };
  const auto low = [](Pair a) {
    return _mm_cvtsi128_si32(a);
  };
  const auto high = [](Pair a) {
    return _mm_extract_epi32(a, 2);
  };
#endif
  Pair s0 = pair(state[0], state[3]), s1 = pair(state[1], state[4]), s2 = pair(state[2], state[5]);
  const auto c0 = pair(early[0], late[0]);
  const auto c1 = pair(early[1], late[1]);
  const auto c2 = pair(early[2], late[2]);
  const auto filter = [&]<bool Last>(Pair v, Pair& state, Pair c) {
    const auto delta = sub(v, state);
    auto scaled = mul(delta, c);
    if constexpr (Last)
      scaled = add(scaled, delta);
    const auto y = add(state, scaled);
    state = add(v, scaled);
    return y;
  };
  for (int i = 0; i < length; ++i) {
    auto v = extend(static_cast<std::int32_t>(in[i]) * 1024);
    v = filter.template operator()<false>(v, s0, c0);
    v = filter.template operator()<false>(v, s1, c1);
    v = filter.template operator()<true>(v, s2, c2);
#if defined(OPUSCPP_SIMD_NEON)
    const auto round = [](std::int32_t sample) noexcept {
      return static_cast<std::int16_t>(std::clamp(((sample >> 9) + 1) >> 1, -32768, 32767));
    };
    out[2 * i] = round(low(v));
    out[2 * i + 1] = round(high(v));
#else
    const auto rounded = _mm_srai_epi32(_mm_add_epi32(_mm_srai_epi32(v, 9), _mm_set1_epi32(1)), 1);
    const auto packed_lanes = _mm_shuffle_epi32(rounded, _MM_SHUFFLE(2, 0, 2, 0));
    const auto packed = _mm_cvtsi128_si32(_mm_packs_epi32(packed_lanes, packed_lanes));
    std::memcpy(out + 2 * i, &packed, 4);
#endif
  }
  state[0] = low(s0);
  state[1] = low(s1);
  state[2] = low(s2);
  state[3] = high(s0);
  state[4] = high(s1);
  state[5] = high(s2);
}

#if defined(OPUSCPP_SIMD_NEON)
inline std::int32_t fir_dot8(int16x8_t samples, int16x8_t coefficients) noexcept {
  const auto products = vmull_s16(vget_low_s16(samples), vget_low_s16(coefficients));
  return vaddvq_s32(vmlal_high_s16(products, samples, coefficients));
}

struct FirThreePhases {
  int16x8_t first, second, third;

  FirThreePhases(const std::int16_t* phase_first, const std::int16_t* phase_second, const std::int16_t* phase_third) noexcept
      : first(vld1q_s16(phase_first)), second(vld1q_s16(phase_second)), third(vld1q_s16(phase_third)) {}

  template <bool AdvanceThird = true>
  auto operator()(const std::int16_t* window) const noexcept -> std::array<std::int32_t, 3> {
    const auto samples = vld1q_s16(window);
    return {fir_dot8(samples, first), fir_dot8(samples, second), fir_dot8(AdvanceThird ? vld1q_s16(window + 1) : samples, third)};
  }
};
#else
struct FirThreePhases {
  __m256i first_two;
  __m128i third;

  FirThreePhases(const std::int16_t* phase_first, const std::int16_t* phase_second, const std::int16_t* phase_third) noexcept
      : first_two(_mm256_set_m128i(_mm_loadu_si128(reinterpret_cast<const __m128i*>(phase_second)),
                                   _mm_loadu_si128(reinterpret_cast<const __m128i*>(phase_first)))),
        third(_mm_loadu_si128(reinterpret_cast<const __m128i*>(phase_third))) {}

  template <bool AdvanceThird = true>
  auto operator()(const std::int16_t* window) const noexcept -> std::array<std::int32_t, 3> {
    const auto samples = _mm_loadu_si128(reinterpret_cast<const __m128i*>(window));
    auto pairs = _mm256_madd_epi16(_mm256_broadcastsi128_si256(samples), first_two);
    pairs = _mm256_add_epi32(pairs, _mm256_shuffle_epi32(pairs, _MM_SHUFFLE(1, 0, 3, 2)));
    pairs = _mm256_add_epi32(pairs, _mm256_shuffle_epi32(pairs, _MM_SHUFFLE(2, 3, 0, 1)));
    const auto tail = AdvanceThird ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(window + 1)) : samples;
    return {_mm_cvtsi128_si32(_mm256_castsi256_si128(pairs)),
            _mm_cvtsi128_si32(_mm256_extracti128_si256(pairs, 1)), sum_words(_mm_madd_epi16(tail, third))};
  }
};
#endif

struct DecoderLpcTaps {
  std::array<std::int16_t, 16> older{};
  std::array<std::int16_t, 4> newest{};
};

inline DecoderLpcTaps prepare_decoder_lpc(const std::int16_t* coefficients) noexcept {
  DecoderLpcTaps taps;
#if defined(OPUSCPP_SIMD_AVX2)
  // Each 128-bit half of VPACKSSDW interleaves the two input groups.
  constexpr int slots[]{0, 1, 2, 3, 8, 9, 10, 11, 4, 5, 6, 7};
  for (int i = 0; i < 12; ++i)
    taps.older[slots[i]] = coefficients[15 - i];
#else
  for (int i = 0; i < 12; ++i)
    taps.older[i] = coefficients[15 - i];
#endif
  for (int i = 0; i < 4; ++i)
    taps.newest[i] = coefficients[i];
  return taps;
}

[[nodiscard]] inline std::int32_t decoder_lpc16(const std::int32_t* end, const DecoderLpcTaps& taps) noexcept {
#if defined(OPUSCPP_SIMD_NEON)
  const auto terms = [](int32x4_t a, int32x4_t b, const std::int16_t* p) {
    const auto h = vcombine_s16(vshrn_n_s32(a, 16), vshrn_n_s32(b, 16));
    const auto l = vreinterpretq_u16_s16(vcombine_s16(vmovn_s32(a), vmovn_s32(b)));
    return lpc_terms(h, l, vld1q_s16(p));
  };
  const auto older = sum_words(vaddq_s32(terms(vld1q_s32(end - 16), vld1q_s32(end - 12), taps.older.data()),
                                         terms(vld1q_s32(end - 8), vdupq_n_s32(0), taps.older.data() + 8)));
#else
  const auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(end - 16));
  const auto b = _mm256_zextsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(end - 8)));
  const auto h = _mm256_packs_epi32(_mm256_srai_epi32(a, 16), _mm256_srai_epi32(b, 16));
  const auto l = _mm256_packs_epi32(_mm256_srai_epi32(_mm256_slli_epi32(a, 16), 16), _mm256_srai_epi32(_mm256_slli_epi32(b, 16), 16));
  const auto older = sum_words(lpc_terms(h, l, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(taps.older.data()))));
#endif
  // Preserve four independent 32-bit loads at the recurrent end of the history.
  const volatile std::int32_t* newest = end;
  const auto tap = [&](int i) {
    const auto h = static_cast<std::int64_t>(newest[-1 - i]);
    return static_cast<std::uint32_t>((h * taps.newest[i]) >> 16);
  };
  return std::bit_cast<std::int32_t>(8u + static_cast<std::uint32_t>(older) + tap(0) + tap(1) + tap(2) + tap(3));
}

inline bool finite_samples(const float* input, int count) noexcept {
  int i = 0;
#if defined(OPUSCPP_SIMD_AVX2)
  const auto exponent = _mm256_set1_epi32(0x7f800000);
  auto exceptions = _mm256_setzero_si256();
  for (; i + 8 <= count; i += 8) {
    const auto bits = _mm256_castps_si256(_mm256_loadu_ps(input + i));
    exceptions = _mm256_or_si256(exceptions, _mm256_cmpeq_epi32(_mm256_and_si256(bits, exponent), exponent));
  }
  if (!_mm256_testz_si256(exceptions, exceptions))
    return false;
#else
  const auto exponent = vdupq_n_u32(0x7f800000U);
  auto exceptions = vdupq_n_u32(0);
  for (; i + 4 <= count; i += 4) {
    const auto bits = vreinterpretq_u32_f32(vld1q_f32(input + i));
    exceptions = vorrq_u32(exceptions, vceqq_u32(vandq_u32(bits, exponent), exponent));
  }
  if (vmaxvq_u32(exceptions) != 0)
    return false;
#endif
  for (; i < count; ++i)
    if ((std::bit_cast<std::uint32_t>(input[i]) & 0x7f800000U) == 0x7f800000U)
      return false;
  return true;
}

namespace nsq {
#if defined(OPUSCPP_SIMD_AVX2)
using Vector = __m128i;
inline auto load(const std::int32_t* x) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(x)); }
inline void store(std::int32_t* x, Vector y) { _mm_storeu_si128(reinterpret_cast<__m128i*>(x), y); }
inline auto set(int x) { return _mm_set1_epi32(x); }
inline auto add(Vector a, Vector b) { return _mm_add_epi32(a,b); }
inline auto sub(Vector a, Vector b) { return _mm_sub_epi32(a,b); }
inline auto mul(Vector a, Vector b) { return _mm_mullo_epi32(a,b); }
inline auto minimum(Vector a, Vector b) { return _mm_min_epi32(a,b); }
inline auto maximum(Vector a, Vector b) { return _mm_max_epi32(a,b); }
inline auto abs(Vector a) { return _mm_abs_epi32(a); }
inline auto eq(Vector a, Vector b) { return _mm_cmpeq_epi32(a,b); }
inline auto gt(Vector a, Vector b) { return _mm_cmpgt_epi32(a,b); }
inline auto vand(Vector a, Vector b) { return _mm_and_si128(a,b); }
inline auto vor(Vector a, Vector b) { return _mm_or_si128(a,b); }
inline auto select(Vector mask, Vector yes, Vector no) { return _mm_blendv_epi8(no,yes,mask); }
inline auto any(Vector a) { return !_mm_testz_si128(a,a); }
template<int N> inline auto shr(Vector a) { return _mm_srai_epi32(a,N); }
template<int N> inline auto shl(Vector a) { return _mm_slli_epi32(a,N); }
#else
using Vector = int32x4_t;
inline auto load(const std::int32_t* x) { return vld1q_s32(x); }
inline void store(std::int32_t* x, Vector y) { vst1q_s32(x,y); }
inline auto set(int x) { return vdupq_n_s32(x); }
inline auto add(Vector a, Vector b) { return vaddq_s32(a,b); }
inline auto sub(Vector a, Vector b) { return vsubq_s32(a,b); }
inline auto mul(Vector a, Vector b) { return vmulq_s32(a,b); }
inline auto minimum(Vector a, Vector b) { return vminq_s32(a,b); }
inline auto maximum(Vector a, Vector b) { return vmaxq_s32(a,b); }
inline auto abs(Vector a) { return vabsq_s32(a); }
inline auto eq(Vector a, Vector b) { return vreinterpretq_s32_u32(vceqq_s32(a,b)); }
inline auto gt(Vector a, Vector b) { return vreinterpretq_s32_u32(vcgtq_s32(a,b)); }
inline auto vand(Vector a, Vector b) { return vandq_s32(a,b); }
inline auto vor(Vector a, Vector b) { return vorrq_s32(a,b); }
inline auto select(Vector mask, Vector yes, Vector no) { return vbslq_s32(vreinterpretq_u32_s32(mask),yes,no); }
inline auto any(Vector a) { return vmaxvq_u32(vreinterpretq_u32_s32(a)) != 0; }
template<int N> inline auto shr(Vector a) { return vshrq_n_s32(a,N); }
template<int N> inline auto shl(Vector a) { return vshlq_n_s32(a,N); }
#endif

inline bool select_four(std::int32_t* selected_levels, std::int32_t* selected_costs,
                        std::int32_t* alternate_levels, std::int32_t* alternate_costs,
                        int lambda, int offset, bool exact_retry_ties) {
  const auto zero = set(0);
  const auto residual = load(alternate_levels);
  const auto shifted = sub(residual,set(offset));
  auto bin = shr<10>(shifted);
  if (lambda > 2048) {
    const auto deadzone = set(lambda / 2 - 512);
    const auto positive = maximum(sub(shifted,deadzone),zero);
    const auto negative = minimum(add(shifted,deadzone),set(-1));
    bin = shr<10>(select(gt(zero,shifted),negative,positive));
  }
  const auto sign = minimum(maximum(bin,set(-1)),set(1));
  const auto first = add(sub(shl<10>(bin),mul(sign,set(80))),set(offset));
  const auto narrow = vor(eq(bin,zero),eq(bin,set(-1)));
  const auto second = add(add(first,set(1024)),vand(narrow,set(-80)));
  const auto difference1 = shr<16>(shl<16>(sub(residual,first)));
  const auto difference2 = shr<16>(shl<16>(sub(residual,second)));
  const auto rate = set(static_cast<std::int16_t>(lambda));
  const auto cost1 = add(mul(difference1,difference1),mul(abs(first),rate));
  const auto cost2 = add(mul(difference2,difference2),mul(abs(second),rate));
  if (exact_retry_ties && any(eq(cost1,cost2)))
    return false;
  const auto candidate1 = shr<10>(cost1);
  const auto candidate2 = shr<10>(cost2);
  const auto choose_first = gt(candidate2,candidate1);
  const auto prior = load(alternate_costs);
  store(selected_levels,select(choose_first,first,second));
  store(alternate_levels,select(choose_first,second,first));
  store(selected_costs,add(prior,select(choose_first,candidate1,candidate2)));
  store(alternate_costs,add(prior,select(choose_first,candidate2,candidate1)));
  return true;
}
}

inline int silk_dot_groups(double& result, const float* a, const float* b, int length) noexcept {
  int i = 0;
  for (; i < length - 3; i += 4) {
#if defined(OPUSCPP_SIMD_AVX2)
    const auto products = _mm256_mul_pd(_mm256_cvtps_pd(_mm_loadu_ps(a + i)),
                                       _mm256_cvtps_pd(_mm_loadu_ps(b + i)));
    const auto low = _mm256_castpd256_pd128(products);
    const auto high = _mm256_extractf128_pd(products, 1);
    const auto first = _mm_cvtsd_f64(_mm_add_sd(low, _mm_unpackhi_pd(low, low)));
    const auto third = _mm_cvtsd_f64(high);
    const auto fourth = _mm_cvtsd_f64(_mm_unpackhi_pd(high, high));
#else
    const auto left = vld1q_f32(a + i);
    const auto right = vld1q_f32(b + i);
    const auto low = vmulq_f64(vcvt_f64_f32(vget_low_f32(left)), vcvt_f64_f32(vget_low_f32(right)));
    const auto high = vmulq_f64(vcvt_high_f64_f32(left), vcvt_high_f64_f32(right));
    const auto first = vgetq_lane_f64(low, 0) + vgetq_lane_f64(low, 1);
    const auto third = vgetq_lane_f64(high, 0);
    const auto fourth = vgetq_lane_f64(high, 1);
#endif
    result += (first + third) + fourth;
  }
  return i;
}

#if defined(OPUSCPP_SIMD_AVX2)
inline void pitch_xcorr_partial(const float* x, const float* y, float* out, int length, int lags) noexcept {
  const auto mask = _mm256_cmpgt_epi32(_mm256_set1_epi32(lags), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
  auto sum = _mm256_setzero_ps();
  for (int i = 0; i < length; ++i) {
    const auto product = _mm256_mul_ps(_mm256_broadcast_ss(x + i), _mm256_maskload_ps(y + i, mask));
    sum = _mm256_add_ps(sum, product);
  }
  _mm256_maskstore_ps(out, mask, sum);
}
#endif

inline void pitch_xcorr_eight(const float* x, const float* y, float* out, int length) noexcept {
#if defined(OPUSCPP_SIMD_AVX2)
  auto sum = _mm256_setzero_ps();
  for (int i = 0; i < length; ++i) {
    const auto product = _mm256_mul_ps(_mm256_set1_ps(x[i]), _mm256_loadu_ps(y + i));
    sum = _mm256_add_ps(sum, product);
  }
  _mm256_storeu_ps(out, sum);
#else
  auto first = vdupq_n_f32(0.f), second = first;
  for (int i = 0; i < length; ++i) {
    const auto sample = vdupq_n_f32(x[i]);
    first = vaddq_f32(first, vmulq_f32(sample, vld1q_f32(y + i)));
    second = vaddq_f32(second, vmulq_f32(sample, vld1q_f32(y + i + 4)));
  }
  vst1q_f32(out, first);
  vst1q_f32(out + 4, second);
#endif
}

inline void ltp_prediction_5tap8(const std::int32_t* history, const std::int16_t* coefficients, std::int32_t* prediction) noexcept {
#if defined(OPUSCPP_SIMD_AVX2)
  auto sum = _mm256_set1_epi32(2);
  const auto mask = _mm256_set1_epi32(65535);
  for (int tap = 0; tap < 5; ++tap) {
    const auto state = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(history - tap));
    const auto tap_q14 = _mm256_set1_epi32(coefficients[tap]);
    const auto upper = _mm256_mullo_epi32(_mm256_srai_epi32(state, 16),tap_q14);
    const auto lower = _mm256_srai_epi32(_mm256_mullo_epi32(_mm256_and_si256(state,mask),tap_q14),16);
    sum = _mm256_add_epi32(sum, _mm256_add_epi32(upper,lower));
  }
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(prediction),sum);
#elif defined(OPUSCPP_SIMD_NEON)
  auto lo = vdupq_n_s32(2);
  auto hi = lo;
  for (int tap = 0; tap < 5; ++tap) {
    const auto coefficient = vdupq_n_s32(static_cast<std::int32_t>(coefficients[tap]) * 32768);
    lo = vaddq_s32(lo,vqdmulhq_s32(vld1q_s32(history-tap),coefficient));
    hi = vaddq_s32(hi,vqdmulhq_s32(vld1q_s32(history+4-tap),coefficient));
  }
  vst1q_s32(prediction,lo);
  vst1q_s32(prediction+4,hi);
#endif
}

}
#endif
#endif
