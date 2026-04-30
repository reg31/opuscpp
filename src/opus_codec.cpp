#include "opus_codec.h"
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#if defined(OPUS_CODEC_TRACE_RATE)
#include <stdio.h>
#endif
using opus_int8 = std::int8_t;
using opus_uint8 = std::uint8_t;
using opus_int16 = std::int16_t;
using opus_uint16 = std::uint16_t;
using opus_int32 = std::int32_t;
using opus_uint32 = std::uint32_t;
using opus_int64 = std::int64_t;
using opus_uint64 = std::uint64_t;
constexpr auto opus_int32_min = std::numeric_limits<opus_int32>::min();
constexpr auto opus_int32_max = std::numeric_limits<opus_int32>::max();
constexpr auto ec_code_top = static_cast<opus_uint32>(1) << 31;
constexpr auto ec_code_bot = ec_code_top >> 8;
constexpr auto ec_code_mask = ec_code_top - 1;
constexpr auto ec_byte_bits = 8U;
constexpr auto ec_byte_mask = (1U << ec_byte_bits) - 1U;
constexpr auto q7_shift = 7, q7_scale = 1 << q7_shift;
constexpr auto silk_mid_only_score_bias = 0.02f, silk_mid_only_low_speech_bias = 0.01f;
inline constexpr int opus_mode_silk_only = 1000;
inline constexpr int opus_mode_hybrid = 1001;
inline constexpr int opus_mode_celt_only = 1002;
inline constexpr int opus_application_voip = 2048;
inline constexpr int opus_application_audio = 2049;
inline constexpr int opus_application_restricted_lowdelay = 2051;
inline constexpr int opus_application_silk_only = 2052;
inline constexpr int opus_application_celt_only = 2053;
[[nodiscard]] constexpr auto low_bits_mask(const unsigned bits) noexcept -> opus_uint32 {
  return bits == 0 ? 0U : (static_cast<opus_uint32>(1) << bits) - 1U;
}
template <typename T> [[nodiscard]] constexpr auto clamp_value(T value, T low, T high) noexcept -> T {
  return value < low ? low : (high < value ? high : value);
}
[[nodiscard]] constexpr auto score_above(const float value, const float strong, const float ceiling = 1.0f) noexcept -> float {
  return strong >= ceiling ? 0.0f : clamp_value((value - strong) / (ceiling - strong), 0.0f, 1.0f);
}
[[nodiscard]] constexpr auto score_below(const float value, const float soft) noexcept -> float {
  return soft <= 0.0f ? 0.0f : clamp_value((soft - value) / soft, 0.0f, 1.0f);
}
[[nodiscard]] constexpr auto score_centered(const int value, const int center, const int radius) noexcept -> float {
  return radius <= 0 ? 0.0f : clamp_value(1.0f - static_cast<float>(std::abs(value - center)) / static_cast<float>(radius), 0.0f, 1.0f);
}
template <typename First, typename... Rest> [[nodiscard]] constexpr auto min_score(const First first, const Rest... rest) noexcept -> float {
  auto best = static_cast<float>(first);
  ((best = std::min(best, static_cast<float>(rest))), ...);
  return best;
}
[[nodiscard]] constexpr auto parabolic_q7_term(const opus_int32 frac_q7, const opus_int16 coefficient) noexcept -> opus_int32 {
  return frac_q7 + static_cast<opus_int32>((static_cast<opus_int64>(frac_q7 * (q7_scale - frac_q7)) * coefficient) >> 16);
}
[[nodiscard]] constexpr auto bandwidth_to_endband(int bandwidth) noexcept -> int;
template <int Shift> [[nodiscard]] static constexpr auto rounded_rshift(opus_int32 value) noexcept -> opus_int32;
template <int Shift> [[nodiscard]] static constexpr auto rounded_rshift(opus_int64 value) noexcept -> opus_int64;
[[nodiscard]] static constexpr auto rounded_rshift(opus_int64 value, int shift) noexcept -> opus_int64;
[[nodiscard]] static constexpr auto saturate_int16_from_int32(opus_int32 value) noexcept -> opus_int16;
template <int Shift> [[nodiscard]] static inline auto scale_and_saturate_q14(opus_int32 sample_q14, opus_int32 gain) noexcept -> opus_int16;
template <int Shift> [[nodiscard]] static constexpr auto rounded_rshift_to_int16(opus_int32 value) noexcept -> opus_int16;
template <int Shift> [[nodiscard]] static constexpr auto rounded_i16_product_shift(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32;
template <int Shift> [[nodiscard]] static constexpr auto rounded_mul_i16_q16(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32;
template <int Shift> [[nodiscard]] static constexpr auto saturating_left_shift(opus_int32 value) noexcept -> opus_int32;
[[nodiscard]] static constexpr auto saturating_left_shift(opus_int32 value, int shift) noexcept -> opus_int32;
[[nodiscard]] static constexpr auto saturating_add_int32(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32;
[[nodiscard]] static constexpr auto saturating_subtract_int32(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32;
[[nodiscard]] static constexpr auto clamped_midpoint(opus_int32 lhs, opus_int32 rhs, opus_int32 bound0, opus_int32 bound1) noexcept -> opus_int32;
[[nodiscard]] static constexpr auto inverse_prediction_step(opus_int32 lhs, opus_int32 rhs, opus_int32 rc_q31, opus_int32 rc_mult2, int mult2_q) noexcept -> opus_int64;
[[nodiscard]] static constexpr auto delayed_pulse_from_q10(opus_int32 value_q10) noexcept -> opus_int8;
[[nodiscard]] static constexpr auto silk_next_rand_seed(opus_int32 seed) noexcept -> opus_int32;
#if defined(__GNUC__) || defined(__clang__)
#define OPUS_COLD __attribute__((cold))
#define OPUS_NOINLINE __attribute__((noinline))
#define OPUS_ALWAYS_INLINE inline __attribute__((always_inline))
#if !defined(__clang__)
#define OPUS_INT_HOT __attribute__((optimize("O3")))
#define OPUS_GCC_DECODE_FASTPATH 1
#else
#define OPUS_INT_HOT
#define OPUS_GCC_DECODE_FASTPATH 0
#endif
#else
#define OPUS_COLD
#define OPUS_NOINLINE
#define OPUS_ALWAYS_INLINE inline
#define OPUS_INT_HOT
#define OPUS_GCC_DECODE_FASTPATH 0
#endif
#if defined(__clang__)
#define OPUS_SIZE_OPT __attribute__((minsize))
#elif defined(__GNUC__)
#define OPUS_SIZE_OPT __attribute__((optimize("Os")))
#else
#define OPUS_SIZE_OPT
#endif
#if defined(__clang__) && defined(__ANDROID__) && defined(__aarch64__)
#define OPUS_ENCODER_HUB_SIZE_OPT __attribute__((minsize))
#elif defined(__GNUC__) && !defined(__clang__)
#define OPUS_ENCODER_HUB_SIZE_OPT __attribute__((optimize("Os")))
#else
#define OPUS_ENCODER_HUB_SIZE_OPT
#endif
[[nodiscard]] static auto silk_lpc_prediction_q10(const opus_int32 *history_end, std::span<const opus_int16> coefficients) noexcept -> opus_int32;
[[nodiscard]] static OPUS_ALWAYS_INLINE auto silk_ltp_prediction_5tap(const opus_int32 *pred_lag_ptr, std::span<const opus_int16, 5> coefficients) noexcept -> opus_int32;
constexpr void opus_assume(const bool condition) noexcept { static_cast<void>(condition);
}
#if defined(OPUS_CODEC_TRACE_RATE)
#define OPUS_TRACE_RATE(...) fprintf(stderr, __VA_ARGS__)
struct opus_trace_pvq_stats {
  int partitions{}, splits{}, q0{}, k1{}, k2{}, kn{};
};
static thread_local opus_trace_pvq_stats opus_trace_pvq{};
#else
#define OPUS_TRACE_RATE(...) ((void)0)
#endif
template <typename T> static inline void fill_n_items(T *destination, const std::size_t count, const T value) noexcept {
  if constexpr (std::is_integral_v<T> && sizeof(T) == 1) {
    std::memset(destination, static_cast<unsigned char>(value), count);
    return;
  }
  for (std::size_t index = 0; index < count; ++index) destination[index] = value;
}
template <typename T> static inline void zero_n_items(T *destination, const std::size_t count) noexcept {
  if (count == 0) { return;
}
  if constexpr (std::is_integral_v<T> || std::is_enum_v<T> || std::is_floating_point_v<T>) { std::memset(destination, 0, count * sizeof(T)); return;
}
  fill_n_items(destination, count, T{});
}
template <typename T> static inline void copy_n_items(const T *source, const std::size_t count, T *destination) noexcept {
  if (count == 0 || source == destination) {
    return;
  }
  if constexpr (std::is_trivially_copyable_v<T>) {
    std::memcpy(destination, source, count * sizeof(T));
    return;
  }
  for (std::size_t index = 0; index < count; ++index) destination[index] = source[index];
}
template <typename T> [[nodiscard]] constexpr auto optional_span(T *data, const std::size_t size) noexcept -> std::span<T> {
  return data == nullptr ? std::span<T>{} : std::span<T>{data, size};
}
template <typename T> [[nodiscard]] constexpr auto optional_span(const T *data, const std::size_t size) noexcept -> std::span<const T> {
  return data == nullptr ? std::span<const T>{} : std::span<const T>{data, size};
}
template <typename T, std::size_t Rows, std::size_t Columns> [[nodiscard]] constexpr auto flat_matrix_view(const std::array<std::array<T, Columns>, Rows> &matrix) noexcept -> std::span<const T> {
  return {matrix.front().data(), Rows * Columns};
}
template <typename T> struct matrix_ref {
  std::span<T> values;
  int stride;

  [[nodiscard]] constexpr auto operator()(const int row, const int column) const noexcept -> T & {
    return values[static_cast<std::size_t>(row * stride + column)];
  }
};
[[nodiscard]] static constexpr auto silk_pitch_contour_icdf(int fs_kHz, int nb_subfr) noexcept -> std::span<const opus_uint8>;
[[nodiscard]] static constexpr auto silk_pitch_lag_low_bits_icdf(int fs_kHz) noexcept -> std::span<const opus_uint8>;
template <typename T> static inline void move_n_items(const T *source, const std::size_t count, T *destination) noexcept {
  if (count == 0 || source == destination) {
    return;
  }
  if constexpr (std::is_trivially_copyable_v<T>) {
    std::memmove(destination, source, count * sizeof(T));
    return;
  }
  if (destination < source) {
    for (std::size_t index = 0; index < count; ++index) destination[index] = source[index];
    return;
  }
  for (std::size_t index = count; index-- > 0;) destination[index] = source[index];
}
static inline void zero_n_bytes(void *destination, const std::size_t count) noexcept {
  std::memset(destination, 0, count);
}
static inline void copy_n_bytes(const void *source, const std::size_t count, void *destination) noexcept {
  std::memcpy(destination, source, count);
}
static inline void move_n_bytes(const void *source, const std::size_t count, void *destination) noexcept {
  std::memmove(destination, source, count);
}
template <int Order>
[[nodiscard]] static OPUS_ALWAYS_INLINE auto silk_lpc_prediction_q10_fixed(const opus_int32 *history_end, const opus_int16 *coefficients) noexcept -> opus_int32 {
  static_assert(Order == 10 || Order == 16);
  auto tap = [&](const int index) noexcept -> opus_int32 {
    return static_cast<opus_int32>((history_end[-index - 1] * static_cast<opus_int64>(static_cast<opus_int16>(coefficients[index]))) >> 16);
  };
  if constexpr (Order == 16) {
    opus_int32 sum0 = tap(0) + tap(1) + tap(2) + tap(3);
    opus_int32 sum1 = tap(4) + tap(5) + tap(6) + tap(7);
    opus_int32 sum2 = tap(8) + tap(9) + tap(10) + tap(11);
    const opus_int32 sum3 = tap(12) + tap(13) + tap(14) + tap(15);
    return static_cast<opus_int32>(8 + (sum0 + sum1) + (sum2 + sum3));
  } else {
    auto prediction_q10 = static_cast<opus_int32>(Order >> 1);
    for (int tap_index = 0; tap_index < Order; ++tap_index) {
      prediction_q10 = static_cast<opus_int32>(prediction_q10 + ((history_end[-tap_index - 1] * static_cast<opus_int64>(static_cast<opus_int16>(coefficients[tap_index]))) >> 16));
    }
    return prediction_q10;
  }
}
template <int Order>
static OPUS_ALWAYS_INLINE void silk_decode_lpc_subframe_q14(opus_int32 *sLPC_Q14, const opus_int32 *pres_Q14, opus_int16 *pxq, const int length, const opus_int16 *A_Q12, const opus_int32 gain_Q10) noexcept {
  for (int i = 0; i < length; ++i) {
    const auto pred_Q10 = silk_lpc_prediction_q10_fixed<Order>(sLPC_Q14 + 16 + i, A_Q12);
    sLPC_Q14[16 + i] = saturating_add_int32(pres_Q14[i], saturating_left_shift<4>(pred_Q10));
    pxq[i] = scale_and_saturate_q14<8>(sLPC_Q14[16 + i], gain_Q10);
  }
}
[[nodiscard]] static constexpr auto ref_opus_strerror(int error) -> const char *;
struct ref_OpusEncoder;
struct CeltEncoderInternal; struct CeltDecoderInternal; struct CeltModeInternal;
[[nodiscard]] static constexpr auto default_custom_mode() noexcept -> const CeltModeInternal *;
using opus_val16 = float; using opus_val32 = float; using opus_val64 = float; using celt_sig = float; using celt_norm = float;
using celt_ener = float; using celt_glog = float; using opus_res = float; using celt_coef = float;
using ec_window = opus_uint32;
struct ec_ctx {
  unsigned char *buf;
  opus_uint32 storage, end_offs, offs, rng, val, ext;
  ec_window end_window;
  int nend_bits, nbits_total, rem, error;
};
using ec_enc = ec_ctx;
using ec_dec = ec_ctx;
[[nodiscard]] constexpr auto ec_range_bytes(const ec_ctx *state) noexcept -> opus_uint32 { return state->offs;
}
[[nodiscard]] constexpr auto ec_get_buffer(ec_ctx *state) noexcept -> unsigned char * { return state->buf;
}
[[nodiscard]] constexpr auto ec_get_error(const ec_ctx *state) noexcept -> int { return state->error;
}
[[nodiscard]] inline auto ec_tell(const ec_ctx *state) noexcept -> int { return state->nbits_total - (static_cast<int>(sizeof(unsigned) * 8) - __builtin_clz(state->rng));
}
static opus_uint32 ec_tell_frac(const ec_ctx *_this);
[[nodiscard]] constexpr auto celt_udiv(opus_uint32 numerator, opus_uint32 denominator) noexcept -> opus_uint32 {
  opus_assume(denominator > 0);
  return numerator / denominator;
}
[[nodiscard]] constexpr auto celt_sudiv(opus_int32 numerator, opus_int32 denominator) noexcept -> opus_int32 {
  opus_assume(denominator > 0);
  return numerator / denominator;
}
static void ec_enc_init(ec_enc *_this, unsigned char *_buf, opus_uint32 _size), ec_encode(ec_enc *_this, unsigned _fl, unsigned _fh, unsigned _ft), ec_encode_bin(ec_enc *_this, unsigned _fl, unsigned _fh, unsigned _bits), ec_enc_bit_logp(ec_enc *_this, int _val, unsigned _logp),
    ec_enc_icdf(ec_enc *_this, int _s, const unsigned char *_icdf, unsigned _ftb), ec_enc_uint(ec_enc *_this, opus_uint32 _fl, opus_uint32 _ft), ec_enc_bits(ec_enc *_this, opus_uint32 _fl, unsigned _ftb),
    ec_enc_patch_initial_bits(ec_enc *_this, unsigned _val, unsigned _nbits), ec_enc_shrink(ec_enc *_this, opus_uint32 _size), ec_enc_done(ec_enc *_this), ec_dec_init(ec_dec *_this, unsigned char *_buf, opus_uint32 _storage), ec_dec_update(ec_dec *_this, unsigned _fl, unsigned _fh, unsigned _ft);
static unsigned ec_decode(ec_dec *_this, unsigned _ft), ec_decode_bin(ec_dec *_this, unsigned _bits);
static int ec_dec_bit_logp(ec_dec *_this, unsigned _logp), ec_dec_icdf(ec_dec *_this, const unsigned char *_icdf, unsigned _ftb);
static opus_uint32 ec_dec_uint(ec_dec *_this, opus_uint32 _ft), ec_dec_bits(ec_dec *_this, unsigned _ftb);
struct kiss_fft_cpx { float r; float i; };
struct kiss_twiddle_cpx { float r; float i; };
struct kiss_fft_state { int nfft; celt_coef scale; int shift; const kiss_twiddle_cpx *twiddles; const opus_int16 *bitrev; };
struct SILKInfo {
#if defined(OPUS_CODEC_TRACE_RATE)
  int signalType;
#endif
  int offset, bitrateBps, actualSilkBps;
};
[[nodiscard]] constexpr auto bits_to_bitrate(opus_int32 bits, opus_int32 sample_rate, opus_int32 frame_size) noexcept -> opus_int32 { return bits * (6 * sample_rate / frame_size) / 6;
}
[[nodiscard]] constexpr auto bitrate_to_bits(opus_int32 bitrate, opus_int32 sample_rate, opus_int32 frame_size) noexcept -> opus_int32 { return bitrate * 6 / (6 * sample_rate / frame_size);
}
static int celt_encoder_get_size(int channels), celt_encoder_init(CeltEncoderInternal *st, opus_int32 sampling_rate, int channels);
static OPUS_ENCODER_HUB_SIZE_OPT int celt_encode_with_ec(CeltEncoderInternal *st, const opus_res *pcm, int frame_size, unsigned char *compressed, int nbCompressedBytes, ec_enc *enc);
static void celt_encoder_reset_state(CeltEncoderInternal *st), celt_encoder_set_vbr(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_constrained_vbr(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_silk_info(CeltEncoderInternal *st, const SILKInfo *info);
static int celt_encoder_set_complexity(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_start_band(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_end_band(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_stream_channels(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_lsb_depth(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_prediction(CeltEncoderInternal *st, opus_int32 value), celt_encoder_set_bitrate(CeltEncoderInternal *st, opus_int32 value);
[[nodiscard]] static opus_uint32 celt_encoder_final_range(const CeltEncoderInternal *st) noexcept;
[[nodiscard]] static const CeltModeInternal *celt_encoder_mode(const CeltEncoderInternal *st) noexcept;
static int celt_decoder_get_size(int channels), celt_decoder_init(CeltDecoderInternal *st, opus_int32 sampling_rate, int channels);
static int celt_decode_with_ec(CeltDecoderInternal *st, const unsigned char *data, int len, opus_res *pcm, int frame_size, ec_dec *dec, int accum
#if OPUS_GCC_DECODE_FASTPATH
                               , opus_int16 *pcm16 = nullptr
#endif
);
static void celt_decoder_reset_state(CeltDecoderInternal *st);
static int celt_decoder_set_start_band(CeltDecoderInternal *st, opus_int32 value), celt_decoder_set_end_band(CeltDecoderInternal *st, opus_int32 value), celt_decoder_set_stream_channels(CeltDecoderInternal *st, opus_int32 value);
[[nodiscard]] static opus_uint32 celt_decoder_final_range(const CeltDecoderInternal *st) noexcept;
[[nodiscard]] static const CeltModeInternal *celt_decoder_mode(const CeltDecoderInternal *st) noexcept;
template <std::size_t Size> consteval bool icdf_is_descending(const std::array<unsigned char, Size> &table) {
  for (const auto index : std::views::iota(std::size_t{1}, Size)) {
    if (table[index - 1] < table[index]) return false;
  }
  return true;
}
template <typename T, std::size_t Size> consteval bool array_is_strictly_increasing(const std::array<T, Size> &table) {
  for (const auto index : std::views::iota(std::size_t{1}, Size)) {
    if (!(table[index - 1] < table[index])) { return false; }
}
  return true;
}
template <typename T, std::size_t Size> consteval bool array_is_non_decreasing(const std::array<T, Size> &table) {
  for (const auto index : std::views::iota(std::size_t{1}, Size)) {
    if (table[index] < table[index - 1]) { return false; }
}
  return true;
}
template <typename T, std::size_t Rows, std::size_t Columns> consteval bool every_row_is_descending(const std::array<std::array<T, Columns>, Rows> &table) {
  for (const auto &row : table) {
    for (const auto index : std::views::iota(std::size_t{1}, Columns)) {
      if (row[index - 1] < row[index]) return false;
    }
}
  return true;
}
consteval void numeric_blob_fail() {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_trap();
#else
  throw "numeric blob parse failure";
#endif
}
consteval auto numeric_blob_is_whitespace(char ch) noexcept -> bool { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}
consteval auto numeric_blob_is_hex(char ch) noexcept -> bool { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}
consteval auto numeric_blob_hex_value(char ch) noexcept -> unsigned {
  if (ch >= '0' && ch <= '9') { return static_cast<unsigned>(ch - '0');
}
  if (ch >= 'a' && ch <= 'f') { return static_cast<unsigned>(10 + ch - 'a');
}
  if (ch >= 'A' && ch <= 'F') { return static_cast<unsigned>(10 + ch - 'A');
}
  numeric_blob_fail();
  return 0;
}
template <typename T> struct numeric_blob_storage {
  using type = std::conditional_t<std::is_floating_point_v<T>, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>,
                                  std::conditional_t<sizeof(T) == 1, std::uint8_t, std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>>;
};
template <typename T> using numeric_blob_storage_t = typename numeric_blob_storage<T>::type;
template <typename T> consteval auto numeric_blob_value_from_bits(numeric_blob_storage_t<T> bits) noexcept -> T {
  if constexpr (std::is_floating_point_v<T>) {
    return std::bit_cast<T>(bits);
  } else if constexpr (std::is_unsigned_v<T>) {
    return static_cast<T>(bits);
  } else {
    return std::bit_cast<T>(bits);
  }
}
template <typename T, std::size_t Count> consteval auto numeric_blob_array(std::string_view blob) -> std::array<T, Count> {
  using storage_t = numeric_blob_storage_t<T>;
  constexpr auto hex_digits_per_value = sizeof(T) * 2;
  std::array<T, Count> values{};
  auto position = std::size_t{0};
  for (const auto index : std::views::iota(std::size_t{0}, Count)) {
    for (; position < blob.size() && numeric_blob_is_whitespace(blob[position]); ++position) {}
    auto bits = storage_t{};
    for ([[maybe_unused]] const auto digit : std::views::iota(std::size_t{0}, hex_digits_per_value)) {
      if (position >= blob.size() || !numeric_blob_is_hex(blob[position])) {
        numeric_blob_fail();
      }
      bits = static_cast<storage_t>((bits << 4U) | numeric_blob_hex_value(blob[position++]));
    }
    values[index] = numeric_blob_value_from_bits<T>(bits);
  }
  for (; position < blob.size() && numeric_blob_is_whitespace(blob[position]); ++position) {}
  if (position != blob.size()) {
    numeric_blob_fail();
  }
  return values;
}
template <typename T, std::size_t Rows, std::size_t Columns> consteval auto numeric_blob_matrix(std::string_view blob) -> std::array<std::array<T, Columns>, Rows> {
  const auto flat = numeric_blob_array<T, Rows * Columns>(blob);
  std::array<std::array<T, Columns>, Rows> values{};
  auto index = std::size_t{0};
  for (const auto row : std::views::iota(std::size_t{0}, Rows)) {
    for (const auto column : std::views::iota(std::size_t{0}, Columns)) {
      values[row][column] = flat[index++];
    }
  }
  return values;
}
template <typename T, std::size_t Depth0, std::size_t Depth1, std::size_t Depth2> consteval auto numeric_blob_tensor(std::string_view blob) -> std::array<std::array<std::array<T, Depth2>, Depth1>, Depth0> {
  const auto flat = numeric_blob_array<T, Depth0 * Depth1 * Depth2>(blob);
  std::array<std::array<std::array<T, Depth2>, Depth1>, Depth0> values{};
  auto index = std::size_t{0};
  for (const auto outer : std::views::iota(std::size_t{0}, Depth0)) {
    for (const auto middle : std::views::iota(std::size_t{0}, Depth1)) {
      for (const auto inner : std::views::iota(std::size_t{0}, Depth2)) {
        values[outer][middle][inner] = flat[index++];
      }
    }
  }
  return values;
}
template <typename Pair, typename Scalar, std::size_t Count> consteval auto numeric_blob_pair_array(std::string_view blob) -> std::array<Pair, Count> {
  const auto flat = numeric_blob_matrix<Scalar, Count, 2>(blob);
  std::array<Pair, Count> values{};
  for (const auto index : std::views::iota(std::size_t{0}, Count)) {
    values[index] = Pair{flat[index][0], flat[index][1]};
  }
  return values;
}
constexpr std::array<unsigned char, 11> trim_icdf = numeric_blob_array<unsigned char, 11>(R"blob(7E7C776D57291309040200)blob");
constexpr std::array<unsigned char, 4> spread_icdf = numeric_blob_array<unsigned char, 4>(R"blob(19170200)blob");
constexpr std::array<unsigned char, 3> shared_three_step_icdf = numeric_blob_array<unsigned char, 3>(R"blob(020100)blob");
consteval auto make_bit_interleave_table() -> std::array<unsigned char, 16> {
  std::array<unsigned char, 16> table{};
  for (auto value = 0U; value < table.size(); ++value) {
    const auto lower_pair = (value & 0x3U) != 0U ? 1U : 0U;
    const auto upper_pair = (value & 0xCU) != 0U ? 2U : 0U;
    table[value] = static_cast<unsigned char>(lower_pair | upper_pair);
}
  return table;
}
consteval auto make_bit_deinterleave_table() -> std::array<unsigned char, 16> {
  std::array<unsigned char, 16> table{};
  for (auto value = 0U; value < table.size(); ++value) {
    auto expanded = 0U;
    if ((value & 0x1U) != 0U) { expanded |= 0x03U;
}
    if ((value & 0x2U) != 0U) { expanded |= 0x0CU;
}
    if ((value & 0x4U) != 0U) { expanded |= 0x30U;
}
    if ((value & 0x8U) != 0U) { expanded |= 0xC0U;
}
    table[value] = static_cast<unsigned char>(expanded);
}
  return table;
}
struct hybrid_rate_entry { int threshold; std::array<int, 4> rates; };
struct stereo_intensity_tables { std::array<opus_val16, 21> thresholds; std::array<opus_val16, 21> hysteresis; };
constexpr std::array<unsigned char, 16> bit_interleave_table = make_bit_interleave_table();
constexpr std::array<unsigned char, 16> bit_deinterleave_table = make_bit_deinterleave_table();
constexpr std::array<hybrid_rate_entry, 7> hybrid_rate_table{{{0, {0, 0, 0, 0}}, {12000, {10000, 10000, 11000, 11000}}, {16000, {13500, 13500, 15000, 15000}}, {20000, {16000, 16000, 18000, 18000}}, {24000, {18000, 18000, 21000, 21000}}, {32000, {22000, 22000, 28000, 28000}}, {40000, {38000, 38000, 50000, 50000}}}};
constexpr std::array<std::array<opus_val16, 3>, 3> comb_filter_tapset_gains{{{{0.3066406250f, 0.2170410156f, 0.1296386719f}}, {{0.4638671875f, 0.2680664062f, 0.0f}}, {{0.7998046875f, 0.1000976562f, 0.0f}}}};
constexpr stereo_intensity_tables stereo_intensity_table{{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 16.0f, 24.0f, 36.0f, 44.0f, 50.0f, 56.0f, 62.0f, 67.0f, 72.0f, 79.0f, 88.0f, 106.0f, 134.0f},
                                                {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 3.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 8.0f}};
constexpr std::array<opus_val16, 16> celt_anti_collapse_thresh_by_depth = numeric_blob_array<opus_val16, 16>(R"blob(3F0000003EEAC0C73ED744FD3EC5672A3EB504F33EA5FED73E9837F03E8B95C23E8000003E6AC0C73E5744FD3E45672A3E3504F33E25FED73E1837F03E0B95C2)blob");
static_assert(icdf_is_descending(trim_icdf));
static_assert(icdf_is_descending(spread_icdf));
static_assert(icdf_is_descending(shared_three_step_icdf));
static_assert(bit_interleave_table[0xA] == 0x3);
static_assert(bit_deinterleave_table[0x5] == 0x33);
static int resampling_factor(opus_int32 rate);
static void celt_preemphasis(const opus_res *pcmp, celt_sig *inp, int N, int CC, int upsample, const opus_val16 *coef, celt_sig *mem, int clip), comb_filter(opus_val32 *y, opus_val32 *x, int T0, int T1, int N, opus_val16 g0, opus_val16 g1, int tapset0, int tapset1, const celt_coef *window, int overlap), init_caps(const CeltModeInternal *m, std::span<int> cap, int LM, int C);
struct mdct_lookup { int n; const kiss_fft_state *kfft[4]; const float *trig; };
static void clt_mdct_forward_c(const mdct_lookup *l, float *in, float *out, const celt_coef *window, int overlap, int shift, int stride), clt_mdct_backward_c(const mdct_lookup *l, float *in, float *out, const celt_coef *window, int overlap, int shift, int stride),
    clt_mdct_backward_dual_history_c(const mdct_lookup *l, float *in, float *out0, float *out1, const celt_coef *window, int overlap, int shift, int stride);
struct PulseCache { const opus_int16 *index; const unsigned char *bits, *caps; };
struct CeltModeInternal {
  opus_int32 Fs; int overlap, nbEBands, effEBands;
  opus_val16 preemph[4];
  const opus_int16 *eBands;
  int maxLM, shortMdctSize, nbAllocVectors;
  const unsigned char *allocVectors;
  const opus_int16 *logN;
  const celt_coef *window;
  mdct_lookup mdct; PulseCache cache;
};
[[nodiscard]] static constexpr auto fft_bitrev_index(const kiss_fft_state *state, int index) noexcept -> int {
  return state->bitrev[index];
}
[[nodiscard]] consteval auto make_mdct_lookup(const int n, const std::array<const kiss_fft_state *, 4> &kfft, const float *trig) -> mdct_lookup {
  mdct_lookup lookup{n, {}, trig};
  for (auto index = std::size_t{}; index < kfft.size(); ++index) { lookup.kfft[index] = kfft[index];
}
  return lookup;
}
static int celt_decoder_init_state(CeltDecoderInternal *st, const CeltModeInternal *mode, int channels);
struct ref_OpusEncoder;
[[nodiscard]] static int ref_opus_encoder_get_size(int channels);
static int ref_opus_encoder_init(ref_OpusEncoder *st, opus_int32 Fs, int channels, int application);
[[nodiscard]] static opus_int32 ref_opus_encode(ref_OpusEncoder *st, const opus_int16 *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes);
[[nodiscard]] static opus_int32 ref_opus_encode_float(ref_OpusEncoder *st, const float *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes);
struct ref_OpusDecoder;
[[nodiscard]] static int ref_opus_decoder_get_size(int channels);
static int ref_opus_decoder_init(ref_OpusDecoder *st, opus_int32 Fs, int channels);
[[nodiscard]] static int ref_opus_decode(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec);
[[nodiscard]] static int ref_opus_decode_float(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, float *pcm, int frame_size, int decode_fec);
[[nodiscard]] static constexpr int ref_opus_packet_get_bandwidth(const unsigned char *data);
[[nodiscard]] static OPUS_NOINLINE int ref_opus_packet_get_samples_per_frame(const unsigned char *data, opus_int32 Fs);
[[nodiscard]] static constexpr int ref_opus_packet_get_nb_channels(const unsigned char *data);
[[nodiscard]] static constexpr int ref_opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len);
[[nodiscard]] static OPUS_NOINLINE int ref_opus_packet_get_nb_samples(const unsigned char packet[], opus_int32 len, opus_int32 Fs);
[[nodiscard]] static constexpr int ref_opus_decoder_get_nb_samples(const ref_OpusDecoder *dec, const unsigned char packet[], opus_int32 len);
[[nodiscard]] inline auto opus_alloc(std::size_t size) noexcept -> void * { return std::malloc(size);
}
inline auto opus_free(void *ptr) noexcept -> void { std::free(ptr);
}
// Stack scratch buffer. Lives until the enclosing function returns. No heap alloc, no zero-init, no destructor.
// Only use for trivially-destructible types, and only where the buffer is fully written before any read.
#define OPUS_SCRATCH(T, N) static_cast<T *>(__builtin_alloca(static_cast<std::size_t>(N) * sizeof(T)))
template <std::size_t ViewCount, typename T> [[nodiscard]] inline auto partition_workset(std::span<T> storage, const std::size_t view_size) noexcept -> std::array<std::span<T>, ViewCount> {
  opus_assume(storage.size() >= ViewCount * view_size);
  auto remaining = storage.first(ViewCount * view_size); std::array<std::span<T>, ViewCount> views{};
  for (auto &view : views) { view = remaining.first(view_size); remaining = remaining.subspan(view_size);
}
  return views;
}
struct opus_free_deleter { inline void operator()(void *ptr) const noexcept { opus_free(ptr); } };
template <typename T> using opus_owned_ptr = std::unique_ptr<T, opus_free_deleter>;
template <typename T> [[nodiscard]] inline auto make_opus_owned(const std::size_t size) noexcept -> opus_owned_ptr<T> { return opus_owned_ptr<T>(reinterpret_cast<T *>(opus_alloc(size)));
}
static unsigned isqrt32(opus_uint32 _val);
[[nodiscard]] static inline auto celt_maxabs16(const opus_val16 *x, int len) noexcept -> opus_val32 {
  if (len <= 0) { return 0;
}
  auto mn = x[0], mx = x[0];
  for (int i = 1; i < len; ++i) { mn = std::min(mn, x[i]); mx = std::max(mx, x[i]);
}
  return mx > -mn ? mx : -mn;
}
[[nodiscard]] static constexpr auto celt_atan_norm(float x) noexcept -> float {
  const float x_sq = x * x;
  return 0.636619772367581f * (x + x * x_sq * (-3.3331659436225891113281250000e-01f + x_sq * (1.99627041816711425781250000000e-01f + x_sq * (-1.3976582884788513183593750000e-01f + x_sq * (9.79423448443412780761718750000e-02f + x_sq * (-5.7773590087890625000000000000e-02f + x_sq * (2.30401363223791122436523437500e-02f + x_sq * (-4.3554059229791164398193359375e-03f))))))));
}
[[nodiscard]] constexpr auto celt_atan2p_norm(float y, float x) noexcept -> float {
  opus_assume(x >= 0 && y >= 0);
  if (x * x + y * y < 1e-18f) { return 0.0f;
}
  return y < x ? celt_atan_norm(y / x) : 1.0f - celt_atan_norm(x / y);
}
static void celt_float2int16_c(std::span<const float> in, std::span<short> out);
struct packet_frame_set {
  unsigned char toc;
  int nb_frames, framesize;
  std::array<const unsigned char *, 48> frames{};
  std::array<opus_int16, 48> len{};
};
static OPUS_ENCODER_HUB_SIZE_OPT opus_int32 encode_native(ref_OpusEncoder *st, const opus_res *pcm, int frame_size, unsigned char *data, opus_int32 out_data_bytes, int lsb_depth, int float_api);
static int decode_native(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_res *pcm, int frame_size, int decode_fec, int self_delimited, opus_int32 *packet_offset);
[[nodiscard]] constexpr auto align(int value) noexcept -> int {
  union aligned_storage {
    void *pointer;
    opus_int32 integer; opus_val32 scalar;
  }; constexpr auto alignment = static_cast<int>(alignof(aligned_storage));
  return ((value + alignment - 1) / alignment) * alignment;
}
static OPUS_COLD OPUS_NOINLINE int ref_opus_packet_parse_impl(const unsigned char *data, opus_int32 len, int self_delimited, unsigned char *out_toc, const unsigned char *frames[48], opus_int16 size[48], int *payload_offset, opus_int32 *packet_offset, const unsigned char **padding, opus_int32 *padding_len);
static OPUS_COLD OPUS_NOINLINE opus_int32 write_packet_frames(packet_frame_set *frames, int begin, int end, unsigned char *data, opus_int32 maxlen, int self_delimited, int pad);
static OPUS_COLD OPUS_NOINLINE int append_packet_frames(packet_frame_set *frames, const unsigned char *data, opus_int32 len, int self_delimited);
static OPUS_COLD OPUS_NOINLINE int pad_packet(unsigned char *data, opus_int32 len, opus_int32 new_len, int pad);
static int encode_size(int size, unsigned char *data) {
  if (size < 252) { data[0] = size; return 1;
}
  data[0] = 252 + (size & 0x3); data[1] = (size - (int)data[0]) >> 2;
  return 2;
}
static int parse_size(const unsigned char *data, opus_int32 len, opus_int16 *size) {
  if (len < 1) { *size = -1; return -1;
}
  if (data[0] < 252) { *size = data[0]; return 1;
}
  if (len < 2) { *size = -1; return -1;
}
  *size = 4 * data[1] + data[0];
  return 2;
}
static OPUS_NOINLINE int ref_opus_packet_get_samples_per_frame(const unsigned char *data, opus_int32 Fs) {
  int audiosize;
  if (data[0] & 0x80) {
    audiosize = ((data[0] >> 3) & 0x3); audiosize = (Fs << audiosize) / 400;
  } else if ((data[0] & 0x60) == 0x60) {
    audiosize = (data[0] & 0x08) ? Fs / 50 : Fs / 100;
  } else {
    audiosize = ((data[0] >> 3) & 0x3);
    if (audiosize == 3) audiosize = Fs * 60 / 1000;
    else audiosize = (Fs << audiosize) / 100;
}
  return audiosize;
}
static OPUS_COLD OPUS_NOINLINE int ref_opus_packet_parse_impl(const unsigned char *data, opus_int32 len, int self_delimited, unsigned char *out_toc, const unsigned char *frames[48], opus_int16 size[48], int *payload_offset, opus_int32 *packet_offset, const unsigned char **padding, opus_int32 *padding_len) {
  int i, bytes, count, cbr;
  unsigned char ch, toc;
  int framesize;
  opus_int32 last_size, pad = 0;
  const unsigned char *data0 = data;
  if (padding != nullptr) { *padding = nullptr; *padding_len = 0;
}
  if (size == nullptr || len < 0) return -1;
  auto sizes = std::span<opus_int16>{size, std::size_t{48}};
  auto frame_refs = optional_span(frames, sizes.size());
  if (len == 0) return -4;
  framesize = ref_opus_packet_get_samples_per_frame(data, 48000); cbr = 0; toc = *data++; len--; last_size = len;
  switch (toc & 0x3) {
  case 0: count = 1; break;
  case 1:
    count = 2; cbr = 1;
    if (!self_delimited) {
      if (len & 0x1) return -4;
      last_size = len / 2; sizes[0] = (opus_int16)last_size;
}
    break;
  case 2:
    count = 2; bytes = parse_size(data, len, sizes.data()); len -= bytes;
    if (sizes[0] < 0 || sizes[0] > len) return -4;
    data += bytes; last_size = len - sizes[0];
    break;
  default:
    if (len < 1) return -4;
    ch = *data++; count = ch & 0x3F;
    if (count <= 0 || framesize * (opus_int32)count > 5760) return -4;
    len--;
    if (ch & 0x40) {
      for (int p = 255; p == 255;) {
        if (len <= 0) return -4;
        p = *data++; len--;
        const auto tmp = p == 255 ? 254 : p;
        len -= tmp; pad += tmp; }
}
    if (len < 0) return -4;
    cbr = !(ch & 0x80);
    if (!cbr) {
      last_size = len;
      for (i = 0; i < count - 1; i++) {
        bytes = parse_size(data, len, sizes.data() + i); len -= bytes;
        if (sizes[i] < 0 || sizes[i] > len) return -4;
        data += bytes; last_size -= bytes + sizes[i];
}
      if (last_size < 0) return -4;
    } else if (!self_delimited) {
      last_size = len / count;
      if (last_size * count != len) return -4;
      fill_n_items(sizes.data(), static_cast<std::size_t>(count - 1), static_cast<opus_int16>(last_size));
}
    break;
}
  if (self_delimited) {
    bytes = parse_size(data, len, sizes.data() + count - 1); len -= bytes;
    if (sizes[count - 1] < 0 || sizes[count - 1] > len) return -4;
    data += bytes;
    if (cbr) {
      if (sizes[count - 1] * count > len) return -4;
      fill_n_items(sizes.data(), static_cast<std::size_t>(count - 1), sizes[count - 1]);
    } else if (bytes + sizes[count - 1] > last_size) return -4;
  } else {
    if (last_size > 1275) return -4;
    sizes[count - 1] = (opus_int16)last_size;
}
  if (payload_offset) *payload_offset = (int)(data - data0);
  for (i = 0; i < count; i++) {
    if (!frame_refs.empty()) frame_refs[i] = data;
    data += sizes[i];
}
  if (padding != nullptr) { *padding = data; *padding_len = pad;
}
  if (packet_offset) *packet_offset = pad + (opus_int32)(data - data0);
  if (out_toc) *out_toc = toc;
  return count;
}
struct silk_EncControlStruct {
  opus_int32 nChannelsAPI, nChannelsInternal, API_sampleRate, maxInternalSampleRate, minInternalSampleRate, desiredInternalSampleRate, bitRate, internalSampleRate;
  int payloadSize_ms, complexity, useCBR, maxBits, toMono, opusCanSwitch, allowBandwidthSwitch, inWBmodeWithoutVariableLP, stereoWidth_Q14, switchReady, signalType, offset;
};
struct silk_DecControlStruct { opus_int32 nChannelsInternal, nChannelsAPI, internalSampleRate, API_sampleRate; int payloadSize_ms, prevPitchLag; };
static int silk_Get_Encoder_Size(int *encSizeBytes, int channels), silk_InitEncoder(void *encState, int channels, silk_EncControlStruct *encStatus),
    silk_Get_Decoder_Size(int *decSizeBytes), silk_ResetDecoder(void *decState), silk_InitDecoder(void *decState), silk_Decode(void *decState, silk_DecControlStruct *decControl, int lostFlag, int newPacketFlag, ec_dec *psRangeDec, opus_res *samplesOut, opus_int32 *nSamplesOut);
OPUS_ENCODER_HUB_SIZE_OPT int silk_Encode(void *encState, silk_EncControlStruct *encControl, const opus_res *samplesIn, int nSamplesIn, ec_enc *psRangeEnc, opus_int32 *nBytesOut, const int prefillFlag, int activity);
static void silk_destroy_decoder(void *decState) noexcept;
[[nodiscard]] inline auto float2int(float x) noexcept -> opus_int32 { return static_cast<opus_int32>(lrint(x));
}
[[nodiscard]] static inline auto FLOAT2INT16(float x) noexcept -> opus_int16 { return static_cast<opus_int16>(float2int(clamp_value(x * 32768.f, -32768.f, 32767.f)));
}
struct silk_resampler_state_struct {
  opus_int32 sIIR[6], invRatio_Q16; union { opus_int32 i32[36]; opus_int16 i16[36]; } sFIR; opus_int16 delayBuf[96];
  int resampler_function, batchSize, FIR_Order, FIR_Fracs, Fs_in_kHz, Fs_out_kHz, inputDelay;
  const opus_int16 *Coefs;
};
[[nodiscard]] static auto silk_CLZ32(opus_int32 value) noexcept -> opus_int32 { return value ? 32 - (static_cast<int>(sizeof(unsigned) * 8) - __builtin_clz(value)) : 32;
}
static int silk_resampler_init(silk_resampler_state_struct *S, opus_int32 Fs_Hz_in, opus_int32 Fs_Hz_out, int forEnc); static int silk_resampler(silk_resampler_state_struct *S, opus_int16 out[], const opus_int16 in[], opus_int32 inLen);
static void silk_resampler_down2(opus_int32 *S, opus_int16 *out, const opus_int16 *in, opus_int32 inLen); static void silk_resampler_down2_3(opus_int32 *S, opus_int16 *out, const opus_int16 *in, opus_int32 inLen);
static void silk_biquad_alt_stride1(const opus_int16 *in, const opus_int32 *B_Q28, const opus_int32 *A_Q28, opus_int32 *S, opus_int16 *out, const opus_int32 len);
static void silk_LPC_analysis_filter(opus_int16 *out, const opus_int16 *in, const opus_int16 *B, const opus_int32 len, const opus_int32 d); static void silk_bwexpander(std::span<opus_int16> ar, opus_int32 chirp_Q16); static void silk_bwexpander_32(std::span<opus_int32> ar, opus_int32 chirp_Q16);
static opus_int32 silk_LPC_inverse_pred_gain_c(const opus_int16 *A_Q12, const int order); static void silk_ana_filt_bank_1(const opus_int16 *in, opus_int32 *S, opus_int16 *outL, opus_int16 *outH, const opus_int32 N); static opus_int32 silk_lin2log(const opus_int32 inLin); static int silk_sigm_Q15(int in_Q5); static opus_int32 silk_log2lin(const opus_int32 inLog_Q7);
static void silk_sum_sqr_shift(opus_int32 *energy, int *shift, const opus_int16 *x, int len);
static void silk_decode_pitch(opus_int16 lagIndex, opus_int8 contourIndex, int pitch_lags[], const int Fs_kHz, const int nb_subfr);
static void silk_A2NLSF(opus_int16 *NLSF, opus_int32 *a_Q16, const int d); static void silk_NLSF2A(opus_int16 *a_Q12, const opus_int16 *NLSF, const int d); static void silk_LPC_fit(opus_int16 *a_QOUT, opus_int32 *a_QIN, const int QOUT, const int QIN, const int d);
static void silk_insertion_sort_increasing(opus_int32 *a, int *idx, const int L, const int K); static void silk_insertion_sort_increasing_all_values_int16(opus_int16 *a, const int L); static void silk_NLSF_stabilize(opus_int16 *NLSF_Q15, const opus_int16 *NDeltaMin_Q15, const int L); static void silk_NLSF_VQ_weights_laroia(opus_int16 *pNLSFW_Q_OUT, const opus_int16 *pNLSF_Q15, const int D);
static opus_int32 silk_inner_prod_aligned_scale(std::span<const opus_int16> inVec1, std::span<const opus_int16> inVec2, int scale);
[[nodiscard]] constexpr auto silk_ROR32(opus_int32 value, int rotation) noexcept -> opus_int32 {
  const auto bits = static_cast<opus_uint32>(value);
  const auto positive_rotation = static_cast<opus_uint32>(rotation);
  const auto negative_rotation = static_cast<opus_uint32>(-rotation);
  if (rotation == 0) { return value;
}
  if (rotation < 0) { return static_cast<opus_int32>((bits << negative_rotation) | (bits >> (32 - negative_rotation)));
}
  return static_cast<opus_int32>((bits << (32 - positive_rotation)) | (bits >> positive_rotation));
}
static auto silk_CLZ_FRAC(opus_int32 value, opus_int32 *lz, opus_int32 *frac_Q7) noexcept -> void {
  const opus_int32 leading_zeros = silk_CLZ32(value);
  *lz = leading_zeros; *frac_Q7 = silk_ROR32(value, 24 - leading_zeros) & 0x7f;
}
[[nodiscard]] static auto silk_SQRT_APPROX(opus_int32 x) noexcept -> opus_int32 {
  if (x <= 0) { return 0;
}
  opus_int32 lz = 0, frac_Q7 = 0; silk_CLZ_FRAC(x, &lz, &frac_Q7);
  opus_int32 y = (lz & 1) ? 32768 : 46214; y >>= (lz >> 1);
  y = static_cast<opus_int32>(y + ((y * static_cast<opus_int64>(static_cast<opus_int16>(static_cast<opus_int32>(static_cast<opus_int16>(213) * static_cast<opus_int32>(static_cast<opus_int16>(frac_Q7)))))) >> 16));
  return y;
}
[[nodiscard]] static auto silk_DIV32_varQ(const opus_int32 a32, const opus_int32 b32, const int Qres) noexcept -> opus_int32 {
  int a_headrm, b_headrm, lshift;
  opus_int32 b32_inv, a32_nrm, b32_nrm, result; opus_assume(b32 != 0);
  opus_assume(Qres >= 0); a_headrm = silk_CLZ32((((a32) > 0) ? (a32) : -(a32))) - 1;
  a32_nrm = ((opus_int32)((opus_uint32)(a32) << (a_headrm))); b_headrm = silk_CLZ32((((b32) > 0) ? (b32) : -(b32))) - 1;
  b32_nrm = ((opus_int32)((opus_uint32)(b32) << (b_headrm))); b32_inv = ((opus_int32)((0x7FFFFFFF >> 2) / (((b32_nrm) >> (16)))));
  result = ((opus_int32)(((a32_nrm) * (opus_int64)((opus_int16)(b32_inv))) >> 16));
  a32_nrm = ((opus_int32)((opus_uint32)(a32_nrm) - (opus_uint32)(((opus_int32)((opus_uint32)((opus_int32)((((opus_int64)((b32_nrm)) * ((result)))) >> (32))) << (3))))));
  result = ((opus_int32)((result) + (((a32_nrm) * (opus_int64)((opus_int16)(b32_inv))) >> 16))); lshift = 29 + a_headrm - b_headrm - Qres;
  if (lshift < 0) { return saturating_left_shift(result, -lshift);
}
  else if (lshift < 32) { return ((result) >> (lshift));
}
  else { return 0; }
}
[[nodiscard]] static auto silk_INVERSE32_varQ(const opus_int32 b32, const int Qres) noexcept -> opus_int32 {
  int b_headrm, lshift;
  opus_int32 b32_inv, b32_nrm, err_Q32, result; opus_assume(b32 != 0);
  opus_assume(Qres > 0); b_headrm = silk_CLZ32((((b32) > 0) ? (b32) : -(b32))) - 1;
  b32_nrm = ((opus_int32)((opus_uint32)(b32) << (b_headrm))); b32_inv = ((opus_int32)((0x7FFFFFFF >> 2) / (((b32_nrm) >> (16)))));
  result = ((opus_int32)((opus_uint32)(b32_inv) << (16)));
  err_Q32 = ((opus_int32)((opus_uint32)(((opus_int32)1 << 29) - ((opus_int32)(((b32_nrm) * (opus_int64)((opus_int16)(b32_inv))) >> 16))) << (3)));
  result = ((opus_int32)((result) + (((opus_int64)(err_Q32) * (b32_inv)) >> 16))); lshift = 61 - b_headrm - Qres;
  if (lshift <= 0) { return saturating_left_shift(result, -lshift);
}
  else if (lshift < 32) { return ((result) >> (lshift));
}
  else { return 0; }
}
struct silk_nsq_state {
  opus_int16 xq[2 * ((5 * 4) * 16)]; opus_int32 sLTP_shp_Q14[2 * ((5 * 4) * 16)], sLPC_Q14[(5 * 16) + 16], sAR2_Q14[24], sLF_AR_shp_Q14, sDiff_shp_Q14;
  int lagPrev, sLTP_buf_idx, sLTP_shp_buf_idx; opus_int32 rand_seed, prev_gain_Q16; int rewhite_flag;
};
struct silk_VAD_state {
  opus_int32 AnaState[2], AnaState1[2], AnaState2[2], XnrgSubfr[4], NrgRatioSmth_Q8[4]; opus_int16 HPstate;
  opus_int32 NL[4], inv_NL[4], NoiseLevelBias[4], counter;
};
struct silk_LP_state { opus_int32 In_LP_State[2], transition_frame_no; int mode; opus_int32 saved_fs_kHz; };
struct silk_NLSF_CB_struct {
  const opus_int16 nVectors, order, quantStepSize_Q16, invQuantStepSize_Q6;
  const opus_uint8 *CB1_NLSF_Q8;
  const opus_int16 *CB1_Wght_Q9;
  const opus_uint8 *CB1_iCDF, *pred_Q8, *ec_sel, *ec_iCDF, *ec_Rates_Q5;
  const opus_int16 *deltaMin_Q15;
};
struct stereo_enc_state {
  opus_int16 pred_prev_Q13[2], sMid[2], sSide[2]; opus_int32 mid_side_amp_Q0[4]; opus_int16 smth_width_Q14, width_prev_Q14, silent_side_len;
  opus_int8 predIx[3][2][3], mid_only_flags[3];
};
struct stereo_dec_state { opus_int16 pred_prev_Q13[2], sMid[2], sSide[2]; };
struct SideInfoIndices {
  opus_int8 GainsIndices[4], LTPIndex[4], NLSFIndices[16 + 1]; opus_int16 lagIndex;
  opus_int8 contourIndex, signalType, quantOffsetType, NLSFInterpCoef_Q2, PERIndex, LTP_scaleIndex, Seed;
};
struct silk_encoder_state {
  opus_int32 In_HP_State[2], variable_HP_smth1_Q15, variable_HP_smth2_Q15; silk_LP_state sLP; silk_VAD_state sVAD; silk_nsq_state sNSQ; opus_int16 prev_NLSFq_Q15[16];
  int speech_activity_Q8, allow_bandwidth_switch; opus_int8 prevSignalType; int prevLag, pitch_LPC_win_length; opus_int32 API_fs_Hz, prev_API_fs_Hz;
  int maxInternal_fs_Hz, minInternal_fs_Hz, desiredInternal_fs_Hz, fs_kHz, nb_subfr, frame_length, subfr_length, ltp_mem_length, la_pitch, la_shape, shapeWinLength; opus_int32 TargetRate_bps; int PacketSize_ms; opus_int32 frameCounter;
  int Complexity, nStatesDelayedDecision, useInterpolatedNLSFs, shapingLPCOrder, predictLPCOrder, pitchEstimationComplexity, pitchEstimationLPCOrder; opus_int32 pitchEstimationThreshold_Q16, sum_log_gain_Q7;
  int NLSF_MSVQ_Survivors, first_frame_after_reset, controlled_since_last_payload, warping_Q16, useCBR, prefillFlag; const opus_uint8 *pitch_lag_low_bits_iCDF, *pitch_contour_iCDF; const silk_NLSF_CB_struct *psNLSF_CB;
  int input_quality_bands_Q15[4], input_tilt_Q15, SNR_dB_Q7; opus_int8 VAD_flags[3]; SideInfoIndices indices; opus_int8 pulses[((5 * 4) * 16)]; opus_int16 inputBuf[((5 * 4) * 16) + 2];
  int inputBufIx, nFramesPerPacket, nFramesEncoded, nChannelsAPI, nChannelsInternal, ec_prevSignalType; opus_int16 ec_prevLagIndex; silk_resampler_state_struct resampler_state;
};
struct silk_PLC_struct {
  opus_int32 pitchL_Q8, rand_seed, conc_energy, prevGain_Q16[2];
  opus_int16 LTPCoef_Q14[5], prevLPC_Q12[16], randScale_Q14, prevLTP_scale_Q14;
  int last_frame_lost, conc_energy_shift, fs_kHz, nb_subfr, subfr_length;
};
struct silk_CNG_struct { opus_int32 CNG_exc_buf_Q14[((5 * 4) * 16)], CNG_synth_state[16], CNG_smth_Gain_Q16, rand_seed; opus_int16 CNG_smth_NLSF_Q15[16]; int fs_kHz; };
struct silk_decoder_state {
  opus_int32 prev_gain_Q16, exc_Q14[((5 * 4) * 16)], sLPC_Q14_buf[16], fs_API_hz;
  opus_int16 outBuf[((5 * 4) * 16) + 2 * (5 * 16)], prevNLSF_Q15[16], ec_prevLagIndex;
  int lagPrev, fs_kHz, nb_subfr, frame_length, subfr_length, ltp_mem_length, LPC_order, first_frame_after_reset, nFramesDecoded, nFramesPerPacket, ec_prevSignalType, VAD_flags[3], LBRR_flag, LBRR_flags[3], lossCnt, prevSignalType;
  opus_int8 LastGainIndex;
  const opus_uint8 *pitch_lag_low_bits_iCDF, *pitch_contour_iCDF;
  const silk_NLSF_CB_struct *psNLSF_CB;
  silk_resampler_state_struct resampler_state; SideInfoIndices indices; silk_CNG_struct *sCNG; silk_PLC_struct sPLC;
};
static void silk_CNG_Reset(silk_decoder_state *psDec);
static void silk_release_cng(silk_decoder_state *psDec) noexcept {
  if (psDec != nullptr && psDec->sCNG != nullptr) {
    opus_free(psDec->sCNG);
    psDec->sCNG = nullptr;
  }
}
[[nodiscard]] static auto silk_ensure_cng(silk_decoder_state *psDec) noexcept -> silk_CNG_struct * {
  if (psDec->sCNG == nullptr) {
    auto cng = make_opus_owned<silk_CNG_struct>(sizeof(silk_CNG_struct));
    if (!cng) return nullptr;
    zero_n_bytes(cng.get(), sizeof(silk_CNG_struct));
    psDec->sCNG = cng.release();
    silk_CNG_Reset(psDec);
    psDec->sCNG->fs_kHz = psDec->fs_kHz;
  }
  return psDec->sCNG;
}
struct silk_decoder_control { int pitchL[4], LTP_scale_Q14; opus_int32 Gains_Q16[4]; opus_int16 PredCoef_Q12[2][16], LTPCoef_Q14[5 * 4]; };
struct ref_OpusDecoder {
  opus_int32 Fs;
  int channels, stream_channels, mode, prev_mode, bandwidth, frame_size, prev_redundancy, last_packet_duration;
  opus_uint32 rangeFinal;
  int celt_dec_offset, silk_dec_offset;
  silk_DecControlStruct DecControl;
};
static_assert(sizeof(ref_OpusDecoder) <= 80);
static int ref_opus_decoder_get_size(int channels) { int silkDecSizeBytes, ret; if (channels < 1 || channels > 2) return 0; ret = silk_Get_Decoder_Size(&silkDecSizeBytes); if (ret) return 0; return align(sizeof(ref_OpusDecoder)) + align(silkDecSizeBytes) + celt_decoder_get_size(channels);
}
static int ref_opus_decoder_init(ref_OpusDecoder *st, opus_int32 Fs, int channels) {
  if ((Fs != 48000 && Fs != 24000 && Fs != 16000 && Fs != 12000 && Fs != 8000) || (channels != 1 && channels != 2)) return -1;
  zero_n_items((char *)st, static_cast<std::size_t>(ref_opus_decoder_get_size(channels)));
  int silkDecSizeBytes, ret = silk_Get_Decoder_Size(&silkDecSizeBytes);
  if (ret) return -3;
  silkDecSizeBytes = align(silkDecSizeBytes); st->silk_dec_offset = align(sizeof(ref_OpusDecoder));
  st->celt_dec_offset = st->silk_dec_offset + silkDecSizeBytes;
  void *silk_dec = (char *)st + st->silk_dec_offset;
  auto *celt_dec = (CeltDecoderInternal *)((char *)st + st->celt_dec_offset);
  st->stream_channels = st->channels = channels;
  st->Fs = Fs; st->DecControl.API_sampleRate = st->Fs; st->DecControl.nChannelsAPI = st->channels;
  if (silk_InitDecoder(silk_dec)) return -3;
  if (celt_decoder_init(celt_dec, Fs, channels) != 0) return -3;
  st->prev_mode = 0;
  st->frame_size = Fs / 400;
  return 0;
}
static void smooth_fade(const opus_res *in1, const opus_res *in2, opus_res *out, int overlap, int channels, const celt_coef *window, opus_int32 Fs) {
  int inc = 48000 / Fs;
  for (int c = 0; c < channels; c++)
    for (int i = 0; i < overlap; i++) {
      opus_val16 w = window[i * inc]; w *= w;
      out[i * channels + c] = w * in2[i * channels + c] + (1.0f - w) * in1[i * channels + c]; }
}
static int opus_packet_get_mode(const unsigned char *data) {
  if (data[0] & 0x80) return opus_mode_celt_only;
  if ((data[0] & 0x60) == 0x60) return opus_mode_hybrid;
  return opus_mode_silk_only;
}
static int opus_decode_frame(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_res *pcm, int frame_size, int decode_fec) {
  ec_dec dec; opus_int32 silk_frame_size;
  opus_res *pcm_transition = nullptr;
  int i, silk_ret = 0, celt_ret = 0;
  int pcm_transition_silk_size, pcm_transition_celt_size, redundant_audio_size;
  int audiosize, mode, bandwidth, transition = 0, start_band;
  int redundancy = 0, redundancy_bytes = 0, celt_to_silk = 0;
  int F2_5, F5, F10, F20;
  const celt_coef *window;
  opus_uint32 redundant_rng = 0;
  int celt_accum;
  void *silk_dec = (char *)st + st->silk_dec_offset;
  auto *celt_dec = (CeltDecoderInternal *)((char *)st + st->celt_dec_offset);
  F20 = st->Fs / 50; F10 = F20 >> 1; F5 = F10 >> 1; F2_5 = F5 >> 1;
  if (frame_size < F2_5) { return -2;
}
  frame_size = std::min(frame_size, st->Fs / 25 * 3);
  if (len <= 1) { data = nullptr; frame_size = std::min(frame_size, st->frame_size);
}
  if (data != nullptr) {
    audiosize = st->frame_size; mode = st->mode; bandwidth = st->bandwidth; ec_dec_init(&dec, (unsigned char *)data, len);
  } else {
    audiosize = frame_size; mode = st->prev_redundancy ? opus_mode_celt_only : st->prev_mode; bandwidth = 0;
    if (mode == 0) {
      for (i = 0; i < audiosize * st->channels; i++) pcm[i] = 0;
      return audiosize;
}
    if (audiosize > F20) {
      for (; audiosize > 0;) {
        int ret = opus_decode_frame(st, nullptr, 0, pcm, std::min(audiosize, F20), 0);
        if (ret < 0) { return ret;
}
        pcm += ret * st->channels; audiosize -= ret;
}
      return frame_size;
    } else if (audiosize < F20) {
      if (audiosize > F10) audiosize = F10;
      else if (mode != opus_mode_silk_only && audiosize > F5 && audiosize < F10) audiosize = F5; }
}
  celt_accum = (mode != opus_mode_celt_only); pcm_transition_silk_size = 1;
  pcm_transition_celt_size = 1;
  if (data != nullptr && st->prev_mode > 0 && ((mode == opus_mode_celt_only && st->prev_mode != opus_mode_celt_only && !st->prev_redundancy) || (mode != opus_mode_celt_only && st->prev_mode == opus_mode_celt_only))) {
    transition = 1;
    if (mode == opus_mode_celt_only) pcm_transition_celt_size = F5 * st->channels;
    else pcm_transition_silk_size = F5 * st->channels;
}
  auto *pcm_transition_celt = OPUS_SCRATCH(opus_res, pcm_transition_celt_size);
  if (transition && mode == opus_mode_celt_only) {
    pcm_transition = pcm_transition_celt;
    opus_decode_frame(st, nullptr, 0, pcm_transition, std::min(F5, audiosize), 0);
}
  if (audiosize > frame_size) {
    return -1;
  } else { frame_size = audiosize;
}
  if (mode != opus_mode_celt_only) {
    int lost_flag, decoded_samples, pcm_too_small;
    opus_res *pcm_ptr;
    int pcm_silk_size = 1;
    pcm_too_small = (frame_size < F10);
    if (pcm_too_small) pcm_silk_size = F10 * st->channels;
    auto *pcm_silk = OPUS_SCRATCH(opus_res, pcm_silk_size);
    if (pcm_too_small) pcm_ptr = pcm_silk;
    else pcm_ptr = pcm;
    if (st->prev_mode == opus_mode_celt_only) silk_ResetDecoder(silk_dec);
    st->DecControl.payloadSize_ms = std::max(10, 1000 * audiosize / st->Fs);
    if (data != nullptr) {
      st->DecControl.nChannelsInternal = st->stream_channels;
      if (mode == opus_mode_silk_only) {
        if (bandwidth == 1101) { st->DecControl.internalSampleRate = 8000;
}
        else if (bandwidth == 1102) { st->DecControl.internalSampleRate = 12000;
}
        else { st->DecControl.internalSampleRate = 16000;
}
      } else { st->DecControl.internalSampleRate = 16000; }
}
    lost_flag = data == nullptr ? 1 : 2 * !!decode_fec; decoded_samples = 0;
    for (; decoded_samples < frame_size;) {
      int first_frame = decoded_samples == 0;
      silk_ret = silk_Decode(silk_dec, &st->DecControl, lost_flag, first_frame, &dec, pcm_ptr, &silk_frame_size);
      if (silk_ret) { if (lost_flag) { silk_frame_size = frame_size; zero_n_items(pcm_ptr, static_cast<std::size_t>(frame_size * st->channels)); } else { return -3; }
}
      pcm_ptr += silk_frame_size * st->channels; decoded_samples += silk_frame_size;
}
    if (pcm_too_small) { copy_n_items(pcm_silk, static_cast<std::size_t>(frame_size * st->channels), pcm); }
}
  start_band = 0;
  if (!decode_fec && mode != opus_mode_celt_only && data != nullptr && ec_tell(&dec) + 17 + 20 * (mode == opus_mode_hybrid) <= 8 * len) {
    if (mode == opus_mode_hybrid) redundancy = ec_dec_bit_logp(&dec, 12);
    else redundancy = 1;
    if (redundancy) {
      celt_to_silk = ec_dec_bit_logp(&dec, 1);
      redundancy_bytes = mode == opus_mode_hybrid ? (opus_int32)ec_dec_uint(&dec, 256) + 2 : len - ((ec_tell(&dec) + 7) >> 3); len -= redundancy_bytes;
      if (len * 8 < ec_tell(&dec)) { len = 0; redundancy_bytes = 0; redundancy = 0;
}
      dec.storage -= redundancy_bytes; }
}
  if (mode != opus_mode_celt_only) start_band = 17;
  if (redundancy) { transition = 0; pcm_transition_silk_size = 1;
}
  auto *pcm_transition_silk = OPUS_SCRATCH(opus_res, pcm_transition_silk_size);
  if (transition && mode != opus_mode_celt_only) {
    pcm_transition = pcm_transition_silk;
    opus_decode_frame(st, nullptr, 0, pcm_transition, std::min(F5, audiosize), 0);
}
  if (bandwidth) {
    const auto endband = bandwidth_to_endband(bandwidth);
    if (celt_decoder_set_end_band(celt_dec, static_cast<opus_int32>(endband)) != 0) { return -3; }
}
  if (celt_decoder_set_stream_channels(celt_dec, static_cast<opus_int32>(st->stream_channels)) != 0) { return -3;
}
  redundant_audio_size = redundancy ? F5 * st->channels : 1;
  auto *redundant_audio = OPUS_SCRATCH(opus_res, redundant_audio_size);
  if (redundancy && celt_to_silk) {
    if (celt_decoder_set_start_band(celt_dec, 0) != 0) { return -3;
}
      celt_decode_with_ec(celt_dec, data + len, redundancy_bytes, redundant_audio, F5, nullptr, 0);
    redundant_rng = celt_decoder_final_range(celt_dec);
}
  if (celt_decoder_set_start_band(celt_dec, static_cast<opus_int32>(start_band)) != 0) { return -3;
}
  if (mode != opus_mode_silk_only) {
    int celt_frame_size = std::min(F20, frame_size);
    if (mode != st->prev_mode && st->prev_mode > 0 && !st->prev_redundancy) {
      celt_decoder_reset_state(celt_dec);
}
      celt_ret = celt_decode_with_ec(celt_dec, decode_fec ? nullptr : data, len, pcm, celt_frame_size, &dec, celt_accum);
    st->rangeFinal = celt_decoder_final_range(celt_dec);
  } else {
    unsigned char silence[2] = {0xFF, 0xFF};
    if (!celt_accum) { zero_n_items(pcm, static_cast<std::size_t>(frame_size * st->channels));
}
    if (st->prev_mode == opus_mode_hybrid && !(redundancy && celt_to_silk && st->prev_redundancy)) {
      if (celt_decoder_set_start_band(celt_dec, 0) != 0) { return -3;
}
      celt_decode_with_ec(celt_dec, silence, 2, pcm, F2_5, nullptr, celt_accum);
}
    st->rangeFinal = dec.rng;
}
  {
    window = celt_decoder_mode(celt_dec)->window;
}
  if (redundancy && !celt_to_silk) {
    celt_decoder_reset_state(celt_dec);
    if (celt_decoder_set_start_band(celt_dec, 0) != 0) { return -3;
}
    celt_decode_with_ec(celt_dec, data + len, redundancy_bytes, redundant_audio, F5, nullptr, 0);
    redundant_rng = celt_decoder_final_range(celt_dec);
    smooth_fade(pcm + st->channels * (frame_size - F2_5), redundant_audio + st->channels * F2_5, pcm + st->channels * (frame_size - F2_5), F2_5, st->channels, window, st->Fs);
}
  if (redundancy && celt_to_silk && (st->prev_mode != opus_mode_silk_only || st->prev_redundancy)) {
    copy_n_items(redundant_audio, static_cast<std::size_t>(F2_5 * st->channels), pcm);
    smooth_fade(redundant_audio + st->channels * F2_5, pcm + st->channels * F2_5, pcm + st->channels * F2_5, F2_5, st->channels, window, st->Fs);
}
  if (transition) {
    if (audiosize >= F5) {
      copy_n_items(pcm_transition, static_cast<std::size_t>(st->channels * F2_5), pcm);
      smooth_fade(pcm_transition + st->channels * F2_5, pcm + st->channels * F2_5, pcm + st->channels * F2_5, F2_5, st->channels, window, st->Fs);
    } else { smooth_fade(pcm_transition, pcm, pcm, F2_5, st->channels, window, st->Fs); }
}
  if (len <= 1) { st->rangeFinal = 0; } else { st->rangeFinal ^= redundant_rng; } st->prev_mode = mode;
  st->prev_redundancy = redundancy && !celt_to_silk;
  return celt_ret < 0 ? celt_ret : audiosize;
}
static int decode_native(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_res *pcm, int frame_size, int decode_fec, int self_delimited, opus_int32 *packet_offset) {
  int i, nb_samples, count, offset;
  unsigned char toc;
  int packet_frame_size, packet_bandwidth, packet_mode, packet_stream_channels;
  std::array<opus_int16, 48> size;
  if (decode_fec < 0 || decode_fec > 1) return -1;
  if ((decode_fec || len == 0 || data == nullptr) && frame_size % (st->Fs / 400) != 0) return -1;
  if (len == 0 || data == nullptr) {
    int pcm_count = 0;
    for (; pcm_count < frame_size;) {
      int ret = opus_decode_frame(st, nullptr, 0, pcm + pcm_count * st->channels, frame_size - pcm_count, 0);
      if (ret < 0) return ret;
      pcm_count += ret;
}
    opus_assume(pcm_count == frame_size); st->last_packet_duration = pcm_count;
    return pcm_count;
  } else if (len < 0) return -1; packet_mode = opus_packet_get_mode(data); packet_bandwidth = ref_opus_packet_get_bandwidth(data);
  packet_frame_size = ref_opus_packet_get_samples_per_frame(data, st->Fs); packet_stream_channels = ref_opus_packet_get_nb_channels(data);
  count = ref_opus_packet_parse_impl(data, len, self_delimited, &toc, nullptr, size.data(), &offset, packet_offset, nullptr, nullptr);
  if (count < 0) return count;
  data += offset;
  if (decode_fec) {
    int duration_copy, ret;
  if (frame_size < packet_frame_size || packet_mode == opus_mode_celt_only || st->mode == opus_mode_celt_only) return decode_native(st, nullptr, 0, pcm, frame_size, 0, 0, nullptr);
    duration_copy = st->last_packet_duration;
    if (frame_size - packet_frame_size != 0) {
    ret = decode_native(st, nullptr, 0, pcm, frame_size - packet_frame_size, 0, 0, nullptr);
      if (ret < 0) {
        st->last_packet_duration = duration_copy;
        return ret;
}
      opus_assume(ret == frame_size - packet_frame_size);
}
    st->mode = packet_mode; st->bandwidth = packet_bandwidth;
    st->frame_size = packet_frame_size; st->stream_channels = packet_stream_channels;
    ret = opus_decode_frame(st, data, size[0], pcm + st->channels * (frame_size - packet_frame_size), packet_frame_size, 1);
    if (ret < 0) return ret;
    else {
      st->last_packet_duration = frame_size;
      return frame_size; }
}
  if (count * packet_frame_size > frame_size) return -2;
  st->mode = packet_mode; st->bandwidth = packet_bandwidth;
  st->frame_size = packet_frame_size; st->stream_channels = packet_stream_channels; nb_samples = 0;
  for (i = 0; i < count; i++) {
    int ret = opus_decode_frame(st, data, size[i], pcm + nb_samples * st->channels, frame_size - nb_samples, 0);
    if (ret < 0) return ret;
    opus_assume(ret == packet_frame_size); data += size[i];
    nb_samples += ret;
}
  st->last_packet_duration = nb_samples;
  return nb_samples;
}
#if OPUS_GCC_DECODE_FASTPATH
constexpr int opus_decode_int16_fast_unavailable = -1000000;
static int decode_native_celt_i16_fast(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size) {
  if (data == nullptr || len <= 1 || st->Fs != 48000) return opus_decode_int16_fast_unavailable;
  const int packet_mode = opus_packet_get_mode(data);
  if (packet_mode != opus_mode_celt_only) return opus_decode_int16_fast_unavailable;
  if (st->prev_mode > 0 && st->prev_mode != opus_mode_celt_only && !st->prev_redundancy) return opus_decode_int16_fast_unavailable;

  const int packet_bandwidth = ref_opus_packet_get_bandwidth(data);
  const int packet_frame_size = ref_opus_packet_get_samples_per_frame(data, st->Fs);
  const int packet_stream_channels = ref_opus_packet_get_nb_channels(data);
  std::array<opus_int16, 48> size;
  unsigned char toc;
  int offset = 0;
  const int count = ref_opus_packet_parse_impl(data, len, 0, &toc, nullptr, size.data(), &offset, nullptr, nullptr, nullptr);
  if (count < 0) return opus_decode_int16_fast_unavailable;
  if (count * packet_frame_size > frame_size) return opus_decode_int16_fast_unavailable;

  auto *celt_dec = reinterpret_cast<CeltDecoderInternal *>(reinterpret_cast<char *>(st) + st->celt_dec_offset);
  if (packet_bandwidth) {
    const auto endband = bandwidth_to_endband(packet_bandwidth);
    if (celt_decoder_set_end_band(celt_dec, static_cast<opus_int32>(endband)) != 0) return -3;
  }
  if (celt_decoder_set_stream_channels(celt_dec, static_cast<opus_int32>(packet_stream_channels)) != 0) return -3;
  if (celt_decoder_set_start_band(celt_dec, 0) != 0) return -3;

  st->mode = packet_mode; st->bandwidth = packet_bandwidth;
  st->frame_size = packet_frame_size; st->stream_channels = packet_stream_channels;
  data += offset;
  int nb_samples = 0;
  for (int index = 0; index < count; ++index) {
    ec_dec dec;
    ec_dec_init(&dec, const_cast<unsigned char *>(data), static_cast<opus_uint32>(size[index]));
    const int ret = celt_decode_with_ec(celt_dec, data, size[index], nullptr, packet_frame_size, &dec, 0, pcm + nb_samples * st->channels);
    if (ret < 0) return ret;
    opus_assume(ret == packet_frame_size);
    st->rangeFinal = celt_decoder_final_range(celt_dec);
    data += size[index];
    nb_samples += ret;
  }
  st->prev_mode = packet_mode; st->prev_redundancy = 0; st->last_packet_duration = nb_samples;
  return nb_samples;
}
#endif
static int ref_opus_decode(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec) {
  if (frame_size <= 0) return -1;
  opus_assume(st->channels == 1 || st->channels == 2);
#if OPUS_GCC_DECODE_FASTPATH
  if (!decode_fec) {
    const int fast_ret = decode_native_celt_i16_fast(st, data, len, pcm, frame_size);
    if (fast_ret != opus_decode_int16_fast_unavailable) return fast_ret;
  }
#endif
  if (data != nullptr && len > 0 && !decode_fec) {
    const int nb_samples = ref_opus_decoder_get_nb_samples(st, data, len);
    if (nb_samples > 0) frame_size = std::min(frame_size, nb_samples);
    else return -4;
}
  auto *out = OPUS_SCRATCH(opus_res, frame_size * st->channels);
  const int ret = decode_native(st, data, len, out, frame_size, decode_fec, 0, nullptr);
  if (ret > 0) celt_float2int16_c(std::span<const float>{out, static_cast<std::size_t>(ret * st->channels)}, std::span<short>{pcm, static_cast<std::size_t>(ret * st->channels)});
  return ret;
}
static int ref_opus_decode_float(ref_OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec) {
  if (frame_size <= 0) return -1;
  return decode_native(st, data, len, pcm, frame_size, decode_fec, 0, nullptr);
}
[[nodiscard]] constexpr auto encoder_uses_celt(int application) noexcept -> bool { return application != opus_application_silk_only;
}
[[nodiscard]] constexpr auto encoder_uses_silk(int application) noexcept -> bool { return application != opus_application_celt_only;
}
template <typename SilkMode> inline void update_silk_internal_sample_rate(SilkMode &silk_mode, int bandwidth) noexcept {
  if (bandwidth == 1101) {
    silk_mode.maxInternalSampleRate = 8000;
  } else if (bandwidth == 1102) {
    silk_mode.maxInternalSampleRate = 12000;
  } else { silk_mode.maxInternalSampleRate = 16000; }
}
[[nodiscard]] constexpr auto bandwidth_to_endband(const int bandwidth) noexcept -> int {
  switch (bandwidth) {
  case 1101: return 13;
  case 1102:
  case 1103: return 17;
  case 1104: return 19;
  case 1105: return 21;
  default: return 21; }
}
[[nodiscard]] constexpr auto is_supported_opus_frame_size(const opus_int32 Fs, const opus_int32 frame_size) noexcept -> bool {
  const auto scaled_by_50 = 50 * frame_size;
  return 400 * frame_size == Fs || 200 * frame_size == Fs || 100 * frame_size == Fs || scaled_by_50 == Fs || 25 * frame_size == Fs || scaled_by_50 == 3 * Fs || scaled_by_50 == 4 * Fs || scaled_by_50 == 5 * Fs || scaled_by_50 == 6 * Fs;
}
static OPUS_COLD OPUS_NOINLINE void reset_ref_decoder_state(ref_OpusDecoder *st, void *silk_dec, CeltDecoderInternal *celt_dec) {
    celt_decoder_reset_state(celt_dec); silk_ResetDecoder(silk_dec);
  st->DecControl = {};
  st->stream_channels = st->channels; st->bandwidth = 0; st->mode = 0; st->prev_mode = 0;
  st->frame_size = st->Fs / 400; st->prev_redundancy = 0; st->last_packet_duration = 0; st->rangeFinal = 0;
  st->DecControl.API_sampleRate = st->Fs; st->DecControl.nChannelsAPI = st->channels;
}
static OPUS_COLD OPUS_NOINLINE void destroy_ref_decoder_state(ref_OpusDecoder *st) noexcept {
  if (st == nullptr || st->silk_dec_offset <= 0) return;
  silk_destroy_decoder(reinterpret_cast<char *>(st) + st->silk_dec_offset);
}
static constexpr int ref_opus_packet_get_bandwidth(const unsigned char *data) {
  if (data[0] & 0x80) {
    const int bw = 1102 + ((data[0] >> 5) & 0x3);
    return bw == 1102 ? 1101 : bw;
}
  if ((data[0] & 0x60) == 0x60) return (data[0] & 0x10) ? 1105 : 1104;
  return 1101 + ((data[0] >> 5) & 0x3);
}
static constexpr int ref_opus_packet_get_nb_channels(const unsigned char *data) { return (data[0] & 0x4) ? 2 : 1;
}
static constexpr int ref_opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len) {
  if (len < 1) return -1;
  const int count = packet[0] & 0x3;
  if (count == 0) return 1;
  if (count != 3) return 2;
  if (len < 2) return -4;
  return packet[1] & 0x3F;
}
static OPUS_NOINLINE int ref_opus_packet_get_nb_samples(const unsigned char packet[], opus_int32 len, opus_int32 Fs) {
  int samples;
  int count = ref_opus_packet_get_nb_frames(packet, len);
  if (count < 0) return count;
  samples = count * ref_opus_packet_get_samples_per_frame(packet, Fs);
  if (samples * 25 > Fs * 3) return -4;
  else return samples;
}
static constexpr int ref_opus_decoder_get_nb_samples(const ref_OpusDecoder *dec, const unsigned char packet[], opus_int32 len) { return ref_opus_packet_get_nb_samples(packet, len, dec->Fs);
}
static void pitch_downsample(std::span<celt_sig *const> x, opus_val16 *x_lp, int len, int factor), pitch_search(const opus_val16 *x_lp, opus_val16 *y, int len, int max_pitch, int *pitch);
static opus_val16 remove_doubling(opus_val16 *x, int maxperiod, int minperiod, int N, int *T0, int prev_period, opus_val16 prev_gain);
static void xcorr_kernel_c(const opus_val16 *x, const opus_val16 *y, opus_val32 sum[4], int len) {
  opus_assume(len >= 3);
  const auto input = std::span<const opus_val16>{x, static_cast<std::size_t>(len)};
  const auto taps = std::span<const opus_val16>{y, static_cast<std::size_t>(len + 3)};
  for (int index = 0; index < len; ++index) { const auto sample = static_cast<opus_val32>(input[static_cast<std::size_t>(index)]); sum[0] += sample * static_cast<opus_val32>(taps[static_cast<std::size_t>(index)]); sum[1] += sample * static_cast<opus_val32>(taps[static_cast<std::size_t>(index + 1)]); sum[2] += sample * static_cast<opus_val32>(taps[static_cast<std::size_t>(index + 2)]); sum[3] += sample * static_cast<opus_val32>(taps[static_cast<std::size_t>(index + 3)]); }
}
static void dual_inner_prod_c(const opus_val16 *x, const opus_val16 *y01, const opus_val16 *y02, int N, opus_val32 *xy1, opus_val32 *xy2) {
  const auto input = std::span<const opus_val16>{x, static_cast<std::size_t>(N)};
  auto sum_1 = opus_val32{0};
  auto sum_2 = opus_val32{0};
  for (int index = 0; index < N; ++index) {
    const auto sample = static_cast<opus_val32>(input[static_cast<std::size_t>(index)]);
    sum_1 += sample * static_cast<opus_val32>(y01[index]); sum_2 += sample * static_cast<opus_val32>(y02[index]);
}
  *xy1 = sum_1; *xy2 = sum_2;
}
[[nodiscard]] static inline auto celt_inner_prod_c(const opus_val16 *x, const opus_val16 *y, int N) -> opus_val32 {
  opus_val32 sum = 0;
  for (int i = 0; i < N; i++) sum += x[i] * y[i];
  return sum;
}
static void celt_pitch_xcorr_c(std::span<const opus_val16> x, std::span<const opus_val16> y, std::span<opus_val32> xcorr);
template <std::size_t Size> using u8_table = std::array<opus_uint8, Size>; template <std::size_t Size> using i8_table = std::array<opus_int8, Size>;
template <std::size_t Size> using i16_table = std::array<opus_int16, Size>; template <std::size_t Size> using i32_table = std::array<opus_int32, Size>;
template <std::size_t Rows, std::size_t Cols> using u8_matrix = std::array<std::array<opus_uint8, Cols>, Rows>;
template <std::size_t Rows, std::size_t Cols> using i16_matrix = std::array<std::array<opus_int16, Cols>, Rows>; template <std::size_t Rows, std::size_t Cols> using i32_matrix = std::array<std::array<opus_int32, Cols>, Rows>;
struct silk_ltp_codebook_view { std::span<const opus_uint8> gain_icdf, gain_bits_q5; std::span<const opus_int8> vq_q7; std::span<const opus_uint8> vq_gain_q7; int vq_size; };
struct silk_nlsf_codebook_choice { opus_int16 order; const silk_NLSF_CB_struct *codebook; };
namespace {
extern const u8_matrix<3, 64 / 8> silk_gain_iCDF; extern const u8_table<36 - -4 + 1> silk_delta_gain_iCDF;
extern const u8_table<2 * (18 - 2)> silk_pitch_lag_iCDF; extern const u8_table<21> silk_pitch_delta_iCDF;
extern const u8_table<34> silk_pitch_contour_iCDF; extern const u8_table<11> silk_pitch_contour_NB_iCDF;
extern const u8_table<12> silk_pitch_contour_10_ms_iCDF;
extern const u8_table<3> silk_pitch_contour_10_ms_NB_iCDF, silk_uniform3_iCDF, silk_LTP_per_index_iCDF, silk_LTPscale_iCDF;
extern const u8_matrix<10, 16 + 2> silk_pulses_per_block_iCDF; extern const u8_matrix<10 - 1, 16 + 2> silk_pulses_per_block_BITS_Q5;
extern const u8_matrix<2, 10 - 1> silk_rate_levels_iCDF, silk_rate_levels_BITS_Q5; extern const u8_table<4> silk_max_pulses_table, silk_uniform4_iCDF, silk_type_offset_VAD_iCDF;
extern const u8_table<152> silk_shell_code_table0, silk_shell_code_table1, silk_shell_code_table2, silk_shell_code_table3; extern const u8_table<16 + 1> silk_shell_code_table_offsets;
extern const u8_table<2> silk_lsb_iCDF, silk_type_offset_no_VAD_iCDF, silk_stereo_only_code_mid_iCDF; extern const u8_table<42> silk_sign_iCDF;
extern const u8_table<5> silk_uniform5_iCDF, silk_NLSF_interpolation_factor_iCDF; extern const u8_table<6> silk_uniform6_iCDF; extern const u8_table<8> silk_uniform8_iCDF; extern const u8_table<7> silk_NLSF_EXT_iCDF;
extern const std::array<silk_ltp_codebook_view, 3> silk_LTP_codebooks; extern const i16_table<3> silk_LTPScales_table_Q14; extern const i16_table<16> silk_stereo_pred_quant_Q13; extern const u8_table<25> silk_stereo_pred_joint_iCDF;
extern const std::array<std::span<const opus_uint8>, 2> silk_LBRR_flags_iCDFs; extern const silk_NLSF_CB_struct silk_NLSF_CB_WB, silk_NLSF_CB_NB_MB;
extern const i16_matrix<2, 2> silk_Quantization_Offsets_Q10; extern const i32_matrix<5, 3> silk_Transition_LP_B_Q28; extern const i32_matrix<5, 2> silk_Transition_LP_A_Q28; extern const i16_table<128 + 1> silk_LSFCosTab_FIX_Q12;
}
template <typename CB1, typename Weights, typename Icdf, typename Pred, typename Sel, typename EcIcdf, typename Rates, typename Delta>
consteval auto make_silk_nlsf_cb(const int vectors, const int order, const int quant_step_q16, const int inv_quant_step_q6, const CB1 &cb1_q8, const Weights &weights_q9, const Icdf &cb1_icdf, const Pred &pred_q8, const Sel &ec_sel, const EcIcdf &ec_icdf, const Rates &ec_rates_q5, const Delta &delta_min_q15) -> silk_NLSF_CB_struct { return {static_cast<opus_int16>(vectors), static_cast<opus_int16>(order), static_cast<opus_int16>(quant_step_q16), static_cast<opus_int16>(inv_quant_step_q6), cb1_q8.data(), weights_q9.data(), cb1_icdf.data(), pred_q8.data(), ec_sel.data(), ec_icdf.data(), ec_rates_q5.data(), delta_min_q15.data()};
}
[[nodiscard]] constexpr auto silk_nlsf_codebook_for_fs(const int fs_kHz) noexcept -> silk_nlsf_codebook_choice { return fs_kHz == 8 || fs_kHz == 12 ? silk_nlsf_codebook_choice{10, &silk_NLSF_CB_NB_MB} : silk_nlsf_codebook_choice{16, &silk_NLSF_CB_WB};
}
[[nodiscard]] constexpr auto silk_nlsf_cb1_icdf(const silk_NLSF_CB_struct *cb, const int signalType) noexcept -> const opus_uint8 * { return cb->CB1_iCDF + (signalType >> 1) * cb->nVectors;
}
[[nodiscard]] constexpr auto silk_nlsf_cb1_q8(const silk_NLSF_CB_struct *cb, const int index) noexcept -> const opus_uint8 * { return cb->CB1_NLSF_Q8 + index * cb->order;
}
[[nodiscard]] constexpr auto silk_nlsf_weights_q9(const silk_NLSF_CB_struct *cb, const int index) noexcept -> const opus_int16 * { return cb->CB1_Wght_Q9 + index * cb->order;
}
[[nodiscard]] constexpr auto silk_nlsf_ec_sel_row(const silk_NLSF_CB_struct *cb, const int index) noexcept -> const opus_uint8 * { return cb->ec_sel + index * cb->order / 2;
}
static void silk_PLC_Reset(silk_decoder_state *psDec), silk_PLC(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, std::span<opus_int16> frame, int lost), silk_PLC_glue_frames(silk_decoder_state *psDec, std::span<opus_int16> frame);
static void silk_stereo_LR_to_MS(stereo_enc_state *state, opus_int16 x1[], opus_int16 x2[], opus_int8 ix[2][3], opus_int8 *mid_only_flag, opus_int32 mid_side_rates_bps[], opus_int32 total_rate_bps, int prev_speech_act_Q8, int toMono, int fs_kHz, int frame_length),
    silk_stereo_MS_to_LR(stereo_dec_state *state, opus_int16 x1[], opus_int16 x2[], const opus_int32 pred_Q13[], int fs_kHz, int frame_length), silk_stereo_quant_pred(opus_int32 pred_Q13[], opus_int8 ix[2][3]),
    silk_stereo_encode_pred(ec_enc *psRangeEnc, opus_int8 ix[2][3]), silk_stereo_encode_mid_only(ec_enc *psRangeEnc, opus_int8 mid_only_flag), silk_stereo_decode_pred(ec_dec *psRangeDec, opus_int32 pred_Q13[]), silk_stereo_decode_mid_only(ec_dec *psRangeDec, int *decode_only_mid),
    silk_encode_signs(ec_enc *psRangeEnc, std::span<const opus_int8> pulses, const int signalType, const int quantOffsetType, std::span<const int> sum_pulses), silk_decode_signs(ec_dec *psRangeDec, std::span<opus_int16> pulses, const int signalType, const int quantOffsetType, std::span<const int> sum_pulses),
    silk_encode_pulses(ec_enc *psRangeEnc, const int signalType, const int quantOffsetType, std::span<opus_int8> pulses, const int frame_length), silk_shell_encoder(ec_enc *psRangeEnc, std::span<const int, 16> pulses0), silk_shell_decoder(std::span<opus_int16, 16> pulses0, ec_dec *psRangeDec, const int pulses4),
    silk_gains_quant(opus_int8 ind[4], opus_int32 gain_Q16[4], opus_int8 *prev_ind, const int conditional, const int nb_subfr), silk_gains_dequant(opus_int32 gain_Q16[4], const opus_int8 ind[4], opus_int8 *prev_ind, const int conditional, const int nb_subfr),
    silk_interpolate(std::span<opus_int16> xi, std::span<const opus_int16> x0, std::span<const opus_int16> x1, const int ifact_Q2), silk_quant_LTP_gains(opus_int16 B_Q14[4 * 5], opus_int8 cbk_index[4], opus_int8 *periodicity_index, opus_int32 *sum_gain_dB_Q7, int *pred_gain_dB_Q7, const opus_int32 XX_Q17[4 * 5 * 5], const opus_int32 xX_Q17[4 * 5], const int subfr_len, const int nb_subfr),
    silk_VQ_WMat_EC_c(opus_int8 *ind, opus_int32 *res_nrg_Q15, opus_int32 *rate_dist_Q8, int *gain_Q7, const opus_int32 *XX_Q17, const opus_int32 *xX_Q17, const opus_int8 *cb_Q7, const opus_uint8 *cb_gain_Q7, const opus_uint8 *cl_Q5, const int subfr_len, const opus_int32 max_gain_Q7, const int L),
    silk_NSQ_c(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, SideInfoIndices *psIndices, const opus_int16 x16[], opus_int8 pulses[], const opus_int16 *PredCoef_Q12, const opus_int16 LTPCoef_Q14[5 * 4], const opus_int16 AR_Q13[4 * 24], const int HarmShapeGain_Q14[4], const int Tilt_Q14[4], const opus_int32 LF_shp_Q14[4], const opus_int32 Gains_Q16[4], const int pitchL[4], const int Lambda_Q10, const int LTP_scale_Q14),
    silk_NSQ_del_dec_c(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, SideInfoIndices *psIndices, const opus_int16 x16[], opus_int8 pulses[], const opus_int16 *PredCoef_Q12, const opus_int16 LTPCoef_Q14[5 * 4], const opus_int16 AR_Q13[4 * 24], const int HarmShapeGain_Q14[4], const int Tilt_Q14[4], const opus_int32 LF_shp_Q14[4], const opus_int32 Gains_Q16[4], const int pitchL[4], const int Lambda_Q10, const int LTP_scale_Q14),
    silk_LP_variable_cutoff(silk_LP_state *psLP, opus_int16 *frame, const int frame_length), silk_process_NLSFs(silk_encoder_state *psEncC, opus_int16 PredCoef_Q12[2][16], opus_int16 pNLSF_Q15[16], const opus_int16 prev_NLSFq_Q15[16]), silk_NLSF_VQ(opus_int32 err_Q26[], const opus_int16 in_Q15[], const opus_uint8 pCB_Q8[], const opus_int16 pWght_Q9[], const int K, const int LPC_order),
    silk_NLSF_unpack(opus_int16 ec_ix[], opus_uint8 pred_Q8[], const silk_NLSF_CB_struct *psNLSF_CB, const int CB1_index), silk_NLSF_decode(opus_int16 *pNLSF_Q15, opus_int8 *NLSFIndices, const silk_NLSF_CB_struct *psNLSF_CB), silk_decode_indices(silk_decoder_state *psDec, ec_dec *psRangeDec, int FrameIndex, int decode_LBRR, int condCoding),
    silk_decode_parameters(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, int condCoding), silk_decode_core(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, opus_int16 xq[], const opus_int16 pulses[((5 * 4) * 16)]),
    silk_decode_pulses(ec_dec *psRangeDec, std::span<opus_int16> pulses, const int signalType, const int quantOffsetType, const int frame_length), silk_CNG_Reset(silk_decoder_state *psDec), silk_CNG(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, opus_int16 frame[], int length), silk_encode_indices(silk_encoder_state *psEncC, ec_enc *psRangeEnc, int condCoding);
static int check_control_input(silk_EncControlStruct *encControl), silk_control_audio_bandwidth(silk_encoder_state *psEncC, silk_EncControlStruct *encControl), silk_control_SNR(silk_encoder_state *psEncC, opus_int32 TargetRate_bps), silk_VAD_Init(silk_VAD_state *psSilk_VAD), silk_VAD_GetSA_Q8_c(silk_encoder_state *psEncC, const opus_int16 pIn[]),
    silk_reset_decoder(silk_decoder_state *psDec), silk_init_decoder(silk_decoder_state *psDec), silk_decoder_set_fs(silk_decoder_state *psDec, int fs_kHz, opus_int32 fs_API_Hz), silk_decode_frame(silk_decoder_state *psDec, ec_dec *psRangeDec, opus_int16 pOut[], opus_int32 *pN, int lostFlag, int condCoding);
static opus_int32 silk_stereo_find_predictor(opus_int32 *ratio_Q14, const opus_int16 x[], const opus_int16 y[], opus_int32 mid_res_amp_Q0[], int length, int smooth_coef_Q16), silk_gains_ID(const opus_int8 ind[4], const int nb_subfr),
    silk_NLSF_encode(opus_int8 *NLSFIndices, opus_int16 *pNLSF_Q15, const silk_NLSF_CB_struct *psNLSF_CB, const opus_int16 *pW_QW, const int NLSF_mu_Q20, const int nSurvivors, const int signalType),
    silk_NLSF_del_dec_quant(opus_int8 indices[], const opus_int16 x_Q10[], const opus_int16 w_Q5[], const opus_uint8 pred_coef_Q8[], const opus_int16 ec_ix[], const opus_uint8 ec_rates_Q5[], const int quant_step_size_Q16, const opus_int16 inv_quant_step_size_Q6, const opus_int32 mu_Q20, const opus_int16 order);
struct silk_shape_state_FLP { opus_int8 LastGainIndex; float HarmShapeGain_smth, Tilt_smth; };
struct silk_encoder_state_FLP { silk_encoder_state sCmn; silk_shape_state_FLP sShape; float x_buf[2 * ((5 * 4) * 16) + (5 * 16)], LTPCorr; };
struct silk_encoder_control_FLP {
  float Gains[4], PredCoef[2][16], LTPCoef[5 * 4], LTP_scale, AR[4 * 24], LF_MA_shp[4], LF_AR_shp[4], Tilt[4], HarmShapeGain[4], Lambda, input_quality, coding_quality, predGain, LTPredCodGain, ResNrg[4];
  int pitchL[4];
  opus_int32 GainsUnq_Q16[4]; opus_int8 lastGainIndexPrev;
};
struct silk_encoder {
  stereo_enc_state sStereo; opus_int32 nBitsExceeded;
  int nChannelsAPI, nChannelsInternal, nPrevChannelsInternal, timeSinceSwitchAllowed_ms, allowBandwidthSwitch, prev_decode_only_middle;
  silk_encoder_state_FLP state_Fxx[2];
};
struct StereoWidthState { opus_val32 XX, XY, YY; opus_val16 smoothed_width; opus_val16 max_follower; };
struct ref_OpusEncoder {
  int celt_enc_offset, silk_enc_offset; silk_EncControlStruct silk_mode;
  int application, channels, delay_compensation, force_channels; opus_int32 Fs;
  int use_vbr, vbr_constraint; opus_int32 bitrate_bps, user_bitrate_bps; int encoder_buffer;
  int stream_channels; opus_int16 hybrid_stereo_width_Q14; opus_int32 variable_HP_smth2_Q15; opus_val16 prev_HB_gain; opus_val32 hp_mem[4];
  int mode, prev_mode, prev_channels, prev_framesize, bandwidth, auto_bandwidth, silk_bw_switch, first, lightweight_voice_score_Q7, lightweight_music_score_Q7, lightweight_analysis_frames;
  int lightweight_prev_pitch_lag, lightweight_pitch_stability_Q7, lightweight_harmonic_music_Q7; StereoWidthState width_mem;
  opus_val32 peak_signal_energy; int nonfinal_frame; opus_uint32 rangeFinal; opus_res delay_buffer[480 * 2];
};
constexpr std::array<opus_int32, 8> voice_bandwidth_thresholds_common = numeric_blob_array<opus_int32, 8>(R"blob(00002328000002BC00002328000002BC000034BC000003E8000036B0000007D0)blob");
constexpr std::array<opus_int32, 8> music_bandwidth_thresholds_common = numeric_blob_array<opus_int32, 8>(R"blob(00002328000002BC00002328000002BC00002AF8000003E800002EE0000007D0)blob");
constexpr opus_int32 stereo_voice_threshold = 19000;
constexpr opus_int32 stereo_music_threshold = 17000;
constexpr std::array<std::array<opus_int32, 2>, 2> mode_thresholds = numeric_blob_matrix<opus_int32, 2, 2>(R"blob(0000FA00000027100000ABE000002710)blob");
static int ref_opus_encoder_get_size(int channels) { const auto ret = ref_opus_encoder_init(nullptr, 48000, channels, opus_application_audio); return ret < 0 ? 0 : ret;
}
static int ref_opus_encoder_init(ref_OpusEncoder *st, opus_int32 Fs, int channels, int application) {
  void *silk_enc = nullptr;
  CeltEncoderInternal *celt_enc = nullptr;
  int err;
  int ret, silkEncSizeBytes, celtEncSizeBytes = 0;
  int tot_size, base_size;
  if ((Fs != 48000 && Fs != 24000 && Fs != 16000 && Fs != 12000 && Fs != 8000) || (channels != 1 && channels != 2) || (application != opus_application_voip && application != opus_application_audio && application != opus_application_restricted_lowdelay && application != opus_application_silk_only && application != opus_application_celt_only)) return -1;
  ret = silk_Get_Encoder_Size(&silkEncSizeBytes, channels);
  if (ret) return -1;
  silkEncSizeBytes = align(silkEncSizeBytes);
  if (!encoder_uses_silk(application)) silkEncSizeBytes = 0;
  if (encoder_uses_celt(application)) celtEncSizeBytes = celt_encoder_get_size(channels);
  base_size = align(sizeof(ref_OpusEncoder));
  if (application == opus_application_silk_only || application == opus_application_celt_only) {
    base_size = align(base_size - 480 * 2 * sizeof(opus_res));
  } else if (channels == 1) base_size = align(base_size - 480 * sizeof(opus_res));
  tot_size = base_size + silkEncSizeBytes + celtEncSizeBytes;
  if (st == nullptr) { return tot_size;
}
  zero_n_items((char *)st, static_cast<std::size_t>(tot_size)); st->silk_enc_offset = base_size;
  st->celt_enc_offset = st->silk_enc_offset + silkEncSizeBytes; st->stream_channels = st->channels = channels;
  st->Fs = Fs;
  if (encoder_uses_silk(application)) { silk_enc = (char *)st + st->silk_enc_offset; ret = silk_InitEncoder(silk_enc, st->channels, &st->silk_mode);
}
  if (ret) return -3;
  st->silk_mode.nChannelsAPI = channels; st->silk_mode.nChannelsInternal = channels;
  st->silk_mode.API_sampleRate = st->Fs; st->silk_mode.maxInternalSampleRate = 16000;
  st->silk_mode.minInternalSampleRate = 8000; st->silk_mode.desiredInternalSampleRate = 16000;
  st->silk_mode.payloadSize_ms = 20; st->silk_mode.bitRate = 25000; st->silk_mode.complexity = 9;
  st->silk_mode.useCBR = 0;
  if (encoder_uses_celt(application)) {
    celt_enc = (CeltEncoderInternal *)((char *)st + st->celt_enc_offset);
    err = celt_encoder_init(celt_enc, Fs, channels);
    if (err != 0) return -3;
    celt_encoder_set_complexity(celt_enc, static_cast<opus_int32>(st->silk_mode.complexity));
}
  st->use_vbr = 1; st->vbr_constraint = 1; st->user_bitrate_bps = -1000; st->bitrate_bps = 3000 + Fs * channels;
  st->application = application;
  st->force_channels = -1000;
  if (encoder_uses_silk(application) && encoder_uses_celt(application)) { st->encoder_buffer = st->Fs / 100; } else { st->encoder_buffer = 0; }
  st->delay_compensation = st->Fs / 250; st->hybrid_stereo_width_Q14 = 1 << 14; st->prev_HB_gain = 1.0f;
  st->variable_HP_smth2_Q15 = ((opus_int32)((opus_uint32)(silk_lin2log(60)) << (8))); st->first = 1; st->mode = opus_mode_hybrid; st->bandwidth = 1105;
  return 0;
}
static unsigned char gen_toc(int mode, int framerate, int bandwidth, int channels) {
  int period = 0;
  for (; framerate < 400; framerate <<= 1, ++period) {}
  unsigned char toc;
  if (mode == opus_mode_silk_only) {
    toc = (unsigned char)(((bandwidth - 1101) << 5) | ((period - 2) << 3));
  } else if (mode == opus_mode_celt_only) {
    const int tmp = std::max(0, bandwidth - 1102);
    toc = (unsigned char)(0x80 | (tmp << 5) | (period << 3));
  } else { toc = (unsigned char)(0x60 | ((bandwidth - 1104) << 4) | ((period - 2) << 3));
}
  toc |= (unsigned char)((channels == 2) << 2);
  return toc;
}
static void silk_biquad_res(const opus_res *in, const opus_int32 *B_Q28, const opus_int32 *A_Q28, opus_val32 *S, opus_res *out, const opus_int32 len, int stride) {
  constexpr float inv28 = 1.f / (1 << 28);
  const opus_val32 A[2] = {A_Q28[0] * inv28, A_Q28[1] * inv28};
  const opus_val32 B[3] = {B_Q28[0] * inv28, B_Q28[1] * inv28, B_Q28[2] * inv28};
  for (int k = 0; k < len; k++) {
    const opus_val32 inval = in[k * stride];
    const opus_val32 vout = S[0] + B[0] * inval;
    S[0] = S[1] - vout * A[0] + B[1] * inval; S[1] = -vout * A[1] + B[2] * inval + 1e-30f; out[k * stride] = vout; }
}
static void hp_cutoff(const opus_res *in, opus_int32 cutoff_Hz, opus_res *out, opus_val32 *hp_mem, int len, int channels, opus_int32 Fs) {
  opus_int32 B_Q28[3], A_Q28[2]; opus_int32 Fc_Q19, r_Q28, r_Q22;
  opus_assume(cutoff_Hz <= 0x7FFFFFFF / ((opus_int32)((1.5 * 3.14159 / 1000) * ((opus_int64)1 << (19)) + 0.5)));
  Fc_Q19 = ((opus_int32)((((opus_int32)((opus_int16)(((opus_int32)((1.5 * 3.14159 / 1000) * ((opus_int64)1 << (19)) + 0.5)))) * (opus_int32)((opus_int16)(cutoff_Hz)))) / (Fs / 1000)));
  opus_assume(Fc_Q19 > 0 && Fc_Q19 < 32768);
  r_Q28 = ((opus_int32)((1.0) * ((opus_int64)1 << (28)) + 0.5)) - ((((opus_int32)((0.92) * ((opus_int64)1 << (9)) + 0.5))) * (Fc_Q19));
  B_Q28[0] = r_Q28; B_Q28[1] = ((opus_int32)((opus_uint32)(-r_Q28) << (1))); B_Q28[2] = r_Q28; r_Q22 = ((r_Q28) >> (6));
  A_Q28[0] = ((opus_int32)(((opus_int64)(r_Q22) * (((opus_int32)(((opus_int64)(Fc_Q19) * (Fc_Q19)) >> 16)) - ((opus_int32)((2.0) * ((opus_int64)1 << (22)) + 0.5)))) >> 16));
  A_Q28[1] = ((opus_int32)(((opus_int64)(r_Q22) * (r_Q22)) >> 16)); silk_biquad_res(in, B_Q28, A_Q28, hp_mem, out, len, channels);
  if (channels == 2) { silk_biquad_res(in + 1, B_Q28, A_Q28, hp_mem + 2, out + 1, len, channels); }
}
static void dc_reject(const opus_val16 *in, opus_int32 cutoff_Hz, opus_val16 *out, opus_val32 *hp_mem, int len, int channels, opus_int32 Fs) {
  const float coef = 6.3f * cutoff_Hz / Fs, coef2 = 1 - coef;
  if (channels == 2) {
    float m0 = hp_mem[0], m2 = hp_mem[2];
    for (int i = 0; i < len; i++) {
      const opus_val32 x0 = in[2*i], x1 = in[2*i+1];
      out[2*i] = x0 - m0; out[2*i+1] = x1 - m2; m0 = coef * x0 + 1e-30f + coef2 * m0; m2 = coef * x1 + 1e-30f + coef2 * m2;
}
    hp_mem[0] = m0; hp_mem[2] = m2;
  } else {
    float m0 = hp_mem[0];
    for (int i = 0; i < len; i++) {
      const opus_val32 x = in[i];
      out[i] = x - m0; m0 = coef * x + 1e-30f + coef2 * m0;
}
    hp_mem[0] = m0; }
}
static void stereo_fade(const opus_res *in, opus_res *out, opus_val16 g1, opus_val16 g2, int overlap48, int frame_size, int channels, const celt_coef *window, opus_int32 Fs) {
  const int inc = std::max(1, (int)(48000 / Fs)), overlap = overlap48 / inc;
  g1 = 1.0f - g1; g2 = 1.0f - g2;
  int i;
  for (i = 0; i < overlap; i++) {
    opus_val16 w = window[i * inc]; w *= w;
    const opus_val32 g = w * g2 + (1.0f - w) * g1;
    const opus_val32 diff = g * .5f * (in[i*channels] - in[i*channels+1]);
    out[i*channels] -= diff; out[i*channels+1] += diff;
}
  for (; i < frame_size; i++) {
    const opus_val32 diff = g2 * .5f * (in[i*channels] - in[i*channels+1]);
    out[i*channels] -= diff; out[i*channels+1] += diff; }
}
static void gain_fade(const opus_res *in, opus_res *out, opus_val16 g1, opus_val16 g2, int overlap48, int frame_size, int channels, const celt_coef *window, opus_int32 Fs) {
  int i, c;
  const int inc = std::max(1, (int)(48000 / Fs)), overlap = overlap48 / inc;
  if (channels == 1) {
    for (i = 0; i < overlap; i++) {
      opus_val16 w = window[i * inc]; w *= w;
      out[i] = (w * g2 + (1.0f - w) * g1) * in[i];
}
  } else {
    for (i = 0; i < overlap; i++) {
      opus_val16 w = window[i * inc]; w *= w;
      const opus_val16 g = w * g2 + (1.0f - w) * g1;
      out[i * 2] = g * in[i * 2]; out[i * 2 + 1] = g * in[i * 2 + 1]; }
}
  for (c = 0; c < channels; ++c) { for (i = overlap; i < frame_size; i++) { out[i * channels + c] = ((g2) * (in[i * channels + c])); } }
}
static opus_int32 user_bitrate_to_bitrate(ref_OpusEncoder *st, int frame_size, int max_data_bytes) {
  opus_int32 max_bitrate, user_bitrate;
  if (!frame_size) frame_size = st->Fs / 400;
  max_bitrate = bits_to_bitrate(max_data_bytes * 8, st->Fs, frame_size);
  if (st->user_bitrate_bps == -1000) user_bitrate = 60 * st->Fs / frame_size + st->Fs * st->channels;
  else if (st->user_bitrate_bps == -1) user_bitrate = 1500000; else user_bitrate = st->user_bitrate_bps;
  return std::min(user_bitrate, max_bitrate);
}
static opus_int32 frame_size_select(int application, opus_int32 frame_size, opus_int32 Fs) {
  if (frame_size < Fs / 400) { return -1;
}
  const auto new_size = frame_size;
  if (new_size < 0 || new_size > frame_size) { return -1;
}
  if (!is_supported_opus_frame_size(Fs, new_size)) { return -1;
}
  if (application == opus_application_silk_only && new_size < Fs / 100) { return -1;
}
  return new_size;
}
static opus_val16 compute_stereo_width(const opus_res *pcm, int frame_size, opus_int32 Fs, StereoWidthState *mem) {
  opus_val32 xx, xy, yy; opus_val16 sqrt_xx, sqrt_yy;
  opus_val16 qrrt_xx, qrrt_yy;
  int frame_rate, i;
  opus_val16 short_alpha; frame_rate = Fs / frame_size;
  short_alpha = ((opus_val32)(25) * (opus_val32)(1.0f)) / std::max(50, frame_rate); xx = xy = yy = 0;
  for (i = 0; i < frame_size - 3; i += 4) {
    opus_val32 pxx = 0, pxy = 0, pyy = 0; opus_val16 x, y;
    x = (pcm[2 * i]); y = (pcm[2 * i + 1]); pxx = (((opus_val32)(x) * (opus_val32)(x))); pxy = (((opus_val32)(x) * (opus_val32)(y)));
    pyy = (((opus_val32)(y) * (opus_val32)(y))); x = (pcm[2 * i + 2]); y = (pcm[2 * i + 3]); pxx += (((opus_val32)(x) * (opus_val32)(x)));
    pxy += (((opus_val32)(x) * (opus_val32)(y))); pyy += (((opus_val32)(y) * (opus_val32)(y))); x = (pcm[2 * i + 4]); y = (pcm[2 * i + 5]);
    pxx += (((opus_val32)(x) * (opus_val32)(x))); pxy += (((opus_val32)(x) * (opus_val32)(y)));
    pyy += (((opus_val32)(y) * (opus_val32)(y))); x = (pcm[2 * i + 6]); y = (pcm[2 * i + 7]); pxx += (((opus_val32)(x) * (opus_val32)(x)));
    pxy += (((opus_val32)(x) * (opus_val32)(y))); pyy += (((opus_val32)(y) * (opus_val32)(y))); xx += (pxx); xy += (pxy); yy += (pyy);
}
  if (!(xx < 1e9f) || ((xx) != (xx)) || !(yy < 1e9f) || ((yy) != (yy))) { xy = xx = yy = 0;
}
  mem->XX += ((short_alpha) * (xx - mem->XX)); mem->XY = ((1.0f - short_alpha) * (mem->XY)) + ((short_alpha) * (xy));
  mem->YY += ((short_alpha) * (yy - mem->YY)); mem->XX = std::max(0.f, mem->XX);
  mem->XY = std::max(0.f, mem->XY); mem->YY = std::max(0.f, mem->YY);
  if (std::max(mem->XX, mem->YY) > (8e-4f)) {
    opus_val16 corr, ldiff, width; sqrt_xx = ((float)sqrt(mem->XX));
    sqrt_yy = ((float)sqrt(mem->YY)); qrrt_xx = ((float)sqrt(sqrt_xx));
    qrrt_yy = ((float)sqrt(sqrt_yy)); mem->XY = std::min(mem->XY, sqrt_xx * sqrt_yy);
    corr = (((float)(mem->XY) / (1e-15f + ((opus_val32)(sqrt_xx) * (opus_val32)(sqrt_yy)))));
    ldiff = ((opus_val32)(1.0f) * (opus_val32)(((float)fabs(qrrt_xx - qrrt_yy)))) / (1e-15f + qrrt_xx + qrrt_yy);
    const opus_val16 decorrelation = (float)sqrt((1.f) - ((opus_val32)(corr) * (opus_val32)(corr)));
    width = std::min(1.0f, decorrelation) * ldiff;
    mem->smoothed_width += (width - mem->smoothed_width) / frame_rate;
    mem->max_follower = ((mem->max_follower - (.02f) / frame_rate) > (mem->smoothed_width) ? (mem->max_follower - (.02f) / frame_rate) : (mem->smoothed_width));
}
  return (((1.0f) < (((opus_val32)(20) * (opus_val32)(mem->max_follower))) ? (1.0f) : (((opus_val32)(20) * (opus_val32)(mem->max_follower)))));
}
static OPUS_NOINLINE int compute_silk_rate_for_hybrid(int rate, int bandwidth, int frame20ms, int vbr, int channels) {
  int i, N, silk_rate;
  const auto rate_index = frame20ms;
  rate /= channels; N = static_cast<int>(hybrid_rate_table.size());
  for (i = 1; i < N; i++) { if (hybrid_rate_table[static_cast<std::size_t>(i)].threshold > rate) break;
}
  if (i == N) {
    silk_rate = hybrid_rate_table[static_cast<std::size_t>(i - 1)].rates[static_cast<std::size_t>(rate_index)];
    silk_rate += (rate - hybrid_rate_table[static_cast<std::size_t>(i - 1)].threshold) / 2;
  } else {
    opus_int32 lo, hi, x0, x1;
    lo = hybrid_rate_table[static_cast<std::size_t>(i - 1)].rates[static_cast<std::size_t>(rate_index)];
    hi = hybrid_rate_table[static_cast<std::size_t>(i)].rates[static_cast<std::size_t>(rate_index)];
    x0 = hybrid_rate_table[static_cast<std::size_t>(i - 1)].threshold; x1 = hybrid_rate_table[static_cast<std::size_t>(i)].threshold;
    silk_rate = (lo * (x1 - rate) + hi * (rate - x0)) / (x1 - x0);
}
  if (!vbr) { silk_rate += 100;
}
  if (bandwidth == 1104) silk_rate += 300;
  silk_rate *= channels;
  if (channels == 2 && rate >= 12000) silk_rate -= 1000;
  return silk_rate;
}
static opus_int32 compute_equiv_rate(opus_int32 bitrate, int channels, int frame_rate, int vbr, int mode, int complexity) {
  opus_int32 equiv = bitrate;
  if (frame_rate > 50) equiv -= (40 * channels + 20) * (frame_rate - 50);
  if (!vbr) equiv -= equiv / 12;
  equiv = equiv * (90 + complexity) / 100;
  if (mode == opus_mode_silk_only || mode == opus_mode_hybrid) {
    if (complexity < 2) equiv = equiv * 4 / 5;
  } else if (mode == opus_mode_celt_only) {
    if (complexity < 5) equiv = equiv * 9 / 10;
}
  return equiv;
}
constexpr auto hybrid_celt_residual_rate_floor_bps = 0;
constexpr auto hybrid_celt_residual_margin_bps = 2000;
constexpr auto hybrid_silk_lowrate_boost_min_bps = 28000;
constexpr auto hybrid_silk_lowrate_boost_max_bps = 36000;
constexpr auto hybrid_silk_lowrate_target_bps = 35000;
constexpr auto hybrid_silk_lowrate_reserve_bps = 2000;

[[nodiscard]] static constexpr opus_int32 hybrid_silk_lowrate_boost_bps(opus_int32 user_bitrate_bps, opus_int32 silk_bitrate_bps) noexcept {
  if (user_bitrate_bps < hybrid_silk_lowrate_boost_min_bps || user_bitrate_bps > hybrid_silk_lowrate_boost_max_bps) return silk_bitrate_bps;
  const auto payload_limit_bps = std::max<opus_int32>(500, user_bitrate_bps - hybrid_silk_lowrate_reserve_bps);
  const auto target_bps = std::min<opus_int32>(hybrid_silk_lowrate_target_bps, payload_limit_bps);
  return std::min<opus_int32>(payload_limit_bps, std::max<opus_int32>(silk_bitrate_bps, target_bps));
}
constexpr opus_int32 audio_clean_hp_cutoff_hz = 3;
constexpr opus_val16 mono_voice_low_band_keep = 0.86f;
static int is_digital_silence(const opus_res *pcm, int frame_size, int channels, int lsb_depth) {
  int silence = 0;
  opus_val32 sample_max = 0; sample_max = celt_maxabs16(pcm, frame_size * channels);
  silence = (sample_max <= (opus_val16)1 / (1 << lsb_depth));
  return silence;
}
static OPUS_NOINLINE opus_val32 compute_frame_energy(const opus_val16 *pcm, int frame_size, int channels) { int len = frame_size * channels; return celt_inner_prod_c(pcm, pcm, len) / len;
}
static int tone_lpc(const opus_val16 *x, int len, int delay, opus_val32 *lpc);
[[nodiscard]] static int audio_music_marker_from_mono(const opus_val16 *mono, int frame_size) {
  if (frame_size < 64) return 0;
  int delay = 1, fail = 1;
  opus_val32 lpc[2]{};
  for (; delay <= 16 && (fail || (lpc[0] > 1.f && lpc[1] < 0)); delay *= 2) fail = tone_lpc(mono, frame_size, delay, lpc);
  return !fail && lpc[0] * lpc[0] + 3.999999f * lpc[1] < 0 && -lpc[1] > .24f;
}
struct audio_content_markers { int speech, music, harmonic_music, pitch_lag; opus_val32 pitch_corr, diff_ratio, zcr, envelope_cv; };
[[nodiscard]] static audio_content_markers detect_audio_content_markers(const opus_res *pcm, int frame_size, int channels) {
  audio_content_markers markers{};
  if (frame_size < 16) return markers;
  auto *mono = OPUS_SCRATCH(opus_val16, static_cast<std::size_t>(frame_size));
  opus_val64 energy = 0, diff_energy = 0;
  int zc = 0;
  opus_val32 prev = 0;
  for (int i = 0; i < frame_size; ++i) {
    opus_val32 sample = 0;
    for (int c = 0; c < channels; ++c) sample += pcm[i * channels + c];
    sample *= 1.f / channels;
    mono[i] = sample;
    energy += static_cast<opus_val64>(sample) * sample;
    if (i > 0) {
      const auto delta = sample - prev;
      diff_energy += static_cast<opus_val64>(delta) * delta;
      zc += (sample >= 0) != (prev >= 0);
    }
    prev = sample;
  }
  if (energy <= 1e-7f * frame_size) return markers;
  const int lpc_music = audio_music_marker_from_mono(mono, frame_size);
  const auto diff_ratio = static_cast<opus_val32>(diff_energy / (energy + 1e-12f));
  const auto zcr = static_cast<opus_val32>(zc) / frame_size;
  markers.diff_ratio = diff_ratio;
  markers.zcr = zcr;
  if (lpc_music) {
    markers.music = 1;
  }

  constexpr int envelope_blocks = 10;
  std::array<opus_val32, envelope_blocks> envelope{};
  std::array<int, envelope_blocks> counts{};
  for (int i = 0; i < frame_size; ++i) {
    const int block = std::min(envelope_blocks - 1, (i * envelope_blocks) / frame_size);
    envelope[block] += std::abs(mono[i]);
    ++counts[block];
  }
  opus_val64 envelope_mean = 0;
  for (int i = 0; i < envelope_blocks; ++i) {
    if (counts[i]) envelope[i] /= counts[i];
    envelope_mean += envelope[i];
  }
  envelope_mean /= envelope_blocks;
  opus_val64 envelope_var = 0;
  for (int i = 0; i < envelope_blocks; ++i) {
    const auto d = envelope[i] - envelope_mean;
    envelope_var += static_cast<opus_val64>(d) * d;
  }
  const auto envelope_cv = static_cast<opus_val32>(std::sqrt(envelope_var / envelope_blocks) / (envelope_mean + 1e-12f));
  markers.envelope_cv = envelope_cv;
  if (!(diff_ratio > .006f && diff_ratio < .12f && zcr > .018f && zcr < .16f && envelope_cv > .08f)) return markers;
  opus_val64 best_num = 0;
  opus_val64 best_den_a = 1e-12, best_den_b = 1e-12;
  int best_lag = 0;
  const int max_lag = std::min(frame_size / 2, 480);
  for (int lag = 48; lag <= max_lag; lag += 8) {
    opus_val64 num = 0, den_a = 0, den_b = 0;
    for (int i = lag; i < frame_size; ++i) {
      const auto sample = mono[i];
      const auto delayed = mono[i - lag];
      num += static_cast<opus_val64>(sample) * delayed;
      den_a += static_cast<opus_val64>(sample) * sample;
      den_b += static_cast<opus_val64>(delayed) * delayed;
    }
    if (num * num * best_den_a * best_den_b > best_num * best_num * den_a * den_b) {
      best_num = num;
      best_den_a = den_a;
      best_den_b = den_b;
      best_lag = lag;
    }
  }
  const auto pitch_corr = static_cast<opus_val32>(best_num / std::sqrt(best_den_a * best_den_b + 1e-18));
  markers.pitch_corr = pitch_corr;
  markers.pitch_lag = best_lag;
  markers.harmonic_music = pitch_corr > .86f && diff_ratio < .018f && zcr < .055f && envelope_cv < .25f;
  markers.speech = pitch_corr > .78f && !markers.harmonic_music;
  return markers;
}
static int update_lightweight_voice_estimate(ref_OpusEncoder *st, const opus_res *pcm, int frame_size, opus_val16 stereo_width) noexcept {
  auto voice_score = st->lightweight_voice_score_Q7;
  auto music_score = st->lightweight_music_score_Q7;
  st->lightweight_analysis_frames = std::min(st->lightweight_analysis_frames + 1, 127);
  if (st->channels == 2 && stereo_width > .05f) {
    st->lightweight_voice_score_Q7 = 0;
    st->lightweight_pitch_stability_Q7 = 0;
    st->lightweight_harmonic_music_Q7 = std::max(st->lightweight_harmonic_music_Q7, 48);
    st->lightweight_prev_pitch_lag = 0;
    music_score += std::max(1, (115 - music_score) >> 3);
    st->lightweight_music_score_Q7 = clamp_value(music_score, 0, 115);
    return 0;
  }
  const auto markers = detect_audio_content_markers(pcm, frame_size, st->channels);
  auto pitch_stability = st->lightweight_pitch_stability_Q7;
  if (markers.pitch_lag > 0 && markers.pitch_corr > .72f) {
    const auto lag_delta = st->lightweight_prev_pitch_lag > 0 ? std::abs(markers.pitch_lag - st->lightweight_prev_pitch_lag) : 999;
    if (lag_delta <= 8) pitch_stability += std::max(1, (115 - pitch_stability) >> 3);
    else pitch_stability = (pitch_stability * 92) >> 7;
    st->lightweight_prev_pitch_lag = markers.pitch_lag;
  } else {
    pitch_stability = (pitch_stability * 80) >> 7;
    st->lightweight_prev_pitch_lag = 0;
  }
  pitch_stability = clamp_value(pitch_stability, 0, 115);
  const bool sustained_harmonic = markers.harmonic_music
      || (pitch_stability > 34 && markers.pitch_corr > .82f && markers.diff_ratio < .025f && markers.zcr < .075f && markers.envelope_cv < .25f);
  auto harmonic_music = st->lightweight_harmonic_music_Q7;
  if (sustained_harmonic) harmonic_music += std::max(1, (115 - harmonic_music) >> 3);
  else harmonic_music = (harmonic_music * 104) >> 7;
  harmonic_music = clamp_value(harmonic_music, 0, 115);
  if (markers.speech && !sustained_harmonic) voice_score += std::max(1, (115 - voice_score) / 5);
  else voice_score = (voice_score * (sustained_harmonic ? 112 : 126)) >> 7;
  if (markers.music || sustained_harmonic) music_score += std::max(1, (115 - music_score) >> 3);
  else music_score = (music_score * 112) >> 7;
  voice_score = clamp_value(voice_score, 0, 115);
  music_score = clamp_value(music_score, 0, 115);
  st->lightweight_voice_score_Q7 = voice_score;
  st->lightweight_music_score_Q7 = music_score;
  st->lightweight_pitch_stability_Q7 = pitch_stability;
  st->lightweight_harmonic_music_Q7 = harmonic_music;
  if (voice_score > 60 && harmonic_music < 80) return 115;
  if (harmonic_music > 52 && music_score >= voice_score + 8) return 0;
  if (voice_score > 44 && voice_score + 16 >= music_score) return 115;
  if (music_score >= voice_score + 8 && music_score > 36) return 0;
  return 48;
}
static int compute_redundancy_bytes(opus_int32 max_data_bytes, opus_int32 bitrate_bps, int frame_rate, int channels) {
  int redundancy_bytes_cap, redundancy_bytes;
  opus_int32 redundancy_rate, available_bits;
  int base_bits = (40 * channels + 20);
  redundancy_rate = bitrate_bps + base_bits * (200 - frame_rate); redundancy_rate = 3 * redundancy_rate / 2;
  redundancy_bytes = redundancy_rate / 1600; available_bits = max_data_bytes * 8 - 2 * base_bits;
  redundancy_bytes_cap = (available_bits * 240 / (240 + 48000 / frame_rate) + base_bits) / 8;
  redundancy_bytes = std::min(redundancy_bytes, redundancy_bytes_cap);
  if (redundancy_bytes > 4 + 8 * channels) redundancy_bytes = std::min(257, redundancy_bytes);
  else redundancy_bytes = 0;
  return redundancy_bytes;
}
[[nodiscard]] constexpr auto encoder_delay_compensation(const ref_OpusEncoder *st) noexcept -> int {
  return (st->application == opus_application_restricted_lowdelay || st->application == opus_application_celt_only || st->application == opus_application_silk_only) ? 0 : st->delay_compensation;
}
namespace {
struct encoder_stage_storage {
  std::span<opus_res> storage;
  int active_window, encoder_buffer, total_buffer, frame_size, channels;
  [[nodiscard]] auto window_sample_count() const noexcept -> std::size_t { return static_cast<std::size_t>((encoder_buffer + frame_size) * channels);
}
  [[nodiscard]] auto history_sample_count() const noexcept -> std::size_t { return static_cast<std::size_t>(encoder_buffer * channels);
}
  [[nodiscard]] auto frame_sample_count() const noexcept -> std::size_t { return static_cast<std::size_t>(frame_size * channels);
}
  [[nodiscard]] auto celt_window_offset() const noexcept -> std::size_t { return static_cast<std::size_t>((encoder_buffer - total_buffer) * channels);
}
  [[nodiscard]] auto celt_window_sample_count() const noexcept -> std::size_t { return static_cast<std::size_t>((total_buffer + frame_size) * channels);
}
  [[nodiscard]] auto window(const int index) noexcept -> std::span<opus_res> { return storage.subspan(static_cast<std::size_t>(index) * window_sample_count(), window_sample_count());
}
  [[nodiscard]] auto window(const int index) const noexcept -> std::span<const opus_res> { return storage.subspan(static_cast<std::size_t>(index) * window_sample_count(), window_sample_count());
}
  [[nodiscard]] auto active() noexcept -> std::span<opus_res> { return window(active_window);
}
  [[nodiscard]] auto inactive() noexcept -> std::span<opus_res> { return window(active_window ^ 1);
}
  [[nodiscard]] auto active() const noexcept -> std::span<const opus_res> { return window(active_window);
}
  [[nodiscard]] auto inactive() const noexcept -> std::span<const opus_res> { return window(active_window ^ 1);
}
  [[nodiscard]] auto active_history() noexcept -> std::span<opus_res> { return active().first(history_sample_count());
}
  [[nodiscard]] auto next_history() noexcept -> std::span<opus_res> { return inactive().first(history_sample_count());
}
  [[nodiscard]] auto next_history() const noexcept -> std::span<const opus_res> { return inactive().first(history_sample_count());
}
  [[nodiscard]] auto active_frame() noexcept -> std::span<opus_res> { return active().subspan(history_sample_count(), frame_sample_count());
}
  [[nodiscard]] auto active_celt_window() noexcept -> std::span<opus_res> { return active().subspan(celt_window_offset(), celt_window_sample_count());
}
  [[nodiscard]] auto active_celt_prefill(const ref_OpusEncoder *st) noexcept -> std::span<opus_res> { return active().subspan(static_cast<std::size_t>((encoder_buffer - total_buffer - st->Fs / 400) * channels), static_cast<std::size_t>(st->channels * st->Fs / 400));
}
  auto prime_from_encoder(const ref_OpusEncoder *st) noexcept -> void {
    if (history_sample_count() == 0) { return;
}
    copy_n_items(st->delay_buffer, history_sample_count(), active_history().data());
}
  auto snapshot_next_history() noexcept -> void {
    if (history_sample_count() == 0) { return;
}
    copy_n_items(active().data() + frame_sample_count(), history_sample_count(), next_history().data());
}
  auto commit_to_encoder(ref_OpusEncoder *st) const noexcept -> void {
    if (history_sample_count() == 0) { return;
}
    copy_n_items(next_history().data(), history_sample_count(), st->delay_buffer);
}
  auto advance() noexcept -> void { active_window ^= 1;
}
};
[[nodiscard]] constexpr auto encoder_stage_storage_size(const ref_OpusEncoder *st, const int frame_size) noexcept -> std::size_t {
  const auto window_samples = static_cast<std::size_t>((st->encoder_buffer + frame_size) * st->channels);
  return window_samples * 2;
}
[[nodiscard]] inline auto make_encoder_stage_storage(opus_res *storage, const ref_OpusEncoder *st, const int total_buffer, const int frame_size) noexcept -> encoder_stage_storage {
  return {{storage, encoder_stage_storage_size(st, frame_size)}, 0, st->encoder_buffer, total_buffer, frame_size, st->channels};
}
} // namespace
[[nodiscard]] constexpr auto multiframe_encoder_frame_size(const ref_OpusEncoder *st, const int frame_size) noexcept -> int {
  if (st->mode != opus_mode_silk_only) { return st->Fs / 50;
}
  if (frame_size == 2 * st->Fs / 25) { return st->Fs / 25;
}
  if (frame_size == 3 * st->Fs / 25) { return 3 * st->Fs / 50;
}
  return st->Fs / 50;
}
[[nodiscard]] static OPUS_COLD OPUS_NOINLINE auto encode_low_rate_packet(ref_OpusEncoder *st, const int frame_size, const opus_int32 out_data_bytes, opus_int32 max_data_bytes, unsigned char *data) -> opus_int32 {
  int tocmode = st->mode;
  int frame_rate = st->Fs / frame_size;
  int bw = st->bandwidth == 0 ? 1101 : st->bandwidth, packet_code = 0, num_multiframes = 0;
  if (tocmode == 0) { tocmode = opus_mode_silk_only;
}
  if (frame_rate > 100) { tocmode = opus_mode_celt_only;
}
  if (frame_rate == 25 && tocmode != opus_mode_silk_only) { frame_rate = 50; packet_code = 1;
}
  if (frame_rate <= 16) {
    if (out_data_bytes == 1 || (tocmode == opus_mode_silk_only && frame_rate != 10)) {
      tocmode = opus_mode_silk_only; packet_code = frame_rate <= 12; frame_rate = frame_rate == 12 ? 25 : 16;
    } else { num_multiframes = 50 / frame_rate; frame_rate = 50; packet_code = 3; }
}
  if (tocmode == opus_mode_silk_only && bw > 1103) bw = 1103;
  else if (tocmode == opus_mode_celt_only && bw == 1102) bw = 1101; else if (tocmode == opus_mode_hybrid && bw <= 1104) bw = 1104;
  data[0] = gen_toc(tocmode, frame_rate, bw, st->stream_channels); data[0] |= packet_code;
  int ret = packet_code <= 1 ? 1 : 2;
  max_data_bytes = std::max(max_data_bytes, ret);
  if (packet_code == 3) { data[1] = num_multiframes;
}
  if (!st->use_vbr) {
    ret = pad_packet(data, ret, max_data_bytes, 1);
    if (ret == 0) ret = max_data_bytes;
    else ret = -3;
}
  return ret;
}
struct multiframe_encode_params final {
  int float_api, lsb_depth;
  int redundancy, celt_to_silk, to_celt, prefill;
  opus_int32 equiv_rate, cbr_bytes;
};
static OPUS_ENCODER_HUB_SIZE_OPT opus_int32 opus_encode_frame_native(ref_OpusEncoder *st, const opus_res *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes, int float_api, int is_silence, int redundancy, int celt_to_silk, int prefill, opus_int32 equiv_rate, int to_celt, encoder_stage_storage &stage_storage);
[[nodiscard]] static OPUS_COLD OPUS_NOINLINE auto encode_multiframe_packet(ref_OpusEncoder *st, const opus_res *pcm, const int frame_size, unsigned char *data, const opus_int32 out_data_bytes, const multiframe_encode_params &params) -> opus_int32 {
  const int enc_frame_size = multiframe_encoder_frame_size(st, frame_size);
  const int nb_frames = frame_size / enc_frame_size;
  const int max_header_bytes = nb_frames == 2 ? 3 : (2 + (nb_frames - 1) * 2);
  const auto total_buffer = encoder_delay_compensation(st);
  auto *stage_buffer = OPUS_SCRATCH(opus_res, encoder_stage_storage_size(st, enc_frame_size));
  auto stage_storage = make_encoder_stage_storage(stage_buffer, st, total_buffer, enc_frame_size);
  opus_int32 tot_size = 0;
  int dtx_count = 0;
  stage_storage.prime_from_encoder(st);
  const opus_int32 repacketize_len = (st->use_vbr || st->user_bitrate_bps == -1) ? out_data_bytes : std::min(params.cbr_bytes, out_data_bytes);
  const opus_int32 max_len_sum = nb_frames + repacketize_len - max_header_bytes;
  auto *curr_data = OPUS_SCRATCH(unsigned char, max_len_sum);
  packet_frame_set packet_frames{};
  const int bak_to_mono = st->silk_mode.toMono;
  if (bak_to_mono) st->force_channels = 1;
  else st->prev_channels = st->stream_channels;
  for (int frame_index = 0; frame_index < nb_frames; ++frame_index) {
    st->silk_mode.toMono = 0; st->nonfinal_frame = frame_index < (nb_frames - 1);
    const int frame_to_celt = params.to_celt && frame_index == nb_frames - 1;
    const int frame_redundancy = params.redundancy && (frame_to_celt || (!params.to_celt && frame_index == 0));
    opus_int32 curr_max = ((bitrate_to_bits(st->bitrate_bps, st->Fs, enc_frame_size) / 8) < (max_len_sum / nb_frames) ? (bitrate_to_bits(st->bitrate_bps, st->Fs, enc_frame_size) / 8) : (max_len_sum / nb_frames));
    curr_max = std::min(max_len_sum - tot_size, curr_max);
    const auto *frame_pcm = pcm + frame_index * (st->channels * enc_frame_size);
    const int frame_is_silence = is_digital_silence(frame_pcm, enc_frame_size, st->channels, params.lsb_depth);
    const int tmp_len = opus_encode_frame_native(st, frame_pcm, enc_frame_size, curr_data, curr_max, params.float_api, frame_is_silence, frame_redundancy, params.celt_to_silk, params.prefill, params.equiv_rate, frame_to_celt, stage_storage);
    if (tmp_len < 0) {
      stage_storage.commit_to_encoder(st); st->silk_mode.toMono = bak_to_mono;
      return -3;
}
    if (tmp_len == 1) { ++dtx_count;
}
    const int append_ret = append_packet_frames(&packet_frames, curr_data, tmp_len, 0);
    if (append_ret < 0) {
      stage_storage.commit_to_encoder(st); st->silk_mode.toMono = bak_to_mono;
      return -3;
}
    tot_size += tmp_len; curr_data += tmp_len;
    if (frame_index + 1 < nb_frames) { stage_storage.advance(); }
}
  stage_storage.commit_to_encoder(st);
  const int ret = write_packet_frames(&packet_frames, 0, nb_frames, data, repacketize_len, 0, !st->use_vbr && (dtx_count != nb_frames));
  st->silk_mode.toMono = bak_to_mono;
  return ret < 0 ? -3 : ret;
}
static OPUS_ENCODER_HUB_SIZE_OPT opus_int32 encode_native(ref_OpusEncoder *st, const opus_res *pcm, int frame_size, unsigned char *data, opus_int32 out_data_bytes, int lsb_depth, int float_api) {
  void *silk_enc = nullptr;
  CeltEncoderInternal *celt_enc = nullptr;
  int ret = 0, prefill = 0, redundancy = 0, celt_to_silk = 0, to_celt = 0;
  int voice_est, frame_rate, curr_bandwidth;
  opus_int32 equiv_rate, max_rate, max_data_bytes, cbr_bytes = -1; opus_val16 stereo_width;
  int packet_size_cap = 1276;
  int is_silence = 0;
  max_data_bytes = std::min(packet_size_cap * 6, out_data_bytes); st->rangeFinal = 0;
  if (frame_size <= 0 || max_data_bytes <= 0) { return -1;
}
  if (max_data_bytes == 1 && st->Fs == (frame_size * 10)) { return -2;
}
  if (encoder_uses_silk(st->application)) silk_enc = (char *)st + st->silk_enc_offset;
  if (encoder_uses_celt(st->application)) celt_enc = (CeltEncoderInternal *)((char *)st + st->celt_enc_offset);
  is_silence = is_digital_silence(pcm, frame_size, st->channels, lsb_depth);
  if (!is_silence) { st->peak_signal_energy = std::max<opus_val32>(0.999f * st->peak_signal_energy, compute_frame_energy(pcm, frame_size, st->channels));
}
  if (st->channels == 2 && st->force_channels != 1) { stereo_width = compute_stereo_width(pcm, frame_size, st->Fs, &st->width_mem); } else { stereo_width = 0;
}
  st->bitrate_bps = user_bitrate_to_bitrate(st, frame_size, max_data_bytes);
  const bool voip_style = st->application == opus_application_voip;
  frame_rate = st->Fs / frame_size;
  if (!st->use_vbr) {
    cbr_bytes = (((bitrate_to_bits(st->bitrate_bps, st->Fs, frame_size) + 4) / 8) < (max_data_bytes) ? ((bitrate_to_bits(st->bitrate_bps, st->Fs, frame_size) + 4) / 8) : (max_data_bytes));
    st->bitrate_bps = bits_to_bitrate(cbr_bytes * 8, st->Fs, frame_size); max_data_bytes = std::max(1, cbr_bytes);
}
  if (max_data_bytes < 3 || st->bitrate_bps < 3 * frame_rate * 8 || (frame_rate < 50 && (max_data_bytes * (opus_int32)frame_rate < 300 || st->bitrate_bps < 2400))) {
    return encode_low_rate_packet(st, frame_size, out_data_bytes, max_data_bytes, data);
}
  max_rate = bits_to_bitrate(max_data_bytes * 8, st->Fs, frame_size);
  equiv_rate = compute_equiv_rate(st->bitrate_bps, st->channels, st->Fs / frame_size, st->use_vbr, 0, st->silk_mode.complexity);
  if (st->application == opus_application_voip || st->application == opus_application_audio) {
    voice_est = update_lightweight_voice_estimate(st, pcm, frame_size, stereo_width);
    if (voip_style) voice_est = voice_est > 48 ? 115 : 0;
  } else voice_est = 48;
  if (st->force_channels != -1000 && st->channels == 2) {
    st->stream_channels = st->force_channels;
  } else {
    if (st->channels == 2) {
      opus_int32 stereo_threshold = stereo_music_threshold + ((voice_est * voice_est * (stereo_voice_threshold - stereo_music_threshold)) >> 14);
      if (stereo_width < .015f && equiv_rate < 32000) st->stream_channels = 1;
      else {
        if (st->stream_channels == 2) { stereo_threshold -= 1000; } else { stereo_threshold += 1000; }
        st->stream_channels = (equiv_rate > stereo_threshold) ? 2 : 1;
      }
    } else { st->stream_channels = st->channels; }
}
  equiv_rate = compute_equiv_rate(st->bitrate_bps, st->stream_channels, st->Fs / frame_size, st->use_vbr, 0, st->silk_mode.complexity);
  auto threshold = opus_int32{0};
  if (st->application == opus_application_silk_only) {
    st->mode = opus_mode_silk_only;
  } else if (st->application == opus_application_restricted_lowdelay || st->application == opus_application_celt_only) {
    st->mode = opus_mode_celt_only;
  } else {
    opus_int32 mode_voice, mode_music;
    mode_voice = (opus_int32)(((1.0f - stereo_width) * (mode_thresholds[0][0])) + ((stereo_width) * (mode_thresholds[1][0])));
    mode_music = (opus_int32)(((1.0f - stereo_width) * (mode_thresholds[1][1])) + ((stereo_width) * (mode_thresholds[1][1])));
    threshold = mode_music + ((voice_est * voice_est * (mode_voice - mode_music)) >> 14);
    if (voip_style) threshold += 8000;
    if (st->prev_mode == opus_mode_celt_only) { threshold -= 4000; } else if (st->prev_mode > 0) { threshold += 4000; }
    threshold = std::max<opus_int32>(threshold, voip_style ? 23000 : 15000);
    st->mode = (equiv_rate >= threshold) ? opus_mode_celt_only : opus_mode_silk_only;
    if (voice_est <= 16 && st->lightweight_analysis_frames >= 12) {
      const opus_int32 music_celt_switch_bps = voip_style ? 23000 : 15000;
      st->mode = st->bitrate_bps >= music_celt_switch_bps ? opus_mode_celt_only : opus_mode_silk_only;
    } else if (voice_est >= 100) {
      const opus_int32 speech_celt_switch_bps = voip_style ? 68000 : 54000;
      if (st->bitrate_bps < speech_celt_switch_bps) st->mode = opus_mode_silk_only;
    }
    if (max_data_bytes < bitrate_to_bits(frame_rate > 50 ? 9000 : 6000, st->Fs, frame_size) / 8) {
      st->mode = opus_mode_celt_only;
}
}
  if (st->mode != opus_mode_celt_only && frame_size < st->Fs / 100) {
    opus_assume(st->application != opus_application_silk_only); st->mode = opus_mode_celt_only;
}
  if (st->prev_mode > 0 && ((st->mode != opus_mode_celt_only && st->prev_mode == opus_mode_celt_only) || (st->mode == opus_mode_celt_only && st->prev_mode != opus_mode_celt_only))) {
    redundancy = 1; celt_to_silk = (st->mode != opus_mode_celt_only);
    if (!celt_to_silk) {
      if (frame_size >= st->Fs / 100) {
        st->mode = st->prev_mode; to_celt = 1;
      } else { redundancy = 0; } }
}
  if (st->stream_channels == 1 && st->prev_channels == 2 && st->silk_mode.toMono == 0 && st->mode != opus_mode_celt_only && st->prev_mode != opus_mode_celt_only) {
    st->silk_mode.toMono = 1; st->stream_channels = 2;
  } else { st->silk_mode.toMono = 0;
}
  equiv_rate = compute_equiv_rate(st->bitrate_bps, st->stream_channels, st->Fs / frame_size, st->use_vbr, st->mode, st->silk_mode.complexity);
  if (st->mode != opus_mode_celt_only && st->prev_mode == opus_mode_celt_only) { silk_EncControlStruct dummy; silk_InitEncoder(silk_enc, st->channels, &dummy); prefill = 1;
}
  if (st->mode == opus_mode_celt_only || st->first || st->silk_mode.allowBandwidthSwitch) {
    std::array<opus_int32, 8> bandwidth_thresholds;
    int bandwidth = 1105;
    const auto voice_bandwidth_thresholds = std::span<const opus_int32>{voice_bandwidth_thresholds_common};
    const auto music_bandwidth_thresholds = std::span<const opus_int32>{music_bandwidth_thresholds_common};
    for (int threshold_index = 0; threshold_index < 8; ++threshold_index) bandwidth_thresholds[threshold_index] = music_bandwidth_thresholds[threshold_index] + ((voice_est * voice_est * (voice_bandwidth_thresholds[threshold_index] - music_bandwidth_thresholds[threshold_index])) >> 14);
    for (; bandwidth > 1101; --bandwidth) {
      int threshold, hysteresis;
      threshold = bandwidth_thresholds[2 * (bandwidth - 1102)]; hysteresis = bandwidth_thresholds[2 * (bandwidth - 1102) + 1];
      if (!st->first) { if (st->auto_bandwidth >= bandwidth) threshold -= hysteresis; else threshold += hysteresis;
}
      if (equiv_rate >= threshold) break;
}
    if (bandwidth == 1102) bandwidth = 1103;
    st->bandwidth = st->auto_bandwidth = bandwidth;
    if (!st->first && st->mode != opus_mode_celt_only && !st->silk_mode.inWBmodeWithoutVariableLP && st->bandwidth > 1103) st->bandwidth = 1103;
}
  if (st->mode != opus_mode_celt_only && max_rate < 15000) { st->bandwidth = std::min(st->bandwidth, 1103);
}
  if (st->Fs <= 24000 && st->bandwidth > 1104) st->bandwidth = 1104;
  if (st->Fs <= 16000 && st->bandwidth > 1103) st->bandwidth = 1103;
  if (st->Fs <= 12000 && st->bandwidth > 1102) st->bandwidth = 1102;
  if (st->Fs <= 8000 && st->bandwidth > 1101) st->bandwidth = 1101;
  if (st->mode != opus_mode_celt_only && st->Fs >= 24000 && (st->application == opus_application_voip || st->application == opus_application_audio)) {
    const opus_int32 hybrid_floor_bps = voice_est >= 100 ? 14000 : st->channels == 2 ? 12000 : 13000;
    if (st->bitrate_bps < hybrid_floor_bps) st->bandwidth = std::min(st->bandwidth, 1103);
    else if (st->bandwidth <= 1103) st->bandwidth = 1104;
    const opus_int32 hybrid_fullband_floor_bps = st->stream_channels == 2 ? 32000 : 24000;
    if (st->bitrate_bps < hybrid_fullband_floor_bps) st->bandwidth = std::min(st->bandwidth, 1104);
  }
  if (encoder_uses_celt(st->application)) celt_encoder_set_lsb_depth(celt_enc, static_cast<opus_int32>(lsb_depth));
  if (st->mode == opus_mode_celt_only && st->bandwidth == 1102) st->bandwidth = 1103;
  curr_bandwidth = st->bandwidth;
  if (st->application == opus_application_silk_only && curr_bandwidth > 1103) st->bandwidth = curr_bandwidth = 1103;
  if (st->mode == opus_mode_silk_only && curr_bandwidth > 1103) st->mode = opus_mode_hybrid;
  if (st->mode == opus_mode_hybrid && curr_bandwidth <= 1103) st->mode = opus_mode_silk_only;
  if ((frame_size > st->Fs / 50 && (st->mode != opus_mode_silk_only)) || frame_size > 3 * st->Fs / 50) {
    opus_assume(st->use_vbr || st->user_bitrate_bps == -1 || cbr_bytes >= 0);
    return encode_multiframe_packet(st, pcm, frame_size, data, out_data_bytes, {float_api, lsb_depth, redundancy, celt_to_silk, to_celt, prefill, equiv_rate, cbr_bytes});
  } else {
    auto *stage_buffer = OPUS_SCRATCH(opus_res, encoder_stage_storage_size(st, frame_size));
    auto stage_storage = make_encoder_stage_storage(stage_buffer, st, encoder_delay_compensation(st), frame_size);
    stage_storage.prime_from_encoder(st);
    ret = opus_encode_frame_native(st, pcm, frame_size, data, max_data_bytes, float_api, is_silence, redundancy, celt_to_silk, prefill, equiv_rate, to_celt, stage_storage);
    stage_storage.commit_to_encoder(st);
    return ret; }
}
static OPUS_NOINLINE void opus_prepare_frame_highpass(ref_OpusEncoder *st, void *silk_enc, const opus_res *pcm, opus_res *frame_pcm, int frame_size) {
  const int hp_freq_smth1 = st->mode == opus_mode_celt_only ? ((opus_int32)((opus_uint32)(silk_lin2log(60)) << (8))) : ((silk_encoder *)silk_enc)->state_Fxx[0].sCmn.variable_HP_smth1_Q15;
  st->variable_HP_smth2_Q15 = ((opus_int32)((st->variable_HP_smth2_Q15) + (((hp_freq_smth1 - st->variable_HP_smth2_Q15) * (opus_int64)((opus_int16)(((opus_int32)((0.015f) * ((opus_int64)1 << (16)) + 0.5))))) >> 16)));
  const int cutoff_Hz = silk_log2lin(((st->variable_HP_smth2_Q15) >> (8)));
  if (st->application == opus_application_voip) {
    hp_cutoff(pcm, cutoff_Hz, frame_pcm, st->hp_mem, frame_size, st->channels, st->Fs);
  } else if (st->application == opus_application_audio) {
    hp_cutoff(pcm, audio_clean_hp_cutoff_hz, frame_pcm, st->hp_mem, frame_size, st->channels, st->Fs);
    if (st->channels == 1 && st->bitrate_bps >= 20000 && st->bitrate_bps <= 36000) {
      for (int i = 0; i < frame_size; ++i) frame_pcm[i] += mono_voice_low_band_keep * (pcm[i] - frame_pcm[i]);
    }
  } else {
    dc_reject(pcm, 3, frame_pcm, st->hp_mem, frame_size, st->channels, st->Fs);
  }
}
static OPUS_ENCODER_HUB_SIZE_OPT opus_int32 opus_encode_frame_native(ref_OpusEncoder *st, const opus_res *pcm, int frame_size, unsigned char *data, opus_int32 orig_max_data_bytes, int float_api, int is_silence, int redundancy, int celt_to_silk, int prefill, opus_int32 equiv_rate, int to_celt, encoder_stage_storage &stage_storage) {
  void *silk_enc = nullptr;
  CeltEncoderInternal *celt_enc = nullptr;
  const CeltModeInternal *celt_mode = nullptr;
  const bool uses_silk = encoder_uses_silk(st->application), uses_celt = encoder_uses_celt(st->application);
  int ret = 0, max_data_bytes, bits_target, start_band = 0, redundancy_bytes = 0, nb_compr_bytes, apply_padding, frame_rate, curr_bandwidth, activity = -1;
  opus_int32 nBytes = 0;
#if defined(OPUS_CODEC_TRACE_RATE)
  opus_int32 trace_silk_bytes = 0;
#endif
  ec_enc enc;
  opus_uint32 redundant_rng = 0;
  opus_val16 HB_gain;
  auto celt_pcm = stage_storage.active_celt_window();
  auto frame_pcm = stage_storage.active_frame();
  max_data_bytes = ((orig_max_data_bytes) < (1276) ? (orig_max_data_bytes) : (1276));
  st->rangeFinal = 0;
  if (uses_silk) silk_enc = (char *)st + st->silk_enc_offset;
  if (uses_celt) { celt_enc = (CeltEncoderInternal *)((char *)st + st->celt_enc_offset); celt_mode = celt_encoder_mode(celt_enc);
}
  auto celt_set_start = [&](opus_int32 value) { return celt_encoder_set_start_band(celt_enc, value); };
  auto celt_set_end = [&](opus_int32 value) { return celt_encoder_set_end_band(celt_enc, value); };
  auto celt_set_stream_channels = [&](opus_int32 value) { return celt_encoder_set_stream_channels(celt_enc, value); };
  auto celt_set_bitrate = [&](opus_int32 value) { return celt_encoder_set_bitrate(celt_enc, value); };
  auto celt_set_prediction = [&](opus_int32 value) { return celt_encoder_set_prediction(celt_enc, value); };
  auto celt_reset = [&] { celt_encoder_reset_state(celt_enc); return OPUS_OK; };
  auto refresh_redundancy = [&] {
    redundancy_bytes = compute_redundancy_bytes(max_data_bytes, st->bitrate_bps, frame_rate, st->stream_channels);
    redundancy = redundancy_bytes != 0;
  };
  auto celt_final_range = [&](opus_uint32 &rng) { rng = celt_encoder_final_range(celt_enc); };
  auto configure_redundant_celt = [&](int band, int pred) {
    celt_set_start(static_cast<opus_int32>(band));
    if (pred >= 0) celt_set_prediction(static_cast<opus_int32>(pred));
    celt_encoder_set_vbr(celt_enc, 0); celt_set_bitrate(-1);
  };
  curr_bandwidth = st->bandwidth;
  frame_rate = st->Fs / frame_size;
  if (is_silence) { activity = !is_silence;
}
  else if (st->mode == opus_mode_celt_only) { opus_val32 noise_energy = compute_frame_energy(pcm, frame_size, st->channels); activity = st->peak_signal_energy < ((316.23f) * (opus_val64)(.5f * (noise_energy)));
}
  if (st->silk_bw_switch) { redundancy = 1; celt_to_silk = 1; st->silk_bw_switch = 0; prefill = 2;
}
  if (st->mode == opus_mode_celt_only) redundancy = 0;
  if (redundancy) refresh_redundancy();
  if (!uses_celt) { redundancy = 0; redundancy_bytes = 0;
}
  bits_target = ((8 * (max_data_bytes - redundancy_bytes)) < (bitrate_to_bits(st->bitrate_bps, st->Fs, frame_size)) ? (8 * (max_data_bytes - redundancy_bytes)) : (bitrate_to_bits(st->bitrate_bps, st->Fs, frame_size))) - 8;
  data += 1;
  ec_enc_init(&enc, data, orig_max_data_bytes - 1);
  opus_prepare_frame_highpass(st, silk_enc, pcm, frame_pcm.data(), frame_size);
  if (float_api) {
    opus_val32 sum = celt_inner_prod_c(frame_pcm.data(), frame_pcm.data(), frame_size * st->channels);
    if (!(sum < 1e9f) || ((sum) != (sum))) { zero_n_items(frame_pcm.data(), static_cast<std::size_t>(frame_size * st->channels)); st->hp_mem[0] = st->hp_mem[1] = st->hp_mem[2] = st->hp_mem[3] = 0; }
}
  HB_gain = 1.0f;
  if (st->mode != opus_mode_celt_only) {
    opus_int32 total_bitRate, celt_rate;
    const opus_res *pcm_silk;
    total_bitRate = bits_to_bitrate(bits_target, st->Fs, frame_size);
    if (st->mode == opus_mode_hybrid) {
      st->silk_mode.bitRate = compute_silk_rate_for_hybrid(total_bitRate, curr_bandwidth, st->Fs == 50 * frame_size, st->use_vbr, st->stream_channels);
      if (st->use_vbr) st->silk_mode.bitRate = hybrid_silk_lowrate_boost_bps(st->bitrate_bps, st->silk_mode.bitRate);
      celt_rate = total_bitRate - st->silk_mode.bitRate; HB_gain = 1.0f - (((float)exp(0.6931471805599453094 * (-celt_rate * (1.f / 1024)))));
    } else { st->silk_mode.bitRate = total_bitRate;
}
    st->silk_mode.payloadSize_ms = 1000 * frame_size / st->Fs; st->silk_mode.nChannelsAPI = st->channels;
    st->silk_mode.nChannelsInternal = st->stream_channels; st->silk_mode.desiredInternalSampleRate = curr_bandwidth == 1101 ? 8000 : curr_bandwidth == 1102 ? 12000 : 16000;
    st->silk_mode.minInternalSampleRate = st->mode == opus_mode_hybrid ? 16000 : 8000; st->silk_mode.maxInternalSampleRate = 16000;
    if (st->mode == opus_mode_silk_only) {
      opus_int32 effective_max_rate = bits_to_bitrate(max_data_bytes * 8, st->Fs, frame_size);
      if (frame_rate > 50) effective_max_rate = effective_max_rate * 2 / 3;
      st->silk_mode.maxInternalSampleRate = effective_max_rate < 7000 ? 8000 : effective_max_rate < 8000 ? 12000 : 16000;
      st->silk_mode.desiredInternalSampleRate = std::min(st->silk_mode.desiredInternalSampleRate, st->silk_mode.maxInternalSampleRate);
}
    st->silk_mode.useCBR = !st->use_vbr;
    st->silk_mode.maxBits = (max_data_bytes - 1) * 8;
    if (redundancy && redundancy_bytes >= 2) { st->silk_mode.maxBits -= redundancy_bytes * 8 + 1; if (st->mode == opus_mode_hybrid) st->silk_mode.maxBits -= 20;
}
    if (st->silk_mode.useCBR) { if (st->mode == opus_mode_hybrid) {
      opus_int16 other_bits = ((0) > (st->silk_mode.maxBits - st->silk_mode.bitRate * frame_size / st->Fs) ? (0) : (st->silk_mode.maxBits - st->silk_mode.bitRate * frame_size / st->Fs));
      st->silk_mode.maxBits = ((0) > (st->silk_mode.maxBits - other_bits * 3 / 4) ? (0) : (st->silk_mode.maxBits - other_bits * 3 / 4));
      st->silk_mode.useCBR = 0;
    } } else if (st->mode == opus_mode_hybrid) {
      opus_int32 maxBitRate = compute_silk_rate_for_hybrid(st->silk_mode.maxBits * st->Fs / frame_size, curr_bandwidth, st->Fs == 50 * frame_size, st->use_vbr, st->stream_channels);
      st->silk_mode.maxBits = bitrate_to_bits(maxBitRate, st->Fs, frame_size);
}
    if (prefill && uses_celt) {
      opus_int32 zero = 0;
      int prefill_offset = st->channels * (st->encoder_buffer - st->delay_compensation - st->Fs / 400);
      gain_fade(stage_storage.active().data() + prefill_offset, stage_storage.active().data() + prefill_offset, 0, 1.0f, celt_mode->overlap, st->Fs / 400, st->channels, celt_mode->window, st->Fs);
      zero_n_items(stage_storage.active().data(), static_cast<std::size_t>(prefill_offset));
      pcm_silk = stage_storage.active().data();
      silk_Encode(silk_enc, &st->silk_mode, pcm_silk, st->encoder_buffer, nullptr, &zero, prefill, activity);
      st->silk_mode.opusCanSwitch = 0;
}
    pcm_silk = frame_pcm.data();
    ret = silk_Encode(silk_enc, &st->silk_mode, pcm_silk, frame_size, &enc, &nBytes, 0, activity);
#if defined(OPUS_CODEC_TRACE_RATE)
    trace_silk_bytes = nBytes;
#endif
    if (ret) { return -3;
}
    if (st->mode == opus_mode_silk_only) curr_bandwidth = st->silk_mode.internalSampleRate == 8000 ? 1101 : st->silk_mode.internalSampleRate == 12000 ? 1102 : 1103;
    else opus_assume(st->silk_mode.internalSampleRate == 16000);
    st->silk_mode.opusCanSwitch = st->silk_mode.switchReady && !st->nonfinal_frame;
    if (activity == -1) activity = (st->silk_mode.signalType != 0);
    if (nBytes == 0) { st->rangeFinal = 0; data[-1] = gen_toc(st->mode, st->Fs / frame_size, curr_bandwidth, st->stream_channels); return 1;
}
    if (st->silk_mode.opusCanSwitch) {
      if (uses_celt) refresh_redundancy();
      celt_to_silk = 0;
      st->silk_bw_switch = 1; }
}
  if (uses_celt) { const auto endband = bandwidth_to_endband(curr_bandwidth); celt_set_end(static_cast<opus_int32>(endband)); celt_set_stream_channels(static_cast<opus_int32>(st->stream_channels)); celt_set_bitrate(-1);
}
  if (st->mode != opus_mode_silk_only) {
    opus_val32 celt_pred = 2;
    celt_set_prediction(static_cast<opus_int32>(celt_pred));
}
  auto transition_prefill = std::span<opus_res>{};
  if (st->mode != opus_mode_silk_only && st->mode != st->prev_mode && st->prev_mode > 0 && uses_silk) transition_prefill = stage_storage.active_celt_prefill(st);
  stage_storage.snapshot_next_history();
  if ((st->prev_HB_gain < 1.0f || HB_gain < 1.0f) && celt_mode != nullptr) gain_fade(celt_pcm.data(), celt_pcm.data(), st->prev_HB_gain, HB_gain, celt_mode->overlap, frame_size, st->channels, celt_mode->window, st->Fs);
  st->prev_HB_gain = HB_gain;
  if (st->mode != opus_mode_hybrid || st->stream_channels == 1) st->silk_mode.stereoWidth_Q14 = equiv_rate > 32000 ? 16384 : equiv_rate < 16000 ? 0 : 16384 - 2048 * (opus_int32)(32000 - equiv_rate) / (equiv_rate - 14000);
  if (st->channels == 2 && (st->hybrid_stereo_width_Q14 < (1 << 14) || st->silk_mode.stereoWidth_Q14 < (1 << 14))) {
    opus_val16 g1 = st->hybrid_stereo_width_Q14 * (1.f / 16384);
    opus_val16 g2 = (opus_val16)(st->silk_mode.stereoWidth_Q14) * (1.f / 16384);
    if (celt_mode != nullptr) stereo_fade(celt_pcm.data(), celt_pcm.data(), g1, g2, celt_mode->overlap, frame_size, st->channels, celt_mode->window, st->Fs);
    st->hybrid_stereo_width_Q14 = st->silk_mode.stereoWidth_Q14;
}
  if (st->mode != opus_mode_celt_only && ec_tell(&enc) + 17 + 20 * (st->mode == opus_mode_hybrid) <= 8 * (max_data_bytes - 1)) {
    if (st->mode == opus_mode_hybrid) ec_enc_bit_logp(&enc, redundancy, 12);
    if (redundancy) {
      ec_enc_bit_logp(&enc, celt_to_silk, 1);
      int max_redundancy = (st->mode == opus_mode_hybrid) ? (max_data_bytes - 1) - ((ec_tell(&enc) + 8 + 3 + 7) >> 3) : (max_data_bytes - 1) - ((ec_tell(&enc) + 7) >> 3);
      redundancy_bytes = ((max_redundancy) < (redundancy_bytes) ? (max_redundancy) : (redundancy_bytes));
      redundancy_bytes = ((257) < (((2) > (redundancy_bytes) ? (2) : (redundancy_bytes))) ? (257) : (((2) > (redundancy_bytes) ? (2) : (redundancy_bytes))));
      if (st->mode == opus_mode_hybrid) ec_enc_uint(&enc, redundancy_bytes - 2, 256);
}
  } else { redundancy = 0;
}
  if (!redundancy) { st->silk_bw_switch = 0; redundancy_bytes = 0;
}
  if (st->mode != opus_mode_celt_only) start_band = 17;
  if (st->mode == opus_mode_silk_only) { ret = (ec_tell(&enc) + 7) >> 3; ec_enc_done(&enc); nb_compr_bytes = ret;
}
  else { nb_compr_bytes = (max_data_bytes - 1) - redundancy_bytes; ec_enc_shrink(&enc, nb_compr_bytes);
}
  if (st->mode == opus_mode_hybrid) { SILKInfo info;
#if defined(OPUS_CODEC_TRACE_RATE)
    info.signalType = st->silk_mode.signalType;
#endif
    info.offset = st->silk_mode.offset; info.bitrateBps = st->bitrate_bps; info.actualSilkBps = bits_to_bitrate(static_cast<int>(nBytes) * 8, st->Fs, frame_size); celt_encoder_set_silk_info(celt_enc, &info);
}
  if (redundancy && celt_to_silk) {
    configure_redundant_celt(0, -1);
    int err = celt_encode_with_ec(celt_enc, celt_pcm.data(), st->Fs / 200, data + nb_compr_bytes, redundancy_bytes, nullptr);
    if (err < 0) { return -3;
}
    celt_final_range(redundant_rng); celt_reset();
}
  if (uses_celt) celt_set_start(static_cast<opus_int32>(start_band));
  data[-1] = 0;
  if (st->mode != opus_mode_silk_only) {
    celt_encoder_set_vbr(celt_enc, static_cast<opus_int32>(st->use_vbr));
    if (st->mode == opus_mode_hybrid) {
      if (st->use_vbr) { opus_int32 celt_vbr_bps = st->bitrate_bps - st->silk_mode.bitRate; if (st->bitrate_bps >= hybrid_celt_residual_rate_floor_bps) { const auto actual_silk_bps = bits_to_bitrate(static_cast<int>(nBytes) * 8, st->Fs, frame_size); celt_vbr_bps = std::max(celt_vbr_bps, st->bitrate_bps - actual_silk_bps - hybrid_celt_residual_margin_bps); } celt_set_bitrate(static_cast<opus_int32>(std::max<opus_int32>(500, celt_vbr_bps))); celt_encoder_set_constrained_vbr(celt_enc, 0);
}
    } else if (st->use_vbr) {
      celt_encoder_set_vbr(celt_enc, 1); celt_encoder_set_constrained_vbr(celt_enc, static_cast<opus_int32>(st->vbr_constraint)); celt_set_bitrate(static_cast<opus_int32>(st->bitrate_bps));
}
    if (st->mode != st->prev_mode && st->prev_mode > 0 && encoder_uses_silk(st->application)) {
      unsigned char dummy[2];
      celt_reset(); celt_encode_with_ec(celt_enc, transition_prefill.data(), st->Fs / 400, dummy, 2, nullptr); celt_set_prediction(0);
}
    if (ec_tell(&enc) <= 8 * nb_compr_bytes) {
      ret = celt_encode_with_ec(celt_enc, celt_pcm.data(), frame_size, nullptr, nb_compr_bytes, &enc);
      if (ret < 0) { return -3;
}
      if (redundancy && celt_to_silk && st->mode == opus_mode_hybrid && nb_compr_bytes != ret) { move_n_items(data + nb_compr_bytes, static_cast<std::size_t>(redundancy_bytes), data + ret); nb_compr_bytes = ret + redundancy_bytes; }
}
    st->rangeFinal = celt_encoder_final_range(celt_enc);
  } else { st->rangeFinal = enc.rng;
}
  if (redundancy && !celt_to_silk) {
    unsigned char dummy[2];
    int N2 = st->Fs / 200, N4 = st->Fs / 400;
    celt_reset(); configure_redundant_celt(0, 0);
    if (st->mode == opus_mode_hybrid) { nb_compr_bytes = ret; ec_enc_shrink(&enc, nb_compr_bytes);
}
    celt_encode_with_ec(celt_enc, celt_pcm.data() + st->channels * (frame_size - N2 - N4), N4, dummy, 2, nullptr);
    int err = celt_encode_with_ec(celt_enc, celt_pcm.data() + st->channels * (frame_size - N2), N2, data + nb_compr_bytes, redundancy_bytes, nullptr);
    if (err < 0) { return -3;
}
    celt_final_range(redundant_rng);
}
  data--;
  data[0] |= gen_toc(st->mode, st->Fs / frame_size, curr_bandwidth, st->stream_channels);
  st->rangeFinal ^= redundant_rng;
  st->prev_mode = to_celt ? opus_mode_celt_only : st->mode;
  st->prev_channels = st->stream_channels;
  st->prev_framesize = frame_size;
  st->first = 0;
  if (ec_tell(&enc) > (max_data_bytes - 1) * 8) {
    if (max_data_bytes < 2) { return -2;
}
    data[1] = 0; ret = 1; st->rangeFinal = 0;
  } else if (st->mode == opus_mode_silk_only && !redundancy) { for (; ret > 2 && data[ret] == 0; --ret) {}
}
  ret += 1 + redundancy_bytes;
  apply_padding = !st->use_vbr;
  if (apply_padding) { if (pad_packet(data, ret, orig_max_data_bytes, 1) != 0) { return -3; } ret = orig_max_data_bytes;
}
#if defined(OPUS_CODEC_TRACE_RATE)
  OPUS_TRACE_RATE("opus_frame mode=%d Fs=%d channels=%d stream_channels=%d frame_size=%d bitrate=%d silk_bitrate=%d celt_bitrate=%d silk_bytes=%d bytes=%d max_bytes=%d redundancy=%d bandwidth=%d activity=%d dtx=0\n",
                  st->mode, st->Fs, st->channels, st->stream_channels, frame_size, st->bitrate_bps, st->silk_mode.bitRate,
                  st->mode == opus_mode_hybrid ? st->bitrate_bps - st->silk_mode.bitRate : st->mode == opus_mode_celt_only ? st->bitrate_bps : 0,
                  trace_silk_bytes, ret, max_data_bytes, redundancy_bytes, curr_bandwidth, activity);
#endif
  return ret;
}
[[nodiscard]] inline auto ref_opus_encode_impl(ref_OpusEncoder *st, std::span<const opus_res> samples, const int frame_size, unsigned char *data, opus_int32 max_data_bytes, const int lsb_depth) -> opus_int32 {
  return encode_native(st, samples.data(), frame_size, data, max_data_bytes, lsb_depth, 1);
}
static opus_int32 ref_opus_encode(ref_OpusEncoder *st, const opus_int16 *pcm, int analysis_frame_size, unsigned char *data, opus_int32 max_data_bytes) {
  const int frame_size = frame_size_select(st->application, analysis_frame_size, st->Fs);
  if (frame_size <= 0) { return -1;
}
  const auto sample_count = static_cast<std::size_t>(frame_size * st->channels);
  auto input = std::span<opus_res>{OPUS_SCRATCH(opus_res, sample_count), sample_count};
  for (auto index = std::size_t{}; index < sample_count; ++index) input[index] = static_cast<opus_res>(pcm[index] * (1.0f / 32768.0f));
  return ref_opus_encode_impl(st, input, frame_size, data, max_data_bytes, 16);
}
static opus_int32 ref_opus_encode_float(ref_OpusEncoder *st, const float *pcm, int analysis_frame_size, unsigned char *data, opus_int32 out_data_bytes) {
  const int frame_size = frame_size_select(st->application, analysis_frame_size, st->Fs);
  if (frame_size <= 0) { return -1;
}
  return ref_opus_encode_impl(st, std::span<const opus_res>{pcm, static_cast<std::size_t>(frame_size * st->channels)}, frame_size, data, out_data_bytes, 24);
}
[[nodiscard]] inline auto try_set_user_bitrate(ref_OpusEncoder *st, opus_int32 value) noexcept -> bool {
  if (value != -1000 && value != -1) {
    if (value <= 0) { return false;
}
    value = clamp_value(value, static_cast<opus_int32>(500), static_cast<opus_int32>(750000 * st->channels));
}
  st->user_bitrate_bps = value;
  return true;
}
static OPUS_COLD OPUS_NOINLINE void reset_ref_encoder_state(ref_OpusEncoder *st, CeltEncoderInternal *celt_enc) {
  silk_EncControlStruct dummy;
  void *silk_enc = (char *)st + st->silk_enc_offset;
  char *start = (char *)&st->stream_channels;
  zero_n_items(start, static_cast<std::size_t>(st->silk_enc_offset - (start - (char *)st)));
  if (encoder_uses_celt(st->application)) { celt_encoder_reset_state(celt_enc);
}
  if (encoder_uses_silk(st->application)) { silk_InitEncoder(silk_enc, st->channels, &dummy);
}
  st->stream_channels = st->channels; st->hybrid_stereo_width_Q14 = 1 << 14; st->prev_HB_gain = 1.0f; st->first = 1;
  st->mode = opus_mode_hybrid; st->bandwidth = 1105; st->variable_HP_smth2_Q15 = ((opus_int32)((opus_uint32)(silk_lin2log(60)) << (8)));
}
struct packet_frame_block { std::span<const opus_int16> lengths; std::span<const unsigned char *const> frames; };
[[nodiscard]] static inline auto packet_frame_block_view(const packet_frame_set *packet_frames, const int begin, const int count) -> packet_frame_block {
  const auto frame_count = static_cast<std::size_t>(count);
  return {std::span<const opus_int16>{packet_frames->len}.subspan(static_cast<std::size_t>(begin), frame_count), std::span<const unsigned char *const>{packet_frames->frames}.subspan(static_cast<std::size_t>(begin), frame_count)};
}
static OPUS_COLD OPUS_NOINLINE int append_packet_frames(packet_frame_set *packet_frames, const unsigned char *data, opus_int32 len, int self_delimited) {
  unsigned char tmp_toc;
  int curr_nb_frames, ret;
  if (len < 1) return -4;
  if (packet_frames->nb_frames == 0) {
    packet_frames->toc = data[0]; packet_frames->framesize = ref_opus_packet_get_samples_per_frame(data, 8000);
  } else if ((packet_frames->toc & 0xFC) != (data[0] & 0xFC)) { return -4;
}
  curr_nb_frames = ref_opus_packet_get_nb_frames(data, len);
  if (curr_nb_frames < 1) return -4;
  if ((curr_nb_frames + packet_frames->nb_frames) * packet_frames->framesize > 960) { return -4;
}
  ret = ref_opus_packet_parse_impl(data, len, self_delimited, &tmp_toc, packet_frames->frames.data() + packet_frames->nb_frames, packet_frames->len.data() + packet_frames->nb_frames, nullptr, nullptr, nullptr, nullptr);
  if (ret < 1) return ret;
  packet_frames->nb_frames += curr_nb_frames;
  return 0;
}
[[nodiscard]] static constexpr auto packet_output_base_size(const std::span<const opus_int16> lengths, const int self_delimited) noexcept -> opus_int32 { return self_delimited ? 1 + (lengths.back() >= 252) : 0;
}
[[nodiscard]] static inline auto packet_output_is_vbr(const std::span<const opus_int16> lengths) noexcept -> bool {
  if (lengths.empty()) return false;
  const auto first = lengths.front();
  for (std::size_t index = 1; index < lengths.size(); ++index) {
    if (lengths[index] != first) return true;
  }
  return false;
}
static inline void copy_packet_frames(unsigned char *&ptr, const std::span<const unsigned char *const> frames, const std::span<const opus_int16> lengths) {
  for (std::size_t index = 0; index < lengths.size(); ++index) { move_n_items(frames[index], static_cast<std::size_t>(lengths[index]), ptr); ptr += lengths[index]; }
}
static OPUS_COLD OPUS_NOINLINE opus_int32 write_packet_frames(packet_frame_set *packet_frames, int begin, int end, unsigned char *data, opus_int32 maxlen, int self_delimited, int pad) {
  int i, count;
  opus_int32 tot_size; unsigned char *ptr;
  if (begin < 0 || begin >= end || end > packet_frames->nb_frames) { return -1;
}
  count = end - begin;
  const auto frame_block = packet_frame_block_view(packet_frames, begin, count);
  const auto frame_lengths = frame_block.lengths;
  const auto frame_data = frame_block.frames;
  tot_size = packet_output_base_size(frame_lengths, self_delimited); ptr = data;
  if (count == 1 && !pad) {
    tot_size += frame_lengths.front() + 1;
    if (tot_size > maxlen) { return -2;
}
    *ptr++ = packet_frames->toc & 0xFC;
  } else if (count == 2 && !pad) {
    if (frame_lengths[1] == frame_lengths[0]) {
      tot_size += 2 * frame_lengths.front() + 1;
      if (tot_size > maxlen) { return -2;
}
      *ptr++ = (packet_frames->toc & 0xFC) | 0x1;
    } else {
      tot_size += frame_lengths[0] + frame_lengths[1] + 2 + (frame_lengths[0] >= 252);
      if (tot_size > maxlen) { return -2;
}
      *ptr++ = (packet_frames->toc & 0xFC) | 0x2; ptr += encode_size(frame_lengths[0], ptr);
}
  } else {
    const auto vbr = packet_output_is_vbr(frame_lengths);
    int pad_amount = 0;
    ptr = data; tot_size = packet_output_base_size(frame_lengths, self_delimited);
    if (vbr) {
      tot_size += 2;
      for (i = 0; i < count - 1; i++) tot_size += 1 + (frame_lengths[static_cast<std::size_t>(i)] >= 252) + frame_lengths[static_cast<std::size_t>(i)];
      tot_size += frame_lengths.back();
      if (tot_size > maxlen) { return -2;
}
      *ptr++ = (packet_frames->toc & 0xFC) | 0x3; *ptr++ = count | 0x80;
    } else {
      tot_size += count * frame_lengths.front() + 2;
      if (tot_size > maxlen) { return -2;
}
      *ptr++ = (packet_frames->toc & 0xFC) | 0x3; *ptr++ = count;
}
    pad_amount = pad ? (maxlen - tot_size) : 0;
    if (pad_amount != 0) {
      int nb_255s;
      data[1] |= 0x40; nb_255s = (pad_amount - 1) / 255;
      if (tot_size + nb_255s + 1 > maxlen) { return -2;
}
      for (i = 0; i < nb_255s; i++) *ptr++ = 255;
      *ptr++ = pad_amount - 255 * nb_255s - 1; tot_size += pad_amount;
}
    if (vbr) { for (i = 0; i < count - 1; i++) ptr += encode_size(frame_lengths[static_cast<std::size_t>(i)], ptr); }
}
  if (self_delimited) {
    int sdlen = encode_size(frame_lengths.back(), ptr);
    ptr += sdlen;
}
  copy_packet_frames(ptr, frame_data, frame_lengths);
  if (pad) { zero_n_items(ptr, static_cast<std::size_t>(data + maxlen - ptr));
}
  return tot_size;
}
static OPUS_COLD OPUS_NOINLINE int pad_packet(unsigned char *data, opus_int32 len, opus_int32 new_len, int pad) {
  packet_frame_set packet_frames{}; opus_int32 ret;
  if (len < 1) return -1;
  if (len == new_len) return 0;
  else if (len > new_len) return -1;
  auto *copy = OPUS_SCRATCH(unsigned char, len);
  copy_n_items(data, static_cast<std::size_t>(len), copy); ret = append_packet_frames(&packet_frames, copy, len, 0);
  if (ret != 0) return ret;
  ret = write_packet_frames(&packet_frames, 0, packet_frames.nb_frames, data, new_len, 0, pad);
  return ret > 0 ? 0 : static_cast<int>(ret);
}
namespace {
extern constinit const std::array<opus_val16, 25> eMeans;
}
static void amp2Log2(const CeltModeInternal *m, int effEnd, int end, celt_ener *bandE, celt_glog *bandLogE, int C),
    quant_coarse_energy(const CeltModeInternal *m, int start, int end, int effEnd, const celt_glog *eBands, celt_glog *oldEBands, opus_uint32 budget, celt_glog *error, ec_enc *enc, int C, int LM, int nbAvailableBytes, int force_intra, opus_val32 *delayedIntra),
    quant_fine_energy(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, celt_glog *error, int *fine_quant, int *extra_quant, ec_enc *enc, int C), quant_energy_finalise(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, celt_glog *error, int *fine_quant, int *fine_priority, int bits_left, ec_enc *enc, int C),
    unquant_coarse_energy(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, int intra, ec_dec *dec, int C, int LM), unquant_fine_energy(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, int *fine_quant, int *extra_quant, ec_dec *dec, int C),
    unquant_energy_finalise(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, int *fine_quant, int *fine_priority, int bits_left, ec_dec *dec, int C);
static void encode_pulses(const int *_y, int N, int K, ec_enc *enc);
static opus_val32 decode_pulses(int *_y, int N, int K, ec_dec *dec);
[[nodiscard]] constexpr auto get_pulses(int value) noexcept -> int { return value < 8 ? value : (8 + (value & 7)) << ((value >> 3) - 1);
}
[[nodiscard]] constexpr auto bits2pulses(const CeltModeInternal *mode, int band, int lm, int bits) noexcept -> int {
  ++lm;
  const unsigned char *cache = mode->cache.bits + mode->cache.index[lm * mode->nbEBands + band];
  int lo = 0, hi = cache[0];
  --bits;
  for (int i = 0; i < 6; ++i) { const int mid = (lo + hi + 1) >> 1; if (static_cast<int>(cache[mid]) >= bits) hi = mid; else lo = mid;
}
  const int low_distance = bits - (lo == 0 ? -1 : static_cast<int>(cache[lo]));
  const int high_distance = static_cast<int>(cache[hi]) - bits;
  return low_distance <= high_distance ? lo : hi;
}
[[nodiscard]] static constexpr auto pulses2bits(const CeltModeInternal *mode, int band, int lm, int pulses) noexcept -> int {
  ++lm;
  const unsigned char *cache = mode->cache.bits + mode->cache.index[lm * mode->nbEBands + band];
  return pulses == 0 ? 0 : cache[pulses] + 1;
}
static int clt_compute_allocation(const CeltModeInternal *m, int start, int end, const int *offsets, const int *cap, int alloc_trim, int *intensity, int *dual_stereo, opus_int32 total, opus_int32 *balance, int *pulses, int *ebits, int *fine_priority, int C, int LM, ec_ctx *ec, int encode, int prev, int signalBandwidth);
static void compute_mdcts(const CeltModeInternal *mode, int shortBlocks, celt_sig *in, celt_sig *out, int C, int CC, int LM, int upsample);
static void compute_band_energies(const CeltModeInternal *m, const celt_sig *X, celt_ener *bandE, int end, int C, int LM), normalise_bands(const CeltModeInternal *m, const celt_sig *freq, celt_norm *X, const celt_ener *bandE, int end, int C, int M),
    denormalise_bands(const CeltModeInternal *m, const celt_norm *X, celt_sig *freq, const celt_glog *bandE, int start, int end, int M, int downsample, int silence), haar1(celt_norm *X, int N0, int stride),
    quant_all_bands(int encode, const CeltModeInternal *m, int start, int end, celt_norm *X, celt_norm *Y, unsigned char *collapse_masks, const celt_ener *bandE, int *pulses, int shortBlocks, int spread, int dual_stereo, int intensity, int *tf_res, opus_int32 total_bits, opus_int32 balance, ec_ctx *ec, int M, int codedBands, opus_uint32 *seed, int disable_inv),
    anti_collapse(const CeltModeInternal *m, celt_norm *X_, unsigned char *collapse_masks, int LM, int C, int size, int start, int end, const celt_glog *logE, const celt_glog *prev1logE, const celt_glog *prev2logE, const int *pulses, opus_uint32 seed, int encode), exp_rotation(celt_norm *X, int len, int dir, int stride, int K, int spread);
static opus_int16 bitexact_cos(opus_int16 x);
static opus_uint32 celt_lcg_rand(opus_uint32 seed);
static opus_val16 op_pvq_search_c(celt_norm *X, int *iy, int K, int N);
static unsigned alg_quant(celt_norm *X, int N, int K, int spread, int B, ec_enc *enc, opus_val32 gain, int resynth);
static unsigned alg_unquant(celt_norm *X, int N, int K, int spread, int B, ec_dec *dec, opus_val32 gain);
static void renormalise_vector(celt_norm *X, int N, opus_val32 gain); static opus_int32 stereo_itheta(const celt_norm *X, const celt_norm *Y, int stereo, int N);
static int hysteresis_decision(opus_val16 val, std::span<const opus_val16> thresholds, std::span<const opus_val16> hysteresis, int prev) {
  int i = 0;
  while (i < static_cast<int>(thresholds.size()) && val >= thresholds[static_cast<std::size_t>(i)]) ++i;
  if (i > prev && val < thresholds[prev] + hysteresis[prev]) i = prev;
  if (i < prev && val > thresholds[prev - 1] - hysteresis[prev - 1]) i = prev;
  return i;
}
static opus_uint32 celt_lcg_rand(opus_uint32 seed) { return 1664525 * seed + 1013904223;
}
static opus_int16 bitexact_cos(opus_int16 x) {
  opus_int16 x2; opus_int32 tmp = (4096 + ((opus_int32)(x) * (x))) >> 13;
  opus_assume(tmp <= 32767); x2 = tmp;
  x2 = (32767 - x2) + ((16384 + ((opus_int32)(opus_int16)(x2) * (opus_int16)((-7651 + ((16384 + ((opus_int32)(opus_int16)(x2) * (opus_int16)((8277 + ((16384 + ((opus_int32)(opus_int16)(-626) * (opus_int16)(x2))) >> 15))))) >> 15))))) >> 15);
  opus_assume(x2 <= 32766);
  return 1 + x2;
}
static int bitexact_log2tan(int isin, int icos) {
  int lc, ls;
  lc = (((int)sizeof(unsigned) * 8) - (__builtin_clz(icos)));
  ls = (((int)sizeof(unsigned) * 8) - (__builtin_clz(isin))); icos <<= 15 - lc; isin <<= 15 - ls;
  return (ls - lc) * (1 << 11) + ((16384 + ((opus_int32)(opus_int16)(isin) * (opus_int16)(((16384 + ((opus_int32)(opus_int16)(isin) * (opus_int16)(-2597))) >> 15) + 7932))) >> 15) - ((16384 + ((opus_int32)(opus_int16)(icos) * (opus_int16)(((16384 + ((opus_int32)(opus_int16)(icos) * (opus_int16)(-2597))) >> 15) + 7932))) >> 15);
}
static void compute_band_energies(const CeltModeInternal *m, const celt_sig *X, celt_ener *bandE, int end, int C, int LM) {
  const opus_int16 *eBands = m->eBands;
  const int N = m->shortMdctSize << LM;
  for (int c = 0; c < C; ++c)
    for (int i = 0; i < end; i++) {
      opus_val32 sum = 1e-27f + celt_inner_prod_c(&X[c * N + (eBands[i] << LM)], &X[c * N + (eBands[i] << LM)], (eBands[i + 1] - eBands[i]) << LM);
      bandE[i + c * m->nbEBands] = (float)sqrt(sum); }
}
static void normalise_bands(const CeltModeInternal *m, const celt_sig *freq, celt_norm *X, const celt_ener *bandE, int end, int C, int M) {
  const opus_int16 *eBands = m->eBands;
  const int N = M * m->shortMdctSize;
  for (int c = 0; c < C; ++c)
    for (int i = 0; i < end; i++) {
      opus_val16 g = 1.f / (1e-27f + bandE[i + c * m->nbEBands]);
      for (int j = M * eBands[i]; j < M * eBands[i + 1]; j++) X[j + c * N] = freq[j + c * N] * g; }
}
static void denormalise_bands(const CeltModeInternal *m, const celt_norm *X, celt_sig *freq, const celt_glog *bandLogE, int start, int end, int M, int downsample, int silence) {
  const opus_int16 *__restrict eBands = m->eBands;
  const celt_norm *__restrict X_base = X;
  celt_sig *__restrict freq_base = freq;
  const celt_glog *__restrict band_log = bandLogE;
  const int N = M * m->shortMdctSize;
  int bound = M * eBands[end];
  if (downsample != 1) bound = std::min(bound, N / downsample);
  if (silence) { bound = 0; start = end = 0;
}
  celt_sig *f = freq_base;
  const celt_norm *x = X_base + M * eBands[start];
  const int prefix = M * eBands[start];
  if (prefix != 0) zero_n_items(f, static_cast<std::size_t>(prefix));
  f += prefix;
  for (int i = start; i < end; i++) {
    int j = M * eBands[i];
    const int band_end = M * eBands[i + 1];
    celt_glog lg = band_log[i] + (opus_val32)eMeans[i];
    opus_val32 g = (float)exp(0.6931471805599453094 * std::min(32.f, lg));
    for (; j < band_end; ++j) { *f++ = (*x) * g; x++; }
}
  opus_assume(start <= end); zero_n_items(&freq_base[bound], static_cast<std::size_t>(N - bound));
}
static void anti_collapse(const CeltModeInternal *m, celt_norm *X_, unsigned char *collapse_masks, int LM, int C, int size, int start, int end, const celt_glog *logE, const celt_glog *prev1logE, const celt_glog *prev2logE, const int *pulses, opus_uint32 seed, int encode) {
  for (int i = start; i < end; i++) {
    const int N0 = m->eBands[i + 1] - m->eBands[i];
    opus_assume(pulses[i] >= 0);
    const int depth = celt_udiv(1 + pulses[i], (m->eBands[i + 1] - m->eBands[i])) >> LM;
    const opus_val16 thresh = depth < static_cast<int>(celt_anti_collapse_thresh_by_depth.size()) ? celt_anti_collapse_thresh_by_depth[static_cast<std::size_t>(depth)] : .5f * (float)exp(0.6931471805599453094 * (-.125f * depth));
    const opus_val16 sqrt_1 = 1.f / (float)sqrt(N0 << LM);
    for (int c = 0; c < C; ++c) {
      celt_glog prev1 = prev1logE[c * m->nbEBands + i], prev2 = prev2logE[c * m->nbEBands + i];
      if (!encode && C == 1) { prev1 = std::max(prev1, prev1logE[m->nbEBands + i]); prev2 = std::max(prev2, prev2logE[m->nbEBands + i]);
}
      opus_val32 Ediff = std::max(0.f, logE[c * m->nbEBands + i] - std::min(prev1, prev2));
      celt_norm r = std::min(thresh, 2.f * (float)exp(0.6931471805599453094 * (-Ediff)));
      if (LM == 3) r *= 1.41421356f;
      r *= sqrt_1; celt_norm *X = X_ + c * size + (m->eBands[i] << LM);
      int renormalize = 0;
      for (int k = 0; k < 1 << LM; k++) {
        if (!(collapse_masks[i * C + c] & 1 << k)) {
          for (int j = 0; j < N0; j++) { seed = celt_lcg_rand(seed); X[(j << LM) + k] = (seed & 0x8000 ? r : -r);
}
          renormalize = 1; }
}
      if (renormalize) renormalise_vector(X, N0 << LM, 1.0f); } }
}
static void intensity_stereo(const CeltModeInternal *m, celt_norm *X, const celt_norm *Y, const celt_ener *bandE, int bandID, int N) {
  opus_val16 left = bandE[bandID], right = bandE[bandID + m->nbEBands];
  opus_val16 norm = 1e-15f + (float)sqrt(1e-15f + (opus_val32)left * left + (opus_val32)right * right);
  opus_val16 a1 = (opus_val32)left / (opus_val16)norm, a2 = (opus_val32)right / (opus_val16)norm;
  for (int j = 0; j < N; j++) X[j] = a1 * X[j] + a2 * Y[j];
}
static void stereo_split(celt_norm *X, celt_norm *Y, int N) {
  for (int j = 0; j < N; j++) {
    opus_val32 l = .70710678f * X[j], r = .70710678f * Y[j];
    X[j] = l + r;
    Y[j] = r - l;
  }
}
static void stereo_merge(celt_norm *X, celt_norm *Y, opus_val32 mid, int N) {
  opus_val32 xp = celt_inner_prod_c(Y, X, N), side = celt_inner_prod_c(Y, Y, N);
  xp = mid * xp;
  opus_val32 El = mid * mid + side - 2 * xp, Er = mid * mid + side + 2 * xp;
  if (Er < 6e-4f || El < 6e-4f) {
    copy_n_items(X, static_cast<std::size_t>(N), Y);
    return;
  }
  opus_val32 lgain = 1.f / (float)sqrt(El), rgain = 1.f / (float)sqrt(Er);
  for (int j = 0; j < N; j++) {
    celt_norm l = mid * X[j], r = Y[j];
    X[j] = lgain * (l - r);
    Y[j] = rgain * (l + r);
  }
}
static constexpr std::array<int, 30> ordery_table{1, 0, 3, 0, 2, 1, 7, 0, 4, 3, 6, 1, 5, 2, 15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5};
static void remap_hadamard(celt_norm *X, int N0, int stride, int hadamard, const bool interleave) {
  if (stride <= 1 || N0 <= 0) {
    return;
  }
  const auto count = static_cast<std::size_t>(N0 * stride);
  auto *tmp = OPUS_SCRATCH(celt_norm, count);
  const int *ordery = hadamard ? ordery_table.data() + stride - 2 : nullptr;
  for (int i = 0; i < stride; ++i) {
    const int reordered_i = hadamard ? ordery[i] : i;
    for (int j = 0; j < N0; ++j) {
      if (interleave) {
        tmp[j * stride + i] = X[reordered_i * N0 + j];
      } else {
        tmp[reordered_i * N0 + j] = X[j * stride + i];
      }
    }
  }
  copy_n_items(tmp, count, X);
}
static void deinterleave_hadamard(celt_norm *X, int N0, int stride, int hadamard) {
  remap_hadamard(X, N0, stride, hadamard, false);
}
static void interleave_hadamard(celt_norm *X, int N0, int stride, int hadamard) {
  remap_hadamard(X, N0, stride, hadamard, true);
}
void haar1(celt_norm *X, int N0, int stride) {
  int i, j;
  N0 >>= 1;
  for (i = 0; i < stride; i++) {
    for (j = 0; j < N0; j++) {
      opus_val32 tmp1, tmp2;
      tmp1 = (((.70710678f)) * (X[stride * 2 * j + i]));
      tmp2 = (((.70710678f)) * (X[stride * (2 * j + 1) + i]));
      X[stride * 2 * j + i] = ((tmp1) + (tmp2));
      X[stride * (2 * j + 1) + i] = ((tmp1) - (tmp2));
    }
  }
}
static int compute_qn(int N, int b, int offset, int pulse_cap, int stereo) {
  constexpr std::array<opus_int16, 8> exp2_table8{16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
  int qn, qb;
  int N2 = 2 * N - 1;
  if (stereo && N == 2) N2--;
  qb = celt_sudiv(b + N2 * offset, N2);
  qb = ((b - pulse_cap - (4 << 3)) < (qb) ? (b - pulse_cap - (4 << 3)) : (qb));
  qb = std::min(8 << 3, qb);
  if (qb < (1 << 3 >> 1)) {
    qn = 1;
  } else {
    qn = exp2_table8[qb & 0x7] >> (14 - (qb >> 3));
    qn = (qn + 1) >> 1 << 1;
  }
  opus_assume(qn <= 256);
  return qn;
}
struct band_ctx {
  int encode, resynth, i, intensity, spread, tf_change, theta_round, disable_inv, avoid_split_noise;
  const CeltModeInternal *m; ec_ctx *ec; opus_int32 remaining_bits; const celt_ener *bandE; opus_uint32 seed;
};
struct split_ctx { int inv, imid, iside, delta, itheta, qalloc; };
static void compute_theta(struct band_ctx *ctx, struct split_ctx *sctx, celt_norm *X, celt_norm *Y, int N, int *b, int B, int B0, int LM, int stereo, int *fill) {
  int qn, itheta = 0, itheta_q30 = 0;
  int delta, imid, iside;
  int qalloc, pulse_cap, offset;
  opus_int32 tell;
  int inv = 0, encode;
  const CeltModeInternal *m;
  int i, intensity;
  ec_ctx *ec;
  const celt_ener *bandE;
  encode = ctx->encode; m = ctx->m; i = ctx->i; intensity = ctx->intensity; ec = ctx->ec; bandE = ctx->bandE;
  pulse_cap = m->logN[i] + LM * (1 << 3); offset = (pulse_cap >> 1) - (stereo && N == 2 ? 16 : 4);
  qn = compute_qn(N, *b, offset, pulse_cap, stereo);
  if (stereo && i >= intensity) qn = 1;
  if (encode) { itheta_q30 = stereo_itheta(X, Y, stereo, N); itheta = itheta_q30 >> 16;
}
  tell = ec_tell_frac(ec);
  if (qn != 1) {
    if (encode) {
      if (!stereo || ctx->theta_round == 0) {
        itheta = (itheta * (opus_int32)qn + 8192) >> 14;
        if (!stereo && ctx->avoid_split_noise && itheta > 0 && itheta < qn) {
          int unquantized = celt_udiv((opus_int32)itheta * 16384, qn);
          imid = bitexact_cos((opus_int16)unquantized); iside = bitexact_cos((opus_int16)(16384 - unquantized));
          delta = ((16384 + ((opus_int32)(opus_int16)((N - 1) << 7) * (opus_int16)(bitexact_log2tan(iside, imid)))) >> 15);
          if (delta > *b) itheta = qn;
          else if (delta < -*b) itheta = 0;
}
      } else {
        int down, bias = itheta > 8192 ? 32767 / qn : -32767 / qn;
        const int theta_quant = (itheta * (opus_int32)qn + bias) >> 14;
        down = std::min(qn - 1, std::max(0, theta_quant));
        if (ctx->theta_round < 0) itheta = down;
        else itheta = down + 1; }
}
    if (stereo && N > 2) {
      int p0 = 3, x = itheta, x0 = qn / 2, ft = p0 * (x0 + 1) + x0;
      if (encode) {
        ec_encode(ec, x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0, x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0, ft);
      } else {
        int fs = ec_decode(ec, ft);
        if (fs < (x0 + 1) * p0) x = fs / p0;
        else x = x0 + 1 + (fs - (x0 + 1) * p0);
        ec_dec_update(ec, x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0, x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0, ft);
        itheta = x;
}
    } else if (B0 > 1 || stereo) {
      if (encode) ec_enc_uint(ec, itheta, qn + 1);
      else itheta = ec_dec_uint(ec, qn + 1);
    } else {
      int fs = 1, ft;
      ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
      if (encode) {
        int fl;
        fs = itheta <= (qn >> 1) ? itheta + 1 : qn + 1 - itheta;
        fl = itheta <= (qn >> 1) ? itheta * (itheta + 1) >> 1 : ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
        ec_encode(ec, fl, fl + fs, ft);
      } else {
        int fl = 0;
        int fm = ec_decode(ec, ft);
        if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
          itheta = (isqrt32(8 * (opus_uint32)fm + 1) - 1) >> 1; fs = itheta + 1; fl = itheta * (itheta + 1) >> 1;
        } else {
          itheta = (2 * (qn + 1) - isqrt32(8 * (opus_uint32)(ft - fm - 1) + 1)) >> 1; fs = qn + 1 - itheta;
          fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
}
        ec_dec_update(ec, fl, fl + fs, ft); }
}
    opus_assume(itheta >= 0); itheta = celt_udiv((opus_int32)itheta * 16384, qn);
    if (encode && stereo) {
      if (itheta == 0) intensity_stereo(m, X, Y, bandE, i, N);
      else stereo_split(X, Y, N);
}
  } else if (stereo) {
    if (encode) {
      inv = itheta > 8192 && !ctx->disable_inv;
      if (inv) {
        int j;
        for (j = 0; j < N; j++) Y[j] = -Y[j];
}
      intensity_stereo(m, X, Y, bandE, i, N);
}
    if (*b > 2 << 3 && ctx->remaining_bits > 2 << 3) {
      if (encode) ec_enc_bit_logp(ec, inv, 2);
      else inv = ec_dec_bit_logp(ec, 2);
    } else inv = 0;
    if (ctx->disable_inv) inv = 0;
    itheta = 0; itheta_q30 = 0;
}
  qalloc = ec_tell_frac(ec) - tell; *b -= qalloc;
  if (itheta == 0) {
    imid = 32767; iside = 0; *fill &= (1 << B) - 1; delta = -16384;
  } else if (itheta == 16384) {
    imid = 0; iside = 32767; *fill &= ((1 << B) - 1) << B; delta = 16384;
  } else {
    imid = bitexact_cos((opus_int16)itheta); iside = bitexact_cos((opus_int16)(16384 - itheta));
    delta = ((16384 + ((opus_int32)(opus_int16)((N - 1) << 7) * (opus_int16)(bitexact_log2tan(iside, imid)))) >> 15);
}
  sctx->inv = inv; sctx->imid = imid; sctx->iside = iside; sctx->delta = delta; sctx->itheta = itheta; sctx->qalloc = qalloc;
}
static unsigned quant_band_n1(struct band_ctx *ctx, celt_norm *X, celt_norm *Y, celt_norm *lowband_out) {
  int c, stereo;
  celt_norm *x = X;
  int encode;
  ec_ctx *ec; encode = ctx->encode; ec = ctx->ec; stereo = Y != nullptr;
  for (c = 0; c < 1 + stereo; ++c) {
    int sign = 0;
    if (ctx->remaining_bits >= 1 << 3) {
      if (encode) {
        sign = x[0] < 0; ec_enc_bits(ec, sign, 1);
      } else { sign = ec_dec_bits(ec, 1);
}
      ctx->remaining_bits -= 1 << 3;
}
    if (ctx->resynth) x[0] = sign ? -1.f : 1.f;
    x = Y;
}
  if (lowband_out) lowband_out[0] = (X[0]);
  return 1;
}
static unsigned quant_partition(struct band_ctx *ctx, celt_norm *X, int N, int b, int B, celt_norm *lowband, int LM, opus_val32 gain, int fill) {
  const unsigned char *cache;
  int q, curr_bits;
  int imid = 0, iside = 0;
  int B0 = B;
  opus_val32 mid = 0, side = 0; unsigned cm = 0;
  celt_norm *Y = nullptr;
  int encode;
  const CeltModeInternal *m;
  int i, spread;
  ec_ctx *ec; encode = ctx->encode; m = ctx->m; i = ctx->i;
#if defined(OPUS_CODEC_TRACE_RATE)
  ++opus_trace_pvq.partitions;
#endif
  spread = ctx->spread; ec = ctx->ec; cache = m->cache.bits + m->cache.index[(LM + 1) * m->nbEBands + i];
  if (LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
#if defined(OPUS_CODEC_TRACE_RATE)
    ++opus_trace_pvq.splits;
#endif
    int mbits, sbits, delta;
    int itheta, qalloc;
    struct split_ctx sctx;
    celt_norm *next_lowband2 = nullptr; opus_int32 rebalance;
    N >>= 1; Y = X + N; LM -= 1;
    if (B == 1) fill = (fill & 1) | (fill << 1);
    B = (B + 1) >> 1; compute_theta(ctx, &sctx, X, Y, N, &b, B, B0, LM, 0, &fill); imid = sctx.imid; iside = sctx.iside;
    delta = sctx.delta; itheta = sctx.itheta; qalloc = sctx.qalloc; mid = (1.f / 32768) * imid; side = (1.f / 32768) * iside;
    if (B0 > 1 && (itheta & 0x3fff)) { if (itheta > 8192) delta -= delta >> (4 - LM); else delta = ((0) < (delta + (N << 3 >> (5 - LM))) ? (0) : (delta + (N << 3 >> (5 - LM))));
}
    const int half_delta_bits = (b - delta) / 2;
    mbits = std::max(0, std::min(b, half_delta_bits));
    sbits = b - mbits; ctx->remaining_bits -= qalloc;
    if (lowband) next_lowband2 = lowband + N;
    rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
      cm = quant_partition(ctx, X, N, mbits, B, lowband, LM, ((gain) * (mid)), fill); rebalance = mbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && itheta != 0) sbits += rebalance - (3 << 3);
      cm |= quant_partition(ctx, Y, N, sbits, B, next_lowband2, LM, ((gain) * (side)), fill >> B) << (B0 >> 1);
    } else {
      cm = quant_partition(ctx, Y, N, sbits, B, next_lowband2, LM, ((gain) * (side)), fill >> B) << (B0 >> 1);
      rebalance = sbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && itheta != 16384) mbits += rebalance - (3 << 3);
      cm |= quant_partition(ctx, X, N, mbits, B, lowband, LM, ((gain) * (mid)), fill);
}
  } else {
    q = bits2pulses(m, i, LM, b); curr_bits = pulses2bits(m, i, LM, q); ctx->remaining_bits -= curr_bits;
    for (; ctx->remaining_bits < 0 && q > 0;) { ctx->remaining_bits += curr_bits; q--; curr_bits = pulses2bits(m, i, LM, q); ctx->remaining_bits -= curr_bits;
}
    if (q != 0) {
      int K = get_pulses(q);
#if defined(OPUS_CODEC_TRACE_RATE)
      if (K == 1) ++opus_trace_pvq.k1;
      else if (K == 2) ++opus_trace_pvq.k2;
      else ++opus_trace_pvq.kn;
#endif
      if (encode) {
        cm = alg_quant(X, N, K, spread, B, ec, gain, ctx->resynth);
      } else { cm = alg_unquant(X, N, K, spread, B, ec, gain);
}
    } else {
#if defined(OPUS_CODEC_TRACE_RATE)
      ++opus_trace_pvq.q0;
#endif
      int j;
      if (ctx->resynth) {
        unsigned cm_mask; cm_mask = (unsigned)(1UL << B) - 1; fill &= cm_mask;
        if (!fill) {
          zero_n_items(X, static_cast<std::size_t>(N));
        } else {
          if (lowband == nullptr) {
            for (j = 0; j < N; j++) { ctx->seed = celt_lcg_rand(ctx->seed); X[j] = ((celt_norm)((opus_int32)ctx->seed >> 20));
}
            cm = cm_mask;
          } else {
            for (j = 0; j < N; j++) {
              opus_val16 tmp; ctx->seed = celt_lcg_rand(ctx->seed);
              tmp = (1.0f / 256); tmp = (ctx->seed) & 0x8000 ? tmp : -tmp; X[j] = lowband[j] + tmp;
}
            cm = fill;
}
          renormalise_vector(X, N, gain); } } }
}
  return cm;
}
static unsigned quant_band(struct band_ctx *ctx, celt_norm *X, int N, int b, int B, celt_norm *lowband, int LM, celt_norm *lowband_out, opus_val32 gain, celt_norm *lowband_scratch, int fill) {
  int N0 = N, N_B = N, N_B0, B0 = B, time_divide = 0, recombine = 0, longBlocks;
  unsigned cm = 0;
  int k, encode, tf_change;
  encode = ctx->encode; tf_change = ctx->tf_change; longBlocks = B0 == 1; N_B = celt_udiv(N_B, B);
  if (N == 1) { return quant_band_n1(ctx, X, nullptr, lowband_out);
}
  if (tf_change > 0) recombine = tf_change;
  if (lowband_scratch && lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || B0 > 1)) { copy_n_items(lowband, static_cast<std::size_t>(N), lowband_scratch); lowband = lowband_scratch;
}
  for (k = 0; k < recombine; k++) {
    if (encode) haar1(X, N >> k, 1 << k);
    if (lowband) haar1(lowband, N >> k, 1 << k);
    fill = bit_interleave_table[fill & 0xF] | bit_interleave_table[fill >> 4] << 2;
}
  B >>= recombine; N_B <<= recombine;
  for (; (N_B & 1) == 0 && tf_change < 0; ++tf_change) {
    if (encode) haar1(X, N_B, B);
    if (lowband) haar1(lowband, N_B, B);
    fill |= fill << B; B <<= 1; N_B >>= 1; time_divide++;
}
  B0 = B; N_B0 = N_B;
  if (B0 > 1) {
    if (encode) deinterleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);
    if (lowband) deinterleave_hadamard(lowband, N_B >> recombine, B0 << recombine, longBlocks);
}
  { cm = quant_partition(ctx, X, N, b, B, lowband, LM, gain, fill);
}
  if (ctx->resynth) {
    if (B0 > 1) interleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);
    N_B = N_B0; B = B0;
    for (k = 0; k < time_divide; k++) { B >>= 1; N_B <<= 1; cm |= cm >> B; haar1(X, N_B, B);
}
    for (k = 0; k < recombine; k++) { cm = bit_deinterleave_table[cm]; haar1(X, N0 >> k, 1 << k);
}
    B <<= recombine;
    if (lowband_out) {
      int j;
      opus_val16 n = ((float)sqrt(((N0))));
      for (j = 0; j < N0; j++) lowband_out[j] = ((n) * (X[j]));
}
    cm &= (1 << B) - 1;
}
  return cm;
}
static unsigned quant_band_stereo(struct band_ctx *ctx, celt_norm *X, celt_norm *Y, int N, int b, int B, celt_norm *lowband, int LM, celt_norm *lowband_out, celt_norm *lowband_scratch, int fill) {
  int imid = 0, iside = 0;
  int inv = 0;
  opus_val32 mid = 0, side = 0; unsigned cm = 0;
  int mbits, sbits, delta, itheta, qalloc;
  struct split_ctx sctx;
  int orig_fill, encode;
  ec_ctx *ec; encode = ctx->encode; ec = ctx->ec;
  if (N == 1) { return quant_band_n1(ctx, X, Y, lowband_out);
}
  orig_fill = fill;
  if (encode) {
    if (ctx->bandE[ctx->i] < 1e-10f || ctx->bandE[ctx->m->nbEBands + ctx->i] < 1e-10f) {
      if (ctx->bandE[ctx->i] > ctx->bandE[ctx->m->nbEBands + ctx->i]) copy_n_items(X, static_cast<std::size_t>(N), Y);
      else copy_n_items(Y, static_cast<std::size_t>(N), X); }
}
  compute_theta(ctx, &sctx, X, Y, N, &b, B, B, LM, 1, &fill); inv = sctx.inv; imid = sctx.imid; iside = sctx.iside;
  delta = sctx.delta; itheta = sctx.itheta; qalloc = sctx.qalloc; mid = (1.f / 32768) * imid; side = (1.f / 32768) * iside;
  if (N == 2) {
    int c, sign = 0;
    celt_norm *x2, *y2; mbits = b;
    sbits = 0;
    if (itheta != 0 && itheta != 16384) sbits = 1 << 3;
    mbits -= sbits; c = itheta > 8192; ctx->remaining_bits -= qalloc + sbits; x2 = c ? Y : X; y2 = c ? X : Y;
    if (sbits) {
      if (encode) {
        sign = ((x2[0]) * (y2[1])) - ((x2[1]) * (y2[0])) < 0; ec_enc_bits(ec, sign, 1);
      } else { sign = ec_dec_bits(ec, 1); }
}
    sign = 1 - 2 * sign; cm = quant_band(ctx, x2, N, mbits, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, orig_fill);
    y2[0] = -sign * x2[1]; y2[1] = sign * x2[0];
    if (ctx->resynth) {
      celt_norm tmp; X[0] = ((mid) * (X[0]));
      X[1] = ((mid) * (X[1])); Y[0] = ((side) * (Y[0])); Y[1] = ((side) * (Y[1])); tmp = X[0];
      X[0] = ((tmp) - (Y[0])); Y[0] = ((tmp) + (Y[0])); tmp = X[1]; X[1] = ((tmp) - (Y[1])); Y[1] = ((tmp) + (Y[1]));
}
  } else {
    opus_int32 rebalance;
    const int half_delta_bits = (b - delta) / 2;
    mbits = std::max(0, std::min(b, half_delta_bits));
    sbits = b - mbits; ctx->remaining_bits -= qalloc; rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
      cm = quant_band(ctx, X, N, mbits, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, fill);
      rebalance = mbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && itheta != 0) sbits += rebalance - (3 << 3);
      cm |= quant_band(ctx, Y, N, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
    } else {
      cm = quant_band(ctx, Y, N, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
      rebalance = sbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && itheta != 16384) mbits += rebalance - (3 << 3);
      cm |= quant_band(ctx, X, N, mbits, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, fill); }
}
  if (ctx->resynth) {
    if (N != 2) stereo_merge(X, Y, mid, N);
    if (inv) {
      int j;
      for (j = 0; j < N; j++) Y[j] = -Y[j]; }
}
  return cm;
}
static void special_hybrid_folding(const CeltModeInternal *m, celt_norm *norm, celt_norm *norm2, int start, int M, int dual_stereo) {
  int n1, n2;
  const opus_int16 *eBands = m->eBands;
  n1 = M * (eBands[start + 1] - eBands[start]); n2 = M * (eBands[start + 2] - eBands[start + 1]);
  copy_n_items(&norm[2 * n1 - n2], static_cast<std::size_t>(n2 - n1), &norm[n1]);
  if (dual_stereo) copy_n_items(&norm2[2 * n1 - n2], static_cast<std::size_t>(n2 - n1), &norm2[n1]);
}
static void quant_all_bands(int encode, const CeltModeInternal *m, int start, int end, celt_norm *X_, celt_norm *Y_, unsigned char *collapse_masks, const celt_ener *bandE, int *pulses, int shortBlocks, int spread, int dual_stereo, int intensity, int *tf_res, opus_int32 total_bits, opus_int32 balance, ec_ctx *ec, int LM, int codedBands, opus_uint32 *seed, int disable_inv) {
  int i;
  opus_int32 remaining_bits;
  const opus_int16 *eBands = m->eBands;
  celt_norm *norm, *norm2;
  celt_norm *lowband_scratch;
  int B, M, lowband_offset;
  int update_lowband = 1;
  int C = Y_ != nullptr ? 2 : 1;
  int norm_offset;
  const int resynth = !encode;
  struct band_ctx ctx;
  M = 1 << LM; B = shortBlocks ? M : 1; norm_offset = M * eBands[start];
  const int norm_size = M * eBands[m->nbEBands - 1] - norm_offset;
  norm = OPUS_SCRATCH(celt_norm, C * norm_size);
  zero_n_items(norm, static_cast<std::size_t>(C * norm_size));
  norm2 = norm + norm_size;
  lowband_scratch = X_ + M * eBands[m->effEBands - 1];
  lowband_offset = 0; ctx.bandE = bandE; ctx.ec = ec; ctx.encode = encode;
  ctx.intensity = intensity; ctx.m = m; ctx.seed = *seed; ctx.spread = spread;
  ctx.disable_inv = disable_inv; ctx.resynth = resynth; ctx.theta_round = 0;
  ctx.avoid_split_noise = B > 1;
  for (i = start; i < end; i++) {
    opus_int32 tell;
    int b, N;
    opus_int32 curr_balance;
    int effective_lowband = -1;
    celt_norm *X, *Y;
    int tf_change = 0;
    unsigned x_cm; unsigned y_cm;
    int last;
    ctx.i = i; last = (i == end - 1); X = X_ + M * eBands[i];
    if (Y_ != nullptr) { Y = Y_ + M * eBands[i]; } else { Y = nullptr; } N = M * eBands[i + 1] - M * eBands[i];
    opus_assume(N > 0); tell = ec_tell_frac(ec);
    if (i != start) balance -= tell;
    remaining_bits = total_bits - tell - 1; ctx.remaining_bits = remaining_bits;
    if (i <= codedBands - 1) {
      curr_balance = celt_sudiv(balance, std::min(3, codedBands - i));
      const int raw_bits = std::min(remaining_bits + 1, pulses[i] + curr_balance);
      b = std::max(0, std::min(16383, raw_bits));
    } else { b = 0;
}
    if (resynth && (M * eBands[i] - N >= M * eBands[start] || i == start + 1) && (update_lowband || lowband_offset == 0)) lowband_offset = i;
    if (i == start + 1) special_hybrid_folding(m, norm, norm2, start, M, dual_stereo);
    tf_change = tf_res[i]; ctx.tf_change = tf_change;
    if (i >= m->effEBands) {
      X = norm;
      if (Y_ != nullptr) Y = norm;
      lowband_scratch = nullptr;
}
    if (last) lowband_scratch = nullptr;
    if (lowband_offset != 0 && (spread != (3) || B > 1 || tf_change < 0)) {
      int fold_start, fold_end, fold_i;
      effective_lowband = std::max(0, M * eBands[lowband_offset] - norm_offset - N); fold_start = lowband_offset;
      for (; M * eBands[--fold_start] > effective_lowband + norm_offset;) {}
      fold_end = lowband_offset - 1;
      for (; ++fold_end < i && M * eBands[fold_end] < effective_lowband + norm_offset + N;) {}
      x_cm = y_cm = 0;
      for (fold_i = fold_start;; ++fold_i) {
        x_cm |= collapse_masks[fold_i * C + 0]; y_cm |= collapse_masks[fold_i * C + C - 1];
        if (fold_i + 1 >= fold_end) { break; }
}
    } else x_cm = y_cm = (1 << B) - 1;
    if (dual_stereo && i == intensity) {
      int j;
      dual_stereo = 0;
      if (resynth)
        for (j = 0; j < M * eBands[i] - norm_offset; j++) norm[j] = (.5f * (norm[j] + norm2[j]));
}
    auto *const lowband = effective_lowband != -1 ? norm + effective_lowband : nullptr;
    auto *const lowband2 = effective_lowband != -1 ? norm2 + effective_lowband : nullptr;
    auto *const lowband_out = last ? nullptr : norm + M * eBands[i] - norm_offset;
    auto *const lowband_out2 = last ? nullptr : norm2 + M * eBands[i] - norm_offset;
    if (dual_stereo) {
      x_cm = quant_band(&ctx, X, N, b / 2, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, x_cm);
      y_cm = quant_band(&ctx, Y, N, b / 2, B, lowband2, LM, lowband_out2, 1.0f, lowband_scratch, y_cm);
    } else {
      if (Y != nullptr) {
        ctx.theta_round = 0;
        x_cm = quant_band_stereo(&ctx, X, Y, N, b, B, lowband, LM, lowband_out, lowband_scratch, x_cm | y_cm);
      } else { x_cm = quant_band(&ctx, X, N, b, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, x_cm | y_cm);
}
      y_cm = x_cm;
}
    collapse_masks[i * C + 0] = (unsigned char)x_cm; collapse_masks[i * C + C - 1] = (unsigned char)y_cm;
    balance += pulses[i] + tell; update_lowband = b > (N << 3); ctx.avoid_split_noise = 0;
}
  *seed = ctx.seed;
}
static void _celt_lpc(opus_val16 *_lpc, const opus_val32 *ac, int p), celt_iir(const opus_val32 *x, const opus_val16 *den, opus_val32 *y, int N, int ord, opus_val16 *mem);
static void celt_fir_c(const opus_val16 *x, const opus_val16 *num, opus_val16 *y, int N, int ord);
static int _celt_autocorr(const opus_val16 *x, opus_val32 *ac, const celt_coef *window, int overlap, int lag, int n);
static int resampling_factor(opus_int32 rate) {
  int ret;
  switch (rate) {
  case 48000: ret = 1; break;
  case 24000: ret = 2; break;
  case 16000: ret = 3; break;
  case 12000: ret = 4; break;
  case 8000: ret = 6; break;
  default: ret = 0; break;
}
  return ret;
}
static void comb_filter_const_c(opus_val32 *y, opus_val32 *x, int T, int N, celt_coef g10, celt_coef g11, celt_coef g12) {
  opus_val32 x0, x1, x2, x3, x4;
  int i;
  x4 = x[-T - 2]; x3 = x[-T - 1]; x2 = x[-T]; x1 = x[-T + 1];
  for (i = 0; i < N; i++) {
    x0 = x[i - T + 2]; y[i] = x[i] + ((g10) * (x2)) + ((g11) * (((x1) + (x3)))) + ((g12) * (((x0) + (x4))));
    y[i] = (y[i]); x4 = x3; x3 = x2; x2 = x1; x1 = x0; }
}
static void comb_filter(opus_val32 *y, opus_val32 *x, int T0, int T1, int N, opus_val16 g0, opus_val16 g1, int tapset0, int tapset1, const celt_coef *window, int overlap) {
  int i;
  celt_coef g00, g01, g02, g10, g11, g12; opus_val32 x0, x1, x2, x3, x4;
  if (g0 == 0 && g1 == 0) { if (x != y) move_n_items(x, static_cast<std::size_t>(N), y); return;
}
  T0 = std::max(T0, 15); T1 = std::max(T1, 15);
  g00 = ((g0) * (comb_filter_tapset_gains[tapset0][0])); g01 = ((g0) * (comb_filter_tapset_gains[tapset0][1]));
  g02 = ((g0) * (comb_filter_tapset_gains[tapset0][2])); g10 = ((g1) * (comb_filter_tapset_gains[tapset1][0]));
  g11 = ((g1) * (comb_filter_tapset_gains[tapset1][1])); g12 = ((g1) * (comb_filter_tapset_gains[tapset1][2]));
  x1 = x[-T1 + 1]; x2 = x[-T1]; x3 = x[-T1 - 1]; x4 = x[-T1 - 2];
  if (g0 == g1 && T0 == T1 && tapset0 == tapset1) overlap = 0;
  for (i = 0; i < overlap; i++) {
    celt_coef f; x0 = x[i - T1 + 2];
    f = ((window[i]) * (window[i]));
    y[i] = x[i] + (((((1.0f - f)) * (g00))) * (x[i - T0])) + (((((1.0f - f)) * (g01))) * (((x[i - T0 + 1]) + (x[i - T0 - 1])))) + (((((1.0f - f)) * (g02))) * (((x[i - T0 + 2]) + (x[i - T0 - 2])))) + ((((f) * (g10))) * (x2)) + ((((f) * (g11))) * (((x1) + (x3)))) + ((((f) * (g12))) * (((x0) + (x4))));
    y[i] = (y[i]); x4 = x3; x3 = x2; x2 = x1; x1 = x0;
}
  if (g1 == 0) { if (x != y) move_n_items(x + overlap, static_cast<std::size_t>(N - overlap), y + overlap); return;
}
  comb_filter_const_c(y + i, x + i, T1, N - i, g10, g11, g12);
}
static constinit const std::array<std::array<signed char, 8>, 4> tf_select_table = numeric_blob_matrix<signed char, 4, 8>(R"blob(00FF00FF00FF00FF00FF00FE010001FF00FE00FD020001FF00FE00FD030001FF)blob");
static OPUS_NOINLINE void init_caps(const CeltModeInternal *m, std::span<int> cap, int LM, int C) {
  opus_assume(cap.size() >= static_cast<std::size_t>(m->nbEBands));
  for (int i = 0; i < m->nbEBands; ++i) {
    const auto N = (m->eBands[i + 1] - m->eBands[i]) << LM;
    cap[i] = (m->cache.caps[m->nbEBands * (2 * LM + C - 1) + i] + 64) * C * N >> 2; }
}
[[nodiscard]] static constexpr auto celt_dynalloc_quanta(int width) noexcept -> int { return std::min(width << 3, std::max(6 << 3, width));
}
[[nodiscard]] constexpr auto celt_bandwidth_floor(opus_int32 equiv_rate, int channels) noexcept -> int {
  const auto per_channel_rate = equiv_rate / std::max(channels, 1);
  return per_channel_rate < 32000 ? 13 : per_channel_rate < 48000 ? 16 : per_channel_rate < 60000 ? 18 : per_channel_rate < 80000 ? 19 : 20;
}
[[nodiscard]] constexpr auto celt_hybrid_target(opus_int32 base_target, int LM, opus_val16 tf_estimate, int silk_offset) noexcept -> opus_int32 {
  auto target = base_target;
  if (silk_offset < 100) target += 12 << 3 >> (3 - LM);
  if (silk_offset > 100) target -= 18 << 3 >> (3 - LM);
  target += static_cast<opus_int32>((tf_estimate - (.25f)) * (50 << 3));
  return tf_estimate > (.7f) ? std::max(target, 50 << 3) : target;
}
[[nodiscard]] constexpr auto celt_anti_collapse_reserve(bool is_transient, int LM, opus_int32 bits) noexcept -> int { return is_transient && LM >= 2 && bits >= ((LM + 2) << 3) ? (1 << 3) : 0;
}
static void celt_commit_band_state(std::span<celt_glog> old_band, std::span<celt_glog> old_log, std::span<celt_glog> old_log2, int channels, int nbEBands, int start, int end, bool is_transient, bool mirror_mono) {
  if (mirror_mono && channels > 1) copy_n_items(old_band.data(), static_cast<std::size_t>(nbEBands), old_band.data() + nbEBands);
  if (!is_transient) { copy_n_items(old_log.data(), old_log.size(), old_log2.data()); copy_n_items(old_band.data(), old_band.size(), old_log.data());
}
  else {
    for (auto index = std::size_t{}; index < old_log.size(); ++index) old_log[index] = std::min(old_log[index], old_band[index]);
}
  for (int channel = 0; channel < channels; ++channel) {
    const auto offset = static_cast<std::size_t>(channel * nbEBands);
    if (start > 0) { fill_n_items(old_band.data() + offset, static_cast<std::size_t>(start), 0.f); fill_n_items(old_log.data() + offset, static_cast<std::size_t>(start), -(28.f)); fill_n_items(old_log2.data() + offset, static_cast<std::size_t>(start), -(28.f));
}
    if (end < nbEBands) { const auto tail = static_cast<std::size_t>(nbEBands - end); fill_n_items(old_band.data() + offset + end, tail, 0.f); fill_n_items(old_log.data() + offset + end, tail, -(28.f)); fill_n_items(old_log2.data() + offset + end, tail, -(28.f)); } }
}
static auto celt_encode_dynalloc(ec_enc *enc, std::span<const opus_int16> eBands, std::span<int> offsets, std::span<const int> cap, int start, int end, int C, int LM, opus_int32 &total_bits, int &total_boost) -> opus_int32 {
  int dynalloc_logp = 6; total_bits <<= 3; total_boost = 0; auto tell = ec_tell_frac(enc);
  for (int i = start; i < end; ++i) {
    int boost = 0, dynalloc_loop_logp = dynalloc_logp, j = 0, quanta = celt_dynalloc_quanta(C * (eBands[i + 1] - eBands[i]) << LM);
    for (; static_cast<opus_int32>(tell + (dynalloc_loop_logp << 3)) < total_bits - total_boost && boost < cap[i]; ++j) { const auto flag = j < offsets[i]; ec_enc_bit_logp(enc, flag, dynalloc_loop_logp); tell = ec_tell_frac(enc); if (!flag) break; boost += quanta; total_boost += quanta; dynalloc_loop_logp = 1;
}
    if (j) dynalloc_logp = std::max(2, dynalloc_logp - 1);
    offsets[i] = boost;
}
  return tell;
}
static auto celt_decode_dynalloc(ec_dec *dec, std::span<const opus_int16> eBands, std::span<int> offsets, std::span<const int> cap, int start, int end, int C, int LM, opus_int32 &total_bits) -> opus_int32 {
  int dynalloc_logp = 6; total_bits <<= 3; auto tell = ec_tell_frac(dec);
  for (int i = start; i < end; ++i) {
    int boost = 0, dynalloc_loop_logp = dynalloc_logp, quanta = celt_dynalloc_quanta(C * (eBands[i + 1] - eBands[i]) << LM);
    for (; static_cast<opus_int32>(tell + (dynalloc_loop_logp << 3)) < total_bits && boost < cap[i];) { if (!ec_dec_bit_logp(dec, dynalloc_loop_logp)) { tell = ec_tell_frac(dec); break; } tell = ec_tell_frac(dec); boost += quanta; total_bits -= quanta; dynalloc_loop_logp = 1;
}
    offsets[i] = boost; if (boost > 0) dynalloc_logp = std::max(2, dynalloc_logp - 1);
}
  return tell;
}
[[nodiscard]] static constexpr auto ref_opus_strerror(int error) -> const char * {
  constexpr std::array<const char *, 8> error_strings{"success", "invalid argument", "buffer too small", "internal error", "corrupted stream", "request not implemented", "invalid state", "memory allocation failed"};
  if (error > 0 || error < -7) return "unknown error";
  else return error_strings[-error];
}
struct CeltEncoderInternal {
  const CeltModeInternal *mode; int channels, stream_channels, force_intra, disable_pf, complexity, upsample, start, end; opus_int32 bitrate;
  int vbr, constrained_vbr, lsb_depth, disable_inv; opus_uint32 rng; int spread_decision; opus_val32 delayedIntra;
  int lastCodedBands, prefilter_period; opus_val16 prefilter_gain; int prefilter_tapset, consec_transient; SILKInfo silk_info;
  opus_val32 preemph_memE[2], preemph_memD[2]; opus_int32 vbr_reservoir, vbr_drift, vbr_offset, vbr_count; opus_val32 overlap_max; opus_val16 stereo_saving; int intensity; celt_glog spec_avg; celt_sig in_mem[1];
};
struct celt_encoder_views {
  celt_sig *prefilter_mem{};
  celt_glog *oldBandE{}, *oldLogE{}, *oldLogE2{}, *energyError{};
};
static constexpr int celt_default_overlap = 120;
static constexpr int celt_default_nb_ebands = 21;
static constexpr int celt_encoder_history_size = 1024;
static constexpr int celt_decoder_history_size = 2048;
[[nodiscard]] static constexpr auto celt_encoder_storage_size(const int channels) noexcept -> int {
  return sizeof(CeltEncoderInternal) + (channels * celt_default_overlap - 1) * static_cast<int>(sizeof(celt_sig)) +
         channels * celt_encoder_history_size * static_cast<int>(sizeof(celt_sig)) +
         4 * channels * celt_default_nb_ebands * static_cast<int>(sizeof(celt_glog));
}
[[nodiscard]] static auto make_celt_encoder_views(CeltEncoderInternal *st) noexcept -> celt_encoder_views {
  const auto channels = st->channels, overlap = st->mode->overlap, nbEBands = st->mode->nbEBands; celt_encoder_views views{};
  views.prefilter_mem = st->in_mem + channels * overlap;
  views.oldBandE = reinterpret_cast<celt_glog *>(st->in_mem + channels * (overlap + 1024));
  views.oldLogE = views.oldBandE + channels * nbEBands; views.oldLogE2 = views.oldLogE + channels * nbEBands; views.energyError = views.oldLogE2 + channels * nbEBands;
  return views;
}
static OPUS_COLD OPUS_NOINLINE int celt_encoder_get_size(int channels) { return celt_encoder_storage_size(channels); }
static int celt_custom_encoder_init_arch(CeltEncoderInternal *st, const CeltModeInternal *mode, int channels) {
  if (channels < 0 || channels > 2) return -1;
  if (st == nullptr || mode == nullptr) return -7;
  zero_n_items((char *)st, static_cast<std::size_t>(celt_encoder_get_size(channels))); st->mode = mode;
  st->stream_channels = st->channels = channels; st->upsample = 1; st->start = 0; st->end = st->mode->effEBands;
  st->constrained_vbr = 1;
  st->bitrate = -1; st->vbr = 0; st->force_intra = 0; st->complexity = 5; st->lsb_depth = 24; celt_encoder_reset_state(st);
  return 0;
}
static OPUS_COLD OPUS_NOINLINE int celt_encoder_init(CeltEncoderInternal *st, opus_int32 sampling_rate, int channels) { if (const auto ret = celt_custom_encoder_init_arch(st, default_custom_mode(), channels); ret != 0) return ret; st->upsample = resampling_factor(sampling_rate); return 0;
}
[[nodiscard]] static auto celt_frame_lm(const CeltModeInternal *mode, const int frame_size) noexcept -> int {
  for (int LM = 0; LM <= mode->maxLM; ++LM) if ((mode->shortMdctSize << LM) == frame_size) return LM;
  return -1;
}
[[nodiscard]] constexpr auto celt_effective_end(const CeltModeInternal *mode, const int end) noexcept -> int { return std::min(end, mode->effEBands);
}
static void compute_mdcts(const CeltModeInternal *mode, int shortBlocks, celt_sig *in, celt_sig *out, int C, int CC, int LM, int upsample) {
  const int overlap = mode->overlap;
  int N, B, shift, i, b, c;
  if (shortBlocks) {
    B = shortBlocks; N = mode->shortMdctSize; shift = mode->maxLM;
  } else { B = 1; N = mode->shortMdctSize << LM; shift = mode->maxLM - LM;
}
  for (c = 0; c < CC; ++c)
    for (b = 0; b < B; b++) { clt_mdct_forward_c(&mode->mdct, in + c * (B * N + overlap) + b * N, &out[b + c * N * B], mode->window, overlap, shift, B);
}
  if (CC == 2 && C == 1) {
    const int bn = B * N;
    for (i = 0; i < bn; i++) out[i] = .5f * out[i] + .5f * out[bn + i];
}
  if (upsample != 1) {
    for (c = 0; c < C; ++c) {
      const int bound = B * N / upsample;
      auto *band = out + c * B * N;
      for (int j = 0; j < bound; ++j) band[j] *= upsample;
      zero_n_items(&out[c * B * N + bound], static_cast<std::size_t>(B * N - bound)); } }
}
static void celt_preemphasis(const opus_res *pcmp, celt_sig *inp, int N, int CC, int upsample, const opus_val16 *coef, celt_sig *mem, int clip) {
  int i;
  opus_val16 coef0; celt_sig m;
  int Nu;
  coef0 = coef[0]; m = *mem;
  if (coef[1] == 0 && upsample == 1 && !clip) {
    for (i = 0; i < N; i++) {
      celt_sig x; x = (32768.f * (pcmp[CC * i]));
      inp[i] = x - m; m = ((coef0) * (x));
}
    *mem = m; return;
}
  Nu = N / upsample;
  if (upsample != 1) { zero_n_items(inp, static_cast<std::size_t>(N));
}
  for (i = 0; i < Nu; i++) inp[i * upsample] = (32768.f * (pcmp[CC * i]));
  if (clip) { for (i = 0; i < Nu; i++) { auto &sample = inp[i * upsample]; sample = std::max(-65536.f, std::min(65536.f, sample)); }
}
  {
    for (i = 0; i < N; i++) {
      celt_sig x; x = inp[i];
      inp[i] = x - m; m = ((coef0) * (x)); }
}
  *mem = m;
}
static void tf_encode(int start, int end, int isTransient, int *tf_res, int LM, int tf_select, ec_enc *enc) {
  int curr, i, tf_select_rsv, tf_changed, logp;
  opus_uint32 budget, tell; budget = enc->storage * 8;
  tell = ec_tell(enc); logp = isTransient ? 2 : 4;
  tf_select_rsv = LM > 0 && tell + logp + 1 <= budget; budget -= tf_select_rsv; curr = tf_changed = 0;
  for (i = start; i < end; i++) {
    if (tell + logp <= budget) {
      ec_enc_bit_logp(enc, tf_res[i] ^ curr, logp); tell = ec_tell(enc); curr = tf_res[i]; tf_changed |= curr;
    } else tf_res[i] = curr; logp = isTransient ? 4 : 5;
}
  if (tf_select_rsv && tf_select_table[LM][4 * isTransient + 0 + tf_changed] != tf_select_table[LM][4 * isTransient + 2 + tf_changed]) ec_enc_bit_logp(enc, tf_select, 1);
  else tf_select = 0;
  for (i = start; i < end; i++) tf_res[i] = tf_select_table[LM][4 * isTransient + 2 * tf_select + tf_res[i]];
}
static int alloc_trim_analysis(const CeltModeInternal *m, const celt_norm *X, const celt_glog *bandLogE, int end, int LM, int C, int N0, opus_val16 *stereo_saving, opus_val16 tf_estimate, int intensity, opus_int32 equiv_rate) {
  int i;
  opus_val32 diff = 0;
  int c, trim_index;
  opus_val16 trim = (5.f); opus_val16 logXC, logXC2;
  if (equiv_rate < 64000) {
    trim = (4.f);
  } else if (equiv_rate < 80000) { opus_int32 frac = (equiv_rate - 64000) >> 10; trim = (4.f) + (1.f / 16.f) * frac;
}
  if (C == 2) {
    opus_val16 sum = 0, minXC;
    for (i = 0; i < 8; i++) {
      opus_val32 partial = celt_inner_prod_c(&X[m->eBands[i] << LM], &X[N0 + (m->eBands[i] << LM)], (m->eBands[i + 1] - m->eBands[i]) << LM);
      sum = ((sum) + (((partial))));
}
    sum = (((1.f / 8)) * (sum)); sum = (((1.f)) < (((float)fabs(sum))) ? ((1.f)) : (((float)fabs(sum)))); minXC = sum;
    for (i = 8; i < intensity; i++) {
      opus_val32 partial = celt_inner_prod_c(&X[m->eBands[i] << LM], &X[N0 + (m->eBands[i] << LM)], (m->eBands[i + 1] - m->eBands[i]) << LM);
      minXC = ((minXC) < (((float)fabs(((partial))))) ? (minXC) : (((float)fabs(((partial))))));
}
    minXC = (((1.f)) < (((float)fabs(minXC))) ? ((1.f)) : (((float)fabs(minXC))));
    logXC = ((float)(1.442695040888963387 * log((1.001f) - ((opus_val32)(sum) * (opus_val32)(sum)))));
    const opus_val16 min_logXC = (float)(1.442695040888963387 * log((1.001f) - ((opus_val32)(minXC) * (opus_val32)(minXC))));
    logXC2 = std::max(.5f * logXC, min_logXC);
    trim += ((-(4.f)) > ((((.75f)) * (logXC))) ? (-(4.f)) : ((((.75f)) * (logXC))));
    *stereo_saving = ((*stereo_saving + (0.25f)) < (-(.5f * (logXC2))) ? (*stereo_saving + (0.25f)) : (-(.5f * (logXC2))));
}
  for (c = 0; c < C; ++c) {
    for (i = 0; i < end - 1; i++) { diff += (bandLogE[i + c * m->nbEBands]) * (opus_int32)(2 + 2 * i - end); }
}
  diff /= C * (end - 1);
  const opus_val16 trim_adjust = (diff + (1.f)) / 6;
  trim -= std::max(-(2.f), std::min(2.f, trim_adjust));
  trim -= 2 * (tf_estimate);
  trim_index = (int)floor(.5f + trim); trim_index = std::max(0, std::min(10, trim_index));
  return trim_index;
}
static int stereo_analysis(const CeltModeInternal *m, const celt_norm *X, int LM, int N0) {
  int i, thetas;
  opus_val32 sumLR = 1e-15f, sumMS = 1e-15f;
  for (i = 0; i < 13; i++) {
    int j;
    for (j = m->eBands[i] << LM; j < m->eBands[i + 1] << LM; j++) {
      opus_val32 L, R, M, S; L = (X[j]);
      R = (X[N0 + j]); M = ((L) + (R)); S = ((L) - (R)); sumLR = ((sumLR) + (((((float)fabs(L))) + (((float)fabs(R))))));
      sumMS = ((sumMS) + (((((float)fabs(M))) + (((float)fabs(S)))))); }
}
  sumMS = (((0.707107f)) * (sumMS)); thetas = 13;
  if (LM <= 1) thetas -= 8;
  return (((m->eBands[13] << (LM + 1)) + thetas) * (sumMS)) > ((m->eBands[13] << (LM + 1)) * (sumLR));
}
static celt_glog median_of_5(const celt_glog *x) {
  celt_glog t0, t1, t2, t3, t4; t2 = x[2];
  if (x[0] > x[1]) {
    t0 = x[1]; t1 = x[0];
  } else { t0 = x[0]; t1 = x[1];
}
  if (x[3] > x[4]) {
    t3 = x[4]; t4 = x[3];
  } else { t3 = x[3]; t4 = x[4];
}
  if (t0 > t3) {
    {
      celt_glog tmp = t0; t0 = t3;
      t3 = tmp;
}
    {
      celt_glog tmp = t1; t1 = t4;
      t4 = tmp; }
}
  if (t2 > t1) {
    if (t1 < t3) return std::min(t2, t3);
    else return std::min(t4, t1);
  } else {
    if (t2 < t3) return std::min(t1, t3);
    else return std::min(t2, t4); }
}
static celt_glog median_of_3(const celt_glog *x) {
  celt_glog t0, t1, t2;
  if (x[0] > x[1]) {
    t0 = x[1]; t1 = x[0];
  } else { t0 = x[0]; t1 = x[1];
}
  t2 = x[2];
  if (t1 < t2) return t1;
  else if (t0 < t2) return t2; else return t0;
}
static celt_glog dynalloc_analysis(const celt_glog *bandLogE, const celt_glog *bandLogE2, const celt_glog *oldBandE, int nbEBands, int start, int end, int C, int *offsets, int lsb_depth, const opus_int16 *logN, int isTransient, int vbr, int constrained_vbr, const opus_int16 *eBands, int LM, int effectiveBytes, opus_int32 *tot_boost_, celt_glog *surround_dynalloc, int *importance, int *spread_weight, opus_val16 tone_freq, opus_val32 toneishness) {
  int i, c;
  opus_int32 tot_boost = 0; celt_glog maxDepth;
  auto *follower = OPUS_SCRATCH(celt_glog, C * nbEBands);
  auto *noise_floor = OPUS_SCRATCH(celt_glog, C * nbEBands);
  auto *bandLogE3 = OPUS_SCRATCH(celt_glog, nbEBands);
  zero_n_items(offsets, static_cast<std::size_t>(nbEBands)); maxDepth = -(31.9f);
  for (i = 0; i < end; i++) { noise_floor[i] = (0.0625f) * logN[i] + (.5f) + (9 - lsb_depth) - (eMeans[i]) + (.0062f) * (i + 5) * (i + 5);
}
  for (c = 0; c < C; ++c) { for (i = 0; i < end; i++) maxDepth = std::max(maxDepth, bandLogE[c * nbEBands + i] - noise_floor[i]);
}
  {
    auto *mask = OPUS_SCRATCH(celt_glog, nbEBands);
    auto *sig = OPUS_SCRATCH(celt_glog, nbEBands);
    for (i = 0; i < end; i++) mask[i] = bandLogE[i] - noise_floor[i];
    if (C == 2) { for (i = 0; i < end; i++) mask[i] = std::max(mask[i], bandLogE[nbEBands + i] - noise_floor[i]);
}
    copy_n_items(mask, static_cast<std::size_t>(end), sig);
    for (i = 1; i < end; i++) mask[i] = ((mask[i]) > (mask[i - 1] - (2.f)) ? (mask[i]) : (mask[i - 1] - (2.f)));
    for (i = end - 2; i >= 0; i--) mask[i] = ((mask[i]) > (mask[i + 1] - (3.f)) ? (mask[i]) : (mask[i + 1] - (3.f)));
    const celt_glog mask_floor = std::max(0.f, maxDepth - (12.f));
    for (i = 0; i < end; i++) {
      celt_glog smr = sig[i] - std::max(mask_floor, mask[i]);
      const int raw_shift = std::max(0, -(int)floor(.5f + smr));
      int shift = std::min(5, raw_shift);
      spread_weight[i] = 32 >> shift; }
}
  if (effectiveBytes >= (30 + 5 * LM)) {
    int last = 0;
    for (c = 0; c < C; ++c) {
      celt_glog offset, tmp; celt_glog *f;
      copy_n_items(&bandLogE2[c * nbEBands], static_cast<std::size_t>(end), bandLogE3);
      if (LM == 0) { for (i = 0; i < std::min(8, end); i++) bandLogE3[i] = std::max(bandLogE2[c * nbEBands + i], oldBandE[c * nbEBands + i]);
}
      f = &follower[c * nbEBands]; f[0] = bandLogE3[0];
      for (i = 1; i < end; i++) {
        if (bandLogE3[i] > bandLogE3[i - 1] + (.5f)) last = i;
        f[i] = ((f[i - 1] + (1.5f)) < (bandLogE3[i]) ? (f[i - 1] + (1.5f)) : (bandLogE3[i]));
}
      for (i = last - 1; i >= 0; i--) f[i] = std::min(f[i], std::min(f[i + 1] + (2.f), bandLogE3[i]));
      offset = (1.f);
      for (i = 2; i < end - 2; i++) f[i] = ((f[i]) > (median_of_5(&bandLogE3[i - 2]) - offset) ? (f[i]) : (median_of_5(&bandLogE3[i - 2]) - offset));
      tmp = median_of_3(&bandLogE3[0]) - offset; f[0] = std::max(f[0], tmp);
      f[1] = std::max(f[1], tmp); tmp = median_of_3(&bandLogE3[end - 3]) - offset;
      f[end - 2] = std::max(f[end - 2], tmp); f[end - 1] = std::max(f[end - 1], tmp);
      for (i = 0; i < end; i++) f[i] = std::max(f[i], noise_floor[i]);
}
    if (C == 2) {
      for (i = start; i < end; i++) {
        follower[nbEBands + i] = ((follower[nbEBands + i]) > (follower[i] - (4.f)) ? (follower[nbEBands + i]) : (follower[i] - (4.f)));
        follower[i] = ((follower[i]) > (follower[nbEBands + i] - (4.f)) ? (follower[i]) : (follower[nbEBands + i] - (4.f)));
        follower[i] = (.5f * (std::max(0.f, bandLogE[i] - follower[i]) + std::max(0.f, bandLogE[nbEBands + i] - follower[nbEBands + i])));
}
    } else { for (i = start; i < end; i++) { follower[i] = std::max(0.f, bandLogE[i] - follower[i]); }
}
    for (i = start; i < end; i++) follower[i] = std::max(follower[i], surround_dynalloc[i]);
    for (i = start; i < end; i++) { importance[i] = (int)floor(.5f + 13 * ((float)exp(0.6931471805599453094 * (((follower[i]) < ((4.f)) ? (follower[i]) : ((4.f)))))));
}
    if ((!vbr || constrained_vbr) && !isTransient) { for (i = start; i < end; i++) follower[i] = (.5f * (follower[i]));
}
    for (i = start; i < end; i++) {
      if (i < 8) follower[i] *= 2;
      if (i >= 12) follower[i] = (.5f * (follower[i]));
}
    if (toneishness > (.98f)) {
      int freq_bin = (int)floor(.5 + (tone_freq) * 120 / 3.141592653);
      for (i = start; i < end; i++) {
        if (freq_bin >= eBands[i] && freq_bin <= eBands[i + 1]) follower[i] += (2.f);
        if (freq_bin >= eBands[i] - 1 && freq_bin <= eBands[i + 1] + 1) follower[i] += (1.f);
        if (freq_bin >= eBands[i] - 2 && freq_bin <= eBands[i + 1] + 2) follower[i] += (1.f);
        if (freq_bin >= eBands[i] - 3 && freq_bin <= eBands[i + 1] + 3) follower[i] += (.5f);
}
      if (freq_bin >= eBands[end]) { follower[end - 1] += (2.f); follower[end - 2] += (1.f); }
}
    for (i = start; i < end; i++) {
      int width, boost, boost_bits;
      follower[i] = ((follower[i]) < ((4)) ? (follower[i]) : ((4))); follower[i] = (follower[i]);
      width = C * (eBands[i + 1] - eBands[i]) << LM;
      if (width < 6) {
        boost = (int)(follower[i]); boost_bits = boost * width << 3;
      } else if (width > 48) {
        boost = (int)(follower[i] * 8); boost_bits = (boost * width << 3) / 8;
      } else { boost = (int)(follower[i] * width / 6); boost_bits = boost * 6 << 3;
}
      if ((!vbr || (constrained_vbr && !isTransient)) && (tot_boost + boost_bits) >> 3 >> 3 > 2 * effectiveBytes / 3) {
        opus_int32 cap = ((2 * effectiveBytes / 3) << 3 << 3); offsets[i] = cap - tot_boost;
        tot_boost = cap;
        break;
      } else { offsets[i] = boost; tot_boost += boost_bits; }
}
  } else { for (i = start; i < end; i++) importance[i] = 13;
}
  *tot_boost_ = tot_boost;
  return maxDepth;
}
static int tone_lpc(const opus_val16 *x, int len, int delay, opus_val32 *lpc) {
  int i;
  opus_val32 r00 = 0, r01 = 0, r11 = 0, r02 = 0, r12 = 0, r22 = 0; opus_val32 edges;
  opus_val32 num0, num1, den; opus_assume(len > 2 * delay);
  for (i = 0; i < len - 2 * delay; i++) { r00 += ((opus_val32)(x[i]) * (opus_val32)(x[i])); r01 += ((opus_val32)(x[i]) * (opus_val32)(x[i + delay])); r02 += ((opus_val32)(x[i]) * (opus_val32)(x[i + 2 * delay]));
}
  edges = 0;
  for (i = 0; i < delay; i++) edges += ((opus_val32)(x[len + i - 2 * delay]) * (opus_val32)(x[len + i - 2 * delay])) - ((opus_val32)(x[i]) * (opus_val32)(x[i]));
  r11 = r00 + edges; edges = 0;
  for (i = 0; i < delay; i++) edges += ((opus_val32)(x[len + i - delay]) * (opus_val32)(x[len + i - delay])) - ((opus_val32)(x[i + delay]) * (opus_val32)(x[i + delay]));
  r22 = r11 + edges; edges = 0;
  for (i = 0; i < delay; i++) edges += ((opus_val32)(x[len + i - 2 * delay]) * (opus_val32)(x[len + i - delay])) - ((opus_val32)(x[i]) * (opus_val32)(x[i + delay]));
  r12 = r01 + edges;
  {
    opus_val32 R00, R01, R11, R02, R12, R22; R00 = r00 + r22;
    R01 = r01 + r12; R11 = 2 * r11; R02 = 2 * r02; R12 = r12 + r01; R22 = r00 + r22; r00 = R00; r01 = R01; r11 = R11;
    r02 = R02; r12 = R12; r22 = R22;
}
  den = ((r00) * (r11)) - ((r01) * (r01));
  if (den < .001f * ((r00) * (r11))) return 1;
  num1 = ((r02) * (r11)) - ((r01) * (r12));
  if (num1 >= den) lpc[1] = (1.f);
  else if (num1 <= -den) lpc[1] = -(1.f); else lpc[1] = ((float)(num1) / (den));
  num0 = ((r00) * (r12)) - ((r02) * (r01));
  if ((.5f * (num0)) >= den) lpc[0] = (1.999999f);
  else if ((.5f * (num0)) <= -den) lpc[0] = -(1.999999f); else lpc[0] = ((float)(num0) / (den));
  return 0;
}
static opus_val16 tone_detect(const celt_sig *in, int CC, int N, opus_val32 *toneishness, opus_int32 Fs) {
  int i, delay = 1, fail;
  opus_val32 lpc[2]; opus_val16 freq;
  const opus_val16 *x = in;
  if (CC == 2) {
    auto *x_sum = OPUS_SCRATCH(opus_val16, N);
    for (i = 0; i < N; i++) x_sum[i] = ((((in[i])) + ((in[i + N]))));
    x = x_sum;
}
  fail = tone_lpc(x, N, delay, lpc);
  for (; delay <= Fs / 3000 && (fail || (lpc[0] > (1.f) && lpc[1] < 0));) { delay *= 2; fail = tone_lpc(x, N, delay, lpc);
}
  if (!fail && ((lpc[0]) * (lpc[0])) + (((3.999999)) * (lpc[1])) < 0) {
    *toneishness = -lpc[1]; freq = acos(.5f * lpc[0]) / delay;
  } else { freq = -1; *toneishness = 0;
}
  return freq;
}
static int run_prefilter(CeltEncoderInternal *st, celt_sig *in, celt_sig *prefilter_mem, int CC, int N, int prefilter_tapset, int *pitch, opus_val16 *gain, int *qgain, int enabled, int complexity, opus_val16 tf_estimate, int nbAvailableBytes, opus_val16 tone_freq, opus_val32 toneishness) {
  int c;
  std::array<celt_sig *, 2> pre;
  const CeltModeInternal *mode;
  int pitch_index;
  opus_val16 gain1, pf_threshold;
  int pf_on, qg, overlap;
  std::array<opus_val32, 2> before{}, after{};
  int cancel_pitch = 0;
  constexpr auto max_period = 1024, min_period = 15; mode = st->mode; overlap = mode->overlap;
  // Complexity <5 never performs pitch search; when no old prefilter is active,
  // keep only the histories instead of building the full scratch/filter path.
  if (__builtin_expect(complexity < 5, 0) && (!enabled || toneishness <= (.60f)) && st->prefilter_gain == 0) {
    for (c = 0; c < CC; ++c) {
      copy_n_items(st->in_mem + c * overlap, static_cast<std::size_t>(overlap), in + c * (N + overlap));
      copy_n_items(in + c * (N + overlap) + N, static_cast<std::size_t>(overlap), st->in_mem + c * overlap);
      if (N > max_period) {
        copy_n_items(in + c * (N + overlap) + overlap + N - max_period, static_cast<std::size_t>(max_period), prefilter_mem + c * max_period);
      } else {
        move_n_items(prefilter_mem + c * max_period + N, static_cast<std::size_t>(max_period - N), prefilter_mem + c * max_period);
        copy_n_items(in + c * (N + overlap) + overlap, static_cast<std::size_t>(N), prefilter_mem + c * max_period + max_period - N);
      }
    }
    *gain = 0; *pitch = 15; *qgain = 0;
    return 0;
  }
  auto *_pre = OPUS_SCRATCH(celt_sig, CC * (N + max_period));
  pre[0] = _pre; pre[1] = _pre + (N + max_period);
  for (c = 0; c < CC; ++c) {
    copy_n_items(prefilter_mem + c * max_period, static_cast<std::size_t>(max_period), pre[c]);
    copy_n_items(in + c * (N + overlap) + overlap, static_cast<std::size_t>(N), pre[c] + max_period);
}
  if (enabled && toneishness > (.60f)) {
    int multiple = 1;
    if ((tone_freq) >= (3.1416f)) tone_freq = (3.141593f) - tone_freq;
    for (; (tone_freq) >= multiple * (0.39f); ++multiple) {}
    if ((tone_freq) > (0.006148f)) {
      const int tone_pitch = (int)floor(.5 + 2.f * 3.141592653 * multiple / (tone_freq));
      pitch_index = std::min(tone_pitch, 1024 - 2);
    } else { pitch_index = 15;
}
    gain1 = (.75f);
  } else if (enabled && complexity >= 5) {
    auto *pitch_buf = OPUS_SCRATCH(opus_val16, (max_period + N) >> 1);
    pitch_downsample(std::span<celt_sig *const>{pre.data(), static_cast<std::size_t>(CC)}, pitch_buf, (max_period + N) >> 1, 2);
    pitch_search(pitch_buf + (max_period >> 1), pitch_buf, N, max_period - 3 * min_period, &pitch_index);
    pitch_index = max_period - pitch_index;
    gain1 = remove_doubling(pitch_buf, max_period, min_period, N, &pitch_index, st->prefilter_period, st->prefilter_gain);
    if (pitch_index > max_period - (2)) pitch_index = max_period - (2);
    gain1 = (((.7f)) * (gain1));
  } else { gain1 = 0; pitch_index = 15;
}
  pf_threshold = (.2f);
  if (abs(pitch_index - st->prefilter_period) * 10 > pitch_index) {
    pf_threshold += (.2f);
    if (tf_estimate > (.98f)) gain1 = 0;
}
  if (nbAvailableBytes < 25) pf_threshold += (.1f);
  if (nbAvailableBytes < 35) pf_threshold += (.1f);
  if (st->prefilter_gain > (.4f)) pf_threshold -= (.1f);
  if (st->prefilter_gain > (.55f)) pf_threshold -= (.1f);
  pf_threshold = ((pf_threshold) > ((.2f)) ? (pf_threshold) : ((.2f)));
  if (gain1 < pf_threshold) {
    gain1 = 0; pf_on = 0; qg = 0;
  } else {
    if (((float)fabs(gain1 - st->prefilter_gain)) < (.1f)) gain1 = st->prefilter_gain;
    qg = (int)floor(.5f + gain1 * 32 / 3) - 1; qg = std::max(0, std::min(7, qg));
    gain1 = (0.09375f) * (qg + 1); pf_on = 1;
}
  for (c = 0; c < CC; ++c) {
    int i, offset = mode->shortMdctSize - overlap;
    st->prefilter_period = std::max(st->prefilter_period, 15);
    copy_n_items(st->in_mem + c * (overlap), static_cast<std::size_t>(overlap), in + c * (N + overlap));
    for (i = 0; i < N; i++) before[c] += ((float)fabs((in[c * (N + overlap) + overlap + i])));
    if (offset) comb_filter(in + c * (N + overlap) + overlap, pre[c] + max_period, st->prefilter_period, st->prefilter_period, offset, -st->prefilter_gain, -st->prefilter_gain, st->prefilter_tapset, st->prefilter_tapset, nullptr, 0);
    comb_filter(in + c * (N + overlap) + overlap + offset, pre[c] + max_period + offset, st->prefilter_period, pitch_index, N - offset, -st->prefilter_gain, -gain1, st->prefilter_tapset, prefilter_tapset, mode->window, overlap);
    for (i = 0; i < N; i++) after[c] += ((float)fabs((in[c * (N + overlap) + overlap + i])));
}
  if (CC == 2) {
    std::array<opus_val16, 2> thresh; thresh[0] = (((((.25f)) * (gain1))) * (before[0])) + (((.01f)) * (before[1]));
    thresh[1] = (((((.25f)) * (gain1))) * (before[1])) + (((.01f)) * (before[0]));
    if (after[0] - before[0] > thresh[0] || after[1] - before[1] > thresh[1]) cancel_pitch = 1;
    if (before[0] - after[0] < thresh[0] && before[1] - after[1] < thresh[1]) cancel_pitch = 1;
  } else { if (after[0] > before[0]) cancel_pitch = 1;
}
  if (cancel_pitch) {
    for (c = 0; c < CC; ++c) {
      int offset = mode->shortMdctSize - overlap;
      copy_n_items(pre[c] + max_period, static_cast<std::size_t>(N), in + c * (N + overlap) + overlap);
      comb_filter(in + c * (N + overlap) + overlap + offset, pre[c] + max_period + offset, st->prefilter_period, pitch_index, overlap, -st->prefilter_gain, -0, st->prefilter_tapset, prefilter_tapset, mode->window, overlap);
}
    gain1 = 0; pf_on = 0; qg = 0;
}
  for (c = 0; c < CC; ++c) {
    copy_n_items(in + c * (N + overlap) + N, static_cast<std::size_t>(overlap), st->in_mem + c * (overlap));
    if (N > max_period) {
      copy_n_items(pre[c] + N, static_cast<std::size_t>(max_period), prefilter_mem + c * max_period);
    } else {
      move_n_items(prefilter_mem + c * max_period + N, static_cast<std::size_t>(max_period - N), prefilter_mem + c * max_period);
      copy_n_items(pre[c] + max_period, static_cast<std::size_t>(N), prefilter_mem + c * max_period + max_period - N); }
}
  *gain = gain1; *pitch = pitch_index; *qgain = qg;
  return pf_on;
}
static int compute_vbr(const CeltModeInternal *mode, opus_int32 base_target, int LM, opus_int32 bitrate, int lastCodedBands, int C, int intensity, int constrained_vbr, opus_val16 stereo_saving, int tot_boost, opus_val16 tf_estimate, celt_glog maxDepth, celt_glog temporal_vbr) {
  opus_int32 target;
  int coded_bins, coded_bands;
  opus_val16 tf_calibration;
  int nbEBands;
  const opus_int16 *eBands;
  nbEBands = mode->nbEBands; eBands = mode->eBands;
  coded_bands = lastCodedBands ? lastCodedBands : nbEBands; coded_bins = eBands[coded_bands] << LM;
  if (C == 2) coded_bins += eBands[std::min(intensity, coded_bands)] << LM;
  target = base_target;
  if (C == 2) {
    int coded_stereo_bands, coded_stereo_dof;
    opus_val16 max_frac; coded_stereo_bands = std::min(intensity, coded_bands);
    coded_stereo_dof = (eBands[coded_stereo_bands] << LM) - coded_stereo_bands;
    max_frac = (((opus_val32)(((opus_val32)((0.8f)) * (opus_val32)(coded_stereo_dof)))) / (opus_val16)(coded_bins));
    stereo_saving = ((stereo_saving) < ((1.f)) ? (stereo_saving) : ((1.f)));
    target -= (opus_int32)((((max_frac) * (target))) < ((((opus_val32)(stereo_saving - (0.1f)) * (opus_val32)((coded_stereo_dof << 3))))) ? (((max_frac) * (target))) : ((((opus_val32)(stereo_saving - (0.1f)) * (opus_val32)((coded_stereo_dof << 3))))));
}
  target += tot_boost - (19 << LM); tf_calibration = (0.044f); target += (opus_int32)(((tf_estimate - tf_calibration) * (target)));
  {
    opus_int32 floor_depth;
    int bins = eBands[nbEBands - 2] << LM;
    floor_depth = (opus_int32)((((C * bins << 3)) * (maxDepth))); floor_depth = std::max(floor_depth, target >> 2);
    target = std::min(target, floor_depth);
}
  if (constrained_vbr) { target = base_target + (opus_int32)(((0.67f)) * (target - base_target));
}
  if (tf_estimate < (.2f)) {
    opus_val16 tvbr_factor;
    const opus_int32 rate_margin = std::max<opus_int32>(0, std::min<opus_int32>(32000, 96000 - bitrate));
    opus_val16 amount = (((.0000031f)) * rate_margin);
    tvbr_factor = (((opus_val32)((temporal_vbr)) * (opus_val32)(amount))); target += (opus_int32)((tvbr_factor) * (target));
}
  target = std::min(2 * base_target, target);
  return target;
}
struct celt_encode_layout {
  const CeltModeInternal *mode{};
  const opus_int16 *eBands{};
  int nbEBands{}, overlap{}, start{}, end{}, effEnd{}, LM{}, M{}, N{}, CC{}, C{}, hybrid{};
};
struct celt_input_peak {
  opus_val32 sample_max{};
  int silence{};
};
[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_measure_input_peak(CeltEncoderInternal *st, const opus_res *pcm, const celt_encode_layout &layout) noexcept -> celt_input_peak {
  auto peak = celt_input_peak{};
  peak.sample_max = std::max<opus_val32>(st->overlap_max, celt_maxabs16(pcm, layout.CC * (layout.N - layout.overlap) / st->upsample));
  st->overlap_max = celt_maxabs16(pcm + layout.CC * (layout.N - layout.overlap) / st->upsample, layout.CC * layout.overlap / st->upsample);
  peak.sample_max = std::max(peak.sample_max, st->overlap_max);
  peak.silence = peak.sample_max <= (opus_val16)1 / (1 << st->lsb_depth);
  return peak;
}
static OPUS_ALWAYS_INLINE void celt_preemphasise_input(CeltEncoderInternal *st, const opus_res *pcm, celt_sig *in, celt_sig *prefilter_mem, const celt_encode_layout &layout, opus_val32 sample_max) {
  const int need_clip = sample_max > 65536.f;
  for (int c = 0; c < layout.CC; ++c) {
    celt_preemphasis(pcm + c, in + c * (layout.N + layout.overlap) + layout.overlap, layout.N, layout.CC, st->upsample, layout.mode->preemph, st->preemph_memE + c, need_clip);
    copy_n_items(&prefilter_mem[(1 + c) * (1024) - layout.overlap], static_cast<std::size_t>(layout.overlap), in + c * (layout.N + layout.overlap));
  }
}
struct celt_tone_analysis {
  opus_val16 frequency{-1};
  opus_val32 toneishness{};
};
[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_analyse_tone(celt_sig *in, const celt_encode_layout &layout, opus_val16 tf_estimate) -> celt_tone_analysis {
  auto tone = celt_tone_analysis{};
  tone.frequency = tone_detect(in, layout.CC, layout.N + layout.overlap, &tone.toneishness, layout.mode->Fs);
  tone.toneishness = std::min(tone.toneishness, (1.f) - tf_estimate);
  return tone;
}
struct celt_prefilter_result {
  int pitch_index{15};
  int tapset{};
  int enabled{};
  opus_val16 gain{};
};
static OPUS_NOINLINE auto celt_encode_prefilter(CeltEncoderInternal *st, celt_sig *in, celt_sig *prefilter_mem, ec_enc *enc, const celt_encode_layout &layout, int nbAvailableBytes, opus_int32 total_bits, opus_int32 tell, int silence, opus_val16 tf_estimate, opus_val16 tone_freq, opus_val32 toneishness) -> celt_prefilter_result {
  auto result = celt_prefilter_result{};
  int qg = 0;
  const int can_signal = !layout.hybrid && tell + 16 <= total_bits;
  const int enabled = nbAvailableBytes > 12 * layout.C && can_signal && !silence && !st->disable_pf;
  result.enabled = run_prefilter(st, in, prefilter_mem, layout.CC, layout.N, result.tapset, &result.pitch_index, &result.gain, &qg, enabled, st->complexity, tf_estimate, nbAvailableBytes, tone_freq, toneishness);
  if (result.enabled == 0) {
    if (can_signal) ec_enc_bit_logp(enc, 0, 1);
    return result;
  }
  ec_enc_bit_logp(enc, 1, 1);
  result.pitch_index += 1;
  const int octave = (((int)sizeof(unsigned) * 8) - (__builtin_clz(result.pitch_index))) - 5;
  ec_enc_uint(enc, octave, 6);
  ec_enc_bits(enc, result.pitch_index - (16 << octave), 4 + octave);
  result.pitch_index -= 1;
  ec_enc_bits(enc, qg, 3);
  ec_enc_icdf(enc, result.tapset, shared_three_step_icdf.data(), 2);
  return result;
}
[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_choose_short_blocks(ec_enc *enc, int LM, opus_int32 total_bits, int isTransient, int *transient_got_disabled) noexcept -> int {
  if (LM > 0 && ec_tell(enc) + 3 <= total_bits) return isTransient ? (1 << LM) : 0;
  *transient_got_disabled = 1;
  return 0;
}
[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_update_temporal_vbr(CeltEncoderInternal *st, const celt_glog *bandLogE, const celt_encode_layout &layout, int shortBlocks) noexcept -> celt_glog {
  celt_glog follow = -(10.0f);
  opus_val32 frame_avg = 0;
  const celt_glog offset = shortBlocks ? (.5f * layout.LM) : 0;
  for (int i = layout.start; i < layout.end; ++i) {
    follow = std::max(follow - (1.0f), bandLogE[i] - offset);
    if (layout.C == 2) follow = std::max(follow, bandLogE[i + layout.nbEBands] - offset);
    frame_avg += follow;
  }
  frame_avg /= (layout.end - layout.start);
  auto temporal_vbr = frame_avg - st->spec_avg;
  temporal_vbr = clamp_value(temporal_vbr, -(1.5f), (3.f));
  st->spec_avg += (.02f) * temporal_vbr;
  return temporal_vbr;
}
static OPUS_ALWAYS_INLINE void celt_apply_energy_error_feedback(const celt_glog *oldBandE, celt_glog *bandLogE, const celt_glog *energyError, const celt_encode_layout &layout) noexcept {
  for (int c = 0; c < layout.C; ++c)
    for (int i = layout.start; i < layout.end; i++)
      if (((float)fabs(bandLogE[i + c * layout.nbEBands] - oldBandE[i + c * layout.nbEBands])) < 2.f) bandLogE[i + c * layout.nbEBands] -= 0.25f * energyError[i + c * layout.nbEBands];
}
static OPUS_ALWAYS_INLINE void celt_store_energy_error(celt_glog *energyError, const celt_glog *error, const celt_encode_layout &layout) noexcept {
  for (int c = 0; c < layout.C; ++c)
    for (int i = layout.start; i < layout.end; i++) energyError[i + c * layout.nbEBands] = clamp_value(error[i + c * layout.nbEBands], -(0.5f), (0.5f));
}
static OPUS_ENCODER_HUB_SIZE_OPT int celt_encode_with_ec(CeltEncoderInternal *st, const opus_res *pcm, int frame_size, unsigned char *compressed, int nbCompressedBytes, ec_enc *enc) {
  int N, LM, M, tf_select, nbFilledBytes, nbAvailableBytes, start, end, effEnd, codedBands, alloc_trim;
  int shortBlocks = 0, isTransient = 0, pitch_index = 15, dual_stereo = 0, effectiveBytes;
  int prefilter_tapset = 0, anti_collapse_rsv, anti_collapse_on = 0, silence = 0;
  int signalBandwidth, transient_got_disabled = 0, nbEBands, overlap;
  int hybrid, packet_size_cap = 1275;
  const int CC = st->channels, C = st->stream_channels;
  opus_int32 bits, min_allowed, vbr_rate, total_bits, total_boost, balance, tell, tell0_frac, tot_boost, equiv_rate;
  opus_val16 gain1 = 0, tf_estimate = 0, tone_freq = -1;
  opus_val32 sample_max, toneishness = 0;
  celt_glog maxDepth, temporal_vbr = 0;
  ec_enc _enc;
#if defined(OPUS_CODEC_TRACE_RATE)
  opus_int32 trace_base_target = 0, trace_raw_target = 0, trace_target_with_tell = 0, trace_vbr_rate = 0;
  opus_int32 trace_alloc_pulse_bits = 0, trace_alloc_fine_bits = 0, trace_tell_before_quant = 0, trace_tell_after_quant = 0;
#endif
  if (nbCompressedBytes < 2 || pcm == nullptr) return -1;
  frame_size *= st->upsample;
  const CeltModeInternal *mode = st->mode;
  const opus_int16 *eBands = mode->eBands;
  nbEBands = mode->nbEBands; overlap = mode->overlap;
  start = st->start; end = st->end; hybrid = start != 0;
  LM = celt_frame_lm(mode, frame_size);
  if (LM > mode->maxLM) return -1;
  M = 1 << LM; N = M * mode->shortMdctSize;
  const auto layout = celt_encode_layout{mode, eBands, nbEBands, overlap, start, end, celt_effective_end(mode, end), LM, M, N, CC, C, hybrid};
  const auto views = make_celt_encoder_views(st);
  auto *prefilter_mem = views.prefilter_mem;
  auto *oldBandE = views.oldBandE;
  auto *oldLogE = views.oldLogE;
  auto *oldLogE2 = views.oldLogE2;
  auto *energyError = views.energyError;
  // Packet accounting comes first because the later analysis phases consume the
  // final byte ceiling when deciding which side information can be signalled.
  if (enc == nullptr) { tell0_frac = tell = 1; nbFilledBytes = 0;
}
  else { tell0_frac = ec_tell_frac(enc); tell = ec_tell(enc); nbFilledBytes = (tell + 4) >> 3;
}
  nbCompressedBytes = std::min(nbCompressedBytes, packet_size_cap);
  if (st->vbr && st->bitrate != -1) { vbr_rate = bitrate_to_bits(st->bitrate, mode->Fs, frame_size) << 3; effectiveBytes = vbr_rate >> (3 + 3);
}
  else {
    vbr_rate = 0; opus_int32 tmp = st->bitrate * frame_size + (tell > 1 ? tell * mode->Fs : 0);
    if (st->bitrate != -1) { nbCompressedBytes = std::max(2, std::min(nbCompressedBytes, (tmp + 4 * mode->Fs) / (8 * mode->Fs))); if (enc != nullptr) ec_enc_shrink(enc, nbCompressedBytes);
}
    effectiveBytes = nbCompressedBytes - nbFilledBytes;
}
  nbAvailableBytes = nbCompressedBytes - nbFilledBytes;
  equiv_rate = ((opus_int32)nbCompressedBytes * 8 * 50 << (3 - LM)) - (40 * C + 20) * ((400 >> LM) - 50);
  if (st->bitrate != -1) equiv_rate = std::min(equiv_rate, st->bitrate - (40 * C + 20) * ((400 >> LM) - 50));
  if (enc == nullptr) { ec_enc_init(&_enc, compressed, nbCompressedBytes); enc = &_enc;
}
  if (vbr_rate > 0 && st->constrained_vbr) {
    opus_int32 vbr_bound = vbr_rate;
    opus_int32 max_allowed = std::min(std::max(tell == 1 ? 2 : 0, (vbr_rate + vbr_bound - st->vbr_reservoir) >> (3 + 3)), nbAvailableBytes);
    if (max_allowed < nbAvailableBytes) { nbCompressedBytes = nbFilledBytes + max_allowed; nbAvailableBytes = max_allowed; ec_enc_shrink(enc, nbCompressedBytes); }
}
  total_bits = nbCompressedBytes * 8; effEnd = layout.effEnd;
  // Time-domain preparation: measure silence, pre-emphasise, then decide the
  // optional pitch prefilter before the MDCT consumes the prepared buffer.
  auto *in = OPUS_SCRATCH(celt_sig, CC * (N + overlap));
  const auto peak = celt_measure_input_peak(st, pcm, layout);
  sample_max = peak.sample_max; silence = peak.silence;
  if (tell == 1) ec_enc_bit_logp(enc, silence, 15); else silence = 0;
  if (silence) {
    if (vbr_rate > 0) { effectiveBytes = nbCompressedBytes = std::min(nbCompressedBytes, nbFilledBytes + 2); total_bits = nbCompressedBytes * 8; nbAvailableBytes = 2; ec_enc_shrink(enc, nbCompressedBytes);
}
    tell = nbCompressedBytes * 8; enc->nbits_total += tell - ec_tell(enc);
}
  celt_preemphasise_input(st, pcm, in, prefilter_mem, layout, sample_max);
  const auto tone = celt_analyse_tone(in, layout, tf_estimate);
  tone_freq = tone.frequency; toneishness = tone.toneishness; isTransient = 0;
  const auto prefilter = celt_encode_prefilter(st, in, prefilter_mem, enc, layout, nbAvailableBytes, total_bits, tell, silence, tf_estimate, tone_freq, toneishness);
  [[maybe_unused]] const int pf_on = prefilter.enabled; pitch_index = prefilter.pitch_index; prefilter_tapset = prefilter.tapset; gain1 = prefilter.gain;
  shortBlocks = celt_choose_short_blocks(enc, LM, total_bits, isTransient, &transient_got_disabled);
  // Frequency-domain analysis creates the band energies and scratch workset
  // used by dynalloc, TF signalling, trim, and pulse allocation.
  auto *freq = OPUS_SCRATCH(celt_sig, CC * N);
  auto *bandE = OPUS_SCRATCH(celt_ener, nbEBands * CC);
  auto *bandLogE = OPUS_SCRATCH(celt_glog, nbEBands * CC);
  auto *bandLogE2 = OPUS_SCRATCH(celt_glog, C * nbEBands);
  compute_mdcts(mode, shortBlocks, in, freq, C, CC, LM, st->upsample);
  opus_assume(!((freq[0]) != (freq[0])) && (C == 1 || !((freq[N]) != (freq[N]))));
  compute_band_energies(mode, freq, bandE, effEnd, C, LM);
  amp2Log2(mode, effEnd, end, bandE, bandLogE, C);
  auto *surround_dynalloc = OPUS_SCRATCH(celt_glog, C * nbEBands);
  zero_n_items(surround_dynalloc, static_cast<std::size_t>(C * nbEBands));
  temporal_vbr = celt_update_temporal_vbr(st, bandLogE, layout, shortBlocks);
  copy_n_items(bandLogE, static_cast<std::size_t>(C * nbEBands), bandLogE2);
  if (LM > 0 && ec_tell(enc) + 3 <= total_bits) {
    ec_enc_bit_logp(enc, isTransient, 3);
}
  const auto band_count = static_cast<std::size_t>(nbEBands);
  auto *X = OPUS_SCRATCH(celt_norm, C * N);
  normalise_bands(mode, freq, X, bandE, effEnd, C, M);
  auto band_storage = std::span<int>{OPUS_SCRATCH(int, 8 * band_count), 8 * band_count};
  zero_n_items(band_storage.data(), band_storage.size());
  auto [offsets, importance, spread_weight, tf_res, cap, fine_quant, pulses, fine_priority] = partition_workset<8>(band_storage, band_count);
  maxDepth = dynalloc_analysis(bandLogE, bandLogE2, oldBandE, nbEBands, start, end, C, offsets.data(), st->lsb_depth, mode->logN, isTransient, st->vbr, st->constrained_vbr, eBands, LM, effectiveBytes, &tot_boost, surround_dynalloc, importance.data(), spread_weight.data(), tone_freq, toneishness);
  fill_n_items(tf_res.data(), static_cast<std::size_t>(end), 0); tf_select = 0;
  auto *error = OPUS_SCRATCH(celt_glog, C * nbEBands);
  zero_n_items(error, static_cast<std::size_t>(C * nbEBands));
  celt_apply_energy_error_feedback(oldBandE, bandLogE, energyError, layout);
  {
    quant_coarse_energy(mode, start, end, effEnd, bandLogE, oldBandE, total_bits, error, enc, C, LM, nbAvailableBytes, st->force_intra, &st->delayedIntra);
}
  tf_encode(start, end, isTransient, tf_res.data(), LM, tf_select, enc);
  if (ec_tell(enc) + 4 <= total_bits) {
    st->spread_decision = 0;
    ec_enc_icdf(enc, st->spread_decision, spread_icdf.data(), 5);
  } else st->spread_decision = (2);
  init_caps(mode, cap, LM, C);
  tell = celt_encode_dynalloc(enc, {eBands, static_cast<std::size_t>(nbEBands + 1)}, offsets, cap, start, end, C, LM, total_bits, total_boost);
  if (C == 2) {
    if (LM != 0) dual_stereo = stereo_analysis(mode, X, LM, N);
    st->intensity = hysteresis_decision((opus_val16)(equiv_rate / 1000), stereo_intensity_table.thresholds, stereo_intensity_table.hysteresis, st->intensity);
    st->intensity = clamp_value(st->intensity, start, end);
}
  alloc_trim = 5;
  if (tell + (6 << 3) <= total_bits - total_boost) {
    if (start > 0) { st->stereo_saving = 0; alloc_trim = 5;
}
    else alloc_trim = alloc_trim_analysis(mode, X, bandLogE, end, LM, C, N, &st->stereo_saving, tf_estimate, st->intensity, equiv_rate);
    {
      ec_enc_icdf(enc, alloc_trim, trim_icdf.data(), 7);
}
    tell = ec_tell_frac(enc);
}
  min_allowed = ((tell + total_boost + (1 << (3 + 3)) - 1) >> (3 + 3)) + 2;
  if (hybrid) min_allowed = std::max(min_allowed, (tell0_frac + (37 << 3) + total_boost + (1 << (3 + 3)) - 1) >> (3 + 3));
  auto base_target = opus_int32{0};
  auto target = opus_int32{0};
  // VBR target selection is intentionally kept as one block: it owns the
  // reservoir/drift state and is sensitive to bit-exact packet sizing.
  if (vbr_rate > 0) {
    opus_val16 alpha;
    opus_int32 delta;
    int lm_diff = mode->maxLM - LM;
    nbCompressedBytes = std::min(nbCompressedBytes, packet_size_cap >> (3 - LM));
    base_target = (!hybrid) ? (vbr_rate - ((40 * C + 20) << 3)) : std::max(0, vbr_rate - ((9 * C + 4) << 3));
    if (st->constrained_vbr) base_target += (st->vbr_offset >> lm_diff);
    if (!hybrid) target = compute_vbr(mode, base_target, LM, equiv_rate, st->lastCodedBands, C, st->intensity, st->constrained_vbr, st->stereo_saving, tot_boost, tf_estimate, maxDepth, temporal_vbr);
    else {
      target = celt_hybrid_target(base_target, LM, tf_estimate, st->silk_info.offset);
      const opus_val32 tone_gate = st->silk_info.bitrateBps >= 48000 ? .99f : .80f;
      if (C == 2 && st->silk_info.bitrateBps >= 32000 && st->silk_info.offset < 100 && toneishness > tone_gate) {
        const opus_int32 residual_celt_bps = st->silk_info.bitrateBps - st->silk_info.actualSilkBps;
        if (residual_celt_bps > st->bitrate) target += bitrate_to_bits(residual_celt_bps - st->bitrate, mode->Fs, frame_size) << 3;
      }
    }
#if defined(OPUS_CODEC_TRACE_RATE)
    trace_base_target = base_target;
    trace_raw_target = target;
    trace_vbr_rate = vbr_rate;
#endif
    target = target + tell; nbAvailableBytes = (target + (1 << (3 + 2))) >> (3 + 3);
#if defined(OPUS_CODEC_TRACE_RATE)
    trace_target_with_tell = target;
#endif
    nbAvailableBytes = std::max(min_allowed, nbAvailableBytes);
    nbAvailableBytes = std::min(nbCompressedBytes, nbAvailableBytes); delta = target - vbr_rate; target = nbAvailableBytes << (3 + 3);
    if (silence) { nbAvailableBytes = 2; target = 2 * 8 << 3; delta = 0;
}
    if (st->vbr_count < 970) { st->vbr_count++; alpha = (1.f / (((st->vbr_count + 20)))); } else alpha = (.001f);
    if (st->constrained_vbr) { st->vbr_reservoir += target - vbr_rate; st->vbr_drift += (opus_int32)((alpha) * ((delta * (1 << lm_diff)) - st->vbr_offset - st->vbr_drift)); st->vbr_offset = -st->vbr_drift;
}
    if (st->constrained_vbr && st->vbr_reservoir < 0) { int adjust = (-st->vbr_reservoir) / (8 << 3); nbAvailableBytes += silence ? 0 : adjust; st->vbr_reservoir = 0;
}
    nbCompressedBytes = std::min(nbCompressedBytes, nbAvailableBytes); ec_enc_shrink(enc, nbCompressedBytes);
}
  bits = (((opus_int32)nbCompressedBytes * 8) << 3) - (opus_int32)ec_tell_frac(enc) - 1;
  anti_collapse_rsv = celt_anti_collapse_reserve(isTransient, LM, bits); bits -= anti_collapse_rsv;
  signalBandwidth = end - 1;
  {
    codedBands = clt_compute_allocation(mode, start, end, offsets.data(), cap.data(), alloc_trim, &st->intensity, &dual_stereo, bits, &balance, pulses.data(), fine_quant.data(), fine_priority.data(), C, LM, enc, 1, st->lastCodedBands, signalBandwidth);
}
#if defined(OPUS_CODEC_TRACE_RATE)
  for (int i = start; i < end; ++i) {
    trace_alloc_pulse_bits += pulses[static_cast<std::size_t>(i)];
    trace_alloc_fine_bits += fine_quant[static_cast<std::size_t>(i)] * C;
}
#endif
  if (st->lastCodedBands) st->lastCodedBands = clamp_value(codedBands, st->lastCodedBands - 1, st->lastCodedBands + 1);
  else st->lastCodedBands = codedBands;
  // Quantisation writes the final spectral payload; the remaining steps only
  // close side-channel reserves and commit predictor state for the next frame.
  {
    quant_fine_energy(mode, start, end, oldBandE, error, nullptr, fine_quant.data(), enc, C);
}
  zero_n_items(energyError, static_cast<std::size_t>(nbEBands * CC));
  auto *collapse_masks = OPUS_SCRATCH(unsigned char, C * nbEBands);
  {
#if defined(OPUS_CODEC_TRACE_RATE)
    trace_tell_before_quant = ec_tell_frac(enc);
    opus_trace_pvq = {};
#endif
    quant_all_bands(1, mode, start, end, X, C == 2 ? X + N : nullptr, collapse_masks, bandE, pulses.data(), shortBlocks, st->spread_decision, dual_stereo, st->intensity, tf_res.data(), nbCompressedBytes * (8 << 3) - anti_collapse_rsv, balance, enc, LM, codedBands, &st->rng, st->disable_inv
    );
#if defined(OPUS_CODEC_TRACE_RATE)
    trace_tell_after_quant = ec_tell_frac(enc);
#endif
}
  if (anti_collapse_rsv > 0) {
    anti_collapse_on = st->consec_transient < 2; ec_enc_bits(enc, anti_collapse_on, 1);
}
  quant_energy_finalise(mode, start, end, oldBandE, error, fine_quant.data(), fine_priority.data(), nbCompressedBytes * 8 - ec_tell(enc), enc, C);
  celt_store_energy_error(energyError, error, layout);
if (silence) { fill_n_items(oldBandE, static_cast<std::size_t>(C * nbEBands), -(28.f));
}
  st->prefilter_period = pitch_index; st->prefilter_gain = gain1; st->prefilter_tapset = prefilter_tapset;
  celt_commit_band_state(
      {oldBandE, static_cast<std::size_t>(CC * nbEBands)}, {oldLogE, static_cast<std::size_t>(CC * nbEBands)}, {oldLogE2, static_cast<std::size_t>(CC * nbEBands)},
      CC, nbEBands, start, end, isTransient, CC == 2 && C == 1);
  if (isTransient || transient_got_disabled) { st->consec_transient++; } else { st->consec_transient = 0; } st->rng = enc->rng;
#if defined(OPUS_CODEC_TRACE_RATE)
  OPUS_TRACE_RATE("opus_rate mode=%d hybrid=%d Fs=%d CC=%d C=%d LM=%d vbr=%d total_bitrate=%d celt_bitrate=%d silk_bitrate=%d silk_signal=%d silk_offset=%d vbr_rate_q3=%d base_target_q3=%d raw_target_q3=%d target_with_tell_q3=%d available_bits_q3=%d final_bits_q3=%d tell_before_quant_q3=%d tell_after_quant_q3=%d quant_consumed_q3=%d alloc_pulse_units=%d alloc_fine_units=%d codedBands=%d intensity=%d dual=%d alloc_trim=%d total_boost=%d tot_boost=%d anti_collapse=%d final_packet_bytes=%d reservoir=%d drift=%d tone=%.6f tone_freq=%.6f transient=%d shortBlocks=%d pf=%d gain=%.6f pvq_partitions=%d pvq_splits=%d pvq_q0=%d pvq_k1=%d pvq_k2=%d pvq_kn=%d\n",
                  hybrid ? opus_mode_hybrid : opus_mode_celt_only, hybrid, mode->Fs, CC, C, LM, st->vbr, st->silk_info.bitrateBps, st->bitrate, hybrid ? st->silk_info.bitrateBps - st->bitrate : 0,
                  st->silk_info.signalType, st->silk_info.offset, trace_vbr_rate, trace_base_target, trace_raw_target, trace_target_with_tell,
                  nbAvailableBytes * (8 << 3), nbCompressedBytes * (8 << 3), trace_tell_before_quant, trace_tell_after_quant,
                  trace_tell_after_quant - trace_tell_before_quant, trace_alloc_pulse_bits, trace_alloc_fine_bits, codedBands, st->intensity, dual_stereo,
                  alloc_trim, total_boost, tot_boost, anti_collapse_rsv, nbCompressedBytes, st->vbr_reservoir, st->vbr_drift,
                  static_cast<double>(toneishness), static_cast<double>(tone_freq), isTransient, shortBlocks, pf_on, static_cast<double>(gain1),
                  opus_trace_pvq.partitions, opus_trace_pvq.splits, opus_trace_pvq.q0, opus_trace_pvq.k1, opus_trace_pvq.k2, opus_trace_pvq.kn);
#endif
  ec_enc_done(enc);
  if (ec_get_error(enc)) return -3;
  else return nbCompressedBytes;
}
[[nodiscard]] static auto try_set_custom_encoder_bitrate(CeltEncoderInternal *st, opus_int32 value) noexcept -> bool {
  if (value <= 500 && value != -1) { return false;
}
  st->bitrate = std::min(value, static_cast<opus_int32>(750000 * st->channels));
  return true;
}
static void celt_encoder_reset_state(CeltEncoderInternal *st) {
  const auto views = make_celt_encoder_views(st);
  const auto band_count = static_cast<std::size_t>(st->channels * st->mode->nbEBands);
  zero_n_items(reinterpret_cast<char *>(&st->rng), static_cast<std::size_t>(celt_encoder_get_size(st->channels) - (reinterpret_cast<char *>(&st->rng) - reinterpret_cast<char *>(st))));
  fill_n_items(views.oldLogE, band_count, -(28.f));
  fill_n_items(views.oldLogE2, band_count, -(28.f)); st->vbr_offset = 0;
  st->delayedIntra = 1; st->spread_decision = 2;
}
static int celt_encoder_set_complexity(CeltEncoderInternal *st, opus_int32 value) {
  if (value < 0 || value > 10) return OPUS_BAD_ARG;
  st->complexity = static_cast<int>(value); return OPUS_OK;
}
static int celt_encoder_set_start_band(CeltEncoderInternal *st, opus_int32 value) {
  if (value < 0 || value > st->mode->nbEBands - 1) return OPUS_BAD_ARG;
  st->start = static_cast<int>(value); return OPUS_OK;
}
static int celt_encoder_set_end_band(CeltEncoderInternal *st, opus_int32 value) {
  if (value < 1 || value > st->mode->nbEBands) return OPUS_BAD_ARG;
  st->end = static_cast<int>(value); return OPUS_OK;
}
static int celt_encoder_set_stream_channels(CeltEncoderInternal *st, opus_int32 value) {
  if (value < 1 || value > 2) return OPUS_BAD_ARG;
  st->stream_channels = static_cast<int>(value); return OPUS_OK;
}
static int celt_encoder_set_lsb_depth(CeltEncoderInternal *st, opus_int32 value) {
  if (value < 8 || value > 24) return OPUS_BAD_ARG;
  st->lsb_depth = static_cast<int>(value); return OPUS_OK;
}
static void celt_encoder_set_vbr(CeltEncoderInternal *st, opus_int32 value) { st->vbr = static_cast<int>(value); }
static void celt_encoder_set_constrained_vbr(CeltEncoderInternal *st, opus_int32 value) { st->constrained_vbr = static_cast<int>(value); }
static int celt_encoder_set_prediction(CeltEncoderInternal *st, opus_int32 value) {
  if (value < 0 || value > 2) return OPUS_BAD_ARG;
  st->disable_pf = value <= 1; st->force_intra = value == 0; return OPUS_OK;
}
static int celt_encoder_set_bitrate(CeltEncoderInternal *st, opus_int32 value) { return try_set_custom_encoder_bitrate(st, value) ? OPUS_OK : OPUS_BAD_ARG; }
static void celt_encoder_set_silk_info(CeltEncoderInternal *st, const SILKInfo *info) { if (info) copy_n_items(info, std::size_t{1}, &st->silk_info); }
[[nodiscard]] static opus_uint32 celt_encoder_final_range(const CeltEncoderInternal *st) noexcept { return st->rng; }
[[nodiscard]] static const CeltModeInternal *celt_encoder_mode(const CeltEncoderInternal *st) noexcept { return st->mode; }
struct CeltDecoderInternal {
  const CeltModeInternal *mode; int overlap, channels, stream_channels, downsample, start, end, disable_inv, error, last_pitch_index, loss_duration, plc_duration, last_frame_type, skip_plc, postfilter_period, postfilter_period_old, postfilter_tapset, postfilter_tapset_old, prefilter_and_fold;
  opus_uint32 rng; opus_val16 postfilter_gain, postfilter_gain_old; celt_sig preemph_memD[2], _decode_mem[1];
};
static int celt_decoder_set_start_band(CeltDecoderInternal *st, opus_int32 value) {
  if (value < 0 || value > st->mode->nbEBands - 1) return OPUS_BAD_ARG;
  st->start = value; return OPUS_OK;
}
static int celt_decoder_set_end_band(CeltDecoderInternal *st, opus_int32 value) {
  if (value < 1 || value > st->mode->nbEBands) return OPUS_BAD_ARG;
  st->end = value; return OPUS_OK;
}
static int celt_decoder_set_stream_channels(CeltDecoderInternal *st, opus_int32 value) {
  if (value < 1 || value > 2) return OPUS_BAD_ARG;
  st->stream_channels = value; return OPUS_OK;
}
[[nodiscard]] static opus_uint32 celt_decoder_final_range(const CeltDecoderInternal *st) noexcept { return st->rng; }
[[nodiscard]] static const CeltModeInternal *celt_decoder_mode(const CeltDecoderInternal *st) noexcept { return st->mode; }
inline constexpr int celt_decoder_energy_channel_count = 2;
[[nodiscard]] static constexpr auto celt_decoder_storage_size(const int channels) noexcept -> int {
  return sizeof(CeltDecoderInternal) + (channels * (celt_decoder_history_size + celt_default_overlap) - 1) * static_cast<int>(sizeof(celt_sig)) +
         4 * celt_decoder_energy_channel_count * celt_default_nb_ebands * static_cast<int>(sizeof(celt_glog)) + channels * 24 * static_cast<int>(sizeof(opus_val16));
}
static OPUS_COLD OPUS_NOINLINE int celt_decoder_get_size(int channels) { return celt_decoder_storage_size(channels); }
static void celt_decoder_reset_state(CeltDecoderInternal *st) {
  constexpr auto energy_channels = celt_decoder_energy_channel_count;
  auto *old_band_energy = reinterpret_cast<celt_glog *>(st->_decode_mem + (celt_decoder_history_size + st->overlap) * st->channels);
  auto *old_log_energy = old_band_energy + energy_channels * st->mode->nbEBands;
  auto *old_log_energy2 = old_log_energy + energy_channels * st->mode->nbEBands;
  const auto band_count = static_cast<std::size_t>(energy_channels * st->mode->nbEBands);
  zero_n_items(reinterpret_cast<char *>(&st->rng), static_cast<std::size_t>(celt_decoder_get_size(st->channels) - (reinterpret_cast<char *>(&st->rng) - reinterpret_cast<char *>(st))));
  fill_n_items(old_log_energy, band_count, -(28.f));
  fill_n_items(old_log_energy2, band_count, -(28.f)); st->skip_plc = 1; st->last_frame_type = 0;
}
static OPUS_COLD OPUS_NOINLINE int celt_decoder_init(CeltDecoderInternal *st, opus_int32 sampling_rate, int channels) { if (const auto ret = celt_decoder_init_state(st, default_custom_mode(), channels); ret != 0) return ret; st->downsample = resampling_factor(sampling_rate); return st->downsample == 0 ? -1 : 0; }
static int celt_decoder_init_state(CeltDecoderInternal *st, const CeltModeInternal *mode, int channels) {
  if (channels < 0 || channels > 2) return -1;
  if (st == nullptr) return -7;
  zero_n_items((char *)st, static_cast<std::size_t>(celt_decoder_get_size(channels))); st->mode = mode;
  st->overlap = mode->overlap; st->stream_channels = st->channels = channels; st->downsample = 1; st->start = 0;
  st->end = st->mode->effEBands; st->disable_inv = channels == 1;
  celt_decoder_reset_state(st);
  return 0;
}
static void deemphasis_stereo_simple(celt_sig *x0, celt_sig *x1, opus_res *pcm, int N, const opus_val16 coef0, celt_sig *mem) {
  celt_sig m0, m1;
  int j;
  m0 = mem[0]; m1 = mem[1];
  for (j = 0; j < N; j++) { celt_sig tmp0 = x0[j]+1e-30f+m0, tmp1 = x1[j]+1e-30f+m1; m0 = coef0*tmp0; m1 = coef0*tmp1; pcm[2*j] = (1/32768.f)*tmp0; pcm[2*j+1] = (1/32768.f)*tmp1; }
  mem[0] = m0; mem[1] = m1;
}
static void deemphasis_mono_simple(celt_sig *x, opus_res *pcm, int N, const opus_val16 coef0, celt_sig *mem, int accum) {
  celt_sig m = mem[0];
  if (accum) {
    for (int j = 0; j < N; ++j) { const celt_sig tmp = x[j] + 1e-30f + m; m = coef0 * tmp; pcm[j] += (1 / 32768.f) * tmp; }
  } else {
    for (int j = 0; j < N; ++j) { const celt_sig tmp = x[j] + 1e-30f + m; m = coef0 * tmp; pcm[j] = (1 / 32768.f) * tmp; }
  }
  mem[0] = m;
}
#if OPUS_GCC_DECODE_FASTPATH
static void deemphasis_mono_i16_simple(celt_sig *x, opus_int16 *pcm, int N, const opus_val16 coef0, celt_sig *mem) {
  celt_sig m = mem[0];
  for (int j = 0; j < N; ++j) {
    const celt_sig tmp = x[j] + 1e-30f + m;
    m = coef0 * tmp;
    const opus_res sample = (1 / 32768.f) * tmp;
    pcm[j] = FLOAT2INT16(sample);
  }
  mem[0] = m;
}
static void deemphasis_stereo_i16_simple(celt_sig *x0, celt_sig *x1, opus_int16 *pcm, int N, const opus_val16 coef0, celt_sig *mem) {
  celt_sig m0 = mem[0], m1 = mem[1];
  for (int j = 0; j < N; ++j) {
    const celt_sig tmp0 = x0[j] + 1e-30f + m0, tmp1 = x1[j] + 1e-30f + m1;
    m0 = coef0 * tmp0; m1 = coef0 * tmp1;
    const opus_res sample0 = (1 / 32768.f) * tmp0, sample1 = (1 / 32768.f) * tmp1;
    pcm[2 * j] = FLOAT2INT16(sample0); pcm[2 * j + 1] = FLOAT2INT16(sample1);
  }
  mem[0] = m0; mem[1] = m1;
}
static void deemphasis_i16(std::span<celt_sig *const> in, opus_int16 *pcm, int N, int C, int downsample, const opus_val16 *coef, celt_sig *mem) {
  if (downsample != 1) return;
  if (C == 1) { deemphasis_mono_i16_simple(in[0], pcm, N, coef[0], mem); return; }
  if (C == 2) deemphasis_stereo_i16_simple(in[0], in[1], pcm, N, coef[0], mem);
}
#endif
static void deemphasis(std::span<celt_sig *const> in, opus_res *pcm, int N, int C, int downsample, const opus_val16 *coef, celt_sig *mem, int accum) {
  int c, Nd;
  int apply_downsampling = 0;
  opus_val16 coef0;
  if (downsample == 1 && C == 1) { deemphasis_mono_simple(in[0], pcm, N, coef[0], mem, accum); return; }
  if (downsample == 1 && C == 2 && !accum) { deemphasis_stereo_simple(in[0], in[1], pcm, N, coef[0], mem); return; }
  auto *scratch = static_cast<celt_sig *>(nullptr);
  if (downsample > 1) { scratch = OPUS_SCRATCH(celt_sig, N); }
  coef0 = coef[0]; Nd = N / downsample;
  for (c = 0; c < C; ++c) {
    int j;
    celt_sig *x; opus_res *y;
    celt_sig m = mem[c]; x = in[c];
    y = pcm + c;
    if (downsample > 1) {
      for (j = 0; j < N; j++) { celt_sig tmp = x[j]+1e-30f+m; m = coef0*tmp; scratch[j] = tmp; }
      apply_downsampling = 1;
    } else if (accum) {
      for (j = 0; j < N; j++) { celt_sig tmp = x[j]+m+1e-30f; m = coef0*tmp; y[j*C] = y[j*C]+(1/32768.f)*tmp; }
    } else { for (j = 0; j < N; j++) { celt_sig tmp = x[j]+1e-30f+m; m = coef0*tmp; y[j*C] = (1/32768.f)*tmp; } }
    mem[c] = m;
    if (apply_downsampling) {
      if (accum) { for (j = 0; j < Nd; j++) y[j * C] = ((y[j * C]) + (((1 / 32768.f) * (scratch[j * downsample])))); }
      else { for (j = 0; j < Nd; j++) y[j * C] = ((1 / 32768.f) * (scratch[j * downsample])); }
    }
  }
}
static void celt_synthesis(const CeltModeInternal *mode, celt_norm *X, std::span<celt_sig *const> out_syn, celt_glog *oldBandE, int start, int effEnd, int C, int CC, int isTransient, int LM, int downsample, int silence) {
  int c, i, M, b, B, N, NB, shift, nbEBands, overlap;
  overlap = mode->overlap; nbEBands = mode->nbEBands; N = mode->shortMdctSize << LM;
  M = 1 << LM;
  if (isTransient) {
    B = M; NB = mode->shortMdctSize; shift = mode->maxLM;
  } else { B = 1; NB = mode->shortMdctSize << LM; shift = mode->maxLM - LM; }
  if (CC == C) {
    for (c = 0; c < CC; ++c) {
      auto *freq = X + c * N;
      denormalise_bands(mode, X + c * N, freq, oldBandE + c * nbEBands, start, effEnd, M, downsample, silence);
      for (b = 0; b < B; b++) clt_mdct_backward_c(&mode->mdct, &freq[b], out_syn[c] + NB * b, mode->window, overlap, shift, B);
    }
  } else if (CC == 2) {
    auto *freq = X;
    denormalise_bands(mode, X, freq, oldBandE, start, effEnd, M, downsample, silence);
    for (b = 0; b < B; b++) clt_mdct_backward_dual_history_c(&mode->mdct, &freq[b], out_syn[0] + NB * b, out_syn[1] + NB * b, mode->window, overlap, shift, B);
  } else {
    auto *freq = X;
    celt_sig *freq2; freq2 = out_syn[0] + overlap / 2;
    denormalise_bands(mode, X, freq, oldBandE, start, effEnd, M, downsample, silence);
    denormalise_bands(mode, X + N, freq2, oldBandE + nbEBands, start, effEnd, M, downsample, silence);
    for (i = 0; i < N; i++) freq[i] = (((.5f * (freq[i]))) + ((.5f * (freq2[i]))));
    for (b = 0; b < B; b++) clt_mdct_backward_c(&mode->mdct, &freq[b], out_syn[0] + NB * b, mode->window, overlap, shift, B);
  }
}

static void tf_decode(int start, int end, int isTransient, int *tf_res, int LM, ec_dec *dec) {
  int i, curr, tf_select, tf_select_rsv, tf_changed, logp;
  opus_uint32 budget, tell; budget = dec->storage * 8;
  tell = ec_tell(dec); logp = isTransient ? 2 : 4;
  tf_select_rsv = LM > 0 && tell + logp + 1 <= budget; budget -= tf_select_rsv; tf_changed = curr = 0;
  for (i = start; i < end; i++) {
    if (tell + logp <= budget) { curr ^= ec_dec_bit_logp(dec, logp); tell = ec_tell(dec); tf_changed |= curr; }
    tf_res[i] = curr; logp = isTransient ? 4 : 5;
  }
  tf_select = 0;
  if (tf_select_rsv && tf_select_table[LM][4 * isTransient + 0 + tf_changed] != tf_select_table[LM][4 * isTransient + 2 + tf_changed]) { tf_select = ec_dec_bit_logp(dec, 1); }
  for (i = start; i < end; i++) { tf_res[i] = tf_select_table[LM][4 * isTransient + 2 * tf_select + tf_res[i]]; }
}
constexpr int celt_decode_buffer_size = 2048, celt_plc_max_period = 1024, celt_lpc_order = 24;
struct celt_decoder_views {
  std::array<celt_sig *, 2> decode_mem, out_syn;
  celt_glog *oldBandE{}, *oldLogE{}, *oldLogE2{}, *backgroundLogE{};
  opus_val16 *lpc{};
};
[[nodiscard]] static auto make_celt_decoder_views(CeltDecoderInternal *st, int N) noexcept -> celt_decoder_views {
  const auto overlap = st->mode->overlap; celt_decoder_views views{};
  constexpr auto energy_channels = celt_decoder_energy_channel_count;
  for (int channel = 0; channel < st->channels; ++channel) {
    views.decode_mem[channel] = st->_decode_mem + channel * (celt_decode_buffer_size + overlap);
    views.out_syn[channel] = views.decode_mem[channel] + celt_decode_buffer_size - N;
  }
  views.oldBandE = reinterpret_cast<celt_glog *>(st->_decode_mem + (celt_decode_buffer_size + overlap) * st->channels);
  views.oldLogE = views.oldBandE + energy_channels * st->mode->nbEBands; views.oldLogE2 = views.oldLogE + energy_channels * st->mode->nbEBands;
  views.backgroundLogE = views.oldLogE2 + energy_channels * st->mode->nbEBands; views.lpc = reinterpret_cast<opus_val16 *>(views.backgroundLogE + energy_channels * st->mode->nbEBands);
  return views;
}
static void celt_slide_decode_history(std::span<celt_sig *const> decode_mem, int channels, int N, int overlap) {
  for (int channel = 0; channel < channels; ++channel) move_n_items(decode_mem[channel] + N, static_cast<std::size_t>(celt_decode_buffer_size - N + overlap), decode_mem[channel]);
}
static void celt_apply_postfilter(CeltDecoderInternal *st, std::span<celt_sig *const> out_syn, int channels, int N, int LM, int overlap, int target_period, opus_val16 target_gain, int target_tapset) {
  if (st->postfilter_gain_old == 0 && st->postfilter_gain == 0 && target_gain == 0) {
    st->postfilter_period_old = target_period;
    st->postfilter_period = target_period;
    st->postfilter_gain_old = st->postfilter_gain = 0;
    st->postfilter_tapset_old = target_tapset;
    st->postfilter_tapset = target_tapset;
    return;
  }
  const auto *mode = st->mode;
  for (int channel = 0; channel < channels; ++channel) {
    st->postfilter_period = std::max(st->postfilter_period, 15); st->postfilter_period_old = std::max(st->postfilter_period_old, 15);
    comb_filter(out_syn[channel], out_syn[channel], st->postfilter_period_old, st->postfilter_period, mode->shortMdctSize, st->postfilter_gain_old, st->postfilter_gain, st->postfilter_tapset_old, st->postfilter_tapset, mode->window, overlap);
    if (LM != 0) comb_filter(out_syn[channel] + mode->shortMdctSize, out_syn[channel] + mode->shortMdctSize, st->postfilter_period, target_period, N - mode->shortMdctSize, st->postfilter_gain, target_gain, st->postfilter_tapset, target_tapset, mode->window, overlap);
  }
  st->postfilter_period_old = st->postfilter_period; st->postfilter_gain_old = st->postfilter_gain; st->postfilter_tapset_old = st->postfilter_tapset;
  st->postfilter_period = target_period; st->postfilter_gain = target_gain; st->postfilter_tapset = target_tapset;
  if (LM != 0) { st->postfilter_period_old = st->postfilter_period; st->postfilter_gain_old = st->postfilter_gain; st->postfilter_tapset_old = st->postfilter_tapset; }
}
static void celt_plc_extrapolate_channel(celt_sig *buf, opus_val16 *lpc, const celt_coef *window, int overlap, int N, int pitch_index, int exc_length, opus_val16 fade, bool update_lpc) {
  std::array<opus_val16, celt_plc_max_period + celt_lpc_order> exc_storage; std::array<opus_val16, celt_plc_max_period> fir_tmp; std::array<opus_val16, celt_lpc_order> lpc_mem; auto *exc = exc_storage.data() + celt_lpc_order;
  opus_val16 decay, attenuation; opus_val32 S1 = 0;
    copy_n_items(buf + celt_decode_buffer_size - celt_plc_max_period - celt_lpc_order, static_cast<std::size_t>(celt_plc_max_period + celt_lpc_order), exc_storage.data());
  if (update_lpc) {
    std::array<opus_val32, celt_lpc_order + 1> ac; _celt_autocorr(exc, ac.data(), window, overlap, celt_lpc_order, celt_plc_max_period);
    ac[0] *= 1.0001f; for (int i = 1; i <= celt_lpc_order; ++i) ac[i] -= ac[i] * (0.008f * 0.008f) * i * i; _celt_lpc(lpc, ac.data(), celt_lpc_order);
  }
  celt_fir_c(exc + celt_plc_max_period - exc_length, lpc, fir_tmp.data(), exc_length, celt_lpc_order);
  copy_n_items(fir_tmp.data(), static_cast<std::size_t>(exc_length), exc + celt_plc_max_period - exc_length);
  {
    opus_val32 E1 = 1, E2 = 1; const auto decay_length = exc_length >> 1;
    for (int i = 0; i < decay_length; ++i) {
      auto sample = exc[celt_plc_max_period - decay_length + i]; E1 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
      sample = exc[celt_plc_max_period - 2 * decay_length + i]; E2 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
    }
    E1 = std::min(E1, E2); decay = static_cast<opus_val16>(std::sqrt(static_cast<float>(E1) / E2));
  }
  move_n_items(buf + N, static_cast<std::size_t>(celt_decode_buffer_size - N), buf);
  const auto extrapolation_offset = celt_plc_max_period - pitch_index, extrapolation_len = N + overlap; attenuation = fade * decay;
  for (int i = 0, j = 0; i < extrapolation_len; ++i, ++j) {
    if (j >= pitch_index) { j -= pitch_index; attenuation *= decay; }
    buf[celt_decode_buffer_size - N + i] = attenuation * exc[extrapolation_offset + j];
    const auto sample = buf[celt_decode_buffer_size - celt_plc_max_period - N + extrapolation_offset + j]; S1 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
  }
  for (int i = 0; i < celt_lpc_order; ++i) lpc_mem[i] = buf[celt_decode_buffer_size - N - 1 - i];
  celt_iir(buf + celt_decode_buffer_size - N, lpc, buf + celt_decode_buffer_size - N, extrapolation_len, celt_lpc_order, lpc_mem.data());
  {
    opus_val32 S2 = 0;
    for (int i = 0; i < extrapolation_len; ++i) { const auto sample = buf[celt_decode_buffer_size - N + i]; S2 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample); }
  if (!(S1 > 0.2f * S2)) zero_n_items(buf + celt_decode_buffer_size - N, static_cast<std::size_t>(extrapolation_len));
    else if (S1 < S2) {
      const auto ratio = static_cast<opus_val16>(std::sqrt(static_cast<float>(S1 + 1) / (S2 + 1)));
      for (int i = 0; i < overlap; ++i) { const auto gain = 1.0f - window[i] * (1.0f - ratio); buf[celt_decode_buffer_size - N + i] *= gain; }
      for (int i = overlap; i < extrapolation_len; ++i) buf[celt_decode_buffer_size - N + i] *= ratio;
    }
  }
}
static int celt_plc_pitch_search(CeltDecoderInternal *st, std::span<celt_sig *const> decode_mem) {
  int pitch_index;
  std::array<opus_val16, (2048 >> 1)> lp_pitch_buf;
  (void)st; pitch_downsample(decode_mem, lp_pitch_buf.data(), 2048 >> 1, 2);
  pitch_search(lp_pitch_buf.data() + ((720) >> 1), lp_pitch_buf.data(), 2048 - (720), (720) - (100), &pitch_index);
  pitch_index = (720) - pitch_index;
  return (pitch_index);
}
static void prefilter_and_fold(CeltDecoderInternal *st, int N) {
  const auto overlap = st->overlap, channels = st->channels; const auto decoder = make_celt_decoder_views(st, N);
  auto *etmp = OPUS_SCRATCH(opus_val32, overlap);
  for (int channel = 0; channel < channels; ++channel) {
    comb_filter(etmp, decoder.decode_mem[channel] + celt_decode_buffer_size - N, st->postfilter_period_old, st->postfilter_period, overlap, -st->postfilter_gain_old, -st->postfilter_gain, st->postfilter_tapset_old, st->postfilter_tapset, nullptr, 0);
    for (int i = 0; i < overlap / 2; ++i) decoder.decode_mem[channel][celt_decode_buffer_size - N + i] = (st->mode->window[i] * etmp[overlap - 1 - i]) + (st->mode->window[overlap - i - 1] * etmp[i]);
  }
}
static void celt_decode_lost(CeltDecoderInternal *st, int N, int LM) {
  int c, i;
  const int C = st->channels;
  const CeltModeInternal *mode;
  int nbEBands, overlap, start, loss_duration, curr_frame_type;
  const opus_int16 *eBands;
  mode = st->mode; nbEBands = mode->nbEBands;
  overlap = mode->overlap; eBands = mode->eBands;
  const auto decoder = make_celt_decoder_views(st, N); auto decode_mem = decoder.decode_mem; auto out_syn = decoder.out_syn;
  auto *oldBandE = decoder.oldBandE, *backgroundLogE = decoder.backgroundLogE, *lpc = decoder.lpc; loss_duration = st->loss_duration; start = st->start; curr_frame_type = 3;
  if (st->plc_duration >= 40 || start != 0 || st->skip_plc) curr_frame_type = 2;
  if (curr_frame_type == 2) {
    opus_uint32 seed;
    int end, effEnd;
    celt_glog decay; end = st->end;
    effEnd = std::max(start, std::min(end, mode->effEBands));
    auto *X = OPUS_SCRATCH(celt_norm, C * N);
  zero_n_items(X, static_cast<std::size_t>(C * N));
    celt_slide_decode_history(std::span<celt_sig *const>{decode_mem.data(), static_cast<std::size_t>(C)}, C, N, overlap);
    if (st->prefilter_and_fold) { prefilter_and_fold(st, N); }
    decay = loss_duration == 0 ? (1.5f) : (.5f);
    for (c = 0; c < C; ++c) { for (i = start; i < end; i++) oldBandE[c * nbEBands + i] = std::max(backgroundLogE[c * nbEBands + i], oldBandE[c * nbEBands + i] - decay); }
    seed = st->rng;
    for (c = 0; c < C; c++) {
      for (i = start; i < effEnd; i++) {
        int j, boffs, blen;
        boffs = N * c + (eBands[i] << LM); blen = (eBands[i + 1] - eBands[i]) << LM;
        for (j = 0; j < blen; j++) { seed = celt_lcg_rand(seed); X[boffs + j] = ((celt_norm)((opus_int32)seed >> 20)); }
        renormalise_vector(X + boffs, blen, 1.0f);
      }
    }
    st->rng = seed;
    celt_synthesis(mode, X, std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(C)}, oldBandE, start, effEnd, C, C, 0, LM, st->downsample, 0);
    celt_apply_postfilter(st, std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(C)}, C, N, LM, overlap, st->postfilter_period, st->postfilter_gain, st->postfilter_tapset);
    st->prefilter_and_fold = 0; st->skip_plc = 1;
  } else {
    opus_val16 fade = 1.0f;
    int pitch_index, curr_neural, last_neural;
    curr_neural = curr_frame_type == 4 || curr_frame_type == 5; last_neural = st->last_frame_type == 4 || st->last_frame_type == 5;
    if (st->last_frame_type != 3 && !(last_neural && curr_neural)) {
      st->last_pitch_index = pitch_index = celt_plc_pitch_search(st, std::span<celt_sig *const>{decode_mem.data(), static_cast<std::size_t>(C)});
    } else { pitch_index = st->last_pitch_index; fade = (.8f); }
    const auto exc_length = std::min(2 * pitch_index, celt_plc_max_period); const auto update_lpc = st->last_frame_type != 3 && !(last_neural && curr_neural);
    for (c = 0; c < C; ++c) celt_plc_extrapolate_channel(decode_mem[c], lpc + c * celt_lpc_order, mode->window, overlap, N, pitch_index, exc_length, fade, update_lpc);
    st->prefilter_and_fold = 1;
  }
  st->loss_duration = ((10000) < (loss_duration + (1 << LM)) ? (10000) : (loss_duration + (1 << LM)));
  st->plc_duration = ((10000) < (st->plc_duration + (1 << LM)) ? (10000) : (st->plc_duration + (1 << LM)));
  st->last_frame_type = curr_frame_type;
}
static int celt_decode_with_ec(CeltDecoderInternal *st, const unsigned char *data, int len, opus_res *pcm, int frame_size, ec_dec *dec, int accum
#if OPUS_GCC_DECODE_FASTPATH
                               , opus_int16 *pcm16
#endif
) {
  int c, i, N;
  int spread_decision;
  opus_int32 bits; ec_dec _dec;
  int shortBlocks, isTransient, intra_ener;
  const int CC = st->channels;
  int LM, M;
  int start, end, effEnd, codedBands, alloc_trim, postfilter_pitch;
  opus_val16 postfilter_gain;
  int intensity = 0, dual_stereo = 0;
  opus_int32 total_bits, balance, tell;
  int postfilter_tapset, anti_collapse_rsv;
  int anti_collapse_on = 0, silence, C = st->stream_channels;
  const CeltModeInternal *mode;
  int nbEBands, overlap;
  const opus_int16 *eBands;
  celt_glog max_background_increase;
  mode = st->mode; nbEBands = mode->nbEBands; overlap = mode->overlap; eBands = mode->eBands; start = st->start; end = st->end;
  frame_size *= st->downsample;
  LM = celt_frame_lm(mode, frame_size);
  if (LM > mode->maxLM) return -1;
  M = 1 << LM;
  if (len < 0 || len > 1275
#if OPUS_GCC_DECODE_FASTPATH
      || (pcm == nullptr && pcm16 == nullptr)
#else
      || pcm == nullptr
#endif
  ) return -1;
  N = M * mode->shortMdctSize;
  const auto decoder = make_celt_decoder_views(st, N); auto decode_mem = decoder.decode_mem; auto out_syn = decoder.out_syn;
  auto *oldBandE = decoder.oldBandE, *oldLogE = decoder.oldLogE, *oldLogE2 = decoder.oldLogE2, *backgroundLogE = decoder.backgroundLogE;
  effEnd = celt_effective_end(mode, end);
  if (data == nullptr || len <= 1) {
    if (pcm == nullptr) return -1;
    celt_decode_lost(st, N, LM);
    deemphasis(std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(CC)}, pcm, N, CC, st->downsample, mode->preemph, st->preemph_memD, accum);
    return frame_size / st->downsample;
  }
  if (st->loss_duration == 0) st->skip_plc = 0;
  if (dec == nullptr) { ec_dec_init(&_dec, (unsigned char *)data, len); dec = &_dec; }
  constexpr auto energy_channels = celt_decoder_energy_channel_count;
  if (C == 1) { for (i = 0; i < nbEBands; i++) oldBandE[i] = std::max(oldBandE[i], oldBandE[nbEBands + i]); }
  total_bits = len * 8; tell = ec_tell(dec);
  silence = tell >= total_bits ? 1 : tell == 1 ? ec_dec_bit_logp(dec, 15) : 0;
  if (silence) { tell = len * 8; dec->nbits_total += tell - ec_tell(dec); }
  postfilter_gain = 0; postfilter_pitch = 0; postfilter_tapset = 0;
  if (start == 0 && tell + 16 <= total_bits) {
    if (ec_dec_bit_logp(dec, 1)) {
      int qg, octave;
      octave = ec_dec_uint(dec, 6); postfilter_pitch = (16 << octave) + ec_dec_bits(dec, 4 + octave) - 1; qg = ec_dec_bits(dec, 3);
      if (ec_tell(dec) + 2 <= total_bits) postfilter_tapset = ec_dec_icdf(dec, shared_three_step_icdf.data(), 2);
      postfilter_gain = (.09375f) * (qg + 1);
    }
    tell = ec_tell(dec);
  }
  isTransient = LM > 0 && tell + 3 <= total_bits ? ec_dec_bit_logp(dec, 3) : 0;
  if (isTransient) tell = ec_tell(dec);
  shortBlocks = isTransient ? M : 0; intra_ener = tell + 3 <= total_bits ? ec_dec_bit_logp(dec, 3) : 0;
  if (!intra_ener && st->loss_duration != 0) {
    for (c = 0; c < energy_channels; ++c) {
      celt_glog safety = 0;
      int missing = std::min(10, st->loss_duration >> LM);
      if (LM == 0) safety = (1.5f);
      else if (LM == 1) safety = (.5f);
      for (i = start; i < end; i++) {
        if (oldBandE[c * nbEBands + i] < std::max(oldLogE[c * nbEBands + i], oldLogE2[c * nbEBands + i])) {
          opus_val32 slope; opus_val32 E0, E1, E2;
          E0 = oldBandE[c * nbEBands + i]; E1 = oldLogE[c * nbEBands + i];
          E2 = oldLogE2[c * nbEBands + i]; slope = std::max(E1 - E0, .5f * (E2 - E0));
          slope = std::min(slope, (2.f)); E0 -= std::max((opus_val32)0, (1 + missing) * slope);
          oldBandE[c * nbEBands + i] = std::max(-(20.f), E0);
        } else { oldBandE[c * nbEBands + i] = std::min(std::min(oldBandE[c * nbEBands + i], oldLogE[c * nbEBands + i]), oldLogE2[c * nbEBands + i]); }
        oldBandE[c * nbEBands + i] -= safety;
      }
    }
  }
  unquant_coarse_energy(mode, start, end, oldBandE, intra_ener, dec, C, LM);
  const auto band_count = static_cast<std::size_t>(nbEBands);
  auto band_storage = std::span<int>{OPUS_SCRATCH(int, 6 * band_count), 6 * band_count};
  auto [tf_res, cap, offsets, fine_quant, pulses, fine_priority] = partition_workset<6>(band_storage, band_count);
  tf_decode(start, end, isTransient, tf_res.data(), LM, dec); tell = ec_tell(dec);
  spread_decision = tell + 4 <= total_bits ? ec_dec_icdf(dec, spread_icdf.data(), 5) : 2;
  init_caps(mode, cap, LM, C);
  tell = celt_decode_dynalloc(dec, {eBands, static_cast<std::size_t>(nbEBands + 1)}, offsets, cap, start, end, C, LM, total_bits);
  alloc_trim = tell + (6 << 3) <= total_bits ? ec_dec_icdf(dec, trim_icdf.data(), 7) : 5;
  bits = (((opus_int32)len * 8) << 3) - (opus_int32)ec_tell_frac(dec) - 1;
  anti_collapse_rsv = celt_anti_collapse_reserve(isTransient, LM, bits); bits -= anti_collapse_rsv;
  codedBands = clt_compute_allocation(mode, start, end, offsets.data(), cap.data(), alloc_trim, &intensity, &dual_stereo, bits, &balance, pulses.data(), fine_quant.data(), fine_priority.data(), C, LM, dec, 0, 0, 0);
  unquant_fine_energy(mode, start, end, oldBandE, nullptr, fine_quant.data(), dec, C);
  auto *X = OPUS_SCRATCH(celt_norm, C * N);
  celt_slide_decode_history(std::span<celt_sig *const>{decode_mem.data(), static_cast<std::size_t>(CC)}, CC, N, overlap);
  auto *collapse_masks = OPUS_SCRATCH(unsigned char, C * nbEBands);
#if defined(OPUS_CODEC_TRACE_RATE)
  opus_trace_pvq = {};
#endif
  quant_all_bands(0, mode, start, end, X, C == 2 ? X + N : nullptr, collapse_masks, nullptr, pulses.data(), shortBlocks, spread_decision, dual_stereo, intensity, tf_res.data(), len * (8 << 3) - anti_collapse_rsv, balance, dec, LM, codedBands, &st->rng, st->disable_inv
  );
  if (anti_collapse_rsv > 0) { anti_collapse_on = ec_dec_bits(dec, 1); }
  unquant_energy_finalise(mode, start, end, oldBandE, fine_quant.data(), fine_priority.data(), len * 8 - ec_tell(dec), dec, C);
  if (anti_collapse_on) anti_collapse(mode, X, collapse_masks, LM, C, N, start, end, oldBandE, oldLogE, oldLogE2, pulses.data(), st->rng, 0);
  if (silence) { fill_n_items(oldBandE, static_cast<std::size_t>(C * nbEBands), -(28.f)); }
  if (st->prefilter_and_fold) { prefilter_and_fold(st, N); }
  celt_synthesis(mode, X, std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(CC)}, oldBandE, start, effEnd, C, CC, isTransient, LM, st->downsample, silence);
  celt_apply_postfilter(st, std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(CC)}, CC, N, LM, overlap, postfilter_pitch, postfilter_gain, postfilter_tapset);
  celt_commit_band_state(
      {oldBandE, static_cast<std::size_t>(energy_channels * nbEBands)}, {oldLogE, static_cast<std::size_t>(energy_channels * nbEBands)}, {oldLogE2, static_cast<std::size_t>(energy_channels * nbEBands)},
      energy_channels, nbEBands, start, end, isTransient, energy_channels == 2 && C == 1);
  max_background_increase = std::min(160, st->loss_duration + M) * (0.001f);
  for (int index = 0; index < energy_channels * nbEBands; ++index) backgroundLogE[index] = std::min(backgroundLogE[index] + max_background_increase, oldBandE[index]);
  st->rng = dec->rng;
#if OPUS_GCC_DECODE_FASTPATH
  if (pcm16 != nullptr && !accum && st->downsample == 1) {
    deemphasis_i16(std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(CC)}, pcm16, N, CC, st->downsample, mode->preemph, st->preemph_memD);
  } else {
    if (pcm == nullptr) return -1;
    deemphasis(std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(CC)}, pcm, N, CC, st->downsample, mode->preemph, st->preemph_memD, accum);
  }
#else
  if (pcm == nullptr) return -1;
  deemphasis(std::span<celt_sig *const>{out_syn.data(), static_cast<std::size_t>(CC)}, pcm, N, CC, st->downsample, mode->preemph, st->preemph_memD, accum);
#endif
  st->loss_duration = 0; st->plc_duration = 0; st->last_frame_type = 1; st->prefilter_and_fold = 0;
#if defined(OPUS_CODEC_TRACE_RATE)
  OPUS_TRACE_RATE("opus_decode_celt len=%d Fs=%d CC=%d C=%d LM=%d transient=%d shortBlocks=%d codedBands=%d intensity=%d dual=%d tell_q3=%d pvq_partitions=%d pvq_splits=%d pvq_q0=%d pvq_k1=%d pvq_k2=%d pvq_kn=%d\n",
                  len, mode->Fs, CC, C, LM, isTransient, shortBlocks, codedBands, intensity, dual_stereo, static_cast<int>(ec_tell_frac(dec)),
                  opus_trace_pvq.partitions, opus_trace_pvq.splits, opus_trace_pvq.q0, opus_trace_pvq.k1, opus_trace_pvq.k2, opus_trace_pvq.kn);
#endif
  if (ec_tell(dec) > 8 * len) return -3;
  if (ec_get_error(dec)) st->error = 1;
  return frame_size / st->downsample;
}
[[nodiscard]] static constexpr auto celt_pvq_binomial(int n, int k) noexcept -> opus_uint64 {
  if (k < 0 || k > n) { return 0; }
  if (k > n - k) { k = n - k; }
  opus_uint64 result = 1;
  for (int divisor = 1; divisor <= k; ++divisor) { result = (result * static_cast<opus_uint64>(n - k + divisor)) / static_cast<opus_uint64>(divisor); }
  return result;
}

[[nodiscard]] static constexpr auto celt_pvq_u_entry_raw(int row, int column) noexcept -> opus_uint32 {
  if (row > column) { const auto tmp = row; row = column; column = tmp; }
  if (row == 0) { return column == 0 ? 1U : 0U; }
  if (column == 0) { return 0U; }
  opus_uint64 total = 0;
  const auto last_term = std::min(row - 1, column - 1);
  for (int term = 0; term <= last_term; ++term) { total += celt_pvq_binomial(row - 1, term) * celt_pvq_binomial(row + column - term - 2, column - 1 - term); }
  return static_cast<opus_uint32>(total);
}

static constexpr auto celt_pvq_table_row_count = 14U;
static constexpr auto celt_pvq_table_first_stored_row = 4U;
static constexpr std::array<unsigned char, celt_pvq_table_row_count> celt_pvq_u_row_max{0, 174, 173, 172, 171, 170, 96, 48, 37, 27, 24, 19, 18, 16};

[[nodiscard]] consteval auto celt_pvq_reachable_entry_count() noexcept -> std::size_t {
  auto count = std::size_t{};
  for (auto row = celt_pvq_table_first_stored_row; row < celt_pvq_table_row_count; ++row) { count += static_cast<std::size_t>(celt_pvq_u_row_max[row] - row + 1U); }
  return count;
}

static constexpr auto celt_pvq_table_entry_count = celt_pvq_reachable_entry_count();

[[nodiscard]] consteval auto make_celt_pvq_row_offsets() {
  std::array<opus_uint16, celt_pvq_table_row_count> offsets{};
  auto cursor = opus_uint16{};
  for (auto row = celt_pvq_table_first_stored_row; row < celt_pvq_table_row_count; ++row) {
    offsets[row] = cursor;
    cursor = static_cast<opus_uint16>(cursor + celt_pvq_u_row_max[row] - row + 1U);
  }
  return offsets;
}

[[nodiscard]] consteval auto make_celt_pvq_reachable_table() {
  std::array<opus_uint32, celt_pvq_table_entry_count> table{};
  auto cursor = std::size_t{};
  for (auto row = celt_pvq_table_first_stored_row; row < celt_pvq_table_row_count; ++row) {
    for (auto column = row; column <= celt_pvq_u_row_max[row]; ++column) {
      table[cursor++] = celt_pvq_u_entry_raw(static_cast<int>(row), static_cast<int>(column));
    }
  }
  return table;
}

static constexpr auto celt_pvq_u_row_offsets = make_celt_pvq_row_offsets();
static constexpr auto celt_pvq_u_data = make_celt_pvq_reachable_table();
static_assert(celt_pvq_u_data.size() == 551);

[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_pvq_u_small_row_formula(unsigned row, unsigned column) noexcept -> opus_uint32 {
  switch (row) {
  case 1: return 1U;
  case 2: return 2U * column - 1U;
  default: return 2U * column * (column - 1U) + 1U;
  }
}

[[nodiscard]] static OPUS_COLD OPUS_NOINLINE auto celt_pvq_u_entry_slow(int row, int column) noexcept -> opus_uint32 {
  return celt_pvq_u_entry_raw(row, column);
}

[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_pvq_u_entry_ordered(int row, int column) noexcept -> opus_uint32 {
  if (row == 0) { return column == 0 ? 1U : 0U; }
  if (column == 0) { return 0U; }
  const auto row_index = static_cast<unsigned>(row);
  const auto column_index = static_cast<unsigned>(column);
  if (row_index < celt_pvq_table_first_stored_row) { return celt_pvq_u_small_row_formula(row_index, column_index); }
  if (row_index < celt_pvq_u_row_max.size() && column_index <= celt_pvq_u_row_max[row_index]) {
    return celt_pvq_u_data[celt_pvq_u_row_offsets[row_index] + column_index - row_index];
  }
  return celt_pvq_u_entry_slow(row, column);
}

[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_pvq_u_entry_cwrs(int row, int column) noexcept -> opus_uint32 {
  if (row == 0) { return 0U; }
  const auto row_index = static_cast<unsigned>(row);
  const auto column_index = static_cast<unsigned>(column);
  if (row_index < celt_pvq_table_first_stored_row) { return celt_pvq_u_small_row_formula(row_index, column_index); }
  if (row_index < celt_pvq_u_row_max.size() && column_index <= celt_pvq_u_row_max[row_index]) {
    return celt_pvq_u_data[celt_pvq_u_row_offsets[row_index] + column_index - row_index];
  }
  return celt_pvq_u_entry_slow(row, column);
}

[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_pvq_u_entry(int row, int column) noexcept -> opus_uint32 {
  if (row > column) { const auto tmp = row; row = column; column = tmp; }
  return celt_pvq_u_entry_ordered(row, column);
}

[[nodiscard]] static OPUS_ALWAYS_INLINE auto celt_pvq_u_total(const int n, const int k) noexcept -> opus_uint32 { return celt_pvq_u_entry(n, k) + celt_pvq_u_entry(n, k + 1); }

static opus_uint32 icwrs(int _n, const int *_y) {
  opus_uint32 i;
  int j, k;
  j = _n - 1;
  i = _y[j] < 0; k = abs(_y[j]);
  for (; j-- > 0;) {
    i += celt_pvq_u_entry(_n - j, k); k += abs(_y[j]);
    if (_y[j] < 0) i += celt_pvq_u_entry(_n - j, k + 1);
  }
  return i;
}

static void encode_pulses(const int *_y, int _n, int _k, ec_enc *_enc) {
  ec_enc_uint(_enc, icwrs(_n, _y), celt_pvq_u_total(_n, _k));
}

static OPUS_ALWAYS_INLINE opus_val32 cwrsi(int _n, int _k, opus_uint32 _i, int *_y) {
  opus_uint32 p;
  int s, k0;
  opus_int16 val; opus_val32 yy = 0;
  for (; _n > 2; --_n) {
    opus_uint32 q;
    if (_k >= _n) {
      p = celt_pvq_u_entry_cwrs(_n, _k + 1); s = -(_i >= p); _i -= p & s; k0 = _k; q = celt_pvq_u_entry_cwrs(_n, _n);
      if (q > _i) {
        _k = _n;
        for (p = celt_pvq_u_entry_cwrs(--_k, _n); p > _i; p = celt_pvq_u_entry_cwrs(--_k, _n)) {}
      } else {
        for (p = celt_pvq_u_entry_cwrs(_n, _k); p > _i; p = celt_pvq_u_entry_cwrs(_n, _k)) { --_k; }
      }
      _i -= p; val = (k0 - _k + s) ^ s; *_y++ = val; yy = ((yy) + (opus_val32)(val) * (opus_val32)(val));
    } else {
      p = celt_pvq_u_entry_cwrs(_k, _n); q = celt_pvq_u_entry_cwrs(_k + 1, _n);
      if (p <= _i && _i < q) {
        _i -= p; *_y++ = 0;
      } else {
        s = -(_i >= q); _i -= q & s; k0 = _k;
        for (p = celt_pvq_u_entry_cwrs(--_k, _n); p > _i; p = celt_pvq_u_entry_cwrs(--_k, _n)) {}
        _i -= p; val = (k0 - _k + s) ^ s; *_y++ = val; yy = ((yy) + (opus_val32)(val) * (opus_val32)(val));
      }
    }
  }
  p = 2 * _k + 1; s = -(_i >= p); _i -= p & s; k0 = _k; _k = (_i + 1) >> 1;
  if (_k) _i -= 2 * _k - 1;
  val = (k0 - _k + s) ^ s; *_y++ = val; yy = ((yy) + (opus_val32)(val) * (opus_val32)(val)); s = -(int)_i;
  val = (_k + s) ^ s; *_y = val; yy = ((yy) + (opus_val32)(val) * (opus_val32)(val));
  return yy;
}

static OPUS_ALWAYS_INLINE void celt_decode_one_pulse_zeroed(int *_y, int _n, opus_uint32 index) noexcept {
  if (index < static_cast<opus_uint32>(_n)) { _y[index] = 1; }
  else { _y[static_cast<opus_uint32>((_n << 1) - 1) - index] = -1; }
}

static OPUS_ALWAYS_INLINE auto celt_decode_two_pulses_zeroed(int *_y, int _n, opus_uint32 index) noexcept -> opus_val32 {
  auto *out = _y;
  for (; _n > 2; --_n, ++out) {
    const auto edge_count = static_cast<opus_uint32>((_n << 1) - 1);
    const auto zero_end = edge_count + static_cast<opus_uint32>(2 * (_n - 1) * (_n - 1));
    if (index < edge_count) {
      if (index == 0) { *out = 2; return 4; }
      *out = 1; celt_decode_one_pulse_zeroed(out + 1, _n - 1, index - 1); return 2;
    }
    if (index < zero_end) {
      index -= edge_count;
      continue;
    }
    index -= zero_end;
    if (index == 0) { *out = -2; return 4; }
    *out = -1; celt_decode_one_pulse_zeroed(out + 1, _n - 1, index - 1); return 2;
  }
  switch (index) {
  case 0: out[0] = 2; return 4;
  case 1: out[0] = 1; out[1] = 1; return 2;
  case 2: out[0] = 1; out[1] = -1; return 2;
  case 3: out[1] = 2; return 4;
  case 4: out[1] = -2; return 4;
  case 5: out[0] = -2; return 4;
  case 6: out[0] = -1; out[1] = 1; return 2;
  default: out[0] = -1; out[1] = -1; return 2;
  }
}

static OPUS_ALWAYS_INLINE opus_val32 decode_pulses(int *_y, int _n, int _k, ec_dec *_dec) {
  if (_k == 1) {
    const auto index = ec_dec_uint(_dec, static_cast<opus_uint32>(_n << 1));
    zero_n_items(_y, static_cast<std::size_t>(_n));
    celt_decode_one_pulse_zeroed(_y, _n, index);
    return 1;
  }
  if (_k == 2) {
    const auto index = ec_dec_uint(_dec, static_cast<opus_uint32>(2 * _n * _n));
    zero_n_items(_y, static_cast<std::size_t>(_n));
    return celt_decode_two_pulses_zeroed(_y, _n, index);
  }
  return cwrsi(_n, _k, ec_dec_uint(_dec, celt_pvq_u_total(_n, _k)), _y);
}
static opus_uint32 ec_tell_frac(const ec_ctx *_this) {
  constexpr std::array<unsigned, 8> correction{35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
  opus_uint32 nbits, r;
  int l;
  unsigned b; nbits = _this->nbits_total << 3; l = (((int)sizeof(unsigned) * 8) - (__builtin_clz(_this->rng))); r = _this->rng >> (l - 16);
  b = (r >> 12) - 8; b += r > correction[b]; l = (l << 3) + b;
  return nbits - l;
}
static int ec_read_byte(ec_dec *_this) { return _this->offs < _this->storage ? _this->buf[_this->offs++] : 0; }
static int ec_read_byte_from_end(ec_dec *_this) { return _this->end_offs < _this->storage ? _this->buf[_this->storage - ++(_this->end_offs)] : 0; }
static OPUS_INT_HOT void ec_dec_normalize(ec_dec *_this) {
  do {
    int sym;
    _this->nbits_total += (8); _this->rng <<= (8); sym = _this->rem; _this->rem = ec_read_byte(_this);
    sym = (sym << (8) | _this->rem) >> ((8) - (((32) - 2) % (8) + 1));
    _this->val = ((_this->val << (8)) + (ec_byte_mask & ~sym)) & ec_code_mask;
  } while (_this->rng <= ec_code_bot);
}
static inline void ec_dec_normalize_if_needed(ec_dec *_this) {
  if (__builtin_expect(_this->rng <= ec_code_bot, 0)) ec_dec_normalize(_this);
}
void ec_dec_init(ec_dec *_this, unsigned char *_buf, opus_uint32 _storage) {
  _this->buf = _buf; _this->storage = _storage; _this->end_offs = 0; _this->end_window = 0;
  _this->nend_bits = 0; _this->nbits_total = (32) + 1 - (((32) - (((32) - 2) % (8) + 1)) / (8)) * (8);
  _this->offs = 0; _this->rng = 1U << (((32) - 2) % (8) + 1);
  _this->rem = ec_read_byte(_this); _this->val = _this->rng - 1 - (_this->rem >> ((8) - (((32) - 2) % (8) + 1)));
  _this->error = 0; ec_dec_normalize_if_needed(_this);
}
OPUS_INT_HOT unsigned ec_decode(ec_dec *_this, unsigned _ft) {
  unsigned s; _this->ext = celt_udiv(_this->rng, _ft); s = (unsigned)(_this->val / _this->ext);
  return _ft - ((s + 1) + (((_ft) - (s + 1)) & -((_ft) < (s + 1))));
}
OPUS_INT_HOT unsigned ec_decode_bin(ec_dec *_this, unsigned _bits) {
  unsigned s; _this->ext = _this->rng >> _bits; s = (unsigned)(_this->val / _this->ext);
  return (1U << _bits) - ((s + 1U) + (((1U << _bits) - (s + 1U)) & -((1U << _bits) < (s + 1U))));
}
OPUS_INT_HOT void ec_dec_update(ec_dec *_this, unsigned _fl, unsigned _fh, unsigned _ft) {
  opus_uint32 s = ((_this->ext) * (_ft - _fh)); _this->val -= s;
  _this->rng = _fl > 0 ? ((_this->ext) * (_fh - _fl)) : _this->rng - s; ec_dec_normalize_if_needed(_this);
}
OPUS_INT_HOT int ec_dec_bit_logp(ec_dec *_this, unsigned _logp) {
  opus_uint32 r, d, s;
  int ret;
  r = _this->rng; d = _this->val; s = r >> _logp; ret = d < s;
  if (!ret) _this->val = d - s;
  _this->rng = ret ? s : r - s; ec_dec_normalize_if_needed(_this);
  return ret;
}
OPUS_INT_HOT int ec_dec_icdf(ec_dec *_this, const unsigned char *_icdf, unsigned _ftb) {
  opus_uint32 r, d, s, t;
  int ret;
  s = _this->rng; d = _this->val; r = s >> _ftb; ret = -1;
  for (ret = 0, t = s, s = ((r) * (_icdf[0])); d < s; t = s, s = ((r) * (_icdf[++ret]))) {}
  _this->val = d - s; _this->rng = t - s; ec_dec_normalize_if_needed(_this);
  return ret;
}
OPUS_INT_HOT opus_uint32 ec_dec_uint(ec_dec *_this, opus_uint32 _ft) {
  unsigned ft; unsigned s;
  int ftb;
  _ft--;
  ftb = (((int)sizeof(unsigned) * 8) - (__builtin_clz(_ft)));
  if (ftb > (8)) {
    opus_uint32 t; ftb -= (8);
    ft = (unsigned)(_ft >> ftb) + 1; s = ec_decode(_this, ft);
    ec_dec_update(_this, s, s + 1, ft); t = (opus_uint32)s << ftb | ec_dec_bits(_this, ftb);
    if (t <= _ft) return t;
    _this->error = 1;
    return _ft;
  } else {
    _ft++; s = ec_decode(_this, (unsigned)_ft); ec_dec_update(_this, s, s + 1, (unsigned)_ft);
    return s;
  }
}
OPUS_INT_HOT opus_uint32 ec_dec_bits(ec_dec *_this, unsigned _bits) {
  ec_window window;
  int available;
  opus_uint32 ret; window = _this->end_window;
  available = _this->nend_bits;
  if ((unsigned)available < _bits) {
    for (; available <= ((int)sizeof(ec_window) * 8) - (8); available += (8)) { window |= (ec_window)ec_read_byte_from_end(_this) << available; }
  }
  ret = (opus_uint32)window & low_bits_mask(_bits); window >>= _bits; available -= _bits; _this->end_window = window;
  _this->nend_bits = available; _this->nbits_total += _bits;
  return ret;
}
static int ec_write_byte(ec_enc *_this, unsigned _value) { if (_this->offs + _this->end_offs >= _this->storage) return -1; _this->buf[_this->offs++] = (unsigned char)_value; return 0; }
static int ec_write_byte_at_end(ec_enc *_this, unsigned _value) {
  if (_this->offs + _this->end_offs >= _this->storage) return -1;
  _this->buf[_this->storage - ++(_this->end_offs)] = (unsigned char)_value;
  return 0;
}
static void ec_enc_carry_out(ec_enc *_this, int _c) {
  if (_c != ec_byte_mask) {
    int carry = _c >> (8);
    if (_this->rem >= 0) _this->error |= ec_write_byte(_this, _this->rem + carry);
    if (_this->ext > 0) {
      unsigned sym; sym = (ec_byte_mask + carry) & ec_byte_mask;
      for (; _this->ext > 0; --(_this->ext)) _this->error |= ec_write_byte(_this, sym);
    }
    _this->rem = _c & ec_byte_mask;
  } else _this->ext++;
}
static void ec_enc_normalize(ec_enc *_this) {
  for (; _this->rng <= ec_code_bot;) {
    ec_enc_carry_out(_this, (int)(_this->val >> ((32) - (8) - 1))); _this->val = (_this->val << (8)) & ec_code_mask;
    _this->rng <<= (8); _this->nbits_total += (8);
  }
}
void ec_enc_init(ec_enc *_this, unsigned char *_buf, opus_uint32 _size) {
  _this->buf = _buf; _this->end_offs = 0; _this->end_window = 0; _this->nend_bits = 0;
  _this->nbits_total = (32) + 1; _this->offs = 0; _this->rng = ec_code_top; _this->rem = -1;
  _this->val = 0; _this->ext = 0; _this->storage = _size; _this->error = 0;
}
void ec_encode(ec_enc *_this, unsigned _fl, unsigned _fh, unsigned _ft) {
  opus_uint32 r = celt_udiv(_this->rng, _ft);
  if (_fl > 0) {
    _this->val += _this->rng - ((r) * ((_ft - _fl))); _this->rng = ((r) * ((_fh - _fl)));
  } else _this->rng -= ((r) * ((_ft - _fh))); ec_enc_normalize(_this);
}
void ec_encode_bin(ec_enc *_this, unsigned _fl, unsigned _fh, unsigned _bits) {
  opus_uint32 r = _this->rng >> _bits;
  if (_fl > 0) {
    _this->val += _this->rng - ((r) * (((1U << _bits) - _fl))); _this->rng = ((r) * ((_fh - _fl)));
  } else _this->rng -= ((r) * (((1U << _bits) - _fh))); ec_enc_normalize(_this);
}
void ec_enc_bit_logp(ec_enc *_this, int _val, unsigned _logp) {
  opus_uint32 r, s, l; r = _this->rng;
  l = _this->val; s = r >> _logp; r -= s;
  if (_val) _this->val = l + r;
  _this->rng = _val ? s : r; ec_enc_normalize(_this);
}
void ec_enc_icdf(ec_enc *_this, int _s, const unsigned char *_icdf, unsigned _ftb) {
  opus_uint32 r = _this->rng >> _ftb;
  if (_s > 0) {
    _this->val += _this->rng - ((r) * (_icdf[_s - 1])); _this->rng = ((r) * (_icdf[_s - 1] - _icdf[_s]));
  } else _this->rng -= ((r) * (_icdf[_s])); ec_enc_normalize(_this);
}
void ec_enc_uint(ec_enc *_this, opus_uint32 _fl, opus_uint32 _ft) {
  unsigned ft; unsigned fl;
  int ftb;
  _ft--;
  ftb = (((int)sizeof(unsigned) * 8) - (__builtin_clz(_ft)));
  if (ftb > (8)) {
    ftb -= (8); ft = (_ft >> ftb) + 1; fl = (unsigned)(_fl >> ftb); ec_encode(_this, fl, fl + 1, ft);
    ec_enc_bits(_this, _fl & low_bits_mask(ftb), ftb);
  } else ec_encode(_this, _fl, _fl + 1, _ft + 1);
}
void ec_enc_bits(ec_enc *_this, opus_uint32 _fl, unsigned _bits) {
  ec_window window;
  int used;
  window = _this->end_window; used = _this->nend_bits;
  if (used + _bits > ((int)sizeof(ec_window) * 8)) {
    for (; used >= (8); used -= (8)) { _this->error |= ec_write_byte_at_end(_this, (unsigned)window & ec_byte_mask); window >>= (8); }
  }
  window |= (ec_window)_fl << used; used += _bits; _this->end_window = window; _this->nend_bits = used; _this->nbits_total += _bits;
}
void ec_enc_patch_initial_bits(ec_enc *_this, unsigned _val, unsigned _nbits) {
  int shift;
  unsigned mask; shift = (8) - _nbits; mask = ((1 << _nbits) - 1) << shift;
  if (_this->offs > 0) { _this->buf[0] = (unsigned char)((_this->buf[0] & ~mask) | _val << shift); }
  else if (_this->rem >= 0) { _this->rem = (_this->rem & ~mask) | _val << shift; }
  else if (_this->rng <= (ec_code_top >> _nbits)) { _this->val = (_this->val & ~((opus_uint32)mask << ((32) - (8) - 1))) | (opus_uint32)_val << (((32) - (8) - 1) + shift); }
  else _this->error = -1;
}
void ec_enc_shrink(ec_enc *_this, opus_uint32 _size) {
  move_n_items(_this->buf + _this->storage - _this->end_offs, static_cast<std::size_t>(_this->end_offs), _this->buf + _size - _this->end_offs);
  _this->storage = _size;
}
void ec_enc_done(ec_enc *_this) {
  ec_window window;
  int used;
  opus_uint32 msk, end;
  int l = (32) - (((int)sizeof(unsigned) * 8) - (__builtin_clz(_this->rng)));
  msk = ec_code_mask >> l; end = (_this->val + msk) & ~msk;
  if ((end | msk) >= _this->val + _this->rng) { l++; msk >>= 1; end = (_this->val + msk) & ~msk; }
  for (; l > 0; l -= (8)) { ec_enc_carry_out(_this, (int)(end >> ((32) - (8) - 1))); end = (end << (8)) & ec_code_mask; }
  if (_this->rem >= 0 || _this->ext > 0) ec_enc_carry_out(_this, 0);
  window = _this->end_window; used = _this->nend_bits;
  for (; used >= (8); used -= (8)) { _this->error |= ec_write_byte_at_end(_this, (unsigned)window & ec_byte_mask); window >>= (8); }
  if (!_this->error) {
    if (_this->buf) zero_n_items(_this->buf + _this->offs, static_cast<std::size_t>(_this->storage - _this->offs - _this->end_offs));
    if (used > 0) {
      if (_this->end_offs >= _this->storage) _this->error = -1;
      else {
        l = -l;
        if (_this->offs + _this->end_offs >= _this->storage && l < used) { window &= (1 << l) - 1; _this->error = -1; }
        _this->buf[_this->storage - _this->end_offs - 1] |= (unsigned char)window;
      }
    }
  }
}
static void kf_bfly2(kiss_fft_cpx *Fout, int m, int N) {
  kiss_fft_cpx *Fout2;
  int i;
  (void)m;
  {
    celt_coef tw; tw = (0.7071067812f);
    opus_assume(m == 4);
    for (i = 0; i < N; i++) {
      kiss_fft_cpx t; Fout2 = Fout + 4; t = Fout2[0];
      { (Fout2[0]).r = (Fout[0]).r - (t).r; (Fout2[0]).i = (Fout[0]).i - (t).i;
}
      { (Fout[0]).r += (t).r; (Fout[0]).i += (t).i;
}
      t.r = ((((Fout2[1].r) + (Fout2[1].i))) * (tw)); t.i = ((((Fout2[1].i) - (Fout2[1].r))) * (tw));
      { (Fout2[1]).r = (Fout[1]).r - (t).r; (Fout2[1]).i = (Fout[1]).i - (t).i;
}
      { (Fout[1]).r += (t).r; (Fout[1]).i += (t).i;
}
      t.r = Fout2[2].i; t.i = (-(Fout2[2].r));
      { (Fout2[2]).r = (Fout[2]).r - (t).r; (Fout2[2]).i = (Fout[2]).i - (t).i;
}
      { (Fout[2]).r += (t).r; (Fout[2]).i += (t).i;
}
      t.r = ((((Fout2[3].i) - (Fout2[3].r))) * (tw)); t.i = (((-(((Fout2[3].i) + (Fout2[3].r))))) * (tw));
      { (Fout2[3]).r = (Fout[3]).r - (t).r; (Fout2[3]).i = (Fout[3]).i - (t).i;
}
      { (Fout[3]).r += (t).r; (Fout[3]).i += (t).i;
}
      Fout += 8; } }
}
static void kf_bfly4(kiss_fft_cpx *Fout, const size_t fstride, const kiss_fft_state *st, int m, int N, int mm) {
  int i;
  if (m == 1) {
    for (i = 0; i < N; i++) { kiss_fft_cpx scratch0, scratch1; { (scratch0).r = (*Fout).r - (Fout[2]).r; (scratch0).i = (*Fout).i - (Fout[2]).i; } { (*Fout).r += (Fout[2]).r; (*Fout).i += (Fout[2]).i; } { (scratch1).r = (Fout[1]).r + (Fout[3]).r; (scratch1).i = (Fout[1]).i + (Fout[3]).i; } { (Fout[2]).r = (*Fout).r - (scratch1).r; (Fout[2]).i = (*Fout).i - (scratch1).i; } { (*Fout).r += (scratch1).r; (*Fout).i += (scratch1).i; } { (scratch1).r = (Fout[1]).r - (Fout[3]).r; (scratch1).i = (Fout[1]).i - (Fout[3]).i; } Fout[1].r = ((scratch0.r) + (scratch1.i)); Fout[1].i = ((scratch0.i) - (scratch1.r)); Fout[3].r = ((scratch0.r) - (scratch1.i)); Fout[3].i = ((scratch0.i) + (scratch1.r)); Fout += 4;
}
  } else {
    int j;
    kiss_fft_cpx scratch[6];
    const kiss_twiddle_cpx *tw1, *tw2, *tw3;
    const int m2 = 2 * m;
    const int m3 = 3 * m;
    kiss_fft_cpx *Fout_beg = Fout;
    for (i = 0; i < N; i++) {
      Fout = Fout_beg + i * mm; tw3 = tw2 = tw1 = st->twiddles;
      for (j = 0; j < m; j++) { { (scratch[0]).r = (Fout[m]).r * (*tw1).r - (Fout[m]).i * (*tw1).i; (scratch[0]).i = (Fout[m]).r * (*tw1).i + (Fout[m]).i * (*tw1).r; } { (scratch[1]).r = (Fout[m2]).r * (*tw2).r - (Fout[m2]).i * (*tw2).i; (scratch[1]).i = (Fout[m2]).r * (*tw2).i + (Fout[m2]).i * (*tw2).r; } { (scratch[2]).r = (Fout[m3]).r * (*tw3).r - (Fout[m3]).i * (*tw3).i; (scratch[2]).i = (Fout[m3]).r * (*tw3).i + (Fout[m3]).i * (*tw3).r; } { (scratch[5]).r = (*Fout).r - (scratch[1]).r; (scratch[5]).i = (*Fout).i - (scratch[1]).i; } { (*Fout).r += (scratch[1]).r; (*Fout).i += (scratch[1]).i; } { (scratch[3]).r = (scratch[0]).r + (scratch[2]).r; (scratch[3]).i = (scratch[0]).i + (scratch[2]).i; } { (scratch[4]).r = (scratch[0]).r - (scratch[2]).r; (scratch[4]).i = (scratch[0]).i - (scratch[2]).i; } { (Fout[m2]).r = (*Fout).r - (scratch[3]).r; (Fout[m2]).i = (*Fout).i - (scratch[3]).i; } tw1 += fstride; tw2 += fstride * 2; tw3 += fstride * 3; { (*Fout).r += (scratch[3]).r; (*Fout).i += (scratch[3]).i; } Fout[m].r = ((scratch[5].r) + (scratch[4].i)); Fout[m].i = ((scratch[5].i) - (scratch[4].r)); Fout[m3].r = ((scratch[5].r) - (scratch[4].i)); Fout[m3].i = ((scratch[5].i) + (scratch[4].r)); ++Fout; } } }
}
static void kf_bfly3(kiss_fft_cpx *Fout, const size_t fstride, const kiss_fft_state *st, int m, int N, int mm) {
  int i;
  size_t k;
  const size_t m2 = 2 * m;
  const kiss_twiddle_cpx *tw1, *tw2;
  kiss_fft_cpx scratch[5]; kiss_twiddle_cpx epi3; kiss_fft_cpx *Fout_beg = Fout; epi3 = st->twiddles[fstride * m];
  for (i = 0; i < N; i++) {
    Fout = Fout_beg + i * mm; tw1 = tw2 = st->twiddles;
    for (k = m; k > 0; --k) { { (scratch[1]).r = (Fout[m]).r * (*tw1).r - (Fout[m]).i * (*tw1).i; (scratch[1]).i = (Fout[m]).r * (*tw1).i + (Fout[m]).i * (*tw1).r; } { (scratch[2]).r = (Fout[m2]).r * (*tw2).r - (Fout[m2]).i * (*tw2).i; (scratch[2]).i = (Fout[m2]).r * (*tw2).i + (Fout[m2]).i * (*tw2).r; } { (scratch[3]).r = (scratch[1]).r + (scratch[2]).r; (scratch[3]).i = (scratch[1]).i + (scratch[2]).i; } { (scratch[0]).r = (scratch[1]).r - (scratch[2]).r; (scratch[0]).i = (scratch[1]).i - (scratch[2]).i; } tw1 += fstride; tw2 += fstride * 2; Fout[m].r = ((Fout->r) - (((scratch[3].r) * .5f))); Fout[m].i = ((Fout->i) - (((scratch[3].i) * .5f))); { (scratch[0]).r *= (epi3.i); (scratch[0]).i *= (epi3.i); } { (*Fout).r += (scratch[3]).r; (*Fout).i += (scratch[3]).i; } Fout[m2].r = ((Fout[m].r) + (scratch[0].i)); Fout[m2].i = ((Fout[m].i) - (scratch[0].r)); Fout[m].r = ((Fout[m].r) - (scratch[0].i)); Fout[m].i = ((Fout[m].i) + (scratch[0].r)); ++Fout; } }
}
static void kf_bfly5(kiss_fft_cpx *Fout, const size_t fstride, const kiss_fft_state *st, int m, int N, int mm) {
  kiss_fft_cpx *Fout0, *Fout1, *Fout2, *Fout3, *Fout4;
  int i, u;
  kiss_fft_cpx scratch[13];
  const kiss_twiddle_cpx *tw;
  kiss_twiddle_cpx ya, yb; kiss_fft_cpx *Fout_beg = Fout;
  ya = st->twiddles[fstride * m]; yb = st->twiddles[fstride * 2 * m]; tw = st->twiddles;
  for (i = 0; i < N; i++) {
    Fout = Fout_beg + i * mm; Fout0 = Fout; Fout1 = Fout0 + m; Fout2 = Fout0 + 2 * m; Fout3 = Fout0 + 3 * m; Fout4 = Fout0 + 4 * m;
    for (u = 0; u < m; ++u) { scratch[0] = *Fout0; { (scratch[1]).r = (*Fout1).r * (tw[u * fstride]).r - (*Fout1).i * (tw[u * fstride]).i; (scratch[1]).i = (*Fout1).r * (tw[u * fstride]).i + (*Fout1).i * (tw[u * fstride]).r; } { (scratch[2]).r = (*Fout2).r * (tw[2 * u * fstride]).r - (*Fout2).i * (tw[2 * u * fstride]).i; (scratch[2]).i = (*Fout2).r * (tw[2 * u * fstride]).i + (*Fout2).i * (tw[2 * u * fstride]).r; } { (scratch[3]).r = (*Fout3).r * (tw[3 * u * fstride]).r - (*Fout3).i * (tw[3 * u * fstride]).i; (scratch[3]).i = (*Fout3).r * (tw[3 * u * fstride]).i + (*Fout3).i * (tw[3 * u * fstride]).r; } { (scratch[4]).r = (*Fout4).r * (tw[4 * u * fstride]).r - (*Fout4).i * (tw[4 * u * fstride]).i; (scratch[4]).i = (*Fout4).r * (tw[4 * u * fstride]).i + (*Fout4).i * (tw[4 * u * fstride]).r; } { (scratch[7]).r = (scratch[1]).r + (scratch[4]).r; (scratch[7]).i = (scratch[1]).i + (scratch[4]).i; } { (scratch[10]).r = (scratch[1]).r - (scratch[4]).r; (scratch[10]).i = (scratch[1]).i - (scratch[4]).i; } { (scratch[8]).r = (scratch[2]).r + (scratch[3]).r; (scratch[8]).i = (scratch[2]).i + (scratch[3]).i; } { (scratch[9]).r = (scratch[2]).r - (scratch[3]).r; (scratch[9]).i = (scratch[2]).i - (scratch[3]).i; } Fout0->r = ((Fout0->r) + (((scratch[7].r) + (scratch[8].r)))); Fout0->i = ((Fout0->i) + (((scratch[7].i) + (scratch[8].i)))); scratch[5].r = ((scratch[0].r) + (((((scratch[7].r) * (ya.r))) + (((scratch[8].r) * (yb.r)))))); scratch[5].i = ((scratch[0].i) + (((((scratch[7].i) * (ya.r))) + (((scratch[8].i) * (yb.r)))))); scratch[6].r = ((((scratch[10].i) * (ya.i))) + (((scratch[9].i) * (yb.i)))); scratch[6].i = (-(((((scratch[10].r) * (ya.i))) + (((scratch[9].r) * (yb.i)))))); { (*Fout1).r = (scratch[5]).r - (scratch[6]).r; (*Fout1).i = (scratch[5]).i - (scratch[6]).i; } { (*Fout4).r = (scratch[5]).r + (scratch[6]).r; (*Fout4).i = (scratch[5]).i + (scratch[6]).i; } scratch[11].r = ((scratch[0].r) + (((((scratch[7].r) * (yb.r))) + (((scratch[8].r) * (ya.r)))))); scratch[11].i = ((scratch[0].i) + (((((scratch[7].i) * (yb.r))) + (((scratch[8].i) * (ya.r)))))); scratch[12].r = ((((scratch[9].i) * (ya.i))) - (((scratch[10].i) * (yb.i)))); scratch[12].i = ((((scratch[10].r) * (yb.i))) - (((scratch[9].r) * (ya.i)))); { (*Fout2).r = (scratch[11]).r + (scratch[12]).r; (*Fout2).i = (scratch[11]).i + (scratch[12]).i; } { (*Fout3).r = (scratch[11]).r - (scratch[12]).r; (*Fout3).i = (scratch[11]).i - (scratch[12]).i; } ++Fout0; ++Fout1; ++Fout2; ++Fout3; ++Fout4; } }
}
static void fft_impl_480(kiss_fft_cpx *fout, const kiss_fft_state *st) {
  kf_bfly4(fout, 120, st, 1, 120, 4);
  kf_bfly2(fout, 4, 60);
  kf_bfly4(fout, 15, st, 8, 15, 32);
  kf_bfly3(fout, 5, st, 32, 5, 96);
  kf_bfly5(fout, 1, st, 96, 1, 1);
}
static void fft_impl_240(kiss_fft_cpx *fout, const kiss_fft_state *st) {
  kf_bfly4(fout, 120, st, 1, 60, 4);
  kf_bfly4(fout, 30, st, 4, 15, 16);
  kf_bfly3(fout, 10, st, 16, 5, 48);
  kf_bfly5(fout, 2, st, 48, 1, 1);
}
static void fft_impl_120(kiss_fft_cpx *fout, const kiss_fft_state *st) {
  kf_bfly4(fout, 120, st, 1, 30, 4);
  kf_bfly2(fout, 4, 15);
  kf_bfly3(fout, 20, st, 8, 5, 24);
  kf_bfly5(fout, 4, st, 24, 1, 1);
}
static void fft_impl_60(kiss_fft_cpx *fout, const kiss_fft_state *st) {
  kf_bfly4(fout, 120, st, 1, 15, 4);
  kf_bfly3(fout, 40, st, 4, 5, 12);
  kf_bfly5(fout, 8, st, 12, 1, 1);
}
static void fft_impl(const kiss_fft_state *st, kiss_fft_cpx *fout) {
  switch (st->nfft) {
  case 480: fft_impl_480(fout, st); break;
  case 240: fft_impl_240(fout, st); break;
  case 120: fft_impl_120(fout, st); break;
  default: fft_impl_60(fout, st); break;
  }
}
static void ec_laplace_encode(ec_enc *enc, int *value, unsigned fs, int decay);
static int ec_laplace_decode(ec_dec *dec, unsigned fs, int decay);
static unsigned ec_laplace_get_freq1(unsigned fs0, int decay) {
  unsigned ft; ft = 32768 - (1 << (0)) * (2 * (16)) - fs0;
  return ft * (opus_int32)(16384 - decay) >> 15;
}
static void ec_laplace_encode(ec_enc *enc, int *value, unsigned fs, int decay) {
  unsigned fl;
  int val = *value;
  fl = 0;
  if (val) {
    int s, i;
    s = -(val < 0); val = (val + s) ^ s; fl = fs; fs = ec_laplace_get_freq1(fs, decay);
    for (i = 1; fs > 0 && i < val; i++) { fs *= 2; fl += fs + 2 * (1 << (0)); fs = (fs * (opus_int32)decay) >> 15;
}
    if (!fs) {
      int di, ndi_max;
      ndi_max = (32768 - fl + (1 << (0)) - 1) >> (0); ndi_max = (ndi_max - s) >> 1;
      di = std::min(val - i, ndi_max - 1); fl += (2 * di + 1 + s) * (1 << (0));
      fs = (((1 << (0))) < (32768 - fl) ? ((1 << (0))) : (32768 - fl)); *value = (i + di + s) ^ s;
    } else { fs += (1 << (0)); fl += fs & ~s;
}
    opus_assume(fl + fs <= 32768); opus_assume(fs > 0);
}
  ec_encode_bin(enc, fl, fl + fs, 15);
}
static int ec_laplace_decode(ec_dec *dec, unsigned fs, int decay) {
  int val = 0;
  unsigned fl; unsigned fm; fm = ec_decode_bin(dec, 15); fl = 0;
  if (fm >= fs) {
    val++; fl = fs; fs = ec_laplace_get_freq1(fs, decay) + (1 << (0));
    for (; fs > (1 << (0)) && fm >= fl + 2 * fs;) { fs *= 2; fl += fs; fs = ((fs - 2 * (1 << (0))) * (opus_int32)decay) >> 15; fs += (1 << (0)); val++;
}
    if (fs <= (1 << (0))) {
      int di = (fm - fl) >> ((0) + 1);
      val += di; fl += 2 * di * (1 << (0));
}
    if (fm < fl + fs) val = -val;
    else fl += fs;
}
  opus_assume(fl < 32768); opus_assume(fs > 0);
  opus_assume(fl <= fm); opus_assume(fm < std::min(fl + fs, (unsigned)32768));
  ec_dec_update(dec, fl, std::min(fl + fs, (unsigned)32768), 32768);
  return val;
}
static unsigned isqrt32(opus_uint32 _val) {
  unsigned b; unsigned g;
  int bshift;
  g = 0; bshift = ((((int)sizeof(unsigned) * 8) - (__builtin_clz(_val))) - 1) >> 1; b = 1U << bshift;
  for (; bshift >= 0; --bshift, b >>= 1) {
    opus_uint32 t = (((opus_uint32)g << 1) + b) << bshift;
    if (t <= _val) { g += b; _val -= t; }
}
  return g;
}
static void celt_float2int16_c(std::span<const float> in, std::span<short> out) {
  opus_assume(out.size() >= in.size());
  for (auto index = std::size_t{}; index < in.size(); ++index) out[index] = FLOAT2INT16(in[index]);
}
static void clt_mdct_forward_c(const mdct_lookup *l, float *in, float *out, const celt_coef *window, int overlap, int shift, int stride) {
  int i;
  int N, N2, N4;
  const kiss_fft_state *st = l->kfft[shift];
  const float *trig;
  celt_coef scale; scale = st->scale; N = l->n; trig = l->trig;
  for (i = 0; i < shift; i++) { N >>= 1; trig += N;
}
  N2 = N >> 1; N4 = N >> 2;
  auto *f = OPUS_SCRATCH(float, N2);
  auto *f2 = OPUS_SCRATCH(kiss_fft_cpx, N4);
  {
    const float *xp1 = in + (overlap >> 1);
    const float *xp2 = in + N2 - 1 + (overlap >> 1);
    float *yp = f;
    const celt_coef *wp1 = window + (overlap >> 1);
    const celt_coef *wp2 = window + (overlap >> 1) - 1;
    for (i = 0; i < ((overlap+3)>>2); i++) { *yp++ = xp1[N2]*(*wp2)+(*xp2)*(*wp1); *yp++ = (*xp1)*(*wp1)-xp2[-N2]*(*wp2); xp1+=2; xp2-=2; wp1+=2; wp2-=2;
}
    wp1 = window; wp2 = window + overlap - 1;
    for (; i < N4-((overlap+3)>>2); i++) { *yp++ = *xp2; *yp++ = *xp1; xp1+=2; xp2-=2;
}
    for (; i < N4; i++) { *yp++ = -(xp1[-N2]*(*wp1))+(*xp2)*(*wp2); *yp++ = (*xp1)*(*wp2)+xp2[N2]*(*wp1); xp1+=2; xp2-=2; wp1+=2; wp2-=2; }
}
  {
    float *yp = f;
    const float *t = &trig[0];
    for (i = 0; i < N4; i++) { kiss_fft_cpx yc; float t0 = t[i], t1 = t[N4+i], re = *yp++, im = *yp++; float yr = re*t0-im*t1, yi = im*t0+re*t1; yc.r = yr*scale; yc.i = yi*scale; f2[fft_bitrev_index(st, i)] = yc; }
}
  fft_impl(st, f2);
  {
    const kiss_fft_cpx *fp = f2;
    float *yp1 = out;
    float *yp2 = out + stride * (N2 - 1);
    const float *t = &trig[0];
    for (i = 0; i < N4; i++) { float t0 = t[i], t1 = t[N4+i], yr = fp->i*t1-fp->r*t0, yi = fp->r*t1+fp->i*t0; *yp1 = yr; *yp2 = yi; fp++; yp1+=2*stride; yp2-=2*stride; } }
}
static void clt_mdct_backward_transform_c(const mdct_lookup *l, float *in, float *out, int overlap, int shift, int stride) {
  int i;
  int N, N2, N4;
  const float *trig;
  const auto *__restrict in_base = in;
  auto *__restrict out_base = out;
  N = l->n; trig = l->trig;
  for (i = 0; i < shift; i++) { N >>= 1; trig += N;
}
  N2 = N >> 1; N4 = N >> 2;
  {
    const float *xp1 = in_base;
    const float *xp2 = in_base + stride * (N2 - 1);
    float *yp = out_base + (overlap >> 1);
    const float *t = &trig[0];
    const auto *fft_state = l->kfft[shift];
    for (i = 0; i < N4; i++) { int rev = fft_bitrev_index(fft_state, i); opus_val32 x1 = *xp1, x2 = *xp2; float yr = x2*t[i]+x1*t[N4+i], yi = x1*t[i]-x2*t[N4+i]; yp[2*rev+1] = yr; yp[2*rev] = yi; xp1+=2*stride; xp2-=2*stride; }
}
  fft_impl(l->kfft[shift], (kiss_fft_cpx *)(out_base + (overlap >> 1)));
  {
    float *yp0 = out_base + (overlap >> 1);
    float *yp1 = out_base + (overlap >> 1) + N2 - 2;
    const float *t = &trig[0];
    for (i = 0; i < (N4+1)>>1; i++) { float re = yp0[1], im = yp0[0], t0 = t[i], t1 = t[N4+i], yr = re*t0+im*t1, yi = re*t1-im*t0; re = yp1[1]; im = yp1[0]; yp0[0] = yr; yp1[1] = yi; t0 = t[N4-i-1]; t1 = t[N2-i-1]; yr = re*t0+im*t1; yi = re*t1-im*t0; yp1[0] = yr; yp0[1] = yi; yp0+=2; yp1-=2; }
}
}
static void clt_mdct_backward_overlap_c(float *out, const celt_coef *window, int overlap) {
  auto *__restrict o = out;
  const auto *__restrict w = window;
  const int half = overlap >> 1;
  for (int i = 0; i < half; ++i) {
    const int j = overlap - 1 - i;
    const float x1 = o[j], x2 = o[i], w1 = w[i], w2 = w[j];
    o[i] = x2 * w2 - x1 * w1;
    o[j] = x2 * w1 + x1 * w2;
  }
}
static void clt_mdct_backward_c(const mdct_lookup *l, float *in, float *out, const celt_coef *window, int overlap, int shift, int stride) {
  clt_mdct_backward_transform_c(l, in, out, overlap, shift, stride);
  clt_mdct_backward_overlap_c(out, window, overlap);
}
static void clt_mdct_backward_dual_history_c(const mdct_lookup *l, float *in, float *out0, float *out1, const celt_coef *window, int overlap, int shift, int stride) {
  int N = l->n;
  for (int i = 0; i < shift; ++i) N >>= 1;
  clt_mdct_backward_transform_c(l, in, out0, overlap, shift, stride);
  copy_n_items(out0 + (overlap >> 1), static_cast<std::size_t>(N >> 1), out1 + (overlap >> 1));
  clt_mdct_backward_overlap_c(out0, window, overlap);
  clt_mdct_backward_overlap_c(out1, window, overlap);
}
constexpr std::array<opus_int16, 22> eband5ms = numeric_blob_array<opus_int16, 22>(R"blob(000000010002000300040005000600070008000A000C000E001000140018001C002200280030003C004E0064)blob");
static_assert(array_is_strictly_increasing(eband5ms));
constexpr std::array<unsigned char, 231> band_allocation = numeric_blob_array<unsigned char, 231>(R"blob(0000000000000000000000000000000000000000005A504B453F383128221D14120A00000000000000006E645A544E47413A332D27201A140C000000000000766E675D56504B46413B352F281F170F04000000007E7770685F59534E48423C362F272019110C010000867F787267615B554E48423C362F29231D17100A019089827C716B655F58524C464039332D27211A0F0198918A847B756F69625C56504A433D37312B241401A29B948E857F79736C66605A544D47413B352E1E01ACA59E988F89837D76706A645E57514B453F382D14C8C8C8C8C8C8C8C8C6C1BCB7B2ADA8A39E99948168)blob");
constexpr std::array<celt_coef, 120> window120 = numeric_blob_array<celt_coef, 120>(
    R"blob(388D1C6A3A1EBB523ADC69083B57ED823BB263893C052A033C39DC303C773EB43C9EA31C3CC5F2D13CF186FE3D10AB9B3D2AAD053D46C2843D64E6533D8289113D939F873DA5B2CB3DB8BED13DCCBF3A3DE1AF543DF78A143E07250E3E12F4D93E1F315F3E2BD7683E38E38A3E4652303E541F943E6247BF3E70C68E3E7F97B03E875B523E8F0F603E96E5983E9EDB793EA6EE703EAF1BD83EB760FB3EBFBB113EC827463ED0A2B73ED92A783EE1BB943EEA530C3EF2EDDE3EFB89063F0210BE3F065A1F3F0A9F243F0EDE503F13162B3F1745413F1B6A253F1F83733F238FCE3F278DE63F2B7C743F2F5A3F3F3326193F36DEE73F3A83993F3E13333F418CC53F44EF773F483A7F3F4B6D273F4E86CE3F5186E53F546CF13F57388E3F59E9693F5C7F453F5EF9FA3F6159733F639DAF3F65C6C13F67D4CF3F69C8113F6BA0D23F6D5F6E3F6F04503F708FF43F7202E63F735DBD3F74A11F3F75CDBF3F76E4573F77E5B03F78D2973F79ABE33F7A72733F7B27273F7BCAE73F7C5E9D3F7CE3353F7D599C3F7DC2BD3F7E1F863F7E70DE3F7EB7AB3F7EF4CF3F7F29263F7F55863F7F7ABE3F7F99963F7FB2CC3F7FC7143F7FD71C3F7FE3823F7FECDD3F7FF3B63F7FF88A3F7FFBC83F7FFDD63F7FFF073F7FFFA53F7FFFE83F7FFFFD3F800000)blob");
constexpr std::array<opus_int16, 21> logN400 = numeric_blob_array<opus_int16, 21>(R"blob(000000000000000000000000000000000008000800080008001000100010001500150018001D00220024)blob");
constexpr std::array<opus_int16, 105> cache_index50 = numeric_blob_array<opus_int16, 105>(R"blob(FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF000000000000000000290029002900520052007B00A400C800DE000000000000000000000000000000000029002900290029007B007B007B00A400A400F0010A011B012700290029002900290029002900290029007B007B007B007B00F000F000F0010A010A0131013E01480150007B007B007B007B007B007B007B007B00F000F000F000F0013101310131013E013E0157015F0166016C00F000F000F000F000F000F000F000F00131013101310131015701570157015F015F01720178017E0183)blob");
constexpr std::array<unsigned char, 392> cache_bits50 = numeric_blob_array<unsigned char, 392>(R"blob(2807070707070707070707070707070707070707070707070707070707070707070707070707070707280F171C1F22242627292A2B2C2D2E2F2F3132333435363737393A3B3C3D3E3F3F4142434445464747281421293035393D40424547494B4C4E50525557595B5C5E60626567696B6C6E70727577797B7C7E80281727333C43494F53575B5E616466696B6F7376797C7E8183878B8E919496999B9FA3A6A9ACAEB1B3231C31414E59636B72787E84888D9195999FA5ABB0B4B9BDC0C7CDD3D8DCE1E5E8EFF5FB15213A4F61707D89949DA6AEB6BDC3C9CFD9E3EBF3FB11233F566A7B8B98A5B1BBC5CED6DEE6EDFA191F374B5B6975808A929AA1A8AEB4B9BEC8D0D7DEE5EBF0F5FF102441596E80909FADB9C4CFD9E2EAF2FA0B294A678097ACBFD1E1F1FF092B4F6E8AA3BACFE3F60C2747637B90A4B6C6D6E4F1FD092C51718EA8C0D6EBFF07315A7FA0BFDCF706335F86AACBEA072F577B9BB8D4ED06346189AED0F005396A97C0E7053B6F9ECAF305376793BBE0053C71A1CEF804417AAFE004437FB6EA)blob");
constexpr std::array<unsigned char, 168> cache_caps50 = numeric_blob_array<unsigned char, 168>(R"blob(E0E0E0E0E0E0E0E0A0A0A0A0B9B9B9B2B2A8863D25E0E0E0E0E0E0E0E0F0F0F0F0CFCFCFC6C6B7904228A0A0A0A0A0A0A0A0B9B9B9B9C1C1C1B7B7AC8A4026F0F0F0F0F0F0F0F0CFCFCFCFCCCCCCC1C1B48F4228B9B9B9B9B9B9B9B9C1C1C1C1C1C1C1B7B7AC8A4127CFCFCFCFCFCFCFCFCCCCCCCCC9C9C9BCBCB08D4228C1C1C1C1C1C1C1C1C1C1C1C1C2C2C2B8B8AD8B4127CCCCCCCCCCCCCCCCC9C9C9C9C6C6C6BBBBAF8C4228)blob");
constexpr std::array<kiss_twiddle_cpx, 381> fft_twiddle_prefix = numeric_blob_pair_array<kiss_twiddle_cpx, float, 381>(R"blob(3F800000800000003F7FFA63BC5675BF3F7FE98BBCD6710A3F7FCD79BD20CEE73F7FA62FBD565E3A3F7F73AFBD85F2133F7F35F9BDA0AF2A3F7EED12BDBB65333F7E98FDBDD613043F7E39BCBDF0B7733F7DCF55BE05A8A83F7D59CBBE12EFBB3F7CD925BE20305C3F7C4D67BE2D69F53F7BB698BE3A9BF33F7B14BEBE47C5C23F7A67E2BE54E6CD3F79B009BE61FE823F78ED3CBE6F0C4D3F781F84BE7C0F9C3F7746EABE8483EE3F766377BE8AFA3E3F757536BE916A753F747C30BE97D44C3F737871BE9E377A3F726A03BEA493B73F7150F4BEAAE8BC3F702D4FBEB136413F6EFF21BEB77C013F6DC676BEBDB9B43F6C835EBEC3EF153F6B35E7BECA1BDE3F69DE1EBED03FC93F687C12BED65A923F670FD4BEDC6BF33F659974BEE273AA3F641901BEE871713F628E8DBEEE65073F60FA28BEF44E273F5F5BE6BEFA2C903F5DB3D7BF0000003F5C020FBF02E41B3F5A46A0BF05C2773F58819EBF089AF63F56B31DBF0B6D773F54DB31BF0E39DA3F52F9EFBF1100003F510F6CBF13BFCA3F4F1BBDBF1679183F4D1EF8BF192BCD3F4B1934BF1BD7CA3F490A88BF1E7CF13F46F30ABF211B243F44D2D1BF23B2463F42A9F7BF26423A3F407893BF28CAE33F3E3EBDBF2B4C253F3BFC8FBF2DC5E33F39B222BF3038013F375F90BF32A2653F3504F3BF3504F33F32A265BF375F903F303801BF39B2223F2DC5E3BF3BFC8F3F2B4C25BF3E3EBD3F28CAE3BF4078933F26423ABF42A9F73F23B246BF44D2D13F211B24BF46F30A3F1E7CF1BF490A883F1BD7CABF4B19343F192BCDBF4D1EF83F167918BF4F1BBD3F13BFCABF510F6C3F110000BF52F9EF3F0E39DABF54DB313F0B6D77BF56B31D3F089AF6BF58819E3F05C277BF5A46A03F02E41BBF5C020F3F000000BF5DB3D73EFA2C90BF5F5BE63EF44E27BF60FA283EEE6507BF628E8D3EE87171BF6419013EE273AABF6599743EDC6BF3BF670FD43ED65A92BF687C123ED03FC9BF69DE1E3ECA1BDEBF6B35E73EC3EF15BF6C835E3EBDB9B4BF6DC6763EB77C01BF6EFF213EB13641BF702D4F3EAAE8BCBF7150F43EA493B7BF726A033E9E377ABF7378713E97D44CBF747C303E916A75BF7575363E8AFA3EBF7663773E8483EEBF7746EA3E7C0F9CBF781F843E6F0C4DBF78ED3C3E61FE82BF79B0093E54E6CDBF7A67E23E47C5C2BF7B14BE3E3A9BF3BF7BB6983E2D69F5BF7C4D673E20305CBF7CD9253E12EFBBBF7D59CB3E05A8A8BF7DCF553DF0B773BF7E39BC3DD61304BF7E98FD3DBB6533BF7EED123DA0AF2ABF7F35F93D85F213BF7F73AF3D565E3ABF7FA62F3D20CEE7BF7FCD793CD6710ABF7FE98B3C5675BFBF7FFA63248D3132BF800000BC5675BFBF7FFA63BCD6710ABF7FE98BBD20CEE7BF7FCD79BD565E3ABF7FA62FBD85F213BF7F73AFBDA0AF2ABF7F35F9BDBB6533BF7EED12BDD61304BF7E98FDBDF0B773BF7E39BCBE05A8A8BF7DCF55BE12EFBBBF7D59CBBE20305CBF7CD925BE2D69F5BF7C4D67BE3A9BF3BF7BB698BE47C5C2BF7B14BEBE54E6CDBF7A67E2BE61FE82BF79B009BE6F0C4DBF78ED3CBE7C0F9CBF781F84BE8483EEBF7746EABE8AFA3EBF766377BE916A75BF757536BE97D44CBF747C30BE9E377ABF737871BEA493B7BF726A03BEAAE8BCBF7150F4BEB13641BF702D4FBEB77C01BF6EFF21BEBDB9B4BF6DC676BEC3EF15BF6C835EBECA1BDEBF6B35E7BED03FC9BF69DE1EBED65A92BF687C12BEDC6BF3BF670FD4BEE273AABF659974BEE87171BF641901BEEE6507BF628E8DBEF44E27BF60FA28BEFA2C90BF5F5BE6BF000000BF5DB3D7BF02E41BBF5C020FBF05C277BF5A46A0BF089AF6BF58819EBF0B6D77BF56B31DBF0E39DABF54DB31BF110000BF52F9EFBF13BFCABF510F6CBF167918BF4F1BBDBF192BCDBF4D1EF8BF1BD7CABF4B1934BF1E7CF1BF490A88BF211B24BF46F30ABF23B246BF44D2D1BF26423ABF42A9F7BF28CAE3BF407893BF2B4C25BF3E3EBDBF2DC5E3BF3BFC8FBF303801BF39B222BF32A265BF375F90BF3504F3BF3504F3BF375F90BF32A265BF39B222BF303801BF3BFC8FBF2DC5E3BF3E3EBDBF2B4C25BF407893BF28CAE3BF42A9F7BF26423ABF44D2D1BF23B246BF46F30ABF211B24BF490A88BF1E7CF1BF4B1934BF1BD7CABF4D1EF8BF192BCDBF4F1BBDBF167918BF510F6CBF13BFCABF52F9EFBF110000BF54DB31BF0E39DABF56B31DBF0B6D77BF58819EBF089AF6BF5A46A0BF05C277BF5C020FBF02E41BBF5DB3D7BF000000BF5F5BE6BEFA2C90BF60FA28BEF44E27BF628E8DBEEE6507BF641901BEE87171BF659974BEE273AABF670FD4BEDC6BF3BF687C12BED65A92BF69DE1EBED03FC9BF6B35E7BECA1BDEBF6C835EBEC3EF15BF6DC676BEBDB9B4BF6EFF21BEB77C01BF702D4FBEB13641BF7150F4BEAAE8BCBF726A03BEA493B7BF737871BE9E377ABF747C30BE97D44CBF757536BE916A75BF766377BE8AFA3EBF7746EABE8483EEBF781F84BE7C0F9CBF78ED3CBE6F0C4DBF79B009BE61FE82BF7A67E2BE54E6CDBF7B14BEBE47C5C2BF7BB698BE3A9BF3BF7C4D67BE2D69F5BF7CD925BE20305CBF7D59CBBE12EFBBBF7DCF55BE05A8A8BF7E39BCBDF0B773BF7E98FDBDD61304BF7EED12BDBB6533BF7F35F9BDA0AF2ABF7F73AFBD85F213BF7FA62FBD565E3ABF7FCD79BD20CEE7BF7FE98BBCD6710ABF7FFA63BC5675BFBF800000A50D3132BF7FFA633C5675BFBF7FE98B3CD6710ABF7FCD793D20CEE7BF7FA62F3D565E3ABF7F73AF3D85F213BF7F35F93DA0AF2ABF7EED123DBB6533BF7E98FD3DD61304BF7E39BC3DF0B773BF7DCF553E05A8A8BF7D59CB3E12EFBBBF7CD9253E20305CBF7C4D673E2D69F5BF7BB6983E3A9BF3BF7B14BE3E47C5C2BF7A67E23E54E6CDBF79B0093E61FE82BF78ED3C3E6F0C4DBF781F843E7C0F9CBF7746EA3E8483EEBF7663773E8AFA3EBF7575363E916A75BF747C303E97D44CBF7378713E9E377ABF726A033EA493B7BF7150F43EAAE8BCBF702D4F3EB13641BF6EFF213EB77C01BF6DC6763EBDB9B4BF6C835E3EC3EF15BF6B35E73ECA1BDEBF69DE1E3ED03FC9BF687C123ED65A92BF670FD43EDC6BF3BF6599743EE273AABF6419013EE87171BF628E8D3EEE6507BF60FA283EF44E27BF5F5BE63EFA2C90BF5DB3D73F000000BF5C020F3F02E41BBF5A46A03F05C277BF58819E3F089AF6BF56B31D3F0B6D77BF54DB313F0E39DABF52F9EF3F110000BF510F6C3F13BFCABF4F1BBD3F167918BF4D1EF83F192BCDBF4B19343F1BD7CABF490A883F1E7CF1BF46F30A3F211B24BF44D2D13F23B246BF42A9F73F26423ABF4078933F28CAE3BF3E3EBD3F2B4C25BF3BFC8F3F2DC5E3BF39B2223F303801BF375F903F32A265BF3504F33F3504F3BF32A2653F375F90BF3038013F39B222BF2DC5E33F3BFC8FBF2B4C253F3E3EBDBF28CAE33F407893BF26423A3F42A9F7BF23B2463F44D2D1BF211B243F46F30ABF1E7CF13F490A88BF1BD7CA3F4B1934BF192BCD3F4D1EF8BF1679183F4F1BBDBF13BFCA3F510F6CBF1100003F52F9EFBF0E39DA3F54DB31BF0B6D773F56B31DBF089AF63F58819EBF05C2773F5A46A0BF02E41B3F5C020FBF0000003F5DB3D7BEFA2C903F5F5BE6BEF44E273F60FA28BEEE65073F628E8DBEE871713F641901BEE273AA3F659974BEDC6BF33F670FD4BED65A923F687C12BED03FC93F69DE1EBECA1BDE3F6B35E7BEC3EF153F6C835EBEBDB9B43F6DC676BEB77C013F6EFF21BEB136413F702D4FBEAAE8BC3F7150F4BEA493B73F726A03BE9E377A3F737871BE97D44C3F747C30BE916A753F757536BE8AFA3E3F766377BE8483EE3F7746EABE7C0F9C3F781F84BE6F0C4D3F78ED3CBE61FE823F79B009BE54E6CD3F7A67E2BE47C5C23F7B14BEBE3A9BF33F7BB698BE2D69F53F7C4D67BE20305C3F7CD925BE12EFBB3F7D59CBBE05A8A83F7DCF55BDF0B7733F7E39BCBDD613043F7E98FDBDBB65333F7EED12BDA0AF2A3F7F35F9BD85F2133F7F73AFBD565E3A3F7FA62FBD20CEE73F7FCD79BCD6710A3F7FE98BBC5675BF3F7FFA63A553C9CA3F8000003C5675BF3F7FFA633CD6710A3F7FE98B3D20CEE73F7FCD793D565E3A3F7FA62F3D85F2133F7F73AF3DA0AF2A3F7F35F93DBB65333F7EED123DD613043F7E98FD3DF0B7733F7E39BC3E05A8A83F7DCF553E12EFBB3F7D59CB3E20305C3F7CD9253E2D69F53F7C4D673E3A9BF33F7BB6983E47C5C23F7B14BE3E54E6CD3F7A67E23E61FE823F79B0093E6F0C4D3F78ED3C3E7C0F9C3F781F843E8483EE3F7746EA)blob");
static_assert(fft_twiddle_prefix.size() > 380);
struct CeltMdctFftSlab { std::array<celt_coef, 1800> mdct; };
constexpr CeltMdctFftSlab celt_mdct_fft_slab{numeric_blob_array<celt_coef, 1800>(R"blob(3F7FFFFF3F7FFF8E3F7FFE6A3F7FFC933F7FFA073F7FF6C83F7FF2D63F7FEE303F7FE8D63F7FE2C83F7FDC073F7FD4933F7FCC6A3F7FC38F3F7FBA003F7FAFBD3F7FA4C73F7F991D3F7F8CC03F7F7FB03F7F71ED3F7F63763F7F544B3F7F446E3F7F33DE3F7F229A3F7F10A33F7EFDFA3F7EEA9D3F7ED68D3F7EC1CB3F7EAC563F7E962E3F7E7F533F7E67C63F7E4F863F7E36943F7E1CEF3F7E02983F7DE78F3F7DCBD33F7DAF663F7D92463F7D74743F7D55F13F7D36BC3F7D16D53F7CF63C3F7CD4F23F7CB2F63F7C90493F7C6CEB3F7C48DB3F7C241B3F7BFEA93F7BD8873F7BB1B43F7B8A303F7B61FC3F7B39173F7B0F823F7AE53D3F7ABA483F7A8EA23F7A624D3F7A35493F7A07943F79D9303F79AA1D3F797A5A3F7949E93F7918C83F78E6F93F78B47B3F78814E3F784D733F7818EA3F77E3B23F77ADCD3F77773A3F773FF93F77080A3F76CF6E3F7696253F765C2F3F76218C3F75E63C3F75AA403F756D973F7530423F74F2413F74B3943F74743B3F7434373F73F3873F73B22C3F7370273F732D763F72EA1A3F72A6143F7261643F721C0A3F71D6053F718F573F7148003F70FFFF3F70B7553F706E023F7024063F6FD9623F6F8E153F6F42203F6EF5843F6EA83F3F6E5A543F6E0BC03F6DBC863F6D6CA53F6D1C1D3F6CCAEF3F6C791B3F6C26A03F6BD3813F6B7FBB3F6B2B503F6AD6403F6A808C3F6A2A323F69D3353F697B933F69234E3F68CA643F6870D83F6816A83F67BBD53F6760603F6704483F66A78F3F664A333F65EC363F658D973F652E573F64CE773F646DF53F640CD43F63AB123F6348B13F62E5B03F6282103F621DD13F61B8F33F6153773F60ED5D3F6086A43F601F4F3F5FB75B3F5F4ECB3F5EE59E3F5E7BD53F5E11703F5DA66F3F5D3AD23F5CCE9A3F5C61C73F5BF4593F5B86513F5B17AE3F5AA8733F5A389D3F59C82F3F5957273F58E5873F58734F3F58007F3F578D173F5719183F56A4823F562F563F55B9933F55433A3F54CC4B3F5454C73F53DCAE3F5364013F52EABF3F5270E93F51F67F3F517B823F50FFF23F5083CF3F50071A3F4F89D33F4F0BFA3F4E8D903F4E0E953F4D8F093F4D0EED3F4C8E413F4C0D063F4B8B3B3F4B08E13F4A85F93F4A02833F497E7F3F48F9EE3F4874D03F47EF243F4768ED3F46E22A3F465ADB3F45D3003F454A9C3F44C1AC3F4438323F43AE2F3F4323A33F42988D3F420CEF3F4180C83F40F41A3F4066E53F3FD9283F3F4AE53F3EBC1B3F3E2CCC3F3D9CF73F3D0C9D3F3C7BBF3F3BEA5C3F3B58753F3AC60B3F3A331D3F399FAD3F390BBB3F3877473F37E2513F374CDA3F36B6E33F36206C3F3589743F34F1FD3F345A083F33C1933F3328A13F328F303F31F5433F315AD83F30BFF13F30248E3F2F88B03F2EEC563F2E4F813F2DB2323F2D14693F2C76273F2BD76C3F2B38383F2A988B3F29F8683F2957CC3F28B6BA3F2815323F2773343F26D0C03F262DD73F258A793F24E6A73F2442623F239DA93F22F87D3F2252DF3F21ACCF3F21064E3F205F5B3F1FB7F83F1F10253F1E67E23F1DBF313F1D16103F1C6C813F1BC2843F1B181A3F1A6D443F19C2003F1916513F186A373F17BDB13F1710C13F1663673F15B5A43F1507773F1458E23F13A9E43F12FA7F3F124AB33F119A803F10E9E73F1038E83F0F87843F0ED5BC3F0E238F3F0D70FE3F0CBE0A3F0C0AB33F0B56FA3F0AA2DF3F09EE633F0939863F0884493F07CEAC3F0718B03F0662553F05AB9B3F04F4843F043D0F3F03853E3F02CD103F0214863F015BA13F00A2623EFFD1903EFE5DA83EFCE90E3EFB73C33EF9FDC73EF8871C3EF70FC23EF597BB3EF41F073EF2A5A83EF12B9F3EEFB0ED3EEE35923EECB9903EEB3CE93EE9BF9B3EE841AA3EE6C3163EE543E03EE3C4093EE243923EE0C27C3EDF40C93EDDBE793EDC3B8D3EDAB8073ED933E73ED7AF2F3ED629E03ED4A3FA3ED31D7F3ED1966F3ED00ECD3ECE86983ECCFDD33ECB747E3EC9EA9A3EC860283EC6D52A3EC549A03EC3BD8B3EC230ED3EC0A3C73EBF161A3EBD87E73EBBF92E3EBA69F23EB8DA333EB749F23EB5B9303EB427EF3EB296303EB103F33EAF713A3EADDE053EAC4A573EAAB6303EA921913EA78C7B3EA5F6F03EA460F13EA2CA7D3EA133983E9F9C413E9E047B3E9C6C453E9AD3A23E993A923E97A1173E9607313E946CE23E92D22B3E91370C3E8F9B883E8DFF9F3E8C63523E8AC6A33E8929923E878C213E85EE513E8450233E82B1983E8112B23E7EE6E13E7BA7AC3E7867C63E7527313E71E5F13E6EA4073E6B61753E681E3E3E64DA643E6195EA3E5E50D13E5B0B1D3E57C4CE3E547DE83E51366E3E4DEE5F3E4AA5C13E475C943E4412DC3E40C89A3E3D7DD13E3A32823E36E6B13E339A5F3E304D903E2D00453E29B2803E2664443E2315943E1FC6713E1C76DE3E1926DD3E15D6703E12859A3E0F345E3E0BE2BD3E0890BA3E053E573E01EB973DFD30F63DF68A0C3DEFE2763DE93A373DE291563DDBE7D53DD53DB93DCE93083DC7E7C63DC13BF73DBA8FA13DB3E2C83DAD35713DA687A03D9FD95B3D992AA53D927B833D8BCBFB3D851C113D7CD7923D6F76523D6214683D54B1E13D474EC43D39EB1B3D2C86F03D1F224C3D11BD383D0457BD3CEDE3CC3CD317773CB84A8D3C9D7D233C82AF493C4FC2283C1A252C3BC90F883B3BA855B9D6774FBB714616BBE3DE4CBC278C76BC5D2952BC8962C9BCA43088BCBEFDD5BCD9CA9BBCF496C8BD07B125BD151687BD227B80BD2FE007BD3D4412BD4AA799BD580A91BD656CF2BD72CEB2BD8017E4BD86C814BD8D77E7BD942756BD9AD65CBDA184F7BDA83320BDAEE0D3BDB58E0BBDBC3AC3BDC2E6F8BDC992A4BDD03DC2BDD6E84FBDDD9243BDE43B9DBDEAE458BDF18C6BBDF833D7BDFEDA94BE02C04FBE0612F8BE096544BE0CB72EBE1008B7BE1359DABE16AA95BE19FAE7BE1D4ACDBE209A44BE23E94ABE2737DEBE2A85FCBE2DD3A3BE3120D0BE346D80BE37B9B1BE3B0562BE3E5090BE419B37BE44E558BE482EEDBE4B77F7BE4EC071BE52085ABE554FB0BE589670BE5BDC98BE5F2226BE626718BE65AB6ABE68EF1BBE6C3229BE6F7491BE72B651BE75F766BE7937CFBE7C778ABE7FB692BE817A74BE831944BE84B7B8BE8655CEBE87F387BE8990E0BE8B2DD8BE8CCA6EBE8E66A2BE900272BE919DDDBE9338E2BE94D37FBE966DB4BE980780BE99A0E0BE9B39D5BE9CD25DBE9E6A77BEA00222BEA1995CBEA33025BEA4C67BBEA65C5EBEA7F1CCBEA986C4BEAB1B45BEACAF4EBEAE42DEBEAFD5F4BEB1688EBEB2FAACBEB48C4BBEB61D6DBEB7AE0EBEB93E2FBEBACDCDBEBC5CE9BEBDEB80BEBF7992BEC1071EBEC29422BEC4209DBEC5AC8FBEC737F6BEC8C2D2BECA4D20BECBD6E0BECD6012BECEE8B3BED070C3BED1F841BED37F2BBED50581BED68B42BED8106BBED994FDBEDB18F7BEDC9C57BEDE1F1CBEDFA145BEE122D1BEE2A3BFBEE4240EBEE5A3BDBEE722CBBEE8A136BEEA1EFEBEEB9C22BEED18A0BEEE9479BEF00FA9BEF18A32BEF30410BEF47D44BEF5F5CDBEF76DA9BEF8E4D7BEFA5B57BEFBD126BEFD4646BEFEBAB3BF001737BF00D0BABF0189E3BF0242B1BF02FB24BF03B33ABF046AF5BF052252BF05D952BF068FF4BF074638BF07FC1CBF08B1A1BF0966C7BF0A1B8BBF0ACFEFBF0B83F2BF0C3792BF0CEAD0BF0D9DABBF0E5023BF0F0237BF0FB3E7BF106532BF111617BF11C696BF1276B0BF132662BF13D5ADBF148491BF15330CBF15E11EBF168EC8BF173C07BF17E8DDBF189547BF194147BF19ECDBBF1A9803BF1B42BFBF1BED0EBF1C96EFBF1D4062BF1DE967BF1E91FEBF1F3A24BF1FE1DCBF208922BF212FF9BF21D65EBF227C51BF2321D3BF23C6E2BF246B7EBF250FA7BF25B35BBF26569CBF26F968BF279BBEBF283D9FBF28DF0ABF297FFEBF2A207CBF2AC082BF2B6010BF2BFF26BF2C9DC3BF2D3BE7BF2DD991BF2E76C2BF2F1378BF2FAFB3BF304B73BF30E6B7BF31817FBF321BCABF32B598BF334EE9BF33E7BCBF348011BF3517E7BF35AF3EBF364616BF36DC6DBF377244BF38079BBF389C70BF3930C4BF39C496BF3A57E5BF3AEAB2BF3B7CFBBF3C0EC1BF3CA003BF3D30C0BF3DC0F9BF3E50ACBF3EDFDABF3F6E82BF3FFCA4BF408A3FBF411753BF41A3DFBF422FE3BF42BB5FBF434652BF43D0BDBF445A9EBF44E3F5BF456CC2BF45F504BF467CBBBF4703E8BF478A88BF48109CBF489624BF491B1FBF499F8DBF4A236EBF4AA6C1BF4B2985BF4BABBBBF4C2D62BF4CAE79BF4D2F01BF4DAEF9BF4E2E61BF4EAD38BF4F2B7EBF4FA932BF502655BF50A2E5BF511EE3BF519A4FBF521527BF528F6CBF53091DBF53823ABF53FAC3BF5472B6BF54EA15BF5560DEBF55D712BF564CAFBF56C1B6BF573626BF57A9FFBF581D41BF588FEBBF5901FEBF597377BF59E459BF5A54A1BF5AC450BF5B3366BF5BA1E1BF5C0FC3BF5C7D0ABF5CE9B6BF5D55C8BF5DC13DBF5E2C18BF5E9656BF5EFFF8BF5F68FEBF5FD167BF603933BF60A061BF6106F2BF616CE5BF61D239BF6236F0BF629B07BF62FE7FBF636158BF63C392BF64252BBF648625BF64E67EBF654636BF65A54EBF6603C4BF666199BF66BECCBF671B5DBF67774DBF67D299BF682D43BF68874ABF68E0AEBF69396EBF69918BBF69E904BF6A3FD8BF6A9608BF6AEB94BF6B407ABF6B94BCBF6BE858BF6C3B4FBF6C8DA0BF6CDF4ABF6D304FBF6D80ADBF6DD064BF6E1F75BF6E6DDEBF6EBBA0BF6F08BBBF6F552DBF6FA0F8BF6FEC1BBF703695BF708066BF70C98FBF71120FBF7159E5BF71A113BF71E796BF722D70BF7272A0BF72B726BF72FB01BF733E32BF7380B8BF73C293BF7403C3BF744448BF748422BF74C34FBF7501D2BF753FA8BF757CD2BF75B94FBF75F520BF763045BF766ABDBF76A488BF76DDA6BF771616BF774DD9BF7784EFBF77BB57BF77F110BF78261CBF785A7ABF788E29BF78C12BBF78F37DBF792521BF795616BF79865BBF79B5F2BF79E4DABF7A1312BF7A409ABF7A6D73BF7A999CBF7AC516BF7AEFDFBF7B19F8BF7B4361BF7B6C1ABF7B9422BF7BBB79BF7BE220BF7C0816BF7C2D5CBF7C51F0BF7C75D3BF7C9905BF7CBB85BF7CDD55BF7CFE73BF7D1EDFBF7D3E9ABF7D5DA2BF7D7BF9BF7D999FBF7DB692BF7DD2D3BF7DEE62BF7E093FBF7E2369BF7E3CE1BF7E55A7BF7E6DBABF7E851BBF7E9BC9BF7EB1C4BF7EC70CBF7EDBA2BF7EEF85BF7F02B5BF7F1532BF7F26FCBF7F3812BF7F4876BF7F5827BF7F6724BF7F756EBF7F8305BF7F8FE8BF7F9C18BF7FA795BF7FB25EBF7FBC74BF7FC5D7BF7FCE85BF7FD681BF7FDDC8BF7FE45DBF7FEA3DBF7FEF6ABF7FF3E3BF7FF7A9BF7FFABBBF7FFD19BF7FFEC4BF7FFFBB3F7FFFFA3F7FFE393F7FF9A93F7FF24B3F7FE81E3F7FDB223F7FCB593F7FB8C13F7FA35B3F7F8B283F7F70273F7F525A3F7F31BF3F7F0E583F7EE8253F7EBF263F7E935C3F7E64C83F7E33693F7DFF413F7DC84F3F7D8E963F7D52143F7D12CB3F7CD0BC3F7C8BE73F7C444D3F7BF9EF3F7BACCD3F7B5CE93F7B0A433F7AB4DD3F7A5CB63F7A01D13F79A42E3F7943CE3F78E0B33F787ADC3F78124C3F77A7043F7739043F76C84F3F7654E43F75DEC63F7565F63F74EA753F746C443F73EB653F7367DA3F72E1A33F7258C23F71CD393F713F093F70AE343F701ABB3F6F84A03F6EEBE43F6E508A3F6DB2933F6D12013F6C6ED53F6BC9113F6B20B73F6A75CA3F69C84A3F6918393F68659B3F67B0703F66F8BA3F663E7C3F6581B83F64C26F3F6400A53F633C5A3F6275913F61AC4C3F60E08E3F6012593F5F41AE3F5E6E913F5D99033F5CC1083F5BE6A03F5B09CF3F5A2A983F5948FB3F5864FD3F577EA03F5695E53F55AAD03F54BD633F53CDA13F52DB8C3F51E7273F50F0753F4FF7793F4EFC343F4DFEAB3F4CFEDF3F4BFCD43F4AF88C3F49F20A3F48E9523F47DE653F46D1473F45C1FB3F44B0843F439CE53F4287213F416F3A3F4055353F3F39133F3E1AD93F3CFA893F3BD8263F3AB3B43F398D363F3864AF3F373A233F360D943F34DF063F33AE7C3F327BF93F3147823F3011193F2ED8C23F2D9E803F2C62563F2B24493F29E45B3F28A2903F275EEB3F2619713F24D2253F23890A3F223E233F20F1763F1FA3043F1E52D23F1D00E43F1BAD3D3F1A57E13F1900D43F17A8193F164DB43F14F1AA3F1393FD3F1234B23F10D3CD3F0F71513F0E0D423F0CA7A53F0B407C3F09D7CD3F086D9B3F0701E93F0594BD3F04261A3F02B6043F01447E3EFFA31D3EFCBA6F3EF9CEFB3EF6E0CB3EF3EFE53EF0FC523EEE061B3EEB0D483EE811E03EE513EE3EE213783EDF10883EDC0B253ED903583ED5F92B3ED2ECA53ECFDDCE3ECCCCB03EC9B9533EC6A3C03EC38BFF3EC072193EBD56173EBA38013EB717E13EB3F5BE3EB0D1A33EADAB963EAA83A33EA759D03EA42E283EA100B33E9DD17A3E9AA0863E976DE03E9439913E9103A13E8DCC1B3E8A93063E87586C3E841C573E80DECF3E7B3FB83E74BF123E6E3BBE3E67B5CC3E612D503E5AA25C3E5415013E4D85533E46F3653E405F483E39C90F3E3330CC3E2C96933E25FA743E1F5C843E18BCD53E121B793E0B78823E04D4043DFC5C213DEF0D783DE1BC2F3DD4686B3DC712543DB9BA0D3DAC5FBE3D9F038A3D91A5983D84460D3D6DCA1D3D5305853D383E9D3D1D75AF3D02AB073CCFBDE23C9A236D3C490E903BBBA822BA56774EBBF145ABBC63DCE3BCA78A39BCDD2429BD095DD7BD242817BD3EF08ABD59B6E6BD747ADEBD879E14BD94FD3CBDA25AC2BDAFB680BDBD1051BDCA680FBDD7BD95BDE510BEBDF26163BDFFAF60BE067D47BE0D2165BE13C3F7BE1A64EABE21042BBE27A1A8BE2E3D4FBE34D70DBE3B6ECFBE420483BE489816BE4F2976BE55B890BE5C4553BE62CFABBE695786BE6FDCD2BE765F7DBE7CDF74BE81AE53BE84EB7FBE882737BE8B6170BE8E9A22BE91D143BE9506CCBE983AB2BE9B6CECBE9E9D72BEA1CC3BBEA4F93EBEA82471BEAB4DCDBEAE7547BEB19AD8BEB4BE77BEB7E019BEBAFFB8BEBE1D49BEC138C6BEC45223BEC76959BECA7E60BECD912EBED0A1BBBED3AFFEBED6BBEFBED9C585BEDCCCB8BEDFD17FBEE2D3D1BEE5D3A7BEE8D0F8BEEBCBBBBEEEC3E8BEF1B977BEF4AC5FBEF79C98BEFA8A1ABEFD74DDBF002E6CBF01A102BF03122CBF0481E6BF05F02BBF075CFABF08C84BBF0A321DBF0B9A6BBF0D0131BF0E666BBF0FCA16BF112C2CBF128CACBF13EB8FBF1548D4BF16A475BF17FE70BF1956BFBF1AAD61BF1C0250BF1D5589BF1EA709BF1FF6CBBF2144CCBF229108BF23DB7CBF252423BF266AFCBF27B001BF28F32FBF2A3483BF2B73F9BF2CB18EBF2DED3EBF2F2706BF305EE2BF3194CFBF32C8C9BF33FACEBF352AD9BF3658E7BF3784F6BF38AF02BF39D707BF3AFD02BF3C20F0BF3D42CFBF3E6299BF3F804EBF409BE9BF41B567BF42CCC5BF43E201BF44F516BF460603BF4714C3BF482155BF492BB5BF4A33E0BF4B39D4BF4C3D8DBF4D3F08BF4E3E44BF4F3B3CBF5035EFBF512E59BF522478BF531849BF5409C9BF54F8F6BF55E5CDBF56D04CBF57B870BF589E36BF59819CBF5A629FBF5B413DBF5C1D74BF5CF741BF5DCEA1BF5EA393BF5F7614BF604621BF6113B9BF61DED9BF62A77FBF636DA8BF643153BF64F27EBF65B125BF666D48BF6726E4BF67DDF7BF689280BF69447BBF69F3E7BF6AA0C3BF6B4B0CBF6BF2C0BF6C97DEBF6D3A64BF6DDA4FBF6E779FBF6F1252BF6FAA66BF703FD8BF70D2A8BF7162D4BF71F05BBF727B3ABF730370BF7388FDBF740BDDBF748C11BF750996BF75846BBF75FC8FBF767200BF76E4BDBF7754C5BF77C217BF782CB2BF789493BF78F9BBBF795C28BF79BBD9BF7A18CCBF7A7302BF7ACA78BF7B1F2FBF7B7124BF7BC058BF7C0CC9BF7C5676BF7C9D5FBF7CE182BF7D22E0BF7D6177BF7D9D47BF7DD64FBF7E0C8EBF7E4004BF7E70B0BF7E9E92BF7EC9A9BF7EF1F5BF7F1775BF7F3A29BF7F5A10BF7F772BBF7F9178BF7FA8F8BF7FBDAABF7FCF8FBF7FDEA5BF7FEAEDBF7FF466BF7FFB11BF7FFEED3F7FFFEA3F7FF8E53F7FE6A63F7FC92D3F7FA07C3F7F6C953F7F2D793F7EE32C3F7E8DB13F7E2D0B3F7DC13F3F7D4A523F7CC8483F7C3B283F7BA2F73F7AFFBD3F7A51803F7998483F78D41E3F7805093F772B133F7646463F7556AC3F745C4E3F7357383F7247763F712D133F70081C3F6ED89E3F6D9EA53F6C5A403F6B0B7E3F69B26B3F684F193F66E1963F6569F23F63E83E3F625C8B3F60C6EA3F5F276D3F5D7E263F5BCB283F5A0E863F5848533F5678A33F549F8B3F52BD203F50D1763F4EDCA33F4CDEBD3F4AD7DB3F48C8133F46AF7C3F448E2F3F4264423F4031CE3F3DF6EC3F3BB3B53F3968423F3714AD3F34B9103F3255863F2FEA293F2D77153F2AFC653F287A353F25F0A13F235FC63F20C7C03F1E28AD3F1B82A93F18D5D43F16224A3F13682A3F10A7943F0DE0A43F0B137C3F0840393F0566FD3F0287E73EFF462E3EF9715B3EF391983EEDA7253EE7B2463EE1B33D3EDBAA4D3ED597BB3ECF7BCA3EC956BF3EC328E03EBCF2713EB6B3B83EB06CFC3EAA1E823EA3C8933E9D6B743E97076D3E909CC63E8A2BC83E83B4BA3E7A6FCA3E6D6B243E605C133E53432B3E4621003E38F6233E2BC3293E1E88A73E1147303E03FF5A3DED63733DD2BDC83DB80EDF3D9D57E33D829A013D4FACC73D1A1C713CC90AB03C3BA759BAD67749BC7143FEBCE3D73FBD278140BD5D0F88BD894A0FBDA40654BDBEBB67BDD9681DBDF40B48BE0751E0BE14982CBE21D7F4BE2F10A2BE3C41A3BE496A61BE568A4ABE63A0C9BE70AD4DBE7DAF41BE85530BBE8BC89CBE92380BBE98A111BE9F0364BEA55EBEBEABB2D7BEB1FF68BEB8442ABEBE80D6BEC4B528BECAE0D9BED103A3BED71D43BEDD2D73BEE333EFBEE93074BEEF22BEBEF50A8CBEFAE799BF005CD3BF034037BF061DDABF08F59BBF0BC759BF0E92F7BF115853BF14174FBF16CFCCBF1981ABBF1C2CCFBF1ED119BF216E6CBF2404AABF2693B6BF291B74BF2B9BC6BF2E1492BF3085BBBF32EF25BF3550B6BF37AA54BF39FBE3BF3C4549BF3E866DBF40BF36BF42EF8ABF451752BF473674BF494CD9BF4B5A6ABF4D5F0FBF4F5AB2BF514D3DBF533699BF5516B2BF56ED72BF58BAC4BF5A7E95BF5C38CFBF5DE962BF5F9038BF612D40BF62C067BF64499CBF65C8CDBF673DEABF68A8E3BF6A09A7BF6B6026BF6CAC53BF6DEE1EBF6F2579BF705257BF7174AABF728C66BF73997EBF749BE7BF759395BF76807DBF776295BF7839D4BF79062FBF79C79DBF7A7E17BF7B2994BF7BCA0CBF7C5F7ABF7CE9D5BF7D6918BF7DDD3EBF7E4640BF7EA41CBF7EF6CCBF7F3E4DBF7F7A9CBF7FABB6BF7FD199BF7FEC43BF7FFBB43F7FFFA63F7FE3943F7F9A9C3F7F24CC3F7E82383F7DB2FD3F7CB73F3F7B8F2A3F7A3AF33F78BAD43F770F113F7537F63F7335D53F7109083F6EB1F13F6C30F93F6986903F66B32F3F63B7533F6093843F5D484E3F59D6453F563E033F52802C3F4E9D653F4A965E3F466BCC3F421E6A3F3DAEF93F391E413F346D0D3F2F9C323F2AAC883F259EEB3F2074403F1B2D6E3F15CB623F104F0E3F0AB9683F050B6C3EFE8C2F3EF2D4DE3EE6F2F13EDAE8803ECEB7A73EC262893EB5EB4F3EA9542B3E9C9F523E8FCEFE3E82E56F3E6BC9D13E519F653E3750323E1CE0D63E0255F33DCF68663D9A00813D48FB2F3CBBA432BB567736BCF13D4BBD63C0AFBDA75D61BDDCBD0DBE08FAFFBE237F70BE3DE731BE582DA0BE724E22BE862210BE930587BE9FCF32BEAC7CD4BEB90C32BEC57B18BED1C759BEDDEECCBEE9EF4EBEF5C6C5BF00B98FBF067925BF0C2124BF11B08CBF172665BF1C81B9BF21C197BF26E515BF2BEB4ABF30D355BF359C5ABF3A4582BF3ECDFCBF4334FCBF4779BCBF4B9B7CBF4F9983BF53731FBF5727A0BF5AB662BF5E1EC5BF61602FBF647A0EBF676BD8BF6A3507BF6CD51EBF6F4BA8BF719836BF73BA61BF75B1C9BF777E16BF791EF6BF7A9421BF7BDD55BF7CFA59BF7DEAFABF7EAF0EBF7F4674BF7FB10FBF7FEECE)blob")};
constexpr std::array<signed char, 36> fft_factor_slab{
    5, 96, 3, 32, 4, 8, 2, 4, 4, 1, 0,
    5, 48, 3, 16, 4, 4, 4, 1, 0,
    5, 24, 3, 8, 2, 4, 4, 1, 0,
    5, 12, 3, 4, 4, 1, 0};

template <std::size_t N, std::size_t FactorOffset>
[[nodiscard]] consteval auto make_fft_bitrev_table() {
  std::array<opus_int16, N> table{};
  for (std::size_t input = 0; input < N; ++input) {
    auto index = static_cast<int>(input);
    auto stride = static_cast<int>(N);
    auto reversed = 0;
    for (std::size_t factor_index = FactorOffset; fft_factor_slab[factor_index] != 0; factor_index += 2) {
      const auto radix = fft_factor_slab[factor_index];
      stride /= radix;
      reversed += (index % radix) * stride;
      index /= radix;
    }
    table[input] = static_cast<opus_int16>(reversed);
  }
  return table;
}

constexpr auto fft_bitrev_480 = make_fft_bitrev_table<480, 0>();
constexpr auto fft_bitrev_240 = make_fft_bitrev_table<240, 11>();
constexpr auto fft_bitrev_120 = make_fft_bitrev_table<120, 20>();
constexpr auto fft_bitrev_60 = make_fft_bitrev_table<60, 29>();
constexpr kiss_fft_state fft_state48000_960_0{480, 0.0020833334f, -1, fft_twiddle_prefix.data(), fft_bitrev_480.data()};
constexpr kiss_fft_state fft_state48000_960_1{240, 0.0041666669f, 1, fft_twiddle_prefix.data(), fft_bitrev_240.data()};
constexpr kiss_fft_state fft_state48000_960_2{120, 0.0083333338f, 2, fft_twiddle_prefix.data(), fft_bitrev_120.data()};
constexpr kiss_fft_state fft_state48000_960_3{60, 0.016666668f, 3, fft_twiddle_prefix.data(), fft_bitrev_60.data()};
constexpr CeltModeInternal mode48000_960_120 = {48000, 120, 21, 21, {0.85000610f, 0.0000000f, 1.0000000f, 1.0000000f}, eband5ms.data(), 3, 120, 11, band_allocation.data(), logN400.data(), window120.data(),
                                              make_mdct_lookup(1920, {&fft_state48000_960_0, &fft_state48000_960_1, &fft_state48000_960_2, &fft_state48000_960_3}, celt_mdct_fft_slab.mdct.data()),
                                              {cache_index50.data(), cache_bits50.data(), cache_caps50.data()}};
[[nodiscard]] static constexpr auto default_custom_mode() noexcept -> const CeltModeInternal * { return &mode48000_960_120;
}
static void find_best_pitch(opus_val32 *xcorr, opus_val16 *y, int len, int max_pitch, int *best_pitch) {
  int i, j;
  opus_val32 Syy = 1; std::array<opus_val16, 2> best_num{};
  std::array<opus_val32, 2> best_den{}; best_num[0] = -1;
  best_num[1] = -1; best_den[0] = 0; best_den[1] = 0; best_pitch[0] = 0; best_pitch[1] = 1;
  for (j = 0; j < len; j++) Syy = ((Syy) + ((((opus_val32)(y[j]) * (opus_val32)(y[j])))));
  for (i = 0; i < max_pitch; i++) {
    if (xcorr[i] > 0) {
      opus_val16 num; opus_val32 xcorr16 = ((xcorr[i]));
      xcorr16 *= 1e-12f; num = ((xcorr16) * (xcorr16));
      if (((num) * (best_den[1])) > ((best_num[1]) * (Syy))) {
        if (((num) * (best_den[0])) > ((best_num[0]) * (Syy))) {
          best_num[1] = best_num[0]; best_den[1] = best_den[0]; best_pitch[1] = best_pitch[0]; best_num[0] = num;
          best_den[0] = Syy; best_pitch[0] = i;
        } else { best_num[1] = num; best_den[1] = Syy; best_pitch[1] = i; } }
}
    Syy += (((opus_val32)(y[i + len]) * (opus_val32)(y[i + len]))) - (((opus_val32)(y[i]) * (opus_val32)(y[i]))); Syy = std::max(1.f, Syy); }
}
static void celt_fir5(opus_val16 *x, const opus_val16 *num, int N) {
  int i;
  opus_val16 num0, num1, num2, num3, num4; opus_val32 mem0, mem1, mem2, mem3, mem4;
  num0 = num[0]; num1 = num[1]; num2 = num[2]; num3 = num[3]; num4 = num[4]; mem0 = 0; mem1 = 0; mem2 = 0; mem3 = 0; mem4 = 0;
  for (i = 0; i < N; i++) {
    opus_val32 sum = ((x[i])); sum = ((sum) + (opus_val32)(num0) * (opus_val32)(mem0));
    sum = ((sum) + (opus_val32)(num1) * (opus_val32)(mem1)); sum = ((sum) + (opus_val32)(num2) * (opus_val32)(mem2));
    sum = ((sum) + (opus_val32)(num3) * (opus_val32)(mem3)); sum = ((sum) + (opus_val32)(num4) * (opus_val32)(mem4));
    mem4 = mem3; mem3 = mem2; mem2 = mem1; mem1 = mem0; mem0 = x[i]; x[i] = (sum); }
}
static void pitch_downsample(std::span<celt_sig *const> x, opus_val16 *x_lp, int len, int factor) {
  int i;
  std::array<opus_val32, 5> ac{};
  opus_val16 tmp = 1.0f;
  std::array<opus_val16, 4> lpc{};
  std::array<opus_val16, 5> lpc2{};
  opus_val16 c1 = (.8f);
  int offset = factor / 2;
  for (i = 1; i < len; i++) x_lp[i] = .25f * x[0][(factor * i - offset)] + .25f * x[0][(factor * i + offset)] + .5f * x[0][factor * i];
  x_lp[0] = .25f * x[0][offset] + .5f * x[0][0];
  if (x.size() == 2) {
    for (i = 1; i < len; i++) x_lp[i] += .25f * x[1][(factor * i - offset)] + .25f * x[1][(factor * i + offset)] + .5f * x[1][factor * i];
    x_lp[0] += .25f * x[1][offset] + .5f * x[1][0];
}
  _celt_autocorr(x_lp, ac.data(), nullptr, 0, 4, len); ac[0] *= 1.0001f;
  for (i = 1; i <= 4; i++) { ac[i] -= ac[i] * (.008f * i) * (.008f * i);
}
  _celt_lpc(lpc.data(), ac.data(), 4);
  for (i = 0; i < 4; i++) { tmp = (((.9f)) * (tmp)); lpc[i] = ((lpc[i]) * (tmp));
}
  lpc2[0] = lpc[0] + (.8f); lpc2[1] = lpc[1] + ((c1) * (lpc[0]));
  lpc2[2] = lpc[2] + ((c1) * (lpc[1])); lpc2[3] = lpc[3] + ((c1) * (lpc[2])); lpc2[4] = ((c1) * (lpc[3])); celt_fir5(x_lp, lpc2.data(), len);
}
static void celt_pitch_xcorr_c(std::span<const opus_val16> x, std::span<const opus_val16> y, std::span<opus_val32> xcorr) {
  int i;
  const auto len = static_cast<int>(x.size());
  const auto max_pitch = static_cast<int>(xcorr.size());
  opus_assume(max_pitch > 0); opus_assume(y.size() >= x.size() + xcorr.size() - 1);
  opus_assume((reinterpret_cast<std::size_t>(x.data()) & 3) == 0);
  for (i = 0; i < max_pitch - 3; i += 4) {
    std::array<opus_val32, 4> sum{0, 0, 0, 0}; xcorr_kernel_c(x.data(), y.data() + i, sum.data(), len);
    xcorr[static_cast<std::size_t>(i)] = sum[0]; xcorr[static_cast<std::size_t>(i + 1)] = sum[1];
    xcorr[static_cast<std::size_t>(i + 2)] = sum[2]; xcorr[static_cast<std::size_t>(i + 3)] = sum[3];
}
  for (; i < max_pitch; i++) { opus_val32 sum = celt_inner_prod_c(x.data(), y.data() + i, len); xcorr[static_cast<std::size_t>(i)] = sum; }
}
static void pitch_search(const opus_val16 *x_lp, opus_val16 *y, int len, int max_pitch, int *pitch) {
  int i, j;
  int lag;
  int best_pitch[2] = {0, 0};
  int offset;
  opus_assume(len > 0); opus_assume(max_pitch > 0);
  lag = len + max_pitch;
  auto *x_lp4 = OPUS_SCRATCH(opus_val16, len >> 2);
  auto *y_lp4 = OPUS_SCRATCH(opus_val16, lag >> 2);
  auto *xcorr = OPUS_SCRATCH(opus_val32, max_pitch >> 1);
  for (j = 0; j < len >> 2; j++) x_lp4[j] = x_lp[2 * j];
  for (j = 0; j < lag >> 2; j++) y_lp4[j] = y[2 * j];
  celt_pitch_xcorr_c(std::span<const opus_val16>{x_lp4, static_cast<std::size_t>(len >> 2)}, std::span<const opus_val16>{y_lp4, static_cast<std::size_t>((len >> 2) + (max_pitch >> 2) - 1)}, std::span<opus_val32>{xcorr, static_cast<std::size_t>(max_pitch >> 2)});
  find_best_pitch(xcorr, y_lp4, len >> 2, max_pitch >> 2, best_pitch);
  for (i = 0; i < max_pitch >> 1; i++) {
    opus_val32 sum; xcorr[i] = 0;
    if (abs(i - 2 * best_pitch[0]) > 2 && abs(i - 2 * best_pitch[1]) > 2) continue;
    sum = celt_inner_prod_c(x_lp, y + i, len >> 1); xcorr[i] = std::max(-1.f, sum);
}
  find_best_pitch(xcorr, y, len >> 1, max_pitch >> 1, best_pitch);
  if (best_pitch[0] > 0 && best_pitch[0] < (max_pitch >> 1) - 1) {
    opus_val32 a, b, c; a = xcorr[best_pitch[0] - 1];
    b = xcorr[best_pitch[0]]; c = xcorr[best_pitch[0] + 1];
    if ((c - a) > (((.7f)) * (b - a))) offset = 1;
    else if ((a - c) > (((.7f)) * (b - c))) offset = -1; else offset = 0;
  } else { offset = 0;
}
  *pitch = 2 * best_pitch[0] - offset;
}
static opus_val16 compute_pitch_gain(opus_val32 xy, opus_val32 xx, opus_val32 yy) { return xy / ((float)sqrt(1 + xx * yy));
}
constexpr std::array<int, 16> second_check = numeric_blob_array<int, 16>(R"blob(00000000000000000000000300000002000000030000000200000005000000020000000300000002000000030000000200000005000000020000000300000002)blob");
static opus_val16 remove_doubling(opus_val16 *x, int maxperiod, int minperiod, int N, int *T0_, int prev_period, opus_val16 prev_gain) {
  int k, i, T, T0;
  opus_val16 g, g0; opus_val16 pg;
  opus_val32 xy, xx, yy, xy2; opus_val32 xcorr[3];
  opus_val32 best_xy, best_yy;
  int offset, minperiod0;
  minperiod0 = minperiod; maxperiod /= 2; minperiod /= 2; *T0_ /= 2; prev_period /= 2; N /= 2; x += maxperiod;
  if (*T0_ >= maxperiod) *T0_ = maxperiod - 1;
  T = T0 = *T0_;
  auto *yy_lookup = OPUS_SCRATCH(opus_val32, maxperiod + 1);
  dual_inner_prod_c(x, x, x - T0, N, &xx, &xy); yy_lookup[0] = xx;
  yy = xx;
  for (i = 1; i <= maxperiod; i++) {
    yy = yy + ((opus_val32)(x[-i]) * (opus_val32)(x[-i])) - ((opus_val32)(x[N - i]) * (opus_val32)(x[N - i]));
    yy_lookup[i] = std::max(0.f, yy);
}
  yy = yy_lookup[T0]; best_xy = xy; best_yy = yy; g = g0 = compute_pitch_gain(xy, xx, yy);
  for (k = 2; k <= 15; k++) {
    int T1, T1b;
    opus_val16 g1, cont = 0, thresh; T1 = celt_udiv(2 * T0 + k, 2 * k);
    if (T1 < minperiod) break;
    if (k == 2) {
      if (T1 + T0 > maxperiod) T1b = T0;
      else T1b = T0 + T1;
    } else { T1b = celt_udiv(2 * second_check[k] * T0 + k, 2 * k);
}
    dual_inner_prod_c(x, &x[-T1], &x[-T1b], N, &xy, &xy2); xy = (.5f * (xy + xy2));
    yy = (.5f * (yy_lookup[T1] + yy_lookup[T1b])); g1 = compute_pitch_gain(xy, xx, yy);
    if (abs(T1 - prev_period) <= 1) cont = prev_gain;
    else if (abs(T1 - prev_period) <= 2 && 5 * k * k < T0) cont = (.5f * (prev_gain)); else cont = 0;
    thresh = (((.3f)) > ((((.7f)) * (g0)) - cont) ? ((.3f)) : ((((.7f)) * (g0)) - cont));
    if (T1 < 3 * minperiod) thresh = (((.4f)) > ((((.85f)) * (g0)) - cont) ? ((.4f)) : ((((.85f)) * (g0)) - cont));
    else if (T1 < 2 * minperiod) thresh = (((.5f)) > ((((.9f)) * (g0)) - cont) ? ((.5f)) : ((((.9f)) * (g0)) - cont));
    if (g1 > thresh) { best_xy = xy; best_yy = yy; T = T1; g = g1; }
}
  best_xy = std::max(0.f, best_xy);
  if (best_yy <= best_xy) pg = 1.0f;
  else pg = (((float)(best_xy) / (best_yy + 1)));
  for (k = 0; k < 3; k++) xcorr[k] = celt_inner_prod_c(x, x - (T + k - 1), N);
  if ((xcorr[2] - xcorr[0]) > (((.7f)) * (xcorr[1] - xcorr[0]))) offset = 1;
  else if ((xcorr[0] - xcorr[2]) > (((.7f)) * (xcorr[1] - xcorr[2]))) offset = -1; else offset = 0;
  if (pg > g) pg = g;
  *T0_ = 2 * T + offset;
  if (*T0_ < minperiod0) *T0_ = minperiod0;
  return pg;
}
static void _celt_lpc(opus_val16 *_lpc, const opus_val32 *ac, int p) {
  int i, j;
  opus_val32 r, error = ac[0];
  float *lpc = _lpc;
  zero_n_items(lpc, static_cast<std::size_t>(p));
  if (ac[0] > 1e-10f) {
    for (i = 0; i < p; i++) {
      opus_val32 rr = 0;
      for (j = 0; j < i; j++) rr += ((lpc[j]) * (ac[i - j]));
      rr += (ac[i + 1]); r = -((float)((rr)) / (error)); lpc[i] = (r);
      for (j = 0; j < (i + 1) >> 1; j++) {
        opus_val32 tmp1, tmp2; tmp1 = lpc[j];
        tmp2 = lpc[i - 1 - j]; lpc[j] = tmp1 + ((r) * (tmp2)); lpc[i - 1 - j] = tmp2 + ((r) * (tmp1));
}
      error = error - ((((r) * (r))) * (error));
      if (error <= .001f * ac[0]) break; } }
}
static void celt_fir_c(const opus_val16 *x, const opus_val16 *num, opus_val16 *y, int N, int ord) {
  int i, j;
  opus_assume(x != y);
  auto *rnum = OPUS_SCRATCH(opus_val16, ord);
  std::reverse_copy(num, num + ord, rnum);
  for (i = 0; i < N - 3; i += 4) {
    opus_val32 sum[4]; sum[0] = ((x[i]));
    sum[1] = ((x[i + 1])); sum[2] = ((x[i + 2])); sum[3] = ((x[i + 3])); xcorr_kernel_c(rnum, x + i - ord, sum, ord);
    y[i] = (sum[0]); y[i + 1] = (sum[1]); y[i + 2] = (sum[2]); y[i + 3] = (sum[3]);
}
  for (; i < N; i++) {
    opus_val32 sum = ((x[i]));
    for (j = 0; j < ord; j++) sum = ((sum) + (opus_val32)(rnum[j]) * (opus_val32)(x[i + j - ord]));
    y[i] = (sum); }
}
static void celt_iir(const opus_val32 *_x, const opus_val16 *den, opus_val32 *_y, int N, int ord, opus_val16 *mem) {
  int i, j;
  opus_assume((ord & 3) == 0);
  auto *rden = OPUS_SCRATCH(opus_val16, ord);
  auto *y = OPUS_SCRATCH(opus_val16, N + ord);
  std::reverse_copy(den, den + ord, rden);
  for (int index = 0; index < ord; ++index) y[index] = -mem[ord - 1 - index];
  zero_n_items(y + ord, static_cast<std::size_t>(N));
  for (i = 0; i < N - 3; i += 4) {
    opus_val32 sum[4]; sum[0] = _x[i];
    sum[1] = _x[i + 1]; sum[2] = _x[i + 2]; sum[3] = _x[i + 3]; xcorr_kernel_c(rden, y + i, sum, ord);
    y[i + ord] = -(sum[0]); _y[i] = sum[0];
    sum[1] = ((sum[1]) + (opus_val32)(y[i + ord]) * (opus_val32)(den[0])); y[i + ord + 1] = -(sum[1]);
    _y[i + 1] = sum[1]; sum[2] = ((sum[2]) + (opus_val32)(y[i + ord + 1]) * (opus_val32)(den[0]));
    sum[2] = ((sum[2]) + (opus_val32)(y[i + ord]) * (opus_val32)(den[1])); y[i + ord + 2] = -(sum[2]);
    _y[i + 2] = sum[2]; sum[3] = ((sum[3]) + (opus_val32)(y[i + ord + 2]) * (opus_val32)(den[0]));
    sum[3] = ((sum[3]) + (opus_val32)(y[i + ord + 1]) * (opus_val32)(den[1]));
    sum[3] = ((sum[3]) + (opus_val32)(y[i + ord]) * (opus_val32)(den[2])); y[i + ord + 3] = -(sum[3]); _y[i + 3] = sum[3];
}
  for (; i < N; i++) {
    opus_val32 sum = _x[i];
    for (j = 0; j < ord; j++) sum -= ((opus_val32)(rden[j]) * (opus_val32)(y[i + j]));
    y[i + ord] = (sum); _y[i] = sum;
}
  for (i = 0; i < ord; i++) mem[i] = _y[N - i - 1];
}
static int _celt_autocorr(const opus_val16 *x, opus_val32 *ac, const celt_coef *window, int overlap, int lag, int n) {
  opus_val32 d;
  int i, k;
  int fastN = n - lag, shift;
  const opus_val16 *xptr;
  opus_val16 *xx = nullptr;
  opus_assume(n > 0); opus_assume(overlap >= 0);
  if (overlap == 0) {
    xptr = x;
  } else {
    xx = OPUS_SCRATCH(opus_val16, n);
    copy_n_items(x, static_cast<std::size_t>(n), xx);
    for (i = 0; i < overlap; i++) { opus_val16 w = window[i]; xx[i] = x[i]*w; xx[n-i-1] = x[n-i-1]*w;
}
    xptr = xx;
}
  shift = 0;
  celt_pitch_xcorr_c(std::span<const opus_val16>{xptr, static_cast<std::size_t>(fastN)}, std::span<const opus_val16>{xptr, static_cast<std::size_t>(fastN + lag)}, std::span<opus_val32>{ac, static_cast<std::size_t>(lag + 1)});
  for (k = 0; k <= lag; k++) {
    for (i = k + fastN, d = 0; i < n; i++) d = ((d) + (opus_val32)(xptr[i]) * (opus_val32)(xptr[i - k]));
    ac[k] += d;
}
  return shift;
}
namespace {
constinit const std::array<opus_val16, 25> eMeans = numeric_blob_array<opus_val16, 25>(R"blob(40CE000040C8000040B8000040AA000040A20000409A000040900000408C0000409C00004096000040920000408E0000409C000040940000408A000040900000408C00004094000040980000408E00004070000040700000407000004070000040700000)blob");
}
constexpr std::array<opus_val16, 4> pred_coef = numeric_blob_array<opus_val16, 4>(R"blob(3F6600003F4C00003F2600003F000000)blob");
constexpr std::array<opus_val16, 4> beta_coef = numeric_blob_array<opus_val16, 4>(R"blob(3F6B86003F2E14003EBD70003E4CD000)blob");
constexpr opus_val16 beta_intra = 4915 / 32768.;
constexpr std::array<std::array<std::array<unsigned char, 42>, 2>, 4> e_prob_model = numeric_blob_tensor<unsigned char, 4, 2, 42>(R"blob(487F41814280418040803E80408040805C4E5C4F5C4E5A4F742973287228841A841A9111A10CB00AB10B18B3308A3687368435863885378437843D7246604A584B58574A59425B43643B6C3278287A25612B4E32534E5451584B564A57475A495D4A5D4A6D287224752275228F1191129213A20CA50AB207BD06BE08B10917B236733F66426245634A59475B495B4E5956505C425D40663B673C683C75347B2C8A23851F61264D2D3D5A5D3C692A6B296E2D7426712670267C1A841B88138C149B0E9F109E12AA0DB10ABB08C006AF099F0A15B23B6E47564B5554535B42584957485C4B6248693A6B367334723770388133842896218C1D62234D2A2A7960426C2B6F28752C7B20782477217F2186228B15931798149E199A1AA615AD10B80DB80A960D8B0F16B23F724A5254535C52673E6048604365496B48713776347D3476347537873189279D20911D61214D28)blob");
[[nodiscard]] constexpr auto floor_to_int_reference(opus_val32 value) noexcept -> int { return static_cast<int>(std::floor(static_cast<double>(value)));
}
static opus_val32 loss_distortion(const celt_glog *eBands, celt_glog *oldEBands, int start, int end, int len, int C) {
  int c, i;
  opus_val32 dist = 0;
  for (c = 0; c < C; ++c) {
    for (i = start; i < end; i++) {
      celt_glog d = (((eBands[i + c * len]) - (oldEBands[i + c * len])));
      dist = ((dist) + (opus_val32)(d) * (opus_val32)(d)); }
}
  return ((200) < ((dist)) ? (200) : ((dist)));
}
static int quant_coarse_energy_impl(const CeltModeInternal *m, int start, int end, const celt_glog *eBands, celt_glog *oldEBands, opus_int32 budget, opus_int32 tell, const unsigned char *prob_model, celt_glog *error, ec_enc *enc, int C, int LM, int intra, celt_glog max_decay) {
  int i, c;
  int badness = 0;
  opus_val32 prev[2] = {0, 0}; opus_val16 coef, beta;
  if (tell + 3 <= budget) ec_enc_bit_logp(enc, intra, 3);
  if (intra) {
    coef = 0; beta = beta_intra;
  } else { beta = beta_coef[LM]; coef = pred_coef[LM];
}
  for (i = start; i < end; i++) {
    for (c = 0; c < C; ++c) {
      int bits_left;
      int qi, qi0;
      opus_val32 q; celt_glog x;
      opus_val32 f, tmp; celt_glog oldE, decay_bound;
      x = eBands[i + c * m->nbEBands]; oldE = ((-(9.f)) > (oldEBands[i + c * m->nbEBands]) ? (-(9.f)) : (oldEBands[i + c * m->nbEBands]));
      f = x - coef * oldE - prev[c]; qi = floor_to_int_reference(.5f + f);
      decay_bound = ((-(28.f)) > (oldEBands[i + c * m->nbEBands]) ? (-(28.f)) : (oldEBands[i + c * m->nbEBands])) - max_decay;
      if (qi < 0 && x < decay_bound) {
        qi += (int)(((decay_bound) - (x)));
        if (qi > 0) qi = 0;
}
      qi0 = qi; tell = ec_tell(enc); bits_left = budget - tell - 3 * C * (end - i);
      if (i != start && bits_left < 30) {
        if (bits_left < 24) qi = std::min(1, qi);
        if (bits_left < 16) qi = std::max(-1, qi);
}
      if (budget - tell >= 15) {
        int pi = 2 * std::min(i, 20);
        ec_laplace_encode(enc, &qi, prob_model[pi] << 7, prob_model[pi + 1] << 6);
      } else if (budget - tell >= 2) {
        qi = std::max(-1, std::min(qi, 1)); ec_enc_icdf(enc, 2 * qi ^ -(qi < 0), shared_three_step_icdf.data(), 2);
      } else if (budget - tell >= 1) {
        qi = std::min(0, qi); ec_enc_bit_logp(enc, -qi, 1);
      } else qi = -1; error[i + c * m->nbEBands] = f - (qi); badness += abs(qi0 - qi); q = (opus_val32)((qi));
      tmp = ((coef) * (oldE)) + prev[c] + q; oldEBands[i + c * m->nbEBands] = tmp; prev[c] = prev[c] + q - ((beta) * (q)); }
}
  return badness;
}
static void quant_coarse_energy(const CeltModeInternal *m, int start, int end, int effEnd, const celt_glog *eBands, celt_glog *oldEBands, opus_uint32 budget, celt_glog *error, ec_enc *enc, int C, int LM, int nbAvailableBytes, int force_intra, opus_val32 *delayedIntra) {
  int intra;
  celt_glog max_decay;
  opus_uint32 tell;
  opus_val32 new_distortion;
  intra = force_intra || (*delayedIntra > 2 * C * (end - start) && nbAvailableBytes > (end - start) * C);
  new_distortion = loss_distortion(eBands, oldEBands, start, effEnd, m->nbEBands, C); tell = ec_tell(enc);
  if (tell + 3 > budget) intra = 0;
  max_decay = (16.f);
  if (end - start > 10) { max_decay = std::min(max_decay, .125f * nbAvailableBytes);
}
  quant_coarse_energy_impl(m, start, end, eBands, oldEBands, budget, tell, e_prob_model[LM][intra].data(), error, enc, C, LM, intra, max_decay);
  if (intra) *delayedIntra = new_distortion;
  else *delayedIntra = ((((((pred_coef[LM]) * (pred_coef[LM]))) * (*delayedIntra))) + (new_distortion));
}
template <bool Encode, typename Coder>
void process_fine_energy(const CeltModeInternal *m, int start, int end, std::span<celt_glog> oldEBands, std::span<celt_glog> error, std::span<const int> prev_quant, std::span<const int> extra_quant, Coder *coder, int C) {
  for (int i = start; i < end; ++i) {
    const auto extra_bits = extra_quant[static_cast<std::size_t>(i)];
    if (extra_bits <= 0 || ec_tell(coder) + C * extra_bits > static_cast<opus_int32>(coder->storage * 8)) { continue;
}
    const auto prev = prev_quant.empty() ? 0 : prev_quant[static_cast<std::size_t>(i)];
    const auto bins = 1 << extra_bits;
    for (int c = 0; c < C; ++c) {
      int q2 = 0;
      if constexpr (Encode) {
        q2 = floor_to_int_reference((error[i + c * m->nbEBands] * (1 << prev) + .5f) * bins);
        q2 = clamp_value(q2, 0, bins - 1); ec_enc_bits(coder, q2, extra_bits);
      } else { q2 = ec_dec_bits(coder, extra_bits);
}
      auto offset = (q2 + .5f) * (1 << (14 - extra_bits)) * (1.f / 16384) - .5f;
      offset *= (1 << (14 - prev)) * (1.f / 16384); oldEBands[static_cast<std::size_t>(i + c * m->nbEBands)] += offset;
      if constexpr (Encode) { error[static_cast<std::size_t>(i + c * m->nbEBands)] -= offset; } } }
}
static void quant_fine_energy(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, celt_glog *error, int *prev_quant, int *extra_quant, ec_enc *enc, int C) {
  const auto band_count = static_cast<std::size_t>(C * m->nbEBands);
  process_fine_energy<true>(m, start, end, std::span{oldEBands, band_count}, optional_span(error, band_count), optional_span(prev_quant, static_cast<std::size_t>(m->nbEBands)), std::span<const int>{extra_quant, static_cast<std::size_t>(m->nbEBands)}, enc, C);
}
template <bool Encode, typename Coder>
void process_energy_finalise(const CeltModeInternal *m, int start, int end, std::span<celt_glog> oldEBands, std::span<celt_glog> error, std::span<const int> fine_quant, std::span<const int> fine_priority, int bits_left, Coder *coder, int C) {
  for (int prio = 0; prio < 2; ++prio) {
    for (int i = start; i < end && bits_left >= C; ++i) {
      if (fine_quant[static_cast<std::size_t>(i)] >= 8 || fine_priority[static_cast<std::size_t>(i)] != prio) { continue;
}
      for (int c = 0; c < C; ++c) {
        const auto q2 = [&]() {
          if constexpr (Encode) {
            const auto encoded = error[static_cast<std::size_t>(i + c * m->nbEBands)] < 0 ? 0 : 1;
            ec_enc_bits(coder, encoded, 1);
            return encoded;
          } else { return static_cast<int>(ec_dec_bits(coder, 1));
}
        }();
        const auto offset = (q2 - .5f) * (1 << (14 - fine_quant[static_cast<std::size_t>(i)] - 1)) * (1.f / 16384);
        if (!oldEBands.empty()) { oldEBands[static_cast<std::size_t>(i + c * m->nbEBands)] += offset;
}
        if constexpr (Encode) { error[static_cast<std::size_t>(i + c * m->nbEBands)] -= offset;
}
        bits_left--; } } }
}
static void quant_energy_finalise(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, celt_glog *error, int *fine_quant, int *fine_priority, int bits_left, ec_enc *enc, int C) {
  const auto band_count = static_cast<std::size_t>(C * m->nbEBands);
  process_energy_finalise<true>(m, start, end, optional_span(oldEBands, band_count), optional_span(error, band_count), std::span<const int>{fine_quant, static_cast<std::size_t>(m->nbEBands)}, std::span<const int>{fine_priority, static_cast<std::size_t>(m->nbEBands)}, bits_left, enc, C);
}
static void unquant_coarse_energy(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, int intra, ec_dec *dec, int C, int LM) {
  const unsigned char *prob_model = e_prob_model[LM][intra].data();
  int i, c;
  opus_val64 prev[2] = {0, 0}; opus_val16 coef, beta;
  opus_int32 budget, tell;
  if (intra) {
    coef = 0; beta = beta_intra;
  } else { beta = beta_coef[LM]; coef = pred_coef[LM];
}
  budget = dec->storage * 8;
  for (i = start; i < end; i++) {
    for (c = 0; c < C; ++c) {
      int qi;
      opus_val32 q, tmp; opus_assume(c < 2);
      tell = ec_tell(dec);
      if (budget - tell >= 15) {
        int pi = 2 * std::min(i, 20);
        qi = ec_laplace_decode(dec, prob_model[pi] << 7, prob_model[pi + 1] << 6);
      } else if (budget - tell >= 2) {
        qi = ec_dec_icdf(dec, shared_three_step_icdf.data(), 2); qi = (qi >> 1) ^ -(qi & 1);
      } else if (budget - tell >= 1) {
        qi = -ec_dec_bit_logp(dec, 1);
      } else qi = -1; q = (opus_val32)((qi));
      oldEBands[i + c * m->nbEBands] = ((-(9.f)) > (oldEBands[i + c * m->nbEBands]) ? (-(9.f)) : (oldEBands[i + c * m->nbEBands]));
      tmp = ((coef) * (oldEBands[i + c * m->nbEBands])) + prev[c] + q; oldEBands[i + c * m->nbEBands] = tmp;
      prev[c] = prev[c] + q - ((beta) * (q)); } }
}
static void unquant_fine_energy(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, int *prev_quant, int *extra_quant, ec_dec *dec, int C) {
  const auto band_count = static_cast<std::size_t>(C * m->nbEBands);
  process_fine_energy<false>(m, start, end, std::span{oldEBands, band_count}, {}, optional_span(prev_quant, static_cast<std::size_t>(m->nbEBands)), std::span<const int>{extra_quant, static_cast<std::size_t>(m->nbEBands)}, dec, C);
}
static void unquant_energy_finalise(const CeltModeInternal *m, int start, int end, celt_glog *oldEBands, int *fine_quant, int *fine_priority, int bits_left, ec_dec *dec, int C) {
  const auto band_count = static_cast<std::size_t>(C * m->nbEBands);
  process_energy_finalise<false>(m, start, end, optional_span(oldEBands, band_count), {}, std::span<const int>{fine_quant, static_cast<std::size_t>(m->nbEBands)}, std::span<const int>{fine_priority, static_cast<std::size_t>(m->nbEBands)}, bits_left, dec, C);
}
static void amp2Log2(const CeltModeInternal *m, int effEnd, int end, celt_ener *bandE, celt_glog *bandLogE, int C) {
  int c, i;
  for (c = 0; c < C; ++c) {
    for (i = 0; i < effEnd; i++) { bandLogE[i + c * m->nbEBands] = static_cast<float>(1.442695040888963387 * ::log(static_cast<double>(bandE[i + c * m->nbEBands]))) - ((celt_glog)eMeans[i]);
}
    for (i = effEnd; i < end; i++) bandLogE[c * m->nbEBands + i] = -(14.f); }
}
constexpr std::array<unsigned char, 24> LOG2_FRAC_TABLE = numeric_blob_array<unsigned char, 24>(R"blob(00080D10131517181A1B1C1D1E1F20202122222324242525)blob");
static int interp_bits2pulses(const CeltModeInternal *m, int start, int end, int skip_start, const int *bits1, const int *bits2, const int *thresh, const int *cap, opus_int32 total, opus_int32 *_balance, int skip_rsv, int *intensity, int intensity_rsv, int *dual_stereo, int dual_stereo_rsv, int *bits, int *ebits, int *fine_priority, int C, int LM, ec_ctx *ec, int encode, int prev, int signalBandwidth) {
  opus_int32 psum;
  int lo, hi, i, j, logM, stereo;
  int codedBands = -1, alloc_floor;
  opus_int32 left, percoeff;
  int done;
  opus_int32 balance; alloc_floor = C << 3;
  stereo = C > 1; logM = LM << 3; lo = 0; hi = 1 << 6;
  for (i = 0; i < 6; i++) {
    int mid = (lo + hi) >> 1;
    psum = 0; done = 0;
    for (j = end; j-- > start;) {
      int tmp = bits1[j] + (mid * (opus_int32)bits2[j] >> 6);
      if (tmp >= thresh[j] || done) {
        done = 1; psum += std::min(tmp, cap[j]);
      } else { if (tmp >= alloc_floor) psum += alloc_floor; }
}
    if (psum > total) hi = mid;
    else lo = mid;
}
  psum = 0; done = 0;
  for (j = end; j-- > start;) {
    int tmp = bits1[j] + ((opus_int32)lo * bits2[j] >> 6);
    if (tmp < thresh[j] && !done) {
      if (tmp >= alloc_floor) tmp = alloc_floor;
      else tmp = 0;
    } else done = 1; tmp = std::min(tmp, cap[j]); bits[j] = tmp; psum += tmp;
}
  for (codedBands = end;; codedBands--) {
    int band_width, band_bits, rem;
    j = codedBands - 1;
    if (j <= skip_start) {
      total += skip_rsv;
      break;
}
    left = total - psum; percoeff = celt_udiv(left, m->eBands[codedBands] - m->eBands[start]);
    left -= (m->eBands[codedBands] - m->eBands[start]) * percoeff;
    rem = ((left - (m->eBands[j] - m->eBands[start])) > (0) ? (left - (m->eBands[j] - m->eBands[start])) : (0));
    band_width = m->eBands[codedBands] - m->eBands[j]; band_bits = (int)(bits[j] + percoeff * band_width + rem);
    if (band_bits >= ((thresh[j]) > (alloc_floor + (1 << 3)) ? (thresh[j]) : (alloc_floor + (1 << 3)))) {
      if (encode) {
        int depth_threshold;
        if (codedBands > 17) depth_threshold = j < prev ? 7 : 9;
        else depth_threshold = 0;
        if (codedBands <= start + 2 || (band_bits > (depth_threshold * band_width << LM << 3) >> 4 && j <= signalBandwidth)) {
          ec_enc_bit_logp(ec, 1, 1);
          break;
}
        ec_enc_bit_logp(ec, 0, 1);
      } else if (ec_dec_bit_logp(ec, 1)) { break;
}
      psum += 1 << 3; band_bits -= 1 << 3;
}
    psum -= bits[j] + intensity_rsv;
    if (intensity_rsv > 0) intensity_rsv = LOG2_FRAC_TABLE[j - start];
    psum += intensity_rsv;
    if (band_bits >= alloc_floor) {
      psum += alloc_floor; bits[j] = alloc_floor;
    } else { bits[j] = 0; }
}
  opus_assume(codedBands > start);
  if (intensity_rsv > 0) {
    if (encode) {
      *intensity = std::min(*intensity, codedBands); ec_enc_uint(ec, *intensity - start, codedBands + 1 - start);
    } else *intensity = start + ec_dec_uint(ec, codedBands + 1 - start);
  } else *intensity = 0;
  if (*intensity <= start) { total += dual_stereo_rsv; dual_stereo_rsv = 0;
}
  if (dual_stereo_rsv > 0) {
    if (encode) ec_enc_bit_logp(ec, *dual_stereo, 1);
    else *dual_stereo = ec_dec_bit_logp(ec, 1);
  } else *dual_stereo = 0; left = total - psum; percoeff = celt_udiv(left, m->eBands[codedBands] - m->eBands[start]);
  left -= (m->eBands[codedBands] - m->eBands[start]) * percoeff;
  for (j = start; j < codedBands; j++) bits[j] += ((int)percoeff * (m->eBands[j + 1] - m->eBands[j]));
  for (j = start; j < codedBands; j++) {
    int tmp = (int)std::min(left, m->eBands[j + 1] - m->eBands[j]);
    bits[j] += tmp; left -= tmp;
}
  balance = 0;
  for (j = start; j < codedBands; j++) {
    int N0, N, den, offset, NClogN;
    opus_int32 excess, bit; opus_assume(bits[j] >= 0);
    N0 = m->eBands[j + 1] - m->eBands[j]; N = N0 << LM; bit = (opus_int32)bits[j] + balance;
    if (N > 1) {
      excess = std::max(bit - cap[j], 0); bits[j] = bit - excess;
      den = (C * N + ((C == 2 && N > 2 && !*dual_stereo && j < *intensity) ? 1 : 0));
      NClogN = den * (m->logN[j] + logM); offset = (NClogN >> 1) - den * 21;
      if (N == 2) offset += den << 3 >> 2;
      if (bits[j] + offset < den * 2 << 3) offset += NClogN >> 2;
      else if (bits[j] + offset < den * 3 << 3) offset += NClogN >> 3;
      ebits[j] = ((0) > ((bits[j] + offset + (den << (3 - 1)))) ? (0) : ((bits[j] + offset + (den << (3 - 1)))));
      ebits[j] = celt_udiv(ebits[j], den) >> 3;
      if (C * ebits[j] > (bits[j] >> 3)) ebits[j] = bits[j] >> stereo >> 3;
      ebits[j] = std::min(ebits[j], 8); fine_priority[j] = ebits[j] * (den << 3) >= bits[j] + offset; bits[j] -= C * ebits[j] << 3;
    } else { excess = ((0) > (bit - (C << 3)) ? (0) : (bit - (C << 3))); bits[j] = bit - excess; ebits[j] = 0; fine_priority[j] = 1;
}
    if (excess > 0) {
      int extra_fine, extra_bits;
      extra_fine = ((excess >> (stereo + 3)) < (8 - ebits[j]) ? (excess >> (stereo + 3)) : (8 - ebits[j]));
      ebits[j] += extra_fine; extra_bits = extra_fine * C << 3; fine_priority[j] = extra_bits >= excess - balance; excess -= extra_bits;
}
    balance = excess; opus_assume(bits[j] >= 0);
    opus_assume(ebits[j] >= 0);
}
  *_balance = balance;
  for (; j < end; j++) { ebits[j] = bits[j] >> stereo >> 3; opus_assume(C * ebits[j] << 3 == bits[j]); bits[j] = 0; fine_priority[j] = ebits[j] < 1;
}
  return codedBands;
}
static int clt_compute_allocation(const CeltModeInternal *m, int start, int end, const int *offsets, const int *cap, int alloc_trim, int *intensity, int *dual_stereo, opus_int32 total, opus_int32 *balance, int *pulses, int *ebits, int *fine_priority, int C, int LM, ec_ctx *ec, int encode, int prev, int signalBandwidth) {
  int lo, hi, len, j, codedBands, skip_start, skip_rsv, intensity_rsv, dual_stereo_rsv;
  total = std::max(total, 0); len = m->nbEBands; skip_start = start; skip_rsv = total >= 1 << 3 ? 1 << 3 : 0;
  total -= skip_rsv; intensity_rsv = dual_stereo_rsv = 0;
  if (C == 2) {
    intensity_rsv = LOG2_FRAC_TABLE[end - start];
    if (intensity_rsv > total) intensity_rsv = 0;
    else { total -= intensity_rsv; dual_stereo_rsv = total >= 1 << 3 ? 1 << 3 : 0; total -= dual_stereo_rsv; }
}
  auto allocation_storage = std::span<int>{OPUS_SCRATCH(int, 4 * static_cast<std::size_t>(len)), 4 * static_cast<std::size_t>(len)};
  zero_n_items(allocation_storage.data(), allocation_storage.size());
  auto [bits1, bits2, thresh, trim_offset] = partition_workset<4>(allocation_storage, static_cast<std::size_t>(len));
  for (j = start; j < end; j++) { thresh[j] = (((C) << 3) > ((3 * (m->eBands[j + 1] - m->eBands[j]) << LM << 3) >> 4) ? ((C) << 3) : ((3 * (m->eBands[j + 1] - m->eBands[j]) << LM << 3) >> 4)); trim_offset[j] = C * (m->eBands[j + 1] - m->eBands[j]) * (alloc_trim - 5 - LM) * (end - j - 1) * (1 << (LM + 3)) >> 6; if ((m->eBands[j + 1] - m->eBands[j]) << LM == 1) trim_offset[j] -= C << 3;
}
  lo = 1; hi = m->nbAllocVectors - 1;
  for (; lo <= hi;) {
    int done = 0, psum = 0, mid = (lo + hi) >> 1;
    for (j = end; j-- > start;) {
      int bitsj, N = m->eBands[j + 1] - m->eBands[j];
      bitsj = C * N * m->allocVectors[mid * len + j] << LM >> 2;
      if (bitsj > 0) bitsj = std::max(0, bitsj + trim_offset[j]);
      bitsj += offsets[j];
      if (bitsj >= thresh[j] || done) {
        done = 1; psum += std::min(bitsj, cap[j]);
      } else { if (bitsj >= C << 3) psum += C << 3; }
}
    if (psum > total) hi = mid - 1;
    else lo = mid + 1;
}
  hi = lo--;
  for (j = start; j < end; j++) { int bits1j, bits2j; int N = m->eBands[j + 1] - m->eBands[j]; bits1j = C * N * m->allocVectors[lo * len + j] << LM >> 2; bits2j = hi >= m->nbAllocVectors ? cap[j] : C * N * m->allocVectors[hi * len + j] << LM >> 2; if (bits1j > 0) bits1j = std::max(0, bits1j + trim_offset[j]); if (bits2j > 0) bits2j = std::max(0, bits2j + trim_offset[j]); if (lo > 0) bits1j += offsets[j]; bits2j += offsets[j]; if (offsets[j] > 0) skip_start = j; bits2j = std::max(0, bits2j - bits1j); bits1[j] = bits1j; bits2[j] = bits2j;
}
  codedBands = interp_bits2pulses(m, start, end, skip_start, bits1.data(), bits2.data(), thresh.data(), cap, total, balance, skip_rsv, intensity, intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits, fine_priority, C, LM, ec, encode, prev, signalBandwidth);
  return codedBands;
}
static void exp_rotation1(celt_norm *X, int len, int stride, opus_val16 c, opus_val16 s) {
  int i;
  opus_val16 ms; celt_norm *Xptr;
  Xptr = X; ms = (-(s));
  for (i = 0; i < len - stride; i++) { celt_norm x1, x2; x1 = Xptr[0]; x2 = Xptr[stride]; Xptr[stride] = ((((((opus_val32)(c) * (opus_val32)(x2))) + (opus_val32)(s) * (opus_val32)(x1)))); *Xptr++ = ((((((opus_val32)(c) * (opus_val32)(x1))) + (opus_val32)(ms) * (opus_val32)(x2))));
}
  Xptr = &X[len - 2 * stride - 1];
  for (i = len - 2 * stride - 1; i >= 0; i--) { celt_norm x1, x2; x1 = Xptr[0]; x2 = Xptr[stride]; Xptr[stride] = ((((((opus_val32)(c) * (opus_val32)(x2))) + (opus_val32)(s) * (opus_val32)(x1)))); *Xptr-- = ((((((opus_val32)(c) * (opus_val32)(x1))) + (opus_val32)(ms) * (opus_val32)(x2)))); }
}
static void exp_rotation(celt_norm *X, int len, int dir, int stride, int K, int spread) {
  constexpr std::array<int, 3> SPREAD_FACTOR{15, 10, 5};
  int i;
  opus_val16 c, s; opus_val16 gain, theta;
  int stride2 = 0, factor;
  if (2 * K >= len || spread == (0)) return;
  factor = SPREAD_FACTOR[spread - 1];
  gain = (((opus_val32)((opus_val32)(((opus_val16)1.f)) * (opus_val32)(len))) / ((opus_val32)(len + factor * K)));
  theta = (.5f * (((gain) * (gain)))); c = ((float)cos((.5f * 3.1415926535897931) * ((theta))));
  s = ((float)cos((.5f * 3.1415926535897931) * ((((1.0f) - (theta))))));
  if (len >= 8 * stride) {
    stride2 = 1;
    for (; (stride2 * stride2 + stride2) * stride + (stride >> 2) < len; ++stride2) {}
}
  len = celt_udiv(len, stride);
  for (i = 0; i < stride; i++) {
    if (dir < 0) {
      if (stride2) exp_rotation1(X + i * len, len, stride2, s, c);
      exp_rotation1(X + i * len, len, 1, c, s);
    } else {
      exp_rotation1(X + i * len, len, 1, c, -s);
      if (stride2) exp_rotation1(X + i * len, len, stride2, s, -c); } }
}
static unsigned normalise_residual_and_extract_collapse_mask(int *iy, celt_norm *X, int N, int B, opus_val32 Ryy, opus_val32 gain) {
  const opus_val32 g = (1.f / (float)sqrt(Ryy)) * gain;
  if (B <= 1) {
    for (int index = 0; index < N; ++index) X[index] = static_cast<opus_val32>(iy[index]) * g;
    return 1;
  }
  const int N0 = celt_udiv(N, B);
  auto collapse_mask = unsigned{};
  for (int band = 0; band < B; ++band) {
    auto tmp = unsigned{};
    const int base = band * N0;
    for (int offset = 0; offset < N0; ++offset) {
      const auto value = iy[base + offset];
      tmp |= static_cast<unsigned>(value);
      X[base + offset] = static_cast<opus_val32>(value) * g;
    }
    collapse_mask |= (tmp != 0) << band;
  }
  for (int index = B * N0; index < N; ++index) X[index] = static_cast<opus_val32>(iy[index]) * g;
  return collapse_mask;
}
static unsigned extract_collapse_mask(int *iy, int N, int B) {
  unsigned collapse_mask;
  int N0, i;
  if (B <= 1) return 1;
  N0 = celt_udiv(N, B); collapse_mask = 0;
  for (i = 0; i < B; ++i) {
    unsigned tmp = 0;
    for (int row_index = 0; row_index < N0; ++row_index) tmp |= iy[i * N0 + row_index];
    collapse_mask |= (tmp != 0) << i;
}
  return collapse_mask;
}
static opus_val16 op_pvq_search_c(celt_norm *X, int *iy, int K, int N) {
  int i, j;
  int pulsesLeft;
  opus_val32 sum, xy; opus_val16 yy;
  auto *y = OPUS_SCRATCH(celt_norm, N);
  auto *signx = OPUS_SCRATCH(int, N); sum = 0;
  for (j = 0; j < N; ++j) { signx[j] = X[j] < 0; X[j] = ((float)fabs(X[j])); iy[j] = 0; y[j] = 0;
}
  if (K == 1) {
    int best_id = 0;
    for (int candidate = 1; candidate < N; ++candidate) if (X[candidate] > X[best_id]) best_id = candidate;
    iy[best_id] = 1;
    for (int idx = 0; idx < N; ++idx) iy[idx] = (iy[idx] ^ -signx[idx]) + signx[idx];
    return 1;
}
  xy = yy = 0; pulsesLeft = K;
  if (K > (N >> 1)) {
    opus_val16 rcp;
    for (int idx = 0; idx < N; ++idx) sum += X[idx];
    if (!(sum > 1e-15f && sum < 64)) {
      X[0] = (1.f);
      for (int idx = 1; idx < N; ++idx) X[idx] = 0;
      sum = (1.f);
}
    rcp = (((K + 0.8f) * ((1.f / (sum)))));
    for (j = 0; j < N; ++j) {
      iy[j] = (int)floor(rcp * X[j]); y[j] = (celt_norm)iy[j];
      yy = ((yy) + (opus_val32)(y[j]) * (opus_val32)(y[j])); xy = ((xy) + (opus_val32)(X[j]) * (opus_val32)(y[j]));
      y[j] *= 2; pulsesLeft -= iy[j]; }
}
  opus_assume(pulsesLeft >= 0);
  if (pulsesLeft > N + 3) {
    opus_val16 tmp = (opus_val16)pulsesLeft; yy = ((yy) + (opus_val32)(tmp) * (opus_val32)(tmp));
    yy = ((yy) + (opus_val32)(tmp) * (opus_val32)(y[0])); iy[0] += pulsesLeft; pulsesLeft = 0;
}
  for (i = 0; i < pulsesLeft; i++) {
    opus_val16 Rxy, Ryy;
    int best_id;
    opus_val32 best_num; opus_val16 best_den;
    best_id = 0; yy = ((yy) + (1)); Rxy = ((((xy) + ((X[0]))))); Ryy = ((yy) + (y[0]));
    Rxy = ((Rxy) * (Rxy)); best_den = Ryy; best_num = Rxy;
    for (int candidate = 1; candidate < N; ++candidate) {
      Rxy = ((((xy) + ((X[candidate]))))); Ryy = ((yy) + (y[candidate])); Rxy = ((Rxy) * (Rxy));
      if ((__builtin_expect(!!(((opus_val32)(best_den) * (opus_val32)(Rxy)) > ((opus_val32)(Ryy) * (opus_val32)(best_num))), 0))) { best_den = Ryy; best_num = Rxy; best_id = candidate; }
}
    xy = ((xy) + ((X[best_id]))); yy = ((yy) + (y[best_id])); y[best_id] += 2; iy[best_id]++;
}
  for (int idx = 0; idx < N; ++idx) iy[idx] = (iy[idx] ^ -signx[idx]) + signx[idx];
  return yy;
}
static unsigned alg_quant(celt_norm *X, int N, int K, int spread, int B, ec_enc *enc, opus_val32 gain, int resynth) {
  opus_val32 yy; unsigned collapse_mask;
  opus_assume(K > 0); opus_assume(N > 1);
  auto *iy = OPUS_SCRATCH(int, static_cast<std::size_t>(N + 3));
  zero_n_items(iy, static_cast<std::size_t>(N + 3));
  exp_rotation(X, N, 1, B, K, spread);
  {
    yy = (op_pvq_search_c(X, iy, K, N)); collapse_mask = extract_collapse_mask(iy, N, B); encode_pulses(iy, N, K, enc);
    if (resynth) static_cast<void>(normalise_residual_and_extract_collapse_mask(iy, X, N, 1, yy, gain));
}
  if (resynth) exp_rotation(X, N, -1, B, K, spread);
  return collapse_mask;
}
static unsigned alg_unquant(celt_norm *X, int N, int K, int spread, int B, ec_dec *dec, opus_val32 gain) {
  opus_val32 Ryy; unsigned collapse_mask;
  opus_assume(K > 0); opus_assume(N > 1);
  auto *iy = OPUS_SCRATCH(int, N);
  Ryy = decode_pulses(iy, N, K, dec); collapse_mask = normalise_residual_and_extract_collapse_mask(iy, X, N, B, Ryy, gain);
  exp_rotation(X, N, -1, B, K, spread);
  return collapse_mask;
}
static void renormalise_vector(celt_norm *X, int N, opus_val32 gain) {
  int i;
  opus_val32 E; opus_val16 g;
  opus_val32 t; celt_norm *xptr;
  E = 1e-15f + celt_inner_prod_c(X, X, N); t = (E); g = ((((1.f / ((float)sqrt(t))))) * (gain)); xptr = X;
  for (i = 0; i < N; i++) { *xptr = ((((opus_val32)(g) * (opus_val32)(*xptr)))); xptr++; }
}
static opus_int32 stereo_itheta(const celt_norm *X, const celt_norm *Y, int stereo, int N) {
  int i, itheta;
  opus_val32 mid, side; opus_val32 Emid, Eside;
  Emid = Eside = 0;
  if (stereo) {
    for (i = 0; i < N; i++) { celt_norm m, s; m = (((X[i]) + (Y[i]))); s = (((X[i]) - (Y[i]))); Emid = ((Emid) + (opus_val32)(m) * (opus_val32)(m)); Eside = ((Eside) + (opus_val32)(s) * (opus_val32)(s));
}
  } else { Emid += celt_inner_prod_c(X, X, N); Eside += celt_inner_prod_c(Y, Y, N);
}
  mid = ((float)sqrt(Emid)); side = ((float)sqrt(Eside)); itheta = (int)floor(.5f + 65536.f * 16384 * celt_atan2p_norm(side, mid));
  return itheta;
}
static auto silk_CNG_exc(opus_int32 exc_Q14[], opus_int32 exc_buf_Q14[], int length, opus_int32 *rand_seed) noexcept -> void {
  opus_int32 seed = *rand_seed;
  int exc_mask = 255;
  for (; exc_mask > length; exc_mask = ((exc_mask) >> (1))) {}
  for (int i = 0; i < length; ++i) {
    seed = silk_next_rand_seed(seed);
    const int idx = static_cast<int>(((seed) >> (24)) & exc_mask);
    opus_assume(idx >= 0); opus_assume(idx <= 255);
    exc_Q14[i] = exc_buf_Q14[idx];
}
  *rand_seed = seed;
}
constexpr auto silk_max_fs_kHz = 16, silk_max_frame_length = 20 * silk_max_fs_kHz, silk_max_subfr_length = 5 * silk_max_fs_kHz, silk_max_ltp_mem_length = 20 * silk_max_fs_kHz, silk_max_ltp_buffer_length = silk_max_ltp_mem_length + silk_max_frame_length, silk_max_shell_blocks = silk_max_frame_length / 16, silk_max_delayed_decision_states = 4;
static void silk_CNG_Reset(silk_decoder_state *psDec) {
  if (psDec->sCNG == nullptr) return;
  auto *psCNG = psDec->sCNG;
  int i, NLSF_step_Q15, NLSF_acc_Q15;
  NLSF_step_Q15 = ((opus_int32)((0x7FFF) / (psDec->LPC_order + 1))); NLSF_acc_Q15 = 0;
  for (i = 0; i < psDec->LPC_order; i++) { NLSF_acc_Q15 += NLSF_step_Q15; psCNG->CNG_smth_NLSF_Q15[i] = NLSF_acc_Q15;
}
  psCNG->CNG_smth_Gain_Q16 = 0; psCNG->rand_seed = 3176576;
}
void silk_CNG(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, opus_int16 frame[], int length) {
  int i, subfr;
  opus_int32 LPC_pred_Q10, max_Gain_Q16, gain_Q16, gain_Q10; opus_int16 A_Q12[16];
  const bool updates_cng_history = psDec->lossCnt == 0 && psDec->prevSignalType == 0;
  const bool generates_cng = psDec->lossCnt != 0;
  silk_CNG_struct *psCNG = (updates_cng_history || generates_cng) ? silk_ensure_cng(psDec) : psDec->sCNG;
  if (psCNG == nullptr) return;
  if (psDec->fs_kHz != psCNG->fs_kHz) { silk_CNG_Reset(psDec); psCNG->fs_kHz = psDec->fs_kHz;
}
  if (updates_cng_history) {
    for (i = 0; i < psDec->LPC_order; i++) { psCNG->CNG_smth_NLSF_Q15[i] += ((opus_int32)((((opus_int32)psDec->prevNLSF_Q15[i] - (opus_int32)psCNG->CNG_smth_NLSF_Q15[i]) * (opus_int64)((opus_int16)(16348))) >> 16));
}
    max_Gain_Q16 = 0; subfr = 0;
    for (i = 0; i < psDec->nb_subfr; i++) { if (psDecCtrl->Gains_Q16[i] > max_Gain_Q16) { max_Gain_Q16 = psDecCtrl->Gains_Q16[i]; subfr = i; }
}
    move_n_bytes(psCNG->CNG_exc_buf_Q14, static_cast<std::size_t>((psDec->nb_subfr - 1) * psDec->subfr_length * sizeof(opus_int32)), &psCNG->CNG_exc_buf_Q14[psDec->subfr_length]);
    copy_n_bytes(&psDec->exc_Q14[subfr * psDec->subfr_length], static_cast<std::size_t>(psDec->subfr_length * sizeof(opus_int32)), psCNG->CNG_exc_buf_Q14);
    for (i = 0; i < psDec->nb_subfr; i++) { psCNG->CNG_smth_Gain_Q16 += ((opus_int32)(((psDecCtrl->Gains_Q16[i]-psCNG->CNG_smth_Gain_Q16)*(opus_int64)((opus_int16)(4634)))>>16)); if (((opus_int32)(((opus_int64)(psCNG->CNG_smth_Gain_Q16)*(46396))>>16)) > psDecCtrl->Gains_Q16[i]) { psCNG->CNG_smth_Gain_Q16 = psDecCtrl->Gains_Q16[i]; } }
}
  if (generates_cng) {
    std::array<opus_int32, silk_max_frame_length + 16> CNG_sig_Q14_storage{}; auto *CNG_sig_Q14 = CNG_sig_Q14_storage.data();
    gain_Q16 = ((opus_int32)(((opus_int64)(psDec->sPLC.randScale_Q14) * (psDec->sPLC.prevGain_Q16[1])) >> 16));
    if (gain_Q16 >= (1 << 21) || psCNG->CNG_smth_Gain_Q16 > (1 << 23)) {
      gain_Q16 = (((gain_Q16) >> 16) * ((gain_Q16) >> 16));
      gain_Q16 = ((((((psCNG->CNG_smth_Gain_Q16) >> 16) * ((psCNG->CNG_smth_Gain_Q16) >> 16)))) - (((opus_int32)((opus_uint32)((gain_Q16)) << ((5))))));
      gain_Q16 = ((opus_int32)((opus_uint32)(silk_SQRT_APPROX(gain_Q16)) << (16)));
    } else {
      gain_Q16 = ((opus_int32)(((opus_int64)(gain_Q16) * (gain_Q16)) >> 16));
      gain_Q16 = (((((opus_int32)(((opus_int64)(psCNG->CNG_smth_Gain_Q16) * (psCNG->CNG_smth_Gain_Q16)) >> 16)))) - (((opus_int32)((opus_uint32)((gain_Q16)) << ((5))))));
      gain_Q16 = ((opus_int32)((opus_uint32)(silk_SQRT_APPROX(gain_Q16)) << (8)));
}
    gain_Q10 = ((gain_Q16) >> (6)); silk_CNG_exc(CNG_sig_Q14 + 16, psCNG->CNG_exc_buf_Q14, length, &psCNG->rand_seed);
    silk_NLSF2A(A_Q12, psCNG->CNG_smth_NLSF_Q15, psDec->LPC_order);
    copy_n_bytes(psCNG->CNG_synth_state, static_cast<std::size_t>(16 * sizeof(opus_int32)), CNG_sig_Q14);
    opus_assume(psDec->LPC_order == 10 || psDec->LPC_order == 16);
    const auto coefficients = std::span<const opus_int16>{A_Q12, static_cast<std::size_t>(psDec->LPC_order)};
    for (i = 0; i < length; ++i) {
      LPC_pred_Q10 = silk_lpc_prediction_q10(CNG_sig_Q14 + 16 + i, coefficients);
      CNG_sig_Q14[16 + i] = saturating_add_int32(CNG_sig_Q14[16 + i], saturating_left_shift<4>(LPC_pred_Q10));
      const auto cng_sample = scale_and_saturate_q14<8>(CNG_sig_Q14[16 + i], gain_Q10);
      frame[i] = saturate_int16_from_int32(static_cast<opus_int32>(frame[i]) + cng_sample);
    }
    copy_n_bytes(&CNG_sig_Q14[length], static_cast<std::size_t>(16 * sizeof(opus_int32)), psCNG->CNG_synth_state);
  } else { zero_n_bytes(psCNG->CNG_synth_state, static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int32))); }
}
template <bool Encode, typename Coder, typename PulseSpan>
void silk_process_signs(Coder *coder, PulseSpan pulses, const int signalType, const int quantOffsetType, std::span<const int> sum_pulses) {
  std::array<opus_uint8, 2> icdf{0, 0};
  const auto *icdf_ptr = silk_sign_iCDF.data() + 7 * (quantOffsetType + (signalType << 1));
  for (auto block_index = std::size_t{}; block_index < sum_pulses.size(); ++block_index) {
    const auto pulse_count = sum_pulses[block_index];
    if (pulse_count <= 0) { continue;
}
    icdf[0] = icdf_ptr[std::min(pulse_count & 0x1F, 6)];
    auto pulse_block = pulses.subspan(block_index * 16, 16);
    for (auto pulse_index = std::size_t{}; pulse_index < pulse_block.size(); ++pulse_index) {
      auto &pulse = pulse_block[pulse_index];
      if constexpr (Encode) {
        if (pulse != 0) { ec_enc_icdf(coder, (pulse >> 15) + 1, icdf.data(), 8);
}
      } else if (pulse > 0) { pulse *= static_cast<opus_int16>((ec_dec_icdf(coder, icdf.data(), 8) << 1) - 1); } } }
}
void silk_encode_signs(ec_enc *psRangeEnc, std::span<const opus_int8> pulses, const int signalType, const int quantOffsetType, std::span<const int> sum_pulses) { silk_process_signs<true>(psRangeEnc, pulses, signalType, quantOffsetType, sum_pulses);
}
void silk_decode_signs(ec_dec *psRangeDec, std::span<opus_int16> pulses, const int signalType, const int quantOffsetType, std::span<const int> sum_pulses) { silk_process_signs<false>(psRangeDec, pulses, signalType, quantOffsetType, sum_pulses);
}
int silk_reset_decoder(silk_decoder_state *psDec) {
  silk_release_cng(psDec);
  zero_n_bytes(&psDec->prev_gain_Q16, static_cast<std::size_t>(sizeof(silk_decoder_state) - ((char *)&psDec->prev_gain_Q16 - (char *)psDec)));
  psDec->first_frame_after_reset = 1; psDec->prev_gain_Q16 = 65536;
  silk_CNG_Reset(psDec); silk_PLC_Reset(psDec);
  return 0;
}
int silk_init_decoder(silk_decoder_state *psDec) {
  silk_release_cng(psDec);
  zero_n_bytes(psDec, static_cast<std::size_t>(sizeof(silk_decoder_state))); silk_reset_decoder(psDec);
  return (0);
}
void silk_decode_core(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, opus_int16 xq[], const opus_int16 pulses[((5 * 4) * 16)]) {
  int i, k, lag = 0, start_idx, sLTP_buf_idx, NLSF_interpolation_flag, signalType;
  opus_int16 *A_Q12, *B_Q14, *pxq, A_Q12_tmp[16];
  opus_int32 LTP_pred_Q13, Gain_Q10, inv_gain_Q31, gain_adj_Q16, rand_seed, offset_Q10;
  opus_int32 *pred_lag_ptr, *pexc_Q14, *pres_Q14; opus_assume(psDec->prev_gain_Q16 != 0);
  opus_assume(psDec->ltp_mem_length <= silk_max_ltp_mem_length); opus_assume(psDec->ltp_mem_length + psDec->frame_length <= silk_max_ltp_buffer_length); opus_assume(psDec->subfr_length <= silk_max_subfr_length);
  std::array<opus_int16, silk_max_ltp_mem_length> sLTP_storage; auto *sLTP = sLTP_storage.data(); std::array<opus_int32, silk_max_ltp_buffer_length> sLTP_Q15_storage; auto *sLTP_Q15 = sLTP_Q15_storage.data(); std::array<opus_int32, silk_max_subfr_length> res_Q14_storage; auto *res_Q14 = res_Q14_storage.data(); std::array<opus_int32, silk_max_subfr_length + 16> sLPC_Q14_storage; auto *sLPC_Q14 = sLPC_Q14_storage.data();
  offset_Q10 = silk_Quantization_Offsets_Q10[psDec->indices.signalType >> 1][psDec->indices.quantOffsetType];
  if (psDec->indices.NLSFInterpCoef_Q2 < 1 << 2) {
    NLSF_interpolation_flag = 1;
  } else { NLSF_interpolation_flag = 0;
}
  rand_seed = psDec->indices.Seed;
  for (i = 0; i < psDec->frame_length; i++) {
    rand_seed = silk_next_rand_seed(rand_seed);
    psDec->exc_Q14[i] = ((opus_int32)((opus_uint32)((opus_int32)pulses[i]) << (14)));
    if (psDec->exc_Q14[i] > 0) {
      psDec->exc_Q14[i] -= 80 << 4;
    } else if (psDec->exc_Q14[i] < 0) { psDec->exc_Q14[i] += 80 << 4;
}
    psDec->exc_Q14[i] += offset_Q10 << 4;
    if (rand_seed < 0) { psDec->exc_Q14[i] = -psDec->exc_Q14[i];
}
    rand_seed = ((opus_int32)((opus_uint32)(rand_seed) + (opus_uint32)(pulses[i])));
}
  copy_n_bytes(psDec->sLPC_Q14_buf, static_cast<std::size_t>(16 * sizeof(opus_int32)), sLPC_Q14);
  pexc_Q14 = psDec->exc_Q14; pxq = xq; sLTP_buf_idx = psDec->ltp_mem_length;
  for (k = 0; k < psDec->nb_subfr; k++) {
    pres_Q14 = res_Q14; A_Q12 = psDecCtrl->PredCoef_Q12[k >> 1];
    copy_n_bytes(A_Q12, static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int16)), A_Q12_tmp);
    B_Q14 = &psDecCtrl->LTPCoef_Q14[k * 5]; signalType = psDec->indices.signalType;
    const auto b_q14_coefficients = std::span<const opus_int16, 5>{B_Q14, 5};
    Gain_Q10 = ((psDecCtrl->Gains_Q16[k]) >> (6)); inv_gain_Q31 = silk_INVERSE32_varQ(psDecCtrl->Gains_Q16[k], 47);
    if (psDecCtrl->Gains_Q16[k] != psDec->prev_gain_Q16) {
      gain_adj_Q16 = silk_DIV32_varQ(psDec->prev_gain_Q16, psDecCtrl->Gains_Q16[k], 16);
      for (int i = 0; i < 16; ++i) sLPC_Q14[i] = (opus_int32)(((opus_int64)gain_adj_Q16 * sLPC_Q14[i]) >> 16);
    } else { gain_adj_Q16 = (opus_int32)1 << 16;
}
    opus_assume(inv_gain_Q31 != 0); psDec->prev_gain_Q16 = psDecCtrl->Gains_Q16[k];
    if (psDec->lossCnt && psDec->prevSignalType == 2 && psDec->indices.signalType != 2 && k < 4 / 2) {
      zero_n_bytes(B_Q14, static_cast<std::size_t>(5 * sizeof(opus_int16)));
      B_Q14[5 / 2] = ((opus_int32)((0.25) * ((opus_int64)1 << (14)) + 0.5)); signalType = 2; psDecCtrl->pitchL[k] = psDec->lagPrev;
}
    if (signalType == 2) {
      lag = psDecCtrl->pitchL[k];
      if (k == 0 || (k == 2 && NLSF_interpolation_flag)) {
        start_idx = psDec->ltp_mem_length - lag - psDec->LPC_order - 5 / 2; opus_assume(start_idx > 0);
        if (k == 2) { copy_n_bytes(xq, static_cast<std::size_t>(2 * psDec->subfr_length * sizeof(opus_int16)), &psDec->outBuf[psDec->ltp_mem_length]);
}
        silk_LPC_analysis_filter(&sLTP[start_idx], &psDec->outBuf[start_idx + k * psDec->subfr_length], A_Q12, psDec->ltp_mem_length - start_idx, psDec->LPC_order);
        if (k == 0) { inv_gain_Q31 = ((opus_int32)((opus_uint32)(((opus_int32)(((inv_gain_Q31) * (opus_int64)((opus_int16)(psDecCtrl->LTP_scale_Q14))) >> 16))) << (2)));
}
        for (i = 0; i < lag + 5 / 2; i++) { sLTP_Q15[sLTP_buf_idx - i - 1] = ((opus_int32)(((inv_gain_Q31) * (opus_int64)((opus_int16)(sLTP[psDec->ltp_mem_length - i - 1]))) >> 16));
}
      } else {
        if (gain_adj_Q16 != (opus_int32)1 << 16) {
          for (i = 0; i < lag + 5 / 2; i++) { sLTP_Q15[sLTP_buf_idx - i - 1] = ((opus_int32)(((opus_int64)(gain_adj_Q16) * (sLTP_Q15[sLTP_buf_idx - i - 1])) >> 16)); } } }
}
    if (signalType == 2) {
      pred_lag_ptr = &sLTP_Q15[sLTP_buf_idx - lag + 5 / 2];
      for (i = 0; i < psDec->subfr_length; i++) {
        LTP_pred_Q13 = silk_ltp_prediction_5tap(pred_lag_ptr, b_q14_coefficients);
        ++pred_lag_ptr;
        pres_Q14[i] = (((pexc_Q14[i])) + (((opus_int32)((opus_uint32)((LTP_pred_Q13)) << ((1))))));
        sLTP_Q15[sLTP_buf_idx] = ((opus_int32)((opus_uint32)(pres_Q14[i]) << (1)));
        ++sLTP_buf_idx;
}
    } else { pres_Q14 = pexc_Q14;
}
    opus_assume(psDec->LPC_order == 10 || psDec->LPC_order == 16);
    if (psDec->LPC_order == 16) {
      silk_decode_lpc_subframe_q14<16>(sLPC_Q14, pres_Q14, pxq, psDec->subfr_length, A_Q12_tmp, Gain_Q10);
    } else {
      silk_decode_lpc_subframe_q14<10>(sLPC_Q14, pres_Q14, pxq, psDec->subfr_length, A_Q12_tmp, Gain_Q10);
    }
    copy_n_bytes(&sLPC_Q14[psDec->subfr_length], static_cast<std::size_t>(16 * sizeof(opus_int32)), sLPC_Q14);
    pexc_Q14 += psDec->subfr_length; pxq += psDec->subfr_length;
}
  copy_n_bytes(sLPC_Q14, static_cast<std::size_t>(16 * sizeof(opus_int32)), psDec->sLPC_Q14_buf);
}
int silk_decode_frame(silk_decoder_state *psDec, ec_dec *psRangeDec, opus_int16 pOut[], opus_int32 *pN, int lostFlag, int condCoding) {
  int L, mv_len, ret = 0;
  L = psDec->frame_length; silk_decoder_control psDecCtrl[1]; psDecCtrl->LTP_scale_Q14 = 0; opus_assume(L > 0 && L <= ((5 * 4) * 16));
  if (lostFlag == 0 || (lostFlag == 2 && psDec->LBRR_flags[psDec->nFramesDecoded] == 1)) {
    std::array<opus_int16, silk_max_frame_length> pulses_storage;
    auto pulses = std::span<opus_int16>{pulses_storage}.first(static_cast<std::size_t>((L + 16 - 1) & ~(16 - 1)));
    silk_decode_indices(psDec, psRangeDec, psDec->nFramesDecoded, lostFlag, condCoding);
    silk_decode_pulses(psRangeDec, pulses, psDec->indices.signalType, psDec->indices.quantOffsetType, psDec->frame_length);
    silk_decode_parameters(psDec, psDecCtrl, condCoding); silk_decode_core(psDec, psDecCtrl, pOut, pulses.data());
    opus_assume(psDec->ltp_mem_length >= psDec->frame_length); mv_len = psDec->ltp_mem_length - psDec->frame_length;
    move_n_bytes(&psDec->outBuf[psDec->frame_length], static_cast<std::size_t>(mv_len * sizeof(opus_int16)), psDec->outBuf);
    copy_n_bytes(pOut, static_cast<std::size_t>(psDec->frame_length * sizeof(opus_int16)), &psDec->outBuf[mv_len]);
    silk_PLC(psDec, psDecCtrl, std::span<opus_int16>{pOut, static_cast<std::size_t>(L)}, 0); psDec->lossCnt = 0;
    psDec->prevSignalType = psDec->indices.signalType;
    opus_assume(psDec->prevSignalType >= 0 && psDec->prevSignalType <= 2); psDec->first_frame_after_reset = 0;
  } else {
    silk_PLC(psDec, psDecCtrl, std::span<opus_int16>{pOut, static_cast<std::size_t>(L)}, 1);
    opus_assume(psDec->ltp_mem_length >= psDec->frame_length); mv_len = psDec->ltp_mem_length - psDec->frame_length;
    move_n_bytes(&psDec->outBuf[psDec->frame_length], static_cast<std::size_t>(mv_len * sizeof(opus_int16)), psDec->outBuf);
    copy_n_bytes(pOut, static_cast<std::size_t>(psDec->frame_length * sizeof(opus_int16)), &psDec->outBuf[mv_len]);
}
  silk_CNG(psDec, psDecCtrl, pOut, L); silk_PLC_glue_frames(psDec, std::span<opus_int16>{pOut, static_cast<std::size_t>(L)});
  psDec->lagPrev = psDecCtrl->pitchL[psDec->nb_subfr - 1]; *pN = L;
  return ret;
}
void silk_decode_parameters(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, int condCoding) {
  int i, k, Ix;
  opus_int16 pNLSF_Q15[16], pNLSF0_Q15[16];
  const opus_int8 *cbk_ptr_Q7;
  silk_gains_dequant(psDecCtrl->Gains_Q16, psDec->indices.GainsIndices, &psDec->LastGainIndex, condCoding == 2, psDec->nb_subfr);
  silk_NLSF_decode(pNLSF_Q15, psDec->indices.NLSFIndices, psDec->psNLSF_CB);
  silk_NLSF2A(psDecCtrl->PredCoef_Q12[1], pNLSF_Q15, psDec->LPC_order);
  if (psDec->first_frame_after_reset == 1) { psDec->indices.NLSFInterpCoef_Q2 = 4;
}
  if (psDec->indices.NLSFInterpCoef_Q2 < 4) {
    for (i = 0; i < psDec->LPC_order; i++) { pNLSF0_Q15[i] = psDec->prevNLSF_Q15[i] + ((((psDec->indices.NLSFInterpCoef_Q2) * (pNLSF_Q15[i] - psDec->prevNLSF_Q15[i]))) >> (2));
}
    silk_NLSF2A(psDecCtrl->PredCoef_Q12[0], pNLSF0_Q15, psDec->LPC_order);
  } else { copy_n_bytes(psDecCtrl->PredCoef_Q12[1], static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int16)), psDecCtrl->PredCoef_Q12[0]);
}
  copy_n_bytes(pNLSF_Q15, static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int16)), psDec->prevNLSF_Q15);
  if (psDec->lossCnt) {
  silk_bwexpander(std::span<opus_int16>{psDecCtrl->PredCoef_Q12[0], static_cast<std::size_t>(psDec->LPC_order)}, 63570);
  silk_bwexpander(std::span<opus_int16>{psDecCtrl->PredCoef_Q12[1], static_cast<std::size_t>(psDec->LPC_order)}, 63570);
}
  if (psDec->indices.signalType == 2) {
    silk_decode_pitch(psDec->indices.lagIndex, psDec->indices.contourIndex, psDecCtrl->pitchL, psDec->fs_kHz, psDec->nb_subfr);
    cbk_ptr_Q7 = silk_LTP_codebooks[psDec->indices.PERIndex].vq_q7.data();
    for (k = 0; k < psDec->nb_subfr; k++) {
      Ix = psDec->indices.LTPIndex[k];
      for (i = 0; i < 5; i++) { psDecCtrl->LTPCoef_Q14[k * 5 + i] = ((opus_int32)((opus_uint32)(cbk_ptr_Q7[Ix * 5 + i]) << (7))); }
}
    Ix = psDec->indices.LTP_scaleIndex; psDecCtrl->LTP_scale_Q14 = silk_LTPScales_table_Q14[Ix];
  } else {
    zero_n_bytes(psDecCtrl->pitchL, static_cast<std::size_t>(psDec->nb_subfr * sizeof(int)));
    zero_n_bytes(psDecCtrl->LTPCoef_Q14, static_cast<std::size_t>(5 * psDec->nb_subfr * sizeof(opus_int16)));
    psDec->indices.PERIndex = 0; psDecCtrl->LTP_scale_Q14 = 0; }
}
void silk_decode_indices(silk_decoder_state *psDec, ec_dec *psRangeDec, int FrameIndex, int decode_LBRR, int condCoding) {
  int i, k, Ix;
  int decode_absolute_lagIndex, delta_lagIndex;
  opus_int16 ec_ix[16]; opus_uint8 pred_Q8[16];
  if (decode_LBRR || psDec->VAD_flags[FrameIndex]) {
    Ix = ec_dec_icdf(psRangeDec, silk_type_offset_VAD_iCDF.data(), 8) + 2;
  } else { Ix = ec_dec_icdf(psRangeDec, silk_type_offset_no_VAD_iCDF.data(), 8);
}
  psDec->indices.signalType = (opus_int8)((Ix) >> (1)); psDec->indices.quantOffsetType = (opus_int8)(Ix & 1);
  if (condCoding == 2) {
    psDec->indices.GainsIndices[0] = (opus_int8)ec_dec_icdf(psRangeDec, silk_delta_gain_iCDF.data(), 8);
  } else {
    psDec->indices.GainsIndices[0] = (opus_int8)((opus_int32)((opus_uint32)(ec_dec_icdf(psRangeDec, silk_gain_iCDF[psDec->indices.signalType].data(), 8)) << (3)));
    psDec->indices.GainsIndices[0] += (opus_int8)ec_dec_icdf(psRangeDec, silk_uniform8_iCDF.data(), 8);
}
  for (i = 1; i < psDec->nb_subfr; i++) { psDec->indices.GainsIndices[i] = (opus_int8)ec_dec_icdf(psRangeDec, silk_delta_gain_iCDF.data(), 8);
}
  psDec->indices.NLSFIndices[0] = (opus_int8)ec_dec_icdf(psRangeDec, silk_nlsf_cb1_icdf(psDec->psNLSF_CB, psDec->indices.signalType), 8);
  silk_NLSF_unpack(ec_ix, pred_Q8, psDec->psNLSF_CB, psDec->indices.NLSFIndices[0]);
  opus_assume(psDec->psNLSF_CB->order == psDec->LPC_order);
  for (i = 0; i < psDec->psNLSF_CB->order; i++) {
    Ix = ec_dec_icdf(psRangeDec, &psDec->psNLSF_CB->ec_iCDF[ec_ix[i]], 8);
    if (Ix == 0) {
      Ix -= ec_dec_icdf(psRangeDec, silk_NLSF_EXT_iCDF.data(), 8);
    } else if (Ix == 2 * 4) { Ix += ec_dec_icdf(psRangeDec, silk_NLSF_EXT_iCDF.data(), 8);
}
    psDec->indices.NLSFIndices[i + 1] = (opus_int8)(Ix - 4);
}
  if (psDec->nb_subfr == 4) {
    psDec->indices.NLSFInterpCoef_Q2 = (opus_int8)ec_dec_icdf(psRangeDec, silk_NLSF_interpolation_factor_iCDF.data(), 8);
  } else { psDec->indices.NLSFInterpCoef_Q2 = 4;
}
  if (psDec->indices.signalType == 2) {
    decode_absolute_lagIndex = 1;
    if (condCoding == 2 && psDec->ec_prevSignalType == 2) {
      delta_lagIndex = (opus_int16)ec_dec_icdf(psRangeDec, silk_pitch_delta_iCDF.data(), 8);
      if (delta_lagIndex > 0) {
        delta_lagIndex = delta_lagIndex - 9;
        psDec->indices.lagIndex = (opus_int16)(psDec->ec_prevLagIndex + delta_lagIndex); decode_absolute_lagIndex = 0; }
}
    if (decode_absolute_lagIndex) {
      psDec->indices.lagIndex = (opus_int16)ec_dec_icdf(psRangeDec, silk_pitch_lag_iCDF.data(), 8) * ((psDec->fs_kHz) >> (1));
      psDec->indices.lagIndex += (opus_int16)ec_dec_icdf(psRangeDec, psDec->pitch_lag_low_bits_iCDF, 8);
}
    psDec->ec_prevLagIndex = psDec->indices.lagIndex;
    psDec->indices.contourIndex = (opus_int8)ec_dec_icdf(psRangeDec, psDec->pitch_contour_iCDF, 8);
    psDec->indices.PERIndex = (opus_int8)ec_dec_icdf(psRangeDec, silk_LTP_per_index_iCDF.data(), 8);
    for (k = 0; k < psDec->nb_subfr; k++) { psDec->indices.LTPIndex[k] = (opus_int8)ec_dec_icdf(psRangeDec, silk_LTP_codebooks[psDec->indices.PERIndex].gain_icdf.data(), 8);
}
    if (condCoding == 0) {
      psDec->indices.LTP_scaleIndex = (opus_int8)ec_dec_icdf(psRangeDec, silk_LTPscale_iCDF.data(), 8);
    } else { psDec->indices.LTP_scaleIndex = 0; }
}
  psDec->ec_prevSignalType = psDec->indices.signalType;
  psDec->indices.Seed = (opus_int8)ec_dec_icdf(psRangeDec, silk_uniform4_iCDF.data(), 8);
}
void silk_decode_pulses(ec_dec *psRangeDec, std::span<opus_int16> pulses, const int signalType, const int quantOffsetType, const int frame_length) {
  int i, j, k, iter, abs_q, nLS, RateLevelIndex;
  int sum_pulses[(((5 * 4) * 16) / 16)], nLshifts[(((5 * 4) * 16) / 16)];
  opus_int16 *pulses_ptr;
  const opus_uint8 *cdf_ptr;
  RateLevelIndex = ec_dec_icdf(psRangeDec, silk_rate_levels_iCDF[signalType >> 1].data(), 8);
  opus_assume(1 << 4 == 16); iter = ((frame_length) >> (4));
  if (iter * 16 < frame_length) { opus_assume(frame_length == 12 * 10); iter++;
}
  cdf_ptr = silk_pulses_per_block_iCDF[RateLevelIndex].data();
  for (i = 0; i < iter; i++) {
    nLshifts[i] = 0; sum_pulses[i] = ec_dec_icdf(psRangeDec, cdf_ptr, 8);
    for (; sum_pulses[i] == 16 + 1;) { nLshifts[i]++; sum_pulses[i] = ec_dec_icdf(psRangeDec, silk_pulses_per_block_iCDF[10 - 1].data() + (nLshifts[i] == 10), 8); }
}
  for (i = 0; i < iter; i++) {
    if (sum_pulses[i] > 0) {
      silk_shell_decoder(std::span<opus_int16, 16>{pulses.data() + ((opus_int32)((opus_int16)(i)) * (opus_int32)((opus_int16)(16))), 16}, psRangeDec, sum_pulses[i]);
    } else { zero_n_bytes(&pulses[static_cast<std::size_t>(((opus_int32)((opus_int16)(i)) * (opus_int32)((opus_int16)(16))))], static_cast<std::size_t>(16 * sizeof(opus_int16))); }
}
  for (i = 0; i < iter; i++) {
    if (nLshifts[i] > 0) {
      nLS = nLshifts[i]; pulses_ptr = pulses.data() + ((opus_int32)((opus_int16)(i)) * (opus_int32)((opus_int16)(16)));
      for (k = 0; k < 16; k++) {
        abs_q = pulses_ptr[k];
        for (j = 0; j < nLS; j++) { abs_q = ((opus_int32)((opus_uint32)(abs_q) << (1))); abs_q += ec_dec_icdf(psRangeDec, silk_lsb_iCDF.data(), 8);
}
        pulses_ptr[k] = abs_q;
}
      sum_pulses[i] |= nLS << 5; }
}
  silk_decode_signs(psRangeDec, std::span<opus_int16>{pulses.data(), static_cast<std::size_t>(iter * 16)}, signalType, quantOffsetType, std::span<const int>{sum_pulses, static_cast<std::size_t>(iter)});
}
int silk_decoder_set_fs(silk_decoder_state *psDec, int fs_kHz, opus_int32 fs_API_Hz) {
  int frame_length, ret = 0;
  opus_assume(fs_kHz == 8 || fs_kHz == 12 || fs_kHz == 16);
  opus_assume(psDec->nb_subfr == 4 || psDec->nb_subfr == 4 / 2);
  psDec->subfr_length = ((opus_int32)((opus_int16)(5)) * (opus_int32)((opus_int16)(fs_kHz)));
  frame_length = ((opus_int32)((opus_int16)(psDec->nb_subfr)) * (opus_int32)((opus_int16)(psDec->subfr_length)));
  if (psDec->fs_kHz != fs_kHz || psDec->fs_API_hz != fs_API_Hz) {
    ret += silk_resampler_init(&psDec->resampler_state, ((opus_int32)((opus_int16)(fs_kHz)) * (opus_int32)((opus_int16)(1000))), fs_API_Hz, 0);
    psDec->fs_API_hz = fs_API_Hz;
}
  if (psDec->fs_kHz != fs_kHz || frame_length != psDec->frame_length) {
    psDec->pitch_contour_iCDF = silk_pitch_contour_icdf(fs_kHz, psDec->nb_subfr).data();
    if (psDec->fs_kHz != fs_kHz) {
      psDec->ltp_mem_length = ((opus_int32)((opus_int16)(20)) * (opus_int32)((opus_int16)(fs_kHz)));
      const auto nlsf_choice = silk_nlsf_codebook_for_fs(fs_kHz);
      psDec->LPC_order = nlsf_choice.order; psDec->psNLSF_CB = nlsf_choice.codebook;
      psDec->pitch_lag_low_bits_iCDF = silk_pitch_lag_low_bits_icdf(fs_kHz).data(); psDec->first_frame_after_reset = 1;
      psDec->lagPrev = 100; psDec->LastGainIndex = 10;
      psDec->prevSignalType = 0; zero_n_bytes(psDec->outBuf, static_cast<std::size_t>(sizeof(psDec->outBuf)));
      zero_n_bytes(psDec->sLPC_Q14_buf, static_cast<std::size_t>(sizeof(psDec->sLPC_Q14_buf)));
}
    psDec->fs_kHz = fs_kHz; psDec->frame_length = frame_length;
}
  opus_assume(psDec->frame_length > 0 && psDec->frame_length <= ((5 * 4) * 16));
  return ret;
}
struct silk_decoder { silk_decoder_state channel_state; silk_decoder_state *side_channel_state; stereo_dec_state sStereo; int nChannelsAPI, nChannelsInternal, prev_decode_only_middle; };
struct silk_decoder_channel_view {
  silk_decoder_state *mid;
  silk_decoder_state *side;
  [[nodiscard]] auto operator[](int index) const noexcept -> silk_decoder_state & { return index == 0 ? *mid : *side; }
};
[[nodiscard]] static auto silk_ensure_side_channel(silk_decoder *decoder) noexcept -> silk_decoder_state * {
  if (decoder->side_channel_state == nullptr) {
    auto side = make_opus_owned<silk_decoder_state>(sizeof(silk_decoder_state));
    if (!side) return nullptr;
    zero_n_bytes(side.get(), sizeof(silk_decoder_state));
    if (silk_init_decoder(side.get()) != 0) {
      silk_release_cng(side.get());
      return nullptr;
    }
    decoder->side_channel_state = side.release();
  }
  return decoder->side_channel_state;
}
static void silk_release_side_channel(silk_decoder *decoder) noexcept {
  if (decoder != nullptr && decoder->side_channel_state != nullptr) {
    silk_release_cng(decoder->side_channel_state);
    opus_free(decoder->side_channel_state);
    decoder->side_channel_state = nullptr;
  }
}
static void silk_destroy_decoder(void *decState) noexcept {
  if (decState == nullptr) return;
  auto *decoder = static_cast<silk_decoder *>(decState);
  silk_release_cng(&decoder->channel_state);
  silk_release_side_channel(decoder);
}
[[nodiscard]] static auto silk_channels(silk_decoder *decoder) noexcept -> silk_decoder_channel_view {
  return {&decoder->channel_state, decoder->side_channel_state};
}
int silk_Get_Decoder_Size(int *decSizeBytes) { *decSizeBytes = sizeof(silk_decoder); return 0;
}
int silk_ResetDecoder(void *decState) {
  int ret = 0; auto *decoder = static_cast<silk_decoder *>(decState);
  ret = silk_reset_decoder(&decoder->channel_state);
  silk_release_side_channel(decoder);
  zero_n_bytes(&decoder->sStereo, static_cast<std::size_t>(sizeof(decoder->sStereo))); decoder->prev_decode_only_middle = 0; return ret;
}
int silk_InitDecoder(void *decState) {
  int ret = 0; auto *decoder = static_cast<silk_decoder *>(decState);
  silk_destroy_decoder(decoder);
  zero_n_bytes(decoder, sizeof(silk_decoder));
  ret = silk_init_decoder(&decoder->channel_state);
  zero_n_bytes(&decoder->sStereo, static_cast<std::size_t>(sizeof(decoder->sStereo))); decoder->prev_decode_only_middle = 0; return ret;
}
int silk_Decode(void *decState, silk_DecControlStruct *decControl, int lostFlag, int newPacketFlag, ec_dec *psRangeDec, opus_res *samplesOut, opus_int32 *nSamplesOut) {
  int i, n, decode_only_middle = 0, ret = 0;
  opus_int32 nSamplesOutDec = 0, LBRR_symbol; opus_int16 *samplesOut1_tmp[2];
  opus_int32 MS_pred_Q13[2] = {0}; opus_int16 *resample_out_ptr;
  silk_decoder *psDec = (silk_decoder *)decState;
  if ((decControl->nChannelsInternal == 2 || psDec->nChannelsInternal == 2 || decControl->nChannelsAPI == 2) && silk_ensure_side_channel(psDec) == nullptr) return -1;
  auto channel_state = silk_channels(psDec);
  int has_side, stereo_to_mono;
  opus_assume(decControl->nChannelsInternal == 1 || decControl->nChannelsInternal == 2);
  if (newPacketFlag) { for (n = 0; n < decControl->nChannelsInternal; n++) { channel_state[n].nFramesDecoded = 0; }
}
  if (decControl->nChannelsInternal > psDec->nChannelsInternal && decControl->nChannelsInternal == 2) { ret += silk_init_decoder(&channel_state[1]);
}
  stereo_to_mono = decControl->nChannelsInternal == 1 && psDec->nChannelsInternal == 2 && (decControl->internalSampleRate == 1000 * channel_state[0].fs_kHz);
  if (channel_state[0].nFramesDecoded == 0) {
    for (n = 0; n < decControl->nChannelsInternal; n++) {
      int fs_kHz_dec;
      if (decControl->payloadSize_ms == 0) {
        channel_state[n].nFramesPerPacket = 1; channel_state[n].nb_subfr = 2;
      } else if (decControl->payloadSize_ms == 10) {
        channel_state[n].nFramesPerPacket = 1; channel_state[n].nb_subfr = 2;
      } else if (decControl->payloadSize_ms == 20) {
        channel_state[n].nFramesPerPacket = 1; channel_state[n].nb_subfr = 4;
      } else if (decControl->payloadSize_ms == 40) {
        channel_state[n].nFramesPerPacket = 2; channel_state[n].nb_subfr = 4;
      } else if (decControl->payloadSize_ms == 60) {
        channel_state[n].nFramesPerPacket = 3; channel_state[n].nb_subfr = 4;
      } else { return -203;
}
      fs_kHz_dec = (decControl->internalSampleRate >> 10) + 1;
      if (fs_kHz_dec != 8 && fs_kHz_dec != 12 && fs_kHz_dec != 16) { return -200;
}
      ret += silk_decoder_set_fs(&channel_state[n], fs_kHz_dec, decControl->API_sampleRate); }
}
  if (decControl->nChannelsAPI == 2 && decControl->nChannelsInternal == 2 && (psDec->nChannelsAPI == 1 || psDec->nChannelsInternal == 1)) {
    zero_n_bytes(psDec->sStereo.pred_prev_Q13, static_cast<std::size_t>(sizeof(psDec->sStereo.pred_prev_Q13)));
    zero_n_bytes(psDec->sStereo.sSide, static_cast<std::size_t>(sizeof(psDec->sStereo.sSide)));
    copy_n_bytes(&channel_state[0].resampler_state, static_cast<std::size_t>(sizeof(silk_resampler_state_struct)), &channel_state[1].resampler_state);
}
  psDec->nChannelsAPI = decControl->nChannelsAPI; psDec->nChannelsInternal = decControl->nChannelsInternal;
  if (decControl->API_sampleRate > (opus_int32)48 * 1000 || decControl->API_sampleRate < 8000) {
    ret = -200;
    return (ret);
}
  if (lostFlag != 1 && channel_state[0].nFramesDecoded == 0) {
    for (n = 0; n < decControl->nChannelsInternal; n++) {
      for (i = 0; i < channel_state[n].nFramesPerPacket; i++) { channel_state[n].VAD_flags[i] = ec_dec_bit_logp(psRangeDec, 1);
}
      channel_state[n].LBRR_flag = ec_dec_bit_logp(psRangeDec, 1);
}
    for (n = 0; n < decControl->nChannelsInternal; n++) {
      zero_n_bytes(channel_state[n].LBRR_flags, static_cast<std::size_t>(sizeof(channel_state[n].LBRR_flags)));
      if (channel_state[n].LBRR_flag) {
        if (channel_state[n].nFramesPerPacket == 1) {
          channel_state[n].LBRR_flags[0] = 1;
        } else {
        LBRR_symbol = ec_dec_icdf(psRangeDec, silk_LBRR_flags_iCDFs[channel_state[n].nFramesPerPacket - 2].data(), 8) + 1;
          for (i = 0; i < channel_state[n].nFramesPerPacket; i++) { channel_state[n].LBRR_flags[i] = ((LBRR_symbol) >> (i)) & 1; } } }
}
    if (lostFlag == 0) {
      for (i = 0; i < channel_state[0].nFramesPerPacket; i++) {
        for (n = 0; n < decControl->nChannelsInternal; n++) {
          if (channel_state[n].LBRR_flags[i]) {
            opus_int16 pulses[((5 * 4) * 16)];
            int condCoding;
            if (decControl->nChannelsInternal == 2 && n == 0) {
              silk_stereo_decode_pred(psRangeDec, MS_pred_Q13);
              if (channel_state[1].LBRR_flags[i] == 0) { silk_stereo_decode_mid_only(psRangeDec, &decode_only_middle); }
}
            if (i > 0 && channel_state[n].LBRR_flags[i - 1]) {
              condCoding = 2;
            } else { condCoding = 0;
}
            silk_decode_indices(&channel_state[n], psRangeDec, i, 1, condCoding);
            silk_decode_pulses(psRangeDec, std::span<opus_int16>{pulses, static_cast<std::size_t>(((channel_state[n].frame_length + 16 - 1) & ~(16 - 1)))}, channel_state[n].indices.signalType, channel_state[n].indices.quantOffsetType, channel_state[n].frame_length); } } } }
}
  if (decControl->nChannelsInternal == 2) {
    if (lostFlag == 0 || (lostFlag == 2 && channel_state[0].LBRR_flags[channel_state[0].nFramesDecoded] == 1)) {
      silk_stereo_decode_pred(psRangeDec, MS_pred_Q13);
      if ((lostFlag == 0 && channel_state[1].VAD_flags[channel_state[0].nFramesDecoded] == 0) || (lostFlag == 2 && channel_state[1].LBRR_flags[channel_state[0].nFramesDecoded] == 0)) {
        silk_stereo_decode_mid_only(psRangeDec, &decode_only_middle);
      } else { decode_only_middle = 0;
}
    } else { for (n = 0; n < 2; n++) { MS_pred_Q13[n] = psDec->sStereo.pred_prev_Q13[n]; } }
}
  if (decControl->nChannelsInternal == 2 && decode_only_middle == 0 && psDec->prev_decode_only_middle == 1) {
    zero_n_bytes(channel_state[1].outBuf, static_cast<std::size_t>(sizeof(channel_state[1].outBuf)));
    zero_n_bytes(channel_state[1].sLPC_Q14_buf, static_cast<std::size_t>(sizeof(channel_state[1].sLPC_Q14_buf)));
    channel_state[1].lagPrev = 100; channel_state[1].LastGainIndex = 10;
    channel_state[1].prevSignalType = 0; channel_state[1].first_frame_after_reset = 1;
}
  auto *samplesOut1_tmp_storage1 = OPUS_SCRATCH(opus_int16, decControl->nChannelsInternal * (channel_state[0].frame_length + 2));
  samplesOut1_tmp[0] = samplesOut1_tmp_storage1; samplesOut1_tmp[1] = samplesOut1_tmp_storage1 + channel_state[0].frame_length + 2;
  if (lostFlag == 0) {
    has_side = !decode_only_middle;
  } else { has_side = !psDec->prev_decode_only_middle || (decControl->nChannelsInternal == 2 && lostFlag == 2 && channel_state[1].LBRR_flags[channel_state[1].nFramesDecoded] == 1);
}
  for (n = 0; n < decControl->nChannelsInternal; n++) {
    if (n == 0 || has_side) {
      int FrameIndex, condCoding;
      FrameIndex = channel_state[0].nFramesDecoded - n;
      if (FrameIndex <= 0) {
        condCoding = 0;
      } else if (lostFlag == 2) {
        condCoding = channel_state[n].LBRR_flags[FrameIndex - 1] ? 2 : 0;
      } else if (n > 0 && psDec->prev_decode_only_middle) {
        condCoding = 1;
      } else { condCoding = 2;
}
      ret += silk_decode_frame(&channel_state[n], psRangeDec, &samplesOut1_tmp[n][2], &nSamplesOutDec, lostFlag, condCoding);
    } else { zero_n_bytes(&samplesOut1_tmp[n][2], static_cast<std::size_t>(nSamplesOutDec * sizeof(opus_int16)));
}
    channel_state[n].nFramesDecoded++;
}
  if (decControl->nChannelsAPI == 2 && decControl->nChannelsInternal == 2) {
    silk_stereo_MS_to_LR(&psDec->sStereo, samplesOut1_tmp[0], samplesOut1_tmp[1], MS_pred_Q13, channel_state[0].fs_kHz, nSamplesOutDec);
  } else {
    copy_n_bytes(psDec->sStereo.sMid, static_cast<std::size_t>(2 * sizeof(opus_int16)), samplesOut1_tmp[0]);
    copy_n_bytes(&samplesOut1_tmp[0][nSamplesOutDec], static_cast<std::size_t>(2 * sizeof(opus_int16)), psDec->sStereo.sMid);
}
  *nSamplesOut = ((opus_int32)((nSamplesOutDec * decControl->API_sampleRate) / (((opus_int32)((opus_int16)(channel_state[0].fs_kHz)) * (opus_int32)((opus_int16)(1000))))));
  auto *samplesOut2_tmp = OPUS_SCRATCH(opus_int16, *nSamplesOut);
  resample_out_ptr = samplesOut2_tmp;
  for (n = 0; n < std::min(decControl->nChannelsAPI, decControl->nChannelsInternal); n++) {
    ret += silk_resampler(&channel_state[n].resampler_state, resample_out_ptr, &samplesOut1_tmp[n][1], nSamplesOutDec);
    const float scale = 1 / 32768.f; const int nOut = *nSamplesOut;
    if (decControl->nChannelsAPI == 2) {
      for (i = 0; i < nOut; i++) { samplesOut[n + 2 * i] = resample_out_ptr[i] * scale;
}
    } else {
      for (i = 0; i < nOut; i++) { samplesOut[i] = resample_out_ptr[i] * scale; } }
}
  if (decControl->nChannelsAPI == 2 && decControl->nChannelsInternal == 1) {
    const int nOut = *nSamplesOut; const float scale = 1 / 32768.f;
    if (stereo_to_mono) {
      ret += silk_resampler(&channel_state[1].resampler_state, resample_out_ptr, &samplesOut1_tmp[0][1], nSamplesOutDec);
      for (i = 0; i < nOut; i++) { samplesOut[1 + 2 * i] = resample_out_ptr[i] * scale;
}
    } else { for (i = 0; i < nOut; i++) { samplesOut[1 + 2 * i] = samplesOut[0 + 2 * i]; } }
}
  if (channel_state[0].prevSignalType == 2) {
    int mult_tab[3] = {6, 4, 3};
    decControl->prevPitchLag = channel_state[0].lagPrev * mult_tab[(channel_state[0].fs_kHz - 8) >> 2];
  } else { decControl->prevPitchLag = 0;
}
  if (lostFlag == 1) {
    for (i = 0; i < psDec->nChannelsInternal; i++) channel_state[i].LastGainIndex = 10;
  } else { psDec->prev_decode_only_middle = decode_only_middle;
}
  return ret;
}
static void silk_bwexpander_FLP(std::span<float> ar, const float chirp), silk_k2a_FLP(float *A, const float *rc, opus_int32 order),
    silk_autocorrelation_FLP(float *results, const float *inputData, int inputDataSize, int correlationCount), silk_insertion_sort_decreasing_FLP(float *a, int *idx, const int L, const int K), silk_scale_vector_FLP(float *data1, float gain, int dataSize),
    silk_scale_copy_vector_FLP(float *data_out, const float *data_in, float gain, int dataSize), silk_HP_variable_cutoff(silk_encoder_state_FLP state_Fxx[]), silk_encode_do_VAD_FLP(silk_encoder_state_FLP *psEnc, int activity),
    silk_noise_shape_analysis_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, const float *pitch_res, const float *x), silk_warped_autocorrelation_FLP(float *corr, const float *input, const float warping, const int length, const int order),
    silk_LTP_scale_ctrl_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, int condCoding), silk_find_pitch_lags_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, float res[], const float x[]),
    silk_find_pred_coefs_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, const float res_pitch[], const float x[], int condCoding), silk_find_LPC_FLP(silk_encoder_state *psEncC, opus_int16 NLSF_Q15[], const float x[], const float minInvGain),
    silk_find_LTP_FLP(float XX[4 * 5 * 5], float xX[4 * 5], const float r_ptr[], const int lag[4], const int subfr_length, const int nb_subfr), silk_LTP_analysis_filter_FLP(float *LTP_res, const float *x, const float B[5 * 4], const int pitchL[4], const float invGains[4], const int subfr_length, const int nb_subfr, const int pre_length),
    silk_residual_energy_FLP(float nrgs[4], const float x[], float a[2][16], const float gains[], const int subfr_length, const int nb_subfr, const int LPC_order), silk_LPC_analysis_filter_FLP(float r_LPC[], const float PredCoef[], const float s[], const int length, const int Order),
    silk_quant_LTP_gains_FLP(float B[4 * 5], opus_int8 cbk_index[4], opus_int8 *periodicity_index, opus_int32 *sum_log_gain_Q7, float *pred_gain_dB, const float XX[4 * 5 * 5], const float xX[4 * 5], const int subfr_len, const int nb_subfr),
    silk_process_gains_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, int condCoding), silk_corrMatrix_FLP(std::span<const float> x, int L, int Order, std::span<float> XX), silk_corrVector_FLP(std::span<const float> x, std::span<const float> t, std::span<float> Xt),
    silk_apply_sine_window_FLP(std::span<float> px_win, std::span<const float> px, int win_type), silk_A2NLSF_FLP(opus_int16 *NLSF_Q15, const float *pAR, const int LPC_order), silk_NLSF2A_FLP(float *pAR, const opus_int16 *NLSF_Q15, const int LPC_order),
    silk_process_NLSFs_FLP(silk_encoder_state *psEncC, float PredCoef[2][16], opus_int16 NLSF_Q15[16], const opus_int16 prev_NLSF_Q15[16]), silk_NSQ_wrapper_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, SideInfoIndices *psIndices, silk_nsq_state *psNSQ, opus_int8 pulses[], const float x[]);
static int silk_pitch_analysis_core_FLP(const float *frame, int *pitch_out, opus_int16 *lagIndex, opus_int8 *contourIndex, float *LTPCorr, int prevLag, const float search_thres1, const float search_thres2, const int Fs_kHz, const int complexity, const int nb_subfr),
    silk_encode_frame_FLP(silk_encoder_state_FLP *psEnc, opus_int32 *pnBytesOut, ec_enc *psRangeEnc, int condCoding, int maxBits, int useCBR), silk_init_encoder(silk_encoder_state_FLP *psEnc),
    silk_control_encoder(silk_encoder_state_FLP *psEnc, silk_EncControlStruct *encControl, const int allow_bw_switch, const int force_fs_kHz);
static float silk_schur_FLP(float refl_coef[], const float auto_corr[], int order), silk_burg_modified_FLP(float A[], const float x[], const float minInvGain, const int subfr_length, const int nb_subfr, const int D);
static double silk_inner_product_FLP_c(const float *data1, const float *data2, int dataSize), silk_energy_FLP(const float *data, int dataSize);
[[nodiscard]] constexpr auto silk_abs_float_reference(float x) noexcept -> float { return static_cast<float>(std::fabs(static_cast<double>(x)));
}
[[nodiscard]] constexpr auto silk_sqrt_reference(float x) noexcept -> float { return static_cast<float>(std::sqrt(static_cast<double>(x)));
}
[[nodiscard]] constexpr auto silk_pow_reference(float base, float exponent) noexcept -> float { return static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent)));
}
[[nodiscard]] constexpr auto silk_sigmoid(float x) noexcept -> float { return static_cast<float>(1.0 / (1.0 + std::exp(static_cast<double>(-x))));
}
[[nodiscard]] static auto silk_float2int(float x) noexcept -> opus_int32 { return static_cast<opus_int32>(float2int(x));
}
static auto silk_float2short_array(opus_int16 *out, const float *in, opus_int32 length) noexcept -> void {
  for (opus_int32 index = 0; index < length; ++index) out[index] = static_cast<opus_int16>(clamp_value(silk_float2int(in[index]), static_cast<opus_int32>(-32768), static_cast<opus_int32>(32767)));
}
static auto silk_short2float_array(float *out, const opus_int16 *in, opus_int32 length) noexcept -> void {
  for (opus_int32 index = 0; index < length; ++index) out[index] = static_cast<float>(in[index]);
}
[[nodiscard]] constexpr auto silk_log2(double x) noexcept -> float { return static_cast<float>(3.32192809488736 * std::log10(x));
}
static int silk_QueryEncoder(const void *encState, silk_EncControlStruct *encStatus);
int silk_Get_Encoder_Size(int *encSizeBytes, int channels) {
  int ret = 0;
  *encSizeBytes = sizeof(silk_encoder);
  if (channels == 1) { *encSizeBytes -= sizeof(silk_encoder_state_FLP);
}
  return ret;
}
int silk_InitEncoder(void *encState, int channels, silk_EncControlStruct *encStatus) {
  silk_encoder *psEnc;
  int n, ret = 0;
  psEnc = (silk_encoder *)encState;
  zero_n_bytes(psEnc, static_cast<std::size_t>(sizeof(silk_encoder) - (channels == 1) * sizeof(silk_encoder_state_FLP)));
  for (n = 0; n < channels; n++) { if (ret += silk_init_encoder(&psEnc->state_Fxx[n])) {}
}
  psEnc->nChannelsAPI = 1; psEnc->nChannelsInternal = 1;
  if (ret += silk_QueryEncoder(encState, encStatus)) {}
  return ret;
}
static int silk_QueryEncoder(const void *encState, silk_EncControlStruct *encStatus) {
  int ret = 0;
  silk_encoder_state_FLP *state_Fxx; silk_encoder *psEnc = (silk_encoder *)encState;
  state_Fxx = psEnc->state_Fxx; encStatus->nChannelsAPI = psEnc->nChannelsAPI;
  encStatus->nChannelsInternal = psEnc->nChannelsInternal; encStatus->API_sampleRate = state_Fxx[0].sCmn.API_fs_Hz;
  encStatus->maxInternalSampleRate = state_Fxx[0].sCmn.maxInternal_fs_Hz;
  encStatus->minInternalSampleRate = state_Fxx[0].sCmn.minInternal_fs_Hz;
  encStatus->desiredInternalSampleRate = state_Fxx[0].sCmn.desiredInternal_fs_Hz;
  encStatus->payloadSize_ms = state_Fxx[0].sCmn.PacketSize_ms; encStatus->bitRate = state_Fxx[0].sCmn.TargetRate_bps;
  encStatus->complexity = state_Fxx[0].sCmn.Complexity; encStatus->useCBR = state_Fxx[0].sCmn.useCBR;
  encStatus->internalSampleRate = ((opus_int32)((opus_int16)(state_Fxx[0].sCmn.fs_kHz)) * (opus_int32)((opus_int16)(1000)));
  encStatus->allowBandwidthSwitch = state_Fxx[0].sCmn.allow_bandwidth_switch;
  encStatus->inWBmodeWithoutVariableLP = state_Fxx[0].sCmn.fs_kHz == 16 && state_Fxx[0].sCmn.sLP.mode == 0;
  return ret;
}
OPUS_ENCODER_HUB_SIZE_OPT int silk_Encode(void *encState, silk_EncControlStruct *encControl, const opus_res *samplesIn, int nSamplesIn, ec_enc *psRangeEnc, opus_int32 *nBytesOut, const int prefillFlag, int activity) {
  int n, i, nBits, flags, tmp_payloadSize_ms = 0, tmp_complexity = 0, ret = 0;
  int nSamplesToBuffer, nSamplesToBufferMax, nBlocksOf10ms;
  int nSamplesFromInput = 0, nSamplesFromInputMax;
  int speech_act_thr_for_switch_Q8;
  opus_int32 TargetRate_bps, MStargetRates_bps[2], channelRate_bps, sum;
  silk_encoder *psEnc = (silk_encoder *)encState;
  int curr_block, tot_blocks;
  opus_assume(encControl->nChannelsAPI >= encControl->nChannelsInternal && encControl->nChannelsAPI >= psEnc->nChannelsInternal);
  for (n = 0; n < encControl->nChannelsAPI; n++) psEnc->state_Fxx[n].sCmn.nFramesEncoded = 0;
  if ((ret = check_control_input(encControl)) != 0) return ret;
  encControl->switchReady = 0;
  if (encControl->nChannelsInternal > psEnc->nChannelsInternal) {
    ret += silk_init_encoder(&psEnc->state_Fxx[1]);
    zero_n_bytes(psEnc->sStereo.pred_prev_Q13, static_cast<std::size_t>(sizeof(psEnc->sStereo.pred_prev_Q13)));
    zero_n_bytes(psEnc->sStereo.sSide, static_cast<std::size_t>(sizeof(psEnc->sStereo.sSide)));
    psEnc->sStereo.mid_side_amp_Q0[0] = 0; psEnc->sStereo.mid_side_amp_Q0[1] = 1;
    psEnc->sStereo.mid_side_amp_Q0[2] = 0; psEnc->sStereo.mid_side_amp_Q0[3] = 1; psEnc->sStereo.width_prev_Q14 = 0;
    psEnc->sStereo.smth_width_Q14 = ((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5));
    if (psEnc->nChannelsAPI == 2) {
      copy_n_bytes(&psEnc->state_Fxx[0].sCmn.resampler_state, static_cast<std::size_t>(sizeof(silk_resampler_state_struct)), &psEnc->state_Fxx[1].sCmn.resampler_state);
      copy_n_bytes(&psEnc->state_Fxx[0].sCmn.In_HP_State, static_cast<std::size_t>(sizeof(psEnc->state_Fxx[1].sCmn.In_HP_State)), &psEnc->state_Fxx[1].sCmn.In_HP_State); }
}
  psEnc->nChannelsAPI = encControl->nChannelsAPI; psEnc->nChannelsInternal = encControl->nChannelsInternal;
  nBlocksOf10ms = ((opus_int32)((100 * nSamplesIn) / (encControl->API_sampleRate)));
  tot_blocks = (nBlocksOf10ms > 1) ? nBlocksOf10ms >> 1 : 1; curr_block = 0;
  if (prefillFlag) {
    silk_LP_state save_LP;
    if (nBlocksOf10ms != 1) { return -101;
}
    if (prefillFlag == 2) { save_LP = psEnc->state_Fxx[0].sCmn.sLP; save_LP.saved_fs_kHz = psEnc->state_Fxx[0].sCmn.fs_kHz;
}
    for (n = 0; n < encControl->nChannelsInternal; n++) {
      ret = silk_init_encoder(&psEnc->state_Fxx[n]);
      if (prefillFlag == 2) { psEnc->state_Fxx[n].sCmn.sLP = save_LP;
}
      opus_assume(!ret);
}
    tmp_payloadSize_ms = encControl->payloadSize_ms; encControl->payloadSize_ms = 10;
    tmp_complexity = encControl->complexity; encControl->complexity = 0;
    for (n = 0; n < encControl->nChannelsInternal; n++) { psEnc->state_Fxx[n].sCmn.controlled_since_last_payload = 0; psEnc->state_Fxx[n].sCmn.prefillFlag = 1;
}
  } else if (nBlocksOf10ms * encControl->API_sampleRate != 100 * nSamplesIn || nSamplesIn < 0 || 1000 * (opus_int32)nSamplesIn > encControl->payloadSize_ms * encControl->API_sampleRate) return -101;
  for (n = 0; n < encControl->nChannelsInternal; n++) {
    int force_fs_kHz = (n == 1) ? psEnc->state_Fxx[0].sCmn.fs_kHz : 0;
    if ((ret = silk_control_encoder(&psEnc->state_Fxx[n], encControl, psEnc->allowBandwidthSwitch, force_fs_kHz)) != 0) { return ret;
}
}
  opus_assume(encControl->nChannelsInternal == 1 || psEnc->state_Fxx[0].sCmn.fs_kHz == psEnc->state_Fxx[1].sCmn.fs_kHz);
  nSamplesToBufferMax = 10 * nBlocksOf10ms * psEnc->state_Fxx[0].sCmn.fs_kHz;
  nSamplesFromInputMax = ((opus_int32)((nSamplesToBufferMax * psEnc->state_Fxx[0].sCmn.API_fs_Hz) / (psEnc->state_Fxx[0].sCmn.fs_kHz * 1000)));
  auto *buf = OPUS_SCRATCH(opus_int16, nSamplesFromInputMax);
  auto resample_input = [&](int channel, int samples_to_buffer, auto &&sample, bool advance = true) {
    auto &state = psEnc->state_Fxx[channel].sCmn;
    for (n = 0; n < nSamplesFromInput; n++) buf[n] = sample(n);
    ret += silk_resampler(&state.resampler_state, &state.inputBuf[state.inputBufIx + 2], buf, nSamplesFromInput);
    if (advance) state.inputBufIx += samples_to_buffer;
  };
  for (auto buffering_input = true; buffering_input;) {
    nSamplesToBuffer = psEnc->state_Fxx[0].sCmn.frame_length - psEnc->state_Fxx[0].sCmn.inputBufIx;
    nSamplesToBuffer = std::min(nSamplesToBuffer, nSamplesToBufferMax);
    if (encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 2) {
      nSamplesToBuffer = std::min(nSamplesToBuffer, psEnc->state_Fxx[1].sCmn.frame_length - psEnc->state_Fxx[1].sCmn.inputBufIx);
      nSamplesToBuffer = std::min(nSamplesToBuffer, 10 * nBlocksOf10ms * psEnc->state_Fxx[1].sCmn.fs_kHz);
    } else if (encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 1 && psEnc->nPrevChannelsInternal == 2 && psEnc->state_Fxx[0].sCmn.nFramesEncoded == 0) {
      nSamplesToBuffer = std::min(nSamplesToBuffer, psEnc->state_Fxx[1].sCmn.frame_length - psEnc->state_Fxx[1].sCmn.inputBufIx);
    }
    if (nSamplesToBuffer <= 0) return -101;
    nSamplesFromInput = ((opus_int32)((nSamplesToBuffer * psEnc->state_Fxx[0].sCmn.API_fs_Hz) / (psEnc->state_Fxx[0].sCmn.fs_kHz * 1000)));
    if (encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 2) {
      int id = psEnc->state_Fxx[0].sCmn.nFramesEncoded;
      if (psEnc->nPrevChannelsInternal == 1 && id == 0) { copy_n_bytes(&psEnc->state_Fxx[0].sCmn.resampler_state, static_cast<std::size_t>(sizeof(psEnc->state_Fxx[1].sCmn.resampler_state)), &psEnc->state_Fxx[1].sCmn.resampler_state);
}
      resample_input(0, nSamplesToBuffer, [&](int idx) { return FLOAT2INT16(samplesIn[2 * idx]); });
      resample_input(1, nSamplesToBuffer, [&](int idx) { return FLOAT2INT16(samplesIn[2 * idx + 1]); });
    } else if (encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 1) {
      auto mixed_sample = [&](int idx) { sum = FLOAT2INT16(samplesIn[2 * idx] + samplesIn[2 * idx + 1]); return static_cast<opus_int16>(rounded_rshift<1>(sum)); };
      const int mono_input_start = psEnc->state_Fxx[0].sCmn.inputBufIx;
      resample_input(0, nSamplesToBuffer, mixed_sample);
      if (psEnc->nPrevChannelsInternal == 2 && psEnc->state_Fxx[0].sCmn.nFramesEncoded == 0) {
        const int side_input_start = psEnc->state_Fxx[1].sCmn.inputBufIx;
        resample_input(1, 0, mixed_sample, false);
        for (n = 0; n < nSamplesToBuffer; n++) {
          psEnc->state_Fxx[0].sCmn.inputBuf[mono_input_start + n + 2] =
              (psEnc->state_Fxx[0].sCmn.inputBuf[mono_input_start + n + 2] +
               psEnc->state_Fxx[1].sCmn.inputBuf[side_input_start + n + 2]) >>
              1;
        }
}
    } else {
      opus_assume(encControl->nChannelsAPI == 1 && encControl->nChannelsInternal == 1);
      resample_input(0, nSamplesToBuffer, [&](int idx) { return FLOAT2INT16(samplesIn[idx]); });
}
    samplesIn += nSamplesFromInput * encControl->nChannelsAPI; nSamplesIn -= nSamplesFromInput; psEnc->allowBandwidthSwitch = 0;
    if (psEnc->state_Fxx[0].sCmn.inputBufIx >= psEnc->state_Fxx[0].sCmn.frame_length) {
      opus_assume(psEnc->state_Fxx[0].sCmn.inputBufIx == psEnc->state_Fxx[0].sCmn.frame_length);
      opus_assume(encControl->nChannelsInternal == 1 || psEnc->state_Fxx[1].sCmn.inputBufIx == psEnc->state_Fxx[1].sCmn.frame_length);
      if (psEnc->state_Fxx[0].sCmn.nFramesEncoded == 0 && !prefillFlag) {
        opus_uint8 iCDF[2] = {0, 0};
        iCDF[0] = 256 - ((256) >> ((psEnc->state_Fxx[0].sCmn.nFramesPerPacket + 1) * encControl->nChannelsInternal));
        {
          ec_enc_icdf(psRangeEnc, 0, iCDF, 8);
}
}
      silk_HP_variable_cutoff(psEnc->state_Fxx); nBits = ((opus_int32)((((encControl->bitRate) * (encControl->payloadSize_ms))) / (1000)));
      auto &state0 = psEnc->state_Fxx[0].sCmn;
      nBits /= state0.nFramesPerPacket;
      TargetRate_bps = nBits * (encControl->payloadSize_ms == 10 ? 100 : 50) - 2 * psEnc->nBitsExceeded;
      if (!prefillFlag && state0.nFramesEncoded > 0) {
        const opus_int32 bitsBalance = ec_tell(psRangeEnc) - nBits * state0.nFramesEncoded;
        TargetRate_bps -= 2 * bitsBalance;
}
      TargetRate_bps = clamp_value(TargetRate_bps, std::min(5000, encControl->bitRate), std::max(5000, encControl->bitRate));
      if (encControl->nChannelsInternal == 2) {
        const int frame_index = state0.nFramesEncoded;
        silk_stereo_LR_to_MS(&psEnc->sStereo, &state0.inputBuf[2], &psEnc->state_Fxx[1].sCmn.inputBuf[2], psEnc->sStereo.predIx[frame_index], &psEnc->sStereo.mid_only_flags[frame_index], MStargetRates_bps, TargetRate_bps, state0.speech_activity_Q8, encControl->toMono, state0.fs_kHz, state0.frame_length);
        if (psEnc->sStereo.mid_only_flags[frame_index] == 0) {
          if (psEnc->prev_decode_only_middle == 1) {
            zero_n_bytes(&psEnc->state_Fxx[1].sShape, static_cast<std::size_t>(sizeof(psEnc->state_Fxx[1].sShape))); zero_n_bytes(&psEnc->state_Fxx[1].sCmn.sNSQ, static_cast<std::size_t>(sizeof(psEnc->state_Fxx[1].sCmn.sNSQ)));
            zero_n_bytes(psEnc->state_Fxx[1].sCmn.prev_NLSFq_Q15, static_cast<std::size_t>(sizeof(psEnc->state_Fxx[1].sCmn.prev_NLSFq_Q15))); zero_n_bytes(&psEnc->state_Fxx[1].sCmn.sLP.In_LP_State, static_cast<std::size_t>(sizeof(psEnc->state_Fxx[1].sCmn.sLP.In_LP_State)));
            psEnc->state_Fxx[1].sCmn.prevLag = psEnc->state_Fxx[1].sCmn.sNSQ.lagPrev = 100; psEnc->state_Fxx[1].sShape.LastGainIndex = 10;
            psEnc->state_Fxx[1].sCmn.prevSignalType = 0; psEnc->state_Fxx[1].sCmn.sNSQ.prev_gain_Q16 = 65536; psEnc->state_Fxx[1].sCmn.first_frame_after_reset = 1;
}
          silk_encode_do_VAD_FLP(&psEnc->state_Fxx[1], activity);
        } else { psEnc->state_Fxx[1].sCmn.VAD_flags[frame_index] = 0;
}
        if (!prefillFlag) {
          {
            silk_stereo_encode_pred(psRangeEnc, psEnc->sStereo.predIx[frame_index]);
}
          if (!psEnc->state_Fxx[1].sCmn.VAD_flags[frame_index]) {
            silk_stereo_encode_mid_only(psRangeEnc, psEnc->sStereo.mid_only_flags[frame_index]); }
}
      } else {
        copy_n_bytes(psEnc->sStereo.sMid, static_cast<std::size_t>(2 * sizeof(opus_int16)), psEnc->state_Fxx[0].sCmn.inputBuf);
        copy_n_bytes(&psEnc->state_Fxx[0].sCmn.inputBuf[psEnc->state_Fxx[0].sCmn.frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), psEnc->sStereo.sMid);
}
      silk_encode_do_VAD_FLP(&psEnc->state_Fxx[0], activity);
      for (n = 0; n < encControl->nChannelsInternal; n++) {
        int maxBits = encControl->maxBits;
        if (tot_blocks == 2 && curr_block == 0) maxBits = maxBits * 3 / 5;
        else if (tot_blocks == 3 && curr_block < 2) maxBits = maxBits * (curr_block == 0 ? 2 : 3) / (curr_block == 0 ? 5 : 4);
        int useCBR = encControl->useCBR && curr_block == tot_blocks - 1;
        channelRate_bps = encControl->nChannelsInternal == 1 ? TargetRate_bps : MStargetRates_bps[n];
        if (encControl->nChannelsInternal == 2 && n == 0 && MStargetRates_bps[1] > 0) { useCBR = 0; maxBits -= encControl->maxBits / (tot_blocks * 2);
}
        if (channelRate_bps > 0) {
          silk_control_SNR(&psEnc->state_Fxx[n].sCmn, channelRate_bps);
          const int condCoding = state0.nFramesEncoded - n <= 0 ? 0 : (n > 0 && psEnc->prev_decode_only_middle ? 1 : 2);
          if ((ret = silk_encode_frame_FLP(&psEnc->state_Fxx[n], nBytesOut, psRangeEnc, condCoding, maxBits, useCBR)) != 0) {}
}
        psEnc->state_Fxx[n].sCmn.controlled_since_last_payload = 0; psEnc->state_Fxx[n].sCmn.inputBufIx = 0;
        psEnc->state_Fxx[n].sCmn.nFramesEncoded++;
}
      psEnc->prev_decode_only_middle = psEnc->sStereo.mid_only_flags[state0.nFramesEncoded - 1];
      if (*nBytesOut > 0 && state0.nFramesEncoded == state0.nFramesPerPacket) {
        flags = 0;
        for (n = 0; n < encControl->nChannelsInternal; n++) {
          for (i = 0; i < psEnc->state_Fxx[n].sCmn.nFramesPerPacket; i++) { flags = ((opus_int32)((opus_uint32)(flags) << (1))); flags |= psEnc->state_Fxx[n].sCmn.VAD_flags[i];
}
          flags = ((opus_int32)((opus_uint32)(flags) << (1)));
}
        if (!prefillFlag) {
          ec_enc_patch_initial_bits(psRangeEnc, flags, (state0.nFramesPerPacket + 1) * encControl->nChannelsInternal);
}
        psEnc->nBitsExceeded += *nBytesOut * 8;
        psEnc->nBitsExceeded -= ((opus_int32)((((encControl->bitRate) * (encControl->payloadSize_ms))) / (1000)));
        psEnc->nBitsExceeded = clamp_value(psEnc->nBitsExceeded, 0, 10000);
        speech_act_thr_for_switch_Q8 = ((opus_int32)((((opus_int32)((0.05f) * ((opus_int64)1 << (8)) + 0.5))) + (((((opus_int32)(((1 - 0.05f) / 5000) * ((opus_int64)1 << (16 + 8)) + 0.5))) * (opus_int64)((opus_int16)(psEnc->timeSinceSwitchAllowed_ms))) >> 16)));
        if (state0.speech_activity_Q8 < speech_act_thr_for_switch_Q8) { psEnc->allowBandwidthSwitch = 1; psEnc->timeSinceSwitchAllowed_ms = 0;
}
        else { psEnc->allowBandwidthSwitch = 0; psEnc->timeSinceSwitchAllowed_ms += encControl->payloadSize_ms; }
}
      if (nSamplesIn == 0) buffering_input = false; else curr_block++;
    } else buffering_input = false;
}
  psEnc->nPrevChannelsInternal = encControl->nChannelsInternal; encControl->allowBandwidthSwitch = psEnc->allowBandwidthSwitch;
  encControl->inWBmodeWithoutVariableLP = psEnc->state_Fxx[0].sCmn.fs_kHz == 16 && psEnc->state_Fxx[0].sCmn.sLP.mode == 0;
  encControl->internalSampleRate = ((opus_int32)((opus_int16)(psEnc->state_Fxx[0].sCmn.fs_kHz)) * (opus_int32)((opus_int16)(1000)));
  encControl->stereoWidth_Q14 = encControl->toMono ? 0 : psEnc->sStereo.smth_width_Q14;
  if (prefillFlag) {
    encControl->payloadSize_ms = tmp_payloadSize_ms; encControl->complexity = tmp_complexity;
    for (n = 0; n < encControl->nChannelsInternal; n++) { psEnc->state_Fxx[n].sCmn.controlled_since_last_payload = 0; psEnc->state_Fxx[n].sCmn.prefillFlag = 0; }
}
  encControl->signalType = psEnc->state_Fxx[0].sCmn.indices.signalType;
  encControl->offset = silk_Quantization_Offsets_Q10[psEnc->state_Fxx[0].sCmn.indices.signalType >> 1][psEnc->state_Fxx[0].sCmn.indices.quantOffsetType];
  return ret;
}
void silk_encode_indices(silk_encoder_state *psEncC, ec_enc *psRangeEnc, int condCoding) {
  int i, k, typeOffset, encode_absolute_lagIndex, delta_lagIndex;
  opus_int16 ec_ix[16]; opus_uint8 pred_Q8[16];
  const SideInfoIndices *psIndices = &psEncC->indices;
  typeOffset = 2 * psIndices->signalType + psIndices->quantOffsetType; opus_assume(typeOffset >= 0 && typeOffset < 6);
  {
    const auto *type_offset_icdf = typeOffset >= 2 ? silk_type_offset_VAD_iCDF.data() : silk_type_offset_no_VAD_iCDF.data();
    ec_enc_icdf(psRangeEnc, typeOffset >= 2 ? typeOffset - 2 : typeOffset, type_offset_icdf, 8);
}
  if (condCoding == 2) {
    opus_assume(psIndices->GainsIndices[0] >= 0 && psIndices->GainsIndices[0] < 36 - -4 + 1);
    ec_enc_icdf(psRangeEnc, psIndices->GainsIndices[0], silk_delta_gain_iCDF.data(), 8);
  } else {
    opus_assume(psIndices->GainsIndices[0] >= 0 && psIndices->GainsIndices[0] < 64);
    {
      ec_enc_icdf(psRangeEnc, ((psIndices->GainsIndices[0]) >> (3)), silk_gain_iCDF[psIndices->signalType].data(), 8);
}
    {
      ec_enc_icdf(psRangeEnc, psIndices->GainsIndices[0] & 7, silk_uniform8_iCDF.data(), 8); }
}
  for (i = 1; i < psEncC->nb_subfr; i++) {
    opus_assume(psIndices->GainsIndices[i] >= 0 && psIndices->GainsIndices[i] < 36 - -4 + 1);
    ec_enc_icdf(psRangeEnc, psIndices->GainsIndices[i], silk_delta_gain_iCDF.data(), 8);
}
  {
    ec_enc_icdf(psRangeEnc, psIndices->NLSFIndices[0], &psEncC->psNLSF_CB->CB1_iCDF[(psIndices->signalType >> 1) * psEncC->psNLSF_CB->nVectors], 8);
    silk_NLSF_unpack(ec_ix, pred_Q8, psEncC->psNLSF_CB, psIndices->NLSFIndices[0]);
}
  {
    opus_assume(psEncC->psNLSF_CB->order == psEncC->predictLPCOrder);
    for (i = 0; i < psEncC->psNLSF_CB->order; i++) {
      if (psIndices->NLSFIndices[i + 1] >= 4) {
        ec_enc_icdf(psRangeEnc, 2 * 4, &psEncC->psNLSF_CB->ec_iCDF[ec_ix[i]], 8);
        ec_enc_icdf(psRangeEnc, psIndices->NLSFIndices[i + 1] - 4, silk_NLSF_EXT_iCDF.data(), 8);
      } else if (psIndices->NLSFIndices[i + 1] <= -4) {
        ec_enc_icdf(psRangeEnc, 0, &psEncC->psNLSF_CB->ec_iCDF[ec_ix[i]], 8);
        ec_enc_icdf(psRangeEnc, -psIndices->NLSFIndices[i + 1] - 4, silk_NLSF_EXT_iCDF.data(), 8);
      } else {
        ec_enc_icdf(psRangeEnc, psIndices->NLSFIndices[i + 1] + 4, &psEncC->psNLSF_CB->ec_iCDF[ec_ix[i]], 8); } }
}
  if (psEncC->nb_subfr == 4) {
    opus_assume(psIndices->NLSFInterpCoef_Q2 >= 0 && psIndices->NLSFInterpCoef_Q2 < 5);
    ec_enc_icdf(psRangeEnc, psIndices->NLSFInterpCoef_Q2, silk_NLSF_interpolation_factor_iCDF.data(), 8);
}
  if (psIndices->signalType == 2) {
    encode_absolute_lagIndex = 1;
    if (condCoding == 2 && psEncC->ec_prevSignalType == 2) {
      delta_lagIndex = psIndices->lagIndex - psEncC->ec_prevLagIndex;
      if (delta_lagIndex < -8 || delta_lagIndex > 11) {
        delta_lagIndex = 0;
      } else { delta_lagIndex = delta_lagIndex + 9; encode_absolute_lagIndex = 0;
}
      opus_assume(delta_lagIndex >= 0 && delta_lagIndex < 21);
      ec_enc_icdf(psRangeEnc, delta_lagIndex, silk_pitch_delta_iCDF.data(), 8);
}
    if (encode_absolute_lagIndex) {
      opus_int32 pitch_high_bits, pitch_low_bits;
      pitch_high_bits = ((opus_int32)((psIndices->lagIndex) / (((psEncC->fs_kHz) >> (1)))));
      pitch_low_bits = psIndices->lagIndex - ((opus_int32)((opus_int16)(pitch_high_bits)) * (opus_int32)((opus_int16)(((psEncC->fs_kHz) >> (1)))));
      opus_assume(pitch_low_bits < psEncC->fs_kHz / 2); opus_assume(pitch_high_bits < 32);
      ec_enc_icdf(psRangeEnc, pitch_high_bits, silk_pitch_lag_iCDF.data(), 8);
      ec_enc_icdf(psRangeEnc, pitch_low_bits, psEncC->pitch_lag_low_bits_iCDF, 8);
}
    psEncC->ec_prevLagIndex = psIndices->lagIndex; opus_assume(psIndices->contourIndex >= 0);
    opus_assume((psIndices->contourIndex < 34 && psEncC->fs_kHz > 8 && psEncC->nb_subfr == 4) || (psIndices->contourIndex < 11 && psEncC->fs_kHz == 8 && psEncC->nb_subfr == 4) || (psIndices->contourIndex < 12 && psEncC->fs_kHz > 8 && psEncC->nb_subfr == 2) || (psIndices->contourIndex < 3 && psEncC->fs_kHz == 8 && psEncC->nb_subfr == 2));
    ec_enc_icdf(psRangeEnc, psIndices->contourIndex, psEncC->pitch_contour_iCDF, 8);
    opus_assume(psIndices->PERIndex >= 0 && psIndices->PERIndex < 3);
    ec_enc_icdf(psRangeEnc, psIndices->PERIndex, silk_LTP_per_index_iCDF.data(), 8);
    for (k = 0; k < psEncC->nb_subfr; k++) { opus_assume(psIndices->LTPIndex[k] >= 0 && psIndices->LTPIndex[k] < (8 << psIndices->PERIndex)); ec_enc_icdf(psRangeEnc, psIndices->LTPIndex[k], silk_LTP_codebooks[psIndices->PERIndex].gain_icdf.data(), 8);
}
    if (condCoding == 0) {
      opus_assume(psIndices->LTP_scaleIndex >= 0 && psIndices->LTP_scaleIndex < 3);
      ec_enc_icdf(psRangeEnc, psIndices->LTP_scaleIndex, silk_LTPscale_iCDF.data(), 8);
}
    opus_assume(!condCoding || psIndices->LTP_scaleIndex == 0);
}
  psEncC->ec_prevSignalType = psIndices->signalType;
  {
    opus_assume(psIndices->Seed >= 0 && psIndices->Seed < 4);
    ec_enc_icdf(psRangeEnc, psIndices->Seed, silk_uniform4_iCDF.data(), 8); }
}
static auto combine_and_check(std::span<int> pulses_comb, std::span<const int> pulses_in, int max_pulses) -> int {
  const auto input = pulses_in.first(pulses_comb.size() * 2);
  for (int index = 0; index < static_cast<int>(pulses_comb.size()); ++index) {
    const auto sum = input[static_cast<std::size_t>(2 * index)] + input[static_cast<std::size_t>(2 * index + 1)];
    if (sum > max_pulses) { return 1;
}
    pulses_comb[static_cast<std::size_t>(index)] = sum;
}
  return 0;
}
void silk_encode_pulses(ec_enc *psRangeEnc, const int signalType, const int quantOffsetType, std::span<opus_int8> pulses, const int frame_length) {
  int i, k, j, iter, bit, nLS, scale_down, RateLevelIndex = 0;
  opus_int32 abs_q, minSumBits_Q5, sumBits_Q5; std::array<int, 8> pulses_comb;
  int *abs_pulses_ptr;
  const opus_int8 *pulses_ptr;
  const opus_uint8 *cdf_ptr, *nBits_ptr;
  opus_assume(1 << 4 == 16); iter = ((frame_length) >> (4));
  if (iter * 16 < frame_length) {
    opus_assume(frame_length == 12 * 10); iter++;
    zero_n_bytes(&pulses[static_cast<std::size_t>(frame_length)], static_cast<std::size_t>(16 * sizeof(opus_int8)));
}
  std::array<int, silk_max_frame_length> abs_pulses_storage; auto *abs_pulses = abs_pulses_storage.data();
  for (auto index = std::size_t{}; index < static_cast<std::size_t>(iter * 16); ++index) abs_pulses[index] = std::abs(static_cast<int>(pulses[index]));
  std::array<int, silk_max_shell_blocks> sum_pulses_storage; auto *sum_pulses = sum_pulses_storage.data();
  std::array<int, silk_max_shell_blocks> nRshifts_storage; auto *nRshifts = nRshifts_storage.data();
  abs_pulses_ptr = abs_pulses;
  for (i = 0; i < iter; i++) {
    nRshifts[i] = 0;
    for (scale_down = 1; scale_down != 0;) {
      scale_down = combine_and_check(std::span<int>{pulses_comb}.first(8), std::span<const int>{abs_pulses_ptr, 16}, silk_max_pulses_table[0]);
      scale_down += combine_and_check(std::span<int>{pulses_comb}.first(4), std::span<const int>{pulses_comb}.first(8), silk_max_pulses_table[1]);
      scale_down += combine_and_check(std::span<int>{pulses_comb}.first(2), std::span<const int>{pulses_comb}.first(4), silk_max_pulses_table[2]);
      scale_down += combine_and_check(std::span<int>{sum_pulses + static_cast<std::size_t>(i), 1}, std::span<const int>{pulses_comb}.first(2), silk_max_pulses_table[3]);
      if (scale_down != 0) {
        nRshifts[static_cast<std::size_t>(i)]++;
        for (k = 0; k < 16; k++) { abs_pulses_ptr[k] = ((abs_pulses_ptr[k]) >> (1)); } }
}
    abs_pulses_ptr += 16;
}
  minSumBits_Q5 = 0x7FFFFFFF;
  for (k = 0; k < 10 - 1; k++) {
    nBits_ptr = silk_pulses_per_block_BITS_Q5[k].data(); sumBits_Q5 = silk_rate_levels_BITS_Q5[signalType >> 1][k];
    for (i = 0; i < iter; i++) {
      if (nRshifts[static_cast<std::size_t>(i)] > 0) {
        sumBits_Q5 += nBits_ptr[16 + 1];
      } else { sumBits_Q5 += nBits_ptr[sum_pulses[static_cast<std::size_t>(i)]]; }
}
    if (sumBits_Q5 < minSumBits_Q5) { minSumBits_Q5 = sumBits_Q5; RateLevelIndex = k; }
}
  ec_enc_icdf(psRangeEnc, RateLevelIndex, silk_rate_levels_iCDF[signalType >> 1].data(), 8);
  cdf_ptr = silk_pulses_per_block_iCDF[RateLevelIndex].data();
  for (i = 0; i < iter; i++) {
    if (nRshifts[static_cast<std::size_t>(i)] == 0) {
      ec_enc_icdf(psRangeEnc, sum_pulses[static_cast<std::size_t>(i)], cdf_ptr, 8);
    } else {
      ec_enc_icdf(psRangeEnc, 16 + 1, cdf_ptr, 8);
      for (k = 0; k < nRshifts[static_cast<std::size_t>(i)] - 1; k++) { ec_enc_icdf(psRangeEnc, 16 + 1, silk_pulses_per_block_iCDF[10 - 1].data(), 8);
}
      ec_enc_icdf(psRangeEnc, sum_pulses[static_cast<std::size_t>(i)], silk_pulses_per_block_iCDF[10 - 1].data(), 8); }
}
  for (i = 0; i < iter; i++) {
    if (sum_pulses[static_cast<std::size_t>(i)] > 0) { silk_shell_encoder(psRangeEnc, std::span<const int, 16>{abs_pulses + static_cast<std::size_t>(i * 16), 16}); }
}
  for (i = 0; i < iter; i++) {
    if (nRshifts[static_cast<std::size_t>(i)] > 0) {
      pulses_ptr = pulses.data() + i * 16; nLS = nRshifts[static_cast<std::size_t>(i)] - 1;
      for (k = 0; k < 16; k++) {
        abs_q = (opus_int8)(((pulses_ptr[k]) > 0) ? (pulses_ptr[k]) : -(pulses_ptr[k]));
        for (j = nLS; j > 0; j--) { bit = ((abs_q) >> (j)) & 1; ec_enc_icdf(psRangeEnc, bit, silk_lsb_iCDF.data(), 8);
}
        bit = abs_q & 1; ec_enc_icdf(psRangeEnc, bit, silk_lsb_iCDF.data(), 8); } }
}
  silk_encode_signs(psRangeEnc, std::span<const opus_int8>{pulses.data(), static_cast<std::size_t>(iter * 16)}, signalType, quantOffsetType, std::span<const int>{sum_pulses, static_cast<std::size_t>(iter)});
}
void silk_gains_quant(opus_int8 ind[4], opus_int32 gain_Q16[4], opus_int8 *prev_ind, const int conditional, const int nb_subfr) {
  int k, double_step_size_threshold;
  for (k = 0; k < nb_subfr; k++) {
    ind[k] = ((opus_int32)(((((65536 * (64 - 1)) / (((88 - 2) * 128) / 6))) * (opus_int64)((opus_int16)(silk_lin2log(gain_Q16[k]) - ((2 * 128) / 6 + 16 * 128)))) >> 16));
    if (ind[k] < *prev_ind) { ind[k]++;
}
    ind[k] = static_cast<opus_int8>(clamp_value<int>(ind[k], 0, 64 - 1));
    if (k == 0 && conditional == 0) {
      ind[k] = static_cast<opus_int8>(clamp_value<int>(ind[k], *prev_ind - 4, 64 - 1));
      *prev_ind = ind[k];
    } else {
      ind[k] = ind[k] - *prev_ind; double_step_size_threshold = 2 * 36 - 64 + *prev_ind;
      if (ind[k] > double_step_size_threshold) { ind[k] = double_step_size_threshold + ((ind[k] - double_step_size_threshold + 1) >> (1));
}
      ind[k] = static_cast<opus_int8>(clamp_value<int>(ind[k], -4, 36));
      if (ind[k] > double_step_size_threshold) {
        *prev_ind += ((opus_int32)((opus_uint32)(ind[k]) << (1))) - double_step_size_threshold;
        *prev_ind = std::min(*prev_ind, (opus_int8)(64 - 1));
      } else { *prev_ind += ind[k];
}
      ind[k] -= -4;
}
    gain_Q16[k] = silk_log2lin(std::min(((opus_int32)(((((65536 * (((88 - 2) * 128) / 6)) / (64 - 1))) * (opus_int64)((opus_int16)(*prev_ind))) >> 16)) + ((2 * 128) / 6 + 16 * 128), 3967)); }
}
void silk_gains_dequant(opus_int32 gain_Q16[4], const opus_int8 ind[4], opus_int8 *prev_ind, const int conditional, const int nb_subfr) {
  int k, ind_tmp, double_step_size_threshold;
  for (k = 0; k < nb_subfr; k++) {
    if (k == 0 && conditional == 0) {
      *prev_ind = std::max(ind[k], (opus_int8)(*prev_ind - 16));
    } else {
      ind_tmp = ind[k] + -4; double_step_size_threshold = 2 * 36 - 64 + *prev_ind;
      if (ind_tmp > double_step_size_threshold) {
        *prev_ind += ((opus_int32)((opus_uint32)(ind_tmp) << (1))) - double_step_size_threshold;
      } else { *prev_ind += ind_tmp; }
}
    *prev_ind = static_cast<opus_int8>(clamp_value<int>(*prev_ind, 0, 64 - 1));
    gain_Q16[k] = silk_log2lin(std::min(((opus_int32)(((((65536 * (((88 - 2) * 128) / 6)) / (64 - 1))) * (opus_int64)((opus_int16)(*prev_ind))) >> 16)) + ((2 * 128) / 6 + 16 * 128), 3967)); }
}
opus_int32 silk_gains_ID(const opus_int8 ind[4], const int nb_subfr) {
  int k;
  opus_int32 gainsID = 0;
  for (k = 0; k < nb_subfr; k++) { gainsID = (((ind[k])) + (((opus_int32)((opus_uint32)((gainsID)) << ((8))))));
}
  return gainsID;
}
static void silk_interpolate(std::span<opus_int16> xi, std::span<const opus_int16> x0, std::span<const opus_int16> x1, const int ifact_Q2) {
  opus_assume(ifact_Q2 >= 0); opus_assume(ifact_Q2 <= 4);
  const auto count = static_cast<int>(xi.size());
  for (int i = 0; i < count; i++) { xi[static_cast<std::size_t>(i)] = (opus_int16)((x0[static_cast<std::size_t>(i)]) + (((((opus_int32)((opus_int16)(x1[static_cast<std::size_t>(i)] - x0[static_cast<std::size_t>(i)])) * (opus_int32)((opus_int16)(ifact_Q2))))) >> ((2)))); }
}
static void silk_LP_interpolate_filter_taps(opus_int32 B_Q28[3], opus_int32 A_Q28[2], const int ind, const opus_int32 fac_Q16) {
  const auto b_out = std::span<opus_int32, 3>{B_Q28, 3};
  const auto a_out = std::span<opus_int32, 2>{A_Q28, 2};
  if (ind >= 5 - 1) {
    copy_n_items(silk_Transition_LP_B_Q28[5 - 1].data(), silk_Transition_LP_B_Q28[5 - 1].size(), b_out.data());
    copy_n_items(silk_Transition_LP_A_Q28[5 - 1].data(), silk_Transition_LP_A_Q28[5 - 1].size(), a_out.data()); return;
}
  if (fac_Q16 <= 0) {
    copy_n_items(silk_Transition_LP_B_Q28[ind].data(), silk_Transition_LP_B_Q28[ind].size(), b_out.data());
    copy_n_items(silk_Transition_LP_A_Q28[ind].data(), silk_Transition_LP_A_Q28[ind].size(), a_out.data()); return;
}
  if (fac_Q16 < 32768) {
    for (int index = 0; index < 3; ++index) { b_out[static_cast<std::size_t>(index)] = static_cast<opus_int32>(silk_Transition_LP_B_Q28[ind][index] + (((silk_Transition_LP_B_Q28[ind + 1][index] - silk_Transition_LP_B_Q28[ind][index]) * static_cast<opus_int64>(static_cast<opus_int16>(fac_Q16))) >> 16));
}
    for (int index = 0; index < 2; ++index) { a_out[static_cast<std::size_t>(index)] = static_cast<opus_int32>(silk_Transition_LP_A_Q28[ind][index] + (((silk_Transition_LP_A_Q28[ind + 1][index] - silk_Transition_LP_A_Q28[ind][index]) * static_cast<opus_int64>(static_cast<opus_int16>(fac_Q16))) >> 16));
}
    return;
}
  opus_assume(fac_Q16 - (1 << 16) == saturate_int16_from_int32(fac_Q16 - (1 << 16)));
  const auto delta = static_cast<opus_int16>(fac_Q16 - (static_cast<opus_int32>(1) << 16));
  for (int index = 0; index < 3; ++index) { b_out[static_cast<std::size_t>(index)] = static_cast<opus_int32>(silk_Transition_LP_B_Q28[ind + 1][index] + (((silk_Transition_LP_B_Q28[ind + 1][index] - silk_Transition_LP_B_Q28[ind][index]) * static_cast<opus_int64>(delta)) >> 16));
}
  for (int index = 0; index < 2; ++index) { a_out[static_cast<std::size_t>(index)] = static_cast<opus_int32>(silk_Transition_LP_A_Q28[ind + 1][index] + (((silk_Transition_LP_A_Q28[ind + 1][index] - silk_Transition_LP_A_Q28[ind][index]) * static_cast<opus_int64>(delta)) >> 16)); }
}
void silk_LP_variable_cutoff(silk_LP_state *psLP, opus_int16 *frame, const int frame_length) {
  opus_int32 B_Q28[3], A_Q28[2], fac_Q16 = 0;
  int ind = 0;
  opus_assume(psLP->transition_frame_no >= 0 && psLP->transition_frame_no <= (5120 / (5 * 4)));
  if (psLP->mode != 0) {
    fac_Q16 = ((opus_int32)((opus_uint32)((5120 / (5 * 4)) - psLP->transition_frame_no) << (16 - 6)));
    ind = ((fac_Q16) >> (16)); fac_Q16 -= ((opus_int32)((opus_uint32)(ind) << (16)));
    opus_assume(ind >= 0); opus_assume(ind < 5);
    silk_LP_interpolate_filter_taps(B_Q28, A_Q28, ind, fac_Q16);
    psLP->transition_frame_no = ((0) > ((5120 / (5 * 4))) ? ((psLP->transition_frame_no + psLP->mode) > (0) ? (0) : ((psLP->transition_frame_no + psLP->mode) < ((5120 / (5 * 4))) ? ((5120 / (5 * 4))) : (psLP->transition_frame_no + psLP->mode))) : ((psLP->transition_frame_no + psLP->mode) > ((5120 / (5 * 4))) ? ((5120 / (5 * 4))) : ((psLP->transition_frame_no + psLP->mode) < (0) ? (0) : (psLP->transition_frame_no + psLP->mode))));
    opus_assume(3 == 3 && 2 == 2);
    silk_biquad_alt_stride1(frame, B_Q28, A_Q28, psLP->In_LP_State, frame, frame_length); }
}
static void silk_NLSF_residual_dequant(opus_int16 x_Q10[], const opus_int8 indices[], const opus_uint8 pred_coef_Q8[], const int quant_step_size_Q16, const opus_int16 order) {
  constexpr auto adjustment = static_cast<opus_int32>((0.1) * (static_cast<opus_int64>(1) << 10) + 0.5);
  auto residual = opus_int32{0};
  for (int index = order - 1; index >= 0; --index) { const auto prediction = static_cast<opus_int32>((static_cast<opus_int32>(static_cast<opus_int16>(residual)) * static_cast<opus_int32>(static_cast<opus_int16>(static_cast<opus_int16>(pred_coef_Q8[index])))) >> 8); residual = static_cast<opus_int32>(static_cast<opus_uint32>(indices[index]) << 10); if (residual > 0) { residual -= adjustment; } else if (residual < 0) { residual += adjustment; } residual = static_cast<opus_int32>(prediction + ((static_cast<opus_int32>(residual) * static_cast<opus_int64>(static_cast<opus_int16>(quant_step_size_Q16))) >> 16)); x_Q10[index] = residual; }
}
void silk_NLSF_decode(opus_int16 *pNLSF_Q15, opus_int8 *NLSFIndices, const silk_NLSF_CB_struct *psNLSF_CB) {
  int i;
  opus_uint8 pred_Q8[16]; opus_int16 ec_ix[16];
  opus_int16 res_Q10[16]; opus_int32 NLSF_Q15_tmp;
  const opus_uint8 *pCB_element;
  const opus_int16 *pCB_Wght_Q9;
  silk_NLSF_unpack(ec_ix, pred_Q8, psNLSF_CB, NLSFIndices[0]);
  silk_NLSF_residual_dequant(res_Q10, &NLSFIndices[1], pred_Q8, psNLSF_CB->quantStepSize_Q16, psNLSF_CB->order);
  pCB_element = &psNLSF_CB->CB1_NLSF_Q8[NLSFIndices[0] * psNLSF_CB->order];
  pCB_Wght_Q9 = &psNLSF_CB->CB1_Wght_Q9[NLSFIndices[0] * psNLSF_CB->order];
  for (i = 0; i < psNLSF_CB->order; i++) { NLSF_Q15_tmp = (((((opus_int32)((((opus_int32)((opus_uint32)((opus_int32)res_Q10[i]) << (14)))) / (pCB_Wght_Q9[i]))))) + (((opus_int32)((opus_uint32)(((opus_int16)pCB_element[i])) << ((7)))))); pNLSF_Q15[i] = (opus_int16)((0) > (32767) ? ((NLSF_Q15_tmp) > (0) ? (0) : ((NLSF_Q15_tmp) < (32767) ? (32767) : (NLSF_Q15_tmp))) : ((NLSF_Q15_tmp) > (32767) ? (32767) : ((NLSF_Q15_tmp) < (0) ? (0) : (NLSF_Q15_tmp))));
}
  silk_NLSF_stabilize(pNLSF_Q15, psNLSF_CB->deltaMin_Q15, psNLSF_CB->order);
}
static auto silk_NSQ_noise_shape_feedback_loop_c(const opus_int32 *data0, opus_int32 *data1, const opus_int16 *coef, int order) -> opus_int32 {
  auto delayed_0 = data0[0];
  auto delayed_1 = data1[0];
  data1[0] = delayed_0;
  auto feedback = order >> 1;
  feedback = static_cast<opus_int32>(feedback + ((delayed_0 * static_cast<opus_int64>(static_cast<opus_int16>(coef[0]))) >> 16));
  for (int index = 2; index < order; index += 2) {
    delayed_0 = data1[index - 1]; data1[index - 1] = delayed_1;
    feedback = static_cast<opus_int32>(feedback + ((delayed_1 * static_cast<opus_int64>(static_cast<opus_int16>(coef[index - 1]))) >> 16));
    delayed_1 = data1[index]; data1[index] = delayed_0;
    feedback = static_cast<opus_int32>(feedback + ((delayed_0 * static_cast<opus_int64>(static_cast<opus_int16>(coef[index]))) >> 16));
}
  data1[order - 1] = delayed_1;
  feedback = static_cast<opus_int32>(feedback + ((delayed_1 * static_cast<opus_int64>(static_cast<opus_int16>(coef[order - 1]))) >> 16));
  return static_cast<opus_int32>(static_cast<opus_uint32>(feedback) << 1);
}
struct silk_nsq_sample_state { opus_int32 Q_Q10, RD_Q10, xq_Q14, LF_AR_Q14, Diff_Q14, sLTP_shp_Q14, LPC_exc_Q14; };
struct silk_nsq_quantization_candidates { std::array<opus_int32, 2> q_Q10{}, rd_Q20{}; };
static auto silk_finish_nsq(const silk_encoder_state *psEncC, silk_nsq_state *NSQ) noexcept -> void { move_n_bytes(&NSQ->xq[psEncC->frame_length], static_cast<std::size_t>(psEncC->ltp_mem_length * sizeof(opus_int16)), NSQ->xq); move_n_bytes(&NSQ->sLTP_shp_Q14[psEncC->frame_length], static_cast<std::size_t>(psEncC->ltp_mem_length * sizeof(opus_int32)), NSQ->sLTP_shp_Q14);
}
[[nodiscard]] constexpr auto silk_pack_harm_shape_gain(opus_int32 gain_q14) noexcept -> opus_int32 { return (gain_q14 >> 2) | static_cast<opus_int32>(static_cast<opus_uint32>(gain_q14 >> 1) << 16);
}
[[nodiscard]] static constexpr auto silk_signed_clamped_residual(opus_int32 residual_q10, opus_int32 seed) noexcept -> opus_int32 { return clamp_value(seed < 0 ? -residual_q10 : residual_q10, -(31 << 10), 30 << 10);
}
[[nodiscard]] constexpr auto silk_square_i16(opus_int32 value) noexcept -> opus_int32 { return static_cast<opus_int32>(static_cast<opus_int16>(value)) * static_cast<opus_int32>(static_cast<opus_int16>(value));
}
[[nodiscard]] static auto silk_quantize_candidates(opus_int32 residual_q10, int Lambda_Q10, int offset_Q10) noexcept -> silk_nsq_quantization_candidates {
  auto q1_Q10 = residual_q10 - offset_Q10, q1_Q0 = q1_Q10 >> 10;
  if (Lambda_Q10 > 2048) {
    const auto rdo_offset = Lambda_Q10 / 2 - 512;
    if (q1_Q10 > rdo_offset) { q1_Q0 = (q1_Q10 - rdo_offset) >> 10; } else if (q1_Q10 < -rdo_offset) { q1_Q0 = (q1_Q10 + rdo_offset) >> 10; } else { q1_Q0 = q1_Q10 < 0 ? -1 : 0; }
}
  opus_int32 q2_Q10, rd1_Q20, rd2_Q20;
  if (q1_Q0 > 0) {
    q1_Q10 = (q1_Q0 << 10) - 80 + offset_Q10; q2_Q10 = q1_Q10 + 1024; rd1_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(q1_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10)); rd2_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(q2_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10));
  } else if (q1_Q0 == 0) {
    q1_Q10 = offset_Q10; q2_Q10 = q1_Q10 + (1024 - 80); rd1_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(q1_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10)); rd2_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(q2_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10));
  } else if (q1_Q0 == -1) {
    q2_Q10 = offset_Q10; q1_Q10 = q2_Q10 - (1024 - 80); rd1_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(-q1_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10)); rd2_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(q2_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10));
  } else {
    q1_Q10 = (q1_Q0 << 10) + 80 + offset_Q10; q2_Q10 = q1_Q10 + 1024; rd1_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(-q1_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10)); rd2_Q20 = static_cast<opus_int32>(static_cast<opus_int16>(-q2_Q10)) * static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10));
}
  return {{q1_Q10, q2_Q10}, {rd1_Q20 + silk_square_i16(residual_q10 - q1_Q10), rd2_Q20 + silk_square_i16(residual_q10 - q2_Q10)}};
}
[[nodiscard]] static constexpr auto silk_harmonic_shaping(const opus_int32 *shp_lag_ptr, opus_int32 HarmShapeFIRPacked_Q14) noexcept -> opus_int32;
[[nodiscard]] constexpr auto silk_nsq_build_sample(opus_int32 q_Q10, opus_int32 seed, opus_int32 LTP_pred_Q14, opus_int32 LPC_pred_Q14, opus_int32 x_Q10, opus_int32 n_AR_Q14, opus_int32 n_LF_Q14) noexcept -> silk_nsq_sample_state;
static inline void scale_q16_buffer(opus_int32 *data, const int count, const opus_int32 gain_Q16) noexcept {
  for (int i = 0; i < count; ++i) data[i] = static_cast<opus_int32>((static_cast<opus_int64>(gain_Q16) * data[i]) >> 16);
}
static auto silk_nsq_scale_common(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, std::span<const opus_int16> x16, std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP, std::span<opus_int32> sLTP_Q15, int subfr, const int LTP_scale_Q14, std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL, const int signal_type, const int ltp_scale_end) noexcept -> opus_int32;
static void silk_nsq_scale_states(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, std::span<const opus_int16> x16, std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP, std::span<opus_int32> sLTP_Q15, int subfr, const int LTP_scale_Q14, std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL, const int signal_type) {
  static_cast<void>(silk_nsq_scale_common(psEncC, NSQ, x16, x_sc_Q10, sLTP, sLTP_Q15, subfr, LTP_scale_Q14, Gains_Q16, pitchL, signal_type, NSQ->sLTP_buf_idx));
}
static void silk_noise_shape_quantizer(silk_nsq_state *NSQ, int signalType, std::span<const opus_int32> x_sc_Q10, std::span<opus_int8> pulses, std::span<opus_int16> xq, std::span<opus_int32> sLTP_Q15, std::span<const opus_int16> a_Q12, std::span<const opus_int16> b_Q14, std::span<const opus_int16> AR_shp_Q13, int lag, opus_int32 HarmShapeFIRPacked_Q14, int Tilt_Q14, opus_int32 LF_shp_Q14, opus_int32 Gain_Q16, int Lambda_Q10, int offset_Q10);
void silk_NSQ_c(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, SideInfoIndices *psIndices, const opus_int16 x16[], opus_int8 pulses[], const opus_int16 *PredCoef_Q12, const opus_int16 LTPCoef_Q14[5 * 4], const opus_int16 AR_Q13[4 * 24], const int HarmShapeGain_Q14[4], const int Tilt_Q14[4], const opus_int32 LF_shp_Q14[4], const opus_int32 Gains_Q16[4], const int pitchL[4], const int Lambda_Q10, const int LTP_scale_Q14) {
  int k, lag, start_idx, LSF_interpolation_flag;
  const opus_int16 *A_Q12, *B_Q14, *AR_shp_Q13;
  opus_int16 *pxq; opus_int32 HarmShapeFIRPacked_Q14;
  int offset_Q10;
  NSQ->rand_seed = psIndices->Seed; lag = NSQ->lagPrev;
  opus_assume(NSQ->prev_gain_Q16 != 0);
  offset_Q10 = silk_Quantization_Offsets_Q10[psIndices->signalType >> 1][psIndices->quantOffsetType];
  LSF_interpolation_flag = psIndices->NLSFInterpCoef_Q2 != 4;
  opus_assume(psEncC->ltp_mem_length + psEncC->frame_length <= silk_max_ltp_buffer_length); opus_assume(psEncC->subfr_length <= silk_max_subfr_length);
  std::array<opus_int32, silk_max_ltp_buffer_length> sLTP_Q15_storage; std::array<opus_int16, silk_max_ltp_buffer_length> sLTP_storage; auto *sLTP = sLTP_storage.data(); std::array<opus_int32, silk_max_subfr_length> x_sc_Q10_storage;
  NSQ->sLTP_shp_buf_idx = psEncC->ltp_mem_length; NSQ->sLTP_buf_idx = psEncC->ltp_mem_length; pxq = &NSQ->xq[psEncC->ltp_mem_length];
  for (k = 0; k < psEncC->nb_subfr; k++) {
    A_Q12 = &PredCoef_Q12[((k >> 1) | (1 - LSF_interpolation_flag)) * 16]; B_Q14 = &LTPCoef_Q14[k * 5];
    AR_shp_Q13 = &AR_Q13[k * 24]; opus_assume(HarmShapeGain_Q14[k] >= 0); HarmShapeFIRPacked_Q14 = silk_pack_harm_shape_gain(HarmShapeGain_Q14[k]); NSQ->rewhite_flag = 0;
    if (psIndices->signalType == 2) {
      lag = pitchL[k];
      if ((k & (3 - ((opus_int32)((opus_uint32)(LSF_interpolation_flag) << (1))))) == 0) {
        start_idx = psEncC->ltp_mem_length - lag - psEncC->predictLPCOrder - 5 / 2; opus_assume(start_idx > 0);
        silk_LPC_analysis_filter(&sLTP[start_idx], &NSQ->xq[start_idx + k * psEncC->subfr_length], A_Q12, psEncC->ltp_mem_length - start_idx, psEncC->predictLPCOrder);
        NSQ->rewhite_flag = 1; NSQ->sLTP_buf_idx = psEncC->ltp_mem_length; }
}
    silk_nsq_scale_states(psEncC, NSQ, {x16, static_cast<std::size_t>(psEncC->subfr_length)}, x_sc_Q10_storage, sLTP_storage, sLTP_Q15_storage, k, LTP_scale_Q14, {Gains_Q16, 4}, {pitchL, 4}, psIndices->signalType);
    silk_noise_shape_quantizer(NSQ, psIndices->signalType, x_sc_Q10_storage, {pulses, static_cast<std::size_t>(psEncC->subfr_length)}, {pxq, static_cast<std::size_t>(psEncC->subfr_length)}, sLTP_Q15_storage, {A_Q12, static_cast<std::size_t>(psEncC->predictLPCOrder)}, {B_Q14, 5}, {AR_shp_Q13, static_cast<std::size_t>(psEncC->shapingLPCOrder)}, lag, HarmShapeFIRPacked_Q14, Tilt_Q14[k], LF_shp_Q14[k], Gains_Q16[k], Lambda_Q10, offset_Q10);
    x16 += psEncC->subfr_length; pulses += psEncC->subfr_length; pxq += psEncC->subfr_length;
}
  NSQ->lagPrev = pitchL[psEncC->nb_subfr - 1]; silk_finish_nsq(psEncC, NSQ);
}
static void silk_noise_shape_quantizer(silk_nsq_state *NSQ, int signalType, std::span<const opus_int32> x_sc_Q10, std::span<opus_int8> pulses, std::span<opus_int16> xq, std::span<opus_int32> sLTP_Q15, std::span<const opus_int16> a_Q12, std::span<const opus_int16> b_Q14, std::span<const opus_int16> AR_shp_Q13, int lag, opus_int32 HarmShapeFIRPacked_Q14, int Tilt_Q14, opus_int32 LF_shp_Q14, opus_int32 Gain_Q16, int Lambda_Q10, int offset_Q10) {
  int i;
  opus_int32 LTP_pred_Q13, LPC_pred_Q10, n_AR_Q12, n_LTP_Q13;
  opus_int32 n_LF_Q12, r_Q10, Gain_Q10, tmp1;
  opus_int32 *psLPC_Q14, *shp_lag_ptr, *pred_lag_ptr;
  const auto length = static_cast<int>(x_sc_Q10.size()); const auto shapingLPCOrder = static_cast<int>(AR_shp_Q13.size());
  const auto b_q14_coefficients = b_Q14.first<5>();
  shp_lag_ptr = &NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - lag + 3 / 2];
  pred_lag_ptr = &sLTP_Q15[NSQ->sLTP_buf_idx - lag + 5 / 2]; Gain_Q10 = ((Gain_Q16) >> (6)); psLPC_Q14 = &NSQ->sLPC_Q14[16 - 1];
  for (i = 0; i < length; i++) {
    NSQ->rand_seed = silk_next_rand_seed(NSQ->rand_seed);
    LPC_pred_Q10 = silk_lpc_prediction_q10(psLPC_Q14 + 1, a_Q12);
    if (signalType == 2) { LTP_pred_Q13 = silk_ltp_prediction_5tap(pred_lag_ptr, b_q14_coefficients); ++pred_lag_ptr; } else { LTP_pred_Q13 = 0;
}
    opus_assume((shapingLPCOrder & 1) == 0);
    n_AR_Q12 = silk_NSQ_noise_shape_feedback_loop_c(&NSQ->sDiff_shp_Q14, NSQ->sAR2_Q14, AR_shp_Q13.data(), shapingLPCOrder);
    n_AR_Q12 = ((opus_int32)((n_AR_Q12) + (((NSQ->sLF_AR_shp_Q14) * (opus_int64)((opus_int16)(Tilt_Q14))) >> 16)));
    n_LF_Q12 = ((opus_int32)(((NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - 1]) * (opus_int64)((opus_int16)(LF_shp_Q14))) >> 16));
    n_LF_Q12 = ((opus_int32)((n_LF_Q12) + (((NSQ->sLF_AR_shp_Q14) * ((opus_int64)(LF_shp_Q14) >> 16)) >> 16)));
    opus_assume(lag > 0 || signalType != 2);
    tmp1 = ((opus_int32)((opus_uint32)(((opus_int32)((opus_uint32)(LPC_pred_Q10) << (2)))) - (opus_uint32)(n_AR_Q12)));
    tmp1 = ((opus_int32)((opus_uint32)(tmp1) - (opus_uint32)(n_LF_Q12)));
    if (lag > 0) {
      n_LTP_Q13 = saturating_left_shift<1>(silk_harmonic_shaping(shp_lag_ptr, HarmShapeFIRPacked_Q14)); ++shp_lag_ptr;
      tmp1 = rounded_rshift<3>(saturating_add_int32(LTP_pred_Q13 - n_LTP_Q13, static_cast<opus_int32>(static_cast<opus_uint32>(tmp1) << 1)));
    } else { tmp1 = rounded_rshift<2>(tmp1);
}
    r_Q10 = silk_signed_clamped_residual(x_sc_Q10[i] - tmp1, NSQ->rand_seed);
    const auto candidates = silk_quantize_candidates(r_Q10, Lambda_Q10, offset_Q10);
    const auto best_index = candidates.rd_Q20[1] < candidates.rd_Q20[0];
    const auto sample = silk_nsq_build_sample(candidates.q_Q10[best_index], NSQ->rand_seed, saturating_left_shift<1>(LTP_pred_Q13), saturating_left_shift<4>(LPC_pred_Q10), x_sc_Q10[i], saturating_left_shift<2>(n_AR_Q12), saturating_left_shift<2>(n_LF_Q12));
    pulses[i] = delayed_pulse_from_q10(sample.Q_Q10);
    xq[i] = scale_and_saturate_q14<8>(sample.xq_Q14, Gain_Q10); psLPC_Q14++; *psLPC_Q14 = sample.xq_Q14;
    NSQ->sDiff_shp_Q14 = sample.Diff_Q14; NSQ->sLF_AR_shp_Q14 = sample.LF_AR_Q14;
    NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx] = sample.sLTP_shp_Q14;
    sLTP_Q15[NSQ->sLTP_buf_idx] = static_cast<opus_int32>(static_cast<opus_uint32>(sample.LPC_exc_Q14) << 1); NSQ->sLTP_shp_buf_idx++;
    NSQ->sLTP_buf_idx++; NSQ->rand_seed = ((opus_int32)((opus_uint32)(NSQ->rand_seed) + (opus_uint32)(pulses[i])));
}
  copy_n_bytes(&NSQ->sLPC_Q14[length], static_cast<std::size_t>(16 * sizeof(opus_int32)), NSQ->sLPC_Q14);
}
static auto silk_nsq_scale_common(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, std::span<const opus_int16> x16, std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP, std::span<opus_int32> sLTP_Q15, int subfr, const int LTP_scale_Q14, std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL, const int signal_type, const int ltp_scale_end) noexcept -> opus_int32 {
  const auto lag = pitchL[subfr]; auto inv_gain_Q31 = silk_INVERSE32_varQ(std::max(Gains_Q16[subfr], 1), 47);
  opus_assume(inv_gain_Q31 != 0); const auto inv_gain_Q26 = rounded_rshift<5>(inv_gain_Q31);
  for (auto index = std::size_t{}; index < x16.size(); ++index) x_sc_Q10[index] = static_cast<opus_int32>((static_cast<opus_int64>(x16[index]) * inv_gain_Q26) >> 16);
  if (NSQ->rewhite_flag) { if (subfr == 0) { inv_gain_Q31 = static_cast<opus_int32>(static_cast<opus_uint32>(static_cast<opus_int32>((inv_gain_Q31 * static_cast<opus_int64>(static_cast<opus_int16>(LTP_scale_Q14))) >> 16)) << 2); } for (auto i = NSQ->sLTP_buf_idx - lag - 5 / 2; i < NSQ->sLTP_buf_idx; i++) { opus_assume(i < ((5 * 4) * 16)); sLTP_Q15[i] = static_cast<opus_int32>((inv_gain_Q31 * static_cast<opus_int64>(static_cast<opus_int16>(sLTP[i]))) >> 16); }
}
  if (Gains_Q16[subfr] == NSQ->prev_gain_Q16) { return static_cast<opus_int32>(1) << 16;
}
  const auto gain_adj_Q16 = silk_DIV32_varQ(NSQ->prev_gain_Q16, Gains_Q16[subfr], 16);
  scale_q16_buffer(NSQ->sLTP_shp_Q14 + (NSQ->sLTP_shp_buf_idx - psEncC->ltp_mem_length), psEncC->ltp_mem_length, gain_adj_Q16);
  if (signal_type == 2 && NSQ->rewhite_flag == 0) { const auto ltp_start = NSQ->sLTP_buf_idx - lag - 5 / 2; scale_q16_buffer(sLTP_Q15.data() + ltp_start, ltp_scale_end - ltp_start, gain_adj_Q16);
}
  NSQ->sLF_AR_shp_Q14 = static_cast<opus_int32>((static_cast<opus_int64>(gain_adj_Q16) * NSQ->sLF_AR_shp_Q14) >> 16); NSQ->sDiff_shp_Q14 = static_cast<opus_int32>((static_cast<opus_int64>(gain_adj_Q16) * NSQ->sDiff_shp_Q14) >> 16); scale_q16_buffer(NSQ->sLPC_Q14, 16, gain_adj_Q16); scale_q16_buffer(NSQ->sAR2_Q14, 24, gain_adj_Q16); NSQ->prev_gain_Q16 = Gains_Q16[subfr]; return gain_adj_Q16;
}
struct NSQ_del_dec_struct { opus_int32 sLPC_Q14[(5 * 16) + 16], RandState[40], Q_Q10[40], Xq_Q14[40], Pred_Q15[40], Shape_Q14[40], sAR2_Q14[24], LF_AR_Q14, Diff_Q14, Seed, SeedInit, RD_Q10; };
using NSQ_sample_struct = silk_nsq_sample_state;
using NSQ_sample_pair = std::array<NSQ_sample_struct, 2>;
template <int Shift> [[nodiscard]] static constexpr auto rounded_rshift(opus_int32 value) noexcept -> opus_int32 { if constexpr (Shift == 1) { return (value >> 1) + (value & 1); } else { return ((value >> (Shift - 1)) + 1) >> 1; }
}
template <int Shift> [[nodiscard]] static constexpr auto rounded_rshift(opus_int64 value) noexcept -> opus_int64 { if constexpr (Shift == 1) { return (value >> 1) + (value & 1); } else { return ((value >> (Shift - 1)) + 1) >> 1; }
}
[[nodiscard]] static constexpr auto rounded_rshift(opus_int64 value, const int shift) noexcept -> opus_int64 { return shift == 1 ? ((value >> 1) + (value & 1)) : (((value >> (shift - 1)) + 1) >> 1);
}
[[nodiscard]] static constexpr auto saturate_int16_from_int32(opus_int32 value) noexcept -> opus_int16 { return static_cast<opus_int16>(clamp_value(value, static_cast<opus_int32>(-32768), static_cast<opus_int32>(32767)));
}
template <int Shift> [[nodiscard]] static inline auto scale_and_saturate_q14(opus_int32 sample_q14, opus_int32 gain) noexcept -> opus_int16 {
  const auto scaled = static_cast<opus_int32>((static_cast<opus_int64>(sample_q14) * gain) >> 16);
  return saturate_int16_from_int32(rounded_rshift<Shift>(scaled));
}
template <int Shift> [[nodiscard]] static constexpr auto rounded_rshift_to_int16(opus_int32 value) noexcept -> opus_int16 { return saturate_int16_from_int32(rounded_rshift<Shift>(value));
}
template <int Shift> [[nodiscard]] static constexpr auto rounded_i16_product_shift(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 { return static_cast<opus_int32>(rounded_rshift<Shift>(static_cast<opus_int64>(static_cast<opus_int16>(lhs)) * static_cast<opus_int64>(static_cast<opus_int16>(rhs))));
}
template <int Shift> [[nodiscard]] static constexpr auto rounded_mul_i16_q16(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 { return rounded_rshift<Shift>(static_cast<opus_int32>((static_cast<opus_int64>(lhs) * static_cast<opus_int64>(static_cast<opus_int16>(rhs))) >> 16));
}
template <int Shift> [[nodiscard]] static constexpr auto saturating_left_shift(opus_int32 value) noexcept -> opus_int32 { constexpr auto min_input = opus_int32_min >> Shift; constexpr auto max_input = opus_int32_max >> Shift; if (value < min_input) { return opus_int32_min; } if (value > max_input) { return opus_int32_max; } return static_cast<opus_int32>(static_cast<opus_int64>(value) * (static_cast<opus_int64>(1) << Shift));
}
[[nodiscard]] static constexpr auto saturating_left_shift(opus_int32 value, int shift) noexcept -> opus_int32 {
  if (shift <= 0) { return value;
}
  const auto min_input = opus_int32_min >> shift;
  const auto max_input = opus_int32_max >> shift;
  if (value < min_input) { return opus_int32_min;
}
  if (value > max_input) { return opus_int32_max;
}
  return static_cast<opus_int32>(static_cast<opus_int64>(value) * (static_cast<opus_int64>(1) << shift));
}
[[nodiscard]] static constexpr auto saturating_add_int32(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  const auto sum = static_cast<opus_uint32>(lhs) + static_cast<opus_uint32>(rhs);
  if ((sum & 0x80000000U) == 0) { return ((static_cast<opus_uint32>(lhs) & static_cast<opus_uint32>(rhs) & 0x80000000U) != 0) ? opus_int32_min : static_cast<opus_int32>(sum);
}
  return (((static_cast<opus_uint32>(lhs) | static_cast<opus_uint32>(rhs)) & 0x80000000U) == 0) ? opus_int32_max : static_cast<opus_int32>(sum);
}
[[nodiscard]] static constexpr auto saturating_subtract_int32(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  const auto difference = static_cast<opus_uint32>(lhs) - static_cast<opus_uint32>(rhs);
  if ((difference & 0x80000000U) == 0) { return ((static_cast<opus_uint32>(lhs) & (static_cast<opus_uint32>(rhs) ^ 0x80000000U) & 0x80000000U) != 0) ? opus_int32_min : static_cast<opus_int32>(difference);
}
  return (((static_cast<opus_uint32>(lhs) ^ 0x80000000U) & static_cast<opus_uint32>(rhs) & 0x80000000U) != 0) ? opus_int32_max : static_cast<opus_int32>(difference);
}
[[nodiscard]] static constexpr auto clamped_midpoint(opus_int32 lhs, opus_int32 rhs, opus_int32 bound0, opus_int32 bound1) noexcept -> opus_int32 { return clamp_value(rounded_rshift<1>(lhs + rhs), std::min(bound0, bound1), std::max(bound0, bound1));
}
[[nodiscard]] static constexpr auto inverse_prediction_step(opus_int32 lhs, opus_int32 rhs, opus_int32 rc_q31, opus_int32 rc_mult2, int mult2_q) noexcept -> opus_int64 {
  const auto reflected = saturating_subtract_int32(lhs, static_cast<opus_int32>(rounded_rshift<31>(static_cast<opus_int64>(rhs) * rc_q31)));
  return rounded_rshift(static_cast<opus_int64>(reflected) * rc_mult2, mult2_q);
}
[[nodiscard]] static constexpr auto delayed_pulse_from_q10(opus_int32 value_q10) noexcept -> opus_int8 { return static_cast<opus_int8>(rounded_rshift<10>(value_q10));
}
[[nodiscard]] static constexpr auto silk_next_rand_seed(const opus_int32 seed) noexcept -> opus_int32 {
  return static_cast<opus_int32>(static_cast<opus_uint32>(907633515U) + static_cast<opus_uint32>(seed) * static_cast<opus_uint32>(196314165U));
}
[[nodiscard]] static auto silk_lpc_prediction_q10(const opus_int32 *history_end, const std::span<const opus_int16> coefficients) noexcept -> opus_int32 {
  auto prediction_q10 = static_cast<opus_int32>(coefficients.size() >> 1);
  for (std::size_t tap = 0; tap < coefficients.size(); ++tap) {
    prediction_q10 = static_cast<opus_int32>(prediction_q10 + (((history_end[-static_cast<std::ptrdiff_t>(tap) - 1]) * static_cast<opus_int64>(static_cast<opus_int16>(coefficients[tap]))) >> 16));
}
  return prediction_q10;
}
[[nodiscard]] static OPUS_ALWAYS_INLINE auto silk_ltp_prediction_5tap(const opus_int32 *pred_lag_ptr, const std::span<const opus_int16, 5> coefficients) noexcept -> opus_int32 {
  auto prediction = opus_int32{2};
  for (int tap = 0; tap < 5; ++tap) {
    prediction = static_cast<opus_int32>(prediction + (((pred_lag_ptr[-tap]) * static_cast<opus_int64>(static_cast<opus_int16>(coefficients[static_cast<std::size_t>(tap)]))) >> 16));
}
  return prediction;
}
[[nodiscard]] static constexpr auto silk_harmonic_shaping(const opus_int32 *shp_lag_ptr, opus_int32 HarmShapeFIRPacked_Q14) noexcept -> opus_int32 {
  return saturating_add_int32(static_cast<opus_int32>((static_cast<opus_int64>(saturating_add_int32(shp_lag_ptr[0], shp_lag_ptr[-2])) * static_cast<opus_int16>(HarmShapeFIRPacked_Q14)) >> 16), static_cast<opus_int32>((static_cast<opus_int64>(shp_lag_ptr[-1]) * static_cast<opus_int16>(HarmShapeFIRPacked_Q14 >> 16)) >> 16));
}
[[nodiscard]] constexpr auto silk_nsq_build_sample(opus_int32 q_Q10, opus_int32 seed, opus_int32 LTP_pred_Q14, opus_int32 LPC_pred_Q14, opus_int32 x_Q10, opus_int32 n_AR_Q14, opus_int32 n_LF_Q14) noexcept -> silk_nsq_sample_state {
  auto exc_Q14 = static_cast<opus_int32>(static_cast<opus_uint32>(q_Q10) << 4);
  if (seed < 0) { exc_Q14 = -exc_Q14;
}
  const auto LPC_exc_Q14 = static_cast<opus_int32>(exc_Q14 + LTP_pred_Q14), xq_Q14 = static_cast<opus_int32>(LPC_exc_Q14 + LPC_pred_Q14), Diff_Q14 = static_cast<opus_int32>(xq_Q14 - static_cast<opus_int32>(static_cast<opus_uint32>(x_Q10) << 4));
  return {q_Q10, 0, xq_Q14, static_cast<opus_int32>(Diff_Q14 - n_AR_Q14), Diff_Q14, saturating_subtract_int32(static_cast<opus_int32>(Diff_Q14 - n_AR_Q14), n_LF_Q14), LPC_exc_Q14};
}
template <typename StateSpan> [[nodiscard]] static auto silk_best_delayed_state_index(const StateSpan states) noexcept -> int {
  int best = 0;
  for (int i = 1; i < static_cast<int>(states.size()); ++i) {
    if (states[static_cast<std::size_t>(i)].RD_Q10 < states[static_cast<std::size_t>(best)].RD_Q10) best = i;
  }
  return best;
}
static void silk_nsq_del_dec_scale_states(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, std::span<NSQ_del_dec_struct> psDelDec, std::span<const opus_int16> x16, std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP, std::span<opus_int32> sLTP_Q15, int subfr, const int LTP_scale_Q14, std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL, const int signal_type, const int decisionDelay); static void silk_noise_shape_quantizer_del_dec(silk_nsq_state *NSQ, std::span<NSQ_del_dec_struct> psDelDec, int signalType, std::span<const opus_int32> x_Q10, std::span<opus_int8> pulses, std::span<opus_int16> xq, std::span<opus_int32> sLTP_Q15, std::span<opus_int32> delayedGain_Q10, std::span<const opus_int16> a_Q12, std::span<const opus_int16> b_Q14, std::span<const opus_int16> AR_shp_Q13, int lag, opus_int32 HarmShapeFIRPacked_Q14, int Tilt_Q14, opus_int32 LF_shp_Q14, opus_int32 Gain_Q16, int Lambda_Q10, int offset_Q10, int subfr, int warping_Q16, int *smpl_buf_idx, int decisionDelay);
void silk_NSQ_del_dec_c(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, SideInfoIndices *psIndices, const opus_int16 x16[], opus_int8 pulses[], const opus_int16 *PredCoef_Q12, const opus_int16 LTPCoef_Q14[5 * 4], const opus_int16 AR_Q13[4 * 24], const int HarmShapeGain_Q14[4], const int Tilt_Q14[4], const opus_int32 LF_shp_Q14[4], const opus_int32 Gains_Q16[4], const int pitchL[4], const int Lambda_Q10, const int LTP_scale_Q14) {
  int i, k, lag, start_idx, LSF_interpolation_flag, Winner_ind, subfr, last_smple_idx, smpl_buf_idx, decisionDelay, offset_Q10;
  const opus_int16 *A_Q12, *B_Q14, *AR_shp_Q13; opus_int16 *pxq; opus_int32 HarmShapeFIRPacked_Q14;
  opus_int32 Gain_Q10; NSQ_del_dec_struct *psDD;
  lag = NSQ->lagPrev; opus_assume(NSQ->prev_gain_Q16 != 0);
  opus_assume(psEncC->nStatesDelayedDecision <= silk_max_delayed_decision_states); opus_assume(psEncC->ltp_mem_length + psEncC->frame_length <= silk_max_ltp_buffer_length); opus_assume(psEncC->subfr_length <= silk_max_subfr_length);
  std::array<NSQ_del_dec_struct, silk_max_delayed_decision_states> psDelDec_storage{}; auto delayed_states = std::span{psDelDec_storage}.first(static_cast<std::size_t>(psEncC->nStatesDelayedDecision)); auto *psDelDec = delayed_states.data();
  for (k = 0; k < psEncC->nStatesDelayedDecision; k++) { psDD = &psDelDec[k]; psDD->Seed = (k+psIndices->Seed)&3; psDD->SeedInit = psDD->Seed; psDD->RD_Q10 = 0; psDD->LF_AR_Q14 = NSQ->sLF_AR_shp_Q14; psDD->Diff_Q14 = NSQ->sDiff_shp_Q14; psDD->Shape_Q14[0] = NSQ->sLTP_shp_Q14[psEncC->ltp_mem_length-1]; copy_n_bytes(NSQ->sLPC_Q14, static_cast<std::size_t>(16*sizeof(opus_int32)), psDD->sLPC_Q14); copy_n_bytes(NSQ->sAR2_Q14, static_cast<std::size_t>(sizeof(NSQ->sAR2_Q14)), psDD->sAR2_Q14);
}
  offset_Q10 = silk_Quantization_Offsets_Q10[psIndices->signalType >> 1][psIndices->quantOffsetType]; smpl_buf_idx = 0;
  decisionDelay = std::min(40, psEncC->subfr_length);
  if (psIndices->signalType == 2) {
    for (k = 0; k < psEncC->nb_subfr; k++) { decisionDelay = std::min(decisionDelay, pitchL[k] - 5 / 2 - 1);
}
  } else {
    if (lag > 0) { decisionDelay = std::min(decisionDelay, lag - 5 / 2 - 1); }
}
  LSF_interpolation_flag = psIndices->NLSFInterpCoef_Q2 != 4;
  std::array<opus_int32, silk_max_ltp_buffer_length> sLTP_Q15_storage; std::array<opus_int16, silk_max_ltp_buffer_length> sLTP_storage; auto *sLTP = sLTP_storage.data(); std::array<opus_int32, silk_max_subfr_length> x_sc_Q10_storage;
  std::array<opus_int32, 40> delayedGain_Q10; pxq = &NSQ->xq[psEncC->ltp_mem_length];
  NSQ->sLTP_shp_buf_idx = psEncC->ltp_mem_length; NSQ->sLTP_buf_idx = psEncC->ltp_mem_length; subfr = 0;
  for (k = 0; k < psEncC->nb_subfr; k++) {
    A_Q12 = &PredCoef_Q12[((k >> 1) | (1 - LSF_interpolation_flag)) * 16]; B_Q14 = &LTPCoef_Q14[k * 5];
    AR_shp_Q13 = &AR_Q13[k * 24]; opus_assume(HarmShapeGain_Q14[k] >= 0); HarmShapeFIRPacked_Q14 = silk_pack_harm_shape_gain(HarmShapeGain_Q14[k]); NSQ->rewhite_flag = 0;
    if (psIndices->signalType == 2) {
      lag = pitchL[k];
      if ((k & (3 - ((opus_int32)((opus_uint32)(LSF_interpolation_flag) << (1))))) == 0) {
        if (k == 2) {
          Winner_ind = silk_best_delayed_state_index(delayed_states);
          for (i = 0; i < psEncC->nStatesDelayedDecision; i++) { if (i != Winner_ind) { psDelDec[i].RD_Q10 += (0x7FFFFFFF>>4); opus_assume(psDelDec[i].RD_Q10 >= 0); }
}
          psDD = &psDelDec[Winner_ind]; last_smple_idx = smpl_buf_idx + decisionDelay;
          for (i = 0; i < decisionDelay; i++) { last_smple_idx = (last_smple_idx - 1) % 40; if (last_smple_idx < 0) last_smple_idx += 40; pulses[i - decisionDelay] = delayed_pulse_from_q10(psDD->Q_Q10[last_smple_idx]); pxq[i - decisionDelay] = scale_and_saturate_q14<14>(psDD->Xq_Q14[last_smple_idx], Gains_Q16[1]); NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - decisionDelay + i] = psDD->Shape_Q14[last_smple_idx];
}
          subfr = 0;
}
        start_idx = psEncC->ltp_mem_length - lag - psEncC->predictLPCOrder - 5 / 2; opus_assume(start_idx > 0);
        silk_LPC_analysis_filter(&sLTP[start_idx], &NSQ->xq[start_idx + k * psEncC->subfr_length], A_Q12, psEncC->ltp_mem_length - start_idx, psEncC->predictLPCOrder);
        NSQ->sLTP_buf_idx = psEncC->ltp_mem_length; NSQ->rewhite_flag = 1; }
}
    silk_nsq_del_dec_scale_states(psEncC, NSQ, delayed_states, {x16, static_cast<std::size_t>(psEncC->subfr_length)}, x_sc_Q10_storage, sLTP_storage, sLTP_Q15_storage, k, LTP_scale_Q14, {Gains_Q16, 4}, {pitchL, 4}, psIndices->signalType, decisionDelay);
    const auto delayed_output_prefix = subfr > 0 ? decisionDelay : 0;
    silk_noise_shape_quantizer_del_dec(NSQ, delayed_states, psIndices->signalType, x_sc_Q10_storage, {pulses - delayed_output_prefix, static_cast<std::size_t>(psEncC->subfr_length + delayed_output_prefix)}, {pxq - delayed_output_prefix, static_cast<std::size_t>(psEncC->subfr_length + delayed_output_prefix)}, sLTP_Q15_storage, delayedGain_Q10, {A_Q12, static_cast<std::size_t>(psEncC->predictLPCOrder)}, {B_Q14, 5}, {AR_shp_Q13, static_cast<std::size_t>(psEncC->shapingLPCOrder)}, lag, HarmShapeFIRPacked_Q14, Tilt_Q14[k], LF_shp_Q14[k], Gains_Q16[k], Lambda_Q10, offset_Q10, subfr++, psEncC->warping_Q16, &smpl_buf_idx, decisionDelay);
    x16 += psEncC->subfr_length; pulses += psEncC->subfr_length; pxq += psEncC->subfr_length;
}
  Winner_ind = silk_best_delayed_state_index(delayed_states);
  psDD = &psDelDec[Winner_ind]; psIndices->Seed = psDD->SeedInit;
  last_smple_idx = smpl_buf_idx + decisionDelay; Gain_Q10 = ((Gains_Q16[psEncC->nb_subfr - 1]) >> (6));
  for (i = 0; i < decisionDelay; i++) { last_smple_idx = (last_smple_idx-1)%40; if (last_smple_idx<0) last_smple_idx += 40; pulses[i-decisionDelay] = delayed_pulse_from_q10(psDD->Q_Q10[last_smple_idx]); pxq[i-decisionDelay] = scale_and_saturate_q14<8>(psDD->Xq_Q14[last_smple_idx], Gain_Q10); NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx-decisionDelay+i] = psDD->Shape_Q14[last_smple_idx];
}
  copy_n_bytes(&psDD->sLPC_Q14[psEncC->subfr_length], static_cast<std::size_t>(16 * sizeof(opus_int32)), NSQ->sLPC_Q14);
  copy_n_bytes(psDD->sAR2_Q14, static_cast<std::size_t>(sizeof(psDD->sAR2_Q14)), NSQ->sAR2_Q14);
  NSQ->sLF_AR_shp_Q14 = psDD->LF_AR_Q14; NSQ->sDiff_shp_Q14 = psDD->Diff_Q14; NSQ->lagPrev = pitchL[psEncC->nb_subfr - 1]; silk_finish_nsq(psEncC, NSQ);
}
static void silk_noise_shape_quantizer_del_dec(silk_nsq_state *NSQ, std::span<NSQ_del_dec_struct> psDelDec, int signalType, std::span<const opus_int32> x_Q10, std::span<opus_int8> pulses, std::span<opus_int16> xq, std::span<opus_int32> sLTP_Q15, std::span<opus_int32> delayedGain_Q10, std::span<const opus_int16> a_Q12, std::span<const opus_int16> b_Q14, std::span<const opus_int16> AR_shp_Q13, int lag, opus_int32 HarmShapeFIRPacked_Q14, int Tilt_Q14, opus_int32 LF_shp_Q14, opus_int32 Gain_Q16, int Lambda_Q10, int offset_Q10, int subfr, int warping_Q16, int *smpl_buf_idx, int decisionDelay) {
  int i, j, k, Winner_ind, RDmin_ind, RDmax_ind, last_smple_idx;
  opus_int32 Winner_rand_state, LTP_pred_Q14, LPC_pred_Q14, n_AR_Q14, n_LTP_Q14, n_LF_Q14, r_Q10, RDmin_Q10, RDmax_Q10, Gain_Q10, tmp1, tmp2, *pred_lag_ptr, *shp_lag_ptr, *psLPC_Q14; NSQ_del_dec_struct *psDD;
  NSQ_sample_struct *psSS; const auto length = static_cast<int>(x_Q10.size()); const auto shapingLPCOrder = static_cast<int>(AR_shp_Q13.size()); const auto nStatesDelayedDecision = static_cast<int>(psDelDec.size()); opus_assume(nStatesDelayedDecision > 0); opus_assume(nStatesDelayedDecision <= silk_max_delayed_decision_states);
  std::array<NSQ_sample_pair, silk_max_delayed_decision_states> psSampleState;
  const auto b_q14_coefficients = b_Q14.first<5>();
  shp_lag_ptr = &NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - lag + 3 / 2];
  pred_lag_ptr = &sLTP_Q15[NSQ->sLTP_buf_idx - lag + 5 / 2]; Gain_Q10 = ((Gain_Q16) >> (6));
  for (i = 0; i < length; i++) {
    if (signalType == 2) { LTP_pred_Q14 = static_cast<opus_int32>(static_cast<opus_uint32>(silk_ltp_prediction_5tap(pred_lag_ptr, b_q14_coefficients)) << 1); ++pred_lag_ptr; } else { LTP_pred_Q14 = 0;
}
    if (lag > 0) { n_LTP_Q14 = saturating_subtract_int32(LTP_pred_Q14, saturating_left_shift<2>(silk_harmonic_shaping(shp_lag_ptr, HarmShapeFIRPacked_Q14))); ++shp_lag_ptr; } else { n_LTP_Q14 = 0;
}
    for (k = 0; k < nStatesDelayedDecision; k++) {
      psDD = &psDelDec[k]; psSS = psSampleState[k].data();
      psDD->Seed = silk_next_rand_seed(psDD->Seed);
      psLPC_Q14 = &psDD->sLPC_Q14[16 - 1 + i];
      LPC_pred_Q14 = saturating_left_shift<4>(silk_lpc_prediction_q10(psLPC_Q14 + 1, a_Q12)); opus_assume((shapingLPCOrder & 1) == 0);
      tmp2 = ((opus_int32)((psDD->Diff_Q14) + (((psDD->sAR2_Q14[0]) * (opus_int64)((opus_int16)(warping_Q16))) >> 16)));
      tmp1 = ((opus_int32)((psDD->sAR2_Q14[0]) + (((((opus_int32)((opus_uint32)(psDD->sAR2_Q14[1]) - (opus_uint32)(tmp2)))) * (opus_int64)((opus_int16)(warping_Q16))) >> 16)));
      psDD->sAR2_Q14[0] = tmp2; n_AR_Q14 = ((shapingLPCOrder) >> (1));
      n_AR_Q14 = ((opus_int32)((n_AR_Q14) + (((tmp2) * (opus_int64)((opus_int16)(AR_shp_Q13[0]))) >> 16)));
      for (j = 2; j < shapingLPCOrder; j += 2) { tmp2 = ((opus_int32)((psDD->sAR2_Q14[j - 1]) + (((((opus_int32)((opus_uint32)(psDD->sAR2_Q14[j + 0]) - (opus_uint32)(tmp1)))) * (opus_int64)((opus_int16)(warping_Q16))) >> 16))); psDD->sAR2_Q14[j - 1] = tmp1; n_AR_Q14 = ((opus_int32)((n_AR_Q14) + (((tmp1) * (opus_int64)((opus_int16)(AR_shp_Q13[j - 1]))) >> 16))); tmp1 = ((opus_int32)((psDD->sAR2_Q14[j + 0]) + (((((opus_int32)((opus_uint32)(psDD->sAR2_Q14[j + 1]) - (opus_uint32)(tmp2)))) * (opus_int64)((opus_int16)(warping_Q16))) >> 16))); psDD->sAR2_Q14[j + 0] = tmp2; n_AR_Q14 = ((opus_int32)((n_AR_Q14) + (((tmp2) * (opus_int64)((opus_int16)(AR_shp_Q13[j]))) >> 16)));
}
      psDD->sAR2_Q14[shapingLPCOrder - 1] = tmp1;
      n_AR_Q14 = ((opus_int32)((n_AR_Q14) + (((tmp1) * (opus_int64)((opus_int16)(AR_shp_Q13[shapingLPCOrder - 1]))) >> 16)));
      n_AR_Q14 = ((opus_int32)((opus_uint32)(n_AR_Q14) << (1)));
      n_AR_Q14 = ((opus_int32)((n_AR_Q14) + (((psDD->LF_AR_Q14) * (opus_int64)((opus_int16)(Tilt_Q14))) >> 16)));
      n_AR_Q14 = ((opus_int32)((opus_uint32)(n_AR_Q14) << (2)));
      n_LF_Q14 = ((opus_int32)(((psDD->Shape_Q14[*smpl_buf_idx]) * (opus_int64)((opus_int16)(LF_shp_Q14))) >> 16));
      n_LF_Q14 = ((opus_int32)((n_LF_Q14) + (((psDD->LF_AR_Q14) * ((opus_int64)(LF_shp_Q14) >> 16)) >> 16)));
      n_LF_Q14 = ((opus_int32)((opus_uint32)(n_LF_Q14) << (2)));
      tmp1 = ((((opus_uint32)(n_AR_Q14) + (opus_uint32)(n_LF_Q14)) & 0x80000000) == 0 ? ((((n_AR_Q14) & (n_LF_Q14)) & 0x80000000) != 0 ? ((opus_int32)0x80000000) : (n_AR_Q14) + (n_LF_Q14)) : ((((n_AR_Q14) | (n_LF_Q14)) & 0x80000000) == 0 ? 0x7FFFFFFF : (n_AR_Q14) + (n_LF_Q14)));
      tmp2 = ((opus_int32)((opus_uint32)(n_LTP_Q14) + (opus_uint32)(LPC_pred_Q14)));
      tmp1 = ((((opus_uint32)(tmp2) - (opus_uint32)(tmp1)) & 0x80000000) == 0 ? (((tmp2) & ((tmp1) ^ 0x80000000) & 0x80000000) ? ((opus_int32)0x80000000) : (tmp2) - (tmp1)) : ((((tmp2) ^ 0x80000000) & (tmp1) & 0x80000000) ? 0x7FFFFFFF : (tmp2) - (tmp1)));
      tmp1 = rounded_rshift<4>(tmp1); r_Q10 = silk_signed_clamped_residual(x_Q10[i] - tmp1, psDD->Seed);
      const auto candidates = silk_quantize_candidates(r_Q10, Lambda_Q10, offset_Q10);
      const auto candidate0 = static_cast<opus_int32>(candidates.rd_Q20[0] >> 10);
      const auto candidate1 = static_cast<opus_int32>(candidates.rd_Q20[1] >> 10);
      const auto first_is_q0 = candidate0 < candidate1;
      psSS[0] = silk_nsq_build_sample(candidates.q_Q10[first_is_q0 ? 0 : 1], psDD->Seed, LTP_pred_Q14, LPC_pred_Q14, x_Q10[i], n_AR_Q14, n_LF_Q14);
      psSS[1] = silk_nsq_build_sample(candidates.q_Q10[first_is_q0 ? 1 : 0], psDD->Seed, LTP_pred_Q14, LPC_pred_Q14, x_Q10[i], n_AR_Q14, n_LF_Q14);
      psSS[0].RD_Q10 = psDD->RD_Q10 + (first_is_q0 ? candidate0 : candidate1);
      psSS[1].RD_Q10 = psDD->RD_Q10 + (first_is_q0 ? candidate1 : candidate0);
}
    *smpl_buf_idx = (*smpl_buf_idx - 1) % 40;
    if (*smpl_buf_idx < 0) *smpl_buf_idx += 40;
    last_smple_idx = (*smpl_buf_idx + decisionDelay) % 40; RDmin_Q10 = psSampleState[0][0].RD_Q10; Winner_ind = 0;
    for (k = 1; k < nStatesDelayedDecision; k++) { if (psSampleState[k][0].RD_Q10 < RDmin_Q10) { RDmin_Q10 = psSampleState[k][0].RD_Q10; Winner_ind = k; }
}
    Winner_rand_state = psDelDec[Winner_ind].RandState[last_smple_idx];
    for (k = 0; k < nStatesDelayedDecision; k++) { if (psDelDec[k].RandState[last_smple_idx] != Winner_rand_state) { psSampleState[k][0].RD_Q10 = ((psSampleState[k][0].RD_Q10) + (0x7FFFFFFF >> 4)); psSampleState[k][1].RD_Q10 = ((psSampleState[k][1].RD_Q10) + (0x7FFFFFFF >> 4)); opus_assume(psSampleState[k][0].RD_Q10 >= 0); }
}
    RDmax_Q10 = psSampleState[0][0].RD_Q10; RDmin_Q10 = psSampleState[0][1].RD_Q10; RDmax_ind = 0; RDmin_ind = 0;
    for (k = 1; k < nStatesDelayedDecision; k++) { if (psSampleState[k][0].RD_Q10 > RDmax_Q10) { RDmax_Q10 = psSampleState[k][0].RD_Q10; RDmax_ind = k; } if (psSampleState[k][1].RD_Q10 < RDmin_Q10) { RDmin_Q10 = psSampleState[k][1].RD_Q10; RDmin_ind = k; }
}
    if (RDmin_Q10 < RDmax_Q10) { copy_n_bytes(((opus_int32 *)&psDelDec[RDmin_ind]) + i, static_cast<std::size_t>(sizeof(NSQ_del_dec_struct) - i * sizeof(opus_int32)), ((opus_int32 *)&psDelDec[RDmax_ind]) + i); copy_n_bytes(&psSampleState[RDmin_ind][1], static_cast<std::size_t>(sizeof(NSQ_sample_struct)), &psSampleState[RDmax_ind][0]);
}
    psDD = &psDelDec[Winner_ind];
    if (subfr > 0 || i >= decisionDelay) { const auto output_index = static_cast<std::size_t>(subfr > 0 ? i : i - decisionDelay); pulses[output_index] = delayed_pulse_from_q10(psDD->Q_Q10[last_smple_idx]); xq[output_index] = scale_and_saturate_q14<8>(psDD->Xq_Q14[last_smple_idx], delayedGain_Q10[last_smple_idx]); NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - decisionDelay] = psDD->Shape_Q14[last_smple_idx]; sLTP_Q15[NSQ->sLTP_buf_idx - decisionDelay] = psDD->Pred_Q15[last_smple_idx];
}
    NSQ->sLTP_shp_buf_idx++; NSQ->sLTP_buf_idx++;
    for (k = 0; k < nStatesDelayedDecision; k++) { psDD = &psDelDec[k]; psSS = &psSampleState[k][0]; psDD->LF_AR_Q14 = psSS->LF_AR_Q14; psDD->Diff_Q14 = psSS->Diff_Q14; psDD->sLPC_Q14[16 + i] = psSS->xq_Q14; psDD->Xq_Q14[*smpl_buf_idx] = psSS->xq_Q14; psDD->Q_Q10[*smpl_buf_idx] = psSS->Q_Q10; psDD->Pred_Q15[*smpl_buf_idx] = ((opus_int32)((opus_uint32)(psSS->LPC_exc_Q14) << (1))); psDD->Shape_Q14[*smpl_buf_idx] = psSS->sLTP_shp_Q14; psDD->Seed = static_cast<opus_int32>(static_cast<opus_uint32>(psDD->Seed) + static_cast<opus_uint32>(rounded_rshift<10>(psSS->Q_Q10))); psDD->RandState[*smpl_buf_idx] = psDD->Seed; psDD->RD_Q10 = psSS->RD_Q10;
}
    delayedGain_Q10[*smpl_buf_idx] = Gain_Q10;
}
  for (k = 0; k < nStatesDelayedDecision; k++) { psDD = &psDelDec[k]; copy_n_bytes(&psDD->sLPC_Q14[length], static_cast<std::size_t>(16 * sizeof(opus_int32)), psDD->sLPC_Q14); }
}
static void silk_nsq_del_dec_scale_states(const silk_encoder_state *psEncC, silk_nsq_state *NSQ, std::span<NSQ_del_dec_struct> psDelDec, std::span<const opus_int16> x16, std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP, std::span<opus_int32> sLTP_Q15, int subfr, const int LTP_scale_Q14, std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL, const int signal_type, const int decisionDelay) {
  const auto gain_adj_Q16 = silk_nsq_scale_common(psEncC, NSQ, x16, x_sc_Q10, sLTP, sLTP_Q15, subfr, LTP_scale_Q14, Gains_Q16, pitchL, signal_type, NSQ->sLTP_buf_idx - decisionDelay);
  if (gain_adj_Q16 != (static_cast<opus_int32>(1) << 16)) { for (auto &state : psDelDec) { state.LF_AR_Q14 = static_cast<opus_int32>((static_cast<opus_int64>(gain_adj_Q16) * state.LF_AR_Q14) >> 16); state.Diff_Q14 = static_cast<opus_int32>((static_cast<opus_int64>(gain_adj_Q16) * state.Diff_Q14) >> 16); scale_q16_buffer(state.sLPC_Q14, 16, gain_adj_Q16); scale_q16_buffer(state.sAR2_Q14, 24, gain_adj_Q16); scale_q16_buffer(state.Pred_Q15, 40, gain_adj_Q16); scale_q16_buffer(state.Shape_Q14, 40, gain_adj_Q16); } }
}
constexpr std::array<opus_int16, 2> HARM_ATT_Q15 = numeric_blob_array<opus_int16, 2>(R"blob(7CCC7000)blob");
constexpr std::array<opus_int16, 2> PLC_RAND_ATTENUATE_V_Q15 = numeric_blob_array<opus_int16, 2>(R"blob(799A6666)blob");
constexpr std::array<opus_int16, 2> PLC_RAND_ATTENUATE_UV_Q15 = numeric_blob_array<opus_int16, 2>(R"blob(7EB87333)blob");
static void silk_PLC_update(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl);
static void silk_PLC_conceal(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, std::span<opus_int16> frame);
void silk_PLC_Reset(silk_decoder_state *psDec) {
  psDec->sPLC.pitchL_Q8 = ((opus_int32)((opus_uint32)(psDec->frame_length) << (8 - 1)));
  psDec->sPLC.prevGain_Q16[0] = ((opus_int32)((1) * ((opus_int64)1 << (16)) + 0.5));
  psDec->sPLC.prevGain_Q16[1] = ((opus_int32)((1) * ((opus_int64)1 << (16)) + 0.5)); psDec->sPLC.subfr_length = 20;
  psDec->sPLC.nb_subfr = 2;
}
void silk_PLC(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, std::span<opus_int16> frame, int lost) {
  if (psDec->fs_kHz != psDec->sPLC.fs_kHz) { silk_PLC_Reset(psDec); psDec->sPLC.fs_kHz = psDec->fs_kHz;
}
  if (lost) {
    silk_PLC_conceal(psDec, psDecCtrl, frame); psDec->lossCnt++;
  } else { silk_PLC_update(psDec, psDecCtrl); }
}
static void silk_PLC_update(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl) {
  opus_int32 LTP_Gain_Q14, temp_LTP_Gain_Q14;
  int i, j;
  silk_PLC_struct *psPLC; psPLC = &psDec->sPLC; psDec->prevSignalType = psDec->indices.signalType; LTP_Gain_Q14 = 0;
  if (psDec->indices.signalType == 2) {
    for (j = 0; j * psDec->subfr_length < psDecCtrl->pitchL[psDec->nb_subfr - 1]; j++) {
      if (j == psDec->nb_subfr) { break;
}
      temp_LTP_Gain_Q14 = 0;
      for (i = 0; i < 5; i++) { temp_LTP_Gain_Q14 += psDecCtrl->LTPCoef_Q14[(psDec->nb_subfr - 1 - j) * 5 + i];
}
      if (temp_LTP_Gain_Q14 > LTP_Gain_Q14) {
        LTP_Gain_Q14 = temp_LTP_Gain_Q14;
        copy_n_bytes(&psDecCtrl->LTPCoef_Q14[((opus_int32)((opus_int16)(psDec->nb_subfr - 1 - j)) * (opus_int32)((opus_int16)(5)))], static_cast<std::size_t>(5 * sizeof(opus_int16)), psPLC->LTPCoef_Q14);
        psPLC->pitchL_Q8 = ((opus_int32)((opus_uint32)(psDecCtrl->pitchL[psDec->nb_subfr - 1 - j]) << (8))); }
}
    zero_n_bytes(psPLC->LTPCoef_Q14, static_cast<std::size_t>(5 * sizeof(opus_int16))); psPLC->LTPCoef_Q14[5 / 2] = LTP_Gain_Q14;
    if (LTP_Gain_Q14 < 11469) {
      int scale_Q10;
      opus_int32 tmp = ((opus_int32)((opus_uint32)(11469) << (10)));
      scale_Q10 = ((opus_int32)((tmp) / ((((LTP_Gain_Q14) > (1)) ? (LTP_Gain_Q14) : (1)))));
      for (i = 0; i < 5; i++) { psPLC->LTPCoef_Q14[i] = ((((opus_int32)((opus_int16)(psPLC->LTPCoef_Q14[i])) * (opus_int32)((opus_int16)(scale_Q10)))) >> (10));
}
    } else if (LTP_Gain_Q14 > 15565) {
      int scale_Q14;
      opus_int32 tmp = ((opus_int32)((opus_uint32)(15565) << (14)));
      scale_Q14 = ((opus_int32)((tmp) / ((((LTP_Gain_Q14) > (1)) ? (LTP_Gain_Q14) : (1)))));
      for (i = 0; i < 5; i++) { psPLC->LTPCoef_Q14[i] = ((((opus_int32)((opus_int16)(psPLC->LTPCoef_Q14[i])) * (opus_int32)((opus_int16)(scale_Q14)))) >> (14)); }
}
  } else {
    psPLC->pitchL_Q8 = ((opus_int32)((opus_uint32)(((opus_int32)((opus_int16)(psDec->fs_kHz)) * (opus_int32)((opus_int16)(18)))) << (8)));
    zero_n_bytes(psPLC->LTPCoef_Q14, static_cast<std::size_t>(5 * sizeof(opus_int16)));
}
  copy_n_bytes(psDecCtrl->PredCoef_Q12[1], static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int16)), psPLC->prevLPC_Q12);
  psPLC->prevLTP_scale_Q14 = psDecCtrl->LTP_scale_Q14;
  copy_n_bytes(&psDecCtrl->Gains_Q16[psDec->nb_subfr - 2], static_cast<std::size_t>(2 * sizeof(opus_int32)), psPLC->prevGain_Q16);
  psPLC->subfr_length = psDec->subfr_length; psPLC->nb_subfr = psDec->nb_subfr;
}
static void silk_PLC_energy(opus_int32 *energy1, int *shift1, opus_int32 *energy2, int *shift2, std::span<const opus_int32> exc_Q14, std::span<const opus_int32> prevGain_Q10, int subfr_length, int nb_subfr) {
  int i, k;
  opus_assume(subfr_length <= silk_max_subfr_length); std::array<opus_int16, 2 * silk_max_subfr_length> exc_buf_storage{}; auto *exc_buf = exc_buf_storage.data();
  auto *exc_buf_ptr = exc_buf;
  for (k = 0; k < 2; k++) { for (i = 0; i < subfr_length; i++) { exc_buf_ptr[i] = scale_and_saturate_q14<8>(exc_Q14[i + (k + nb_subfr - 2) * subfr_length], prevGain_Q10[k]); } exc_buf_ptr += subfr_length;
}
  silk_sum_sqr_shift(energy1, shift1, exc_buf, subfr_length); silk_sum_sqr_shift(energy2, shift2, exc_buf + subfr_length, subfr_length);
}
static void silk_PLC_conceal(silk_decoder_state *psDec, silk_decoder_control *psDecCtrl, std::span<opus_int16> frame) {
  int i, j, k;
  int lag, idx, sLTP_buf_idx, shift1, shift2;
  opus_int32 rand_seed, harm_Gain_Q15, rand_Gain_Q15, inv_gain_Q30;
  opus_int32 energy1, energy2, *rand_ptr, *pred_lag_ptr; opus_int32 LPC_pred_Q10, LTP_pred_Q12;
  opus_int16 rand_scale_Q14; opus_int16 *B_Q14;
  opus_int32 *sLPC_Q14_ptr; std::array<opus_int16, 16> A_Q12{};
  silk_PLC_struct *psPLC = &psDec->sPLC; std::array<opus_int32, 2> prevGain_Q10{};
  opus_assume(psDec->ltp_mem_length <= silk_max_ltp_mem_length); opus_assume(psDec->ltp_mem_length + psDec->frame_length <= silk_max_ltp_buffer_length);
  std::array<opus_int32, silk_max_ltp_buffer_length> sLTP_Q14_storage{}; auto *sLTP_Q14 = sLTP_Q14_storage.data(); std::array<opus_int16, silk_max_ltp_mem_length> sLTP_storage{}; auto *sLTP = sLTP_storage.data();
  prevGain_Q10[0] = ((psPLC->prevGain_Q16[0]) >> (6)); prevGain_Q10[1] = ((psPLC->prevGain_Q16[1]) >> (6));
  if (psDec->first_frame_after_reset) { zero_n_items(psPLC->prevLPC_Q12, std::size(psPLC->prevLPC_Q12));
}
  silk_PLC_energy(&energy1, &shift1, &energy2, &shift2, {psDec->exc_Q14, static_cast<std::size_t>(psDec->subfr_length * psDec->nb_subfr)}, prevGain_Q10, psDec->subfr_length, psDec->nb_subfr);
  if (((energy1) >> (shift2)) < ((energy2) >> (shift1))) { rand_ptr = &psDec->exc_Q14[std::max(0, (psPLC->nb_subfr - 1) * psPLC->subfr_length - 128)]; } else { rand_ptr = &psDec->exc_Q14[std::max(0, psPLC->nb_subfr * psPLC->subfr_length - 128)];
}
  B_Q14 = psPLC->LTPCoef_Q14; rand_scale_Q14 = psPLC->randScale_Q14; harm_Gain_Q15 = HARM_ATT_Q15[std::min(2 - 1, psDec->lossCnt)];
  if (psDec->prevSignalType == 2) { rand_Gain_Q15 = PLC_RAND_ATTENUATE_V_Q15[std::min(2 - 1, psDec->lossCnt)]; } else { rand_Gain_Q15 = PLC_RAND_ATTENUATE_UV_Q15[std::min(2 - 1, psDec->lossCnt)];
}
  silk_bwexpander(std::span<opus_int16>{psPLC->prevLPC_Q12, static_cast<std::size_t>(psDec->LPC_order)}, ((opus_int32)((0.99) * ((opus_int64)1 << (16)) + 0.5)));
  copy_n_items(psPLC->prevLPC_Q12, static_cast<std::size_t>(psDec->LPC_order), A_Q12.data());
  if (psDec->lossCnt == 0) {
    rand_scale_Q14 = 1 << 14;
    if (psDec->prevSignalType == 2) { for (i = 0; i < 5; i++) { rand_scale_Q14 -= B_Q14[i]; } rand_scale_Q14 = std::max((opus_int16)3277, rand_scale_Q14); rand_scale_Q14 = (opus_int16)((((opus_int32)((opus_int16)(rand_scale_Q14)) * (opus_int32)((opus_int16)(psPLC->prevLTP_scale_Q14)))) >> (14)); } else { opus_int32 invGain_Q30, down_scale_Q30; invGain_Q30 = silk_LPC_inverse_pred_gain_c(psPLC->prevLPC_Q12, psDec->LPC_order); down_scale_Q30 = std::min((((opus_int32)1 << 30) >> (3)), invGain_Q30); down_scale_Q30 = std::max((((opus_int32)1 << 30) >> (8)), down_scale_Q30); down_scale_Q30 = ((opus_int32)((opus_uint32)(down_scale_Q30) << (3))); rand_Gain_Q15 = ((((opus_int32)(((down_scale_Q30) * (opus_int64)((opus_int16)(rand_Gain_Q15))) >> 16))) >> (14)); }
}
  rand_seed = psPLC->rand_seed; lag = rounded_rshift<8>(psPLC->pitchL_Q8);
  sLTP_buf_idx = psDec->ltp_mem_length; idx = psDec->ltp_mem_length - lag - psDec->LPC_order - 5 / 2;
  opus_assume(idx > 0);
  silk_LPC_analysis_filter(&sLTP[idx], &psDec->outBuf[idx], A_Q12.data(), psDec->ltp_mem_length - idx, psDec->LPC_order);
  inv_gain_Q30 = silk_INVERSE32_varQ(psPLC->prevGain_Q16[1], 46);
  inv_gain_Q30 = std::min(inv_gain_Q30, std::numeric_limits<opus_int32>::max() >> 1);
  for (i = idx + psDec->LPC_order; i < psDec->ltp_mem_length; i++) { sLTP_Q14[i] = ((opus_int32)(((inv_gain_Q30) * (opus_int64)((opus_int16)(sLTP[i]))) >> 16));
}
  for (k = 0; k < psDec->nb_subfr; k++) {
    const auto b_q14_coefficients = std::span<const opus_int16, 5>{B_Q14, 5};
    pred_lag_ptr = &sLTP_Q14[sLTP_buf_idx - lag + 5 / 2];
    for (i = 0; i < psDec->subfr_length; i++) {
      LTP_pred_Q12 = silk_ltp_prediction_5tap(pred_lag_ptr, b_q14_coefficients);
      ++pred_lag_ptr;
      rand_seed = silk_next_rand_seed(rand_seed);
      idx = ((rand_seed) >> (25)) & (128 - 1);
      sLTP_Q14[sLTP_buf_idx] = ((opus_int32)((opus_uint32)(((opus_int32)((LTP_pred_Q12) + (((rand_ptr[idx]) * (opus_int64)((opus_int16)(rand_scale_Q14))) >> 16)))) << (2)));
      ++sLTP_buf_idx;
}
    for (j = 0; j < 5; j++) { B_Q14[j] = ((((opus_int32)((opus_int16)(harm_Gain_Q15)) * (opus_int32)((opus_int16)(B_Q14[j])))) >> (15));
}
    rand_scale_Q14 = ((((opus_int32)((opus_int16)(rand_scale_Q14)) * (opus_int32)((opus_int16)(rand_Gain_Q15)))) >> (15));
    psPLC->pitchL_Q8 = ((opus_int32)((psPLC->pitchL_Q8) + (((psPLC->pitchL_Q8) * (opus_int64)((opus_int16)(655))) >> 16)));
    psPLC->pitchL_Q8 = std::min(psPLC->pitchL_Q8, ((opus_int32)((opus_uint32)(((opus_int32)((opus_int16)(18)) * (opus_int32)((opus_int16)(psDec->fs_kHz)))) << (8))));
    lag = rounded_rshift<8>(psPLC->pitchL_Q8);
}
  sLPC_Q14_ptr = &sLTP_Q14[psDec->ltp_mem_length - 16];
  copy_n_bytes(psDec->sLPC_Q14_buf, static_cast<std::size_t>(16 * sizeof(opus_int32)), sLPC_Q14_ptr);
  opus_assume(psDec->LPC_order >= 10);
  const auto coefficients = std::span<const opus_int16>{A_Q12.data(), static_cast<std::size_t>(psDec->LPC_order)};
  for (i = 0; i < psDec->frame_length; i++) {
    LPC_pred_Q10 = silk_lpc_prediction_q10(sLPC_Q14_ptr + 16 + i, coefficients);
    sLPC_Q14_ptr[16 + i] = saturating_add_int32(sLPC_Q14_ptr[16 + i], saturating_left_shift<4>(LPC_pred_Q10));
    frame[i] = scale_and_saturate_q14<8>(sLPC_Q14_ptr[16 + i], prevGain_Q10[1]);
  }
  copy_n_bytes(&sLPC_Q14_ptr[psDec->frame_length], static_cast<std::size_t>(16 * sizeof(opus_int32)), psDec->sLPC_Q14_buf);
  psPLC->rand_seed = rand_seed; psPLC->randScale_Q14 = rand_scale_Q14;
fill_n_items(psDecCtrl->pitchL, std::size(psDecCtrl->pitchL), lag);
}
void silk_PLC_glue_frames(silk_decoder_state *psDec, std::span<opus_int16> frame) {
  int i, energy_shift;
  opus_int32 energy; silk_PLC_struct *psPLC;
  const auto length = static_cast<int>(frame.size());
  psPLC = &psDec->sPLC;
  if (psDec->lossCnt) { silk_sum_sqr_shift(&psPLC->conc_energy, &psPLC->conc_energy_shift, frame.data(), length); psPLC->last_frame_lost = 1; } else {
    if (psDec->sPLC.last_frame_lost) {
      silk_sum_sqr_shift(&energy, &energy_shift, frame.data(), length);
      if (energy_shift > psPLC->conc_energy_shift) { psPLC->conc_energy = ((psPLC->conc_energy) >> (energy_shift - psPLC->conc_energy_shift)); } else if (energy_shift < psPLC->conc_energy_shift) { energy = ((energy) >> (psPLC->conc_energy_shift - energy_shift));
}
      if (energy > psPLC->conc_energy) { opus_int32 frac_Q24, LZ; opus_int32 gain_Q16, slope_Q16; LZ = silk_CLZ32(psPLC->conc_energy); LZ = LZ - 1; psPLC->conc_energy = ((opus_int32)((opus_uint32)(psPLC->conc_energy) << (LZ))); energy = ((energy) >> (std::max(24 - LZ, 0))); frac_Q24 = ((opus_int32)((psPLC->conc_energy) / ((((energy) > (1)) ? (energy) : (1))))); gain_Q16 = ((opus_int32)((opus_uint32)(silk_SQRT_APPROX(frac_Q24)) << (4))); slope_Q16 = ((opus_int32)((((opus_int32)1 << 16) - gain_Q16) / (length))); slope_Q16 = ((opus_int32)((opus_uint32)(slope_Q16) << (2))); for (i = 0; i < length; i++) { frame[i] = ((opus_int32)(((gain_Q16) * (opus_int64)((opus_int16)(frame[i]))) >> 16)); gain_Q16 += slope_Q16; if (gain_Q16 > (opus_int32)1 << 16) { break; } } }
}
    psPLC->last_frame_lost = 0; }
}
static auto encode_split(ec_enc *psRangeEnc, const int p_child1, const int p, std::span<const opus_uint8> shell_table) noexcept -> void {
  if (p > 0) { ec_enc_icdf(psRangeEnc, p_child1, shell_table.data() + silk_shell_code_table_offsets[static_cast<std::size_t>(p)], 8); }
}
static auto decode_split(opus_int16 &p_child1, opus_int16 &p_child2, ec_dec *psRangeDec, const int p, std::span<const opus_uint8> shell_table) noexcept -> void {
  if (p > 0) { p_child1 = ec_dec_icdf(psRangeDec, shell_table.data() + silk_shell_code_table_offsets[static_cast<std::size_t>(p)], 8); p_child2 = p - p_child1; } else { p_child1 = 0; p_child2 = 0; }
}
[[nodiscard]] static constexpr auto silk_shell_code_table(const std::size_t pulse_count) noexcept -> std::span<const opus_uint8> {
  opus_assume(pulse_count >= 2 && pulse_count <= 16);
  switch (pulse_count) { case 2: return silk_shell_code_table0; case 4: return silk_shell_code_table1; case 8: return silk_shell_code_table2; default: return silk_shell_code_table3; }
}
[[nodiscard]] static inline auto pulse_sum(std::span<const int> pulses) noexcept -> int {
  int sum = 0;
  for (const int pulse : pulses) { sum += pulse;
}
  return sum;
}
static void silk_shell_encode_node(ec_enc *psRangeEnc, std::span<const int> pulses) {
  if (pulses.size() <= 1) { return;
}
  const auto half = pulses.size() / 2;
  encode_split(psRangeEnc, pulse_sum(pulses.first(half)), pulse_sum(pulses), silk_shell_code_table(pulses.size()));
  silk_shell_encode_node(psRangeEnc, pulses.first(half)); silk_shell_encode_node(psRangeEnc, pulses.last(half));
}
static void silk_shell_decode_node(std::span<opus_int16> pulses, ec_dec *psRangeDec, const int total_pulses) {
  if (pulses.size() == 1) { pulses.front() = static_cast<opus_int16>(total_pulses); return;
}
  const auto half = pulses.size() / 2;
  opus_int16 left = 0, right = 0;
  decode_split(left, right, psRangeDec, total_pulses, silk_shell_code_table(pulses.size()));
  silk_shell_decode_node(pulses.first(half), psRangeDec, left); silk_shell_decode_node(pulses.last(half), psRangeDec, right);
}
void silk_shell_encoder(ec_enc *psRangeEnc, std::span<const int, 16> pulses0) { silk_shell_encode_node(psRangeEnc, std::span<const int>{pulses0});
}
void silk_shell_decoder(std::span<opus_int16, 16> pulses0, ec_dec *psRangeDec, const int pulses4) { silk_shell_decode_node(std::span<opus_int16>{pulses0}, psRangeDec, pulses4);
}
namespace {
constexpr std::array<std::array<opus_uint8, 64 / 8>, 3> silk_gain_iCDF = numeric_blob_matrix<opus_uint8, 3, 8>(R"blob(E0702C0F03020100FEEDC08446170400FFFCE29B3D0B0200)blob");
constexpr std::array<opus_uint8, 36 - -4 + 1> silk_delta_gain_iCDF = numeric_blob_array<opus_uint8, 41>(R"blob(FAF5EACB47322A2623211F1D1C1B1A191817161514131211100F0E0D0C0B0A09080706050403020100)blob");
constexpr std::array<opus_uint8, 3> silk_LTP_per_index_iCDF = numeric_blob_array<opus_uint8, 3>(R"blob(B36300)blob");
constexpr std::array<opus_uint8, 8> silk_LTP_gain_iCDF_0 = numeric_blob_array<opus_uint8, 8>(R"blob(47382B1E150C0600)blob");
constexpr std::array<opus_uint8, 16> silk_LTP_gain_iCDF_1 = numeric_blob_array<opus_uint8, 16>(R"blob(C7A5907C6D6054473D332A20170F0800)blob");
constexpr std::array<opus_uint8, 32> silk_LTP_gain_iCDF_2 = numeric_blob_array<opus_uint8, 32>(R"blob(F1E1D3C7BBAFA4998E847B7269605850484039322C26211D1814100C09050200)blob");
constexpr std::array<opus_uint8, 8> silk_LTP_gain_BITS_Q5_0 = numeric_blob_array<opus_uint8, 8>(R"blob(0F838A8A9B9BADAD)blob");
constexpr std::array<opus_uint8, 16> silk_LTP_gain_BITS_Q5_1 = numeric_blob_array<opus_uint8, 16>(R"blob(455D7376838A8D8A96969B969BA0A6A0)blob");
constexpr std::array<opus_uint8, 32> silk_LTP_gain_BITS_Q5_2 = numeric_blob_array<opus_uint8, 32>(R"blob(8380868D8D8D919191969B9B9B9BA0A0A0A0A6A6ADADB6C0B6C0C0C0CDC0CDE0)blob");
constexpr std::array<std::array<opus_int8, 5>, 8> silk_LTP_gain_vq_0 = numeric_blob_matrix<opus_int8, 8, 5>(R"blob(040618070500000200000C1C290DFCF70F2A190E01FE3E29F7F62541FC03FA044207F8100E26FD21)blob");
constexpr std::array<std::array<opus_int8, 5>, 16> silk_LTP_gain_vq_1 = numeric_blob_matrix<opus_int8, 16, 5>(R"blob(0D1627170CFF24401BFAF90A372B11010108010106F54A35F7F4374CF408FD035D1BFC1A273B03F802004D0B09F8162CFA0728091A0309F91465F90403F82A1A00F121440217FE372EFE0F03FF151029)blob");
constexpr std::array<std::array<opus_int8, 5>, 32> silk_LTP_gain_vq_2 = numeric_blob_matrix<opus_int8, 32, 5>(R"blob(FA1B3D2705F52A580401FE3C4106FCFFFB493801F7135E1DF7000C63060408ED662EF303020D030209EB5448EEF52E68EA081226301700F04653EB0B05F57516F8FA1775F40303F85F1C04F60F4D3CF1FF047C02FC03265418E7020D2A0D1F15FC382EFFFF234FF313F94158F7F214045131E314004B03EF05F72C5CF801FD16451FFA5F29F405274310FC0100FA7837DCF32C7A04E851050B03070200090A58)blob");
constexpr std::array<opus_uint8, 8> silk_LTP_gain_vq_0_gain = numeric_blob_array<opus_uint8, 8>(R"blob(2E025A575D5B5262)blob");
constexpr std::array<opus_uint8, 16> silk_LTP_gain_vq_1_gain = numeric_blob_array<opus_uint8, 16>(R"blob(6D78760C71737577633B576F3F6F7050)blob");
constexpr std::array<opus_uint8, 32> silk_LTP_gain_vq_2_gain = numeric_blob_array<opus_uint8, 32>(R"blob(7E7C7D7C81797E17847F7F7F7E7F7A858286657677917E567C787B77AAAD6B6D)blob");
constexpr std::array<silk_ltp_codebook_view, 3> silk_LTP_codebooks{{{silk_LTP_gain_iCDF_0, silk_LTP_gain_BITS_Q5_0, flat_matrix_view(silk_LTP_gain_vq_0), silk_LTP_gain_vq_0_gain, static_cast<int>(silk_LTP_gain_vq_0.size())},
                                                                     {silk_LTP_gain_iCDF_1, silk_LTP_gain_BITS_Q5_1, flat_matrix_view(silk_LTP_gain_vq_1), silk_LTP_gain_vq_1_gain, static_cast<int>(silk_LTP_gain_vq_1.size())},
                                                                     {silk_LTP_gain_iCDF_2, silk_LTP_gain_BITS_Q5_2, flat_matrix_view(silk_LTP_gain_vq_2), silk_LTP_gain_vq_2_gain, static_cast<int>(silk_LTP_gain_vq_2.size())}}};
constexpr std::array<opus_uint8, 320> silk_NLSF_CB1_NB_MB_Q8 = numeric_blob_array<opus_uint8, 320>(R"blob(0C233C536C849DB4CEE40F20374D657D97AFC9E1132A42597289A2B8D1E60C193248617893ACC8DF1A2C455A72879FB4CDE10D1635506A829CB4CDE40F192C405A738EA8C4DE13183E52647891A8BED6161F324F677897AACBE3151D2D416A7C96ABC4E01E314B61798EA5BAD1E5131934465D748FA6C0DB1A223E4B617691A7C2D9192138465B718FA5C4DF15223348617591ABC4DE141D32435A7590A8C5DD161F30425F7592A8C4DE1821334D74869EB4C8E0151C46576A7C95AAC2D91A213540537598ADCCE11B22415F6C819BAED2E1141A486371839AB0C8DB222B3D4E5D729BB1CDE5171D36617C8AA3B3D1E51E26385976819EB2C8E7151D313F556F8EA3C1DE1B304D67859EB3C4D7E81D2F4A637C97B0C6DCED212A3D4C5D799BAECFE11D355770889AAABCD0E3181E34548396A6BACBE52530405468769CB1C9E6)blob");
constexpr std::array<opus_int16, 320> silk_NLSF_CB1_Wght_Q9 = numeric_blob_array<opus_int16, 320>(
    R"blob(0B51090A090A090A08EF08EF090A08FC091708EF0B480A14095A093F090A08E208E208E208E2089209B709240924090A090A090A09240924093F09320C900ACE09240924090A08E208AD089F08D50892099C09AA093F095A095A095A095A093F0967090A0D970BF0084F089F08E208E208E208EF090A08D50CD20C450A14095A08C708AD089F08920892084210000F0508AD0A3C0A3C0967090A095A093F081A0C6A0CAC093F08AD09F909820924090A087708AD0D0A0DA00AA6089208D5099C0932093F089F0835093209740917093F095A097409740974099C093F0EC30E2D098209DF093F08E208E208FC089F08000CB60C990A990B1E098F091708FC08FC08E2084F0CBF0CE40AC10AF6098F08D508D508C7084F08350B390BA50A49093F09670932089208C708C708420C990C7D0A490A1408E2088508C708AD08AD085D0C6A0CEE0AB4096708E208E208E208EF089208420C450CC8099C080D08EF09C4093F09B7098208850DB30CD2090A0A8C0A5709AA093F095A0924084F0D5F0DCF0BDE0BF008FC079E08AD08E208E208E20D4C0D2608270A7F0B390932097408E209AA09EC0EB00DA0079E0A640B5109DF095A093F099C08D50BD40CC80AB40B480AB4086A084F08EF08BA08C70E6F0E4907E907B10A640A8C0A1409C40917093F0C870D550932081A0B480B48092409B708C708770D0A0D260B1E0ADC0917086A08E208EF0842080D091708FC088508770885093F0A490A8C0A8C09F90967098208AD08D508AD08AD092409740A2F0A8C0BDE0CAC0AF60B4809AA081A08FC090A0932094C08AD086A084F08EF09C40AE90AE90A3C0A14093F0E5C0E8108BA072E08850AC10AA60A7109D1089F0AE90C580AA609F90B1E09D10885095A08AD0885)blob");
constexpr std::array<opus_uint8, 64> silk_NLSF_CB1_iCDF_NB_MB = numeric_blob_array<opus_uint8, 64>(R"blob(D4B294816C6055524F4D3D3B39383331302D2A29282624221F1E150C0A030100FFF5F4ECE9E1D9CBBEB0AFA195887D72665B51473C342B231C1413120C0B0500)blob");
constexpr std::array<opus_uint8, 160> silk_NLSF_CB2_SELECT_NB_MB = numeric_blob_array<opus_uint8, 160>(R"blob(10000000006342242422242222222253452434227466464444B0664444224155445424748D988BAA84BBB8D88984F9A8B98B6866644444B2DAB9B9AAF4D8BBBBAAF4BBBBDB8A679BB8B98974B79B988884D9B8B8AAA4D9AB9B8BF4A9B8B9AAA4D8DFDA8AD68FBCDAA8F48D889BAAA88ADCDB8BA4DBCAD889A8BAF6B98B74B9DBB98A64648664662244446444A8CBDDDAA8A79A886846A4F6AB898B899BDADB8B)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_iCDF_NB_MB = numeric_blob_array<opus_uint8, 72>(R"blob(FFFEFDEE0E03020100FFFEFCDA2303020100FFFEFAD03B04020100FFFEF6C2470A020100FFFCECB75208020100FFFCEBB45A11020100FFF8E0AB611E040100FFFEECAD5F25070100)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_BITS_NB_MB_Q5 = numeric_blob_array<opus_uint8, 72>(R"blob(FFFFFF830691FFFFFFFFFFEC5D0F60FFFFFFFFFFC2531947DDFFFFFFFFA2492242A2FFFFFFD27E492B39ADFFFFFFC97D47303A82FFFFFFA66E49393E68D2FFFFFB7B41374464ABFF)blob");
constexpr std::array<opus_uint8, 18> silk_NLSF_PRED_NB_MB_Q8 = numeric_blob_array<opus_uint8, 18>(R"blob(B38A8C9497959997A37443523B5C4864595C)blob");
constexpr std::array<opus_int16, 11> silk_NLSF_DELTA_MIN_NB_MB_Q15 = numeric_blob_array<opus_int16, 11>(R"blob(00FA00030006000300030003000400030003000301CD)blob");
constinit const silk_NLSF_CB_struct silk_NLSF_CB_NB_MB = make_silk_nlsf_cb(32, 10, ((opus_int32)((0.18) * ((opus_int64)1 << (16)) + 0.5)), ((opus_int32)((1.0 / 0.18) * ((opus_int64)1 << (6)) + 0.5)), silk_NLSF_CB1_NB_MB_Q8, silk_NLSF_CB1_Wght_Q9, silk_NLSF_CB1_iCDF_NB_MB, silk_NLSF_PRED_NB_MB_Q8, silk_NLSF_CB2_SELECT_NB_MB, silk_NLSF_CB2_iCDF_NB_MB, silk_NLSF_CB2_BITS_NB_MB_Q5, silk_NLSF_DELTA_MIN_NB_MB_Q15);
constexpr std::array<opus_uint8, 512> silk_NLSF_CB1_WB_Q8 = numeric_blob_array<opus_uint8, 512>(
    R"blob(07172636455564748393A2B2C1D0DFEF0D192937455362707F8E9DABBBCBDCEC0F1522333D4E5C6A7E8898A7B9CDE1F00A1524323F4F5F6E7E8D9DADBDCDDDED111425333B4E596B7B8696A4B8CDE0F00A0F203343516070818E9EADBDCCDCEC08152533414F62717E8A9BA8B3C0D1DA0C0F22373F4E576C768394A7B9CBDBEC10132024384F5B6C76889AABBACCDCED0B1C2B3A4A5969788796A5B4C4D3E2F10610212E3C4B5C6B7B899CA9B9C7D6E10B131E2C394A5969798798A9BACADAEA0C131D2E39475864788494A5B6C7D8E91117232E384D5C6A7B8698A7B9CCDEED0E112D353F4B596B738497ABBCCEDDF009101D283847586777899AABBDCDDEED10132430394C5769768496A7B9CADAEC0C111D3647515E687E8895A4B6C9DDED0F1C2F3E4F6173818E9BA8B4C2D0DFEE080E1E2D3E4E5E6F7F8F9FAFC0CFDFEF111E313E4F5C6B778491A0AEBECCDCEB0E13242D3D4C5B6C798A9AACBDCDDEEE0C121F2D3C4C5B6B7B8A9AABBBCCDDEC0D111F2B35465367728395A7B9CBDCED1116232A3A4E5D6E7D8B9BAABCCEE0F0080F2232435363738392A2B2C1D1E0EF0D10294249565F6F808996A3B7CEE1F1111925343F4B5C66778490A0AFBFD4E7131F31415364758593A1AEBBC8D5E3F2121F34445867757E8A95A3B1C0CFDFEF101D2F3D4C5A6A778593A1B0C1D1E0F00F1523323D4956616E77818DAFC6DAED)blob");
constexpr std::array<opus_int16, 512> silk_NLSF_CB1_WB_Wght_Q9 = numeric_blob_array<opus_int16, 512>(
    R"blob(0E490B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B930B930B6D0B1E0C900C0D0B9C0BF00BF00BC20BC20BC20B930B930BC20B9C0B480B1E0B1E0AA60F500FAE0BA50C870C870B760BF00B1E0C320CAC0B6D0B1E0A3C09F90ADC0B6D0DBC0C7D0BC20C1F0BCB0B480B6D0B6D0B6D0B6D0B480B480B480B480B480AC113BE13BE0B760DF50D390BF00C0D0AE90C580C580B9C0B1E09D109EC0AC10B48114C10350A8C0AC10B9C0BC20B6D0B1E0BA50BCB0B6D0B6D0B6D0B6D0B480AA60E240BCB0B9C0BF00BF00B390AF60BF00C900BE70BA50CDB0CDB0BA50CEE0BAF146B139609EC0D0A0DC60D390C7D0C160D300BA50A8C0A570A7F0AE90B1E0A7113D914361207114C099C0B510BE70C870C610A7F0AB40B480B1E0AE90B1E0A8C0C320B480B930B6D0B6D0B6D0B6D0B930B930B930B930B6D0B6D0B930B930B93106A0C870BA50C1F0BC20B480B480B6D0B9C0B390B640BCB0B9C0BC20C7D0B390EB00EB00CAC0C1F0BA50B480B6D0B480B9C0B760AE90AE90B1E0B480B480A640F0E0FAE0C870C320CAC0B760BE70B930B930C0D0B1E0AE90AE90AE90AE90A140F050FF00D1D0DBC0C160AB40BC20B760C320C0D0B1E0B1E0A570A570B1E0AF6141B131E0C990F050D710C610B510D550D7B0A8C0A140A710AB40B1E0AF60AC1100D0ECD0CDB0C580B6D0B480B480B6D0AE90AB40AE90AB40AE90B1E0B480AF613D913BE0BE70DD90CAC0BF00C0D0B800C1F0B510AB40AB40AB40B1E0AE90A3C10D510D50B2C09DF0C870D300D300C030C030D300BF00B1E0A570A140AA60AC10BF00B640AF60B480AB40A7F0B510C1F0C4E0C4E0C900C610BF00BC20B930B1E11170F2A0B6D0B480B1E0B480B1E0B1E0B480B480B480B1E0B480B6D0B480B1E0BA50B640B640BA50BA50BF00C320C900C4E0BF00BC20B9C0B9C0B9C0B6D0AB4108510350CEE0D130B6D0B930B480BA50BA50B1E0AE90AB40B1E0B1E0B1E0AE90FF00FAE0C1F0BC20B6D0B6D0B6D0B480B6D0B6D0B1E0B1E0B1E0AE90B480ADC120711DF0C610D710C870BA50B510BDE0C320AB40A7F0A7F0A7F0AB40AE90A8C103510AD0ECD0E490AA60ADC0B480B480BC20B9C0B6D0B1E0A7F0A7F0AE90B4810770DE20AC10B1E0B1E0B480B480B480B6D0B6D0B480B6D0B6D0B6D0B930B481436133908D50D680ECD0D970D130B1E0CEE0D970C4E0B51099C09B70AC10B6D0D7B0E650C320C7D0D1D0BE70C870C870BA50C900C0D0B6D0B6D0A7F09EC09820BA50BC20AE90AE90AB40AE90B1E0B9C0BF00C1F0C4E0C4E0C4E0C1F0BC20BC20B800B390A7F0AA60ADC0BC20D680DD90D1D0CAC0BF00BC20B930B6D0B480B1E0BCB0B800B510BC20BC20B9C0BCB0C1F0BF00BF00BC20B480B1E0B6D0B6D0B480F500F7F0BC20C7D0D1D0C900CDB0CDB0D970E780D710AA60885099C0A140A2F)blob");
constexpr std::array<opus_uint8, 64> silk_NLSF_CB1_iCDF_WB = numeric_blob_array<opus_uint8, 64>(R"blob(E1CCC9B8B7AF9E9A99877773716E6D63625F4F443432302D2B201F1B120A0300FFFBEBE6D4C9C4B6A7A6A3978A7C6E685A4E4C4645392D2218150B0605040300)blob");
constexpr std::array<opus_uint8, 256> silk_NLSF_CB2_SELECT_WB = numeric_blob_array<opus_uint8, 256>(R"blob(00000000000000016466664444242260A46B9EB9B4B98B664042242222000120D08B8DBF98B99B6860AB68A6666666840100000000101000506D4E6BB98B6765D0D48D8BAD997B672400000000000001300000000000002044877B7777674562446778767666476286889DB8B6998B86D0A8F84BBD8F796B2031222222001102D2EB8B7BB9896986628768B664B7AB86644644464242228340A666442402010086A6664422224284D4F69E8B6B6B576664DB7D7A8976678472878969AB6A3222A4D68D8FB9977967C022000000000001D06D4ABB86F99F89666E9A7657657765000200242442442360A4666424000221A78AAE6664540202646B787724C51800)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_iCDF_WB = numeric_blob_array<opus_uint8, 72>(R"blob(FFFEFDF40C03020100FFFEFCE02603020100FFFEFBD13904020100FFFEF4C34504020100FFFBE8B85407020100FFFEF0BA560E020100FFFEEFB25B1E050100FFF8E3B16413020100)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_BITS_WB_Q5 = numeric_blob_array<opus_uint8, 72>(R"blob(FFFFFF9C049AFFFFFFFFFFE3660F5CFFFFFFFFFFD5531848ECFFFFFFFF964C213FD6FFFFFFBE794D2B37B9FFFFFFF589472B3B8BFFFFFFFF834232426BC2FFFFA6744C37357DFFFF)blob");
constexpr std::array<opus_uint8, 30> silk_NLSF_PRED_WB_Q8 = numeric_blob_array<opus_uint8, 30>(R"blob(AF94A0B0B2ADAEA4B1AEC4B6C6C0B6443E423C4875555A7688978EA08E9B)blob");
constexpr std::array<opus_int16, 17> silk_NLSF_DELTA_MIN_WB_Q15 = numeric_blob_array<opus_int16, 17>(R"blob(0064000300280003000300030005000E000E000A000B00030008000900070003015B)blob");
constinit const silk_NLSF_CB_struct silk_NLSF_CB_WB = make_silk_nlsf_cb(32, 16, ((opus_int32)((0.15) * ((opus_int64)1 << (16)) + 0.5)), ((opus_int32)((1.0 / 0.15) * ((opus_int64)1 << (6)) + 0.5)), silk_NLSF_CB1_WB_Q8, silk_NLSF_CB1_WB_Wght_Q9, silk_NLSF_CB1_iCDF_WB, silk_NLSF_PRED_WB_Q8, silk_NLSF_CB2_SELECT_WB, silk_NLSF_CB2_iCDF_WB, silk_NLSF_CB2_BITS_WB_Q5, silk_NLSF_DELTA_MIN_WB_Q15);
constexpr std::array<opus_int16, 16> silk_stereo_pred_quant_Q13 = numeric_blob_array<opus_int16, 16>(R"blob(CA5CD8BEDFB6E29AE69CEC78F47AFCCC03340B86138819641D66204A274235A4)blob");
constexpr std::array<opus_uint8, 25> silk_stereo_pred_joint_iCDF = numeric_blob_array<opus_uint8, 25>(R"blob(F9F7F6F5F4EAD2CAC9C8C5AE523B3837362E160C0B0A090700)blob");
constexpr std::array<opus_uint8, 2> silk_stereo_only_code_mid_iCDF = numeric_blob_array<opus_uint8, 2>(R"blob(4000)blob");
constexpr std::array<opus_uint8, 3> silk_LBRR_flags_2_iCDF = numeric_blob_array<opus_uint8, 3>(R"blob(CB9600)blob");
constexpr std::array<opus_uint8, 7> silk_LBRR_flags_3_iCDF = numeric_blob_array<opus_uint8, 7>(R"blob(D7C3A67D6E5200)blob");
constexpr std::array<std::span<const opus_uint8>, 2> silk_LBRR_flags_iCDFs{std::span<const opus_uint8>{silk_LBRR_flags_2_iCDF}, std::span<const opus_uint8>{silk_LBRR_flags_3_iCDF}};
constexpr std::array<opus_uint8, 2> silk_lsb_iCDF = numeric_blob_array<opus_uint8, 2>(R"blob(7800)blob");
constexpr std::array<opus_uint8, 3> silk_LTPscale_iCDF = numeric_blob_array<opus_uint8, 3>(R"blob(804000)blob");
constexpr std::array<opus_uint8, 4> silk_type_offset_VAD_iCDF = numeric_blob_array<opus_uint8, 4>(R"blob(E89E0A00)blob");
constexpr std::array<opus_uint8, 2> silk_type_offset_no_VAD_iCDF = numeric_blob_array<opus_uint8, 2>(R"blob(E600)blob");
constexpr std::array<opus_uint8, 5> silk_NLSF_interpolation_factor_iCDF = numeric_blob_array<opus_uint8, 5>(R"blob(F3DDC0B500)blob");
constexpr std::array<std::array<opus_int16, 2>, 2> silk_Quantization_Offsets_Q10 = numeric_blob_matrix<opus_int16, 2, 2>(R"blob(006400F000200064)blob");
constexpr std::array<opus_int16, 3> silk_LTPScales_table_Q14 = numeric_blob_array<opus_int16, 3>(R"blob(3CCD30002000)blob");
constexpr std::array<opus_uint8, 3> silk_uniform3_iCDF = numeric_blob_array<opus_uint8, 3>(R"blob(AB5500)blob");
constexpr std::array<opus_uint8, 4> silk_uniform4_iCDF = numeric_blob_array<opus_uint8, 4>(R"blob(C0804000)blob");
constexpr std::array<opus_uint8, 5> silk_uniform5_iCDF = numeric_blob_array<opus_uint8, 5>(R"blob(CD9A663300)blob");
constexpr std::array<opus_uint8, 6> silk_uniform6_iCDF = numeric_blob_array<opus_uint8, 6>(R"blob(D5AB80552B00)blob");
constexpr std::array<opus_uint8, 8> silk_uniform8_iCDF = numeric_blob_array<opus_uint8, 8>(R"blob(E0C0A08060402000)blob");
constexpr std::array<opus_uint8, 7> silk_NLSF_EXT_iCDF = numeric_blob_array<opus_uint8, 7>(R"blob(64281007030100)blob");
constexpr std::array<std::array<opus_int32, 3>, 5> silk_Transition_LP_B_Q28 = numeric_blob_matrix<opus_int32, 5, 3>(R"blob(0EF2670A1DE4CD560EF2670A0C82527519049A590C8252750A311146146203ED0A31114607D702DA0FADC6F907D702DA0552B6220AA4FADA0552B622)blob");
constexpr std::array<std::array<opus_int32, 2>, 5> silk_Transition_LP_A_Q28 = numeric_blob_matrix<opus_int32, 5, 2>(R"blob(1E2EF3460E4BE32B1880661F0A1D2C1C124861DA06F49CED0B1330EC04A590E3021DA4ED036BDF0A)blob");
constexpr std::array<opus_uint8, 2 * (18 - 2)> silk_pitch_lag_iCDF = numeric_blob_array<opus_uint8, 32>(R"blob(FDFAF4E9D4B69683786E6255483C31282019130F0D0B09080706050403020100)blob");
constexpr std::array<opus_uint8, 21> silk_pitch_delta_iCDF = numeric_blob_array<opus_uint8, 21>(R"blob(D2D0CECBC7C1B7A88E684A34251B140E0A06040200)blob");
constexpr std::array<opus_uint8, 34> silk_pitch_contour_iCDF = numeric_blob_array<opus_uint8, 34>(R"blob(DFC9B7A7988A7C6F62584F463E38322C27231F1B181512100E0C0A08060403020100)blob");
constexpr std::array<opus_uint8, 11> silk_pitch_contour_NB_iCDF = numeric_blob_array<opus_uint8, 11>(R"blob(BCB09B8A7761432B1A0A00)blob");
constexpr std::array<opus_uint8, 12> silk_pitch_contour_10_ms_iCDF = numeric_blob_array<opus_uint8, 12>(R"blob(A577503D2F231B140E090400)blob");
constexpr std::array<opus_uint8, 3> silk_pitch_contour_10_ms_NB_iCDF = numeric_blob_array<opus_uint8, 3>(R"blob(713F00)blob");
constexpr std::array<opus_uint8, 4> silk_max_pulses_table = numeric_blob_array<opus_uint8, 4>(R"blob(080A0C10)blob");
constexpr std::array<std::array<opus_uint8, 18>, 10> silk_pulses_per_block_iCDF = numeric_blob_matrix<opus_uint8, 10, 18>(R"blob(7D331A120F0C0B0A09080706050403020100C6692D160F0C0B0A09080706050403020100D5A274533B2B2018120F0C09070605030200EFBB743B1C100B0A09080706050403020100FAE5BC8756331E130D0A0806050403020100F9EBD5B99C80675342352A211A15110D0A00FEF9EBCEA4764D2E1B100A07050403020100FFFDF9EFDCBF9C77553925170F0A06040200FFFDFBF6EDDFCBB3987C624B37281D150F00FFFEFDF7DCA26A432A1C120C090604030200)blob");
constexpr std::array<std::array<opus_uint8, 18>, 9> silk_pulses_per_block_BITS_Q5 = numeric_blob_matrix<opus_uint8, 9, 18>(R"blob(1F396BA0CDCDFFFFFFFFFFFFFFFFFFFFFFFF452F436FA6CDFFFFFFFFFFFFFFFFFFFFFFFF524A4F5F6D8091A0ADCDCDCDE0FFFFE0FFE07D4A3B45618DB6FFFFFFFFFFFFFFFFFFFFFFAD7355494C5C7391ADCDE0E0FFFFFFFFFFFFA686716665666B767D8A919BA6B6C0C0CD96E0B68665534F55617891ADCDE0FFFFFFFFFFFFE0C09678655C595D667686A0B6C0E0E0E0FFE0E0B69B86766D68666A6F768391A0AD83)blob");
constexpr std::array<std::array<opus_uint8, 9>, 2> silk_rate_levels_iCDF = numeric_blob_matrix<opus_uint8, 2, 9>(R"blob(F1BEB284574A290E00DFC19D8C6A39271200)blob");
constexpr std::array<std::array<opus_uint8, 9>, 2> silk_rate_levels_BITS_Q5 = numeric_blob_matrix<opus_uint8, 2, 9>(R"blob(834A8D4F508A5F68865F635B7D5D4C7B737B)blob");
constexpr std::array<opus_uint8, 152> silk_shell_code_table0 = numeric_blob_array<opus_uint8, 152>(R"blob(8000D62A00EB801500F4B8480B00F8D6802A0700F8E1AA50190500FBECC67E36120300FAEED39F52230F0500FAE7CBA8805835190600FCEED8B9946C4728120400FDF3E1C7A6805A391F0D0300FEF6E9D4B7936D492C170A0200FFFAF0DFC6A6805A3A2110060100FFFBF4E7D2B5926E4B2E190C050100FFFDF8EEDDC4A4805C3C231208030100FFFDF9F2E5D0B4926E4C301B0E07030100)blob");
constexpr std::array<opus_uint8, 152> silk_shell_code_table1 = numeric_blob_array<opus_uint8, 152>(R"blob(8100CF3200EC811400F5B9480A00F9D5812A0600FAE2A9571B0400FBE9C2823E140400FAECCFA0632F110300FFF0D9B68351290B0100FFFEE9C99F6B3D14020100FFF9E9CEAA80563217070100FFFAEED9BA946C462712060100FFFCF3E2C8A6805A381E0D040100FFFCF5E7D1B4926E4C2F190B040100FFFDF8EDDBC2A3805D3E251308030100FFFEFAF1E2CDB1916F4F331E0F06020100)blob");
constexpr std::array<opus_uint8, 152> silk_shell_code_table2 = numeric_blob_array<opus_uint8, 152>(R"blob(8100CB3600EA811700F5B8490A00FAD781290500FCE8AD56180300FDF0C881380F0200FDF4D9A45E260A0100FDF5E2BD84471B070100FDF6E7CB9F693817060100FFF8EBD5B385552F13050100FFFEF3DDC29F7546250C020100FFFEF8EAD0AB8055301608020100FFFEFAF0DCBD956B43241006020100FFFEFBF3E3C9A6805A371D0D05020100FFFEFCF6EAD5B7936D492B160A04020100)blob");
constinit const std::array<opus_uint8, 152> silk_shell_code_table3 = numeric_blob_array<opus_uint8, 152>(R"blob(8200C83A00E7821A00F4B84C0C00F9D6822B0600FCE8AD57180300FDF1CB83380E0200FEF6DDA75E23080100FEF9E8C1824117050100FFFBEFD3A2632D0F040100FFFBF3DFBA834A210B030100FFFCF5E6CA9E69391808020100FFFDF7EBD6B384542C1307020100FFFEFAF0DFC49F7045240F06020100FFFEFDF5E7D1B0885D371B0B03020100FFFEFDFCEFDDC29E754C2A120403020100)blob");
constinit const std::array<opus_uint8, 17> silk_shell_code_table_offsets = numeric_blob_array<opus_uint8, 17>(R"blob(00000205090E141B232C36414D5A687787)blob");
constinit const std::array<opus_uint8, 42> silk_sign_iCDF = numeric_blob_array<opus_uint8, 42>(R"blob(FE31434D525D63C60B12181F242DFF2E424E575E68D00E15202A3342FF5E686D707376F8354550585F66)blob");
static_assert(every_row_is_descending(silk_gain_iCDF));
static_assert(icdf_is_descending(silk_delta_gain_iCDF));
static_assert(icdf_is_descending(silk_pitch_lag_iCDF));
static_assert(icdf_is_descending(silk_pitch_delta_iCDF));
static_assert(icdf_is_descending(silk_pitch_contour_iCDF));
static_assert(icdf_is_descending(silk_pitch_contour_NB_iCDF));
static_assert(icdf_is_descending(silk_pitch_contour_10_ms_iCDF));
static_assert(icdf_is_descending(silk_pitch_contour_10_ms_NB_iCDF));
static_assert(every_row_is_descending(silk_pulses_per_block_iCDF));
static_assert(every_row_is_descending(silk_rate_levels_iCDF));
}
static void silk_VAD_GetNoiseLevels(const opus_int32 pX[4], silk_VAD_state *psSilk_VAD);
int silk_VAD_Init(silk_VAD_state *psSilk_VAD) {
  int b, ret = 0;
  zero_n_bytes(psSilk_VAD, static_cast<std::size_t>(sizeof(silk_VAD_state)));
  for (b = 0; b < 4; b++) { psSilk_VAD->NoiseLevelBias[b] = std::max((opus_int32)(50/(b+1)),1); psSilk_VAD->NL[b] = 100*psSilk_VAD->NoiseLevelBias[b]; psSilk_VAD->inv_NL[b] = (opus_int32)(0x7FFFFFFF/psSilk_VAD->NL[b]); psSilk_VAD->NrgRatioSmth_Q8[b] = 25600;
}
  psSilk_VAD->counter = 15;
  return (ret);
}
constexpr std::array<opus_int32, 4> tiltWeights = numeric_blob_array<opus_int32, 4>(R"blob(0000753000001770FFFFD120FFFFD120)blob");
int silk_VAD_GetSA_Q8_c(silk_encoder_state *psEncC, const opus_int16 pIn[]) {
  int SA_Q15, pSNR_dB_Q7, input_tilt, decimated_framelength1, decimated_framelength2, decimated_framelength, dec_subframe_length, dec_subframe_offset, SNR_Q7, i, b, s;
  opus_int32 sumSquared = 0, smooth_coef_Q16; opus_int16 HPstateTmp;
  opus_int32 Xnrg[4]; opus_int32 NrgToNoiseRatio_Q8[4];
  opus_int32 speech_nrg, x_tmp;
  int X_offset[4];
  int ret = 0;
  silk_VAD_state *psSilk_VAD = &psEncC->sVAD; opus_assume(4 == 4);
  opus_assume(((5 * 4) * 16) >= psEncC->frame_length); opus_assume(psEncC->frame_length <= 512);
  opus_assume(psEncC->frame_length == 8 * ((psEncC->frame_length) >> (3)));
  decimated_framelength1 = ((psEncC->frame_length) >> (1)); decimated_framelength2 = ((psEncC->frame_length) >> (2));
  decimated_framelength = ((psEncC->frame_length) >> (3)); X_offset[0] = 0;
  X_offset[1] = decimated_framelength + decimated_framelength2; X_offset[2] = X_offset[1] + decimated_framelength;
  X_offset[3] = X_offset[2] + decimated_framelength2;
  auto *X = OPUS_SCRATCH(opus_int16, X_offset[3] + decimated_framelength1);
  silk_ana_filt_bank_1(pIn, &psSilk_VAD->AnaState[0], X, &X[X_offset[3]], psEncC->frame_length);
  silk_ana_filt_bank_1(X, &psSilk_VAD->AnaState1[0], X, &X[X_offset[2]], decimated_framelength1);
  silk_ana_filt_bank_1(X, &psSilk_VAD->AnaState2[0], X, &X[X_offset[1]], decimated_framelength2);
  X[decimated_framelength - 1] = ((X[decimated_framelength - 1]) >> (1)); HPstateTmp = X[decimated_framelength - 1];
  for (i = decimated_framelength - 1; i > 0; i--) { X[i - 1] = ((X[i - 1]) >> (1)); X[i] -= X[i - 1];
}
  X[0] -= psSilk_VAD->HPstate; psSilk_VAD->HPstate = HPstateTmp;
  for (b = 0; b < 4; b++) {
    decimated_framelength = ((psEncC->frame_length) >> (std::min(4 - b, 4 - 1)));
    dec_subframe_length = ((decimated_framelength) >> (2)); dec_subframe_offset = 0; Xnrg[b] = psSilk_VAD->XnrgSubfr[b];
    for (s = 0; s < (1 << 2); s++) { sumSquared = 0; for (i = 0; i < dec_subframe_length; i++) { x_tmp = ((X[X_offset[b] + i + dec_subframe_offset]) >> (3)); sumSquared = ((sumSquared) + ((opus_int32)((opus_int16)(x_tmp))) * (opus_int32)((opus_int16)(x_tmp))); opus_assume(sumSquared >= 0); } if (s < (1 << 2) - 1) { Xnrg[b] = ((((opus_uint32)(Xnrg[b]) + (opus_uint32)(sumSquared)) & 0x80000000) ? 0x7FFFFFFF : ((Xnrg[b]) + (sumSquared))); } else { Xnrg[b] = ((((opus_uint32)(Xnrg[b]) + (opus_uint32)(((sumSquared) >> (1)))) & 0x80000000) ? 0x7FFFFFFF : ((Xnrg[b]) + (((sumSquared) >> (1))))); } dec_subframe_offset += dec_subframe_length;
}
    psSilk_VAD->XnrgSubfr[b] = sumSquared;
}
  silk_VAD_GetNoiseLevels(&Xnrg[0], psSilk_VAD); sumSquared = 0; input_tilt = 0;
  for (b = 0; b < 4; b++) {
    speech_nrg = Xnrg[b] - psSilk_VAD->NL[b];
    if (speech_nrg > 0) { if ((Xnrg[b] & 0xFF800000) == 0) { NrgToNoiseRatio_Q8[b] = ((opus_int32)((((opus_int32)((opus_uint32)(Xnrg[b]) << (8)))) / (psSilk_VAD->NL[b] + 1))); } else { NrgToNoiseRatio_Q8[b] = ((opus_int32)((Xnrg[b]) / (((psSilk_VAD->NL[b]) >> (8)) + 1))); } SNR_Q7 = silk_lin2log(NrgToNoiseRatio_Q8[b]) - 8 * 128; sumSquared = ((sumSquared) + ((opus_int32)((opus_int16)(SNR_Q7))) * (opus_int32)((opus_int16)(SNR_Q7))); if (speech_nrg < ((opus_int32)1 << 20)) { SNR_Q7 = ((opus_int32)(((((opus_int32)((opus_uint32)(silk_SQRT_APPROX(speech_nrg)) << (6)))) * (opus_int64)((opus_int16)(SNR_Q7))) >> 16)); } input_tilt = ((opus_int32)((input_tilt) + (((tiltWeights[b]) * (opus_int64)((opus_int16)(SNR_Q7))) >> 16))); } else { NrgToNoiseRatio_Q8[b] = 256; }
}
  sumSquared = ((opus_int32)((sumSquared) / (4))); pSNR_dB_Q7 = (opus_int16)(3 * silk_SQRT_APPROX(sumSquared));
  SA_Q15 = silk_sigm_Q15(((opus_int32)(((45000) * (opus_int64)((opus_int16)(pSNR_dB_Q7))) >> 16)) - 128);
  psEncC->input_tilt_Q15 = ((opus_int32)((opus_uint32)(silk_sigm_Q15(input_tilt) - 16384) << (1))); speech_nrg = 0;
  for (b = 0; b < 4; b++) { speech_nrg += (b + 1) * ((Xnrg[b] - psSilk_VAD->NL[b]) >> (4));
}
  if (psEncC->frame_length == 20 * psEncC->fs_kHz) { speech_nrg = ((speech_nrg) >> (1));
}
  if (speech_nrg <= 0) {
    SA_Q15 = ((SA_Q15) >> (1));
  } else if (speech_nrg < 16384) {
    speech_nrg = ((opus_int32)((opus_uint32)(speech_nrg) << (16))); speech_nrg = silk_SQRT_APPROX(speech_nrg);
    SA_Q15 = ((opus_int32)(((32768 + speech_nrg) * (opus_int64)((opus_int16)(SA_Q15))) >> 16));
}
  psEncC->speech_activity_Q8 = std::min(((SA_Q15) >> (7)), 0xFF);
  smooth_coef_Q16 = ((opus_int32)(((4096) * (opus_int64)((opus_int16)(((opus_int32)((((opus_int32)SA_Q15) * (opus_int64)((opus_int16)(SA_Q15))) >> 16))))) >> 16));
  if (psEncC->frame_length == 10 * psEncC->fs_kHz) { smooth_coef_Q16 >>= 1;
}
  for (b = 0; b < 4; b++) { psSilk_VAD->NrgRatioSmth_Q8[b] = ((opus_int32)((psSilk_VAD->NrgRatioSmth_Q8[b])+(((NrgToNoiseRatio_Q8[b]-psSilk_VAD->NrgRatioSmth_Q8[b])*(opus_int64)((opus_int16)(smooth_coef_Q16)))>>16))); SNR_Q7 = 3*(silk_lin2log(psSilk_VAD->NrgRatioSmth_Q8[b])-8*128); psEncC->input_quality_bands_Q15[b] = silk_sigm_Q15(((SNR_Q7-16*128)>>(4)));
}
  return (ret);
}
static void silk_VAD_GetNoiseLevels(const opus_int32 pX[4], silk_VAD_state *psSilk_VAD) {
  int k;
  opus_int32 nl, nrg, inv_nrg;
  int coef, min_coef;
  if (psSilk_VAD->counter < 1000) { min_coef = ((opus_int32)((0x7FFF) / (((psSilk_VAD->counter) >> (4)) + 1))); psSilk_VAD->counter++; } else { min_coef = 0;
}
  for (k = 0; k < 4; k++) {
    nl = psSilk_VAD->NL[k]; opus_assume(nl >= 0);
    nrg = ((((opus_uint32)(pX[k]) + (opus_uint32)(psSilk_VAD->NoiseLevelBias[k])) & 0x80000000) ? 0x7FFFFFFF : ((pX[k]) + (psSilk_VAD->NoiseLevelBias[k])));
    opus_assume(nrg > 0); inv_nrg = ((opus_int32)((0x7FFFFFFF) / (nrg)));
    opus_assume(inv_nrg >= 0);
    if (nrg > ((opus_int32)((opus_uint32)(nl) << (3)))) { coef = 1024 >> 3; } else if (nrg < nl) { coef = 1024; } else { coef = ((opus_int32)(((((opus_int32)(((opus_int64)(inv_nrg) * (nl)) >> 16))) * (opus_int64)((opus_int16)(1024 << 1))) >> 16));
}
    coef = std::max(coef, min_coef);
    psSilk_VAD->inv_NL[k] = ((opus_int32)((psSilk_VAD->inv_NL[k]) + (((inv_nrg - psSilk_VAD->inv_NL[k]) * (opus_int64)((opus_int16)(coef))) >> 16)));
    opus_assume(psSilk_VAD->inv_NL[k] >= 0); nl = ((opus_int32)((0x7FFFFFFF) / (psSilk_VAD->inv_NL[k])));
    opus_assume(nl >= 0); nl = (((nl) < (0x00FFFFFF)) ? (nl) : (0x00FFFFFF));
    psSilk_VAD->NL[k] = nl; }
}
int silk_control_audio_bandwidth(silk_encoder_state *psEncC, silk_EncControlStruct *encControl) {
  int fs_kHz, orig_kHz;
  opus_int32 fs_Hz; orig_kHz = psEncC->fs_kHz;
  if (orig_kHz == 0) { orig_kHz = psEncC->sLP.saved_fs_kHz;
}
  fs_kHz = orig_kHz; fs_Hz = ((opus_int32)((opus_int16)(fs_kHz)) * (opus_int32)((opus_int16)(1000)));
  if (fs_Hz == 0) { fs_Hz = (((psEncC->desiredInternal_fs_Hz) < (psEncC->API_fs_Hz)) ? (psEncC->desiredInternal_fs_Hz) : (psEncC->API_fs_Hz)); fs_kHz = ((opus_int32)((fs_Hz) / (1000)));
}
  else if (fs_Hz > psEncC->API_fs_Hz || fs_Hz > psEncC->maxInternal_fs_Hz || fs_Hz < psEncC->minInternal_fs_Hz) { fs_Hz = psEncC->API_fs_Hz; fs_Hz = (((fs_Hz) < (psEncC->maxInternal_fs_Hz)) ? (fs_Hz) : (psEncC->maxInternal_fs_Hz)); fs_Hz = (((fs_Hz) > (psEncC->minInternal_fs_Hz)) ? (fs_Hz) : (psEncC->minInternal_fs_Hz)); fs_kHz = ((opus_int32)((fs_Hz) / (1000)));
}
  else {
    if (psEncC->sLP.transition_frame_no >= (5120 / (5 * 4))) { psEncC->sLP.mode = 0;
}
    if (psEncC->allow_bandwidth_switch || encControl->opusCanSwitch) {
      if (((opus_int32)((opus_int16)(orig_kHz)) * (opus_int32)((opus_int16)(1000))) > psEncC->desiredInternal_fs_Hz) {
        if (psEncC->sLP.mode == 0) { psEncC->sLP.transition_frame_no = (5120 / (5 * 4)); zero_n_bytes(psEncC->sLP.In_LP_State, static_cast<std::size_t>(sizeof(psEncC->sLP.In_LP_State)));
}
        if (encControl->opusCanSwitch) { psEncC->sLP.mode = 0; fs_kHz = orig_kHz == 16 ? 12 : 8;
}
        else { if (psEncC->sLP.transition_frame_no <= 0) { encControl->switchReady = 1; encControl->maxBits -= encControl->maxBits * 5 / (encControl->payloadSize_ms + 5); } else { psEncC->sLP.mode = -2; }
}
      } else if (((opus_int32)((opus_int16)(orig_kHz)) * (opus_int32)((opus_int16)(1000))) < psEncC->desiredInternal_fs_Hz) {
        if (encControl->opusCanSwitch) { fs_kHz = orig_kHz == 8 ? 12 : 16; psEncC->sLP.transition_frame_no = 0; zero_n_bytes(psEncC->sLP.In_LP_State, static_cast<std::size_t>(sizeof(psEncC->sLP.In_LP_State))); psEncC->sLP.mode = 1;
}
        else { if (psEncC->sLP.mode == 0) { encControl->switchReady = 1; encControl->maxBits -= encControl->maxBits * 5 / (encControl->payloadSize_ms + 5); } else { psEncC->sLP.mode = 1; }
}
      } else { if (psEncC->sLP.mode < 0) psEncC->sLP.mode = 1; } }
}
  return fs_kHz;
}
void silk_quant_LTP_gains(opus_int16 B_Q14[4 * 5], opus_int8 cbk_index[4], opus_int8 *periodicity_index, opus_int32 *sum_log_gain_Q7, int *pred_gain_dB_Q7, const opus_int32 XX_Q17[4 * 5 * 5], const opus_int32 xX_Q17[4 * 5], const int subfr_len, const int nb_subfr) {
  int j, k, cbk_size;
  opus_int8 temp_idx[4];
  const opus_uint8 *cl_ptr_Q5;
  const opus_int8 *cbk_ptr_Q7;
  const opus_uint8 *cbk_gain_ptr_Q7;
  const opus_int32 *XX_Q17_ptr, *xX_Q17_ptr;
  opus_int32 res_nrg_Q15_subfr, res_nrg_Q15 = 0, rate_dist_Q7_subfr, rate_dist_Q7, min_rate_dist_Q7;
  opus_int32 sum_log_gain_tmp_Q7, best_sum_log_gain_Q7, max_gain_Q7;
  int gain_Q7 = 0;
  min_rate_dist_Q7 = 0x7FFFFFFF; best_sum_log_gain_Q7 = 0;
  for (k = 0; k < 3; k++) {
    opus_int32 gain_safety = ((opus_int32)((0.4) * ((opus_int64)1 << (7)) + 0.5));
    const auto &ltp_codebook = silk_LTP_codebooks[k];
    cl_ptr_Q5 = ltp_codebook.gain_bits_q5.data(); cbk_ptr_Q7 = ltp_codebook.vq_q7.data();
    cbk_gain_ptr_Q7 = ltp_codebook.vq_gain_q7.data(); cbk_size = ltp_codebook.vq_size;
    XX_Q17_ptr = XX_Q17; xX_Q17_ptr = xX_Q17; res_nrg_Q15 = 0; rate_dist_Q7 = 0; sum_log_gain_tmp_Q7 = *sum_log_gain_Q7;
    for (j = 0; j < nb_subfr; j++) {
      max_gain_Q7 = silk_log2lin((((opus_int32)((250.0f / 6.0) * ((opus_int64)1 << (7)) + 0.5)) - sum_log_gain_tmp_Q7) + ((opus_int32)((7) * ((opus_int64)1 << (7)) + 0.5))) - gain_safety;
      silk_VQ_WMat_EC_c(&temp_idx[j], &res_nrg_Q15_subfr, &rate_dist_Q7_subfr, &gain_Q7, XX_Q17_ptr, xX_Q17_ptr, cbk_ptr_Q7, cbk_gain_ptr_Q7, cl_ptr_Q5, subfr_len, max_gain_Q7, cbk_size);
      res_nrg_Q15 = ((((opus_uint32)(res_nrg_Q15) + (opus_uint32)(res_nrg_Q15_subfr)) & 0x80000000) ? 0x7FFFFFFF : ((res_nrg_Q15) + (res_nrg_Q15_subfr)));
      rate_dist_Q7 = ((((opus_uint32)(rate_dist_Q7) + (opus_uint32)(rate_dist_Q7_subfr)) & 0x80000000) ? 0x7FFFFFFF : ((rate_dist_Q7) + (rate_dist_Q7_subfr)));
      const opus_int32 log_gain_delta_Q7 = silk_lin2log(gain_safety + gain_Q7) - ((opus_int32)((7) * ((opus_int64)1 << (7)) + 0.5));
      sum_log_gain_tmp_Q7 = std::max(0, sum_log_gain_tmp_Q7 + log_gain_delta_Q7);
      XX_Q17_ptr += 5 * 5; xX_Q17_ptr += 5;
}
    if (rate_dist_Q7 <= min_rate_dist_Q7) { min_rate_dist_Q7 = rate_dist_Q7; *periodicity_index = (opus_int8)k; copy_n_bytes(temp_idx, static_cast<std::size_t>(nb_subfr * sizeof(opus_int8)), cbk_index); best_sum_log_gain_Q7 = sum_log_gain_tmp_Q7; }
}
  cbk_ptr_Q7 = silk_LTP_codebooks[*periodicity_index].vq_q7.data();
  for (j = 0; j < nb_subfr; j++) { for (k = 0; k < 5; k++) { B_Q14[j * 5 + k] = ((opus_int32)((opus_uint32)(cbk_ptr_Q7[cbk_index[j] * 5 + k]) << (7))); }
}
  if (nb_subfr == 2) { res_nrg_Q15 = ((res_nrg_Q15) >> (1)); } else { res_nrg_Q15 = ((res_nrg_Q15) >> (2));
}
  *sum_log_gain_Q7 = best_sum_log_gain_Q7;
  *pred_gain_dB_Q7 = (int)((opus_int32)((opus_int16)(-3)) * (opus_int32)((opus_int16)(silk_lin2log(res_nrg_Q15) - (15 << 7))));
}
void silk_VQ_WMat_EC_c(opus_int8 *ind, opus_int32 *res_nrg_Q15, opus_int32 *rate_dist_Q8, int *gain_Q7, const opus_int32 *XX_Q17, const opus_int32 *xX_Q17, const opus_int8 *cb_Q7, const opus_uint8 *cb_gain_Q7, const opus_uint8 *cl_Q5, const int subfr_len, const opus_int32 max_gain_Q7, const int L) {
  int k, gain_tmp_Q7;
  const opus_int8 *cb_row_Q7;
  opus_int32 neg_xX_Q24[5]; opus_int32 sum1_Q15, sum2_Q24;
  opus_int32 bits_res_Q8, bits_tot_Q8; neg_xX_Q24[0] = -((opus_int32)((opus_uint32)(xX_Q17[0]) << (7)));
  neg_xX_Q24[1] = -((opus_int32)((opus_uint32)(xX_Q17[1]) << (7))); neg_xX_Q24[2] = -((opus_int32)((opus_uint32)(xX_Q17[2]) << (7)));
  neg_xX_Q24[3] = -((opus_int32)((opus_uint32)(xX_Q17[3]) << (7)));
  neg_xX_Q24[4] = -((opus_int32)((opus_uint32)(xX_Q17[4]) << (7))); *rate_dist_Q8 = 0x7FFFFFFF;
  *res_nrg_Q15 = 0x7FFFFFFF; cb_row_Q7 = cb_Q7; *ind = 0;
  for (k = 0; k < L; k++) {
    opus_int32 penalty; gain_tmp_Q7 = cb_gain_Q7[k];
    sum1_Q15 = ((opus_int32)((1.001) * ((opus_int64)1 << (15)) + 0.5));
    penalty = ((opus_int32)((opus_uint32)((((((gain_tmp_Q7) - (max_gain_Q7))) > (0)) ? (((gain_tmp_Q7) - (max_gain_Q7))) : (0))) << (11)));
    sum2_Q24 = (((neg_xX_Q24[0])) + (((XX_Q17[1]) * (cb_row_Q7[1])))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[2]) * (cb_row_Q7[2]))));
    sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[3]) * (cb_row_Q7[3])))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[4]) * (cb_row_Q7[4]))));
    sum2_Q24 = ((opus_int32)((opus_uint32)(sum2_Q24) << (1))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[0]) * (cb_row_Q7[0]))));
    sum1_Q15 = ((opus_int32)((sum1_Q15) + (((sum2_Q24) * (opus_int64)((opus_int16)(cb_row_Q7[0]))) >> 16)));
    sum2_Q24 = (((neg_xX_Q24[1])) + (((XX_Q17[7]) * (cb_row_Q7[2])))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[8]) * (cb_row_Q7[3]))));
    sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[9]) * (cb_row_Q7[4])))); sum2_Q24 = ((opus_int32)((opus_uint32)(sum2_Q24) << (1)));
    sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[6]) * (cb_row_Q7[1]))));
    sum1_Q15 = ((opus_int32)((sum1_Q15) + (((sum2_Q24) * (opus_int64)((opus_int16)(cb_row_Q7[1]))) >> 16)));
    sum2_Q24 = (((neg_xX_Q24[2])) + (((XX_Q17[13]) * (cb_row_Q7[3])))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[14]) * (cb_row_Q7[4]))));
    sum2_Q24 = ((opus_int32)((opus_uint32)(sum2_Q24) << (1))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[12]) * (cb_row_Q7[2]))));
    sum1_Q15 = ((opus_int32)((sum1_Q15) + (((sum2_Q24) * (opus_int64)((opus_int16)(cb_row_Q7[2]))) >> 16)));
    sum2_Q24 = (((neg_xX_Q24[3])) + (((XX_Q17[19]) * (cb_row_Q7[4])))); sum2_Q24 = ((opus_int32)((opus_uint32)(sum2_Q24) << (1)));
    sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[18]) * (cb_row_Q7[3]))));
    sum1_Q15 = ((opus_int32)((sum1_Q15) + (((sum2_Q24) * (opus_int64)((opus_int16)(cb_row_Q7[3]))) >> 16)));
    sum2_Q24 = ((opus_int32)((opus_uint32)(neg_xX_Q24[4]) << (1))); sum2_Q24 = (((sum2_Q24)) + (((XX_Q17[24]) * (cb_row_Q7[4]))));
    sum1_Q15 = ((opus_int32)((sum1_Q15) + (((sum2_Q24) * (opus_int64)((opus_int16)(cb_row_Q7[4]))) >> 16)));
    if (sum1_Q15 >= 0) {
      bits_res_Q8 = ((opus_int32)((opus_int16)(subfr_len)) * (opus_int32)((opus_int16)(silk_lin2log(sum1_Q15 + penalty) - (15 << 7))));
      bits_tot_Q8 = (((bits_res_Q8)) + (((opus_int32)((opus_uint32)((cl_Q5[k])) << ((3 - 1))))));
      if (bits_tot_Q8 <= *rate_dist_Q8) { *rate_dist_Q8 = bits_tot_Q8; *res_nrg_Q15 = sum1_Q15 + penalty; *ind = (opus_int8)k; *gain_Q7 = gain_tmp_Q7; }
}
    cb_row_Q7 += 5; }
}
void silk_HP_variable_cutoff(silk_encoder_state_FLP state_Fxx[]) {
  int quality_Q15;
  opus_int32 pitch_freq_Hz_Q16, pitch_freq_log_Q7, delta_freq_Q7; silk_encoder_state *psEncC1 = &state_Fxx[0].sCmn;
  if (psEncC1->prevSignalType == 2) {
    pitch_freq_Hz_Q16 = ((opus_int32)((((opus_int32)((opus_uint32)(((psEncC1->fs_kHz) * (1000))) << (16)))) / (psEncC1->prevLag)));
    pitch_freq_log_Q7 = silk_lin2log(pitch_freq_Hz_Q16) - (16 << 7); quality_Q15 = psEncC1->input_quality_bands_Q15[0];
    pitch_freq_log_Q7 = ((opus_int32)((pitch_freq_log_Q7) + (((((opus_int32)(((((opus_int32)((opus_uint32)(-quality_Q15) << (2)))) * (opus_int64)((opus_int16)(quality_Q15))) >> 16))) * (opus_int64)((opus_int16)(pitch_freq_log_Q7 - (silk_lin2log(((opus_int32)((60) * ((opus_int64)1 << (16)) + 0.5))) - (16 << 7))))) >> 16)));
    delta_freq_Q7 = pitch_freq_log_Q7 - ((psEncC1->variable_HP_smth1_Q15) >> (8));
    if (delta_freq_Q7 < 0) delta_freq_Q7 = ((delta_freq_Q7) * (3));
    constexpr opus_int32 delta_limit_Q7 = ((opus_int32)((0.4f) * ((opus_int64)1 << (7)) + 0.5));
    delta_freq_Q7 = std::max(-delta_limit_Q7, std::min(delta_limit_Q7, delta_freq_Q7));
    psEncC1->variable_HP_smth1_Q15 = ((opus_int32)((psEncC1->variable_HP_smth1_Q15) + (((((opus_int32)((opus_int16)(psEncC1->speech_activity_Q8)) * (opus_int32)((opus_int16)(delta_freq_Q7)))) * (opus_int64)((opus_int16)(((opus_int32)((0.1f) * ((opus_int64)1 << (16)) + 0.5))))) >> 16)));
    const opus_int32 hp_min_Q15 = ((opus_int32)((opus_uint32)(silk_lin2log(60)) << (8)));
    const opus_int32 hp_max_Q15 = ((opus_int32)((opus_uint32)(silk_lin2log(100)) << (8)));
    psEncC1->variable_HP_smth1_Q15 = std::max(hp_min_Q15, std::min(hp_max_Q15, psEncC1->variable_HP_smth1_Q15)); }
}
opus_int32 silk_NLSF_encode(opus_int8 *NLSFIndices, opus_int16 *pNLSF_Q15, const silk_NLSF_CB_struct *psNLSF_CB, const opus_int16 *pW_Q2, const int NLSF_mu_Q20, const int nSurvivors, const int signalType) {
  int i, s, ind1, bestIndex, prob_Q8, bits_q7;
  opus_int32 W_tmp_Q9, ret; opus_int16 res_Q10[16];
  opus_int16 NLSF_tmp_Q15[16]; opus_int16 W_adj_Q5[16];
  opus_uint8 pred_Q8[16]; opus_int16 ec_ix[16];
  const opus_uint8 *pCB_element, *iCDF_ptr;
  const opus_int16 *pCB_Wght_Q9;
  opus_assume(signalType >= 0 && signalType <= 2); opus_assume(NLSF_mu_Q20 <= 32767 && NLSF_mu_Q20 >= 0);
  silk_NLSF_stabilize(pNLSF_Q15, psNLSF_CB->deltaMin_Q15, psNLSF_CB->order);
  auto *err_Q24 = OPUS_SCRATCH(opus_int32, psNLSF_CB->nVectors);
  silk_NLSF_VQ(err_Q24, pNLSF_Q15, psNLSF_CB->CB1_NLSF_Q8, psNLSF_CB->CB1_Wght_Q9, psNLSF_CB->nVectors, psNLSF_CB->order);
  auto *tempIndices1 = OPUS_SCRATCH(int, nSurvivors);
  silk_insertion_sort_increasing(err_Q24, tempIndices1, psNLSF_CB->nVectors, nSurvivors);
  auto *RD_Q25 = OPUS_SCRATCH(opus_int32, nSurvivors);
  auto *tempIndices2 = OPUS_SCRATCH(opus_int8, nSurvivors * 16);
  for (s = 0; s < nSurvivors; s++) {
    ind1 = tempIndices1[s]; pCB_element = silk_nlsf_cb1_q8(psNLSF_CB, ind1); pCB_Wght_Q9 = silk_nlsf_weights_q9(psNLSF_CB, ind1);
    for (i = 0; i < psNLSF_CB->order; i++) { NLSF_tmp_Q15[i] = ((opus_int16)((opus_uint16)((opus_int16)pCB_element[i]) << (7))); W_tmp_Q9 = pCB_Wght_Q9[i]; res_Q10[i] = (opus_int16)((((opus_int32)((opus_int16)(pNLSF_Q15[i] - NLSF_tmp_Q15[i])) * (opus_int32)((opus_int16)(W_tmp_Q9)))) >> (14)); W_adj_Q5[i] = silk_DIV32_varQ((opus_int32)pW_Q2[i], ((opus_int32)((opus_int16)(W_tmp_Q9)) * (opus_int32)((opus_int16)(W_tmp_Q9))), 21);
}
    silk_NLSF_unpack(ec_ix, pred_Q8, psNLSF_CB, ind1);
    RD_Q25[s] = silk_NLSF_del_dec_quant(&tempIndices2[s * 16], res_Q10, W_adj_Q5, pred_Q8, ec_ix, psNLSF_CB->ec_Rates_Q5, psNLSF_CB->quantStepSize_Q16, psNLSF_CB->invQuantStepSize_Q6, NLSF_mu_Q20, psNLSF_CB->order);
    iCDF_ptr = silk_nlsf_cb1_icdf(psNLSF_CB, signalType);
    if (ind1 == 0) { prob_Q8 = 256 - iCDF_ptr[ind1]; } else { prob_Q8 = iCDF_ptr[ind1 - 1] - iCDF_ptr[ind1];
}
    bits_q7 = (8 << 7) - silk_lin2log(prob_Q8);
    RD_Q25[s] = ((RD_Q25[s]) + ((opus_int32)((opus_int16)(bits_q7))) * (opus_int32)((opus_int16)(((NLSF_mu_Q20) >> (2)))));
}
  silk_insertion_sort_increasing(RD_Q25, &bestIndex, nSurvivors, 1); NLSFIndices[0] = (opus_int8)tempIndices1[bestIndex];
  copy_n_bytes(&tempIndices2[bestIndex * 16], static_cast<std::size_t>(psNLSF_CB->order * sizeof(opus_int8)), &NLSFIndices[1]);
  silk_NLSF_decode(pNLSF_Q15, NLSFIndices, psNLSF_CB); ret = RD_Q25[0];
  return ret;
}
void silk_NLSF_VQ(opus_int32 err_Q24[], const opus_int16 in_Q15[], const opus_uint8 pCB_Q8[], const opus_int16 pWght_Q9[], const int K, const int LPC_order) {
  int i, m;
  opus_int32 diff_Q15, diffw_Q24, sum_error_Q24, pred_Q24;
  const opus_int16 *w_Q9_ptr;
  const opus_uint8 *cb_Q8_ptr;
  opus_assume((LPC_order & 1) == 0); cb_Q8_ptr = pCB_Q8;
  w_Q9_ptr = pWght_Q9;
  for (i = 0; i < K; i++) {
    sum_error_Q24 = 0; pred_Q24 = 0;
    for (m = LPC_order - 2; m >= 0; m -= 2) { diff_Q15 = (((in_Q15[m + 1])) - (((opus_int32)((opus_uint32)(((opus_int32)cb_Q8_ptr[m + 1])) << ((7)))))); diffw_Q24 = ((opus_int32)((opus_int16)(diff_Q15)) * (opus_int32)((opus_int16)(w_Q9_ptr[m + 1]))); sum_error_Q24 = ((sum_error_Q24) + (((((((diffw_Q24)) - ((((pred_Q24)) >> ((1)))))) > 0) ? ((((diffw_Q24)) - ((((pred_Q24)) >> ((1)))))) : -((((diffw_Q24)) - ((((pred_Q24)) >> ((1))))))))); pred_Q24 = diffw_Q24; diff_Q15 = (((in_Q15[m])) - (((opus_int32)((opus_uint32)(((opus_int32)cb_Q8_ptr[m])) << ((7)))))); diffw_Q24 = ((opus_int32)((opus_int16)(diff_Q15)) * (opus_int32)((opus_int16)(w_Q9_ptr[m]))); sum_error_Q24 = ((sum_error_Q24) + (((((((diffw_Q24)) - ((((pred_Q24)) >> ((1)))))) > 0) ? ((((diffw_Q24)) - ((((pred_Q24)) >> ((1)))))) : -((((diffw_Q24)) - ((((pred_Q24)) >> ((1)))))))));  pred_Q24 = diffw_Q24; opus_assume(sum_error_Q24 >= 0);
}
    err_Q24[i] = sum_error_Q24; cb_Q8_ptr += LPC_order; w_Q9_ptr += LPC_order; }
}
void silk_NLSF_unpack(opus_int16 ec_ix[], opus_uint8 pred_Q8[], const silk_NLSF_CB_struct *psNLSF_CB, const int CB1_index) {
  int i;
  opus_uint8 entry;
  auto *ec_sel_ptr = silk_nlsf_ec_sel_row(psNLSF_CB, CB1_index);
  for (i = 0; i < psNLSF_CB->order; i += 2) { entry = *ec_sel_ptr++; ec_ix[i] = ((opus_int32)((opus_int16)(((entry) >> (1)) & 7)) * (opus_int32)((opus_int16)(2 * 4 + 1))); pred_Q8[i] = psNLSF_CB->pred_Q8[i + (entry & 1) * (psNLSF_CB->order - 1)]; ec_ix[i + 1] = ((opus_int32)((opus_int16)(((entry) >> (5)) & 7)) * (opus_int32)((opus_int16)(2 * 4 + 1))); pred_Q8[i + 1] = psNLSF_CB->pred_Q8[i + (((entry) >> (4)) & 1) * (psNLSF_CB->order - 1) + 1]; }
}
opus_int32 silk_NLSF_del_dec_quant(opus_int8 indices[], const opus_int16 x_Q10[], const opus_int16 w_Q5[], const opus_uint8 pred_coef_Q8[], const opus_int16 ec_ix[], const opus_uint8 ec_rates_Q5[], const int quant_step_size_Q16, const opus_int16 inv_quant_step_size_Q6, const opus_int32 mu_Q20, const opus_int16 order) {
  int i, j, nStates, ind_tmp, ind_min_max, ind_max_min, in_Q10, res_Q10;
  int pred_Q10, diff_Q10, rate0_Q5, rate1_Q5;
  opus_int16 out0_Q10, out1_Q10; opus_int32 RD_tmp_Q25, min_Q25, min_max_Q25, max_min_Q25;
  int ind_sort[(1 << 2)];
  opus_int8 ind[(1 << 2)][16]; opus_int16 prev_out_Q10[2 * (1 << 2)];
  opus_int32 RD_Q25[2 * (1 << 2)]; opus_int32 RD_min_Q25[(1 << 2)];
  opus_int32 RD_max_Q25[(1 << 2)];
  const opus_uint8 *rates_Q5;
  int out0_Q10_table[2 * 10];
  int out1_Q10_table[2 * 10];
  for (i = -10; i <= 10 - 1; i++) {
    out0_Q10 = ((opus_int32)((opus_uint32)(i) << (10))); out1_Q10 = ((out0_Q10) + (1024));
    if (i > 0) { out0_Q10 = ((out0_Q10) - (((opus_int32)((0.1) * ((opus_int64)1 << (10)) + 0.5)))); out1_Q10 = ((out1_Q10) - (((opus_int32)((0.1) * ((opus_int64)1 << (10)) + 0.5))));
}
    else if (i == 0) { out1_Q10 = ((out1_Q10) - (((opus_int32)((0.1) * ((opus_int64)1 << (10)) + 0.5))));
}
    else if (i == -1) { out0_Q10 = ((out0_Q10) + (((opus_int32)((0.1) * ((opus_int64)1 << (10)) + 0.5))));
}
    else { out0_Q10 = ((out0_Q10) + (((opus_int32)((0.1) * ((opus_int64)1 << (10)) + 0.5)))); out1_Q10 = ((out1_Q10) + (((opus_int32)((0.1) * ((opus_int64)1 << (10)) + 0.5))));
}
    out0_Q10_table[i + 10] = ((((opus_int32)((opus_int16)(out0_Q10)) * (opus_int32)((opus_int16)(quant_step_size_Q16)))) >> (16));
    out1_Q10_table[i + 10] = ((((opus_int32)((opus_int16)(out1_Q10)) * (opus_int32)((opus_int16)(quant_step_size_Q16)))) >> (16));
}
  opus_assume(((1 << 2) & ((1 << 2) - 1)) == 0); nStates = 1;
  RD_Q25[0] = 0; prev_out_Q10[0] = 0;
  for (i = order - 1; i >= 0; i--) {
    rates_Q5 = &ec_rates_Q5[ec_ix[i]]; in_Q10 = x_Q10[i];
    for (j = 0; j < nStates; j++) {
      pred_Q10 = ((((opus_int32)((opus_int16)((opus_int16)pred_coef_Q8[i])) * (opus_int32)((opus_int16)(prev_out_Q10[j])))) >> (8));
      res_Q10 = ((in_Q10) - (pred_Q10));
      ind_tmp = ((((opus_int32)((opus_int16)(inv_quant_step_size_Q6)) * (opus_int32)((opus_int16)(res_Q10)))) >> (16));
      ind_tmp = ((-10) > (10 - 1) ? ((ind_tmp) > (-10) ? (-10) : ((ind_tmp) < (10 - 1) ? (10 - 1) : (ind_tmp))) : ((ind_tmp) > (10 - 1) ? (10 - 1) : ((ind_tmp) < (-10) ? (-10) : (ind_tmp))));
      ind[j][i] = (opus_int8)ind_tmp; out0_Q10 = out0_Q10_table[ind_tmp + 10];
      out1_Q10 = out1_Q10_table[ind_tmp + 10]; out0_Q10 = ((out0_Q10) + (pred_Q10));
      out1_Q10 = ((out1_Q10) + (pred_Q10)); prev_out_Q10[j] = out0_Q10; prev_out_Q10[j + nStates] = out1_Q10;
      if (ind_tmp + 1 >= 4) { if (ind_tmp + 1 == 4) { rate0_Q5 = rates_Q5[ind_tmp + 4]; rate1_Q5 = 280; } else { rate0_Q5 = ((280 - 43 * 4) + ((opus_int32)((opus_int16)(43))) * (opus_int32)((opus_int16)(ind_tmp))); rate1_Q5 = ((rate0_Q5) + (43)); }
}
      else if (ind_tmp <= -4) { if (ind_tmp == -4) { rate0_Q5 = 280; rate1_Q5 = rates_Q5[ind_tmp + 1 + 4]; } else { rate0_Q5 = ((280 - 43 * 4) + ((opus_int32)((opus_int16)(-43))) * (opus_int32)((opus_int16)(ind_tmp))); rate1_Q5 = ((rate0_Q5) - (43)); }
}
      else { rate0_Q5 = rates_Q5[ind_tmp + 4]; rate1_Q5 = rates_Q5[ind_tmp + 1 + 4];
}
      RD_tmp_Q25 = RD_Q25[j]; diff_Q10 = ((in_Q10) - (out0_Q10));
      RD_Q25[j] = (((((RD_tmp_Q25)) + (((((opus_int32)((opus_int16)(diff_Q10)) * (opus_int32)((opus_int16)(diff_Q10)))) * (w_Q5[i]))))) + ((opus_int32)((opus_int16)(mu_Q20))) * (opus_int32)((opus_int16)(rate0_Q5)));
      diff_Q10 = ((in_Q10) - (out1_Q10));
      RD_Q25[j + nStates] = (((((RD_tmp_Q25)) + (((((opus_int32)((opus_int16)(diff_Q10)) * (opus_int32)((opus_int16)(diff_Q10)))) * (w_Q5[i]))))) + ((opus_int32)((opus_int16)(mu_Q20))) * (opus_int32)((opus_int16)(rate1_Q5)));
}
    if (nStates <= (1 << 2) / 2) {
      for (j = 0; j < nStates; j++) { ind[j + nStates][i] = ind[j][i] + 1;
}
      nStates = ((opus_int32)((opus_uint32)(nStates) << (1)));
      for (j = nStates; j < (1 << 2); j++) { ind[j][i] = ind[j - nStates][i];
}
    } else {
      for (j = 0; j < (1 << 2); j++) {
        if (RD_Q25[j] > RD_Q25[j + (1 << 2)]) { RD_max_Q25[j] = RD_Q25[j]; RD_min_Q25[j] = RD_Q25[j + (1 << 2)]; RD_Q25[j] = RD_min_Q25[j]; RD_Q25[j + (1 << 2)] = RD_max_Q25[j]; out0_Q10 = prev_out_Q10[j]; prev_out_Q10[j] = prev_out_Q10[j + (1 << 2)]; prev_out_Q10[j + (1 << 2)] = out0_Q10; ind_sort[j] = j + (1 << 2); } else { RD_min_Q25[j] = RD_Q25[j]; RD_max_Q25[j] = RD_Q25[j + (1 << 2)]; ind_sort[j] = j; }
}
      for (auto separating_paths = true; separating_paths;) {
        min_max_Q25 = 0x7FFFFFFF; max_min_Q25 = 0; ind_min_max = 0; ind_max_min = 0;
        for (j = 0; j < (1 << 2); j++) {
          if (min_max_Q25 > RD_max_Q25[j]) { min_max_Q25 = RD_max_Q25[j]; ind_min_max = j;
}
          if (max_min_Q25 < RD_min_Q25[j]) { max_min_Q25 = RD_min_Q25[j]; ind_max_min = j; }
}
        if (min_max_Q25 >= max_min_Q25) {
          separating_paths = false;
          continue;
}
        ind_sort[ind_max_min] = ind_sort[ind_min_max] ^ (1 << 2); RD_Q25[ind_max_min] = RD_Q25[ind_min_max + (1 << 2)];
        prev_out_Q10[ind_max_min] = prev_out_Q10[ind_min_max + (1 << 2)]; RD_min_Q25[ind_max_min] = 0; RD_max_Q25[ind_min_max] = 0x7FFFFFFF;
        copy_n_bytes(ind[ind_min_max], static_cast<std::size_t>(16 * sizeof(opus_int8)), ind[ind_max_min]);
}
      for (j = 0; j < (1 << 2); j++) { ind[j][i] += ((ind_sort[j]) >> (2)); } }
}
  ind_tmp = 0; min_Q25 = 0x7FFFFFFF;
  for (j = 0; j < 2 * (1 << 2); j++) { if (min_Q25 > RD_Q25[j]) { min_Q25 = RD_Q25[j]; ind_tmp = j; }
}
  for (j = 0; j < order; j++) { indices[j] = ind[ind_tmp & ((1 << 2) - 1)][j]; opus_assume(indices[j] >= -10); opus_assume(indices[j] <= 10);
}
  indices[0] += ((ind_tmp) >> (2)); opus_assume(indices[0] <= 10);
  opus_assume(min_Q25 >= 0);
  return min_Q25;
}
void silk_process_NLSFs(silk_encoder_state *psEncC, opus_int16 PredCoef_Q12[2][16], opus_int16 pNLSF_Q15[16], const opus_int16 prev_NLSFq_Q15[16]) {
  int i, doInterpolate;
  int NLSF_mu_Q20;
  opus_int16 i_sqr_Q15; opus_int16 pNLSF0_temp_Q15[16]{};
  opus_int16 pNLSFW_QW[16]; opus_int16 pNLSFW0_temp_QW[16];
  opus_assume(psEncC->speech_activity_Q8 >= 0);
  opus_assume(psEncC->speech_activity_Q8 <= ((opus_int32)((1.0) * ((opus_int64)1 << (8)) + 0.5)));
  opus_assume(psEncC->useInterpolatedNLSFs == 1 || psEncC->indices.NLSFInterpCoef_Q2 == (1 << 2));
  NLSF_mu_Q20 = ((opus_int32)((((opus_int32)((0.003) * ((opus_int64)1 << (20)) + 0.5))) + (((((opus_int32)((-0.001) * ((opus_int64)1 << (28)) + 0.5))) * (opus_int64)((opus_int16)(psEncC->speech_activity_Q8))) >> 16)));
  if (psEncC->nb_subfr == 2) { NLSF_mu_Q20 = ((NLSF_mu_Q20) + (((NLSF_mu_Q20)) >> ((1))));
}
  opus_assume(NLSF_mu_Q20 > 0); opus_assume(NLSF_mu_Q20 <= ((opus_int32)((0.005) * ((opus_int64)1 << (20)) + 0.5)));
  silk_NLSF_VQ_weights_laroia(pNLSFW_QW, pNLSF_Q15, psEncC->predictLPCOrder);
  doInterpolate = (psEncC->useInterpolatedNLSFs == 1) && (psEncC->indices.NLSFInterpCoef_Q2 < 4);
  if (doInterpolate) { silk_interpolate(std::span<opus_int16>{pNLSF0_temp_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, std::span<const opus_int16>{prev_NLSFq_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, std::span<const opus_int16>{pNLSF_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, psEncC->indices.NLSFInterpCoef_Q2); silk_NLSF_VQ_weights_laroia(pNLSFW0_temp_QW, pNLSF0_temp_Q15, psEncC->predictLPCOrder); i_sqr_Q15 = ((opus_int32)((opus_uint32)(((opus_int32)((opus_int16)(psEncC->indices.NLSFInterpCoef_Q2)) * (opus_int32)((opus_int16)(psEncC->indices.NLSFInterpCoef_Q2)))) << (11))); for (i = 0; i < psEncC->predictLPCOrder; i++) { pNLSFW_QW[i] = ((((pNLSFW_QW[i]) >> (1))) + (((((opus_int32)((opus_int16)(pNLSFW0_temp_QW[i])) * (opus_int32)((opus_int16)(i_sqr_Q15)))) >> (16)))); opus_assume(pNLSFW_QW[i] >= 1); }
}
  silk_NLSF_encode(psEncC->indices.NLSFIndices, pNLSF_Q15, psEncC->psNLSF_CB, pNLSFW_QW, NLSF_mu_Q20, psEncC->NLSF_MSVQ_Survivors, psEncC->indices.signalType);
  silk_NLSF2A(PredCoef_Q12[1], pNLSF_Q15, psEncC->predictLPCOrder);
  if (doInterpolate) { silk_interpolate(std::span<opus_int16>{pNLSF0_temp_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, std::span<const opus_int16>{prev_NLSFq_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, std::span<const opus_int16>{pNLSF_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, psEncC->indices.NLSFInterpCoef_Q2); silk_NLSF2A(PredCoef_Q12[0], pNLSF0_temp_Q15, psEncC->predictLPCOrder);
}
  else { opus_assume(psEncC->predictLPCOrder <= 16); copy_n_bytes(PredCoef_Q12[1], static_cast<std::size_t>(psEncC->predictLPCOrder * sizeof(opus_int16)), PredCoef_Q12[0]); }
}
void silk_stereo_LR_to_MS(stereo_enc_state *state, opus_int16 x1[], opus_int16 x2[], opus_int8 ix[2][3], opus_int8 *mid_only_flag, opus_int32 mid_side_rates_bps[], opus_int32 total_rate_bps, int prev_speech_act_Q8, int toMono, int fs_kHz, int frame_length) {
  int n, is10msFrame, denom_Q16, delta0_Q13, delta1_Q13;
  opus_int32 sum, diff, smooth_coef_Q16, pred_Q13[2], pred0_Q13, pred1_Q13, LP_ratio_Q14, HP_ratio_Q14, frac_Q16, frac_3_Q16, min_mid_rate_bps, width_Q14, w_Q24, deltaw_Q24;
  opus_int16 *mid = &x1[-2];
  auto *side = OPUS_SCRATCH(opus_int16, frame_length + 2);
  for (n = 0; n < frame_length + 2; n++) {
    sum = x1[n - 2] + (opus_int32)x2[n - 2];
    diff = x1[n - 2] - (opus_int32)x2[n - 2];
    mid[n] = static_cast<opus_int16>(rounded_rshift<1>(sum));
    side[n] = rounded_rshift_to_int16<1>(diff);
}
  copy_n_bytes(state->sMid, static_cast<std::size_t>(2 * sizeof(opus_int16)), mid);
  copy_n_bytes(state->sSide, static_cast<std::size_t>(2 * sizeof(opus_int16)), side);
  copy_n_bytes(&mid[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sMid);
  copy_n_bytes(&side[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sSide);
  auto *LP_mid = OPUS_SCRATCH(opus_int16, frame_length);
  auto *HP_mid = OPUS_SCRATCH(opus_int16, frame_length);
  for (n = 0; n < frame_length; n++) {
    sum = rounded_rshift<2>((mid[n] + static_cast<opus_int32>(mid[n + 2])) + (static_cast<opus_int32>(static_cast<opus_uint32>(mid[n + 1])) << 1));
    LP_mid[n] = sum;
    HP_mid[n] = mid[n + 1] - sum;
}
  auto *LP_side = OPUS_SCRATCH(opus_int16, frame_length);
  auto *HP_side = OPUS_SCRATCH(opus_int16, frame_length);
  for (n = 0; n < frame_length; n++) {
    sum = rounded_rshift<2>((side[n] + static_cast<opus_int32>(side[n + 2])) + (static_cast<opus_int32>(static_cast<opus_uint32>(side[n + 1])) << 1));
    LP_side[n] = sum;
    HP_side[n] = side[n + 1] - sum;
}
  is10msFrame = frame_length == 10 * fs_kHz;
  smooth_coef_Q16 = is10msFrame ? ((opus_int32)((0.01 / 2) * ((opus_int64)1 << (16)) + 0.5)) : ((opus_int32)((0.01) * ((opus_int64)1 << (16)) + 0.5));
  smooth_coef_Q16 = ((opus_int32)(((((opus_int32)((opus_int16)(prev_speech_act_Q8)) * (opus_int32)((opus_int16)(prev_speech_act_Q8)))) * (opus_int64)((opus_int16)(smooth_coef_Q16))) >> 16));
  pred_Q13[0] = silk_stereo_find_predictor(&LP_ratio_Q14, LP_mid, LP_side, &state->mid_side_amp_Q0[0], frame_length, smooth_coef_Q16);
  pred_Q13[1] = silk_stereo_find_predictor(&HP_ratio_Q14, HP_mid, HP_side, &state->mid_side_amp_Q0[2], frame_length, smooth_coef_Q16);
  frac_Q16 = ((HP_ratio_Q14) + ((opus_int32)((opus_int16)(LP_ratio_Q14))) * (opus_int32)((opus_int16)(3)));
  frac_Q16 = (((frac_Q16) < (((opus_int32)((1) * ((opus_int64)1 << (16)) + 0.5)))) ? (frac_Q16) : (((opus_int32)((1) * ((opus_int64)1 << (16)) + 0.5))));
  total_rate_bps -= is10msFrame ? 1200 : 600;
  if (total_rate_bps < 1) { total_rate_bps = 1;
}
  min_mid_rate_bps = ((2000) + ((opus_int32)((opus_int16)(fs_kHz))) * (opus_int32)((opus_int16)(600)));
  opus_assume(min_mid_rate_bps < 32767); frac_3_Q16 = ((3) * (frac_Q16));
  mid_side_rates_bps[0] = silk_DIV32_varQ(total_rate_bps, ((opus_int32)((8 + 5) * ((opus_int64)1 << (16)) + 0.5)) + frac_3_Q16, 16 + 3);
  if (mid_side_rates_bps[0] < min_mid_rate_bps) { mid_side_rates_bps[0] = min_mid_rate_bps; mid_side_rates_bps[1] = total_rate_bps - mid_side_rates_bps[0]; width_Q14 = silk_DIV32_varQ(((opus_int32)((opus_uint32)(mid_side_rates_bps[1]) << (1))) - min_mid_rate_bps, ((opus_int32)(((((opus_int32)((1) * ((opus_int64)1 << (16)) + 0.5)) + frac_3_Q16) * (opus_int64)((opus_int16)(min_mid_rate_bps))) >> 16)), 14 + 2); width_Q14 = ((0) > (((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5))) ? ((width_Q14) > (0) ? (0) : ((width_Q14) < (((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5))) ? (((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5))) : (width_Q14))) : ((width_Q14) > (((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5))) ? (((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5))) : ((width_Q14) < (0) ? (0) : (width_Q14))));
}
  else { mid_side_rates_bps[1] = total_rate_bps - mid_side_rates_bps[0]; width_Q14 = ((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5));
}
  state->smth_width_Q14 = (opus_int16)((opus_int32)((state->smth_width_Q14) + (((width_Q14 - state->smth_width_Q14) * (opus_int64)((opus_int16)(smooth_coef_Q16))) >> 16)));
  *mid_only_flag = 0;
  const auto quantize_smoothed_pred = [&]() { pred_Q13[0] = ((((opus_int32)((opus_int16)(state->smth_width_Q14)) * (opus_int32)((opus_int16)(pred_Q13[0])))) >> (14)); pred_Q13[1] = ((((opus_int32)((opus_int16)(state->smth_width_Q14)) * (opus_int32)((opus_int16)(pred_Q13[1])))) >> (14)); silk_stereo_quant_pred(pred_Q13, ix); };
  const auto reset_side_prediction = [&]() { width_Q14 = 0; pred_Q13[0] = 0; pred_Q13[1] = 0; };
  const auto mid_only_state_score = [&]() { const auto was_mid_only = state->width_prev_Q14 == 0;
    return std::max(score_below(static_cast<float>(8 * total_rate_bps), static_cast<float>((was_mid_only ? 13 : 11) * min_mid_rate_bps)), score_below(static_cast<float>((static_cast<opus_int32>(((frac_Q16) * (opus_int64)((opus_int16)(state->smth_width_Q14))) >> 16))), static_cast<float>(was_mid_only ? ((opus_int32)((0.05) * ((opus_int64)1 << (14)) + 0.5)) : ((opus_int32)((0.02) * ((opus_int64)1 << (14)) + 0.5))))); };
  if (toMono) { reset_side_prediction(); silk_stereo_quant_pred(pred_Q13, ix);
}
  else if (mid_only_state_score() + silk_mid_only_score_bias * score_above(static_cast<float>(total_rate_bps), 30000.0f, 48000.0f)
           + silk_mid_only_low_speech_bias * min_score(score_centered(total_rate_bps, 24000, 4000), score_below(static_cast<float>(prev_speech_act_Q8), 32.0f)) > 0.0f) { quantize_smoothed_pred(); reset_side_prediction(); if (state->width_prev_Q14 == 0) { mid_side_rates_bps[0] = total_rate_bps; mid_side_rates_bps[1] = 0; *mid_only_flag = 1; }
}
  else if (state->smth_width_Q14 > ((opus_int32)((0.95) * ((opus_int64)1 << (14)) + 0.5))) { silk_stereo_quant_pred(pred_Q13, ix); width_Q14 = ((opus_int32)((1) * ((opus_int64)1 << (14)) + 0.5));
}
  else { quantize_smoothed_pred(); width_Q14 = state->smth_width_Q14;
}
  if (*mid_only_flag == 1) { state->silent_side_len += frame_length - 8 * fs_kHz; if (state->silent_side_len < 5 * fs_kHz) { *mid_only_flag = 0; } else { state->silent_side_len = 10000; } } else { state->silent_side_len = 0;
}
  if (*mid_only_flag == 0 && mid_side_rates_bps[1] < 1) { mid_side_rates_bps[1] = 1; mid_side_rates_bps[0] = std::max(1, total_rate_bps - mid_side_rates_bps[1]);
}
  pred0_Q13 = -state->pred_prev_Q13[0]; pred1_Q13 = -state->pred_prev_Q13[1];
  w_Q24 = ((opus_int32)((opus_uint32)(state->width_prev_Q14) << (10))); denom_Q16 = ((opus_int32)(((opus_int32)1 << 16) / (8 * fs_kHz)));
  delta0_Q13 = -rounded_i16_product_shift<16>(pred_Q13[0] - state->pred_prev_Q13[0], denom_Q16);
  delta1_Q13 = -rounded_i16_product_shift<16>(pred_Q13[1] - state->pred_prev_Q13[1], denom_Q16);
  deltaw_Q24 = ((opus_int32)((opus_uint32)(((opus_int32)(((width_Q14 - state->width_prev_Q14) * (opus_int64)((opus_int16)(denom_Q16))) >> 16))) << (10)));
  for (n = 0; n < 8 * fs_kHz; n++) { pred0_Q13 += delta0_Q13; pred1_Q13 += delta1_Q13; w_Q24 += deltaw_Q24; sum = ((opus_int32)((opus_uint32)((((mid[n] + (opus_int32)mid[n + 2])) + (((opus_int32)((opus_uint32)((mid[n + 1])) << ((1))))))) << (9))); sum = ((opus_int32)((((opus_int32)(((w_Q24) * (opus_int64)((opus_int16)(side[n + 1]))) >> 16))) + (((sum) * (opus_int64)((opus_int16)(pred0_Q13))) >> 16))); sum = ((opus_int32)((sum) + (((((opus_int32)((opus_uint32)((opus_int32)mid[n + 1]) << (11)))) * (opus_int64)((opus_int16)(pred1_Q13))) >> 16))); x2[n - 1] = rounded_rshift_to_int16<8>(sum);
}
  pred0_Q13 = -pred_Q13[0]; pred1_Q13 = -pred_Q13[1]; w_Q24 = ((opus_int32)((opus_uint32)(width_Q14) << (10)));
  for (n = 8 * fs_kHz; n < frame_length; n++) { sum = ((opus_int32)((opus_uint32)((((mid[n] + (opus_int32)mid[n + 2])) + (((opus_int32)((opus_uint32)((mid[n + 1])) << ((1))))))) << (9))); sum = ((opus_int32)((((opus_int32)(((w_Q24) * (opus_int64)((opus_int16)(side[n + 1]))) >> 16))) + (((sum) * (opus_int64)((opus_int16)(pred0_Q13))) >> 16))); sum = ((opus_int32)((sum) + (((((opus_int32)((opus_uint32)((opus_int32)mid[n + 1]) << (11)))) * (opus_int64)((opus_int16)(pred1_Q13))) >> 16))); x2[n - 1] = rounded_rshift_to_int16<8>(sum);
}
  state->pred_prev_Q13[0] = (opus_int16)pred_Q13[0]; state->pred_prev_Q13[1] = (opus_int16)pred_Q13[1];
  state->width_prev_Q14 = (opus_int16)width_Q14;
}
void silk_stereo_MS_to_LR(stereo_dec_state *state, opus_int16 x1[], opus_int16 x2[], const opus_int32 pred_Q13[], int fs_kHz, int frame_length) {
  int n, denom_Q16, delta0_Q13, delta1_Q13;
  opus_int32 sum, diff, pred0_Q13, pred1_Q13;
  copy_n_bytes(state->sMid, static_cast<std::size_t>(2 * sizeof(opus_int16)), x1);
  copy_n_bytes(state->sSide, static_cast<std::size_t>(2 * sizeof(opus_int16)), x2);
  copy_n_bytes(&x1[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sMid);
  copy_n_bytes(&x2[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sSide);
  pred0_Q13 = state->pred_prev_Q13[0]; pred1_Q13 = state->pred_prev_Q13[1];
  denom_Q16 = ((opus_int32)(((opus_int32)1 << 16) / (8 * fs_kHz)));
  delta0_Q13 = rounded_i16_product_shift<16>(pred_Q13[0] - state->pred_prev_Q13[0], denom_Q16);
  delta1_Q13 = rounded_i16_product_shift<16>(pred_Q13[1] - state->pred_prev_Q13[1], denom_Q16);
  for (n = 0; n < 8 * fs_kHz; n++) { pred0_Q13 += delta0_Q13; pred1_Q13 += delta1_Q13; sum = ((opus_int32)((opus_uint32)((((x1[n] + (opus_int32)x1[n + 2])) + (((opus_int32)((opus_uint32)((x1[n + 1])) << ((1))))))) << (9))); sum = ((opus_int32)((((opus_int32)((opus_uint32)((opus_int32)x2[n + 1]) << (8)))) + (((sum) * (opus_int64)((opus_int16)(pred0_Q13))) >> 16))); sum = ((opus_int32)((sum) + (((((opus_int32)((opus_uint32)((opus_int32)x1[n + 1]) << (11)))) * (opus_int64)((opus_int16)(pred1_Q13))) >> 16))); x2[n + 1] = rounded_rshift_to_int16<8>(sum);
}
  pred0_Q13 = pred_Q13[0]; pred1_Q13 = pred_Q13[1];
  for (n = 8 * fs_kHz; n < frame_length; n++) { sum = ((opus_int32)((opus_uint32)((((x1[n] + (opus_int32)x1[n + 2])) + (((opus_int32)((opus_uint32)((x1[n + 1])) << ((1))))))) << (9))); sum = ((opus_int32)((((opus_int32)((opus_uint32)((opus_int32)x2[n + 1]) << (8)))) + (((sum) * (opus_int64)((opus_int16)(pred0_Q13))) >> 16))); sum = ((opus_int32)((sum) + (((((opus_int32)((opus_uint32)((opus_int32)x1[n + 1]) << (11)))) * (opus_int64)((opus_int16)(pred1_Q13))) >> 16))); x2[n + 1] = rounded_rshift_to_int16<8>(sum);
}
  state->pred_prev_Q13[0] = pred_Q13[0]; state->pred_prev_Q13[1] = pred_Q13[1];
  for (n = 0; n < frame_length; n++) { sum = x1[n+1]+(opus_int32)x2[n+1]; diff = x1[n+1]-(opus_int32)x2[n+1]; x1[n+1] = saturate_int16_from_int32(sum); x2[n+1] = saturate_int16_from_int32(diff); }
}
int check_control_input(silk_EncControlStruct *encControl) {
  opus_assume(encControl != nullptr);
  if (((encControl->API_sampleRate != 8000) && (encControl->API_sampleRate != 12000) && (encControl->API_sampleRate != 16000) && (encControl->API_sampleRate != 24000) && (encControl->API_sampleRate != 32000) && (encControl->API_sampleRate != 44100) && (encControl->API_sampleRate != 48000)) || ((encControl->desiredInternalSampleRate != 8000) && (encControl->desiredInternalSampleRate != 12000) && (encControl->desiredInternalSampleRate != 16000)) || ((encControl->maxInternalSampleRate != 8000) && (encControl->maxInternalSampleRate != 12000) && (encControl->maxInternalSampleRate != 16000)) || ((encControl->minInternalSampleRate != 8000) && (encControl->minInternalSampleRate != 12000) && (encControl->minInternalSampleRate != 16000)) || (encControl->minInternalSampleRate > encControl->desiredInternalSampleRate) || (encControl->maxInternalSampleRate < encControl->desiredInternalSampleRate) || (encControl->minInternalSampleRate > encControl->maxInternalSampleRate)) { return -102;
}
  if (encControl->payloadSize_ms != 10 && encControl->payloadSize_ms != 20 && encControl->payloadSize_ms != 40 && encControl->payloadSize_ms != 60) { return -103;
}
  if (encControl->useCBR < 0 || encControl->useCBR > 1) { return -109;
}
  if (encControl->nChannelsAPI < 1 || encControl->nChannelsAPI > 2) { return -111;
}
  if (encControl->nChannelsInternal < 1 || encControl->nChannelsInternal > 2) { return -111;
}
  if (encControl->nChannelsInternal > encControl->nChannelsAPI) { return -111;
}
  if (encControl->complexity < 0 || encControl->complexity > 10) { return -106;
}
  return 0;
}
constexpr std::array<unsigned char, 117 - 10> silk_TargetRate_NB_21 = numeric_blob_array<unsigned char, 107>(R"blob(000F27343D444A4F54585C5F6366696C6F7275777A7C7E81838587898B8E8F91939597999B9D9EA0A2A3A5A7A8AAABADAEB0B1B3B4B6B7B9BABBBDBEC0C1C2C4C5C7C8C9CBCCCDCFD0D1D3D4D5D7D8D9DBDCDDDFE0E1E3E4E6E7E8EAEBECEEEFF1F2F3F5F6F8F9FAFCFDFF)blob");
constexpr std::array<unsigned char, 165 - 10> silk_TargetRate_MB_21 = numeric_blob_array<unsigned char, 155>(R"blob(00001C2B343B41464A4E5155575A5D5F626466696B6D6F71737476787A7B7D7F808283858688898A8C8D8F90919394959798999A9C9D9E9FA0A2A3A4A5A6A7A8A9ABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBCBDBEBFC0C1C2C3C4C5C6C7C8C9CACBCBCCCDCECFD0D1D2D3D4D5D6D6D7D8D9DADBDCDDDEDFE0E0E1E2E3E4E5E6E7E8E9EAEBECECEDEEEFF0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF)blob");
constexpr std::array<unsigned char, 201 - 10> silk_TargetRate_WB_21 = numeric_blob_array<unsigned char, 191>(R"blob(000000081D2931383E42464A4D505356585B5D5F61636567696B6C6E707173747677797A7B7D7E7F81828384868788898A8C8D8E8F909192939495969798999A9C9D9E9F9FA0A1A2A3A4A5A6A7A8A9AAABABACADAEAFB0B1B1B2B3B4B5B5B6B7B8B9B9BABBBCBDBDBEBFC0C0C1C2C3C3C4C5C6C6C7C8C8C9CACBCBCCCDCECECFD0D1D1D2D3D3D4D5D6D6D7D8D8D9DADBDBDCDDDDDEDFE0E0E1E2E2E3E4E5E5E6E7E8E8E9EAEAEBECEDEDEEEFF0F0F1F2F3F3F4F5F6F6F7F8F9F9FAFBFCFDFF)blob");
static_assert(array_is_non_decreasing(silk_TargetRate_NB_21));
static_assert(array_is_non_decreasing(silk_TargetRate_MB_21));
static_assert(array_is_non_decreasing(silk_TargetRate_WB_21));
int silk_control_SNR(silk_encoder_state *psEncC, opus_int32 TargetRate_bps) {
  int id, bound;
  std::span<const unsigned char> snr_table; psEncC->TargetRate_bps = TargetRate_bps;
  if (psEncC->nb_subfr == 2) { TargetRate_bps -= 2000 + psEncC->fs_kHz / 16;
}
  if (psEncC->fs_kHz == 8) { snr_table = silk_TargetRate_NB_21; } else if (psEncC->fs_kHz == 12) { snr_table = silk_TargetRate_MB_21; } else { snr_table = silk_TargetRate_WB_21;
}
  bound = static_cast<int>(snr_table.size()); id = (TargetRate_bps + 200) / 400; id = (((id - 10) < (bound - 1)) ? (id - 10) : (bound - 1));
  if (id <= 0) { psEncC->SNR_dB_Q7 = 0; } else { psEncC->SNR_dB_Q7 = snr_table[static_cast<std::size_t>(id)] * 21;
}
  return 0;
}
int silk_init_encoder(silk_encoder_state_FLP *psEnc) {
  int ret = 0;
  zero_n_bytes(psEnc, static_cast<std::size_t>(sizeof(silk_encoder_state_FLP)));
  psEnc->sCmn.variable_HP_smth1_Q15 = ((opus_int32)((opus_uint32)(silk_lin2log(((opus_int32)((60) * ((opus_int64)1 << (16)) + 0.5))) - (16 << 7)) << (8)));
  psEnc->sCmn.variable_HP_smth2_Q15 = psEnc->sCmn.variable_HP_smth1_Q15; psEnc->sCmn.first_frame_after_reset = 1;
  ret += silk_VAD_Init(&psEnc->sCmn.sVAD);
  return ret;
}
namespace {
extern constinit const std::array<std::array<opus_int8, 11>, 4> silk_CB_lags_stage2;
extern constinit const std::array<std::array<opus_int8, 34>, 4> silk_CB_lags_stage3;
extern constinit const std::array<std::array<std::array<opus_int8, 2>, 4>, 2 + 1> silk_Lag_range_stage3;
extern constinit const std::array<opus_int8, 2 + 1> silk_nb_cbk_searchs_stage3;
extern constinit const std::array<std::array<opus_int8, 3>, 2> silk_CB_lags_stage2_10_ms;
extern constinit const std::array<std::array<opus_int8, 12>, 2> silk_CB_lags_stage3_10_ms;
extern constinit const std::array<std::array<opus_int8, 2>, 2> silk_Lag_range_stage3_10_ms;
}
static int silk_setup_resamplers(silk_encoder_state_FLP *psEnc, int fs_kHz);
static int silk_setup_fs(silk_encoder_state_FLP *psEnc, int fs_kHz, int PacketSize_ms);
static int silk_setup_complexity(silk_encoder_state *psEncC, int Complexity);
int silk_control_encoder(silk_encoder_state_FLP *psEnc, silk_EncControlStruct *encControl, const int allow_bw_switch, const int force_fs_kHz) {
  int fs_kHz, ret = 0;
  psEnc->sCmn.useCBR = encControl->useCBR;
  psEnc->sCmn.API_fs_Hz = encControl->API_sampleRate; psEnc->sCmn.maxInternal_fs_Hz = encControl->maxInternalSampleRate;
  psEnc->sCmn.minInternal_fs_Hz = encControl->minInternalSampleRate;
  psEnc->sCmn.desiredInternal_fs_Hz = encControl->desiredInternalSampleRate;
  psEnc->sCmn.nChannelsAPI = encControl->nChannelsAPI;
  psEnc->sCmn.nChannelsInternal = encControl->nChannelsInternal; psEnc->sCmn.allow_bandwidth_switch = allow_bw_switch;
  if (psEnc->sCmn.controlled_since_last_payload != 0 && psEnc->sCmn.prefillFlag == 0) {
    if (psEnc->sCmn.API_fs_Hz != psEnc->sCmn.prev_API_fs_Hz && psEnc->sCmn.fs_kHz > 0) { ret += silk_setup_resamplers(psEnc, psEnc->sCmn.fs_kHz);
}
    return ret;
}
  fs_kHz = silk_control_audio_bandwidth(&psEnc->sCmn, encControl);
  if (force_fs_kHz) { fs_kHz = force_fs_kHz;
}
  ret += silk_setup_resamplers(psEnc, fs_kHz); ret += silk_setup_fs(psEnc, fs_kHz, encControl->payloadSize_ms);
  ret += silk_setup_complexity(&psEnc->sCmn, encControl->complexity);
  psEnc->sCmn.controlled_since_last_payload = 1;
  return ret;
}
static int silk_setup_resamplers(silk_encoder_state_FLP *psEnc, int fs_kHz) {
  int ret = 0;
  if (psEnc->sCmn.fs_kHz != fs_kHz || psEnc->sCmn.prev_API_fs_Hz != psEnc->sCmn.API_fs_Hz) {
    if (psEnc->sCmn.fs_kHz == 0) { ret += silk_resampler_init(&psEnc->sCmn.resampler_state, psEnc->sCmn.API_fs_Hz, fs_kHz * 1000, 1);
}
    else {
      opus_int32 new_buf_samples, api_buf_samples, old_buf_samples;
      opus_int32 buf_length_ms = ((opus_int32)((opus_uint32)(psEnc->sCmn.nb_subfr * 5) << (1))) + 5;
      old_buf_samples = buf_length_ms * psEnc->sCmn.fs_kHz; new_buf_samples = buf_length_ms * fs_kHz;
      auto *x_bufFIX = OPUS_SCRATCH(opus_int16, (((old_buf_samples) > (new_buf_samples)) ? (old_buf_samples) : (new_buf_samples)));
      silk_float2short_array(x_bufFIX, psEnc->x_buf, old_buf_samples); silk_resampler_state_struct temp_resampler_state[1];
      ret += silk_resampler_init(temp_resampler_state, ((opus_int32)((opus_int16)(psEnc->sCmn.fs_kHz)) * (opus_int32)((opus_int16)(1000))), psEnc->sCmn.API_fs_Hz, 0);
      api_buf_samples = buf_length_ms * ((opus_int32)((psEnc->sCmn.API_fs_Hz) / (1000)));
      auto *x_buf_API_fs_Hz = OPUS_SCRATCH(opus_int16, api_buf_samples);
      ret += silk_resampler(temp_resampler_state, x_buf_API_fs_Hz, x_bufFIX, old_buf_samples);
      ret += silk_resampler_init(&psEnc->sCmn.resampler_state, psEnc->sCmn.API_fs_Hz, ((opus_int32)((opus_int16)(fs_kHz)) * (opus_int32)((opus_int16)(1000))), 1);
      ret += silk_resampler(&psEnc->sCmn.resampler_state, x_bufFIX, x_buf_API_fs_Hz, api_buf_samples);
      silk_short2float_array(psEnc->x_buf, x_bufFIX, new_buf_samples); }
}
  psEnc->sCmn.prev_API_fs_Hz = psEnc->sCmn.API_fs_Hz;
  return ret;
}
static int silk_setup_fs(silk_encoder_state_FLP *psEnc, int fs_kHz, int PacketSize_ms) {
  int ret = 0;
  if (PacketSize_ms != psEnc->sCmn.PacketSize_ms) {
    if ((PacketSize_ms != 10) && (PacketSize_ms != 20) && (PacketSize_ms != 40) && (PacketSize_ms != 60)) { ret = -103;
}
    if (PacketSize_ms <= 10) { psEnc->sCmn.nFramesPerPacket = 1; psEnc->sCmn.nb_subfr = PacketSize_ms == 10 ? 2 : 1; psEnc->sCmn.frame_length = ((opus_int32)((opus_int16)(PacketSize_ms)) * (opus_int32)((opus_int16)(fs_kHz))); psEnc->sCmn.pitch_LPC_win_length = ((opus_int32)((opus_int16)((10 + (2 << 1)))) * (opus_int32)((opus_int16)(fs_kHz)));
}
    else { psEnc->sCmn.nFramesPerPacket = ((opus_int32)((PacketSize_ms) / ((5 * 4)))); psEnc->sCmn.nb_subfr = 4; psEnc->sCmn.frame_length = ((opus_int32)((opus_int16)(20)) * (opus_int32)((opus_int16)(fs_kHz))); psEnc->sCmn.pitch_LPC_win_length = ((opus_int32)((opus_int16)((20 + (2 << 1)))) * (opus_int32)((opus_int16)(fs_kHz)));
}
    psEnc->sCmn.pitch_contour_iCDF = silk_pitch_contour_icdf(fs_kHz, psEnc->sCmn.nb_subfr).data();
    psEnc->sCmn.PacketSize_ms = PacketSize_ms; psEnc->sCmn.TargetRate_bps = 0;
}
  opus_assume(fs_kHz == 8 || fs_kHz == 12 || fs_kHz == 16);
  opus_assume(psEnc->sCmn.nb_subfr == 2 || psEnc->sCmn.nb_subfr == 4);
  if (psEnc->sCmn.fs_kHz != fs_kHz) {
    zero_n_bytes(&psEnc->sShape, static_cast<std::size_t>(sizeof(psEnc->sShape)));
    zero_n_bytes(&psEnc->sCmn.sNSQ, static_cast<std::size_t>(sizeof(psEnc->sCmn.sNSQ)));
    zero_n_bytes(psEnc->sCmn.prev_NLSFq_Q15, static_cast<std::size_t>(sizeof(psEnc->sCmn.prev_NLSFq_Q15)));
    zero_n_bytes(&psEnc->sCmn.sLP.In_LP_State, static_cast<std::size_t>(sizeof(psEnc->sCmn.sLP.In_LP_State)));
    psEnc->sCmn.inputBufIx = 0; psEnc->sCmn.nFramesEncoded = 0; psEnc->sCmn.TargetRate_bps = 0; psEnc->sCmn.prevLag = 100;
    psEnc->sCmn.first_frame_after_reset = 1; psEnc->sShape.LastGainIndex = 10;
    psEnc->sCmn.sNSQ.lagPrev = 100; psEnc->sCmn.sNSQ.prev_gain_Q16 = 65536; psEnc->sCmn.prevSignalType = 0; psEnc->sCmn.fs_kHz = fs_kHz;
    psEnc->sCmn.pitch_contour_iCDF = silk_pitch_contour_icdf(psEnc->sCmn.fs_kHz, psEnc->sCmn.nb_subfr).data();
    const auto nlsf_choice = silk_nlsf_codebook_for_fs(psEnc->sCmn.fs_kHz);
    psEnc->sCmn.predictLPCOrder = nlsf_choice.order; psEnc->sCmn.psNLSF_CB = nlsf_choice.codebook; psEnc->sCmn.subfr_length = 5 * fs_kHz;
    psEnc->sCmn.frame_length = ((opus_int32)((opus_int16)(psEnc->sCmn.subfr_length)) * (opus_int32)((opus_int16)(psEnc->sCmn.nb_subfr)));
    psEnc->sCmn.ltp_mem_length = ((opus_int32)((opus_int16)(20)) * (opus_int32)((opus_int16)(fs_kHz)));
    psEnc->sCmn.la_pitch = ((opus_int32)((opus_int16)(2)) * (opus_int32)((opus_int16)(fs_kHz)));
    if (psEnc->sCmn.nb_subfr == 4) {
      psEnc->sCmn.pitch_LPC_win_length = ((opus_int32)((opus_int16)((20 + (2 << 1)))) * (opus_int32)((opus_int16)(fs_kHz)));
    } else { psEnc->sCmn.pitch_LPC_win_length = ((opus_int32)((opus_int16)((10 + (2 << 1)))) * (opus_int32)((opus_int16)(fs_kHz)));
}
    psEnc->sCmn.pitch_lag_low_bits_iCDF = silk_pitch_lag_low_bits_icdf(psEnc->sCmn.fs_kHz).data();
}
  opus_assume((psEnc->sCmn.subfr_length * psEnc->sCmn.nb_subfr) == psEnc->sCmn.frame_length);
  return ret;
}
static int silk_setup_complexity(silk_encoder_state *psEncC, int Complexity) {
  opus_assume(Complexity >= 0 && Complexity <= 10);
  struct complexity_tier { opus_int8 pec, pelpc, slpc, la_mult, nsd, uin, msvq, warped; opus_int32 pet_q16; };
  static constexpr std::array<complexity_tier, 7> tiers{{{0, 6, 12, 3, 1, 0, 2, 0, static_cast<opus_int32>((0.80) * (opus_int64{1} << 16) + 0.5)}, {1, 8, 14, 5, 1, 0, 3, 0, static_cast<opus_int32>((0.76) * (opus_int64{1} << 16) + 0.5)}, {0, 6, 12, 3, 2, 0, 2, 0, static_cast<opus_int32>((0.80) * (opus_int64{1} << 16) + 0.5)}, {1, 8, 14, 5, 2, 0, 4, 0, static_cast<opus_int32>((0.76) * (opus_int64{1} << 16) + 0.5)}, {1, 10, 16, 5, 2, 1, 6, 1, static_cast<opus_int32>((0.74) * (opus_int64{1} << 16) + 0.5)}, {1, 12, 20, 5, 3, 1, 8, 1, static_cast<opus_int32>((0.72) * (opus_int64{1} << 16) + 0.5)}, {2, 16, 24, 5, 4, 1, 16, 1, static_cast<opus_int32>((0.70) * (opus_int64{1} << 16) + 0.5)}}};
  static constexpr std::array<opus_uint8, 11> complexity_to_tier{0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6};
  const auto &t = tiers[complexity_to_tier[Complexity]];
  psEncC->pitchEstimationComplexity = t.pec;
  psEncC->pitchEstimationThreshold_Q16 = t.pet_q16;
  psEncC->pitchEstimationLPCOrder = std::min<int>(t.pelpc, psEncC->predictLPCOrder);
  psEncC->shapingLPCOrder = t.slpc;
  psEncC->la_shape = t.la_mult * psEncC->fs_kHz;
  psEncC->nStatesDelayedDecision = t.nsd;
  psEncC->useInterpolatedNLSFs = t.uin;
  psEncC->NLSF_MSVQ_Survivors = t.msvq;
  psEncC->warping_Q16 = t.warped ? psEncC->fs_kHz * static_cast<opus_int32>((0.015f) * (opus_int64{1} << 16) + 0.5) : 0;
  psEncC->shapeWinLength = 5 * psEncC->fs_kHz + 2 * psEncC->la_shape;
  psEncC->Complexity = Complexity;
  opus_assume(psEncC->pitchEstimationLPCOrder <= 16); opus_assume(psEncC->shapingLPCOrder <= 24);
  opus_assume(psEncC->nStatesDelayedDecision <= 4); opus_assume(psEncC->warping_Q16 <= 32767);
  opus_assume(psEncC->la_shape <= (5 * 16)); opus_assume(psEncC->shapeWinLength <= (15 * 16));
  return 0;
}
static void silk_A2NLSF_trans_poly(opus_int32 *p, const int dd) {
  int k, n;
  for (k = 2; k <= dd; k++) { for (n = dd; n > k; n--) { p[n - 2] -= p[n]; } p[k - 2] -= ((opus_int32)((opus_uint32)(p[k]) << (1))); }
}
static opus_int32 silk_A2NLSF_eval_poly(opus_int32 *p, const opus_int32 x, const int dd) {
  int n;
  opus_int32 x_Q16, y32; y32 = p[dd];
  x_Q16 = ((opus_int32)((opus_uint32)(x) << (4)));
  if ((__builtin_expect(!!(8 == dd), 1))) {
    y32 = ((opus_int32)((p[7]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[6]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[5]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[4]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[3]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[2]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[1]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
    y32 = ((opus_int32)((p[0]) + (((opus_int64)(y32) * (x_Q16)) >> 16)));
  } else {
    for (n = dd - 1; n >= 0; n--) { y32 = ((opus_int32)((p[n]) + (((opus_int64)(y32) * (x_Q16)) >> 16))); }
}
  return y32;
}
static void silk_A2NLSF_init(const opus_int32 *a_Q16, opus_int32 *P, opus_int32 *Q, const int dd) {
  int k;
  P[dd] = ((opus_int32)((opus_uint32)(1) << (16))); Q[dd] = ((opus_int32)((opus_uint32)(1) << (16)));
  for (k = 0; k < dd; k++) { P[k] = -a_Q16[dd - k - 1] - a_Q16[dd + k]; Q[k] = -a_Q16[dd - k - 1] + a_Q16[dd + k];
}
  for (k = dd; k > 0; k--) { P[k - 1] -= P[k]; Q[k - 1] += Q[k];
}
  silk_A2NLSF_trans_poly(P, dd); silk_A2NLSF_trans_poly(Q, dd);
}
void silk_A2NLSF(opus_int16 *NLSF, opus_int32 *a_Q16, const int d) {
  int i, k, m, dd, root_ix, ffrac;
  opus_int32 xlo, xhi, xmid; opus_int32 ylo, yhi, ymid, thr;
  opus_int32 nom, den; opus_int32 P[24 / 2 + 1];
  opus_int32 Q[24 / 2 + 1]; opus_int32 *PQ[2];
  opus_int32 *p; PQ[0] = P;
  PQ[1] = Q; dd = ((d) >> (1)); silk_A2NLSF_init(a_Q16, P, Q, dd); p = P;
  xlo = silk_LSFCosTab_FIX_Q12[0]; ylo = silk_A2NLSF_eval_poly(p, xlo, dd);
  if (ylo < 0) {
    NLSF[0] = 0; p = Q; ylo = silk_A2NLSF_eval_poly(p, xlo, dd); root_ix = 1;
  } else { root_ix = 0;
}
  k = 1; i = 0; thr = 0;
  for (; root_ix < d;) {
    xhi = silk_LSFCosTab_FIX_Q12[k]; yhi = silk_A2NLSF_eval_poly(p, xhi, dd);
    if ((ylo <= 0 && yhi >= thr) || (ylo >= 0 && yhi <= -thr)) {
      if (yhi == 0) {
        thr = 1;
      } else { thr = 0;
}
      ffrac = -256;
      for (m = 0; m < 3; m++) {
        xmid = rounded_rshift<1>(xlo + xhi); ymid = silk_A2NLSF_eval_poly(p, xmid, dd);
        if ((ylo <= 0 && ymid >= 0) || (ylo >= 0 && ymid <= 0)) {
          xhi = xmid; yhi = ymid;
        } else { xlo = xmid; ylo = ymid; ffrac = ((ffrac) + (((128)) >> ((m)))); }
}
      if ((((ylo) > 0) ? (ylo) : -(ylo)) < 65536) {
        den = ylo - yhi; nom = ((opus_int32)((opus_uint32)(ylo) << (8 - 3))) + ((den) >> (1));
        if (den != 0) { ffrac += ((opus_int32)((nom) / (den)));
}
      } else { ffrac += ((opus_int32)((ylo) / (((ylo - yhi) >> (8 - 3)))));
}
      NLSF[root_ix] = (opus_int16)std::min(((opus_int32)((opus_uint32)((opus_int32)k) << (8))) + ffrac, 0x7FFF);
      opus_assume(NLSF[root_ix] >= 0); root_ix++;
      if (root_ix >= d) { continue;
}
      p = PQ[root_ix & 1]; xlo = silk_LSFCosTab_FIX_Q12[k - 1]; ylo = ((opus_int32)((opus_uint32)(1 - (root_ix & 2)) << (12)));
    } else {
      k++; xlo = xhi; ylo = yhi; thr = 0;
      if (k > 128) {
        i++;
        if (i > 16) {
          NLSF[0] = (opus_int16)((opus_int32)((1 << 15) / (d + 1)));
          for (k = 1; k < d; k++) { NLSF[k] = (opus_int16)((NLSF[k - 1]) + (NLSF[0]));
}
          return;
}
    silk_bwexpander_32(std::span<opus_int32>{a_Q16, static_cast<std::size_t>(d)}, 65536 - ((opus_int32)((opus_uint32)(1) << (i)))); silk_A2NLSF_init(a_Q16, P, Q, dd); p = P;
        xlo = silk_LSFCosTab_FIX_Q12[0]; ylo = silk_A2NLSF_eval_poly(p, xlo, dd);
        if (ylo < 0) {
          NLSF[0] = 0; p = Q; ylo = silk_A2NLSF_eval_poly(p, xlo, dd); root_ix = 1;
        } else { root_ix = 0;
}
        k = 1; } } }
}
constexpr opus_int16 A_fb1_20 = 5394 << 1;
constexpr opus_int16 A_fb1_21 = -24290;
void silk_ana_filt_bank_1(const opus_int16 *in, opus_int32 *S, opus_int16 *outL, opus_int16 *outH, const opus_int32 N) {
  int k, N2 = ((N) >> (1));
  opus_int32 in32, X, Y, out_1, out_2;
  for (k = 0; k < N2; k++) { in32 = ((opus_int32)((opus_uint32)((opus_int32)in[2 * k]) << (10))); Y = ((in32) - (S[0])); X = ((opus_int32)((Y) + (((Y) * (opus_int64)((opus_int16)(A_fb1_21))) >> 16))); out_1 = ((S[0]) + (X)); S[0] = ((in32) + (X)); in32 = ((opus_int32)((opus_uint32)((opus_int32)in[2 * k + 1]) << (10))); Y = ((in32) - (S[1])); X = ((opus_int32)(((Y) * (opus_int64)((opus_int16)(A_fb1_20))) >> 16)); out_2 = ((S[1]) + (X)); S[1] = ((in32) + (X)); outL[k] = rounded_rshift_to_int16<11>(saturating_add_int32(out_2, out_1)); outH[k] = rounded_rshift_to_int16<11>(saturating_subtract_int32(out_2, out_1)); }
}
void silk_biquad_alt_stride1(const opus_int16 *in, const opus_int32 *B_Q28, const opus_int32 *A_Q28, opus_int32 *S, opus_int16 *out, const opus_int32 len) {
  int k;
  opus_int32 inval, A0_U_Q28, A0_L_Q28, A1_U_Q28, A1_L_Q28, out32_Q14; A0_L_Q28 = (-A_Q28[0]) & 0x00003FFF;
  A0_U_Q28 = ((-A_Q28[0]) >> (14)); A1_L_Q28 = (-A_Q28[1]) & 0x00003FFF; A1_U_Q28 = ((-A_Q28[1]) >> (14));
  for (k = 0; k < len; k++) { inval = in[k]; out32_Q14 = ((opus_int32)((opus_uint32)(((opus_int32)((S[0]) + (((B_Q28[0]) * (opus_int64)((opus_int16)(inval))) >> 16)))) << (2))); S[0] = S[1] + rounded_mul_i16_q16<14>(out32_Q14, A0_L_Q28); S[0] = ((opus_int32)((S[0]) + (((out32_Q14) * (opus_int64)((opus_int16)(A0_U_Q28))) >> 16))); S[0] = ((opus_int32)((S[0]) + (((B_Q28[1]) * (opus_int64)((opus_int16)(inval))) >> 16))); S[1] = rounded_mul_i16_q16<14>(out32_Q14, A1_L_Q28); S[1] = ((opus_int32)((S[1]) + (((out32_Q14) * (opus_int64)((opus_int16)(A1_U_Q28))) >> 16))); S[1] = ((opus_int32)((S[1]) + (((B_Q28[2]) * (opus_int64)((opus_int16)(inval))) >> 16))); out[k] = saturate_int16_from_int32((out32_Q14 + (1 << 14) - 1) >> 14); }
}
void silk_bwexpander_32(std::span<opus_int32> ar, opus_int32 chirp_Q16) {
  if (ar.empty()) { return;
}
  const opus_int32 chirp_minus_one_Q16 = chirp_Q16 - 65536;
  for (auto index = std::size_t{}; index + 1 < ar.size(); ++index) {
    ar[index] = static_cast<opus_int32>((static_cast<opus_int64>(chirp_Q16) * ar[index]) >> 16);
    chirp_Q16 += static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(chirp_Q16) * chirp_minus_one_Q16));
}
  ar.back() = static_cast<opus_int32>((static_cast<opus_int64>(chirp_Q16) * ar.back()) >> 16);
}
void silk_bwexpander(std::span<opus_int16> ar, opus_int32 chirp_Q16) {
  if (ar.empty()) { return;
}
  const opus_int32 chirp_minus_one_Q16 = chirp_Q16 - 65536;
  for (auto index = std::size_t{}; index + 1 < ar.size(); ++index) {
    ar[index] = static_cast<opus_int16>(rounded_rshift<16>(static_cast<opus_int64>(chirp_Q16) * ar[index]));
    chirp_Q16 += static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(chirp_Q16) * chirp_minus_one_Q16));
}
  ar.back() = static_cast<opus_int16>(rounded_rshift<16>(static_cast<opus_int64>(chirp_Q16) * ar.back()));
}
template <typename T, std::size_t Rows, std::size_t Columns> [[nodiscard]] constexpr auto flat_table_span(const std::array<std::array<T, Columns>, Rows> &table) noexcept -> std::span<const T> { return {table[0].data(), Rows * Columns};
}
template <typename T, std::size_t Outer, std::size_t Rows, std::size_t Columns> [[nodiscard]] constexpr auto flat_table_span(const std::array<std::array<std::array<T, Columns>, Rows>, Outer> &table, const std::size_t index) noexcept -> std::span<const T> { return {table[index][0].data(), Rows * Columns};
}
struct silk_lag_codebook_view { std::span<const opus_int8> entries; int cbk_size; int nb_cbk_search; [[nodiscard]] constexpr auto at(const int subframe, const int codebook_index) const noexcept -> opus_int8 { return entries[static_cast<std::size_t>(subframe * cbk_size + codebook_index)]; } };
struct silk_lag_range_view { std::span<const opus_int8> entries; [[nodiscard]] constexpr auto low(const int subframe) const noexcept -> opus_int8 { return entries[static_cast<std::size_t>(subframe * 2)]; } [[nodiscard]] constexpr auto high(const int subframe) const noexcept -> opus_int8 { return entries[static_cast<std::size_t>(subframe * 2 + 1)]; } };
[[nodiscard]] constexpr auto silk_decode_pitch_codebook_view(const int fs_kHz, const int nb_subfr) noexcept -> silk_lag_codebook_view {
  if (fs_kHz == 8) {
    if (nb_subfr == 4) { return {flat_table_span(silk_CB_lags_stage2), 11, 11};
}
    return {flat_table_span(silk_CB_lags_stage2_10_ms), 3, 3};
}
  if (nb_subfr == 4) { return {flat_table_span(silk_CB_lags_stage3), 34, 34};
}
  return {flat_table_span(silk_CB_lags_stage3_10_ms), 12, 12};
}
[[nodiscard]] constexpr auto silk_stage2_pitch_codebook_view(const int fs_kHz, const int nb_subfr, const int complexity) noexcept -> silk_lag_codebook_view {
  if (nb_subfr == 4) { return {flat_table_span(silk_CB_lags_stage2), 11, fs_kHz == 8 && complexity > 0 ? 11 : 3};
}
  return {flat_table_span(silk_CB_lags_stage2_10_ms), 3, 3};
}
[[nodiscard]] static constexpr auto silk_stage3_pitch_codebook_view(const int nb_subfr, const int complexity) noexcept -> silk_lag_codebook_view {
  if (nb_subfr == 4) { return {flat_table_span(silk_CB_lags_stage3), 34, silk_nb_cbk_searchs_stage3[static_cast<std::size_t>(complexity)]};
}
  return {flat_table_span(silk_CB_lags_stage3_10_ms), 12, 12};
}
[[nodiscard]] constexpr auto silk_stage3_lag_range_view(const int nb_subfr, const int complexity) noexcept -> silk_lag_range_view {
  if (nb_subfr == 4) { return {flat_table_span(silk_Lag_range_stage3, static_cast<std::size_t>(complexity))};
}
  return {flat_table_span(silk_Lag_range_stage3_10_ms)};
}
[[nodiscard]] static constexpr auto silk_pitch_contour_icdf(const int fs_kHz, const int nb_subfr) noexcept -> std::span<const opus_uint8> {
  if (fs_kHz == 8) { return nb_subfr == 4 ? std::span<const opus_uint8>{silk_pitch_contour_NB_iCDF} : std::span<const opus_uint8>{silk_pitch_contour_10_ms_NB_iCDF};
}
  return nb_subfr == 4 ? std::span<const opus_uint8>{silk_pitch_contour_iCDF} : std::span<const opus_uint8>{silk_pitch_contour_10_ms_iCDF};
}
[[nodiscard]] static constexpr auto silk_pitch_lag_low_bits_icdf(const int fs_kHz) noexcept -> std::span<const opus_uint8> {
  if (fs_kHz == 16) { return silk_uniform8_iCDF;
}
  return fs_kHz == 12 ? std::span<const opus_uint8>{silk_uniform6_iCDF} : std::span<const opus_uint8>{silk_uniform4_iCDF};
}

struct silk_resampler_ratio_config { int fir_fracs; int fir_order; std::span<const opus_int16> coefs; bool valid; };
[[nodiscard]] static constexpr auto silk_select_resampler_config(int input_hz, int output_hz) noexcept -> silk_resampler_ratio_config;
void silk_decode_pitch(opus_int16 lagIndex, opus_int8 contourIndex, int pitch_lags[], const int Fs_kHz, const int nb_subfr) {
  const auto codebook = silk_decode_pitch_codebook_view(Fs_kHz, nb_subfr);
  const int min_lag = ((opus_int32)((opus_int16)(2)) * (opus_int32)((opus_int16)(Fs_kHz)));
  const int max_lag = ((opus_int32)((opus_int16)(18)) * (opus_int32)((opus_int16)(Fs_kHz)));
  const int lag = min_lag + lagIndex;
  for (int k = 0; k < nb_subfr; k++) { pitch_lags[k] = lag + codebook.at(k, contourIndex); pitch_lags[k] = clamp_value(pitch_lags[k], min_lag, max_lag); }
}
static opus_int32 silk_inner_prod_aligned_scale(std::span<const opus_int16> inVec1, std::span<const opus_int16> inVec2, const int scale) {
  opus_assume(inVec1.size() == inVec2.size()); opus_int32 sum = 0;
  for (auto index = std::size_t{}; index < inVec1.size(); ++index) { sum += (static_cast<opus_int32>(inVec1[index]) * static_cast<opus_int32>(inVec2[index])) >> scale;
}
  return sum;
}
opus_int32 silk_lin2log(const opus_int32 inLin) {
  opus_int32 lz, frac_Q7; silk_CLZ_FRAC(inLin, &lz, &frac_Q7);
  return parabolic_q7_term(frac_Q7, 179) + static_cast<opus_int32>(static_cast<opus_uint32>(31 - lz) << q7_shift);
}
opus_int32 silk_log2lin(const opus_int32 inLog_Q7) {
  opus_int32 out, frac_Q7;
  if (inLog_Q7 < 0) {
    return 0;
  } else if (inLog_Q7 >= 3967) { return opus_int32_max;
}
  out = static_cast<opus_int32>(static_cast<opus_uint32>(1) << (inLog_Q7 >> q7_shift)); frac_Q7 = inLog_Q7 & 0x7F;
  const auto frac_term = parabolic_q7_term(frac_Q7, -174);
  if (inLog_Q7 < 2048) {
    out += static_cast<opus_int32>((static_cast<opus_int64>(out) * frac_term) >> q7_shift);
  } else { out += (out >> q7_shift) * frac_term;
}
  return out;
}
void silk_LPC_analysis_filter(opus_int16 *out, const opus_int16 *in, const opus_int16 *B, const opus_int32 len, const opus_int32 d) {
  int j, ix;
  opus_int32 out32_Q12, out32;
  const opus_int16 *in_ptr;
  opus_assume(d >= 6); opus_assume((d & 1) == 0);
  opus_assume(d <= len); for (ix = d; ix < len; ix++) {
    in_ptr = &in[ix - 1]; out32_Q12 = ((opus_int32)((opus_int16)(in_ptr[0])) * (opus_int32)((opus_int16)(B[0])));
    out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-1]))) * (opus_int32)((opus_int16)(B[1]))))));
    out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-2]))) * (opus_int32)((opus_int16)(B[2]))))));
    out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-3]))) * (opus_int32)((opus_int16)(B[3]))))));
    out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-4]))) * (opus_int32)((opus_int16)(B[4]))))));
    out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-5]))) * (opus_int32)((opus_int16)(B[5]))))));
    for (j = 6; j < d; j += 2) {
      out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-j]))) * (opus_int32)((opus_int16)(B[j]))))));
      out32_Q12 = (((opus_int32)((opus_uint32)((out32_Q12)) + (opus_uint32)(((opus_int32)((opus_int16)(in_ptr[-j - 1]))) * (opus_int32)((opus_int16)(B[j + 1]))))));
}
    out32_Q12 = ((opus_int32)((opus_uint32)(((opus_int32)((opus_uint32)((opus_int32)in_ptr[1]) << (12)))) - (opus_uint32)(out32_Q12)));
    out32 = rounded_rshift<12>(out32_Q12); out[ix] = saturate_int16_from_int32(out32);
}
  zero_n_bytes(out, static_cast<std::size_t>(d * sizeof(opus_int16)));
}
static opus_int32 LPC_inverse_pred_gain_QA_c(opus_int32 A_QA[24], const int order) {
  int k, n, mult2Q;
  opus_int32 invGain_Q30, rc_Q31, rc_mult1_Q30, rc_mult2, tmp1, tmp2;
  invGain_Q30 = ((opus_int32)((1) * ((opus_int64)1 << (30)) + 0.5));
  for (k = order - 1; k > 0; k--) {
    if ((A_QA[k] > ((opus_int32)((0.99975) * ((opus_int64)1 << (24)) + 0.5))) || (A_QA[k] < -((opus_int32)((0.99975) * ((opus_int64)1 << (24)) + 0.5)))) { return 0;
}
    rc_Q31 = -((opus_int32)((opus_uint32)(A_QA[k]) << (31 - 24)));
    rc_mult1_Q30 = ((((opus_int32)((1) * ((opus_int64)1 << (30)) + 0.5))) - ((opus_int32)((((opus_int64)((rc_Q31)) * ((rc_Q31)))) >> (32))));
    opus_assume(rc_mult1_Q30 > (1 << 15)); opus_assume(rc_mult1_Q30 <= (1 << 30));
    invGain_Q30 = ((opus_int32)((opus_uint32)((opus_int32)((((opus_int64)((invGain_Q30)) * ((rc_mult1_Q30)))) >> (32))) << (2)));
    opus_assume(invGain_Q30 >= 0); opus_assume(invGain_Q30 <= (1 << 30));
    if (invGain_Q30 < ((opus_int32)((1.0f / 1e4f) * ((opus_int64)1 << (30)) + 0.5))) { return 0;
}
    mult2Q = 32 - silk_CLZ32((((rc_mult1_Q30) > 0) ? (rc_mult1_Q30) : -(rc_mult1_Q30)));
    rc_mult2 = silk_INVERSE32_varQ(rc_mult1_Q30, mult2Q + 30);
    for (n = 0; n < (k + 1) >> 1; n++) {
      opus_int64 tmp64; tmp1 = A_QA[n];
      tmp2 = A_QA[k - n - 1]; tmp64 = inverse_prediction_step(tmp1, tmp2, rc_Q31, rc_mult2, mult2Q);
      if (tmp64 > 0x7FFFFFFF || tmp64 < ((opus_int32)0x80000000)) { return 0;
}
      A_QA[n] = (opus_int32)tmp64; tmp64 = inverse_prediction_step(tmp2, tmp1, rc_Q31, rc_mult2, mult2Q);
      if (tmp64 > 0x7FFFFFFF || tmp64 < ((opus_int32)0x80000000)) { return 0;
}
      A_QA[k - n - 1] = (opus_int32)tmp64; }
}
  if ((A_QA[k] > ((opus_int32)((0.99975) * ((opus_int64)1 << (24)) + 0.5))) || (A_QA[k] < -((opus_int32)((0.99975) * ((opus_int64)1 << (24)) + 0.5)))) { return 0;
}
  rc_Q31 = -((opus_int32)((opus_uint32)(A_QA[0]) << (31 - 24)));
  rc_mult1_Q30 = ((((opus_int32)((1) * ((opus_int64)1 << (30)) + 0.5))) - ((opus_int32)((((opus_int64)((rc_Q31)) * ((rc_Q31)))) >> (32))));
  invGain_Q30 = ((opus_int32)((opus_uint32)((opus_int32)((((opus_int64)((invGain_Q30)) * ((rc_mult1_Q30)))) >> (32))) << (2)));
  opus_assume(invGain_Q30 >= 0); opus_assume(invGain_Q30 <= (1 << 30));
  if (invGain_Q30 < ((opus_int32)((1.0f / 1e4f) * ((opus_int64)1 << (30)) + 0.5))) { return 0;
}
  return invGain_Q30;
}
opus_int32 silk_LPC_inverse_pred_gain_c(const opus_int16 *A_Q12, const int order) {
  int k;
  opus_int32 Atmp_QA[24]; opus_int32 DC_resp = 0;
  for (k = 0; k < order; k++) { DC_resp += (opus_int32)A_Q12[k]; Atmp_QA[k] = ((opus_int32)((opus_uint32)((opus_int32)A_Q12[k]) << (24 - 12)));
}
  if (DC_resp >= 4096) { return 0;
}
  return LPC_inverse_pred_gain_QA_c(Atmp_QA, order);
}
namespace {
constinit const std::array<opus_int16, 128 + 1> silk_LSFCosTab_FIX_Q12 = numeric_blob_array<opus_int16, 129>(R"blob(20001FFE1FF61FEA1FD81FC21FA81F881F621F3A1F0A1ED81EA01E621E221DDC1D901D421CEE1C961C3A1BD81B721B0A1A9C1A2A19B4193A18BC183C17B6172E16A01610157E14E8144E13B01310126E11C8111E10740FC60F160E640DAE0CF80C400B840AC80A0A094A088A07C60702063E057804B203EA0322025A019200CA0000FF36FE6EFDA6FCDEFC16FB4EFA88F9C2F8FEF83AF776F6B6F5F6F538F47CF3C0F308F252F19CF0EAF03AEF8CEEE2EE38ED92ECF0EC50EBB2EB18EA82E9F0E960E8D2E84AE7C4E744E6C6E64CE5D6E564E4F6E48EE428E3C6E36AE312E2BEE270E224E1DEE19EE160E128E0F6E0C6E09EE078E058E03EE028E016E00AE002E000)blob");
}
static void silk_NLSF2A_find_poly(opus_int32 *out, const opus_int32 *cLSF, int dd) {
  int k, n;
  opus_int32 ftmp; out[0] = ((opus_int32)((opus_uint32)(1) << (16)));
  out[1] = -cLSF[0];
  for (k = 1; k < dd; k++) {
    ftmp = cLSF[2 * k];
    out[k + 1] = static_cast<opus_int32>(static_cast<opus_uint32>(out[k - 1]) << 1) - static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(ftmp) * out[k]));
    for (n = k; n > 1; n--) { out[n] += out[n - 2] - static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(ftmp) * out[n - 1]));
}
    out[1] -= ftmp; }
}
void silk_NLSF2A(opus_int16 *a_Q12, const opus_int16 *NLSF, const int d) {
  constexpr std::array<unsigned char, 16> ordering16{0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
  constexpr std::array<unsigned char, 10> ordering10{0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
  const unsigned char *ordering;
  int k, i, dd;
  opus_int32 cos_LSF_QA[24]{}; opus_int32 P[24 / 2 + 1], Q[24 / 2 + 1];
  opus_int32 Ptmp, Qtmp, f_int, f_frac, cos_val, delta; opus_int32 a32_QA1[24];
  opus_assume(128 == 128); opus_assume(d == 10 || d == 16);
  ordering = d == 16 ? ordering16.data() : ordering10.data();
  for (k = 0; k < d; k++) {
    opus_assume(NLSF[k] >= 0); f_int = ((NLSF[k]) >> (15 - 7));
    f_frac = NLSF[k] - ((opus_int32)((opus_uint32)(f_int) << (15 - 7))); opus_assume(f_int >= 0);
    opus_assume(f_int < 128); cos_val = silk_LSFCosTab_FIX_Q12[f_int];
    delta = silk_LSFCosTab_FIX_Q12[f_int + 1] - cos_val;
    cos_LSF_QA[ordering[k]] = rounded_rshift<4>((static_cast<opus_int32>(static_cast<opus_uint32>(cos_val)) << 8) + (delta * f_frac));
}
  dd = ((d) >> (1)); silk_NLSF2A_find_poly(P, &cos_LSF_QA[0], dd); silk_NLSF2A_find_poly(Q, &cos_LSF_QA[1], dd);
  for (k = 0; k < dd; k++) { Ptmp = P[k + 1] + P[k]; Qtmp = Q[k + 1] - Q[k]; a32_QA1[k] = -Qtmp - Ptmp; a32_QA1[d - k - 1] = Qtmp - Ptmp;
}
  silk_LPC_fit(a_Q12, a32_QA1, 12, 16 + 1, d);
  for (i = 0; silk_LPC_inverse_pred_gain_c(a_Q12, d) == 0 && i < 16; i++) {
  silk_bwexpander_32(std::span<opus_int32>{a32_QA1, static_cast<std::size_t>(d)}, 65536 - ((opus_int32)((opus_uint32)(2) << (i))));
    for (k = 0; k < d; k++) { a_Q12[k] = static_cast<opus_int16>(rounded_rshift<5>(a32_QA1[k])); } }
}
void silk_NLSF_stabilize(opus_int16 *NLSF_Q15, const opus_int16 *NDeltaMin_Q15, const int L) {
  int i, I = 0, k, loops;
  opus_int16 center_freq_Q15; opus_int32 diff_Q15, min_diff_Q15, min_center_Q15, max_center_Q15;
  opus_assume(NDeltaMin_Q15[L] >= 1);
  for (loops = 0; loops < 20; loops++) {
    min_diff_Q15 = NLSF_Q15[0] - NDeltaMin_Q15[0]; I = 0;
    for (i = 1; i <= L - 1; i++) {
      diff_Q15 = NLSF_Q15[i] - (NLSF_Q15[i - 1] + NDeltaMin_Q15[i]);
      if (diff_Q15 < min_diff_Q15) { min_diff_Q15 = diff_Q15; I = i; }
}
    diff_Q15 = (1 << 15) - (NLSF_Q15[L - 1] + NDeltaMin_Q15[L]);
    if (diff_Q15 < min_diff_Q15) { min_diff_Q15 = diff_Q15; I = L;
}
    if (min_diff_Q15 >= 0) { return;
}
    if (I == 0) {
      NLSF_Q15[0] = NDeltaMin_Q15[0];
    } else if (I == L) {
      NLSF_Q15[L - 1] = (1 << 15) - NDeltaMin_Q15[L];
    } else {
      min_center_Q15 = 0;
      for (k = 0; k < I; k++) { min_center_Q15 += NDeltaMin_Q15[k];
}
      min_center_Q15 += ((NDeltaMin_Q15[I]) >> (1)); max_center_Q15 = 1 << 15;
      for (k = L; k > I; k--) { max_center_Q15 -= NDeltaMin_Q15[k];
}
      max_center_Q15 -= ((NDeltaMin_Q15[I]) >> (1));
      center_freq_Q15 = static_cast<opus_int16>(clamped_midpoint(NLSF_Q15[I - 1], NLSF_Q15[I], min_center_Q15, max_center_Q15));
      NLSF_Q15[I - 1] = center_freq_Q15 - ((NDeltaMin_Q15[I]) >> (1)); NLSF_Q15[I] = NLSF_Q15[I - 1] + NDeltaMin_Q15[I]; }
}
  if (loops == 20) {
    silk_insertion_sort_increasing_all_values_int16(&NLSF_Q15[0], L); NLSF_Q15[0] = std::max(NLSF_Q15[0], NDeltaMin_Q15[0]);
    for (i = 1; i < L; i++) NLSF_Q15[i] = std::max(NLSF_Q15[i], saturate_int16_from_int32(static_cast<opus_int32>(NLSF_Q15[i - 1]) + NDeltaMin_Q15[i]));
    NLSF_Q15[L - 1] = std::min(NLSF_Q15[L - 1], (opus_int16)((1 << 15) - NDeltaMin_Q15[L]));
    for (i = L - 2; i >= 0; i--) NLSF_Q15[i] = std::min(NLSF_Q15[i], (opus_int16)(NLSF_Q15[i + 1] - NDeltaMin_Q15[i + 1])); }
}
void silk_NLSF_VQ_weights_laroia(opus_int16 *pNLSFW_Q_OUT, const opus_int16 *pNLSF_Q15, const int D) {
  int k;
  opus_int32 tmp1_int, tmp2_int; opus_assume(D > 0);
  opus_assume((D & 1) == 0); tmp1_int = std::max((opus_int32)pNLSF_Q15[0], 1);
  tmp1_int = ((opus_int32)(((opus_int32)1 << (15 + 2)) / (tmp1_int))); tmp2_int = std::max((opus_int32)(pNLSF_Q15[1] - pNLSF_Q15[0]), 1);
  tmp2_int = ((opus_int32)(((opus_int32)1 << (15 + 2)) / (tmp2_int)));
  pNLSFW_Q_OUT[0] = (opus_int16)std::min(tmp1_int + tmp2_int, 0x7FFF); opus_assume(pNLSFW_Q_OUT[0] > 0);
  for (k = 1; k < D - 1; k += 2) {
    tmp1_int = std::max(pNLSF_Q15[k + 1] - pNLSF_Q15[k], 1); tmp1_int = ((opus_int32)(((opus_int32)1 << (15 + 2)) / (tmp1_int)));
    pNLSFW_Q_OUT[k] = (opus_int16)std::min(tmp1_int + tmp2_int, 0x7FFF); opus_assume(pNLSFW_Q_OUT[k] > 0);
    tmp2_int = std::max(pNLSF_Q15[k + 2] - pNLSF_Q15[k + 1], 1); tmp2_int = ((opus_int32)(((opus_int32)1 << (15 + 2)) / (tmp2_int)));
    pNLSFW_Q_OUT[k + 1] = (opus_int16)std::min(tmp1_int + tmp2_int, 0x7FFF); opus_assume(pNLSFW_Q_OUT[k + 1] > 0);
}
  tmp1_int = std::max((1 << 15) - pNLSF_Q15[D - 1], 1); tmp1_int = ((opus_int32)(((opus_int32)1 << (15 + 2)) / (tmp1_int)));
  pNLSFW_Q_OUT[D - 1] = (opus_int16)std::min(tmp1_int + tmp2_int, 0x7FFF); opus_assume(pNLSFW_Q_OUT[D - 1] > 0);
}
namespace {
constinit const std::array<std::array<opus_int8, 3>, 2> silk_CB_lags_stage2_10_ms = numeric_blob_matrix<opus_int8, 2, 3>(R"blob(000100000001)blob");
constinit const std::array<std::array<opus_int8, 12>, 2> silk_CB_lags_stage3_10_ms = numeric_blob_matrix<opus_int8, 2, 12>(R"blob(000001FF01FF02FE02FE03FD00010001FF02FF02FE03FE03)blob");
constinit const std::array<std::array<opus_int8, 2>, 2> silk_Lag_range_stage3_10_ms = numeric_blob_matrix<opus_int8, 2, 2>(R"blob(FD07FE07)blob");
constinit const std::array<std::array<opus_int8, 11>, 4> silk_CB_lags_stage2 = numeric_blob_matrix<opus_int8, 4, 11>(R"blob(0002FFFFFF0000010100010001000000000001000000000001000000010000000000FF02010001010000FFFF)blob");
constinit const std::array<std::array<opus_int8, 34>, 4> silk_CB_lags_stage3 = numeric_blob_matrix<opus_int8, 4, 34>(R"blob(000001FF0001FF00FF01FE02FEFE02FD0203FDFC03FC0404FB05FAFB06F9060508F700000100000000000000FF01000001FF0001FFFF01FF0201FF02FEFE02FE020203FD0001000000000000010001000001FF0100000201FF02FFFF02FF0202FF03FEFEFE0300010000010001FF02FF02FF0203FE03FEFE0404FD05FDFC06FC0605FB08FAFBF909)blob");
constinit const std::array<std::array<std::array<opus_int8, 2>, 4>, 3> silk_Lag_range_stage3 = numeric_blob_tensor<opus_int8, 3, 4, 2>(R"blob(FB08FF06FF06FC0AFA0AFE06FF06FB0AF70CFD07FE07F90D)blob");
constinit const std::array<opus_int8, 2 + 1> silk_nb_cbk_searchs_stage3 = numeric_blob_array<opus_int8, 3>(R"blob(101822)blob");
}
constexpr opus_int16 silk_resampler_down2_0 = 9872;
constexpr opus_int16 silk_resampler_down2_1 = 39809 - 65536;
constexpr std::array<opus_int16, 3> silk_resampler_up2_hq_0 = numeric_blob_array<opus_int16, 3>(R"blob(06D23A8A98AB)blob");
constexpr std::array<opus_int16, 3> silk_resampler_up2_hq_1 = numeric_blob_array<opus_int16, 3>(R"blob(1AC664A9D8F6)blob");
namespace {
extern constinit const std::array<opus_int16, 2 + 3 * 18 / 2> silk_Resampler_3_4_COEFS;
extern constinit const std::array<opus_int16, 2 + 2 * 18 / 2> silk_Resampler_2_3_COEFS;
extern constinit const std::array<opus_int16, 2 + 24 / 2> silk_Resampler_1_2_COEFS;
extern constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_3_COEFS, silk_Resampler_1_4_COEFS, silk_Resampler_1_6_COEFS;
extern constinit const std::array<opus_int16, 2 + 2 * 2> silk_Resampler_2_3_COEFS_LQ;
extern constinit const std::array<std::array<opus_int16, 8 / 2>, 12> silk_resampler_frac_FIR_12;
}
[[nodiscard]] static constexpr auto silk_select_resampler_config(const int input_hz, const int output_hz) noexcept -> silk_resampler_ratio_config {
  if (output_hz * 4 == input_hz * 3) { return {3, 18, silk_Resampler_3_4_COEFS, true};
}
  if (output_hz * 3 == input_hz * 2) { return {2, 18, silk_Resampler_2_3_COEFS, true};
}
  if (output_hz * 2 == input_hz) { return {1, 24, silk_Resampler_1_2_COEFS, true};
}
  if (output_hz * 3 == input_hz) { return {1, 36, silk_Resampler_1_3_COEFS, true};
}
  if (output_hz * 4 == input_hz) { return {1, 36, silk_Resampler_1_4_COEFS, true};
}
  if (output_hz * 6 == input_hz) { return {1, 36, silk_Resampler_1_6_COEFS, true};
}
  return {0, 0, {}, false};
}
static void silk_resampler_private_IIR_FIR(void *SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen), silk_resampler_private_down_FIR(void *SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen),
    silk_resampler_private_up2_HQ_wrapper(void *SS, opus_int16 *out, const opus_int16 *in, opus_int32 len), silk_resampler_private_up2_HQ(opus_int32 *S, opus_int16 *out, const opus_int16 *in, opus_int32 len),
    silk_resampler_private_AR2(opus_int32 S[], opus_int32 out_Q8[], const opus_int16 in[], const opus_int16 A_Q14[], opus_int32 len);
constexpr std::array<std::array<opus_int8, 3>, 6> delay_matrix_enc = numeric_blob_matrix<opus_int8, 6, 3>(R"blob(06000300070300010A000206120A0C00002C)blob");
constexpr std::array<std::array<opus_int8, 6>, 3> delay_matrix_dec = numeric_blob_matrix<opus_int8, 3, 6>(R"blob(04000200000000090407040400030C070707)blob");
int silk_resampler_init(silk_resampler_state_struct *S, opus_int32 Fs_Hz_in, opus_int32 Fs_Hz_out, int forEnc) {
  int up2x;
  zero_n_bytes(S, static_cast<std::size_t>(sizeof(silk_resampler_state_struct)));
  if (forEnc) {
    if ((Fs_Hz_in != 8000 && Fs_Hz_in != 12000 && Fs_Hz_in != 16000 && Fs_Hz_in != 24000 && Fs_Hz_in != 48000) || (Fs_Hz_out != 8000 && Fs_Hz_out != 12000 && Fs_Hz_out != 16000)) { return -1;
}
    S->inputDelay = delay_matrix_enc[((5) < ((((((Fs_Hz_in) >> 12) - ((Fs_Hz_in) > 16000)) >> ((Fs_Hz_in) > 24000)) - 1)) ? (5) : ((((((Fs_Hz_in) >> 12) - ((Fs_Hz_in) > 16000)) >> ((Fs_Hz_in) > 24000)) - 1)))][((5) < ((((((Fs_Hz_out) >> 12) - ((Fs_Hz_out) > 16000)) >> ((Fs_Hz_out) > 24000)) - 1)) ? (5) : ((((((Fs_Hz_out) >> 12) - ((Fs_Hz_out) > 16000)) >> ((Fs_Hz_out) > 24000)) - 1)))];
  } else {
    if ((Fs_Hz_in != 8000 && Fs_Hz_in != 12000 && Fs_Hz_in != 16000) || (Fs_Hz_out != 8000 && Fs_Hz_out != 12000 && Fs_Hz_out != 16000 && Fs_Hz_out != 24000 && Fs_Hz_out != 48000)) { return -1;
}
    S->inputDelay = delay_matrix_dec[((5) < ((((((Fs_Hz_in) >> 12) - ((Fs_Hz_in) > 16000)) >> ((Fs_Hz_in) > 24000)) - 1)) ? (5) : ((((((Fs_Hz_in) >> 12) - ((Fs_Hz_in) > 16000)) >> ((Fs_Hz_in) > 24000)) - 1)))][((5) < ((((((Fs_Hz_out) >> 12) - ((Fs_Hz_out) > 16000)) >> ((Fs_Hz_out) > 24000)) - 1)) ? (5) : ((((((Fs_Hz_out) >> 12) - ((Fs_Hz_out) > 16000)) >> ((Fs_Hz_out) > 24000)) - 1)))];
}
  S->Fs_in_kHz = ((opus_int32)((Fs_Hz_in) / (1000))); S->Fs_out_kHz = ((opus_int32)((Fs_Hz_out) / (1000)));
  S->batchSize = S->Fs_in_kHz * 10; up2x = 0;
  if (Fs_Hz_out > Fs_Hz_in) {
    if (Fs_Hz_out == ((Fs_Hz_in) * (2))) {
      S->resampler_function = (1);
    } else { S->resampler_function = (2); up2x = 1;
}
  } else if (Fs_Hz_out < Fs_Hz_in) {
    S->resampler_function = (3);
    const auto config = silk_select_resampler_config(Fs_Hz_in, Fs_Hz_out);
    if (!config.valid) { return -1;
}
    S->FIR_Fracs = config.fir_fracs; S->FIR_Order = config.fir_order; S->Coefs = config.coefs.data();
  } else { S->resampler_function = (0);
}
  S->invRatio_Q16 = ((opus_int32)((opus_uint32)(((opus_int32)((((opus_int32)((opus_uint32)(Fs_Hz_in) << (14 + up2x)))) / (Fs_Hz_out)))) << (2)));
  for (; ((opus_int32)(((opus_int64)(S->invRatio_Q16) * (Fs_Hz_out)) >> 16)) < ((opus_int32)((opus_uint32)(Fs_Hz_in) << (up2x))); ++S->invRatio_Q16) {}
  return 0;
}
int silk_resampler(silk_resampler_state_struct *S, opus_int16 out[], const opus_int16 in[], opus_int32 inLen) {
  int nSamples;
  if (__builtin_expect(!!(S->Fs_in_kHz <= 0 || S->Fs_out_kHz <= 0 || S->inputDelay < 0 || S->inputDelay > S->Fs_in_kHz || inLen < S->Fs_in_kHz), 0)) return -1;
  nSamples = S->Fs_in_kHz - S->inputDelay;
  copy_n_bytes(in, static_cast<std::size_t>(nSamples * sizeof(opus_int16)), &S->delayBuf[S->inputDelay]);
  switch (S->resampler_function) {
  case (1):
    silk_resampler_private_up2_HQ_wrapper(S, out, S->delayBuf, S->Fs_in_kHz);
    silk_resampler_private_up2_HQ_wrapper(S, &out[S->Fs_out_kHz], &in[nSamples], inLen - S->Fs_in_kHz);
    break;
  case (2):
    silk_resampler_private_IIR_FIR(S, out, S->delayBuf, S->Fs_in_kHz);
    silk_resampler_private_IIR_FIR(S, &out[S->Fs_out_kHz], &in[nSamples], inLen - S->Fs_in_kHz);
    break;
  case (3):
    silk_resampler_private_down_FIR(S, out, S->delayBuf, S->Fs_in_kHz);
    silk_resampler_private_down_FIR(S, &out[S->Fs_out_kHz], &in[nSamples], inLen - S->Fs_in_kHz);
    break;
  default: copy_n_bytes(S->delayBuf, static_cast<std::size_t>(S->Fs_in_kHz * sizeof(opus_int16)), out); copy_n_bytes(&in[nSamples], static_cast<std::size_t>((inLen - S->Fs_in_kHz) * sizeof(opus_int16)), &out[S->Fs_out_kHz]);
}
  copy_n_bytes(&in[inLen - S->inputDelay], static_cast<std::size_t>(S->inputDelay * sizeof(opus_int16)), S->delayBuf);
  return 0;
}
void silk_resampler_down2_3(opus_int32 *S, opus_int16 *out, const opus_int16 *in, opus_int32 inLen) {
  opus_int32 nSamplesIn = 0, counter, res_Q6; opus_int32 *buf_ptr;
  opus_int32 buf[(10 * 48) + 4]; copy_n_bytes(S, static_cast<std::size_t>(4 * sizeof(opus_int32)), buf);
  for (; inLen > 0;) {
    nSamplesIn = (((inLen) < ((10 * 48))) ? (inLen) : ((10 * 48)));
    silk_resampler_private_AR2(&S[4], &buf[4], in, silk_Resampler_2_3_COEFS_LQ.data(), nSamplesIn); buf_ptr = buf; counter = nSamplesIn;
    for (; counter > 2; counter -= 3, buf_ptr += 3) {
      res_Q6 = ((opus_int32)(((buf_ptr[0]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[2]))) >> 16));
      res_Q6 = ((opus_int32)((res_Q6) + (((buf_ptr[1]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[3]))) >> 16)));
      res_Q6 = ((opus_int32)((res_Q6) + (((buf_ptr[2]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[5]))) >> 16)));
      res_Q6 = ((opus_int32)((res_Q6) + (((buf_ptr[3]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[4]))) >> 16)));
      *out++ = rounded_rshift_to_int16<6>(res_Q6);
      res_Q6 = ((opus_int32)(((buf_ptr[1]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[4]))) >> 16));
      res_Q6 = ((opus_int32)((res_Q6) + (((buf_ptr[2]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[5]))) >> 16)));
      res_Q6 = ((opus_int32)((res_Q6) + (((buf_ptr[3]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[3]))) >> 16)));
      res_Q6 = ((opus_int32)((res_Q6) + (((buf_ptr[4]) * (opus_int64)((opus_int16)(silk_Resampler_2_3_COEFS_LQ[2]))) >> 16)));
      *out++ = rounded_rshift_to_int16<6>(res_Q6);
}
    in += nSamplesIn; inLen -= nSamplesIn;
    if (inLen > 0) { copy_n_bytes(&buf[nSamplesIn], static_cast<std::size_t>(4 * sizeof(opus_int32)), buf); }
}
  copy_n_bytes(&buf[nSamplesIn], static_cast<std::size_t>(4 * sizeof(opus_int32)), S);
}
void silk_resampler_down2(opus_int32 *S, opus_int16 *out, const opus_int16 *in, opus_int32 inLen) {
  opus_int32 k, len2 = ((inLen) >> (1)); opus_int32 in32, out32, Y, X;
  opus_assume(silk_resampler_down2_0 > 0); opus_assume(silk_resampler_down2_1 < 0);
  for (k = 0; k < len2; k++) {
    in32 = ((opus_int32)((opus_uint32)((opus_int32)in[2 * k]) << (10))); Y = ((in32) - (S[0]));
    X = ((opus_int32)((Y) + (((Y) * (opus_int64)((opus_int16)(silk_resampler_down2_1))) >> 16)));
    out32 = ((S[0]) + (X)); S[0] = ((in32) + (X));
    in32 = ((opus_int32)((opus_uint32)((opus_int32)in[2 * k + 1]) << (10))); Y = ((in32) - (S[1]));
    X = ((opus_int32)(((Y) * (opus_int64)((opus_int16)(silk_resampler_down2_0))) >> 16)); out32 = ((out32) + (S[1]));
    out32 = ((out32) + (X)); S[1] = ((in32) + (X)); out[k] = rounded_rshift_to_int16<11>(out32); }
}
static void silk_resampler_private_AR2(opus_int32 S[], opus_int32 out_Q8[], const opus_int16 in[], const opus_int16 A_Q14[], opus_int32 len) {
  opus_int32 k, out32;
  for (k = 0; k < len; k++) {
    out32 = (((S[0])) + (((opus_int32)((opus_uint32)(((opus_int32)in[k])) << ((8)))))); out_Q8[k] = out32;
    out32 = ((opus_int32)((opus_uint32)(out32) << (2)));
    S[0] = ((opus_int32)((S[1]) + (((out32) * (opus_int64)((opus_int16)(A_Q14[0]))) >> 16)));
    S[1] = ((opus_int32)(((out32) * (opus_int64)((opus_int16)(A_Q14[1]))) >> 16)); }
}
namespace {
[[nodiscard]] constexpr auto resampler_round_shift(opus_int32 value, int shift) noexcept -> opus_int32 { return shift == 1 ? (value >> 1) + (value & 1) : (((value >> (shift - 1)) + 1) >> 1);
}
[[nodiscard]] constexpr auto resampler_saturate_int16(opus_int32 value) noexcept -> opus_int16 { return static_cast<opus_int16>(clamp_value(value, static_cast<opus_int32>(-32768), static_cast<opus_int32>(32767)));
}
[[nodiscard]] constexpr auto resampler_round_shift_saturate_int16(opus_int32 value, int shift) noexcept -> opus_int16 { return resampler_saturate_int16(resampler_round_shift(value, shift));
}
[[nodiscard]] constexpr auto resampler_mul_q16(opus_int32 sample, opus_int16 coef) noexcept -> opus_int32 { return static_cast<opus_int32>((sample * static_cast<opus_int64>(coef)) >> 16);
}
template <std::size_t HalfOrder> [[nodiscard]] constexpr auto resampler_fractional_fir_q6(std::span<const opus_int32, 2 * HalfOrder> buffer, std::span<const opus_int16, HalfOrder> leading, std::span<const opus_int16, HalfOrder> trailing) noexcept -> opus_int32 {
  auto acc = opus_int32{0};
  for (auto tap = std::size_t{}; tap < HalfOrder; ++tap) { acc = static_cast<opus_int32>(acc + resampler_mul_q16(buffer[tap], leading[tap]));
}
  for (auto tap = std::size_t{}; tap < HalfOrder; ++tap) { acc = static_cast<opus_int32>(acc + resampler_mul_q16(buffer[2 * HalfOrder - 1 - tap], trailing[tap]));
}
  return acc;
}
template <std::size_t HalfOrder> [[nodiscard]] constexpr auto resampler_symmetric_fir_q6(std::span<const opus_int32, 2 * HalfOrder> buffer, std::span<const opus_int16, HalfOrder> coefficients) noexcept -> opus_int32 {
  auto acc = opus_int32{0};
  for (auto tap = std::size_t{}; tap < HalfOrder; ++tap) {
    const auto pair_sum = static_cast<opus_int32>(buffer[tap] + buffer[2 * HalfOrder - 1 - tap]);
    acc = static_cast<opus_int32>(acc + resampler_mul_q16(pair_sum, coefficients[tap]));
}
  return acc;
}
[[nodiscard]] constexpr auto resampler_frac_fir_12_q15(std::span<const opus_int16, 8> buffer, std::span<const opus_int16, 4> leading, std::span<const opus_int16, 4> trailing) noexcept -> opus_int32 {
  auto acc = opus_int32{0};
  for (auto tap = std::size_t{}; tap < 4; ++tap) { acc += static_cast<opus_int32>(buffer[tap]) * static_cast<opus_int32>(leading[tap]);
}
  for (auto tap = std::size_t{}; tap < 4; ++tap) { acc += static_cast<opus_int32>(buffer[4 + tap]) * static_cast<opus_int32>(trailing[3 - tap]);
}
  return acc;
}
} // namespace
static auto silk_resampler_private_down_FIR_INTERPOL(opus_int16 *out, opus_int32 *buf, const opus_int16 *FIR_Coefs, int FIR_Order, int FIR_Fracs, opus_int32 max_index_Q16, opus_int32 index_increment_Q16) noexcept -> opus_int16 * {
  opus_int32 index_Q16, res_Q6; opus_int32 *buf_ptr;
  opus_int32 interpol_ind;
  switch (FIR_Order) {
  case 18:
    for (index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
      buf_ptr = buf + ((index_Q16) >> (16));
      interpol_ind = ((opus_int32)(((index_Q16 & 0xFFFF) * (opus_int64)((opus_int16)(FIR_Fracs))) >> 16));
      const auto leading = std::span<const opus_int16, 9>{&FIR_Coefs[9 * interpol_ind], 9};
      const auto trailing = std::span<const opus_int16, 9>{&FIR_Coefs[9 * (FIR_Fracs - 1 - interpol_ind)], 9};
      res_Q6 = resampler_fractional_fir_q6<9>(std::span<const opus_int32, 18>{buf_ptr, 18}, leading, trailing);
      *out++ = resampler_round_shift_saturate_int16(res_Q6, 6);
}
    break;
  case 24: {
    const auto coefficients = std::span<const opus_int16, 12>{FIR_Coefs, 12};
    for (index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
      buf_ptr = buf + ((index_Q16) >> (16));
      res_Q6 = resampler_symmetric_fir_q6<12>(std::span<const opus_int32, 24>{buf_ptr, 24}, coefficients);
      *out++ = resampler_round_shift_saturate_int16(res_Q6, 6);
}
    break;
}
  case 36: {
    const auto coefficients = std::span<const opus_int16, 18>{FIR_Coefs, 18};
    for (index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
      buf_ptr = buf + ((index_Q16) >> (16));
      res_Q6 = resampler_symmetric_fir_q6<18>(std::span<const opus_int32, 36>{buf_ptr, 36}, coefficients);
      *out++ = resampler_round_shift_saturate_int16(res_Q6, 6);
}
    break;
}
  default: break;
}
  return out;
}
static void silk_resampler_private_down_FIR(void *SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen) {
  silk_resampler_state_struct *S = (silk_resampler_state_struct *)SS; opus_int32 nSamplesIn = 0;
  opus_int32 max_index_Q16, index_increment_Q16;
  const opus_int16 *FIR_Coefs;
  auto *buf = OPUS_SCRATCH(opus_int32, S->batchSize + S->FIR_Order);
  copy_n_bytes(S->sFIR.i32, static_cast<std::size_t>(S->FIR_Order * sizeof(opus_int32)), buf);
  FIR_Coefs = &S->Coefs[2]; index_increment_Q16 = S->invRatio_Q16;
  for (; inLen > 1;) {
    nSamplesIn = (((inLen) < (S->batchSize)) ? (inLen) : (S->batchSize));
    silk_resampler_private_AR2(S->sIIR, &buf[S->FIR_Order], in, S->Coefs, nSamplesIn);
    max_index_Q16 = ((opus_int32)((opus_uint32)(nSamplesIn) << (16)));
    out = silk_resampler_private_down_FIR_INTERPOL(out, buf, FIR_Coefs, S->FIR_Order, S->FIR_Fracs, max_index_Q16, index_increment_Q16);
    in += nSamplesIn; inLen -= nSamplesIn;
    if (inLen > 1) { copy_n_bytes(&buf[nSamplesIn], static_cast<std::size_t>(S->FIR_Order * sizeof(opus_int32)), buf); }
}
  copy_n_bytes(&buf[nSamplesIn], static_cast<std::size_t>(S->FIR_Order * sizeof(opus_int32)), S->sFIR.i32);
}
static auto silk_resampler_private_IIR_FIR_INTERPOL(opus_int16 *out, opus_int16 *buf, opus_int32 max_index_Q16, opus_int32 index_increment_Q16) noexcept -> opus_int16 * {
  opus_int32 index_Q16, res_Q15; opus_int16 *buf_ptr;
  opus_int32 table_index;
  for (index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
    table_index = ((opus_int32)(((index_Q16 & 0xFFFF) * (opus_int64)((opus_int16)(12))) >> 16)); buf_ptr = &buf[index_Q16 >> 16];
    const auto leading = std::span<const opus_int16, 4>{silk_resampler_frac_FIR_12[table_index]};
    const auto trailing = std::span<const opus_int16, 4>{silk_resampler_frac_FIR_12[11 - table_index]};
    res_Q15 = resampler_frac_fir_12_q15(std::span<const opus_int16, 8>{buf_ptr, 8}, leading, trailing);
    *out++ = resampler_round_shift_saturate_int16(res_Q15, 15);
}
  return out;
}
static void silk_resampler_private_IIR_FIR(void *SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen) {
  silk_resampler_state_struct *S = (silk_resampler_state_struct *)SS; opus_int32 nSamplesIn = 0;
  opus_int32 max_index_Q16, index_increment_Q16;
  auto *buf = OPUS_SCRATCH(opus_int16, 2 * S->batchSize + 8);
  copy_n_bytes(S->sFIR.i16, static_cast<std::size_t>(8 * sizeof(opus_int16)), buf); index_increment_Q16 = S->invRatio_Q16;
  for (; inLen > 0;) {
    nSamplesIn = (((inLen) < (S->batchSize)) ? (inLen) : (S->batchSize)); silk_resampler_private_up2_HQ(S->sIIR, &buf[8], in, nSamplesIn);
    max_index_Q16 = ((opus_int32)((opus_uint32)(nSamplesIn) << (16 + 1)));
    out = silk_resampler_private_IIR_FIR_INTERPOL(out, buf, max_index_Q16, index_increment_Q16); in += nSamplesIn; inLen -= nSamplesIn;
    if (inLen > 0) { copy_n_bytes(&buf[nSamplesIn << 1], static_cast<std::size_t>(8 * sizeof(opus_int16)), buf); }
}
  copy_n_bytes(&buf[nSamplesIn << 1], static_cast<std::size_t>(8 * sizeof(opus_int16)), S->sFIR.i16);
}
[[nodiscard]] static auto silk_resampler_up2_hq_branch(std::span<opus_int32, 3> state, const opus_int32 in32, const std::array<opus_int16, 3> &coeffs) noexcept -> opus_int32 {
  auto update_stage = [](const opus_int32 input, opus_int32 &memory, const opus_int16 coefficient) noexcept {
    const auto delta = input - memory;
    const auto scaled = static_cast<opus_int32>((delta * static_cast<opus_int64>(coefficient)) >> 16);
    const auto output = memory + scaled;
    memory = input + scaled;
    return output;
  };
  const auto stage0 = update_stage(in32, state[0], coeffs[0]);
  const auto stage1 = update_stage(stage0, state[1], coeffs[1]);
  const auto delta = stage1 - state[2];
  const auto scaled = static_cast<opus_int32>(delta + ((delta * static_cast<opus_int64>(coeffs[2])) >> 16));
  const auto output = state[2] + scaled;
  state[2] = stage1 + scaled;
  return output;
}
static void silk_resampler_private_up2_HQ(opus_int32 *S, opus_int16 *out, const opus_int16 *in, opus_int32 len) {
  opus_assume(silk_resampler_up2_hq_0[0] > 0); opus_assume(silk_resampler_up2_hq_0[1] > 0);
  opus_assume(silk_resampler_up2_hq_0[2] < 0); opus_assume(silk_resampler_up2_hq_1[0] > 0);
  opus_assume(silk_resampler_up2_hq_1[1] > 0); opus_assume(silk_resampler_up2_hq_1[2] < 0);
  for (opus_int32 k = 0; k < len; ++k) {
    const auto in32 = static_cast<opus_int32>(static_cast<opus_uint32>(static_cast<opus_int32>(in[k])) << 10);
    out[2 * k] = rounded_rshift_to_int16<10>(silk_resampler_up2_hq_branch(std::span<opus_int32, 3>{S, 3}, in32, silk_resampler_up2_hq_0));
    out[2 * k + 1] = rounded_rshift_to_int16<10>(silk_resampler_up2_hq_branch(std::span<opus_int32, 3>{S + 3, 3}, in32, silk_resampler_up2_hq_1)); }
}
void silk_resampler_private_up2_HQ_wrapper(void *SS, opus_int16 *out, const opus_int16 *in, opus_int32 len) { silk_resampler_state_struct *S = (silk_resampler_state_struct *)SS; silk_resampler_private_up2_HQ(S->sIIR, out, in, len);
}
namespace {
constinit const std::array<opus_int16, 2 + 3 * 18 / 2> silk_Resampler_3_4_COEFS = numeric_blob_array<opus_int16, 29>(R"blob(AF2AC9D5FFCF00400011FF630161FE1000A32B2756BDFFD90006005BFF5600BA0017FC8018C04DD8FFEDFFDC0066FFA7FFE80148FC490A083E25)blob");
constinit const std::array<opus_int16, 2 + 2 * 18 / 2> silk_Resampler_2_3_COEFS = numeric_blob_array<opus_int16, 20>(R"blob(C787C93D00400080FF8600240136FD00024824334545000C00800012FF720120FF8BFC9F101B387B)blob");
constinit const std::array<opus_int16, 2 + 24 / 2> silk_Resampler_1_2_COEFS = numeric_blob_array<opus_int16, 14>(R"blob(0268C80DFFF60027003AFFD2FFAC007800B8FEC5FDE3050415042340)blob");
constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_3_COEFS = numeric_blob_array<opus_int16, 20>(R"blob(3EE6C4C6FFF300000014001A0005FFE1FFD5FFFC0041005A0007FF63FF08FFD40251062F0A340CC7)blob");
constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_4_COEFS = numeric_blob_array<opus_int16, 20>(R"blob(57E4C5050003FFF2FFECFFF10002001900250019FFF0FFB9FF95FFB100320124026F03D6050805B8)blob");
constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_6_COEFS = numeric_blob_array<opus_int16, 20>(R"blob(6B94C4670011000C00080001FFF6FFEAFFE2FFE0FFEA0003002C006400A800F3013D017D01AD01C7)blob");
constinit const std::array<opus_int16, 2 + 2 * 2> silk_Resampler_2_3_COEFS_LQ = numeric_blob_array<opus_int16, 6>(R"blob(F513E695125929F3061F2054)blob");
constinit const std::array<std::array<opus_int16, 8 / 2>, 12> silk_resampler_frac_FIR_12 = numeric_blob_matrix<opus_int16, 12, 4>(R"blob(00BDFDA8026977670075FF61FBD27408003400DDF6A86E74FFFC0211F2EA66E5FFD002F6F08C5DA5FFB00389EF755306FF9D03CCEF824766FF9503C7F08B3B27FF990380F2612EAEFFA50305F4CF225EFFB90263F7A11698FFD201A9FAA10BB4)blob");
}
constexpr std::array<opus_int32, 6> sigm_LUT_slope_Q10 = numeric_blob_array<opus_int32, 6>(R"blob(000000ED00000099000000490000001E0000000C00000007)blob");
constexpr std::array<opus_int32, 6> sigm_LUT_pos_Q15 = numeric_blob_array<opus_int32, 6>(R"blob(0000400000005D93000070BD000079ED00007DB200007F24)blob");
constexpr std::array<opus_int32, 6> sigm_LUT_neg_Q15 = numeric_blob_array<opus_int32, 6>(R"blob(000040000000226C00000F42000006120000024D000000DB)blob");
int silk_sigm_Q15(int in_Q5) {
  int ind;
  if (in_Q5 < 0) {
    in_Q5 = -in_Q5;
    if (in_Q5 >= 6 * 32) {
      return 0;
    } else {
      ind = ((in_Q5) >> (5));
      return (sigm_LUT_neg_Q15[ind] - ((opus_int32)((opus_int16)(sigm_LUT_slope_Q10[ind])) * (opus_int32)((opus_int16)(in_Q5 & 0x1F))));
}
  } else {
    if (in_Q5 >= 6 * 32) {
      return 32767;
    } else {
      ind = ((in_Q5) >> (5));
      return (sigm_LUT_pos_Q15[ind] + ((opus_int32)((opus_int16)(sigm_LUT_slope_Q10[ind])) * (opus_int32)((opus_int16)(in_Q5 & 0x1F)))); } }
}
void silk_insertion_sort_increasing(opus_int32 *a, int *idx, const int L, const int K) {
  opus_assume(K > 0); opus_assume(L > 0);
  opus_assume(L >= K);
  for (int i = 0; i < K; ++i) idx[i] = i;
  for (int i = 1; i < K; i++) {
    const opus_int32 value = a[i];
    int j = i - 1;
    for (; (j >= 0) && (value < a[j]); j--) { a[j + 1] = a[j]; idx[j + 1] = idx[j];
}
    a[j + 1] = value; idx[j + 1] = i;
}
  for (int i = K; i < L; i++) {
    const opus_int32 value = a[i];
    if (value < a[K - 1]) {
      int j = K - 2;
      for (; (j >= 0) && (value < a[j]); j--) { a[j + 1] = a[j]; idx[j + 1] = idx[j];
}
      a[j + 1] = value; idx[j + 1] = i; } }
}
void silk_insertion_sort_increasing_all_values_int16(opus_int16 *a, const int L) {
  opus_assume(L > 0);
  for (int i = 1; i < L; i++) {
    const int value = a[i];
    int j = i - 1;
    for (; (j >= 0) && (value < a[j]); j--) { a[j + 1] = a[j];
}
    a[j + 1] = value; }
}
[[nodiscard]] static auto silk_shifted_sum_sqr(std::span<const opus_int16> samples, const int shft, const opus_int32 initial = 0) noexcept -> opus_int32 {
  auto energy = initial;
  const auto even_count = samples.size() & ~std::size_t{1};
  for (std::size_t index = 0; index < even_count; index += 2) {
    auto pair_energy = static_cast<opus_int32>(samples[index]) * static_cast<opus_int32>(samples[index]);
    pair_energy = static_cast<opus_int32>(static_cast<opus_uint32>(pair_energy) + static_cast<opus_uint32>(static_cast<opus_int32>(samples[index + 1]) * static_cast<opus_int32>(samples[index + 1])));
    energy = static_cast<opus_int32>(energy + (static_cast<opus_int32>(pair_energy) >> shft));
}
  if (even_count != samples.size()) {
    const auto tail_energy = static_cast<opus_int32>(samples[even_count]) * static_cast<opus_int32>(samples[even_count]);
    energy = static_cast<opus_int32>(energy + (static_cast<opus_int32>(tail_energy) >> shft));
}
  return energy;
}
void silk_sum_sqr_shift(opus_int32 *energy, int *shift, const opus_int16 *x, int len) {
  const auto samples = std::span<const opus_int16>{x, static_cast<std::size_t>(len)};
  auto shft = 31 - silk_CLZ32(len);
  auto nrg = silk_shifted_sum_sqr(samples, shft, len);
  opus_assume(nrg >= 0); shft = std::max(0, shft + 3 - silk_CLZ32(nrg));
  nrg = silk_shifted_sum_sqr(samples, shft); opus_assume(nrg >= 0); *shift = shft; *energy = nrg;
}
[[nodiscard]] static auto silk_stereo_pred_step_q13(const int coarse_index) noexcept -> opus_int32 {
  const auto low_Q13 = silk_stereo_pred_quant_Q13[coarse_index];
  return static_cast<opus_int32>(((silk_stereo_pred_quant_Q13[coarse_index + 1] - low_Q13) * static_cast<opus_int64>(static_cast<opus_int16>(static_cast<opus_int32>((0.5 / 5) * (static_cast<opus_int64>(1) << 16) + 0.5)))) >> 16);
}
[[nodiscard]] static auto silk_stereo_pred_level_q13(const int coarse_index, const int fine_index) noexcept -> opus_int32 { return silk_stereo_pred_quant_Q13[coarse_index] + static_cast<opus_int32>(static_cast<opus_int16>(silk_stereo_pred_step_q13(coarse_index))) * static_cast<opus_int32>(static_cast<opus_int16>(2 * fine_index + 1));
}
void silk_stereo_decode_pred(ec_dec *psRangeDec, opus_int32 pred_Q13[]) {
  int ix[2][3]{};
  const int joint_index = ec_dec_icdf(psRangeDec, silk_stereo_pred_joint_iCDF.data(), 8);
  ix[0][2] = joint_index / 5;
  ix[1][2] = joint_index - 5 * ix[0][2];
  for (int channel = 0; channel < 2; ++channel) {
    ix[channel][0] = ec_dec_icdf(psRangeDec, silk_uniform3_iCDF.data(), 8);
    ix[channel][1] = ec_dec_icdf(psRangeDec, silk_uniform5_iCDF.data(), 8);
    pred_Q13[channel] = silk_stereo_pred_level_q13(ix[channel][0] + 3 * ix[channel][2], ix[channel][1]);
  }
  pred_Q13[0] -= pred_Q13[1];
}
void silk_stereo_decode_mid_only(ec_dec *psRangeDec, int *decode_only_mid) { *decode_only_mid = ec_dec_icdf(psRangeDec, silk_stereo_only_code_mid_iCDF.data(), 8);
}
void silk_stereo_encode_pred(ec_enc *psRangeEnc, opus_int8 ix[2][3]) {
  const int joint_index = 5 * ix[0][2] + ix[1][2];
  opus_assume(joint_index < 25);
  ec_enc_icdf(psRangeEnc, joint_index, silk_stereo_pred_joint_iCDF.data(), 8);
  for (int channel = 0; channel < 2; ++channel) {
    opus_assume(ix[channel][0] < 3);
    opus_assume(ix[channel][1] < 5);
    ec_enc_icdf(psRangeEnc, ix[channel][0], silk_uniform3_iCDF.data(), 8);
    ec_enc_icdf(psRangeEnc, ix[channel][1], silk_uniform5_iCDF.data(), 8);
  }
}
void silk_stereo_encode_mid_only(ec_enc *psRangeEnc, opus_int8 mid_only_flag) {
  ec_enc_icdf(psRangeEnc, mid_only_flag, silk_stereo_only_code_mid_iCDF.data(), 8);
}
opus_int32 silk_stereo_find_predictor(opus_int32 *ratio_Q14, const opus_int16 x[], const opus_int16 y[], opus_int32 mid_res_amp_Q0[], int length, int smooth_coef_Q16) {
  int scale, scale1, scale2;
  opus_int32 nrgx, nrgy, corr, pred_Q13, pred2_Q10; silk_sum_sqr_shift(&nrgx, &scale1, x, length);
  silk_sum_sqr_shift(&nrgy, &scale2, y, length); scale = std::max(scale1, scale2);
  scale = scale + (scale & 1); nrgy = ((nrgy) >> (scale - scale2)); nrgx = ((nrgx) >> (scale - scale1)); nrgx = std::max(nrgx, 1);
  corr = silk_inner_prod_aligned_scale(std::span<const opus_int16>{x, static_cast<std::size_t>(length)}, std::span<const opus_int16>{y, static_cast<std::size_t>(length)}, scale);
  pred_Q13 = silk_DIV32_varQ(corr, nrgx, 13);
  pred_Q13 = ((-(1 << 14)) > (1 << 14) ? ((pred_Q13) > (-(1 << 14)) ? (-(1 << 14)) : ((pred_Q13) < (1 << 14) ? (1 << 14) : (pred_Q13))) : ((pred_Q13) > (1 << 14) ? (1 << 14) : ((pred_Q13) < (-(1 << 14)) ? (-(1 << 14)) : (pred_Q13))));
  pred2_Q10 = ((opus_int32)(((pred_Q13) * (opus_int64)((opus_int16)(pred_Q13))) >> 16));
  smooth_coef_Q16 = (int)std::max(smooth_coef_Q16, (((pred2_Q10) > 0) ? (pred2_Q10) : -(pred2_Q10)));
  opus_assume(smooth_coef_Q16 < 32768); scale = ((scale) >> (1));
  mid_res_amp_Q0[0] = ((opus_int32)((mid_res_amp_Q0[0]) + (((((opus_int32)((opus_uint32)(silk_SQRT_APPROX(nrgx)) << (scale))) - mid_res_amp_Q0[0]) * (opus_int64)((opus_int16)(smooth_coef_Q16))) >> 16)));
  nrgy = (((nrgy)) - (((opus_int32)((opus_uint32)((((opus_int32)(((corr) * (opus_int64)((opus_int16)(pred_Q13))) >> 16)))) << ((3 + 1))))));
  nrgy = (((nrgy)) + (((opus_int32)((opus_uint32)((((opus_int32)(((nrgx) * (opus_int64)((opus_int16)(pred2_Q10))) >> 16)))) << ((6))))));
  mid_res_amp_Q0[1] = ((opus_int32)((mid_res_amp_Q0[1]) + (((((opus_int32)((opus_uint32)(silk_SQRT_APPROX(nrgy)) << (scale))) - mid_res_amp_Q0[1]) * (opus_int64)((opus_int16)(smooth_coef_Q16))) >> 16)));
  *ratio_Q14 = silk_DIV32_varQ(mid_res_amp_Q0[1], (((mid_res_amp_Q0[0]) > (1)) ? (mid_res_amp_Q0[0]) : (1)), 14);
  *ratio_Q14 = ((0) > (32767) ? ((*ratio_Q14) > (0) ? (0) : ((*ratio_Q14) < (32767) ? (32767) : (*ratio_Q14))) : ((*ratio_Q14) > (32767) ? (32767) : ((*ratio_Q14) < (0) ? (0) : (*ratio_Q14))));
  return pred_Q13;
}
void silk_stereo_quant_pred(opus_int32 pred_Q13[], opus_int8 ix[2][3]) {
  opus_int32 quant_pred_Q13 = 0;
  for (int n = 0; n < 2; ++n) {
    auto searching = true;
    auto err_min_Q13 = 0x7FFFFFFF;
    for (int i = 0; i < 16 - 1 && searching; ++i) {
      for (int j = 0; j < 5; ++j) {
        const auto lvl_Q13 = silk_stereo_pred_level_q13(i, j);
        const auto err_Q13 = (((pred_Q13[n] - lvl_Q13) > 0) ? (pred_Q13[n] - lvl_Q13) : -(pred_Q13[n] - lvl_Q13));
        if (err_Q13 < err_min_Q13) {
          err_min_Q13 = err_Q13; quant_pred_Q13 = lvl_Q13; ix[n][0] = i; ix[n][1] = j;
        } else {
          searching = false;
          break; } }
}
    ix[n][2] = ((opus_int32)((ix[n][0]) / (3))); ix[n][0] -= ix[n][2] * 3; pred_Q13[n] = quant_pred_Q13;
}
  pred_Q13[0] -= pred_Q13[1];
}
void silk_LPC_fit(opus_int16 *a_QOUT, opus_int32 *a_QIN, const int QOUT, const int QIN, const int d) {
  int i, k, idx = 0;
  opus_int32 maxabs, absval, chirp_Q16;
  for (i = 0; i < 10; i++) {
    maxabs = 0;
    for (k = 0; k < d; k++) { absval = std::abs(a_QIN[k]); if (absval > maxabs) { maxabs = absval; idx = k; }
}
    maxabs = resampler_round_shift(maxabs, QIN - QOUT);
    if (maxabs > 0x7FFF) {
      maxabs = (((maxabs) < (163838)) ? (maxabs) : (163838));
      chirp_Q16 = ((opus_int32)((0.999) * ((opus_int64)1 << (16)) + 0.5)) - ((opus_int32)((((opus_int32)((opus_uint32)(maxabs - 0x7FFF) << (14)))) / (((((maxabs) * (idx + 1))) >> (2)))));
  silk_bwexpander_32(std::span<opus_int32>{a_QIN, static_cast<std::size_t>(d)}, chirp_Q16);
    } else { break; }
}
  if (i == 10) {
    for (k = 0; k < d; k++) { a_QOUT[k] = resampler_round_shift_saturate_int16(a_QIN[k], QIN-QOUT); a_QIN[k] = (opus_int32)((opus_uint32)((opus_int32)a_QOUT[k])<<(QIN-QOUT));
}
  } else {
    for (k = 0; k < d; k++) { a_QOUT[k] = static_cast<opus_int16>(resampler_round_shift(a_QIN[k], QIN - QOUT)); } }
}
void silk_apply_sine_window_FLP(std::span<float> px_win, const std::span<const float> px, const int win_type) {
  float freq, c, S0, S1;
  opus_assume(win_type == 1 || win_type == 2); opus_assume(px_win.size() == px.size());
  opus_assume((px_win.size() & 3U) == 0);
  const auto length = static_cast<int>(px_win.size());
  freq = (3.1415926536f) / (length + 1); c = 2.0f - freq * freq;
  if (win_type < 2) {
    S0 = 0.0f; S1 = freq;
  } else { S0 = 1.0f; S1 = 0.5f * c;
}
  for (int block_index = 0; block_index < length / 4; ++block_index) {
    const auto k = static_cast<std::size_t>(block_index * 4);
    px_win[k + 0] = px[k + 0] * 0.5f * (S0 + S1); px_win[k + 1] = px[k + 1] * S1;
    S0 = c * S1 - S0; px_win[k + 2] = px[k + 2] * 0.5f * (S1 + S0); px_win[k + 3] = px[k + 3] * S0; S1 = c * S0 - S1; }
}
void silk_corrVector_FLP(const std::span<const float> x, const std::span<const float> t, std::span<float> Xt) {
  const auto order = Xt.size();
  opus_assume(x.size() >= t.size() + order - 1);
  for (auto lag = std::size_t{}; lag < order; ++lag) {
    const auto offset = order - lag - 1;
    Xt[lag] = static_cast<float>(silk_inner_product_FLP_c(x.data() + offset, t.data(), static_cast<int>(t.size()))); }
}
void silk_corrMatrix_FLP(const std::span<const float> x, const int L, const int Order, std::span<float> XX) {
  double energy;
  opus_assume(x.size() >= static_cast<std::size_t>(L + Order - 1));
  auto xx = matrix_ref<float>{XX, Order};
  const auto last = static_cast<std::size_t>(Order - 1);
  energy = silk_energy_FLP(x.data() + last, L); xx(0, 0) = static_cast<float>(energy);
  for (int j = 1; j < Order; ++j) {
    energy += x[last - static_cast<std::size_t>(j)] * x[last - static_cast<std::size_t>(j)] - x[last + static_cast<std::size_t>(L - j)] * x[last + static_cast<std::size_t>(L - j)];
    xx(j, j) = static_cast<float>(energy);
}
  for (int lag = 1; lag < Order; ++lag) {
    energy = silk_inner_product_FLP_c(x.data() + last, x.data() + last - static_cast<std::size_t>(lag), L);
    xx(lag, 0) = static_cast<float>(energy); xx(0, lag) = static_cast<float>(energy);
    for (int j = 1; j < (Order - lag); ++j) {
      const auto left = last - static_cast<std::size_t>(j);
      const auto right = left - static_cast<std::size_t>(lag);
      const auto tail = last + static_cast<std::size_t>(L - j);
      energy += x[left] * x[right] - x[tail] * x[tail - static_cast<std::size_t>(lag)];
      xx(lag + j, j) = static_cast<float>(energy); xx(j, lag + j) = static_cast<float>(energy); } }
}
void silk_encode_do_VAD_FLP(silk_encoder_state_FLP *psEnc, int activity) {
  const int activity_threshold = ((opus_int32)((0.05f) * ((opus_int64)1 << (8)) + 0.5));
  silk_VAD_GetSA_Q8_c(&psEnc->sCmn, psEnc->sCmn.inputBuf + 1);
  if (activity == 0 && psEnc->sCmn.speech_activity_Q8 >= activity_threshold) { psEnc->sCmn.speech_activity_Q8 = activity_threshold - 1;
}
  if (psEnc->sCmn.speech_activity_Q8 < activity_threshold) {
    psEnc->sCmn.indices.signalType = 0;
    psEnc->sCmn.VAD_flags[psEnc->sCmn.nFramesEncoded] = 0;
  } else {
    psEnc->sCmn.indices.signalType = 1; psEnc->sCmn.VAD_flags[psEnc->sCmn.nFramesEncoded] = 1; }
}
OPUS_SIZE_OPT int silk_encode_frame_FLP(silk_encoder_state_FLP *psEnc, opus_int32 *pnBytesOut, ec_enc *psRangeEnc, int condCoding, int maxBits, int useCBR) {
  silk_encoder_control_FLP sEncCtrl;
  int i, iter, maxIter, found_upper, found_lower, ret = 0;
  float *x_frame, *res_pitch_frame;
  float res_pitch[2 * ((5 * 4) * 16) + (2 * 16)];
  ec_enc sRangeEnc_copy, sRangeEnc_copy2;
  opus_int32 seed_copy, nBits, nBits_lower, nBits_upper, gainMult_lower, gainMult_upper;
  opus_int32 gainsID, gainsID_lower, gainsID_upper; opus_int16 gainMult_Q8, ec_prevLagIndex_copy;
  int ec_prevSignalType_copy;
  opus_int8 LastGainIndex_copy2; opus_int32 pGains_Q16[4];
  int gain_lock[4] = {0};
  opus_int16 best_gain_mult[4];
  int best_sum[4];
  int bits_margin;
  silk_nsq_state sNSQ_copy[2]; bits_margin = useCBR ? 5 : maxBits / 4;
  LastGainIndex_copy2 = nBits_lower = nBits_upper = gainMult_lower = gainMult_upper = 0;
  psEnc->sCmn.indices.Seed = psEnc->sCmn.frameCounter++ & 3; x_frame = psEnc->x_buf + psEnc->sCmn.ltp_mem_length;
  res_pitch_frame = res_pitch + psEnc->sCmn.ltp_mem_length;
  silk_LP_variable_cutoff(&psEnc->sCmn.sLP, psEnc->sCmn.inputBuf + 1, psEnc->sCmn.frame_length);
  silk_short2float_array(x_frame + 5 * psEnc->sCmn.fs_kHz, psEnc->sCmn.inputBuf + 1, psEnc->sCmn.frame_length);
  for (i = 0; i < 8; i++) { x_frame[5 * psEnc->sCmn.fs_kHz + i * (psEnc->sCmn.frame_length >> 3)] += (1 - (i & 2)) * 1e-6f;
}
  if (!psEnc->sCmn.prefillFlag) {
    silk_find_pitch_lags_FLP(psEnc, &sEncCtrl, res_pitch, x_frame);
    silk_noise_shape_analysis_FLP(psEnc, &sEncCtrl, res_pitch_frame, x_frame);
    silk_find_pred_coefs_FLP(psEnc, &sEncCtrl, res_pitch_frame, x_frame, condCoding);
    silk_process_gains_FLP(psEnc, &sEncCtrl, condCoding);
    maxIter = 6; gainMult_Q8 = ((opus_int32)((1) * ((opus_int64)1 << (8)) + 0.5)); found_lower = 0; found_upper = 0;
    gainsID = silk_gains_ID(psEnc->sCmn.indices.GainsIndices, psEnc->sCmn.nb_subfr); gainsID_lower = -1;
    gainsID_upper = -1; copy_n_bytes(psRangeEnc, static_cast<std::size_t>(sizeof(ec_enc)), &sRangeEnc_copy);
    copy_n_bytes(&psEnc->sCmn.sNSQ, static_cast<std::size_t>(sizeof(silk_nsq_state)), &sNSQ_copy[0]);
    seed_copy = psEnc->sCmn.indices.Seed; ec_prevLagIndex_copy = psEnc->sCmn.ec_prevLagIndex;
    ec_prevSignalType_copy = psEnc->sCmn.ec_prevSignalType; opus_uint8 ec_buf_copy[1275];
    for (iter = 0;; iter++) {
      if (gainsID == gainsID_lower) {
        nBits = nBits_lower;
      } else if (gainsID == gainsID_upper) {
        nBits = nBits_upper;
      } else {
        if (iter > 0) {
          copy_n_bytes(&sRangeEnc_copy, static_cast<std::size_t>(sizeof(ec_enc)), psRangeEnc);
          copy_n_bytes(&sNSQ_copy[0], static_cast<std::size_t>(sizeof(silk_nsq_state)), &psEnc->sCmn.sNSQ);
          psEnc->sCmn.indices.Seed = seed_copy; psEnc->sCmn.ec_prevLagIndex = ec_prevLagIndex_copy;
          psEnc->sCmn.ec_prevSignalType = ec_prevSignalType_copy;
}
        silk_NSQ_wrapper_FLP(psEnc, &sEncCtrl, &psEnc->sCmn.indices, &psEnc->sCmn.sNSQ, psEnc->sCmn.pulses, x_frame);
        if (iter == maxIter && !found_lower) { copy_n_bytes(psRangeEnc, static_cast<std::size_t>(sizeof(ec_enc)), &sRangeEnc_copy2);
}
        silk_encode_indices(&psEnc->sCmn, psRangeEnc, condCoding);
        silk_encode_pulses(psRangeEnc, psEnc->sCmn.indices.signalType, psEnc->sCmn.indices.quantOffsetType, std::span<opus_int8>{psEnc->sCmn.pulses, static_cast<std::size_t>((psEnc->sCmn.frame_length + 16 - 1) & ~(16 - 1))}, psEnc->sCmn.frame_length);
        nBits = ec_tell(psRangeEnc);
        if (iter == maxIter && !found_lower && nBits > maxBits) {
          copy_n_bytes(&sRangeEnc_copy2, static_cast<std::size_t>(sizeof(ec_enc)), psRangeEnc);
          psEnc->sShape.LastGainIndex = sEncCtrl.lastGainIndexPrev;
fill_n_items(psEnc->sCmn.indices.GainsIndices, static_cast<std::size_t>(psEnc->sCmn.nb_subfr), static_cast<opus_int8>(4));
          if (condCoding != 2) { psEnc->sCmn.indices.GainsIndices[0] = sEncCtrl.lastGainIndexPrev;
}
          psEnc->sCmn.ec_prevLagIndex = ec_prevLagIndex_copy; psEnc->sCmn.ec_prevSignalType = ec_prevSignalType_copy;
          zero_n_items(psEnc->sCmn.pulses, static_cast<std::size_t>(psEnc->sCmn.frame_length));
          silk_encode_indices(&psEnc->sCmn, psRangeEnc, condCoding);
          silk_encode_pulses(psRangeEnc, psEnc->sCmn.indices.signalType, psEnc->sCmn.indices.quantOffsetType, std::span<opus_int8>{psEnc->sCmn.pulses, static_cast<std::size_t>((psEnc->sCmn.frame_length + 16 - 1) & ~(16 - 1))}, psEnc->sCmn.frame_length);
          nBits = ec_tell(psRangeEnc);
}
        if (useCBR == 0 && iter == 0 && nBits <= maxBits) { break; }
}
      if (iter == maxIter) {
        if (found_lower && (gainsID == gainsID_lower || nBits > maxBits)) {
          copy_n_bytes(&sRangeEnc_copy2, static_cast<std::size_t>(sizeof(ec_enc)), psRangeEnc);
          opus_assume(sRangeEnc_copy2.offs <= 1275);
          copy_n_bytes(ec_buf_copy, static_cast<std::size_t>(sRangeEnc_copy2.offs), psRangeEnc->buf);
          copy_n_bytes(&sNSQ_copy[1], static_cast<std::size_t>(sizeof(silk_nsq_state)), &psEnc->sCmn.sNSQ);
          psEnc->sShape.LastGainIndex = LastGainIndex_copy2;
}
        break;
}
      if (nBits > maxBits) {
        if (found_lower == 0 && iter >= 2) {
          sEncCtrl.Lambda = (((sEncCtrl.Lambda * 1.5f) > (1.5f)) ? (sEncCtrl.Lambda * 1.5f) : (1.5f));
          psEnc->sCmn.indices.quantOffsetType = 0; found_upper = 0; gainsID_upper = -1;
        } else { found_upper = 1; nBits_upper = nBits; gainMult_upper = gainMult_Q8; gainsID_upper = gainsID;
}
      } else if (nBits < maxBits - bits_margin) {
        found_lower = 1; nBits_lower = nBits; gainMult_lower = gainMult_Q8;
        if (gainsID != gainsID_lower) {
          gainsID_lower = gainsID; copy_n_bytes(psRangeEnc, static_cast<std::size_t>(sizeof(ec_enc)), &sRangeEnc_copy2);
          opus_assume(psRangeEnc->offs <= 1275);
          copy_n_bytes(psRangeEnc->buf, static_cast<std::size_t>(psRangeEnc->offs), ec_buf_copy);
          copy_n_bytes(&psEnc->sCmn.sNSQ, static_cast<std::size_t>(sizeof(silk_nsq_state)), &sNSQ_copy[1]);
          LastGainIndex_copy2 = psEnc->sShape.LastGainIndex;
}
      } else { break;
}
      if (!found_lower && nBits > maxBits) {
        int j;
        for (i = 0; i < psEnc->sCmn.nb_subfr; i++) {
          int sum = 0;
          for (j = i * psEnc->sCmn.subfr_length; j < (i + 1) * psEnc->sCmn.subfr_length; j++) { sum += abs(psEnc->sCmn.pulses[j]);
}
          if (iter == 0 || (sum < best_sum[i] && !gain_lock[i])) {
            best_sum[i] = sum; best_gain_mult[i] = gainMult_Q8;
          } else { gain_lock[i] = 1; } }
}
      if ((found_lower & found_upper) == 0) {
        if (nBits > maxBits) {
          gainMult_Q8 = std::min(1024, gainMult_Q8 * 3 / 2);
        } else { gainMult_Q8 = std::max(64, gainMult_Q8 * 4 / 5);
}
      } else {
        gainMult_Q8 = gainMult_lower + ((gainMult_upper - gainMult_lower) * (maxBits - nBits_lower)) / (nBits_upper - nBits_lower);
        if (gainMult_Q8 > (((gainMult_lower)) + ((((gainMult_upper - gainMult_lower)) >> ((2)))))) {
          gainMult_Q8 = (((gainMult_lower)) + ((((gainMult_upper - gainMult_lower)) >> ((2)))));
        } else if (gainMult_Q8 < (((gainMult_upper)) - ((((gainMult_upper - gainMult_lower)) >> ((2)))))) { gainMult_Q8 = (((gainMult_upper)) - ((((gainMult_upper - gainMult_lower)) >> ((2))))); }
}
      for (i = 0; i < psEnc->sCmn.nb_subfr; i++) {
        opus_int16 tmp;
        if (gain_lock[i]) {
          tmp = best_gain_mult[i];
        } else { tmp = gainMult_Q8;
}
        pGains_Q16[i] = (((opus_int32)((opus_uint32)(((((((opus_int32)0x80000000)) >> ((8)))) > (((0x7FFFFFFF) >> ((8)))) ? (((((opus_int32)(((sEncCtrl.GainsUnq_Q16[i]) * (opus_int64)((opus_int16)(tmp))) >> 16)))) > (((((opus_int32)0x80000000)) >> ((8)))) ? (((((opus_int32)0x80000000)) >> ((8)))) : (((((opus_int32)(((sEncCtrl.GainsUnq_Q16[i]) * (opus_int64)((opus_int16)(tmp))) >> 16)))) < (((0x7FFFFFFF) >> ((8)))) ? (((0x7FFFFFFF) >> ((8)))) : ((((opus_int32)(((sEncCtrl.GainsUnq_Q16[i]) * (opus_int64)((opus_int16)(tmp))) >> 16)))))) : (((((opus_int32)(((sEncCtrl.GainsUnq_Q16[i]) * (opus_int64)((opus_int16)(tmp))) >> 16)))) > (((0x7FFFFFFF) >> ((8)))) ? (((0x7FFFFFFF) >> ((8)))) : (((((opus_int32)(((sEncCtrl.GainsUnq_Q16[i]) * (opus_int64)((opus_int16)(tmp))) >> 16)))) < (((((opus_int32)0x80000000)) >> ((8)))) ? (((((opus_int32)0x80000000)) >> ((8)))) : ((((opus_int32)(((sEncCtrl.GainsUnq_Q16[i]) * (opus_int64)((opus_int16)(tmp))) >> 16)))))))) << ((8)))));
}
      psEnc->sShape.LastGainIndex = sEncCtrl.lastGainIndexPrev;
      silk_gains_quant(psEnc->sCmn.indices.GainsIndices, pGains_Q16, &psEnc->sShape.LastGainIndex, condCoding == 2, psEnc->sCmn.nb_subfr);
      gainsID = silk_gains_ID(psEnc->sCmn.indices.GainsIndices, psEnc->sCmn.nb_subfr);
      for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) sEncCtrl.Gains[index] = pGains_Q16[index] / 65536.0f; }
}
  move_n_bytes(&psEnc->x_buf[psEnc->sCmn.frame_length], static_cast<std::size_t>((psEnc->sCmn.ltp_mem_length + 5 * psEnc->sCmn.fs_kHz) * sizeof(float)), psEnc->x_buf);
  if (psEnc->sCmn.prefillFlag) {
    *pnBytesOut = 0;
    return ret;
}
  psEnc->sCmn.prevLag = sEncCtrl.pitchL[psEnc->sCmn.nb_subfr - 1];
  psEnc->sCmn.prevSignalType = psEnc->sCmn.indices.signalType; psEnc->sCmn.first_frame_after_reset = 0;
  *pnBytesOut = ((ec_tell(psRangeEnc) + 7) >> (3));
  return ret;
}
void silk_find_LPC_FLP(silk_encoder_state *psEncC, opus_int16 NLSF_Q15[], const float x[], const float minInvGain) {
  int k, subfr_length;
  std::array<float, 16> a{};
  float res_nrg, res_nrg_2nd, res_nrg_interp;
  std::array<opus_int16, 16> NLSF0_Q15{};
  std::array<float, 16> a_tmp{};
  std::array<float, ((5 * 4) * 16) + 4 * 16> LPC_res{};
  subfr_length = psEncC->subfr_length + psEncC->predictLPCOrder; psEncC->indices.NLSFInterpCoef_Q2 = 4;
  res_nrg = silk_burg_modified_FLP(a.data(), x, minInvGain, subfr_length, psEncC->nb_subfr, psEncC->predictLPCOrder);
  if (psEncC->useInterpolatedNLSFs && !psEncC->first_frame_after_reset && psEncC->nb_subfr == 4) {
    res_nrg -= silk_burg_modified_FLP(a_tmp.data(), x + (4 / 2) * subfr_length, minInvGain, subfr_length, 4 / 2, psEncC->predictLPCOrder);
    silk_A2NLSF_FLP(NLSF_Q15, a_tmp.data(), psEncC->predictLPCOrder); res_nrg_2nd = 3.40282346638528859811704183484516925e+38F;
    for (k = 3; k >= 0; k--) {
      silk_interpolate(std::span<opus_int16>{NLSF0_Q15.data(), static_cast<std::size_t>(psEncC->predictLPCOrder)}, std::span<const opus_int16>{psEncC->prev_NLSFq_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, std::span<const opus_int16>{NLSF_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, k);
      silk_NLSF2A_FLP(a_tmp.data(), NLSF0_Q15.data(), psEncC->predictLPCOrder);
      silk_LPC_analysis_filter_FLP(LPC_res.data(), a_tmp.data(), x, 2 * subfr_length, psEncC->predictLPCOrder);
      res_nrg_interp = (float)(silk_energy_FLP(LPC_res.data() + psEncC->predictLPCOrder, subfr_length - psEncC->predictLPCOrder) + silk_energy_FLP(LPC_res.data() + psEncC->predictLPCOrder + subfr_length, subfr_length - psEncC->predictLPCOrder));
      if (res_nrg_interp < res_nrg) {
        res_nrg = res_nrg_interp; psEncC->indices.NLSFInterpCoef_Q2 = (opus_int8)k;
      } else if (res_nrg_interp > res_nrg_2nd) { break;
}
      res_nrg_2nd = res_nrg_interp; }
}
  if (psEncC->indices.NLSFInterpCoef_Q2 == 4) { silk_A2NLSF_FLP(NLSF_Q15, a.data(), psEncC->predictLPCOrder);
}
  opus_assume(psEncC->indices.NLSFInterpCoef_Q2 == 4 || (psEncC->useInterpolatedNLSFs && !psEncC->first_frame_after_reset && psEncC->nb_subfr == 4));
}
void silk_find_LTP_FLP(float XX[4 * 5 * 5], float xX[4 * 5], const float r_ptr[], const int lag[4], const int subfr_length, const int nb_subfr) {
  int k;
  float *xX_ptr, *XX_ptr;
  const float *lag_ptr;
  float xx, temp;
  xX_ptr = xX; XX_ptr = XX;
  for (k = 0; k < nb_subfr; k++) {
    lag_ptr = r_ptr - (lag[k] + 5 / 2); silk_corrMatrix_FLP(std::span<const float>{lag_ptr, static_cast<std::size_t>(subfr_length + 4)}, subfr_length, 5, std::span<float>{XX_ptr, static_cast<std::size_t>(25)});
    silk_corrVector_FLP(std::span<const float>{lag_ptr, static_cast<std::size_t>(subfr_length + 4)}, std::span<const float>{r_ptr, static_cast<std::size_t>(subfr_length)}, std::span<float>{xX_ptr, static_cast<std::size_t>(5)}); xx = (float)silk_energy_FLP(r_ptr, subfr_length + 5);
    temp = 1.0f / (((xx) > (0.03f * 0.5f * (XX_ptr[0] + XX_ptr[24]) + 1.0f)) ? (xx) : (0.03f * 0.5f * (XX_ptr[0] + XX_ptr[24]) + 1.0f));
    silk_scale_vector_FLP(XX_ptr, temp, 5 * 5); silk_scale_vector_FLP(xX_ptr, temp, 5); r_ptr += subfr_length; XX_ptr += 5 * 5; xX_ptr += 5; }
}
OPUS_SIZE_OPT void silk_find_pitch_lags_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, float res[], const float x[]) {
  int buf_len;
  float thrhld, res_nrg;
  const float *x_buf_ptr, *x_buf;
  std::array<float, 16 + 1> auto_corr{};
  std::array<float, 16> A{};
  std::array<float, 16> refl_coef{};
  std::array<float, ((20 + (2 << 1)) * 16)> Wsig{};
  float *Wsig_ptr;
  buf_len = psEnc->sCmn.la_pitch + psEnc->sCmn.frame_length + psEnc->sCmn.ltp_mem_length;
  opus_assume(buf_len >= psEnc->sCmn.pitch_LPC_win_length); x_buf = x - psEnc->sCmn.ltp_mem_length;
  x_buf_ptr = x_buf + buf_len - psEnc->sCmn.pitch_LPC_win_length; Wsig_ptr = Wsig.data();
  silk_apply_sine_window_FLP(std::span<float>{Wsig_ptr, static_cast<std::size_t>(psEnc->sCmn.la_pitch)}, std::span<const float>{x_buf_ptr, static_cast<std::size_t>(psEnc->sCmn.la_pitch)}, 1); Wsig_ptr += psEnc->sCmn.la_pitch;
  x_buf_ptr += psEnc->sCmn.la_pitch;
  copy_n_bytes(x_buf_ptr, static_cast<std::size_t>((psEnc->sCmn.pitch_LPC_win_length - (psEnc->sCmn.la_pitch << 1)) * sizeof(float)), Wsig_ptr);
  Wsig_ptr += psEnc->sCmn.pitch_LPC_win_length - (psEnc->sCmn.la_pitch << 1);
  x_buf_ptr += psEnc->sCmn.pitch_LPC_win_length - (psEnc->sCmn.la_pitch << 1);
  silk_apply_sine_window_FLP(std::span<float>{Wsig_ptr, static_cast<std::size_t>(psEnc->sCmn.la_pitch)}, std::span<const float>{x_buf_ptr, static_cast<std::size_t>(psEnc->sCmn.la_pitch)}, 2);
  silk_autocorrelation_FLP(auto_corr.data(), Wsig.data(), psEnc->sCmn.pitch_LPC_win_length, psEnc->sCmn.pitchEstimationLPCOrder + 1);
  auto_corr[0] += auto_corr[0] * 1e-3f + 1; res_nrg = silk_schur_FLP(refl_coef.data(), auto_corr.data(), psEnc->sCmn.pitchEstimationLPCOrder);
  psEncCtrl->predGain = auto_corr[0] / (((res_nrg) > (1.0f)) ? (res_nrg) : (1.0f));
  silk_k2a_FLP(A.data(), refl_coef.data(), psEnc->sCmn.pitchEstimationLPCOrder); silk_bwexpander_FLP(std::span<float>{A.data(), static_cast<std::size_t>(psEnc->sCmn.pitchEstimationLPCOrder)}, 0.99f);
  silk_LPC_analysis_filter_FLP(res, A.data(), x_buf, buf_len, psEnc->sCmn.pitchEstimationLPCOrder);
  if (psEnc->sCmn.indices.signalType != 0 && psEnc->sCmn.first_frame_after_reset == 0) {
    thrhld = 0.6f; thrhld -= 0.004f * psEnc->sCmn.pitchEstimationLPCOrder;
    thrhld -= 0.1f * psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f); thrhld -= 0.15f * (psEnc->sCmn.prevSignalType >> 1);
    thrhld -= 0.1f * psEnc->sCmn.input_tilt_Q15 * (1.0f / 32768.0f);
    if (silk_pitch_analysis_core_FLP(res, psEncCtrl->pitchL, &psEnc->sCmn.indices.lagIndex, &psEnc->sCmn.indices.contourIndex, &psEnc->LTPCorr, psEnc->sCmn.prevLag, psEnc->sCmn.pitchEstimationThreshold_Q16 / 65536.0f, thrhld, psEnc->sCmn.fs_kHz, psEnc->sCmn.pitchEstimationComplexity, psEnc->sCmn.nb_subfr) == 0) {
      psEnc->sCmn.indices.signalType = 2;
    } else { psEnc->sCmn.indices.signalType = 1;
}
  } else {
    zero_n_bytes(psEncCtrl->pitchL, static_cast<std::size_t>(sizeof(psEncCtrl->pitchL)));
    psEnc->sCmn.indices.lagIndex = 0; psEnc->sCmn.indices.contourIndex = 0; psEnc->LTPCorr = 0; }
}
OPUS_SIZE_OPT void silk_find_pred_coefs_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, const float res_pitch[], const float x[], int condCoding) {
  int i;
  std::array<float, 4 * 5 * 5> XXLTP{};
  std::array<float, 4 * 5> xXLTP{};
  std::array<float, 4> invGains{};
  std::array<opus_int16, 16> NLSF_Q15{};
  const float *x_ptr;
  float *x_pre_ptr; std::array<float, 4 * 16 + ((5 * 4) * 16)> LPC_in_pre{};
  float minInvGain;
  for (i = 0; i < psEnc->sCmn.nb_subfr; i++) { opus_assume(psEncCtrl->Gains[i] > 0.0f); invGains[i] = 1.0f / psEncCtrl->Gains[i];
}
  if (psEnc->sCmn.indices.signalType == 2) {
    opus_assume(psEnc->sCmn.ltp_mem_length - psEnc->sCmn.predictLPCOrder >= psEncCtrl->pitchL[0] + 5 / 2);
    silk_find_LTP_FLP(XXLTP.data(), xXLTP.data(), res_pitch, psEncCtrl->pitchL, psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr);
    silk_quant_LTP_gains_FLP(psEncCtrl->LTPCoef, psEnc->sCmn.indices.LTPIndex, &psEnc->sCmn.indices.PERIndex, &psEnc->sCmn.sum_log_gain_Q7, &psEncCtrl->LTPredCodGain, XXLTP.data(), xXLTP.data(), psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr);
    silk_LTP_scale_ctrl_FLP(psEnc, psEncCtrl, condCoding);
    silk_LTP_analysis_filter_FLP(LPC_in_pre.data(), x - psEnc->sCmn.predictLPCOrder, psEncCtrl->LTPCoef, psEncCtrl->pitchL, invGains.data(), psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr, psEnc->sCmn.predictLPCOrder);
  } else {
    x_ptr = x - psEnc->sCmn.predictLPCOrder; x_pre_ptr = LPC_in_pre.data();
    for (i = 0; i < psEnc->sCmn.nb_subfr; i++) { silk_scale_copy_vector_FLP(x_pre_ptr, x_ptr, invGains[i], psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder); x_pre_ptr += psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder; x_ptr += psEnc->sCmn.subfr_length;
}
    zero_n_bytes(psEncCtrl->LTPCoef, static_cast<std::size_t>(psEnc->sCmn.nb_subfr * 5 * sizeof(float)));
    psEncCtrl->LTPredCodGain = 0.0f; psEnc->sCmn.sum_log_gain_Q7 = 0;
}
  if (psEnc->sCmn.first_frame_after_reset) {
    minInvGain = 1.0f / 1e2f;
  } else { minInvGain = (float)pow(2, psEncCtrl->LTPredCodGain / 3) / 1e4f; minInvGain /= 0.25f + 0.75f * psEncCtrl->coding_quality;
}
  silk_find_LPC_FLP(&psEnc->sCmn, NLSF_Q15.data(), LPC_in_pre.data(), minInvGain);
  silk_process_NLSFs_FLP(&psEnc->sCmn, psEncCtrl->PredCoef, NLSF_Q15.data(), psEnc->sCmn.prev_NLSFq_Q15);
  silk_residual_energy_FLP(psEncCtrl->ResNrg, LPC_in_pre.data(), psEncCtrl->PredCoef, psEncCtrl->Gains, psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr, psEnc->sCmn.predictLPCOrder);
  copy_n_bytes(NLSF_Q15.data(), static_cast<std::size_t>(sizeof(psEnc->sCmn.prev_NLSFq_Q15)), psEnc->sCmn.prev_NLSFq_Q15);
}
namespace {
template <std::size_t Order> auto silk_lpc_analysis_filter_impl(std::span<float> residual, std::span<const float, Order> pred_coef, std::span<const float> signal) noexcept -> void {
  for (auto index = static_cast<int>(Order); index < static_cast<int>(signal.size()); ++index) {
    const auto *history = signal.data() + index - 1;
    auto prediction = 0.0f;
    for (std::size_t tap = 0; tap < Order; ++tap) { prediction += history[-static_cast<int>(tap)] * pred_coef[tap];
}
    residual[index] = history[1] - prediction; }
}
[[nodiscard]] constexpr auto max_abs_index(std::span<const float> coefs) noexcept -> std::pair<float, int> {
  auto max_abs = -1.0f;
  auto max_index = 0;
  for (auto index = 0; index < static_cast<int>(coefs.size()); ++index) {
    const auto magnitude = static_cast<float>(std::fabs(coefs[index]));
    if (magnitude > max_abs) { max_abs = magnitude; max_index = index; }
}
  return {max_abs, max_index};
}
} // namespace
void silk_LPC_analysis_filter_FLP(float r_LPC[], const float PredCoef[], const float s[], const int length, const int Order) {
  opus_assume(Order <= length);
  auto residual = std::span<float>{r_LPC, static_cast<std::size_t>(length)};
  auto signal = std::span<const float>{s, static_cast<std::size_t>(length)};
  switch (Order) {
  case 6: silk_lpc_analysis_filter_impl<6>(residual, std::span<const float, 6>{PredCoef, 6}, signal); break;
  case 8: silk_lpc_analysis_filter_impl<8>(residual, std::span<const float, 8>{PredCoef, 8}, signal); break;
  case 10: silk_lpc_analysis_filter_impl<10>(residual, std::span<const float, 10>{PredCoef, 10}, signal); break;
  case 12: silk_lpc_analysis_filter_impl<12>(residual, std::span<const float, 12>{PredCoef, 12}, signal); break;
  case 16: silk_lpc_analysis_filter_impl<16>(residual, std::span<const float, 16>{PredCoef, 16}, signal); break;
  default: break;
}
fill_n_items(residual.data(), static_cast<std::size_t>(Order), 0.0f);
}
void silk_LTP_analysis_filter_FLP(float *LTP_res, const float *x, const float B[5 * 4], const int pitchL[4], const float invGains[4], const int subfr_length, const int nb_subfr, const int pre_length) {
  const float *x_ptr, *x_lag_ptr;
  std::array<float, 5> Btmp{};
  float *LTP_res_ptr;
  float inv_gain;
  int k, i, j;
  x_ptr = x; LTP_res_ptr = LTP_res;
  for (k = 0; k < nb_subfr; k++) {
    x_lag_ptr = x_ptr - pitchL[k]; inv_gain = invGains[k];
    copy_n_bytes(B + k * 5, 5 * sizeof(float), Btmp.data());
    for (i = 0; i < subfr_length+pre_length; i++) { LTP_res_ptr[i] = x_ptr[i]; for (j=0;j<5;j++) { LTP_res_ptr[i] -= Btmp[j]*x_lag_ptr[5/2-j]; } LTP_res_ptr[i] *= inv_gain; x_lag_ptr++;
}
    LTP_res_ptr += subfr_length + pre_length; x_ptr += subfr_length; }
}
void silk_LTP_scale_ctrl_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, int condCoding) {
  constexpr auto code_independently = 0;
  constexpr auto plc_assumed_packet_loss_percent = 3;
  // Bias only the 32 kbps stereo SILK window toward cleaner post-loss recovery.
  if (condCoding == code_independently && psEnc->sCmn.nChannelsInternal == 2 && psEnc->sCmn.TargetRate_bps >= hybrid_silk_lowrate_boost_min_bps && psEnc->sCmn.TargetRate_bps <= hybrid_silk_lowrate_boost_max_bps) {
    const auto round_loss = plc_assumed_packet_loss_percent * psEnc->sCmn.nFramesPerPacket;
    const auto scaled_gain = static_cast<opus_int32>(psEncCtrl->LTPredCodGain) * round_loss;
    psEnc->sCmn.indices.LTP_scaleIndex = scaled_gain > silk_log2lin(2900 - psEnc->sCmn.SNR_dB_Q7);
    psEnc->sCmn.indices.LTP_scaleIndex += scaled_gain > silk_log2lin(3900 - psEnc->sCmn.SNR_dB_Q7);
  } else {
    psEnc->sCmn.indices.LTP_scaleIndex = 0;
  }
  psEncCtrl->LTP_scale = (float)silk_LTPScales_table_Q14[psEnc->sCmn.indices.LTP_scaleIndex] / 16384.0f;
}
[[nodiscard]] constexpr auto warped_gain(std::span<const float> coefs, float lambda) noexcept -> float {
  lambda = -lambda;
  auto gain = coefs.back();
  for (auto index = static_cast<int>(coefs.size()) - 2; index >= 0; --index) { gain = lambda * gain + coefs[index];
}
  return static_cast<float>(1.0f / (1.0f - lambda * gain));
}
static auto warped_true2monic_coefs(std::span<float> coefs, float lambda, float limit) noexcept -> void {
  for (auto index = static_cast<int>(coefs.size()) - 1; index > 0; --index) { coefs[index - 1] -= lambda * coefs[index];
}
  auto gain = (1.0f - lambda * lambda) / (1.0f + lambda * coefs.front());
  for (auto index = std::size_t{}; index < coefs.size(); ++index) { coefs[index] *= gain;
}
  for (auto iter = 0; iter < 10; ++iter) {
    const auto [max_abs, max_index] = max_abs_index(coefs);
    if (max_abs <= limit) { return;
}
    for (auto index = 1; index < static_cast<int>(coefs.size()); ++index) { coefs[index - 1] += lambda * coefs[index];
}
    gain = 1.0f / gain;
    for (auto index = std::size_t{}; index < coefs.size(); ++index) { coefs[index] *= gain;
}
    const auto chirp = 0.99f - (0.8f + 0.1f * iter) * (max_abs - limit) / (max_abs * (max_index + 1));
    silk_bwexpander_FLP(coefs, chirp);
    for (auto index = static_cast<int>(coefs.size()) - 1; index > 0; --index) { coefs[index - 1] -= lambda * coefs[index];
}
    gain = (1.0f - lambda * lambda) / (1.0f + lambda * coefs.front());
    for (auto index = std::size_t{}; index < coefs.size(); ++index) { coefs[index] *= gain; } }
}
static auto limit_coefs(std::span<float> coefs, float limit) noexcept -> void {
  for (auto iter = 0; iter < 10; ++iter) {
    const auto [max_abs, max_index] = max_abs_index(coefs);
    if (max_abs <= limit) { return;
}
    const auto chirp = 0.99f - (0.8f + 0.1f * iter) * (max_abs - limit) / (max_abs * (max_index + 1));
    silk_bwexpander_FLP(coefs, chirp); }
}
OPUS_SIZE_OPT void silk_noise_shape_analysis_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, const float *pitch_res, const float *x) {
  silk_shape_state_FLP *psShapeSt = &psEnc->sShape;
  int k, nSamples, nSegs;
  float SNR_adj_dB, HarmShapeGain, Tilt;
  float nrg, log_energy, log_energy_prev, energy_variation;
  float BWExp, gain_mult, gain_add, strength, b, warping;
  std::array<float, (15 * 16)> x_windowed{};
  std::array<float, 24 + 1> auto_corr{};
  std::array<float, 24 + 1> rc{};
  const float *x_ptr, *pitch_res_ptr;
  x_ptr = x - psEnc->sCmn.la_shape; SNR_adj_dB = psEnc->sCmn.SNR_dB_Q7 * (1 / 128.0f);
  psEncCtrl->input_quality = 0.5f * (psEnc->sCmn.input_quality_bands_Q15[0] + psEnc->sCmn.input_quality_bands_Q15[1]) * (1.0f / 32768.0f);
  psEncCtrl->coding_quality = silk_sigmoid(0.25f * (SNR_adj_dB - 20.0f));
  if (psEnc->sCmn.useCBR == 0) {
    b = 1.0f - psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f);
    SNR_adj_dB -= 2.0f * psEncCtrl->coding_quality * (0.5f + 0.5f * psEncCtrl->input_quality) * b * b;
}
  if (psEnc->sCmn.indices.signalType == 2) {
    SNR_adj_dB += 2.0f * psEnc->LTPCorr;
  } else { SNR_adj_dB += (-0.4f * psEnc->sCmn.SNR_dB_Q7 * (1 / 128.0f) + 6.0f) * (1.0f - psEncCtrl->input_quality);
}
  if (psEnc->sCmn.indices.signalType == 2) {
    psEnc->sCmn.indices.quantOffsetType = 0;
  } else {
    nSamples = 2 * psEnc->sCmn.fs_kHz; energy_variation = 0.0f; log_energy_prev = 0.0f; pitch_res_ptr = pitch_res;
    nSegs = ((opus_int32)((opus_int16)(5)) * (opus_int32)((opus_int16)(psEnc->sCmn.nb_subfr))) / 2;
    for (k = 0; k < nSegs; k++) {
      nrg = (float)nSamples + (float)silk_energy_FLP(pitch_res_ptr, nSamples); log_energy = silk_log2(nrg);
      if (k > 0) { energy_variation += silk_abs_float_reference(log_energy - log_energy_prev);
}
      log_energy_prev = log_energy; pitch_res_ptr += nSamples;
}
  if (energy_variation > 0.6f * (nSegs - 1)) {
      psEnc->sCmn.indices.quantOffsetType = 0;
    } else { psEnc->sCmn.indices.quantOffsetType = 1; }
}
  strength = 1e-3f * psEncCtrl->predGain; BWExp = 0.94f / (1.0f + strength * strength);
  warping = (float)psEnc->sCmn.warping_Q16 / 65536.0f + 0.01f * psEncCtrl->coding_quality;
  for (k = 0; k < psEnc->sCmn.nb_subfr; k++) {
    int shift, slope_part, flat_part;
    flat_part = psEnc->sCmn.fs_kHz * 3; slope_part = (psEnc->sCmn.shapeWinLength - flat_part) / 2;
    silk_apply_sine_window_FLP(std::span<float>{x_windowed.data(), static_cast<std::size_t>(slope_part)}, std::span<const float>{x_ptr, static_cast<std::size_t>(slope_part)}, 1); shift = slope_part;
    copy_n_bytes(x_ptr + shift, static_cast<std::size_t>(flat_part * sizeof(float)), x_windowed.data() + shift);
    shift += flat_part; silk_apply_sine_window_FLP(std::span<float>{x_windowed.data() + shift, static_cast<std::size_t>(slope_part)}, std::span<const float>{x_ptr + shift, static_cast<std::size_t>(slope_part)}, 2); x_ptr += psEnc->sCmn.subfr_length;
    if (psEnc->sCmn.warping_Q16 > 0) {
      silk_warped_autocorrelation_FLP(auto_corr.data(), x_windowed.data(), warping, psEnc->sCmn.shapeWinLength, psEnc->sCmn.shapingLPCOrder);
    } else { silk_autocorrelation_FLP(auto_corr.data(), x_windowed.data(), psEnc->sCmn.shapeWinLength, psEnc->sCmn.shapingLPCOrder + 1);
}
    auto_corr[0] += auto_corr[0] * 3e-5f + 1.0f; nrg = silk_schur_FLP(rc.data(), auto_corr.data(), psEnc->sCmn.shapingLPCOrder);
    silk_k2a_FLP(&psEncCtrl->AR[k * 24], rc.data(), psEnc->sCmn.shapingLPCOrder); psEncCtrl->Gains[k] = silk_sqrt_reference(nrg);
    if (psEnc->sCmn.warping_Q16 > 0) { psEncCtrl->Gains[k] *= warped_gain(std::span<const float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, warping);
}
    silk_bwexpander_FLP(std::span<float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, BWExp);
    if (psEnc->sCmn.warping_Q16 > 0) {
      warped_true2monic_coefs(std::span<float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, warping, 3.999f);
    } else {
      limit_coefs(std::span<float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, 3.999f); }
}
  gain_mult = silk_pow_reference(2.0f, -0.16f * SNR_adj_dB); gain_add = silk_pow_reference(2.0f, 0.16f * 2);
  for (int i = 0; i < psEnc->sCmn.nb_subfr; ++i) psEncCtrl->Gains[i] = psEncCtrl->Gains[i] * gain_mult + gain_add;
  strength = 4.0f * (1.0f + 0.5f * (psEnc->sCmn.input_quality_bands_Q15[0] * (1.0f / 32768.0f) - 1.0f));
  strength *= psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f);
  if (psEnc->sCmn.indices.signalType == 2) {
    for (k = 0; k < psEnc->sCmn.nb_subfr; k++) {
      b = 0.2f / psEnc->sCmn.fs_kHz + 3.0f / psEncCtrl->pitchL[k]; psEncCtrl->LF_MA_shp[k] = -1.0f + b;
      psEncCtrl->LF_AR_shp[k] = 1.0f - b - b * strength;
}
    Tilt = -0.25f - (1 - 0.25f) * 0.35f * psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f);
  } else {
    b = 1.3f / psEnc->sCmn.fs_kHz; psEncCtrl->LF_MA_shp[0] = -1.0f + b; psEncCtrl->LF_AR_shp[0] = 1.0f - b - b * strength * 0.6f;
fill_n_items(psEncCtrl->LF_MA_shp + 1, static_cast<std::size_t>(psEnc->sCmn.nb_subfr - 1), psEncCtrl->LF_MA_shp[0]);
fill_n_items(psEncCtrl->LF_AR_shp + 1, static_cast<std::size_t>(psEnc->sCmn.nb_subfr - 1), psEncCtrl->LF_AR_shp[0]);
    Tilt = -0.25f;
}
  if (1 && psEnc->sCmn.indices.signalType == 2) {
    HarmShapeGain = 0.3f; HarmShapeGain += 0.2f * (1.0f - (1.0f - psEncCtrl->coding_quality) * psEncCtrl->input_quality);
    HarmShapeGain *= silk_sqrt_reference(psEnc->LTPCorr);
  } else { HarmShapeGain = 0.0f;
}
  for (k = 0; k < psEnc->sCmn.nb_subfr; k++) { psShapeSt->HarmShapeGain_smth += 0.4f * (HarmShapeGain - psShapeSt->HarmShapeGain_smth); psEncCtrl->HarmShapeGain[k] = psShapeSt->HarmShapeGain_smth; psShapeSt->Tilt_smth += 0.4f * (Tilt - psShapeSt->Tilt_smth); psEncCtrl->Tilt[k] = psShapeSt->Tilt_smth; }
}
void silk_process_gains_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, int condCoding) {
  silk_shape_state_FLP *psShapeSt = &psEnc->sShape;
  int k;
  std::array<opus_int32, 4> pGains_Q16{};
  float s, InvMaxSqrVal, gain, quant_offset;
  if (psEnc->sCmn.indices.signalType == 2) {
    s = 1.0f - 0.5f * silk_sigmoid(0.25f * (psEncCtrl->LTPredCodGain - 12.0f));
    for (int i = 0; i < psEnc->sCmn.nb_subfr; ++i) psEncCtrl->Gains[i] *= s;
}
  InvMaxSqrVal = (float)(pow(2.0f, 0.33f * (21.0f - psEnc->sCmn.SNR_dB_Q7 * (1 / 128.0f))) / psEnc->sCmn.subfr_length);
  for (k = 0; k < psEnc->sCmn.nb_subfr; k++) { gain = (float)sqrt(psEncCtrl->Gains[k]*psEncCtrl->Gains[k]+psEncCtrl->ResNrg[k]*InvMaxSqrVal); psEncCtrl->Gains[k] = std::min(gain, 32767.0f);
}
  for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) pGains_Q16[index] = static_cast<opus_int32>(psEncCtrl->Gains[index] * 65536.0f);
  copy_n_bytes(pGains_Q16.data(), static_cast<std::size_t>(psEnc->sCmn.nb_subfr * sizeof(opus_int32)), psEncCtrl->GainsUnq_Q16);
  psEncCtrl->lastGainIndexPrev = psShapeSt->LastGainIndex;
  silk_gains_quant(psEnc->sCmn.indices.GainsIndices, pGains_Q16.data(), &psShapeSt->LastGainIndex, condCoding == 2, psEnc->sCmn.nb_subfr);
  for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) psEncCtrl->Gains[index] = pGains_Q16[index] / 65536.0f;
  if (psEnc->sCmn.indices.signalType == 2) {
    psEnc->sCmn.indices.quantOffsetType = psEncCtrl->LTPredCodGain + psEnc->sCmn.input_tilt_Q15 * (1.0f / 32768.0f) > 1.0f ? 0 : 1;
}
  quant_offset = silk_Quantization_Offsets_Q10[psEnc->sCmn.indices.signalType >> 1][psEnc->sCmn.indices.quantOffsetType] / 1024.0f;
  psEncCtrl->Lambda = 1.2f + -0.05f * psEnc->sCmn.nStatesDelayedDecision + -0.2f * psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f) + -0.1f * psEncCtrl->input_quality + -0.2f * psEncCtrl->coding_quality + 0.8f * quant_offset;
  opus_assume(psEncCtrl->Lambda > 0.0f); opus_assume(psEncCtrl->Lambda < 2.0f);
}
void silk_residual_energy_FLP(float nrgs[4], const float x[], float a[2][16], const float gains[], const int subfr_length, const int nb_subfr, const int LPC_order) {
  int shift;
  std::array<float, (((5 * 4) * 16) + 4 * 16) / 2> LPC_res{};
  auto *LPC_res_ptr = LPC_res.data() + LPC_order; shift = LPC_order + subfr_length;
  silk_LPC_analysis_filter_FLP(LPC_res.data(), a[0], x + 0 * shift, 2 * shift, LPC_order);
  nrgs[0] = (float)(gains[0] * gains[0] * silk_energy_FLP(LPC_res_ptr + 0 * shift, subfr_length));
  nrgs[1] = (float)(gains[1] * gains[1] * silk_energy_FLP(LPC_res_ptr + 1 * shift, subfr_length));
  if (nb_subfr == 4) {
    silk_LPC_analysis_filter_FLP(LPC_res.data(), a[1], x + 2 * shift, 2 * shift, LPC_order);
    nrgs[2] = (float)(gains[2] * gains[2] * silk_energy_FLP(LPC_res_ptr + 0 * shift, subfr_length));
    nrgs[3] = (float)(gains[3] * gains[3] * silk_energy_FLP(LPC_res_ptr + 1 * shift, subfr_length)); }
}
void silk_warped_autocorrelation_FLP(float *corr, const float *input, const float warping, const int length, const int order) {
  opus_assume((order & 1) == 0);
  double state[24 + 1] = {0};
  double C[24 + 1] = {0};
  for (int n = 0; n < length; n++) {
    double tmp1 = input[n];
    for (int i = 0; i < order; i += 2) {
      double tmp2 = state[i] + warping * state[i + 1] - warping * tmp1;
      state[i] = tmp1; C[i] += state[0] * tmp1;
      tmp1 = state[i + 1] + warping * state[i + 2] - warping * tmp2; state[i + 1] = tmp2; C[i + 1] += state[0] * tmp2;
}
    state[order] = tmp1; C[order] += state[0] * tmp1;
}
  for (int index = 0; index <= order; ++index) corr[index] = static_cast<float>(C[index]);
}
void silk_A2NLSF_FLP(opus_int16 *NLSF_Q15, const float *pAR, const int LPC_order) {
  std::array<opus_int32, 16> a_fix_Q16{};
  for (int index = 0; index < LPC_order; ++index) a_fix_Q16[index] = silk_float2int(pAR[index] * 65536.0f);
  silk_A2NLSF(NLSF_Q15, a_fix_Q16.data(), LPC_order);
}
void silk_NLSF2A_FLP(float *pAR, const opus_int16 *NLSF_Q15, const int LPC_order) {
  std::array<opus_int16, 16> a_fix_Q12{}; silk_NLSF2A(a_fix_Q12.data(), NLSF_Q15, LPC_order);
  for (int index = 0; index < LPC_order; ++index) pAR[index] = static_cast<float>(a_fix_Q12[index]) * (1.0f / 4096.0f);
}
void silk_process_NLSFs_FLP(silk_encoder_state *psEncC, float PredCoef[2][16], opus_int16 NLSF_Q15[16], const opus_int16 prev_NLSF_Q15[16]) {
  opus_int16 PredCoef_Q12[2][16]; silk_process_NLSFs(psEncC, PredCoef_Q12, NLSF_Q15, prev_NLSF_Q15);
  for (int j = 0; j < 2; j++) {
    for (int index = 0; index < psEncC->predictLPCOrder; ++index) PredCoef[j][index] = static_cast<float>(PredCoef_Q12[j][index]) * (1.0f / 4096.0f); }
}
OPUS_SIZE_OPT void silk_NSQ_wrapper_FLP(silk_encoder_state_FLP *psEnc, silk_encoder_control_FLP *psEncCtrl, SideInfoIndices *psIndices, silk_nsq_state *psNSQ, opus_int8 pulses[], const float x[]) {
  int i;
  std::array<opus_int16, ((5 * 4) * 16)> x16{}; std::array<opus_int32, 4> Gains_Q16{};
  opus_int16 PredCoef_Q12[2][16]; opus_int16 LTPCoef_Q14[5 * 4];
  int LTP_scale_Q14;
  std::array<opus_int16, 4 * 24> AR_Q13{}; std::array<opus_int32, 4> LF_shp_Q14{};
  int Lambda_Q10;
  std::array<int, 4> Tilt_Q14{}; std::array<int, 4> HarmShapeGain_Q14{};
  for (i = 0; i < psEnc->sCmn.nb_subfr; i++) {
    for (int index = 0; index < psEnc->sCmn.shapingLPCOrder; ++index) AR_Q13[static_cast<std::size_t>(i * 24 + index)] = static_cast<opus_int16>(silk_float2int(psEncCtrl->AR[i * 24 + index] * 8192.0f));
}
  for (i = 0; i < psEnc->sCmn.nb_subfr; i++) { LF_shp_Q14[i] = ((opus_int32)((opus_uint32)(silk_float2int(psEncCtrl->LF_AR_shp[i]*16384.0f))<<(16)))|(opus_uint16)silk_float2int(psEncCtrl->LF_MA_shp[i]*16384.0f); Tilt_Q14[i] = (int)silk_float2int(psEncCtrl->Tilt[i]*16384.0f); HarmShapeGain_Q14[i] = (int)silk_float2int(psEncCtrl->HarmShapeGain[i]*16384.0f);
}
  Lambda_Q10 = (int)silk_float2int(psEncCtrl->Lambda * 1024.0f);
  for (int index = 0; index < psEnc->sCmn.nb_subfr * 5; ++index) LTPCoef_Q14[index] = static_cast<opus_int16>(silk_float2int(psEncCtrl->LTPCoef[index] * 16384.0f));
  for (int j = 0; j < 2; j++) {
    for (int index = 0; index < psEnc->sCmn.predictLPCOrder; ++index) PredCoef_Q12[j][index] = static_cast<opus_int16>(silk_float2int(psEncCtrl->PredCoef[j][index] * 4096.0f));
}
  for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) {
    const auto v = silk_float2int(psEncCtrl->Gains[index] * 65536.0f);
    opus_assume(v > 0);
    Gains_Q16[index] = v;
}
  if (psIndices->signalType == 2) {
    LTP_scale_Q14 = silk_LTPScales_table_Q14[psIndices->LTP_scaleIndex];
  } else { LTP_scale_Q14 = 0;
}
  for (int index = 0; index < psEnc->sCmn.frame_length; ++index) x16[index] = static_cast<opus_int16>(silk_float2int(x[index]));
  if (psEnc->sCmn.nStatesDelayedDecision > 1 || psEnc->sCmn.warping_Q16 > 0) {
    silk_NSQ_del_dec_c(&psEnc->sCmn, psNSQ, psIndices, x16.data(), pulses, PredCoef_Q12[0], LTPCoef_Q14, AR_Q13.data(), HarmShapeGain_Q14.data(), Tilt_Q14.data(), LF_shp_Q14.data(), Gains_Q16.data(), psEncCtrl->pitchL, Lambda_Q10, LTP_scale_Q14);
  } else { silk_NSQ_c(&psEnc->sCmn, psNSQ, psIndices, x16.data(), pulses, PredCoef_Q12[0], LTPCoef_Q14, AR_Q13.data(), HarmShapeGain_Q14.data(), Tilt_Q14.data(), LF_shp_Q14.data(), Gains_Q16.data(), psEncCtrl->pitchL, Lambda_Q10, LTP_scale_Q14); }
}
void silk_quant_LTP_gains_FLP(float B[4 * 5], opus_int8 cbk_index[4], opus_int8 *periodicity_index, opus_int32 *sum_log_gain_Q7, float *pred_gain_dB, const float XX[4 * 5 * 5], const float xX[4 * 5], const int subfr_len, const int nb_subfr) {
  int pred_gain_dB_Q7;
  std::array<opus_int16, 4 * 5> B_Q14{}; std::array<opus_int32, 4 * 5 * 5> XX_Q17{}; std::array<opus_int32, 4 * 5> xX_Q17{};
  for (int index = 0; index < nb_subfr * 5 * 5; ++index) XX_Q17[index] = static_cast<opus_int32>(silk_float2int(XX[index] * 131072.0f));
  for (int index = 0; index < nb_subfr * 5; ++index) xX_Q17[index] = static_cast<opus_int32>(silk_float2int(xX[index] * 131072.0f));
  silk_quant_LTP_gains(B_Q14.data(), cbk_index, periodicity_index, sum_log_gain_Q7, &pred_gain_dB_Q7, XX_Q17.data(), xX_Q17.data(), subfr_len, nb_subfr);
  for (int index = 0; index < nb_subfr * 5; ++index) B[index] = static_cast<float>(B_Q14[index]) * (1.0f / 16384.0f);
  *pred_gain_dB = (float)pred_gain_dB_Q7 * (1.0f / 128.0f);
}
void silk_autocorrelation_FLP(float *results, const float *inputData, int inputDataSize, int correlationCount) {
  if (correlationCount > inputDataSize) correlationCount = inputDataSize;
  for (int i = 0; i < correlationCount; i++) { results[i] = (float)silk_inner_product_FLP_c(inputData, inputData + i, inputDataSize - i); }
}
OPUS_SIZE_OPT float silk_burg_modified_FLP(float A[], const float x[], const float minInvGain, const int subfr_length, const int nb_subfr, const int D) {
  int k, n, s, reached_max_gain;
  double C0, invGain, num, nrg_f, nrg_b, rc, Atmp, tmp1, tmp2;
  const float *x_ptr;
  double C_first_row[24], C_last_row[24];
  double CAf[24 + 1], CAb[24 + 1];
  double Af[24];
  opus_assume(subfr_length * nb_subfr <= 384); C0 = silk_energy_FLP(x, nb_subfr * subfr_length);
  zero_n_bytes(C_first_row, static_cast<std::size_t>(24 * sizeof(double)));
  for (s = 0; s < nb_subfr; s++) {
    x_ptr = x + s * subfr_length;
    for (n = 1; n < D + 1; n++) { C_first_row[n - 1] += silk_inner_product_FLP_c(x_ptr, x_ptr + n, subfr_length - n); }
}
  copy_n_bytes(C_first_row, static_cast<std::size_t>(24 * sizeof(double)), C_last_row);
  CAb[0] = CAf[0] = C0 + 1e-5f * C0 + 1e-9f; invGain = 1.0f; reached_max_gain = 0;
  for (n = 0; n < D; n++) {
    for (s = 0; s < nb_subfr; s++) {
      x_ptr = x + s * subfr_length; tmp1 = x_ptr[n]; tmp2 = x_ptr[subfr_length - n - 1];
      for (k = 0; k < n; k++) { C_first_row[k] -= x_ptr[n]*x_ptr[n-k-1]; C_last_row[k] -= x_ptr[subfr_length-n-1]*x_ptr[subfr_length-n+k]; Atmp = Af[k]; tmp1 += x_ptr[n-k-1]*Atmp; tmp2 += x_ptr[subfr_length-n+k]*Atmp;
}
      for (k = 0; k <= n; k++) { CAf[k] -= tmp1*x_ptr[n-k]; CAb[k] -= tmp2*x_ptr[subfr_length-n+k-1]; }
}
    tmp1 = C_first_row[n]; tmp2 = C_last_row[n];
    for (k = 0; k < n; k++) { Atmp = Af[k]; tmp1 += C_last_row[n-k-1]*Atmp; tmp2 += C_first_row[n-k-1]*Atmp;
}
    CAf[n + 1] = tmp1; CAb[n + 1] = tmp2; num = CAb[n + 1]; nrg_b = CAb[0]; nrg_f = CAf[0];
    for (k = 0; k < n; k++) { Atmp = Af[k]; num += CAb[n-k]*Atmp; nrg_b += CAb[k+1]*Atmp; nrg_f += CAf[k+1]*Atmp;
}
    opus_assume(nrg_f > 0.0); opus_assume(nrg_b > 0.0);
    rc = -2.0 * num / (nrg_f + nrg_b); opus_assume(rc > -1.0 && rc < 1.0); tmp1 = invGain * (1.0 - rc * rc);
    if (tmp1 <= minInvGain) {
      rc = sqrt(1.0 - minInvGain / invGain);
      if (num > 0) { rc = -rc;
}
      invGain = minInvGain; reached_max_gain = 1;
    } else { invGain = tmp1;
}
    for (k = 0; k < (n + 1) >> 1; k++) { tmp1 = Af[k]; tmp2 = Af[n - k - 1]; Af[k] = tmp1 + rc * tmp2; Af[n - k - 1] = tmp2 + rc * tmp1;
}
    Af[n] = rc;
if (reached_max_gain) { fill_n_items(Af + n + 1, static_cast<std::size_t>(D - n - 1), 0.0); break;
}
    for (k = 0; k <= n + 1; k++) { tmp1 = CAf[k]; CAf[k] += rc * CAb[n - k + 1]; CAb[n - k + 1] += rc * tmp1; }
}
  if (reached_max_gain) {
    for (int index = 0; index < D; ++index) A[index] = static_cast<float>(-Af[index]);
    for (s = 0; s < nb_subfr; s++) { C0 -= silk_energy_FLP(x + s * subfr_length, D);
}
    nrg_f = C0 * invGain;
  } else {
    nrg_f = CAf[0]; tmp1 = 1.0;
    for (k = 0; k < D; k++) { Atmp = Af[k]; nrg_f += CAf[k+1]*Atmp; tmp1 += Atmp*Atmp; A[k] = (float)-Atmp;
}
    nrg_f -= 1e-5f * C0 * tmp1;
}
  return (float)nrg_f;
}
void silk_bwexpander_FLP(std::span<float> ar, const float chirp) {
  float cfac = chirp;
  for (auto index = std::size_t{}; index < ar.size(); ++index) { ar[index] *= cfac; cfac *= chirp; }
}
double silk_energy_FLP(const float *data, int dataSize) {
  int i; double result = 0.0;
  for (i = 0; i < dataSize - 3; i += 4) { result += data[i]*(double)data[i] + data[i+1]*(double)data[i+1] + data[i+2]*(double)data[i+2] + data[i+3]*(double)data[i+3];
}
  for (; i < dataSize; i++) { result += data[i] * (double)data[i];
}
  opus_assume(result >= 0.0);
  return result;
}
double silk_inner_product_FLP_c(const float *data1, const float *data2, int dataSize) {
  int i; double result = 0.0;
  for (i = 0; i < dataSize - 3; i += 4) { result += data1[i]*(double)data2[i] + data1[i+1]*(double)data2[i+1] + data1[i+2]*(double)data2[i+2] + data1[i+3]*(double)data2[i+3];
}
  for (; i < dataSize; i++) { result += data1[i] * (double)data2[i];
}
  return result;
}
void silk_k2a_FLP(float *A, const float *rc, opus_int32 order) {
  for (int k = 0; k < order; k++) {
    const float rck = rc[k];
    for (int n = 0; n < (k + 1) >> 1; n++) {
      const float t1 = A[n], t2 = A[k - n - 1];
      A[n] = t1 + t2 * rck; A[k - n - 1] = t2 + t1 * rck;
}
    A[k] = -rck; }
}
static void silk_P_Ana_calc_corr_st3(float cross_corr_st3[4][34][5], const float frame[], int start_lag, int sf_length, int nb_subfr, int complexity);
static void silk_P_Ana_calc_energy_st3(float energies_st3[4][34][5], const float frame[], int start_lag, int sf_length, int nb_subfr, int complexity);
int silk_pitch_analysis_core_FLP(const float *frame, int *pitch_out, opus_int16 *lagIndex, opus_int8 *contourIndex, float *LTPCorr, int prevLag, const float search_thres1, const float search_thres2, const int Fs_kHz, const int complexity, const int nb_subfr) {
  int i, k, d, j;
  float frame_8kHz[((4 * 5) + 4 * 5) * 8];
  float frame_4kHz[((4 * 5) + 4 * 5) * 4];
  opus_int16 frame_8_FIX[((4 * 5) + 4 * 5) * 8]; opus_int16 frame_4_FIX[((4 * 5) + 4 * 5) * 4];
  opus_int32 filt_state[6];
  float threshold, contour_bias;
  float C[4][((18 * 16) >> 1) + 5];
  opus_val32 xcorr[18 * 4 - 2 * 4 + 1];
  float CC[11];
  const float *target_ptr, *basis_ptr;
  double cross_corr, normalizer, energy, energy_tmp;
  int d_srch[24];
  opus_int16 d_comp[((18 * 16) >> 1) + 5];
  int length_d_srch, length_d_comp;
  float Cmax, CCmax, CCmax_b, CCmax_new_b, CCmax_new;
  int CBimax, CBimax_new, lag, start_lag, end_lag, lag_new;
  float lag_log2, prevLag_log2, delta_lag_log2_sqr;
  float energies_st3[4][34][5];
  float cross_corr_st3[4][34][5];
  int lag_counter, frame_length, frame_length_8kHz, frame_length_4kHz, sf_length, sf_length_8kHz, sf_length_4kHz, min_lag, min_lag_8kHz, min_lag_4kHz, max_lag, max_lag_8kHz, max_lag_4kHz;
  opus_assume(Fs_kHz == 8 || Fs_kHz == 12 || Fs_kHz == 16); opus_assume(complexity >= 0);
  opus_assume(complexity <= 2); opus_assume(search_thres1 >= 0.0f && search_thres1 <= 1.0f);
  opus_assume(search_thres2 >= 0.0f && search_thres2 <= 1.0f); frame_length = ((4 * 5) + nb_subfr * 5) * Fs_kHz;
  frame_length_4kHz = ((4 * 5) + nb_subfr * 5) * 4; frame_length_8kHz = ((4 * 5) + nb_subfr * 5) * 8;
  sf_length = 5 * Fs_kHz; sf_length_4kHz = 5 * 4; sf_length_8kHz = 5 * 8; min_lag = 2 * Fs_kHz;
  min_lag_4kHz = 2 * 4; min_lag_8kHz = 2 * 8; max_lag = 18 * Fs_kHz - 1; max_lag_4kHz = 18 * 4; max_lag_8kHz = 18 * 8 - 1;
  if (Fs_kHz == 16) {
    opus_int16 frame_16_FIX[16 * ((4 * 5) + 4 * 5)]; silk_float2short_array(frame_16_FIX, frame, frame_length);
    zero_n_bytes(filt_state, static_cast<std::size_t>(2 * sizeof(opus_int32)));
    silk_resampler_down2(filt_state, frame_8_FIX, frame_16_FIX, frame_length);
    silk_short2float_array(frame_8kHz, frame_8_FIX, frame_length_8kHz);
  } else if (Fs_kHz == 12) {
    opus_int16 frame_12_FIX[12 * ((4 * 5) + 4 * 5)]; silk_float2short_array(frame_12_FIX, frame, frame_length);
    zero_n_bytes(filt_state, static_cast<std::size_t>(6 * sizeof(opus_int32)));
    silk_resampler_down2_3(filt_state, frame_8_FIX, frame_12_FIX, frame_length);
    silk_short2float_array(frame_8kHz, frame_8_FIX, frame_length_8kHz);
  } else { opus_assume(Fs_kHz == 8); silk_float2short_array(frame_8_FIX, frame, frame_length_8kHz);
}
  zero_n_bytes(filt_state, static_cast<std::size_t>(2 * sizeof(opus_int32)));
  silk_resampler_down2(filt_state, frame_4_FIX, frame_8_FIX, frame_length_8kHz);
  silk_short2float_array(frame_4kHz, frame_4_FIX, frame_length_4kHz);
  for (i = frame_length_4kHz - 1; i > 0; i--) { frame_4kHz[i] = saturate_int16_from_int32(static_cast<opus_int32>(frame_4kHz[i]) + frame_4kHz[i - 1]);
}
  zero_n_bytes(C, sizeof(C));
  target_ptr = &frame_4kHz[((opus_int32)((opus_uint32)(sf_length_4kHz) << (2)))];
  for (k = 0; k < nb_subfr >> 1; k++) {
    opus_assume(target_ptr >= frame_4kHz); opus_assume(target_ptr + sf_length_8kHz <= frame_4kHz + frame_length_4kHz);
    basis_ptr = target_ptr - min_lag_4kHz; opus_assume(basis_ptr >= frame_4kHz);
    opus_assume(basis_ptr + sf_length_8kHz <= frame_4kHz + frame_length_4kHz);
    celt_pitch_xcorr_c(std::span<const opus_val16>{target_ptr, static_cast<std::size_t>(sf_length_8kHz)}, std::span<const opus_val16>{target_ptr - max_lag_4kHz, static_cast<std::size_t>(sf_length_8kHz + max_lag_4kHz - min_lag_4kHz)}, std::span<opus_val32>{xcorr, static_cast<std::size_t>(max_lag_4kHz - min_lag_4kHz + 1)});
    cross_corr = xcorr[max_lag_4kHz - min_lag_4kHz];
    normalizer = silk_energy_FLP(target_ptr, sf_length_8kHz) + silk_energy_FLP(basis_ptr, sf_length_8kHz) + sf_length_8kHz * 4000.0f;
    C[0][min_lag_4kHz] += (float)(2 * cross_corr / normalizer);
    for (d = min_lag_4kHz+1; d <= max_lag_4kHz; d++) { basis_ptr--; opus_assume(basis_ptr >= frame_4kHz); opus_assume(basis_ptr+sf_length_8kHz <= frame_4kHz+frame_length_4kHz); cross_corr = xcorr[max_lag_4kHz-d]; normalizer += basis_ptr[0]*(double)basis_ptr[0]-basis_ptr[sf_length_8kHz]*(double)basis_ptr[sf_length_8kHz]; C[0][d] += (float)(2*cross_corr/normalizer);
}
    target_ptr += sf_length_8kHz;
}
  for (i = max_lag_4kHz; i >= min_lag_4kHz; i--) { C[0][i] -= C[0][i] * i / 4096.0f;
}
  length_d_srch = 4 + 2 * complexity; opus_assume(3 * length_d_srch <= 24);
  silk_insertion_sort_decreasing_FLP(&C[0][min_lag_4kHz], d_srch, max_lag_4kHz - min_lag_4kHz + 1, length_d_srch);
  Cmax = C[0][min_lag_4kHz];
  if (Cmax < 0.2f) {
    zero_n_bytes(pitch_out, static_cast<std::size_t>(nb_subfr * sizeof(int))); *LTPCorr = 0.0f; *lagIndex = 0; *contourIndex = 0;
    return 1;
}
  threshold = search_thres1 * Cmax;
  for (i = 0; i < length_d_srch; i++) {
    if (C[0][min_lag_4kHz + i] > threshold) {
      d_srch[i] = ((opus_int32)((opus_uint32)(d_srch[i] + min_lag_4kHz) << (1)));
    } else {
      length_d_srch = i;
      break; }
}
  opus_assume(length_d_srch > 0);
zero_n_items(d_comp + min_lag_8kHz - 5, static_cast<std::size_t>(max_lag_8kHz - min_lag_8kHz + 10));
  for (i = 0; i < length_d_srch; i++) { d_comp[d_srch[i]] = 1;
}
  for (i = max_lag_8kHz + 3; i >= min_lag_8kHz; i--) { d_comp[i] += d_comp[i - 1] + d_comp[i - 2];
}
  length_d_srch = 0;
  for (i = min_lag_8kHz; i < max_lag_8kHz + 1; i++) {
    if (d_comp[i + 1] > 0) { d_srch[length_d_srch] = i; length_d_srch++; }
}
  for (i = max_lag_8kHz + 3; i >= min_lag_8kHz; i--) { d_comp[i] += d_comp[i - 1] + d_comp[i - 2] + d_comp[i - 3];
}
  length_d_comp = 0;
  for (i = min_lag_8kHz; i < max_lag_8kHz + 4; i++) {
    if (d_comp[i] > 0) { d_comp[length_d_comp] = (opus_int16)(i - 2); length_d_comp++; }
}
  zero_n_bytes(C, static_cast<std::size_t>(4 * (((18 * 16) >> 1) + 5) * sizeof(float)));
  if (Fs_kHz == 8) { target_ptr = &frame[(4 * 5) * 8]; } else { target_ptr = &frame_8kHz[(4 * 5) * 8];
}
  for (k = 0; k < nb_subfr; k++) {
    energy_tmp = silk_energy_FLP(target_ptr, sf_length_8kHz) + 1.0;
    for (j = 0; j < length_d_comp; j++) {
      d = d_comp[j]; basis_ptr = target_ptr - d; cross_corr = silk_inner_product_FLP_c(basis_ptr, target_ptr, sf_length_8kHz);
      if (cross_corr > 0.0f) {
        energy = silk_energy_FLP(basis_ptr, sf_length_8kHz); C[k][d] = (float)(2 * cross_corr / (energy + energy_tmp));
      } else { C[k][d] = 0.0f; }
}
    target_ptr += sf_length_8kHz;
}
  CCmax = 0.0f; CCmax_b = -1000.0f; CBimax = 0; lag = -1;
  if (prevLag > 0) {
    if (Fs_kHz == 12) {
      prevLag = ((opus_int32)((opus_uint32)(prevLag) << (1))) / 3;
    } else if (Fs_kHz == 16) { prevLag = ((prevLag) >> (1));
}
    prevLag_log2 = silk_log2((float)prevLag);
  } else { prevLag_log2 = 0;
}
  const auto stage2_codebook = silk_stage2_pitch_codebook_view(Fs_kHz, nb_subfr, complexity);
  for (k = 0; k < length_d_srch; k++) {
    d = d_srch[k];
    for (j = 0; j < stage2_codebook.nb_cbk_search; j++) { CC[j] = 0.0f; for (i=0;i<nb_subfr;i++) { CC[j] += C[i][d+stage2_codebook.at(i,j)]; }
}
    CCmax_new = -1000.0f; CBimax_new = 0;
    for (i = 0; i < stage2_codebook.nb_cbk_search; i++) {
      if (CC[i] > CCmax_new) { CCmax_new = CC[i]; CBimax_new = i; }
}
    lag_log2 = silk_log2((float)d); CCmax_new_b = CCmax_new - 0.2f * nb_subfr * lag_log2;
    if (prevLag > 0) {
      delta_lag_log2_sqr = lag_log2 - prevLag_log2; delta_lag_log2_sqr *= delta_lag_log2_sqr;
      CCmax_new_b -= 0.2f * nb_subfr * (*LTPCorr) * delta_lag_log2_sqr / (delta_lag_log2_sqr + 0.5f);
}
    if (CCmax_new_b > CCmax_b && CCmax_new > nb_subfr * search_thres2) { CCmax_b = CCmax_new_b; CCmax = CCmax_new; lag = d; CBimax = CBimax_new; }
}
  if (lag == -1) {
    zero_n_bytes(pitch_out, static_cast<std::size_t>(4 * sizeof(int))); *LTPCorr = 0.0f; *lagIndex = 0; *contourIndex = 0;
    return 1;
}
  *LTPCorr = (float)(CCmax / nb_subfr); opus_assume(*LTPCorr >= 0.0f);
  if (Fs_kHz > 8) {
    opus_assume(lag == saturate_int16_from_int32(lag));
    if (Fs_kHz == 12) {
      lag = rounded_i16_product_shift<1>(lag, 3);
    } else { lag = ((opus_int32)((opus_uint32)(lag) << (1)));
}
    lag = ((min_lag) > (max_lag) ? ((lag) > (min_lag) ? (min_lag) : ((lag) < (max_lag) ? (max_lag) : (lag))) : ((lag) > (max_lag) ? (max_lag) : ((lag) < (min_lag) ? (min_lag) : (lag))));
    start_lag = std::max(lag - 2, min_lag); end_lag = std::min(lag + 2, max_lag); lag_new = lag; CBimax = 0;
    CCmax = -1000.0f; silk_P_Ana_calc_corr_st3(cross_corr_st3, frame, start_lag, sf_length, nb_subfr, complexity);
    silk_P_Ana_calc_energy_st3(energies_st3, frame, start_lag, sf_length, nb_subfr, complexity); lag_counter = 0;
    opus_assume(lag == saturate_int16_from_int32(lag)); contour_bias = 0.05f / lag;
    const auto stage3_codebook = silk_stage3_pitch_codebook_view(nb_subfr, complexity);
    target_ptr = &frame[(4 * 5) * Fs_kHz]; energy_tmp = silk_energy_FLP(target_ptr, nb_subfr * sf_length) + 1.0;
    for (d = start_lag; d <= end_lag; d++) {
      for (j = 0; j < stage3_codebook.nb_cbk_search; j++) {
        cross_corr = 0.0; energy = energy_tmp;
        for (k = 0; k < nb_subfr; k++) { cross_corr += cross_corr_st3[k][j][lag_counter]; energy += energies_st3[k][j][lag_counter];
}
        if (cross_corr > 0.0) {
          CCmax_new = (float)(2 * cross_corr / energy); CCmax_new *= 1.0f - contour_bias * j;
        } else { CCmax_new = 0.0f;
}
        if (CCmax_new > CCmax && (d + static_cast<int>(stage3_codebook.at(0, j))) <= max_lag) { CCmax = CCmax_new; lag_new = d; CBimax = j; }
}
      lag_counter++;
}
    for (k = 0; k < nb_subfr; k++) { pitch_out[k] = lag_new+stage3_codebook.at(k,CBimax); pitch_out[k] = ((min_lag)>(18*Fs_kHz)?((pitch_out[k])>(min_lag)?(min_lag):((pitch_out[k])<(18*Fs_kHz)?(18*Fs_kHz):(pitch_out[k]))):((pitch_out[k])>(18*Fs_kHz)?(18*Fs_kHz):((pitch_out[k])<(min_lag)?(min_lag):(pitch_out[k]))));
}
    *lagIndex = (opus_int16)(lag_new - min_lag); *contourIndex = (opus_int8)CBimax;
  } else {
    for (k = 0; k < nb_subfr; k++) { pitch_out[k] = lag+stage2_codebook.at(k,CBimax); pitch_out[k] = ((min_lag_8kHz)>(18*8)?((pitch_out[k])>(min_lag_8kHz)?(min_lag_8kHz):((pitch_out[k])<(18*8)?(18*8):(pitch_out[k]))):((pitch_out[k])>(18*8)?(18*8):((pitch_out[k])<(min_lag_8kHz)?(min_lag_8kHz):(pitch_out[k]))));
}
    *lagIndex = (opus_int16)(lag - min_lag_8kHz); *contourIndex = (opus_int8)CBimax;
}
  opus_assume(*lagIndex >= 0);
  return 0;
}
static void silk_P_Ana_calc_corr_st3(float cross_corr_st3[4][34][5], const float frame[], int start_lag, int sf_length, int nb_subfr, int complexity) {
  opus_assume(complexity >= 0); opus_assume(complexity <= 2);
  const auto lag_ranges = silk_stage3_lag_range_view(nb_subfr, complexity);
  const auto codebook = silk_stage3_pitch_codebook_view(nb_subfr, complexity);
  const float *target_ptr = &frame[((opus_int32)((opus_uint32)(sf_length) << (2)))];
  for (int k = 0; k < nb_subfr; k++) {
    const int lag_low = lag_ranges.low(k);
    const int lag_high = lag_ranges.high(k);
    const int lag_count = lag_high - lag_low + 1;
    opus_assume(lag_count <= 22);
    float scratch_mem[22];
    opus_val32 xcorr[22];
    celt_pitch_xcorr_c(std::span<const opus_val16>{target_ptr, static_cast<std::size_t>(sf_length)}, std::span<const opus_val16>{target_ptr - start_lag - lag_high, static_cast<std::size_t>(sf_length + lag_high - lag_low)}, std::span<opus_val32>{xcorr, static_cast<std::size_t>(lag_count)});
    for (int j = 0; j < lag_count; j++) scratch_mem[j] = xcorr[lag_high - lag_low - j];
    const int delta = lag_low;
    for (int i = 0; i < codebook.nb_cbk_search; i++) {
      const int idx = codebook.at(k, i) - delta;
      copy_n_items(scratch_mem + idx, std::size(cross_corr_st3[k][i]), cross_corr_st3[k][i]);
}
    target_ptr += sf_length; }
}
static void silk_P_Ana_calc_energy_st3(float energies_st3[4][34][5], const float frame[], int start_lag, int sf_length, int nb_subfr, int complexity) {
  opus_assume(complexity >= 0); opus_assume(complexity <= 2);
  const auto lag_ranges = silk_stage3_lag_range_view(nb_subfr, complexity);
  const auto codebook = silk_stage3_pitch_codebook_view(nb_subfr, complexity);
  const float *target_ptr = &frame[((opus_int32)((opus_uint32)(sf_length) << (2)))];
  for (int k = 0; k < nb_subfr; k++) {
    float scratch_mem[22];
    const float *basis_ptr = target_ptr - (start_lag + lag_ranges.low(k));
    double energy = silk_energy_FLP(basis_ptr, sf_length) + 1e-3;
    opus_assume(energy >= 0.0); scratch_mem[0] = (float)energy;
    const int lag_diff = lag_ranges.high(k) - lag_ranges.low(k) + 1;
    for (int i = 1; i < lag_diff; i++) { energy -= basis_ptr[sf_length-i]*(double)basis_ptr[sf_length-i]; opus_assume(energy>=0.0); energy += basis_ptr[-i]*(double)basis_ptr[-i]; opus_assume(energy>=0.0); opus_assume(i<22); scratch_mem[i] = (float)energy;
}
    const int delta = lag_ranges.low(k);
    for (int i = 0; i < codebook.nb_cbk_search; i++) {
      const int idx = codebook.at(k, i) - delta;
      copy_n_items(scratch_mem + idx, std::size(energies_st3[k][i]), energies_st3[k][i]);
}
    target_ptr += sf_length; }
}
void silk_scale_copy_vector_FLP(float *data_out, const float *data_in, float gain, int dataSize) {
  const auto count = static_cast<std::size_t>(dataSize > 0 ? dataSize : 0);
  for (auto index = std::size_t{}; index < count; ++index) data_out[index] = data_in[index] * gain;
}
void silk_scale_vector_FLP(float *data1, float gain, int dataSize) {
  const auto count = static_cast<std::size_t>(dataSize > 0 ? dataSize : 0);
  for (auto index = std::size_t{}; index < count; ++index) data1[index] *= gain;
}
float silk_schur_FLP(float refl_coef[], const float auto_corr[], int order) {
  opus_assume(order >= 0 && order <= 24);
  double C[24 + 1][2];
  for (int i = 0; i <= order; ++i) C[i][0] = C[i][1] = auto_corr[i];
  for (int k = 0; k < order; k++) {
    const double rc_tmp = -C[k + 1][0] / (((C[0][1]) > (1e-9f)) ? (C[0][1]) : (1e-9f));
    refl_coef[k] = (float)rc_tmp;
    for (int n = 0; n < order-k; n++) { const double c0 = C[n+k+1][0], c1 = C[n][1]; C[n+k+1][0] = c0+c1*rc_tmp; C[n][1] = c1+c0*rc_tmp; }
}
  return (float)C[0][1];
}
void silk_insertion_sort_decreasing_FLP(float *a, int *idx, const int L, const int K) {
  opus_assume(K > 0); opus_assume(L > 0);
  opus_assume(L >= K);
  for (int i = 0; i < K; ++i) idx[i] = i;
  for (int i = 1; i < K; i++) {
    const float value = a[i];
    int j = i - 1;
    for (; (j >= 0) && (value > a[j]); j--) { a[j + 1] = a[j]; idx[j + 1] = idx[j];
}
    a[j + 1] = value; idx[j + 1] = i;
}
  for (int i = K; i < L; i++) {
    const float value = a[i];
    if (value > a[K - 1]) {
      int j = K - 2;
      for (; (j >= 0) && (value > a[j]); j--) { a[j + 1] = a[j]; idx[j + 1] = idx[j];
}
      a[j + 1] = value; idx[j + 1] = i; } }
}
namespace {
void assign_error(int *error, int value) noexcept {
  if (error != nullptr) { *error = value; }
}
[[nodiscard]] constexpr bool has_required_storage(const void *data, int required_count) noexcept { return required_count <= 0 || data != nullptr;
}
[[nodiscard]] auto encoder_allocation_size(int channels) noexcept -> std::size_t {
  const int state_size = ref_opus_encoder_get_size(channels);
  return state_size > 0 ? static_cast<std::size_t>(state_size) : 0U;
}
[[nodiscard]] auto decoder_allocation_size(int channels) noexcept -> std::size_t {
  const int state_size = ref_opus_decoder_get_size(channels);
  return state_size > 0 ? static_cast<std::size_t>(state_size) : 0U;
}
[[nodiscard]] OPUS_COLD OPUS_NOINLINE auto create_encoder_state(int Fs, int channels, int application, int *error) noexcept -> OpusEncoder * {
  const std::size_t allocation_size = encoder_allocation_size(channels);
  if (allocation_size == 0U) {
    assign_error(error, OPUS_BAD_ARG);
    return nullptr;
}
  auto state = make_opus_owned<OpusEncoder>(allocation_size);
  if (state == nullptr) {
    assign_error(error, OPUS_ALLOC_FAIL);
    return nullptr;
}
  const int ret = ref_opus_encoder_init(reinterpret_cast<ref_OpusEncoder *>(state.get()), Fs, channels, application);
  if (ret != OPUS_OK) {
    assign_error(error, ret);
    return nullptr;
}
  assign_error(error, OPUS_OK);
  return state.release();
}
[[nodiscard]] OPUS_COLD OPUS_NOINLINE auto create_decoder_state(int Fs, int channels, int *error) noexcept -> OpusDecoder * {
  const std::size_t allocation_size = decoder_allocation_size(channels);
  if (allocation_size == 0U) {
    assign_error(error, OPUS_BAD_ARG);
    return nullptr;
}
  auto state = make_opus_owned<OpusDecoder>(allocation_size);
  if (state == nullptr) {
    assign_error(error, OPUS_ALLOC_FAIL);
    return nullptr;
}
  const int ret = ref_opus_decoder_init(reinterpret_cast<ref_OpusDecoder *>(state.get()), Fs, channels);
  if (ret != OPUS_OK) {
    assign_error(error, ret);
    return nullptr;
}
  assign_error(error, OPUS_OK);
  return state.release();
}
[[nodiscard]] OPUS_COLD OPUS_NOINLINE auto dispatch_encoder_control(ref_OpusEncoder *st, int request, va_list &ap) noexcept -> int {
  CeltEncoderInternal *celt_enc = encoder_uses_celt(st->application) ? reinterpret_cast<CeltEncoderInternal *>(reinterpret_cast<char *>(st) + st->celt_enc_offset) : nullptr;
  switch (request) {
  case OPUS_SET_BITRATE_REQUEST: {
    const auto value = __builtin_va_arg(ap, opus_int32);
    return try_set_user_bitrate(st, value) ? OPUS_OK : OPUS_BAD_ARG;
}
  case OPUS_GET_BITRATE_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_int32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = user_bitrate_to_bitrate(st, st->prev_framesize, 1276);
    return OPUS_OK;
}
  case OPUS_SET_VBR_REQUEST: {
    const auto value = __builtin_va_arg(ap, opus_int32);
    if (value < 0 || value > 1) { return OPUS_BAD_ARG;
}
    st->use_vbr = value;
    st->silk_mode.useCBR = 1 - value;
    return OPUS_OK;
}
  case OPUS_GET_VBR_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_int32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = st->use_vbr;
    return OPUS_OK;
}
  case OPUS_SET_VBR_CONSTRAINT_REQUEST: {
    const auto value = __builtin_va_arg(ap, opus_int32);
    if (value < 0 || value > 1) { return OPUS_BAD_ARG;
}
    st->vbr_constraint = value;
    return OPUS_OK;
}
  case OPUS_GET_VBR_CONSTRAINT_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_int32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = st->vbr_constraint;
    return OPUS_OK;
}
  case OPUS_SET_COMPLEXITY_REQUEST: {
    const auto value = __builtin_va_arg(ap, opus_int32);
    if (value < 0 || value > 10) { return OPUS_BAD_ARG;
}
    st->silk_mode.complexity = value;
    if (celt_enc != nullptr) { celt_encoder_set_complexity(celt_enc, static_cast<opus_int32>(value));
}
    return OPUS_OK;
}
  case OPUS_GET_COMPLEXITY_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_int32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = st->silk_mode.complexity;
    return OPUS_OK;
}
  case OPUS_GET_FINAL_RANGE_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_uint32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = st->rangeFinal;
    return OPUS_OK;
}
  case OPUS_RESET_STATE: reset_ref_encoder_state(st, celt_enc); return OPUS_OK;
  default: return OPUS_UNIMPLEMENTED; }
}
[[nodiscard]] OPUS_COLD OPUS_NOINLINE auto dispatch_decoder_control(ref_OpusDecoder *st, int request, va_list &ap) noexcept -> int {
  switch (request) {
  case OPUS_GET_LAST_PACKET_DURATION_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_int32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = st->last_packet_duration;
    return OPUS_OK;
}
  case OPUS_GET_FINAL_RANGE_REQUEST: {
    auto *value = __builtin_va_arg(ap, opus_uint32 *);
    if (value == nullptr) { return OPUS_BAD_ARG;
}
    *value = st->rangeFinal;
    return OPUS_OK;
}
  case OPUS_RESET_STATE: {
    void *silk_dec = reinterpret_cast<char *>(st) + st->silk_dec_offset;
    auto *celt_dec = reinterpret_cast<CeltDecoderInternal *>(reinterpret_cast<char *>(st) + st->celt_dec_offset);
    reset_ref_decoder_state(st, silk_dec, celt_dec);
    return OPUS_OK;
}
  default: return OPUS_UNIMPLEMENTED; }
}
[[nodiscard]] auto encode_int16_frame(OpusEncoder *st, const int16_t *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept -> int {
  if (st == nullptr || !has_required_storage(pcm, frame_size) || !has_required_storage(data, max_data_bytes)) { return OPUS_BAD_ARG;
}
  return static_cast<int>(ref_opus_encode(reinterpret_cast<ref_OpusEncoder *>(st), pcm, frame_size, data, max_data_bytes));
}
[[nodiscard]] auto encode_float_frame(OpusEncoder *st, const float *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept -> int {
  if (st == nullptr || !has_required_storage(pcm, frame_size) || !has_required_storage(data, max_data_bytes)) { return OPUS_BAD_ARG;
}
  return static_cast<int>(ref_opus_encode_float(reinterpret_cast<ref_OpusEncoder *>(st), pcm, frame_size, data, max_data_bytes));
}
[[nodiscard]] auto decode_int16_frame(OpusDecoder *st, const unsigned char *data, int len, int16_t *pcm, int frame_size, int decode_fec) noexcept -> int {
  if (st == nullptr || !has_required_storage(data, len) || !has_required_storage(pcm, frame_size)) { return OPUS_BAD_ARG;
}
  return ref_opus_decode(reinterpret_cast<ref_OpusDecoder *>(st), data, len, pcm, frame_size, decode_fec);
}
[[nodiscard]] auto decode_float_frame(OpusDecoder *st, const unsigned char *data, int len, float *pcm, int frame_size, int decode_fec) noexcept -> int {
  if (st == nullptr || !has_required_storage(data, len) || !has_required_storage(pcm, frame_size)) { return OPUS_BAD_ARG;
}
  return ref_opus_decode_float(reinterpret_cast<ref_OpusDecoder *>(st), data, len, pcm, frame_size, decode_fec);
}
} // namespace
OpusEncoder *opus_encoder_create(int Fs, int channels, int application, int *error) noexcept { return create_encoder_state(Fs, channels, application, error);
}
void opus_encoder_destroy(OpusEncoder *st) noexcept { opus_free(st);
}
int opus_encoder_ctl(OpusEncoder *st, int request, ...) noexcept {
  va_list ap; __builtin_va_start(ap, request);
  const int ret = st == nullptr ? OPUS_BAD_ARG : dispatch_encoder_control(reinterpret_cast<ref_OpusEncoder *>(st), request, ap);
  __builtin_va_end(ap);
  return ret;
}
int opus_encode(OpusEncoder *st, const int16_t *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept { return encode_int16_frame(st, pcm, frame_size, data, max_data_bytes);
}
int opus_encode_float(OpusEncoder *st, const float *pcm, int frame_size, unsigned char *data, int max_data_bytes) noexcept { return encode_float_frame(st, pcm, frame_size, data, max_data_bytes);
}
OpusDecoder *opus_decoder_create(int Fs, int channels, int *error) noexcept { return create_decoder_state(Fs, channels, error);
}
void opus_decoder_destroy(OpusDecoder *st) noexcept {
  destroy_ref_decoder_state(reinterpret_cast<ref_OpusDecoder *>(st));
  opus_free(st);
}
int opus_decoder_ctl(OpusDecoder *st, int request, ...) noexcept {
  va_list ap; __builtin_va_start(ap, request);
  const int ret = st == nullptr ? OPUS_BAD_ARG : dispatch_decoder_control(reinterpret_cast<ref_OpusDecoder *>(st), request, ap);
  __builtin_va_end(ap);
  return ret;
}
int opus_decode(OpusDecoder *st, const unsigned char *data, int len, int16_t *pcm, int frame_size, int decode_fec) noexcept { return decode_int16_frame(st, data, len, pcm, frame_size, decode_fec);
}
int opus_decode_float(OpusDecoder *st, const unsigned char *data, int len, float *pcm, int frame_size, int decode_fec) noexcept { return decode_float_frame(st, data, len, pcm, frame_size, decode_fec);
}
int opus_packet_get_nb_samples(const unsigned char *data, int len, int Fs) noexcept {
  if (!has_required_storage(data, len)) { return OPUS_BAD_ARG;
}
  return ref_opus_packet_get_nb_samples(data, len, Fs);
}
[[nodiscard]] auto opus_strerror(int error) noexcept -> const char * { return ref_opus_strerror(error);
}
