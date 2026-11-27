#ifndef OPUSCPP_INTERNAL_SIMD_H
#define OPUSCPP_INTERNAL_SIMD_H

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#if (defined(__aarch64__) && defined(__ARM_NEON)) || (defined(_M_ARM64) && !defined(_M_ARM64EC))
#  define OPUSCPP_SIMD_NEON 1
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <arm64_neon.h>
#  else
#    include <arm_neon.h>
#  endif
#elif defined(__AVX2__) && (defined(__x86_64__) || defined(_M_X64))
#  define OPUSCPP_SIMD_AVX2 1
#  include <immintrin.h>
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
#if defined(OPUSCPP_SIMD_RESAMPLER)
inline void resampler_up2_hq(std::int32_t* state, std::int16_t* out, const std::int16_t* in, int length,
                              const std::array<std::int16_t, 3>& early,
                              const std::array<std::int16_t, 3>& late) noexcept {
#if defined(OPUSCPP_SIMD_NEON)
  using Pair=int32x2_t;
  const auto pair=[](int a,int b) {
    return vset_lane_s32(b,vdup_n_s32(a),1);
  };
  const auto extend=[](int v) { return vdup_n_s32(v); };
  const auto add=[](Pair a,Pair b){return vadd_s32(a,b);};
  const auto sub=[](Pair a,Pair b){return vsub_s32(a,b);};
  const auto mul=[](Pair a,Pair b){return vshrn_n_s64(vmull_s32(a,b),16);};
  const auto low=[](Pair a){return vget_lane_s32(a,0);};
  const auto high=[](Pair a){return vget_lane_s32(a,1);};
#else
  using Pair=__m128i;
  const auto pair=[](int a,int b) {return _mm_setr_epi32(a,0,b,0);};
  const auto extend=[](int v){return _mm_set1_epi32(v);};
  const auto add=[](Pair a,Pair b){return _mm_add_epi32(a,b);};
  const auto sub=[](Pair a,Pair b){return _mm_sub_epi32(a,b);};
  const auto mul=[](Pair a,Pair b){return _mm_srli_epi64(_mm_mul_epi32(a,b),16);};
  const auto low=[](Pair a){return _mm_cvtsi128_si32(a);};
  const auto high=[](Pair a){return _mm_extract_epi32(a,2);};
#endif
  Pair s0=pair(state[0],state[3]),s1=pair(state[1],state[4]),s2=pair(state[2],state[5]);
  const auto c0=pair(early[0],late[0]);
  const auto c1=pair(early[1],late[1]);
  const auto c2=pair(early[2],late[2]);
  const auto filter=[&]<bool Last>(Pair v,Pair& state,Pair c) {
    const auto delta=sub(v,state);
    auto scaled=mul(delta,c);
    if constexpr(Last) scaled=add(scaled,delta);
    const auto y=add(state,scaled);
    state=add(v,scaled);
    return y;
  };
  for (int i = 0; i < length; ++i) {
    auto v=extend(static_cast<std::int32_t>(in[i]) * 1024);
    v=filter.template operator()<false>(v,s0,c0);
    v=filter.template operator()<false>(v,s1,c1);
    v=filter.template operator()<true>(v,s2,c2);
#if defined(OPUSCPP_SIMD_NEON)
    const auto round = [](std::int32_t sample) noexcept {
      return static_cast<std::int16_t>(std::clamp(((sample >> 9) + 1) >> 1, -32768, 32767));
    };
    out[2 * i] = round(low(v));
    out[2 * i + 1] = round(high(v));
#else
    const auto rounded=_mm_srai_epi32(_mm_add_epi32(_mm_srai_epi32(v,9),_mm_set1_epi32(1)),1);
    const auto packed_lanes=_mm_shuffle_epi32(rounded,_MM_SHUFFLE(2,0,2,0));
    const auto packed=_mm_cvtsi128_si32(_mm_packs_epi32(packed_lanes,packed_lanes));
    std::memcpy(out+2*i,&packed,4);
#endif
  }
  state[0]=low(s0);state[1]=low(s1);state[2]=low(s2);
  state[3]=high(s0);state[4]=high(s1);state[5]=high(s2);
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
#endif

#if defined(OPUSCPP_SIMD_DECODER_LPC)
struct DecoderLpcTaps {
  std::array<std::int16_t,16> older{};
  std::array<std::int16_t,4> newest{};
};

inline DecoderLpcTaps prepare_decoder_lpc(const std::int16_t* coefficients) noexcept {
  DecoderLpcTaps taps;
#if defined(OPUSCPP_SIMD_AVX2)
  // Each 128-bit half of VPACKSSDW interleaves the two input groups.
  constexpr int slots[]{0,1,2,3,8,9,10,11,4,5,6,7};
  for (int i = 0; i < 12; ++i)
    taps.older[slots[i]] = coefficients[15-i];
#else
  for (int i = 0; i < 12; ++i)
    taps.older[i] = coefficients[15-i];
#endif
  for (int i = 0; i < 4; ++i)
    taps.newest[i] = coefficients[i];
  return taps;
}

[[nodiscard]] inline std::int32_t decoder_lpc16(const std::int32_t* end, const DecoderLpcTaps& taps) noexcept {
#if defined(OPUSCPP_SIMD_NEON)
  const auto terms = [](int32x4_t a, int32x4_t b, const std::int16_t* p) {
    const auto h = vcombine_s16(vshrn_n_s32(a,16),vshrn_n_s32(b,16));
    const auto l = vreinterpretq_u16_s16(vcombine_s16(vmovn_s32(a),vmovn_s32(b)));
    return lpc_terms(h,l,vld1q_s16(p));
  };
  const auto older = sum_words(vaddq_s32(terms(vld1q_s32(end-16),vld1q_s32(end-12),taps.older.data()),
                                        terms(vld1q_s32(end-8),vdupq_n_s32(0),taps.older.data()+8)));
#else
  const auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(end-16));
  const auto b = _mm256_zextsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(end-8)));
  const auto h = _mm256_packs_epi32(_mm256_srai_epi32(a,16),_mm256_srai_epi32(b,16));
  const auto l = _mm256_packs_epi32(_mm256_srai_epi32(_mm256_slli_epi32(a,16),16),_mm256_srai_epi32(_mm256_slli_epi32(b,16),16));
  const auto older = sum_words(lpc_terms(h,l,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(taps.older.data()))));
#endif
  // Preserve four independent 32-bit loads at the recurrent end of the history.
  const volatile std::int32_t* newest = end;
  const auto tap = [&](int i) {
    const auto h = static_cast<std::int64_t>(newest[-1-i]);
    return static_cast<std::uint32_t>((h*taps.newest[i]) >> 16);
  };
  return std::bit_cast<std::int32_t>(8u+static_cast<std::uint32_t>(older)+tap(0)+tap(1)+tap(2)+tap(3));
}
#endif
}
#endif
#endif
