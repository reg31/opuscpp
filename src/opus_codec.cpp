#include "opus_codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

using opus_int8 = std::int8_t;
using opus_uint8 = std::uint8_t;
using opus_uint16 = std::uint16_t;
using opus_int64 = std::int64_t;

constexpr auto opus_int32_min = std::numeric_limits<opus_int32>::min();
constexpr auto opus_int32_max = std::numeric_limits<opus_int32>::max();

constexpr auto ec_code_top = static_cast<opus_uint32>(1) << 31;
constexpr auto ec_code_bot = ec_code_top >> 8;
constexpr auto ec_code_mask = ec_code_top - 1;
constexpr auto ec_byte_mask = (1U << 8U) - 1U;

constexpr auto q7_shift = 7;
constexpr auto q7_scale = 1 << q7_shift;

constexpr opus_int32 silk_log_60_q7 = 756;
constexpr opus_int32 silk_log_100_q7 = 851;
constexpr opus_int32 silk_log_60_q15 = silk_log_60_q7 << 8;
constexpr opus_int32 silk_log_100_q15 = silk_log_100_q7 << 8;

constexpr auto silk_mid_only_score_bias = 0.02f;
constexpr auto silk_mid_only_low_speech_bias = 0.01f;

constexpr int opus_mode_silk_only = 1000;
constexpr int opus_mode_hybrid = 1001;
constexpr int opus_mode_celt_only = 1002;

constexpr int opus_max_frame_samples_48k = 5760;
constexpr int opus_max_pcm_samples = opus_max_frame_samples_48k * 2;
constexpr int opus_max_multiframe_packet_bytes = 1276 * 6;

constexpr int celt_default_overlap = 120;
constexpr int celt_default_nb_ebands = 21;
constexpr int celt_stereo_analysis_bands = 13;
constexpr int celt_max_channels = 2;
constexpr int celt_sample_rate = 48000;
constexpr int celt_short_mdct_size = 120;
constexpr int celt_max_lm = 3;
constexpr int celt_allocation_vector_count = 11;
constexpr std::array<float, 4> celt_preemphasis{0.85000610f, 0.0f, 1.0f, 1.0f};
constexpr int celt_max_frame_samples = 960;
constexpr int celt_max_pitch_period = 1024;
constexpr int celt_min_pitch_period = 15;
constexpr int celt_max_band_samples = 22 * 8;
constexpr int celt_max_norm_samples = celt_max_channels * 78 * 8;
constexpr int encoder_max_stage_samples = opus_max_frame_samples_48k;

constexpr int silk_nlsf_max_cb1_vectors = 32;
constexpr int silk_nlsf_max_survivors = 16;
constexpr int silk_nlsf_max_order = 16;
constexpr int silk_vad_max_work_samples = 400;

[[nodiscard]] constexpr auto is_supported_sample_rate(const opus_int32 Fs) noexcept -> bool {
  return Fs == 48000 || Fs == 24000 || Fs == 16000 || Fs == 12000 || Fs == 8000;
}

[[nodiscard]] constexpr auto is_supported_channel_count(const int channels) noexcept -> bool {
  return channels == 1 || channels == 2;
}

[[nodiscard]] constexpr auto has_required_storage(const void* data, int required_count) noexcept -> bool {
  return required_count <= 0 || data != nullptr;
}

[[nodiscard]] constexpr auto interleaved_frame_size(std::size_t samples, int channels) noexcept -> int {
  if (channels == 2) {
    if ((samples & 1U) != 0) {
      return -1;
    }
    samples >>= 1U;
  }
  return samples <= static_cast<std::size_t>(std::numeric_limits<int>::max()) ? static_cast<int>(samples) : -1;
}

[[nodiscard]] constexpr auto low_bits_mask(const unsigned bits) noexcept -> opus_uint32 {
  return bits == 0 ? 0U : (static_cast<opus_uint32>(1) << bits) - 1U;
}

template <std::totally_ordered T> [[nodiscard]] constexpr auto clamp_value(T value, T low, T high) noexcept -> T {
  return value < low ? low : (high < value ? high : value);
}

[[nodiscard]] constexpr auto score_below(const float value, const float soft) noexcept -> float {
  return soft <= 0.0f ? 0.0f : clamp_value((soft - value) / soft, 0.0f, 1.0f);
}

[[nodiscard]] constexpr auto parabolic_q7_term(const opus_int32 frac_q7, const opus_int16 coefficient) noexcept -> opus_int32 {
  return frac_q7 + static_cast<opus_int32>((static_cast<opus_int64>(frac_q7 * (q7_scale - frac_q7)) * coefficient) >> 16);
}

[[nodiscard]] constexpr auto wrap_shift_left(opus_int32 value, unsigned shift) noexcept -> opus_int32 {
  return std::bit_cast<opus_int32>(std::bit_cast<opus_uint32>(value) << shift);
}

[[nodiscard]] constexpr auto wrap_add(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return std::bit_cast<opus_int32>(std::bit_cast<opus_uint32>(lhs) + std::bit_cast<opus_uint32>(rhs));
}

[[nodiscard]] constexpr auto wrap_subtract(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return std::bit_cast<opus_int32>(std::bit_cast<opus_uint32>(lhs) - std::bit_cast<opus_uint32>(rhs));
}

[[nodiscard]] constexpr auto silk_mul_wb(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return static_cast<opus_int32>((static_cast<opus_int64>(lhs) * static_cast<opus_int16>(rhs)) >> 16);
}

[[nodiscard]] constexpr auto silk_mla_wb(opus_int32 accumulator, opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return wrap_add(accumulator, silk_mul_wb(lhs, rhs));
}

[[nodiscard]] constexpr auto silk_mul_high(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return static_cast<opus_int32>((static_cast<opus_int64>(lhs) * rhs) >> 32);
}

[[nodiscard]] constexpr auto multiply_q16(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return static_cast<opus_int32>((static_cast<opus_int64>(lhs) * rhs) >> 16);
}

template <int Shift>
  requires(Shift >= 0)
[[nodiscard]] constexpr auto silk_mul_i16_shift(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return static_cast<opus_int32>(static_cast<opus_int16>(lhs)) * static_cast<opus_int16>(rhs) >> Shift;
}

template <int FractionBits, std::floating_point T> [[nodiscard]] consteval auto fixed_q(T value) noexcept -> opus_int32 {
  return static_cast<opus_int32>(value * static_cast<T>(opus_int64{1} << FractionBits) + static_cast<T>(0.5));
}

[[nodiscard]] constexpr auto bandwidth_to_endband(const int bandwidth) noexcept -> int {
  constexpr std::array end_bands{13, 17, 17, 19, 21};
  return bandwidth >= 1101 && bandwidth <= 1105 ? end_bands[static_cast<std::size_t>(bandwidth - 1101)] : 21;
}
template <int Shift, std::signed_integral Integer>
  requires(Shift > 0)
[[nodiscard]] static auto rounded_rshift(Integer value) noexcept -> Integer {
  if constexpr (Shift == 1) {
    return (value >> 1) + (value & 1);
  }
  return ((value >> (Shift - 1)) + 1) >> 1;
}

template <std::signed_integral Integer> [[nodiscard]] static constexpr auto rounded_rshift(Integer value, int shift) noexcept -> Integer {
  return shift == 1 ? (value >> 1) + (value & 1) : ((value >> (shift - 1)) + 1) >> 1;
}

[[nodiscard]] static auto saturate_int16_from_int32(opus_int32 value) noexcept -> opus_int16 {
  return static_cast<opus_int16>(clamp_value(value, static_cast<opus_int32>(-32768), static_cast<opus_int32>(32767)));
}

[[nodiscard]] static constexpr auto saturate_int32(opus_int64 value) noexcept -> opus_int32 {
  return static_cast<opus_int32>(clamp_value(value, static_cast<opus_int64>(opus_int32_min), static_cast<opus_int64>(opus_int32_max)));
}

template <int Shift>
  requires(Shift > 0)
[[nodiscard]] static auto scale_and_saturate_q14(opus_int32 sample_q14, opus_int32 gain) noexcept -> opus_int16 {
  return saturate_int16_from_int32(rounded_rshift<Shift>(multiply_q16(sample_q14, gain)));
}

template <int Shift>
  requires(Shift > 0)
[[nodiscard]] static auto saturating_left_shift(opus_int32 value) noexcept -> opus_int32 {
  return saturate_int32(static_cast<opus_int64>(value) * (opus_int64{1} << Shift));
}

[[nodiscard]] static auto saturating_left_shift(opus_int32 value, int shift) noexcept -> opus_int32 {
  return shift <= 0 ? value : saturate_int32(static_cast<opus_int64>(value) * (opus_int64{1} << shift));
}

[[nodiscard]] static auto saturating_add_int32(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return saturate_int32(static_cast<opus_int64>(lhs) + rhs);
}

[[nodiscard]] static auto delayed_pulse_from_q10(opus_int32 value_q10) noexcept -> opus_int8 {
  return static_cast<opus_int8>(rounded_rshift<10>(value_q10));
}

[[nodiscard]] static auto silk_next_rand_seed(const opus_int32 seed) noexcept -> opus_int32 {
  return std::bit_cast<opus_int32>(opus_uint32{907633515} + static_cast<opus_uint32>(seed) * opus_uint32{196314165});
}

template <typename T> static void zero_n_items(T* destination, const std::size_t count) noexcept {
  std::memset(destination, 0, count * sizeof(T));
}

template <typename T> static void copy_n_items(const T* source, const std::size_t count, T* destination) noexcept {
  if (count == 0 || source == destination) {
    return;
  }
  std::memcpy(destination, source, count * sizeof(T));
}

[[nodiscard]] static constexpr auto silk_pitch_contour_icdf(int fs_kHz, int nb_subfr) noexcept -> std::span<const opus_uint8>;
[[nodiscard]] static constexpr auto silk_pitch_lag_low_bits_icdf(int fs_kHz) noexcept -> std::span<const opus_uint8>;

template <typename T> static void move_n_items(const T* source, const std::size_t count, T* destination) noexcept {
  std::memmove(destination, source, count * sizeof(T));
}

static void zero_n_bytes(void* destination, const std::size_t count) noexcept {
  std::memset(destination, 0, count);
}

static void copy_n_bytes(const void* source, const std::size_t count, void* destination) noexcept {
  std::memcpy(destination, source, count);
}

static void move_n_bytes(const void* source, const std::size_t count, void* destination) noexcept {
  std::memmove(destination, source, count);
}

template <typename T> static void zero_object(T& value) noexcept {
  zero_n_bytes(&value, sizeof(value));
}

template <typename T>
  requires std::is_trivially_copyable_v<T>
static void zero_object_tail(T& value, std::size_t offset) noexcept {
  zero_n_bytes(reinterpret_cast<std::byte*>(&value) + offset, sizeof(value) - offset);
}

template <typename T> [[nodiscard]] static auto offset_ptr(void* base, int offset) noexcept -> T* {
  return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(base) + offset);
}

template <typename T> [[nodiscard]] static auto offset_ptr(const void* base, int offset) noexcept -> const T* {
  return reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(base) + offset);
}

template <int Order>
  requires(Order == 10 || Order == 16)
[[nodiscard]] static auto silk_lpc_prediction_q10_fixed(const opus_int32* history_end, const opus_int16* coefficients) noexcept
    -> opus_int32 {
  auto tap = [&](const int index) noexcept {
    return silk_mul_wb(history_end[-index - 1], coefficients[index]);
  };
  if constexpr (Order == 16) {
    opus_int32 sum0 = tap(0) + tap(1) + tap(2) + tap(3);
    opus_int32 sum1 = tap(4) + tap(5) + tap(6) + tap(7);
    opus_int32 sum2 = tap(8) + tap(9) + tap(10) + tap(11);
    const opus_int32 sum3 = tap(12) + tap(13) + tap(14) + tap(15);
    return static_cast<opus_int32>(8 + (sum0 + sum1) + (sum2 + sum3));
  }
  auto prediction_q10 = static_cast<opus_int32>(Order >> 1);
  for (int tap_index = 0; tap_index < Order; ++tap_index) {
    prediction_q10 = silk_mla_wb(prediction_q10, history_end[-tap_index - 1], coefficients[tap_index]);
  }
  return prediction_q10;
}

template <int Order>
  requires(Order == 10 || Order == 16)
static inline void silk_decode_lpc_subframe_q14(opus_int32* sLPC_Q14, const opus_int32* pres_Q14, opus_int16* pxq, const int length,
                                                const opus_int16* A_Q12, const opus_int32 gain_Q10) noexcept {
  for (int i = 0; i < length; ++i) {
    const auto pred_Q10 = silk_lpc_prediction_q10_fixed<Order>(sLPC_Q14 + 16 + i, A_Q12);
    sLPC_Q14[16 + i] = saturating_add_int32(pres_Q14[i], saturating_left_shift<4>(pred_Q10));
    pxq[i] = scale_and_saturate_q14<8>(sLPC_Q14[16 + i], gain_Q10);
  }
}

[[nodiscard]] static auto silk_lpc_prediction_q10(const opus_int32* history_end, const opus_int16* coefficients, const int order) noexcept
    -> opus_int32 {
  return order == 10 ? silk_lpc_prediction_q10_fixed<10>(history_end, coefficients)
                     : silk_lpc_prediction_q10_fixed<16>(history_end, coefficients);
}

[[nodiscard]] static auto silk_ltp_prediction_5tap(const opus_int32* pred_lag_ptr, const opus_int16* coefficients) noexcept -> opus_int32 {
  auto prediction = opus_int32{2};
  for (int tap = 0; tap < 5; ++tap) {
    prediction = silk_mla_wb(prediction, pred_lag_ptr[-tap], coefficients[static_cast<std::size_t>(tap)]);
  }
  return prediction;
}

struct OpusEncoder;
struct CeltEncoderInternal;
struct CeltDecoderInternal;
struct CeltModeInternal;
[[nodiscard]] static constexpr auto celt_mode() noexcept -> const CeltModeInternal*;
using opus_val16 = float;
using opus_val32 = float;
using opus_val64 = float;
using celt_sig = float;
using celt_norm = float;
using celt_ener = float;
using celt_glog = float;
using opus_res = float;
using celt_coef = float;
using ec_window = opus_uint32;
struct ec_ctx {
  unsigned char* buf;
  opus_uint32 storage, end_offs, offs, rng, val, ext;
  ec_window end_window;
  int nend_bits, nbits_total, rem, error;
};
using ec_enc = ec_ctx;
using ec_dec = ec_ctx;

[[nodiscard]] inline auto ec_tell(const ec_ctx* state) noexcept -> int {
  return state->nbits_total - std::bit_width(state->rng);
}

static opus_uint32 ec_tell_frac(const ec_ctx* _this);
[[nodiscard]] constexpr auto celt_udiv(opus_uint32 numerator, opus_uint32 denominator) noexcept -> opus_uint32 {
  return numerator / denominator;
}

[[nodiscard]] constexpr auto celt_sudiv(opus_int32 numerator, opus_int32 denominator) noexcept -> opus_int32 {
  return numerator / denominator;
}

static void ec_enc_init(ec_enc* _this, unsigned char* _buf, opus_uint32 _size);
static void ec_encode(ec_enc* _this, unsigned _fl, unsigned _fh, unsigned _ft);
static void ec_enc_bit_logp(ec_enc* _this, int _val, unsigned _logp);
static void ec_enc_icdf(ec_enc* _this, int _s, const unsigned char* _icdf, unsigned _ftb);
static void ec_enc_uint(ec_enc* _this, opus_uint32 _fl, opus_uint32 _ft);
static void ec_enc_bits(ec_enc* _this, opus_uint32 _fl, unsigned _ftb);
static void ec_enc_shrink(ec_enc* _this, opus_uint32 _size);
static void ec_enc_done(ec_enc* _this);
static void ec_dec_init(ec_dec* _this, unsigned char* _buf, opus_uint32 _storage);
static void ec_dec_update(ec_dec* _this, unsigned _fl, unsigned _fh, unsigned _ft);
static unsigned ec_decode(ec_dec* _this, unsigned _ft);
static int ec_dec_bit_logp(ec_dec* _this, unsigned _logp);
static int ec_dec_icdf(ec_dec* _this, const unsigned char* _icdf, unsigned _ftb);
static opus_uint32 ec_dec_uint(ec_dec* _this, opus_uint32 _ft);
static opus_uint32 ec_dec_bits(ec_dec* _this, unsigned _ftb);
struct kiss_fft_cpx {
  float r;
  float i;
};
struct kiss_twiddle_cpx {
  float r;
  float i;
};

[[nodiscard]] constexpr auto complex_add(kiss_fft_cpx lhs, kiss_fft_cpx rhs) noexcept -> kiss_fft_cpx {
  return {lhs.r + rhs.r, lhs.i + rhs.i};
}

[[nodiscard]] constexpr auto complex_subtract(kiss_fft_cpx lhs, kiss_fft_cpx rhs) noexcept -> kiss_fft_cpx {
  return {lhs.r - rhs.r, lhs.i - rhs.i};
}

[[nodiscard]] constexpr auto complex_multiply(kiss_fft_cpx lhs, kiss_twiddle_cpx rhs) noexcept -> kiss_fft_cpx {
  return {lhs.r * rhs.r - lhs.i * rhs.i, lhs.r * rhs.i + lhs.i * rhs.r};
}

struct kiss_fft_state {
  int nfft;
  celt_coef scale;
  const kiss_twiddle_cpx* twiddles;
  const opus_int16* bitrev;
};
struct SILKInfo {
  int offset, bitrateBps, actualSilkBps;
};

struct CeltEncoderInternal {
  int channels, stream_channels, complexity, upsample, start, end;
  opus_int32 bitrate, midrate_quality_boost_bps;
  int vbr, constrained_vbr, lsb_depth;
  bool prediction_disabled, audio_application;
  opus_uint32 rng;
  opus_uint16 prefilter_period;
  opus_uint8 high_z_tonal_Q7, input_diff_Q10, consec_transient, lastCodedBands;
  bool lowrate_refinement, content_vbr;
  opus_val32 delayedIntra, prefilter_gain;
  SILKInfo silk_info;
  opus_val32 preemph_memE[2];
  opus_int32 vbr_reservoir, vbr_drift, vbr_offset, vbr_count;
  opus_val32 overlap_max;
  opus_val16 stereo_saving;
  int intensity;
  celt_glog spec_avg;
};

struct alignas(8) CeltDecoderInternal {
  int channels, stream_channels, downsample, start, end, output_postfilter_level;
  int last_pitch_index, loss_duration, last_frame_type, skip_plc, postfilter_period, postfilter_period_old, postfilter_tapset,
      postfilter_tapset_old;
  opus_uint32 rng;
  int output_postfilter_auto_hold, output_postfilter_average_bitrate;
  opus_val16 postfilter_gain, postfilter_gain_old;
  opus_val16 output_postfilter_smoothed_gain;
  celt_sig preemph_memD[2], output_postfilter_mem[2];
};

struct OpusDecoder;
[[nodiscard]] static int decoder_packet_bitrate(const OpusDecoder* st, opus_int32 packet_bytes, int samples) noexcept;
[[nodiscard]] constexpr auto bitrate_to_bits(opus_int32 bitrate, opus_int32 sample_rate, opus_int32 frame_size) noexcept -> opus_int32 {
  return bitrate * 6 / (6 * sample_rate / frame_size);
}

[[nodiscard]] constexpr auto bits_to_bitrate_for_frame_rate(opus_int32 bits, int frame_rate) noexcept -> opus_int32 {
  return frame_rate == 16 ? bits * 100 / 6 : bits * frame_rate;
}

[[nodiscard]] constexpr auto bitrate_to_bits_for_frame_rate(opus_int32 bitrate, int frame_rate) noexcept -> opus_int32 {
  return frame_rate == 16 ? bitrate * 6 / 100 : bitrate / frame_rate;
}

struct vbr_frame_budget final {
  opus_int32 allocator_bits;
  opus_int32 max_bytes;
};

[[nodiscard]] constexpr auto make_vbr_frame_budget(opus_int32 target_bits, opus_int32 credit_bits) noexcept -> vbr_frame_budget {
  constexpr opus_int32 max_credit_spend_num = 1;
  constexpr opus_int32 max_credit_spend_den = 5;
  const opus_int32 max_spend_bits = target_bits * max_credit_spend_num / max_credit_spend_den;
  const opus_int32 spend_credit_bits = std::min(clamp_value(credit_bits, opus_int32{0}, target_bits * 25), max_spend_bits);
  const opus_int32 max_bytes = std::max<opus_int32>(2, (target_bits + spend_credit_bits) / 8);
  constexpr opus_int32 packet_rounding_credit_bits = 4 * 8;
  const bool useful_credit = spend_credit_bits > packet_rounding_credit_bits && spend_credit_bits >= max_spend_bits / 4;
  const opus_int32 allocator_spend_bits = useful_credit ? spend_credit_bits : 0;
  return {std::min(target_bits + allocator_spend_bits, max_bytes * 8), max_bytes};
}

[[nodiscard]] constexpr auto update_vbr_credit(opus_int32 credit_bits, opus_int32 actual_bytes, opus_int32 target_bits) noexcept
    -> opus_int32 {
  credit_bits += target_bits - actual_bytes * 8;
  const opus_int32 max_credit_bits = std::max<opus_int32>(target_bits * 25, 1000);
  return clamp_value(credit_bits, -std::max<opus_int32>(target_bits, 8), max_credit_bits);
}

static void celt_encoder_init(CeltEncoderInternal* st, opus_int32 sampling_rate, int channels);
static int celt_encode_with_ec(CeltEncoderInternal* st, const opus_res* pcm, int frame_size, unsigned char* compressed,
                               int nbCompressedBytes, ec_enc* enc);
static void celt_encoder_reset_state(CeltEncoderInternal* st);
static void celt_decoder_init(CeltDecoderInternal* st, opus_int32 sampling_rate, int channels);
static int celt_decode_with_ec(CeltDecoderInternal* st, const unsigned char* data, int len, opus_res* pcm, int frame_size, ec_dec* dec,
                               opus_int16* pcm16 = nullptr, OpusDecoder* output_filter_decoder = nullptr, int packet_bitrate_bps = 0);
static void celt_decoder_reset_state(CeltDecoderInternal* st);
static void apply_decoder_output_postfilter(OpusDecoder* st, opus_res* pcm, int samples, opus_int32 packet_bytes);
static void convert_decoder_output_postfilter(OpusDecoder* st, const opus_res* input, opus_int16* output, int samples,
                                              opus_int32 packet_bytes);
consteval auto numeric_blob_hex_value(char ch) -> unsigned {
  return ch <= '9' ? static_cast<unsigned>(ch - '0') : static_cast<unsigned>((ch | 0x20) - 'a' + 10);
}

template <typename T>
using numeric_blob_storage_t =
    std::conditional_t<sizeof(T) == 1, std::uint8_t,
                       std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
template <typename T, std::size_t Characters> consteval auto numeric_blob_array(const char (&blob)[Characters]) {
  using storage_t = numeric_blob_storage_t<T>;
  static_assert(sizeof(storage_t) == sizeof(T));
  std::array<T, (Characters - 1) / (2 * sizeof(T))> values{};
  auto position = std::size_t{};
  for (auto& value : values) {
    auto bits = storage_t{};
    for (std::size_t digit = 0; digit < sizeof(T) * 2; ++digit) {
      bits = static_cast<storage_t>((bits << 4U) | numeric_blob_hex_value(blob[position++]));
    }
    value = std::bit_cast<T>(bits);
  }
  return values;
}

template <typename T, std::size_t Columns, std::size_t Characters> consteval auto numeric_blob_matrix(const char (&blob)[Characters]) {
  return std::bit_cast<std::array<std::array<T, Columns>, (Characters - 1) / (2 * sizeof(T) * Columns)>>(numeric_blob_array<T>(blob));
}

constexpr std::array<unsigned char, 11> trim_icdf = numeric_blob_array<unsigned char>(R"blob(7E7C776D57291309040200)blob");
constexpr std::array<unsigned char, 4> spread_icdf = numeric_blob_array<unsigned char>(R"blob(19170200)blob");
constexpr std::array<unsigned char, 3> shared_three_step_icdf = numeric_blob_array<unsigned char>(R"blob(020100)blob");
struct hybrid_rate_entry {
  opus_uint16 threshold;
  opus_uint16 rate;
};
struct stereo_intensity_tables {
  std::array<opus_uint8, 21> thresholds;
  std::array<opus_uint8, 21> hysteresis;
};
constexpr std::array<unsigned char, 16> bit_interleave_table =
    numeric_blob_array<unsigned char>(R"blob(00010101020303030203030302030303)blob");
constexpr std::array<unsigned char, 16> bit_deinterleave_table =
    numeric_blob_array<unsigned char>(R"blob(00030C0F30333C3FC0C3CCCFF0F3FCFF)blob");
constexpr std::array<hybrid_rate_entry, 7> hybrid_rate_table{
    {{0, 0}, {12000, 10000}, {16000, 13500}, {20000, 16000}, {24000, 18000}, {32000, 22000}, {40000, 38000}}};
constexpr std::array<hybrid_rate_entry, 7> hybrid_fec_rate_table{
    {{0, 0}, {12000, 11000}, {16000, 15000}, {20000, 18000}, {24000, 21000}, {32000, 28000}, {64000, 50000}}};
constexpr std::array<opus_uint16, 10> fec_thresholds{12000, 1000, 14000, 1000, 16000, 1000, 20000, 1000, 22000, 1000};
constexpr opus_int32 low_rate_tonal_celt_max_bps = 32000;
constexpr opus_int32 low_rate_tonal_celt_boost_bps = 4000;
constexpr opus_val32 low_rate_tonal_celt_min_tone = .45f;
constexpr opus_int32 audio_midrate_celt_quality_boost_min_bps = 22000;
constexpr opus_int32 audio_midrate_celt_quality_boost_max_bps = 28000;
constexpr opus_int32 audio_midrate_celt_quality_boost_bps = 1750;
constexpr std::array<std::array<opus_val16, 3>, 3> comb_filter_tapset_gains =
    numeric_blob_matrix<opus_val16, 3>(R"blob(3E9D00003E5E40003E04C0003EED80003E894000000000003F4CC0003DCD000000000000)blob");
constexpr stereo_intensity_tables stereo_intensity_table{{1, 2, 3, 4, 5, 6, 7, 8, 16, 24, 36, 44, 50, 56, 62, 67, 72, 79, 88, 106, 134},
                                                         {1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 4, 5, 6, 8, 8}};
constexpr std::array<opus_val16, 16> celt_anti_collapse_thresh_by_depth = numeric_blob_array<opus_val16>(
    R"blob(3F0000003EEAC0C73ED744FD3EC5672A3EB504F33EA5FED73E9837F03E8B95C23E8000003E6AC0C73E5744FD3E45672A3E3504F33E25FED73E1837F03E0B95C2)blob");
struct mdct_lookup {
  int n;
  const kiss_fft_state* kfft[4];
  const float* trig;
};
static void clt_mdct_forward_c(const mdct_lookup* l, float* in, float* out, const celt_coef* window, int overlap, int shift, int stride);
static void clt_mdct_backward_c(const mdct_lookup* l, float* in, float* out, const celt_coef* window, int overlap, int shift, int stride);
static void clt_mdct_backward_dual_history_c(const mdct_lookup* l, float* in, float* out0, float* out1, const celt_coef* window,
                                             int overlap, int shift, int stride);
static void clt_mdct_backward_stereo_20ms_c(const mdct_lookup* l, float* in0, float* in1, float* out0, float* out1, const celt_coef* window,
                                            int overlap);
struct CeltModeInternal {
  const opus_int16* eBands;
  const unsigned char* allocVectors;
  const opus_int16* logN;
  const celt_coef* window;
  mdct_lookup mdct;
  const opus_int16* cache_index;
  const unsigned char* cache_bits;
  const unsigned char* cache_caps;
};
[[nodiscard]] static constexpr int ref_opus_packet_get_bandwidth(const unsigned char* data);
[[nodiscard]] static constexpr int ref_opus_packet_get_nb_channels(const unsigned char* data);
[[nodiscard]] static auto celt_maxabs16(const opus_val16* x, int len) noexcept -> opus_val32 {
  if (len <= 0) {
    return 0;
  }
  auto mn = x[0];
  auto mx = x[0];
  for (int index = 1; index < len; ++index) {
    const auto sample = x[index];
    mn = std::min(mn, sample);
    mx = std::max(mx, sample);
  }
  return mx > -mn ? mx : -mn;
}

[[nodiscard]] static constexpr auto celt_atan_norm(float x) noexcept -> float {
  const float x_sq = x * x;
  return 0.636619772367581f * (x + x * x_sq *
                                       (-3.3331659436225891113281250000e-01f +
                                        x_sq * (1.99627041816711425781250000000e-01f +
                                                x_sq * (-1.3976582884788513183593750000e-01f +
                                                        x_sq * (9.79423448443412780761718750000e-02f +
                                                                x_sq * (-5.7773590087890625000000000000e-02f +
                                                                        x_sq * (2.30401363223791122436523437500e-02f +
                                                                                x_sq * (-4.3554059229791164398193359375e-03f))))))));
}

static void celt_float2int16_c(const float* in, opus_int16* out, std::size_t count);
struct packet_frame_set {
  unsigned char toc = 0;
  int nb_frames = 0;
  int framesize = 0;
  std::array<const unsigned char*, 48> frames;
  std::array<opus_int16, 48> len;
};
[[nodiscard]] constexpr auto align(int value) noexcept -> int {
  constexpr auto alignment = static_cast<int>(std::max({alignof(void*), alignof(opus_int32), alignof(opus_val32)}));
  return ((value + alignment - 1) / alignment) * alignment;
}

static opus_int32 write_packet_frames(packet_frame_set* frames, unsigned char* data, opus_int32 maxlen, int pad);
static int append_packet_frames(packet_frame_set* frames, const unsigned char* data, opus_int32 len);
static int pad_packet(unsigned char* data, opus_int32 len, opus_int32 new_len);
static int encode_size(int size, unsigned char* data) {
  if (size < 252) {
    data[0] = static_cast<unsigned char>(size);
    return 1;
  }
  data[0] = static_cast<unsigned char>(252 + (size & 0x3));
  data[1] = static_cast<unsigned char>((size - static_cast<int>(data[0])) >> 2);
  return 2;
}

static bool parse_size(const unsigned char*& data, opus_int32& len, opus_int16& size) {
  if (len < 1) {
    return false;
  }
  const int bytes = 1 + (data[0] >= 252);
  if (bytes > len) {
    return false;
  }
  size = static_cast<opus_int16>(data[0] + (bytes == 2 ? 4 * data[1] : 0));
  data += bytes;
  len -= bytes;
  return true;
}

static int ref_opus_packet_get_samples_per_frame(const unsigned char* data, opus_int32 Fs) {
  const auto sample_rate = static_cast<opus_int64>(Fs);
  if (data[0] & 0x80) {
    const int frame_shift = (data[0] >> 3) & 0x3;
    return static_cast<int>(sample_rate * (1 << frame_shift) / 400);
  }
  if ((data[0] & 0x60) == 0x60) {
    return (data[0] & 0x08) ? Fs / 50 : Fs / 100;
  }
  const int frame_shift = (data[0] >> 3) & 0x3;
  return frame_shift == 3 ? static_cast<int>(sample_rate * 60 / 1000) : static_cast<int>(sample_rate * (1 << frame_shift) / 100);
}

static int ref_opus_packet_parse_impl(const unsigned char* data, opus_int32 len, const unsigned char* frames[48], opus_int16 size[48],
                                      int* payload_offset) {
  const unsigned char* data0 = data;
  if (size == nullptr || len < 0) {
    return -1;
  }
  if (len == 0) {
    return -4;
  }
  auto sizes = std::span<opus_int16>{size, std::size_t{48}};
  const auto toc = *data++;
  opus_int32 last_size = --len;
  int count = 1;
  switch (toc & 3) {
  case 0:
    break;
  case 1:
    count = 2;
    if (len & 0x1) {
      return -4;
    }
    last_size = len / 2;
    sizes[0] = static_cast<opus_int16>(last_size);
    break;
  case 2: {
    count = 2;
    if (!parse_size(data, len, sizes[0]) || sizes[0] > len) {
      return -4;
    }
    last_size = len - sizes[0];
    break;
  }
  default: {
    if (len < 1) {
      return -4;
    }
    const auto ch = *data++;
    count = ch & 0x3F;
    const int framesize = ref_opus_packet_get_samples_per_frame(data0, 48000);
    if (count <= 0 || framesize * static_cast<opus_int32>(count) > 5760) {
      return -4;
    }
    --len;
    if (ch & 0x40) {
      for (int p = 255; p == 255;) {
        if (len <= 0) {
          return -4;
        }
        p = *data++;
        len -= 1 + (p == 255 ? 254 : p);
      }
    }
    if (len < 0) {
      return -4;
    }
    const bool cbr = (ch & 0x80) == 0;
    if (!cbr) {
      opus_int32 payload_bytes = 0;
      for (int i = 0; i < count - 1; ++i) {
        if (!parse_size(data, len, sizes[i]) || sizes[i] > len) {
          return -4;
        }
        payload_bytes += sizes[i];
      }
      last_size = len - payload_bytes;
      if (last_size < 0) {
        return -4;
      }
    } else {
      last_size = len / count;
      if (last_size * count != len) {
        return -4;
      }
      std::fill_n(sizes.data(), static_cast<std::size_t>(count - 1), static_cast<opus_int16>(last_size));
    }
    break;
  }
  }
  if (last_size > 1275) {
    return -4;
  }
  sizes[count - 1] = static_cast<opus_int16>(last_size);
  if (payload_offset) {
    *payload_offset = static_cast<int>(data - data0);
  }
  if (frames != nullptr) {
    for (int i = 0; i < count; ++i) {
      frames[i] = data;
      data += sizes[i];
    }
  }
  return count;
}
struct silk_EncControlStruct {
  opus_int32 nChannelsAPI, nChannelsInternal, API_sampleRate, maxInternalSampleRate, minInternalSampleRate, desiredInternalSampleRate,
      bitRate, internalSampleRate;
  int payloadSize_ms, packetLossPercentage, complexity, useInBandFEC, LBRR_coded, useCBR, maxBits, toMono, opusCanSwitch,
      allowBandwidthSwitch, inWBmodeWithoutVariableLP, stereoWidth_Q14, switchReady, signalType, offset, preserveStereo;
};
struct silk_DecControlStruct {
  opus_int32 nChannelsInternal, nChannelsAPI, internalSampleRate, API_sampleRate;
  int payloadSize_ms;
};
[[nodiscard]] constexpr auto silk_encoder_get_size(int channels) noexcept -> int;
static void silk_InitEncoder(void* encState, int channels);
[[nodiscard]] constexpr auto silk_decoder_get_size() noexcept -> int;
static void silk_ResetDecoder(void* decState);
static int silk_Decode(void* decState, silk_DecControlStruct* decControl, int lostFlag, int newPacketFlag, ec_dec* psRangeDec,
                       opus_res* samplesOut, opus_int16* samplesOut16, opus_int32* nSamplesOut);
static void silk_Encode(void* encState, silk_EncControlStruct* encControl, const opus_res* samplesIn, int nSamplesIn, ec_enc* psRangeEnc,
                        opus_int32* nBytesOut, const int prefillFlag);
[[nodiscard]] inline auto float2int(float x) noexcept -> opus_int32 {
  return static_cast<opus_int32>(std::lrint(x));
}

[[nodiscard]] static inline auto pcm_float2int(float x) noexcept -> opus_int32 {
  if constexpr (std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(opus_int32)) {
    const float y = x + 12582912.0f;
    return std::bit_cast<opus_int32>(y) - 0x4B400000;
  }
  return float2int(x);
}

[[nodiscard]] static inline auto zero_tiny_float_mem(float value) noexcept -> float {
  return std::fabs(value) < 1e-15f ? 0.0f : value;
}

[[nodiscard]] static inline auto signal_to_float_pcm(float value) noexcept -> float {
  constexpr auto tiny_output_as_signal = 32768.0f * 1e-20f;
  return std::fabs(value) < tiny_output_as_signal ? 0.0f : (1.0f / 32768.0f) * value;
}

[[nodiscard]] static inline auto FLOAT2INT16(float x) noexcept -> opus_int16 {
  return static_cast<opus_int16>(pcm_float2int(clamp_value(x * 32768.f, -32768.f, 32767.f)));
}
struct silk_resampler_state_struct {
  opus_int32 sIIR[6];
  opus_int32 invRatio_Q16;
  union {
    opus_int32 i32[36];
    opus_int16 i16[36];
  } sFIR;
  opus_int16 delayBuf[96];
  int resampler_function, FIR_Order, FIR_Fracs, Fs_in_kHz, Fs_out_kHz, inputDelay;
  const opus_int16* Coefs;
};
[[nodiscard]] static auto silk_CLZ32(opus_int32 value) noexcept -> opus_int32 {
  return value ? std::countl_zero(std::bit_cast<opus_uint32>(value)) : 32;
}

static void silk_resampler_init(silk_resampler_state_struct* S, opus_int32 Fs_Hz_in, opus_int32 Fs_Hz_out, int forEnc);
static void silk_resampler(silk_resampler_state_struct* S, opus_int16 out[], const opus_int16 in[], opus_int32 inLen);
template <typename T> static void silk_bwexpander(T* ar, std::size_t count, opus_int32 chirp_Q16);
static opus_int32 silk_LPC_inverse_pred_gain_c(const opus_int16* A_Q12, const int order);
static void silk_ana_filt_bank_1(const opus_int16* in, opus_int32 S[2], opus_int16* outL, opus_int16* outH, const opus_int32 N);
static int silk_sigm_Q15(int in_Q5);
static void silk_sum_sqr_shift(opus_int32* energy, int* shift, const opus_int16* x, int len);
static void silk_decode_pitch(opus_int16 lagIndex, opus_uint8 contourIndex, int pitch_lags[], const int Fs_kHz, const int nb_subfr);
static void silk_NLSF2A(opus_int16* a_Q12, const opus_int16* NLSF, const int d);
static void silk_LPC_fit(opus_int16* a_QOUT, opus_int32* a_QIN, int d);
static void silk_insertion_sort_increasing(opus_int32* a, int* idx, const int L, const int K);
static void silk_insertion_sort_increasing_all_values_int16(opus_int16* a, const int L);
static void silk_NLSF_stabilize(opus_int16* NLSF_Q15, const opus_int16* NDeltaMin_Q15, const int L);
static void silk_NLSF_VQ_weights_laroia(opus_int16* pNLSFW_Q_OUT, const opus_int16* pNLSF_Q15, const int D);
[[nodiscard]] constexpr auto silk_ROR32(opus_int32 value, int rotation) noexcept -> opus_int32 {
  const auto bits = std::bit_cast<opus_uint32>(value);
  const auto positive_rotation = static_cast<opus_uint32>(rotation);
  const auto negative_rotation = static_cast<opus_uint32>(-rotation);
  if (rotation == 0) {
    return value;
  }
  return rotation < 0 ? std::bit_cast<opus_int32>((bits << negative_rotation) | (bits >> (32 - negative_rotation)))
                      : std::bit_cast<opus_int32>((bits << (32 - positive_rotation)) | (bits >> positive_rotation));
}

static auto silk_CLZ_FRAC(opus_int32 value, opus_int32* lz, opus_int32* frac_Q7) noexcept -> void {
  const opus_int32 leading_zeros = silk_CLZ32(value);
  *lz = leading_zeros;
  *frac_Q7 = silk_ROR32(value, 24 - leading_zeros) & 0x7f;
}

[[nodiscard]] static inline auto silk_varq_shift(opus_int32 value, int lshift) noexcept -> opus_int32 {
  return lshift <= 0 ? saturating_left_shift(value, -lshift) : lshift < 32 ? value >> lshift : 0;
}

[[nodiscard]] static auto silk_SQRT_APPROX(opus_int32 x) noexcept -> opus_int32 {
  if (x <= 0) {
    return 0;
  }
  opus_int32 lz = 0, frac_Q7 = 0;
  silk_CLZ_FRAC(x, &lz, &frac_Q7);
  opus_int32 y = (lz & 1) ? 32768 : 46214;
  y >>= (lz >> 1);
  y = static_cast<opus_int32>(y + ((y * static_cast<opus_int64>(static_cast<opus_int16>(static_cast<opus_int32>(
                                            static_cast<opus_int16>(213) * static_cast<opus_int32>(static_cast<opus_int16>(frac_Q7)))))) >>
                                   16));
  return y;
}

[[nodiscard]] static auto silk_DIV32_varQ(const opus_int32 a32, const opus_int32 b32, const int Qres) noexcept -> opus_int32 {
  const auto a_headroom = silk_CLZ32(a32 > 0 ? a32 : wrap_subtract(0, a32)) - 1;
  const auto b_headroom = silk_CLZ32(b32 > 0 ? b32 : wrap_subtract(0, b32)) - 1;
  auto normalized_a = wrap_shift_left(a32, a_headroom);
  const auto normalized_b = wrap_shift_left(b32, b_headroom);
  const auto inverse_b = (0x7FFFFFFF >> 2) / (normalized_b >> 16);
  auto result = silk_mul_wb(normalized_a, inverse_b);
  normalized_a = wrap_subtract(normalized_a, wrap_shift_left(silk_mul_high(normalized_b, result), 3));
  result = silk_mla_wb(result, normalized_a, inverse_b);
  return silk_varq_shift(result, 29 + a_headroom - b_headroom - Qres);
}

[[nodiscard]] static auto silk_INVERSE32_varQ(const opus_int32 b32, const int Qres) noexcept -> opus_int32 {
  const auto b_headroom = silk_CLZ32(b32 > 0 ? b32 : wrap_subtract(0, b32)) - 1;
  const auto normalized_b = wrap_shift_left(b32, b_headroom);
  const auto inverse_b = (0x7FFFFFFF >> 2) / (normalized_b >> 16);
  auto result = wrap_shift_left(inverse_b, 16);
  const auto error_Q32 = wrap_shift_left(wrap_subtract(1 << 29, silk_mul_wb(normalized_b, inverse_b)), 3);
  result = wrap_add(result, static_cast<opus_int32>((static_cast<opus_int64>(error_Q32) * inverse_b) >> 16));
  return silk_varq_shift(result, 61 - b_headroom - Qres);
}
struct silk_nsq_state {
  opus_int16 xq[2 * ((5 * 4) * 16)];
  opus_int32 sLTP_shp_Q14[2 * ((5 * 4) * 16)], sLPC_Q14[(5 * 16) + 16], sAR2_Q14[24], sLF_AR_shp_Q14, sDiff_shp_Q14;
  int lagPrev, sLTP_buf_idx, sLTP_shp_buf_idx;
  opus_int32 rand_seed, prev_gain_Q16;
  int rewhite_flag;
};
struct silk_VAD_state {
  std::array<opus_int32, 2> AnaState, AnaState1, AnaState2;
  std::array<opus_int32, 4> XnrgSubfr, NrgRatioSmth_Q8;
  opus_int16 HPstate;
  std::array<opus_int32, 4> NL, inv_NL;
  opus_int32 counter;
};
struct silk_LP_state {
  std::array<opus_int32, 2> In_LP_State;
  opus_int32 transition_frame_no;
  int mode;
  opus_int32 saved_fs_kHz;
};
struct silk_NLSF_CB_struct {
  const opus_int16 nVectors, order, quantStepSize_Q16, invQuantStepSize_Q6;
  const opus_uint8* CB1_NLSF_Q8;
  const opus_int16* CB1_Wght_Q9;
  const opus_uint8 *CB1_iCDF, *pred_Q8, *ec_sel, *ec_iCDF, *ec_Rates_Q5;
  const opus_int16* deltaMin_Q15;
};
using silk_stereo_pred_indices = std::array<std::array<opus_int8, 3>, 2>;

struct stereo_enc_state {
  std::array<opus_int16, 2> pred_prev_Q13, sMid, sSide;
  std::array<opus_int32, 4> mid_side_amp_Q0;
  opus_int16 smth_width_Q14, width_prev_Q14, silent_side_len;
  std::array<silk_stereo_pred_indices, 3> predIx;
  std::array<opus_uint8, 3> mid_only_flags;
};
struct stereo_dec_state {
  std::array<opus_int16, 2> pred_prev_Q13, sMid, sSide;
};
struct SideInfoIndices {
  opus_int16 lagIndex;
  opus_int8 GainsIndices[4];
  opus_uint8 LTPIndex[4];
  opus_int8 NLSFIndices[16 + 1];
  opus_uint8 contourIndex, signalType, quantOffsetType, NLSFInterpCoef_Q2, PERIndex, LTP_scaleIndex, Seed;
};

struct silk_lbrr_channel_state {
  std::array<SideInfoIndices, 3> indices;
  std::array<std::array<opus_int8, (5 * 4) * 16>, 3> pulses;
  std::array<opus_int8, 3> flags;
  opus_int8 previous_gain_index;
  silk_nsq_state nsq;
  int enabled, gain_increase;
};

struct silk_lbrr_state {
  std::array<silk_lbrr_channel_state, celt_max_channels> channels;
  opus_int32 average_bits;
  int frames_per_packet, channels_in_packet;
};

struct silk_encoder_state {
  opus_int32 variable_HP_smth1_Q15;
  silk_LP_state sLP;
  silk_VAD_state sVAD;
  silk_nsq_state sNSQ;
  std::array<opus_int16, 16> prev_NLSFq_Q15;
  int speech_activity_Q8;
  opus_int8 prevSignalType;
  int prevLag;
  opus_int32 API_fs_Hz;
  int fs_kHz, nb_subfr, frame_length, subfr_length, ltp_mem_length, la_shape, shapeWinLength;
  opus_int32 TargetRate_bps;
  opus_int32 frameCounter;
  int Complexity, nStatesDelayedDecision, shapingLPCOrder, predictLPCOrder, pitchEstimationComplexity, pitchEstimationLPCOrder;
  opus_int32 pitchEstimationThreshold_Q16, sum_log_gain_Q7;
  int NLSF_MSVQ_Survivors, first_frame_after_reset, warping_Q16, useCBR, prefillFlag;
  const silk_NLSF_CB_struct* psNLSF_CB;
  std::array<int, 4> input_quality_bands_Q15;
  int input_tilt_Q15, SNR_dB_Q7;
  std::array<opus_int8, 3> VAD_flags;
  SideInfoIndices indices;
  opus_int8 pulses[((5 * 4) * 16)];
  opus_int16 inputBuf[((5 * 4) * 16) + 2];
  int inputBufIx, nFramesPerPacket, nFramesEncoded, nChannelsInternal, ec_prevSignalType;
  opus_int16 ec_prevLagIndex;
  silk_resampler_state_struct resampler_state;
};
struct silk_PLC_struct {
  opus_int32 pitchL_Q8, rand_seed, conc_energy, prevGain_Q16[2];
  std::array<opus_uint16, 4> pitch_history;
  opus_int16 LTPCoef_Q14, prevLPC_Q12[16];
  opus_int16 randScale_Q14, prevLTP_scale_Q14;
  opus_uint8 pitch_history_index;
  int last_frame_lost, conc_energy_shift, nb_subfr, subfr_length;
};
struct silk_CNG_struct {
  opus_int32 CNG_exc_buf_Q14[((5 * 4) * 16)], CNG_synth_state[16], CNG_smth_Gain_Q16, rand_seed;
  opus_int16 CNG_smth_NLSF_Q15[16];
};
struct silk_decoder_state {
  opus_int32 prev_gain_Q16, exc_Q14[((5 * 4) * 16)], sLPC_Q14_buf[16];
  opus_int16 outBuf[((5 * 4) * 16) + 2 * (5 * 16)], prevNLSF_Q15[16], ec_prevLagIndex;
  SideInfoIndices indices;
  int lagPrev, fs_kHz, nb_subfr, frame_length, subfr_length, ltp_mem_length, LPC_order, first_frame_after_reset, nFramesDecoded,
      nFramesPerPacket, ec_prevSignalType, VAD_flags[3], LBRR_flags[3], lossCnt, prevSignalType;
  opus_int8 LastGainIndex;
  const silk_NLSF_CB_struct* psNLSF_CB;
  silk_CNG_struct* sCNG;
  silk_resampler_state_struct resampler_state;
  silk_PLC_struct sPLC;
};
static void silk_CNG_Reset(silk_decoder_state* psDec);
static void silk_release_cng(silk_decoder_state* psDec) noexcept {
  if (psDec != nullptr && psDec->sCNG != nullptr) {
    std::free(psDec->sCNG);
    psDec->sCNG = nullptr;
  }
}

[[nodiscard]] static auto silk_ensure_cng(silk_decoder_state* psDec) noexcept -> silk_CNG_struct* {
  if (psDec->sCNG == nullptr) {
    auto* cng = static_cast<silk_CNG_struct*>(std::calloc(1, sizeof(silk_CNG_struct)));
    if (cng == nullptr) {
      return nullptr;
    }
    psDec->sCNG = cng;
    silk_CNG_Reset(psDec);
  }
  return psDec->sCNG;
}
struct silk_decoder_control {
  int pitchL[4];
  int LTP_scale_Q14;
  opus_int32 Gains_Q16[4];
  opus_int16 PredCoef_Q12[2][16], LTPCoef_Q14[5 * 4];
};
struct OpusDecoder {
  opus_int32 Fs;
  int channels;
  int stream_channels, mode, prev_mode, bandwidth, frame_size, prev_redundancy, last_packet_duration;
  opus_uint32 rangeFinal;
};
static_assert(std::is_standard_layout_v<OpusDecoder>);
static_assert(sizeof(OpusDecoder) <= 80);
[[nodiscard]] static inline auto decoder_silk_state(OpusDecoder* st) noexcept -> void* {
  return offset_ptr<void>(st, align(sizeof(OpusDecoder)));
}

[[nodiscard]] static inline auto decoder_celt_state(OpusDecoder* st) noexcept -> CeltDecoderInternal* {
  return offset_ptr<CeltDecoderInternal>(st, align(sizeof(OpusDecoder)) + align(silk_decoder_get_size()));
}

static void ref_opus_decoder_init(OpusDecoder* st, opus_int32 Fs, int channels) {
  *st = {};
  auto* silk_dec = decoder_silk_state(st);
  auto* celt_dec = decoder_celt_state(st);
  zero_n_bytes(silk_dec, static_cast<std::size_t>(silk_decoder_get_size()));
  st->stream_channels = st->channels = channels;
  st->Fs = Fs;
  silk_ResetDecoder(silk_dec);
  celt_decoder_init(celt_dec, Fs, channels);
  st->prev_mode = 0;
  st->frame_size = Fs / 400;
}

static void smooth_fade(const opus_res* in1, const opus_res* in2, opus_res* out, int overlap, int channels, opus_int32 Fs) {
  int inc = 48000 / Fs;
  for (int c = 0; c < channels; c++) {
    for (int i = 0; i < overlap; i++) {
      opus_val16 w = celt_mode()->window[i * inc];
      w *= w;
      out[i * channels + c] = w * in2[i * channels + c] + (1.0f - w) * in1[i * channels + c];
    }
  }
}

static int celt_decode_then_add(CeltDecoderInternal* celt_dec, const unsigned char* data, int len, opus_res* pcm, int frame_size,
                                ec_dec* dec, int channels) {
  std::array<opus_res, celt_max_frame_samples * celt_max_channels> mix;
  const int ret = celt_decode_with_ec(celt_dec, data, len, mix.data(), frame_size, dec);
  if (ret >= 0) {
    const auto mix_samples = static_cast<std::size_t>(ret) * static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < mix_samples; ++i) {
      pcm[i] += mix[i];
    }
  }
  return ret;
}

static int opus_packet_get_mode(const unsigned char* data) {
  return data[0] & 0x80 ? opus_mode_celt_only : (data[0] & 0x60) == 0x60 ? opus_mode_hybrid : opus_mode_silk_only;
}

static inline void decoder_apply_packet_state(OpusDecoder* st, int mode, int bandwidth, int frame_size, int stream_channels) noexcept {
  st->mode = mode;
  st->bandwidth = bandwidth;
  st->frame_size = frame_size;
  st->stream_channels = stream_channels;
}

static int opus_decode_frame(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_res* pcm, int frame_size, int decode_fec) {
  ec_dec dec;
  opus_int32 silk_frame_size;
  opus_res* pcm_transition = nullptr;
  int celt_ret = 0, transition = 0;
  int redundancy = 0, redundancy_bytes = 0, celt_to_silk = 0;
  opus_uint32 redundant_rng = 0;
  auto* silk_dec = decoder_silk_state(st);
  auto* celt_dec = decoder_celt_state(st);
  const int F20 = st->Fs / 50;
  const int F10 = F20 >> 1;
  const int F5 = F10 >> 1;
  const int F2_5 = F5 >> 1;
  if (frame_size < F2_5) {
    return -2;
  }
  frame_size = std::min(frame_size, st->Fs / 25 * 3);
  if (len <= 1) {
    data = nullptr;
    frame_size = std::min(frame_size, st->frame_size);
  }
  int audiosize = data != nullptr ? st->frame_size : frame_size;
  const int mode = data != nullptr ? st->mode : st->prev_redundancy ? opus_mode_celt_only : st->prev_mode;
  const int bandwidth = data != nullptr ? st->bandwidth : 0;
  if (data != nullptr) {
    ec_dec_init(&dec, const_cast<unsigned char*>(data), len);
  } else {
    if (mode == 0) {
      zero_n_items(pcm, static_cast<std::size_t>(audiosize * st->channels));
      return audiosize;
    }
    if (audiosize > F20) {
      while (audiosize > 0) {
        const int ret = opus_decode_frame(st, nullptr, 0, pcm, std::min(audiosize, F20), 0);
        if (ret < 0) {
          return ret;
        }
        pcm += ret * st->channels;
        audiosize -= ret;
      }
      return frame_size;
    }
    if (audiosize < F20) {
      if (audiosize > F10) {
        audiosize = F10;
      } else if (mode != opus_mode_silk_only && audiosize > F5 && audiosize < F10)
        audiosize = F5;
    }
  }
  if (data != nullptr && st->prev_mode > 0 &&
      ((mode == opus_mode_celt_only && st->prev_mode != opus_mode_celt_only && !st->prev_redundancy) ||
       (mode != opus_mode_celt_only && st->prev_mode == opus_mode_celt_only))) {
    transition = 1;
  }
  std::array<opus_res, celt_max_frame_samples * celt_max_channels / 4> auxiliary_storage;
  if (transition && mode == opus_mode_celt_only) {
    pcm_transition = auxiliary_storage.data();
    opus_decode_frame(st, nullptr, 0, pcm_transition, std::min(F5, audiosize), 0);
  }
  if (audiosize > frame_size) {
    return -1;
  }
  frame_size = audiosize;
  if (mode != opus_mode_celt_only) {
    const bool pcm_too_small = frame_size < F10;
    std::array<opus_res, celt_max_frame_samples> pcm_silk;
    auto* pcm_ptr = pcm_too_small ? pcm_silk.data() : pcm;
    if (st->prev_mode == opus_mode_celt_only) {
      silk_ResetDecoder(silk_dec);
    }
    const int silk_bandwidth = data != nullptr ? bandwidth : st->bandwidth;
    silk_DecControlStruct dec_control{st->stream_channels, st->channels,
                                      mode == opus_mode_silk_only ? silk_bandwidth == 1101   ? 8000
                                                                    : silk_bandwidth == 1102 ? 12000
                                                                                             : 16000
                                                                  : 16000,
                                      st->Fs, std::max(10, 1000 * audiosize / st->Fs)};
    const int lost_flag = data == nullptr ? 1 : 2 * !!decode_fec;
    int decoded_samples = 0;
    for (; decoded_samples < frame_size;) {
      const int first_frame = decoded_samples == 0;
      const int silk_ret = silk_Decode(silk_dec, &dec_control, lost_flag, first_frame, &dec, pcm_ptr, nullptr, &silk_frame_size);
      if (silk_ret) {
        if (lost_flag) {
          silk_frame_size = frame_size;
          zero_n_items(pcm_ptr, static_cast<std::size_t>(frame_size * st->channels));
        } else {
          return -3;
        }
      }
      pcm_ptr += silk_frame_size * st->channels;
      decoded_samples += silk_frame_size;
    }
    if (pcm_too_small) {
      copy_n_items(pcm_silk.data(), static_cast<std::size_t>(frame_size * st->channels), pcm);
    }
  }
  if (!decode_fec && mode != opus_mode_celt_only && data != nullptr && ec_tell(&dec) + 17 + 20 * (mode == opus_mode_hybrid) <= 8 * len) {
    if (mode == opus_mode_hybrid) {
      redundancy = ec_dec_bit_logp(&dec, 12);
    } else
      redundancy = 1;
    if (redundancy) {
      celt_to_silk = ec_dec_bit_logp(&dec, 1);
      redundancy_bytes = mode == opus_mode_hybrid ? static_cast<opus_int32>(ec_dec_uint(&dec, 256)) + 2 : len - ((ec_tell(&dec) + 7) >> 3);
      len -= redundancy_bytes;
      if (len * 8 < ec_tell(&dec)) {
        len = 0;
        redundancy_bytes = 0;
        redundancy = 0;
      }
      dec.storage -= redundancy_bytes;
    }
  }
  const int start_band = mode != opus_mode_celt_only ? 17 : 0;
  if (redundancy) {
    transition = 0;
  }
  if (transition && mode != opus_mode_celt_only) {
    pcm_transition = auxiliary_storage.data();
    opus_decode_frame(st, nullptr, 0, pcm_transition, std::min(F5, audiosize), 0);
  }
  if (bandwidth) {
    const auto endband = bandwidth_to_endband(bandwidth);
    celt_dec->end = endband;
  }
  celt_dec->stream_channels = st->stream_channels;
  auto* redundant_audio = auxiliary_storage.data();
  if (redundancy && celt_to_silk) {
    celt_dec->start = 0;
    celt_decode_with_ec(celt_dec, data + len, redundancy_bytes, redundant_audio, F5, nullptr);
    redundant_rng = celt_dec->rng;
  }
  celt_dec->start = start_band;
  if (mode != opus_mode_silk_only) {
    int celt_frame_size = std::min(F20, frame_size);
    if (mode != st->prev_mode && st->prev_mode > 0 && !st->prev_redundancy) {
      celt_decoder_reset_state(celt_dec);
    }
    celt_ret = mode != opus_mode_celt_only
                   ? celt_decode_then_add(celt_dec, decode_fec ? nullptr : data, len, pcm, celt_frame_size, &dec, st->channels)
                   : celt_decode_with_ec(celt_dec, decode_fec ? nullptr : data, len, pcm, celt_frame_size, &dec);
    st->rangeFinal = celt_dec->rng;
  } else {
    constexpr unsigned char silence[2] = {0xFF, 0xFF};
    if (st->prev_mode == opus_mode_hybrid && !(redundancy && celt_to_silk && st->prev_redundancy)) {
      celt_dec->start = 0;
      celt_decode_then_add(celt_dec, silence, 2, pcm, F2_5, nullptr, st->channels);
    }
    st->rangeFinal = dec.rng;
  }
  if (redundancy && !celt_to_silk) {
    celt_decoder_reset_state(celt_dec);
    celt_dec->start = 0;
    celt_decode_with_ec(celt_dec, data + len, redundancy_bytes, redundant_audio, F5, nullptr);
    redundant_rng = celt_dec->rng;
    smooth_fade(pcm + st->channels * (frame_size - F2_5), redundant_audio + st->channels * F2_5, pcm + st->channels * (frame_size - F2_5),
                F2_5, st->channels, st->Fs);
  }
  if (redundancy && celt_to_silk && (st->prev_mode != opus_mode_silk_only || st->prev_redundancy)) {
    copy_n_items(redundant_audio, static_cast<std::size_t>(F2_5 * st->channels), pcm);
    smooth_fade(redundant_audio + st->channels * F2_5, pcm + st->channels * F2_5, pcm + st->channels * F2_5, F2_5, st->channels, st->Fs);
  }
  if (transition) {
    if (audiosize >= F5) {
      copy_n_items(pcm_transition, static_cast<std::size_t>(st->channels * F2_5), pcm);
      smooth_fade(pcm_transition + st->channels * F2_5, pcm + st->channels * F2_5, pcm + st->channels * F2_5, F2_5, st->channels, st->Fs);
    } else {
      smooth_fade(pcm_transition, pcm, pcm, F2_5, st->channels, st->Fs);
    }
  }
  if (len <= 1) {
    st->rangeFinal = 0;
  } else {
    st->rangeFinal ^= redundant_rng;
  }
  st->prev_mode = mode;
  st->prev_redundancy = redundancy && !celt_to_silk;
  return celt_ret < 0 ? celt_ret : audiosize;
}

static int decode_native(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_res* pcm, int frame_size, int decode_fec) {
  if (decode_fec < 0 || decode_fec > 1) {
    return -1;
  }
  if ((decode_fec || len == 0 || data == nullptr) && frame_size % (st->Fs / 400) != 0) {
    return -1;
  }
  if (len == 0 || data == nullptr) {
    int pcm_count = 0;
    for (; pcm_count < frame_size;) {
      int ret = opus_decode_frame(st, nullptr, 0, pcm + pcm_count * st->channels, frame_size - pcm_count, 0);
      if (ret < 0) {
        return ret;
      }
      pcm_count += ret;
    }
    st->last_packet_duration = pcm_count;
    return pcm_count;
  } else if (len < 0) {
    return -1;
  }
  const int packet_mode = opus_packet_get_mode(data);
  const int packet_bandwidth = ref_opus_packet_get_bandwidth(data);
  const int packet_frame_size = ref_opus_packet_get_samples_per_frame(data, st->Fs);
  const int packet_stream_channels = ref_opus_packet_get_nb_channels(data);
  std::array<opus_int16, 48> frame_lengths;
  int payload_offset;
  const int frame_count = ref_opus_packet_parse_impl(data, len, nullptr, frame_lengths.data(), &payload_offset);
  if (frame_count < 0) {
    return frame_count;
  }
  data += payload_offset;
  if (decode_fec) {
    if (frame_size < packet_frame_size || packet_mode == opus_mode_celt_only || st->mode == opus_mode_celt_only) {
      return decode_native(st, nullptr, 0, pcm, frame_size, 0);
    }
    const int duration_copy = st->last_packet_duration;
    int ret = 0;
    if (frame_size - packet_frame_size != 0) {
      ret = decode_native(st, nullptr, 0, pcm, frame_size - packet_frame_size, 0);
      if (ret < 0) {
        st->last_packet_duration = duration_copy;
        return ret;
      }
    }
    decoder_apply_packet_state(st, packet_mode, packet_bandwidth, packet_frame_size, packet_stream_channels);
    ret = opus_decode_frame(st, data, frame_lengths[0], pcm + st->channels * (frame_size - packet_frame_size), packet_frame_size, 1);
    if (ret < 0) {
      return ret;
    }
    st->last_packet_duration = frame_size;
    return frame_size;
  }
  if (frame_count * packet_frame_size > frame_size) {
    return -2;
  }
  decoder_apply_packet_state(st, packet_mode, packet_bandwidth, packet_frame_size, packet_stream_channels);
  int nb_samples = 0;
  for (int i = 0; i < frame_count; i++) {
    int ret = opus_decode_frame(st, data, frame_lengths[i], pcm + nb_samples * st->channels, frame_size - nb_samples, 0);
    if (ret < 0) {
      return ret;
    }
    data += frame_lengths[i];
    nb_samples += ret;
  }
  st->last_packet_duration = nb_samples;
  return nb_samples;
}

constexpr int opus_decode_fast_unavailable = -1000000;
template <int Mode>
  requires(Mode == opus_mode_silk_only || Mode == opus_mode_celt_only)
static int decode_native_mode_direct_fast(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_res* pcm, opus_int16* pcm16,
                                          int frame_size, bool fuse_output_postfilter = false) {
  if constexpr (Mode == opus_mode_celt_only) {
    if (st->Fs != 48000 || (pcm == nullptr && pcm16 == nullptr) || (st->prev_mode > 0 && st->prev_mode != Mode && !st->prev_redundancy)) {
      return opus_decode_fast_unavailable;
    }
  } else if (fuse_output_postfilter || st->prev_redundancy || (st->prev_mode > 0 && st->prev_mode != Mode)) {
    return opus_decode_fast_unavailable;
  }
  const int packet_bandwidth = ref_opus_packet_get_bandwidth(data);
  const int packet_frame_size = ref_opus_packet_get_samples_per_frame(data, st->Fs);
  const int packet_stream_channels = ref_opus_packet_get_nb_channels(data);
  std::array<opus_int16, 48> frame_lengths;
  int payload_offset;
  const int frame_count = ref_opus_packet_parse_impl(data, len, nullptr, frame_lengths.data(), &payload_offset);
  if (frame_count < 0 || frame_count * packet_frame_size > frame_size || (fuse_output_postfilter && frame_count != 1)) {
    return opus_decode_fast_unavailable;
  }
  const int packet_bitrate_bps = fuse_output_postfilter ? decoder_packet_bitrate(st, len, frame_count * packet_frame_size) : 0;
  decoder_apply_packet_state(st, Mode, packet_bandwidth, packet_frame_size, packet_stream_channels);
  auto* celt_dec = decoder_celt_state(st);
  auto* silk_dec = decoder_silk_state(st);
  if constexpr (Mode == opus_mode_celt_only) {
    if (packet_bandwidth) {
      celt_dec->end = bandwidth_to_endband(packet_bandwidth);
    }
    celt_dec->stream_channels = packet_stream_channels;
    celt_dec->start = 0;
  }
  data += payload_offset;
  int nb_samples = 0;
  silk_DecControlStruct dec_control{};
  if constexpr (Mode == opus_mode_silk_only) {
    dec_control = {packet_stream_channels, st->channels,
                   packet_bandwidth == 1101   ? 8000
                   : packet_bandwidth == 1102 ? 12000
                                              : 16000,
                   st->Fs, std::max(10, 1000 * packet_frame_size / st->Fs)};
  }
  for (int index = 0; index < frame_count; ++index) {
    ec_dec dec;
    ec_dec_init(&dec, const_cast<unsigned char*>(data), static_cast<opus_uint32>(frame_lengths[index]));
    int decoded_samples;
    if constexpr (Mode == opus_mode_celt_only) {
      const int sample_offset = nb_samples * st->channels;
      decoded_samples = celt_decode_with_ec(celt_dec, data, frame_lengths[index], pcm != nullptr ? pcm + sample_offset : nullptr,
                                            packet_frame_size, &dec, pcm16 != nullptr ? pcm16 + sample_offset : nullptr,
                                            fuse_output_postfilter ? st : nullptr, packet_bitrate_bps);
      if (decoded_samples < 0) {
        return decoded_samples;
      }
      st->rangeFinal = celt_dec->rng;
    } else {
      decoded_samples = 0;
      while (decoded_samples < packet_frame_size) {
        opus_int32 silk_frame_size = 0;
        if (silk_Decode(silk_dec, &dec_control, 0, decoded_samples == 0, &dec, nullptr,
                        pcm16 + (nb_samples + decoded_samples) * st->channels, &silk_frame_size) != 0) {
          return -3;
        }
        decoded_samples += silk_frame_size;
      }
      st->rangeFinal = dec.rng;
    }
    data += frame_lengths[index];
    nb_samples += decoded_samples;
  }
  st->prev_mode = Mode;
  st->prev_redundancy = 0;
  st->last_packet_duration = nb_samples;
  return nb_samples;
}

static int decode_native_direct_fast(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_res* pcm, opus_int16* pcm16,
                                     int frame_size, int decode_fec, bool fuse_output_postfilter = false) {
  if (decode_fec || data == nullptr || len <= 1) {
    return opus_decode_fast_unavailable;
  }
  const int packet_mode = opus_packet_get_mode(data);
  if (packet_mode == opus_mode_celt_only) {
    return decode_native_mode_direct_fast<opus_mode_celt_only>(st, data, len, pcm, pcm16, frame_size, fuse_output_postfilter);
  }
  return pcm16 != nullptr && packet_mode == opus_mode_silk_only
             ? decode_native_mode_direct_fast<opus_mode_silk_only>(st, data, len, nullptr, pcm16, frame_size, fuse_output_postfilter)
             : opus_decode_fast_unavailable;
}

[[nodiscard]] static int decoder_packet_bitrate(const OpusDecoder* st, opus_int32 packet_bytes, int samples) noexcept {
  return static_cast<int>(std::min<opus_int64>((static_cast<opus_int64>(packet_bytes) * 8 * st->Fs) / samples, opus_int32_max));
}

template <std::size_t Capacity>
static int decode_pcm16_fallback(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_int16* pcm, int frame_size,
                                 int decode_fec, bool output_postfilter_enabled) {
  std::array<opus_res, Capacity> output_storage;
  const int ret = decode_native(st, data, len, output_storage.data(), frame_size, decode_fec);
  if (ret > 0) {
    if (output_postfilter_enabled) {
      if (st->channels == 2 && decoder_celt_state(st)->output_postfilter_level == 3) {
        apply_decoder_output_postfilter(st, output_storage.data(), ret, len);
        celt_float2int16_c(output_storage.data(), pcm, static_cast<std::size_t>(ret * st->channels));
      } else {
        convert_decoder_output_postfilter(st, output_storage.data(), pcm, ret, len);
      }
    } else {
      celt_float2int16_c(output_storage.data(), pcm, static_cast<std::size_t>(ret * st->channels));
    }
  }
  return ret;
}

template <typename Sample>
static int decode_to_output(OpusDecoder* st, const unsigned char* data, opus_int32 len, Sample* pcm, int frame_size, int decode_fec) {
  if (frame_size <= 0) {
    return -1;
  }
  if (frame_size > opus_max_frame_samples_48k && (decode_fec || data == nullptr || len <= 0)) {
    return -1;
  }
  auto* celt_dec = decoder_celt_state(st);
  const bool output_postfilter_enabled = celt_dec->output_postfilter_level != 0;
  if constexpr (std::same_as<Sample, float>) {
    const int fast_ret = decode_native_direct_fast(st, data, len, pcm, nullptr, frame_size, decode_fec);
    if (fast_ret != opus_decode_fast_unavailable) {
      if (output_postfilter_enabled) {
        apply_decoder_output_postfilter(st, pcm, fast_ret, len);
      }
      return fast_ret;
    }
    const int ret = decode_native(st, data, len, pcm, frame_size, decode_fec);
    if (ret > 0 && output_postfilter_enabled) {
      apply_decoder_output_postfilter(st, pcm, ret, len);
    }
    return ret;
  } else {
    const bool preserve_stereo_auto = output_postfilter_enabled && st->channels == 2 && celt_dec->output_postfilter_level == 3;
    const bool fuse_output_postfilter = output_postfilter_enabled && !preserve_stereo_auto;
    const int fast_ret = preserve_stereo_auto
                             ? opus_decode_fast_unavailable
                             : decode_native_direct_fast(st, data, len, nullptr, pcm, frame_size, decode_fec, fuse_output_postfilter);
    if (fast_ret != opus_decode_fast_unavailable) {
      return fast_ret;
    }
    if (frame_size <= celt_max_frame_samples) {
      return decode_pcm16_fallback<celt_max_frame_samples * celt_max_channels>(st, data, len, pcm, frame_size, decode_fec,
                                                                               output_postfilter_enabled);
    }
    return decode_pcm16_fallback<opus_max_pcm_samples>(st, data, len, pcm, frame_size, decode_fec, output_postfilter_enabled);
  }
}

int opus_decode(OpusDecoder* st, const unsigned char* data, int len, opus_int16* pcm, int frame_size, int decode_fec) noexcept {
  return st == nullptr || !has_required_storage(data, len) || !has_required_storage(pcm, frame_size)
             ? OPUS_BAD_ARG
             : decode_to_output(st, data, len, pcm, frame_size, decode_fec);
}

int opus_decode(OpusDecoder* st, std::span<const unsigned char> packet, std::span<opus_int16> pcm, int decode_fec) noexcept {
  if (st == nullptr || pcm.empty() || packet.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return OPUS_BAD_ARG;
  }
  const int frame_size = interleaved_frame_size(pcm.size(), st->channels);
  const int packet_size = static_cast<int>(packet.size());
  return frame_size < 0 ? OPUS_BAD_ARG : decode_to_output(st, packet.data(), packet_size, pcm.data(), frame_size, decode_fec);
}

int opus_decode_float(OpusDecoder* st, const unsigned char* data, int len, float* pcm, int frame_size, int decode_fec) noexcept {
  return st == nullptr || !has_required_storage(data, len) || !has_required_storage(pcm, frame_size)
             ? OPUS_BAD_ARG
             : decode_to_output(st, data, len, pcm, frame_size, decode_fec);
}

[[nodiscard]] constexpr auto encoder_uses_silk(int application) noexcept -> bool {
  return application != OPUS_APPLICATION_RESTRICTED_LOWDELAY;
}

static constexpr int ref_opus_packet_get_bandwidth(const unsigned char* data) {
  if (data[0] & 0x80) {
    const int bw = 1102 + ((data[0] >> 5) & 0x3);
    return bw == 1102 ? 1101 : bw;
  }
  return (data[0] & 0x60) == 0x60 ? (data[0] & 0x10) != 0 ? 1105 : 1104 : 1101 + ((data[0] >> 5) & 0x3);
}

static constexpr int ref_opus_packet_get_nb_channels(const unsigned char* data) {
  return (data[0] & 0x4) != 0 ? 2 : 1;
}

static constexpr int ref_opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len) {
  if (len < 1) {
    return -1;
  }
  const int count = packet[0] & 0x3;
  if (count == 0) {
    return 1;
  }
  return count != 3 ? 2 : len < 2 ? -4 : packet[1] & 0x3F;
}

int opus_packet_get_nb_samples(const unsigned char* packet, int len, int Fs) noexcept {
  if (!has_required_storage(packet, len)) {
    return OPUS_BAD_ARG;
  }
  const int count = ref_opus_packet_get_nb_frames(packet, len);
  if (count < 0) {
    return count;
  }
  const auto samples = static_cast<opus_int64>(count) * ref_opus_packet_get_samples_per_frame(packet, Fs);
  return samples < opus_int32_min || samples > opus_int32_max || samples * 25 > static_cast<opus_int64>(Fs) * 3 ? -4
                                                                                                                : static_cast<int>(samples);
}

static void pitch_downsample(celt_sig* const* x, int channels, opus_val16* x_lp, int len);
static void pitch_search(const opus_val16* x_lp, opus_val16* y, int len, int max_pitch, int* pitch);
static opus_val16 remove_doubling(opus_val16* x, int N, int* T0, int prev_period, opus_val16 prev_gain);
static void xcorr_kernel_c(const opus_val16* x, const opus_val16* y, std::span<opus_val32, 4> sum, int len) {
  const auto* y0 = y;
  const auto* y1 = y + 1;
  const auto* y2 = y + 2;
  const auto* y3 = y + 3;
  for (int index = 0; index < len; ++index) {
    const auto sample = static_cast<opus_val32>(x[index]);
    sum[0] += sample * static_cast<opus_val32>(y0[index]);
    sum[1] += sample * static_cast<opus_val32>(y1[index]);
    sum[2] += sample * static_cast<opus_val32>(y2[index]);
    sum[3] += sample * static_cast<opus_val32>(y3[index]);
  }
}

static void dual_inner_prod_c(const opus_val16* x, const opus_val16* y01, const opus_val16* y02, int N, opus_val32& xy1, opus_val32& xy2) {
  auto sum_1 = opus_val32{0};
  auto sum_2 = opus_val32{0};
  for (int index = 0; index < N; ++index) {
    const auto sample = static_cast<opus_val32>(x[index]);
    sum_1 += sample * static_cast<opus_val32>(y01[index]);
    sum_2 += sample * static_cast<opus_val32>(y02[index]);
  }
  xy1 = sum_1;
  xy2 = sum_2;
}

[[nodiscard]] static auto celt_inner_prod_c(const opus_val16* x, const opus_val16* y, int N) -> opus_val32 {
  opus_val32 sum = 0;
  for (int i = 0; i < N; i++) {
    sum += x[i] * y[i];
  }
  return sum;
}

struct silk_ltp_codebook_view {
  std::span<const opus_uint8> gain_icdf, gain_bits_q5;
  std::span<const opus_int8> vq_q7;
  std::span<const opus_uint8> vq_gain_q7;
  int vq_size;
};

consteval auto make_silk_nlsf_cb(const int vectors, const int order, const int quant_step_q16, const int inv_quant_step_q6,
                                 const auto& cb1_q8, const auto& weights_q9, const auto& cb1_icdf, const auto& pred_q8, const auto& ec_sel,
                                 const auto& ec_icdf, const auto& ec_rates_q5, const auto& delta_min_q15) -> silk_NLSF_CB_struct {
  return {static_cast<opus_int16>(vectors),
          static_cast<opus_int16>(order),
          static_cast<opus_int16>(quant_step_q16),
          static_cast<opus_int16>(inv_quant_step_q6),
          cb1_q8.data(),
          weights_q9.data(),
          cb1_icdf.data(),
          pred_q8.data(),
          ec_sel.data(),
          ec_icdf.data(),
          ec_rates_q5.data(),
          delta_min_q15.data()};
}

namespace {
constexpr std::array<std::array<opus_uint8, 64 / 8>, 3> silk_gain_iCDF =
    numeric_blob_matrix<opus_uint8, 8>(R"blob(E0702C0F03020100FEEDC08446170400FFFCE29B3D0B0200)blob");
constexpr std::array<opus_uint8, 36 - -4 + 1> silk_delta_gain_iCDF =
    numeric_blob_array<opus_uint8>(R"blob(FAF5EACB47322A2623211F1D1C1B1A191817161514131211100F0E0D0C0B0A09080706050403020100)blob");
constexpr std::array<opus_uint8, 3> silk_LTP_per_index_iCDF = numeric_blob_array<opus_uint8>(R"blob(B36300)blob");
constexpr std::array<opus_uint8, 8> silk_LTP_gain_iCDF_0 = numeric_blob_array<opus_uint8>(R"blob(47382B1E150C0600)blob");
constexpr std::array<opus_uint8, 16> silk_LTP_gain_iCDF_1 = numeric_blob_array<opus_uint8>(R"blob(C7A5907C6D6054473D332A20170F0800)blob");
constexpr std::array<opus_uint8, 32> silk_LTP_gain_iCDF_2 =
    numeric_blob_array<opus_uint8>(R"blob(F1E1D3C7BBAFA4998E847B7269605850484039322C26211D1814100C09050200)blob");
constexpr std::array<opus_uint8, 8> silk_LTP_gain_BITS_Q5_0 = numeric_blob_array<opus_uint8>(R"blob(0F838A8A9B9BADAD)blob");
constexpr std::array<opus_uint8, 16> silk_LTP_gain_BITS_Q5_1 =
    numeric_blob_array<opus_uint8>(R"blob(455D7376838A8D8A96969B969BA0A6A0)blob");
constexpr std::array<opus_uint8, 32> silk_LTP_gain_BITS_Q5_2 =
    numeric_blob_array<opus_uint8>(R"blob(8380868D8D8D919191969B9B9B9BA0A0A0A0A6A6ADADB6C0B6C0C0C0CDC0CDE0)blob");
constexpr std::array<opus_int8, 8 * 5> silk_LTP_gain_vq_0 =
    numeric_blob_array<opus_int8>(R"blob(040618070500000200000C1C290DFCF70F2A190E01FE3E29F7F62541FC03FA044207F8100E26FD21)blob");
constexpr std::array<opus_int8, 16 * 5> silk_LTP_gain_vq_1 = numeric_blob_array<opus_int8>(
    R"blob(0D1627170CFF24401BFAF90A372B11010108010106F54A35F7F4374CF408FD035D1BFC1A273B03F802004D0B09F8162CFA0728091A0309F91465F90403F82A1A00F121440217FE372EFE0F03FF151029)blob");
constexpr std::array<opus_int8, 32 * 5> silk_LTP_gain_vq_2 = numeric_blob_array<opus_int8>(
    R"blob(FA1B3D2705F52A580401FE3C4106FCFFFB493801F7135E1DF7000C63060408ED662EF303020D030209EB5448EEF52E68EA081226301700F04653EB0B05F57516F8FA1775F40303F85F1C04F60F4D3CF1FF047C02FC03265418E7020D2A0D1F15FC382EFFFF234FF313F94158F7F214045131E314004B03EF05F72C5CF801FD16451FFA5F29F405274310FC0100FA7837DCF32C7A04E851050B03070200090A58)blob");
constexpr std::array<opus_uint8, 8> silk_LTP_gain_vq_0_gain = numeric_blob_array<opus_uint8>(R"blob(2E025A575D5B5262)blob");
constexpr std::array<opus_uint8, 16> silk_LTP_gain_vq_1_gain =
    numeric_blob_array<opus_uint8>(R"blob(6D78760C71737577633B576F3F6F7050)blob");
constexpr std::array<opus_uint8, 32> silk_LTP_gain_vq_2_gain =
    numeric_blob_array<opus_uint8>(R"blob(7E7C7D7C81797E17847F7F7F7E7F7A858286657677917E567C787B77AAAD6B6D)blob");
[[nodiscard]] constexpr auto silk_LTP_codebook(int index) noexcept -> silk_ltp_codebook_view {
  switch (index) {
  case 0:
    return {silk_LTP_gain_iCDF_0, silk_LTP_gain_BITS_Q5_0, silk_LTP_gain_vq_0, silk_LTP_gain_vq_0_gain,
            static_cast<int>(silk_LTP_gain_vq_0_gain.size())};
  case 1:
    return {silk_LTP_gain_iCDF_1, silk_LTP_gain_BITS_Q5_1, silk_LTP_gain_vq_1, silk_LTP_gain_vq_1_gain,
            static_cast<int>(silk_LTP_gain_vq_1_gain.size())};
  default:
    return {silk_LTP_gain_iCDF_2, silk_LTP_gain_BITS_Q5_2, silk_LTP_gain_vq_2, silk_LTP_gain_vq_2_gain,
            static_cast<int>(silk_LTP_gain_vq_2_gain.size())};
  }
}

constexpr std::array<opus_uint8, 320> silk_NLSF_CB1_NB_MB_Q8 = numeric_blob_array<opus_uint8>(
    R"blob(0C233C536C849DB4CEE40F20374D657D97AFC9E1132A42597289A2B8D1E60C193248617893ACC8DF1A2C455A72879FB4CDE10D1635506A829CB4CDE40F192C405A738EA8C4DE13183E52647891A8BED6161F324F677897AACBE3151D2D416A7C96ABC4E01E314B61798EA5BAD1E5131934465D748FA6C0DB1A223E4B617691A7C2D9192138465B718FA5C4DF15223348617591ABC4DE141D32435A7590A8C5DD161F30425F7592A8C4DE1821334D74869EB4C8E0151C46576A7C95AAC2D91A213540537598ADCCE11B22415F6C819BAED2E1141A486371839AB0C8DB222B3D4E5D729BB1CDE5171D36617C8AA3B3D1E51E26385976819EB2C8E7151D313F556F8EA3C1DE1B304D67859EB3C4D7E81D2F4A637C97B0C6DCED212A3D4C5D799BAECFE11D355770889AAABCD0E3181E34548396A6BACBE52530405468769CB1C9E6)blob");
constexpr std::array<opus_int16, 320> silk_NLSF_CB1_Wght_Q9 = numeric_blob_array<opus_int16>(
    R"blob(0B51090A090A090A08EF08EF090A08FC091708EF0B480A14095A093F090A08E208E208E208E2089209B709240924090A090A090A09240924093F09320C900ACE09240924090A08E208AD089F08D50892099C09AA093F095A095A095A095A093F0967090A0D970BF0084F089F08E208E208E208EF090A08D50CD20C450A14095A08C708AD089F08920892084210000F0508AD0A3C0A3C0967090A095A093F081A0C6A0CAC093F08AD09F909820924090A087708AD0D0A0DA00AA6089208D5099C0932093F089F0835093209740917093F095A097409740974099C093F0EC30E2D098209DF093F08E208E208FC089F08000CB60C990A990B1E098F091708FC08FC08E2084F0CBF0CE40AC10AF6098F08D508D508C7084F08350B390BA50A49093F09670932089208C708C708420C990C7D0A490A1408E2088508C708AD08AD085D0C6A0CEE0AB4096708E208E208E208EF089208420C450CC8099C080D08EF09C4093F09B7098208850DB30CD2090A0A8C0A5709AA093F095A0924084F0D5F0DCF0BDE0BF008FC079E08AD08E208E208E20D4C0D2608270A7F0B390932097408E209AA09EC0EB00DA0079E0A640B5109DF095A093F099C08D50BD40CC80AB40B480AB4086A084F08EF08BA08C70E6F0E4907E907B10A640A8C0A1409C40917093F0C870D550932081A0B480B48092409B708C708770D0A0D260B1E0ADC0917086A08E208EF0842080D091708FC088508770885093F0A490A8C0A8C09F90967098208AD08D508AD08AD092409740A2F0A8C0BDE0CAC0AF60B4809AA081A08FC090A0932094C08AD086A084F08EF09C40AE90AE90A3C0A14093F0E5C0E8108BA072E08850AC10AA60A7109D1089F0AE90C580AA609F90B1E09D10885095A08AD0885)blob");
constexpr std::array<opus_uint8, 64> silk_NLSF_CB1_iCDF_NB_MB = numeric_blob_array<opus_uint8>(
    R"blob(D4B294816C6055524F4D3D3B39383331302D2A29282624221F1E150C0A030100FFF5F4ECE9E1D9CBBEB0AFA195887D72665B51473C342B231C1413120C0B0500)blob");
constexpr std::array<opus_uint8, 160> silk_NLSF_CB2_SELECT_NB_MB = numeric_blob_array<opus_uint8>(
    R"blob(10000000006342242422242222222253452434227466464444B0664444224155445424748D988BAA84BBB8D88984F9A8B98B6866644444B2DAB9B9AAF4D8BBBBAAF4BBBBDB8A679BB8B98974B79B988884D9B8B8AAA4D9AB9B8BF4A9B8B9AAA4D8DFDA8AD68FBCDAA8F48D889BAAA88ADCDB8BA4DBCAD889A8BAF6B98B74B9DBB98A64648664662244446444A8CBDDDAA8A79A886846A4F6AB898B899BDADB8B)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_iCDF_NB_MB = numeric_blob_array<opus_uint8>(
    R"blob(FFFEFDEE0E03020100FFFEFCDA2303020100FFFEFAD03B04020100FFFEF6C2470A020100FFFCECB75208020100FFFCEBB45A11020100FFF8E0AB611E040100FFFEECAD5F25070100)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_BITS_NB_MB_Q5 = numeric_blob_array<opus_uint8>(
    R"blob(FFFFFF830691FFFFFFFFFFEC5D0F60FFFFFFFFFFC2531947DDFFFFFFFFA2492242A2FFFFFFD27E492B39ADFFFFFFC97D47303A82FFFFFFA66E49393E68D2FFFFFB7B41374464ABFF)blob");
constexpr std::array<opus_uint8, 18> silk_NLSF_PRED_NB_MB_Q8 =
    numeric_blob_array<opus_uint8>(R"blob(B38A8C9497959997A37443523B5C4864595C)blob");
constexpr std::array<opus_int16, 11> silk_NLSF_DELTA_MIN_NB_MB_Q15 =
    numeric_blob_array<opus_int16>(R"blob(00FA00030006000300030003000400030003000301CD)blob");
constinit const silk_NLSF_CB_struct silk_NLSF_CB_NB_MB =
    make_silk_nlsf_cb(32, 10, fixed_q<16>(0.18), fixed_q<6>(1.0 / 0.18), silk_NLSF_CB1_NB_MB_Q8, silk_NLSF_CB1_Wght_Q9,
                      silk_NLSF_CB1_iCDF_NB_MB, silk_NLSF_PRED_NB_MB_Q8, silk_NLSF_CB2_SELECT_NB_MB, silk_NLSF_CB2_iCDF_NB_MB,
                      silk_NLSF_CB2_BITS_NB_MB_Q5, silk_NLSF_DELTA_MIN_NB_MB_Q15);
constexpr std::array<opus_uint8, 512> silk_NLSF_CB1_WB_Q8 = numeric_blob_array<opus_uint8>(
    R"blob(07172636455564748393A2B2C1D0DFEF0D192937455362707F8E9DABBBCBDCEC0F1522333D4E5C6A7E8898A7B9CDE1F00A1524323F4F5F6E7E8D9DADBDCDDDED111425333B4E596B7B8696A4B8CDE0F00A0F203343516070818E9EADBDCCDCEC08152533414F62717E8A9BA8B3C0D1DA0C0F22373F4E576C768394A7B9CBDBEC10132024384F5B6C76889AABBACCDCED0B1C2B3A4A5969788796A5B4C4D3E2F10610212E3C4B5C6B7B899CA9B9C7D6E10B131E2C394A5969798798A9BACADAEA0C131D2E39475864788494A5B6C7D8E91117232E384D5C6A7B8698A7B9CCDEED0E112D353F4B596B738497ABBCCEDDF009101D283847586777899AABBDCDDEED10132430394C5769768496A7B9CADAEC0C111D3647515E687E8895A4B6C9DDED0F1C2F3E4F6173818E9BA8B4C2D0DFEE080E1E2D3E4E5E6F7F8F9FAFC0CFDFEF111E313E4F5C6B778491A0AEBECCDCEB0E13242D3D4C5B6C798A9AACBDCDDEEE0C121F2D3C4C5B6B7B8A9AABBBCCDDEC0D111F2B35465367728395A7B9CBDCED1116232A3A4E5D6E7D8B9BAABCCEE0F0080F2232435363738392A2B2C1D1E0EF0D10294249565F6F808996A3B7CEE1F1111925343F4B5C66778490A0AFBFD4E7131F31415364758593A1AEBBC8D5E3F2121F34445867757E8A95A3B1C0CFDFEF101D2F3D4C5A6A778593A1B0C1D1E0F00F1523323D4956616E77818DAFC6DAED)blob");
constexpr std::array<opus_int16, 512> silk_NLSF_CB1_WB_Wght_Q9 = numeric_blob_array<opus_int16>(
    R"blob(0E490B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B6D0B930B930B6D0B1E0C900C0D0B9C0BF00BF00BC20BC20BC20B930B930BC20B9C0B480B1E0B1E0AA60F500FAE0BA50C870C870B760BF00B1E0C320CAC0B6D0B1E0A3C09F90ADC0B6D0DBC0C7D0BC20C1F0BCB0B480B6D0B6D0B6D0B6D0B480B480B480B480B480AC113BE13BE0B760DF50D390BF00C0D0AE90C580C580B9C0B1E09D109EC0AC10B48114C10350A8C0AC10B9C0BC20B6D0B1E0BA50BCB0B6D0B6D0B6D0B6D0B480AA60E240BCB0B9C0BF00BF00B390AF60BF00C900BE70BA50CDB0CDB0BA50CEE0BAF146B139609EC0D0A0DC60D390C7D0C160D300BA50A8C0A570A7F0AE90B1E0A7113D914361207114C099C0B510BE70C870C610A7F0AB40B480B1E0AE90B1E0A8C0C320B480B930B6D0B6D0B6D0B6D0B930B930B930B930B6D0B6D0B930B930B93106A0C870BA50C1F0BC20B480B480B6D0B9C0B390B640BCB0B9C0BC20C7D0B390EB00EB00CAC0C1F0BA50B480B6D0B480B9C0B760AE90AE90B1E0B480B480A640F0E0FAE0C870C320CAC0B760BE70B930B930C0D0B1E0AE90AE90AE90AE90A140F050FF00D1D0DBC0C160AB40BC20B760C320C0D0B1E0B1E0A570A570B1E0AF6141B131E0C990F050D710C610B510D550D7B0A8C0A140A710AB40B1E0AF60AC1100D0ECD0CDB0C580B6D0B480B480B6D0AE90AB40AE90AB40AE90B1E0B480AF613D913BE0BE70DD90CAC0BF00C0D0B800C1F0B510AB40AB40AB40B1E0AE90A3C10D510D50B2C09DF0C870D300D300C030C030D300BF00B1E0A570A140AA60AC10BF00B640AF60B480AB40A7F0B510C1F0C4E0C4E0C900C610BF00BC20B930B1E11170F2A0B6D0B480B1E0B480B1E0B1E0B480B480B480B1E0B480B6D0B480B1E0BA50B640B640BA50BA50BF00C320C900C4E0BF00BC20B9C0B9C0B9C0B6D0AB4108510350CEE0D130B6D0B930B480BA50BA50B1E0AE90AB40B1E0B1E0B1E0AE90FF00FAE0C1F0BC20B6D0B6D0B6D0B480B6D0B6D0B1E0B1E0B1E0AE90B480ADC120711DF0C610D710C870BA50B510BDE0C320AB40A7F0A7F0A7F0AB40AE90A8C103510AD0ECD0E490AA60ADC0B480B480BC20B9C0B6D0B1E0A7F0A7F0AE90B4810770DE20AC10B1E0B1E0B480B480B480B6D0B6D0B480B6D0B6D0B6D0B930B481436133908D50D680ECD0D970D130B1E0CEE0D970C4E0B51099C09B70AC10B6D0D7B0E650C320C7D0D1D0BE70C870C870BA50C900C0D0B6D0B6D0A7F09EC09820BA50BC20AE90AE90AB40AE90B1E0B9C0BF00C1F0C4E0C4E0C4E0C1F0BC20BC20B800B390A7F0AA60ADC0BC20D680DD90D1D0CAC0BF00BC20B930B6D0B480B1E0BCB0B800B510BC20BC20B9C0BCB0C1F0BF00BF00BC20B480B1E0B6D0B6D0B480F500F7F0BC20C7D0D1D0C900CDB0CDB0D970E780D710AA60885099C0A140A2F)blob");
constexpr std::array<opus_uint8, 64> silk_NLSF_CB1_iCDF_WB = numeric_blob_array<opus_uint8>(
    R"blob(E1CCC9B8B7AF9E9A99877773716E6D63625F4F443432302D2B201F1B120A0300FFFBEBE6D4C9C4B6A7A6A3978A7C6E685A4E4C4645392D2218150B0605040300)blob");
constexpr std::array<opus_uint8, 256> silk_NLSF_CB2_SELECT_WB = numeric_blob_array<opus_uint8>(
    R"blob(00000000000000016466664444242260A46B9EB9B4B98B664042242222000120D08B8DBF98B99B6860AB68A6666666840100000000101000506D4E6BB98B6765D0D48D8BAD997B672400000000000001300000000000002044877B7777674562446778767666476286889DB8B6998B86D0A8F84BBD8F796B2031222222001102D2EB8B7BB9896986628768B664B7AB86644644464242228340A666442402010086A6664422224284D4F69E8B6B6B576664DB7D7A8976678472878969AB6A3222A4D68D8FB9977967C022000000000001D06D4ABB86F99F89666E9A7657657765000200242442442360A4666424000221A78AAE6664540202646B787724C51800)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_iCDF_WB = numeric_blob_array<opus_uint8>(
    R"blob(FFFEFDF40C03020100FFFEFCE02603020100FFFEFBD13904020100FFFEF4C34504020100FFFBE8B85407020100FFFEF0BA560E020100FFFEEFB25B1E050100FFF8E3B16413020100)blob");
constexpr std::array<opus_uint8, 72> silk_NLSF_CB2_BITS_WB_Q5 = numeric_blob_array<opus_uint8>(
    R"blob(FFFFFF9C049AFFFFFFFFFFE3660F5CFFFFFFFFFFD5531848ECFFFFFFFF964C213FD6FFFFFFBE794D2B37B9FFFFFFF589472B3B8BFFFFFFFF834232426BC2FFFFA6744C37357DFFFF)blob");
constexpr std::array<opus_uint8, 30> silk_NLSF_PRED_WB_Q8 =
    numeric_blob_array<opus_uint8>(R"blob(AF94A0B0B2ADAEA4B1AEC4B6C6C0B6443E423C4875555A7688978EA08E9B)blob");
constexpr std::array<opus_int16, 17> silk_NLSF_DELTA_MIN_WB_Q15 =
    numeric_blob_array<opus_int16>(R"blob(0064000300280003000300030005000E000E000A000B00030008000900070003015B)blob");
constinit const silk_NLSF_CB_struct silk_NLSF_CB_WB = make_silk_nlsf_cb(
    32, 16, fixed_q<16>(0.15), fixed_q<6>(1.0 / 0.15), silk_NLSF_CB1_WB_Q8, silk_NLSF_CB1_WB_Wght_Q9, silk_NLSF_CB1_iCDF_WB,
    silk_NLSF_PRED_WB_Q8, silk_NLSF_CB2_SELECT_WB, silk_NLSF_CB2_iCDF_WB, silk_NLSF_CB2_BITS_WB_Q5, silk_NLSF_DELTA_MIN_WB_Q15);
constexpr std::array<opus_int16, 16> silk_stereo_pred_quant_Q13 =
    numeric_blob_array<opus_int16>(R"blob(CA5CD8BEDFB6E29AE69CEC78F47AFCCC03340B86138819641D66204A274235A4)blob");
constexpr std::array<opus_uint8, 25> silk_stereo_pred_joint_iCDF =
    numeric_blob_array<opus_uint8>(R"blob(F9F7F6F5F4EAD2CAC9C8C5AE523B3837362E160C0B0A090700)blob");
constexpr std::array<opus_uint8, 2> silk_stereo_only_code_mid_iCDF = numeric_blob_array<opus_uint8>(R"blob(4000)blob");
constexpr std::array<opus_uint8, 3> silk_LBRR_flags_2_iCDF = numeric_blob_array<opus_uint8>(R"blob(CB9600)blob");
constexpr std::array<opus_uint8, 7> silk_LBRR_flags_3_iCDF = numeric_blob_array<opus_uint8>(R"blob(D7C3A67D6E5200)blob");
constexpr std::array<opus_uint8, 2> silk_lsb_iCDF = numeric_blob_array<opus_uint8>(R"blob(7800)blob");
constexpr std::array<opus_uint8, 3> silk_LTPscale_iCDF = numeric_blob_array<opus_uint8>(R"blob(804000)blob");
constexpr std::array<opus_uint8, 4> silk_type_offset_VAD_iCDF = numeric_blob_array<opus_uint8>(R"blob(E89E0A00)blob");
constexpr std::array<opus_uint8, 2> silk_type_offset_no_VAD_iCDF = numeric_blob_array<opus_uint8>(R"blob(E600)blob");
constexpr std::array<opus_uint8, 5> silk_NLSF_interpolation_factor_iCDF = numeric_blob_array<opus_uint8>(R"blob(F3DDC0B500)blob");
constexpr std::array<std::array<opus_uint8, 2>, 2> silk_Quantization_Offsets_Q10{{{100, 240}, {32, 100}}};
constexpr std::array<opus_int16, 3> silk_LTPScales_table_Q14{15565, 12288, 8192};
constexpr std::array<opus_uint8, 3> silk_uniform3_iCDF{171, 85, 0};
constexpr std::array<opus_uint8, 4> silk_uniform4_iCDF{192, 128, 64, 0};
constexpr std::array<opus_uint8, 5> silk_uniform5_iCDF{205, 154, 102, 51, 0};
constexpr std::array<opus_uint8, 6> silk_uniform6_iCDF{213, 171, 128, 85, 43, 0};
constexpr std::array<opus_uint8, 8> silk_uniform8_iCDF{224, 192, 160, 128, 96, 64, 32, 0};
constexpr std::array<opus_uint8, 7> silk_NLSF_EXT_iCDF = numeric_blob_array<opus_uint8>(R"blob(64281007030100)blob");
constexpr std::array<std::array<opus_int32, 3>, 5> silk_Transition_LP_B_Q28 = numeric_blob_matrix<opus_int32, 3>(
    R"blob(0EF2670A1DE4CD560EF2670A0C82527519049A590C8252750A311146146203ED0A31114607D702DA0FADC6F907D702DA0552B6220AA4FADA0552B622)blob");
constexpr std::array<std::array<opus_int32, 2>, 5> silk_Transition_LP_A_Q28 =
    numeric_blob_matrix<opus_int32, 2>(R"blob(1E2EF3460E4BE32B1880661F0A1D2C1C124861DA06F49CED0B1330EC04A590E3021DA4ED036BDF0A)blob");
constexpr std::array<opus_uint8, 2 * (18 - 2)> silk_pitch_lag_iCDF =
    numeric_blob_array<opus_uint8>(R"blob(FDFAF4E9D4B69683786E6255483C31282019130F0D0B09080706050403020100)blob");
constexpr std::array<opus_uint8, 21> silk_pitch_delta_iCDF =
    numeric_blob_array<opus_uint8>(R"blob(D2D0CECBC7C1B7A88E684A34251B140E0A06040200)blob");
constexpr std::array<opus_uint8, 34> silk_pitch_contour_iCDF =
    numeric_blob_array<opus_uint8>(R"blob(DFC9B7A7988A7C6F62584F463E38322C27231F1B181512100E0C0A08060403020100)blob");
constexpr std::array<opus_uint8, 11> silk_pitch_contour_NB_iCDF = numeric_blob_array<opus_uint8>(R"blob(BCB09B8A7761432B1A0A00)blob");
constexpr std::array<opus_uint8, 12> silk_pitch_contour_10_ms_iCDF = numeric_blob_array<opus_uint8>(R"blob(A577503D2F231B140E090400)blob");
constexpr std::array<opus_uint8, 3> silk_pitch_contour_10_ms_NB_iCDF = numeric_blob_array<opus_uint8>(R"blob(713F00)blob");
constexpr std::array<opus_uint8, 4> silk_max_pulses_table{8, 10, 12, 16};
constexpr std::array<std::array<opus_uint8, 18>, 10> silk_pulses_per_block_iCDF = numeric_blob_matrix<opus_uint8, 18>(
    R"blob(7D331A120F0C0B0A09080706050403020100C6692D160F0C0B0A09080706050403020100D5A274533B2B2018120F0C09070605030200EFBB743B1C100B0A09080706050403020100FAE5BC8756331E130D0A0806050403020100F9EBD5B99C80675342352A211A15110D0A00FEF9EBCEA4764D2E1B100A07050403020100FFFDF9EFDCBF9C77553925170F0A06040200FFFDFBF6EDDFCBB3987C624B37281D150F00FFFEFDF7DCA26A432A1C120C090604030200)blob");
constexpr std::array<std::array<opus_uint8, 18>, 9> silk_pulses_per_block_BITS_Q5 = numeric_blob_matrix<opus_uint8, 18>(
    R"blob(1F396BA0CDCDFFFFFFFFFFFFFFFFFFFFFFFF452F436FA6CDFFFFFFFFFFFFFFFFFFFFFFFF524A4F5F6D8091A0ADCDCDCDE0FFFFE0FFE07D4A3B45618DB6FFFFFFFFFFFFFFFFFFFFFFAD7355494C5C7391ADCDE0E0FFFFFFFFFFFFA686716665666B767D8A919BA6B6C0C0CD96E0B68665534F55617891ADCDE0FFFFFFFFFFFFE0C09678655C595D667686A0B6C0E0E0E0FFE0E0B69B86766D68666A6F768391A0AD83)blob");
constexpr std::array<std::array<opus_uint8, 9>, 2> silk_rate_levels_iCDF =
    numeric_blob_matrix<opus_uint8, 9>(R"blob(F1BEB284574A290E00DFC19D8C6A39271200)blob");
constexpr std::array<std::array<opus_uint8, 9>, 2> silk_rate_levels_BITS_Q5 =
    numeric_blob_matrix<opus_uint8, 9>(R"blob(834A8D4F508A5F68865F635B7D5D4C7B737B)blob");
constexpr std::array<opus_uint8, 152> silk_shell_code_table0 = numeric_blob_array<opus_uint8>(
    R"blob(8000D62A00EB801500F4B8480B00F8D6802A0700F8E1AA50190500FBECC67E36120300FAEED39F52230F0500FAE7CBA8805835190600FCEED8B9946C4728120400FDF3E1C7A6805A391F0D0300FEF6E9D4B7936D492C170A0200FFFAF0DFC6A6805A3A2110060100FFFBF4E7D2B5926E4B2E190C050100FFFDF8EEDDC4A4805C3C231208030100FFFDF9F2E5D0B4926E4C301B0E07030100)blob");
constexpr std::array<opus_uint8, 152> silk_shell_code_table1 = numeric_blob_array<opus_uint8>(
    R"blob(8100CF3200EC811400F5B9480A00F9D5812A0600FAE2A9571B0400FBE9C2823E140400FAECCFA0632F110300FFF0D9B68351290B0100FFFEE9C99F6B3D14020100FFF9E9CEAA80563217070100FFFAEED9BA946C462712060100FFFCF3E2C8A6805A381E0D040100FFFCF5E7D1B4926E4C2F190B040100FFFDF8EDDBC2A3805D3E251308030100FFFEFAF1E2CDB1916F4F331E0F06020100)blob");
constexpr std::array<opus_uint8, 152> silk_shell_code_table2 = numeric_blob_array<opus_uint8>(
    R"blob(8100CB3600EA811700F5B8490A00FAD781290500FCE8AD56180300FDF0C881380F0200FDF4D9A45E260A0100FDF5E2BD84471B070100FDF6E7CB9F693817060100FFF8EBD5B385552F13050100FFFEF3DDC29F7546250C020100FFFEF8EAD0AB8055301608020100FFFEFAF0DCBD956B43241006020100FFFEFBF3E3C9A6805A371D0D05020100FFFEFCF6EAD5B7936D492B160A04020100)blob");
constinit const std::array<opus_uint8, 152> silk_shell_code_table3 = numeric_blob_array<opus_uint8>(
    R"blob(8200C83A00E7821A00F4B84C0C00F9D6822B0600FCE8AD57180300FDF1CB83380E0200FEF6DDA75E23080100FEF9E8C1824117050100FFFBEFD3A2632D0F040100FFFBF3DFBA834A210B030100FFFCF5E6CA9E69391808020100FFFDF7EBD6B384542C1307020100FFFEFAF0DFC49F7045240F06020100FFFEFDF5E7D1B0885D371B0B03020100FFFEFDFCEFDDC29E754C2A120403020100)blob");
constinit const std::array<opus_uint8, 17> silk_shell_code_table_offsets =
    numeric_blob_array<opus_uint8>(R"blob(00000205090E141B232C36414D5A687787)blob");
constinit const std::array<opus_uint8, 42> silk_sign_iCDF =
    numeric_blob_array<opus_uint8>(R"blob(FE31434D525D63C60B12181F242DFF2E424E575E68D00E15202A3342FF5E686D707376F8354550585F66)blob");
constinit const std::array<opus_int16, 128 + 1> silk_LSFCosTab_FIX_Q12 = numeric_blob_array<opus_int16>(
    R"blob(20001FFE1FF61FEA1FD81FC21FA81F881F621F3A1F0A1ED81EA01E621E221DDC1D901D421CEE1C961C3A1BD81B721B0A1A9C1A2A19B4193A18BC183C17B6172E16A01610157E14E8144E13B01310126E11C8111E10740FC60F160E640DAE0CF80C400B840AC80A0A094A088A07C60702063E057804B203EA0322025A019200CA0000FF36FE6EFDA6FCDEFC16FB4EFA88F9C2F8FEF83AF776F6B6F5F6F538F47CF3C0F308F252F19CF0EAF03AEF8CEEE2EE38ED92ECF0EC50EBB2EB18EA82E9F0E960E8D2E84AE7C4E744E6C6E64CE5D6E564E4F6E48EE428E3C6E36AE312E2BEE270E224E1DEE19EE160E128E0F6E0C6E09EE078E058E03EE028E016E00AE002E000)blob");
} // namespace

[[nodiscard]] constexpr auto silk_nlsf_codebook_for_fs(const int fs_kHz) noexcept -> const silk_NLSF_CB_struct* {
  return fs_kHz == 8 || fs_kHz == 12 ? &silk_NLSF_CB_NB_MB : &silk_NLSF_CB_WB;
}

[[nodiscard]] constexpr auto silk_nlsf_cb1_icdf(const silk_NLSF_CB_struct* cb, const int signalType) noexcept -> const opus_uint8* {
  return cb->CB1_iCDF + (signalType >> 1) * cb->nVectors;
}

static void silk_PLC_Reset(silk_decoder_state* psDec);
static void silk_PLC(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl, std::span<opus_int16> frame, int lost);
static void silk_PLC_glue_frames(silk_decoder_state* psDec, std::span<opus_int16> frame);
static void silk_stereo_MS_to_LR(stereo_dec_state* state, opus_int16 x1[], opus_int16 x2[], const opus_int32 pred_Q13[], int fs_kHz,
                                 int frame_length);
static void silk_stereo_LR_to_MS(stereo_enc_state* state, opus_int16 x1[], opus_int16 x2[], silk_stereo_pred_indices& ix,
                                 opus_uint8* mid_only_flag, opus_int32 mid_side_rates_bps[], opus_int32 total_rate_bps,
                                 int prev_speech_act_Q8, int toMono, int preserve_stereo, int fs_kHz, int frame_length);
static inline void silk_stereo_encode_pred(ec_enc* psRangeEnc, const silk_stereo_pred_indices& ix);
static void silk_stereo_encode_mid_only(ec_enc* psRangeEnc, opus_int8 mid_only_flag);
static void silk_stereo_decode_pred(ec_dec* psRangeDec, std::span<opus_int32, 2> pred_Q13);
static void silk_stereo_decode_mid_only(ec_dec* psRangeDec, int& decode_only_mid);
template <bool Encode, typename Pulse> static void silk_shell_code_node(ec_ctx* coder, std::span<Pulse> pulses, int total_pulses);
template <bool Encode, typename Pulse>
static void silk_process_pulses(ec_ctx* coder, std::span<Pulse> pulses, int signal_type, int quant_offset_type, int frame_length);
static void silk_gains_dequant(opus_int32 gain_Q16[4], const opus_int8 ind[4], opus_int8* prev_ind, const int conditional,
                               const int nb_subfr);
static void silk_VQ_WMat_EC_c(opus_uint8* ind, opus_int32* res_nrg_Q15, opus_int32* rate_dist_Q8, int* gain_Q7, const opus_int32* XX_Q17,
                              const opus_int32* xX_Q17, const opus_int8* cb_Q7, const opus_uint8* cb_gain_Q7, const opus_uint8* cl_Q5,
                              const int subfr_len, const opus_int32 max_gain_Q7, const int L);
static void silk_NLSF_VQ(opus_int32 err_Q26[], const opus_int16 in_Q15[], const opus_uint8 pCB_Q8[], const opus_int16 pWght_Q9[],
                         const int K, const int LPC_order);
static void silk_NLSF_unpack(std::span<opus_int16, 16> ec_ix, std::span<opus_uint8, 16> pred_Q8, const silk_NLSF_CB_struct* psNLSF_CB,
                             const int CB1_index);
static void silk_NLSF_decode(std::span<opus_int16, 16> pNLSF_Q15, std::span<opus_int8, 17> NLSFIndices,
                             const silk_NLSF_CB_struct* psNLSF_CB);
static opus_int32 silk_NLSF_del_dec_quant(std::span<opus_int8, 16> indices, std::span<const opus_int16, 16> x_Q10,
                                          std::span<const opus_int16, 16> w_Q5, std::span<const opus_uint8, 16> pred_coef_Q8,
                                          std::span<const opus_int16, 16> ec_ix, const opus_uint8 ec_rates_Q5[],
                                          const int quant_step_size_Q16, const opus_int16 inv_quant_step_size_Q6, const opus_int32 mu_Q20,
                                          const opus_int16 order);
static void silk_decode_indices(silk_decoder_state* psDec, ec_dec* psRangeDec, int FrameIndex, int decode_LBRR, int condCoding);
static void silk_decode_parameters(silk_decoder_state& state, silk_decoder_control& control, int condCoding);
static opus_int32 silk_lin2log(const opus_int32 inLin);
static opus_int32 silk_log2lin(const opus_int32 inLog_Q7);
static void silk_LPC_analysis_filter(opus_int16* out, const opus_int16* in, const opus_int16* B, opus_int32 len, opus_int32 d);
static void silk_control_SNR(silk_encoder_state* psEncC, opus_int32 TargetRate_bps);
struct silk_shape_state_FLP {
  opus_int8 LastGainIndex;
  float HarmShapeGain_smth, Tilt_smth;
};
struct silk_encoder_state_FLP {
  silk_encoder_state sCmn;
  silk_shape_state_FLP sShape;
  float x_buf[2 * ((5 * 4) * 16) + (5 * 16)], LTPCorr;
};
struct silk_encoder_control_FLP {
  float Gains[4], PredCoef[2][16], LTPCoef[5 * 4], AR[4 * 24], LF_MA_shp[4], LF_AR_shp[4], Tilt[4], HarmShapeGain[4], Lambda, input_quality,
      coding_quality, predGain, LTPredCodGain, ResNrg[4];
  int pitchL[4];
  opus_int32 GainsUnq_Q16[4];
  int lastGainIndexPrev;
};
struct silk_encoder {
  stereo_enc_state sStereo;
  silk_lbrr_state* lbrr;
  opus_int32 nBitsExceeded;
  int nChannelsAPI, nChannelsInternal, nPrevChannelsInternal, timeSinceSwitchAllowed_ms, allowBandwidthSwitch, prev_decode_only_middle;
};

[[nodiscard]] static inline auto silk_encoder_channel_states(silk_encoder* encoder) noexcept -> silk_encoder_state_FLP* {
  return offset_ptr<silk_encoder_state_FLP>(encoder, align(sizeof(silk_encoder)));
}
struct StereoWidthState {
  opus_val32 XX, XY, YY;
  opus_val16 smoothed_width;
  opus_val16 max_follower;
};
struct OpusEncoder {
  opus_uint16 celt_enc_offset, silk_enc_offset;
  silk_EncControlStruct silk_mode;
  int application, channels, delay_compensation;
  opus_int32 Fs;
  int use_vbr, vbr_constraint;
  opus_int32 user_bitrate_bps;
  int encoder_buffer, use_dtx;
  opus_int32 bitrate_bps;
  opus_int32 vbr_budget_reservoir_bits;
  opus_int16 stream_channels;
  opus_int16 hybrid_stereo_width_Q14;
  opus_uint16 vbr_target_remainder;
  opus_int32 variable_HP_smth2_Q15;
  opus_val16 prev_HB_gain;
  opus_val32 hp_mem[4];
  opus_val32 audio_speech_hp_mem[4], audio_music_hp_mem[4];
  int mode, prev_mode, prev_channels, prev_framesize, bandwidth, auto_bandwidth, silk_bw_switch, lightweight_voice_score_Q7,
      lightweight_music_score_Q7, lightweight_vad_score_Q7, lightweight_analysis_frames;
  int lightweight_harmonic_music_Q7, lightweight_high_z_tonal_Q7, audio_preprocess_mode, audio_preprocess_hold;
  int preprocess_filter_state;
  StereoWidthState width_mem;
  opus_val32 peak_signal_energy;
  opus_uint32 rangeFinal;
  int nb_no_activity_ms_Q1;
  opus_val32 dtx_smoothed_energy;
  int voip_noise_confidence_Q7;
};
static_assert(std::is_standard_layout_v<OpusEncoder>);

[[nodiscard]] static auto next_vbr_target_bits(OpusEncoder* st, int frame_size) noexcept -> opus_int32 {
  const auto numerator = static_cast<opus_int64>(st->bitrate_bps) * frame_size + st->vbr_target_remainder;
  st->vbr_target_remainder = static_cast<opus_uint16>(numerator % st->Fs);
  return static_cast<opus_int32>(numerator / st->Fs);
}

static void reset_vbr_budget(OpusEncoder* st) noexcept {
  st->vbr_budget_reservoir_bits = 0;
  st->vbr_target_remainder = 0;
}

[[nodiscard]] static constexpr auto encoder_delay_buffer_count(int channels, int application) noexcept -> std::size_t {
  return encoder_uses_silk(application) ? static_cast<std::size_t>(480 * channels) : 0;
}

[[nodiscard]] static inline auto encoder_delay_buffer(OpusEncoder* st) noexcept -> opus_res* {
  return offset_ptr<opus_res>(st, align(sizeof(OpusEncoder)));
}

[[nodiscard]] static inline auto encoder_delay_buffer(const OpusEncoder* st) noexcept -> const opus_res* {
  return offset_ptr<opus_res>(st, align(sizeof(OpusEncoder)));
}

[[nodiscard]] static inline auto encoder_silk_state(OpusEncoder* st) noexcept -> void* {
  return offset_ptr<void>(st, st->silk_enc_offset);
}

[[nodiscard]] static inline auto encoder_celt_state(OpusEncoder* st) noexcept -> CeltEncoderInternal* {
  return offset_ptr<CeltEncoderInternal>(st, st->celt_enc_offset);
}

[[nodiscard]] static auto ensure_encoder_lbrr_state(OpusEncoder* st) noexcept -> bool {
  if (!encoder_uses_silk(st->application)) {
    return true;
  }
  auto* silk_enc = static_cast<silk_encoder*>(encoder_silk_state(st));
  if (silk_enc->lbrr == nullptr) {
    silk_enc->lbrr = static_cast<silk_lbrr_state*>(std::calloc(1, sizeof(silk_lbrr_state)));
  }
  return silk_enc->lbrr != nullptr;
}

static void reset_encoder_silk_state(OpusEncoder* st) {
  auto* silk_enc = static_cast<silk_encoder*>(encoder_silk_state(st));
  auto* lbrr = silk_enc->lbrr;
  silk_InitEncoder(silk_enc, st->channels);
  silk_enc->lbrr = lbrr;
  if (lbrr != nullptr) {
    zero_object(*lbrr);
  }
}

static void release_encoder_silk_state(OpusEncoder* st) noexcept {
  if (st != nullptr && encoder_uses_silk(st->application)) {
    auto* silk_enc = static_cast<silk_encoder*>(encoder_silk_state(st));
    std::free(silk_enc->lbrr);
    silk_enc->lbrr = nullptr;
  }
}

constexpr std::array<opus_uint16, 8> voice_bandwidth_thresholds_common{9000, 700, 9000, 700, 13500, 1000, 14000, 2000};
constexpr std::array<opus_uint16, 8> music_bandwidth_thresholds_common{9000, 700, 9000, 700, 11000, 1000, 12000, 2000};
constexpr opus_int32 stereo_voice_threshold = 19000;
constexpr opus_int32 stereo_music_threshold = 17000;
constexpr opus_int32 mono_voice_mode_threshold = 64000;
constexpr opus_int32 stereo_voice_mode_threshold = 44000;
constexpr opus_int32 music_mode_threshold = 8000;
constexpr int audio_preprocess_music = 0;
constexpr int audio_preprocess_speech = 1;
constexpr int preprocess_lowrate_voip_celt = 2;
constexpr int preprocess_lowrate_voip_continuous = 3;
constexpr int audio_preprocess_warmup_frames = 12;
constexpr int audio_preprocess_hold_frames = 50;
constexpr int fec_mode_settle_frames = 4;
constexpr int voip_mode_probe_frames = 12;
constexpr int voip_mode_min_voiced_frames = 3;
constexpr int voip_mode_max_voiced_frames = 8;
constexpr int stereo_preservation_probe_frames = 8;
constexpr int quiet_tonal_bypass_hold_frames = 250;
constexpr int tonal_confirmation_frames = 3;
constexpr int preprocess_filter_general_audio = -2;
constexpr int preprocess_filter_stable_tonal = -1;
constexpr int preprocess_filter_default = 1;
constexpr int preprocess_filter_quiet_voice = 2;
constexpr int silk_preserve_stereo_bias = 1;
constexpr int silk_preserve_stereo_force = 2;
constexpr opus_val32 quiet_voice_probe_energy = .02f;
static void ref_opus_encoder_init(OpusEncoder* st, opus_int32 Fs, int channels, int application) {
  void* silk_enc = nullptr;
  CeltEncoderInternal* celt_enc = nullptr;
  auto silkEncSizeBytes = align(silk_encoder_get_size(channels));
  if (!encoder_uses_silk(application)) {
    silkEncSizeBytes = 0;
  }
  const auto base_size =
      align(sizeof(OpusEncoder)) + align(static_cast<int>(encoder_delay_buffer_count(channels, application) * sizeof(opus_res)));
  *st = {};
  std::uninitialized_default_construct_n(encoder_delay_buffer(st), encoder_delay_buffer_count(channels, application));
  zero_n_items(encoder_delay_buffer(st), encoder_delay_buffer_count(channels, application));
  st->silk_enc_offset = base_size;
  st->celt_enc_offset = st->silk_enc_offset + silkEncSizeBytes;
  st->stream_channels = st->channels = channels;
  st->Fs = Fs;
  if (encoder_uses_silk(application)) {
    silk_enc = encoder_silk_state(st);
    silk_InitEncoder(silk_enc, st->channels);
  }
  st->silk_mode.API_sampleRate = st->Fs;
  st->silk_mode.complexity = 9;
  celt_enc = encoder_celt_state(st);
  celt_encoder_init(celt_enc, Fs, channels);
  celt_enc->complexity = st->silk_mode.complexity;
  celt_enc->audio_application = application == OPUS_APPLICATION_AUDIO;
  st->use_vbr = 1;
  st->vbr_constraint = 1;
  st->user_bitrate_bps = -1000;
  st->application = application;
  st->encoder_buffer = encoder_uses_silk(application) ? st->Fs / 100 : 0;
  st->delay_compensation = st->Fs / 250;
  st->hybrid_stereo_width_Q14 = 1 << 14;
  st->prev_HB_gain = 1.0f;
  st->variable_HP_smth2_Q15 = silk_log_60_q15;
  st->mode = opus_mode_hybrid;
  st->bandwidth = 1105;
  st->audio_preprocess_mode = audio_preprocess_music;
  st->audio_preprocess_hold = audio_preprocess_warmup_frames;
}

static unsigned char gen_toc(int mode, int framerate, int bandwidth, int channels) {
  const int period = std::bit_width(static_cast<unsigned>((399 + framerate) / framerate - 1));
  unsigned char toc;
  if (mode == opus_mode_silk_only) {
    toc = static_cast<unsigned char>(((bandwidth - 1101) << 5) | ((period - 2) << 3));
  } else if (mode == opus_mode_celt_only) {
    const int tmp = std::max(0, bandwidth - 1102);
    toc = static_cast<unsigned char>(0x80 | (tmp << 5) | (period << 3));
  } else {
    toc = static_cast<unsigned char>(0x60 | ((bandwidth - 1104) << 4) | ((period - 2) << 3));
  }
  toc |= static_cast<unsigned char>((channels == 2) << 2);
  return toc;
}

static void silk_biquad_res(const opus_res* in, opus_int32 r_Q28, const opus_int32* A_Q28, opus_val32* S, opus_res* out,
                            const opus_int32 len, int stride) {
  constexpr float inv28 = 1.f / (1 << 28);
  const std::array<opus_val32, 2> A{A_Q28[0] * inv28, A_Q28[1] * inv28};
  const opus_val32 r = r_Q28 * inv28;
  for (int k = 0; k < len; k++) {
    const opus_val32 inval = in[k * stride];
    const opus_val32 filtered_input = r * inval;
    const opus_val32 vout = S[0] + filtered_input;
    S[0] = S[1] - vout * A[0] - 2.f * filtered_input;
    S[1] = -vout * A[1] + filtered_input + 1e-30f;
    out[k * stride] = vout;
  }
  S[0] = zero_tiny_float_mem(S[0]);
  S[1] = zero_tiny_float_mem(S[1]);
}

static void hp_cutoff(const opus_res* in, opus_int32 cutoff_Hz, opus_res* out, opus_val32* hp_mem, int len, int channels, opus_int32 Fs) {
  const opus_int32 Fc_Q19 = static_cast<opus_int16>(2471) * static_cast<opus_int16>(cutoff_Hz) / (Fs / 1000);
  const opus_int32 r_Q28 = (1 << 28) - 471 * Fc_Q19;
  const opus_int32 r_Q22 = r_Q28 >> 6;
  const opus_int32 fc_squared_Q22 = static_cast<opus_int32>((static_cast<opus_int64>(Fc_Q19) * Fc_Q19) >> 16);
  const opus_int32 A_Q28[2]{static_cast<opus_int32>((static_cast<opus_int64>(r_Q22) * (fc_squared_Q22 - (2 << 22))) >> 16),
                            static_cast<opus_int32>((static_cast<opus_int64>(r_Q22) * r_Q22) >> 16)};
  silk_biquad_res(in, r_Q28, A_Q28, hp_mem, out, len, channels);
  if (channels == 2) {
    silk_biquad_res(in + 1, r_Q28, A_Q28, hp_mem + 2, out + 1, len, channels);
  }
}

template <int Channels> static void dc_reject_channels(const opus_val16* in, opus_val16* out, opus_val32* hp_mem, int len, float coef) {
  std::array<float, Channels> memory;
  for (int channel = 0; channel < Channels; ++channel) {
    memory[channel] = hp_mem[2 * channel];
  }
  const float feedback = 1 - coef;
  for (int i = 0; i < len; ++i) {
    for (int channel = 0; channel < Channels; ++channel) {
      const auto index = Channels * i + channel;
      const float sample = in[index];
      out[index] = sample - memory[channel];
      memory[channel] = coef * sample + 1e-30f + feedback * memory[channel];
    }
  }
  for (int channel = 0; channel < Channels; ++channel) {
    hp_mem[2 * channel] = zero_tiny_float_mem(memory[channel]);
  }
}

static void dc_reject(const opus_val16* in, opus_val16* out, opus_val32* hp_mem, int len, int channels, opus_int32 Fs) {
  constexpr opus_int32 cutoff_Hz = 3;
  const float coef = 6.3f * cutoff_Hz / Fs;
  if (channels == 2) {
    dc_reject_channels<2>(in, out, hp_mem, len, coef);
  } else {
    dc_reject_channels<1>(in, out, hp_mem, len, coef);
  }
}

template <typename Apply>
static void fade_frames(opus_val16 first_gain, opus_val16 final_gain, int frame_size, opus_int32 Fs, Apply&& apply) {
  const int step = std::max(1, static_cast<int>(48000 / Fs));
  const int overlap = celt_default_overlap / step;
  int frame = 0;
  for (; frame < overlap; ++frame) {
    const auto weight = celt_mode()->window[frame * step] * celt_mode()->window[frame * step];
    apply(frame, weight * final_gain + (1.0f - weight) * first_gain);
  }
  for (; frame < frame_size; ++frame) {
    apply(frame, final_gain);
  }
}

static void stereo_fade(const opus_res* in, opus_res* out, opus_val16 g1, opus_val16 g2, int frame_size, int channels, opus_int32 Fs) {
  g1 = 1.0f - g1;
  g2 = 1.0f - g2;
  fade_frames(g1, g2, frame_size, Fs, [&](int frame, opus_val16 gain) {
    const int index = frame * channels;
    const opus_val32 difference = gain * .5f * (in[index] - in[index + 1]);
    out[index] -= difference;
    out[index + 1] += difference;
  });
}

static inline void gain_fade(const opus_res* in, opus_res* out, opus_val16 g1, opus_val16 g2, int frame_size, int channels, opus_int32 Fs) {
  fade_frames(g1, g2, frame_size, Fs, [&](int frame, opus_val16 gain) {
    const int offset = frame * channels;
    for (int channel = 0; channel < channels; ++channel) {
      out[offset + channel] = gain * in[offset + channel];
    }
  });
}

static opus_int32 user_bitrate_to_bitrate(OpusEncoder* st, int frame_rate, int max_data_bytes) {
  opus_int32 max_bitrate, user_bitrate;
  if (!frame_rate) {
    frame_rate = 400;
  }
  max_bitrate = bits_to_bitrate_for_frame_rate(max_data_bytes * 8, frame_rate);
  if (st->user_bitrate_bps == -1000) {
    user_bitrate = 60 * frame_rate + st->Fs * st->channels;
  } else if (st->user_bitrate_bps == -1)
    user_bitrate = 1500000;
  else
    user_bitrate = st->user_bitrate_bps;
  return std::min(user_bitrate, max_bitrate);
}

static opus_int32 frame_size_select(opus_int32 frame_size, opus_int32 Fs) {
  const auto samples = static_cast<opus_int64>(frame_size);
  const auto scaled_by_50 = 50 * samples;
  const bool supported = 400 * samples == Fs || 200 * samples == Fs || 100 * samples == Fs || scaled_by_50 == Fs || 25 * samples == Fs ||
                         scaled_by_50 == 3 * Fs || scaled_by_50 == 4 * Fs || scaled_by_50 == 5 * Fs || scaled_by_50 == 6 * Fs;
  return frame_size < Fs / 400 || !supported ? -1 : frame_size;
}

struct frame_activity_metrics {
  int is_silence;
  opus_val32 energy;
  opus_val32 mono_diff_ratio, mono_zero_cross_rate;
};

struct encoder_frame_analysis {
  opus_val16 stereo_width{};
  frame_activity_metrics activity{};
};

template <bool MeasureWidth = false>
static frame_activity_metrics measure_frame_activity(const opus_res* pcm, int frame_size, int channels, int lsb_depth,
                                                     StereoWidthState* width_mem = nullptr, opus_val16* stereo_width = nullptr,
                                                     opus_int32 sample_rate = 48000) {
  opus_val32 energy = 0, sample_max = 0, xx = 0, xy = 0, yy = 0;
  opus_val64 mono_energy = 0, mono_diff_energy = 0;
  opus_val32 previous_mono = pcm[0];
  int zero_crossings = 0;
  for (int i = 0; i < frame_size; ++i) {
    const auto left = pcm[i * channels];
    const auto right = pcm[i * channels + channels - 1];
    const auto mono = channels == 1 ? left : .5f * (left + right);
    energy += channels == 1 ? left * left : left * left + right * right;
    sample_max = std::max(sample_max, channels == 1 ? std::abs(left) : std::max(std::abs(left), std::abs(right)));
    mono_energy += static_cast<opus_val64>(mono) * mono;
    if (i != 0) {
      zero_crossings += (mono >= 0) != (previous_mono >= 0);
      const auto delta = mono - previous_mono;
      mono_diff_energy += static_cast<opus_val64>(delta) * delta;
    }
    previous_mono = mono;
    if constexpr (MeasureWidth) {
      xx += left * left;
      xy += left * right;
      yy += right * right;
    }
  }
  if constexpr (MeasureWidth) {
    const auto frame_rate = static_cast<opus_val16>(sample_rate / frame_size);
    const auto alpha = 25.0f / std::max<opus_val16>(50, frame_rate);
    if (!(xx < 1e9f) || xx != xx || !(yy < 1e9f) || yy != yy) {
      xy = xx = yy = 0;
    }
    width_mem->XX += alpha * (xx - width_mem->XX);
    width_mem->XY = (1.0f - alpha) * width_mem->XY + alpha * xy;
    width_mem->YY += alpha * (yy - width_mem->YY);
    width_mem->XX = std::max(0.f, width_mem->XX);
    width_mem->XY = std::max(0.f, width_mem->XY);
    width_mem->YY = std::max(0.f, width_mem->YY);
    if (std::max(width_mem->XX, width_mem->YY) > 8e-4f) {
      const auto sqrt_xx = static_cast<opus_val16>(std::sqrt(width_mem->XX));
      const auto sqrt_yy = static_cast<opus_val16>(std::sqrt(width_mem->YY));
      const auto fourth_xx = static_cast<opus_val16>(std::sqrt(sqrt_xx));
      const auto fourth_yy = static_cast<opus_val16>(std::sqrt(sqrt_yy));
      width_mem->XY = std::min(width_mem->XY, sqrt_xx * sqrt_yy);
      const auto correlation = width_mem->XY / (1e-15f + sqrt_xx * sqrt_yy);
      const auto level_difference = std::abs(fourth_xx - fourth_yy) / (1e-15f + fourth_xx + fourth_yy);
      const auto decorrelation = static_cast<opus_val16>(std::sqrt(1.f - correlation * correlation));
      const auto width = std::min(1.0f, decorrelation) * level_difference;
      width_mem->smoothed_width += (width - width_mem->smoothed_width) / frame_rate;
      width_mem->max_follower = std::max(width_mem->max_follower - 0.02f / frame_rate, width_mem->smoothed_width);
    }
    *stereo_width = std::min(1.0f, 20.0f * width_mem->max_follower);
  }
  const auto sample_count = frame_size * channels;
  return {sample_max <= static_cast<opus_val16>(1) / (1 << lsb_depth), energy / sample_count,
          static_cast<opus_val32>(mono_diff_energy / (mono_energy + 1e-12f)),
          static_cast<opus_val32>(zero_crossings) / std::max(1, frame_size - 1)};
}

static int compute_silk_rate_for_hybrid(int rate, int bandwidth, int vbr, int channels, bool fec) {
  rate /= channels;

  const auto& rate_table = fec ? hybrid_fec_rate_table : hybrid_rate_table;
  const auto table_size = static_cast<int>(rate_table.size());
  auto table_index = 1;
  for (; table_index < table_size; ++table_index) {
    if (rate_table[static_cast<std::size_t>(table_index)].threshold > rate) {
      break;
    }
  }

  int silk_rate;
  if (table_index == table_size) {
    const auto& last_entry = rate_table[static_cast<std::size_t>(table_index - 1)];
    silk_rate = last_entry.rate;
    silk_rate += (rate - last_entry.threshold) / 2;
  } else {
    const auto& lo_entry = rate_table[static_cast<std::size_t>(table_index - 1)];
    const auto& hi_entry = rate_table[static_cast<std::size_t>(table_index)];
    const auto lo = lo_entry.rate;
    const auto hi = hi_entry.rate;
    const auto x0 = lo_entry.threshold;
    const auto x1 = hi_entry.threshold;
    silk_rate = (lo * (x1 - rate) + hi * (rate - x0)) / (x1 - x0);
  }

  if (!vbr) {
    silk_rate += 100;
  }
  if (bandwidth == 1104) {
    silk_rate += 300;
  }
  silk_rate *= channels;
  if (channels == 2 && rate >= 12000) {
    silk_rate -= 1000;
  }
  return silk_rate;
}

[[nodiscard]] static auto decide_fec(const silk_EncControlStruct& control, int mode, int& bandwidth, opus_int32 rate) noexcept -> int {
  if (!control.useInBandFEC || control.packetLossPercentage == 0 || mode == opus_mode_celt_only) {
    return 0;
  }
  const int original_bandwidth = bandwidth;
  while (bandwidth >= 1101) {
    const auto index = static_cast<std::size_t>(2 * (bandwidth - 1101));
    auto threshold = static_cast<opus_int32>(fec_thresholds[index]);
    const auto hysteresis = static_cast<opus_int32>(fec_thresholds[index + 1]);
    threshold += control.LBRR_coded ? -hysteresis : hysteresis;
    threshold = threshold * (125 - std::min(control.packetLossPercentage, 25)) / 100;
    if (rate > threshold) {
      return 1;
    }
    if (control.packetLossPercentage <= 5 || bandwidth == 1101) {
      break;
    }
    --bandwidth;
  }
  bandwidth = original_bandwidth;
  return 0;
}

static opus_int32 compute_equiv_rate(opus_int32 bitrate, int channels, int frame_rate, int vbr, int mode, int complexity) {
  opus_int32 equiv = bitrate;
  if (frame_rate > 50) {
    equiv -= (40 * channels + 20) * (frame_rate - 50);
  }
  if (!vbr) {
    equiv -= equiv / 12;
  }
  equiv = equiv * (90 + complexity) / 100;
  if (mode == opus_mode_silk_only || mode == opus_mode_hybrid) {
    if (complexity < 2) {
      equiv = equiv * 4 / 5;
    }
  } else if (mode == opus_mode_celt_only) {
    if (complexity < 5) {
      equiv = equiv * 9 / 10;
    }
  }
  return equiv;
}

constexpr auto hybrid_silk_lowrate_boost_min_bps = 28000;
constexpr auto hybrid_silk_lowrate_boost_max_bps = 36000;
constexpr auto hybrid_silk_lowrate_target_bps = 24000;
constexpr auto hybrid_silk_lowrate_reserve_bps = 2000;
constexpr opus_int32 voip_mono_silk_budget_boost_min_bps = 16000;
constexpr opus_int32 voip_mono_silk_budget_boost_max_bps = 64000;
constexpr opus_int32 voip_mono_silk_budget_boost_default_bps = 3000;
constexpr opus_int32 voip_mono_silk_budget_boost_lowrate_bps = 2000;
constexpr int lightweight_analysis_frame_limit = 127;

[[nodiscard]] static constexpr opus_int32 hybrid_silk_lowrate_boost_bps(opus_int32 user_bitrate_bps, opus_int32 silk_bitrate_bps) noexcept {
  if (user_bitrate_bps < hybrid_silk_lowrate_boost_min_bps || user_bitrate_bps > hybrid_silk_lowrate_boost_max_bps) {
    return silk_bitrate_bps;
  }
  const auto payload_limit_bps = std::max<opus_int32>(500, user_bitrate_bps - hybrid_silk_lowrate_reserve_bps);
  const auto target_bps = std::min<opus_int32>(hybrid_silk_lowrate_target_bps, payload_limit_bps);
  return std::min<opus_int32>(payload_limit_bps, std::max<opus_int32>(silk_bitrate_bps, target_bps));
}

[[nodiscard]] static constexpr auto voip_mono_silk_budget_boost(const OpusEncoder* st) noexcept -> opus_int32 {
  if (st->application != OPUS_APPLICATION_VOIP || st->channels != 1 || st->bitrate_bps < voip_mono_silk_budget_boost_min_bps ||
      st->bitrate_bps >= voip_mono_silk_budget_boost_max_bps) {
    return 0;
  }
  return st->bitrate_bps <= voip_mono_silk_budget_boost_min_bps ? voip_mono_silk_budget_boost_lowrate_bps
                                                                : voip_mono_silk_budget_boost_default_bps;
}

constexpr opus_int32 voip_voice_low_band_keep_min_bps = 16000;
constexpr opus_int32 voip_noisy_voice_low_band_min_bps = 22000;
constexpr opus_val32 voip_noisy_voice_energy_max = 0.001f;
constexpr opus_val32 voip_noisy_voice_diff_ratio_min = 0.18f;
constexpr opus_val32 voip_noisy_voice_zero_cross_min = 0.14f;
constexpr opus_val16 voip_noisy_voice_smoothing = 0.18f;
constexpr int voip_noise_confidence_apply_Q7 = 90;
constexpr opus_val32 voip_quiet_hissy_voice_diff_ratio_min = 0.40f;
constexpr opus_val16 voip_quiet_hissy_voice_low_band_keep = 0.50f;
constexpr opus_val16 voip_mid_diff_voice_low_band_keep = 0.42f;
constexpr opus_int32 celt_energy_feedback_bypass_min_bps = 80000;
constexpr opus_int32 celt_energy_feedback_bypass_max_bps = 112000;

[[nodiscard]] static constexpr bool is_sparse_high_z_tonal_frame(const frame_activity_metrics& metrics) noexcept {
  return metrics.energy > 1e-5f && metrics.mono_diff_ratio > .40f && metrics.mono_zero_cross_rate > .20f;
}

static int update_lightweight_voice_estimate(OpusEncoder* st, opus_val16 stereo_width,
                                             const frame_activity_metrics& frame_metrics) noexcept {
  const auto track_score = [](int score, bool detected, int attack, int release_Q7) noexcept {
    score = detected ? score + std::max(1, (115 - score) / attack) : (score * release_Q7) >> 7;
    return clamp_value(score, 0, 115);
  };
  auto& voice_score = st->lightweight_voice_score_Q7;
  auto& music_score = st->lightweight_music_score_Q7;
  auto& vad_score = st->lightweight_vad_score_Q7;
  st->lightweight_analysis_frames = std::min(st->lightweight_analysis_frames + 1, lightweight_analysis_frame_limit);
  const bool stereo_music = st->channels == 2 && stereo_width > .05f;
  const bool active = !frame_metrics.is_silence && frame_metrics.energy > 1e-7f;
  const auto diff = frame_metrics.mono_diff_ratio;
  const auto zcr = frame_metrics.mono_zero_cross_rate;
  const bool high_z = active && is_sparse_high_z_tonal_frame(frame_metrics);
  const bool sustained_harmonic = active && diff < .025f && zcr < .075f;
  const bool speech_activity = active && diff > .006f && diff < .12f && zcr > .018f && zcr < .18f && !sustained_harmonic;
  auto& harmonic_music = st->lightweight_harmonic_music_Q7;
  harmonic_music = track_score(harmonic_music, sustained_harmonic, 8, 104);
  auto& high_z_tonal = st->lightweight_high_z_tonal_Q7;
  high_z_tonal = track_score(high_z_tonal, high_z, 4, high_z_tonal > 64 && st->bitrate_bps < 40000 ? 124 : 104);
  vad_score = track_score(vad_score, speech_activity, 4, 112);
  const bool lowrate_vad_policy = st->bitrate_bps < 40000;
  voice_score = track_score(voice_score, speech_activity && !stereo_music, lowrate_vad_policy && vad_score > 64 ? 4 : 5,
                            sustained_harmonic ? 112 : 126);
  music_score = track_score(music_score, stereo_music || sustained_harmonic || high_z, 8, 112);
  if (stereo_music) {
    return 0;
  }
  if (lowrate_vad_policy && vad_score > 80 && voice_score > 48 && harmonic_music < 64) {
    return 115;
  }
  if (voice_score > 60 && harmonic_music < 80) {
    return 115;
  }
  if (harmonic_music > 52 && music_score >= voice_score + 8) {
    return 0;
  }
  return voice_score > 44 && voice_score + 16 >= music_score ? 115 : music_score >= voice_score + 8 && music_score > 36 ? 0 : 48;
}

static void update_voip_noise_confidence(OpusEncoder* st, const frame_activity_metrics& metrics) noexcept {
  if (st->channels != 1 || st->voip_noise_confidence_Q7 < 0) {
    return;
  }
  auto& confidence = st->voip_noise_confidence_Q7;

  if (confidence >= 115) {
    return;
  }
  const bool broadband_noise = !metrics.is_silence && metrics.energy > 1e-7f && metrics.energy < .03f && metrics.mono_diff_ratio > .40f &&
                               metrics.mono_zero_cross_rate > .22f;
  if (broadband_noise) {
    confidence = std::min(115, confidence + std::max(1, (115 - confidence) >> 2));
    if (confidence >= 64) {
      confidence = 115;
    }
  } else {
    confidence = std::max(0, confidence - 4);
  }
}

[[nodiscard]] static auto classify_encoder_frame(OpusEncoder* st, const encoder_frame_analysis& analysis) noexcept -> int {
  int voice_est = 48;
  if (st->application == OPUS_APPLICATION_VOIP || st->application == OPUS_APPLICATION_AUDIO) {
    voice_est = update_lightweight_voice_estimate(st, analysis.stereo_width, analysis.activity);
    if (st->application == OPUS_APPLICATION_VOIP) {
      voice_est = voice_est > 48 ? 115 : 0;
      update_voip_noise_confidence(st, analysis.activity);
    }
  }
  return voice_est;
}

[[nodiscard]] static opus_int32 quality_mode_threshold(int voice_weight, opus_val16 stereo_width, bool voip_style, int prev_mode) noexcept {
  const auto mode_voice =
      static_cast<opus_int32>((1.0f - stereo_width) * mono_voice_mode_threshold + stereo_width * stereo_voice_mode_threshold);
  auto threshold = music_mode_threshold + ((voice_weight * (mode_voice - music_mode_threshold)) >> 14);
  if (voip_style) {
    threshold += 8000;
  }
  if (prev_mode == opus_mode_celt_only) {
    threshold -= 8000;
  } else if (prev_mode > 0)
    threshold += 8000;
  return std::max<opus_int32>(threshold, voip_style ? 23000 : 15000);
}

[[nodiscard]] static opus_int32 quality_bandwidth_threshold(int voice_weight, opus_int32 music_threshold,
                                                            opus_int32 voice_threshold) noexcept {
  return music_threshold + ((voice_weight * (voice_threshold - music_threshold)) >> 14);
}

constexpr int dtx_entry_ms_Q1 = 10 * 20 * 2;
constexpr int dtx_refresh_ms_Q1 = (10 + 20) * 20 * 2;
constexpr opus_val32 dtx_digital_silence_max_energy = 2e-8f;
constexpr opus_val32 dtx_audible_activity_min_energy = 1e-5f;

[[nodiscard]] static bool should_emit_dtx(OpusEncoder* st, const frame_activity_metrics& metrics, int frame_size) noexcept {
  bool active;
  if (metrics.is_silence || (metrics.energy <= dtx_digital_silence_max_energy && metrics.mono_zero_cross_rate == 0)) {
    st->dtx_smoothed_energy *= .875f;
    active = false;
  } else {
    const auto previous_energy = st->dtx_smoothed_energy;
    st->dtx_smoothed_energy += .125f * (metrics.energy - st->dtx_smoothed_energy);
    const bool changing_ambience =
        previous_energy > 1e-9f && std::abs(metrics.energy - previous_energy) > .5f * (metrics.energy + previous_energy);
    const bool protected_history = st->lightweight_voice_score_Q7 >= 24 || st->lightweight_vad_score_Q7 >= 24 ||
                                   st->lightweight_music_score_Q7 >= 24 || st->lightweight_harmonic_music_Q7 >= 24 ||
                                   st->lightweight_high_z_tonal_Q7 >= 24;
    const bool quiet_speech_shape = metrics.mono_diff_ratio > .0001f && metrics.mono_diff_ratio < .18f &&
                                    metrics.mono_zero_cross_rate > .001f && metrics.mono_zero_cross_rate < .22f;
    active = metrics.energy >= dtx_audible_activity_min_energy || changing_ambience || protected_history || quiet_speech_shape;
  }
  if (active) {
    st->nb_no_activity_ms_Q1 = 0;
    return false;
  }

  st->nb_no_activity_ms_Q1 += 2 * 1000 * frame_size / st->Fs;
  if (st->nb_no_activity_ms_Q1 <= dtx_entry_ms_Q1) {
    return false;
  }
  if (st->nb_no_activity_ms_Q1 <= dtx_refresh_ms_Q1) {
    return true;
  }
  st->nb_no_activity_ms_Q1 = dtx_entry_ms_Q1;
  return false;
}

[[nodiscard]] static bool encoder_is_in_dtx(const OpusEncoder* st) noexcept {
  return st->use_dtx && st->nb_no_activity_ms_Q1 >= dtx_entry_ms_Q1;
}

[[nodiscard]] static opus_int32 finalize_dtx_packet(OpusEncoder* st, unsigned char* data, opus_int32 length, bool emit_dtx) noexcept {
  if (!emit_dtx || length <= 0) {
    return length;
  }
  st->rangeFinal = 0;
  data[0] &= 0xFC;
  return 1;
}

[[nodiscard]] static int choose_audio_preprocess_mode(OpusEncoder* st) noexcept {
  if (st->audio_preprocess_hold > 0) {
    --st->audio_preprocess_hold;
    return st->audio_preprocess_mode;
  }
  int next_mode = st->audio_preprocess_mode;
  if (st->lightweight_analysis_frames >= audio_preprocess_warmup_frames) {
    const bool speech_like = st->channels == 1 && st->lightweight_voice_score_Q7 > 88 && st->lightweight_harmonic_music_Q7 < 32 &&
                             st->lightweight_music_score_Q7 < 32;
    const bool music_like = st->lightweight_harmonic_music_Q7 > 48;
    if (speech_like) {
      next_mode = audio_preprocess_speech;
    } else if (music_like)
      next_mode = audio_preprocess_music;
  }
  if (next_mode != st->audio_preprocess_mode) {
    st->audio_preprocess_mode = next_mode;
    st->audio_preprocess_hold = audio_preprocess_hold_frames;
    zero_n_items(next_mode == audio_preprocess_speech ? st->audio_speech_hp_mem : st->audio_music_hp_mem, 4);
  }
  return st->audio_preprocess_mode;
}

static int compute_redundancy_bytes(opus_int32 max_data_bytes, opus_int32 bitrate_bps, int frame_rate, int channels) {
  const int base_bits = 40 * channels + 20;
  auto redundancy_rate = bitrate_bps + base_bits * (200 - frame_rate);
  redundancy_rate = 3 * redundancy_rate / 2;

  auto redundancy_bytes = redundancy_rate / 1600;
  const opus_int32 available_bits = max_data_bytes * 8 - 2 * base_bits;
  const int redundancy_bytes_cap = (available_bits * 240 / (240 + 48000 / frame_rate) + base_bits) / 8;
  redundancy_bytes = std::min(redundancy_bytes, redundancy_bytes_cap);
  if (redundancy_bytes > 4 + 8 * channels) {
    redundancy_bytes = std::min(257, redundancy_bytes);
  } else
    redundancy_bytes = 0;
  return redundancy_bytes;
}

static inline void apply_previous_sample_tilt(opus_res* pcm, int frame_size, int channels, opus_val16 smoothing,
                                              opus_val16 gain = 1.0f) noexcept {
  for (int channel = 0; channel < channels; ++channel) {
    opus_res previous = pcm[channel] * gain;
    pcm[channel] = previous;
    for (int i = channel + channels; i < frame_size * channels; i += channels) {
      const opus_res current = pcm[i] * gain;
      pcm[i] = current + smoothing * (previous - current);
      previous = current;
    }
  }
}

static void blend_filtered_input(opus_res* filtered, const opus_res* input, int samples, opus_val16 keep) noexcept {
  for (int index = 0; index < samples; ++index) {
    filtered[index] += keep * (input[index] - filtered[index]);
  }
}

[[nodiscard]] static constexpr auto encoder_error_balance_filter_for(const OpusEncoder* st, const frame_activity_metrics& metrics) noexcept
    -> opus_val16 {
  const auto bitrate = st->bitrate_bps;
  if (st->application == OPUS_APPLICATION_AUDIO) {
    const bool stereo_94 =
        st->channels == 2 && (bitrate == 24000 || (bitrate == 32000 && st->mode == opus_mode_celt_only &&
                                                   st->lightweight_high_z_tonal_Q7 < 48 && !is_sparse_high_z_tonal_frame(metrics)));
    if (stereo_94) {
      return .94f;
    }
    if (st->channels == 2 && (bitrate == 16000 || bitrate == 64000)) {
      return .98f;
    }
    if (bitrate >= 16000 && bitrate < 40000) {
      return .975f;
    }
    return bitrate >= 128000 ? .997f : 1.0f;
  }
  if (st->application == OPUS_APPLICATION_VOIP) {
    if (st->preprocess_filter_state == preprocess_filter_quiet_voice && bitrate >= 40000 && bitrate < 64000) {
      return .984f;
    }
    if (bitrate >= 24000 && bitrate <= 64000) {
      return bitrate >= 40000 ? .985f : .994f;
    }
    if (bitrate >= 80000) {
      return bitrate <= 128000 ? .994f : .998f;
    }
  }
  return 1.0f;
}

[[nodiscard]] constexpr auto encoder_delay_compensation(const OpusEncoder* st) noexcept -> int {
  return st->application == OPUS_APPLICATION_RESTRICTED_LOWDELAY ? 0 : st->delay_compensation;
}

namespace {
struct encoder_stage_storage {
  std::span<opus_res> active, next;
  std::size_t history_samples, celt_offset;

  [[nodiscard]] auto active_frame() noexcept -> std::span<opus_res> {
    return active.subspan(history_samples);
  }
  [[nodiscard]] auto active_celt_window() noexcept -> std::span<opus_res> {
    return active.subspan(celt_offset);
  }
  [[nodiscard]] auto active_celt_prefill(const OpusEncoder* st) noexcept -> std::span<opus_res> {
    return active.subspan(celt_offset - static_cast<std::size_t>(st->Fs / 400 * st->channels),
                          static_cast<std::size_t>(st->channels * st->Fs / 400));
  }
  auto prime_from_encoder(const OpusEncoder* st) noexcept -> void {
    if (history_samples != 0) {
      copy_n_items(encoder_delay_buffer(st), history_samples, active.data());
    }
  }
  auto finish_frame(OpusEncoder* st, bool has_next) noexcept -> void {
    if (history_samples != 0) {
      auto* source = active.data() + active.size() - history_samples;
      if (has_next) {
        copy_n_items(source, history_samples, next.data());
      } else {
        copy_n_items(source, history_samples, encoder_delay_buffer(st));
      }
    }
  }
  auto advance() noexcept -> void {
    std::swap(active, next);
  }
};

[[nodiscard]] inline auto make_encoder_stage_storage(opus_res* storage, const OpusEncoder* st, const int total_buffer,
                                                     const int frame_size) noexcept -> encoder_stage_storage {
  const auto window_samples = static_cast<std::size_t>((st->encoder_buffer + frame_size) * st->channels);
  return {{storage, window_samples},
          {storage + window_samples, window_samples},
          static_cast<std::size_t>(st->encoder_buffer * st->channels),
          static_cast<std::size_t>((st->encoder_buffer - total_buffer) * st->channels)};
}
} // namespace

[[nodiscard]] static auto encode_low_rate_packet(OpusEncoder* st, const int frame_size, const opus_int32 out_data_bytes,
                                                 opus_int32 max_data_bytes, unsigned char* data) -> opus_int32 {
  int tocmode = st->mode;
  int frame_rate = st->Fs / frame_size;
  int bw = st->bandwidth == 0 ? 1101 : st->bandwidth, packet_code = 0, num_multiframes = 0;
  if (tocmode == 0) {
    tocmode = opus_mode_silk_only;
  }
  if (frame_rate > 100) {
    tocmode = opus_mode_celt_only;
  }
  if (frame_rate == 25 && tocmode != opus_mode_silk_only) {
    frame_rate = 50;
    packet_code = 1;
  }
  if (frame_rate <= 16) {
    if (out_data_bytes == 1 || (tocmode == opus_mode_silk_only && frame_rate != 10)) {
      tocmode = opus_mode_silk_only;
      packet_code = frame_rate <= 12;
      frame_rate = frame_rate == 12 ? 25 : 16;
    } else {
      num_multiframes = 50 / frame_rate;
      frame_rate = 50;
      packet_code = 3;
    }
  }
  if (tocmode == opus_mode_silk_only && bw > 1103) {
    bw = 1103;
  } else if (tocmode == opus_mode_celt_only && bw == 1102)
    bw = 1101;
  else if (tocmode == opus_mode_hybrid && bw <= 1104)
    bw = 1104;
  data[0] = gen_toc(tocmode, frame_rate, bw, st->stream_channels);
  data[0] |= packet_code;
  int ret = packet_code <= 1 ? 1 : 2;
  max_data_bytes = std::max(max_data_bytes, ret);
  if (packet_code == 3) {
    data[1] = num_multiframes;
  }
  if (!st->use_vbr) {
    ret = pad_packet(data, ret, max_data_bytes);
    if (ret == 0) {
      ret = max_data_bytes;
    } else
      ret = -3;
  }
  return ret;
}
struct multiframe_encode_params final {
  bool float_api;
  bool governed_vbr;
  int lsb_depth;
  int redundancy, celt_to_silk, to_celt, prefill;
  opus_int32 equiv_rate, cbr_bytes, target_bits;
};
static opus_int32 opus_encode_frame_native(OpusEncoder* st, const opus_res* pcm, int frame_size, unsigned char* data,
                                           opus_int32 max_data_bytes, opus_int32 allocator_target_bits, bool float_api,
                                           const frame_activity_metrics& metrics, int redundancy, int celt_to_silk, int prefill,
                                           opus_int32 equiv_rate, int to_celt, bool nonfinal_frame, bool skip_celt_for_dtx,
                                           encoder_stage_storage& stage_storage);
[[nodiscard]] static auto encode_multiframe_packet(OpusEncoder* st, const opus_res* pcm, const int frame_size, unsigned char* data,
                                                   const opus_int32 out_data_bytes, const multiframe_encode_params& params,
                                                   std::array<opus_res, encoder_max_stage_samples>& stage_buffer_storage) -> opus_int32 {
  const int enc_frame_size = st->mode != opus_mode_silk_only ? st->Fs / 50
                             : frame_size == 2 * st->Fs / 25 ? st->Fs / 25
                             : frame_size == 3 * st->Fs / 25 ? 3 * st->Fs / 50
                                                             : st->Fs / 50;
  const int nb_frames = frame_size / enc_frame_size;
  const int max_header_bytes = nb_frames == 2 ? 3 : (2 + (nb_frames - 1) * 2);
  const auto total_buffer = encoder_delay_compensation(st);
  auto* stage_buffer = stage_buffer_storage.data();
  auto stage_storage = make_encoder_stage_storage(stage_buffer, st, total_buffer, enc_frame_size);
  opus_int32 tot_size = 0;
  int dtx_count = 0;
  stage_storage.prime_from_encoder(st);
  opus_int32 repacketize_len = (st->use_vbr || st->user_bitrate_bps == -1) ? out_data_bytes : std::min(params.cbr_bytes, out_data_bytes);
  const opus_int32 packet_target_bits = params.governed_vbr ? params.target_bits : bitrate_to_bits(st->bitrate_bps, st->Fs, frame_size);
  const auto packet_budget = make_vbr_frame_budget(packet_target_bits, st->vbr_budget_reservoir_bits);
  if (params.governed_vbr) {
    repacketize_len = std::min(repacketize_len, packet_budget.max_bytes);
  }
  const opus_int32 max_len_sum = nb_frames + repacketize_len - max_header_bytes;
  opus_int32 local_credit_bits = st->vbr_budget_reservoir_bits;
  opus_int32 remaining_target_bits = packet_target_bits;
  std::array<unsigned char, opus_max_multiframe_packet_bytes> packet_storage;
  auto* curr_data = packet_storage.data();
  packet_frame_set packet_frames;
  const int bak_to_mono = st->silk_mode.toMono;
  int result = 0;
  if (!bak_to_mono) {
    st->prev_channels = st->stream_channels;
  }
  for (int frame_index = 0; frame_index < nb_frames; ++frame_index) {
    st->silk_mode.toMono = 0;
    const int frame_to_celt = params.to_celt && frame_index == nb_frames - 1;
    const int frame_redundancy = params.redundancy && (frame_to_celt || (!params.to_celt && frame_index == 0));
    const int remaining_frames = nb_frames - frame_index;
    const opus_int32 frame_target_bits = remaining_target_bits / remaining_frames;
    const auto frame_budget = make_vbr_frame_budget(frame_target_bits, local_credit_bits);
    const opus_int32 frame_max_bytes = params.governed_vbr ? frame_budget.max_bytes : frame_target_bits / 8;
    opus_int32 curr_max = std::min(max_len_sum - tot_size, frame_max_bytes);
    const auto* frame_pcm = pcm + frame_index * (st->channels * enc_frame_size);
    const auto frame_metrics = measure_frame_activity(frame_pcm, enc_frame_size, st->channels, params.lsb_depth);
    const bool emit_dtx = st->use_dtx && should_emit_dtx(st, frame_metrics, enc_frame_size);
    const opus_int32 allocator_target_bits = params.governed_vbr ? frame_budget.allocator_bits : 0;
    int tmp_len =
        opus_encode_frame_native(st, frame_pcm, enc_frame_size, curr_data, curr_max, allocator_target_bits, params.float_api, frame_metrics,
                                 frame_redundancy, params.celt_to_silk, params.prefill, params.equiv_rate, frame_to_celt,
                                 frame_index < nb_frames - 1, emit_dtx && st->mode == opus_mode_hybrid, stage_storage);
    if (tmp_len < 0) {
      result = -3;
      break;
    }
    tmp_len = finalize_dtx_packet(st, curr_data, tmp_len, emit_dtx);
    if (tmp_len == 1) {
      ++dtx_count;
    }
    if (append_packet_frames(&packet_frames, curr_data, tmp_len) < 0) {
      result = -3;
      break;
    }
    tot_size += tmp_len;
    curr_data += tmp_len;
    local_credit_bits = update_vbr_credit(local_credit_bits, tmp_len, frame_target_bits);
    remaining_target_bits -= frame_target_bits;
    if (frame_index + 1 < nb_frames) {
      stage_storage.advance();
    }
  }
  st->silk_mode.toMono = bak_to_mono;
  if (result < 0) {
    return result;
  }
  result = write_packet_frames(&packet_frames, data, repacketize_len, !st->use_vbr && dtx_count != nb_frames);
  if (encoder_is_in_dtx(st)) {
    reset_vbr_budget(st);
  } else if (params.governed_vbr && result > 0) {
    st->vbr_budget_reservoir_bits = update_vbr_credit(st->vbr_budget_reservoir_bits, result, packet_target_bits);
  }
  return result < 0 ? -3 : result;
}

static opus_int32 encode_native(OpusEncoder* st, const opus_res* pcm, int frame_size, unsigned char* data, opus_int32 out_data_bytes,
                                int lsb_depth, bool float_api) {
  int prefill = 0, redundancy = 0, celt_to_silk = 0, to_celt = 0;
  opus_int32 cbr_bytes = -1;
  auto max_data_bytes = std::min(1276 * 6, out_data_bytes);
  st->rangeFinal = 0;
  if (frame_size <= 0 || max_data_bytes <= 0) {
    return -1;
  }
  if (max_data_bytes == 1 && st->Fs == (frame_size * 10)) {
    return -2;
  }
  auto* celt_enc = encoder_celt_state(st);
  const int frame_rate = st->Fs / frame_size;
  st->bitrate_bps = user_bitrate_to_bitrate(st, frame_rate, max_data_bytes);
  const bool voip_style = st->application == OPUS_APPLICATION_VOIP;
  const bool first = st->prev_mode == 0;
  const bool governed_vbr = st->use_vbr && st->vbr_constraint && st->user_bitrate_bps > 0;
  const opus_int32 requested_frame_bits =
      governed_vbr ? next_vbr_target_bits(st, frame_size) : bitrate_to_bits_for_frame_rate(st->bitrate_bps, frame_rate);
  const auto frame_budget = make_vbr_frame_budget(requested_frame_bits, st->vbr_budget_reservoir_bits);
  if (!st->use_vbr) {
    const opus_int32 cbr_budget_bytes = (bitrate_to_bits_for_frame_rate(st->bitrate_bps, frame_rate) + 4) / 8;
    cbr_bytes = std::min(cbr_budget_bytes, max_data_bytes);
    st->bitrate_bps = bits_to_bitrate_for_frame_rate(cbr_bytes * 8, frame_rate);
    max_data_bytes = std::max(1, cbr_bytes);
  }

  std::array<opus_res, encoder_max_stage_samples> stage_buffer_storage;
  encoder_frame_analysis analysis;
  if (st->channels == 1) {
    analysis.activity = measure_frame_activity(pcm, frame_size, 1, lsb_depth);
  } else {
    analysis.activity = measure_frame_activity<true>(pcm, frame_size, 2, lsb_depth, &st->width_mem, &analysis.stereo_width, st->Fs);
  }
  const auto& frame_metrics = analysis.activity;
  if (!frame_metrics.is_silence) {
    st->peak_signal_energy = std::max<opus_val32>(0.999f * st->peak_signal_energy, frame_metrics.energy);
  }
  if (max_data_bytes < 3 || st->bitrate_bps < 3 * frame_rate * 8 ||
      (frame_rate < 50 && (max_data_bytes * static_cast<opus_int32>(frame_rate) < 300 || st->bitrate_bps < 2400))) {
    return encode_low_rate_packet(st, frame_size, out_data_bytes, max_data_bytes, data);
  }
  const auto voice_est = classify_encoder_frame(st, analysis);
  const auto voice_weight = voice_est * voice_est;
  const bool probing_voip_mode = voip_style && st->channels == 1 && st->bitrate_bps > 16000 && st->bitrate_bps <= 64000 &&
                                 st->lightweight_analysis_frames <= voip_mode_probe_frames;
  if (probing_voip_mode && frame_metrics.mono_diff_ratio > .012f && frame_metrics.mono_diff_ratio < .12f &&
      frame_metrics.mono_zero_cross_rate > .018f && frame_metrics.mono_zero_cross_rate < .18f) {
    ++st->audio_preprocess_hold;
  }
  if (probing_voip_mode && st->lightweight_analysis_frames == voip_mode_probe_frames &&
      st->audio_preprocess_hold >= audio_preprocess_warmup_frames + voip_mode_min_voiced_frames &&
      st->audio_preprocess_hold <= audio_preprocess_warmup_frames + voip_mode_max_voiced_frames && st->lightweight_high_z_tonal_Q7 < 64) {
    st->audio_preprocess_mode = audio_preprocess_speech;
  }
  const bool probing_lowrate_voip =
      voip_style && st->channels == 1 && st->bitrate_bps <= 16000 && st->lightweight_analysis_frames <= audio_preprocess_warmup_frames;
  if (probing_lowrate_voip && st->audio_preprocess_mode != preprocess_lowrate_voip_celt) {
    if (st->lightweight_analysis_frames == 1) {
      st->audio_preprocess_mode = frame_metrics.energy >= .005f ? preprocess_lowrate_voip_continuous : audio_preprocess_music;
    } else if (st->audio_preprocess_mode == preprocess_lowrate_voip_continuous && frame_metrics.energy < .005f) {
      st->audio_preprocess_mode = audio_preprocess_music;
    }
  }
  const bool early_quiet_voice_probe = st->bitrate_bps == 64000;
  if (voip_style && st->channels == 1 && st->preprocess_filter_state == 0 &&
      st->lightweight_analysis_frames >= (early_quiet_voice_probe ? 1 : audio_preprocess_warmup_frames)) {
    const auto quiet_energy = early_quiet_voice_probe ? .01f : quiet_voice_probe_energy;
    st->preprocess_filter_state = st->peak_signal_energy < quiet_energy ? preprocess_filter_quiet_voice : preprocess_filter_default;
  }
  celt_enc->high_z_tonal_Q7 = static_cast<opus_uint8>(st->lightweight_high_z_tonal_Q7);
  celt_enc->input_diff_Q10 = static_cast<opus_uint8>(clamp_value(static_cast<int>(1024.f * frame_metrics.mono_diff_ratio + .5f), 0, 255));
  const bool sparse_tonal_frame = is_sparse_high_z_tonal_frame(frame_metrics);
  const bool sparse_high_z_tonal = st->lightweight_high_z_tonal_Q7 > 0 || sparse_tonal_frame;
  const bool preserve_stereo = st->application == OPUS_APPLICATION_AUDIO && st->channels == 2 &&
                               ((st->bitrate_bps == 48000 && sparse_high_z_tonal) ||
                                (st->bitrate_bps >= 56000 && st->bitrate_bps < 80000 &&
                                 (st->lightweight_analysis_frames <= stereo_preservation_probe_frames || sparse_high_z_tonal)));
  st->silk_mode.preserveStereo = preserve_stereo ? (st->bitrate_bps == 48000 ? silk_preserve_stereo_force : silk_preserve_stereo_bias) : 0;
  if (st->channels == 2) {
    const opus_int32 channel_equiv_rate =
        compute_equiv_rate(st->bitrate_bps, st->channels, frame_rate, st->use_vbr, 0, st->silk_mode.complexity);
    opus_int32 stereo_threshold = quality_bandwidth_threshold(voice_weight, stereo_music_threshold, stereo_voice_threshold);
    if (analysis.stereo_width < .015f && channel_equiv_rate < 32000) {
      st->stream_channels = 1;
    } else {
      stereo_threshold += st->stream_channels == 2 ? -1000 : 1000;
      st->stream_channels = (channel_equiv_rate > stereo_threshold) ? 2 : 1;
    }
  } else {
    st->stream_channels = st->channels;
  }
  auto equiv_rate = compute_equiv_rate(st->bitrate_bps, st->stream_channels, frame_rate, st->use_vbr, 0, st->silk_mode.complexity);
  if (st->application == OPUS_APPLICATION_RESTRICTED_LOWDELAY) {
    st->mode = opus_mode_celt_only;
  } else {
    const auto threshold = quality_mode_threshold(voice_weight, analysis.stereo_width, voip_style, st->prev_mode);
    st->mode = (equiv_rate >= threshold) ? opus_mode_celt_only : opus_mode_silk_only;
    const bool mature = st->lightweight_analysis_frames >= audio_preprocess_warmup_frames;
    const bool tonal = st->lightweight_harmonic_music_Q7 >= 64 || st->lightweight_high_z_tonal_Q7 > 64;
    const bool speech = voice_est >= 100 || st->lightweight_vad_score_Q7 > 48;
    const bool stable_voip_speech = voip_style && st->audio_preprocess_mode == audio_preprocess_speech;
    if (voip_style && st->channels == 1 && (stable_voip_speech || (speech && !tonal)) && st->bitrate_bps <= 64000) {
      st->mode = opus_mode_silk_only;
    } else if (mature && voice_est <= 16) {
      st->mode = st->bitrate_bps >= (voip_style ? 23000 : 15000) ? opus_mode_celt_only : opus_mode_silk_only;
    }
    if (max_data_bytes < bitrate_to_bits_for_frame_rate(frame_rate > 50 ? 9000 : 6000, frame_rate) / 8) {
      st->mode = opus_mode_celt_only;
    }
  }
  if (st->application == OPUS_APPLICATION_AUDIO && st->channels == 1 && st->bitrate_bps < 20000) {
    st->mode = opus_mode_celt_only;
  }
  if (voip_style && st->channels == 1 && st->bitrate_bps == 64000 && st->preprocess_filter_state == preprocess_filter_quiet_voice) {
    st->mode = opus_mode_silk_only;
  }
  if (st->application == OPUS_APPLICATION_AUDIO && st->channels == 2) {
    const bool confident_high_z_tonal = st->lightweight_high_z_tonal_Q7 > 64 || sparse_tonal_frame;
    const bool segment_selected_bitrate = st->bitrate_bps >= 48000 && st->bitrate_bps < 112000;
    if (segment_selected_bitrate && st->preprocess_filter_state >= 0) {
      const bool stable_tonal_frame = frame_metrics.mono_diff_ratio > .0015f && frame_metrics.mono_diff_ratio < .003f &&
                                      frame_metrics.mono_zero_cross_rate > .008f && frame_metrics.mono_zero_cross_rate < .018f;
      st->preprocess_filter_state = !stable_tonal_frame                ? preprocess_filter_general_audio
                                    : st->preprocess_filter_state == 2 ? preprocess_filter_stable_tonal
                                                                       : st->preprocess_filter_state + 1;
    }
    const bool stable_tonal_segment = st->preprocess_filter_state != preprocess_filter_general_audio;
    if (st->bitrate_bps == 32000 && !confident_high_z_tonal) {
      st->mode = opus_mode_celt_only;
    } else if (segment_selected_bitrate && (st->bitrate_bps >= 56000 || !confident_high_z_tonal) && !stable_tonal_segment) {
      st->mode = opus_mode_hybrid;
    }
  }
  if (voip_style && st->channels == 1 && st->bitrate_bps <= 16000) {
    const bool noisy_start =
        frame_metrics.energy > .003f && frame_metrics.mono_diff_ratio > .60f && frame_metrics.mono_zero_cross_rate > .27f;
    const bool locked_tonal_start =
        frame_metrics.energy > .002f && frame_metrics.mono_diff_ratio < .002f && frame_metrics.mono_zero_cross_rate < .015f;
    if (st->lightweight_analysis_frames <= audio_preprocess_warmup_frames && (noisy_start || locked_tonal_start)) {
      st->audio_preprocess_mode = preprocess_lowrate_voip_celt;
    }
    if (st->audio_preprocess_mode == preprocess_lowrate_voip_celt) {
      st->mode = opus_mode_celt_only;
    }
  }
  const bool severe_voip_noise = frame_metrics.mono_diff_ratio > .8f && frame_metrics.mono_zero_cross_rate > .30f;
  if (voip_style && st->channels == 1 && (st->voip_noise_confidence_Q7 >= voip_noise_confidence_apply_Q7 || severe_voip_noise)) {
    st->mode = st->bitrate_bps <= 24000 ? opus_mode_silk_only : st->bitrate_bps <= 48000 ? opus_mode_hybrid : st->mode;
  }
  if (voip_style && st->silk_mode.useInBandFEC && st->silk_mode.packetLossPercentage > 0 && st->mode == opus_mode_celt_only &&
      st->lightweight_analysis_frames >= fec_mode_settle_frames && frame_size >= st->Fs / 100 &&
      st->bitrate_bps <= std::array{32000, 48000}[static_cast<std::size_t>(st->channels - 1)]) {
    st->mode = opus_mode_silk_only;
  }
  const bool refine_lowrate_voip_celt = st->use_vbr && voip_style && st->channels == 1 && st->silk_mode.complexity >= 5 &&
                                        st->bitrate_bps <= 16000 && frame_size == st->Fs / 50 && st->mode == opus_mode_celt_only &&
                                        st->audio_preprocess_mode == preprocess_lowrate_voip_celt;
  celt_enc->lowrate_refinement = refine_lowrate_voip_celt;
  if (st->mode != opus_mode_celt_only && frame_size < st->Fs / 100) {
    st->mode = opus_mode_celt_only;
  }
  if (st->prev_mode > 0 && (st->mode == opus_mode_celt_only) != (st->prev_mode == opus_mode_celt_only)) {
    redundancy = 1;
    celt_to_silk = (st->mode != opus_mode_celt_only);
    if (!celt_to_silk) {
      if (frame_size >= st->Fs / 100) {
        st->mode = st->prev_mode;
        to_celt = 1;
      } else {
        redundancy = 0;
      }
    }
  }
  if (st->stream_channels == 1 && st->prev_channels == 2 && st->silk_mode.toMono == 0 && st->mode != opus_mode_celt_only &&
      st->prev_mode != opus_mode_celt_only) {
    st->silk_mode.toMono = 1;
    st->stream_channels = 2;
  } else {
    st->silk_mode.toMono = 0;
  }
  equiv_rate = compute_equiv_rate(st->bitrate_bps, st->stream_channels, frame_rate, st->use_vbr, st->mode, st->silk_mode.complexity);
  if (st->mode != opus_mode_celt_only && st->prev_mode == opus_mode_celt_only) {
    reset_encoder_silk_state(st);
    prefill = 1;
  }
  if (st->mode == opus_mode_celt_only || first || st->silk_mode.allowBandwidthSwitch) {
    int bandwidth = 1105;
    for (; bandwidth > 1101; --bandwidth) {
      const auto threshold_slot = static_cast<std::size_t>(2 * (bandwidth - 1102));
      int threshold = quality_bandwidth_threshold(voice_weight, music_bandwidth_thresholds_common[threshold_slot],
                                                  voice_bandwidth_thresholds_common[threshold_slot]);
      const int hysteresis = quality_bandwidth_threshold(voice_weight, music_bandwidth_thresholds_common[threshold_slot + 1],
                                                         voice_bandwidth_thresholds_common[threshold_slot + 1]);
      if (!first) {
        threshold += st->auto_bandwidth >= bandwidth ? -hysteresis : hysteresis;
      }
      if (equiv_rate >= threshold) {
        break;
      }
    }
    if (bandwidth == 1102) {
      bandwidth = 1103;
    }
    st->bandwidth = st->auto_bandwidth = bandwidth;
    if (!first && st->mode != opus_mode_celt_only && !st->silk_mode.inWBmodeWithoutVariableLP && st->bandwidth > 1103) {
      st->bandwidth = 1103;
    }
  }
  if (st->mode != opus_mode_celt_only && bits_to_bitrate_for_frame_rate(max_data_bytes * 8, frame_rate) < 15000) {
    st->bandwidth = std::min(st->bandwidth, 1103);
  }
  const auto sample_rate_bandwidth = 1101 + (st->Fs >= 12000) + (st->Fs >= 16000) + (st->Fs >= 24000) + (st->Fs >= 48000);
  st->bandwidth = std::min(st->bandwidth, sample_rate_bandwidth);
  if (st->mode != opus_mode_celt_only && st->Fs >= 24000) {
    const opus_int32 hybrid_floor_bps = voice_est >= 100 && st->lightweight_harmonic_music_Q7 < 80 ? 14000
                                        : st->channels == 2                                        ? 12000
                                                                                                   : 13000;
    if (st->bitrate_bps < hybrid_floor_bps) {
      st->bandwidth = std::min(st->bandwidth, 1103);
    } else if (st->bandwidth <= 1103)
      st->bandwidth = 1104;
    const opus_int32 hybrid_fullband_floor_bps = st->stream_channels == 2 ? 32000 : 24000;
    if (st->bitrate_bps < hybrid_fullband_floor_bps) {
      st->bandwidth = std::min(st->bandwidth, 1104);
    }
  }
  celt_enc->lsb_depth = lsb_depth;
  celt_enc->midrate_quality_boost_bps = st->application == OPUS_APPLICATION_AUDIO && st->mode == opus_mode_celt_only &&
                                                frame_size == st->Fs / 50 && st->bitrate_bps >= audio_midrate_celt_quality_boost_min_bps &&
                                                st->bitrate_bps < audio_midrate_celt_quality_boost_max_bps
                                            ? audio_midrate_celt_quality_boost_bps
                                            : opus_int32{0};
  if (st->application == OPUS_APPLICATION_AUDIO && st->mode == opus_mode_celt_only && frame_size == st->Fs / 50 &&
      st->bitrate_bps >= 22000 && st->bitrate_bps < 36000 && st->channels == 2 && frame_metrics.energy > .006f &&
      frame_metrics.mono_diff_ratio > 0 && frame_metrics.mono_diff_ratio < .006f) {
    st->bandwidth = std::min(st->bandwidth, 1104);
  }
  st->silk_mode.LBRR_coded = st->silk_mode.useInBandFEC ? decide_fec(st->silk_mode, st->mode, st->bandwidth, equiv_rate) : 0;
  if (st->bandwidth == 1102 && st->mode == opus_mode_celt_only) {
    st->bandwidth = 1103;
  } else if (st->bandwidth > 1103 && st->mode == opus_mode_silk_only) {
    st->mode = opus_mode_hybrid;
  } else if (st->bandwidth <= 1103 && st->mode == opus_mode_hybrid) {
    st->mode = opus_mode_silk_only;
  }
  if ((frame_size > st->Fs / 50 && (st->mode != opus_mode_silk_only)) || frame_size > 3 * st->Fs / 50) {
    return encode_multiframe_packet(
        st, pcm, frame_size, data, out_data_bytes,
        {float_api, governed_vbr, lsb_depth, redundancy, celt_to_silk, to_celt, prefill, equiv_rate, cbr_bytes, requested_frame_bits},
        stage_buffer_storage);
  }
  auto stage_storage = make_encoder_stage_storage(stage_buffer_storage.data(), st, encoder_delay_compensation(st), frame_size);
  stage_storage.prime_from_encoder(st);
  if (governed_vbr) {
    max_data_bytes = std::min(max_data_bytes, frame_budget.max_bytes);
  }
  const bool emit_dtx = st->use_dtx && should_emit_dtx(st, frame_metrics, frame_size);
  const opus_int32 allocator_target_bits = governed_vbr ? frame_budget.allocator_bits : 0;
  auto ret =
      opus_encode_frame_native(st, pcm, frame_size, data, max_data_bytes, allocator_target_bits, float_api, frame_metrics, redundancy,
                               celt_to_silk, prefill, equiv_rate, to_celt, false, emit_dtx && st->mode == opus_mode_hybrid, stage_storage);
  ret = finalize_dtx_packet(st, data, ret, emit_dtx);
  if (encoder_is_in_dtx(st)) {
    reset_vbr_budget(st);
  } else if (governed_vbr && ret > 0) {
    st->vbr_budget_reservoir_bits = update_vbr_credit(st->vbr_budget_reservoir_bits, ret, requested_frame_bits);
  }
  return ret;
}

static void opus_prepare_frame_highpass(OpusEncoder* st, void* silk_enc, const opus_res* pcm, opus_res* frame_pcm, int frame_size,
                                        const frame_activity_metrics& frame_metrics) {
  if (st->application == OPUS_APPLICATION_VOIP) {
    if (st->preprocess_filter_state == preprocess_filter_quiet_voice) {
      dc_reject(pcm, frame_pcm, st->audio_speech_hp_mem, frame_size, st->channels, st->Fs);
    } else {
      const int hp_freq_smth1 = st->mode == opus_mode_celt_only
                                    ? silk_log_60_q15
                                    : silk_encoder_channel_states(static_cast<silk_encoder*>(silk_enc))[0].sCmn.variable_HP_smth1_Q15;
      st->variable_HP_smth2_Q15 += silk_mul_wb(hp_freq_smth1 - st->variable_HP_smth2_Q15, fixed_q<16>(0.015f));
      const int cutoff_Hz = silk_log2lin(((st->variable_HP_smth2_Q15) >> (8)));
      hp_cutoff(pcm, cutoff_Hz, frame_pcm, st->hp_mem, frame_size, st->channels, st->Fs);
    }
    if (st->channels == 1) {
      auto low_band_keep = opus_val16{0};
      if (st->bitrate_bps <= 16000) {
        const auto diff = frame_metrics.mono_diff_ratio;
        low_band_keep = st->audio_preprocess_mode == preprocess_lowrate_voip_continuous ? .25f
                        : diff >= .015f && diff < .055f                                 ? voip_mid_diff_voice_low_band_keep
                                                                                        : 0.f;
      } else if (st->bitrate_bps <= 64000 && !st->use_dtx && st->preprocess_filter_state != preprocess_filter_quiet_voice) {
        low_band_keep = .30f;
      }
      if (st->bitrate_bps >= voip_voice_low_band_keep_min_bps && st->bitrate_bps < voip_noisy_voice_low_band_min_bps &&
          frame_metrics.energy < voip_noisy_voice_energy_max && frame_metrics.mono_diff_ratio > voip_quiet_hissy_voice_diff_ratio_min) {
        low_band_keep = voip_quiet_hissy_voice_low_band_keep;
      }
      if (low_band_keep > 0) {
        blend_filtered_input(frame_pcm, pcm, frame_size, low_band_keep);
        if (st->bitrate_bps > 16000) {
          dc_reject(frame_pcm, frame_pcm, st->audio_speech_hp_mem, frame_size, 1, st->Fs);
        }
      }
      if (st->bitrate_bps >= voip_voice_low_band_keep_min_bps && st->bitrate_bps <= 24000 &&
          frame_metrics.mono_diff_ratio > voip_noisy_voice_diff_ratio_min &&
          frame_metrics.mono_zero_cross_rate > voip_noisy_voice_zero_cross_min) {
        apply_previous_sample_tilt(frame_pcm, frame_size, 1, voip_noisy_voice_smoothing);
      }
      if (st->audio_preprocess_mode == preprocess_lowrate_voip_continuous && frame_metrics.energy > .004f && frame_metrics.energy < .015f &&
          frame_metrics.mono_diff_ratio > .04f && frame_metrics.mono_diff_ratio < .18f && frame_metrics.mono_zero_cross_rate < .12f) {
        apply_previous_sample_tilt(frame_pcm, frame_size, 1, -.020f);
      }
    }
  } else if (st->application == OPUS_APPLICATION_AUDIO) {
    const bool low_z_mono = st->channels == 1 && frame_metrics.energy > 1e-4f && frame_metrics.mono_diff_ratio < .001f &&
                            frame_metrics.mono_zero_cross_rate < .005f;
    const bool quiet_tonal_stereo = st->channels == 2 && st->bitrate_bps < 20000 && frame_metrics.energy > 5e-5f &&
                                    frame_metrics.energy < .0008f && frame_metrics.mono_diff_ratio > .015f &&
                                    frame_metrics.mono_diff_ratio < .12f && frame_metrics.mono_zero_cross_rate > .025f &&
                                    frame_metrics.mono_zero_cross_rate < .12f;
    if (st->bitrate_bps < 20000) {
      if (st->preprocess_filter_state >= tonal_confirmation_frames) {
        --st->preprocess_filter_state;
      } else if (quiet_tonal_stereo) {
        st->preprocess_filter_state =
            st->preprocess_filter_state == tonal_confirmation_frames - 1 ? quiet_tonal_bypass_hold_frames : st->preprocess_filter_state + 1;
      } else {
        st->preprocess_filter_state = 0;
      }
    }
    const bool bypass_music_hp =
        low_z_mono || st->preprocess_filter_state > 0 ||
        (st->channels == 2 && ((st->mode == opus_mode_celt_only && (st->bitrate_bps < 20000 || st->bitrate_bps >= 128000)) ||
                               (st->bitrate_bps >= 20000 && st->bitrate_bps < 80000 && st->lightweight_high_z_tonal_Q7 > 64 &&
                                is_sparse_high_z_tonal_frame(frame_metrics))));
    if (bypass_music_hp) {
      copy_n_items(pcm, static_cast<std::size_t>(frame_size * st->channels), frame_pcm);
    } else if (choose_audio_preprocess_mode(st) == audio_preprocess_speech) {
      dc_reject(pcm, frame_pcm, st->audio_speech_hp_mem, frame_size, st->channels, st->Fs);
    } else {
      hp_cutoff(pcm, 3, frame_pcm, st->audio_music_hp_mem, frame_size, st->channels, st->Fs);
      if (st->channels == 1 && st->bitrate_bps >= 20000 && st->bitrate_bps <= 36000) {
        const bool clean_music = st->bitrate_bps < 28000 && st->lightweight_music_score_Q7 > 16 && st->lightweight_voice_score_Q7 == 0;
        blend_filtered_input(frame_pcm, pcm, frame_size, clean_music ? .94f : .86f);
        if (clean_music) {
          apply_previous_sample_tilt(frame_pcm, frame_size, 1, .08f);
        }
      }
    }
  } else {
    dc_reject(pcm, frame_pcm, st->hp_mem, frame_size, st->channels, st->Fs);
  }
  const auto gain = encoder_error_balance_filter_for(st, frame_metrics);
  const bool final_audio_tilt = st->application == OPUS_APPLICATION_AUDIO && st->channels == 1 && st->bitrate_bps >= 28000 &&
                                st->bitrate_bps <= 40000 && st->lightweight_high_z_tonal_Q7 < 64;
  if (final_audio_tilt) {
    apply_previous_sample_tilt(frame_pcm, frame_size, 1, -.035f, gain);
  } else if (gain != 1.0f) {
    apply_previous_sample_tilt(frame_pcm, frame_size, st->channels, 0, gain);
  }
}

static opus_int32 opus_encode_frame_native(OpusEncoder* st, const opus_res* pcm, int frame_size, unsigned char* data,
                                           opus_int32 orig_max_data_bytes, opus_int32 allocator_target_bits, bool float_api,
                                           const frame_activity_metrics& metrics, int redundancy, int celt_to_silk, int prefill,
                                           opus_int32 equiv_rate, int to_celt, bool nonfinal_frame, bool skip_celt_for_dtx,
                                           encoder_stage_storage& stage_storage) {
  int ret = 0, redundancy_bytes = 0, nb_compr_bytes;
  opus_int32 nBytes = 0;
  ec_enc enc;
  opus_uint32 redundant_rng = 0;
  auto celt_pcm = stage_storage.active_celt_window();
  auto frame_pcm = stage_storage.active_frame();
  const int max_data_bytes = std::min<opus_int32>(orig_max_data_bytes, 1276);
  const int frame_rate = st->Fs / frame_size;
  st->rangeFinal = 0;
  void* silk_enc = encoder_uses_silk(st->application) ? encoder_silk_state(st) : nullptr;
  auto* celt_enc = encoder_celt_state(st);
  opus_int32 voip_silk_boost = 0;
  auto refresh_redundancy = [&] {
    redundancy_bytes = compute_redundancy_bytes(max_data_bytes, st->bitrate_bps, frame_rate, st->stream_channels);
    redundancy = redundancy_bytes != 0;
  };
  auto configure_redundant_celt = [&](bool disable_prediction) {
    celt_enc->start = 0;
    if (disable_prediction) {
      celt_enc->prediction_disabled = true;
    }
    celt_enc->vbr = 0;
    celt_enc->bitrate = -1;
  };
  int curr_bandwidth = st->bandwidth;
  if (st->silk_bw_switch) {
    redundancy = 1;
    celt_to_silk = 1;
    st->silk_bw_switch = 0;
    prefill = 2;
  }
  if (st->mode == opus_mode_celt_only || skip_celt_for_dtx) {
    redundancy = 0;
  }
  if (redundancy) {
    refresh_redundancy();
  }
  const opus_int32 nominal_target_bits = bitrate_to_bits_for_frame_rate(st->bitrate_bps, frame_rate);
  allocator_target_bits = allocator_target_bits > 0 ? allocator_target_bits : nominal_target_bits;
  const opus_int32 silk_target_bits = st->mode == opus_mode_hybrid ? nominal_target_bits : allocator_target_bits;
  const int bits_target = std::min(8 * (max_data_bytes - redundancy_bytes), silk_target_bits) - 8;
  const opus_int32 allocator_bitrate_bps = bits_to_bitrate_for_frame_rate(allocator_target_bits, frame_rate);
  data += 1;
  ec_enc_init(&enc, data, orig_max_data_bytes - 1);
  opus_prepare_frame_highpass(st, silk_enc, pcm, frame_pcm.data(), frame_size, metrics);
  if (st->use_dtx && metrics.energy <= dtx_digital_silence_max_energy && metrics.mono_zero_cross_rate == 0) [[unlikely]] {
    zero_n_items(frame_pcm.data(), static_cast<std::size_t>(frame_size * st->channels));
  }
  if (float_api) {
    const auto max_safe_input_energy = 1e9f / (frame_size * st->channels);
    if (!std::isfinite(metrics.energy) || metrics.energy >= max_safe_input_energy) {
      zero_n_items(frame_pcm.data(), static_cast<std::size_t>(frame_size * st->channels));
      st->hp_mem[0] = st->hp_mem[1] = st->hp_mem[2] = st->hp_mem[3] = 0;
      zero_n_items(st->audio_speech_hp_mem, 4);
      zero_n_items(st->audio_music_hp_mem, 4);
    }
  }
  opus_val16 HB_gain = 1.0f;
  if (st->mode != opus_mode_celt_only) {
    voip_silk_boost = voip_mono_silk_budget_boost(st);
    const opus_int32 total_bitRate = bits_to_bitrate_for_frame_rate(bits_target, frame_rate);
    if (st->mode == opus_mode_hybrid) {
      st->silk_mode.bitRate =
          compute_silk_rate_for_hybrid(total_bitRate, curr_bandwidth, st->use_vbr, st->stream_channels, st->silk_mode.LBRR_coded != 0);
      if (st->use_vbr) {
        st->silk_mode.bitRate = hybrid_silk_lowrate_boost_bps(st->bitrate_bps, st->silk_mode.bitRate);
      }
      if (voip_silk_boost != 0) {
        st->silk_mode.bitRate = std::min<opus_int32>(total_bitRate - 500, st->silk_mode.bitRate + voip_silk_boost);
      }
      const opus_int32 celt_rate = total_bitRate - st->silk_mode.bitRate;
      HB_gain = 1.0f - std::exp2(-celt_rate * (1.f / 1024));
    } else {
      st->silk_mode.bitRate = total_bitRate;
    }
    st->silk_mode.payloadSize_ms = 1000 / frame_rate;
    st->silk_mode.nChannelsAPI = st->channels;
    st->silk_mode.nChannelsInternal = st->stream_channels;
    st->silk_mode.desiredInternalSampleRate = curr_bandwidth == 1101 ? 8000 : curr_bandwidth == 1102 ? 12000 : 16000;
    st->silk_mode.minInternalSampleRate = st->mode == opus_mode_hybrid ? 16000 : 8000;
    st->silk_mode.maxInternalSampleRate = 16000;
    if (st->mode == opus_mode_silk_only) {
      opus_int32 effective_max_rate = bits_to_bitrate_for_frame_rate(max_data_bytes * 8, frame_rate);
      if (frame_rate > 50) {
        effective_max_rate = effective_max_rate * 2 / 3;
      }
      st->silk_mode.maxInternalSampleRate = effective_max_rate < 7000 ? 8000 : effective_max_rate < 8000 ? 12000 : 16000;
      st->silk_mode.desiredInternalSampleRate = std::min(st->silk_mode.desiredInternalSampleRate, st->silk_mode.maxInternalSampleRate);
    }
    st->silk_mode.useCBR = !st->use_vbr;
    st->silk_mode.maxBits = (max_data_bytes - 1) * 8;
    if (redundancy && redundancy_bytes >= 2) {
      st->silk_mode.maxBits -= redundancy_bytes * 8 + 1;
      if (st->mode == opus_mode_hybrid) {
        st->silk_mode.maxBits -= 20;
      }
    }
    if (st->mode == opus_mode_hybrid) {
      if (st->silk_mode.useCBR) {
        const auto reserved_bits = st->silk_mode.bitRate * frame_size / st->Fs;
        const auto other_bits = std::max<opus_int32>(0, st->silk_mode.maxBits - reserved_bits);
        st->silk_mode.maxBits = std::max<opus_int32>(0, st->silk_mode.maxBits - other_bits * 3 / 4);
        st->silk_mode.useCBR = 0;
      } else {
        opus_int32 maxBitRate = compute_silk_rate_for_hybrid(st->silk_mode.maxBits * frame_rate, curr_bandwidth, st->use_vbr,
                                                             st->stream_channels, st->silk_mode.LBRR_coded != 0);
        if (voip_silk_boost != 0) {
          maxBitRate = std::min<opus_int32>(st->silk_mode.maxBits * frame_rate, maxBitRate + voip_silk_boost);
        }
        st->silk_mode.maxBits = bitrate_to_bits_for_frame_rate(maxBitRate, frame_rate);
      }
    }
    if (prefill) {
      opus_int32 zero = 0;
      int prefill_offset = st->channels * (st->encoder_buffer - st->delay_compensation - st->Fs / 400);
      gain_fade(stage_storage.active.data() + prefill_offset, stage_storage.active.data() + prefill_offset, 0, 1.0f, st->Fs / 400,
                st->channels, st->Fs);
      zero_n_items(stage_storage.active.data(), static_cast<std::size_t>(prefill_offset));
      silk_Encode(silk_enc, &st->silk_mode, stage_storage.active.data(), st->encoder_buffer, nullptr, &zero, prefill);
      st->silk_mode.opusCanSwitch = 0;
    }
    silk_Encode(silk_enc, &st->silk_mode, frame_pcm.data(), frame_size, &enc, &nBytes, 0);
    if (st->mode == opus_mode_silk_only) {
      curr_bandwidth = st->silk_mode.internalSampleRate == 8000 ? 1101 : st->silk_mode.internalSampleRate == 12000 ? 1102 : 1103;
    } else
      st->silk_mode.opusCanSwitch = st->silk_mode.switchReady && !nonfinal_frame;
    if (nBytes == 0) {
      stage_storage.finish_frame(st, nonfinal_frame);
      st->rangeFinal = 0;
      data[-1] = gen_toc(st->mode, frame_rate, curr_bandwidth, st->stream_channels);
      return 1;
    }
    if (st->silk_mode.opusCanSwitch) {
      refresh_redundancy();
      celt_to_silk = 0;
      st->silk_bw_switch = 1;
    }
    if (skip_celt_for_dtx) {
      redundancy = 0;
      redundancy_bytes = 0;
    }
  }
  celt_enc->end = bandwidth_to_endband(curr_bandwidth);
  celt_enc->stream_channels = st->stream_channels;
  celt_enc->bitrate = -1;
  if (st->mode != opus_mode_silk_only) {
    celt_enc->prediction_disabled = false;
  }
  auto transition_prefill = std::span<opus_res>{};
  if (st->mode != opus_mode_silk_only && st->mode != st->prev_mode && st->prev_mode > 0 && silk_enc != nullptr) {
    transition_prefill = stage_storage.active_celt_prefill(st);
  }
  stage_storage.finish_frame(st, nonfinal_frame);
  if (st->prev_HB_gain < 1.0f || HB_gain < 1.0f) {
    gain_fade(celt_pcm.data(), celt_pcm.data(), st->prev_HB_gain, HB_gain, frame_size, st->channels, st->Fs);
  }
  st->prev_HB_gain = HB_gain;
  if (st->mode != opus_mode_hybrid || st->stream_channels == 1)
    st->silk_mode.stereoWidth_Q14 = equiv_rate > 32000 ? 16384
                                    : equiv_rate < 16000
                                        ? 0
                                        : 16384 - 2048 * static_cast<opus_int32>(32000 - equiv_rate) / (equiv_rate - 14000);
  if (st->channels == 2 && (st->hybrid_stereo_width_Q14 < (1 << 14) || st->silk_mode.stereoWidth_Q14 < (1 << 14))) {
    opus_val16 g1 = st->hybrid_stereo_width_Q14 * (1.f / 16384);
    opus_val16 g2 = static_cast<opus_val16>(st->silk_mode.stereoWidth_Q14) * (1.f / 16384);
    stereo_fade(celt_pcm.data(), celt_pcm.data(), g1, g2, frame_size, st->channels, st->Fs);
    st->hybrid_stereo_width_Q14 = st->silk_mode.stereoWidth_Q14;
  }
  if (st->mode != opus_mode_celt_only && ec_tell(&enc) + 17 + 20 * (st->mode == opus_mode_hybrid) <= 8 * (max_data_bytes - 1)) {
    if (st->mode == opus_mode_hybrid) {
      ec_enc_bit_logp(&enc, redundancy, 12);
    }
    if (redundancy) {
      ec_enc_bit_logp(&enc, celt_to_silk, 1);
      const int max_redundancy = st->mode == opus_mode_hybrid ? (max_data_bytes - 1) - ((ec_tell(&enc) + 18) >> 3)
                                                              : (max_data_bytes - 1) - ((ec_tell(&enc) + 7) >> 3);
      redundancy_bytes = std::min(max_redundancy, redundancy_bytes);
      redundancy_bytes = clamp_value(redundancy_bytes, 2, 257);
      if (st->mode == opus_mode_hybrid) {
        ec_enc_uint(&enc, redundancy_bytes - 2, 256);
      }
    }
  } else {
    redundancy = 0;
  }
  if (!redundancy) {
    st->silk_bw_switch = 0;
    redundancy_bytes = 0;
  }
  if (st->mode == opus_mode_silk_only) {
    ret = (ec_tell(&enc) + 7) >> 3;
    ec_enc_done(&enc);
    nb_compr_bytes = ret;
  } else {
    nb_compr_bytes = (max_data_bytes - 1) - redundancy_bytes;
    ec_enc_shrink(&enc, nb_compr_bytes);
  }
  if (st->mode == opus_mode_hybrid) {
    celt_enc->silk_info = {st->silk_mode.offset, allocator_bitrate_bps,
                           bits_to_bitrate_for_frame_rate(static_cast<int>(nBytes) * 8, frame_rate)};
  }
  if (redundancy && celt_to_silk) {
    configure_redundant_celt(false);
    const int err = celt_encode_with_ec(celt_enc, celt_pcm.data(), st->Fs / 200, data + nb_compr_bytes, redundancy_bytes, nullptr);
    if (err < 0) {
      return -3;
    }
    redundant_rng = celt_enc->rng;
    celt_encoder_reset_state(celt_enc);
  }
  celt_enc->start = st->mode == opus_mode_celt_only ? 0 : 17;
  celt_enc->content_vbr = false;
  if (st->mode != opus_mode_silk_only) {
    celt_enc->vbr = st->use_vbr;
    if (st->mode == opus_mode_hybrid) {
      if (st->use_vbr) {
        opus_int32 celt_vbr_bps = allocator_bitrate_bps - st->silk_mode.bitRate;
        if (voip_silk_boost != 0) {
          const opus_int32 remaining_bits = std::max<opus_int32>(0, 8 * nb_compr_bytes - ec_tell(&enc));
          celt_vbr_bps = std::max(celt_vbr_bps, bits_to_bitrate_for_frame_rate(remaining_bits, frame_rate));
        }
        celt_enc->bitrate = std::min<opus_int32>(std::max<opus_int32>(500, celt_vbr_bps), 750000 * celt_enc->channels);
        celt_enc->constrained_vbr = 0;
      }
    } else if (st->use_vbr) {
      const int celt_lm = std::countr_zero(static_cast<unsigned>(frame_size * celt_enc->upsample / celt_short_mdct_size));
      celt_enc->constrained_vbr = st->vbr_constraint;
      celt_enc->content_vbr = st->application == OPUS_APPLICATION_AUDIO && st->bandwidth == 1105 && st->bitrate_bps < 56000 &&
                              nominal_target_bits >= 8 * (30 + 5 * celt_lm);
      celt_enc->bitrate = std::min<opus_int32>(allocator_bitrate_bps, 750000 * celt_enc->channels);
    }
    if (st->mode != st->prev_mode && st->prev_mode > 0 && encoder_uses_silk(st->application)) {
      unsigned char dummy[2];
      celt_encoder_reset_state(celt_enc);
      celt_encode_with_ec(celt_enc, transition_prefill.data(), st->Fs / 400, dummy, 2, nullptr);
      celt_enc->prediction_disabled = true;
    }
    if (!skip_celt_for_dtx && ec_tell(&enc) <= 8 * nb_compr_bytes) {
      ret = celt_encode_with_ec(celt_enc, celt_pcm.data(), frame_size, nullptr, nb_compr_bytes, &enc);
      if (ret < 0) {
        return -3;
      }
      if (redundancy && celt_to_silk && st->mode == opus_mode_hybrid && nb_compr_bytes != ret) {
        move_n_items(data + nb_compr_bytes, static_cast<std::size_t>(redundancy_bytes), data + ret);
        nb_compr_bytes = ret + redundancy_bytes;
      }
    }
    st->rangeFinal = celt_enc->rng;
  } else {
    st->rangeFinal = enc.rng;
  }
  if (redundancy && !celt_to_silk) {
    unsigned char dummy[2];
    const int N2 = st->Fs / 200;
    const int N4 = st->Fs / 400;
    celt_encoder_reset_state(celt_enc);
    configure_redundant_celt(true);
    if (st->mode == opus_mode_hybrid) {
      nb_compr_bytes = ret;
      ec_enc_shrink(&enc, nb_compr_bytes);
    }
    celt_encode_with_ec(celt_enc, celt_pcm.data() + st->channels * (frame_size - N2 - N4), N4, dummy, 2, nullptr);
    const int err = celt_encode_with_ec(celt_enc, celt_pcm.data() + st->channels * (frame_size - N2), N2, data + nb_compr_bytes,
                                        redundancy_bytes, nullptr);
    if (err < 0) {
      return -3;
    }
    redundant_rng = celt_enc->rng;
  }
  data--;
  data[0] = gen_toc(st->mode, frame_rate, curr_bandwidth, st->stream_channels);
  st->rangeFinal ^= redundant_rng;
  st->prev_mode = to_celt ? opus_mode_celt_only : st->mode;
  st->prev_channels = st->stream_channels;
  st->prev_framesize = frame_size;
  if (ec_tell(&enc) > (max_data_bytes - 1) * 8) {
    if (max_data_bytes < 2) {
      return -2;
    }
    data[1] = 0;
    ret = 1;
    st->rangeFinal = 0;
  } else if (st->mode == opus_mode_silk_only && !redundancy) {
    for (; ret > 2 && data[ret] == 0; --ret) {}
  }
  ret += 1 + redundancy_bytes;
  if (!st->use_vbr) {
    if (pad_packet(data, ret, orig_max_data_bytes) != 0) {
      return -3;
    }
    ret = orig_max_data_bytes;
  }
  return ret;
}

template <std::size_t Capacity>
static opus_int32 encode_pcm16_input(OpusEncoder* st, const opus_int16* pcm, int frame_size, unsigned char* data,
                                     opus_int32 max_data_bytes) {
  const auto sample_count = static_cast<std::size_t>(frame_size * st->channels);
  std::array<opus_res, Capacity> input_storage;
  for (std::size_t index = 0; index < sample_count; ++index) {
    input_storage[index] = static_cast<opus_res>(pcm[index] * (1.0f / 32768.0f));
  }
  return encode_native(st, input_storage.data(), frame_size, data, max_data_bytes, 16, false);
}

int opus_encode(OpusEncoder* st, const opus_int16* pcm, int analysis_frame_size, unsigned char* data, int max_data_bytes) noexcept {
  if (st == nullptr || !has_required_storage(pcm, analysis_frame_size) || !has_required_storage(data, max_data_bytes)) {
    return OPUS_BAD_ARG;
  }
  const int frame_size = frame_size_select(analysis_frame_size, st->Fs);
  if (frame_size <= 0) {
    return -1;
  }
  return frame_size <= celt_max_frame_samples
             ? encode_pcm16_input<celt_max_frame_samples * celt_max_channels>(st, pcm, frame_size, data, max_data_bytes)
             : encode_pcm16_input<opus_max_pcm_samples>(st, pcm, frame_size, data, max_data_bytes);
}

int opus_encode(OpusEncoder* st, std::span<const opus_int16> pcm, std::span<unsigned char> packet) noexcept {
  if (st == nullptr || packet.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return OPUS_BAD_ARG;
  }
  const int frame_size = interleaved_frame_size(pcm.size(), st->channels);
  const int packet_size = static_cast<int>(packet.size());
  return frame_size < 0 ? OPUS_BAD_ARG : opus_encode(st, pcm.data(), frame_size, packet.data(), packet_size);
}

int opus_encode_float(OpusEncoder* st, const float* pcm, int analysis_frame_size, unsigned char* data, int out_data_bytes) noexcept {
  if (st == nullptr || !has_required_storage(pcm, analysis_frame_size) || !has_required_storage(data, out_data_bytes)) {
    return OPUS_BAD_ARG;
  }
  const int frame_size = frame_size_select(analysis_frame_size, st->Fs);
  return frame_size <= 0 ? -1 : encode_native(st, pcm, frame_size, data, out_data_bytes, 24, true);
}

static void reset_ref_encoder_state(OpusEncoder* st, CeltEncoderInternal* celt_enc) {
  zero_object_tail(*st, offsetof(OpusEncoder, bitrate_bps));
  zero_n_items(encoder_delay_buffer(st), encoder_delay_buffer_count(st->channels, st->application));
  celt_encoder_reset_state(celt_enc);
  if (encoder_uses_silk(st->application)) {
    reset_encoder_silk_state(st);
  }
  st->stream_channels = st->channels;
  st->hybrid_stereo_width_Q14 = 1 << 14;
  st->prev_HB_gain = 1.0f;
  st->mode = opus_mode_hybrid;
  st->bandwidth = 1105;
  st->variable_HP_smth2_Q15 = silk_log_60_q15;
  st->audio_preprocess_mode = audio_preprocess_music;
  st->audio_preprocess_hold = audio_preprocess_warmup_frames;
}

static int append_packet_frames(packet_frame_set* packet_frames, const unsigned char* data, opus_int32 len) {
  if (len < 1) {
    return -4;
  }
  if (packet_frames->nb_frames == 0) {
    packet_frames->toc = data[0];
    packet_frames->framesize = ref_opus_packet_get_samples_per_frame(data, 8000);
  } else if ((packet_frames->toc & 0xFC) != (data[0] & 0xFC)) {
    return -4;
  }
  const int ret = ref_opus_packet_parse_impl(data, len, packet_frames->frames.data() + packet_frames->nb_frames,
                                             packet_frames->len.data() + packet_frames->nb_frames, nullptr);
  if (ret < 1 || (ret + packet_frames->nb_frames) * packet_frames->framesize > 960) {
    return -4;
  }
  packet_frames->nb_frames += ret;
  return 0;
}

static opus_int32 write_packet_frames(packet_frame_set* packet_frames, unsigned char* data, opus_int32 maxlen, int pad) {
  if (packet_frames->nb_frames <= 0) {
    return -1;
  }
  const int count = packet_frames->nb_frames;
  const auto frame_count = static_cast<std::size_t>(count);
  const auto frame_lengths = std::span<const opus_int16>{packet_frames->len}.first(frame_count);
  const auto frame_data = std::span<const unsigned char* const>{packet_frames->frames}.first(frame_count);
  bool vbr = false;
  opus_int32 payload_size = 0;
  for (const auto length : frame_lengths) {
    payload_size += length;
    vbr |= length != frame_lengths.front();
  }
  const int code = !pad && count == 1 ? 0 : !pad && count == 2 ? (vbr ? 2 : 1) : 3;
  opus_int32 total_size = payload_size + (code == 3 ? 2 : 1);
  if (code == 2) {
    total_size += 1 + (frame_lengths.front() >= 252);
  } else if (code == 3 && vbr) {
    for (const auto length : frame_lengths.first(frame_count - 1)) {
      total_size += 1 + (length >= 252);
    }
  }
  if (total_size > maxlen) {
    return -2;
  }
  auto* ptr = data;
  *ptr++ = static_cast<unsigned char>((packet_frames->toc & 0xFC) | code);
  if (code == 2) {
    ptr += encode_size(frame_lengths.front(), ptr);
  } else if (code == 3) {
    *ptr++ = static_cast<unsigned char>(count | (vbr ? 0x80 : 0));
    const int padding = pad ? maxlen - total_size : 0;
    if (padding > 0) {
      data[1] |= 0x40;
      for (int remaining = padding; remaining > 255; remaining -= 255) {
        *ptr++ = 255;
      }
      *ptr++ = static_cast<unsigned char>((padding - 1) % 255);
      total_size += padding;
    }
    if (vbr) {
      for (const auto length : frame_lengths.first(frame_count - 1)) {
        ptr += encode_size(length, ptr);
      }
    }
  }
  for (std::size_t index = 0; index < frame_count; ++index) {
    move_n_items(frame_data[index], static_cast<std::size_t>(frame_lengths[index]), ptr);
    ptr += frame_lengths[index];
  }
  if (pad) {
    zero_n_items(ptr, static_cast<std::size_t>(data + maxlen - ptr));
  }
  return total_size;
}

static int pad_packet(unsigned char* data, opus_int32 len, opus_int32 new_len) {
  if (len < 1 || len > new_len) {
    return -1;
  }
  if (len == new_len) {
    return 0;
  }
  std::array<unsigned char, opus_max_multiframe_packet_bytes> copy;
  copy_n_items(data, static_cast<std::size_t>(len), copy.data());
  packet_frame_set packet_frames;
  const int ret = append_packet_frames(&packet_frames, copy.data(), len);
  if (ret != 0) {
    return ret;
  }
  const auto length = write_packet_frames(&packet_frames, data, new_len, true);
  return length > 0 ? 0 : static_cast<int>(length);
}
namespace {
constexpr auto eMeans = numeric_blob_array<opus_val16>(
    R"blob(40CE000040C8000040B8000040AA000040A20000409A000040900000408C0000409C00004096000040920000408E0000409C000040940000408A000040900000408C00004094000040980000408E00004070000040700000407000004070000040700000)blob");
extern constinit const std::array<opus_val16, celt_default_nb_ebands> celt_noise_floor_base;
} // namespace

static inline void quant_coarse_energy(int start, int end, const celt_glog* eBands, celt_glog* oldEBands, opus_uint32 budget,
                                       celt_glog* error, ec_enc* enc, int C, int LM, int nbAvailableBytes, int force_intra,
                                       opus_val32* delayedIntra);
template <bool Encode>
static void process_fine_energy(int start, int end, celt_glog* oldEBands, celt_glog* error, const int* prev_quant, const int* extra_quant,
                                ec_ctx* coder, int C);
template <bool Encode>
static void process_energy_finalise(int start, int end, celt_glog* oldEBands, celt_glog* error, const int* fine_quant,
                                    const int* fine_priority, int bits_left, ec_ctx* coder, int C);
static void unquant_coarse_energy(int start, int end, celt_glog* oldEBands, int intra, ec_dec* dec, int C, int LM);
static int clt_compute_allocation(int start, int end, const int* offsets, const int* cap, int alloc_trim, int* intensity, int* dual_stereo,
                                  opus_int32 total, opus_int32* balance, int* pulses, int* ebits, int* fine_priority, int C, int LM,
                                  ec_ctx* ec, int encode, int prev, int signalBandwidth);

constexpr int celt_bits2pulses_lut_lm_count = 4;
constexpr int celt_bits2pulses_lut_rows = celt_bits2pulses_lut_lm_count * celt_default_nb_ebands;
constexpr int celt_bits2pulses_lut_entries = 15936;

struct celt_bits2pulses_lut_table {
  std::array<opus_uint16, celt_bits2pulses_lut_rows + 1> offsets{};
  std::array<opus_uint8, celt_bits2pulses_lut_entries> values{};
};

namespace {
extern constinit const celt_bits2pulses_lut_table celt_bits2pulses_lut;
}

[[nodiscard]] constexpr auto celt_bits2pulses_search(const unsigned char* cache, int bits) noexcept -> int {
  int lo = 0;
  int hi = cache[0];
  --bits;
  for (int i = 0; i < 6; ++i) {
    const int mid = (lo + hi + 1) >> 1;
    if (static_cast<int>(cache[mid]) >= bits) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  const int low_distance = bits - (lo == 0 ? -1 : static_cast<int>(cache[lo]));
  const int high_distance = static_cast<int>(cache[hi]) - bits;
  return low_distance <= high_distance ? lo : hi;
}

struct celt_pulse_budget {
  int pulses;
  int bits;
};

[[nodiscard]] static auto compute_celt_pulse_budget(const unsigned char* cache, int band, int lm, int bits) noexcept -> celt_pulse_budget {
  ++lm;
  int pulses;
  if (bits >= 0 && lm > 0 && lm <= celt_bits2pulses_lut_lm_count) {
    const auto row = static_cast<std::size_t>((lm - 1) * celt_default_nb_ebands + band);
    const auto begin = celt_bits2pulses_lut.offsets[row];
    const auto end = celt_bits2pulses_lut.offsets[row + 1];
    pulses = static_cast<unsigned>(bits) < static_cast<unsigned>(end - begin)
                 ? celt_bits2pulses_lut.values[begin + static_cast<unsigned>(bits)]
                 : cache[0];
  } else {
    pulses = celt_bits2pulses_search(cache, bits);
  }
  return {pulses, pulses == 0 ? 0 : cache[pulses] + 1};
}

static inline unsigned alg_quant(celt_norm* X, int N, int K, int spread, int B, ec_enc* enc);
static unsigned alg_unquant(celt_norm* X, int N, int K, int spread, int B, ec_dec* dec, opus_val32 gain, opus_int16* iy);
static void renormalise_vector(celt_norm* X, int N, opus_val32 gain);
static opus_int32 stereo_itheta(const celt_norm* X, const celt_norm* Y, int stereo, int N);
static int hysteresis_decision(int val, std::span<const opus_uint8> thresholds, std::span<const opus_uint8> hysteresis, int prev) {
  int i = 0;
  while (i < static_cast<int>(thresholds.size()) && val >= thresholds[static_cast<std::size_t>(i)]) {
    ++i;
  }
  if (i > prev && val < thresholds[prev] + hysteresis[prev]) {
    i = prev;
  }
  if (i < prev && val > thresholds[prev - 1] - hysteresis[prev - 1]) {
    i = prev;
  }
  return i;
}

static opus_uint32 celt_lcg_rand(opus_uint32 seed) {
  return 1664525 * seed + 1013904223;
}

static void negate_n(celt_norm* data, int count) {
  for (int index = 0; index < count; ++index) {
    data[index] = -data[index];
  }
}

static opus_int16 bitexact_cos(opus_int16 x) {
  const auto x32 = static_cast<opus_int32>(x);
  auto x2 = static_cast<opus_int16>((4096 + x32 * x32) >> 13);
  const auto inner = static_cast<opus_int16>(8277 + ((16384 + static_cast<opus_int32>(static_cast<opus_int16>(-626)) * x2) >> 15));
  const auto outer = static_cast<opus_int16>(-7651 + ((16384 + static_cast<opus_int32>(x2) * inner) >> 15));
  x2 = static_cast<opus_int16>((32767 - x2) + ((16384 + static_cast<opus_int32>(x2) * outer) >> 15));
  return 1 + x2;
}

[[nodiscard]] constexpr auto bitexact_log2tan_poly(int value) noexcept -> int {
  const auto x = static_cast<opus_int16>(value);
  const auto inner = static_cast<opus_int16>(((16384 + static_cast<opus_int32>(x) * static_cast<opus_int16>(-2597)) >> 15) + 7932);
  return (16384 + static_cast<opus_int32>(x) * inner) >> 15;
}

static int bitexact_log2tan(int isin, int icos) {
  const int lc = std::bit_width(static_cast<opus_uint32>(icos));
  const int ls = std::bit_width(static_cast<opus_uint32>(isin));
  icos <<= 15 - lc;
  isin <<= 15 - ls;
  return (ls - lc) * (1 << 11) + bitexact_log2tan_poly(isin) - bitexact_log2tan_poly(icos);
}

static void compute_band_energies_and_normalise(celt_sig* X, celt_ener* bandE, celt_glog* bandLogE, int start, int end, int C, int LM) {
  const opus_int16* eBands = celt_mode()->eBands;
  const int nbEBands = celt_default_nb_ebands;
  const int M = 1 << LM;
  const int N = M * celt_short_mdct_size;
  const int analysis_prefix = C == 2 ? celt_stereo_analysis_bands : 0;
  for (int c = 0; c < C; ++c) {
    const int channel_offset = c * N;
    const int energy_offset = c * nbEBands;
    for (int i = 0; i < end; ++i) {
      const int band_begin = M * eBands[i];
      const int band_width = M * (eBands[i + 1] - eBands[i]);
      auto* band = X + channel_offset + band_begin;
      const opus_val32 sum = 1e-27f + celt_inner_prod_c(band, band, band_width);
      bandE[i + energy_offset] = std::sqrt(sum);
      bandLogE[i + energy_offset] =
          static_cast<float>(1.442695040888963387 * std::log(static_cast<double>(bandE[i + energy_offset]))) - eMeans[i];
      if (i < analysis_prefix || i >= start) {
        const opus_val16 gain = 1.f / (1e-27f + bandE[i + energy_offset]);
        for (int j = 0; j < band_width; ++j) {
          band[j] *= gain;
        }
      }
    }
  }
}

template <std::size_t Channels>
static void denormalise_bands(std::array<const celt_norm*, Channels> normalized, std::array<celt_sig*, Channels> frequency,
                              std::array<const celt_glog*, Channels> band_log_energy, int start, int end, int M, int downsample,
                              int silence) {
  const opus_int16* eBands = celt_mode()->eBands;
  const int N = M * celt_short_mdct_size;
  int bound = M * eBands[end];
  if (downsample != 1) {
    bound = std::min(bound, N / downsample);
  }
  if (silence) {
    for (auto* channel : frequency) {
      zero_n_items(channel, static_cast<std::size_t>(N));
    }
    return;
  }
  const int prefix = M * eBands[start];
  for (auto* channel : frequency) {
    zero_n_items(channel, static_cast<std::size_t>(prefix));
  }
  for (int i = start; i < end; ++i) {
    const int band_begin = M * eBands[i];
    const int band_end = M * eBands[i + 1];
    std::array<opus_val32, Channels> gain;
    for (std::size_t channel = 0; channel < Channels; ++channel) {
      gain[channel] = exp2f(std::min(32.f, band_log_energy[channel][i] + static_cast<opus_val32>(eMeans[i])));
    }
    for (int j = band_begin; j < band_end; ++j) {
      for (std::size_t channel = 0; channel < Channels; ++channel) {
        frequency[channel][j] = normalized[channel][j] * gain[channel];
      }
    }
  }
  for (auto* channel : frequency) {
    zero_n_items(channel + bound, static_cast<std::size_t>(N - bound));
  }
}

static void anti_collapse(celt_norm* X_, unsigned char* collapse_masks, int LM, int C, int size, int start, int end, const celt_glog* logE,
                          const celt_glog* prev1logE, const celt_glog* prev2logE, const int* pulses, opus_uint32 seed) {
  const auto* eBands = celt_mode()->eBands;
  const int nbEBands = celt_default_nb_ebands;
  for (int i = start; i < end; i++) {
    const int band_start = eBands[i];
    const int N0 = eBands[i + 1] - band_start;
    const int depth = celt_udiv(1 + pulses[i], N0) >> LM;
    const opus_val16 thresh = depth < static_cast<int>(celt_anti_collapse_thresh_by_depth.size())
                                  ? celt_anti_collapse_thresh_by_depth[static_cast<std::size_t>(depth)]
                                  : .5f * static_cast<float>(std::exp(0.6931471805599453094 * (-.125f * depth)));
    const opus_val16 sqrt_1 = 1.f / static_cast<float>(std::sqrt(N0 << LM));
    for (int c = 0; c < C; ++c) {
      const int log_offset = c * nbEBands + i;
      celt_glog prev1 = prev1logE[log_offset], prev2 = prev2logE[log_offset];
      if (C == 1) {
        prev1 = std::max(prev1, prev1logE[nbEBands + i]);
        prev2 = std::max(prev2, prev2logE[nbEBands + i]);
      }
      opus_val32 Ediff = std::max(0.f, logE[log_offset] - std::min(prev1, prev2));
      celt_norm r = std::min(thresh, 2.f * static_cast<float>(std::exp(0.6931471805599453094 * (-Ediff))));
      if (LM == 3) {
        r *= 1.41421356f;
      }
      r *= sqrt_1;
      celt_norm* X = X_ + c * size + (band_start << LM);
      int renormalize = 0;
      for (int k = 0; k < 1 << LM; k++) {
        if (!(collapse_masks[i * C + c] & 1 << k)) {
          for (int j = 0; j < N0; j++) {
            seed = celt_lcg_rand(seed);
            X[(j << LM) + k] = (seed & 0x8000 ? r : -r);
          }
          renormalize = 1;
        }
      }
      if (renormalize) {
        renormalise_vector(X, N0 << LM, 1.0f);
      }
    }
  }
}

static void intensity_stereo(celt_norm* X, const celt_norm* Y, const celt_ener* bandE, int bandID, int N) {
  opus_val16 left = bandE[bandID], right = bandE[bandID + celt_default_nb_ebands];
  opus_val16 norm = 1e-15f + std::sqrt(1e-15f + static_cast<opus_val32>(left) * left + static_cast<opus_val32>(right) * right);
  opus_val16 a1 = static_cast<opus_val32>(left) / norm, a2 = static_cast<opus_val32>(right) / norm;
  for (int j = 0; j < N; j++) {
    X[j] = a1 * X[j] + a2 * Y[j];
  }
}

static inline void stereo_split(celt_norm* X, celt_norm* Y, int N) {
  for (int j = 0; j < N; j++) {
    opus_val32 l = .70710678f * X[j], r = .70710678f * Y[j];
    X[j] = l + r;
    Y[j] = r - l;
  }
}

static void stereo_merge(celt_norm* X, celt_norm* Y, opus_val32 mid, int N) {
  opus_val32 xp = celt_inner_prod_c(Y, X, N), side = celt_inner_prod_c(Y, Y, N);
  xp = mid * xp;
  opus_val32 El = mid * mid + side - 2 * xp, Er = mid * mid + side + 2 * xp;
  if (Er < 6e-4f || El < 6e-4f) {
    copy_n_items(X, static_cast<std::size_t>(N), Y);
    return;
  }
  opus_val32 lgain = 1.f / std::sqrt(El), rgain = 1.f / std::sqrt(Er);
  for (int j = 0; j < N; j++) {
    celt_norm l = mid * X[j], r = Y[j];
    X[j] = lgain * (l - r);
    Y[j] = rgain * (l + r);
  }
}

static constexpr std::array<opus_uint8, 30> ordery_table{1, 0, 3, 0,  2, 1,  7, 0,  4, 3, 6, 1,  5, 2,  15,
                                                         0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5};
static void remap_hadamard(celt_norm* X, int N0, int stride, int hadamard, const bool interleave) {
  if (stride <= 1 || N0 <= 0) {
    return;
  }
  const auto count = static_cast<std::size_t>(N0 * stride);
  std::array<celt_norm, celt_max_band_samples> tmp;
  const opus_uint8* ordery = hadamard ? ordery_table.data() + stride - 2 : nullptr;
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
  copy_n_items(tmp.data(), count, X);
}

static void deinterleave_hadamard(celt_norm* X, int N0, int stride, int hadamard) {
  remap_hadamard(X, N0, stride, hadamard, false);
}

static void haar1(celt_norm* X, int N0, int stride) {
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

static constexpr auto celt_qn_table = numeric_blob_array<opus_uint8>(
    R"blob(01010101020202020202020202040404040404060606060808080A0A0C0C0E0E1012141416181A1E2022262A2E32363A40464C525A626C76808C98A6B6C6D8EA00)blob");
[[nodiscard]] static inline auto celt_quantized_theta_runtime(const int theta, const int qn) noexcept -> int {
  return celt_udiv(static_cast<opus_uint32>(theta) * 16384U, static_cast<opus_uint32>(qn));
}

static int compute_qn(int N, int b, int offset, int pulse_cap, int stereo) {
  int N2 = 2 * N - 1;
  if (stereo && N == 2) {
    N2--;
  }
  auto qb = celt_sudiv(b + N2 * offset, N2);
  qb = std::min(qb, b - pulse_cap - (4 << 3));
  qb = std::min(8 << 3, qb);
  const auto encoded_qn = celt_qn_table[static_cast<std::size_t>(std::max(0, qb))];
  return encoded_qn == 0 ? 256 : encoded_qn;
}

[[nodiscard]] static auto celt_isqrt32_runtime(opus_uint32 value) noexcept -> unsigned {
  unsigned root = 0;
  int shift = (std::bit_width(static_cast<opus_uint32>(value)) - 1) >> 1;
  for (auto bit = 1U << shift; shift >= 0; --shift, bit >>= 1) {
    const auto trial = ((static_cast<opus_uint32>(root) << 1) + bit) << shift;
    if (trial <= value) {
      root += bit;
      value -= trial;
    }
  }
  return root;
}
struct band_ctx {
  int encode, resynth, i, intensity, spread, tf_change, disable_inv, avoid_split_noise;
  ec_ctx* ec;
  opus_int32 remaining_bits;
  const celt_ener* bandE;
  opus_uint32 seed;
  opus_int16* decode_pulse_scratch;
};
struct split_ctx {
  int inv, imid, iside, delta, itheta, qalloc;
};
static void compute_theta(band_ctx* ctx, split_ctx* sctx, celt_norm* X, celt_norm* Y, int N, int* b, int B, int B0, int LM, int stereo,
                          int* fill) {
  int qn, itheta = 0, itheta_q30 = 0;
  int delta, imid, iside;
  int qalloc, pulse_cap, offset;
  opus_int32 tell;
  int inv = 0;
  int i, intensity;
  const int encode = ctx->encode;
  i = ctx->i;
  intensity = ctx->intensity;
  auto* ec = ctx->ec;
  const auto* bandE = ctx->bandE;
  pulse_cap = celt_mode()->logN[i] + LM * (1 << 3);
  offset = (pulse_cap >> 1) - (stereo && N == 2 ? 16 : 4);
  qn = compute_qn(N, *b, offset, pulse_cap, stereo);
  if (stereo && i >= intensity) {
    qn = 1;
  }
  if (encode) {
    itheta_q30 = stereo_itheta(X, Y, stereo, N);
    itheta = itheta_q30 >> 16;
  }
  tell = ec_tell_frac(ec);
  if (qn != 1) {
    if (encode) {
      itheta = (itheta * static_cast<opus_int32>(qn) + 8192) >> 14;
      if (!stereo && ctx->avoid_split_noise && itheta > 0 && itheta < qn) {
        int unquantized = celt_quantized_theta_runtime(itheta, qn);
        imid = bitexact_cos(static_cast<opus_int16>(unquantized));
        iside = bitexact_cos(static_cast<opus_int16>(16384 - unquantized));
        delta = ((16384 + (static_cast<opus_int32>(static_cast<opus_int16>((N - 1) << 7)) *
                           static_cast<opus_int16>(bitexact_log2tan(iside, imid)))) >>
                 15);
        if (delta > *b) {
          itheta = qn;
        } else if (delta < -*b) {
          itheta = 0;
        }
      }
    }
    if (stereo && N > 2) {
      int p0 = 3, x = itheta, x0 = qn / 2, ft = p0 * (x0 + 1) + x0;
      if (encode) {
        ec_encode(ec, x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0, x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0, ft);
      } else {
        int fs = ec_decode(ec, ft);
        if (fs < (x0 + 1) * p0) {
          x = fs / p0;
        } else
          x = x0 + 1 + (fs - (x0 + 1) * p0);
        ec_dec_update(ec, x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0, x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0, ft);
        itheta = x;
      }
    } else if (B0 > 1 || stereo) {
      if (encode) {
        ec_enc_uint(ec, itheta, qn + 1);
      } else
        itheta = ec_dec_uint(ec, qn + 1);
    } else {
      int fs = 1, ft;
      ft = ((qn >> 1) + 1) * ((qn >> 1) + 1);
      if (encode) {
        fs = itheta <= (qn >> 1) ? itheta + 1 : qn + 1 - itheta;
        const int fl = itheta <= (qn >> 1) ? itheta * (itheta + 1) >> 1 : ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
        ec_encode(ec, fl, fl + fs, ft);
      } else {
        int fl = 0;
        int fm = ec_decode(ec, ft);
        if (fm < ((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
          itheta = static_cast<int>((celt_isqrt32_runtime(8 * static_cast<opus_uint32>(fm) + 1) - 1) >> 1);
          fs = itheta + 1;
          fl = itheta * (itheta + 1) >> 1;
        } else {
          const int theta_value = ft - fm - 1;
          const int theta_ceil = static_cast<int>((celt_isqrt32_runtime(8 * static_cast<opus_uint32>(theta_value) + 1) + 1) >> 1);
          itheta = qn + 1 - theta_ceil;
          fs = qn + 1 - itheta;
          fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
        }
        ec_dec_update(ec, fl, fl + fs, ft);
      }
    }
    itheta = celt_quantized_theta_runtime(itheta, qn);
    if (encode && stereo) {
      if (itheta == 0) {
        intensity_stereo(X, Y, bandE, i, N);
      } else
        stereo_split(X, Y, N);
    }
  } else if (stereo) {
    if (encode) {
      inv = itheta > 8192 && !ctx->disable_inv;
      if (inv) {
        negate_n(Y, N);
      }
      intensity_stereo(X, Y, bandE, i, N);
    }
    if (*b > 2 << 3 && ctx->remaining_bits > 2 << 3) {
      if (encode) {
        ec_enc_bit_logp(ec, inv, 2);
      } else
        inv = ec_dec_bit_logp(ec, 2);
    } else
      inv = 0;
    if (ctx->disable_inv) {
      inv = 0;
    }
    itheta = 0;
  }
  qalloc = ec_tell_frac(ec) - tell;
  *b -= qalloc;
  if (itheta == 0) {
    imid = 32767;
    iside = 0;
    *fill &= (1 << B) - 1;
    delta = -16384;
  } else if (itheta == 16384) {
    imid = 0;
    iside = 32767;
    *fill &= ((1 << B) - 1) << B;
    delta = 16384;
  } else {
    imid = bitexact_cos(static_cast<opus_int16>(itheta));
    iside = bitexact_cos(static_cast<opus_int16>(16384 - itheta));
    delta = ((16384 +
              (static_cast<opus_int32>(static_cast<opus_int16>((N - 1) << 7)) * static_cast<opus_int16>(bitexact_log2tan(iside, imid)))) >>
             15);
  }
  *sctx = {inv, imid, iside, delta, itheta, qalloc};
}

static unsigned quant_band_n1(band_ctx* ctx, celt_norm* X, celt_norm* Y, celt_norm* lowband_out) {
  int c;
  celt_norm* x = X;
  const int encode = ctx->encode;
  auto* ec = ctx->ec;
  const int stereo = Y != nullptr;
  for (c = 0; c < 1 + stereo; ++c) {
    int sign = 0;
    if (ctx->remaining_bits >= 1 << 3) {
      if (encode) {
        sign = x[0] < 0;
        ec_enc_bits(ec, sign, 1);
      } else {
        sign = ec_dec_bits(ec, 1);
      }
      ctx->remaining_bits -= 1 << 3;
    }
    if (ctx->resynth) {
      x[0] = sign ? -1.f : 1.f;
    }
    x = Y;
  }
  if (ctx->resynth && lowband_out) {
    lowband_out[0] = X[0];
  }
  return 1;
}

static unsigned quant_partition(band_ctx* ctx, celt_norm* X, int N, int b, int B, celt_norm* lowband, int LM, opus_val32 gain, int fill,
                                const unsigned char* cache) {
  int q, curr_bits;
  int B0 = B;
  unsigned cm = 0;
  celt_norm* Y = nullptr;
  int i, spread;
  const int encode = ctx->encode;
  i = ctx->i;
  spread = ctx->spread;
  auto* ec = ctx->ec;
  if (LM != -1 && b > cache[cache[0]] + 12 && N > 2) {
    int mbits, sbits;
    celt_norm* next_lowband2 = nullptr;
    opus_int32 rebalance;
    N >>= 1;
    Y = X + N;
    LM -= 1;
    const auto* child_cache = celt_mode()->cache_bits + celt_mode()->cache_index[(LM + 1) * celt_default_nb_ebands + i];
    if (B == 1) {
      fill = (fill & 1) | (fill << 1);
    }
    B = (B + 1) >> 1;
    split_ctx split;
    compute_theta(ctx, &split, X, Y, N, &b, B, B0, LM, 0, &fill);
    const auto mid = (1.f / 32768) * split.imid;
    const auto side = (1.f / 32768) * split.iside;
    if (B0 > 1 && (split.itheta & 0x3fff)) {
      if (split.itheta > 8192) {
        split.delta -= split.delta >> (4 - LM);
      } else {
        split.delta = std::min(0, split.delta + ((N << 3) >> (5 - LM)));
      }
    }
    if (lowband) {
      next_lowband2 = lowband + N;
    }
    const int half_delta_bits = (b - split.delta) / 2;
    mbits = std::max(0, std::min(b, half_delta_bits));
    sbits = b - mbits;
    ctx->remaining_bits -= split.qalloc;
    rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
      cm = quant_partition(ctx, X, N, mbits, B, lowband, LM, gain * mid, fill, child_cache);
      rebalance = mbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && split.itheta != 0) {
        sbits += rebalance - (3 << 3);
      }
      cm |= quant_partition(ctx, Y, N, sbits, B, next_lowband2, LM, gain * side, fill >> B, child_cache) << (B0 >> 1);
    } else {
      cm = quant_partition(ctx, Y, N, sbits, B, next_lowband2, LM, gain * side, fill >> B, child_cache) << (B0 >> 1);
      rebalance = sbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && split.itheta != 16384) {
        mbits += rebalance - (3 << 3);
      }
      cm |= quant_partition(ctx, X, N, mbits, B, lowband, LM, gain * mid, fill, child_cache);
    }
  } else {
    const auto pulse_budget = compute_celt_pulse_budget(cache, i, LM, b);
    q = pulse_budget.pulses;
    curr_bits = pulse_budget.bits;
    ctx->remaining_bits -= curr_bits;
    for (; ctx->remaining_bits < 0 && q > 0;) {
      ctx->remaining_bits += curr_bits;
      q--;
      curr_bits = q == 0 ? 0 : cache[q] + 1;
      ctx->remaining_bits -= curr_bits;
    }
    if (q != 0) {
      int K = q < 8 ? q : (8 + (q & 7)) << ((q >> 3) - 1);
      cm = encode ? alg_quant(X, N, K, spread, B, ec) : alg_unquant(X, N, K, spread, B, ec, gain, ctx->decode_pulse_scratch);
    } else {
      int j;
      if (ctx->resynth) {
        const auto cm_mask = static_cast<unsigned>(1UL << B) - 1;
        fill &= cm_mask;
        if (!fill) {
          zero_n_items(X, static_cast<std::size_t>(N));
        } else {
          if (lowband == nullptr) {
            for (j = 0; j < N; j++) {
              ctx->seed = celt_lcg_rand(ctx->seed);
              X[j] = (static_cast<celt_norm>(static_cast<opus_int32>(ctx->seed) >> 20));
            }
            cm = cm_mask;
          } else {
            for (j = 0; j < N; j++) {
              ctx->seed = celt_lcg_rand(ctx->seed);
              opus_val16 tmp = (1.0f / 256);
              tmp = (ctx->seed) & 0x8000 ? tmp : -tmp;
              X[j] = lowband[j] + tmp;
            }
            cm = fill;
          }
          renormalise_vector(X, N, gain);
        }
      }
    }
  }
  return cm;
}

static unsigned quant_band(band_ctx* ctx, celt_norm* X, int N, int b, int B, celt_norm* lowband, int LM, celt_norm* lowband_out,
                           opus_val32 gain, celt_norm* lowband_scratch, int fill) {
  int N0 = N, N_B = N, N_B0, B0 = B, time_divide = 0, recombine = 0, longBlocks;
  unsigned cm = 0;
  int k, encode, tf_change;
  encode = ctx->encode;
  tf_change = ctx->tf_change;
  longBlocks = B0 == 1;
  N_B = celt_udiv(N_B, B);
  if (N == 1) {
    return quant_band_n1(ctx, X, nullptr, lowband_out);
  }
  if (tf_change > 0) {
    recombine = tf_change;
  }
  if (lowband_scratch && lowband && (recombine || ((N_B & 1) == 0 && tf_change < 0) || B0 > 1)) {
    copy_n_items(lowband, static_cast<std::size_t>(N), lowband_scratch);
    lowband = lowband_scratch;
  }
  for (k = 0; k < recombine; k++) {
    if (encode) {
      haar1(X, N >> k, 1 << k);
    }
    if (lowband) {
      haar1(lowband, N >> k, 1 << k);
    }
    fill = bit_interleave_table[fill & 0xF] | bit_interleave_table[fill >> 4] << 2;
  }
  B >>= recombine;
  N_B <<= recombine;
  for (; (N_B & 1) == 0 && tf_change < 0; ++tf_change) {
    if (encode) {
      haar1(X, N_B, B);
    }
    if (lowband) {
      haar1(lowband, N_B, B);
    }
    fill |= fill << B;
    B <<= 1;
    N_B >>= 1;
    time_divide++;
  }
  B0 = B;
  N_B0 = N_B;
  if (B0 > 1) {
    if (encode) {
      deinterleave_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks);
    }
    if (lowband) {
      deinterleave_hadamard(lowband, N_B >> recombine, B0 << recombine, longBlocks);
    }
  }
  const auto* cache = celt_mode()->cache_bits + celt_mode()->cache_index[(LM + 1) * celt_default_nb_ebands + ctx->i];
  cm = quant_partition(ctx, X, N, b, B, lowband, LM, gain, fill, cache);
  if (ctx->resynth) {
    if (B0 > 1) {
      remap_hadamard(X, N_B >> recombine, B0 << recombine, longBlocks, true);
    }
    N_B = N_B0;
    B = B0;
    for (k = 0; k < time_divide; k++) {
      B >>= 1;
      N_B <<= 1;
      cm |= cm >> B;
      haar1(X, N_B, B);
    }
    for (k = 0; k < recombine; k++) {
      cm = bit_deinterleave_table[cm];
      haar1(X, N0 >> k, 1 << k);
    }
    B <<= recombine;
    if (lowband_out) {
      int j;
      opus_val16 n = (static_cast<float>(std::sqrt(((N0)))));
      for (j = 0; j < N0; j++) {
        lowband_out[j] = ((n) * (X[j]));
      }
    }
    cm &= (1 << B) - 1;
  }
  return cm;
}

static unsigned quant_band_stereo(band_ctx* ctx, celt_norm* X, celt_norm* Y, int N, int b, int B, celt_norm* lowband, int LM,
                                  celt_norm* lowband_out, celt_norm* lowband_scratch, int fill) {
  unsigned cm = 0;
  int mbits, sbits;
  const int encode = ctx->encode;
  auto* ec = ctx->ec;
  if (N == 1) {
    return quant_band_n1(ctx, X, Y, lowband_out);
  }
  const int orig_fill = fill;
  if (encode) {
    if (ctx->bandE[ctx->i] < 1e-10f || ctx->bandE[celt_default_nb_ebands + ctx->i] < 1e-10f) {
      if (ctx->bandE[ctx->i] > ctx->bandE[celt_default_nb_ebands + ctx->i]) {
        copy_n_items(X, static_cast<std::size_t>(N), Y);
      } else
        copy_n_items(Y, static_cast<std::size_t>(N), X);
    }
  }
  split_ctx split;
  compute_theta(ctx, &split, X, Y, N, &b, B, B, LM, 1, &fill);
  const auto mid = (1.f / 32768) * split.imid;
  const auto side = (1.f / 32768) * split.iside;
  if (N == 2) {
    int c, sign = 0;
    celt_norm *x2, *y2;
    mbits = b;
    sbits = 0;
    if (split.itheta != 0 && split.itheta != 16384) {
      sbits = 1 << 3;
    }
    mbits -= sbits;
    c = split.itheta > 8192;
    ctx->remaining_bits -= split.qalloc + sbits;
    x2 = c ? Y : X;
    y2 = c ? X : Y;
    if (sbits) {
      if (encode) {
        sign = ((x2[0]) * (y2[1])) - ((x2[1]) * (y2[0])) < 0;
        ec_enc_bits(ec, sign, 1);
      } else {
        sign = ec_dec_bits(ec, 1);
      }
    }
    sign = 1 - 2 * sign;
    cm = quant_band(ctx, x2, N, mbits, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, orig_fill);
    if (ctx->resynth) {
      y2[0] = -sign * x2[1];
      y2[1] = sign * x2[0];
      X[0] = ((mid) * (X[0]));
      X[1] = ((mid) * (X[1]));
      Y[0] = ((side) * (Y[0]));
      Y[1] = ((side) * (Y[1]));
      auto tmp = X[0];
      X[0] = ((tmp) - (Y[0]));
      Y[0] = ((tmp) + (Y[0]));
      tmp = X[1];
      X[1] = ((tmp) - (Y[1]));
      Y[1] = ((tmp) + (Y[1]));
    }
  } else {
    const int half_delta_bits = (b - split.delta) / 2;
    mbits = std::max(0, std::min(b, half_delta_bits));
    sbits = b - mbits;
    ctx->remaining_bits -= split.qalloc;
    auto rebalance = ctx->remaining_bits;
    if (mbits >= sbits) {
      cm = quant_band(ctx, X, N, mbits, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, fill);
      rebalance = mbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && split.itheta != 0) {
        sbits += rebalance - (3 << 3);
      }
      cm |= quant_band(ctx, Y, N, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
    } else {
      cm = quant_band(ctx, Y, N, sbits, B, nullptr, LM, nullptr, side, nullptr, fill >> B);
      rebalance = sbits - (rebalance - ctx->remaining_bits);
      if (rebalance > 3 << 3 && split.itheta != 16384) {
        mbits += rebalance - (3 << 3);
      }
      cm |= quant_band(ctx, X, N, mbits, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, fill);
    }
  }
  if (ctx->resynth) {
    if (N != 2) {
      stereo_merge(X, Y, mid, N);
    }
    if (split.inv) {
      negate_n(Y, N);
    }
  }
  return cm;
}

static void special_hybrid_folding(celt_norm* norm, celt_norm* norm2, int start, int M, int dual_stereo) {
  int n1, n2;
  const opus_int16* eBands = celt_mode()->eBands;
  n1 = M * (eBands[start + 1] - eBands[start]);
  n2 = M * (eBands[start + 2] - eBands[start + 1]);
  copy_n_items(&norm[2 * n1 - n2], static_cast<std::size_t>(n2 - n1), &norm[n1]);
  if (dual_stereo) {
    copy_n_items(&norm2[2 * n1 - n2], static_cast<std::size_t>(n2 - n1), &norm2[n1]);
  }
}

static void quant_all_bands(int encode, int start, int end, celt_norm* X_, celt_norm* Y_, unsigned char* collapse_masks,
                            const celt_ener* bandE, int* pulses, int shortBlocks, int spread, int dual_stereo, int intensity, int* tf_res,
                            opus_int32 total_bits, opus_int32 balance, ec_ctx* ec, int LM, int codedBands, opus_uint32* seed,
                            int disable_inv) {
  int i;
  opus_int32 remaining_bits;
  const opus_int16* eBands = celt_mode()->eBands;
  celt_norm *norm, *norm2;
  celt_norm* lowband_scratch;
  int B, M, lowband_offset;
  int update_lowband = 1;
  int C = Y_ != nullptr ? 2 : 1;
  const int resynth = !encode;
  struct band_ctx ctx;
  M = 1 << LM;
  B = shortBlocks ? M : 1;
  const int norm_offset = M * eBands[start];
  const int norm_size = M * eBands[celt_default_nb_ebands - 1] - norm_offset;
  std::array<celt_norm, celt_max_norm_samples> norm_storage;
  norm = norm_storage.data();
  norm2 = norm + norm_size;
  lowband_scratch = X_ + M * eBands[celt_default_nb_ebands - 1];
  std::array<opus_int16, celt_max_band_samples> decode_pulse_scratch_storage;
  auto* decode_pulse_scratch = !encode ? decode_pulse_scratch_storage.data() : nullptr;
  lowband_offset = 0;
  ctx.bandE = bandE;
  ctx.ec = ec;
  ctx.encode = encode;
  ctx.intensity = intensity;
  ctx.seed = *seed;
  ctx.spread = spread;
  ctx.disable_inv = disable_inv;
  ctx.resynth = resynth;
  ctx.decode_pulse_scratch = decode_pulse_scratch;
  ctx.avoid_split_noise = B > 1;
  const int process_end = encode ? std::min(end, codedBands) : end;
  for (i = start; i < process_end; i++) {
    opus_int32 tell;
    int b, N;
    opus_int32 curr_balance;
    int effective_lowband = -1;
    celt_norm *X, *Y;
    int tf_change = 0;
    unsigned x_cm;
    unsigned y_cm;
    int last;
    ctx.i = i;
    last = (i == end - 1);
    X = X_ + M * eBands[i];
    Y = Y_ != nullptr ? Y_ + M * eBands[i] : nullptr;
    N = M * eBands[i + 1] - M * eBands[i];
    tell = ec_tell_frac(ec);
    if (i != start) {
      balance -= tell;
    }
    remaining_bits = total_bits - tell - 1;
    ctx.remaining_bits = remaining_bits;
    if (i <= codedBands - 1) {
      curr_balance = celt_sudiv(balance, std::min(3, codedBands - i));
      const int raw_bits = std::min(remaining_bits + 1, pulses[i] + curr_balance);
      b = std::max(0, std::min(16383, raw_bits));
    } else {
      b = 0;
    }
    if (resynth) {
      if ((M * eBands[i] - N >= M * eBands[start] || i == start + 1) && (update_lowband || lowband_offset == 0)) {
        lowband_offset = i;
      }
      if (i == start + 1) {
        special_hybrid_folding(norm, norm2, start, M, dual_stereo);
      }
    }
    tf_change = tf_res[i];
    ctx.tf_change = tf_change;
    if (last) {
      lowband_scratch = nullptr;
    }
    if (lowband_offset != 0 && (spread != 3 || B > 1 || tf_change < 0)) {
      int fold_start, fold_end, fold_i;
      effective_lowband = std::max(0, M * eBands[lowband_offset] - norm_offset - N);
      fold_start = lowband_offset;
      for (; M * eBands[--fold_start] > effective_lowband + norm_offset;) {}
      fold_end = lowband_offset - 1;
      for (; ++fold_end < i && M * eBands[fold_end] < effective_lowband + norm_offset + N;) {}
      x_cm = y_cm = 0;
      for (fold_i = fold_start;; ++fold_i) {
        x_cm |= collapse_masks[fold_i * C];
        y_cm |= collapse_masks[fold_i * C + C - 1];
        if (fold_i + 1 >= fold_end) {
          break;
        }
      }
    } else
      x_cm = y_cm = (1 << B) - 1;
    if (dual_stereo && i == intensity) {
      int j;
      dual_stereo = 0;
      if (resynth)
        for (j = 0; j < M * eBands[i] - norm_offset; j++) {
          norm[j] = (.5f * (norm[j] + norm2[j]));
        }
    }
    auto* const lowband = effective_lowband != -1 ? norm + effective_lowband : nullptr;
    auto* const lowband2 = effective_lowband != -1 ? norm2 + effective_lowband : nullptr;
    auto* const lowband_out = last ? nullptr : norm + M * eBands[i] - norm_offset;
    auto* const lowband_out2 = last ? nullptr : norm2 + M * eBands[i] - norm_offset;
    if (dual_stereo) {
      x_cm = quant_band(&ctx, X, N, b / 2, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, x_cm);
      y_cm = quant_band(&ctx, Y, N, b / 2, B, lowband2, LM, lowband_out2, 1.0f, lowband_scratch, y_cm);
    } else {
      if (Y != nullptr) {
        x_cm = quant_band_stereo(&ctx, X, Y, N, b, B, lowband, LM, lowband_out, lowband_scratch, x_cm | y_cm);
      } else {
        x_cm = quant_band(&ctx, X, N, b, B, lowband, LM, lowband_out, 1.0f, lowband_scratch, x_cm | y_cm);
      }
      y_cm = x_cm;
    }
    if (resynth) {
      collapse_masks[i * C] = static_cast<unsigned char>(x_cm);
      collapse_masks[i * C + C - 1] = static_cast<unsigned char>(y_cm);
    }
    balance += pulses[i] + tell;
    update_lowband = b > (N << 3);
    ctx.avoid_split_noise = 0;
  }
  *seed = ctx.seed;
}

static void _celt_lpc(opus_val16* _lpc, const opus_val32* ac, int p);
static void celt_fir_c(const opus_val16* x, const opus_val16* num, opus_val16* y, int N);
static void celt_iir(const opus_val32* x, const opus_val16* den, opus_val32* y, int N, opus_val16* mem);
static void _celt_autocorr(const opus_val16* x, opus_val32* ac, const celt_coef* window, int overlap, int lag, int n);
static int resampling_factor(opus_int32 rate) {
  return is_supported_sample_rate(rate) ? 48000 / rate : 0;
}

static void comb_filter_const_c(opus_val32* y, opus_val32* x, int T, int N, celt_coef g10, celt_coef g11, celt_coef g12) {
  for (int i = 0; i < N; ++i) {
    const opus_val32 delayed_0 = x[i - T];
    const opus_val32 delayed_1 = x[i - T + 1] + x[i - T - 1];
    const opus_val32 delayed_2 = x[i - T + 2] + x[i - T - 2];
    y[i] = x[i] + g10 * delayed_0 + g11 * delayed_1 + g12 * delayed_2;
  }
}

static void comb_filter(opus_val32* y, opus_val32* x, int T0, int T1, int N, opus_val16 g0, opus_val16 g1, int tapset0, int tapset1,
                        const celt_coef* window, int overlap) {
  int i;
  celt_coef g00, g01, g02, g10, g11, g12;
  if (g0 == 0 && g1 == 0) {
    if (x != y) {
      move_n_items(x, static_cast<std::size_t>(N), y);
    }
    return;
  }
  T0 = std::max(T0, 15);
  T1 = std::max(T1, 15);
  g00 = ((g0) * (comb_filter_tapset_gains[tapset0][0]));
  g01 = ((g0) * (comb_filter_tapset_gains[tapset0][1]));
  g02 = ((g0) * (comb_filter_tapset_gains[tapset0][2]));
  g10 = ((g1) * (comb_filter_tapset_gains[tapset1][0]));
  g11 = ((g1) * (comb_filter_tapset_gains[tapset1][1]));
  g12 = ((g1) * (comb_filter_tapset_gains[tapset1][2]));
  if (g0 == g1 && T0 == T1 && tapset0 == tapset1) {
    overlap = 0;
  }
  for (i = 0; i < overlap; i++) {
    celt_coef f = ((window[i]) * (window[i]));
    const opus_val32 old_period = g00 * x[i - T0] + g01 * (x[i - T0 + 1] + x[i - T0 - 1]) + g02 * (x[i - T0 + 2] + x[i - T0 - 2]);
    const opus_val32 new_period = g10 * x[i - T1] + g11 * (x[i - T1 + 1] + x[i - T1 - 1]) + g12 * (x[i - T1 + 2] + x[i - T1 - 2]);
    y[i] = x[i] + (1.0f - f) * old_period + f * new_period;
  }
  if (g1 == 0) {
    if (x != y) {
      move_n_items(x + overlap, static_cast<std::size_t>(N - overlap), y + overlap);
    }
    return;
  }
  comb_filter_const_c(y + i, x + i, T1, N - i, g10, g11, g12);
}

static constexpr std::array<std::array<signed char, 8>, 4> tf_select_table =
    numeric_blob_matrix<signed char, 8>(R"blob(00FF00FF00FF00FF00FF00FE010001FF00FE00FD020001FF00FE00FD030001FF)blob");
static void init_caps(std::span<int> cap, int LM, int C) {
  const int nbEBands = celt_default_nb_ebands;
  const auto* cache_caps = celt_mode()->cache_caps + nbEBands * (2 * LM + C - 1);
  for (int i = 0; i < celt_default_nb_ebands; ++i) {
    const auto N = (celt_mode()->eBands[i + 1] - celt_mode()->eBands[i]) << LM;
    cap[i] = (cache_caps[i] + 64) * C * N >> 2;
  }
}

[[nodiscard]] constexpr auto celt_hybrid_target(opus_int32 base_target, int LM, int silk_offset) noexcept -> opus_int32 {
  auto target = base_target;
  if (silk_offset < 100) {
    target += 12 << 3 >> (3 - LM);
  }
  if (silk_offset > 100) {
    target -= 18 << 3 >> (3 - LM);
  }
  return target - 100;
}

[[nodiscard]] constexpr auto celt_anti_collapse_reserve(bool is_transient, int LM, opus_int32 bits) noexcept -> int {
  return is_transient && LM >= 2 && bits >= ((LM + 2) << 3) ? (1 << 3) : 0;
}

static void celt_commit_band_state(celt_glog* old_band, celt_glog* old_log, celt_glog* old_log2, int channels, int start, int end,
                                   bool is_transient, bool mirror_mono) {
  constexpr int nbEBands = celt_default_nb_ebands;
  const auto count = static_cast<std::size_t>(channels * nbEBands);
  if (mirror_mono && channels > 1) {
    copy_n_items(old_band, static_cast<std::size_t>(nbEBands), old_band + nbEBands);
  }
  if (!is_transient) {
    copy_n_items(old_log, count, old_log2);
    copy_n_items(old_band, count, old_log);
  } else {
    for (auto index = std::size_t{}; index < count; ++index)
      old_log[index] = std::min(old_log[index], old_band[index]);
  }
  for (int channel = 0; channel < channels; ++channel) {
    auto* band = old_band + channel * nbEBands;
    auto* log = old_log + channel * nbEBands;
    auto* log2 = old_log2 + channel * nbEBands;
    if (start > 0) {
      std::fill_n(band, static_cast<std::size_t>(start), 0.f);
      std::fill_n(log, static_cast<std::size_t>(start), -(28.f));
      std::fill_n(log2, static_cast<std::size_t>(start), -(28.f));
    }
    if (end < nbEBands) {
      const auto tail = static_cast<std::size_t>(nbEBands - end);
      std::fill_n(band + end, tail, 0.f);
      std::fill_n(log + end, tail, -(28.f));
      std::fill_n(log2 + end, tail, -(28.f));
    }
  }
}

template <bool Encode>
static inline auto process_celt_dynalloc(ec_ctx* coder, std::span<const opus_int16> eBands, std::span<int> offsets,
                                         std::span<const int> cap, int start, int end, int C, int LM, opus_int32& total_bits,
                                         int& total_boost) -> opus_int32 {
  int dynalloc_logp = 6;
  total_bits <<= 3;
  total_boost = 0;
  auto tell = ec_tell_frac(coder);
  for (int i = start; i < end; ++i) {
    int boost = 0, dynalloc_loop_logp = dynalloc_logp, j = 0;
    const int width = C * (eBands[i + 1] - eBands[i]) << LM;
    const int quanta = std::min(width << 3, std::max(6 << 3, width));
    for (; static_cast<opus_int32>(tell + (dynalloc_loop_logp << 3)) < total_bits - total_boost && boost < cap[i]; ++j) {
      int flag;
      if constexpr (Encode) {
        flag = j < offsets[i];
        ec_enc_bit_logp(coder, flag, dynalloc_loop_logp);
      } else {
        flag = ec_dec_bit_logp(coder, dynalloc_loop_logp);
      }
      tell = ec_tell_frac(coder);
      if (!flag) {
        break;
      }
      boost += quanta;
      total_boost += quanta;
      dynalloc_loop_logp = 1;
    }
    if (j) {
      dynalloc_logp = std::max(2, dynalloc_logp - 1);
    }
    offsets[i] = boost;
  }
  return tell;
}

static constexpr int celt_encoder_history_size = 1024;
static constexpr int celt_decoder_history_size = 2048;

[[nodiscard]] static inline auto celt_encoder_storage(CeltEncoderInternal* st) noexcept -> celt_sig* {
  static_assert(sizeof(CeltEncoderInternal) % alignof(celt_sig) == 0);
  return reinterpret_cast<celt_sig*>(reinterpret_cast<std::byte*>(st) + sizeof(CeltEncoderInternal));
}

[[nodiscard]] static constexpr auto celt_encoder_storage_count(int channels) noexcept -> std::size_t {
  return static_cast<std::size_t>(channels) * (celt_default_overlap + celt_encoder_history_size + 4 * celt_default_nb_ebands);
}

struct celt_encoder_views {
  celt_sig* prefilter_mem{};
  celt_glog *oldBandE{}, *oldLogE{}, *oldLogE2{}, *energyError{};
};

[[nodiscard]] static inline auto make_celt_encoder_views(CeltEncoderInternal* st) noexcept -> celt_encoder_views {
  const auto channels = st->channels, overlap = celt_default_overlap, nbEBands = celt_default_nb_ebands;
  auto* const in_mem = celt_encoder_storage(st);
  celt_encoder_views views{};
  views.prefilter_mem = in_mem + channels * overlap;
  views.oldBandE = reinterpret_cast<celt_glog*>(in_mem + channels * (overlap + 1024));
  views.oldLogE = views.oldBandE + channels * nbEBands;
  views.oldLogE2 = views.oldLogE + channels * nbEBands;
  views.energyError = views.oldLogE2 + channels * nbEBands;
  return views;
}

static void celt_encoder_init(CeltEncoderInternal* st, opus_int32 sampling_rate, int channels) {
  *st = {};
  std::uninitialized_default_construct_n(celt_encoder_storage(st), celt_encoder_storage_count(channels));
  st->channels = channels;
  st->upsample = resampling_factor(sampling_rate);
  celt_encoder_reset_state(st);
}

[[nodiscard]] static auto celt_frame_lm(const int frame_size) noexcept -> int {
  const auto scale = static_cast<unsigned>(frame_size / celt_short_mdct_size);
  return frame_size % celt_short_mdct_size == 0 && std::has_single_bit(scale) && scale <= (1U << celt_max_lm)
             ? static_cast<int>(std::countr_zero(scale))
             : -1;
}

static void compute_mdcts(int shortBlocks, celt_sig* in, celt_sig* out, int C, int CC, int LM, int upsample) {
  const int overlap = celt_default_overlap;
  int N, B, shift, i, b, c;
  if (shortBlocks) {
    B = shortBlocks;
    N = celt_short_mdct_size;
    shift = celt_max_lm;
  } else {
    B = 1;
    N = celt_short_mdct_size << LM;
    shift = celt_max_lm - LM;
  }
  for (c = 0; c < CC; ++c)
    for (b = 0; b < B; b++) {
      clt_mdct_forward_c(&celt_mode()->mdct, in + c * (B * N + overlap) + b * N, &out[b + c * N * B], celt_mode()->window, overlap, shift,
                         B);
    }
  if (CC == 2 && C == 1) {
    const int bn = B * N;
    for (i = 0; i < bn; i++) {
      out[i] = .5f * out[i] + .5f * out[bn + i];
    }
  }
  if (upsample != 1) {
    for (c = 0; c < C; ++c) {
      const int bound = B * N / upsample;
      auto* band = out + c * B * N;
      for (int j = 0; j < bound; ++j) {
        band[j] *= upsample;
      }
      zero_n_items(&out[c * B * N + bound], static_cast<std::size_t>(B * N - bound));
    }
  }
}

template <bool Encode> static inline void process_tf_changes(int start, int end, int isTransient, int* tf_res, int LM, ec_ctx* coder) {
  opus_uint32 budget = coder->storage * 8;
  opus_uint32 tell = ec_tell(coder);
  int curr = 0, tf_changed = 0, tf_select = 0;
  int logp = isTransient ? 2 : 4;
  const int tf_select_rsv = LM > 0 && tell + logp + 1 <= budget;
  budget -= tf_select_rsv;
  for (int i = start; i < end; ++i) {
    if (tell + logp <= budget) {
      if constexpr (Encode) {
        ec_enc_bit_logp(coder, tf_res[i] ^ curr, logp);
        curr = tf_res[i];
      } else {
        curr ^= ec_dec_bit_logp(coder, logp);
      }
      tell = ec_tell(coder);
      tf_changed |= curr;
    }
    tf_res[i] = curr;
    logp = isTransient ? 4 : 5;
  }
  if (tf_select_rsv && tf_select_table[LM][4 * isTransient + 0 + tf_changed] != tf_select_table[LM][4 * isTransient + 2 + tf_changed]) {
    if constexpr (Encode) {
      ec_enc_bit_logp(coder, tf_select, 1);
    } else {
      tf_select = ec_dec_bit_logp(coder, 1);
    }
  } else {
    tf_select = 0;
  }
  for (int i = start; i < end; ++i) {
    tf_res[i] = tf_select_table[LM][4 * isTransient + 2 * tf_select + tf_res[i]];
  }
}

static inline int alloc_trim_analysis(const celt_norm* X, const celt_glog* bandLogE, int end, int LM, int C, int N0,
                                      opus_val16* stereo_saving, int intensity, opus_int32 equiv_rate) {
  int i;
  opus_val32 diff = 0;
  int c, trim_index;
  opus_val16 trim = (5.f);
  opus_val16 logXC, logXC2;
  if (equiv_rate < 64000) {
    trim = (4.f);
  } else if (equiv_rate < 80000) {
    opus_int32 frac = (equiv_rate - 64000) >> 10;
    trim = (4.f) + (1.f / 16.f) * frac;
  }
  const auto* eBands = celt_mode()->eBands;
  if (C == 2) {
    opus_val16 sum = 0, minXC;
    for (i = 0; i < 8; i++) {
      const int band_start = eBands[i] << LM;
      opus_val32 partial = celt_inner_prod_c(&X[band_start], &X[N0 + band_start], (eBands[i + 1] - eBands[i]) << LM);
      sum = ((sum) + (((partial))));
    }
    sum = (((1.f / 8)) * (sum));
    sum = std::min(1.0f, std::fabs(sum));
    minXC = sum;
    for (i = 8; i < intensity; i++) {
      const int band_start = eBands[i] << LM;
      opus_val32 partial = celt_inner_prod_c(&X[band_start], &X[N0 + band_start], (eBands[i + 1] - eBands[i]) << LM);
      minXC = std::min(minXC, std::fabs(partial));
    }
    minXC = std::min(1.0f, std::fabs(minXC));
    logXC = (static_cast<float>(1.442695040888963387 * std::log((1.001f) - (static_cast<opus_val32>(sum) * static_cast<opus_val32>(sum)))));
    const opus_val16 min_logXC =
        static_cast<float>(1.442695040888963387 * std::log((1.001f) - (static_cast<opus_val32>(minXC) * static_cast<opus_val32>(minXC))));
    logXC2 = std::max(.5f * logXC, min_logXC);
    trim += std::max(-4.f, .75f * logXC);
    *stereo_saving = std::min(*stereo_saving + .25f, -.5f * logXC2);
  }
  const int nbEBands = celt_default_nb_ebands;
  for (c = 0; c < C; ++c) {
    const int band_offset = c * nbEBands;
    for (i = 0; i < end - 1; i++) {
      diff += (bandLogE[band_offset + i]) * static_cast<opus_int32>(2 + 2 * i - end);
    }
  }
  diff /= C * (end - 1);
  const opus_val16 trim_adjust = (diff + (1.f)) / 6;
  trim -= std::max(-(2.f), std::min(2.f, trim_adjust));
  if (equiv_rate >= 64000) {
    trim += (1.f);
  }
  trim_index = static_cast<int>(std::floor(.5f + trim));
  trim_index = std::max(0, std::min(10, trim_index));
  return trim_index;
}

static inline int stereo_analysis(const celt_norm* X, int LM, int N0) {
  opus_val32 sumLR = 1e-15f, sumMS = 1e-15f;
  const auto* eBands = celt_mode()->eBands;
  for (int i = 0; i < celt_stereo_analysis_bands; ++i) {
    for (int j = eBands[i] << LM; j < eBands[i + 1] << LM; ++j) {
      const opus_val32 left = X[j];
      const opus_val32 right = X[N0 + j];
      sumLR += std::fabs(left) + std::fabs(right);
      sumMS += std::fabs(left + right) + std::fabs(left - right);
    }
  }
  sumMS *= 0.707107f;
  int thetas = celt_stereo_analysis_bands;
  if (LM <= 1) {
    thetas -= 8;
  }
  const int band_width = eBands[celt_stereo_analysis_bands] << (LM + 1);
  return (band_width + thetas) * sumMS > band_width * sumLR;
}

static celt_glog median_of_5(const celt_glog* x) {
  auto t0 = std::min(x[0], x[1]);
  auto t1 = std::max(x[0], x[1]);
  auto t3 = std::min(x[3], x[4]);
  auto t4 = std::max(x[3], x[4]);
  if (t0 > t3) {
    t3 = t0;
    std::swap(t1, t4);
  }
  if (x[2] > t1) {
    return t1 < t3 ? std::min(x[2], t3) : std::min(t4, t1);
  }
  return x[2] < t3 ? std::min(t1, t3) : std::min(x[2], t4);
}

static celt_glog median_of_3(const celt_glog* x) {
  const auto low = std::min(x[0], x[1]);
  const auto high = std::max(x[0], x[1]);
  return high < x[2] ? high : (low < x[2] ? x[2] : low);
}

static inline void apply_tone_dynalloc_boost(celt_glog* follower, int start, int end, opus_val16 tone_freq,
                                             opus_val32 toneishness) noexcept {
  if (toneishness <= (.98f)) {
    return;
  }
  const auto* eBands = celt_mode()->eBands;
  const int freq_bin = static_cast<int>(std::floor(.5 + tone_freq * 120 / 3.141592653));
  constexpr std::array<celt_glog, 4> distance_boost{4.5f, 2.5f, 1.5f, .5f};
  for (int i = start; i < end; ++i) {
    const auto distance = std::max({eBands[i] - freq_bin, freq_bin - eBands[i + 1], 0});
    if (distance < static_cast<int>(distance_boost.size())) {
      follower[i] += distance_boost[static_cast<std::size_t>(distance)];
    }
  }
  if (freq_bin >= eBands[end]) {
    follower[end - 1] += (2.f);
    follower[end - 2] += (1.f);
  }
}

static inline void apply_low_rate_lf_dynalloc_boost(celt_glog* follower, int start, int end, int LM, int effectiveBytes,
                                                    opus_val32 toneishness) noexcept {
  const int low_rate_start = 30 + 5 * LM;
  if (start != 0 || end <= 1 || effectiveBytes < low_rate_start || effectiveBytes >= 160 || toneishness <= (.30f)) {
    return;
  }

  celt_glog dyn_peak = -1.f;
  int dyn_peak_i = -1;
  for (int i = start; i < end; ++i) {
    if (follower[i] > dyn_peak) {
      dyn_peak = follower[i];
      dyn_peak_i = i;
    }
  }

  const celt_glog low_need = std::max(follower[0], follower[1]);
  const bool harmonic_veto = dyn_peak_i >= 3 && dyn_peak > low_need + (1.f);
  if (!(low_need > (.5f)) || harmonic_veto) {
    return;
  }

  const auto low_rate_lf_boost = std::min<celt_glog>(1.f, (.025f) * (effectiveBytes - low_rate_start));
  follower[0] += low_rate_lf_boost;
  follower[1] += (.5f) * low_rate_lf_boost;
}

static inline celt_glog dynalloc_analysis(const CeltEncoderInternal* st, const celt_glog* bandLogE, celt_glog* bandLogE2,
                                          const celt_glog* oldBandE, int* offsets, int isTransient, int LM, int effectiveBytes,
                                          opus_int32* tot_boost_, opus_val16 tone_freq, opus_val32 toneishness) {
  constexpr int nbEBands = celt_default_nb_ebands;
  const int start = st->start;
  const int end = st->end;
  const int C = st->stream_channels;
  const auto* eBands = celt_mode()->eBands;
  int i, c;
  opus_int32 tot_boost = 0;
  std::array<celt_glog, celt_max_channels * celt_default_nb_ebands> follower;
  std::array<celt_glog, celt_default_nb_ebands> noise_floor;
  zero_n_items(offsets, static_cast<std::size_t>(nbEBands));
  celt_glog maxDepth = -(31.9f);
  for (i = 0; i < end; i++) {
    noise_floor[i] = celt_noise_floor_base[i] + (9 - st->lsb_depth);
    maxDepth = std::max(maxDepth, bandLogE[i] - noise_floor[i]);
  }
  if (C == 2) {
    for (i = 0; i < end; i++) {
      maxDepth = std::max(maxDepth, bandLogE[nbEBands + i] - noise_floor[i]);
    }
  }
  if (effectiveBytes >= (30 + 5 * LM) || st->lowrate_refinement) {
    int last = 0;
    for (c = 0; c < C; ++c) {
      celt_glog offset, tmp;
      celt_glog* f;
      auto* bandLogE3 = &bandLogE2[c * nbEBands];
      if (LM == 0) {
        for (i = 0; i < std::min(8, end); i++) {
          bandLogE3[i] = std::max(bandLogE2[c * nbEBands + i], oldBandE[c * nbEBands + i]);
        }
      }
      f = follower.data() + c * nbEBands;
      f[0] = bandLogE3[0];
      for (i = 1; i < end; i++) {
        if (bandLogE3[i] > bandLogE3[i - 1] + (.5f)) {
          last = i;
        }
        f[i] = std::min(f[i - 1] + 1.5f, bandLogE3[i]);
      }
      for (i = last - 1; i >= 0; i--) {
        f[i] = std::min(f[i], std::min(f[i + 1] + (2.f), bandLogE3[i]));
      }
      offset = (1.f);
      for (i = 2; i < end - 2; i++) {
        f[i] = std::max(f[i], median_of_5(&bandLogE3[i - 2]) - offset);
      }
      tmp = median_of_3(&bandLogE3[0]) - offset;
      f[0] = std::max(f[0], tmp);
      f[1] = std::max(f[1], tmp);
      tmp = median_of_3(&bandLogE3[end - 3]) - offset;
      f[end - 2] = std::max(f[end - 2], tmp);
      f[end - 1] = std::max(f[end - 1], tmp);
      for (i = 0; i < end; i++) {
        f[i] = std::max(f[i], noise_floor[i]);
      }
    }
    if (C == 2) {
      for (i = start; i < end; i++) {
        follower[nbEBands + i] = ((follower[nbEBands + i]) > (follower[i] - (4.f)) ? (follower[nbEBands + i]) : (follower[i] - (4.f)));
        follower[i] = ((follower[i]) > (follower[nbEBands + i] - (4.f)) ? (follower[i]) : (follower[nbEBands + i] - (4.f)));
        follower[i] = (.5f * (std::max(0.f, bandLogE[i] - follower[i]) + std::max(0.f, bandLogE[nbEBands + i] - follower[nbEBands + i])));
      }
    } else {
      for (i = start; i < end; i++) {
        follower[i] = std::max(0.f, bandLogE[i] - follower[i]);
      }
    }
    const bool constrained_steady = (!st->vbr || st->constrained_vbr) && !isTransient;
    for (i = start; i < end; i++) {
      if (constrained_steady) {
        if (i >= 12) {
          follower[i] *= .25f;
        } else if (i >= 8) {
          follower[i] *= .5f;
        }
      } else if (i < 8) {
        follower[i] *= 2;
      } else if (i >= 12) {
        follower[i] *= .5f;
      }
    }
    apply_tone_dynalloc_boost(follower.data(), start, end, tone_freq, toneishness);
    apply_low_rate_lf_dynalloc_boost(follower.data(), start, end, LM, effectiveBytes, toneishness);
    if (effectiveBytes > 320) {
      follower[0] += std::min<celt_glog>(1.5f, 1e-3f * (effectiveBytes - 320));
    }
    for (i = start; i < end; i++) {
      int width, boost, boost_bits;
      follower[i] = std::min(follower[i], 4.0f);
      width = C * (eBands[i + 1] - eBands[i]) << LM;
      if (width < 6) {
        boost = static_cast<int>(follower[i]);
        boost_bits = boost * width << 3;
      } else if (width > 48) {
        boost = static_cast<int>(follower[i] * 8);
        boost_bits = (boost * width << 3) / 8;
      } else {
        boost = static_cast<int>(follower[i] * width / 6);
        boost_bits = boost * 6 << 3;
      }
      if ((!st->vbr || (st->constrained_vbr && !isTransient)) && (tot_boost + boost_bits) >> 3 >> 3 > 2 * effectiveBytes / 3) {
        opus_int32 cap = ((2 * effectiveBytes / 3) << 3 << 3);
        offsets[i] = cap - tot_boost;
        tot_boost = cap;
        break;
      } else {
        offsets[i] = boost;
        tot_boost += boost_bits;
      }
    }
  }
  *tot_boost_ = tot_boost;
  return maxDepth;
}

static int tone_lpc(const opus_val16* x, int len, int delay, opus_val32* lpc) {
  opus_val32 r00 = 0, r01 = 0, r11 = 0, r02 = 0, r12 = 0, r22 = 0;
  for (int i = 0; i < len - 2 * delay; i++) {
    r00 += (static_cast<opus_val32>(x[i]) * static_cast<opus_val32>(x[i]));
    r01 += (static_cast<opus_val32>(x[i]) * static_cast<opus_val32>(x[i + delay]));
    r02 += (static_cast<opus_val32>(x[i]) * static_cast<opus_val32>(x[i + 2 * delay]));
  }
  opus_val32 edge11 = 0, edge22 = 0, edge12 = 0;
  for (int i = 0; i < delay; i++) {
    const opus_val32 early0 = x[i], early1 = x[i + delay];
    const opus_val32 late0 = x[len + i - 2 * delay], late1 = x[len + i - delay];
    edge11 += late0 * late0 - early0 * early0;
    edge22 += late1 * late1 - early1 * early1;
    edge12 += late0 * late1 - early0 * early1;
  }
  r11 = r00 + edge11;
  r22 = r11 + edge22;
  r12 = r01 + edge12;
  r00 += r22;
  r01 += r12;
  r11 *= 2;
  r02 *= 2;
  r12 = r01;
  const opus_val32 den = r00 * r11 - r01 * r01;
  if (den < .001f * r00 * r11) {
    return 1;
  }
  const opus_val32 num1 = r02 * r11 - r01 * r12;
  if (num1 >= den) {
    lpc[1] = (1.f);
  } else if (num1 <= -den)
    lpc[1] = -(1.f);
  else
    lpc[1] = (static_cast<float>(num1) / (den));
  const opus_val32 num0 = r00 * r12 - r02 * r01;
  if ((.5f * (num0)) >= den) {
    lpc[0] = (1.999999f);
  } else if ((.5f * (num0)) <= -den)
    lpc[0] = -(1.999999f);
  else
    lpc[0] = (static_cast<float>(num0) / (den));
  return 0;
}

static inline opus_val16 tone_detect(const celt_sig* in, int CC, int N, opus_val32* toneishness) {
  int delay = 1;
  std::array<opus_val32, 2> lpc;
  std::array<opus_val16, OPUS_FRAME_SIZE_20MS + celt_default_overlap> x_sum;
  const opus_val16* x = in;
  if (CC == 2) {
    for (int i = 0; i < N; i++) {
      x_sum[i] = 0.5f * (in[i] + in[i + N]);
    }
    x = x_sum.data();
  }
  int fail = tone_lpc(x, N, delay, lpc.data());
  for (; delay <= celt_sample_rate / 3000 && (fail || (lpc[0] > (1.f) && lpc[1] < 0));) {
    delay *= 2;
    fail = tone_lpc(x, N, delay, lpc.data());
  }
  if (!fail && ((lpc[0]) * (lpc[0])) + (((3.999999)) * (lpc[1])) < 0) {
    *toneishness = -lpc[1];
    return acos(.5f * lpc[0]) / delay;
  }
  *toneishness = 0;
  return -1;
}

static int run_prefilter(CeltEncoderInternal* st, celt_sig* in, celt_sig* prefilter_mem, int CC, int N, int* pitch, opus_val16* gain,
                         int* qgain, int enabled, int complexity, int nbAvailableBytes, opus_val16 tone_freq, opus_val32 toneishness,
                         const std::array<opus_val32, celt_max_channels>& input_abs_sum) {
  std::array<celt_sig*, celt_max_channels> pre{};
  int pitch_index;
  opus_val16 gain1, pf_threshold;
  int pf_on, qg;
  std::array<opus_val32, celt_max_channels> output_abs_sum{};
  bool cancel_pitch = false;
  constexpr auto max_period = celt_max_pitch_period, min_period = celt_min_pitch_period;
  const int overlap = celt_default_overlap;
  if (st->prefilter_gain == 0 && (!enabled || (complexity < 5 && toneishness <= (.99f)))) {
    for (int c = 0; c < CC; ++c) {
      copy_n_items(celt_encoder_storage(st) + c * overlap, static_cast<std::size_t>(overlap), in + c * (N + overlap));
      copy_n_items(in + c * (N + overlap) + N, static_cast<std::size_t>(overlap), celt_encoder_storage(st) + c * overlap);
      if (N > max_period) {
        copy_n_items(in + c * (N + overlap) + overlap + N - max_period, static_cast<std::size_t>(max_period),
                     prefilter_mem + c * max_period);
      } else {
        move_n_items(prefilter_mem + c * max_period + N, static_cast<std::size_t>(max_period - N), prefilter_mem + c * max_period);
        copy_n_items(in + c * (N + overlap) + overlap, static_cast<std::size_t>(N), prefilter_mem + c * max_period + max_period - N);
      }
    }
    *gain = 0;
    *pitch = 15;
    *qgain = 0;
    return 0;
  }
  std::array<celt_sig, celt_max_channels*(celt_max_frame_samples + celt_max_pitch_period)> prefilter_storage;
  pre[0] = prefilter_storage.data();
  pre[1] = pre[0] + N + max_period;
  for (int c = 0; c < CC; ++c) {
    copy_n_items(prefilter_mem + c * max_period, static_cast<std::size_t>(max_period), pre[c]);
    copy_n_items(in + c * (N + overlap) + overlap, static_cast<std::size_t>(N), pre[c] + max_period);
  }
  if (enabled && toneishness > (.99f)) {
    int multiple = 1;
    if ((tone_freq) >= (3.1416f)) {
      tone_freq = (3.141593f) - tone_freq;
    }
    for (; (tone_freq) >= multiple * (0.39f); ++multiple) {}
    if ((tone_freq) > (0.006148f)) {
      const int tone_pitch = static_cast<int>(std::floor(.5 + 2.f * 3.141592653 * multiple / (tone_freq)));
      pitch_index = std::min(tone_pitch, max_period - 2);
    } else {
      pitch_index = 15;
    }
    gain1 = (.75f);
  } else if (enabled && complexity >= 5) {
    if (!st->lowrate_refinement && st->prefilter_gain > (.2f) && st->prefilter_period >= min_period) {
      pitch_index = st->prefilter_period;
      gain1 = st->prefilter_gain;
    } else {
      std::array<opus_val16, (celt_max_pitch_period + celt_max_frame_samples) / 2> pitch_storage;
      auto* pitch_buf = pitch_storage.data();
      pitch_downsample(pre.data(), CC, pitch_buf, (max_period + N) >> 1);
      pitch_search(pitch_buf + (max_period >> 1), pitch_buf, N, max_period - 3 * min_period, &pitch_index);
      pitch_index = max_period - pitch_index;
      gain1 = remove_doubling(pitch_buf, N, &pitch_index, st->prefilter_period, st->prefilter_gain);
      if (pitch_index > max_period - (2)) {
        pitch_index = max_period - (2);
      }
      gain1 = (((.7f)) * (gain1));
    }
  } else {
    gain1 = 0;
    pitch_index = 15;
  }
  pf_threshold = (.2f);
  if (std::abs(pitch_index - st->prefilter_period) * 10 > pitch_index) {
    pf_threshold += (.2f);
  }
  if (nbAvailableBytes < 25) {
    pf_threshold += (.1f);
  }
  if (nbAvailableBytes < 35) {
    pf_threshold += (.1f);
  }
  if (st->prefilter_gain > (.4f)) {
    pf_threshold -= (.1f);
  }
  if (st->prefilter_gain > (.55f)) {
    pf_threshold -= (.1f);
  }
  pf_threshold = std::max(pf_threshold, .2f);
  if (gain1 < pf_threshold) {
    gain1 = 0;
    pf_on = 0;
    qg = 0;
  } else {
    if (std::abs(gain1 - st->prefilter_gain) < .1f) {
      gain1 = st->prefilter_gain;
    }
    qg = static_cast<int>(std::floor(.5f + gain1 * 32 / 3)) - 1;
    qg = std::clamp(qg, 0, 7);
    gain1 = (0.09375f) * (qg + 1);
    pf_on = 1;
  }
  for (int c = 0; c < CC; ++c) {
    st->prefilter_period = std::max<opus_uint16>(st->prefilter_period, 15);
    copy_n_items(celt_encoder_storage(st) + c * overlap, static_cast<std::size_t>(overlap), in + c * (N + overlap));
    comb_filter(in + c * (N + overlap) + overlap, pre[c] + max_period, st->prefilter_period, pitch_index, N, -st->prefilter_gain, -gain1, 0,
                0, celt_mode()->window, overlap);
    for (int i = 0; i < N; ++i) {
      output_abs_sum[c] += std::abs(in[c * (N + overlap) + overlap + i]);
    }
  }
  if (CC == 2) {
    std::array<opus_val16, 2> thresh;
    thresh[0] = .25f * gain1 * input_abs_sum[0] + .01f * input_abs_sum[1];
    thresh[1] = .25f * gain1 * input_abs_sum[1] + .01f * input_abs_sum[0];
    if (output_abs_sum[0] - input_abs_sum[0] > thresh[0] || output_abs_sum[1] - input_abs_sum[1] > thresh[1]) {
      cancel_pitch = true;
    }
    if (input_abs_sum[0] - output_abs_sum[0] < thresh[0] && input_abs_sum[1] - output_abs_sum[1] < thresh[1]) {
      cancel_pitch = true;
    }
  } else if (output_abs_sum[0] > input_abs_sum[0]) {
    cancel_pitch = true;
  }
  if (cancel_pitch) {
    for (int c = 0; c < CC; ++c) {
      copy_n_items(pre[c] + max_period, static_cast<std::size_t>(N), in + c * (N + overlap) + overlap);
      comb_filter(in + c * (N + overlap) + overlap, pre[c] + max_period, st->prefilter_period, pitch_index, overlap, -st->prefilter_gain, 0,
                  0, 0, celt_mode()->window, overlap);
    }
    gain1 = 0;
    pf_on = 0;
    qg = 0;
  }
  for (int c = 0; c < CC; ++c) {
    copy_n_items(in + c * (N + overlap) + N, static_cast<std::size_t>(overlap), celt_encoder_storage(st) + c * overlap);
    if (N > max_period) {
      copy_n_items(pre[c] + N, static_cast<std::size_t>(max_period), prefilter_mem + c * max_period);
    } else {
      move_n_items(prefilter_mem + c * max_period + N, static_cast<std::size_t>(max_period - N), prefilter_mem + c * max_period);
      copy_n_items(pre[c] + max_period, static_cast<std::size_t>(N), prefilter_mem + c * max_period + max_period - N);
    }
  }
  *gain = gain1;
  *pitch = pitch_index;
  *qgain = qg;
  return pf_on;
}

static inline int compute_vbr(opus_int32 base_target, int LM, opus_int32 bitrate, int lastCodedBands, int C, int intensity,
                              int constrained_vbr, opus_val16 stereo_saving, int tot_boost, celt_glog maxDepth, celt_glog temporal_vbr,
                              bool content_vbr) {
  const int nbEBands = celt_default_nb_ebands;
  const opus_int16* eBands = celt_mode()->eBands;
  const int coded_bands = lastCodedBands ? lastCodedBands : nbEBands;
  int coded_bins = eBands[coded_bands] << LM;
  if (C == 2) {
    coded_bins += eBands[std::min(intensity, coded_bands)] << LM;
  }
  opus_int32 target = base_target;
  if (C == 2) {
    const int coded_stereo_bands = std::min(intensity, coded_bands);
    const int coded_stereo_dof = (eBands[coded_stereo_bands] << LM) - coded_stereo_bands;
    const opus_val16 max_frac = 0.8f * static_cast<opus_val32>(coded_stereo_dof) / static_cast<opus_val16>(coded_bins);
    stereo_saving = std::min(stereo_saving, 1.0f);
    target -= static_cast<opus_int32>(
        (((max_frac) * (target))) < (((static_cast<opus_val32>(stereo_saving - (0.1f)) * static_cast<opus_val32>((coded_stereo_dof << 3)))))
            ? (((max_frac) * (target)))
            : (((static_cast<opus_val32>(stereo_saving - (0.1f)) * static_cast<opus_val32>((coded_stereo_dof << 3))))));
  }
  target += tot_boost - (19 << LM);
  const opus_val16 tf_calibration = (0.044f);
  target += static_cast<opus_int32>((-tf_calibration) * target);
  {
    const int bins = eBands[nbEBands - 2] << LM;
    opus_int32 floor_depth = static_cast<opus_int32>((C * bins << 3) * maxDepth);
    floor_depth = std::max(floor_depth, target >> 2);
    target = std::min(target, floor_depth);
  }
  if (constrained_vbr) {
    target = base_target + static_cast<opus_int32>((.67f + .07f * content_vbr) * (target - base_target));
  }
  const opus_int32 rate_margin = std::max<opus_int32>(0, std::min<opus_int32>(32000, 96000 - bitrate));
  const opus_val16 amount = (((.0000031f)) * rate_margin);
  const opus_val16 tvbr_factor = ((static_cast<opus_val32>((temporal_vbr)) * static_cast<opus_val32>(amount)));
  target += static_cast<opus_int32>((tvbr_factor) * (target));
  target = std::min(2 * base_target, target);
  return target;
}

struct celt_input_metrics {
  std::array<opus_val32, celt_max_channels> abs_sum{};
  int silence{};
};

[[nodiscard]] static auto celt_transient_hint(const opus_val32* in, int length, int channels, opus_val32 threshold) noexcept -> bool {
  for (int channel = 0; channel < channels; ++channel) {
    const auto* input = in + channel * length;
    opus_val32 total = 0;
    opus_val32 maximum = 0;
    for (int i = 4; i < length; i += 4) {
      const opus_val32 difference = std::abs(input[i] - input[i - 4]);
      total += difference;
      maximum = std::max(maximum, difference);
    }
    if (maximum * (length / 4) > threshold * total) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] static inline auto celt_preemphasise_input(CeltEncoderInternal* st, const opus_res* pcm, celt_sig* in,
                                                         celt_sig* prefilter_mem, int N) -> celt_input_metrics {
  const int channels = st->channels;
  constexpr int overlap = celt_default_overlap;
  const auto preemphasise = [](const opus_res* pcmp, celt_sig* inp, int N, int CC, int upsample, celt_sig* mem, int clip) -> opus_val32 {
    opus_val32 abs_sum = 0;
    celt_sig m = *mem;
    if (upsample == 1 && !clip) {
      for (int i = 0; i < N; ++i) {
        const celt_sig x = 32768.f * pcmp[CC * i];
        inp[i] = x - m;
        abs_sum += std::abs(inp[i]);
        m = celt_preemphasis[0] * x;
      }
      *mem = m;
      return abs_sum;
    }
    const int Nu = N / upsample;
    if (upsample != 1) {
      zero_n_items(inp, static_cast<std::size_t>(N));
    }
    for (int i = 0; i < Nu; ++i) {
      const auto sample = 32768.f * pcmp[CC * i];
      inp[i * upsample] = clip ? std::clamp(sample, -65536.f, 65536.f) : sample;
    }
    for (int i = 0; i < N; ++i) {
      const celt_sig x = inp[i];
      inp[i] = x - m;
      abs_sum += std::abs(inp[i]);
      m = celt_preemphasis[0] * x;
    }
    *mem = m;
    return abs_sum;
  };
  celt_input_metrics metrics;
  if (st->upsample == 1) {
    const std::array old_mem{st->preemph_memE[0], st->preemph_memE[1]};
    const opus_val32 old_overlap_max = st->overlap_max;
    const int tail_begin = N - overlap;
    opus_val16 frame_max = 0, tail_max = 0;
    constexpr opus_val16 coef0 = celt_preemphasis[0];
    for (int c = 0; c < channels; ++c) {
      celt_sig m = st->preemph_memE[static_cast<std::size_t>(c)];
      auto* dst = in + c * (N + overlap) + overlap;
      for (int i = 0; i < N; ++i) {
        const opus_val16 sample = pcm[channels * i + c];
        const auto magnitude = std::abs(sample);
        frame_max = std::max(frame_max, magnitude);
        if (i >= tail_begin) {
          tail_max = std::max(tail_max, magnitude);
        }
        const celt_sig x = 32768.f * sample;
        dst[i] = x - m;
        metrics.abs_sum[static_cast<std::size_t>(c)] += std::abs(dst[i]);
        m = coef0 * x;
      }
      st->preemph_memE[static_cast<std::size_t>(c)] = m;
      copy_n_items(&prefilter_mem[(1 + c) * (1024) - overlap], static_cast<std::size_t>(overlap), in + c * (N + overlap));
    }
    st->overlap_max = tail_max;
    if (frame_max <= 65536.f) {
      const opus_val32 sample_max = std::max(frame_max, old_overlap_max);
      metrics.silence = sample_max <= static_cast<opus_val16>(1) / (1 << st->lsb_depth);
      return metrics;
    }
    std::copy_n(old_mem.data(), channels, st->preemph_memE);
    st->overlap_max = old_overlap_max;
  }
  opus_val32 sample_max = std::max<opus_val32>(st->overlap_max, celt_maxabs16(pcm, channels * (N - overlap) / st->upsample));
  st->overlap_max = celt_maxabs16(pcm + channels * (N - overlap) / st->upsample, channels * overlap / st->upsample);
  sample_max = std::max(sample_max, st->overlap_max);
  metrics.silence = sample_max <= static_cast<opus_val16>(1) / (1 << st->lsb_depth);
  const bool need_clip = sample_max > 65536.f;
  for (int c = 0; c < channels; ++c) {
    metrics.abs_sum[static_cast<std::size_t>(c)] =
        preemphasise(pcm + c, in + c * (N + overlap) + overlap, N, channels, st->upsample, st->preemph_memE + c, need_clip);
    copy_n_items(&prefilter_mem[(1 + c) * (1024) - overlap], static_cast<std::size_t>(overlap), in + c * (N + overlap));
  }
  return metrics;
}

struct celt_prefilter_result {
  int pitch_index{15};
  int enabled{};
  opus_val16 gain{};
};

static auto celt_encode_prefilter(CeltEncoderInternal* st, celt_sig* in, celt_sig* prefilter_mem, ec_enc* enc, int N, int nbAvailableBytes,
                                  opus_int32 total_bits, opus_int32 tell, int silence, opus_val16 tone_freq, opus_val32 toneishness,
                                  const std::array<opus_val32, celt_max_channels>& input_abs_sum) -> celt_prefilter_result {
  auto result = celt_prefilter_result{};
  int qg = 0;
  const int can_signal = st->start == 0 && tell + 16 <= total_bits;
  const int enabled = nbAvailableBytes > 12 * st->stream_channels && can_signal && !silence && !st->prediction_disabled;
  result.enabled = run_prefilter(st, in, prefilter_mem, st->channels, N, &result.pitch_index, &result.gain, &qg, enabled, st->complexity,
                                 nbAvailableBytes, tone_freq, toneishness, input_abs_sum);
  if (result.enabled == 0) {
    if (can_signal) {
      ec_enc_bit_logp(enc, 0, 1);
    }
    return result;
  }
  ec_enc_bit_logp(enc, 1, 1);
  result.pitch_index += 1;
  const int octave = std::bit_width(static_cast<opus_uint32>(result.pitch_index)) - 5;
  ec_enc_uint(enc, octave, 6);
  ec_enc_bits(enc, result.pitch_index - (16 << octave), 4 + octave);
  result.pitch_index -= 1;
  ec_enc_bits(enc, qg, 3);
  ec_enc_icdf(enc, 0, shared_three_step_icdf.data(), 2);
  return result;
}

[[nodiscard]] static inline auto celt_update_temporal_vbr(CeltEncoderInternal* st, const celt_glog* bandLogE, int LM,
                                                          int shortBlocks) noexcept -> celt_glog {
  celt_glog follow = -(10.0f);
  opus_val32 frame_avg = 0;
  const celt_glog offset = shortBlocks ? (.5f * LM) : 0;
  for (int i = st->start; i < st->end; ++i) {
    follow = std::max(follow - (1.0f), bandLogE[i] - offset);
    if (st->stream_channels == 2) {
      follow = std::max(follow, bandLogE[i + celt_default_nb_ebands] - offset);
    }
    frame_avg += follow;
  }
  frame_avg /= (st->end - st->start);
  auto temporal_vbr = frame_avg - st->spec_avg;
  temporal_vbr = clamp_value(temporal_vbr, -(1.5f), (3.f));
  st->spec_avg += (.02f) * temporal_vbr;
  return temporal_vbr;
}

[[nodiscard]] static auto celt_adjust_alloc_trim(int alloc_trim, const CeltEncoderInternal* st, bool hybrid, int channels,
                                                 opus_val32 toneishness) noexcept -> int {
  if (hybrid) {
    return st->high_z_tonal_Q7 > 64 ? clamp_value(alloc_trim - 4, 0, 10) : alloc_trim;
  }
  if (st->high_z_tonal_Q7 > 64) {
    if (st->bitrate >= 16000 && st->bitrate < 24000) {
      alloc_trim = clamp_value(alloc_trim + 2, 0, 10);
    } else if (st->bitrate >= 40000 && st->bitrate < 56000) {
      alloc_trim = clamp_value(alloc_trim - 2, 0, 10);
    }
  }
  if (st->bitrate > 0) {
    const int missing_bands = clamp_value(celt_default_nb_ebands - st->end, 0, 3);
    const int spectral_budget = st->bitrate / std::max(1, st->end);
    const int lowrate_diff_limit = st->bitrate <= 16000 ? 22 : 56;
    const bool balanced_tonal = st->input_diff_Q10 >= 1 && st->input_diff_Q10 <= 3 && toneishness >= .40f && toneishness <= .55f;
    const bool lowrate_stereo = st->channels == 2 && spectral_budget < 1400 && st->input_diff_Q10 < lowrate_diff_limit && !balanced_tonal;
    const int scarcity_boost = lowrate_stereo ? 0 : std::min(4, (std::max(0, 2050 - spectral_budget) + 49) / 50);
    const int bandwidth_boost = missing_bands > 0 ? (spectral_budget < 1200 ? std::min(7, 2 * missing_bands) : 7) : 0;
    alloc_trim = clamp_value(alloc_trim + std::max(bandwidth_boost, scarcity_boost), 0, 10);
  }
  if (channels == 2 && st->bitrate >= 56000 && st->bitrate < 80000) {
    alloc_trim = clamp_value(alloc_trim + (st->high_z_tonal_Q7 > 64 ? -1 : 1), 0, 10);
  } else if (st->bitrate >= 80000 && st->bitrate < 112000) {
    alloc_trim = clamp_value(alloc_trim + 2, 0, 10);
  }
  return alloc_trim;
}

static int celt_balance_lowrate_stereo_trim(int alloc_trim, const celt_glog* bandLogE, int end, int LM, int effectiveBytes,
                                            opus_int32 total_boost, std::span<const int> offsets) {
  if (end == celt_default_nb_ebands) {
    return alloc_trim;
  }
  const auto* eBands = celt_mode()->eBands;
  const int coefficient_count = 2 * eBands[end] << LM;
  const opus_val32 base_depth = std::max<opus_int32>(0, (effectiveBytes * 8 << 3) - total_boost) * (1.f / 8) / coefficient_count;
  opus_val32 mean = 0;
  for (int i = 0; i < end; ++i) {
    const int width = eBands[i + 1] - eBands[i];
    mean += width * .5f * (bandLogE[i] + bandLogE[celt_default_nb_ebands + i]);
  }
  mean /= eBands[end];
  opus_val32 gradient = 0;
  opus_val32 distortion_sum = 0;
  for (int i = 0; i < end; ++i) {
    const int width = eBands[i + 1] - eBands[i];
    const int coefficients = 2 * width << LM;
    const int trim_bits = 2 * width * (alloc_trim - 5 - LM) * (end - i - 1) * (1 << (LM + 3)) >> 6;
    const opus_val32 bit_depth = base_depth + (offsets[i] + trim_bits) * (1.f / 8) / coefficients;
    const opus_val32 energy = .5f * (bandLogE[i] + bandLogE[celt_default_nb_ebands + i]);
    const opus_val32 distortion = width * std::exp2(.25f * clamp_value(energy - mean, -4.f, 4.f) - 2 * bit_depth);
    const opus_val32 position = (2.f * i + 1 - end) / end;
    gradient -= distortion * position;
    distortion_sum += distortion;
  }
  const opus_val32 imbalance = gradient / distortion_sum;
  const bool correctable = distortion_sum > .15f * eBands[end] && std::fabs(imbalance) < .6f;
  return clamp_value(alloc_trim + (correctable && imbalance > .15f) - (correctable && imbalance < -.15f), 0, 10);
}

template <typename Operation> static inline void for_each_celt_band(const CeltEncoderInternal* st, Operation operation) noexcept {
  for (int c = 0; c < st->stream_channels; ++c) {
    const int band_offset = c * celt_default_nb_ebands;
    for (int i = st->start; i < st->end; ++i) {
      operation(band_offset + i);
    }
  }
}

static int celt_encode_with_ec(CeltEncoderInternal* st, const opus_res* pcm, int frame_size, unsigned char* compressed,
                               int nbCompressedBytes, ec_enc* enc) {
  frame_size *= st->upsample;
  const opus_int16* eBands = celt_mode()->eBands;
  constexpr int nbEBands = celt_default_nb_ebands;
  constexpr int overlap = celt_default_overlap;
  const int start = st->start;
  const int end = st->end;
  const bool hybrid = start != 0;
  const int LM = celt_frame_lm(frame_size);
  const int M = 1 << LM;
  const int N = M * celt_short_mdct_size;
  const int CC = st->channels;
  const int C = st->stream_channels;
  auto [prefilter_mem, oldBandE, oldLogE, oldLogE2, energyError] = make_celt_encoder_views(st);
  opus_int32 tell = enc == nullptr ? 1 : ec_tell(enc);
  const opus_int32 tell0_frac = enc == nullptr ? 1 : ec_tell_frac(enc);
  const int nbFilledBytes = enc == nullptr ? 0 : (tell + 4) >> 3;
  nbCompressedBytes = std::min(nbCompressedBytes, 1275);
  opus_int32 vbr_rate;
  int effectiveBytes;
  if (st->vbr && st->bitrate != -1) {
    vbr_rate = bitrate_to_bits(st->bitrate, celt_sample_rate, frame_size) << 3;
    effectiveBytes = vbr_rate >> (3 + 3);
  } else {
    vbr_rate = 0;
    opus_int32 tmp = st->bitrate * frame_size + (tell > 1 ? tell * celt_sample_rate : 0);
    if (st->bitrate != -1) {
      nbCompressedBytes = std::max(2, std::min(nbCompressedBytes, (tmp + 4 * celt_sample_rate) / (8 * celt_sample_rate)));
      if (enc != nullptr) {
        ec_enc_shrink(enc, nbCompressedBytes);
      }
    }
    effectiveBytes = nbCompressedBytes - nbFilledBytes;
  }
  int nbAvailableBytes = nbCompressedBytes - nbFilledBytes;
  auto equiv_rate = (static_cast<opus_int32>(nbCompressedBytes) * 8 * 50 << (3 - LM)) - (40 * C + 20) * ((400 >> LM) - 50);
  ec_enc local_encoder;
  if (st->bitrate != -1) {
    equiv_rate = std::min(equiv_rate, st->bitrate - (40 * C + 20) * ((400 >> LM) - 50));
  }
  if (enc == nullptr) {
    ec_enc_init(&local_encoder, compressed, nbCompressedBytes);
    enc = &local_encoder;
  }
  if (vbr_rate > 0 && st->constrained_vbr) {
    const opus_int32 max_allowed = std::min(std::max(tell == 1 ? 2 : 0, (2 * vbr_rate - st->vbr_reservoir) >> (3 + 3)), nbAvailableBytes);
    if (max_allowed < nbAvailableBytes) {
      nbCompressedBytes = nbFilledBytes + max_allowed;
      nbAvailableBytes = max_allowed;
      ec_enc_shrink(enc, nbCompressedBytes);
    }
  }
  opus_int32 total_bits = nbCompressedBytes * 8;
  std::array<celt_sig, celt_max_channels*(celt_max_frame_samples + celt_default_overlap)> input_storage;
  auto* in = input_storage.data();
  const auto input_metrics = celt_preemphasise_input(st, pcm, in, prefilter_mem, N);
  int silence = input_metrics.silence;
  if (tell == 1) {
    ec_enc_bit_logp(enc, silence, 15);
  } else
    silence = 0;
  if (silence) {
    if (vbr_rate > 0) {
      effectiveBytes = nbCompressedBytes = std::min(nbCompressedBytes, nbFilledBytes + 2);
      total_bits = nbCompressedBytes * 8;
      nbAvailableBytes = 2;
      ec_enc_shrink(enc, nbCompressedBytes);
    }
    tell = nbCompressedBytes * 8;
    enc->nbits_total += tell - ec_tell(enc);
  }
  opus_val32 toneishness = 0;
  const auto tone_frequency = silence ? opus_val16{-1} : tone_detect(in, CC, N + overlap, &toneishness);
  int isTransient = 0;
  if (!silence && hybrid && st->complexity >= 1) {
    opus_val32 transient_threshold = 8.f;
    if (C == 2 && st->bitrate > 0 && st->silk_info.bitrateBps > 0) {
      const auto highband_share = 100 * st->bitrate / st->silk_info.bitrateBps;
      if (highband_share >= 20 && highband_share <= 32) {
        transient_threshold = 24.f;
      }
    }
    isTransient = celt_transient_hint(in, N + overlap, CC, transient_threshold);
  }
  const auto prefilter = celt_encode_prefilter(st, in, prefilter_mem, enc, N, nbAvailableBytes, total_bits, tell, silence, tone_frequency,
                                               toneishness, input_metrics.abs_sum);
  const bool transient_enabled = LM > 0 && ec_tell(enc) + 3 <= total_bits;
  const int transient_got_disabled = !transient_enabled;
  const int shortBlocks = transient_enabled && isTransient ? 1 << LM : 0;
  std::array<celt_sig, celt_max_channels * celt_max_frame_samples> frequency_storage;
  auto* freq = frequency_storage.data();
  compute_mdcts(shortBlocks, in, freq, C, CC, LM, st->upsample);
  celt_ener* bandE = in;
  celt_glog* bandLogE = bandE + nbEBands * CC;
  celt_glog* bandLogE2 = bandLogE + nbEBands * CC;
  compute_band_energies_and_normalise(freq, bandE, bandLogE, start, end, C, LM);
  const auto temporal_vbr = celt_update_temporal_vbr(st, bandLogE, LM, shortBlocks);
  copy_n_items(bandLogE, static_cast<std::size_t>(C * nbEBands), bandLogE2);
  if (transient_enabled) {
    ec_enc_bit_logp(enc, isTransient, 3);
  }
  auto* X = freq;
  std::array<std::array<int, celt_default_nb_ebands>, 6> band_workspace;
  auto& [offsets, tf_res, cap, fine_quant, pulses, fine_priority] = band_workspace;
  opus_int32 tot_boost = 0;
  const opus_val32 maxDepth = dynalloc_analysis(st, bandLogE, bandLogE2, oldBandE, offsets.data(), isTransient, LM, effectiveBytes,
                                                &tot_boost, tone_frequency, toneishness);
  std::fill_n(tf_res.data(), static_cast<std::size_t>(end), st->lowrate_refinement ? 1 : 0);
  auto* error = bandLogE2;
  zero_n_items(error, static_cast<std::size_t>(C * nbEBands));
  if (st->bitrate < celt_energy_feedback_bypass_min_bps || st->bitrate >= celt_energy_feedback_bypass_max_bps) {
    for_each_celt_band(st, [&](int index) {
      if (std::fabs(bandLogE[index] - oldBandE[index]) < 2.f) {
        bandLogE[index] -= 0.25f * energyError[index];
      }
    });
  }
  quant_coarse_energy(start, end, bandLogE, oldBandE, total_bits, error, enc, C, LM, nbAvailableBytes, st->prediction_disabled,
                      &st->delayedIntra);
  process_tf_changes<true>(start, end, isTransient, tf_res.data(), LM, enc);
  const bool signal_spread = ec_tell(enc) + 4 <= total_bits;
  int spread_decision = 2;
  if (signal_spread) {
    spread_decision = 0;
    if (st->audio_application && C == 2 && st->input_diff_Q10 >= 128 && 4 * effectiveBytes < C * N && toneishness >= .40f &&
        (hybrid ? 5 * st->bitrate >= st->silk_info.bitrateBps : 16 * effectiveBytes >= C * N)) {
      spread_decision = 2;
    }
    ec_enc_icdf(enc, spread_decision, spread_icdf.data(), 5);
  }
  init_caps(cap, LM, C);
  opus_int32 total_boost;
  tell = process_celt_dynalloc<true>(enc, {eBands, static_cast<std::size_t>(nbEBands + 1)}, offsets, cap, start, end, C, LM, total_bits,
                                     total_boost);
  int dual_stereo = 0;
  if (C == 2) {
    if (LM != 0) {
      dual_stereo = stereo_analysis(X, LM, N);
    }
    st->intensity =
        hysteresis_decision(equiv_rate / 1000, stereo_intensity_table.thresholds, stereo_intensity_table.hysteresis, st->intensity);
    st->intensity = clamp_value(st->intensity, start, end);
  }
  int alloc_trim = 5;
  if (tell + (6 << 3) <= total_bits - total_boost) {
    if (start > 0) {
      st->stereo_saving = 0;
      alloc_trim = 5;
    } else {
      alloc_trim = alloc_trim_analysis(X, bandLogE, end, LM, C, N, &st->stereo_saving, st->intensity, equiv_rate);
    }
    alloc_trim = celt_adjust_alloc_trim(alloc_trim, st, hybrid, C, toneishness);
    if (!hybrid && C == 2) {
      alloc_trim = celt_balance_lowrate_stereo_trim(alloc_trim, bandLogE, end, LM, effectiveBytes, total_boost, offsets);
    }
    ec_enc_icdf(enc, alloc_trim, trim_icdf.data(), 7);
    tell = ec_tell_frac(enc);
  }
  int min_allowed = ((tell + total_boost + (1 << (3 + 3)) - 1) >> (3 + 3)) + 2;
  if (hybrid) {
    min_allowed = std::max(min_allowed, (tell0_frac + (37 << 3) + total_boost + (1 << (3 + 3)) - 1) >> (3 + 3));
  }
  if (vbr_rate > 0) {
    const int lm_diff = celt_max_lm - LM;
    nbCompressedBytes = std::min(nbCompressedBytes, 1275 >> (3 - LM));
    opus_int32 base_target = !hybrid ? vbr_rate - ((40 * C + 20) << 3) : std::max(0, vbr_rate - ((9 * C + 4) << 3));
    if (st->constrained_vbr) {
      base_target += (st->vbr_offset >> lm_diff);
    }
    opus_int32 target = hybrid ? celt_hybrid_target(base_target, LM, st->silk_info.offset)
                               : compute_vbr(base_target, LM, equiv_rate, st->lastCodedBands, C, st->intensity, st->constrained_vbr,
                                             st->stereo_saving, tot_boost, maxDepth, temporal_vbr, st->content_vbr);
    if (!hybrid) {
      if (C == 1 && st->bitrate <= low_rate_tonal_celt_max_bps && toneishness > low_rate_tonal_celt_min_tone) {
        target += bitrate_to_bits(low_rate_tonal_celt_boost_bps, celt_sample_rate, frame_size) << 3;
      }
      if (C == 1 && st->midrate_quality_boost_bps > 0) {
        target += bitrate_to_bits(st->midrate_quality_boost_bps, celt_sample_rate, frame_size) << 3;
      }
    } else {
      if (st->high_z_tonal_Q7 > 64 && st->silk_info.bitrateBps >= 56000 && st->silk_info.bitrateBps < 80000) {
        target += bitrate_to_bits(5000, celt_sample_rate, frame_size) << 3;
      }
      const opus_val32 tone_gate = st->silk_info.bitrateBps >= 48000 ? .99f : .80f;
      if (C == 2 && st->silk_info.bitrateBps >= 32000 && st->silk_info.offset < 100 && toneishness > tone_gate) {
        const opus_int32 residual_celt_bps = st->silk_info.bitrateBps - st->silk_info.actualSilkBps;
        if (residual_celt_bps > st->bitrate) {
          target += bitrate_to_bits(residual_celt_bps - st->bitrate, celt_sample_rate, frame_size) << 3;
        }
      }
    }
    target += tell;
    nbAvailableBytes = std::min(nbCompressedBytes, std::max(min_allowed, (target + (1 << (3 + 2))) >> (3 + 3)));
    opus_int32 delta = target - vbr_rate;
    target = nbAvailableBytes << (3 + 3);
    if (silence) {
      nbAvailableBytes = 2;
      target = 2 * 8 << 3;
      delta = 0;
    }
    const opus_val16 alpha = st->vbr_count < 970 ? 1.f / (++st->vbr_count + 20) : .001f;
    if (st->constrained_vbr && !st->content_vbr) {
      st->vbr_reservoir += target - vbr_rate;
      st->vbr_drift += static_cast<opus_int32>((alpha) * ((delta * (1 << lm_diff)) - st->vbr_offset - st->vbr_drift));
      st->vbr_offset = -st->vbr_drift;
      if (st->vbr_reservoir < 0) {
        const int adjust = (-st->vbr_reservoir) / (8 << 3);
        nbAvailableBytes += silence ? 0 : adjust;
        st->vbr_reservoir = 0;
      }
    }
    nbCompressedBytes = std::min(nbCompressedBytes, nbAvailableBytes);
    ec_enc_shrink(enc, nbCompressedBytes);
  }
  opus_int32 bits = ((static_cast<opus_int32>(nbCompressedBytes) * 8) << 3) - static_cast<opus_int32>(ec_tell_frac(enc)) - 1;
  const int anti_collapse_rsv = celt_anti_collapse_reserve(isTransient, LM, bits);
  bits -= anti_collapse_rsv;
  opus_int32 balance;
  const int codedBands =
      clt_compute_allocation(start, end, offsets.data(), cap.data(), alloc_trim, &st->intensity, &dual_stereo, bits, &balance,
                             pulses.data(), fine_quant.data(), fine_priority.data(), C, LM, enc, 1, st->lastCodedBands, end - 1);
  st->lastCodedBands =
      static_cast<opus_uint8>(st->lastCodedBands ? clamp_value(codedBands, st->lastCodedBands - 1, st->lastCodedBands + 1) : codedBands);
  process_fine_energy<true>(start, end, oldBandE, error, nullptr, fine_quant.data(), enc, C);
  zero_n_items(energyError, static_cast<std::size_t>(nbEBands * CC));
  quant_all_bands(1, start, end, X, C == 2 ? X + N : nullptr, nullptr, bandE, pulses.data(), shortBlocks, spread_decision, dual_stereo,
                  st->intensity, tf_res.data(), nbCompressedBytes * (8 << 3) - anti_collapse_rsv, balance, enc, LM, codedBands, &st->rng,
                  0);
  if (anti_collapse_rsv > 0) {
    ec_enc_bits(enc, st->consec_transient < 2, 1);
  }
  process_energy_finalise<true>(start, end, oldBandE, error, fine_quant.data(), fine_priority.data(), nbCompressedBytes * 8 - ec_tell(enc),
                                enc, C);
  for_each_celt_band(st, [&](int index) {
    energyError[index] = clamp_value(error[index], -0.5f, 0.5f);
  });
  if (silence) {
    std::fill_n(oldBandE, static_cast<std::size_t>(C * nbEBands), -(28.f));
  }
  st->prefilter_period = static_cast<opus_uint16>(prefilter.pitch_index);
  st->prefilter_gain = prefilter.gain;
  celt_commit_band_state(oldBandE, oldLogE, oldLogE2, CC, start, end, isTransient, CC == 2 && C == 1);
  st->consec_transient = isTransient || transient_got_disabled ? st->consec_transient + (st->consec_transient < 2) : 0;
  st->rng = enc->rng;
  ec_enc_done(enc);
  return enc->error ? -3 : nbCompressedBytes;
}

static void celt_encoder_reset_state(CeltEncoderInternal* st) {
  static_assert(std::is_standard_layout_v<CeltEncoderInternal>);
  zero_object_tail(*st, offsetof(CeltEncoderInternal, rng));
  zero_n_items(celt_encoder_storage(st), celt_encoder_storage_count(st->channels));
  const auto views = make_celt_encoder_views(st);
  const auto band_count = static_cast<std::size_t>(st->channels * celt_default_nb_ebands);
  std::fill_n(views.oldLogE, band_count, -(28.f));
  std::fill_n(views.oldLogE2, band_count, -(28.f));
  st->vbr_offset = 0;
  st->delayedIntra = 1;
}

[[nodiscard]] static inline auto celt_decoder_storage(CeltDecoderInternal* st) noexcept -> celt_sig* {
  static_assert(sizeof(CeltDecoderInternal) % alignof(celt_sig) == 0);
  return reinterpret_cast<celt_sig*>(reinterpret_cast<std::byte*>(st) + sizeof(CeltDecoderInternal));
}

[[nodiscard]] static bool decoder_has_speech_activity(OpusDecoder* st) noexcept {
  const auto* celt_dec = decoder_celt_state(st);
  if (st->mode == opus_mode_celt_only) {
    return celt_dec->postfilter_gain > 0 && celt_dec->postfilter_period >= 15;
  }
  const auto* silk_dec = static_cast<const silk_decoder_state*>(decoder_silk_state(st));
  return silk_dec->lossCnt == 0 && silk_dec->prevSignalType != 0;
}

[[nodiscard]] static opus_val16 decoder_output_postfilter_gain(OpusDecoder* st, int packet_bitrate_bps) noexcept {
  auto* celt_dec = decoder_celt_state(st);
  const int level = celt_dec->output_postfilter_level;
  if (level <= 0) {
    return 0;
  }
  if (level == 1) {
    return 0.025f;
  }
  if (level == 2) {
    return 0.10f;
  }

  if (st->channels == 1) {
    auto& average_bitrate = celt_dec->output_postfilter_average_bitrate;
    average_bitrate = average_bitrate == 0 ? packet_bitrate_bps : (7 * average_bitrate + packet_bitrate_bps + 4) / 8;
    opus_val16 gain = 0;
    if (st->mode == opus_mode_celt_only && packet_bitrate_bps < 20000) {
      gain = 0.30f;
    } else if (packet_bitrate_bps >= 28000 && packet_bitrate_bps < 40000) {
      gain = 0.245f;
    } else if (packet_bitrate_bps >= 80000) {
      gain = 0.15f;
    } else if (st->mode == opus_mode_celt_only && average_bitrate >= 20000 && average_bitrate < 28000) {
      gain = 0.08f;
    } else {
      celt_dec->output_postfilter_auto_hold = std::max(0, celt_dec->output_postfilter_auto_hold - 1);
      return 0;
    }
    const bool speech_active = decoder_has_speech_activity(st);
    celt_dec->output_postfilter_auto_hold = speech_active ? 3 : std::max(0, celt_dec->output_postfilter_auto_hold - 1);
    return speech_active || celt_dec->output_postfilter_auto_hold > 0 ? gain : 0;
  }

  if (st->channels == 2 && st->mode != opus_mode_celt_only && packet_bitrate_bps >= 20000 && packet_bitrate_bps < 36000 &&
      decoder_has_speech_activity(st)) {
    return 0.125f;
  }
  return 0;
}

struct output_postfilter_parameters {
  opus_val16 gain;
  opus_val16 energy_scale;
  opus_val16 pole;
};

[[nodiscard]] static auto prepare_output_postfilter(CeltDecoderInternal* st, int channels, opus_val16 target_gain) noexcept
    -> output_postfilter_parameters {
  const int level = st->output_postfilter_level;
  opus_val16 gain = target_gain;
  if (level == 3 && channels == 1) {
    if (st->output_postfilter_smoothed_gain == 0) {
      st->output_postfilter_smoothed_gain = target_gain;
    }
    const auto smoothing = st->output_postfilter_auto_hold > 0 ? 0.75f : 0.25f;
    st->output_postfilter_smoothed_gain += smoothing * (target_gain - st->output_postfilter_smoothed_gain);
    gain = st->output_postfilter_smoothed_gain;
  }
  if (gain <= 1e-5f) {
    st->output_postfilter_smoothed_gain = 0;
    return {0, 1, 0.08f};
  }
  const bool smoothing_active = level == 3 && channels == 1 && std::abs(target_gain - gain) > 1e-5f;
  const opus_val16 energy_scale = smoothing_active ? 1.0f + std::min(0.01f, 0.04f * gain) : 1.0f;
  const opus_val16 pole = level == 3 && channels == 2 && gain < 0.05f ? 0.076f : 0.08f;
  return {gain, energy_scale, pole};
}

template <typename Output>
static void celt_decoder_write_output_postfilter(CeltDecoderInternal* st, const opus_res* input, Output* output, int samples, int channels,
                                                 opus_val16 target_gain) noexcept {
  if (st->output_postfilter_level <= 0 || input == nullptr || output == nullptr || samples <= 0) {
    return;
  }
  const auto [gain, energy_scale, pole] = prepare_output_postfilter(st, channels, target_gain);
  if (gain == 0) {
    for (int channel = 0; channel < channels; ++channel) {
      st->output_postfilter_mem[channel] = input[(samples - 1) * channels + channel];
    }
    if constexpr (std::same_as<Output, opus_int16>) {
      celt_float2int16_c(input, output, static_cast<std::size_t>(samples * channels));
    }
    return;
  }
  const auto correction = gain * (1.0f - pole);
  if (channels == 1) {
    auto low = st->output_postfilter_mem[0];
    for (int i = 0; i < samples; ++i) {
      const opus_res x = input[i];
      opus_res filtered;
      if constexpr (std::same_as<Output, opus_int16>) {
        const auto delta = x - low;
        low += pole * delta;
        filtered = clamp_value((x - correction * delta) * energy_scale, -1.0f, 1.0f);
      } else {
        low += pole * (x - low);
        filtered = clamp_value((x - gain * (x - low)) * energy_scale, -1.0f, 1.0f);
      }
      if constexpr (std::same_as<Output, opus_int16>) {
        output[i] = FLOAT2INT16(filtered);
      } else {
        output[i] = filtered;
      }
    }
    st->output_postfilter_mem[0] = low;
    return;
  }
  for (int i = 0; i < samples; ++i) {
    const auto base = i * channels;
    for (int channel = 0; channel < channels; ++channel) {
      auto& low = st->output_postfilter_mem[channel];
      const opus_res x = input[base + channel];
      opus_res filtered;
      if constexpr (std::same_as<Output, opus_int16>) {
        const auto delta = x - low;
        low += pole * delta;
        filtered = clamp_value((x - correction * delta) * energy_scale, -1.0f, 1.0f);
      } else {
        low += pole * (x - low);
        filtered = clamp_value((x - gain * (x - low)) * energy_scale, -1.0f, 1.0f);
      }
      if constexpr (std::same_as<Output, opus_int16>) {
        output[base + channel] = FLOAT2INT16(filtered);
      } else {
        output[base + channel] = filtered;
      }
    }
  }
}

static void apply_decoder_output_postfilter(OpusDecoder* st, opus_res* pcm, int samples, opus_int32 packet_bytes) {
  const auto packet_bitrate_bps = decoder_packet_bitrate(st, packet_bytes, samples);
  celt_decoder_write_output_postfilter(decoder_celt_state(st), pcm, pcm, samples, st->channels,
                                       decoder_output_postfilter_gain(st, packet_bitrate_bps));
}

static void convert_decoder_output_postfilter(OpusDecoder* st, const opus_res* input, opus_int16* output, int samples,
                                              opus_int32 packet_bytes) {
  const auto packet_bitrate_bps = decoder_packet_bitrate(st, packet_bytes, samples);
  celt_decoder_write_output_postfilter(decoder_celt_state(st), input, output, samples, st->channels,
                                       decoder_output_postfilter_gain(st, packet_bitrate_bps));
}

constexpr int celt_decoder_energy_channel_count = 2;
[[nodiscard]] static constexpr auto celt_decoder_storage_count(int channels) noexcept -> std::size_t {
  return static_cast<std::size_t>(channels) * (celt_decoder_history_size + celt_default_overlap) +
         4 * celt_decoder_energy_channel_count * celt_default_nb_ebands + static_cast<std::size_t>(channels) * 24;
}

static void celt_decoder_reset_state(CeltDecoderInternal* st) {
  constexpr auto energy_channels = celt_decoder_energy_channel_count;
  static_assert(std::is_standard_layout_v<CeltDecoderInternal>);
  zero_object_tail(*st, offsetof(CeltDecoderInternal, last_pitch_index));
  zero_n_items(celt_decoder_storage(st), celt_decoder_storage_count(st->channels));
  auto* old_band_energy =
      reinterpret_cast<celt_glog*>(celt_decoder_storage(st) + (celt_decoder_history_size + celt_default_overlap) * st->channels);
  auto* old_log_energy = old_band_energy + energy_channels * celt_default_nb_ebands;
  auto* old_log_energy2 = old_log_energy + energy_channels * celt_default_nb_ebands;
  const auto band_count = static_cast<std::size_t>(energy_channels * celt_default_nb_ebands);
  std::fill_n(old_log_energy, band_count, -(28.f));
  std::fill_n(old_log_energy2, band_count, -(28.f));
  st->skip_plc = 1;
  st->last_frame_type = 0;
}

static void celt_decoder_init(CeltDecoderInternal* st, opus_int32 sampling_rate, int channels) {
  *st = {};
  std::uninitialized_default_construct_n(celt_decoder_storage(st), celt_decoder_storage_count(channels));
  st->channels = channels;
  st->downsample = resampling_factor(sampling_rate);
  celt_decoder_reset_state(st);
}

template <typename Sample> [[nodiscard]] static inline auto deemphasis_output(celt_sig sample) noexcept -> Sample {
  if constexpr (std::same_as<Sample, opus_int16>) {
    return static_cast<opus_int16>(pcm_float2int(clamp_value(sample, -32768.f, 32767.f)));
  }
  return signal_to_float_pcm(sample);
}

template <typename Sample> static inline void deemphasis_stereo_simple(celt_sig* x0, celt_sig* x1, Sample* pcm, int N, celt_sig* mem) {
  celt_sig m0, m1;
  int j;
  m0 = mem[0];
  m1 = mem[1];
  for (j = 0; j < N; j++) {
    celt_sig tmp0 = x0[j] + 1e-30f + m0, tmp1 = x1[j] + 1e-30f + m1;
    m0 = celt_preemphasis[0] * tmp0;
    m1 = celt_preemphasis[0] * tmp1;
    pcm[2 * j] = deemphasis_output<Sample>(tmp0);
    pcm[2 * j + 1] = deemphasis_output<Sample>(tmp1);
  }
  mem[0] = zero_tiny_float_mem(m0);
  mem[1] = zero_tiny_float_mem(m1);
}

template <typename Sample> static inline void deemphasis_mono_simple(celt_sig* x, Sample* pcm, int N, celt_sig* mem) {
  celt_sig m = mem[0];
  for (int j = 0; j < N; ++j) {
    const celt_sig tmp = x[j] + 1e-30f + m;
    m = celt_preemphasis[0] * tmp;
    pcm[j] = deemphasis_output<Sample>(tmp);
  }
  mem[0] = zero_tiny_float_mem(m);
}

static void deemphasis_postfiltered_pcm16(CeltDecoderInternal* st, celt_sig* const* input, opus_int16* output, int samples, int channels,
                                          output_postfilter_parameters parameters) noexcept {
  const auto [gain, energy_scale, pole] = parameters;
  for (int channel = 0; channel < channels; ++channel) {
    celt_sig deemphasis_mem = st->preemph_memD[channel];
    opus_res low = st->output_postfilter_mem[channel];
    if (gain == 0) {
      celt_sig last_sample = 0;
      for (int index = 0; index < samples; ++index) {
        const celt_sig sample = input[channel][index] + 1e-30f + deemphasis_mem;
        deemphasis_mem = celt_preemphasis[0] * sample;
        last_sample = sample;
        output[index * channels + channel] = deemphasis_output<opus_int16>(sample);
      }
      low = signal_to_float_pcm(last_sample);
    } else {
      const auto correction = gain * (1.0f - pole);
      if (gain <= 0.08f && energy_scale == 1.0f) {
        const auto pair_pole = pole * (2.0f - pole);
        int index = 0;
        for (; index + 1 < samples; index += 2) {
          const celt_sig sample0 = input[channel][index] + 1e-30f + deemphasis_mem;
          deemphasis_mem = celt_preemphasis[0] * sample0;
          const opus_res x0 = signal_to_float_pcm(sample0);
          const celt_sig sample1 = input[channel][index + 1] + 1e-30f + deemphasis_mem;
          deemphasis_mem = celt_preemphasis[0] * sample1;
          const opus_res x1 = signal_to_float_pcm(sample1);
          output[index * channels + channel] = FLOAT2INT16(x0 - correction * (x0 - low));
          output[(index + 1) * channels + channel] = FLOAT2INT16(x1 - correction * (x1 - low));
          low += pair_pole * (0.5f * (x0 + x1) - low);
        }
        if (index < samples) {
          const celt_sig sample = input[channel][index] + 1e-30f + deemphasis_mem;
          deemphasis_mem = celt_preemphasis[0] * sample;
          const opus_res x = signal_to_float_pcm(sample);
          const auto delta = x - low;
          low += pole * delta;
          output[index * channels + channel] = FLOAT2INT16(x - correction * delta);
        }
      } else {
        for (int index = 0; index < samples; ++index) {
          const celt_sig sample = input[channel][index] + 1e-30f + deemphasis_mem;
          deemphasis_mem = celt_preemphasis[0] * sample;
          const opus_res x = signal_to_float_pcm(sample);
          const auto delta = x - low;
          low += pole * delta;
          output[index * channels + channel] = FLOAT2INT16(clamp_value((x - correction * delta) * energy_scale, -1.0f, 1.0f));
        }
      }
    }
    st->preemph_memD[channel] = zero_tiny_float_mem(deemphasis_mem);
    st->output_postfilter_mem[channel] = low;
  }
}

static void deemphasis(celt_sig* const* in, opus_res* pcm, int N, int C, int downsample, celt_sig* mem) {
  if (downsample == 1 && C == 1) {
    deemphasis_mono_simple(in[0], pcm, N, mem);
    return;
  }
  if (downsample == 1 && C == 2) {
    deemphasis_stereo_simple(in[0], in[1], pcm, N, mem);
    return;
  }
  for (int c = 0; c < C; ++c) {
    celt_sig m = mem[c];
    int next_output = 0;
    int output = 0;
    for (int j = 0; j < N; ++j) {
      const celt_sig sample = in[c][j] + 1e-30f + m;
      m = celt_preemphasis[0] * sample;
      if (j == next_output) {
        pcm[output++ * C + c] = signal_to_float_pcm(sample);
        next_output += downsample;
      }
    }
    mem[c] = zero_tiny_float_mem(m);
  }
}

static void celt_synthesis(celt_norm* X, celt_sig* const* out_syn, celt_glog* oldBandE, int start, int effEnd, int C, int CC,
                           int isTransient, int LM, int downsample, int silence) {
  int c, i, M, b, B, N, NB, shift, nbEBands, overlap;
  overlap = celt_default_overlap;
  nbEBands = celt_default_nb_ebands;
  N = celt_short_mdct_size << LM;
  M = 1 << LM;
  if (isTransient) {
    B = M;
    NB = celt_short_mdct_size;
    shift = celt_max_lm;
  } else {
    B = 1;
    NB = celt_short_mdct_size << LM;
    shift = celt_max_lm - LM;
  }
  if (CC == C && C == 2) {
    auto* freq0 = X;
    auto* freq1 = X + N;
    denormalise_bands<2>({freq0, freq1}, {freq0, freq1}, {oldBandE, oldBandE + nbEBands}, start, effEnd, M, downsample, silence);
    if (B == 1 && shift == 0) {
      clt_mdct_backward_stereo_20ms_c(&celt_mode()->mdct, freq0, freq1, out_syn[0], out_syn[1], celt_mode()->window, overlap);
    } else
      for (b = 0; b < B; b++) {
        clt_mdct_backward_c(&celt_mode()->mdct, &freq0[b], out_syn[0] + NB * b, celt_mode()->window, overlap, shift, B);
        clt_mdct_backward_c(&celt_mode()->mdct, &freq1[b], out_syn[1] + NB * b, celt_mode()->window, overlap, shift, B);
      }
  } else if (CC == C) {
    for (c = 0; c < CC; ++c) {
      auto* freq = X + c * N;
      denormalise_bands<1>({freq}, {freq}, {oldBandE + c * nbEBands}, start, effEnd, M, downsample, silence);
      for (b = 0; b < B; b++) {
        clt_mdct_backward_c(&celt_mode()->mdct, &freq[b], out_syn[c] + NB * b, celt_mode()->window, overlap, shift, B);
      }
    }
  } else if (CC == 2) {
    auto* freq = X;
    denormalise_bands<1>({X}, {freq}, {oldBandE}, start, effEnd, M, downsample, silence);
    for (b = 0; b < B; b++) {
      clt_mdct_backward_dual_history_c(&celt_mode()->mdct, &freq[b], out_syn[0] + NB * b, out_syn[1] + NB * b, celt_mode()->window, overlap,
                                       shift, B);
    }
  } else {
    auto* freq = X;
    auto* freq2 = out_syn[0] + overlap / 2;
    denormalise_bands<1>({X}, {freq}, {oldBandE}, start, effEnd, M, downsample, silence);
    denormalise_bands<1>({X + N}, {freq2}, {oldBandE + nbEBands}, start, effEnd, M, downsample, silence);
    for (i = 0; i < N; i++) {
      freq[i] = (((.5f * (freq[i]))) + ((.5f * (freq2[i]))));
    }
    for (b = 0; b < B; b++) {
      clt_mdct_backward_c(&celt_mode()->mdct, &freq[b], out_syn[0] + NB * b, celt_mode()->window, overlap, shift, B);
    }
  }
}

constexpr int celt_decode_buffer_size = 2048, celt_plc_max_period = 1024, celt_lpc_order = 24;
struct celt_decoder_views {
  std::array<celt_sig*, 2> decode_mem, out_syn;
  celt_glog *oldBandE{}, *oldLogE{}, *oldLogE2{}, *backgroundLogE{};
  opus_val16* lpc{};
};
[[nodiscard]] static auto make_celt_decoder_views(CeltDecoderInternal* st, int N) noexcept -> celt_decoder_views {
  const auto overlap = celt_default_overlap;
  auto* const decode_storage = celt_decoder_storage(st);
  celt_decoder_views views{};
  constexpr auto energy_channels = celt_decoder_energy_channel_count;
  for (int channel = 0; channel < st->channels; ++channel) {
    views.decode_mem[channel] = decode_storage + channel * (celt_decode_buffer_size + overlap);
    views.out_syn[channel] = views.decode_mem[channel] + celt_decode_buffer_size - N;
  }
  views.oldBandE = reinterpret_cast<celt_glog*>(decode_storage + (celt_decode_buffer_size + overlap) * st->channels);
  views.oldLogE = views.oldBandE + energy_channels * celt_default_nb_ebands;
  views.oldLogE2 = views.oldLogE + energy_channels * celt_default_nb_ebands;
  views.backgroundLogE = views.oldLogE2 + energy_channels * celt_default_nb_ebands;
  views.lpc = reinterpret_cast<opus_val16*>(views.backgroundLogE + energy_channels * celt_default_nb_ebands);
  return views;
}

static void celt_slide_decode_history(celt_sig* const* decode_mem, int channels, int N) {
  const auto count = static_cast<std::size_t>(celt_decode_buffer_size - N + celt_default_overlap);
  if (channels == 2) {
    move_n_items(decode_mem[0] + N, count, decode_mem[0]);
    move_n_items(decode_mem[1] + N, count, decode_mem[1]);
    return;
  }
  move_n_items(decode_mem[0] + N, count, decode_mem[0]);
}

static void celt_apply_postfilter(CeltDecoderInternal* st, celt_sig* const* out_syn, int channels, int N, int LM, int target_period,
                                  opus_val16 target_gain, int target_tapset) {
  if (st->postfilter_gain_old == 0 && st->postfilter_gain == 0 && target_gain == 0) {
    st->postfilter_period_old = target_period;
    st->postfilter_period = target_period;
    st->postfilter_gain_old = st->postfilter_gain = 0;
    st->postfilter_tapset_old = target_tapset;
    st->postfilter_tapset = target_tapset;
    return;
  }
  for (int channel = 0; channel < channels; ++channel) {
    st->postfilter_period = std::max(st->postfilter_period, 15);
    st->postfilter_period_old = std::max(st->postfilter_period_old, 15);
    comb_filter(out_syn[channel], out_syn[channel], st->postfilter_period_old, st->postfilter_period, celt_short_mdct_size,
                st->postfilter_gain_old, st->postfilter_gain, st->postfilter_tapset_old, st->postfilter_tapset, celt_mode()->window,
                celt_default_overlap);
    if (LM != 0)
      comb_filter(out_syn[channel] + celt_short_mdct_size, out_syn[channel] + celt_short_mdct_size, st->postfilter_period, target_period,
                  N - celt_short_mdct_size, st->postfilter_gain, target_gain, st->postfilter_tapset, target_tapset, celt_mode()->window,
                  celt_default_overlap);
  }
  st->postfilter_period_old = st->postfilter_period;
  st->postfilter_gain_old = st->postfilter_gain;
  st->postfilter_tapset_old = st->postfilter_tapset;
  st->postfilter_period = target_period;
  st->postfilter_gain = target_gain;
  st->postfilter_tapset = target_tapset;
  if (LM != 0) {
    st->postfilter_period_old = st->postfilter_period;
    st->postfilter_gain_old = st->postfilter_gain;
    st->postfilter_tapset_old = st->postfilter_tapset;
  }
}

static void celt_plc_extrapolate_channel(celt_sig* buf, opus_val16* lpc, int N, int pitch_index, int exc_length, opus_val16 fade,
                                         bool update_lpc) {
  std::array<opus_val16, celt_plc_max_period + celt_lpc_order> exc_storage;
  std::array<opus_val16, celt_lpc_order> lpc_mem;
  auto* exc = exc_storage.data() + celt_lpc_order;
  opus_val16 decay, attenuation;
  opus_val32 S1 = 0;
  copy_n_items(buf + celt_decode_buffer_size - celt_plc_max_period - celt_lpc_order,
               static_cast<std::size_t>(celt_plc_max_period + celt_lpc_order), exc_storage.data());
  if (update_lpc) {
    std::array<opus_val32, celt_lpc_order + 1> ac;
    _celt_autocorr(exc, ac.data(), celt_mode()->window, celt_default_overlap, celt_lpc_order, celt_plc_max_period);
    ac[0] *= 1.0001f;
    for (int i = 1; i <= celt_lpc_order; ++i) {
      ac[i] -= ac[i] * (0.008f * 0.008f) * i * i;
    }
    _celt_lpc(lpc, ac.data(), celt_lpc_order);
  }
  const auto safe_exc_length = std::max(exc_length, 0);
  std::array<opus_val16, celt_plc_max_period> fir_tmp;
  celt_fir_c(exc + celt_plc_max_period - safe_exc_length, lpc, fir_tmp.data(), safe_exc_length);
  copy_n_items(fir_tmp.data(), static_cast<std::size_t>(safe_exc_length), exc + celt_plc_max_period - safe_exc_length);
  {
    opus_val32 E1 = 1, E2 = 1;
    const auto decay_length = exc_length >> 1;
    for (int i = 0; i < decay_length; ++i) {
      auto sample = exc[celt_plc_max_period - decay_length + i];
      E1 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
      sample = exc[celt_plc_max_period - 2 * decay_length + i];
      E2 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
    }
    E1 = std::min(E1, E2);
    decay = static_cast<opus_val16>(std::sqrt(static_cast<float>(E1) / E2));
  }
  move_n_items(buf + N, static_cast<std::size_t>(celt_decode_buffer_size - N), buf);
  const auto extrapolation_offset = celt_plc_max_period - pitch_index, extrapolation_len = N + celt_default_overlap;
  attenuation = fade * decay;
  for (int i = 0, j = 0; i < extrapolation_len; ++i, ++j) {
    if (j >= pitch_index) {
      j -= pitch_index;
      attenuation *= decay;
    }
    buf[celt_decode_buffer_size - N + i] = attenuation * exc[extrapolation_offset + j];
    const auto sample = buf[celt_decode_buffer_size - celt_plc_max_period - N + extrapolation_offset + j];
    S1 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
  }
  for (int i = 0; i < celt_lpc_order; ++i) {
    lpc_mem[i] = buf[celt_decode_buffer_size - N - 1 - i];
  }
  celt_iir(buf + celt_decode_buffer_size - N, lpc, buf + celt_decode_buffer_size - N, extrapolation_len, lpc_mem.data());
  {
    opus_val32 S2 = 0;
    for (int i = 0; i < extrapolation_len; ++i) {
      const auto sample = buf[celt_decode_buffer_size - N + i];
      S2 += static_cast<opus_val32>(sample) * static_cast<opus_val32>(sample);
    }
    if (!(S1 > 0.2f * S2)) {
      zero_n_items(buf + celt_decode_buffer_size - N, static_cast<std::size_t>(extrapolation_len));
    } else if (S1 < S2) {
      const auto ratio = static_cast<opus_val16>(std::sqrt(static_cast<float>(S1 + 1) / (S2 + 1)));
      for (int i = 0; i < celt_default_overlap; ++i) {
        const auto gain = 1.0f - celt_mode()->window[i] * (1.0f - ratio);
        buf[celt_decode_buffer_size - N + i] *= gain;
      }
      for (int i = celt_default_overlap; i < extrapolation_len; ++i) {
        buf[celt_decode_buffer_size - N + i] *= ratio;
      }
    }
  }
}

static inline int celt_plc_pitch_search(std::span<celt_sig* const> decode_mem) {
  int pitch_index;
  std::array<opus_val16, (2048 >> 1)> lp_pitch_buf;
  pitch_downsample(decode_mem.data(), static_cast<int>(decode_mem.size()), lp_pitch_buf.data(), 2048 >> 1);
  pitch_search(lp_pitch_buf.data() + ((720) >> 1), lp_pitch_buf.data(), 2048 - (720), (720) - (100), &pitch_index);
  pitch_index = (720) - pitch_index;
  return pitch_index;
}

static void prefilter_and_fold(CeltDecoderInternal* st, int N) {
  constexpr auto overlap = celt_default_overlap;
  const auto channels = st->channels;
  const auto decoder = make_celt_decoder_views(st, N);
  std::array<opus_val32, celt_default_overlap> filter_storage;
  auto* etmp = filter_storage.data();
  for (int channel = 0; channel < channels; ++channel) {
    comb_filter(etmp, decoder.decode_mem[channel] + celt_decode_buffer_size - N, st->postfilter_period_old, st->postfilter_period, overlap,
                -st->postfilter_gain_old, -st->postfilter_gain, st->postfilter_tapset_old, st->postfilter_tapset, nullptr, 0);
    for (int i = 0; i < overlap / 2; ++i)
      decoder.decode_mem[channel][celt_decode_buffer_size - N + i] =
          (celt_mode()->window[i] * etmp[overlap - 1 - i]) + (celt_mode()->window[overlap - i - 1] * etmp[i]);
  }
}

static void celt_decode_lost(CeltDecoderInternal* st, int N, int LM) {
  const int C = st->channels;
  constexpr int nbEBands = celt_default_nb_ebands;
  const auto* eBands = celt_mode()->eBands;
  const auto decoder = make_celt_decoder_views(st, N);
  auto decode_mem = decoder.decode_mem;
  auto out_syn = decoder.out_syn;
  auto *oldBandE = decoder.oldBandE, *backgroundLogE = decoder.backgroundLogE, *lpc = decoder.lpc;
  const int loss_duration = st->loss_duration;
  const int start = st->start;
  const bool use_noise_fill = loss_duration >= 40 || start != 0 || st->skip_plc;
  if (use_noise_fill) {
    const int end = st->end;
    const int effEnd = std::max(start, std::min(end, celt_default_nb_ebands));
    std::array<celt_norm, celt_max_channels * celt_max_frame_samples> spectrum;
    zero_n_items(spectrum.data(), static_cast<std::size_t>(C * N));
    celt_slide_decode_history(decode_mem.data(), C, N);
    if (st->last_frame_type == 3) {
      prefilter_and_fold(st, N);
    }
    const celt_glog decay = loss_duration == 0 ? 1.5f : .5f;
    for (int c = 0; c < C; ++c) {
      for (int i = start; i < end; i++) {
        oldBandE[c * nbEBands + i] = std::max(backgroundLogE[c * nbEBands + i], oldBandE[c * nbEBands + i] - decay);
      }
    }
    auto seed = st->rng;
    for (int c = 0; c < C; c++) {
      for (int i = start; i < effEnd; i++) {
        const int boffs = N * c + (eBands[i] << LM);
        const int blen = (eBands[i + 1] - eBands[i]) << LM;
        for (int j = 0; j < blen; j++) {
          seed = celt_lcg_rand(seed);
          spectrum[boffs + j] = static_cast<celt_norm>(static_cast<opus_int32>(seed) >> 20);
        }
        renormalise_vector(spectrum.data() + boffs, blen, 1.0f);
      }
    }
    st->rng = seed;
    celt_synthesis(spectrum.data(), out_syn.data(), oldBandE, start, effEnd, C, C, 0, LM, st->downsample, 0);
    celt_apply_postfilter(st, out_syn.data(), C, N, LM, st->postfilter_period, st->postfilter_gain, st->postfilter_tapset);
    st->skip_plc = 1;
  } else {
    opus_val16 fade = 1.0f;
    int pitch_index;
    if (st->last_frame_type != 3) {
      st->last_pitch_index = pitch_index =
          celt_plc_pitch_search(std::span<celt_sig* const>{decode_mem.data(), static_cast<std::size_t>(C)});
    } else {
      pitch_index = st->last_pitch_index;
      fade = (.8f);
    }
    const auto exc_length = std::min(2 * pitch_index, celt_plc_max_period);
    const auto update_lpc = st->last_frame_type != 3;
    for (int c = 0; c < C; ++c)
      celt_plc_extrapolate_channel(decode_mem[c], lpc + c * celt_lpc_order, N, pitch_index, exc_length, fade, update_lpc);
  }
  st->loss_duration = std::min(10000, loss_duration + (1 << LM));
  st->last_frame_type = use_noise_fill ? 2 : 3;
}

static int celt_decode_with_ec(CeltDecoderInternal* st, const unsigned char* data, int len, opus_res* pcm, int frame_size, ec_dec* dec,
                               opus_int16* pcm16, OpusDecoder* output_filter_decoder, int packet_bitrate_bps) {
  ec_dec _dec;
  const int CC = st->channels;
  const int C = st->stream_channels;
  constexpr int nbEBands = celt_default_nb_ebands;
  const auto* eBands = celt_mode()->eBands;
  const int start = st->start;
  const int end = st->end;
  int intensity = 0, dual_stereo = 0;
  frame_size *= st->downsample;
  const int LM = celt_frame_lm(frame_size);
  if (LM < 0) {
    return -1;
  }
  const int M = 1 << LM;
  if (len < 0 || len > 1275 || (pcm == nullptr && pcm16 == nullptr)) {
    return -1;
  }
  const int N = M * celt_short_mdct_size;
  const auto decoder = make_celt_decoder_views(st, N);
  auto decode_mem = decoder.decode_mem;
  auto out_syn = decoder.out_syn;
  auto *oldBandE = decoder.oldBandE, *oldLogE = decoder.oldLogE, *oldLogE2 = decoder.oldLogE2, *backgroundLogE = decoder.backgroundLogE;
  if (data == nullptr || len <= 1) {
    if (pcm == nullptr) {
      return -1;
    }
    celt_decode_lost(st, N, LM);
    deemphasis(out_syn.data(), pcm, N, CC, st->downsample, st->preemph_memD);
    return frame_size / st->downsample;
  }
  if (st->loss_duration == 0) {
    st->skip_plc = 0;
  }
  if (dec == nullptr) {
    ec_dec_init(&_dec, const_cast<unsigned char*>(data), len);
    dec = &_dec;
  }
  constexpr auto energy_channels = celt_decoder_energy_channel_count;
  if (C == 1) {
    for (int i = 0; i < nbEBands; i++) {
      oldBandE[i] = std::max(oldBandE[i], oldBandE[nbEBands + i]);
    }
  }
  opus_int32 total_bits = len * 8;
  opus_int32 tell = ec_tell(dec);
  const int silence = tell >= total_bits ? 1 : tell == 1 ? ec_dec_bit_logp(dec, 15) : 0;
  if (silence) {
    tell = len * 8;
    dec->nbits_total += tell - ec_tell(dec);
  }
  opus_val16 postfilter_gain = 0;
  int postfilter_pitch = 0;
  int postfilter_tapset = 0;
  if (start == 0 && tell + 16 <= total_bits) {
    if (ec_dec_bit_logp(dec, 1)) {
      const int octave = ec_dec_uint(dec, 6);
      postfilter_pitch = (16 << octave) + ec_dec_bits(dec, 4 + octave) - 1;
      const int qg = ec_dec_bits(dec, 3);
      if (ec_tell(dec) + 2 <= total_bits) {
        postfilter_tapset = ec_dec_icdf(dec, shared_three_step_icdf.data(), 2);
      }
      postfilter_gain = (.09375f) * (qg + 1);
    }
    tell = ec_tell(dec);
  }
  const int isTransient = LM > 0 && tell + 3 <= total_bits ? ec_dec_bit_logp(dec, 3) : 0;
  if (isTransient) {
    tell = ec_tell(dec);
  }
  const int shortBlocks = isTransient ? M : 0;
  const int intra_ener = tell + 3 <= total_bits ? ec_dec_bit_logp(dec, 3) : 0;
  if (!intra_ener && st->loss_duration != 0) {
    for (int c = 0; c < energy_channels; ++c) {
      const celt_glog safety = LM == 0 ? 1.5f : LM == 1 ? .5f : 0;
      const int missing = std::min(10, st->loss_duration >> LM);
      const int base = c * nbEBands;
      for (int i = start; i < end; ++i) {
        const int index = base + i;
        if (oldBandE[index] < std::max(oldLogE[index], oldLogE2[index])) {
          opus_val32 E0 = oldBandE[index];
          const opus_val32 E1 = oldLogE[index];
          const opus_val32 E2 = oldLogE2[index];
          opus_val32 slope = std::max(E1 - E0, .5f * (E2 - E0));
          slope = std::min(slope, (2.f));
          E0 -= std::max(static_cast<opus_val32>(0), (1 + missing) * slope);
          oldBandE[index] = std::max(-(20.f), E0);
        } else {
          oldBandE[index] = std::min(std::min(oldBandE[index], oldLogE[index]), oldLogE2[index]);
        }
        oldBandE[index] -= safety;
      }
    }
  }
  unquant_coarse_energy(start, end, oldBandE, intra_ener, dec, C, LM);
  std::array<std::array<int, celt_default_nb_ebands>, 6> band_workspace;
  auto& [tf_res, cap, offsets, fine_quant, pulses, fine_priority] = band_workspace;
  process_tf_changes<false>(start, end, isTransient, tf_res.data(), LM, dec);
  tell = ec_tell(dec);
  const int spread_decision = tell + 4 <= total_bits ? ec_dec_icdf(dec, spread_icdf.data(), 5) : 2;
  init_caps(cap, LM, C);
  int total_boost;
  tell = process_celt_dynalloc<false>(dec, {eBands, static_cast<std::size_t>(nbEBands + 1)}, offsets, cap, start, end, C, LM, total_bits,
                                      total_boost);
  total_bits -= total_boost;
  const int alloc_trim = tell + (6 << 3) <= total_bits ? ec_dec_icdf(dec, trim_icdf.data(), 7) : 5;
  opus_int32 bits = ((static_cast<opus_int32>(len) * 8) << 3) - static_cast<opus_int32>(ec_tell_frac(dec)) - 1;
  const int anti_collapse_rsv = celt_anti_collapse_reserve(isTransient, LM, bits);
  bits -= anti_collapse_rsv;
  opus_int32 balance;
  const int codedBands = clt_compute_allocation(start, end, offsets.data(), cap.data(), alloc_trim, &intensity, &dual_stereo, bits,
                                                &balance, pulses.data(), fine_quant.data(), fine_priority.data(), C, LM, dec, 0, 0, 0);
  process_fine_energy<false>(start, end, oldBandE, nullptr, nullptr, fine_quant.data(), dec, C);
  std::array<celt_norm, celt_max_channels * celt_max_frame_samples> spectrum;
  celt_slide_decode_history(decode_mem.data(), CC, N);
  std::array<unsigned char, celt_max_channels * celt_default_nb_ebands> collapse_masks;
  quant_all_bands(0, start, end, spectrum.data(), C == 2 ? spectrum.data() + N : nullptr, collapse_masks.data(), nullptr, pulses.data(),
                  shortBlocks, spread_decision, dual_stereo, intensity, tf_res.data(), len * (8 << 3) - anti_collapse_rsv, balance, dec, LM,
                  codedBands, &st->rng, st->channels == 1);
  const int anti_collapse_on = anti_collapse_rsv > 0 ? ec_dec_bits(dec, 1) : 0;
  process_energy_finalise<false>(start, end, oldBandE, nullptr, fine_quant.data(), fine_priority.data(), len * 8 - ec_tell(dec), dec, C);
  if (anti_collapse_on) {
    anti_collapse(spectrum.data(), collapse_masks.data(), LM, C, N, start, end, oldBandE, oldLogE, oldLogE2, pulses.data(), st->rng);
  }
  if (silence) {
    std::fill_n(oldBandE, static_cast<std::size_t>(C * nbEBands), -(28.f));
  }
  if (st->last_frame_type == 3) {
    prefilter_and_fold(st, N);
  }
  celt_synthesis(spectrum.data(), out_syn.data(), oldBandE, start, end, C, CC, isTransient, LM, st->downsample, silence);
  celt_apply_postfilter(st, out_syn.data(), CC, N, LM, postfilter_pitch, postfilter_gain, postfilter_tapset);
  celt_commit_band_state(oldBandE, oldLogE, oldLogE2, energy_channels, start, end, isTransient, energy_channels == 2 && C == 1);
  const celt_glog max_background_increase = std::min(160, st->loss_duration + M) * .001f;
  for (int index = 0; index < CC * nbEBands; ++index) {
    backgroundLogE[index] = std::min(backgroundLogE[index] + max_background_increase, oldBandE[index]);
  }
  st->rng = dec->rng;
  if (output_filter_decoder != nullptr && pcm16 != nullptr && st->downsample == 1) {
    const auto target_gain = decoder_output_postfilter_gain(output_filter_decoder, packet_bitrate_bps);
    const auto parameters = prepare_output_postfilter(st, CC, target_gain);
    deemphasis_postfiltered_pcm16(st, out_syn.data(), pcm16, N, CC, parameters);
  } else if (pcm16 != nullptr && st->downsample == 1) {
    if (CC == 1) {
      deemphasis_mono_simple(out_syn[0], pcm16, N, st->preemph_memD);
    } else {
      deemphasis_stereo_simple(out_syn[0], out_syn[1], pcm16, N, st->preemph_memD);
    }
  } else {
    deemphasis(out_syn.data(), pcm, N, CC, st->downsample, st->preemph_memD);
  }
  st->loss_duration = 0;
  st->last_frame_type = 1;
  return ec_tell(dec) > 8 * len ? -3 : frame_size / st->downsample;
}

static constexpr auto celt_pvq_table_row_count = 15U;
static constexpr auto celt_pvq_fast_row_count = celt_pvq_table_row_count;
static constexpr std::array<opus_uint8, celt_pvq_fast_row_count> celt_pvq_fast_row_max_columns{176, 176, 176, 176, 176, 176, 96, 54,
                                                                                               37,  28,  24,  19,  18,  16,  14};

[[nodiscard]] consteval auto make_celt_pvq_fast_row_offsets() noexcept {
  std::array<opus_uint16, celt_pvq_fast_row_count + 1> offsets{};
  for (std::size_t row = 0; row < celt_pvq_fast_row_count; ++row) {
    offsets[row + 1] = offsets[row] + celt_pvq_fast_row_max_columns[row] + 1U;
  }
  return offsets;
}

static constexpr auto celt_pvq_fast_row_offsets_storage = make_celt_pvq_fast_row_offsets();
static constexpr auto celt_pvq_fast_data_count = celt_pvq_fast_row_offsets_storage.back();

[[nodiscard]] consteval auto make_celt_pvq_u_fast_rows() noexcept {
  std::array<opus_uint32, celt_pvq_fast_data_count> rows{};
  const auto get = [&](std::size_t row, std::size_t column) {
    if (row > column) {
      std::swap(row, column);
    }
    return rows[celt_pvq_fast_row_offsets_storage[row] + column];
  };
  for (std::size_t row_index = 0; row_index < celt_pvq_fast_row_count; ++row_index) {
    const auto offset = celt_pvq_fast_row_offsets_storage[row_index];
    for (unsigned column = 0; column <= celt_pvq_fast_row_max_columns[row_index]; ++column) {
      rows[offset + column] = row_index == 0       ? static_cast<opus_uint32>(column == 0)
                              : column < row_index ? get(column, row_index)
                              : column == 0        ? 0U
                                            : get(row_index - 1, column) + rows[offset + column - 1] + get(row_index - 1, column - 1);
    }
  }
  return rows;
}

static constexpr auto celt_pvq_u_fast_rows_storage = make_celt_pvq_u_fast_rows();

[[nodiscard]] consteval auto make_celt_pvq_fast_rows() noexcept {
  std::array<const opus_uint32*, celt_pvq_fast_row_count> rows{};
  for (std::size_t row = 0; row < rows.size(); ++row) {
    rows[row] = celt_pvq_u_fast_rows_storage.data() + celt_pvq_fast_row_offsets_storage[row];
  }
  return rows;
}

static constexpr auto celt_pvq_fast_rows = make_celt_pvq_fast_rows();

[[nodiscard]] static auto celt_pvq_u_entry(int row, int column) noexcept -> opus_uint32 {
  if (row > column) {
    std::swap(row, column);
  }
  return celt_pvq_fast_rows[static_cast<unsigned>(row)][static_cast<unsigned>(column)];
}

[[nodiscard]] static inline auto celt_pvq_v_entry(int dimensions, int pulses) noexcept -> opus_uint32 {
  if (dimensions <= pulses) {
    const auto* row = celt_pvq_fast_rows[static_cast<unsigned>(dimensions)];
    return row[pulses] + row[pulses + 1];
  }
  return celt_pvq_fast_rows[static_cast<unsigned>(pulses)][dimensions] + celt_pvq_fast_rows[static_cast<unsigned>(pulses + 1)][dimensions];
}

template <typename Writer> static auto celt_pvq_unrank_impl(int n, int k, opus_uint32 index, Writer& writer) noexcept -> opus_val32 {
  opus_val32 yy = 0;
  int position = 0;
  for (; n > 2; --n, ++position) {
    opus_uint32 p, q;
    const int k0 = k;
    int sign;
    if (k >= n) {
      const auto* row = celt_pvq_fast_rows[static_cast<unsigned>(n)];
      p = row[k + 1];
      sign = -(index >= p);
      index -= p & sign;
      if (row[n] > index) {
        for (k = n; (p = celt_pvq_fast_rows[static_cast<unsigned>(--k)][n]) > index;) {}
      } else {
        for (p = row[k]; p > index; p = row[--k]) {}
      }
    } else {
      p = celt_pvq_fast_rows[static_cast<unsigned>(k)][n];
      q = celt_pvq_fast_rows[static_cast<unsigned>(k + 1)][n];
      if (p <= index && index < q) {
        index -= p;
        writer(position, 0);
        continue;
      }
      sign = -(index >= q);
      index -= q & sign;
      while ((p = celt_pvq_fast_rows[static_cast<unsigned>(--k)][n]) > index) {}
    }
    index -= p;
    const auto value = static_cast<opus_int16>((k0 - k + sign) ^ sign);
    writer(position, value);
    yy += static_cast<opus_val32>(value * value);
  }
  auto p = static_cast<opus_uint32>(2 * k + 1);
  const int sign0 = -(index >= p);
  index -= p & sign0;
  const int k0 = k;
  k = (index + 1) >> 1;
  if (k != 0) {
    index -= 2 * k - 1;
  }
  auto value = static_cast<opus_int16>((k0 - k + sign0) ^ sign0);
  writer(position, value);
  yy += static_cast<opus_val32>(value * value);
  const int sign1 = -static_cast<int>(index);
  value = static_cast<opus_int16>((k + sign1) ^ sign1);
  writer(position + 1, value);
  return yy + static_cast<opus_val32>(value * value);
}

template <typename Writer> static inline auto celt_pvq_decode_unrank(int n, int k, ec_dec* dec, Writer& writer) noexcept -> opus_val32 {
  const auto index = ec_dec_uint(dec, celt_pvq_v_entry(n, k));
  return celt_pvq_unrank_impl(n, k, index, writer);
}

static opus_uint32 ec_tell_frac(const ec_ctx* _this) {
  constexpr std::array<opus_uint16, 8> corrections{35733, 38967, 42495, 46340, 50535, 55109, 60097, 65535};
  const opus_uint32 nbits = _this->nbits_total << 3;
  int l = std::bit_width(_this->rng);
  const opus_uint32 r = _this->rng >> (l - 16);
  unsigned b = (r >> 12) - 8;
  b += r > corrections[b];
  l = (l << 3) + b;
  return nbits - l;
}

static int ec_read_byte(ec_dec* _this) {
  return _this->offs < _this->storage ? _this->buf[_this->offs++] : 0;
}

static void ec_dec_normalize(ec_dec* _this) {
  do {
    _this->nbits_total += (8);
    _this->rng <<= (8);
    int sym = _this->rem;
    _this->rem = ec_read_byte(_this);
    sym = (sym << (8) | _this->rem) >> ((8) - (((32) - 2) % (8) + 1));
    _this->val = ((_this->val << (8)) + (ec_byte_mask & ~sym)) & ec_code_mask;
  } while (_this->rng <= ec_code_bot);
}

static void ec_dec_normalize_if_needed(ec_dec* _this) {
  if (_this->rng <= ec_code_bot) {
    ec_dec_normalize(_this);
  }
}

void ec_dec_init(ec_dec* _this, unsigned char* _buf, opus_uint32 _storage) {
  _this->buf = _buf;
  _this->storage = _storage;
  _this->end_offs = 0;
  _this->end_window = 0;
  _this->nend_bits = 0;
  _this->nbits_total = (32) + 1 - (((32) - (((32) - 2) % (8) + 1)) / (8)) * (8);
  _this->offs = 0;
  _this->rng = 1U << (((32) - 2) % (8) + 1);
  _this->rem = ec_read_byte(_this);
  _this->val = _this->rng - 1 - (_this->rem >> ((8) - (((32) - 2) % (8) + 1)));
  _this->error = 0;
  ec_dec_normalize_if_needed(_this);
}

static unsigned ec_decode(ec_dec* _this, unsigned _ft) {
  _this->ext = celt_udiv(_this->rng, _ft);
  const unsigned s = static_cast<unsigned>(_this->val / _this->ext);
  return _ft - ((s + 1) + (((_ft) - (s + 1)) & -((_ft) < (s + 1))));
}

static void ec_dec_update(ec_dec* _this, unsigned _fl, unsigned _fh, unsigned _ft) {
  opus_uint32 s = ((_this->ext) * (_ft - _fh));
  _this->val -= s;
  _this->rng = _fl > 0 ? ((_this->ext) * (_fh - _fl)) : _this->rng - s;
  ec_dec_normalize_if_needed(_this);
}

static int ec_dec_bit_logp(ec_dec* _this, unsigned _logp) {
  const opus_uint32 r = _this->rng;
  const opus_uint32 d = _this->val;
  const opus_uint32 s = r >> _logp;
  const int ret = d < s;
  if (!ret) {
    _this->val = d - s;
  }
  _this->rng = ret ? s : r - s;
  ec_dec_normalize_if_needed(_this);
  return ret;
}

static int ec_dec_icdf(ec_dec* _this, const unsigned char* _icdf, unsigned _ftb) {
  opus_uint32 s, t;
  int ret;
  s = _this->rng;
  const opus_uint32 d = _this->val;
  const opus_uint32 r = s >> _ftb;
  for (ret = 0, t = s, s = ((r) * (_icdf[0])); d < s; t = s, s = ((r) * (_icdf[++ret]))) {}
  _this->val = d - s;
  _this->rng = t - s;
  ec_dec_normalize_if_needed(_this);
  return ret;
}

static opus_uint32 ec_dec_uint(ec_dec* _this, opus_uint32 _ft) {
  _ft--;
  int ftb = std::bit_width(_ft);
  if (ftb > (8)) {
    ftb -= (8);
    const unsigned ft = static_cast<unsigned>(_ft >> ftb) + 1;
    const unsigned s = ec_decode(_this, ft);
    ec_dec_update(_this, s, s + 1, ft);
    const opus_uint32 t = static_cast<opus_uint32>(s) << ftb | ec_dec_bits(_this, ftb);
    if (t <= _ft) {
      return t;
    }
    _this->error = 1;
    return _ft;
  }
  _ft++;
  const unsigned s = ec_decode(_this, static_cast<unsigned>(_ft));
  ec_dec_update(_this, s, s + 1, static_cast<unsigned>(_ft));
  return s;
}

static opus_uint32 ec_dec_bits(ec_dec* _this, unsigned _bits) {
  ec_window window = _this->end_window;
  int available = _this->nend_bits;
  if (static_cast<unsigned>(available) < _bits) {
    for (; available <= (static_cast<int>(sizeof(ec_window)) * 8) - (8); available += (8)) {
      const int byte = _this->end_offs < _this->storage ? _this->buf[_this->storage - ++(_this->end_offs)] : 0;
      window |= static_cast<ec_window>(byte) << available;
    }
  }
  const opus_uint32 ret = static_cast<opus_uint32>(window) & low_bits_mask(_bits);
  window >>= _bits;
  available -= _bits;
  _this->end_window = window;
  _this->nend_bits = available;
  _this->nbits_total += _bits;
  return ret;
}

static int ec_write_byte(ec_enc* _this, unsigned _value) {
  if (_this->offs + _this->end_offs >= _this->storage) {
    return -1;
  }
  _this->buf[_this->offs++] = static_cast<unsigned char>(_value);
  return 0;
}

static int ec_write_byte_at_end(ec_enc* _this, unsigned _value) {
  if (_this->offs + _this->end_offs >= _this->storage) {
    return -1;
  }
  _this->buf[_this->storage - ++(_this->end_offs)] = static_cast<unsigned char>(_value);
  return 0;
}

static void ec_enc_carry_out(ec_enc* _this, int _c) {
  if (_c != ec_byte_mask) {
    int carry = _c >> (8);
    if (_this->rem >= 0) {
      _this->error |= ec_write_byte(_this, _this->rem + carry);
    }
    if (_this->ext > 0) {
      const auto sym = (ec_byte_mask + carry) & ec_byte_mask;
      for (; _this->ext > 0; --(_this->ext)) {
        _this->error |= ec_write_byte(_this, sym);
      }
    }
    _this->rem = _c & ec_byte_mask;
  } else
    _this->ext++;
}

static void ec_enc_normalize(ec_enc* _this) {
  for (; _this->rng <= ec_code_bot;) {
    ec_enc_carry_out(_this, static_cast<int>(_this->val >> ((32) - (8) - 1)));
    _this->val = (_this->val << (8)) & ec_code_mask;
    _this->rng <<= (8);
    _this->nbits_total += (8);
  }
}

void ec_enc_init(ec_enc* _this, unsigned char* _buf, opus_uint32 _size) {
  _this->buf = _buf;
  _this->end_offs = 0;
  _this->end_window = 0;
  _this->nend_bits = 0;
  _this->nbits_total = (32) + 1;
  _this->offs = 0;
  _this->rng = ec_code_top;
  _this->rem = -1;
  _this->val = 0;
  _this->ext = 0;
  _this->storage = _size;
  _this->error = 0;
}

void ec_encode(ec_enc* _this, unsigned _fl, unsigned _fh, unsigned _ft) {
  opus_uint32 r = celt_udiv(_this->rng, _ft);
  if (_fl > 0) {
    _this->val += _this->rng - ((r) * ((_ft - _fl)));
    _this->rng = ((r) * ((_fh - _fl)));
  } else
    _this->rng -= ((r) * ((_ft - _fh)));
  ec_enc_normalize(_this);
}

void ec_enc_bit_logp(ec_enc* _this, int _val, unsigned _logp) {
  opus_uint32 r, s, l;
  r = _this->rng;
  l = _this->val;
  s = r >> _logp;
  r -= s;
  if (_val) {
    _this->val = l + r;
  }
  _this->rng = _val ? s : r;
  ec_enc_normalize(_this);
}

void ec_enc_icdf(ec_enc* _this, int _s, const unsigned char* _icdf, unsigned _ftb) {
  opus_uint32 r = _this->rng >> _ftb;
  if (_s > 0) {
    _this->val += _this->rng - ((r) * (_icdf[_s - 1]));
    _this->rng = ((r) * (_icdf[_s - 1] - _icdf[_s]));
  } else
    _this->rng -= ((r) * (_icdf[_s]));
  ec_enc_normalize(_this);
}

void ec_enc_uint(ec_enc* _this, opus_uint32 _fl, opus_uint32 _ft) {
  _ft--;
  int ftb = std::bit_width(_ft);
  if (ftb > (8)) {
    ftb -= (8);
    const unsigned ft = (_ft >> ftb) + 1;
    const unsigned fl = static_cast<unsigned>(_fl >> ftb);
    ec_encode(_this, fl, fl + 1, ft);
    ec_enc_bits(_this, _fl & low_bits_mask(ftb), ftb);
  } else
    ec_encode(_this, _fl, _fl + 1, _ft + 1);
}

void ec_enc_bits(ec_enc* _this, opus_uint32 _fl, unsigned _bits) {
  ec_window window = _this->end_window;
  int used = _this->nend_bits;
  if (used + _bits > (static_cast<int>(sizeof(ec_window)) * 8)) {
    for (; used >= (8); used -= (8)) {
      _this->error |= ec_write_byte_at_end(_this, static_cast<unsigned>(window) & ec_byte_mask);
      window >>= (8);
    }
  }
  window |= static_cast<ec_window>(_fl) << used;
  used += _bits;
  _this->end_window = window;
  _this->nend_bits = used;
  _this->nbits_total += _bits;
}

static void ec_enc_patch_initial_bits(ec_enc* _this, unsigned _val, unsigned _nbits) {
  const int shift = (8) - _nbits;
  const unsigned mask = ((1 << _nbits) - 1) << shift;
  if (_this->offs > 0) {
    _this->buf[0] = static_cast<unsigned char>((_this->buf[0] & ~mask) | _val << shift);
  } else if (_this->rem >= 0) {
    _this->rem = (_this->rem & ~mask) | _val << shift;
  } else if (_this->rng <= (ec_code_top >> _nbits)) {
    _this->val = (_this->val & ~(static_cast<opus_uint32>(mask) << ((32) - (8) - 1))) | static_cast<opus_uint32>(_val)
                                                                                            << (((32) - (8) - 1) + shift);
  } else
    _this->error = -1;
}

void ec_enc_shrink(ec_enc* _this, opus_uint32 _size) {
  move_n_items(_this->buf + _this->storage - _this->end_offs, static_cast<std::size_t>(_this->end_offs),
               _this->buf + _size - _this->end_offs);
  _this->storage = _size;
}

void ec_enc_done(ec_enc* _this) {
  ec_window window;
  int used;
  opus_uint32 msk, end;
  int l = (32) - std::bit_width(_this->rng);
  msk = ec_code_mask >> l;
  end = (_this->val + msk) & ~msk;
  if ((end | msk) >= _this->val + _this->rng) {
    l++;
    msk >>= 1;
    end = (_this->val + msk) & ~msk;
  }
  for (; l > 0; l -= (8)) {
    ec_enc_carry_out(_this, static_cast<int>(end >> ((32) - (8) - 1)));
    end = (end << (8)) & ec_code_mask;
  }
  if (_this->rem >= 0 || _this->ext > 0) {
    ec_enc_carry_out(_this, 0);
  }
  window = _this->end_window;
  used = _this->nend_bits;
  for (; used >= (8); used -= (8)) {
    _this->error |= ec_write_byte_at_end(_this, static_cast<unsigned>(window) & ec_byte_mask);
    window >>= (8);
  }
  if (!_this->error) {
    if (_this->buf) {
      zero_n_items(_this->buf + _this->offs, static_cast<std::size_t>(_this->storage - _this->offs - _this->end_offs));
    }
    if (used > 0) {
      if (_this->end_offs >= _this->storage) {
        _this->error = -1;
      } else {
        l = -l;
        if (_this->offs + _this->end_offs >= _this->storage && l < used) {
          window &= (1 << l) - 1;
          _this->error = -1;
        }
        _this->buf[_this->storage - _this->end_offs - 1] |= static_cast<unsigned char>(window);
      }
    }
  }
}

static void kf_bfly2_step(kiss_fft_cpx* Fout) {
  constexpr celt_coef tw = 0.7071067812f;
  auto* Fout2 = Fout + 4;
  auto t = Fout2[0];
  Fout2[0] = complex_subtract(Fout[0], t);
  Fout[0] = complex_add(Fout[0], t);
  t.r = (Fout2[1].r + Fout2[1].i) * tw;
  t.i = (Fout2[1].i - Fout2[1].r) * tw;
  Fout2[1] = complex_subtract(Fout[1], t);
  Fout[1] = complex_add(Fout[1], t);
  t.r = Fout2[2].i;
  t.i = -Fout2[2].r;
  Fout2[2] = complex_subtract(Fout[2], t);
  Fout[2] = complex_add(Fout[2], t);
  t.r = (Fout2[3].i - Fout2[3].r) * tw;
  t.i = -(Fout2[3].i + Fout2[3].r) * tw;
  Fout2[3] = complex_subtract(Fout[3], t);
  Fout[3] = complex_add(Fout[3], t);
}

static void kf_bfly2(kiss_fft_cpx* Fout, int N) {
  for (int i = 0; i < N; ++i) {
    kf_bfly2_step(Fout);
    Fout += 8;
  }
}

static void kf_bfly4_m1_step(kiss_fft_cpx* Fout) {
  const auto scratch0 = complex_subtract(Fout[0], Fout[2]);
  Fout[0] = complex_add(Fout[0], Fout[2]);
  auto scratch1 = complex_add(Fout[1], Fout[3]);
  Fout[2] = complex_subtract(Fout[0], scratch1);
  Fout[0] = complex_add(Fout[0], scratch1);
  scratch1 = complex_subtract(Fout[1], Fout[3]);
  Fout[1].r = scratch0.r + scratch1.i;
  Fout[1].i = scratch0.i - scratch1.r;
  Fout[3].r = scratch0.r - scratch1.i;
  Fout[3].i = scratch0.i + scratch1.r;
}

static void kf_bfly4_m1(kiss_fft_cpx* output, int count) {
  for (int i = 0; i < count; ++i, output += 4) {
    kf_bfly4_m1_step(output);
  }
}

static void kf_bfly4(kiss_fft_cpx* Fout, const size_t fstride, const kiss_fft_state* st, int m) {
  const int m2 = 2 * m, m3 = 3 * m;
  for (int j = 0; j < m; ++j) {
    auto* output = Fout + j;
    for (int i = 0; i < 15; ++i, output += 4 * m) {
      const auto scratch0 = complex_multiply(output[m], st->twiddles[j * fstride]);
      const auto scratch1 = complex_multiply(output[m2], st->twiddles[2 * j * fstride]);
      const auto scratch2 = complex_multiply(output[m3], st->twiddles[3 * j * fstride]);
      const auto scratch5 = complex_subtract(*output, scratch1);
      output->r += scratch1.r;
      output->i += scratch1.i;
      const auto scratch3 = complex_add(scratch0, scratch2);
      const auto scratch4 = complex_subtract(scratch0, scratch2);
      output[m2].r = output->r - scratch3.r;
      output[m2].i = output->i - scratch3.i;
      output->r += scratch3.r;
      output->i += scratch3.i;
      output[m].r = scratch5.r + scratch4.i;
      output[m].i = scratch5.i - scratch4.r;
      output[m3].r = scratch5.r - scratch4.i;
      output[m3].i = scratch5.i + scratch4.r;
    }
  }
}

static void kf_bfly3(kiss_fft_cpx* Fout, const size_t fstride, const kiss_fft_state* st, int m) {
  const size_t m2 = 2 * m;
  const kiss_twiddle_cpx epi3 = st->twiddles[fstride * m];
  for (size_t index = 0; index < static_cast<size_t>(m); ++index) {
    auto* output = Fout + index;
    for (int i = 0; i < 5; ++i, output += 3 * m) {
      const auto scratch1 = complex_multiply(output[m], st->twiddles[index * fstride]);
      const auto scratch2 = complex_multiply(output[m2], st->twiddles[2 * index * fstride]);
      const auto scratch3 = complex_add(scratch1, scratch2);
      auto scratch0 = complex_subtract(scratch1, scratch2);
      output[m].r = output->r - scratch3.r * .5f;
      output[m].i = output->i - scratch3.i * .5f;
      scratch0.r *= epi3.i;
      scratch0.i *= epi3.i;
      output->r += scratch3.r;
      output->i += scratch3.i;
      output[m2].r = output[m].r + scratch0.i;
      output[m2].i = output[m].i - scratch0.r;
      output[m].r -= scratch0.i;
      output[m].i += scratch0.r;
    }
  }
}

static void kf_bfly5(kiss_fft_cpx* Fout, const size_t fstride, const kiss_fft_state* st, int m) {
  std::array<kiss_fft_cpx, 13> scratch;
  const kiss_twiddle_cpx ya = st->twiddles[fstride * m];
  const kiss_twiddle_cpx yb = st->twiddles[fstride * 2 * m];
  const kiss_twiddle_cpx* tw = st->twiddles;
  for (int u = 0; u < m; ++u) {
    scratch[0] = Fout[u];
    scratch[1] = complex_multiply(Fout[m + u], tw[u * fstride]);
    scratch[2] = complex_multiply(Fout[2 * m + u], tw[2 * u * fstride]);
    scratch[3] = complex_multiply(Fout[3 * m + u], tw[3 * u * fstride]);
    scratch[4] = complex_multiply(Fout[4 * m + u], tw[4 * u * fstride]);
    scratch[7] = complex_add(scratch[1], scratch[4]);
    scratch[10] = complex_subtract(scratch[1], scratch[4]);
    scratch[8] = complex_add(scratch[2], scratch[3]);
    scratch[9] = complex_subtract(scratch[2], scratch[3]);
    Fout[u] = complex_add(scratch[0], complex_add(scratch[7], scratch[8]));
    scratch[5] = {scratch[0].r + (scratch[7].r * ya.r + scratch[8].r * yb.r), scratch[0].i + (scratch[7].i * ya.r + scratch[8].i * yb.r)};
    scratch[6] = {scratch[10].i * ya.i + scratch[9].i * yb.i, -(scratch[10].r * ya.i + scratch[9].r * yb.i)};
    Fout[m + u] = complex_subtract(scratch[5], scratch[6]);
    Fout[4 * m + u] = complex_add(scratch[5], scratch[6]);
    scratch[11] = {scratch[0].r + (scratch[7].r * yb.r + scratch[8].r * ya.r), scratch[0].i + (scratch[7].i * yb.r + scratch[8].i * ya.r)};
    scratch[12] = {scratch[9].i * ya.i - scratch[10].i * yb.i, scratch[10].r * yb.i - scratch[9].r * ya.i};
    Fout[2 * m + u] = complex_add(scratch[11], scratch[12]);
    Fout[3 * m + u] = complex_subtract(scratch[11], scratch[12]);
  }
}

namespace {

constexpr std::array<opus_uint8, 15> pfa_fft15_input_permutation{2, 10, 5, 12, 7, 0, 14, 9, 4, 11, 1, 6, 13, 8, 3};
constexpr std::array<opus_uint8, 32> pfa_split_radix_permutation{0, 16, 8, 24, 4, 20, 28, 12, 2,  18, 10, 26, 30, 14, 6,  22,
                                                                 1, 17, 9, 25, 5, 21, 29, 13, 31, 15, 7,  23, 3,  19, 27, 11};
constexpr std::array<float, 9> pfa_fft32_cosines{1.0f,         0.980785251f, 0.923879504f, 0.831469595f, 0.707106769f,
                                                 0.555570245f, 0.382683426f, 0.195090324f, 0.0f};

consteval auto make_pfa_input_map_480() noexcept -> std::array<opus_uint16, 480> {
  std::array<opus_uint16, 480> map{};
  int block = 0;
  int position = 0;
  for (auto& destination : map) {
    destination = static_cast<opus_uint16>(15 * block + pfa_fft15_input_permutation[position]);
    block = (block + 15) & 31;
    if (++position == 15) {
      position = 0;
    }
  }
  return map;
}

constexpr auto pfa_input_map_480 = make_pfa_input_map_480();

void pfa_butterfly(float& difference, float& sum, float left, float right) noexcept {
  difference = left - right;
  sum = left + right;
}

void pfa_split_radix_butterflies(kiss_fft_cpx& a0, kiss_fft_cpx& a1, kiss_fft_cpx& a2, kiss_fft_cpx& a3, float t1, float t2, float t5,
                                 float t6) noexcept {
  const float r0 = a0.r;
  const float i0 = a0.i;
  const float r1 = a1.r;
  const float i1 = a1.i;
  float t3, t4;
  pfa_butterfly(t3, t5, t5, t1);
  pfa_butterfly(a2.r, a0.r, r0, t5);
  pfa_butterfly(a3.i, a1.i, i1, t3);
  pfa_butterfly(t4, t6, t2, t6);
  pfa_butterfly(a3.r, a1.r, r1, t4);
  pfa_butterfly(a2.i, a0.i, i0, t6);
}

void pfa_split_radix_transform(kiss_fft_cpx& a0, kiss_fft_cpx& a1, kiss_fft_cpx& a2, kiss_fft_cpx& a3, float real,
                               float imaginary) noexcept {
  const float t1 = a2.r * real + a2.i * imaginary;
  const float t2 = a2.i * real - a2.r * imaginary;
  const float t5 = a3.r * real - a3.i * imaginary;
  const float t6 = a3.r * imaginary + a3.i * real;
  pfa_split_radix_butterflies(a0, a1, a2, a3, t1, t2, t5, t6);
}

void pfa_fft4(kiss_fft_cpx* values) noexcept {
  float t1, t2, t3, t4, t5, t6, t7, t8;
  pfa_butterfly(t3, t1, values[0].r, values[1].r);
  pfa_butterfly(t8, t6, values[3].r, values[2].r);
  pfa_butterfly(values[2].r, values[0].r, t1, t6);
  pfa_butterfly(t4, t2, values[0].i, values[1].i);
  pfa_butterfly(t7, t5, values[2].i, values[3].i);
  pfa_butterfly(values[3].i, values[1].i, t4, t8);
  pfa_butterfly(values[3].r, values[1].r, t3, t7);
  pfa_butterfly(values[2].i, values[0].i, t2, t5);
}

void pfa_fft8(kiss_fft_cpx* values) noexcept {
  pfa_fft4(values);
  const float t1 = values[4].r + values[5].r;
  values[5].r = values[4].r - values[5].r;
  const float t2 = values[4].i + values[5].i;
  values[5].i = values[4].i - values[5].i;
  const float t5 = values[6].r + values[7].r;
  values[7].r = values[6].r - values[7].r;
  const float t6 = values[6].i + values[7].i;
  values[7].i = values[6].i - values[7].i;
  pfa_split_radix_butterflies(values[0], values[2], values[4], values[6], t1, t2, t5, t6);
  pfa_split_radix_transform(values[1], values[3], values[5], values[7], pfa_fft32_cosines[4], pfa_fft32_cosines[4]);
}

void pfa_fft16(kiss_fft_cpx* values) noexcept {
  pfa_fft8(values);
  pfa_fft4(values + 8);
  pfa_fft4(values + 12);
  pfa_split_radix_butterflies(values[0], values[4], values[8], values[12], values[8].r, values[8].i, values[12].r, values[12].i);
  pfa_split_radix_transform(values[2], values[6], values[10], values[14], pfa_fft32_cosines[4], pfa_fft32_cosines[4]);
  pfa_split_radix_transform(values[1], values[5], values[9], values[13], pfa_fft32_cosines[2], pfa_fft32_cosines[6]);
  pfa_split_radix_transform(values[3], values[7], values[11], values[15], pfa_fft32_cosines[6], pfa_fft32_cosines[2]);
}

void pfa_fft32(kiss_fft_cpx* values) noexcept {
  pfa_fft16(values);
  pfa_fft8(values + 16);
  pfa_fft8(values + 24);
  for (int index = 0; index < 4; ++index) {
    const int offset = 2 * index;
    pfa_split_radix_transform(values[offset], values[8 + offset], values[16 + offset], values[24 + offset], pfa_fft32_cosines[offset],
                              pfa_fft32_cosines[8 - offset]);
    pfa_split_radix_transform(values[offset + 1], values[9 + offset], values[17 + offset], values[25 + offset],
                              pfa_fft32_cosines[offset + 1], pfa_fft32_cosines[7 - offset]);
  }
}

void pfa_fft3(const kiss_fft_cpx& in0, const kiss_fft_cpx& in1, const kiss_fft_cpx& in2, kiss_fft_cpx& out0, kiss_fft_cpx& out1,
              kiss_fft_cpx& out2) noexcept {
  constexpr float sine = 0.866025388f;
  const float sum_r = in1.r + in2.r;
  const float difference_r = in1.r - in2.r;
  const float sum_i = in1.i + in2.i;
  const float difference_i = in1.i - in2.i;
  out0 = {in0.r + sum_r, in0.i + sum_i};
  const float cross_r = difference_i * sine;
  const float cross_i = difference_r * sine;
  const float base_r = in0.r - 0.5f * sum_r;
  const float base_i = in0.i - 0.5f * sum_i;
  out1 = {base_r + cross_r, base_i - cross_i};
  out2 = {base_r - cross_r, base_i + cross_i};
}

constexpr std::array<opus_uint8, 15> pfa_fft15_output_indices{0, 3, 6, 9, 12, 5, 8, 11, 14, 2, 10, 13, 1, 4, 7};

void pfa_fft5(const kiss_fft_cpx* input, kiss_fft_cpx* output, const opus_uint8* indices) noexcept {
  constexpr float cos_2pi_5 = 0.309017003f;
  constexpr float cos_4pi_5 = 0.809016994f;
  constexpr float sin_2pi_5 = 0.951056540f;
  constexpr float sin_4pi_5 = 0.587785252f;
  const float difference14_r = input[1].r - input[4].r;
  const float sum14_r = input[1].r + input[4].r;
  const float difference14_i = input[1].i - input[4].i;
  const float sum14_i = input[1].i + input[4].i;
  const float difference23_r = input[2].r - input[3].r;
  const float sum23_r = input[2].r + input[3].r;
  const float difference23_i = input[2].i - input[3].i;
  const float sum23_i = input[2].i + input[3].i;
  const float t4_r = sum14_r * cos_2pi_5 - sum23_r * cos_4pi_5;
  const float t0_r = sum23_r * cos_2pi_5 - sum14_r * cos_4pi_5;
  const float t4_i = sum14_i * cos_2pi_5 - sum23_i * cos_4pi_5;
  const float t0_i = sum23_i * cos_2pi_5 - sum14_i * cos_4pi_5;
  const float t5_r = difference14_i * sin_2pi_5 + difference23_i * sin_4pi_5;
  const float t1_r = difference14_i * sin_4pi_5 - difference23_i * sin_2pi_5;
  const float t5_i = -(difference14_r * sin_2pi_5 + difference23_r * sin_4pi_5);
  const float t1_i = difference23_r * sin_2pi_5 - difference14_r * sin_4pi_5;
  output[static_cast<int>(indices[0]) * 32] = {input[0].r + sum14_r + sum23_r, input[0].i + sum14_i + sum23_i};
  output[static_cast<int>(indices[1]) * 32] = {input[0].r + t4_r + t5_r, input[0].i + t4_i + t5_i};
  output[static_cast<int>(indices[2]) * 32] = {input[0].r + t0_r + t1_r, input[0].i + t0_i + t1_i};
  output[static_cast<int>(indices[3]) * 32] = {input[0].r + t0_r - t1_r, input[0].i + t0_i - t1_i};
  output[static_cast<int>(indices[4]) * 32] = {input[0].r + t4_r - t5_r, input[0].i + t4_i - t5_i};
}

void pfa_fft15(const kiss_fft_cpx* input, kiss_fft_cpx* output) noexcept {
  std::array<kiss_fft_cpx, 15> temporary;
  pfa_fft3(input[2], input[0], input[1], temporary[0], temporary[5], temporary[10]);
  pfa_fft3(input[13], input[5], input[9], temporary[1], temporary[6], temporary[11]);
  pfa_fft3(input[11], input[3], input[7], temporary[2], temporary[7], temporary[12]);
  pfa_fft3(input[14], input[6], input[10], temporary[3], temporary[8], temporary[13]);
  pfa_fft3(input[12], input[4], input[8], temporary[4], temporary[9], temporary[14]);
  pfa_fft5(temporary.data(), output, pfa_fft15_output_indices.data());
  pfa_fft5(temporary.data() + 5, output, pfa_fft15_output_indices.data() + 5);
  pfa_fft5(temporary.data() + 10, output, pfa_fft15_output_indices.data() + 10);
}

} // namespace

static void fft_impl_480(kiss_fft_cpx* fout, const kiss_fft_state* st) {
  kf_bfly4_m1(fout, 120);
  kf_bfly2(fout, 60);
  kf_bfly4(fout, 15, st, 8);
  kf_bfly3(fout, 5, st, 32);
  kf_bfly5(fout, 1, st, 96);
}

static void fft_impl(const kiss_fft_state* st, kiss_fft_cpx* fout) {
  switch (st->nfft) {
  case 480:
    fft_impl_480(fout, st);
    break;
  case 240:
    kf_bfly4_m1(fout, 60);
    kf_bfly4(fout, 30, st, 4);
    kf_bfly3(fout, 10, st, 16);
    kf_bfly5(fout, 2, st, 48);
    break;
  case 120:
    kf_bfly4_m1(fout, 30);
    kf_bfly2(fout, 15);
    kf_bfly3(fout, 20, st, 8);
    kf_bfly5(fout, 4, st, 24);
    break;
  default:
    kf_bfly4_m1(fout, 15);
    kf_bfly3(fout, 40, st, 4);
    kf_bfly5(fout, 8, st, 12);
    break;
  }
}

static unsigned ec_laplace_get_freq1(unsigned fs0, int decay) {
  const auto ft = 32768 - 2 * 16 - fs0;
  return ft * static_cast<opus_int32>(16384 - decay) >> 15;
}

static void ec_laplace_encode(ec_enc* enc, int* value, unsigned fs, int decay) {
  int val = *value;
  unsigned fl = 0;
  if (val) {
    const int s = -(val < 0);
    val = (val + s) ^ s;
    fl = fs;
    fs = ec_laplace_get_freq1(fs, decay);
    int i = 1;
    for (; fs > 0 && i < val; i++) {
      fs *= 2;
      fl += fs + 2 * (1 << (0));
      fs = (fs * static_cast<opus_int32>(decay)) >> 15;
    }
    if (!fs) {
      int ndi_max = (32768 - fl + (1 << (0)) - 1) >> (0);
      ndi_max = (ndi_max - s) >> 1;
      const int di = std::min(val - i, ndi_max - 1);
      fl += (2 * di + 1 + s) * (1 << (0));
      fs = (((1 << (0))) < (32768 - fl) ? ((1 << (0))) : (32768 - fl));
      *value = (i + di + s) ^ s;
    } else {
      fs += (1 << (0));
      fl += fs & ~s;
    }
  }
  const opus_uint32 range = enc->rng >> 15;
  if (fl > 0) {
    enc->val += enc->rng - range * ((1U << 15) - fl);
    enc->rng = range * fs;
  } else {
    enc->rng -= range * ((1U << 15) - fs);
  }
  ec_enc_normalize(enc);
}

static int ec_laplace_decode(ec_dec* dec, unsigned fs, int decay) {
  int val = 0;
  dec->ext = dec->rng >> 15;
  const unsigned scaled = static_cast<unsigned>(dec->val / dec->ext);
  const unsigned fm = (1U << 15) - ((scaled + 1U) + (((1U << 15) - (scaled + 1U)) & -((1U << 15) < (scaled + 1U))));
  unsigned fl = 0;
  if (fm >= fs) {
    val++;
    fl = fs;
    fs = ec_laplace_get_freq1(fs, decay) + (1 << (0));
    for (; fs > (1 << (0)) && fm >= fl + 2 * fs;) {
      fs *= 2;
      fl += fs;
      fs = ((fs - 2 * (1 << (0))) * static_cast<opus_int32>(decay)) >> 15;
      fs += (1 << (0));
      val++;
    }
    if (fs <= (1 << (0))) {
      const int di = (fm - fl) >> ((0) + 1);
      val += di;
      fl += 2 * di * (1 << (0));
    }
    if (fm < fl + fs) {
      val = -val;
    } else
      fl += fs;
  }
  ec_dec_update(dec, fl, std::min(fl + fs, static_cast<unsigned>(32768)), 32768);
  return val;
}

static void celt_float2int16_c(const float* in, opus_int16* out, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = FLOAT2INT16(in[i]);
  }
}

template <bool Fixed20ms>
static void clt_mdct_forward_transform(const mdct_lookup* l, float* in, float* out, const celt_coef* window, int overlap, int shift,
                                       int stride) {
  const auto* st = l->kfft[Fixed20ms ? 0 : shift];
  const auto scale = st->scale;
  const int N = Fixed20ms ? 1920 : l->n >> shift;
  const float* trig = Fixed20ms ? l->trig : l->trig + l->n - N;
  const int N2 = N >> 1;
  const int N4 = N >> 2;
  std::array<float, celt_max_frame_samples> folded_storage;
  std::array<kiss_fft_cpx, celt_max_frame_samples / 2> fft_storage;
  auto* f = folded_storage.data();
  auto* f2 = Fixed20ms ? reinterpret_cast<kiss_fft_cpx*>(out) : fft_storage.data();
  const int overlap_quarters = (overlap + 3) >> 2;
  int i = 0;
  for (; i < overlap_quarters; ++i) {
    const int left = (overlap >> 1) + 2 * i, right = N2 - 1 + (overlap >> 1) - 2 * i;
    f[2 * i] = in[left + N2] * window[(overlap >> 1) - 1 - 2 * i] + in[right] * window[left];
    f[2 * i + 1] = in[left] * window[left] - in[right - N2] * window[(overlap >> 1) - 1 - 2 * i];
  }
  for (; i < N4 - overlap_quarters; ++i) {
    f[2 * i] = in[N2 - 1 + (overlap >> 1) - 2 * i];
    f[2 * i + 1] = in[(overlap >> 1) + 2 * i];
  }
  for (; i < N4; ++i) {
    const int left = (overlap >> 1) + 2 * i, right = N2 - 1 + (overlap >> 1) - 2 * i;
    const int window_left = 2 * (i - (N4 - overlap_quarters)), window_right = overlap - 1 - window_left;
    f[2 * i] = -in[left - N2] * window[window_left] + in[right] * window[window_right];
    f[2 * i + 1] = in[left] * window[window_right] + in[right + N2] * window[window_left];
  }
  for (int i = 0; i < N4; ++i) {
    const auto re = f[2 * i], im = f[2 * i + 1], t0 = trig[i], t1 = trig[N4 + i];
    f2[st->bitrev[i]] = {(re * t0 - im * t1) * scale, (im * t0 + re * t1) * scale};
  }
  fft_impl(st, f2);
  if constexpr (Fixed20ms) {
    for (int i = 0; i < N4 / 2; ++i) {
      const int mirror = N4 - 1 - i;
      const auto low = f2[i];
      const auto high = f2[mirror];
      const auto low_t0 = trig[i], low_t1 = trig[N4 + i];
      const auto high_t0 = trig[mirror], high_t1 = trig[N4 + mirror];
      out[2 * i] = low.i * low_t1 - low.r * low_t0;
      out[N2 - 1 - 2 * i] = low.r * low_t1 + low.i * low_t0;
      out[2 * mirror] = high.i * high_t1 - high.r * high_t0;
      out[N2 - 1 - 2 * mirror] = high.r * high_t1 + high.i * high_t0;
    }
  } else {
    for (int i = 0; i < N4; ++i) {
      const auto t0 = trig[i], t1 = trig[N4 + i];
      out[2 * stride * i] = f2[i].i * t1 - f2[i].r * t0;
      out[stride * (N2 - 1 - 2 * i)] = f2[i].r * t1 + f2[i].i * t0;
    }
  }
}

static void clt_mdct_forward_c(const mdct_lookup* l, float* in, float* out, const celt_coef* window, int overlap, int shift, int stride) {
  if (shift == 0 && stride == 1 && l->n == 1920) {
    clt_mdct_forward_transform<true>(l, in, out, window, overlap, 0, 1);
  } else {
    clt_mdct_forward_transform<false>(l, in, out, window, overlap, shift, stride);
  }
}

static void clt_mdct_backward_transform_20ms(const mdct_lookup* lookup, float* input, float* output, int overlap) noexcept {
  constexpr int n2 = 960;
  constexpr int n4 = 480;
  const float* trig = lookup->trig;
  auto* work = reinterpret_cast<kiss_fft_cpx*>(output + (overlap >> 1));
  const float* front = input;
  const float* back = input + n2 - 1;
  for (int index = 0; index < n4; ++index) {
    const float x1 = *front;
    const float x2 = *back;
    work[pfa_input_map_480[index]] = {x1 * trig[index] - x2 * trig[n4 + index], x2 * trig[index] + x1 * trig[n4 + index]};
    front += 2;
    back -= 2;
  }

  auto* transformed = reinterpret_cast<kiss_fft_cpx*>(input);
  for (int index = 0; index < 32; ++index) {
    pfa_fft15(work + 15 * pfa_split_radix_permutation[index], transformed + index);
  }
  for (int row = 0; row < 15; ++row) {
    pfa_fft32(transformed + 32 * row);
  }

  float* output_front = output + (overlap >> 1);
  float* output_back = output_front + n2 - 2;
  int position = 0;
  for (int index = 0; index < (n4 + 1) >> 1; ++index) {
    const int column = index & 31;
    const auto first = transformed[32 * position + column];
    const auto last = transformed[32 * (14 - position) + ((31 - index) & 31)];
    float t0 = trig[index];
    float t1 = trig[n4 + index];
    output_front[0] = first.i * t0 + first.r * t1;
    output_back[1] = first.i * t1 - first.r * t0;
    t0 = trig[n4 - index - 1];
    t1 = trig[n2 - index - 1];
    output_back[0] = last.i * t0 + last.r * t1;
    output_front[1] = last.i * t1 - last.r * t0;
    output_front += 2;
    output_back -= 2;
    if (++position == 15) {
      position = 0;
    }
  }
}

template <bool Fixed20ms>
static void clt_mdct_backward_transform(const mdct_lookup* lookup, float* input, float* output, int overlap, int shift = 0,
                                        int stride = 1) {
  if constexpr (Fixed20ms) {
    clt_mdct_backward_transform_20ms(lookup, input, output, overlap);
    return;
  }
  const int N = lookup->n >> shift;
  const float* trig = lookup->trig + lookup->n - N;
  const int N2 = N >> 1;
  const int N4 = N >> 2;
  const auto* fft_state = lookup->kfft[shift];
  const float* xp1 = input;
  const float* xp2 = input + stride * (N2 - 1);
  float* yp = output + (overlap >> 1);
  for (int i = 0; i < N4; ++i) {
    const int rev = fft_state->bitrev[i];
    const opus_val32 x1 = *xp1;
    const opus_val32 x2 = *xp2;
    const float yr = x2 * trig[i] + x1 * trig[N4 + i];
    const float yi = x1 * trig[i] - x2 * trig[N4 + i];
    yp[2 * rev + 1] = yr;
    yp[2 * rev] = yi;
    xp1 += 2 * stride;
    xp2 -= 2 * stride;
  }
  fft_impl(fft_state, reinterpret_cast<kiss_fft_cpx*>(output + (overlap >> 1)));
  float* yp0 = output + (overlap >> 1);
  float* yp1 = output + (overlap >> 1) + N2 - 2;
  for (int i = 0; i < (N4 + 1) >> 1; ++i) {
    float re = yp0[1];
    float im = yp0[0];
    float t0 = trig[i];
    float t1 = trig[N4 + i];
    float yr = re * t0 + im * t1;
    float yi = re * t1 - im * t0;
    re = yp1[1];
    im = yp1[0];
    yp0[0] = yr;
    yp1[1] = yi;
    t0 = trig[N4 - i - 1];
    t1 = trig[N2 - i - 1];
    yr = re * t0 + im * t1;
    yi = re * t1 - im * t0;
    yp1[0] = yr;
    yp0[1] = yi;
    yp0 += 2;
    yp1 -= 2;
  }
}

static void clt_mdct_backward_transform_c(const mdct_lookup* l, float* in, float* out, int overlap, int shift, int stride) {
  if (shift == 0 && stride == 1 && l->n == 1920) {
    clt_mdct_backward_transform<true>(l, in, out, overlap);
  } else {
    clt_mdct_backward_transform<false>(l, in, out, overlap, shift, stride);
  }
}

static void clt_mdct_backward_overlap_c(float* out, const celt_coef* window, int overlap) {
  for (int i = 0; i < overlap / 2; ++i) {
    const int mirror = overlap - 1 - i;
    const float low = out[i], high = out[mirror], a = window[i], b = window[mirror];
    out[i] = low * b - high * a;
    out[mirror] = low * a + high * b;
  }
}

static void clt_mdct_backward_stereo_20ms_c(const mdct_lookup* l, float* in0, float* in1, float* out0, float* out1, const celt_coef* window,
                                            int overlap) {
  clt_mdct_backward_transform<true>(l, in0, out0, overlap);
  clt_mdct_backward_transform<true>(l, in1, out1, overlap);
  clt_mdct_backward_overlap_c(out0, window, overlap);
  clt_mdct_backward_overlap_c(out1, window, overlap);
}

static void clt_mdct_backward_c(const mdct_lookup* l, float* in, float* out, const celt_coef* window, int overlap, int shift, int stride) {
  clt_mdct_backward_transform_c(l, in, out, overlap, shift, stride);
  clt_mdct_backward_overlap_c(out, window, overlap);
}

static void clt_mdct_backward_dual_history_c(const mdct_lookup* l, float* in, float* out0, float* out1, const celt_coef* window,
                                             int overlap, int shift, int stride) {
  const int N = l->n >> shift;
  clt_mdct_backward_transform_c(l, in, out0, overlap, shift, stride);
  copy_n_items(out0 + (overlap >> 1), static_cast<std::size_t>(N >> 1), out1 + (overlap >> 1));
  clt_mdct_backward_overlap_c(out0, window, overlap);
  clt_mdct_backward_overlap_c(out1, window, overlap);
}

constexpr std::array<opus_int16, 22> eband5ms =
    numeric_blob_array<opus_int16>(R"blob(000000010002000300040005000600070008000A000C000E001000140018001C002200280030003C004E0064)blob");
constexpr std::array<unsigned char, 231> band_allocation = numeric_blob_array<unsigned char>(
    R"blob(0000000000000000000000000000000000000000005A504B453F383128221D14120A00000000000000006E645A544E47413A332D27201A140C000000000000766E675D56504B46413B352F281F170F04000000007E7770685F59534E48423C362F272019110C010000867F787267615B554E48423C362F29231D17100A019089827C716B655F58524C464039332D27211A0F0198918A847B756F69625C56504A433D37312B241401A29B948E857F79736C66605A544D47413B352E1E01ACA59E988F89837D76706A645E57514B453F382D14C8C8C8C8C8C8C8C8C6C1BCB7B2ADA8A39E99948168)blob");
constexpr std::array<opus_int16, 21> logN400 =
    numeric_blob_array<opus_int16>(R"blob(000000000000000000000000000000000008000800080008001000100010001500150018001D00220024)blob");
constexpr std::array<opus_int16, 105> cache_index50 = numeric_blob_array<opus_int16>(
    R"blob(FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF000000000000000000290029002900520052007B00A400C800DE000000000000000000000000000000000029002900290029007B007B007B00A400A400F0010A011B012700290029002900290029002900290029007B007B007B007B00F000F000F0010A010A0131013E01480150007B007B007B007B007B007B007B007B00F000F000F000F0013101310131013E013E0157015F0166016C00F000F000F000F000F000F000F000F00131013101310131015701570157015F015F01720178017E0183)blob");
constexpr std::array<unsigned char, 392> cache_bits50 = numeric_blob_array<unsigned char>(
    R"blob(2807070707070707070707070707070707070707070707070707070707070707070707070707070707280F171C1F22242627292A2B2C2D2E2F2F3132333435363737393A3B3C3D3E3F3F4142434445464747281421293035393D40424547494B4C4E50525557595B5C5E60626567696B6C6E70727577797B7C7E80281727333C43494F53575B5E616466696B6F7376797C7E8183878B8E919496999B9FA3A6A9ACAEB1B3231C31414E59636B72787E84888D9195999FA5ABB0B4B9BDC0C7CDD3D8DCE1E5E8EFF5FB15213A4F61707D89949DA6AEB6BDC3C9CFD9E3EBF3FB11233F566A7B8B98A5B1BBC5CED6DEE6EDFA191F374B5B6975808A929AA1A8AEB4B9BEC8D0D7DEE5EBF0F5FF102441596E80909FADB9C4CFD9E2EAF2FA0B294A678097ACBFD1E1F1FF092B4F6E8AA3BACFE3F60C2747637B90A4B6C6D6E4F1FD092C51718EA8C0D6EBFF07315A7FA0BFDCF706335F86AACBEA072F577B9BB8D4ED06346189AED0F005396A97C0E7053B6F9ECAF305376793BBE0053C71A1CEF804417AAFE004437FB6EA)blob");
constexpr std::array<unsigned char, 168> cache_caps50 = numeric_blob_array<unsigned char>(
    R"blob(E0E0E0E0E0E0E0E0A0A0A0A0B9B9B9B2B2A8863D25E0E0E0E0E0E0E0E0F0F0F0F0CFCFCFC6C6B7904228A0A0A0A0A0A0A0A0B9B9B9B9C1C1C1B7B7AC8A4026F0F0F0F0F0F0F0F0CFCFCFCFCCCCCCC1C1B48F4228B9B9B9B9B9B9B9B9C1C1C1C1C1C1C1B7B7AC8A4127CFCFCFCFCFCFCFCFCCCCCCCCC9C9C9BCBCB08D4228C1C1C1C1C1C1C1C1C1C1C1C1C2C2C2B8B8AD8B4127CCCCCCCCCCCCCCCCC9C9C9C9C6C6C6BBBBAF8C4228)blob");

namespace {
[[nodiscard]] consteval auto make_celt_bits2pulses_lut() noexcept -> celt_bits2pulses_lut_table {
  celt_bits2pulses_lut_table table{};
  std::size_t entry = 0;
  int row = 0;
  for (int lm = 1; lm <= celt_bits2pulses_lut_lm_count; ++lm) {
    for (int band = 0; band < celt_default_nb_ebands; ++band, ++row) {
      const auto cache_offset = cache_index50[static_cast<std::size_t>(lm * celt_default_nb_ebands + band)];
      const auto max_pulse = cache_bits50[static_cast<std::size_t>(cache_offset)];
      const auto count = cache_bits50[static_cast<std::size_t>(cache_offset + max_pulse)] + 2;
      int upper = 1;
      for (int bits = 0; bits < count; ++bits) {
        const int target = bits - 1;
        while (upper < max_pulse && cache_bits50[static_cast<std::size_t>(cache_offset + upper)] < target) {
          ++upper;
        }
        const int lower = upper - 1;
        const int lower_value = lower == 0 ? -1 : cache_bits50[static_cast<std::size_t>(cache_offset + lower)];
        const int upper_value = cache_bits50[static_cast<std::size_t>(cache_offset + upper)];
        table.values[entry++] = static_cast<opus_uint8>(target - lower_value <= upper_value - target ? lower : upper);
      }
      table.offsets[static_cast<std::size_t>(row + 1)] = static_cast<opus_uint16>(entry);
    }
  }
  return table;
}

constinit const celt_bits2pulses_lut_table celt_bits2pulses_lut = make_celt_bits2pulses_lut();
} // namespace

struct celt_generated_tables {
  std::array<kiss_twiddle_cpx, 381> fft_twiddles;
  std::array<celt_coef, 1800> mdct_trig;
  std::array<opus_int16, 480> fft_bitrev_480;
  std::array<opus_int16, 240> fft_bitrev_240;
  std::array<opus_int16, 120> fft_bitrev_120;
  std::array<opus_int16, 60> fft_bitrev_60;
  std::array<celt_coef, 120> window;
};

consteval auto celt_sine(double angle) noexcept -> double {
  constexpr double pi = 3.14159265358979323846264338327950288;
  constexpr double two_pi = 2.0 * pi;
  while (angle > pi) {
    angle -= two_pi;
  }
  while (angle < -pi) {
    angle += two_pi;
  }
  if (angle > pi / 2.0) {
    angle = pi - angle;
  } else if (angle < -pi / 2.0) {
    angle = -pi - angle;
  }
  const double squared = angle * angle;
  double term = angle;
  double result = angle;
  for (int index = 1; index < 12; ++index) {
    term *= -squared / static_cast<double>((index * 2) * (index * 2 + 1));
    result += term;
  }
  return result;
}

consteval auto celt_cosine(double angle) noexcept -> double {
  constexpr double half_pi = 1.57079632679489661923132169163975144;
  return celt_sine(half_pi + angle);
}

template <std::size_t Size, std::size_t FactorCount>
consteval void fill_fft_bitrev(std::array<opus_int16, Size>& values, int output, std::size_t index, std::size_t stride,
                               const std::array<int, FactorCount>& factors, std::size_t level, int remaining) noexcept {
  const int radix = factors[level];
  const int next_remaining = remaining / radix;
  if (next_remaining == 1) {
    for (int digit = 0; digit < radix; ++digit) {
      values[index + static_cast<std::size_t>(digit) * stride] = static_cast<opus_int16>(output + digit);
    }
    return;
  }
  for (int digit = 0; digit < radix; ++digit) {
    fill_fft_bitrev(values, output, index, stride * static_cast<std::size_t>(radix), factors, level + 1, next_remaining);
    index += stride;
    output += next_remaining;
  }
}

template <std::size_t Size, int... Factors> consteval auto make_fft_bitrev() noexcept -> std::array<opus_int16, Size> {
  constexpr std::array factors{Factors...};
  std::array<opus_int16, Size> values{};
  fill_fft_bitrev(values, 0, 0, 1, factors, 0, static_cast<int>(Size));
  return values;
}

consteval auto make_celt_tables() noexcept -> celt_generated_tables {
  constexpr double pi = 3.14159265358979323846264338327950288;
  celt_generated_tables tables{};
  for (std::size_t index = 0; index < tables.fft_twiddles.size(); ++index) {
    const double phase = -2.0 * pi * static_cast<double>(index) / 480.0;
    tables.fft_twiddles[index] = {static_cast<float>(celt_cosine(phase)), static_cast<float>(celt_sine(phase))};
  }
  tables.fft_twiddles[0].i = -0.0f;
  tables.fft_twiddles[120].r = 6.123234262925839e-17f;
  tables.fft_twiddles[240].i = -1.2246468525851679e-16f;
  tables.fft_twiddles[360].r = -1.8369701465288538e-16f;
  std::size_t offset = 0;
  for (int size = 1920; size >= 240; size >>= 1) {
    for (int index = 0; index < size / 2; ++index) {
      tables.mdct_trig[offset++] = static_cast<float>(celt_cosine(2.0 * pi * (static_cast<double>(index) + .125) / size));
    }
  }
  tables.fft_bitrev_480 = make_fft_bitrev<480, 5, 3, 4, 2, 4>();
  tables.fft_bitrev_240 = make_fft_bitrev<240, 5, 3, 4, 4>();
  tables.fft_bitrev_120 = make_fft_bitrev<120, 5, 3, 2, 4>();
  tables.fft_bitrev_60 = make_fft_bitrev<60, 5, 3, 4>();
  for (std::size_t index = 0; index < tables.window.size(); ++index) {
    const double inner = celt_sine(.5 * pi * (static_cast<double>(index) + .5) / tables.window.size());
    tables.window[index] = static_cast<float>(celt_sine(.5 * pi * inner * inner));
  }
  return tables;
}

static constexpr celt_generated_tables celt_tables = make_celt_tables();

static constexpr kiss_fft_state fft_state48000_960_0{480, 1.f / 480, celt_tables.fft_twiddles.data(), celt_tables.fft_bitrev_480.data()};
static constexpr kiss_fft_state fft_state48000_960_1{240, 1.f / 240, celt_tables.fft_twiddles.data(), celt_tables.fft_bitrev_240.data()};
static constexpr kiss_fft_state fft_state48000_960_2{120, 1.f / 120, celt_tables.fft_twiddles.data(), celt_tables.fft_bitrev_120.data()};
static constexpr kiss_fft_state fft_state48000_960_3{60, 1.f / 60, celt_tables.fft_twiddles.data(), celt_tables.fft_bitrev_60.data()};
static constexpr CeltModeInternal mode48000_960_120 = {
    eband5ms.data(),
    band_allocation.data(),
    logN400.data(),
    celt_tables.window.data(),
    mdct_lookup{
        1920, {&fft_state48000_960_0, &fft_state48000_960_1, &fft_state48000_960_2, &fft_state48000_960_3}, celt_tables.mdct_trig.data()},
    cache_index50.data(),
    cache_bits50.data(),
    cache_caps50.data()};
[[nodiscard]] static constexpr auto celt_mode() noexcept -> const CeltModeInternal* {
  return &mode48000_960_120;
}

static void find_best_pitch(opus_val32* xcorr, opus_val16* y, int len, int max_pitch, int* best_pitch) {
  opus_val32 Syy = 1;
  opus_val16 best_num0 = -1, best_num1 = -1;
  opus_val32 best_den0 = 0, best_den1 = 0;
  int best_pitch0 = 0, best_pitch1 = 1;
  for (int j = 0; j < len; j++) {
    Syy = ((Syy) + (((static_cast<opus_val32>(y[j]) * static_cast<opus_val32>(y[j])))));
  }
  for (int i = 0; i < max_pitch; i++) {
    if (xcorr[i] > 0) {
      opus_val32 xcorr16 = ((xcorr[i]));
      xcorr16 *= 1e-12f;
      const opus_val16 num = ((xcorr16) * (xcorr16));
      if (((num)*best_den1) > (best_num1 * Syy)) {
        if (((num)*best_den0) > (best_num0 * Syy)) {
          best_num1 = best_num0;
          best_den1 = best_den0;
          best_pitch1 = best_pitch0;
          best_num0 = num;
          best_den0 = Syy;
          best_pitch0 = i;
        } else {
          best_num1 = num;
          best_den1 = Syy;
          best_pitch1 = i;
        }
      }
    }
    Syy += ((static_cast<opus_val32>(y[i + len]) * static_cast<opus_val32>(y[i + len]))) -
           ((static_cast<opus_val32>(y[i]) * static_cast<opus_val32>(y[i])));
    Syy = std::max(1.f, Syy);
  }
  best_pitch[0] = best_pitch0;
  best_pitch[1] = best_pitch1;
}

static void celt_fir5(opus_val16* x, const opus_val16* num, int N) {
  const opus_val16 num0 = num[0];
  const opus_val16 num1 = num[1];
  const opus_val16 num2 = num[2];
  const opus_val16 num3 = num[3];
  const opus_val16 num4 = num[4];
  opus_val32 mem0 = 0;
  opus_val32 mem1 = 0;
  opus_val32 mem2 = 0;
  opus_val32 mem3 = 0;
  opus_val32 mem4 = 0;
  for (int i = 0; i < N; i++) {
    opus_val32 sum = ((x[i]));
    sum = ((sum) + static_cast<opus_val32>(num0) * (mem0));
    sum = ((sum) + static_cast<opus_val32>(num1) * (mem1));
    sum = ((sum) + static_cast<opus_val32>(num2) * (mem2));
    sum = ((sum) + static_cast<opus_val32>(num3) * (mem3));
    sum = ((sum) + static_cast<opus_val32>(num4) * (mem4));
    mem4 = mem3;
    mem3 = mem2;
    mem2 = mem1;
    mem1 = mem0;
    mem0 = x[i];
    x[i] = (sum);
  }
}

static void pitch_downsample(celt_sig* const* x, int channels, opus_val16* x_lp, int len) {
  std::array<opus_val32, 5> ac{};
  opus_val16 tmp = 1.0f;
  std::array<opus_val16, 4> lpc{};
  std::array<opus_val16, 5> lpc2{};
  opus_val16 c1 = (.8f);
  constexpr int factor = 2, offset = 1;
  for (int i = 1; i < len; i++) {
    x_lp[i] = .25f * x[0][(factor * i - offset)] + .25f * x[0][(factor * i + offset)] + .5f * x[0][factor * i];
  }
  x_lp[0] = .25f * x[0][offset] + .5f * x[0][0];
  if (channels == 2) {
    for (int i = 1; i < len; i++) {
      x_lp[i] += .25f * x[1][(factor * i - offset)] + .25f * x[1][(factor * i + offset)] + .5f * x[1][factor * i];
    }
    x_lp[0] += .25f * x[1][offset] + .5f * x[1][0];
  }
  _celt_autocorr(x_lp, ac.data(), nullptr, 0, 4, len);
  ac[0] *= 1.0001f;
  for (int i = 1; i <= 4; i++) {
    ac[i] -= ac[i] * (.008f * i) * (.008f * i);
  }
  _celt_lpc(lpc.data(), ac.data(), 4);
  for (int i = 0; i < 4; i++) {
    tmp = (((.9f)) * (tmp));
    lpc[i] = ((lpc[i]) * (tmp));
  }
  lpc2[0] = lpc[0] + (.8f);
  lpc2[1] = lpc[1] + ((c1) * (lpc[0]));
  lpc2[2] = lpc[2] + ((c1) * (lpc[1]));
  lpc2[3] = lpc[3] + ((c1) * (lpc[2]));
  lpc2[4] = ((c1) * (lpc[3]));
  celt_fir5(x_lp, lpc2.data(), len);
}

static void celt_pitch_xcorr_c(const opus_val16* x, const opus_val16* y, opus_val32* xcorr, int len, int max_pitch) {
  int i;
  for (i = 0; i < max_pitch - 3; i += 4) {
    std::array<opus_val32, 4> sum{};
    xcorr_kernel_c(x, y + i, sum, len);
    xcorr[i] = sum[0];
    xcorr[i + 1] = sum[1];
    xcorr[i + 2] = sum[2];
    xcorr[i + 3] = sum[3];
  }
  for (; i < max_pitch; i++) {
    opus_val32 sum = celt_inner_prod_c(x, y + i, len);
    xcorr[i] = sum;
  }
}

static void pitch_search(const opus_val16* x_lp, opus_val16* y, int len, int max_pitch, int* pitch) {
  std::array<int, 2> best_pitch{};
  int offset;
  const int lag = len + max_pitch;
  std::array<opus_val16, celt_max_pitch_period / 2> x_lp4;
  std::array<opus_val16, celt_max_pitch_period / 2> y_lp4;
  std::array<opus_val32, celt_max_pitch_period / 2> xcorr;
  for (int j = 0; j < len >> 2; j++) {
    x_lp4[j] = x_lp[2 * j];
  }
  for (int j = 0; j < lag >> 2; j++) {
    y_lp4[j] = y[2 * j];
  }
  celt_pitch_xcorr_c(x_lp4.data(), y_lp4.data(), xcorr.data(), len >> 2, max_pitch >> 2);
  find_best_pitch(xcorr.data(), y_lp4.data(), len >> 2, max_pitch >> 2, best_pitch.data());
  for (int i = 0; i < max_pitch >> 1; i++) {
    xcorr[i] = 0;
    if (std::abs(i - 2 * best_pitch[0]) > 2 && std::abs(i - 2 * best_pitch[1]) > 2) {
      continue;
    }
    const opus_val32 sum = celt_inner_prod_c(x_lp, y + i, len >> 1);
    xcorr[i] = std::max(-1.f, sum);
  }
  find_best_pitch(xcorr.data(), y, len >> 1, max_pitch >> 1, best_pitch.data());
  if (best_pitch[0] > 0 && best_pitch[0] < (max_pitch >> 1) - 1) {
    const opus_val32 a = xcorr[best_pitch[0] - 1];
    const opus_val32 b = xcorr[best_pitch[0]];
    const opus_val32 c = xcorr[best_pitch[0] + 1];
    if ((c - a) > (((.7f)) * (b - a))) {
      offset = 1;
    } else if ((a - c) > (((.7f)) * (b - c)))
      offset = -1;
    else
      offset = 0;
  } else {
    offset = 0;
  }
  *pitch = 2 * best_pitch[0] - offset;
}

static opus_val16 compute_pitch_gain(opus_val32 xy, opus_val32 xx, opus_val32 yy) {
  return xy / (std::sqrt(1 + xx * yy));
}

constexpr std::array<opus_uint8, 16> second_check{0, 0, 3, 2, 3, 2, 5, 2, 3, 2, 3, 2, 5, 2, 3, 2};
static opus_val16 remove_doubling(opus_val16* x, int N, int* T0_, int prev_period, opus_val16 prev_gain) {
  constexpr int maxperiod = celt_max_pitch_period / 2;
  constexpr int minperiod = celt_min_pitch_period / 2;
  constexpr int minperiod0 = celt_min_pitch_period;
  opus_val32 xy, xx;
  std::array<opus_val32, 3> xcorr;
  int offset;
  *T0_ /= 2;
  prev_period /= 2;
  N /= 2;
  x += maxperiod;
  if (*T0_ >= maxperiod) {
    *T0_ = maxperiod - 1;
  }
  int T = *T0_;
  const int T0 = T;
  std::array<opus_val32, celt_max_pitch_period / 2 + 1> yy_lookup;
  dual_inner_prod_c(x, x, x - T0, N, xx, xy);
  yy_lookup[0] = xx;
  opus_val32 yy = xx;
  for (int i = 1; i <= maxperiod; i++) {
    yy = yy + (static_cast<opus_val32>(x[-i]) * static_cast<opus_val32>(x[-i])) -
         (static_cast<opus_val32>(x[N - i]) * static_cast<opus_val32>(x[N - i]));
    yy_lookup[i] = std::max(0.f, yy);
  }
  yy = yy_lookup[T0];
  opus_val32 best_xy = xy;
  opus_val32 best_yy = yy;
  opus_val16 g = compute_pitch_gain(xy, xx, yy);
  const opus_val16 g0 = g;
  for (int k = 2; k <= 15; k++) {
    const int T1 = celt_udiv(2 * T0 + k, 2 * k);
    if (T1 < minperiod) {
      break;
    }
    int T1b;
    if (k == 2) {
      if (T1 + T0 > maxperiod) {
        T1b = T0;
      } else
        T1b = T0 + T1;
    } else {
      T1b = celt_udiv(2 * second_check[k] * T0 + k, 2 * k);
    }
    opus_val32 xy2;
    dual_inner_prod_c(x, &x[-T1], &x[-T1b], N, xy, xy2);
    xy = (.5f * (xy + xy2));
    yy = (.5f * (yy_lookup[T1] + yy_lookup[T1b]));
    const opus_val16 g1 = compute_pitch_gain(xy, xx, yy);
    const opus_val16 cont = std::abs(T1 - prev_period) <= 1                     ? prev_gain
                            : std::abs(T1 - prev_period) <= 2 && 5 * k * k < T0 ? .5f * prev_gain
                                                                                : 0;
    opus_val16 thresh = std::max(.3f, .7f * g0 - cont);
    if (T1 < 3 * minperiod) {
      thresh = std::max(.4f, .85f * g0 - cont);
    } else if (T1 < 2 * minperiod) {
      thresh = std::max(.5f, .9f * g0 - cont);
    }
    if (g1 > thresh) {
      best_xy = xy;
      best_yy = yy;
      T = T1;
      g = g1;
    }
  }
  if (T < minperiod * 2) {
    const int T1 = T * 5 / 8;
    const int T2 = T * 6 / 8;
    opus_val32 xy2;
    dual_inner_prod_c(x, &x[-T1], &x[-T2], N, xy, xy2);
    const opus_val16 g1 = compute_pitch_gain(xy, xx, yy_lookup[T1]);
    const opus_val16 g2 = compute_pitch_gain(xy2, xx, yy_lookup[T2]);
    if (g1 >= g || g2 >= g) {
      g = 0;
    }
  }
  best_xy = std::max(0.f, best_xy);
  const opus_val16 pg = std::min(g, best_yy <= best_xy ? 1.0f : static_cast<float>(best_xy) / (best_yy + 1));
  for (int k = 0; k < 3; k++) {
    xcorr[static_cast<std::size_t>(k)] = celt_inner_prod_c(x, x - (T + k - 1), N);
  }
  if ((xcorr[2] - xcorr[0]) > (((.7f)) * (xcorr[1] - xcorr[0]))) {
    offset = 1;
  } else if ((xcorr[0] - xcorr[2]) > (((.7f)) * (xcorr[1] - xcorr[2])))
    offset = -1;
  else
    offset = 0;
  *T0_ = 2 * T + offset;
  if (*T0_ < minperiod0) {
    *T0_ = minperiod0;
  }
  return pg;
}

static void _celt_lpc(opus_val16* _lpc, const opus_val32* ac, int p) {
  opus_val32 error = ac[0];
  float* lpc = _lpc;
  zero_n_items(lpc, static_cast<std::size_t>(p));
  if (ac[0] > 1e-10f) {
    for (int i = 0; i < p; i++) {
      opus_val32 rr = 0;
      for (int j = 0; j < i; j++) {
        rr += ((lpc[j]) * (ac[i - j]));
      }
      rr += (ac[i + 1]);
      const opus_val32 r = -(static_cast<float>((rr)) / (error));
      lpc[i] = r;
      for (int j = 0; j < (i + 1) >> 1; j++) {
        const opus_val32 tmp1 = lpc[j];
        const opus_val32 tmp2 = lpc[i - 1 - j];
        lpc[j] = tmp1 + r * tmp2;
        lpc[i - 1 - j] = tmp2 + r * tmp1;
      }
      error -= r * r * error;
      if (error <= .001f * ac[0]) {
        break;
      }
    }
  }
}

static void celt_fir_c(const opus_val16* x, const opus_val16* num, opus_val16* y, int N) {
  std::array<opus_val16, celt_lpc_order> rnum;
  std::reverse_copy(num, num + celt_lpc_order, rnum.begin());
  int i = 0;
  for (; i < N - 3; i += 4) {
    std::array<opus_val32, 4> sum;
    sum[0] = ((x[i]));
    sum[1] = ((x[i + 1]));
    sum[2] = ((x[i + 2]));
    sum[3] = ((x[i + 3]));
    xcorr_kernel_c(rnum.data(), x + i - celt_lpc_order, sum, celt_lpc_order);
    y[i] = (sum[0]);
    y[i + 1] = (sum[1]);
    y[i + 2] = (sum[2]);
    y[i + 3] = (sum[3]);
  }
  for (; i < N; i++) {
    opus_val32 sum = ((x[i]));
    for (int j = 0; j < celt_lpc_order; j++) {
      sum = ((sum) + static_cast<opus_val32>(rnum[j]) * static_cast<opus_val32>(x[i + j - celt_lpc_order]));
    }
    y[i] = (sum);
  }
}

static void celt_iir(const opus_val32* _x, const opus_val16* den, opus_val32* _y, int N, opus_val16* mem) {
  std::array<opus_val16, celt_lpc_order> rden;
  std::array<opus_val16, celt_plc_max_period + celt_lpc_order> y;
  std::reverse_copy(den, den + celt_lpc_order, rden.begin());
  for (int index = 0; index < celt_lpc_order; ++index) {
    y[index] = -mem[celt_lpc_order - 1 - index];
  }
  zero_n_items(y.data() + celt_lpc_order, static_cast<std::size_t>(N));
  int i = 0;
  for (; i < N - 3; i += 4) {
    std::array<opus_val32, 4> sum;
    sum[0] = _x[i];
    sum[1] = _x[i + 1];
    sum[2] = _x[i + 2];
    sum[3] = _x[i + 3];
    xcorr_kernel_c(rden.data(), y.data() + i, sum, celt_lpc_order);
    y[i + celt_lpc_order] = -(sum[0]);
    _y[i] = sum[0];
    sum[1] = ((sum[1]) + static_cast<opus_val32>(y[i + celt_lpc_order]) * static_cast<opus_val32>(den[0]));
    y[i + celt_lpc_order + 1] = -(sum[1]);
    _y[i + 1] = sum[1];
    sum[2] = ((sum[2]) + static_cast<opus_val32>(y[i + celt_lpc_order + 1]) * static_cast<opus_val32>(den[0]));
    sum[2] = ((sum[2]) + static_cast<opus_val32>(y[i + celt_lpc_order]) * static_cast<opus_val32>(den[1]));
    y[i + celt_lpc_order + 2] = -(sum[2]);
    _y[i + 2] = sum[2];
    sum[3] = ((sum[3]) + static_cast<opus_val32>(y[i + celt_lpc_order + 2]) * static_cast<opus_val32>(den[0]));
    sum[3] = ((sum[3]) + static_cast<opus_val32>(y[i + celt_lpc_order + 1]) * static_cast<opus_val32>(den[1]));
    sum[3] = ((sum[3]) + static_cast<opus_val32>(y[i + celt_lpc_order]) * static_cast<opus_val32>(den[2]));
    y[i + celt_lpc_order + 3] = -(sum[3]);
    _y[i + 3] = sum[3];
  }
  for (; i < N; i++) {
    opus_val32 sum = _x[i];
    for (int j = 0; j < celt_lpc_order; j++) {
      sum -= (static_cast<opus_val32>(rden[j]) * static_cast<opus_val32>(y[i + j]));
    }
    y[i + celt_lpc_order] = (sum);
    _y[i] = sum;
  }
  for (i = 0; i < celt_lpc_order; i++) {
    mem[i] = _y[N - i - 1];
  }
}

static void _celt_autocorr(const opus_val16* x, opus_val32* ac, const celt_coef* window, int overlap, int lag, int n) {
  const int fastN = n - lag;
  auto accumulate = [&](const opus_val16* input) {
    celt_pitch_xcorr_c(input, input, ac, fastN, lag + 1);
    for (int k = 0; k <= lag; ++k) {
      opus_val32 tail = 0;
      for (int i = k + fastN; i < n; ++i) {
        tail += input[i] * input[i - k];
      }
      ac[k] += tail;
    }
  };
  if (overlap == 0) {
    accumulate(x);
    return;
  }

  std::array<opus_val16, celt_plc_max_period> windowed_storage;
  copy_n_items(x, static_cast<std::size_t>(n), windowed_storage.data());
  for (int i = 0; i < overlap; ++i) {
    const opus_val16 weight = window[i];
    windowed_storage[i] = x[i] * weight;
    windowed_storage[n - i - 1] = x[n - i - 1] * weight;
  }
  accumulate(windowed_storage.data());
}
namespace {
template <std::size_t Count>
  requires(Count >= celt_default_nb_ebands)
[[nodiscard]] consteval auto make_celt_noise_floor_base(const std::array<opus_int16, Count>& logN) noexcept {
  std::array<opus_val16, celt_default_nb_ebands> base{};
  for (std::size_t i = 0; i < base.size(); ++i) {
    base[i] = (0.0625f) * logN[i] + (.5f) - eMeans[i] + (.0062f) * static_cast<opus_val16>((i + 5) * (i + 5));
  }
  return base;
}

constinit const std::array<opus_val16, celt_default_nb_ebands> celt_noise_floor_base = make_celt_noise_floor_base(logN400);
} // namespace

constexpr std::array<opus_val16, 4> pred_coef = numeric_blob_array<opus_val16>(R"blob(3F6600003F4C00003F2600003F000000)blob");
constexpr std::array<opus_val16, 4> beta_coef = numeric_blob_array<opus_val16>(R"blob(3F6B86003F2E14003EBD70003E4CD000)blob");
constexpr opus_val16 beta_intra = 4915 / 32768.;
constexpr std::array<std::array<unsigned char, 42>, 8> e_prob_model = numeric_blob_matrix<unsigned char, 42>(
    R"blob(487F41814280418040803E80408040805C4E5C4F5C4E5A4F742973287228841A841A9111A10CB00AB10B18B3308A3687368435863885378437843D7246604A584B58574A59425B43643B6C3278287A25612B4E32534E5451584B564A57475A495D4A5D4A6D287224752275228F1191129213A20CA50AB207BD06BE08B10917B236733F66426245634A59475B495B4E5956505C425D40663B673C683C75347B2C8A23851F61264D2D3D5A5D3C692A6B296E2D7426712670267C1A841B88138C149B0E9F109E12AA0DB10ABB08C006AF099F0A15B23B6E47564B5554535B42584957485C4B6248693A6B367334723770388133842896218C1D62234D2A2A7960426C2B6F28752C7B20782477217F2186228B15931798149E199A1AA615AD10B80DB80A960D8B0F16B23F724A5254535C52673E6048604365496B48713776347D3476347537873189279D20911D61214D28)blob");
[[nodiscard]] constexpr auto floor_to_int_reference(opus_val32 value) noexcept -> int {
  return static_cast<int>(std::floor(static_cast<double>(value)));
}

static inline opus_val32 loss_distortion(const celt_glog* eBands, celt_glog* oldEBands, int start, int end, int C) {
  opus_val32 dist = 0;
  for (int c = 0; c < C; ++c) {
    const int band_offset = c * celt_default_nb_ebands;
    for (int i = start; i < end; ++i) {
      const int index = band_offset + i;
      celt_glog d = (((eBands[index]) - (oldEBands[index])));
      dist = ((dist) + static_cast<opus_val32>(d) * static_cast<opus_val32>(d));
    }
  }
  return std::min(200.0f, dist);
}

template <bool Encode>
static void process_coarse_energy(int start, int end, const celt_glog* eBands, celt_glog* oldEBands, opus_int32 budget, opus_int32 tell,
                                  const unsigned char* prob_model, celt_glog* error, ec_ctx* coder, int C, int LM, int intra,
                                  celt_glog max_decay) {
  std::array<opus_val32, 2> prev{};
  if constexpr (Encode) {
    if (tell + 3 <= budget) {
      ec_enc_bit_logp(coder, intra, 3);
    }
  }
  const opus_val16 coef = intra ? 0 : pred_coef[LM];
  const opus_val16 beta = intra ? beta_intra : beta_coef[LM];
  for (int i = start; i < end; ++i) {
    for (int c = 0; c < C; ++c) {
      const int index = i + c * celt_default_nb_ebands;
      const celt_glog old_energy = std::max(-9.f, oldEBands[index]);
      int qi;
      if constexpr (Encode) {
        const celt_glog input_energy = eBands[index];
        const opus_val32 residual = input_energy - coef * old_energy - prev[c];
        qi = floor_to_int_reference(.5f + residual);
        const auto decay_bound = std::max(-28.f, oldEBands[index]) - max_decay;
        if (qi < 0 && input_energy < decay_bound) {
          qi = std::min(0, qi + static_cast<int>(decay_bound - input_energy));
        }
        tell = ec_tell(coder);
        const int bits_left = budget - tell - 3 * C * (end - i);
        if (i != start && bits_left < 30) {
          if (bits_left < 24) {
            qi = std::min(1, qi);
          }
          if (bits_left < 16) {
            qi = std::max(-1, qi);
          }
        }
        if (budget - tell >= 15) {
          const int probability_index = 2 * std::min(i, 20);
          ec_laplace_encode(coder, &qi, prob_model[probability_index] << 7, prob_model[probability_index + 1] << 6);
        } else if (budget - tell >= 2) {
          qi = clamp_value(qi, -1, 1);
          ec_enc_icdf(coder, 2 * qi ^ -(qi < 0), shared_three_step_icdf.data(), 2);
        } else if (budget - tell >= 1) {
          qi = std::min(0, qi);
          ec_enc_bit_logp(coder, -qi, 1);
        } else {
          qi = -1;
        }
        error[index] = residual - qi;
      } else {
        tell = ec_tell(coder);
        if (budget - tell >= 15) {
          const int probability_index = 2 * std::min(i, 20);
          qi = ec_laplace_decode(coder, prob_model[probability_index] << 7, prob_model[probability_index + 1] << 6);
        } else if (budget - tell >= 2) {
          qi = ec_dec_icdf(coder, shared_three_step_icdf.data(), 2);
          qi = (qi >> 1) ^ -(qi & 1);
        } else {
          qi = budget - tell >= 1 ? -ec_dec_bit_logp(coder, 1) : -1;
        }
      }
      const auto quantized = static_cast<opus_val32>(qi);
      oldEBands[index] = coef * old_energy + prev[c] + quantized;
      prev[c] += quantized - beta * quantized;
    }
  }
}

static void quant_coarse_energy(int start, int end, const celt_glog* eBands, celt_glog* oldEBands, opus_uint32 budget, celt_glog* error,
                                ec_enc* enc, int C, int LM, int nbAvailableBytes, int force_intra, opus_val32* delayedIntra) {
  celt_glog max_decay;
  int intra = force_intra || (*delayedIntra > 2 * C * (end - start) && nbAvailableBytes > (end - start) * C);
  const opus_val32 new_distortion = loss_distortion(eBands, oldEBands, start, end, C);
  const opus_uint32 tell = ec_tell(enc);
  if (tell + 3 > budget) {
    intra = 0;
  }
  max_decay = (16.f);
  if (end - start > 10) {
    max_decay = std::min(max_decay, .125f * nbAvailableBytes);
  }
  process_coarse_energy<true>(start, end, eBands, oldEBands, budget, tell, e_prob_model[static_cast<std::size_t>(LM * 2 + intra)].data(),
                              error, enc, C, LM, intra, max_decay);
  if (intra) {
    *delayedIntra = new_distortion;
  } else {
    *delayedIntra = pred_coef[LM] * pred_coef[LM] * *delayedIntra + new_distortion;
  }
}

template <bool Encode>
static void process_fine_energy(int start, int end, celt_glog* oldEBands, celt_glog* error, const int* prev_quant, const int* extra_quant,
                                ec_ctx* coder, int C) {
  for (int i = start; i < end; ++i) {
    const auto extra_bits = extra_quant[i];
    if (extra_bits <= 0 || ec_tell(coder) + C * extra_bits > static_cast<opus_int32>(coder->storage * 8)) {
      continue;
    }
    const auto prev = prev_quant == nullptr ? 0 : prev_quant[i];
    const auto bins = 1 << extra_bits;
    for (int c = 0; c < C; ++c) {
      int q2 = 0;
      if constexpr (Encode) {
        q2 = floor_to_int_reference((error[i + c * celt_default_nb_ebands] * (1 << prev) + .5f) * bins);
        q2 = clamp_value(q2, 0, bins - 1);
        ec_enc_bits(coder, q2, extra_bits);
      } else {
        q2 = ec_dec_bits(coder, extra_bits);
      }
      auto offset = (q2 + .5f) * (1 << (14 - extra_bits)) * (1.f / 16384) - .5f;
      offset *= (1 << (14 - prev)) * (1.f / 16384);
      oldEBands[i + c * celt_default_nb_ebands] += offset;
      if constexpr (Encode) {
        error[i + c * celt_default_nb_ebands] -= offset;
      }
    }
  }
}

template <bool Encode>
static void process_energy_finalise(int start, int end, celt_glog* oldEBands, celt_glog* error, const int* fine_quant,
                                    const int* fine_priority, int bits_left, ec_ctx* coder, int C) {
  for (int prio = 0; prio < 2; ++prio) {
    for (int i = start; i < end && bits_left >= C; ++i) {
      if (fine_quant[i] >= 8 || fine_priority[i] != prio) {
        continue;
      }
      for (int c = 0; c < C; ++c) {
        const auto q2 = [&]() {
          if constexpr (Encode) {
            const auto encoded = error[i + c * celt_default_nb_ebands] < 0 ? 0 : 1;
            ec_enc_bits(coder, encoded, 1);
            return encoded;
          }
          return static_cast<int>(ec_dec_bits(coder, 1));
        }();
        const auto offset = (q2 - .5f) * (1 << (14 - fine_quant[i] - 1)) * (1.f / 16384);
        if (oldEBands != nullptr) {
          oldEBands[i + c * celt_default_nb_ebands] += offset;
        }
        if constexpr (Encode) {
          error[i + c * celt_default_nb_ebands] -= offset;
        }
        bits_left--;
      }
    }
  }
}

static void unquant_coarse_energy(int start, int end, celt_glog* oldEBands, int intra, ec_dec* dec, int C, int LM) {
  process_coarse_energy<false>(start, end, nullptr, oldEBands, dec->storage * 8, 0,
                               e_prob_model[static_cast<std::size_t>(LM * 2 + intra)].data(), nullptr, dec, C, LM, intra, 0);
}

constexpr std::array<unsigned char, 24> LOG2_FRAC_TABLE =
    numeric_blob_array<unsigned char>(R"blob(00080D10131517181A1B1C1D1E1F20202122222324242525)blob");
static int interp_bits2pulses(int start, int end, int skip_start, const int* bits1, const int* bits2, const int* thresh, const int* cap,
                              opus_int32 total, opus_int32* _balance, int skip_rsv, int* intensity, int intensity_rsv, int* dual_stereo,
                              int dual_stereo_rsv, int* bits, int* ebits, int* fine_priority, int C, int LM, ec_ctx* ec, int encode,
                              int prev, int signalBandwidth) {
  const int alloc_floor = C << 3;
  const int stereo = C > 1;
  const int logM = LM << 3;
  const auto interpolate = [&]<bool Store>(int fraction) {
    opus_int32 sum = 0;
    bool done = false;
    for (int band = end; band-- > start;) {
      int value = bits1[band] + (fraction * static_cast<opus_int32>(bits2[band]) >> 6);
      if (value < thresh[band] && !done) {
        value = value >= alloc_floor ? alloc_floor : 0;
      } else {
        done = true;
      }
      value = std::min(value, cap[band]);
      if constexpr (Store) {
        bits[band] = value;
      }
      sum += value;
    }
    return sum;
  };
  int lo = 0;
  int hi = 1 << 6;
  for (int iteration = 0; iteration < 6; ++iteration) {
    const int mid = (lo + hi) >> 1;
    if (interpolate.operator()<false>(mid) > total) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  opus_int32 psum = interpolate.operator()<true>(lo);
  int codedBands = end;
  opus_int32 left;
  opus_int32 percoeff;
  for (;; --codedBands) {
    const int band = codedBands - 1;
    if (band <= skip_start) {
      total += skip_rsv;
      break;
    }
    left = total - psum;
    percoeff = celt_udiv(left, celt_mode()->eBands[codedBands] - celt_mode()->eBands[start]);
    left -= (celt_mode()->eBands[codedBands] - celt_mode()->eBands[start]) * percoeff;
    const int rem = std::max<opus_int32>(left - (celt_mode()->eBands[band] - celt_mode()->eBands[start]), 0);
    const int band_width = celt_mode()->eBands[codedBands] - celt_mode()->eBands[band];
    int band_bits = bits[band] + percoeff * band_width + rem;
    if (band_bits >= std::max(thresh[band], alloc_floor + (1 << 3))) {
      if (encode) {
        const int depth_threshold = codedBands > 17 ? (band < prev ? 7 : 9) : 0;
        if (codedBands <= start + 2 || (band_bits > (depth_threshold * band_width << LM << 3) >> 4 && band <= signalBandwidth)) {
          ec_enc_bit_logp(ec, 1, 1);
          break;
        }
        ec_enc_bit_logp(ec, 0, 1);
      } else if (ec_dec_bit_logp(ec, 1)) {
        break;
      }
      psum += 1 << 3;
      band_bits -= 1 << 3;
    }
    psum -= bits[band] + intensity_rsv;
    if (intensity_rsv > 0) {
      intensity_rsv = LOG2_FRAC_TABLE[band - start];
    }
    psum += intensity_rsv;
    if (band_bits >= alloc_floor) {
      psum += alloc_floor;
      bits[band] = alloc_floor;
    } else {
      bits[band] = 0;
    }
  }
  if (intensity_rsv > 0) {
    if (encode) {
      *intensity = std::min(*intensity, codedBands);
      ec_enc_uint(ec, *intensity - start, codedBands + 1 - start);
    } else
      *intensity = start + ec_dec_uint(ec, codedBands + 1 - start);
  } else
    *intensity = 0;
  if (*intensity <= start) {
    total += dual_stereo_rsv;
    dual_stereo_rsv = 0;
  }
  if (dual_stereo_rsv > 0) {
    if (encode) {
      ec_enc_bit_logp(ec, *dual_stereo, 1);
    } else
      *dual_stereo = ec_dec_bit_logp(ec, 1);
  } else
    *dual_stereo = 0;
  left = total - psum;
  percoeff = celt_udiv(left, celt_mode()->eBands[codedBands] - celt_mode()->eBands[start]);
  left -= (celt_mode()->eBands[codedBands] - celt_mode()->eBands[start]) * percoeff;
  opus_int32 balance = 0;
  int band = start;
  for (; band < codedBands; ++band) {
    const int N0 = celt_mode()->eBands[band + 1] - celt_mode()->eBands[band];
    const int remainder = static_cast<int>(std::min(left, N0));
    bits[band] += static_cast<int>(percoeff) * N0 + remainder;
    left -= remainder;
    const int N = N0 << LM;
    const opus_int32 bit = static_cast<opus_int32>(bits[band]) + balance;
    opus_int32 excess;
    if (N > 1) {
      excess = std::max(bit - cap[band], 0);
      bits[band] = bit - excess;
      const int den = C * N + (C == 2 && N > 2 && !*dual_stereo && band < *intensity);
      const int NClogN = den * (celt_mode()->logN[band] + logM);
      int offset = (NClogN >> 1) - den * 21;
      if (N == 2) {
        offset += den << 3 >> 2;
      }
      if (bits[band] + offset < den * 2 << 3) {
        offset += NClogN >> 2;
      } else if (bits[band] + offset < den * 3 << 3) {
        offset += NClogN >> 3;
      }
      ebits[band] = celt_udiv(std::max(0, bits[band] + offset + (den << 2)), den) >> 3;
      if (C * ebits[band] > (bits[band] >> 3)) {
        ebits[band] = bits[band] >> stereo >> 3;
      }
      ebits[band] = std::min(ebits[band], 8);
      fine_priority[band] = ebits[band] * (den << 3) >= bits[band] + offset;
      bits[band] -= C * ebits[band] << 3;
    } else {
      excess = std::max(0, bit - (C << 3));
      bits[band] = bit - excess;
      ebits[band] = 0;
      fine_priority[band] = 1;
    }
    if (excess > 0) {
      const int extra_fine = std::min<int>(excess >> (stereo + 3), 8 - ebits[band]);
      ebits[band] += extra_fine;
      const int extra_bits = extra_fine * C << 3;
      fine_priority[band] = extra_bits >= excess - balance;
      excess -= extra_bits;
    }
    balance = excess;
  }
  *_balance = balance;
  for (; band < end; ++band) {
    ebits[band] = bits[band] >> stereo >> 3;
    bits[band] = 0;
    fine_priority[band] = ebits[band] < 1;
  }
  return codedBands;
}

static int clt_compute_allocation(int start, int end, const int* offsets, const int* cap, int alloc_trim, int* intensity, int* dual_stereo,
                                  opus_int32 total, opus_int32* balance, int* pulses, int* ebits, int* fine_priority, int C, int LM,
                                  ec_ctx* ec, int encode, int prev, int signalBandwidth) {
  total = std::max(total, 0);
  constexpr int len = celt_default_nb_ebands;
  int skip_start = start;
  const int skip_rsv = total >= 1 << 3 ? 1 << 3 : 0;
  total -= skip_rsv;
  int intensity_rsv = 0;
  int dual_stereo_rsv = 0;
  if (C == 2) {
    intensity_rsv = LOG2_FRAC_TABLE[end - start];
    if (intensity_rsv > total) {
      intensity_rsv = 0;
    } else {
      total -= intensity_rsv;
      dual_stereo_rsv = total >= 1 << 3 ? 1 << 3 : 0;
      total -= dual_stereo_rsv;
    }
  }
  std::array<std::array<int, celt_default_nb_ebands>, 4> allocation_workspace;
  auto& [bits1, bits2, thresh, trim_offset] = allocation_workspace;
  for (int band = start; band < end; ++band) {
    const auto band_width = celt_mode()->eBands[band + 1] - celt_mode()->eBands[band];
    const auto channel_min_bits = C << 3;
    const auto band_min_bits = ((3 * band_width) << LM << 3) >> 4;
    thresh[band] = std::max(channel_min_bits, band_min_bits);
    trim_offset[band] = C * band_width * (alloc_trim - 5 - LM) * (end - band - 1) * (1 << (LM + 3)) >> 6;
    if ((band_width << LM) == 1) {
      trim_offset[band] -= channel_min_bits;
    }
  }
  const auto vector_bits = [&](int vector, int band, bool add_offset) {
    const int width = celt_mode()->eBands[band + 1] - celt_mode()->eBands[band];
    int value = C * width * celt_mode()->allocVectors[vector * len + band] << LM >> 2;
    if (value > 0) {
      value = std::max(0, value + trim_offset[band]);
    }
    return value + (add_offset ? offsets[band] : 0);
  };
  int lo = 1;
  int hi = celt_allocation_vector_count - 1;
  for (; lo <= hi;) {
    bool done = false;
    int psum = 0;
    const int mid = (lo + hi) >> 1;
    for (int band = end; band-- > start;) {
      const int value = vector_bits(mid, band, true);
      if (value >= thresh[band] || done) {
        done = true;
        psum += std::min(value, cap[band]);
      } else if (value >= C << 3) {
        psum += C << 3;
      }
    }
    if (psum > total) {
      hi = mid - 1;
    } else
      lo = mid + 1;
  }
  hi = lo--;
  for (int band = start; band < end; ++band) {
    const int bits1j = vector_bits(lo, band, lo > 0);
    int bits2j =
        hi >= celt_allocation_vector_count ? std::max(0, cap[band] + trim_offset[band]) + offsets[band] : vector_bits(hi, band, true);
    if (offsets[band] > 0) {
      skip_start = band;
    }
    bits2j = std::max(0, bits2j - bits1j);
    bits1[band] = bits1j;
    bits2[band] = bits2j;
  }
  return interp_bits2pulses(start, end, skip_start, bits1.data(), bits2.data(), thresh.data(), cap, total, balance, skip_rsv, intensity,
                            intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits, fine_priority, C, LM, ec, encode, prev,
                            signalBandwidth);
}

static void exp_rotation1(celt_norm* X, int len, int stride, opus_val16 c, opus_val16 s) {
  const opus_val16 ms = -s;
  const auto rotate = [&](int i) {
    const celt_norm x1 = X[i];
    const celt_norm x2 = X[i + stride];
    X[i + stride] = c * x2 + s * x1;
    X[i] = c * x1 + ms * x2;
  };
  for (int i = 0; i < len - stride; ++i) {
    rotate(i);
  }
  for (int i = len - 2 * stride - 1; i >= 0; --i) {
    rotate(i);
  }
}

static void exp_rotation1_stride1(celt_norm* X, int len, opus_val16 c, opus_val16 s) {
  const opus_val16 ms = -s;
  for (int i = 0; i < len - 1; ++i) {
    const celt_norm x1 = X[i];
    const celt_norm x2 = X[i + 1];
    X[i + 1] = c * x2 + s * x1;
    X[i] = c * x1 + ms * x2;
  }
  for (int i = len - 3; i >= 0; --i) {
    const celt_norm x1 = X[i];
    const celt_norm x2 = X[i + 1];
    X[i + 1] = c * x2 + s * x1;
    X[i] = c * x1 + ms * x2;
  }
}

static void exp_rotation(celt_norm* X, int len, int dir, int stride, int K, int spread) {
  constexpr std::array<opus_uint8, 3> SPREAD_FACTOR{15, 10, 5};
  if (2 * K >= len || spread == 0) {
    return;
  }
  const int factor = SPREAD_FACTOR[spread - 1];
  const auto gain = static_cast<opus_val16>(static_cast<opus_val32>(len) / static_cast<opus_val32>(len + factor * K));
  const auto theta = static_cast<opus_val16>(.5f * gain * gain);
  const auto angle = (.5f * 3.1415926535897931) * theta;
  const auto c = static_cast<opus_val16>(std::cos(angle));
  const auto s = static_cast<opus_val16>(std::sin(angle));
  int stride2 = 0;
  if (len >= 8 * stride) {
    stride2 = 1;
    for (; (stride2 * stride2 + stride2) * stride + (stride >> 2) < len; ++stride2) {}
  }
  len = celt_udiv(len, stride);
  for (int i = 0; i < stride; ++i) {
    if (dir < 0) {
      if (stride2) {
        exp_rotation1(X + i * len, len, stride2, s, c);
      }
      exp_rotation1_stride1(X + i * len, len, c, s);
    } else {
      exp_rotation1_stride1(X + i * len, len, c, -s);
      if (stride2) {
        exp_rotation1(X + i * len, len, stride2, s, -c);
      }
    }
  }
}

static unsigned normalise_residual_and_extract_collapse_mask(const opus_int16* iy, celt_norm* X, int N, int B, opus_val32 Ryy,
                                                             opus_val32 gain) {
  const opus_val32 g = (1.f / std::sqrt(Ryy)) * gain;
  if (B <= 1) {
    for (int index = 0; index < N; ++index) {
      X[index] = static_cast<opus_val32>(iy[index]) * g;
    }
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
  for (int index = B * N0; index < N; ++index) {
    X[index] = static_cast<opus_val32>(iy[index]) * g;
  }
  return collapse_mask;
}

struct celt_pvq_quant_result {
  unsigned collapse_mask;
  opus_uint32 index;
};

static auto op_pvq_search_c(celt_norm* X, int K, int N, int B) -> celt_pvq_quant_result {
  std::array<int, celt_max_band_samples> iy;
  std::array<celt_norm, celt_max_band_samples> y;
  for (int j = 0; j < N; ++j) {
    iy[j] = X[j] < 0;
    X[j] = std::abs(X[j]);
  }
  opus_val32 xy = 0;
  opus_val16 yy = 0;
  int pulsesLeft = K;
  if (K > (N >> 1)) {
    opus_val32 sum = 0;
    for (int idx = 0; idx < N; ++idx) {
      sum += X[idx];
    }
    if (!(sum > 1e-15f && sum < 64)) {
      X[0] = (1.f);
      zero_n_items(X + 1, static_cast<std::size_t>(N - 1));
      sum = (1.f);
    }
    const opus_val16 rcp = (K + 0.8f) * (1.f / sum);
    for (int j = 0; j < N; ++j) {
      const int pulse_int = static_cast<int>(std::floor(rcp * X[j]));
      const auto pulse = static_cast<celt_norm>(pulse_int);
      iy[j] = (pulse_int << 1) | (iy[j] & 1);
      yy += static_cast<opus_val32>(pulse) * static_cast<opus_val32>(pulse);
      xy += static_cast<opus_val32>(X[j]) * static_cast<opus_val32>(pulse);
      y[j] = 2 * pulse;
      pulsesLeft -= pulse_int;
    }
  } else {
    zero_n_items(y.data(), static_cast<std::size_t>(N));
  }
  if (pulsesLeft > N + 3) {
    const auto tmp = static_cast<opus_val16>(pulsesLeft);
    yy += static_cast<opus_val32>(tmp) * static_cast<opus_val32>(tmp);
    yy += static_cast<opus_val32>(tmp) * static_cast<opus_val32>(y[0]);
    y[0] += 2 * pulsesLeft;
    iy[0] += 2 * pulsesLeft;
    pulsesLeft = 0;
  }
  for (int i = 0; i < pulsesLeft; ++i) {
    int best_id = 0;
    yy += 1;
    opus_val16 Rxy = xy + X[0];
    opus_val16 Ryy = yy + y[0];
    Rxy *= Rxy;
    opus_val16 best_den = Ryy;
    opus_val32 best_num = Rxy;
    for (int candidate = 1; candidate < N; ++candidate) {
      Rxy = xy + X[candidate];
      Ryy = yy + y[candidate];
      Rxy *= Rxy;
      if (static_cast<opus_val32>(best_den) * static_cast<opus_val32>(Rxy) >
          static_cast<opus_val32>(Ryy) * static_cast<opus_val32>(best_num)) {
        best_den = Ryy;
        best_num = Rxy;
        best_id = candidate;
      }
    }
    xy += X[best_id];
    yy += y[best_id];
    y[best_id] += 2;
    iy[best_id] += 2;
  }
  auto collapse_mask = B <= 1 ? 1U : 0U;
  const int N0 = B <= 1 ? 0 : celt_udiv(N, B);
  const int collapse_limit = B <= 1 ? 0 : B * N0;
  int j = N - 1;
  int packed_pulse = iy[j];
  int k = packed_pulse >> 1;
  opus_uint32 index = (k != 0 && (packed_pulse & 1) != 0) ? 1U : 0U;
  if (j < collapse_limit && k != 0) {
    collapse_mask |= 1U << celt_udiv(j, N0);
  }
  for (; j-- > 0;) {
    packed_pulse = iy[j];
    const int pulse = packed_pulse >> 1;
    index += celt_pvq_u_entry(N - j, k);
    k += pulse;
    if (pulse != 0) {
      if (j < collapse_limit) {
        collapse_mask |= 1U << celt_udiv(j, N0);
      }
      if ((packed_pulse & 1) != 0) {
        index += celt_pvq_u_entry(N - j, k + 1);
      }
    }
  }
  return {collapse_mask, index};
}

static unsigned alg_quant(celt_norm* X, int N, int K, int spread, int B, ec_enc* enc) {
  exp_rotation(X, N, 1, B, K, spread);
  if (K == 1) {
    int best_id = 0;
    for (int candidate = 1; candidate < N; ++candidate) {
      if (std::abs(X[candidate]) > std::abs(X[best_id])) {
        best_id = candidate;
      }
    }
    const auto encoded_pulse = static_cast<opus_uint32>(X[best_id] < 0 ? (N << 1) - 1 - best_id : best_id);
    ec_enc_uint(enc, encoded_pulse, static_cast<opus_uint32>(N << 1));
    if (B <= 1) {
      return 1;
    }
    const int N0 = celt_udiv(N, B);
    return best_id < B * N0 ? 1U << celt_udiv(best_id, N0) : 0;
  }
  const auto quant = op_pvq_search_c(X, K, N, B);
  ec_enc_uint(enc, quant.index, celt_pvq_v_entry(N, K));
  return quant.collapse_mask;
}

static unsigned alg_unquant(celt_norm* X, int N, int K, int spread, int B, ec_dec* dec, opus_val32 gain, opus_int16* iy) {
  if (K == 1) {
    const auto encoded = ec_dec_uint(dec, static_cast<opus_uint32>(N << 1));
    const bool negative = encoded >= static_cast<opus_uint32>(N);
    const int position = negative ? (N << 1) - 1 - static_cast<int>(encoded) : static_cast<int>(encoded);
    zero_n_items(X, static_cast<std::size_t>(N));
    X[position] = negative ? -gain : gain;
    unsigned collapse_mask = B <= 1 ? 1 : 0;
    if (B > 1) {
      const int block_size = celt_udiv(N, B);
      if (position < B * block_size) {
        collapse_mask = 1U << celt_udiv(position, block_size);
      }
    }
    exp_rotation(X, N, -1, B, K, spread);
    return collapse_mask;
  }
  if (K <= 4) {
    std::array<int, 4> positions;
    std::array<int, 4> values;
    int count = 0;
    auto add_pulse = [&](int position, int value) noexcept {
      if (value != 0) {
        positions[static_cast<std::size_t>(count)] = position;
        values[static_cast<std::size_t>(count++)] = value;
      }
    };
    auto writer = [&](int position, opus_int16 value) noexcept {
      add_pulse(position, value);
    };
    const opus_val32 Ryy = celt_pvq_decode_unrank(N, K, dec, writer);
    zero_n_items(X, static_cast<std::size_t>(N));
    const opus_val32 g = (1.f / std::sqrt(Ryy)) * gain;
    unsigned collapse_mask = B <= 1 ? 1 : 0;
    const int block_size = B <= 1 ? N : celt_udiv(N, B);
    const int active_length = B * block_size;
    for (int index = 0; index < count; ++index) {
      const int position = positions[static_cast<std::size_t>(index)];
      X[position] = static_cast<opus_val32>(values[static_cast<std::size_t>(index)]) * g;
      if (B > 1 && position < active_length) {
        collapse_mask |= 1U << celt_udiv(position, block_size);
      }
    }
    exp_rotation(X, N, -1, B, K, spread);
    return collapse_mask;
  }
  auto* out = iy;
  auto writer = [&](int, opus_int16 value) noexcept {
    *out++ = value;
  };
  const opus_val32 Ryy = celt_pvq_decode_unrank(N, K, dec, writer);
  const unsigned collapse_mask = normalise_residual_and_extract_collapse_mask(iy, X, N, B, Ryy, gain);
  exp_rotation(X, N, -1, B, K, spread);
  return collapse_mask;
}

static void renormalise_vector(celt_norm* X, int N, opus_val32 gain) {
  const opus_val32 E = 1e-15f + celt_inner_prod_c(X, X, N);
  const opus_val16 g = (1.f / std::sqrt(E)) * gain;
  for (int i = 0; i < N; ++i) {
    X[i] = g * X[i];
  }
}

static opus_int32 stereo_itheta(const celt_norm* X, const celt_norm* Y, int stereo, int N) {
  opus_val32 Emid = 0;
  opus_val32 Eside = 0;
  if (stereo) {
    for (int i = 0; i < N; i++) {
      const celt_norm m = (((X[i]) + (Y[i])));
      const celt_norm s = (((X[i]) - (Y[i])));
      Emid = ((Emid) + static_cast<opus_val32>(m) * static_cast<opus_val32>(m));
      Eside = ((Eside) + static_cast<opus_val32>(s) * static_cast<opus_val32>(s));
    }
  } else {
    Emid += celt_inner_prod_c(X, X, N);
    Eside += celt_inner_prod_c(Y, Y, N);
  }
  const opus_val32 mid = std::sqrt(Emid);
  const opus_val32 side = std::sqrt(Eside);
  const auto angle = side * side + mid * mid < 1e-18f ? 0.0f : side < mid ? celt_atan_norm(side / mid) : 1.0f - celt_atan_norm(mid / side);
  return static_cast<int>(std::floor(.5f + 65536.f * 16384 * angle));
}

constexpr auto silk_max_fs_kHz = 16, silk_max_frame_length = 20 * silk_max_fs_kHz, silk_max_subfr_length = 5 * silk_max_fs_kHz,
               silk_max_ltp_mem_length = 20 * silk_max_fs_kHz, silk_max_ltp_buffer_length = silk_max_ltp_mem_length + silk_max_frame_length,
               silk_max_delayed_decision_states = 4;
constexpr auto silk_max_resampler_batch_size = 480, silk_max_resampler_fir_order = 36;
constexpr auto silk_max_resampler_reconfig_samples = 45 * silk_max_fs_kHz;
constexpr auto silk_max_resampler_api_reconfig_samples = 45 * 48;
static void silk_CNG_Reset(silk_decoder_state* psDec) {
  if (psDec->sCNG == nullptr) {
    return;
  }
  auto* psCNG = psDec->sCNG;
  const int NLSF_step_Q15 = (static_cast<opus_int32>((0x7FFF) / (psDec->LPC_order + 1)));
  int NLSF_acc_Q15 = 0;
  for (int i = 0; i < psDec->LPC_order; i++) {
    NLSF_acc_Q15 += NLSF_step_Q15;
    psCNG->CNG_smth_NLSF_Q15[i] = NLSF_acc_Q15;
  }
  psCNG->CNG_smth_Gain_Q16 = 0;
  psCNG->rand_seed = 3176576;
}

static void silk_CNG(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl, opus_int16 frame[], int length) {
  opus_int16 A_Q12[16];
  const bool updates_cng_history = psDec->lossCnt == 0 && psDec->prevSignalType == 0;
  const bool generates_cng = psDec->lossCnt != 0;
  silk_CNG_struct* psCNG = (updates_cng_history || generates_cng) ? silk_ensure_cng(psDec) : psDec->sCNG;
  if (psCNG == nullptr) {
    return;
  }
  if (updates_cng_history) {
    for (int i = 0; i < psDec->LPC_order; i++) {
      psCNG->CNG_smth_NLSF_Q15[i] = silk_mla_wb(psCNG->CNG_smth_NLSF_Q15[i], psDec->prevNLSF_Q15[i] - psCNG->CNG_smth_NLSF_Q15[i], 16348);
    }
    opus_int32 max_Gain_Q16 = 0;
    int subfr = 0;
    for (int i = 0; i < psDec->nb_subfr; i++) {
      if (psDecCtrl->Gains_Q16[i] > max_Gain_Q16) {
        max_Gain_Q16 = psDecCtrl->Gains_Q16[i];
        subfr = i;
      }
    }
    move_n_bytes(psCNG->CNG_exc_buf_Q14, static_cast<std::size_t>((psDec->nb_subfr - 1) * psDec->subfr_length * sizeof(opus_int32)),
                 &psCNG->CNG_exc_buf_Q14[psDec->subfr_length]);
    copy_n_bytes(&psDec->exc_Q14[subfr * psDec->subfr_length], static_cast<std::size_t>(psDec->subfr_length * sizeof(opus_int32)),
                 psCNG->CNG_exc_buf_Q14);
    for (int i = 0; i < psDec->nb_subfr; i++) {
      psCNG->CNG_smth_Gain_Q16 = silk_mla_wb(psCNG->CNG_smth_Gain_Q16, psDecCtrl->Gains_Q16[i] - psCNG->CNG_smth_Gain_Q16, 4634);
      if (multiply_q16(psCNG->CNG_smth_Gain_Q16, 46396) > psDecCtrl->Gains_Q16[i]) {
        psCNG->CNG_smth_Gain_Q16 = psDecCtrl->Gains_Q16[i];
      }
    }
  }
  if (generates_cng) {
    opus_int32 CNG_sig_Q14[silk_max_frame_length + 16]{};
    opus_int32 gain_Q16 = multiply_q16(psDec->sPLC.randScale_Q14, psDec->sPLC.prevGain_Q16[1]);
    if (gain_Q16 >= (1 << 21) || psCNG->CNG_smth_Gain_Q16 > (1 << 23)) {
      const auto gain = gain_Q16 >> 16;
      const auto cng_gain = psCNG->CNG_smth_Gain_Q16 >> 16;
      gain_Q16 = wrap_subtract(cng_gain * cng_gain, wrap_shift_left(gain * gain, 5));
      gain_Q16 = wrap_shift_left(silk_SQRT_APPROX(gain_Q16), 16);
    } else {
      gain_Q16 = wrap_subtract(multiply_q16(psCNG->CNG_smth_Gain_Q16, psCNG->CNG_smth_Gain_Q16),
                               wrap_shift_left(multiply_q16(gain_Q16, gain_Q16), 5));
      gain_Q16 = wrap_shift_left(silk_SQRT_APPROX(gain_Q16), 8);
    }
    const opus_int32 gain_Q10 = ((gain_Q16) >> (6));
    int exc_mask = 255;
    for (; exc_mask > length; exc_mask >>= 1) {}
    for (int i = 0; i < length; ++i) {
      psCNG->rand_seed = silk_next_rand_seed(psCNG->rand_seed);
      CNG_sig_Q14[16 + i] = psCNG->CNG_exc_buf_Q14[(psCNG->rand_seed >> 24) & exc_mask];
    }
    silk_NLSF2A(A_Q12, psCNG->CNG_smth_NLSF_Q15, psDec->LPC_order);
    copy_n_bytes(psCNG->CNG_synth_state, static_cast<std::size_t>(16 * sizeof(opus_int32)), CNG_sig_Q14);
    for (int i = 0; i < length; ++i) {
      const opus_int32 LPC_pred_Q10 = silk_lpc_prediction_q10(CNG_sig_Q14 + 16 + i, A_Q12, psDec->LPC_order);
      CNG_sig_Q14[16 + i] = saturating_add_int32(CNG_sig_Q14[16 + i], saturating_left_shift<4>(LPC_pred_Q10));
      const auto cng_sample = scale_and_saturate_q14<8>(CNG_sig_Q14[16 + i], gain_Q10);
      frame[i] = saturate_int16_from_int32(static_cast<opus_int32>(frame[i]) + cng_sample);
    }
    copy_n_bytes(&CNG_sig_Q14[length], static_cast<std::size_t>(16 * sizeof(opus_int32)), psCNG->CNG_synth_state);
  } else {
    zero_n_bytes(psCNG->CNG_synth_state, static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int32)));
  }
}

template <bool Encode, typename Coder, typename PulseSpan>
void silk_process_signs(Coder* coder, PulseSpan pulses, const int signalType, const int quantOffsetType, std::span<const int> sum_pulses) {
  opus_uint8 icdf[2] = {0, 0};
  const auto* icdf_ptr = silk_sign_iCDF.data() + 7 * (quantOffsetType + (signalType << 1));
  for (auto block_index = std::size_t{}; block_index < sum_pulses.size(); ++block_index) {
    const auto pulse_count = sum_pulses[block_index];
    if (pulse_count <= 0) {
      continue;
    }
    icdf[0] = icdf_ptr[std::min(pulse_count & 0x1F, 6)];
    auto pulse_block = pulses.subspan(block_index * 16, 16);
    for (auto pulse_index = std::size_t{}; pulse_index < pulse_block.size(); ++pulse_index) {
      auto& pulse = pulse_block[pulse_index];
      if constexpr (Encode) {
        if (pulse != 0) {
          ec_enc_icdf(coder, (pulse >> 15) + 1, icdf, 8);
        }
      } else if (pulse > 0) {
        pulse *= static_cast<opus_int16>((ec_dec_icdf(coder, icdf, 8) << 1) - 1);
      }
    }
  }
}

static void silk_reset_decoder(silk_decoder_state* psDec) {
  silk_release_cng(psDec);
  *psDec = {};
  psDec->first_frame_after_reset = 1;
  psDec->prev_gain_Q16 = 65536;
  silk_PLC_Reset(psDec);
}

static void silk_decode_core(silk_decoder_state& state, silk_decoder_control& control, opus_int16 xq[],
                             const opus_int16 pulses[((5 * 4) * 16)]) {
  int lag = 0;
  std::array<opus_int16, silk_max_ltp_mem_length> ltp_storage;
  std::array<opus_int32, silk_max_ltp_buffer_length> ltp_q15_storage;
  std::array<opus_int32, silk_max_subfr_length> residual_storage;
  std::array<opus_int32, silk_max_subfr_length + 16> lpc_storage;
  auto* sLTP = ltp_storage.data();
  auto* sLTP_Q15 = ltp_q15_storage.data();
  auto* res_Q14 = residual_storage.data();
  auto* sLPC_Q14 = lpc_storage.data();
  const opus_int32 offset_Q10 = silk_Quantization_Offsets_Q10[state.indices.signalType >> 1][state.indices.quantOffsetType];
  const bool NLSF_interpolation_flag = state.indices.NLSFInterpCoef_Q2 < 1 << 2;
  auto rand_seed = static_cast<opus_int32>(state.indices.Seed);
  const auto decode_excitation = [&](int index) noexcept {
    rand_seed = silk_next_rand_seed(rand_seed);
    auto excitation = wrap_shift_left(pulses[index], 14);
    if (excitation > 0) {
      excitation -= 80 << 4;
    } else if (excitation < 0) {
      excitation += 80 << 4;
    }
    excitation += offset_Q10 << 4;
    if (rand_seed < 0) {
      excitation = -excitation;
    }
    rand_seed = wrap_add(rand_seed, pulses[index]);
    return state.exc_Q14[index] = excitation;
  };
  copy_n_bytes(state.sLPC_Q14_buf, static_cast<std::size_t>(16 * sizeof(opus_int32)), sLPC_Q14);
  auto* pxq = xq;
  int sLTP_buf_idx = state.ltp_mem_length;
  for (int k = 0; k < state.nb_subfr; ++k) {
    const int subframe_offset = k * state.subfr_length;
    auto* pexc_Q14 = state.exc_Q14 + subframe_offset;
    auto* pres_Q14 = res_Q14;
    const auto* A_Q12 = control.PredCoef_Q12[static_cast<std::size_t>(k >> 1)];
    auto* B_Q14 = control.LTPCoef_Q14 + k * 5;
    int signalType = state.indices.signalType;
    const opus_int32 Gain_Q10 = ((control.Gains_Q16[k]) >> (6));
    auto inv_gain_Q31 = silk_INVERSE32_varQ(control.Gains_Q16[k], 47);
    opus_int32 gain_adj_Q16;
    if (control.Gains_Q16[k] != state.prev_gain_Q16) {
      gain_adj_Q16 = silk_DIV32_varQ(state.prev_gain_Q16, control.Gains_Q16[k], 16);
      for (int state_index = 0; state_index < 16; ++state_index) {
        sLPC_Q14[state_index] = multiply_q16(gain_adj_Q16, sLPC_Q14[state_index]);
      }
    } else {
      gain_adj_Q16 = opus_int32{1} << 16;
    }
    state.prev_gain_Q16 = control.Gains_Q16[k];
    if (state.lossCnt && state.prevSignalType == 2 && state.indices.signalType != 2 && k < 4 / 2) {
      zero_n_items(B_Q14, 5);
      B_Q14[5 / 2] = 1 << 12;
      signalType = 2;
      control.pitchL[k] = state.lagPrev;
    }
    if (signalType == 2) {
      lag = control.pitchL[k];
      if (k == 0 || (k == 2 && NLSF_interpolation_flag)) {
        const int start_idx = state.ltp_mem_length - lag - state.LPC_order - 5 / 2;
        if (k == 2) {
          copy_n_bytes(xq, static_cast<std::size_t>(2 * state.subfr_length * sizeof(opus_int16)), &state.outBuf[state.ltp_mem_length]);
        }
        silk_LPC_analysis_filter(&sLTP[start_idx], &state.outBuf[start_idx + k * state.subfr_length], A_Q12,
                                 state.ltp_mem_length - start_idx, state.LPC_order);
        if (k == 0) {
          inv_gain_Q31 = wrap_shift_left(silk_mul_wb(inv_gain_Q31, control.LTP_scale_Q14), 2);
        }
        for (int i = 0; i < lag + 5 / 2; ++i) {
          sLTP_Q15[sLTP_buf_idx - i - 1] = silk_mul_wb(inv_gain_Q31, sLTP[state.ltp_mem_length - i - 1]);
        }
      } else {
        if (gain_adj_Q16 != opus_int32{1} << 16) {
          for (int i = 0; i < lag + 5 / 2; ++i) {
            sLTP_Q15[sLTP_buf_idx - i - 1] = multiply_q16(gain_adj_Q16, sLTP_Q15[sLTP_buf_idx - i - 1]);
          }
        }
      }
      auto* pred_lag_ptr = &sLTP_Q15[sLTP_buf_idx - lag + 5 / 2];
      for (int i = 0; i < state.subfr_length; ++i) {
        const opus_int32 LTP_pred_Q13 = silk_ltp_prediction_5tap(pred_lag_ptr, B_Q14);
        ++pred_lag_ptr;
        pres_Q14[i] = wrap_add(decode_excitation(subframe_offset + i), wrap_shift_left(LTP_pred_Q13, 1));
        sLTP_Q15[sLTP_buf_idx] = wrap_shift_left(pres_Q14[i], 1);
        ++sLTP_buf_idx;
      }
    } else {
      for (int i = 0; i < state.subfr_length; ++i) {
        decode_excitation(subframe_offset + i);
      }
      pres_Q14 = pexc_Q14;
    }
    if (state.LPC_order == 16) {
      silk_decode_lpc_subframe_q14<16>(sLPC_Q14, pres_Q14, pxq, state.subfr_length, A_Q12, Gain_Q10);
    } else {
      silk_decode_lpc_subframe_q14<10>(sLPC_Q14, pres_Q14, pxq, state.subfr_length, A_Q12, Gain_Q10);
    }
    copy_n_bytes(&sLPC_Q14[state.subfr_length], static_cast<std::size_t>(16 * sizeof(opus_int32)), sLPC_Q14);
    pxq += state.subfr_length;
  }
  copy_n_bytes(sLPC_Q14, static_cast<std::size_t>(16 * sizeof(opus_int32)), state.sLPC_Q14_buf);
}

static void silk_decode_frame(silk_decoder_state* psDec, ec_dec* psRangeDec, opus_int16 pOut[], opus_int32* pN, int lostFlag,
                              int condCoding) {
  const int L = psDec->frame_length;
  silk_decoder_control psDecCtrl;
  psDecCtrl.LTP_scale_Q14 = 0;
  if (lostFlag == 0 || (lostFlag == 2 && psDec->LBRR_flags[psDec->nFramesDecoded] == 1)) {
    std::array<opus_int16, silk_max_frame_length> pulse_storage;
    auto pulses = std::span<opus_int16>{pulse_storage.data(), static_cast<std::size_t>((L + 16 - 1) & ~(16 - 1))};
    silk_decode_indices(psDec, psRangeDec, psDec->nFramesDecoded, lostFlag, condCoding);
    silk_process_pulses<false>(psRangeDec, pulses, psDec->indices.signalType, psDec->indices.quantOffsetType, psDec->frame_length);
    silk_decode_parameters(*psDec, psDecCtrl, condCoding);
    silk_decode_core(*psDec, psDecCtrl, pOut, pulses.data());
    silk_PLC(psDec, &psDecCtrl, std::span<opus_int16>{pOut, static_cast<std::size_t>(L)}, 0);
    psDec->lossCnt = 0;
    psDec->prevSignalType = psDec->indices.signalType;
    psDec->first_frame_after_reset = 0;
  } else {
    silk_PLC(psDec, &psDecCtrl, std::span<opus_int16>{pOut, static_cast<std::size_t>(L)}, 1);
  }
  const int move_length = psDec->ltp_mem_length - psDec->frame_length;
  move_n_bytes(&psDec->outBuf[psDec->frame_length], static_cast<std::size_t>(move_length * sizeof(opus_int16)), psDec->outBuf);
  copy_n_bytes(pOut, static_cast<std::size_t>(psDec->frame_length * sizeof(opus_int16)), &psDec->outBuf[move_length]);
  silk_CNG(psDec, &psDecCtrl, pOut, L);
  silk_PLC_glue_frames(psDec, std::span<opus_int16>{pOut, static_cast<std::size_t>(L)});
  psDec->lagPrev = psDecCtrl.pitchL[psDec->nb_subfr - 1];
  *pN = L;
}

static void silk_decode_parameters(silk_decoder_state& state, silk_decoder_control& control, int condCoding) {
  opus_int16 pNLSF_Q15[16], pNLSF0_Q15[16];
  silk_gains_dequant(control.Gains_Q16, state.indices.GainsIndices, &state.LastGainIndex, condCoding == 2, state.nb_subfr);
  silk_NLSF_decode(pNLSF_Q15, state.indices.NLSFIndices, state.psNLSF_CB);
  silk_NLSF2A(control.PredCoef_Q12[1], pNLSF_Q15, state.LPC_order);
  if (state.first_frame_after_reset == 1) {
    state.indices.NLSFInterpCoef_Q2 = 4;
  }
  if (state.indices.NLSFInterpCoef_Q2 < 4) {
    for (int i = 0; i < state.LPC_order; i++) {
      pNLSF0_Q15[i] = state.prevNLSF_Q15[i] + (state.indices.NLSFInterpCoef_Q2 * (pNLSF_Q15[i] - state.prevNLSF_Q15[i]) >> 2);
    }
    silk_NLSF2A(control.PredCoef_Q12[0], pNLSF0_Q15, state.LPC_order);
  } else {
    copy_n_bytes(control.PredCoef_Q12[1], static_cast<std::size_t>(state.LPC_order * sizeof(opus_int16)), control.PredCoef_Q12[0]);
  }
  copy_n_bytes(pNLSF_Q15, static_cast<std::size_t>(state.LPC_order * sizeof(opus_int16)), state.prevNLSF_Q15);
  if (state.lossCnt) {
    silk_bwexpander(control.PredCoef_Q12[0], static_cast<std::size_t>(state.LPC_order), 63570);
    silk_bwexpander(control.PredCoef_Q12[1], static_cast<std::size_t>(state.LPC_order), 63570);
  }
  if (state.indices.signalType == 2) {
    silk_decode_pitch(state.indices.lagIndex, state.indices.contourIndex, control.pitchL, state.fs_kHz, state.nb_subfr);
    const auto* cbk_ptr_Q7 = silk_LTP_codebook(state.indices.PERIndex).vq_q7.data();
    for (int k = 0; k < state.nb_subfr; k++) {
      const int index = state.indices.LTPIndex[k];
      for (int i = 0; i < 5; i++) {
        control.LTPCoef_Q14[static_cast<std::size_t>(k * 5 + i)] = wrap_shift_left(cbk_ptr_Q7[index * 5 + i], 7);
      }
    }
    control.LTP_scale_Q14 = silk_LTPScales_table_Q14[state.indices.LTP_scaleIndex];
  } else {
    zero_n_items(control.pitchL, static_cast<std::size_t>(state.nb_subfr));
    zero_n_items(control.LTPCoef_Q14, static_cast<std::size_t>(5 * state.nb_subfr));
    state.indices.PERIndex = 0;
    control.LTP_scale_Q14 = 0;
  }
}

template <bool Encode> [[nodiscard]] static inline int silk_index_symbol(ec_ctx* coder, int value, const opus_uint8* icdf) {
  if constexpr (Encode) {
    ec_enc_icdf(coder, value, icdf, 8);
    return value;
  }
  return ec_dec_icdf(coder, icdf, 8);
}

template <bool Encode, typename State>
static void silk_process_indices(State* state, SideInfoIndices& indices, ec_ctx* coder, int condCoding) {
  opus_int16 ec_ix[16];
  opus_uint8 pred_Q8[16];
  const int nb_subfr = std::min(state->nb_subfr, 4);
  if (condCoding == 2) {
    indices.GainsIndices[0] =
        static_cast<opus_int8>(silk_index_symbol<Encode>(coder, indices.GainsIndices[0], silk_delta_gain_iCDF.data()));
  } else {
    const auto gain_high = silk_index_symbol<Encode>(coder, indices.GainsIndices[0] >> 3, silk_gain_iCDF[indices.signalType].data());
    const auto gain_low = silk_index_symbol<Encode>(coder, indices.GainsIndices[0] & 7, silk_uniform8_iCDF.data());
    indices.GainsIndices[0] = static_cast<opus_int8>((gain_high << 3) + gain_low);
  }
  for (int i = 1; i < nb_subfr; ++i) {
    indices.GainsIndices[i] =
        static_cast<opus_int8>(silk_index_symbol<Encode>(coder, indices.GainsIndices[i], silk_delta_gain_iCDF.data()));
  }
  indices.NLSFIndices[0] = static_cast<opus_int8>(
      silk_index_symbol<Encode>(coder, indices.NLSFIndices[0], silk_nlsf_cb1_icdf(state->psNLSF_CB, indices.signalType)));
  silk_NLSF_unpack(ec_ix, pred_Q8, state->psNLSF_CB, indices.NLSFIndices[0]);
  for (int i = 0; i < state->psNLSF_CB->order; ++i) {
    const int index = indices.NLSFIndices[i + 1];
    int base = silk_index_symbol<Encode>(coder, clamp_value(index + 4, 0, 8), &state->psNLSF_CB->ec_iCDF[ec_ix[i]]);
    if (base == 0) {
      base -= silk_index_symbol<Encode>(coder, -index - 4, silk_NLSF_EXT_iCDF.data());
    } else if (base == 8) {
      base += silk_index_symbol<Encode>(coder, index - 4, silk_NLSF_EXT_iCDF.data());
    }
    indices.NLSFIndices[i + 1] = static_cast<opus_int8>(base - 4);
  }
  if (state->nb_subfr == 4) {
    indices.NLSFInterpCoef_Q2 =
        static_cast<opus_uint8>(silk_index_symbol<Encode>(coder, indices.NLSFInterpCoef_Q2, silk_NLSF_interpolation_factor_iCDF.data()));
  } else {
    indices.NLSFInterpCoef_Q2 = 4;
  }
  if (indices.signalType == 2) {
    bool absolute_lag = true;
    if (condCoding == 2 && state->ec_prevSignalType == 2) {
      int delta_lag = 0;
      if constexpr (Encode) {
        delta_lag = indices.lagIndex - state->ec_prevLagIndex;
        delta_lag = delta_lag < -8 || delta_lag > 11 ? 0 : delta_lag + 9;
      }
      delta_lag = silk_index_symbol<Encode>(coder, delta_lag, silk_pitch_delta_iCDF.data());
      if (delta_lag > 0) {
        if constexpr (!Encode) {
          indices.lagIndex = static_cast<opus_int16>(state->ec_prevLagIndex + delta_lag - 9);
        }
        absolute_lag = false;
      }
    }
    if (absolute_lag) {
      const int divisor = state->fs_kHz >> 1;
      const auto high = silk_index_symbol<Encode>(coder, indices.lagIndex / divisor, silk_pitch_lag_iCDF.data());
      const auto low =
          silk_index_symbol<Encode>(coder, indices.lagIndex - high * divisor, silk_pitch_lag_low_bits_icdf(state->fs_kHz).data());
      indices.lagIndex = static_cast<opus_int16>(high * divisor + low);
    }
    state->ec_prevLagIndex = indices.lagIndex;
    indices.contourIndex = static_cast<opus_uint8>(
        silk_index_symbol<Encode>(coder, indices.contourIndex, silk_pitch_contour_icdf(state->fs_kHz, state->nb_subfr).data()));
    indices.PERIndex = static_cast<opus_uint8>(silk_index_symbol<Encode>(coder, indices.PERIndex, silk_LTP_per_index_iCDF.data()));
    for (int k = 0; k < nb_subfr; ++k) {
      indices.LTPIndex[k] = static_cast<opus_uint8>(
          silk_index_symbol<Encode>(coder, indices.LTPIndex[k], silk_LTP_codebook(indices.PERIndex).gain_icdf.data()));
    }
    if (condCoding == 0) {
      indices.LTP_scaleIndex = static_cast<opus_uint8>(silk_index_symbol<Encode>(coder, indices.LTP_scaleIndex, silk_LTPscale_iCDF.data()));
    } else {
      indices.LTP_scaleIndex = 0;
    }
  }
  state->ec_prevSignalType = indices.signalType;
  indices.Seed = static_cast<opus_uint8>(silk_index_symbol<Encode>(coder, indices.Seed, silk_uniform4_iCDF.data()));
}

static void silk_decode_indices(silk_decoder_state* psDec, ec_dec* psRangeDec, int FrameIndex, int decode_LBRR, int condCoding) {
  const bool vad = decode_LBRR || psDec->VAD_flags[FrameIndex];
  const int type_offset =
      ec_dec_icdf(psRangeDec, vad ? silk_type_offset_VAD_iCDF.data() : silk_type_offset_no_VAD_iCDF.data(), 8) + 2 * vad;
  psDec->indices.signalType = static_cast<opus_uint8>(type_offset >> 1);
  psDec->indices.quantOffsetType = static_cast<opus_uint8>(type_offset & 1);
  silk_process_indices<false>(psDec, psDec->indices, psRangeDec, condCoding);
}

static auto silk_combine_pulses(std::span<int> combined, std::span<const int> input, int max_pulses) noexcept -> bool {
  for (std::size_t index = 0; index < combined.size(); ++index) {
    const auto sum = input[2 * index] + input[2 * index + 1];
    if (sum > max_pulses) {
      return true;
    }
    combined[index] = sum;
  }
  return false;
}

template <bool Encode, typename Pulse>
static void silk_process_pulses(ec_ctx* coder, std::span<Pulse> pulses, int signal_type, int quant_offset_type, int frame_length) {
  const int block_count = (frame_length + 15) >> 4;
  std::array<int, silk_max_frame_length / 16> pulse_sums;
  std::array<int, silk_max_frame_length / 16> shifts;
  std::array<int, Encode ? silk_max_frame_length : 1> magnitudes;
  int rate_level = 0;

  if constexpr (Encode) {
    if ((frame_length & 15) != 0) {
      zero_n_items(pulses.data() + frame_length, 16);
    }
    for (int index = 0; index < block_count * 16; ++index) {
      magnitudes[index] = std::abs(static_cast<int>(pulses[index]));
    }
    std::array<int, 8> combined;
    for (int block_index = 0; block_index < block_count; ++block_index) {
      shifts[block_index] = 0;
      auto block_magnitudes = std::span<int, 16>{magnitudes.data() + block_index * 16, 16};
      bool scale_down;
      do {
        scale_down = silk_combine_pulses(std::span{combined}.first<8>(), block_magnitudes, silk_max_pulses_table[0]) ||
                     silk_combine_pulses(std::span{combined}.first<4>(), std::span{combined}.first<8>(), silk_max_pulses_table[1]) ||
                     silk_combine_pulses(std::span{combined}.first<2>(), std::span{combined}.first<4>(), silk_max_pulses_table[2]) ||
                     silk_combine_pulses(std::span<int>{pulse_sums.data() + block_index, 1}, std::span{combined}.first<2>(),
                                         silk_max_pulses_table[3]);
        if (scale_down) {
          ++shifts[block_index];
          for (auto& magnitude : block_magnitudes) {
            magnitude >>= 1;
          }
        }
      } while (scale_down);
    }
    auto minimum_bits = opus_int32_max;
    for (int candidate = 0; candidate < 9; ++candidate) {
      const auto* bit_cost = silk_pulses_per_block_BITS_Q5[candidate].data();
      auto bits = static_cast<opus_int32>(silk_rate_levels_BITS_Q5[signal_type >> 1][candidate]);
      for (int block_index = 0; block_index < block_count && bits < minimum_bits; ++block_index) {
        bits += bit_cost[shifts[block_index] > 0 ? 17 : pulse_sums[block_index]];
      }
      if (bits < minimum_bits) {
        minimum_bits = bits;
        rate_level = candidate;
      }
    }
    ec_enc_icdf(coder, rate_level, silk_rate_levels_iCDF[signal_type >> 1].data(), 8);
    const auto* block_icdf = silk_pulses_per_block_iCDF[rate_level].data();
    for (int block_index = 0; block_index < block_count; ++block_index) {
      if (shifts[block_index] == 0) {
        ec_enc_icdf(coder, pulse_sums[block_index], block_icdf, 8);
        continue;
      }
      ec_enc_icdf(coder, 17, block_icdf, 8);
      for (int shift = 1; shift < shifts[block_index]; ++shift) {
        ec_enc_icdf(coder, 17, silk_pulses_per_block_iCDF[9].data(), 8);
      }
      ec_enc_icdf(coder, pulse_sums[block_index], silk_pulses_per_block_iCDF[9].data(), 8);
    }
  } else {
    rate_level = ec_dec_icdf(coder, silk_rate_levels_iCDF[signal_type >> 1].data(), 8);
    const auto* block_icdf = silk_pulses_per_block_iCDF[rate_level].data();
    for (int block_index = 0; block_index < block_count; ++block_index) {
      shifts[block_index] = 0;
      pulse_sums[block_index] = ec_dec_icdf(coder, block_icdf, 8);
      while (pulse_sums[block_index] == 17) {
        ++shifts[block_index];
        pulse_sums[block_index] = ec_dec_icdf(coder, silk_pulses_per_block_iCDF[9].data() + (shifts[block_index] == 10), 8);
      }
    }
  }

  for (int block_index = 0; block_index < block_count; ++block_index) {
    const int offset = block_index * 16;
    if (pulse_sums[block_index] > 0) {
      if constexpr (Encode) {
        silk_shell_code_node<true>(coder, std::span<const int>{magnitudes.data() + offset, 16}, pulse_sums[block_index]);
      } else {
        silk_shell_code_node<false>(coder, std::span<Pulse>{pulses.data() + offset, 16}, pulse_sums[block_index]);
      }
    } else if constexpr (!Encode) {
      zero_n_items(pulses.data() + offset, 16);
    }
  }

  for (int block_index = 0; block_index < block_count; ++block_index) {
    if (shifts[block_index] == 0) {
      continue;
    }
    auto block_pulses = pulses.subspan(static_cast<std::size_t>(block_index * 16), 16);
    if constexpr (Encode) {
      for (const auto pulse : block_pulses) {
        const auto magnitude = std::abs(static_cast<int>(pulse));
        for (int shift = shifts[block_index] - 1; shift >= 0; --shift) {
          ec_enc_icdf(coder, (magnitude >> shift) & 1, silk_lsb_iCDF.data(), 8);
        }
      }
    } else {
      for (auto& pulse : block_pulses) {
        auto magnitude = static_cast<int>(pulse);
        for (int shift = 0; shift < shifts[block_index]; ++shift) {
          magnitude = wrap_shift_left(magnitude, 1) + ec_dec_icdf(coder, silk_lsb_iCDF.data(), 8);
        }
        pulse = static_cast<Pulse>(magnitude);
      }
      pulse_sums[block_index] |= shifts[block_index] << 5;
    }
  }
  const auto sums = std::span<const int>{pulse_sums.data(), static_cast<std::size_t>(block_count)};
  silk_process_signs<Encode>(coder, pulses.first(static_cast<std::size_t>(block_count * 16)), signal_type, quant_offset_type, sums);
}

static void silk_decoder_set_fs(silk_decoder_state* psDec, int fs_kHz, opus_int32 fs_API_Hz) {
  const bool rate_changed = psDec->fs_kHz != fs_kHz;
  psDec->subfr_length = 5 * fs_kHz;
  const int frame_length = psDec->nb_subfr * psDec->subfr_length;
  if (rate_changed) {
    silk_resampler_init(&psDec->resampler_state, fs_kHz * 1000, fs_API_Hz, 0);
  }
  if (rate_changed || frame_length != psDec->frame_length) {
    if (rate_changed) {
      psDec->ltp_mem_length = 20 * fs_kHz;
      psDec->psNLSF_CB = silk_nlsf_codebook_for_fs(fs_kHz);
      psDec->LPC_order = psDec->psNLSF_CB->order;
      psDec->first_frame_after_reset = 1;
      psDec->lagPrev = 100;
      psDec->LastGainIndex = 10;
      psDec->prevSignalType = 0;
      zero_object(psDec->outBuf);
      zero_object(psDec->sLPC_Q14_buf);
    }
    psDec->fs_kHz = fs_kHz;
    psDec->frame_length = frame_length;
    if (rate_changed) {
      silk_CNG_Reset(psDec);
      silk_PLC_Reset(psDec);
    }
  }
}
struct silk_decoder {
  silk_decoder_state channel_state;
  silk_decoder_state* side_channel_state;
  stereo_dec_state sStereo;
  int nChannelsInternal, prev_decode_only_middle;
};
[[nodiscard]] static auto silk_ensure_side_channel(silk_decoder* decoder) noexcept -> silk_decoder_state* {
  if (decoder->side_channel_state == nullptr) {
    auto* side = static_cast<silk_decoder_state*>(std::malloc(sizeof(silk_decoder_state)));
    if (side == nullptr) {
      return nullptr;
    }
    side->sCNG = nullptr;
    silk_reset_decoder(side);
    decoder->side_channel_state = side;
  }
  return decoder->side_channel_state;
}

static void silk_release_side_channel(silk_decoder* decoder) noexcept {
  if (decoder != nullptr && decoder->side_channel_state != nullptr) {
    silk_release_cng(decoder->side_channel_state);
    std::free(decoder->side_channel_state);
    decoder->side_channel_state = nullptr;
  }
}

static void silk_destroy_decoder(void* decState) noexcept {
  if (decState == nullptr) {
    return;
  }
  auto* decoder = static_cast<silk_decoder*>(decState);
  silk_release_cng(&decoder->channel_state);
  silk_release_side_channel(decoder);
}

[[nodiscard]] constexpr auto silk_decoder_get_size() noexcept -> int {
  return static_cast<int>(sizeof(silk_decoder));
}

static void silk_ResetDecoder(void* decState) {
  auto* decoder = static_cast<silk_decoder*>(decState);
  silk_reset_decoder(&decoder->channel_state);
  silk_release_side_channel(decoder);
  zero_object(decoder->sStereo);
  decoder->prev_decode_only_middle = 0;
}

template <typename Sample>
static inline void silk_store_resampled_channel(const opus_int16* resampled, Sample* samplesOut, int output_samples, int api_channels,
                                                int channel) {
  if constexpr (std::same_as<Sample, opus_int16>) {
    for (int i = 0; i < output_samples; ++i) {
      samplesOut[channel + 2 * i] = resampled[i];
    }
  } else if (api_channels == 2) {
    for (int i = 0; i < output_samples; ++i) {
      samplesOut[channel + 2 * i] = resampled[i] * (1 / 32768.f);
    }
  } else {
    for (int i = 0; i < output_samples; ++i) {
      samplesOut[i] = resampled[i] * (1 / 32768.f);
    }
  }
}

static inline void silk_resample_and_store_channel(silk_decoder_state& state, std::span<opus_int16, OPUS_FRAME_SIZE_20MS> resampled,
                                                   const opus_int16* input, int input_samples, opus_res* samplesOut,
                                                   opus_int16* samplesOut16, int output_samples, int api_channels, int channel) {
  auto* resampled_output = samplesOut16 != nullptr && api_channels == 1 ? samplesOut16 : resampled.data();
  silk_resampler(&state.resampler_state, resampled_output, input, input_samples);
  if (resampled_output == samplesOut16) {
    return;
  }
  if (samplesOut16 != nullptr) {
    silk_store_resampled_channel(resampled.data(), samplesOut16, output_samples, api_channels, channel);
  } else {
    silk_store_resampled_channel(resampled.data(), samplesOut, output_samples, api_channels, channel);
  }
}

template <typename Sample> static inline void silk_duplicate_mono_to_right(Sample* samplesOut, int output_samples) {
  for (int i = 0; i < output_samples; ++i) {
    samplesOut[1 + 2 * i] = samplesOut[2 * i];
  }
}

int silk_Decode(void* decState, silk_DecControlStruct* decControl, int lostFlag, int newPacketFlag, ec_dec* psRangeDec,
                opus_res* samplesOut, opus_int16* samplesOut16, opus_int32* nSamplesOut) {
  int decode_only_middle = 0;
  opus_int32 nSamplesOutDec = 0;
  std::array<opus_int32, 2> MS_pred_Q13{};
  silk_decoder* psDec = static_cast<silk_decoder*>(decState);
  if ((decControl->nChannelsInternal == 2 || psDec->nChannelsInternal == 2 || decControl->nChannelsAPI == 2) &&
      silk_ensure_side_channel(psDec) == nullptr)
    return -1;
  auto& mid = psDec->channel_state;
  std::array<silk_decoder_state*, 2> channel_state{&mid, psDec->side_channel_state};
  int has_side, stereo_to_mono;
  if (newPacketFlag) {
    for (int n = 0; n < decControl->nChannelsInternal; ++n) {
      channel_state[n]->nFramesDecoded = 0;
    }
  }
  if (decControl->nChannelsInternal > psDec->nChannelsInternal && decControl->nChannelsInternal == 2) {
    silk_reset_decoder(channel_state[1]);
  }
  stereo_to_mono =
      decControl->nChannelsInternal == 1 && psDec->nChannelsInternal == 2 && (decControl->internalSampleRate == 1000 * mid.fs_kHz);
  if (mid.nFramesDecoded == 0) {
    for (int n = 0; n < decControl->nChannelsInternal; ++n) {
      const int payload_ms = decControl->payloadSize_ms;
      if (payload_ms == 0 || payload_ms == 10) {
        channel_state[n]->nFramesPerPacket = 1;
        channel_state[n]->nb_subfr = 2;
      } else if (payload_ms >= 20 && payload_ms <= 60 && payload_ms % 20 == 0) {
        channel_state[n]->nFramesPerPacket = payload_ms / 20;
        channel_state[n]->nb_subfr = 4;
      } else {
        return -203;
      }
      const int fs_kHz_dec = (decControl->internalSampleRate >> 10) + 1;
      if (fs_kHz_dec != 8 && fs_kHz_dec != 12 && fs_kHz_dec != 16) {
        return -200;
      }
      silk_decoder_set_fs(channel_state[n], fs_kHz_dec, decControl->API_sampleRate);
    }
  }
  if (decControl->nChannelsAPI == 2 && decControl->nChannelsInternal == 2 && psDec->nChannelsInternal == 1) {
    zero_object(psDec->sStereo.pred_prev_Q13);
    zero_object(psDec->sStereo.sSide);
    copy_n_bytes(&mid.resampler_state, static_cast<std::size_t>(sizeof(silk_resampler_state_struct)), &channel_state[1]->resampler_state);
  }
  psDec->nChannelsInternal = decControl->nChannelsInternal;
  if (decControl->API_sampleRate > static_cast<opus_int32>(48) * 1000 || decControl->API_sampleRate < 8000) {
    return -200;
  }
  if (lostFlag != 1 && mid.nFramesDecoded == 0) {
    std::array<int, 2> packet_has_lbrr{};
    for (int n = 0; n < decControl->nChannelsInternal; ++n) {
      for (int i = 0; i < channel_state[n]->nFramesPerPacket; ++i) {
        channel_state[n]->VAD_flags[i] = ec_dec_bit_logp(psRangeDec, 1);
      }
      packet_has_lbrr[n] = ec_dec_bit_logp(psRangeDec, 1);
    }
    for (int n = 0; n < decControl->nChannelsInternal; ++n) {
      zero_object(channel_state[n]->LBRR_flags);
      if (packet_has_lbrr[n]) {
        if (channel_state[n]->nFramesPerPacket == 1) {
          channel_state[n]->LBRR_flags[0] = 1;
        } else {
          const auto* icdf = channel_state[n]->nFramesPerPacket == 2 ? silk_LBRR_flags_2_iCDF.data() : silk_LBRR_flags_3_iCDF.data();
          const auto LBRR_symbol = ec_dec_icdf(psRangeDec, icdf, 8) + 1;
          for (int i = 0; i < channel_state[n]->nFramesPerPacket; ++i) {
            channel_state[n]->LBRR_flags[i] = ((LBRR_symbol) >> (i)) & 1;
          }
        }
      }
    }
    if (lostFlag == 0) {
      for (int i = 0; i < mid.nFramesPerPacket; ++i) {
        for (int n = 0; n < decControl->nChannelsInternal; ++n) {
          if (channel_state[n]->LBRR_flags[i]) {
            std::array<opus_int16, ((5 * 4) * 16)> pulses;
            int condCoding;
            if (decControl->nChannelsInternal == 2 && n == 0) {
              silk_stereo_decode_pred(psRangeDec, MS_pred_Q13);
              if (channel_state[1]->LBRR_flags[i] == 0) {
                silk_stereo_decode_mid_only(psRangeDec, decode_only_middle);
              }
            }
            condCoding = i > 0 && channel_state[n]->LBRR_flags[i - 1] ? 2 : 0;
            silk_decode_indices(channel_state[n], psRangeDec, i, 1, condCoding);
            silk_process_pulses<false>(
                psRangeDec,
                std::span<opus_int16>{pulses.data(), static_cast<std::size_t>(((channel_state[n]->frame_length + 16 - 1) & ~(16 - 1)))},
                channel_state[n]->indices.signalType, channel_state[n]->indices.quantOffsetType, channel_state[n]->frame_length);
          }
        }
      }
    }
  }
  if (decControl->nChannelsInternal == 2) {
    if (lostFlag == 0 || (lostFlag == 2 && mid.LBRR_flags[mid.nFramesDecoded] == 1)) {
      silk_stereo_decode_pred(psRangeDec, MS_pred_Q13);
      if ((lostFlag == 0 && channel_state[1]->VAD_flags[mid.nFramesDecoded] == 0) ||
          (lostFlag == 2 && channel_state[1]->LBRR_flags[mid.nFramesDecoded] == 0)) {
        silk_stereo_decode_mid_only(psRangeDec, decode_only_middle);
      } else {
        decode_only_middle = 0;
      }
    } else {
      std::ranges::copy(psDec->sStereo.pred_prev_Q13, MS_pred_Q13.begin());
    }
  }
  if (decControl->nChannelsInternal == 2 && decode_only_middle == 0 && psDec->prev_decode_only_middle == 1) {
    zero_object(channel_state[1]->outBuf);
    zero_object(channel_state[1]->sLPC_Q14_buf);
    channel_state[1]->lagPrev = 100;
    channel_state[1]->LastGainIndex = 10;
    channel_state[1]->prevSignalType = 0;
    channel_state[1]->first_frame_after_reset = 1;
  }
  std::array<opus_int16, celt_max_channels*(silk_max_frame_length + 2)> decoded_channel_storage;
  auto* samplesOut1_tmp_storage1 = decoded_channel_storage.data();
  std::array<opus_int16*, 2> samplesOut1_tmp{samplesOut1_tmp_storage1, samplesOut1_tmp_storage1 + mid.frame_length + 2};
  has_side = lostFlag == 0 ? !decode_only_middle
                           : !psDec->prev_decode_only_middle || (decControl->nChannelsInternal == 2 && lostFlag == 2 &&
                                                                 channel_state[1]->LBRR_flags[channel_state[1]->nFramesDecoded] == 1);
  for (int n = 0; n < decControl->nChannelsInternal; ++n) {
    if (n == 0 || has_side) {
      const int FrameIndex = mid.nFramesDecoded - n;
      const int condCoding = FrameIndex <= 0                           ? 0
                             : lostFlag == 2                           ? channel_state[n]->LBRR_flags[FrameIndex - 1] ? 2 : 0
                             : n > 0 && psDec->prev_decode_only_middle ? 1
                                                                       : 2;
      silk_decode_frame(channel_state[n], psRangeDec, &samplesOut1_tmp[n][2], &nSamplesOutDec, lostFlag, condCoding);
    } else {
      zero_n_bytes(&samplesOut1_tmp[n][2], static_cast<std::size_t>(nSamplesOutDec * sizeof(opus_int16)));
    }
    channel_state[n]->nFramesDecoded++;
  }
  if (decControl->nChannelsAPI == 2 && decControl->nChannelsInternal == 2) {
    silk_stereo_MS_to_LR(&psDec->sStereo, samplesOut1_tmp[0], samplesOut1_tmp[1], MS_pred_Q13.data(), mid.fs_kHz, nSamplesOutDec);
  } else {
    copy_n_bytes(psDec->sStereo.sMid.data(), static_cast<std::size_t>(2 * sizeof(opus_int16)), samplesOut1_tmp[0]);
    copy_n_bytes(&samplesOut1_tmp[0][nSamplesOutDec], static_cast<std::size_t>(2 * sizeof(opus_int16)), psDec->sStereo.sMid.data());
  }
  *nSamplesOut = static_cast<opus_int32>((nSamplesOutDec * decControl->API_sampleRate) / (mid.fs_kHz * 1000));
  std::array<opus_int16, OPUS_FRAME_SIZE_20MS> samplesOut2_tmp;
  for (int n = 0; n < std::min(decControl->nChannelsAPI, decControl->nChannelsInternal); ++n) {
    silk_resample_and_store_channel(*channel_state[n], samplesOut2_tmp, &samplesOut1_tmp[n][1], nSamplesOutDec, samplesOut, samplesOut16,
                                    *nSamplesOut, decControl->nChannelsAPI, n);
  }
  if (decControl->nChannelsAPI == 2 && decControl->nChannelsInternal == 1) {
    if (stereo_to_mono) {
      silk_resample_and_store_channel(*channel_state[1], samplesOut2_tmp, &samplesOut1_tmp[0][1], nSamplesOutDec, samplesOut, samplesOut16,
                                      *nSamplesOut, decControl->nChannelsAPI, 1);
    } else {
      if (samplesOut16 != nullptr) {
        silk_duplicate_mono_to_right(samplesOut16, *nSamplesOut);
      } else
        silk_duplicate_mono_to_right(samplesOut, *nSamplesOut);
    }
  }
  if (lostFlag == 1) {
    for (int i = 0; i < psDec->nChannelsInternal; ++i) {
      channel_state[i]->LastGainIndex = 10;
    }
  } else {
    psDec->prev_decode_only_middle = decode_only_middle;
  }
  return 0;
}

static void silk_bwexpander_FLP(std::span<float> ar, float chirp);
static void silk_k2a_FLP(float* A, const float* rc, opus_int32 order);
static void silk_autocorrelation_FLP(float* results, const float* inputData, int inputDataSize, int correlationCount);
static void silk_scale_copy_vector_FLP(float* data_out, const float* data_in, float gain, int dataSize);
static void silk_HP_variable_cutoff(silk_encoder_state_FLP state_Fxx[]);
static void silk_noise_shape_analysis_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, const float* pitch_res,
                                          const float* x);
static void silk_LTP_scale_ctrl_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, int condCoding);
static void silk_find_pitch_lags_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, float res[], const float x[]);
static void silk_encode_do_VAD(silk_encoder_state* psEncC);
static void silk_find_pred_coefs_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, const float res_pitch[],
                                     const float x[], int condCoding);
static inline void silk_residual_energy_FLP(float nrgs[4], const float x[], float a[2][16], const float gains[], const int subfr_length,
                                            const int nb_subfr, const int LPC_order);
static void silk_LPC_analysis_filter_FLP(float r_LPC[], const float PredCoef[], const float s[], const int length, const int Order);
static void silk_LTP_analysis_filter_FLP(float* LTP_res, const float* x, const float B[5 * 4], const int pitchL[4], const float invGains[4],
                                         const int subfr_length, const int nb_subfr, const int pre_length);
static inline void silk_warped_autocorrelation_FLP(float* corr, const float* input, const float warping, const int length, const int order);
static inline void silk_quant_LTP_gains_FLP(float B[4 * 5], opus_uint8 cbk_index[4], opus_uint8* periodicity_index,
                                            opus_int32* sum_log_gain_Q7, float* pred_gain_dB, const float XX[4 * 5 * 5],
                                            const float xX[4 * 5], const int subfr_len, const int nb_subfr);
static void silk_process_gains_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, int condCoding);
static void silk_A2NLSF_FLP(opus_int16* NLSF_Q15, const float* pAR, const int LPC_order);
static void silk_NLSF2A_FLP(float* pAR, const opus_int16* NLSF_Q15, const int LPC_order);
static inline void silk_process_NLSFs_FLP(silk_encoder_state* psEncC, float PredCoef[2][16], opus_int16 NLSF_Q15[16],
                                          const opus_int16 prev_NLSF_Q15[16]);
static void silk_NSQ_wrapper_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, SideInfoIndices* psIndices,
                                 silk_nsq_state* psNSQ, opus_int8 pulses[], const opus_int16 samples[]);
static int silk_encode_previous_lbrr(silk_encoder* encoder, silk_encoder_state_FLP* states, const silk_EncControlStruct& control,
                                     ec_enc* range_encoder, std::array<int, celt_max_channels>& packet_has_lbrr);
struct silk_pitch_analysis_result {
  std::array<int, 4> lags{};
  float correlation{};
  opus_int16 lag_index{};
  opus_uint8 contour_index{};
  bool voiced{};
};

static auto silk_pitch_analysis_core_FLP(const float* frame, float previous_correlation, int prevLag, float search_thres1,
                                         float search_thres2, int Fs_kHz, int complexity, int nb_subfr) -> silk_pitch_analysis_result;
static void silk_encode_frame_FLP(silk_encoder_state_FLP* psEnc, silk_lbrr_channel_state* lbrr, opus_int32* pnBytesOut, ec_enc* psRangeEnc,
                                  int condCoding, int maxBits, int useCBR, int lbrr_gain_reduction, bool protect_quiet_lbrr);
static void silk_init_encoder(silk_encoder_state_FLP* psEnc);
static void silk_control_encoder(silk_encoder_state_FLP* psEnc, silk_EncControlStruct* encControl, const int allow_bw_switch,
                                 const int force_fs_kHz);
static void silk_setup_complexity(silk_encoder_state* psEncC, int Complexity);
static float silk_schur_FLP(float refl_coef[], const float auto_corr[], int order);
static float silk_burg_modified_FLP(float A[], const float x[], const float minInvGain, const int subfr_length, const int nb_subfr,
                                    const int D);
static double silk_inner_product_FLP_c(const float* data1, const float* data2, int dataSize);
static double silk_energy_FLP(const float* data, int dataSize);
[[nodiscard]] constexpr auto silk_sqrt_reference(float x) noexcept -> float {
  return static_cast<float>(std::sqrt(static_cast<double>(x)));
}

[[nodiscard]] constexpr auto silk_pow_reference(float base, float exponent) noexcept -> float {
  return static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent)));
}

[[nodiscard]] constexpr auto silk_sigmoid(float x) noexcept -> float {
  return static_cast<float>(1.0 / (1.0 + std::exp(static_cast<double>(-x))));
}

static auto silk_float2short_array(opus_int16* out, const float* in, opus_int32 length) noexcept -> void {
  for (opus_int32 i = 0; i < length; ++i) {
    out[i] = static_cast<opus_int16>(clamp_value(float2int(in[i]), static_cast<opus_int32>(-32768), static_cast<opus_int32>(32767)));
  }
}

static auto silk_short2float_array(float* out, const opus_int16* in, opus_int32 length) noexcept -> void {
  for (opus_int32 i = 0; i < length; ++i) {
    out[i] = static_cast<float>(in[i]);
  }
}

[[nodiscard]] constexpr auto silk_log2(double x) noexcept -> float {
  return static_cast<float>(3.32192809488736 * std::log10(x));
}

[[nodiscard]] constexpr auto silk_encoder_get_size(int channels) noexcept -> int {
  return static_cast<int>(align(sizeof(silk_encoder)) + static_cast<std::size_t>(channels) * sizeof(silk_encoder_state_FLP));
}

static void silk_InitEncoder(void* encState, int channels) {
  auto* psEnc = static_cast<silk_encoder*>(encState);
  *psEnc = {};
  auto* state_Fxx = silk_encoder_channel_states(psEnc);
  std::uninitialized_default_construct_n(state_Fxx, static_cast<std::size_t>(channels));
  for (int n = 0; n < channels; n++) {
    silk_init_encoder(&state_Fxx[n]);
  }
  psEnc->nChannelsAPI = 1;
  psEnc->nChannelsInternal = 1;
}

static void silk_Encode(void* encState, silk_EncControlStruct* encControl, const opus_res* samplesIn, int nSamplesIn, ec_enc* psRangeEnc,
                        opus_int32* nBytesOut, const int prefillFlag) {
  int saved_payload_size_ms = 0, saved_complexity = 0;
  auto* psEnc = static_cast<silk_encoder*>(encState);
  auto* state_Fxx = silk_encoder_channel_states(psEnc);
  for (int n = 0; n < encControl->nChannelsAPI; ++n) {
    state_Fxx[n].sCmn.nFramesEncoded = 0;
  }
  encControl->switchReady = 0;
  if (encControl->nChannelsInternal > psEnc->nChannelsInternal) {
    silk_init_encoder(&state_Fxx[1]);
    zero_object(psEnc->sStereo.pred_prev_Q13);
    zero_object(psEnc->sStereo.sSide);
    psEnc->sStereo.mid_side_amp_Q0 = {0, 1, 0, 1};
    psEnc->sStereo.width_prev_Q14 = 0;
    psEnc->sStereo.smth_width_Q14 = 1 << 14;
    if (psEnc->nChannelsAPI == 2) {
      state_Fxx[1].sCmn.resampler_state = state_Fxx[0].sCmn.resampler_state;
    }
  }
  psEnc->nChannelsAPI = encControl->nChannelsAPI;
  psEnc->nChannelsInternal = encControl->nChannelsInternal;
  const bool stereo_input = encControl->nChannelsAPI == 2;
  const bool stereo_coding = encControl->nChannelsInternal == 2;
  const int nBlocksOf10ms = 100 * nSamplesIn / encControl->API_sampleRate;
  const int tot_blocks = std::max(1, nBlocksOf10ms >> 1);
  int curr_block = 0;
  if (prefillFlag) {
    saved_payload_size_ms = encControl->payloadSize_ms;
    saved_complexity = encControl->complexity;
    silk_LP_state save_LP;
    if (prefillFlag == 2) {
      save_LP = state_Fxx[0].sCmn.sLP;
      save_LP.saved_fs_kHz = state_Fxx[0].sCmn.fs_kHz;
    }
    for (int n = 0; n < encControl->nChannelsInternal; ++n) {
      silk_init_encoder(&state_Fxx[n]);
      if (prefillFlag == 2) {
        state_Fxx[n].sCmn.sLP = save_LP;
      }
      state_Fxx[n].sCmn.prefillFlag = 1;
    }
    encControl->payloadSize_ms = 10;
    encControl->complexity = 0;
  }
  for (int n = 0; n < encControl->nChannelsInternal; ++n) {
    silk_control_encoder(&state_Fxx[n], encControl, psEnc->allowBandwidthSwitch, n == 1 ? state_Fxx[0].sCmn.fs_kHz : 0);
    if (psEnc->lbrr != nullptr && state_Fxx[n].sCmn.first_frame_after_reset) {
      psEnc->lbrr->channels[static_cast<std::size_t>(n)].flags.fill(0);
    }
  }
  if (psEnc->lbrr != nullptr) {
    for (int n = 0; n < encControl->nChannelsInternal; ++n) {
      auto& lbrr = psEnc->lbrr->channels[static_cast<std::size_t>(n)];
      const int previously_enabled = lbrr.enabled;
      lbrr.enabled = !prefillFlag && encControl->LBRR_coded;
      if (lbrr.enabled) {
        const int initial_gain = 7;
        const int minimum_gain = initial_gain - 4;
        lbrr.gain_increase =
            previously_enabled ? std::max(initial_gain - encControl->packetLossPercentage / 5, minimum_gain) : initial_gain;
      }
    }
  }
  const int nSamplesToBufferMax = 10 * nBlocksOf10ms * state_Fxx[0].sCmn.fs_kHz;
  std::array<opus_int16, 2 * silk_max_resampler_batch_size> resampler_input_storage;
  auto* buf = resampler_input_storage.data();
  std::array<int, celt_max_channels> packet_has_lbrr{};
  while (true) {
    int nSamplesToBuffer = std::min(state_Fxx[0].sCmn.frame_length - state_Fxx[0].sCmn.inputBufIx, nSamplesToBufferMax);
    if (stereo_coding) {
      nSamplesToBuffer = std::min(nSamplesToBuffer, state_Fxx[1].sCmn.frame_length - state_Fxx[1].sCmn.inputBufIx);
      nSamplesToBuffer = std::min(nSamplesToBuffer, 10 * nBlocksOf10ms * state_Fxx[1].sCmn.fs_kHz);
    } else if (stereo_input && psEnc->nPrevChannelsInternal == 2 && state_Fxx[0].sCmn.nFramesEncoded == 0) {
      nSamplesToBuffer = std::min(nSamplesToBuffer, state_Fxx[1].sCmn.frame_length - state_Fxx[1].sCmn.inputBufIx);
    }
    const int nSamplesFromInput =
        static_cast<opus_int32>(nSamplesToBuffer * state_Fxx[0].sCmn.API_fs_Hz / (state_Fxx[0].sCmn.fs_kHz * 1000));
    auto resample_input = [&](int channel, const opus_int16* input) {
      auto& state = state_Fxx[channel].sCmn;
      silk_resampler(&state.resampler_state, &state.inputBuf[state.inputBufIx + 2], input, nSamplesFromInput);
    };
    if (stereo_coding) {
      const int id = state_Fxx[0].sCmn.nFramesEncoded;
      if (psEnc->nPrevChannelsInternal == 1 && id == 0) {
        state_Fxx[1].sCmn.resampler_state = state_Fxx[0].sCmn.resampler_state;
      }
      auto* right = buf + nSamplesFromInput;
      for (int n = 0; n < nSamplesFromInput; ++n) {
        buf[n] = FLOAT2INT16(samplesIn[2 * n]);
        right[n] = FLOAT2INT16(samplesIn[2 * n + 1]);
      }
      resample_input(0, buf);
      state_Fxx[0].sCmn.inputBufIx += nSamplesToBuffer;
      resample_input(1, right);
      state_Fxx[1].sCmn.inputBufIx += nSamplesToBuffer;
    } else if (stereo_input) {
      for (int n = 0; n < nSamplesFromInput; ++n) {
        const opus_int32 sum = FLOAT2INT16(samplesIn[2 * n] + samplesIn[2 * n + 1]);
        buf[n] = static_cast<opus_int16>(rounded_rshift<1>(sum));
      }
      const int mono_input_start = state_Fxx[0].sCmn.inputBufIx;
      resample_input(0, buf);
      state_Fxx[0].sCmn.inputBufIx += nSamplesToBuffer;
      if (psEnc->nPrevChannelsInternal == 2 && state_Fxx[0].sCmn.nFramesEncoded == 0) {
        const int side_input_start = state_Fxx[1].sCmn.inputBufIx;
        resample_input(1, buf);
        for (int n = 0; n < nSamplesToBuffer; ++n) {
          state_Fxx[0].sCmn.inputBuf[mono_input_start + n + 2] =
              (state_Fxx[0].sCmn.inputBuf[mono_input_start + n + 2] + state_Fxx[1].sCmn.inputBuf[side_input_start + n + 2]) >> 1;
        }
      }
    } else {
      celt_float2int16_c(samplesIn, buf, static_cast<std::size_t>(nSamplesFromInput));
      resample_input(0, buf);
      state_Fxx[0].sCmn.inputBufIx += nSamplesToBuffer;
    }
    samplesIn += nSamplesFromInput * encControl->nChannelsAPI;
    nSamplesIn -= nSamplesFromInput;
    psEnc->allowBandwidthSwitch = 0;
    if (state_Fxx[0].sCmn.inputBufIx >= state_Fxx[0].sCmn.frame_length) {
      if (state_Fxx[0].sCmn.nFramesEncoded == 0 && !prefillFlag) {
        const std::array<opus_uint8, 2> icdf{
            static_cast<opus_uint8>(256 - (256 >> ((state_Fxx[0].sCmn.nFramesPerPacket + 1) * encControl->nChannelsInternal))), 0};
        ec_enc_icdf(psRangeEnc, 0, icdf.data(), 8);
        if (psEnc->lbrr != nullptr) {
          silk_encode_previous_lbrr(psEnc, state_Fxx, *encControl, psRangeEnc, packet_has_lbrr);
        }
      }
      silk_HP_variable_cutoff(state_Fxx);
      auto& state0 = state_Fxx[0].sCmn;
      const opus_int32 lbrr_bits = psEnc->lbrr == nullptr ? 0 : psEnc->lbrr->average_bits;
      const opus_int32 frameBits =
          std::max<opus_int32>(0, encControl->bitRate * encControl->payloadSize_ms / 1000 - lbrr_bits) / state0.nFramesPerPacket;
      opus_int32 TargetRate_bps = frameBits * (encControl->payloadSize_ms == 10 ? 100 : 50) - 2 * psEnc->nBitsExceeded;
      if (!prefillFlag && state0.nFramesEncoded > 0) {
        const opus_int32 bitsBalance = ec_tell(psRangeEnc) - frameBits * state0.nFramesEncoded;
        TargetRate_bps -= 2 * bitsBalance;
      }
      TargetRate_bps = clamp_value(TargetRate_bps, std::min(5000, encControl->bitRate), std::max(5000, encControl->bitRate));
      opus_int32 MStargetRates_bps[2];
      if (stereo_coding) {
        const int frame_index = state0.nFramesEncoded;
        silk_stereo_LR_to_MS(&psEnc->sStereo, &state0.inputBuf[2], &state_Fxx[1].sCmn.inputBuf[2], psEnc->sStereo.predIx[frame_index],
                             &psEnc->sStereo.mid_only_flags[frame_index], MStargetRates_bps, TargetRate_bps, state0.speech_activity_Q8,
                             encControl->toMono, encControl->preserveStereo, state0.fs_kHz, state0.frame_length);
        if (psEnc->sStereo.mid_only_flags[frame_index] == 0) {
          if (psEnc->prev_decode_only_middle == 1) {
            zero_object(state_Fxx[1].sShape);
            zero_object(state_Fxx[1].sCmn.sNSQ);
            zero_object(state_Fxx[1].sCmn.prev_NLSFq_Q15);
            zero_object(state_Fxx[1].sCmn.sLP.In_LP_State);
            state_Fxx[1].sCmn.prevLag = state_Fxx[1].sCmn.sNSQ.lagPrev = 100;
            state_Fxx[1].sShape.LastGainIndex = 10;
            state_Fxx[1].sCmn.prevSignalType = 0;
            state_Fxx[1].sCmn.sNSQ.prev_gain_Q16 = 65536;
            state_Fxx[1].sCmn.first_frame_after_reset = 1;
          }
          silk_encode_do_VAD(&state_Fxx[1].sCmn);
        } else {
          state_Fxx[1].sCmn.VAD_flags[frame_index] = 0;
        }

        if (!prefillFlag) {
          silk_stereo_encode_pred(psRangeEnc, psEnc->sStereo.predIx[frame_index]);
          if (!state_Fxx[1].sCmn.VAD_flags[frame_index]) {
            silk_stereo_encode_mid_only(psRangeEnc, psEnc->sStereo.mid_only_flags[frame_index]);
          }
        }
      } else {
        copy_n_bytes(psEnc->sStereo.sMid.data(), static_cast<std::size_t>(2 * sizeof(opus_int16)), state_Fxx[0].sCmn.inputBuf);
        copy_n_bytes(&state_Fxx[0].sCmn.inputBuf[state_Fxx[0].sCmn.frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)),
                     psEnc->sStereo.sMid.data());
      }
      silk_encode_do_VAD(&state_Fxx[0].sCmn);
      for (int n = 0; n < encControl->nChannelsInternal; ++n) {
        int maxBits = encControl->maxBits;
        if (tot_blocks == 2 && curr_block == 0) {
          maxBits = maxBits * 3 / 5;
        } else if (tot_blocks == 3 && curr_block < 2)
          maxBits = maxBits * (curr_block == 0 ? 2 : 3) / (curr_block == 0 ? 5 : 4);
        int useCBR = encControl->useCBR && curr_block == tot_blocks - 1;
        const opus_int32 channelRate_bps = stereo_coding ? MStargetRates_bps[n] : TargetRate_bps;
        if (encControl->nChannelsInternal == 2 && n == 0 && MStargetRates_bps[1] > 0) {
          useCBR = 0;
          maxBits -= encControl->maxBits / (tot_blocks * 2);
        }
        if (channelRate_bps > 0) {
          silk_control_SNR(&state_Fxx[n].sCmn, channelRate_bps);
          const int saved_complexity = state_Fxx[n].sCmn.Complexity;
          const bool side_residual_fast_path =
              !encControl->LBRR_coded && encControl->nChannelsInternal == 2 && n == 1 && saved_complexity > 0 && channelRate_bps <= 12000;
          if (side_residual_fast_path) {
            silk_setup_complexity(&state_Fxx[n].sCmn, 0);
          }
          const int condCoding = state0.nFramesEncoded - n <= 0 ? 0 : (n > 0 && psEnc->prev_decode_only_middle ? 1 : 2);
          auto* lbrr = psEnc->lbrr == nullptr ? nullptr : &psEnc->lbrr->channels[static_cast<std::size_t>(n)];
          const int lbrr_gain_reduction = state_Fxx[n].sCmn.nb_subfr == 2 ? 1 : (encControl->nChannelsInternal == 1 || useCBR ? 2 : 0);
          silk_encode_frame_FLP(&state_Fxx[n], lbrr, nBytesOut, psRangeEnc, condCoding, maxBits, useCBR, lbrr_gain_reduction, n == 0);
          if (side_residual_fast_path) {
            silk_setup_complexity(&state_Fxx[n].sCmn, saved_complexity);
          }
        }
        state_Fxx[n].sCmn.inputBufIx = 0;
        state_Fxx[n].sCmn.nFramesEncoded++;
      }
      psEnc->prev_decode_only_middle = psEnc->sStereo.mid_only_flags[state0.nFramesEncoded - 1];
      if (*nBytesOut > 0 && state0.nFramesEncoded == state0.nFramesPerPacket) {
        int flags = 0;
        for (int n = 0; n < encControl->nChannelsInternal; ++n) {
          for (int i = 0; i < state_Fxx[n].sCmn.nFramesPerPacket; ++i) {
            flags = wrap_shift_left(flags, 1);
            flags |= state_Fxx[n].sCmn.VAD_flags[i];
          }
          flags = wrap_shift_left(flags, 1);
          flags |= packet_has_lbrr[static_cast<std::size_t>(n)];
        }
        if (!prefillFlag) {
          ec_enc_patch_initial_bits(psRangeEnc, flags, (state0.nFramesPerPacket + 1) * encControl->nChannelsInternal);
        }
        psEnc->nBitsExceeded += *nBytesOut * 8;
        psEnc->nBitsExceeded -= static_cast<opus_int32>(encControl->bitRate * encControl->payloadSize_ms / 1000);
        psEnc->nBitsExceeded = clamp_value(psEnc->nBitsExceeded, 0, 10000);
        const int switch_threshold =
            fixed_q<8>(0.05f) + silk_mul_wb(fixed_q<24>((1.0f - 0.05f) / 5000.0f), psEnc->timeSinceSwitchAllowed_ms);
        psEnc->allowBandwidthSwitch = state0.speech_activity_Q8 < switch_threshold;
        psEnc->timeSinceSwitchAllowed_ms = psEnc->allowBandwidthSwitch ? 0 : psEnc->timeSinceSwitchAllowed_ms + encControl->payloadSize_ms;
      }
      if (nSamplesIn != 0) {
        ++curr_block;
        continue;
      }
    }
    break;
  }
  psEnc->nPrevChannelsInternal = encControl->nChannelsInternal;
  encControl->allowBandwidthSwitch = psEnc->allowBandwidthSwitch;
  encControl->inWBmodeWithoutVariableLP = state_Fxx[0].sCmn.fs_kHz == 16 && state_Fxx[0].sCmn.sLP.mode == 0;
  encControl->internalSampleRate = state_Fxx[0].sCmn.fs_kHz * 1000;
  encControl->stereoWidth_Q14 = encControl->toMono ? 0 : psEnc->sStereo.smth_width_Q14;
  if (prefillFlag) {
    encControl->payloadSize_ms = saved_payload_size_ms;
    encControl->complexity = saved_complexity;
    for (int n = 0; n < encControl->nChannelsInternal; ++n) {
      state_Fxx[n].sCmn.prefillFlag = 0;
    }
  }
  encControl->signalType = state_Fxx[0].sCmn.indices.signalType;
  encControl->offset = silk_Quantization_Offsets_Q10[state_Fxx[0].sCmn.indices.signalType >> 1][state_Fxx[0].sCmn.indices.quantOffsetType];
}

static void silk_encode_indices(silk_encoder_state* psEncC, SideInfoIndices& indices, ec_enc* psRangeEnc, int condCoding,
                                bool lbrr = false) {
  const int type_offset = 2 * indices.signalType + indices.quantOffsetType;
  const auto* icdf = type_offset >= 2 ? silk_type_offset_VAD_iCDF.data() : silk_type_offset_no_VAD_iCDF.data();
  ec_enc_icdf(psRangeEnc, lbrr || type_offset >= 2 ? type_offset - 2 : type_offset, lbrr ? silk_type_offset_VAD_iCDF.data() : icdf, 8);
  silk_process_indices<true>(psEncC, indices, psRangeEnc, condCoding);
}

static int silk_encode_previous_lbrr(silk_encoder* encoder, silk_encoder_state_FLP* states, const silk_EncControlStruct& control,
                                     ec_enc* range_encoder, std::array<int, celt_max_channels>& packet_has_lbrr) {
  auto& lbrr = *encoder->lbrr;
  const int frames = states[0].sCmn.nFramesPerPacket;
  if ((lbrr.frames_per_packet != 0 && lbrr.frames_per_packet != frames) ||
      (lbrr.channels_in_packet != 0 && lbrr.channels_in_packet != control.nChannelsInternal)) {
    for (auto& channel : lbrr.channels) {
      channel.flags.fill(0);
    }
    lbrr.average_bits = 0;
  }
  const int start_bits = ec_tell(range_encoder);
  for (int channel_index = 0; channel_index < control.nChannelsInternal; ++channel_index) {
    auto& channel = lbrr.channels[static_cast<std::size_t>(channel_index)];
    int symbol = 0;
    for (int frame = 0; frame < frames; ++frame) {
      symbol |= channel.flags[static_cast<std::size_t>(frame)] << frame;
    }
    packet_has_lbrr[static_cast<std::size_t>(channel_index)] = symbol != 0;
    if (symbol != 0 && frames > 1) {
      const auto* icdf = frames == 2 ? silk_LBRR_flags_2_iCDF.data() : silk_LBRR_flags_3_iCDF.data();
      ec_enc_icdf(range_encoder, symbol - 1, icdf, 8);
    }
  }
  for (int frame = 0; frame < frames; ++frame) {
    for (int channel_index = 0; channel_index < control.nChannelsInternal; ++channel_index) {
      auto& channel = lbrr.channels[static_cast<std::size_t>(channel_index)];
      if (!channel.flags[static_cast<std::size_t>(frame)]) {
        continue;
      }
      if (control.nChannelsInternal == 2 && channel_index == 0) {
        silk_stereo_encode_pred(range_encoder, encoder->sStereo.predIx[static_cast<std::size_t>(frame)]);
        if (!lbrr.channels[1].flags[static_cast<std::size_t>(frame)]) {
          silk_stereo_encode_mid_only(range_encoder, encoder->sStereo.mid_only_flags[static_cast<std::size_t>(frame)]);
        }
      }
      const int cond_coding = frame > 0 && channel.flags[static_cast<std::size_t>(frame - 1)] ? 2 : 0;
      auto& state = states[channel_index].sCmn;
      auto& indices = channel.indices[static_cast<std::size_t>(frame)];
      silk_encode_indices(&state, indices, range_encoder, cond_coding, true);
      silk_process_pulses<true>(range_encoder, std::span<opus_int8>{channel.pulses[static_cast<std::size_t>(frame)]}, indices.signalType,
                                indices.quantOffsetType, state.frame_length);
    }
  }
  for (auto& channel : lbrr.channels) {
    channel.flags.fill(0);
  }
  const int current_bits = ec_tell(range_encoder) - start_bits;
  lbrr.average_bits = current_bits < 10 ? 0 : lbrr.average_bits < 10 ? current_bits : (lbrr.average_bits + current_bits) / 2;
  lbrr.frames_per_packet = frames;
  lbrr.channels_in_packet = control.nChannelsInternal;
  return current_bits;
}

static void silk_gains_quant(opus_int8 ind[4], opus_int32 gain_Q16[4], opus_int8* prev_ind, const int conditional, const int nb_subfr) {
  int k, double_step_size_threshold;
  for (k = 0; k < nb_subfr; k++) {
    ind[k] = static_cast<opus_int8>((2251LL * static_cast<opus_int16>(silk_lin2log(gain_Q16[k]) - 2090)) >> 16);
    if (ind[k] < *prev_ind) {
      ind[k]++;
    }
    ind[k] = static_cast<opus_int8>(clamp_value<int>(ind[k], 0, 64 - 1));
    if (k == 0 && conditional == 0) {
      ind[k] = static_cast<opus_int8>(clamp_value<int>(ind[k], *prev_ind - 4, 64 - 1));
      *prev_ind = ind[k];
    } else {
      ind[k] = ind[k] - *prev_ind;
      double_step_size_threshold = 2 * 36 - 64 + *prev_ind;
      if (ind[k] > double_step_size_threshold) {
        ind[k] = double_step_size_threshold + ((ind[k] - double_step_size_threshold + 1) >> (1));
      }
      ind[k] = static_cast<opus_int8>(clamp_value<int>(ind[k], -4, 36));
      if (ind[k] > double_step_size_threshold) {
        *prev_ind += wrap_shift_left(ind[k], 1) - double_step_size_threshold;
        *prev_ind = std::min(*prev_ind, static_cast<opus_int8>(64 - 1));
      } else {
        *prev_ind += ind[k];
      }
      ind[k] -= -4;
    }
    gain_Q16[k] = silk_log2lin(std::min<opus_int32>((1907825LL * static_cast<opus_int16>(*prev_ind) >> 16) + 2090, 3967));
  }
}

void silk_gains_dequant(opus_int32 gain_Q16[4], const opus_int8 ind[4], opus_int8* prev_ind, const int conditional, const int nb_subfr) {
  int k, ind_tmp, double_step_size_threshold;
  for (k = 0; k < nb_subfr; k++) {
    if (k == 0 && conditional == 0) {
      *prev_ind = std::max(ind[k], static_cast<opus_int8>(*prev_ind - 16));
    } else {
      ind_tmp = ind[k] + -4;
      double_step_size_threshold = 2 * 36 - 64 + *prev_ind;
      if (ind_tmp > double_step_size_threshold) {
        *prev_ind += wrap_shift_left(ind_tmp, 1) - double_step_size_threshold;
      } else {
        *prev_ind += ind_tmp;
      }
    }
    *prev_ind = static_cast<opus_int8>(clamp_value<int>(*prev_ind, 0, 64 - 1));
    gain_Q16[k] = silk_log2lin(std::min<opus_int32>((1907825LL * static_cast<opus_int16>(*prev_ind) >> 16) + 2090, 3967));
  }
}

static opus_int32 silk_gains_ID(const opus_int8 ind[4], const int nb_subfr) {
  int k;
  opus_int32 gainsID = 0;
  for (k = 0; k < nb_subfr; k++) {
    gainsID = wrap_add(ind[k], wrap_shift_left(gainsID, 8));
  }
  return gainsID;
}

static void silk_interpolate(std::span<opus_int16> xi, std::span<const opus_int16> x0, std::span<const opus_int16> x1, const int ifact_Q2) {
  for (std::size_t index = 0; index < xi.size(); ++index) {
    xi[index] = static_cast<opus_int16>(x0[index] + silk_mul_i16_shift<2>(x1[index] - x0[index], ifact_Q2));
  }
}

static void silk_LP_interpolate_filter_taps(opus_int32 B_Q28[3], opus_int32 A_Q28[2], const int ind, const opus_int32 fac_Q16) {
  const auto b_out = std::span<opus_int32, 3>{B_Q28, 3};
  const auto a_out = std::span<opus_int32, 2>{A_Q28, 2};
  const auto active = ind < 4 && fac_Q16 > 0;
  const auto upper_half = active && fac_Q16 >= 32768;
  const auto base = std::min(ind + upper_half, 4);
  const auto next = std::min(ind + 1, 4);
  const auto fraction = active ? static_cast<opus_int16>(fac_Q16 - (upper_half << 16)) : opus_int16{0};
  const auto interpolate = [=]<std::size_t Count>(std::span<opus_int32, Count> output, const auto& taps) {
    for (std::size_t index = 0; index < Count; ++index) {
      output[index] =
          static_cast<opus_int32>(taps[base][index] + (((taps[next][index] - taps[ind][index]) * static_cast<opus_int64>(fraction)) >> 16));
    }
  };
  interpolate(b_out, silk_Transition_LP_B_Q28);
  interpolate(a_out, silk_Transition_LP_A_Q28);
}

static void silk_NLSF_residual_dequant(std::span<opus_int16, 16> x_Q10, std::span<const opus_int8, 16> indices,
                                       std::span<const opus_uint8, 16> pred_coef_Q8, const int quant_step_size_Q16,
                                       const opus_int16 order) {
  constexpr auto adjustment = static_cast<opus_int32>((0.1) * (static_cast<opus_int64>(1) << 10) + 0.5);
  auto residual = opus_int32{0};
  for (int index = order - 1; index >= 0; --index) {
    const auto prediction = silk_mul_i16_shift<8>(residual, pred_coef_Q8[index]);
    residual = wrap_shift_left(indices[index], 10);
    if (residual > 0) {
      residual -= adjustment;
    } else if (residual < 0) {
      residual += adjustment;
    }
    residual = prediction + silk_mul_wb(residual, quant_step_size_Q16);
    x_Q10[index] = residual;
  }
}

void silk_NLSF_decode(std::span<opus_int16, 16> pNLSF_Q15, std::span<opus_int8, 17> NLSFIndices, const silk_NLSF_CB_struct* psNLSF_CB) {
  opus_uint8 pred_Q8[16];
  opus_int16 ec_ix[16], res_Q10[16];
  silk_NLSF_unpack(ec_ix, pred_Q8, psNLSF_CB, NLSFIndices[0]);
  silk_NLSF_residual_dequant(res_Q10, NLSFIndices.last<16>(), pred_Q8, psNLSF_CB->quantStepSize_Q16, psNLSF_CB->order);
  const auto* pCB_element = &psNLSF_CB->CB1_NLSF_Q8[NLSFIndices[0] * psNLSF_CB->order];
  const auto* pCB_Wght_Q9 = &psNLSF_CB->CB1_Wght_Q9[NLSFIndices[0] * psNLSF_CB->order];
  for (int i = 0; i < psNLSF_CB->order; ++i) {
    const opus_int32 NLSF_Q15_tmp = wrap_shift_left(res_Q10[i], 14) / pCB_Wght_Q9[i] + wrap_shift_left(pCB_element[i], 7);
    pNLSF_Q15[i] = static_cast<opus_int16>(std::clamp<opus_int32>(NLSF_Q15_tmp, 0, 32767));
  }
  silk_NLSF_stabilize(pNLSF_Q15.data(), psNLSF_CB->deltaMin_Q15, psNLSF_CB->order);
}

template <bool Warped>
static auto silk_nsq_noise_shape_feedback(opus_int32 diff_Q14, opus_int32* state_Q14, const opus_int16* coefficients, int order,
                                          int warping_Q16 = 0) noexcept -> opus_int32 {
  const auto warped = [&](opus_int32 delta) noexcept {
    if constexpr (Warped) {
      return static_cast<opus_int32>((delta * static_cast<opus_int64>(static_cast<opus_int16>(warping_Q16))) >> 16);
    }
    return opus_int32{0};
  };
  auto even = wrap_add(diff_Q14, warped(state_Q14[0]));
  auto odd = wrap_add(state_Q14[0], warped(wrap_subtract(state_Q14[1], even)));
  state_Q14[0] = even;
  auto feedback = wrap_add(order >> 1, silk_mul_wb(even, coefficients[0]));
  for (int index = 2; index < order; index += 2) {
    even = wrap_add(state_Q14[index - 1], warped(wrap_subtract(state_Q14[index], odd)));
    state_Q14[index - 1] = odd;
    feedback = wrap_add(feedback, silk_mul_wb(odd, coefficients[index - 1]));
    odd = wrap_add(state_Q14[index], warped(wrap_subtract(state_Q14[index + 1], even)));
    state_Q14[index] = even;
    feedback = wrap_add(feedback, silk_mul_wb(even, coefficients[index]));
  }
  state_Q14[order - 1] = odd;
  return wrap_shift_left(wrap_add(feedback, silk_mul_wb(odd, coefficients[order - 1])), 1);
}
struct silk_nsq_sample_state {
  opus_int32 Q_Q10, RD_Q10, xq_Q14, LF_AR_Q14, Diff_Q14, sLTP_shp_Q14, LPC_exc_Q14;
};
struct silk_nsq_candidate_pair {
  opus_int32 q1_Q10, q2_Q10, dist1_Q20, dist2_Q20, rate1_Q20, rate2_Q20;
};
static auto silk_finish_nsq(const silk_encoder_state* psEncC, silk_nsq_state* NSQ) noexcept -> void {
  move_n_bytes(&NSQ->xq[psEncC->frame_length], static_cast<std::size_t>(psEncC->ltp_mem_length * sizeof(opus_int16)), NSQ->xq);
  move_n_bytes(&NSQ->sLTP_shp_Q14[psEncC->frame_length], static_cast<std::size_t>(psEncC->ltp_mem_length * sizeof(opus_int32)),
               NSQ->sLTP_shp_Q14);
}

[[nodiscard]] static constexpr auto silk_signed_clamped_residual(opus_int32 residual_q10, opus_int32 seed) noexcept -> opus_int32 {
  return clamp_value(seed < 0 ? -residual_q10 : residual_q10, -(31 << 10), 30 << 10);
}

[[nodiscard]] constexpr auto silk_pack_harm_shape_gain(opus_int32 gain_q14) noexcept -> opus_int32 {
  return (gain_q14 >> 2) | wrap_shift_left(gain_q14 >> 1, 16);
}

[[nodiscard]] constexpr auto silk_square_i16(opus_int32 value) noexcept -> opus_int32 {
  return static_cast<opus_int32>(static_cast<opus_int16>(value)) * static_cast<opus_int32>(static_cast<opus_int16>(value));
}

[[nodiscard]] static auto silk_quantize_candidate_pair(opus_int32 residual_q10, int Lambda_Q10, int offset_Q10) noexcept
    -> silk_nsq_candidate_pair {
  auto q1_Q10 = residual_q10 - offset_Q10;
  auto q1_Q0 = q1_Q10 >> 10;
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
    q1_Q10 = (q1_Q0 << 10) - 80 + offset_Q10;
    q2_Q10 = q1_Q10 + 1024;
  } else if (q1_Q0 == 0) {
    q1_Q10 = offset_Q10;
    q2_Q10 = q1_Q10 + (1024 - 80);
  } else if (q1_Q0 == -1) {
    q2_Q10 = offset_Q10;
    q1_Q10 = q2_Q10 - (1024 - 80);
  } else {
    q1_Q10 = (q1_Q0 << 10) + 80 + offset_Q10;
    q2_Q10 = q1_Q10 + 1024;
  }
  const auto lambda_i16 = static_cast<opus_int32>(static_cast<opus_int16>(Lambda_Q10));
  const auto rd1_bias = static_cast<opus_int32>(static_cast<opus_int16>(q1_Q10 < 0 ? -q1_Q10 : q1_Q10)) * lambda_i16;
  const auto rd2_bias = static_cast<opus_int32>(static_cast<opus_int16>(q2_Q10 < 0 ? -q2_Q10 : q2_Q10)) * lambda_i16;
  const auto dist1 = silk_square_i16(residual_q10 - q1_Q10);
  const auto dist2 = silk_square_i16(residual_q10 - q2_Q10);
  return {q1_Q10, q2_Q10, dist1, dist2, rd1_bias, rd2_bias};
}

[[nodiscard]] static auto silk_harmonic_shaping(const opus_int32* shp_lag_ptr, opus_int32 HarmShapeFIRPacked_Q14) noexcept -> opus_int32;
[[nodiscard]] static auto silk_nsq_build_sample(opus_int32 q_Q10, opus_int32 seed, opus_int32 LTP_pred_Q14, opus_int32 LPC_pred_Q14,
                                                opus_int32 x_Q10, opus_int32 n_AR_Q14, opus_int32 n_LF_Q14) noexcept
    -> silk_nsq_sample_state;
static inline void scale_q16_buffer(opus_int32* data, const int count, const opus_int32 gain_Q16) noexcept {
  for (int i = 0; i < count; ++i) {
    data[i] = multiply_q16(gain_Q16, data[i]);
  }
}

static auto silk_nsq_scale_common(const silk_encoder_state* psEncC, silk_nsq_state* NSQ, std::span<const opus_int16> x16,
                                  std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP, std::span<opus_int32> sLTP_Q15,
                                  int subfr, const int LTP_scale_Q14, std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL,
                                  const int signal_type, const int ltp_scale_end) noexcept -> opus_int32 {
  const auto lag = pitchL[subfr];
  auto inv_gain_Q31 = silk_INVERSE32_varQ(std::max(Gains_Q16[subfr], 1), 47);
  const auto inv_gain_Q26 = rounded_rshift<5>(inv_gain_Q31);
  for (auto index = std::size_t{}; index < x16.size(); ++index)
    x_sc_Q10[index] = multiply_q16(x16[index], inv_gain_Q26);
  if (NSQ->rewhite_flag) {
    if (subfr == 0) {
      inv_gain_Q31 = wrap_shift_left(
          static_cast<opus_int32>((inv_gain_Q31 * static_cast<opus_int64>(static_cast<opus_int16>(LTP_scale_Q14))) >> 16), 2);
    }
    for (auto i = NSQ->sLTP_buf_idx - lag - 5 / 2; i < NSQ->sLTP_buf_idx; i++) {
      sLTP_Q15[i] = static_cast<opus_int32>((inv_gain_Q31 * static_cast<opus_int64>(static_cast<opus_int16>(sLTP[i]))) >> 16);
    }
  }
  if (Gains_Q16[subfr] == NSQ->prev_gain_Q16) {
    return static_cast<opus_int32>(1) << 16;
  }
  const auto gain_adj_Q16 = silk_DIV32_varQ(NSQ->prev_gain_Q16, Gains_Q16[subfr], 16);
  scale_q16_buffer(NSQ->sLTP_shp_Q14 + (NSQ->sLTP_shp_buf_idx - psEncC->ltp_mem_length), psEncC->ltp_mem_length, gain_adj_Q16);
  if (signal_type == 2 && NSQ->rewhite_flag == 0) {
    const auto ltp_start = NSQ->sLTP_buf_idx - lag - 5 / 2;
    scale_q16_buffer(sLTP_Q15.data() + ltp_start, ltp_scale_end - ltp_start, gain_adj_Q16);
  }
  NSQ->sLF_AR_shp_Q14 = multiply_q16(gain_adj_Q16, NSQ->sLF_AR_shp_Q14);
  NSQ->sDiff_shp_Q14 = multiply_q16(gain_adj_Q16, NSQ->sDiff_shp_Q14);
  scale_q16_buffer(NSQ->sLPC_Q14, 16, gain_adj_Q16);
  scale_q16_buffer(NSQ->sAR2_Q14, 24, gain_adj_Q16);
  NSQ->prev_gain_Q16 = Gains_Q16[subfr];
  return gain_adj_Q16;
}

static void silk_noise_shape_quantizer(silk_nsq_state* NSQ, int signalType, std::span<const opus_int32> x_sc_Q10,
                                       std::span<opus_int8> pulses, std::span<opus_int16> xq, std::span<opus_int32> sLTP_Q15,
                                       std::span<const opus_int16> a_Q12, std::span<const opus_int16> b_Q14,
                                       std::span<const opus_int16> AR_shp_Q13, int lag, opus_int32 HarmShapeFIRPacked_Q14, int Tilt_Q14,
                                       opus_int32 LF_shp_Q14, opus_int32 Gain_Q16, int Lambda_Q10, int offset_Q10) {
  int i;
  opus_int32 LTP_pred_Q13, LPC_pred_Q10, n_AR_Q12, n_LTP_Q13;
  opus_int32 n_LF_Q12, r_Q10, Gain_Q10, tmp1;
  opus_int32 *psLPC_Q14, *shp_lag_ptr, *pred_lag_ptr;
  const auto length = static_cast<int>(x_sc_Q10.size());
  const auto shapingLPCOrder = static_cast<int>(AR_shp_Q13.size());
  const auto* b_q14_coefficients = b_Q14.data();
  const auto tilt_Q14_i16 = static_cast<opus_int16>(Tilt_Q14);
  const auto lf_shp_Q14_i16 = static_cast<opus_int16>(LF_shp_Q14);
  const auto lf_shp_Q14_high = static_cast<opus_int64>(LF_shp_Q14) >> 16;
  shp_lag_ptr = &NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - lag + 3 / 2];
  pred_lag_ptr = &sLTP_Q15[NSQ->sLTP_buf_idx - lag + 5 / 2];
  Gain_Q10 = ((Gain_Q16) >> (6));
  psLPC_Q14 = &NSQ->sLPC_Q14[16 - 1];
  for (i = 0; i < length; i++) {
    NSQ->rand_seed = silk_next_rand_seed(NSQ->rand_seed);
    LPC_pred_Q10 = silk_lpc_prediction_q10(psLPC_Q14 + 1, a_Q12.data(), static_cast<int>(a_Q12.size()));
    if (signalType == 2) {
      LTP_pred_Q13 = silk_ltp_prediction_5tap(pred_lag_ptr, b_q14_coefficients);
      ++pred_lag_ptr;
    } else {
      LTP_pred_Q13 = 0;
    }
    n_AR_Q12 = silk_nsq_noise_shape_feedback<false>(NSQ->sDiff_shp_Q14, NSQ->sAR2_Q14, AR_shp_Q13.data(), shapingLPCOrder);
    n_AR_Q12 = (static_cast<opus_int32>((n_AR_Q12) + (((NSQ->sLF_AR_shp_Q14) * static_cast<opus_int64>(tilt_Q14_i16)) >> 16)));
    n_LF_Q12 = (static_cast<opus_int32>(((NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - 1]) * static_cast<opus_int64>(lf_shp_Q14_i16)) >> 16));
    n_LF_Q12 = (static_cast<opus_int32>((n_LF_Q12) + (((NSQ->sLF_AR_shp_Q14) * lf_shp_Q14_high) >> 16)));
    tmp1 = wrap_subtract(wrap_shift_left(LPC_pred_Q10, 2), n_AR_Q12);
    tmp1 = wrap_subtract(tmp1, n_LF_Q12);
    if (lag > 0) {
      n_LTP_Q13 = saturating_left_shift<1>(silk_harmonic_shaping(shp_lag_ptr, HarmShapeFIRPacked_Q14));
      ++shp_lag_ptr;
      tmp1 = rounded_rshift<3>(saturating_add_int32(LTP_pred_Q13 - n_LTP_Q13, wrap_shift_left(tmp1, 1)));
    } else {
      tmp1 = rounded_rshift<2>(tmp1);
    }
    r_Q10 = silk_signed_clamped_residual(x_sc_Q10[i] - tmp1, NSQ->rand_seed);
    const auto candidates = silk_quantize_candidate_pair(r_Q10, Lambda_Q10, offset_Q10);
    const auto best_index = candidates.dist2_Q20 + candidates.rate2_Q20 < candidates.dist1_Q20 + candidates.rate1_Q20;
    const auto sample = silk_nsq_build_sample(best_index == 0 ? candidates.q1_Q10 : candidates.q2_Q10, NSQ->rand_seed,
                                              saturating_left_shift<1>(LTP_pred_Q13), saturating_left_shift<4>(LPC_pred_Q10), x_sc_Q10[i],
                                              saturating_left_shift<2>(n_AR_Q12), saturating_left_shift<2>(n_LF_Q12));
    pulses[i] = delayed_pulse_from_q10(sample.Q_Q10);
    xq[i] = scale_and_saturate_q14<8>(sample.xq_Q14, Gain_Q10);
    psLPC_Q14++;
    *psLPC_Q14 = sample.xq_Q14;
    NSQ->sDiff_shp_Q14 = sample.Diff_Q14;
    NSQ->sLF_AR_shp_Q14 = sample.LF_AR_Q14;
    NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx] = sample.sLTP_shp_Q14;
    sLTP_Q15[NSQ->sLTP_buf_idx] = wrap_shift_left(sample.LPC_exc_Q14, 1);
    NSQ->sLTP_shp_buf_idx++;
    NSQ->sLTP_buf_idx++;
    NSQ->rand_seed = wrap_add(NSQ->rand_seed, pulses[i]);
  }
  copy_n_bytes(&NSQ->sLPC_Q14[length], static_cast<std::size_t>(16 * sizeof(opus_int32)), NSQ->sLPC_Q14);
}

struct NSQ_del_dec_struct {
  opus_int32 sLPC_Q14[(5 * 16) + 16], RandState[40], Q_Q10[40], Xq_Q14[40], Pred_Q15[40], Shape_Q14[40], sAR2_Q14[24], LF_AR_Q14, Diff_Q14,
      Seed, SeedInit, RD_Q10;
};
template <int Shift>
  requires(Shift > 0)
[[nodiscard]] static constexpr auto rounded_rshift_to_int16(opus_int32 value) noexcept -> opus_int16 {
  return saturate_int16_from_int32(rounded_rshift<Shift>(value));
}

template <int Shift>
  requires(Shift > 0)
[[nodiscard]] static constexpr auto rounded_i16_product_shift(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return static_cast<opus_int32>(
      rounded_rshift<Shift>(static_cast<opus_int64>(static_cast<opus_int16>(lhs)) * static_cast<opus_int64>(static_cast<opus_int16>(rhs))));
}

template <int Shift>
  requires(Shift > 0)
[[nodiscard]] static constexpr auto rounded_mul_i16_q16(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return rounded_rshift<Shift>(
      static_cast<opus_int32>((static_cast<opus_int64>(lhs) * static_cast<opus_int64>(static_cast<opus_int16>(rhs))) >> 16));
}

[[nodiscard]] static auto saturating_subtract_int32(opus_int32 lhs, opus_int32 rhs) noexcept -> opus_int32 {
  return saturate_int32(static_cast<opus_int64>(lhs) - rhs);
}

[[nodiscard]] static auto clamped_midpoint(opus_int32 lhs, opus_int32 rhs, opus_int32 bound0, opus_int32 bound1) noexcept -> opus_int32 {
  return clamp_value(rounded_rshift<1>(lhs + rhs), std::min(bound0, bound1), std::max(bound0, bound1));
}

[[nodiscard]] static auto inverse_prediction_step(opus_int32 lhs, opus_int32 rhs, opus_int32 rc_q31, opus_int32 rc_mult2,
                                                  int mult2_q) noexcept -> opus_int64 {
  const auto reflected = saturating_subtract_int32(lhs, static_cast<opus_int32>(rounded_rshift<31>(static_cast<opus_int64>(rhs) * rc_q31)));
  return rounded_rshift(static_cast<opus_int64>(reflected) * rc_mult2, mult2_q);
}

[[nodiscard]] static auto silk_harmonic_shaping(const opus_int32* shp_lag_ptr, opus_int32 HarmShapeFIRPacked_Q14) noexcept -> opus_int32 {
  const auto outer = silk_mul_wb(saturating_add_int32(shp_lag_ptr[0], shp_lag_ptr[-2]), HarmShapeFIRPacked_Q14);
  return saturating_add_int32(outer, silk_mul_wb(shp_lag_ptr[-1], HarmShapeFIRPacked_Q14 >> 16));
}

[[nodiscard]] static auto silk_nsq_build_sample(opus_int32 q_Q10, opus_int32 seed, opus_int32 LTP_pred_Q14, opus_int32 LPC_pred_Q14,
                                                opus_int32 x_Q10, opus_int32 n_AR_Q14, opus_int32 n_LF_Q14) noexcept
    -> silk_nsq_sample_state {
  auto exc_Q14 = wrap_shift_left(q_Q10, 4);
  if (seed < 0) {
    exc_Q14 = -exc_Q14;
  }
  const auto LPC_exc_Q14 = wrap_add(exc_Q14, LTP_pred_Q14), xq_Q14 = wrap_add(LPC_exc_Q14, LPC_pred_Q14),
             Diff_Q14 = wrap_subtract(xq_Q14, wrap_shift_left(x_Q10, 4));
  return {q_Q10,      0,
          xq_Q14,     static_cast<opus_int32>(Diff_Q14 - n_AR_Q14),
          Diff_Q14,   saturating_subtract_int32(static_cast<opus_int32>(Diff_Q14 - n_AR_Q14), n_LF_Q14),
          LPC_exc_Q14};
}

[[nodiscard]] static auto silk_best_delayed_state_index(std::span<const NSQ_del_dec_struct> states) noexcept -> int {
  return static_cast<int>(std::ranges::min_element(states, {}, &NSQ_del_dec_struct::RD_Q10) - states.begin());
}

[[nodiscard]] constexpr auto silk_decision_ring_dec(int index) noexcept -> int {
  return index == 0 ? 39 : index - 1;
}

[[nodiscard]] constexpr auto silk_decision_ring_add(int index, int delay) noexcept -> int {
  const auto wrapped = index + delay;
  return wrapped >= 40 ? wrapped - 40 : wrapped;
}

static void silk_noise_shape_quantizer_del_dec(silk_nsq_state* NSQ, std::span<NSQ_del_dec_struct> psDelDec, int signalType,
                                               std::span<const opus_int32> x_Q10, std::span<opus_int8> pulses, std::span<opus_int16> xq,
                                               std::span<opus_int32> sLTP_Q15, std::span<opus_int32> delayedGain_Q10,
                                               std::span<const opus_int16> a_Q12, std::span<const opus_int16> b_Q14,
                                               std::span<const opus_int16> AR_shp_Q13, int lag, opus_int32 HarmShapeFIRPacked_Q14,
                                               int Tilt_Q14, opus_int32 LF_shp_Q14, opus_int32 Gain_Q16, int Lambda_Q10, int offset_Q10,
                                               int subfr, int warping_Q16, int* smpl_buf_idx, int decisionDelay) {
  int i, k, Winner_ind, RDmin_ind, RDmax_ind, last_smple_idx;
  opus_int32 Winner_rand_state, LTP_pred_Q14, LPC_pred_Q14, n_AR_Q14, n_LTP_Q14, n_LF_Q14, r_Q10, RDmin_Q10, RDmax_Q10, Gain_Q10, tmp1,
      tmp2, *pred_lag_ptr, *shp_lag_ptr, *psLPC_Q14;
  NSQ_del_dec_struct* psDD;
  silk_nsq_sample_state* psSS;
  const auto length = static_cast<int>(x_Q10.size());
  const auto shapingLPCOrder = static_cast<int>(AR_shp_Q13.size());
  const auto nStatesDelayedDecision = static_cast<int>(psDelDec.size());
  std::array<std::array<silk_nsq_sample_state, 2>, silk_max_delayed_decision_states> psSampleState;
  const auto* x_q10_data = x_Q10.data();
  auto* pulses_data = pulses.data();
  auto* xq_data = xq.data();
  auto* sLTP_Q15_data = sLTP_Q15.data();
  auto* delayedGain_Q10_data = delayedGain_Q10.data();
  const auto* b_q14_coefficients = b_Q14.data();
  const auto tilt_Q14_i16 = static_cast<opus_int16>(Tilt_Q14);
  const auto lf_shp_Q14_i16 = static_cast<opus_int16>(LF_shp_Q14);
  const auto lf_shp_Q14_high = static_cast<opus_int64>(LF_shp_Q14) >> 16;
  shp_lag_ptr = &NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - lag + 3 / 2];
  pred_lag_ptr = &sLTP_Q15[NSQ->sLTP_buf_idx - lag + 5 / 2];
  Gain_Q10 = ((Gain_Q16) >> (6));
  for (i = 0; i < length; i++) {
    if (signalType == 2) {
      LTP_pred_Q14 = wrap_shift_left(silk_ltp_prediction_5tap(pred_lag_ptr, b_q14_coefficients), 1);
      ++pred_lag_ptr;
    } else {
      LTP_pred_Q14 = 0;
    }
    if (lag > 0) {
      n_LTP_Q14 =
          saturating_subtract_int32(LTP_pred_Q14, saturating_left_shift<2>(silk_harmonic_shaping(shp_lag_ptr, HarmShapeFIRPacked_Q14)));
      ++shp_lag_ptr;
    } else {
      n_LTP_Q14 = 0;
    }
    for (k = 0; k < nStatesDelayedDecision; k++) {
      psDD = &psDelDec[k];
      psSS = psSampleState[k].data();
      psDD->Seed = silk_next_rand_seed(psDD->Seed);
      psLPC_Q14 = &psDD->sLPC_Q14[16 - 1 + i];
      LPC_pred_Q14 = saturating_left_shift<4>(silk_lpc_prediction_q10(psLPC_Q14 + 1, a_Q12.data(), static_cast<int>(a_Q12.size())));
      n_AR_Q14 = warping_Q16 == 0
                     ? silk_nsq_noise_shape_feedback<false>(psDD->Diff_Q14, psDD->sAR2_Q14, AR_shp_Q13.data(), shapingLPCOrder)
                     : silk_nsq_noise_shape_feedback<true>(psDD->Diff_Q14, psDD->sAR2_Q14, AR_shp_Q13.data(), shapingLPCOrder, warping_Q16);
      n_AR_Q14 = (static_cast<opus_int32>((n_AR_Q14) + (((psDD->LF_AR_Q14) * static_cast<opus_int64>(tilt_Q14_i16)) >> 16)));
      n_AR_Q14 = wrap_shift_left(n_AR_Q14, 2);
      n_LF_Q14 = (static_cast<opus_int32>(((psDD->Shape_Q14[*smpl_buf_idx]) * static_cast<opus_int64>(lf_shp_Q14_i16)) >> 16));
      n_LF_Q14 = (static_cast<opus_int32>((n_LF_Q14) + (((psDD->LF_AR_Q14) * lf_shp_Q14_high) >> 16)));
      n_LF_Q14 = wrap_shift_left(n_LF_Q14, 2);
      tmp1 = saturating_add_int32(n_AR_Q14, n_LF_Q14);
      tmp2 = wrap_add(n_LTP_Q14, LPC_pred_Q14);
      tmp1 = saturating_subtract_int32(tmp2, tmp1);
      tmp1 = rounded_rshift<4>(tmp1);
      r_Q10 = silk_signed_clamped_residual(x_q10_data[i] - tmp1, psDD->Seed);
      const auto candidates = silk_quantize_candidate_pair(r_Q10, Lambda_Q10, offset_Q10);
      const auto candidate0 = static_cast<opus_int32>((candidates.rate1_Q20 + candidates.dist1_Q20) >> 10);
      const auto candidate1 = static_cast<opus_int32>((candidates.rate2_Q20 + candidates.dist2_Q20) >> 10);
      const auto first_is_q0 = candidate0 < candidate1;
      psSS[0] = silk_nsq_build_sample(first_is_q0 ? candidates.q1_Q10 : candidates.q2_Q10, psDD->Seed, LTP_pred_Q14, LPC_pred_Q14,
                                      x_q10_data[i], n_AR_Q14, n_LF_Q14);
      psSS[1] = silk_nsq_build_sample(first_is_q0 ? candidates.q2_Q10 : candidates.q1_Q10, psDD->Seed, LTP_pred_Q14, LPC_pred_Q14,
                                      x_q10_data[i], n_AR_Q14, n_LF_Q14);
      psSS[0].RD_Q10 = psDD->RD_Q10 + (first_is_q0 ? candidate0 : candidate1);
      psSS[1].RD_Q10 = psDD->RD_Q10 + (first_is_q0 ? candidate1 : candidate0);
    }
    *smpl_buf_idx = silk_decision_ring_dec(*smpl_buf_idx);
    last_smple_idx = silk_decision_ring_add(*smpl_buf_idx, decisionDelay);
    RDmin_Q10 = psSampleState[0][0].RD_Q10;
    Winner_ind = 0;
    for (k = 1; k < nStatesDelayedDecision; k++) {
      if (psSampleState[k][0].RD_Q10 < RDmin_Q10) {
        RDmin_Q10 = psSampleState[k][0].RD_Q10;
        Winner_ind = k;
      }
    }
    Winner_rand_state = psDelDec[Winner_ind].RandState[last_smple_idx];
    for (k = 0; k < nStatesDelayedDecision; k++) {
      if (psDelDec[k].RandState[last_smple_idx] != Winner_rand_state) {
        psSampleState[k][0].RD_Q10 = ((psSampleState[k][0].RD_Q10) + (0x7FFFFFFF >> 4));
        psSampleState[k][1].RD_Q10 = ((psSampleState[k][1].RD_Q10) + (0x7FFFFFFF >> 4));
      }
    }
    RDmax_Q10 = psSampleState[0][0].RD_Q10;
    RDmin_Q10 = psSampleState[0][1].RD_Q10;
    RDmax_ind = 0;
    RDmin_ind = 0;
    for (k = 1; k < nStatesDelayedDecision; k++) {
      if (psSampleState[k][0].RD_Q10 > RDmax_Q10) {
        RDmax_Q10 = psSampleState[k][0].RD_Q10;
        RDmax_ind = k;
      }
      if (psSampleState[k][1].RD_Q10 < RDmin_Q10) {
        RDmin_Q10 = psSampleState[k][1].RD_Q10;
        RDmin_ind = k;
      }
    }
    if (RDmin_Q10 < RDmax_Q10) {
      const auto offset = static_cast<std::size_t>(i) * sizeof(opus_int32);
      copy_n_bytes(reinterpret_cast<const std::byte*>(&psDelDec[RDmin_ind]) + offset, sizeof(NSQ_del_dec_struct) - offset,
                   reinterpret_cast<std::byte*>(&psDelDec[RDmax_ind]) + offset);
      psSampleState[RDmax_ind][0] = psSampleState[RDmin_ind][1];
    }
    psDD = &psDelDec[Winner_ind];
    if (subfr > 0 || i >= decisionDelay) {
      const auto output_index = static_cast<std::size_t>(subfr > 0 ? i : i - decisionDelay);
      pulses_data[output_index] = delayed_pulse_from_q10(psDD->Q_Q10[last_smple_idx]);
      xq_data[output_index] = scale_and_saturate_q14<8>(psDD->Xq_Q14[last_smple_idx], delayedGain_Q10_data[last_smple_idx]);
      NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - decisionDelay] = psDD->Shape_Q14[last_smple_idx];
      sLTP_Q15_data[NSQ->sLTP_buf_idx - decisionDelay] = psDD->Pred_Q15[last_smple_idx];
    }
    NSQ->sLTP_shp_buf_idx++;
    NSQ->sLTP_buf_idx++;
    for (k = 0; k < nStatesDelayedDecision; k++) {
      psDD = &psDelDec[k];
      psSS = &psSampleState[k][0];
      psDD->LF_AR_Q14 = psSS->LF_AR_Q14;
      psDD->Diff_Q14 = psSS->Diff_Q14;
      psDD->sLPC_Q14[16 + i] = psSS->xq_Q14;
      psDD->Xq_Q14[*smpl_buf_idx] = psSS->xq_Q14;
      psDD->Q_Q10[*smpl_buf_idx] = psSS->Q_Q10;
      psDD->Pred_Q15[*smpl_buf_idx] = wrap_shift_left(psSS->LPC_exc_Q14, 1);
      psDD->Shape_Q14[*smpl_buf_idx] = psSS->sLTP_shp_Q14;
      psDD->Seed = wrap_add(psDD->Seed, rounded_rshift<10>(psSS->Q_Q10));
      psDD->RandState[*smpl_buf_idx] = psDD->Seed;
      psDD->RD_Q10 = psSS->RD_Q10;
    }
    delayedGain_Q10_data[*smpl_buf_idx] = Gain_Q10;
  }
  for (k = 0; k < nStatesDelayedDecision; k++) {
    psDD = &psDelDec[k];
    copy_n_bytes(&psDD->sLPC_Q14[length], static_cast<std::size_t>(16 * sizeof(opus_int32)), psDD->sLPC_Q14);
  }
}

static void silk_nsq_del_dec_scale_states(const silk_encoder_state* psEncC, silk_nsq_state* NSQ, std::span<NSQ_del_dec_struct> psDelDec,
                                          std::span<const opus_int16> x16, std::span<opus_int32> x_sc_Q10, std::span<const opus_int16> sLTP,
                                          std::span<opus_int32> sLTP_Q15, int subfr, const int LTP_scale_Q14,
                                          std::span<const opus_int32> Gains_Q16, std::span<const int> pitchL, const int signal_type,
                                          const int decisionDelay) {
  const auto gain_adj_Q16 = silk_nsq_scale_common(psEncC, NSQ, x16, x_sc_Q10, sLTP, sLTP_Q15, subfr, LTP_scale_Q14, Gains_Q16, pitchL,
                                                  signal_type, NSQ->sLTP_buf_idx - decisionDelay);
  if (gain_adj_Q16 != (static_cast<opus_int32>(1) << 16)) {
    for (auto& state : psDelDec) {
      state.LF_AR_Q14 = multiply_q16(gain_adj_Q16, state.LF_AR_Q14);
      state.Diff_Q14 = multiply_q16(gain_adj_Q16, state.Diff_Q14);
      scale_q16_buffer(state.sLPC_Q14, 16, gain_adj_Q16);
      scale_q16_buffer(state.sAR2_Q14, 24, gain_adj_Q16);
      scale_q16_buffer(state.Pred_Q15, 40, gain_adj_Q16);
      scale_q16_buffer(state.Shape_Q14, 40, gain_adj_Q16);
    }
  }
}

template <bool Delayed>
static void silk_NSQ(const silk_encoder_state* psEncC, silk_nsq_state* NSQ, SideInfoIndices* psIndices, const opus_int16 x16[],
                     opus_int8 pulses[], const opus_int16* PredCoef_Q12, const opus_int16 LTPCoef_Q14[5 * 4],
                     const opus_int16 AR_Q13[4 * 24], const int HarmShapeGain_Q14[4], const int Tilt_Q14[4], const opus_int32 LF_shp_Q14[4],
                     const opus_int32 Gains_Q16[4], const int pitchL[4], const int Lambda_Q10, const int LTP_scale_Q14) {
  int lag = NSQ->lagPrev;
  std::array<NSQ_del_dec_struct, silk_max_delayed_decision_states> psDelDec_storage{};
  auto delayed_states = std::span{psDelDec_storage}.first(static_cast<std::size_t>(psEncC->nStatesDelayedDecision));
  auto* psDelDec = delayed_states.data();
  if constexpr (Delayed) {
    for (int k = 0; k < psEncC->nStatesDelayedDecision; ++k) {
      auto& state = psDelDec[k];
      state.Seed = (k + psIndices->Seed) & 3;
      state.SeedInit = state.Seed;
      state.RD_Q10 = 0;
      state.LF_AR_Q14 = NSQ->sLF_AR_shp_Q14;
      state.Diff_Q14 = NSQ->sDiff_shp_Q14;
      state.Shape_Q14[0] = NSQ->sLTP_shp_Q14[psEncC->ltp_mem_length - 1];
      copy_n_bytes(NSQ->sLPC_Q14, static_cast<std::size_t>(16 * sizeof(opus_int32)), state.sLPC_Q14);
      copy_n_bytes(NSQ->sAR2_Q14, static_cast<std::size_t>(sizeof(NSQ->sAR2_Q14)), state.sAR2_Q14);
    }
  } else {
    NSQ->rand_seed = psIndices->Seed;
  }
  const int offset_Q10 = silk_Quantization_Offsets_Q10[psIndices->signalType >> 1][psIndices->quantOffsetType];
  int smpl_buf_idx = 0;
  int decisionDelay = std::min(40, psEncC->subfr_length);
  if constexpr (Delayed) {
    if (psIndices->signalType == 2) {
      for (int k = 0; k < psEncC->nb_subfr; ++k) {
        decisionDelay = std::min(decisionDelay, pitchL[k] - 5 / 2 - 1);
      }
    } else if (lag > 0) {
      decisionDelay = std::min(decisionDelay, lag - 5 / 2 - 1);
    }
  }
  const int LSF_interpolation_flag = psIndices->NLSFInterpCoef_Q2 != 4;
  const auto ltp_frame_storage = static_cast<std::size_t>(psEncC->ltp_mem_length + psEncC->frame_length);
  std::array<opus_int32, silk_max_ltp_buffer_length> ltp_q15_buffer;
  std::array<opus_int16, silk_max_ltp_buffer_length> ltp_buffer;
  auto* sLTP_Q15_storage = ltp_q15_buffer.data();
  auto* sLTP_storage = ltp_buffer.data();
  auto* sLTP = sLTP_storage;
  auto* x_sc_Q10_storage = sLTP_Q15_storage + ltp_frame_storage - psEncC->subfr_length;
  std::array<opus_int32, 40> delayedGain_Q10;
  auto* pxq = &NSQ->xq[psEncC->ltp_mem_length];
  NSQ->sLTP_shp_buf_idx = psEncC->ltp_mem_length;
  NSQ->sLTP_buf_idx = psEncC->ltp_mem_length;
  int subfr = 0;
  for (int k = 0; k < psEncC->nb_subfr; ++k) {
    const auto* A_Q12 = &PredCoef_Q12[((k >> 1) | (1 - LSF_interpolation_flag)) * 16];
    const auto* B_Q14 = &LTPCoef_Q14[k * 5];
    const auto* AR_shp_Q13 = &AR_Q13[k * 24];
    const opus_int32 HarmShapeFIRPacked_Q14 = silk_pack_harm_shape_gain(HarmShapeGain_Q14[k]);
    NSQ->rewhite_flag = 0;
    if (psIndices->signalType == 2) {
      lag = pitchL[k];
      if ((k & (3 - (LSF_interpolation_flag << 1))) == 0) {
        if constexpr (Delayed) {
          if (k == 2) {
            const int Winner_ind = silk_best_delayed_state_index(delayed_states);
            for (int i = 0; i < psEncC->nStatesDelayedDecision; ++i) {
              if (i != Winner_ind) {
                psDelDec[i].RD_Q10 += (0x7FFFFFFF >> 4);
              }
            }
            const auto* psDD = &psDelDec[Winner_ind];
            int last_smple_idx = silk_decision_ring_add(smpl_buf_idx, decisionDelay);
            for (int i = 0; i < decisionDelay; ++i) {
              last_smple_idx = silk_decision_ring_dec(last_smple_idx);
              pulses[i - decisionDelay] = delayed_pulse_from_q10(psDD->Q_Q10[last_smple_idx]);
              pxq[i - decisionDelay] = scale_and_saturate_q14<14>(psDD->Xq_Q14[last_smple_idx], Gains_Q16[1]);
              NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - decisionDelay + i] = psDD->Shape_Q14[last_smple_idx];
            }
            subfr = 0;
          }
        }
        const int start_idx = psEncC->ltp_mem_length - lag - psEncC->predictLPCOrder - 5 / 2;
        silk_LPC_analysis_filter(&sLTP[start_idx], &NSQ->xq[start_idx + k * psEncC->subfr_length], A_Q12,
                                 psEncC->ltp_mem_length - start_idx, psEncC->predictLPCOrder);
        NSQ->sLTP_buf_idx = psEncC->ltp_mem_length;
        NSQ->rewhite_flag = 1;
      }
    }
    if constexpr (Delayed) {
      silk_nsq_del_dec_scale_states(psEncC, NSQ, delayed_states, {x16, static_cast<std::size_t>(psEncC->subfr_length)},
                                    {x_sc_Q10_storage, static_cast<std::size_t>(psEncC->subfr_length)}, {sLTP_storage, ltp_frame_storage},
                                    {sLTP_Q15_storage, ltp_frame_storage}, k, LTP_scale_Q14, {Gains_Q16, 4}, {pitchL, 4},
                                    psIndices->signalType, decisionDelay);
      const auto delayed_output_prefix = subfr > 0 ? decisionDelay : 0;
      silk_noise_shape_quantizer_del_dec(
          NSQ, delayed_states, psIndices->signalType, {x_sc_Q10_storage, static_cast<std::size_t>(psEncC->subfr_length)},
          {pulses - delayed_output_prefix, static_cast<std::size_t>(psEncC->subfr_length + delayed_output_prefix)},
          {pxq - delayed_output_prefix, static_cast<std::size_t>(psEncC->subfr_length + delayed_output_prefix)},
          {sLTP_Q15_storage, ltp_frame_storage}, delayedGain_Q10, {A_Q12, static_cast<std::size_t>(psEncC->predictLPCOrder)}, {B_Q14, 5},
          {AR_shp_Q13, static_cast<std::size_t>(psEncC->shapingLPCOrder)}, lag, HarmShapeFIRPacked_Q14, Tilt_Q14[k], LF_shp_Q14[k],
          Gains_Q16[k], Lambda_Q10, offset_Q10, subfr++, psEncC->warping_Q16, &smpl_buf_idx, decisionDelay);
    } else {
      static_cast<void>(silk_nsq_scale_common(psEncC, NSQ, {x16, static_cast<std::size_t>(psEncC->subfr_length)},
                                              {x_sc_Q10_storage, static_cast<std::size_t>(psEncC->subfr_length)},
                                              {sLTP_storage, ltp_frame_storage}, {sLTP_Q15_storage, ltp_frame_storage}, k, LTP_scale_Q14,
                                              {Gains_Q16, 4}, {pitchL, 4}, psIndices->signalType, NSQ->sLTP_buf_idx));
      silk_noise_shape_quantizer(NSQ, psIndices->signalType, {x_sc_Q10_storage, static_cast<std::size_t>(psEncC->subfr_length)},
                                 {pulses, static_cast<std::size_t>(psEncC->subfr_length)},
                                 {pxq, static_cast<std::size_t>(psEncC->subfr_length)}, {sLTP_Q15_storage, ltp_frame_storage},
                                 {A_Q12, static_cast<std::size_t>(psEncC->predictLPCOrder)}, {B_Q14, 5},
                                 {AR_shp_Q13, static_cast<std::size_t>(psEncC->shapingLPCOrder)}, lag, HarmShapeFIRPacked_Q14, Tilt_Q14[k],
                                 LF_shp_Q14[k], Gains_Q16[k], Lambda_Q10, offset_Q10);
    }
    x16 += psEncC->subfr_length;
    pulses += psEncC->subfr_length;
    pxq += psEncC->subfr_length;
  }
  if constexpr (Delayed) {
    const int Winner_ind = silk_best_delayed_state_index(delayed_states);
    const auto* psDD = &psDelDec[Winner_ind];
    psIndices->Seed = psDD->SeedInit;
    int last_smple_idx = silk_decision_ring_add(smpl_buf_idx, decisionDelay);
    const opus_int32 Gain_Q10 = Gains_Q16[psEncC->nb_subfr - 1] >> 6;
    for (int i = 0; i < decisionDelay; ++i) {
      last_smple_idx = silk_decision_ring_dec(last_smple_idx);
      pulses[i - decisionDelay] = delayed_pulse_from_q10(psDD->Q_Q10[last_smple_idx]);
      pxq[i - decisionDelay] = scale_and_saturate_q14<8>(psDD->Xq_Q14[last_smple_idx], Gain_Q10);
      NSQ->sLTP_shp_Q14[NSQ->sLTP_shp_buf_idx - decisionDelay + i] = psDD->Shape_Q14[last_smple_idx];
    }
    copy_n_bytes(&psDD->sLPC_Q14[psEncC->subfr_length], static_cast<std::size_t>(16 * sizeof(opus_int32)), NSQ->sLPC_Q14);
    copy_n_bytes(psDD->sAR2_Q14, static_cast<std::size_t>(sizeof(psDD->sAR2_Q14)), NSQ->sAR2_Q14);
    NSQ->sLF_AR_shp_Q14 = psDD->LF_AR_Q14;
    NSQ->sDiff_shp_Q14 = psDD->Diff_Q14;
  }
  NSQ->lagPrev = pitchL[psEncC->nb_subfr - 1];
  silk_finish_nsq(psEncC, NSQ);
}
[[nodiscard]] static int stable_pitch_average(const std::array<opus_uint16, 3>& history) noexcept {
  int sum = 0;
  int minimum = opus_int32_max;
  int maximum = 0;
  for (const int pitch : history) {
    if (pitch == 0) {
      return 0;
    }
    sum += pitch;
    minimum = std::min(minimum, pitch);
    maximum = std::max(maximum, pitch);
  }
  const int average = (sum + 1) / 3;
  return (maximum - minimum) * 32 <= average ? average : 0;
}

static void silk_PLC_update(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl);
static void silk_PLC_conceal(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl, std::span<opus_int16> frame);
void silk_PLC_Reset(silk_decoder_state* psDec) {
  psDec->sPLC.pitchL_Q8 = wrap_shift_left(psDec->frame_length, 8 - 1);
  psDec->sPLC.prevGain_Q16[0] = 1 << 16;
  psDec->sPLC.prevGain_Q16[1] = 1 << 16;
  psDec->sPLC.pitch_history.fill(0);
  psDec->sPLC.pitch_history_index = 0;
  psDec->sPLC.subfr_length = 20;
  psDec->sPLC.nb_subfr = 2;
}

void silk_PLC(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl, std::span<opus_int16> frame, int lost) {
  if (lost) {
    silk_PLC_conceal(psDec, psDecCtrl, frame);
    psDec->lossCnt++;
  } else {
    silk_PLC_update(psDec, psDecCtrl);
  }
}

static void silk_PLC_update(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl) {
  opus_int32 LTP_Gain_Q14, temp_LTP_Gain_Q14;
  int i, j;
  auto* psPLC = &psDec->sPLC;
  psDec->prevSignalType = psDec->indices.signalType;
  LTP_Gain_Q14 = 0;
  if (psDec->indices.signalType == 2) {
    for (j = 0; j * psDec->subfr_length < psDecCtrl->pitchL[psDec->nb_subfr - 1]; j++) {
      if (j == psDec->nb_subfr) {
        break;
      }
      temp_LTP_Gain_Q14 = 0;
      for (i = 0; i < 5; i++) {
        temp_LTP_Gain_Q14 += psDecCtrl->LTPCoef_Q14[static_cast<std::size_t>((psDec->nb_subfr - 1 - j) * 5 + i)];
      }
      if (temp_LTP_Gain_Q14 > LTP_Gain_Q14) {
        LTP_Gain_Q14 = temp_LTP_Gain_Q14;
        psPLC->pitchL_Q8 = wrap_shift_left(psDecCtrl->pitchL[psDec->nb_subfr - 1 - j], 8);
      }
    }
    psPLC->LTPCoef_Q14 = static_cast<opus_int16>(LTP_Gain_Q14);
    if (LTP_Gain_Q14 < 11469) {
      const int scale_Q10 = (11469 << 10) / std::max(LTP_Gain_Q14, 1);
      psPLC->LTPCoef_Q14 = silk_mul_i16_shift<10>(psPLC->LTPCoef_Q14, scale_Q10);
    } else if (LTP_Gain_Q14 > 15565) {
      const int scale_Q14 = (15565 << 14) / std::max(LTP_Gain_Q14, 1);
      psPLC->LTPCoef_Q14 = silk_mul_i16_shift<14>(psPLC->LTPCoef_Q14, scale_Q14);
    }
  } else {
    psPLC->pitchL_Q8 = wrap_shift_left(18 * psDec->fs_kHz, 8);
    psPLC->LTPCoef_Q14 = 0;
  }
  copy_n_bytes(psDecCtrl->PredCoef_Q12[1], static_cast<std::size_t>(psDec->LPC_order * sizeof(opus_int16)), psPLC->prevLPC_Q12);
  psPLC->prevLTP_scale_Q14 = psDecCtrl->LTP_scale_Q14;
  copy_n_bytes(psDecCtrl->Gains_Q16 + psDec->nb_subfr - 2, static_cast<std::size_t>(2 * sizeof(opus_int32)), psPLC->prevGain_Q16);
  psPLC->subfr_length = psDec->subfr_length;
  psPLC->nb_subfr = psDec->nb_subfr;
  psPLC->pitch_history[static_cast<std::size_t>(psPLC->pitch_history_index)] =
      psDec->indices.signalType == 2 ? static_cast<opus_uint16>(rounded_rshift<8>(psPLC->pitchL_Q8)) : 0;
  psPLC->pitch_history_index = (psPLC->pitch_history_index + 1) & 3;
}

static void silk_PLC_energy(opus_int32* energy1, int* shift1, opus_int32* energy2, int* shift2, std::span<const opus_int32> exc_Q14,
                            std::span<const opus_int32> prevGain_Q10, int subfr_length, int nb_subfr) {
  int i, k;
  opus_int16 exc_buf[2 * silk_max_subfr_length];
  auto* exc_buf_ptr = exc_buf;
  for (k = 0; k < 2; k++) {
    for (i = 0; i < subfr_length; i++) {
      exc_buf_ptr[i] = scale_and_saturate_q14<8>(exc_Q14[i + (k + nb_subfr - 2) * subfr_length], prevGain_Q10[k]);
    }
    exc_buf_ptr += subfr_length;
  }
  silk_sum_sqr_shift(energy1, shift1, exc_buf, subfr_length);
  silk_sum_sqr_shift(energy2, shift2, exc_buf + subfr_length, subfr_length);
}

static void silk_PLC_conceal(silk_decoder_state* psDec, silk_decoder_control* psDecCtrl, std::span<opus_int16> frame) {
  opus_int16 A_Q12[16];
  silk_PLC_struct* psPLC = &psDec->sPLC;
  const opus_int32 prevGain_Q10[2]{((psPLC->prevGain_Q16[0]) >> (6)), ((psPLC->prevGain_Q16[1]) >> (6))};
  std::array<opus_int32, silk_max_ltp_buffer_length> sLTP_Q14{};
  std::array<opus_int16, silk_max_ltp_mem_length> sLTP{};
  if (psDec->first_frame_after_reset) {
    zero_n_items(psPLC->prevLPC_Q12, static_cast<std::size_t>(16));
  }
  if (psDec->lossCnt == 0 && psDec->prevSignalType == 2) {
    const auto next = static_cast<std::size_t>(psPLC->pitch_history_index);
    const std::array<opus_uint16, 3> previous_pitch{psPLC->pitch_history[next], psPLC->pitch_history[(next + 1) & 3],
                                                    psPLC->pitch_history[(next + 2) & 3]};
    const int history_pitch = stable_pitch_average(previous_pitch);
    const int current_pitch = rounded_rshift<8>(psPLC->pitchL_Q8);
    const int pitch_delta = std::abs(history_pitch - current_pitch);
    if (history_pitch > 0 && pitch_delta * 4 >= current_pitch && pitch_delta <= current_pitch) {
      psPLC->pitchL_Q8 = static_cast<opus_int32>(history_pitch << 8);
    }
  }
  opus_int32 energy1;
  opus_int32 energy2;
  int shift1;
  int shift2;
  silk_PLC_energy(&energy1, &shift1, &energy2, &shift2, {psDec->exc_Q14, static_cast<std::size_t>(psDec->subfr_length * psDec->nb_subfr)},
                  prevGain_Q10, psDec->subfr_length, psDec->nb_subfr);
  opus_int32* rand_ptr = ((energy1) >> (shift2)) < ((energy2) >> (shift1))
                             ? &psDec->exc_Q14[std::max(0, (psPLC->nb_subfr - 1) * psPLC->subfr_length - 128)]
                             : &psDec->exc_Q14[std::max(0, psPLC->nb_subfr * psPLC->subfr_length - 128)];
  opus_int16 rand_scale_Q14 = psPLC->randScale_Q14;
  const opus_int32 harm_Gain_Q15 = psDec->lossCnt == 0 ? 31948 : 28672;
  opus_int32 rand_Gain_Q15 = psDec->prevSignalType == 2 ? psDec->lossCnt == 0 ? 31130 : 26214 : psDec->lossCnt == 0 ? 32440 : 29491;
  silk_bwexpander(psPLC->prevLPC_Q12, static_cast<std::size_t>(psDec->LPC_order), fixed_q<16>(0.99));
  copy_n_items(psPLC->prevLPC_Q12, static_cast<std::size_t>(psDec->LPC_order), A_Q12);
  if (psDec->lossCnt == 0) {
    rand_scale_Q14 = 1 << 14;
    if (psDec->prevSignalType == 2) {
      rand_scale_Q14 -= psPLC->LTPCoef_Q14;
      rand_scale_Q14 = std::max(static_cast<opus_int16>(3277), rand_scale_Q14);
      rand_scale_Q14 = static_cast<opus_int16>(silk_mul_i16_shift<14>(rand_scale_Q14, psPLC->prevLTP_scale_Q14));
    } else {
      opus_int32 invGain_Q30, down_scale_Q30;
      invGain_Q30 = silk_LPC_inverse_pred_gain_c(psPLC->prevLPC_Q12, psDec->LPC_order);
      down_scale_Q30 = std::min(((static_cast<opus_int32>(1) << 30) >> (3)), invGain_Q30);
      down_scale_Q30 = std::max(((static_cast<opus_int32>(1) << 30) >> (8)), down_scale_Q30);
      down_scale_Q30 = wrap_shift_left(down_scale_Q30, 3);
      rand_Gain_Q15 = silk_mul_wb(down_scale_Q30, rand_Gain_Q15) >> 14;
    }
  }
  opus_int32 rand_seed = psPLC->rand_seed;
  int lag = rounded_rshift<8>(psPLC->pitchL_Q8);
  int sLTP_buf_idx = psDec->ltp_mem_length;
  int idx = psDec->ltp_mem_length - lag - psDec->LPC_order - 5 / 2;
  silk_LPC_analysis_filter(&sLTP[idx], &psDec->outBuf[idx], A_Q12, psDec->ltp_mem_length - idx, psDec->LPC_order);
  opus_int32 inv_gain_Q30 = silk_INVERSE32_varQ(psPLC->prevGain_Q16[1], 46);
  inv_gain_Q30 = std::min(inv_gain_Q30, std::numeric_limits<opus_int32>::max() >> 1);
  for (int i = idx + psDec->LPC_order; i < psDec->ltp_mem_length; i++) {
    sLTP_Q14[i] = silk_mul_wb(inv_gain_Q30, sLTP[i]);
  }
  for (int k = 0; k < psDec->nb_subfr; k++) {
    auto* pred_lag_ptr = &sLTP_Q14[sLTP_buf_idx - lag + 5 / 2];
    for (int i = 0; i < psDec->subfr_length; i++) {
      const opus_int32 LTP_pred_Q12 =
          static_cast<opus_int32>(2 + ((pred_lag_ptr[-2] * static_cast<opus_int64>(static_cast<opus_int16>(psPLC->LTPCoef_Q14))) >> 16));
      ++pred_lag_ptr;
      rand_seed = silk_next_rand_seed(rand_seed);
      idx = ((rand_seed) >> (25)) & (128 - 1);
      sLTP_Q14[sLTP_buf_idx] = wrap_shift_left(silk_mla_wb(LTP_pred_Q12, rand_ptr[idx], rand_scale_Q14), 2);
      ++sLTP_buf_idx;
    }
    psPLC->LTPCoef_Q14 = silk_mul_i16_shift<15>(harm_Gain_Q15, psPLC->LTPCoef_Q14);
    rand_scale_Q14 = silk_mul_i16_shift<15>(rand_scale_Q14, rand_Gain_Q15);
    psPLC->pitchL_Q8 = silk_mla_wb(psPLC->pitchL_Q8, psPLC->pitchL_Q8, 655);
    psPLC->pitchL_Q8 = std::min(psPLC->pitchL_Q8, wrap_shift_left(18 * psDec->fs_kHz, 8));
    lag = rounded_rshift<8>(psPLC->pitchL_Q8);
  }
  auto* sLPC_Q14_ptr = &sLTP_Q14[psDec->ltp_mem_length - 16];
  copy_n_bytes(psDec->sLPC_Q14_buf, static_cast<std::size_t>(16 * sizeof(opus_int32)), sLPC_Q14_ptr);
  for (int i = 0; i < psDec->frame_length; i++) {
    const opus_int32 LPC_pred_Q10 = silk_lpc_prediction_q10(sLPC_Q14_ptr + 16 + i, A_Q12, psDec->LPC_order);
    sLPC_Q14_ptr[16 + i] = saturating_add_int32(sLPC_Q14_ptr[16 + i], saturating_left_shift<4>(LPC_pred_Q10));
    frame[i] = scale_and_saturate_q14<8>(sLPC_Q14_ptr[16 + i], prevGain_Q10[1]);
  }
  copy_n_bytes(&sLPC_Q14_ptr[psDec->frame_length], static_cast<std::size_t>(16 * sizeof(opus_int32)), psDec->sLPC_Q14_buf);
  psPLC->rand_seed = rand_seed;
  psPLC->randScale_Q14 = rand_scale_Q14;
  std::fill_n(psDecCtrl->pitchL, static_cast<std::size_t>(4), lag);
}

void silk_PLC_glue_frames(silk_decoder_state* psDec, std::span<opus_int16> frame) {
  int energy_shift;
  opus_int32 energy;
  const auto length = static_cast<int>(frame.size());
  auto* psPLC = &psDec->sPLC;
  if (psDec->lossCnt) {
    silk_sum_sqr_shift(&psPLC->conc_energy, &psPLC->conc_energy_shift, frame.data(), length);
    psPLC->last_frame_lost = 1;
  } else {
    if (psDec->sPLC.last_frame_lost) {
      silk_sum_sqr_shift(&energy, &energy_shift, frame.data(), length);
      if (energy_shift > psPLC->conc_energy_shift) {
        psPLC->conc_energy = ((psPLC->conc_energy) >> (energy_shift - psPLC->conc_energy_shift));
      } else if (energy_shift < psPLC->conc_energy_shift) {
        energy = ((energy) >> (psPLC->conc_energy_shift - energy_shift));
      }
      if (energy > psPLC->conc_energy) {
        opus_int32 LZ = silk_CLZ32(psPLC->conc_energy);
        LZ = LZ - 1;
        psPLC->conc_energy = wrap_shift_left(psPLC->conc_energy, LZ);
        energy = ((energy) >> (std::max(24 - LZ, 0)));
        const opus_int32 frac_Q24 = psPLC->conc_energy / std::max(energy, 1);
        opus_int32 gain_Q16 = wrap_shift_left(silk_SQRT_APPROX(frac_Q24), 4);
        opus_int32 slope_Q16 = (static_cast<opus_int32>(((static_cast<opus_int32>(1) << 16) - gain_Q16) / (length)));
        slope_Q16 = wrap_shift_left(slope_Q16, 2);
        for (int i = 0; i < length; i++) {
          frame[i] = (static_cast<opus_int32>(((gain_Q16) * static_cast<opus_int64>(static_cast<opus_int16>(frame[i]))) >> 16));
          gain_Q16 += slope_Q16;
          if (gain_Q16 > static_cast<opus_int32>(1) << 16) {
            break;
          }
        }
      }
    }
    psPLC->last_frame_lost = 0;
  }
}

template <bool Encode, typename Pulse> static void silk_shell_code_node(ec_ctx* coder, std::span<Pulse> pulses, const int total_pulses) {
  if (pulses.size() <= 1) {
    if constexpr (!Encode) {
      pulses.front() = static_cast<opus_int16>(total_pulses);
    }
    return;
  }
  const auto half = pulses.size() / 2;
  auto left = 0;
  if constexpr (Encode) {
    for (const int pulse : pulses.first(half)) {
      left += pulse;
    }
  }
  if (total_pulses > 0) {
    const std::span<const opus_uint8> table = pulses.size() == 2   ? silk_shell_code_table0
                                              : pulses.size() == 4 ? silk_shell_code_table1
                                              : pulses.size() == 8 ? silk_shell_code_table2
                                                                   : silk_shell_code_table3;
    left = silk_index_symbol<Encode>(coder, left, table.data() + silk_shell_code_table_offsets[static_cast<std::size_t>(total_pulses)]);
  }
  silk_shell_code_node<Encode>(coder, pulses.first(half), left);
  silk_shell_code_node<Encode>(coder, pulses.last(half), total_pulses - left);
}

constexpr std::array<opus_int32, 4> silk_vad_noise_level_bias{50, 25, 16, 12};

static void silk_VAD_GetNoiseLevels(std::span<const opus_int32, 4> pX, silk_VAD_state* psSilk_VAD);
static void silk_VAD_Init(silk_VAD_state* psSilk_VAD) {
  zero_object(*psSilk_VAD);
  for (int b = 0; b < 4; ++b) {
    psSilk_VAD->NL[b] = 100 * silk_vad_noise_level_bias[b];
    psSilk_VAD->inv_NL[b] = static_cast<opus_int32>(0x7FFFFFFF / psSilk_VAD->NL[b]);
    psSilk_VAD->NrgRatioSmth_Q8[b] = 25600;
  }
  psSilk_VAD->counter = 15;
}

constexpr std::array<opus_int16, 4> tiltWeights{30000, 6000, -12000, -12000};
static void silk_encode_do_VAD(silk_encoder_state* psEncC) {
  auto* psSilk_VAD = &psEncC->sVAD;
  const auto* pIn = psEncC->inputBuf + 1;
  const int decimated_framelength1 = ((psEncC->frame_length) >> (1));
  const int decimated_framelength2 = ((psEncC->frame_length) >> (2));
  int decimated_framelength = ((psEncC->frame_length) >> (3));
  const std::array<int, 4> X_offset{0, decimated_framelength + decimated_framelength2, 2 * decimated_framelength + decimated_framelength2,
                                    2 * (decimated_framelength + decimated_framelength2)};
  std::array<opus_int16, silk_vad_max_work_samples> X_storage;
  auto* X = X_storage.data();
  silk_ana_filt_bank_1(pIn, psSilk_VAD->AnaState.data(), X, &X[X_offset[3]], psEncC->frame_length);
  silk_ana_filt_bank_1(X, psSilk_VAD->AnaState1.data(), X, &X[X_offset[2]], decimated_framelength1);
  silk_ana_filt_bank_1(X, psSilk_VAD->AnaState2.data(), X, &X[X_offset[1]], decimated_framelength2);
  X[decimated_framelength - 1] = ((X[decimated_framelength - 1]) >> (1));
  const opus_int16 HPstateTmp = X[decimated_framelength - 1];
  for (int i = decimated_framelength - 1; i > 0; i--) {
    X[i - 1] = ((X[i - 1]) >> (1));
    X[i] -= X[i - 1];
  }
  X[0] -= psSilk_VAD->HPstate;
  psSilk_VAD->HPstate = HPstateTmp;
  std::array<opus_int32, 4> Xnrg;
  opus_int32 sumSquared = 0;
  for (int b = 0; b < 4; b++) {
    decimated_framelength = ((psEncC->frame_length) >> (std::min(4 - b, 4 - 1)));
    const int dec_subframe_length = ((decimated_framelength) >> (2));
    int dec_subframe_offset = 0;
    Xnrg[b] = psSilk_VAD->XnrgSubfr[b];
    for (int s = 0; s < (1 << 2); s++) {
      sumSquared = 0;
      for (int i = 0; i < dec_subframe_length; i++) {
        const opus_int32 x_tmp = ((X[X_offset[b] + i + dec_subframe_offset]) >> (3));
        sumSquared = wrap_add(sumSquared, static_cast<opus_int16>(x_tmp) * static_cast<opus_int16>(x_tmp));
      }
      Xnrg[b] = saturating_add_int32(Xnrg[b], s < 3 ? sumSquared : sumSquared >> 1);
      dec_subframe_offset += dec_subframe_length;
    }
    psSilk_VAD->XnrgSubfr[b] = sumSquared;
  }
  silk_VAD_GetNoiseLevels(Xnrg, psSilk_VAD);
  sumSquared = 0;
  opus_int32 input_tilt = 0;
  opus_int32 speech_nrg = 0;
  std::array<opus_int32, 4> NrgToNoiseRatio_Q8;
  for (int b = 0; b < 4; b++) {
    const opus_int32 band_speech_nrg = Xnrg[b] - psSilk_VAD->NL[b];
    speech_nrg += (b + 1) * (band_speech_nrg >> 4);
    if (band_speech_nrg > 0) {
      NrgToNoiseRatio_Q8[b] = (Xnrg[b] & 0xFF800000) == 0 ? wrap_shift_left(Xnrg[b], 8) / (psSilk_VAD->NL[b] + 1)
                                                          : (static_cast<opus_int32>((Xnrg[b]) / (((psSilk_VAD->NL[b]) >> (8)) + 1)));
      opus_int32 SNR_Q7 = silk_lin2log(NrgToNoiseRatio_Q8[b]) - 8 * 128;
      sumSquared = wrap_add(sumSquared, static_cast<opus_int16>(SNR_Q7) * static_cast<opus_int16>(SNR_Q7));
      if (band_speech_nrg < (static_cast<opus_int32>(1) << 20)) {
        SNR_Q7 = silk_mul_wb(wrap_shift_left(silk_SQRT_APPROX(band_speech_nrg), 6), SNR_Q7);
      }
      input_tilt = silk_mla_wb(input_tilt, tiltWeights[b], SNR_Q7);
    } else {
      NrgToNoiseRatio_Q8[b] = 256;
    }
  }
  sumSquared = (static_cast<opus_int32>((sumSquared) / (4)));
  const opus_int32 pSNR_dB_Q7 = static_cast<opus_int16>(3 * silk_SQRT_APPROX(sumSquared));
  opus_int32 SA_Q15 = silk_sigm_Q15(silk_mul_wb(45000, pSNR_dB_Q7) - 128);
  psEncC->input_tilt_Q15 = wrap_shift_left(silk_sigm_Q15(input_tilt) - 16384, 1);
  if (psEncC->frame_length == 20 * psEncC->fs_kHz) {
    speech_nrg = ((speech_nrg) >> (1));
  }
  if (speech_nrg <= 0) {
    SA_Q15 = ((SA_Q15) >> (1));
  } else if (speech_nrg < 16384) {
    speech_nrg = wrap_shift_left(speech_nrg, 16);
    speech_nrg = silk_SQRT_APPROX(speech_nrg);
    SA_Q15 = silk_mul_wb(32768 + speech_nrg, SA_Q15);
  }
  psEncC->speech_activity_Q8 = std::min(((SA_Q15) >> (7)), 0xFF);
  opus_int32 smooth_coef_Q16 = silk_mul_wb(4096, silk_mul_wb(SA_Q15, SA_Q15));
  if (psEncC->frame_length == 10 * psEncC->fs_kHz) {
    smooth_coef_Q16 >>= 1;
  }
  for (int b = 0; b < 4; b++) {
    psSilk_VAD->NrgRatioSmth_Q8[b] =
        silk_mla_wb(psSilk_VAD->NrgRatioSmth_Q8[b], NrgToNoiseRatio_Q8[b] - psSilk_VAD->NrgRatioSmth_Q8[b], smooth_coef_Q16);
    const opus_int32 SNR_Q7 = 3 * (silk_lin2log(psSilk_VAD->NrgRatioSmth_Q8[b]) - 8 * 128);
    psEncC->input_quality_bands_Q15[b] = silk_sigm_Q15(((SNR_Q7 - 16 * 128) >> (4)));
  }
  const int activity_threshold = fixed_q<8>(0.05f);
  const bool active = psEncC->speech_activity_Q8 >= activity_threshold;
  psEncC->indices.signalType = active;
  psEncC->VAD_flags[psEncC->nFramesEncoded] = active;
}

static void silk_VAD_GetNoiseLevels(std::span<const opus_int32, 4> pX, silk_VAD_state* psSilk_VAD) {
  int min_coef = 0;
  if (psSilk_VAD->counter < 1000) {
    min_coef = static_cast<opus_int32>(0x7FFF / ((psSilk_VAD->counter >> 4) + 1));
    ++psSilk_VAD->counter;
  }
  for (int k = 0; k < 4; k++) {
    opus_int32 nl = psSilk_VAD->NL[k];
    const opus_int32 nrg = saturating_add_int32(pX[k], silk_vad_noise_level_bias[k]);
    const opus_int32 inv_nrg = (static_cast<opus_int32>((0x7FFFFFFF) / (nrg)));
    const int coef = std::max(nrg > wrap_shift_left(nl, 3) ? 1024 >> 3
                              : nrg < nl                   ? 1024
                                                           : silk_mul_wb(multiply_q16(inv_nrg, nl), 1024 << 1),
                              min_coef);
    psSilk_VAD->inv_NL[k] = silk_mla_wb(psSilk_VAD->inv_NL[k], inv_nrg - psSilk_VAD->inv_NL[k], coef);
    nl = (static_cast<opus_int32>((0x7FFFFFFF) / (psSilk_VAD->inv_NL[k])));
    nl = std::min(nl, 0x00FFFFFF);
    psSilk_VAD->NL[k] = nl;
  }
}

static int silk_control_audio_bandwidth(silk_encoder_state* psEncC, silk_EncControlStruct* encControl, const bool allow_bandwidth_switch) {
  const int orig_kHz = psEncC->fs_kHz != 0 ? psEncC->fs_kHz : psEncC->sLP.saved_fs_kHz;
  const opus_int32 fs_Hz = orig_kHz * 1000;
  if (fs_Hz == 0) {
    return std::min(encControl->desiredInternalSampleRate, psEncC->API_fs_Hz) / 1000;
  }
  if (fs_Hz > psEncC->API_fs_Hz || fs_Hz > encControl->maxInternalSampleRate || fs_Hz < encControl->minInternalSampleRate) {
    return std::clamp(psEncC->API_fs_Hz, encControl->minInternalSampleRate, encControl->maxInternalSampleRate) / 1000;
  }
  if (psEncC->sLP.transition_frame_no >= (5120 / (5 * 4))) {
    psEncC->sLP.mode = 0;
  }
  if (!allow_bandwidth_switch && !encControl->opusCanSwitch) {
    return orig_kHz;
  }
  int fs_kHz = orig_kHz;
  if (fs_Hz > encControl->desiredInternalSampleRate) {
    if (psEncC->sLP.mode == 0) {
      psEncC->sLP.transition_frame_no = (5120 / (5 * 4));
      zero_object(psEncC->sLP.In_LP_State);
    }
    if (encControl->opusCanSwitch) {
      psEncC->sLP.mode = 0;
      fs_kHz = orig_kHz == 16 ? 12 : 8;
    } else if (psEncC->sLP.transition_frame_no <= 0) {
      encControl->switchReady = 1;
      encControl->maxBits -= encControl->maxBits * 5 / (encControl->payloadSize_ms + 5);
    } else {
      psEncC->sLP.mode = -2;
    }
  } else if (fs_Hz < encControl->desiredInternalSampleRate) {
    if (encControl->opusCanSwitch) {
      fs_kHz = orig_kHz == 8 ? 12 : 16;
      psEncC->sLP.transition_frame_no = 0;
      zero_object(psEncC->sLP.In_LP_State);
      psEncC->sLP.mode = 1;
    } else if (psEncC->sLP.mode == 0) {
      encControl->switchReady = 1;
      encControl->maxBits -= encControl->maxBits * 5 / (encControl->payloadSize_ms + 5);
    } else {
      psEncC->sLP.mode = 1;
    }
  } else if (psEncC->sLP.mode < 0) {
    psEncC->sLP.mode = 1;
  }
  return fs_kHz;
}

static void silk_quant_LTP_gains(opus_int16 B_Q14[4 * 5], opus_uint8 cbk_index[4], opus_uint8* periodicity_index,
                                 opus_int32* sum_log_gain_Q7, int* pred_gain_dB_Q7, const opus_int32 XX_Q17[4 * 5 * 5],
                                 const opus_int32 xX_Q17[4 * 5], const int subfr_len, const int nb_subfr) {
  int j, k, cbk_size;
  opus_uint8 temp_idx[4];
  const opus_uint8* cl_ptr_Q5;
  const opus_int8* cbk_ptr_Q7;
  const opus_uint8* cbk_gain_ptr_Q7;
  const opus_int32 *XX_Q17_ptr, *xX_Q17_ptr;
  opus_int32 res_nrg_Q15_subfr, res_nrg_Q15 = 0, rate_dist_Q7_subfr, rate_dist_Q7, min_rate_dist_Q7;
  opus_int32 sum_log_gain_tmp_Q7, best_sum_log_gain_Q7, max_gain_Q7;
  int gain_Q7 = 0;
  min_rate_dist_Q7 = 0x7FFFFFFF;
  best_sum_log_gain_Q7 = 0;
  for (k = 0; k < 3; k++) {
    opus_int32 gain_safety = fixed_q<7>(0.4);
    const auto ltp_codebook = silk_LTP_codebook(k);
    cl_ptr_Q5 = ltp_codebook.gain_bits_q5.data();
    cbk_ptr_Q7 = ltp_codebook.vq_q7.data();
    cbk_gain_ptr_Q7 = ltp_codebook.vq_gain_q7.data();
    cbk_size = ltp_codebook.vq_size;
    XX_Q17_ptr = XX_Q17;
    xX_Q17_ptr = xX_Q17;
    res_nrg_Q15 = 0;
    rate_dist_Q7 = 0;
    sum_log_gain_tmp_Q7 = *sum_log_gain_Q7;
    for (j = 0; j < nb_subfr; j++) {
      max_gain_Q7 = silk_log2lin(fixed_q<7>(250.0f / 6.0f) - sum_log_gain_tmp_Q7 + (7 << 7)) - gain_safety;
      silk_VQ_WMat_EC_c(&temp_idx[j], &res_nrg_Q15_subfr, &rate_dist_Q7_subfr, &gain_Q7, XX_Q17_ptr, xX_Q17_ptr, cbk_ptr_Q7,
                        cbk_gain_ptr_Q7, cl_ptr_Q5, subfr_len, max_gain_Q7, cbk_size);
      res_nrg_Q15 = (((static_cast<opus_uint32>(res_nrg_Q15) + static_cast<opus_uint32>(res_nrg_Q15_subfr)) & 0x80000000)
                         ? 0x7FFFFFFF
                         : ((res_nrg_Q15) + (res_nrg_Q15_subfr)));
      rate_dist_Q7 = (((static_cast<opus_uint32>(rate_dist_Q7) + static_cast<opus_uint32>(rate_dist_Q7_subfr)) & 0x80000000)
                          ? 0x7FFFFFFF
                          : ((rate_dist_Q7) + (rate_dist_Q7_subfr)));
      const opus_int32 log_gain_delta_Q7 = silk_lin2log(gain_safety + gain_Q7) - (7 << 7);
      sum_log_gain_tmp_Q7 = std::max(0, sum_log_gain_tmp_Q7 + log_gain_delta_Q7);
      XX_Q17_ptr += 5 * 5;
      xX_Q17_ptr += 5;
    }
    if (rate_dist_Q7 <= min_rate_dist_Q7) {
      min_rate_dist_Q7 = rate_dist_Q7;
      *periodicity_index = static_cast<opus_uint8>(k);
      copy_n_items(temp_idx, static_cast<std::size_t>(nb_subfr), cbk_index);
      best_sum_log_gain_Q7 = sum_log_gain_tmp_Q7;
    }
  }
  cbk_ptr_Q7 = silk_LTP_codebook(*periodicity_index).vq_q7.data();
  for (j = 0; j < nb_subfr; j++) {
    for (k = 0; k < 5; k++) {
      B_Q14[j * 5 + k] = wrap_shift_left(cbk_ptr_Q7[cbk_index[j] * 5 + k], 7);
    }
  }
  res_nrg_Q15 = nb_subfr == 2 ? ((res_nrg_Q15) >> (1)) : ((res_nrg_Q15) >> (2));
  *sum_log_gain_Q7 = best_sum_log_gain_Q7;
  *pred_gain_dB_Q7 = static_cast<int>(static_cast<opus_int32>(static_cast<opus_int16>(-3)) *
                                      static_cast<opus_int32>(static_cast<opus_int16>(silk_lin2log(res_nrg_Q15) - (15 << 7))));
}

void silk_VQ_WMat_EC_c(opus_uint8* ind, opus_int32* res_nrg_Q15, opus_int32* rate_dist_Q8, int* gain_Q7, const opus_int32* XX_Q17,
                       const opus_int32* xX_Q17, const opus_int8* cb_Q7, const opus_uint8* cb_gain_Q7, const opus_uint8* cl_Q5,
                       const int subfr_len, const opus_int32 max_gain_Q7, const int L) {
  std::array<opus_int32, 5> neg_xX_Q24;
  for (auto i = std::size_t{}; i < neg_xX_Q24.size(); ++i) {
    neg_xX_Q24[i] = -wrap_shift_left(xX_Q17[i], 7);
  }
  *rate_dist_Q8 = 0x7FFFFFFF;
  *res_nrg_Q15 = 0x7FFFFFFF;
  *ind = 0;
  for (int k = 0; k < L; ++k) {
    const auto* cb_row_Q7 = cb_Q7 + 5 * k;
    const auto gain_tmp_Q7 = cb_gain_Q7[k];
    const auto penalty = wrap_shift_left(std::max(gain_tmp_Q7 - max_gain_Q7, 0), 11);
    auto sum1_Q15 = fixed_q<15>(1.001);
    for (int row = 0; row < 5; ++row) {
      auto sum2_Q24 = neg_xX_Q24[static_cast<std::size_t>(row)];
      for (int column = row + 1; column < 5; ++column) {
        sum2_Q24 = wrap_add(sum2_Q24, XX_Q17[row * 5 + column] * cb_row_Q7[column]);
      }
      sum2_Q24 = wrap_add(wrap_shift_left(sum2_Q24, 1), XX_Q17[row * 5 + row] * cb_row_Q7[row]);
      sum1_Q15 = silk_mla_wb(sum1_Q15, sum2_Q24, cb_row_Q7[row]);
    }
    if (sum1_Q15 >= 0) {
      const auto bits_res_Q8 = static_cast<opus_int16>(subfr_len) * static_cast<opus_int16>(silk_lin2log(sum1_Q15 + penalty) - (15 << 7));
      const auto bits_tot_Q8 = wrap_add(bits_res_Q8, wrap_shift_left(cl_Q5[k], 2));
      if (bits_tot_Q8 <= *rate_dist_Q8) {
        *rate_dist_Q8 = bits_tot_Q8;
        *res_nrg_Q15 = sum1_Q15 + penalty;
        *ind = static_cast<opus_uint8>(k);
        *gain_Q7 = gain_tmp_Q7;
      }
    }
  }
}

void silk_HP_variable_cutoff(silk_encoder_state_FLP state_Fxx[]) {
  silk_encoder_state* psEncC1 = &state_Fxx[0].sCmn;
  if (psEncC1->prevSignalType == 2) {
    const auto pitch_freq_Hz_Q16 = wrap_shift_left(psEncC1->fs_kHz * 1000, 16) / psEncC1->prevLag;
    auto pitch_freq_log_Q7 = silk_lin2log(pitch_freq_Hz_Q16) - (16 << 7);
    const int quality_Q15 = psEncC1->input_quality_bands_Q15[0];
    const auto quality_weight = silk_mul_wb(wrap_shift_left(-quality_Q15, 2), quality_Q15);
    pitch_freq_log_Q7 = silk_mla_wb(pitch_freq_log_Q7, quality_weight, pitch_freq_log_Q7 - silk_log_60_q7);
    auto delta_freq_Q7 = pitch_freq_log_Q7 - (psEncC1->variable_HP_smth1_Q15 >> 8);
    if (delta_freq_Q7 < 0) {
      delta_freq_Q7 *= 3;
    }
    delta_freq_Q7 = clamp_value<opus_int32>(delta_freq_Q7, -51, 51);
    const auto activity_delta = static_cast<opus_int16>(psEncC1->speech_activity_Q8) * static_cast<opus_int16>(delta_freq_Q7);
    psEncC1->variable_HP_smth1_Q15 = silk_mla_wb(psEncC1->variable_HP_smth1_Q15, activity_delta, 6554);
    psEncC1->variable_HP_smth1_Q15 = clamp_value(psEncC1->variable_HP_smth1_Q15, silk_log_60_q15, silk_log_100_q15);
  }
}

static void silk_NLSF_encode(std::span<opus_int8, 17> NLSFIndices, std::span<opus_int16, 16> pNLSF_Q15,
                             const silk_NLSF_CB_struct* psNLSF_CB, std::span<const opus_int16, 16> pW_Q2, const int NLSF_mu_Q20,
                             const int nSurvivors, const int signalType) {
  int i, s, ind1, bestIndex, prob_Q8, bits_q7;
  opus_int32 W_tmp_Q9;
  opus_int16 res_Q10[16], NLSF_tmp_Q15[16], W_adj_Q5[16], ec_ix[16];
  opus_uint8 pred_Q8[16];
  const opus_uint8 *pCB_element, *iCDF_ptr;
  const opus_int16* pCB_Wght_Q9;
  silk_NLSF_stabilize(pNLSF_Q15.data(), psNLSF_CB->deltaMin_Q15, psNLSF_CB->order);
  opus_int32 err_Q24[silk_nlsf_max_cb1_vectors], RD_Q25[silk_nlsf_max_survivors];
  int tempIndices1[silk_nlsf_max_survivors];
  opus_int8 tempIndices2[silk_nlsf_max_survivors][silk_nlsf_max_order];
  silk_NLSF_VQ(err_Q24, pNLSF_Q15.data(), psNLSF_CB->CB1_NLSF_Q8, psNLSF_CB->CB1_Wght_Q9, psNLSF_CB->nVectors, psNLSF_CB->order);
  silk_insertion_sort_increasing(err_Q24, tempIndices1, psNLSF_CB->nVectors, nSurvivors);
  for (s = 0; s < nSurvivors; s++) {
    ind1 = tempIndices1[s];
    pCB_element = psNLSF_CB->CB1_NLSF_Q8 + ind1 * psNLSF_CB->order;
    pCB_Wght_Q9 = psNLSF_CB->CB1_Wght_Q9 + ind1 * psNLSF_CB->order;
    for (i = 0; i < psNLSF_CB->order; i++) {
      NLSF_tmp_Q15[i] = static_cast<opus_int16>(static_cast<opus_uint16>(pCB_element[i]) << 7);
      W_tmp_Q9 = pCB_Wght_Q9[i];
      res_Q10[i] = static_cast<opus_int16>(silk_mul_i16_shift<14>(pNLSF_Q15[i] - NLSF_tmp_Q15[i], W_tmp_Q9));
      W_adj_Q5[i] = silk_DIV32_varQ(
          static_cast<opus_int32>(pW_Q2[i]),
          (static_cast<opus_int32>(static_cast<opus_int16>(W_tmp_Q9)) * static_cast<opus_int32>(static_cast<opus_int16>(W_tmp_Q9))), 21);
    }
    silk_NLSF_unpack(ec_ix, pred_Q8, psNLSF_CB, ind1);
    RD_Q25[s] = silk_NLSF_del_dec_quant(tempIndices2[s], res_Q10, W_adj_Q5, pred_Q8, ec_ix, psNLSF_CB->ec_Rates_Q5,
                                        psNLSF_CB->quantStepSize_Q16, psNLSF_CB->invQuantStepSize_Q6, NLSF_mu_Q20, psNLSF_CB->order);
    iCDF_ptr = silk_nlsf_cb1_icdf(psNLSF_CB, signalType);
    prob_Q8 = ind1 == 0 ? 256 - iCDF_ptr[ind1] : iCDF_ptr[ind1 - 1] - iCDF_ptr[ind1];
    bits_q7 = (8 << 7) - silk_lin2log(prob_Q8);
    RD_Q25[s] += static_cast<opus_int16>(bits_q7) * static_cast<opus_int16>(NLSF_mu_Q20 >> 2);
  }
  silk_insertion_sort_increasing(RD_Q25, &bestIndex, nSurvivors, 1);
  NLSFIndices[0] = static_cast<opus_int8>(tempIndices1[bestIndex]);
  copy_n_bytes(tempIndices2[bestIndex], static_cast<std::size_t>(psNLSF_CB->order * sizeof(opus_int8)), &NLSFIndices[1]);
  silk_NLSF_decode(pNLSF_Q15, NLSFIndices, psNLSF_CB);
}

void silk_NLSF_VQ(opus_int32 err_Q24[], const opus_int16 in_Q15[], const opus_uint8 pCB_Q8[], const opus_int16 pWght_Q9[], const int K,
                  const int LPC_order) {
  auto* cb_Q8_ptr = pCB_Q8;
  auto* w_Q9_ptr = pWght_Q9;
  for (int i = 0; i < K; ++i) {
    opus_int32 sum_error_Q24 = 0;
    opus_int32 pred_Q24 = 0;
    for (int m = LPC_order - 1; m >= 0; --m) {
      const auto diff_Q15 = wrap_subtract(in_Q15[m], wrap_shift_left(cb_Q8_ptr[m], 7));
      const auto diffw_Q24 = static_cast<opus_int16>(diff_Q15) * static_cast<opus_int16>(w_Q9_ptr[m]);
      sum_error_Q24 += std::abs(diffw_Q24 - (pred_Q24 >> 1));
      pred_Q24 = diffw_Q24;
    }
    err_Q24[i] = sum_error_Q24;
    cb_Q8_ptr += LPC_order;
    w_Q9_ptr += LPC_order;
  }
}

void silk_NLSF_unpack(std::span<opus_int16, 16> ec_ix, std::span<opus_uint8, 16> pred_Q8, const silk_NLSF_CB_struct* psNLSF_CB,
                      const int CB1_index) {
  auto* ec_sel_ptr = psNLSF_CB->ec_sel + CB1_index * psNLSF_CB->order / 2;
  for (int i = 0; i < psNLSF_CB->order; i += 2) {
    const opus_uint8 entry = *ec_sel_ptr++;
    ec_ix[i] = (static_cast<opus_int32>(static_cast<opus_int16>(((entry) >> (1)) & 7)) *
                static_cast<opus_int32>(static_cast<opus_int16>(2 * 4 + 1)));
    pred_Q8[i] = psNLSF_CB->pred_Q8[i + (entry & 1) * (psNLSF_CB->order - 1)];
    ec_ix[i + 1] = (static_cast<opus_int32>(static_cast<opus_int16>(((entry) >> (5)) & 7)) *
                    static_cast<opus_int32>(static_cast<opus_int16>(2 * 4 + 1)));
    pred_Q8[i + 1] = psNLSF_CB->pred_Q8[i + (((entry) >> (4)) & 1) * (psNLSF_CB->order - 1) + 1];
  }
}

struct silk_nlsf_del_dec_out_tables {
  std::array<opus_int16, 20> out0;
  std::array<opus_int16, 20> out1;
};

constexpr auto silk_nlsf_quant_offset_q10 = static_cast<opus_int32>((0.1) * (opus_int64{1} << 10) + 0.5);
constexpr auto silk_nlsf_quant_step_nb_mb_q16 = static_cast<int>((0.18) * (opus_int64{1} << 16) + 0.5);
constexpr auto silk_nlsf_quant_step_wb_q16 = static_cast<int>((0.15) * (opus_int64{1} << 16) + 0.5);

template <int QuantStepQ16>
  requires(QuantStepQ16 > 0)
consteval auto make_silk_nlsf_del_dec_out_tables() noexcept -> silk_nlsf_del_dec_out_tables {
  silk_nlsf_del_dec_out_tables tables{};
  for (int i = -10; i <= 10 - 1; ++i) {
    auto out0_Q10 = static_cast<opus_int16>(i * 1024);
    auto out1_Q10 = static_cast<opus_int16>(out0_Q10 + 1024);
    if (i > 0) {
      out0_Q10 = static_cast<opus_int16>(out0_Q10 - silk_nlsf_quant_offset_q10);
      out1_Q10 = static_cast<opus_int16>(out1_Q10 - silk_nlsf_quant_offset_q10);
    } else if (i == 0) {
      out1_Q10 = static_cast<opus_int16>(out1_Q10 - silk_nlsf_quant_offset_q10);
    } else if (i == -1) {
      out0_Q10 = static_cast<opus_int16>(out0_Q10 + silk_nlsf_quant_offset_q10);
    } else {
      out0_Q10 = static_cast<opus_int16>(out0_Q10 + silk_nlsf_quant_offset_q10);
      out1_Q10 = static_cast<opus_int16>(out1_Q10 + silk_nlsf_quant_offset_q10);
    }
    tables.out0[static_cast<std::size_t>(i + 10)] =
        static_cast<opus_int16>((static_cast<opus_int32>(out0_Q10) * static_cast<opus_int32>(QuantStepQ16)) >> 16);
    tables.out1[static_cast<std::size_t>(i + 10)] =
        static_cast<opus_int16>((static_cast<opus_int32>(out1_Q10) * static_cast<opus_int32>(QuantStepQ16)) >> 16);
  }
  return tables;
}

constexpr auto silk_nlsf_del_dec_out_tables_nb_mb = make_silk_nlsf_del_dec_out_tables<silk_nlsf_quant_step_nb_mb_q16>();
constexpr auto silk_nlsf_del_dec_out_tables_wb = make_silk_nlsf_del_dec_out_tables<silk_nlsf_quant_step_wb_q16>();

[[nodiscard]] constexpr auto silk_nlsf_rate_q5(const opus_uint8* rates_Q5, int index) noexcept -> int {
  return index >= 4 ? 280 + 43 * (index - 4) : (index <= -4 ? 280 - 43 * (index + 4) : rates_Q5[index + 4]);
}

[[nodiscard]] constexpr auto silk_nlsf_rd_q25(opus_int32 base_Q25, int in_Q10, opus_int16 out_Q10, opus_int16 weight_Q5, opus_int32 mu_Q20,
                                              int rate_Q5) noexcept -> opus_int32 {
  const auto diff_Q10 = static_cast<opus_int32>(static_cast<opus_int16>(in_Q10 - out_Q10));
  return base_Q25 + diff_Q10 * diff_Q10 * weight_Q5 +
         static_cast<opus_int32>(static_cast<opus_int16>(mu_Q20)) * static_cast<opus_int32>(static_cast<opus_int16>(rate_Q5));
}

opus_int32 silk_NLSF_del_dec_quant(std::span<opus_int8, 16> indices, std::span<const opus_int16, 16> x_Q10,
                                   std::span<const opus_int16, 16> w_Q5, std::span<const opus_uint8, 16> pred_coef_Q8,
                                   std::span<const opus_int16, 16> ec_ix, const opus_uint8 ec_rates_Q5[], const int quant_step_size_Q16,
                                   const opus_int16 inv_quant_step_size_Q6, const opus_int32 mu_Q20, const opus_int16 order) {
  constexpr int state_count = 4;
  std::array<int, state_count> ind_sort;
  std::array<std::array<opus_int8, 16>, state_count> ind;
  std::array<opus_int16, 2 * state_count> prev_out_Q10;
  std::array<opus_int32, 2 * state_count> RD_Q25;
  std::array<opus_int32, state_count> RD_min_Q25;
  std::array<opus_int32, state_count> RD_max_Q25;
  const auto& out_tables =
      quant_step_size_Q16 == silk_nlsf_quant_step_nb_mb_q16 ? silk_nlsf_del_dec_out_tables_nb_mb : silk_nlsf_del_dec_out_tables_wb;
  const auto* out0_Q10_table = out_tables.out0.data();
  const auto* out1_Q10_table = out_tables.out1.data();
  int nStates = 1;
  RD_Q25[0] = 0;
  prev_out_Q10[0] = 0;
  for (int i = order - 1; i >= 0; --i) {
    const auto* rates_Q5 = &ec_rates_Q5[ec_ix[i]];
    const auto in_Q10 = x_Q10[i];
    for (int j = 0; j < nStates; ++j) {
      const auto pred_Q10 = silk_mul_i16_shift<8>(pred_coef_Q8[i], prev_out_Q10[j]);
      const auto res_Q10 = ((in_Q10) - (pred_Q10));
      auto ind_tmp = silk_mul_i16_shift<16>(inv_quant_step_size_Q6, res_Q10);
      ind_tmp = clamp_value(ind_tmp, -10, 9);
      ind[j][i] = static_cast<opus_int8>(ind_tmp);
      const auto out0_Q10 = static_cast<opus_int16>(out0_Q10_table[ind_tmp + 10] + pred_Q10);
      const auto out1_Q10 = static_cast<opus_int16>(out1_Q10_table[ind_tmp + 10] + pred_Q10);
      prev_out_Q10[j] = out0_Q10;
      prev_out_Q10[j + nStates] = out1_Q10;
      const auto rate0_Q5 = silk_nlsf_rate_q5(rates_Q5, ind_tmp);
      const auto rate1_Q5 = silk_nlsf_rate_q5(rates_Q5, ind_tmp + 1);
      const auto RD_tmp_Q25 = RD_Q25[j];
      RD_Q25[j] = silk_nlsf_rd_q25(RD_tmp_Q25, in_Q10, out0_Q10, w_Q5[i], mu_Q20, rate0_Q5);
      RD_Q25[j + nStates] = silk_nlsf_rd_q25(RD_tmp_Q25, in_Q10, out1_Q10, w_Q5[i], mu_Q20, rate1_Q5);
    }
    if (nStates <= state_count / 2) {
      for (int j = 0; j < nStates; ++j) {
        ind[j + nStates][i] = ind[j][i] + 1;
      }
      nStates <<= 1;
      for (int j = nStates; j < state_count; ++j) {
        ind[j][i] = ind[j - nStates][i];
      }
    } else {
      for (int j = 0; j < state_count; ++j) {
        if (RD_Q25[j] > RD_Q25[j + state_count]) {
          RD_max_Q25[j] = RD_Q25[j];
          RD_min_Q25[j] = RD_Q25[j + state_count];
          std::swap(RD_Q25[j], RD_Q25[j + state_count]);
          std::swap(prev_out_Q10[j], prev_out_Q10[j + state_count]);
          ind_sort[j] = j + state_count;
        } else {
          RD_min_Q25[j] = RD_Q25[j];
          RD_max_Q25[j] = RD_Q25[j + state_count];
          ind_sort[j] = j;
        }
      }
      while (true) {
        const auto max_min = std::ranges::max_element(RD_min_Q25);
        const auto min_max = std::ranges::min_element(RD_max_Q25);
        if (*min_max >= *max_min) {
          break;
        }
        const auto ind_min_max = static_cast<int>(min_max - RD_max_Q25.begin());
        const auto ind_max_min = static_cast<int>(max_min - RD_min_Q25.begin());
        ind_sort[ind_max_min] = ind_sort[ind_min_max] ^ state_count;
        RD_Q25[ind_max_min] = RD_Q25[ind_min_max + state_count];
        prev_out_Q10[ind_max_min] = prev_out_Q10[ind_min_max + state_count];
        RD_min_Q25[ind_max_min] = 0;
        RD_max_Q25[ind_min_max] = opus_int32_max;
        ind[ind_max_min] = ind[ind_min_max];
      }
      for (int j = 0; j < state_count; ++j) {
        ind[j][i] += ind_sort[j] >> 2;
      }
    }
  }
  const auto best = std::ranges::min_element(RD_Q25);
  const auto best_state = static_cast<int>(best - RD_Q25.begin());
  const auto best_path = best_state & (state_count - 1);
  copy_n_items(ind[best_path].data(), static_cast<std::size_t>(order), indices.data());
  indices[0] += best_state >> 2;
  return *best;
}

static void silk_process_NLSFs(silk_encoder_state* psEncC, opus_int16 PredCoef_Q12[2][16], opus_int16 pNLSF_Q15[16],
                               const opus_int16 prev_NLSFq_Q15[16]) {
  opus_int16 pNLSF0_temp_Q15[16]{};
  opus_int16 pNLSFW_QW[16], pNLSFW0_temp_QW[16];
  auto NLSF_mu_Q20 = fixed_q<20>(0.003) + silk_mul_wb(fixed_q<28>(-0.001), psEncC->speech_activity_Q8);
  if (psEncC->nb_subfr == 2) {
    NLSF_mu_Q20 = ((NLSF_mu_Q20) + (((NLSF_mu_Q20)) >> ((1))));
  }
  silk_NLSF_VQ_weights_laroia(pNLSFW_QW, pNLSF_Q15, psEncC->predictLPCOrder);
  const auto doInterpolate = psEncC->Complexity >= 4 && psEncC->indices.NLSFInterpCoef_Q2 < 4;
  if (doInterpolate) {
    silk_interpolate(std::span<opus_int16>{pNLSF0_temp_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                     std::span<const opus_int16>{prev_NLSFq_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                     std::span<const opus_int16>{pNLSF_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                     psEncC->indices.NLSFInterpCoef_Q2);
    silk_NLSF_VQ_weights_laroia(pNLSFW0_temp_QW, pNLSF0_temp_Q15, psEncC->predictLPCOrder);
    const auto i_sqr_Q15 = wrap_shift_left(psEncC->indices.NLSFInterpCoef_Q2 * psEncC->indices.NLSFInterpCoef_Q2, 11);
    for (int i = 0; i < psEncC->predictLPCOrder; ++i) {
      pNLSFW_QW[i] = (pNLSFW_QW[i] >> 1) + silk_mul_i16_shift<16>(pNLSFW0_temp_QW[i], i_sqr_Q15);
    }
  }
  silk_NLSF_encode(std::span<opus_int8, 17>{psEncC->indices.NLSFIndices}, std::span<opus_int16, 16>{pNLSF_Q15, 16}, psEncC->psNLSF_CB,
                   pNLSFW_QW, NLSF_mu_Q20, psEncC->NLSF_MSVQ_Survivors, psEncC->indices.signalType);
  silk_NLSF2A(PredCoef_Q12[1], pNLSF_Q15, psEncC->predictLPCOrder);
  if (doInterpolate) {
    silk_interpolate(std::span<opus_int16>{pNLSF0_temp_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                     std::span<const opus_int16>{prev_NLSFq_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                     std::span<const opus_int16>{pNLSF_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                     psEncC->indices.NLSFInterpCoef_Q2);
    silk_NLSF2A(PredCoef_Q12[0], pNLSF0_temp_Q15, psEncC->predictLPCOrder);
  } else {
    copy_n_bytes(PredCoef_Q12[1], static_cast<std::size_t>(psEncC->predictLPCOrder * sizeof(opus_int16)), PredCoef_Q12[0]);
  }
}

[[nodiscard]] static auto silk_stereo_mid_mix_q9(const opus_int16* mid, int n) noexcept -> opus_int32 {
  return wrap_shift_left(mid[n] + 2 * mid[n + 1] + mid[n + 2], 9);
}

[[nodiscard]] static auto silk_stereo_apply_predictors_q8(opus_int32 side_Q8, opus_int32 mid_mix_Q9, opus_int16 mid_center,
                                                          opus_int32 pred0_Q13, opus_int32 pred1_Q13) noexcept -> opus_int32 {
  auto sum = silk_mla_wb(side_Q8, mid_mix_Q9, pred0_Q13);
  sum = silk_mla_wb(sum, wrap_shift_left(mid_center, 11), pred1_Q13);
  return sum;
}
constexpr std::array<unsigned char, 117 - 10> silk_TargetRate_NB_21 = numeric_blob_array<unsigned char>(
    R"blob(000F27343D444A4F54585C5F6366696C6F7275777A7C7E81838587898B8E8F91939597999B9D9EA0A2A3A5A7A8AAABADAEB0B1B3B4B6B7B9BABBBDBEC0C1C2C4C5C7C8C9CBCCCDCFD0D1D3D4D5D7D8D9DBDCDDDFE0E1E3E4E6E7E8EAEBECEEEFF1F2F3F5F6F8F9FAFCFDFF)blob");
constexpr std::array<unsigned char, 165 - 10> silk_TargetRate_MB_21 = numeric_blob_array<unsigned char>(
    R"blob(00001C2B343B41464A4E5155575A5D5F626466696B6D6F71737476787A7B7D7F808283858688898A8C8D8F90919394959798999A9C9D9E9FA0A2A3A4A5A6A7A8A9ABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBCBDBEBFC0C1C2C3C4C5C6C7C8C9CACBCBCCCDCECFD0D1D2D3D4D5D6D6D7D8D9DADBDCDDDEDFE0E0E1E2E3E4E5E6E7E8E9EAEBECECEDEEEFF0F1F2F3F4F5F6F7F8F9FAFBFCFDFEFF)blob");
constexpr std::array<unsigned char, 201 - 10> silk_TargetRate_WB_21 = numeric_blob_array<unsigned char>(
    R"blob(000000081D2931383E42464A4D505356585B5D5F61636567696B6C6E707173747677797A7B7D7E7F81828384868788898A8C8D8E8F909192939495969798999A9C9D9E9F9FA0A1A2A3A4A5A6A7A8A9AAABABACADAEAFB0B1B1B2B3B4B5B5B6B7B8B9B9BABBBCBDBDBEBFC0C0C1C2C3C3C4C5C6C6C7C8C8C9CACBCBCCCDCECECFD0D1D1D2D3D3D4D5D6D6D7D8D8D9DADBDBDCDDDDDEDFE0E0E1E2E2E3E4E5E5E6E7E8E8E9EAEAEBECEDEDEEEFF0F0F1F2F3F3F4F5F6F6F7F8F9F9FAFBFCFDFF)blob");
void silk_control_SNR(silk_encoder_state* psEncC, opus_int32 TargetRate_bps) {
  if (psEncC->TargetRate_bps == TargetRate_bps) {
    return;
  }
  psEncC->TargetRate_bps = TargetRate_bps;
  if (psEncC->nb_subfr == 2) {
    TargetRate_bps -= 2000 + psEncC->fs_kHz / 16;
  }
  const std::span<const unsigned char> snr_table = psEncC->fs_kHz == 8    ? std::span<const unsigned char>{silk_TargetRate_NB_21}
                                                   : psEncC->fs_kHz == 12 ? std::span<const unsigned char>{silk_TargetRate_MB_21}
                                                                          : std::span<const unsigned char>{silk_TargetRate_WB_21};
  const int id = std::min((TargetRate_bps + 200) / 400 - 10, static_cast<int>(snr_table.size()) - 1);
  psEncC->SNR_dB_Q7 = id <= 0 ? 0 : snr_table[static_cast<std::size_t>(id)] * 21;
}

void silk_init_encoder(silk_encoder_state_FLP* psEnc) {
  zero_object(*psEnc);
  psEnc->sCmn.variable_HP_smth1_Q15 = silk_log_60_q15;
  psEnc->sCmn.first_frame_after_reset = 1;
  silk_VAD_Init(&psEnc->sCmn.sVAD);
}
namespace {
constinit const std::array<std::array<opus_int8, 3>, 2> silk_CB_lags_stage2_10_ms =
    numeric_blob_matrix<opus_int8, 3>(R"blob(000100000001)blob");
constinit const std::array<std::array<opus_int8, 12>, 2> silk_CB_lags_stage3_10_ms =
    numeric_blob_matrix<opus_int8, 12>(R"blob(000001FF01FF02FE02FE03FD00010001FF02FF02FE03FE03)blob");
constinit const std::array<std::array<opus_int8, 2>, 2> silk_Lag_range_stage3_10_ms =
    numeric_blob_matrix<opus_int8, 2>(R"blob(FD07FE07)blob");
constinit const std::array<std::array<opus_int8, 11>, 4> silk_CB_lags_stage2 = numeric_blob_matrix<opus_int8, 11>(
    R"blob(0002FFFFFF0000010100010001000000000001000000000001000000010000000000FF02010001010000FFFF)blob");
constinit const std::array<std::array<opus_int8, 34>, 4> silk_CB_lags_stage3 = numeric_blob_matrix<opus_int8, 34>(
    R"blob(000001FF0001FF00FF01FE02FEFE02FD0203FDFC03FC0404FB05FAFB06F9060508F700000100000000000000FF01000001FF0001FFFF01FF0201FF02FEFE02FE020203FD0001000000000000010001000001FF0100000201FF02FFFF02FF0202FF03FEFEFE0300010000010001FF02FF02FF0203FE03FEFE0404FD05FDFC06FC0605FB08FAFBF909)blob");
constinit const std::array<std::array<opus_int8, 4 * 2>, 3> silk_Lag_range_stage3 =
    numeric_blob_matrix<opus_int8, 4 * 2>(R"blob(FB08FF06FF06FC0AFA0AFE06FF06FB0AF70CFD07FE07F90D)blob");
constinit const std::array<opus_int8, 2 + 1> silk_nb_cbk_searchs_stage3 = numeric_blob_array<opus_int8>(R"blob(101822)blob");
} // namespace

static void silk_setup_resamplers(silk_encoder_state_FLP* psEnc, int fs_kHz);
static void silk_setup_fs(silk_encoder_state_FLP* psEnc, int fs_kHz, int PacketSize_ms);
void silk_control_encoder(silk_encoder_state_FLP* psEnc, silk_EncControlStruct* encControl, const int allow_bw_switch,
                          const int force_fs_kHz) {
  psEnc->sCmn.useCBR = encControl->useCBR;
  psEnc->sCmn.API_fs_Hz = encControl->API_sampleRate;
  psEnc->sCmn.nChannelsInternal = encControl->nChannelsInternal;
  int fs_kHz = silk_control_audio_bandwidth(&psEnc->sCmn, encControl, allow_bw_switch != 0);
  if (force_fs_kHz) {
    fs_kHz = force_fs_kHz;
  }
  silk_setup_resamplers(psEnc, fs_kHz);
  silk_setup_fs(psEnc, fs_kHz, encControl->payloadSize_ms);
  silk_setup_complexity(&psEnc->sCmn, encControl->complexity);
}

static void silk_setup_resamplers(silk_encoder_state_FLP* psEnc, int fs_kHz) {
  if (psEnc->sCmn.fs_kHz != fs_kHz) {
    if (psEnc->sCmn.fs_kHz == 0) {
      silk_resampler_init(&psEnc->sCmn.resampler_state, psEnc->sCmn.API_fs_Hz, fs_kHz * 1000, 1);
    } else {
      const opus_int32 buf_length_ms = (psEnc->sCmn.nb_subfr * 5 << 1) + 5;
      const opus_int32 old_buf_samples = buf_length_ms * psEnc->sCmn.fs_kHz;
      const opus_int32 new_buf_samples = buf_length_ms * fs_kHz;
      std::array<opus_int16, silk_max_resampler_reconfig_samples> resampled;
      silk_float2short_array(resampled.data(), psEnc->x_buf, old_buf_samples);
      silk_resampler_state_struct temp_resampler_state{};
      silk_resampler_init(&temp_resampler_state, psEnc->sCmn.fs_kHz * 1000, psEnc->sCmn.API_fs_Hz, 0);
      const opus_int32 api_buf_samples = buf_length_ms * (static_cast<opus_int32>((psEnc->sCmn.API_fs_Hz) / (1000)));
      std::array<opus_int16, silk_max_resampler_api_reconfig_samples> api_samples;
      silk_resampler(&temp_resampler_state, api_samples.data(), resampled.data(), old_buf_samples);
      silk_resampler_init(&psEnc->sCmn.resampler_state, psEnc->sCmn.API_fs_Hz, fs_kHz * 1000, 1);
      silk_resampler(&psEnc->sCmn.resampler_state, resampled.data(), api_samples.data(), api_buf_samples);
      silk_short2float_array(psEnc->x_buf, resampled.data(), new_buf_samples);
    }
  }
}

static void silk_setup_fs(silk_encoder_state_FLP* psEnc, int fs_kHz, int PacketSize_ms) {
  if (PacketSize_ms != 5 * psEnc->sCmn.nb_subfr * psEnc->sCmn.nFramesPerPacket) {
    if (PacketSize_ms <= 10) {
      psEnc->sCmn.nFramesPerPacket = 1;
      psEnc->sCmn.nb_subfr = PacketSize_ms == 10 ? 2 : 1;
      psEnc->sCmn.frame_length = PacketSize_ms * fs_kHz;
    } else {
      psEnc->sCmn.nFramesPerPacket = (static_cast<opus_int32>((PacketSize_ms) / ((5 * 4))));
      psEnc->sCmn.nb_subfr = 4;
      psEnc->sCmn.frame_length = 20 * fs_kHz;
    }
    psEnc->sCmn.TargetRate_bps = 0;
  }
  if (psEnc->sCmn.fs_kHz != fs_kHz) {
    zero_object(psEnc->sShape);
    zero_object(psEnc->sCmn.sNSQ);
    zero_object(psEnc->sCmn.prev_NLSFq_Q15);
    zero_object(psEnc->sCmn.sLP.In_LP_State);
    psEnc->sCmn.inputBufIx = 0;
    psEnc->sCmn.nFramesEncoded = 0;
    psEnc->sCmn.TargetRate_bps = 0;
    psEnc->sCmn.prevLag = 100;
    psEnc->sCmn.first_frame_after_reset = 1;
    psEnc->sShape.LastGainIndex = 10;
    psEnc->sCmn.sNSQ.lagPrev = 100;
    psEnc->sCmn.sNSQ.prev_gain_Q16 = 65536;
    psEnc->sCmn.prevSignalType = 0;
    psEnc->sCmn.fs_kHz = fs_kHz;
    psEnc->sCmn.Complexity = -1;
    psEnc->sCmn.psNLSF_CB = silk_nlsf_codebook_for_fs(psEnc->sCmn.fs_kHz);
    psEnc->sCmn.predictLPCOrder = psEnc->sCmn.psNLSF_CB->order;
    psEnc->sCmn.subfr_length = 5 * fs_kHz;
    psEnc->sCmn.frame_length = psEnc->sCmn.subfr_length * psEnc->sCmn.nb_subfr;
    psEnc->sCmn.ltp_mem_length = 20 * fs_kHz;
  }
}

static void silk_setup_complexity(silk_encoder_state* psEncC, int Complexity) {
  if (psEncC->Complexity == Complexity) {
    return;
  }
  struct complexity_tier {
    opus_uint8 pec, pelpc, slpc, la_mult, nsd, msvq, warped;
    opus_uint16 pet_q16;
  };
  static constexpr std::array<complexity_tier, 7> tiers{{{0, 6, 12, 3, 1, 2, 0, 52429},
                                                         {1, 8, 14, 5, 1, 3, 0, 49807},
                                                         {0, 6, 12, 3, 2, 2, 0, 52429},
                                                         {1, 8, 14, 5, 2, 4, 0, 49807},
                                                         {1, 10, 16, 5, 2, 6, 1, 48497},
                                                         {1, 12, 20, 5, 3, 8, 1, 47186},
                                                         {2, 16, 24, 5, 4, 16, 1, 45875}}};
  static constexpr std::array<opus_uint8, 11> complexity_to_tier{0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6};
  const auto& t = tiers[complexity_to_tier[Complexity]];
  psEncC->pitchEstimationComplexity = t.pec;
  psEncC->pitchEstimationThreshold_Q16 = t.pet_q16;
  psEncC->pitchEstimationLPCOrder = std::min<int>(t.pelpc, psEncC->predictLPCOrder);
  psEncC->shapingLPCOrder = t.slpc;
  psEncC->la_shape = t.la_mult * psEncC->fs_kHz;
  psEncC->nStatesDelayedDecision = t.nsd;
  psEncC->NLSF_MSVQ_Survivors = t.msvq;
  psEncC->warping_Q16 = t.warped ? psEncC->fs_kHz * static_cast<opus_int32>((0.015f) * (opus_int64{1} << 16) + 0.5) : 0;
  psEncC->shapeWinLength = 5 * psEncC->fs_kHz + 2 * psEncC->la_shape;
  psEncC->Complexity = Complexity;
}

static void silk_A2NLSF_trans_poly(opus_int32* p, const int dd) {
  for (int k = 2; k <= dd; ++k) {
    for (int n = dd; n > k; --n) {
      p[n - 2] -= p[n];
    }
    p[k - 2] -= wrap_shift_left(p[k], 1);
  }
}

static opus_int32 silk_A2NLSF_eval_poly(opus_int32* p, const opus_int32 x, const int dd) {
  opus_int32 y32 = p[dd];
  const opus_int32 x_Q16 = wrap_shift_left(x, 4);
  if (8 == dd) {
    y32 = (static_cast<opus_int32>((p[7]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[6]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[5]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[4]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[3]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[2]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[1]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    y32 = (static_cast<opus_int32>((p[0]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
  } else {
    for (int n = dd - 1; n >= 0; --n) {
      y32 = (static_cast<opus_int32>((p[n]) + ((static_cast<opus_int64>(y32) * (x_Q16)) >> 16)));
    }
  }
  return y32;
}

static void silk_A2NLSF_init(const opus_int32* a_Q16, opus_int32* P, opus_int32* Q, const int dd) {
  P[dd] = 1 << 16;
  Q[dd] = 1 << 16;
  for (int k = 0; k < dd; k++) {
    P[k] = -a_Q16[dd - k - 1] - a_Q16[dd + k];
    Q[k] = -a_Q16[dd - k - 1] + a_Q16[dd + k];
  }
  for (int k = dd; k > 0; k--) {
    P[k - 1] -= P[k];
    Q[k - 1] += Q[k];
  }
  silk_A2NLSF_trans_poly(P, dd);
  silk_A2NLSF_trans_poly(Q, dd);
}

static void silk_A2NLSF(opus_int16* NLSF, opus_int32* a_Q16, const int d) {
  opus_int32 P[24 / 2 + 1];
  opus_int32 Q[24 / 2 + 1];
  opus_int32* PQ[2]{P, Q};
  const int dd = ((d) >> (1));
  int retry = 0;
restart_search:
  silk_A2NLSF_init(a_Q16, P, Q, dd);
  opus_int32* p = P;
  opus_int32 xlo = silk_LSFCosTab_FIX_Q12[0];
  opus_int32 ylo = silk_A2NLSF_eval_poly(p, xlo, dd);
  int root_ix = 0;
  if (ylo < 0) {
    NLSF[0] = 0;
    p = Q;
    ylo = silk_A2NLSF_eval_poly(p, xlo, dd);
    root_ix = 1;
  }
  int k = 1;
  opus_int32 thr = 0;
  for (; root_ix < d;) {
    opus_int32 xhi = silk_LSFCosTab_FIX_Q12[k];
    opus_int32 yhi = silk_A2NLSF_eval_poly(p, xhi, dd);
    if ((ylo <= 0 && yhi >= thr) || (ylo >= 0 && yhi <= -thr)) {
      thr = yhi == 0 ? 1 : 0;
      int ffrac = -256;
      for (int m = 0; m < 3; m++) {
        const opus_int32 xmid = rounded_rshift<1>(xlo + xhi);
        const opus_int32 ymid = silk_A2NLSF_eval_poly(p, xmid, dd);
        if ((ylo <= 0 && ymid >= 0) || (ylo >= 0 && ymid <= 0)) {
          xhi = xmid;
          yhi = ymid;
        } else {
          xlo = xmid;
          ylo = ymid;
          ffrac = ((ffrac) + (((128)) >> ((m))));
        }
      }
      if ((((ylo) > 0) ? (ylo) : -(ylo)) < 65536) {
        const opus_int32 den = ylo - yhi;
        const opus_int32 nom = wrap_shift_left(ylo, 8 - 3) + (den >> 1);
        if (den != 0) {
          ffrac += (((nom) / (den)));
        }
      } else {
        ffrac += (((ylo) / (((ylo - yhi) >> (8 - 3)))));
      }
      NLSF[root_ix] = static_cast<opus_int16>(std::min(wrap_shift_left(k, 8) + ffrac, 0x7FFF));
      root_ix++;
      if (root_ix >= d) {
        continue;
      }
      p = PQ[root_ix & 1];
      xlo = silk_LSFCosTab_FIX_Q12[k - 1];
      ylo = wrap_shift_left(1 - (root_ix & 2), 12);
    } else {
      k++;
      xlo = xhi;
      ylo = yhi;
      thr = 0;
      if (k > 128) {
        retry++;
        if (retry > 16) {
          NLSF[0] = static_cast<opus_int16>(static_cast<opus_int32>((1 << 15) / (d + 1)));
          for (k = 1; k < d; k++) {
            NLSF[k] = static_cast<opus_int16>((NLSF[k - 1]) + (NLSF[0]));
          }
          return;
        }
        silk_bwexpander(a_Q16, static_cast<std::size_t>(d), 65536 - wrap_shift_left(1, retry));
        goto restart_search;
      }
    }
  }
}

constexpr opus_int16 A_fb1_20 = 5394 << 1;
constexpr opus_int16 A_fb1_21 = -24290;
template <bool Split>
static void silk_downsample_by2(const opus_int16* in, opus_int32 S[2], opus_int16* outL, opus_int16* outH, opus_int32 N) {
  constexpr opus_int16 coefficient0 = Split ? A_fb1_20 : 9872;
  constexpr opus_int16 coefficient1 = Split ? A_fb1_21 : -25727;
  const int N2 = N >> 1;
  for (int k = 0; k < N2; ++k) {
    opus_int32 in32 = wrap_shift_left(in[2 * k], 10);
    opus_int32 Y = in32 - S[0];
    opus_int32 X = silk_mla_wb(Y, Y, coefficient1);
    const opus_int32 out_1 = S[0] + X;
    S[0] = in32 + X;
    in32 = wrap_shift_left(in[2 * k + 1], 10);
    Y = in32 - S[1];
    X = silk_mul_wb(Y, coefficient0);
    const opus_int32 out_2 = S[1] + X;
    S[1] = in32 + X;
    if constexpr (Split) {
      outL[k] = rounded_rshift_to_int16<11>(saturating_add_int32(out_2, out_1));
      outH[k] = rounded_rshift_to_int16<11>(saturating_subtract_int32(out_2, out_1));
    } else {
      outL[k] = rounded_rshift_to_int16<11>(out_1 + out_2);
    }
  }
}

void silk_ana_filt_bank_1(const opus_int16* in, opus_int32 S[2], opus_int16* outL, opus_int16* outH, opus_int32 N) {
  silk_downsample_by2<true>(in, S, outL, outH, N);
}

static void silk_biquad_alt_stride1(const opus_int16* in, const opus_int32 B_Q28[3], const opus_int32 A_Q28[2], opus_int32 S[2],
                                    opus_int16* out, const opus_int32 len) {
  const opus_int32 A0_L_Q28 = (-A_Q28[0]) & 0x00003FFF;
  const opus_int32 A0_U_Q28 = (-A_Q28[0]) >> 14;
  const opus_int32 A1_L_Q28 = (-A_Q28[1]) & 0x00003FFF;
  const opus_int32 A1_U_Q28 = (-A_Q28[1]) >> 14;
  for (int k = 0; k < len; ++k) {
    const opus_int32 inval = in[k];
    const opus_int32 out32_Q14 = wrap_shift_left(silk_mla_wb(S[0], B_Q28[0], inval), 2);
    S[0] = S[1] + rounded_mul_i16_q16<14>(out32_Q14, A0_L_Q28);
    S[0] = silk_mla_wb(S[0], out32_Q14, A0_U_Q28);
    S[0] = silk_mla_wb(S[0], B_Q28[1], inval);
    S[1] = rounded_mul_i16_q16<14>(out32_Q14, A1_L_Q28);
    S[1] = silk_mla_wb(S[1], out32_Q14, A1_U_Q28);
    S[1] = silk_mla_wb(S[1], B_Q28[2], inval);
    out[k] = saturate_int16_from_int32((out32_Q14 + (1 << 14) - 1) >> 14);
  }
}

template <typename T> void silk_bwexpander(T* ar, std::size_t count, opus_int32 chirp_Q16) {
  if (count == 0) {
    return;
  }
  const opus_int32 chirp_minus_one_Q16 = chirp_Q16 - 65536;
  for (auto index = std::size_t{}; index + 1 < count; ++index) {
    const auto scaled = static_cast<opus_int64>(chirp_Q16) * ar[index];
    if constexpr (std::same_as<T, opus_int16>) {
      ar[index] = static_cast<opus_int16>(rounded_rshift<16>(scaled));
    } else {
      ar[index] = static_cast<opus_int32>(scaled >> 16);
    }
    chirp_Q16 += static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(chirp_Q16) * chirp_minus_one_Q16));
  }
  const auto scaled = static_cast<opus_int64>(chirp_Q16) * ar[count - 1];
  if constexpr (std::same_as<T, opus_int16>) {
    ar[count - 1] = static_cast<opus_int16>(rounded_rshift<16>(scaled));
  } else {
    ar[count - 1] = static_cast<opus_int32>(scaled >> 16);
  }
}

template <typename T, std::size_t Rows, std::size_t Columns>
  requires(Rows > 0 && Columns > 0)
[[nodiscard]] constexpr auto flat_table_span(const std::array<std::array<T, Columns>, Rows>& table) noexcept -> std::span<const T> {
  return {table[0].data(), Rows * Columns};
}

struct silk_lag_codebook_view {
  std::span<const opus_int8> entries;
  int cbk_size;
  int nb_cbk_search;
  [[nodiscard]] constexpr auto at(const int subframe, const int codebook_index) const noexcept -> opus_int8 {
    return entries[static_cast<std::size_t>(subframe * cbk_size + codebook_index)];
  }
};
struct silk_lag_range_view {
  std::span<const opus_int8> entries;
  [[nodiscard]] constexpr auto low(const int subframe) const noexcept -> opus_int8 {
    return entries[static_cast<std::size_t>(subframe * 2)];
  }
  [[nodiscard]] constexpr auto high(const int subframe) const noexcept -> opus_int8 {
    return entries[static_cast<std::size_t>(subframe * 2 + 1)];
  }
};
[[nodiscard]] constexpr auto silk_decode_pitch_codebook_view(const int fs_kHz, const int nb_subfr) noexcept -> silk_lag_codebook_view {
  if (fs_kHz == 8) {
    return nb_subfr == 4 ? silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage2), 11, 11}
                         : silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage2_10_ms), 3, 3};
  }
  return nb_subfr == 4 ? silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage3), 34, 34}
                       : silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage3_10_ms), 12, 12};
}

[[nodiscard]] constexpr auto silk_stage2_pitch_codebook_view(const int fs_kHz, const int nb_subfr, const int complexity) noexcept
    -> silk_lag_codebook_view {
  return nb_subfr == 4 ? silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage2), 11, fs_kHz == 8 && complexity > 0 ? 11 : 3}
                       : silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage2_10_ms), 3, 3};
}

[[nodiscard]] static constexpr auto silk_stage3_pitch_codebook_view(const int nb_subfr, const int complexity) noexcept
    -> silk_lag_codebook_view {
  return nb_subfr == 4 ? silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage3), 34,
                                                silk_nb_cbk_searchs_stage3[static_cast<std::size_t>(complexity)]}
                       : silk_lag_codebook_view{flat_table_span(silk_CB_lags_stage3_10_ms), 12, 12};
}

[[nodiscard]] constexpr auto silk_stage3_lag_range_view(const int nb_subfr, const int complexity) noexcept -> silk_lag_range_view {
  return nb_subfr == 4 ? silk_lag_range_view{std::span<const opus_int8>{silk_Lag_range_stage3[static_cast<std::size_t>(complexity)]}}
                       : silk_lag_range_view{flat_table_span(silk_Lag_range_stage3_10_ms)};
}

[[nodiscard]] static constexpr auto silk_pitch_contour_icdf(const int fs_kHz, const int nb_subfr) noexcept -> std::span<const opus_uint8> {
  if (fs_kHz == 8) {
    return nb_subfr == 4 ? std::span<const opus_uint8>{silk_pitch_contour_NB_iCDF}
                         : std::span<const opus_uint8>{silk_pitch_contour_10_ms_NB_iCDF};
  }
  return nb_subfr == 4 ? std::span<const opus_uint8>{silk_pitch_contour_iCDF} : std::span<const opus_uint8>{silk_pitch_contour_10_ms_iCDF};
}

[[nodiscard]] static constexpr auto silk_pitch_lag_low_bits_icdf(const int fs_kHz) noexcept -> std::span<const opus_uint8> {
  return fs_kHz == 16   ? silk_uniform8_iCDF
         : fs_kHz == 12 ? std::span<const opus_uint8>{silk_uniform6_iCDF}
                        : std::span<const opus_uint8>{silk_uniform4_iCDF};
}

struct silk_resampler_ratio_config {
  int fir_fracs;
  int fir_order;
  std::span<const opus_int16> coefs;
};
void silk_decode_pitch(opus_int16 lagIndex, opus_uint8 contourIndex, int pitch_lags[], const int Fs_kHz, const int nb_subfr) {
  const auto codebook = silk_decode_pitch_codebook_view(Fs_kHz, nb_subfr);
  const int min_lag = 2 * Fs_kHz;
  const int max_lag = 18 * Fs_kHz;
  const int lag = min_lag + lagIndex;
  for (int k = 0; k < nb_subfr; k++) {
    pitch_lags[k] = lag + codebook.at(k, contourIndex);
    pitch_lags[k] = clamp_value(pitch_lags[k], min_lag, max_lag);
  }
}

static opus_int32 silk_inner_prod_aligned_scale(std::span<const opus_int16> inVec1, std::span<const opus_int16> inVec2, const int scale) {
  opus_int32 sum = 0;
  for (auto index = std::size_t{}; index < inVec1.size(); ++index) {
    sum += (static_cast<opus_int32>(inVec1[index]) * static_cast<opus_int32>(inVec2[index])) >> scale;
  }
  return sum;
}

opus_int32 silk_lin2log(const opus_int32 inLin) {
  opus_int32 lz, frac_Q7;
  silk_CLZ_FRAC(inLin, &lz, &frac_Q7);
  return parabolic_q7_term(frac_Q7, 179) + wrap_shift_left(31 - lz, q7_shift);
}

opus_int32 silk_log2lin(const opus_int32 inLog_Q7) {
  opus_int32 out, frac_Q7;
  if (inLog_Q7 < 0) {
    return 0;
  } else if (inLog_Q7 >= 3967) {
    return opus_int32_max;
  }
  out = opus_int32{1} << (inLog_Q7 >> q7_shift);
  frac_Q7 = inLog_Q7 & 0x7F;
  const auto frac_term = parabolic_q7_term(frac_Q7, -174);
  if (inLog_Q7 < 2048) {
    out += static_cast<opus_int32>((static_cast<opus_int64>(out) * frac_term) >> q7_shift);
  } else {
    out += (out >> q7_shift) * frac_term;
  }
  return out;
}

template <int Order>
  requires(Order == 10 || Order == 16)
static inline void silk_LPC_analysis_filter_order(opus_int16* out, const opus_int16* in, const opus_int16* B, const opus_int32 len) {
  for (opus_int32 ix = Order; ix < len; ++ix) {
    const auto* in_ptr = &in[ix - 1];
    auto out32_Q12 = static_cast<opus_int32>(static_cast<opus_int16>(in_ptr[0])) * static_cast<opus_int32>(static_cast<opus_int16>(B[0]));
    for (int j = 1; j < Order; ++j) {
      const auto term =
          static_cast<opus_int32>(static_cast<opus_int16>(in_ptr[-j])) * static_cast<opus_int32>(static_cast<opus_int16>(B[j]));
      out32_Q12 = wrap_add(out32_Q12, term);
    }
    out32_Q12 = wrap_subtract(wrap_shift_left(in_ptr[1], 12), out32_Q12);
    out[ix] = saturate_int16_from_int32(rounded_rshift<12>(out32_Q12));
  }
  zero_n_items(out, static_cast<std::size_t>(Order));
}

void silk_LPC_analysis_filter(opus_int16* out, const opus_int16* in, const opus_int16* B, const opus_int32 len, const opus_int32 d) {
  if (d == 10) {
    silk_LPC_analysis_filter_order<10>(out, in, B, len);
  } else {
    silk_LPC_analysis_filter_order<16>(out, in, B, len);
  }
}

static inline opus_int32 LPC_inverse_pred_gain_QA_c(std::span<opus_int32> A_QA, const int order) {
  int k, n, mult2Q;
  opus_int32 invGain_Q30, rc_Q31, rc_mult1_Q30, rc_mult2, tmp1, tmp2;
  constexpr auto rc_limit_Q24 = fixed_q<24>(0.99975);
  invGain_Q30 = 1 << 30;
  for (k = order - 1; k >= 0; k--) {
    if (A_QA[k] > rc_limit_Q24 || A_QA[k] < -rc_limit_Q24) {
      return 0;
    }
    rc_Q31 = -wrap_shift_left(A_QA[k], 31 - 24);
    rc_mult1_Q30 = (1 << 30) - static_cast<opus_int32>((static_cast<opus_int64>(rc_Q31) * rc_Q31) >> 32);
    invGain_Q30 = wrap_shift_left(silk_mul_high(invGain_Q30, rc_mult1_Q30), 2);
    if (invGain_Q30 < fixed_q<30>(1.0f / 1e4f)) {
      return 0;
    }
    if (k == 0) {
      break;
    }
    mult2Q = 32 - silk_CLZ32((((rc_mult1_Q30) > 0) ? (rc_mult1_Q30) : -(rc_mult1_Q30)));
    rc_mult2 = silk_INVERSE32_varQ(rc_mult1_Q30, mult2Q + 30);
    for (n = 0; n < (k + 1) >> 1; n++) {
      opus_int64 tmp64;
      tmp1 = A_QA[n];
      tmp2 = A_QA[k - n - 1];
      tmp64 = inverse_prediction_step(tmp1, tmp2, rc_Q31, rc_mult2, mult2Q);
      if (tmp64 > opus_int32_max || tmp64 < opus_int32_min) {
        return 0;
      }
      A_QA[n] = static_cast<opus_int32>(tmp64);
      tmp64 = inverse_prediction_step(tmp2, tmp1, rc_Q31, rc_mult2, mult2Q);
      if (tmp64 > opus_int32_max || tmp64 < opus_int32_min) {
        return 0;
      }
      A_QA[k - n - 1] = static_cast<opus_int32>(tmp64);
    }
  }
  return invGain_Q30;
}

opus_int32 silk_LPC_inverse_pred_gain_c(const opus_int16* A_Q12, const int order) {
  std::array<opus_int32, silk_nlsf_max_order> coefficients;
  opus_int32 DC_resp = 0;
  for (int k = 0; k < order; ++k) {
    DC_resp += static_cast<opus_int32>(A_Q12[k]);
    coefficients[k] = wrap_shift_left(A_Q12[k], 24 - 12);
  }
  return DC_resp >= 4096 ? 0 : LPC_inverse_pred_gain_QA_c(std::span{coefficients}.first(static_cast<std::size_t>(order)), order);
}
static void silk_NLSF2A_find_poly(opus_int32* out, const opus_int32* cLSF, int dd) {
  out[0] = 1 << 16;
  out[1] = -cLSF[0];
  for (int k = 1; k < dd; ++k) {
    const opus_int32 ftmp = cLSF[2 * k];
    out[k + 1] =
        wrap_subtract(wrap_shift_left(out[k - 1], 1), static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(ftmp) * out[k])));
    for (int n = k; n > 1; --n) {
      out[n] += out[n - 2] - static_cast<opus_int32>(rounded_rshift<16>(static_cast<opus_int64>(ftmp) * out[n - 1]));
    }
    out[1] -= ftmp;
  }
}

void silk_NLSF2A(opus_int16* a_Q12, const opus_int16* NLSF, const int d) {
  constexpr std::array<unsigned char, 16> ordering16{0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
  constexpr std::array<unsigned char, 10> ordering10{0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
  opus_int32 cos_LSF_QA[24]{};
  opus_int32 P[24 / 2 + 1], Q[24 / 2 + 1];
  opus_int32 a32_QA1[24];
  const auto* ordering = d == 16 ? ordering16.data() : ordering10.data();
  for (int k = 0; k < d; ++k) {
    const opus_int32 f_int = NLSF[k] >> (15 - 7);
    const opus_int32 f_frac = NLSF[k] - wrap_shift_left(f_int, 15 - 7);
    const opus_int32 cos_val = silk_LSFCosTab_FIX_Q12[f_int];
    const opus_int32 delta = silk_LSFCosTab_FIX_Q12[f_int + 1] - cos_val;
    cos_LSF_QA[ordering[k]] = rounded_rshift<4>(wrap_add(wrap_shift_left(cos_val, 8), delta * f_frac));
  }
  const int dd = d >> 1;
  silk_NLSF2A_find_poly(P, &cos_LSF_QA[0], dd);
  silk_NLSF2A_find_poly(Q, &cos_LSF_QA[1], dd);
  for (int k = 0; k < dd; ++k) {
    const opus_int32 Ptmp = P[k + 1] + P[k];
    const opus_int32 Qtmp = Q[k + 1] - Q[k];
    a32_QA1[k] = -Qtmp - Ptmp;
    a32_QA1[d - k - 1] = Qtmp - Ptmp;
  }
  silk_LPC_fit(a_Q12, a32_QA1, d);
  for (int i = 0; silk_LPC_inverse_pred_gain_c(a_Q12, d) == 0 && i < 16; ++i) {
    silk_bwexpander(a32_QA1, static_cast<std::size_t>(d), 65536 - wrap_shift_left(2, i));
    for (int k = 0; k < d; ++k) {
      a_Q12[k] = static_cast<opus_int16>(rounded_rshift<5>(a32_QA1[k]));
    }
  }
}

void silk_NLSF_stabilize(opus_int16* NLSF_Q15, const opus_int16* NDeltaMin_Q15, const int L) {
  int i, I = 0, k;
  opus_int16 center_freq_Q15;
  opus_int32 diff_Q15, min_diff_Q15, min_center_Q15, max_center_Q15;
  for (int loops = 0; loops < 20; ++loops) {
    min_diff_Q15 = NLSF_Q15[0] - NDeltaMin_Q15[0];
    I = 0;
    for (i = 1; i <= L - 1; i++) {
      diff_Q15 = NLSF_Q15[i] - (NLSF_Q15[i - 1] + NDeltaMin_Q15[i]);
      if (diff_Q15 < min_diff_Q15) {
        min_diff_Q15 = diff_Q15;
        I = i;
      }
    }
    diff_Q15 = (1 << 15) - (NLSF_Q15[L - 1] + NDeltaMin_Q15[L]);
    if (diff_Q15 < min_diff_Q15) {
      min_diff_Q15 = diff_Q15;
      I = L;
    }
    if (min_diff_Q15 >= 0) {
      return;
    }
    if (I == 0) {
      NLSF_Q15[0] = NDeltaMin_Q15[0];
    } else if (I == L) {
      NLSF_Q15[L - 1] = (1 << 15) - NDeltaMin_Q15[L];
    } else {
      min_center_Q15 = 0;
      for (k = 0; k < I; k++) {
        min_center_Q15 += NDeltaMin_Q15[k];
      }
      min_center_Q15 += ((NDeltaMin_Q15[I]) >> (1));
      max_center_Q15 = 1 << 15;
      for (k = L; k > I; k--) {
        max_center_Q15 -= NDeltaMin_Q15[k];
      }
      max_center_Q15 -= ((NDeltaMin_Q15[I]) >> (1));
      center_freq_Q15 = static_cast<opus_int16>(clamped_midpoint(NLSF_Q15[I - 1], NLSF_Q15[I], min_center_Q15, max_center_Q15));
      NLSF_Q15[I - 1] = center_freq_Q15 - ((NDeltaMin_Q15[I]) >> (1));
      NLSF_Q15[I] = NLSF_Q15[I - 1] + NDeltaMin_Q15[I];
    }
  }
  silk_insertion_sort_increasing_all_values_int16(&NLSF_Q15[0], L);
  NLSF_Q15[0] = std::max(NLSF_Q15[0], NDeltaMin_Q15[0]);
  for (i = 1; i < L; i++) {
    NLSF_Q15[i] = std::max(NLSF_Q15[i], saturate_int16_from_int32(static_cast<opus_int32>(NLSF_Q15[i - 1]) + NDeltaMin_Q15[i]));
  }
  NLSF_Q15[L - 1] = std::min(NLSF_Q15[L - 1], static_cast<opus_int16>((1 << 15) - NDeltaMin_Q15[L]));
  for (i = L - 2; i >= 0; i--) {
    NLSF_Q15[i] = std::min(NLSF_Q15[i], static_cast<opus_int16>(NLSF_Q15[i + 1] - NDeltaMin_Q15[i + 1]));
  }
}

void silk_NLSF_VQ_weights_laroia(opus_int16* pNLSFW_Q_OUT, const opus_int16* pNLSF_Q15, const int D) {
  constexpr opus_int32 numerator = 1 << 17;
  auto inverse_gap = numerator / std::max<opus_int32>(pNLSF_Q15[0], 1);
  for (int k = 0; k < D - 1; ++k) {
    const auto next = numerator / std::max<opus_int32>(pNLSF_Q15[k + 1] - pNLSF_Q15[k], 1);
    pNLSFW_Q_OUT[k] = static_cast<opus_int16>(std::min(inverse_gap + next, 0x7FFF));
    inverse_gap = next;
  }
  const auto last = numerator / std::max<opus_int32>((1 << 15) - pNLSF_Q15[D - 1], 1);
  pNLSFW_Q_OUT[D - 1] = static_cast<opus_int16>(std::min(inverse_gap + last, 0x7FFF));
}

constexpr std::array<opus_int16, 3> silk_resampler_up2_hq_0 = numeric_blob_array<opus_int16>(R"blob(06D23A8A98AB)blob");
constexpr std::array<opus_int16, 3> silk_resampler_up2_hq_1 = numeric_blob_array<opus_int16>(R"blob(1AC664A9D8F6)blob");
namespace {
constinit const std::array<opus_int16, 2 + 3 * 18 / 2> silk_Resampler_3_4_COEFS = numeric_blob_array<opus_int16>(
    R"blob(AF2AC9D5FFCF00400011FF630161FE1000A32B2756BDFFD90006005BFF5600BA0017FC8018C04DD8FFEDFFDC0066FFA7FFE80148FC490A083E25)blob");
constinit const std::array<opus_int16, 2 + 2 * 18 / 2> silk_Resampler_2_3_COEFS =
    numeric_blob_array<opus_int16>(R"blob(C787C93D00400080FF8600240136FD00024824334545000C00800012FF720120FF8BFC9F101B387B)blob");
constinit const std::array<opus_int16, 2 + 24 / 2> silk_Resampler_1_2_COEFS =
    numeric_blob_array<opus_int16>(R"blob(0268C80DFFF60027003AFFD2FFAC007800B8FEC5FDE3050415042340)blob");
constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_3_COEFS =
    numeric_blob_array<opus_int16>(R"blob(3EE6C4C6FFF300000014001A0005FFE1FFD5FFFC0041005A0007FF63FF08FFD40251062F0A340CC7)blob");
constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_4_COEFS =
    numeric_blob_array<opus_int16>(R"blob(57E4C5050003FFF2FFECFFF10002001900250019FFF0FFB9FF95FFB100320124026F03D6050805B8)blob");
constinit const std::array<opus_int16, 2 + 36 / 2> silk_Resampler_1_6_COEFS =
    numeric_blob_array<opus_int16>(R"blob(6B94C4670011000C00080001FFF6FFEAFFE2FFE0FFEA0003002C006400A800F3013D017D01AD01C7)blob");
constinit const std::array<opus_int16, 2 + 2 * 2> silk_Resampler_2_3_COEFS_LQ =
    numeric_blob_array<opus_int16>(R"blob(F513E695125929F3061F2054)blob");
constinit const std::array<std::array<opus_int16, 8 / 2>, 12> silk_resampler_frac_FIR_12 = numeric_blob_matrix<opus_int16, 4>(
    R"blob(00BDFDA8026977670075FF61FBD27408003400DDF6A86E74FFFC0211F2EA66E5FFD002F6F08C5DA5FFB00389EF755306FF9D03CCEF824766FF9503C7F08B3B27FF990380F2612EAEFFA50305F4CF225EFFB90263F7A11698FFD201A9FAA10BB4)blob");
} // namespace

[[nodiscard]] static constexpr auto silk_select_resampler_config(const int input_hz, const int output_hz) noexcept
    -> silk_resampler_ratio_config {
  if (output_hz * 4 == input_hz * 3) {
    return {3, 18, silk_Resampler_3_4_COEFS};
  }
  if (output_hz * 3 == input_hz * 2) {
    return {2, 18, silk_Resampler_2_3_COEFS};
  }
  if (output_hz * 2 == input_hz) {
    return {1, 24, silk_Resampler_1_2_COEFS};
  }
  if (output_hz * 3 == input_hz) {
    return {1, 36, silk_Resampler_1_3_COEFS};
  }
  if (output_hz * 4 == input_hz) {
    return {1, 36, silk_Resampler_1_4_COEFS};
  }
  return output_hz * 6 == input_hz ? silk_resampler_ratio_config{1, 36, silk_Resampler_1_6_COEFS} : silk_resampler_ratio_config{0, 0, {}};
}

static void silk_resampler_private_IIR_FIR(void* SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen);
static void silk_resampler_private_down_FIR(void* SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen);
static void silk_resampler_private_up2_HQ(opus_int32* S, opus_int16* out, const opus_int16* in, opus_int32 len);
static void silk_resampler_private_AR2(opus_int32 S[], opus_int32 out_Q8[], const opus_int16 in[], const opus_int16 A_Q14[],
                                       opus_int32 len);
constexpr std::array<std::array<opus_uint8, 3>, 6> delay_matrix_enc =
    numeric_blob_matrix<opus_uint8, 3>(R"blob(06000300070300010A000206120A0C00002C)blob");
constexpr std::array<std::array<opus_uint8, 6>, 3> delay_matrix_dec =
    numeric_blob_matrix<opus_uint8, 6>(R"blob(04000200000000090407040400030C070707)blob");
[[nodiscard]] constexpr auto silk_resampler_rate_index(opus_int32 hz) noexcept -> int {
  return std::min(5, (((hz >> 12) - (hz > 16000)) >> (hz > 24000)) - 1);
}

void silk_resampler_init(silk_resampler_state_struct* S, opus_int32 Fs_Hz_in, opus_int32 Fs_Hz_out, int forEnc) {
  int up2x;
  zero_object(*S);
  const int in_index = silk_resampler_rate_index(Fs_Hz_in);
  const int out_index = silk_resampler_rate_index(Fs_Hz_out);
  S->inputDelay = forEnc ? delay_matrix_enc[in_index][out_index] : delay_matrix_dec[in_index][out_index];
  S->Fs_in_kHz = (static_cast<opus_int32>((Fs_Hz_in) / (1000)));
  S->Fs_out_kHz = (static_cast<opus_int32>((Fs_Hz_out) / (1000)));
  up2x = 0;
  if (Fs_Hz_out > Fs_Hz_in) {
    if (Fs_Hz_out == ((Fs_Hz_in) * (2))) {
      S->resampler_function = (1);
    } else {
      S->resampler_function = (2);
      up2x = 1;
    }
  } else if (Fs_Hz_out < Fs_Hz_in) {
    S->resampler_function = (3);
    const auto config = silk_select_resampler_config(Fs_Hz_in, Fs_Hz_out);
    S->FIR_Fracs = config.fir_fracs;
    S->FIR_Order = config.fir_order;
    S->Coefs = config.coefs.data();
  } else {
    S->resampler_function = (0);
  }
  S->invRatio_Q16 = wrap_shift_left(wrap_shift_left(Fs_Hz_in, 14 + up2x) / Fs_Hz_out, 2);
  for (; multiply_q16(S->invRatio_Q16, Fs_Hz_out) < wrap_shift_left(Fs_Hz_in, up2x); ++S->invRatio_Q16) {}
}

void silk_resampler(silk_resampler_state_struct* S, opus_int16 out[], const opus_int16 in[], opus_int32 inLen) {
  int nSamples = S->Fs_in_kHz - S->inputDelay;
  copy_n_bytes(in, static_cast<std::size_t>(nSamples * sizeof(opus_int16)), S->delayBuf + S->inputDelay);
  switch (S->resampler_function) {
  case (1):
    silk_resampler_private_up2_HQ(S->sIIR, out, S->delayBuf, S->Fs_in_kHz);
    silk_resampler_private_up2_HQ(S->sIIR, &out[S->Fs_out_kHz], &in[nSamples], inLen - S->Fs_in_kHz);
    break;
  case (2):
    silk_resampler_private_IIR_FIR(S, out, S->delayBuf, S->Fs_in_kHz);
    silk_resampler_private_IIR_FIR(S, &out[S->Fs_out_kHz], &in[nSamples], inLen - S->Fs_in_kHz);
    break;
  case (3):
    silk_resampler_private_down_FIR(S, out, S->delayBuf, S->Fs_in_kHz);
    silk_resampler_private_down_FIR(S, &out[S->Fs_out_kHz], &in[nSamples], inLen - S->Fs_in_kHz);
    break;
  default:
    copy_n_bytes(S->delayBuf, static_cast<std::size_t>(S->Fs_in_kHz * sizeof(opus_int16)), out);
    copy_n_bytes(&in[nSamples], static_cast<std::size_t>((inLen - S->Fs_in_kHz) * sizeof(opus_int16)), &out[S->Fs_out_kHz]);
  }
  copy_n_bytes(&in[inLen - S->inputDelay], static_cast<std::size_t>(S->inputDelay * sizeof(opus_int16)), S->delayBuf);
}

static void silk_resampler_down2_3(opus_int32* S, opus_int16* out, const opus_int16* in, opus_int32 inLen) {
  opus_int32 nSamplesIn = 0;
  std::array<opus_int32, (10 * 48) + 4> buf;
  copy_n_bytes(S, static_cast<std::size_t>(4 * sizeof(opus_int32)), buf.data());
  for (; inLen > 0;) {
    nSamplesIn = std::min(inLen, 10 * 48);
    silk_resampler_private_AR2(&S[4], buf.data() + 4, in, silk_Resampler_2_3_COEFS_LQ.data(), nSamplesIn);
    auto* buf_ptr = buf.data();
    for (opus_int32 counter = nSamplesIn; counter > 2; counter -= 3, buf_ptr += 3) {
      auto res_Q6 = silk_mul_wb(buf_ptr[0], silk_Resampler_2_3_COEFS_LQ[2]);
      res_Q6 = silk_mla_wb(res_Q6, buf_ptr[1], silk_Resampler_2_3_COEFS_LQ[3]);
      res_Q6 = silk_mla_wb(res_Q6, buf_ptr[2], silk_Resampler_2_3_COEFS_LQ[5]);
      res_Q6 = silk_mla_wb(res_Q6, buf_ptr[3], silk_Resampler_2_3_COEFS_LQ[4]);
      *out++ = rounded_rshift_to_int16<6>(res_Q6);
      res_Q6 = silk_mul_wb(buf_ptr[1], silk_Resampler_2_3_COEFS_LQ[4]);
      res_Q6 = silk_mla_wb(res_Q6, buf_ptr[2], silk_Resampler_2_3_COEFS_LQ[5]);
      res_Q6 = silk_mla_wb(res_Q6, buf_ptr[3], silk_Resampler_2_3_COEFS_LQ[3]);
      res_Q6 = silk_mla_wb(res_Q6, buf_ptr[4], silk_Resampler_2_3_COEFS_LQ[2]);
      *out++ = rounded_rshift_to_int16<6>(res_Q6);
    }
    in += nSamplesIn;
    inLen -= nSamplesIn;
    if (inLen > 0) {
      copy_n_bytes(buf.data() + nSamplesIn, static_cast<std::size_t>(4 * sizeof(opus_int32)), buf.data());
    }
  }
  copy_n_bytes(buf.data() + nSamplesIn, static_cast<std::size_t>(4 * sizeof(opus_int32)), S);
}

static void silk_resampler_down2(opus_int32* S, opus_int16* out, const opus_int16* in, opus_int32 inLen) {
  silk_downsample_by2<false>(in, S, out, nullptr, inLen);
}

static void silk_resampler_private_AR2(opus_int32 S[], opus_int32 out_Q8[], const opus_int16 in[], const opus_int16 A_Q14[],
                                       opus_int32 len) {
  for (opus_int32 k = 0; k < len; ++k) {
    auto out32 = wrap_add(S[0], wrap_shift_left(in[k], 8));
    out_Q8[k] = out32;
    out32 = wrap_shift_left(out32, 2);
    S[0] = silk_mla_wb(S[1], out32, A_Q14[0]);
    S[1] = silk_mul_wb(out32, A_Q14[1]);
  }
}
namespace {
[[nodiscard]] constexpr auto resampler_round_shift_saturate_int16(opus_int32 value, int shift) noexcept -> opus_int16 {
  return saturate_int16_from_int32(rounded_rshift(value, shift));
}

[[nodiscard]] constexpr auto resampler_fractional_fir18_q6(std::span<const opus_int32, 18> buffer, std::span<const opus_int16, 9> leading,
                                                           std::span<const opus_int16, 9> trailing) noexcept -> opus_int32 {
  auto acc = opus_int32{0};
  for (auto tap = std::size_t{}; tap < leading.size(); ++tap) {
    acc = silk_mla_wb(acc, buffer[tap], leading[tap]);
  }
  for (auto tap = std::size_t{}; tap < trailing.size(); ++tap) {
    acc = silk_mla_wb(acc, buffer[buffer.size() - 1 - tap], trailing[tap]);
  }
  return acc;
}

template <std::size_t HalfOrder>
  requires(HalfOrder > 0)
[[nodiscard]] constexpr auto resampler_symmetric_fir_q6(std::span<const opus_int32, 2 * HalfOrder> buffer,
                                                        std::span<const opus_int16, HalfOrder> coefficients) noexcept -> opus_int32 {
  auto acc = opus_int32{0};
  for (auto tap = std::size_t{}; tap < HalfOrder; ++tap) {
    const auto pair_sum = static_cast<opus_int32>(buffer[tap] + buffer[2 * HalfOrder - 1 - tap]);
    acc = silk_mla_wb(acc, pair_sum, coefficients[tap]);
  }
  return acc;
}

[[nodiscard]] constexpr auto resampler_frac_fir_12_q15(std::span<const opus_int16, 8> buffer, std::span<const opus_int16, 4> leading,
                                                       std::span<const opus_int16, 4> trailing) noexcept -> opus_int32 {
  auto acc = opus_int32{0};
  for (auto tap = std::size_t{}; tap < 4; ++tap) {
    acc += static_cast<opus_int32>(buffer[tap]) * static_cast<opus_int32>(leading[tap]);
  }
  for (auto tap = std::size_t{}; tap < 4; ++tap) {
    acc += static_cast<opus_int32>(buffer[4 + tap]) * static_cast<opus_int32>(trailing[3 - tap]);
  }
  return acc;
}

template <std::size_t HalfOrder>
static auto resample_symmetric_fir(opus_int16* out, const opus_int32* buffer, const opus_int16* coefficients, opus_int32 max_index_Q16,
                                   opus_int32 index_increment_Q16) noexcept -> opus_int16* {
  for (opus_int32 index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
    const auto window = std::span<const opus_int32, 2 * HalfOrder>{buffer + (index_Q16 >> 16), 2 * HalfOrder};
    *out++ = resampler_round_shift_saturate_int16(
        resampler_symmetric_fir_q6<HalfOrder>(window, std::span<const opus_int16, HalfOrder>{coefficients, HalfOrder}), 6);
  }
  return out;
}
} // namespace

static auto silk_resampler_private_down_FIR_INTERPOL(opus_int16* out, opus_int32* buf, const opus_int16* FIR_Coefs, int FIR_Order,
                                                     int FIR_Fracs, opus_int32 max_index_Q16, opus_int32 index_increment_Q16) noexcept
    -> opus_int16* {
  switch (FIR_Order) {
  case 18:
    for (opus_int32 index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
      const auto* buf_ptr = buf + (index_Q16 >> 16);
      const opus_int32 interpol_ind =
          static_cast<opus_int32>(((index_Q16 & 0xFFFF) * static_cast<opus_int64>(static_cast<opus_int16>(FIR_Fracs))) >> 16);
      const auto leading = std::span<const opus_int16, 9>{&FIR_Coefs[9 * interpol_ind], 9};
      const auto trailing = std::span<const opus_int16, 9>{&FIR_Coefs[9 * (FIR_Fracs - 1 - interpol_ind)], 9};
      const opus_int32 res_Q6 = resampler_fractional_fir18_q6(std::span<const opus_int32, 18>{buf_ptr, 18}, leading, trailing);
      *out++ = resampler_round_shift_saturate_int16(res_Q6, 6);
    }
    break;
  case 24:
    return resample_symmetric_fir<12>(out, buf, FIR_Coefs, max_index_Q16, index_increment_Q16);
  case 36:
    return resample_symmetric_fir<18>(out, buf, FIR_Coefs, max_index_Q16, index_increment_Q16);
  default:
    return out;
  }
  return out;
}

static void silk_resampler_private_down_FIR(void* SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen) {
  silk_resampler_state_struct* S = static_cast<silk_resampler_state_struct*>(SS);
  opus_int32 nSamplesIn = 0;
  std::array<opus_int32, silk_max_resampler_batch_size + silk_max_resampler_fir_order> buf;
  copy_n_bytes(S->sFIR.i32, static_cast<std::size_t>(S->FIR_Order * sizeof(opus_int32)), buf.data());
  const auto* FIR_Coefs = &S->Coefs[2];
  const opus_int32 index_increment_Q16 = S->invRatio_Q16;
  for (; inLen > 1;) {
    nSamplesIn = std::min(inLen, S->Fs_in_kHz * 10);
    silk_resampler_private_AR2(S->sIIR, buf.data() + S->FIR_Order, in, S->Coefs, nSamplesIn);
    const opus_int32 max_index_Q16 = wrap_shift_left(nSamplesIn, 16);
    out = silk_resampler_private_down_FIR_INTERPOL(out, buf.data(), FIR_Coefs, S->FIR_Order, S->FIR_Fracs, max_index_Q16,
                                                   index_increment_Q16);
    in += nSamplesIn;
    inLen -= nSamplesIn;
    if (inLen > 1) {
      copy_n_bytes(buf.data() + nSamplesIn, static_cast<std::size_t>(S->FIR_Order * sizeof(opus_int32)), buf.data());
    }
  }
  copy_n_bytes(buf.data() + nSamplesIn, static_cast<std::size_t>(S->FIR_Order * sizeof(opus_int32)), S->sFIR.i32);
}

static auto silk_resampler_private_IIR_FIR_INTERPOL(opus_int16* out, opus_int16* buf, opus_int32 max_index_Q16,
                                                    opus_int32 index_increment_Q16) noexcept -> opus_int16* {
  for (opus_int32 index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16) {
    const opus_int32 table_index =
        static_cast<opus_int32>(((index_Q16 & 0xFFFF) * static_cast<opus_int64>(static_cast<opus_int16>(12))) >> 16);
    const auto* buf_ptr = &buf[index_Q16 >> 16];
    const auto leading = std::span<const opus_int16, 4>{silk_resampler_frac_FIR_12[table_index]};
    const auto trailing = std::span<const opus_int16, 4>{silk_resampler_frac_FIR_12[11 - table_index]};
    const opus_int32 res_Q15 = resampler_frac_fir_12_q15(std::span<const opus_int16, 8>{buf_ptr, 8}, leading, trailing);
    *out++ = resampler_round_shift_saturate_int16(res_Q15, 15);
  }
  return out;
}

static void silk_resampler_private_IIR_FIR(void* SS, opus_int16 out[], const opus_int16 in[], opus_int32 inLen) {
  silk_resampler_state_struct* S = static_cast<silk_resampler_state_struct*>(SS);
  opus_int32 nSamplesIn = 0;
  std::array<opus_int16, 2 * silk_max_resampler_batch_size + 8> buf;
  copy_n_bytes(S->sFIR.i16, static_cast<std::size_t>(8 * sizeof(opus_int16)), buf.data());
  const opus_int32 index_increment_Q16 = S->invRatio_Q16;
  for (; inLen > 0;) {
    nSamplesIn = std::min(inLen, S->Fs_in_kHz * 10);
    silk_resampler_private_up2_HQ(S->sIIR, buf.data() + 8, in, nSamplesIn);
    const opus_int32 max_index_Q16 = wrap_shift_left(nSamplesIn, 16 + 1);
    out = silk_resampler_private_IIR_FIR_INTERPOL(out, buf.data(), max_index_Q16, index_increment_Q16);
    in += nSamplesIn;
    inLen -= nSamplesIn;
    if (inLen > 0) {
      copy_n_bytes(buf.data() + (nSamplesIn << 1), static_cast<std::size_t>(8 * sizeof(opus_int16)), buf.data());
    }
  }
  copy_n_bytes(buf.data() + (nSamplesIn << 1), static_cast<std::size_t>(8 * sizeof(opus_int16)), S->sFIR.i16);
}

[[nodiscard]] static auto silk_resampler_up2_hq_branch(std::span<opus_int32, 3> state, const opus_int32 in32,
                                                       const std::array<opus_int16, 3>& coeffs) noexcept -> opus_int32 {
  auto update_stage = [](const opus_int32 input, opus_int32& memory, const opus_int16 coefficient) noexcept {
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

static void silk_resampler_private_up2_HQ(opus_int32* S, opus_int16* out, const opus_int16* in, opus_int32 len) {
  for (opus_int32 k = 0; k < len; ++k) {
    const auto in32 = wrap_shift_left(in[k], 10);
    out[2 * k] = rounded_rshift_to_int16<10>(silk_resampler_up2_hq_branch(std::span<opus_int32, 3>{S, 3}, in32, silk_resampler_up2_hq_0));
    out[2 * k + 1] =
        rounded_rshift_to_int16<10>(silk_resampler_up2_hq_branch(std::span<opus_int32, 3>{S + 3, 3}, in32, silk_resampler_up2_hq_1));
  }
}

constexpr std::array<opus_int16, 6> sigm_LUT_slope_Q10{237, 153, 73, 30, 12, 7};
constexpr std::array<opus_int16, 6> sigm_LUT_pos_Q15{16384, 23955, 28861, 31213, 32178, 32548};
constexpr std::array<opus_int16, 6> sigm_LUT_neg_Q15{16384, 8812, 3906, 1554, 589, 219};
int silk_sigm_Q15(int in_Q5) {
  const bool negative = in_Q5 < 0;
  if (negative) {
    in_Q5 = -in_Q5;
  }
  if (in_Q5 >= 6 * 32) {
    return negative ? 0 : 32767;
  }
  const int index = in_Q5 >> 5;
  const int delta = sigm_LUT_slope_Q10[index] * (in_Q5 & 0x1F);
  return negative ? sigm_LUT_neg_Q15[index] - delta : sigm_LUT_pos_Q15[index] + delta;
}

template <std::totally_ordered T, bool Increasing> static inline void silk_insertion_sort_top_k(T* a, int* idx, const int L, const int K) {
  for (int i = 0; i < K; ++i) {
    idx[i] = i;
  }
  for (int i = 1; i < K; i++) {
    const T value = a[i];
    int j = i - 1;
    for (; j >= 0 && (Increasing ? value < a[j] : value > a[j]); j--) {
      a[j + 1] = a[j];
      idx[j + 1] = idx[j];
    }
    a[j + 1] = value;
    idx[j + 1] = i;
  }
  for (int i = K; i < L; i++) {
    const T value = a[i];
    if (Increasing ? value < a[K - 1] : value > a[K - 1]) {
      int j = K - 2;
      for (; j >= 0 && (Increasing ? value < a[j] : value > a[j]); j--) {
        a[j + 1] = a[j];
        idx[j + 1] = idx[j];
      }
      a[j + 1] = value;
      idx[j + 1] = i;
    }
  }
}

void silk_insertion_sort_increasing(opus_int32* a, int* idx, const int L, const int K) {
  silk_insertion_sort_top_k<opus_int32, true>(a, idx, L, K);
}

void silk_insertion_sort_increasing_all_values_int16(opus_int16* a, const int L) {
  for (int i = 1; i < L; i++) {
    const int value = a[i];
    int j = i - 1;
    for (; (j >= 0) && (value < a[j]); j--) {
      a[j + 1] = a[j];
    }
    a[j + 1] = value;
  }
}

[[nodiscard]] static auto silk_shifted_sum_sqr(std::span<const opus_int16> samples, const int shft, const opus_int32 initial = 0) noexcept
    -> opus_int32 {
  auto energy = initial;
  const auto even_count = samples.size() & ~std::size_t{1};
  for (std::size_t index = 0; index < even_count; index += 2) {
    auto pair_energy = static_cast<opus_int32>(samples[index]) * static_cast<opus_int32>(samples[index]);
    pair_energy = wrap_add(pair_energy, static_cast<opus_int32>(samples[index + 1]) * static_cast<opus_int32>(samples[index + 1]));
    energy = wrap_add(energy, pair_energy >> shft);
  }
  if (even_count != samples.size()) {
    const auto tail_energy = static_cast<opus_int32>(samples[even_count]) * static_cast<opus_int32>(samples[even_count]);
    energy = wrap_add(energy, tail_energy >> shft);
  }
  return energy;
}

void silk_sum_sqr_shift(opus_int32* energy, int* shift, const opus_int16* x, int len) {
  const auto samples = std::span<const opus_int16>{x, static_cast<std::size_t>(len)};
  auto shft = 31 - silk_CLZ32(len);
  auto nrg = silk_shifted_sum_sqr(samples, shft, len);
  shft = std::max(0, shft + 3 - silk_CLZ32(nrg));
  nrg = silk_shifted_sum_sqr(samples, shft);
  *shift = shft;
  *energy = nrg;
}

[[nodiscard]] static auto silk_stereo_pred_level_q13(const int coarse_index, const int fine_index) noexcept -> opus_int32 {
  const auto step = silk_mul_wb(silk_stereo_pred_quant_Q13[coarse_index + 1] - silk_stereo_pred_quant_Q13[coarse_index], 6554);
  return silk_stereo_pred_quant_Q13[coarse_index] +
         static_cast<opus_int32>(static_cast<opus_int16>(step)) * static_cast<opus_int32>(static_cast<opus_int16>(2 * fine_index + 1));
}

static void silk_stereo_split_lp_hp(const opus_int16* source, opus_int16* lowpass, opus_int16* highpass, int frame_length) {
  for (int index = 0; index < frame_length; ++index) {
    const auto sum = rounded_rshift<2>(source[index] + static_cast<opus_int32>(source[index + 2]) + wrap_shift_left(source[index + 1], 1));
    lowpass[index] = sum;
    highpass[index] = source[index + 1] - sum;
  }
}

static opus_int32 silk_stereo_find_predictor(opus_int32& ratio_Q14, const opus_int16 x[], const opus_int16 y[],
                                             std::span<opus_int32, 2> mid_res_amp_Q0, int length, int smooth_coef_Q16) {
  int scale, scale1, scale2;
  opus_int32 nrgx, nrgy, corr, pred_Q13, pred2_Q10;
  silk_sum_sqr_shift(&nrgx, &scale1, x, length);
  silk_sum_sqr_shift(&nrgy, &scale2, y, length);
  scale = std::max(scale1, scale2);
  scale = scale + (scale & 1);
  nrgy = ((nrgy) >> (scale - scale2));
  nrgx = ((nrgx) >> (scale - scale1));
  nrgx = std::max(nrgx, 1);
  corr = silk_inner_prod_aligned_scale(std::span<const opus_int16>{x, static_cast<std::size_t>(length)},
                                       std::span<const opus_int16>{y, static_cast<std::size_t>(length)}, scale);
  pred_Q13 = silk_DIV32_varQ(corr, nrgx, 13);
  pred_Q13 = clamp_value(pred_Q13, -(1 << 14), 1 << 14);
  pred2_Q10 = silk_mul_wb(pred_Q13, pred_Q13);
  smooth_coef_Q16 = std::max(smooth_coef_Q16, std::abs(pred2_Q10));
  scale >>= 1;
  const auto mid_amplitude = wrap_shift_left(silk_SQRT_APPROX(nrgx), scale);
  mid_res_amp_Q0[0] = silk_mla_wb(mid_res_amp_Q0[0], mid_amplitude - mid_res_amp_Q0[0], smooth_coef_Q16);
  nrgy = wrap_subtract(nrgy, wrap_shift_left(silk_mul_wb(corr, pred_Q13), 4));
  nrgy = wrap_add(nrgy, wrap_shift_left(silk_mul_wb(nrgx, pred2_Q10), 6));
  const auto residual_amplitude = wrap_shift_left(silk_SQRT_APPROX(nrgy), scale);
  mid_res_amp_Q0[1] = silk_mla_wb(mid_res_amp_Q0[1], residual_amplitude - mid_res_amp_Q0[1], smooth_coef_Q16);
  ratio_Q14 = silk_DIV32_varQ(mid_res_amp_Q0[1], std::max(mid_res_amp_Q0[0], opus_int32{1}), 14);
  ratio_Q14 = std::clamp<opus_int32>(ratio_Q14, 0, 32767);
  return pred_Q13;
}

static void silk_stereo_quant_pred(std::span<opus_int32, 2> pred_Q13, silk_stereo_pred_indices& ix) {
  opus_int32 quant_pred_Q13 = 0;
  for (int n = 0; n < 2; ++n) {
    auto searching = true;
    auto err_min_Q13 = 0x7FFFFFFF;
    for (int i = 0; i < 16 - 1 && searching; ++i) {
      for (int j = 0; j < 5; ++j) {
        const auto lvl_Q13 = silk_stereo_pred_level_q13(i, j);
        const auto err_Q13 = (((pred_Q13[n] - lvl_Q13) > 0) ? (pred_Q13[n] - lvl_Q13) : -(pred_Q13[n] - lvl_Q13));
        if (err_Q13 < err_min_Q13) {
          err_min_Q13 = err_Q13;
          quant_pred_Q13 = lvl_Q13;
          ix[n][0] = i;
          ix[n][1] = j;
        } else {
          searching = false;
          break;
        }
      }
    }
    ix[n][2] = (static_cast<opus_int32>((ix[n][0]) / (3)));
    ix[n][0] -= ix[n][2] * 3;
    pred_Q13[n] = quant_pred_Q13;
  }
  pred_Q13[0] -= pred_Q13[1];
}

void silk_stereo_decode_pred(ec_dec* psRangeDec, std::span<opus_int32, 2> pred_Q13) {
  std::array<std::array<int, 3>, 2> ix{};
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

void silk_stereo_decode_mid_only(ec_dec* psRangeDec, int& decode_only_mid) {
  decode_only_mid = ec_dec_icdf(psRangeDec, silk_stereo_only_code_mid_iCDF.data(), 8);
}

void silk_stereo_encode_pred(ec_enc* psRangeEnc, const silk_stereo_pred_indices& ix) {
  const int joint_index = 5 * ix[0][2] + ix[1][2];
  ec_enc_icdf(psRangeEnc, joint_index, silk_stereo_pred_joint_iCDF.data(), 8);
  for (int channel = 0; channel < 2; ++channel) {
    ec_enc_icdf(psRangeEnc, ix[channel][0], silk_uniform3_iCDF.data(), 8);
    ec_enc_icdf(psRangeEnc, ix[channel][1], silk_uniform5_iCDF.data(), 8);
  }
}

void silk_stereo_encode_mid_only(ec_enc* psRangeEnc, opus_int8 mid_only_flag) {
  ec_enc_icdf(psRangeEnc, mid_only_flag, silk_stereo_only_code_mid_iCDF.data(), 8);
}

void silk_stereo_LR_to_MS(stereo_enc_state* state, opus_int16 x1[], opus_int16 x2[], silk_stereo_pred_indices& ix,
                          opus_uint8* mid_only_flag, opus_int32 mid_side_rates_bps[], opus_int32 total_rate_bps, int prev_speech_act_Q8,
                          int toMono, int preserve_stereo, int fs_kHz, int frame_length) {
  int n, is10msFrame, denom_Q16, delta0_Q13, delta1_Q13;
  opus_int32 sum, diff, smooth_coef_Q16, pred_Q13[2], pred0_Q13, pred1_Q13, LP_ratio_Q14, HP_ratio_Q14, frac_Q16, frac_3_Q16,
      min_mid_rate_bps, width_Q14, w_Q24, deltaw_Q24;
  opus_int16* mid = &x1[-2];
  opus_int16 side[silk_max_frame_length + 2];
  for (n = 0; n < frame_length + 2; n++) {
    sum = x1[n - 2] + static_cast<opus_int32>(x2[n - 2]);
    diff = x1[n - 2] - static_cast<opus_int32>(x2[n - 2]);
    mid[n] = static_cast<opus_int16>(rounded_rshift<1>(sum));
    side[n] = rounded_rshift_to_int16<1>(diff);
  }
  copy_n_bytes(state->sMid.data(), static_cast<std::size_t>(2 * sizeof(opus_int16)), mid);
  copy_n_bytes(state->sSide.data(), static_cast<std::size_t>(2 * sizeof(opus_int16)), side);
  copy_n_bytes(&mid[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sMid.data());
  copy_n_bytes(&side[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sSide.data());
  std::array<opus_int16, silk_max_frame_length> LP_mid;
  std::array<opus_int16, silk_max_frame_length> HP_mid;
  std::array<opus_int16, silk_max_frame_length> LP_side;
  std::array<opus_int16, silk_max_frame_length> HP_side;
  silk_stereo_split_lp_hp(mid, LP_mid.data(), HP_mid.data(), frame_length);
  silk_stereo_split_lp_hp(side, LP_side.data(), HP_side.data(), frame_length);
  is10msFrame = frame_length == 10 * fs_kHz;
  smooth_coef_Q16 = is10msFrame ? 328 : 655;
  smooth_coef_Q16 = silk_mul_wb(prev_speech_act_Q8 * prev_speech_act_Q8, smooth_coef_Q16);
  pred_Q13[0] = silk_stereo_find_predictor(LP_ratio_Q14, LP_mid.data(), LP_side.data(),
                                           std::span<opus_int32, 2>{state->mid_side_amp_Q0.data(), 2}, frame_length, smooth_coef_Q16);
  pred_Q13[1] = silk_stereo_find_predictor(HP_ratio_Q14, HP_mid.data(), HP_side.data(),
                                           std::span<opus_int32, 2>{state->mid_side_amp_Q0.data() + 2, 2}, frame_length, smooth_coef_Q16);
  frac_Q16 = std::min<opus_int32>(HP_ratio_Q14 + 3 * LP_ratio_Q14, 1 << 16);
  total_rate_bps -= is10msFrame ? 1200 : 600;
  if (total_rate_bps < 1) {
    total_rate_bps = 1;
  }
  min_mid_rate_bps = 2000 + fs_kHz * 600;
  frac_3_Q16 = ((3) * (frac_Q16));
  mid_side_rates_bps[0] = silk_DIV32_varQ(total_rate_bps, (13 << 16) + frac_3_Q16, 16 + 3);
  if (mid_side_rates_bps[0] < min_mid_rate_bps) {
    mid_side_rates_bps[0] = min_mid_rate_bps;
    mid_side_rates_bps[1] = total_rate_bps - mid_side_rates_bps[0];
    width_Q14 = silk_DIV32_varQ(wrap_shift_left(mid_side_rates_bps[1], 1) - min_mid_rate_bps,
                                silk_mul_wb((1 << 16) + frac_3_Q16, min_mid_rate_bps), 14 + 2);
    width_Q14 = clamp_value<opus_int32>(width_Q14, 0, 1 << 14);
  } else {
    mid_side_rates_bps[1] = total_rate_bps - mid_side_rates_bps[0];
    width_Q14 = 1 << 14;
  }
  state->smth_width_Q14 = static_cast<opus_int16>(silk_mla_wb(state->smth_width_Q14, width_Q14 - state->smth_width_Q14, smooth_coef_Q16));
  *mid_only_flag = 0;
  const auto quantize_smoothed_pred = [&]() {
    pred_Q13[0] = silk_mul_i16_shift<14>(state->smth_width_Q14, pred_Q13[0]);
    pred_Q13[1] = silk_mul_i16_shift<14>(state->smth_width_Q14, pred_Q13[1]);
    silk_stereo_quant_pred(std::span<opus_int32, 2>{pred_Q13}, ix);
  };
  const auto reset_side_prediction = [&]() {
    width_Q14 = 0;
    pred_Q13[0] = 0;
    pred_Q13[1] = 0;
  };
  const auto mid_only_state_score = [&]() {
    const auto was_mid_only = state->width_prev_Q14 == 0;
    return std::max(score_below(static_cast<float>(8 * total_rate_bps), static_cast<float>((was_mid_only ? 13 : 11) * min_mid_rate_bps)),
                    score_below(static_cast<float>(silk_mul_wb(frac_Q16, state->smth_width_Q14)),
                                static_cast<float>(was_mid_only ? fixed_q<14>(0.05) : fixed_q<14>(0.02))));
  };
  const auto high_rate_mid_only_bias = clamp_value((static_cast<float>(total_rate_bps) - 30000.0f) * (1.0f / 18000.0f), 0.0f, 1.0f);
  const auto low_speech_mid_only_bias =
      std::min(clamp_value(1.0f - static_cast<float>(std::abs(total_rate_bps - 24000)) * (1.0f / 4000.0f), 0.0f, 1.0f),
               score_below(static_cast<float>(prev_speech_act_Q8), 32.0f));
  if (toMono) {
    reset_side_prediction();
    silk_stereo_quant_pred(std::span<opus_int32, 2>{pred_Q13}, ix);
  } else if (preserve_stereo != silk_preserve_stereo_force &&
             mid_only_state_score() + (preserve_stereo ? 0.f : silk_mid_only_score_bias * high_rate_mid_only_bias) +
                     silk_mid_only_low_speech_bias * low_speech_mid_only_bias >
                 0.0f) {
    quantize_smoothed_pred();
    reset_side_prediction();
    if (state->width_prev_Q14 == 0) {
      mid_side_rates_bps[0] = total_rate_bps;
      mid_side_rates_bps[1] = 0;
      *mid_only_flag = 1;
    }
  } else if (state->smth_width_Q14 > (static_cast<opus_int32>((0.95) * (static_cast<opus_int64>(1) << (14)) + 0.5))) {
    silk_stereo_quant_pred(std::span<opus_int32, 2>{pred_Q13}, ix);
    width_Q14 = 1 << 14;
  } else {
    quantize_smoothed_pred();
    width_Q14 = state->smth_width_Q14;
  }
  if (*mid_only_flag == 1) {
    state->silent_side_len += frame_length - 8 * fs_kHz;
    if (state->silent_side_len < 5 * fs_kHz) {
      *mid_only_flag = 0;
    } else {
      state->silent_side_len = 10000;
    }
  } else {
    state->silent_side_len = 0;
  }
  if (*mid_only_flag == 0 && mid_side_rates_bps[1] < 1) {
    mid_side_rates_bps[1] = 1;
    mid_side_rates_bps[0] = std::max(1, total_rate_bps - mid_side_rates_bps[1]);
  }
  pred0_Q13 = -state->pred_prev_Q13[0];
  pred1_Q13 = -state->pred_prev_Q13[1];
  w_Q24 = wrap_shift_left(state->width_prev_Q14, 10);
  denom_Q16 = (1 << 16) / (8 * fs_kHz);
  delta0_Q13 = -rounded_i16_product_shift<16>(pred_Q13[0] - state->pred_prev_Q13[0], denom_Q16);
  delta1_Q13 = -rounded_i16_product_shift<16>(pred_Q13[1] - state->pred_prev_Q13[1], denom_Q16);
  deltaw_Q24 = wrap_shift_left(silk_mul_wb(width_Q14 - state->width_prev_Q14, denom_Q16), 10);
  for (n = 0; n < 8 * fs_kHz; n++) {
    pred0_Q13 += delta0_Q13;
    pred1_Q13 += delta1_Q13;
    w_Q24 += deltaw_Q24;
    const auto side_Q8 = silk_mul_wb(w_Q24, side[n + 1]);
    sum = silk_stereo_apply_predictors_q8(side_Q8, silk_stereo_mid_mix_q9(mid, n), mid[n + 1], pred0_Q13, pred1_Q13);
    x2[n - 1] = rounded_rshift_to_int16<8>(sum);
  }
  pred0_Q13 = -pred_Q13[0];
  pred1_Q13 = -pred_Q13[1];
  w_Q24 = wrap_shift_left(width_Q14, 10);
  for (n = 8 * fs_kHz; n < frame_length; n++) {
    const auto side_Q8 = silk_mul_wb(w_Q24, side[n + 1]);
    sum = silk_stereo_apply_predictors_q8(side_Q8, silk_stereo_mid_mix_q9(mid, n), mid[n + 1], pred0_Q13, pred1_Q13);
    x2[n - 1] = rounded_rshift_to_int16<8>(sum);
  }
  state->pred_prev_Q13[0] = static_cast<opus_int16>(pred_Q13[0]);
  state->pred_prev_Q13[1] = static_cast<opus_int16>(pred_Q13[1]);
  state->width_prev_Q14 = static_cast<opus_int16>(width_Q14);
}

void silk_stereo_MS_to_LR(stereo_dec_state* state, opus_int16 x1[], opus_int16 x2[], const opus_int32 pred_Q13[], int fs_kHz,
                          int frame_length) {
  copy_n_bytes(state->sMid.data(), static_cast<std::size_t>(2 * sizeof(opus_int16)), x1);
  copy_n_bytes(state->sSide.data(), static_cast<std::size_t>(2 * sizeof(opus_int16)), x2);
  copy_n_bytes(&x1[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sMid.data());
  copy_n_bytes(&x2[frame_length], static_cast<std::size_t>(2 * sizeof(opus_int16)), state->sSide.data());
  opus_int32 pred0_Q13 = state->pred_prev_Q13[0];
  opus_int32 pred1_Q13 = state->pred_prev_Q13[1];
  const opus_int32 denom_Q16 = static_cast<opus_int32>((static_cast<opus_int32>(1) << 16) / (8 * fs_kHz));
  const opus_int32 delta0_Q13 = rounded_i16_product_shift<16>(pred_Q13[0] - state->pred_prev_Q13[0], denom_Q16);
  const opus_int32 delta1_Q13 = rounded_i16_product_shift<16>(pred_Q13[1] - state->pred_prev_Q13[1], denom_Q16);
  for (int n = 0; n < 8 * fs_kHz; ++n) {
    pred0_Q13 += delta0_Q13;
    pred1_Q13 += delta1_Q13;
    const auto side_Q8 = wrap_shift_left(x2[n + 1], 8);
    const opus_int32 sum = silk_stereo_apply_predictors_q8(side_Q8, silk_stereo_mid_mix_q9(x1, n), x1[n + 1], pred0_Q13, pred1_Q13);
    x2[n + 1] = rounded_rshift_to_int16<8>(sum);
  }
  pred0_Q13 = pred_Q13[0];
  pred1_Q13 = pred_Q13[1];
  for (int n = 8 * fs_kHz; n < frame_length; ++n) {
    const auto side_Q8 = wrap_shift_left(x2[n + 1], 8);
    const opus_int32 sum = silk_stereo_apply_predictors_q8(side_Q8, silk_stereo_mid_mix_q9(x1, n), x1[n + 1], pred0_Q13, pred1_Q13);
    x2[n + 1] = rounded_rshift_to_int16<8>(sum);
  }
  state->pred_prev_Q13[0] = pred_Q13[0];
  state->pred_prev_Q13[1] = pred_Q13[1];
  for (int n = 0; n < frame_length; ++n) {
    const opus_int32 sum = x1[n + 1] + static_cast<opus_int32>(x2[n + 1]);
    const opus_int32 diff = x1[n + 1] - static_cast<opus_int32>(x2[n + 1]);
    x1[n + 1] = saturate_int16_from_int32(sum);
    x2[n + 1] = saturate_int16_from_int32(diff);
  }
}

void silk_LPC_fit(opus_int16* a_QOUT, opus_int32* a_QIN, const int d) {
  constexpr int QOUT = 12, QIN = 17;
  int i = 0;
  int idx = 0;
  for (; i < 10; ++i) {
    opus_int32 maxabs = 0;
    for (int k = 0; k < d; ++k) {
      const opus_int32 absval = std::abs(a_QIN[k]);
      if (absval > maxabs) {
        maxabs = absval;
        idx = k;
      }
    }
    maxabs = rounded_rshift(maxabs, QIN - QOUT);
    if (maxabs > 0x7FFF) {
      maxabs = std::min(maxabs, 163838);
      const opus_int32 chirp_Q16 = fixed_q<16>(0.999) - wrap_shift_left(maxabs - 0x7FFF, 14) / ((maxabs * (idx + 1)) >> 2);
      silk_bwexpander(a_QIN, static_cast<std::size_t>(d), chirp_Q16);
    } else {
      break;
    }
  }
  if (i == 10) {
    for (int k = 0; k < d; ++k) {
      a_QOUT[k] = resampler_round_shift_saturate_int16(a_QIN[k], QIN - QOUT);
      a_QIN[k] = wrap_shift_left(a_QOUT[k], QIN - QOUT);
    }
  } else {
    for (int k = 0; k < d; ++k) {
      a_QOUT[k] = static_cast<opus_int16>(rounded_rshift(a_QIN[k], QIN - QOUT));
    }
  }
}

static void silk_apply_sine_window_FLP(std::span<float> px_win, const std::span<const float> px, const int win_type) {
  const auto length = static_cast<int>(px_win.size());
  const float freq = 3.1415926536f / (length + 1);
  const float c = 2.0f - freq * freq;
  float S0 = win_type < 2 ? 0.0f : 1.0f;
  float S1 = win_type < 2 ? freq : 0.5f * c;
  for (int block_index = 0; block_index < length / 4; ++block_index) {
    const auto k = static_cast<std::size_t>(block_index * 4);
    px_win[k + 0] = px[k + 0] * 0.5f * (S0 + S1);
    px_win[k + 1] = px[k + 1] * S1;
    S0 = c * S1 - S0;
    px_win[k + 2] = px[k + 2] * 0.5f * (S1 + S0);
    px_win[k + 3] = px[k + 3] * S0;
    S1 = c * S0 - S1;
  }
}

static void silk_corrVector_FLP(const std::span<const float> x, const std::span<const float> t, std::span<float> Xt) {
  const auto order = Xt.size();
  for (auto lag = std::size_t{}; lag < order; ++lag) {
    const auto offset = order - lag - 1;
    Xt[lag] = static_cast<float>(silk_inner_product_FLP_c(x.data() + offset, t.data(), static_cast<int>(t.size())));
  }
}

static void silk_corrMatrix_FLP(const std::span<const float> x, const int L, std::span<float> XX) {
  constexpr int Order = 5;
  auto xx = [XX](const int row, const int column) mutable -> float& {
    return XX[static_cast<std::size_t>(row * Order + column)];
  };
  const auto last = static_cast<std::size_t>(Order - 1);
  double energy = silk_energy_FLP(x.data() + last, L);
  xx(0, 0) = static_cast<float>(energy);
  for (int j = 1; j < Order; ++j) {
    energy += x[last - static_cast<std::size_t>(j)] * x[last - static_cast<std::size_t>(j)] -
              x[last + static_cast<std::size_t>(L - j)] * x[last + static_cast<std::size_t>(L - j)];
    xx(j, j) = static_cast<float>(energy);
  }
  for (int lag = 1; lag < Order; ++lag) {
    energy = silk_inner_product_FLP_c(x.data() + last, x.data() + last - static_cast<std::size_t>(lag), L);
    xx(lag, 0) = static_cast<float>(energy);
    xx(0, lag) = static_cast<float>(energy);
    for (int j = 1; j < (Order - lag); ++j) {
      const auto left = last - static_cast<std::size_t>(j);
      const auto right = left - static_cast<std::size_t>(lag);
      const auto tail = last + static_cast<std::size_t>(L - j);
      energy += x[left] * x[right] - x[tail] * x[tail - static_cast<std::size_t>(lag)];
      xx(lag + j, j) = static_cast<float>(energy);
      xx(j, lag + j) = static_cast<float>(energy);
    }
  }
}

static void silk_encode_indices_and_pulses(silk_encoder_state* psEncC, ec_enc* psRangeEnc, int condCoding) {
  silk_encode_indices(psEncC, psEncC->indices, psRangeEnc, condCoding);
  silk_process_pulses<true>(psRangeEnc,
                            std::span<opus_int8>{psEncC->pulses, static_cast<std::size_t>((psEncC->frame_length + 16 - 1) & ~(16 - 1))},
                            psEncC->indices.signalType, psEncC->indices.quantOffsetType, psEncC->frame_length);
}

static void silk_generate_lbrr(silk_encoder_state_FLP* psEnc, silk_lbrr_channel_state* lbrr, silk_encoder_control_FLP* control,
                               const opus_int16* samples, int condCoding, int gain_reduction, bool protect_quiet) {
  if (!protect_quiet && psEnc->sCmn.speech_activity_Q8 <= fixed_q<8>(0.3f)) {
    return;
  }
  const auto frame = static_cast<std::size_t>(psEnc->sCmn.nFramesEncoded);
  lbrr->flags[frame] = 1;
  lbrr->indices[frame] = psEnc->sCmn.indices;
  lbrr->nsq = psEnc->sCmn.sNSQ;
  std::array<float, 4> original_gains;
  std::copy_n(control->Gains, static_cast<std::size_t>(psEnc->sCmn.nb_subfr), original_gains.begin());
  auto& indices = lbrr->indices[frame];
  if (frame == 0 || lbrr->flags[frame - 1] == 0) {
    lbrr->previous_gain_index = psEnc->sShape.LastGainIndex;
    indices.GainsIndices[0] =
        static_cast<opus_int8>(std::min<int>(indices.GainsIndices[0] + std::max(lbrr->gain_increase - gain_reduction, 2), 63));
  }
  std::array<opus_int32, 4> gains_Q16;
  silk_gains_dequant(gains_Q16.data(), indices.GainsIndices, &lbrr->previous_gain_index, condCoding == 2, psEnc->sCmn.nb_subfr);
  for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) {
    control->Gains[index] = gains_Q16[static_cast<std::size_t>(index)] * (1.0f / 65536.0f);
  }
  const float original_lambda = control->Lambda;
  control->Lambda *= psEnc->sCmn.nb_subfr != 2                           ? .9f
                     : psEnc->sCmn.input_tilt_Q15 < -10000               ? .95f
                     : psEnc->sCmn.speech_activity_Q8 < fixed_q<8>(.75f) ? .8f
                                                                         : .9f;
  silk_NSQ_wrapper_FLP(psEnc, control, &indices, &lbrr->nsq, lbrr->pulses[frame].data(), samples);
  control->Lambda = original_lambda;
  std::copy_n(original_gains.begin(), static_cast<std::size_t>(psEnc->sCmn.nb_subfr), control->Gains);
}

struct silk_gain_search_bound {
  opus_int32 bits{}, multiplier{}, id{-1};
};

void silk_encode_frame_FLP(silk_encoder_state_FLP* psEnc, silk_lbrr_channel_state* lbrr, opus_int32* pnBytesOut, ec_enc* psRangeEnc,
                           int condCoding, int maxBits, int useCBR, int lbrr_gain_reduction, bool protect_quiet_lbrr) {
  silk_encoder_control_FLP sEncCtrl;
  psEnc->sCmn.indices.Seed = psEnc->sCmn.frameCounter++ & 3;
  auto* x_frame = psEnc->x_buf + psEnc->sCmn.ltp_mem_length;
  auto& low_pass = psEnc->sCmn.sLP;
  if (low_pass.mode != 0) {
    constexpr auto transition_frames = 5120 / (5 * 4);
    std::array<opus_int32, 3> B_Q28{};
    std::array<opus_int32, 2> A_Q28{};
    auto fac_Q16 = wrap_shift_left(transition_frames - low_pass.transition_frame_no, 16 - 6);
    const auto index = fac_Q16 >> 16;
    fac_Q16 -= wrap_shift_left(index, 16);
    silk_LP_interpolate_filter_taps(B_Q28.data(), A_Q28.data(), index, fac_Q16);
    low_pass.transition_frame_no = clamp_value(low_pass.transition_frame_no + low_pass.mode, 0, transition_frames);
    silk_biquad_alt_stride1(psEnc->sCmn.inputBuf + 1, B_Q28.data(), A_Q28.data(), low_pass.In_LP_State.data(), psEnc->sCmn.inputBuf + 1,
                            psEnc->sCmn.frame_length);
  }
  silk_short2float_array(x_frame + 5 * psEnc->sCmn.fs_kHz, psEnc->sCmn.inputBuf + 1, psEnc->sCmn.frame_length);
  for (int i = 0; i < 8; i++) {
    x_frame[5 * psEnc->sCmn.fs_kHz + i * (psEnc->sCmn.frame_length >> 3)] += (1 - (i & 2)) * 1e-6f;
  }
  if (!psEnc->sCmn.prefillFlag) {
    {
      std::array<float, silk_max_ltp_buffer_length + silk_max_frame_length / 2> pitch_residual;
      auto* res_pitch_frame = pitch_residual.data() + psEnc->sCmn.ltp_mem_length;
      silk_find_pitch_lags_FLP(psEnc, &sEncCtrl, pitch_residual.data(), x_frame);
      silk_noise_shape_analysis_FLP(psEnc, &sEncCtrl, res_pitch_frame, x_frame);
      silk_find_pred_coefs_FLP(psEnc, &sEncCtrl, res_pitch_frame, x_frame, condCoding);
      silk_process_gains_FLP(psEnc, &sEncCtrl, condCoding);
    }
    std::array<opus_int16, silk_max_frame_length> nsq_samples;
    for (int index = 0; index < psEnc->sCmn.frame_length; ++index) {
      nsq_samples[index] = static_cast<opus_int16>(float2int(x_frame[index]));
    }
    if (lbrr != nullptr && lbrr->enabled) {
      silk_generate_lbrr(psEnc, lbrr, &sEncCtrl, nsq_samples.data(), condCoding, lbrr_gain_reduction, protect_quiet_lbrr);
    }
    constexpr int max_iterations = 6;
    silk_gain_search_bound lower, upper;
    opus_int32 gainsID = silk_gains_ID(psEnc->sCmn.indices.GainsIndices, psEnc->sCmn.nb_subfr);
    opus_int16 gainMult_Q8 = 256;
    opus_int8 LastGainIndex_copy2 = 0;
    opus_int32 pGains_Q16[4];
    int gain_lock[4] = {};
    opus_int16 best_gain_mult[4];
    int best_sum[4];
    silk_nsq_state sNSQ_copy[2];
    ec_enc sRangeEnc_copy, sRangeEnc_copy2;
    sRangeEnc_copy = *psRangeEnc;
    sNSQ_copy[0] = psEnc->sCmn.sNSQ;
    const opus_int32 seed_copy = psEnc->sCmn.indices.Seed;
    const opus_int16 ec_prevLagIndex_copy = psEnc->sCmn.ec_prevLagIndex;
    const int ec_prevSignalType_copy = psEnc->sCmn.ec_prevSignalType;
    opus_uint8 ec_buf_copy[1275];
    const int bits_margin = useCBR ? 5 : maxBits / 4;
    for (int iter = 0;; ++iter) {
      opus_int32 nBits;
      if (gainsID == lower.id) {
        nBits = lower.bits;
      } else if (gainsID == upper.id) {
        nBits = upper.bits;
      } else {
        if (iter > 0) {
          *psRangeEnc = sRangeEnc_copy;
          psEnc->sCmn.sNSQ = sNSQ_copy[0];
          psEnc->sCmn.indices.Seed = seed_copy;
          psEnc->sCmn.ec_prevLagIndex = ec_prevLagIndex_copy;
          psEnc->sCmn.ec_prevSignalType = ec_prevSignalType_copy;
        }
        silk_NSQ_wrapper_FLP(psEnc, &sEncCtrl, &psEnc->sCmn.indices, &psEnc->sCmn.sNSQ, psEnc->sCmn.pulses, nsq_samples.data());
        if (iter == max_iterations && lower.id < 0) {
          sRangeEnc_copy2 = *psRangeEnc;
        }
        silk_encode_indices_and_pulses(&psEnc->sCmn, psRangeEnc, condCoding);
        nBits = ec_tell(psRangeEnc);
        if (iter == max_iterations && lower.id < 0 && nBits > maxBits) {
          *psRangeEnc = sRangeEnc_copy2;
          psEnc->sShape.LastGainIndex = sEncCtrl.lastGainIndexPrev;
          std::fill_n(psEnc->sCmn.indices.GainsIndices, static_cast<std::size_t>(psEnc->sCmn.nb_subfr), static_cast<opus_int8>(4));
          if (condCoding != 2) {
            psEnc->sCmn.indices.GainsIndices[0] = sEncCtrl.lastGainIndexPrev;
          }
          psEnc->sCmn.ec_prevLagIndex = ec_prevLagIndex_copy;
          psEnc->sCmn.ec_prevSignalType = ec_prevSignalType_copy;
          zero_n_items(psEnc->sCmn.pulses, static_cast<std::size_t>(psEnc->sCmn.frame_length));
          silk_encode_indices_and_pulses(&psEnc->sCmn, psRangeEnc, condCoding);
          nBits = ec_tell(psRangeEnc);
        }
        if (!useCBR && iter == 0 && nBits <= maxBits) {
          break;
        }
      }
      if (iter == max_iterations) {
        if (lower.id >= 0 && (gainsID == lower.id || nBits > maxBits)) {
          *psRangeEnc = sRangeEnc_copy2;
          copy_n_bytes(ec_buf_copy, static_cast<std::size_t>(sRangeEnc_copy2.offs), psRangeEnc->buf);
          psEnc->sCmn.sNSQ = sNSQ_copy[1];
          psEnc->sShape.LastGainIndex = LastGainIndex_copy2;
        }
        break;
      }
      if (nBits > maxBits) {
        if (lower.id < 0 && iter >= 2) {
          sEncCtrl.Lambda = std::max(sEncCtrl.Lambda * 1.5f, 1.5f);
          psEnc->sCmn.indices.quantOffsetType = 0;
          upper = {};
        } else {
          upper = {nBits, gainMult_Q8, gainsID};
        }
      } else if (nBits < maxBits - bits_margin) {
        if (gainsID != lower.id) {
          sRangeEnc_copy2 = *psRangeEnc;
          copy_n_bytes(psRangeEnc->buf, static_cast<std::size_t>(psRangeEnc->offs), ec_buf_copy);
          sNSQ_copy[1] = psEnc->sCmn.sNSQ;
          LastGainIndex_copy2 = psEnc->sShape.LastGainIndex;
        }
        lower = {nBits, gainMult_Q8, gainsID};
      } else {
        break;
      }
      if (lower.id < 0 && nBits > maxBits) {
        for (int i = 0; i < psEnc->sCmn.nb_subfr; ++i) {
          int sum = 0;
          for (int j = i * psEnc->sCmn.subfr_length; j < (i + 1) * psEnc->sCmn.subfr_length; ++j) {
            sum += std::abs(psEnc->sCmn.pulses[j]);
          }
          if (iter == 0 || (sum < best_sum[i] && !gain_lock[i])) {
            best_sum[i] = sum;
            best_gain_mult[i] = gainMult_Q8;
          } else {
            gain_lock[i] = 1;
          }
        }
      }
      if (lower.id < 0 || upper.id < 0) {
        gainMult_Q8 = nBits > maxBits ? std::min(1024, gainMult_Q8 * 3 / 2) : std::max(64, gainMult_Q8 * 4 / 5);
      } else {
        gainMult_Q8 = lower.multiplier + ((upper.multiplier - lower.multiplier) * (maxBits - lower.bits)) / (upper.bits - lower.bits);
        const opus_int32 quarter_range = (upper.multiplier - lower.multiplier) >> 2;
        gainMult_Q8 = static_cast<opus_int16>(
            clamp_value<opus_int32>(gainMult_Q8, upper.multiplier - quarter_range, lower.multiplier + quarter_range));
      }
      for (int i = 0; i < psEnc->sCmn.nb_subfr; ++i) {
        const opus_int16 tmp = gain_lock[i] ? best_gain_mult[i] : gainMult_Q8;
        const auto gain_Q8 =
            static_cast<opus_int32>((static_cast<opus_int64>(sEncCtrl.GainsUnq_Q16[i]) * static_cast<opus_int16>(tmp)) >> 16);
        const auto clamped_gain_Q8 = clamp_value(gain_Q8, opus_int32_min >> 8, opus_int32_max >> 8);
        pGains_Q16[i] = wrap_shift_left(clamped_gain_Q8, 8);
      }
      psEnc->sShape.LastGainIndex = sEncCtrl.lastGainIndexPrev;
      silk_gains_quant(psEnc->sCmn.indices.GainsIndices, pGains_Q16, &psEnc->sShape.LastGainIndex, condCoding == 2, psEnc->sCmn.nb_subfr);
      gainsID = silk_gains_ID(psEnc->sCmn.indices.GainsIndices, psEnc->sCmn.nb_subfr);
      for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) {
        sEncCtrl.Gains[index] = pGains_Q16[index] / 65536.0f;
      }
    }
  }
  move_n_bytes(&psEnc->x_buf[psEnc->sCmn.frame_length],
               static_cast<std::size_t>((psEnc->sCmn.ltp_mem_length + 5 * psEnc->sCmn.fs_kHz) * sizeof(float)), psEnc->x_buf);
  if (psEnc->sCmn.prefillFlag) {
    *pnBytesOut = 0;
    return;
  }
  psEnc->sCmn.prevLag = sEncCtrl.pitchL[psEnc->sCmn.nb_subfr - 1];
  psEnc->sCmn.prevSignalType = psEnc->sCmn.indices.signalType;
  psEnc->sCmn.first_frame_after_reset = 0;
  *pnBytesOut = ((ec_tell(psRangeEnc) + 7) >> (3));
}

static void silk_find_LPC_FLP(silk_encoder_state* psEncC, opus_int16 NLSF_Q15[], const float x[], const float minInvGain) {
  int k, subfr_length;
  float a[16]{};
  float res_nrg, res_nrg_2nd, res_nrg_interp;
  opus_int16 NLSF0_Q15[16]{};
  float a_tmp[16]{}, LPC_res[((5 * 4) * 16) + 4 * 16]{};
  subfr_length = psEncC->subfr_length + psEncC->predictLPCOrder;
  psEncC->indices.NLSFInterpCoef_Q2 = 4;
  res_nrg = silk_burg_modified_FLP(a, x, minInvGain, subfr_length, psEncC->nb_subfr, psEncC->predictLPCOrder);
  if (psEncC->Complexity >= 4 && !psEncC->first_frame_after_reset && psEncC->nb_subfr == 4) {
    res_nrg -= silk_burg_modified_FLP(a_tmp, x + (4 / 2) * subfr_length, minInvGain, subfr_length, 4 / 2, psEncC->predictLPCOrder);
    silk_A2NLSF_FLP(NLSF_Q15, a_tmp, psEncC->predictLPCOrder);
    res_nrg_2nd = 3.40282346638528859811704183484516925e+38F;
    for (k = 3; k >= 0; k--) {
      silk_interpolate(std::span<opus_int16>{NLSF0_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)},
                       std::span<const opus_int16>{psEncC->prev_NLSFq_Q15.data(), static_cast<std::size_t>(psEncC->predictLPCOrder)},
                       std::span<const opus_int16>{NLSF_Q15, static_cast<std::size_t>(psEncC->predictLPCOrder)}, k);
      silk_NLSF2A_FLP(a_tmp, NLSF0_Q15, psEncC->predictLPCOrder);
      silk_LPC_analysis_filter_FLP(LPC_res, a_tmp, x, 2 * subfr_length, psEncC->predictLPCOrder);
      res_nrg_interp =
          static_cast<float>(silk_energy_FLP(LPC_res + psEncC->predictLPCOrder, subfr_length - psEncC->predictLPCOrder) +
                             silk_energy_FLP(LPC_res + psEncC->predictLPCOrder + subfr_length, subfr_length - psEncC->predictLPCOrder));
      if (res_nrg_interp < res_nrg) {
        res_nrg = res_nrg_interp;
        psEncC->indices.NLSFInterpCoef_Q2 = static_cast<opus_uint8>(k);
      } else if (res_nrg_interp > res_nrg_2nd) {
        break;
      }
      res_nrg_2nd = res_nrg_interp;
    }
  }
  if (psEncC->indices.NLSFInterpCoef_Q2 == 4) {
    silk_A2NLSF_FLP(NLSF_Q15, a, psEncC->predictLPCOrder);
  }
}

static void silk_find_LTP_FLP(float XX[4 * 5 * 5], float xX[4 * 5], const float r_ptr[], const int lag[4], const int subfr_length,
                              const int nb_subfr) {
  auto* xX_ptr = xX;
  auto* XX_ptr = XX;
  for (int k = 0; k < nb_subfr; ++k) {
    const auto* lag_ptr = r_ptr - (lag[k] + 5 / 2);
    silk_corrMatrix_FLP(std::span<const float>{lag_ptr, static_cast<std::size_t>(subfr_length + 4)}, subfr_length,
                        std::span<float>{XX_ptr, static_cast<std::size_t>(25)});
    silk_corrVector_FLP(std::span<const float>{lag_ptr, static_cast<std::size_t>(subfr_length + 4)},
                        std::span<const float>{r_ptr, static_cast<std::size_t>(subfr_length)},
                        std::span<float>{xX_ptr, static_cast<std::size_t>(5)});
    const float xx = static_cast<float>(silk_energy_FLP(r_ptr, subfr_length + 5));
    const float temp = 1.0f / std::max(xx, 0.015f * (XX_ptr[0] + XX_ptr[24]) + 1.0f);
    silk_scale_copy_vector_FLP(XX_ptr, XX_ptr, temp, 5 * 5);
    silk_scale_copy_vector_FLP(xX_ptr, xX_ptr, temp, 5);
    r_ptr += subfr_length;
    XX_ptr += 5 * 5;
    xX_ptr += 5;
  }
}

void silk_find_pitch_lags_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, float res[], const float x[]) {
  float thrhld, res_nrg;
  float auto_corr[16 + 1], A[16], refl_coef[16];
  auto* Wsig = res;
  const int lookahead = 2 * psEnc->sCmn.fs_kHz;
  const int pitch_window = (psEnc->sCmn.nb_subfr == 4 ? 24 : 14) * psEnc->sCmn.fs_kHz;
  const int buf_len = lookahead + psEnc->sCmn.frame_length + psEnc->sCmn.ltp_mem_length;
  const auto* x_buf = x - psEnc->sCmn.ltp_mem_length;
  const auto* x_buf_ptr = x_buf + buf_len - pitch_window;
  auto* Wsig_ptr = Wsig;
  silk_apply_sine_window_FLP(std::span<float>{Wsig_ptr, static_cast<std::size_t>(lookahead)},
                             std::span<const float>{x_buf_ptr, static_cast<std::size_t>(lookahead)}, 1);
  Wsig_ptr += lookahead;
  x_buf_ptr += lookahead;
  copy_n_bytes(x_buf_ptr, static_cast<std::size_t>((pitch_window - 2 * lookahead) * sizeof(float)), Wsig_ptr);
  Wsig_ptr += pitch_window - 2 * lookahead;
  x_buf_ptr += pitch_window - 2 * lookahead;
  silk_apply_sine_window_FLP(std::span<float>{Wsig_ptr, static_cast<std::size_t>(lookahead)},
                             std::span<const float>{x_buf_ptr, static_cast<std::size_t>(lookahead)}, 2);
  silk_autocorrelation_FLP(auto_corr, Wsig, pitch_window, psEnc->sCmn.pitchEstimationLPCOrder + 1);
  auto_corr[0] += auto_corr[0] * 1e-3f + 1;
  res_nrg = silk_schur_FLP(refl_coef, auto_corr, psEnc->sCmn.pitchEstimationLPCOrder);
  psEncCtrl->predGain = auto_corr[0] / std::max(res_nrg, 1.0f);
  silk_k2a_FLP(A, refl_coef, psEnc->sCmn.pitchEstimationLPCOrder);
  silk_bwexpander_FLP(std::span<float>{A, static_cast<std::size_t>(psEnc->sCmn.pitchEstimationLPCOrder)}, 0.99f);
  silk_LPC_analysis_filter_FLP(res, A, x_buf, buf_len, psEnc->sCmn.pitchEstimationLPCOrder);
  if (psEnc->sCmn.indices.signalType != 0 && psEnc->sCmn.first_frame_after_reset == 0) {
    thrhld = 0.6f;
    thrhld -= 0.004f * psEnc->sCmn.pitchEstimationLPCOrder;
    thrhld -= 0.1f * psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f);
    thrhld -= 0.15f * (psEnc->sCmn.prevSignalType >> 1);
    thrhld -= 0.1f * psEnc->sCmn.input_tilt_Q15 * (1.0f / 32768.0f);
    const auto pitch =
        silk_pitch_analysis_core_FLP(res, psEnc->LTPCorr, psEnc->sCmn.prevLag, psEnc->sCmn.pitchEstimationThreshold_Q16 / 65536.0f, thrhld,
                                     psEnc->sCmn.fs_kHz, psEnc->sCmn.pitchEstimationComplexity, psEnc->sCmn.nb_subfr);
    copy_n_items(pitch.lags.data(), static_cast<std::size_t>(psEnc->sCmn.nb_subfr), psEncCtrl->pitchL);
    psEnc->sCmn.indices.lagIndex = pitch.lag_index;
    psEnc->sCmn.indices.contourIndex = pitch.contour_index;
    psEnc->LTPCorr = pitch.correlation;
    psEnc->sCmn.indices.signalType = pitch.voiced ? 2 : 1;
  } else {
    zero_object(psEncCtrl->pitchL);
    psEnc->sCmn.indices.lagIndex = 0;
    psEnc->sCmn.indices.contourIndex = 0;
    psEnc->LTPCorr = 0;
  }
}

void silk_find_pred_coefs_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, const float res_pitch[], const float x[],
                              int condCoding) {
  int i;
  float invGains[4];
  opus_int16 NLSF_Q15[16]{};
  const float* x_ptr;
  float* x_pre_ptr;
  std::array<float, 4 * (silk_max_subfr_length + silk_nlsf_max_order)> lpc_input_storage;
  auto* LPC_in_pre = lpc_input_storage.data();
  auto* XXLTP = LPC_in_pre;
  auto* xXLTP = XXLTP + psEnc->sCmn.nb_subfr * 5 * 5;
  float minInvGain;
  for (i = 0; i < psEnc->sCmn.nb_subfr; i++) {
    invGains[i] = 1.0f / psEncCtrl->Gains[i];
  }
  if (psEnc->sCmn.indices.signalType == 2) {
    silk_find_LTP_FLP(XXLTP, xXLTP, res_pitch, psEncCtrl->pitchL, psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr);
    silk_quant_LTP_gains_FLP(psEncCtrl->LTPCoef, psEnc->sCmn.indices.LTPIndex, &psEnc->sCmn.indices.PERIndex, &psEnc->sCmn.sum_log_gain_Q7,
                             &psEncCtrl->LTPredCodGain, XXLTP, xXLTP, psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr);
    silk_LTP_scale_ctrl_FLP(psEnc, psEncCtrl, condCoding);
    silk_LTP_analysis_filter_FLP(LPC_in_pre, x - psEnc->sCmn.predictLPCOrder, psEncCtrl->LTPCoef, psEncCtrl->pitchL, invGains,
                                 psEnc->sCmn.subfr_length, psEnc->sCmn.nb_subfr, psEnc->sCmn.predictLPCOrder);
  } else {
    x_ptr = x - psEnc->sCmn.predictLPCOrder;
    x_pre_ptr = LPC_in_pre;
    for (i = 0; i < psEnc->sCmn.nb_subfr; i++) {
      silk_scale_copy_vector_FLP(x_pre_ptr, x_ptr, invGains[i], psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder);
      x_pre_ptr += psEnc->sCmn.subfr_length + psEnc->sCmn.predictLPCOrder;
      x_ptr += psEnc->sCmn.subfr_length;
    }
    zero_n_items(psEncCtrl->LTPCoef, static_cast<std::size_t>(psEnc->sCmn.nb_subfr * 5));
    psEncCtrl->LTPredCodGain = 0.0f;
    psEnc->sCmn.sum_log_gain_Q7 = 0;
  }
  if (psEnc->sCmn.first_frame_after_reset) {
    minInvGain = 1.0f / 1e2f;
  } else {
    minInvGain = static_cast<float>(std::pow(2, psEncCtrl->LTPredCodGain / 3)) / 1e4f;
    minInvGain /= 0.25f + 0.75f * psEncCtrl->coding_quality;
  }
  silk_find_LPC_FLP(&psEnc->sCmn, NLSF_Q15, LPC_in_pre, minInvGain);
  silk_process_NLSFs_FLP(&psEnc->sCmn, psEncCtrl->PredCoef, NLSF_Q15, psEnc->sCmn.prev_NLSFq_Q15.data());
  silk_residual_energy_FLP(psEncCtrl->ResNrg, LPC_in_pre, psEncCtrl->PredCoef, psEncCtrl->Gains, psEnc->sCmn.subfr_length,
                           psEnc->sCmn.nb_subfr, psEnc->sCmn.predictLPCOrder);
  copy_n_bytes(NLSF_Q15, static_cast<std::size_t>(sizeof(psEnc->sCmn.prev_NLSFq_Q15)), psEnc->sCmn.prev_NLSFq_Q15.data());
}
namespace {
template <std::size_t Order>
  requires(Order > 0)
auto silk_lpc_analysis_filter_impl(std::span<float> residual, std::span<const float, Order> pred_coef,
                                   std::span<const float> signal) noexcept -> void {
  for (auto index = static_cast<int>(Order); index < static_cast<int>(signal.size()); ++index) {
    const auto* history = signal.data() + index - 1;
    auto prediction = 0.0f;
    for (std::size_t tap = 0; tap < Order; ++tap) {
      prediction += history[-static_cast<int>(tap)] * pred_coef[tap];
    }
    residual[index] = history[1] - prediction;
  }
}
struct max_abs_result {
  float value;
  int index;
};
[[nodiscard]] constexpr auto max_abs_index(std::span<const float> coefs) noexcept -> max_abs_result {
  auto max_abs = -1.0f;
  auto max_index = 0;
  for (auto index = 0; index < static_cast<int>(coefs.size()); ++index) {
    const auto magnitude = static_cast<float>(std::fabs(coefs[index]));
    if (magnitude > max_abs) {
      max_abs = magnitude;
      max_index = index;
    }
  }
  return {max_abs, max_index};
}
} // namespace

void silk_LPC_analysis_filter_FLP(float r_LPC[], const float PredCoef[], const float s[], const int length, const int Order) {
  auto residual = std::span<float>{r_LPC, static_cast<std::size_t>(length)};
  auto signal = std::span<const float>{s, static_cast<std::size_t>(length)};
  if (Order == 10) {
    silk_lpc_analysis_filter_impl<10>(residual, std::span<const float, 10>{PredCoef, 10}, signal);
  } else
    silk_lpc_analysis_filter_impl<16>(residual, std::span<const float, 16>{PredCoef, 16}, signal);
  std::fill_n(residual.data(), static_cast<std::size_t>(Order), 0.0f);
}

void silk_LTP_analysis_filter_FLP(float* LTP_res, const float* x, const float B[5 * 4], const int pitchL[4], const float invGains[4],
                                  const int subfr_length, const int nb_subfr, const int pre_length) {
  auto* x_ptr = x;
  auto* LTP_res_ptr = LTP_res;
  for (int k = 0; k < nb_subfr; ++k) {
    const auto* b = B + k * 5;
    const auto* x_lag_ptr = x_ptr - pitchL[k];
    const float inv_gain = invGains[k];
    for (int i = 0; i < subfr_length + pre_length; ++i) {
      auto residual = x_ptr[i];
      for (int j = 0; j < 5; ++j) {
        residual -= b[j] * x_lag_ptr[5 / 2 - j];
      }
      LTP_res_ptr[i] = residual * inv_gain;
      x_lag_ptr++;
    }
    LTP_res_ptr += subfr_length + pre_length;
    x_ptr += subfr_length;
  }
}

void silk_LTP_scale_ctrl_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, int condCoding) {
  constexpr auto code_independently = 0;
  constexpr auto plc_assumed_packet_loss_percent = 3;
  if (condCoding == code_independently && psEnc->sCmn.nChannelsInternal == 2 &&
      psEnc->sCmn.TargetRate_bps >= hybrid_silk_lowrate_boost_min_bps && psEnc->sCmn.TargetRate_bps <= hybrid_silk_lowrate_boost_max_bps) {
    const auto round_loss = plc_assumed_packet_loss_percent * psEnc->sCmn.nFramesPerPacket;
    const auto scaled_gain = static_cast<opus_int32>(psEncCtrl->LTPredCodGain) * round_loss;
    psEnc->sCmn.indices.LTP_scaleIndex = scaled_gain > silk_log2lin(2900 - psEnc->sCmn.SNR_dB_Q7);
    psEnc->sCmn.indices.LTP_scaleIndex += scaled_gain > silk_log2lin(3900 - psEnc->sCmn.SNR_dB_Q7);
  } else {
    psEnc->sCmn.indices.LTP_scaleIndex = 0;
  }
}

[[nodiscard]] static inline auto warped_gain(std::span<const float> coefs, float lambda) noexcept -> float {
  lambda = -lambda;
  auto gain = coefs.back();
  for (auto index = static_cast<int>(coefs.size()) - 2; index >= 0; --index) {
    gain = lambda * gain + coefs[index];
  }
  return static_cast<float>(1.0f / (1.0f - lambda * gain));
}

static inline auto warped_true2monic_coefs(std::span<float> coefs, float lambda, float limit) noexcept -> void {
  for (auto index = static_cast<int>(coefs.size()) - 1; index > 0; --index) {
    coefs[index - 1] -= lambda * coefs[index];
  }
  auto gain = (1.0f - lambda * lambda) / (1.0f + lambda * coefs.front());
  for (auto index = std::size_t{}; index < coefs.size(); ++index) {
    coefs[index] *= gain;
  }
  for (auto iter = 0; iter < 10; ++iter) {
    const auto [max_abs, max_index] = max_abs_index(coefs);
    if (max_abs <= limit) {
      return;
    }
    for (auto index = 1; index < static_cast<int>(coefs.size()); ++index) {
      coefs[index - 1] += lambda * coefs[index];
    }
    gain = 1.0f / gain;
    for (auto index = std::size_t{}; index < coefs.size(); ++index) {
      coefs[index] *= gain;
    }
    const auto chirp = 0.99f - (0.8f + 0.1f * iter) * (max_abs - limit) / (max_abs * (max_index + 1));
    silk_bwexpander_FLP(coefs, chirp);
    for (auto index = static_cast<int>(coefs.size()) - 1; index > 0; --index) {
      coefs[index - 1] -= lambda * coefs[index];
    }
    gain = (1.0f - lambda * lambda) / (1.0f + lambda * coefs.front());
    for (auto index = std::size_t{}; index < coefs.size(); ++index) {
      coefs[index] *= gain;
    }
  }
}

static inline auto limit_coefs(std::span<float> coefs, float limit) noexcept -> void {
  for (auto iter = 0; iter < 10; ++iter) {
    const auto [max_abs, max_index] = max_abs_index(coefs);
    if (max_abs <= limit) {
      return;
    }
    const auto chirp = 0.99f - (0.8f + 0.1f * iter) * (max_abs - limit) / (max_abs * (max_index + 1));
    silk_bwexpander_FLP(coefs, chirp);
  }
}

void silk_noise_shape_analysis_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, const float* pitch_res,
                                   const float* x) {
  silk_shape_state_FLP* psShapeSt = &psEnc->sShape;
  int k, nSamples, nSegs;
  float SNR_adj_dB, HarmShapeGain, Tilt;
  float nrg, log_energy, log_energy_prev, energy_variation;
  float BWExp, gain_mult, gain_add, strength, b, warping;
  float x_windowed[15 * 16], auto_corr[24 + 1], rc[24 + 1];
  const float *x_ptr, *pitch_res_ptr;
  x_ptr = x - psEnc->sCmn.la_shape;
  SNR_adj_dB = psEnc->sCmn.SNR_dB_Q7 * (1 / 128.0f);
  const bool voiced = psEnc->sCmn.indices.signalType == 2;
  psEncCtrl->input_quality = 0.5f * (psEnc->sCmn.input_quality_bands_Q15[0] + psEnc->sCmn.input_quality_bands_Q15[1]) * (1.0f / 32768.0f);
  psEncCtrl->coding_quality = silk_sigmoid(0.25f * (SNR_adj_dB - 20.0f));
  if (psEnc->sCmn.useCBR == 0) {
    b = 1.0f - psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f);
    SNR_adj_dB -= 2.0f * psEncCtrl->coding_quality * (0.5f + 0.5f * psEncCtrl->input_quality) * b * b;
  }
  if (voiced) {
    SNR_adj_dB += 2.0f * psEnc->LTPCorr;
  } else {
    SNR_adj_dB += (-0.4f * psEnc->sCmn.SNR_dB_Q7 * (1 / 128.0f) + 6.0f) * (1.0f - psEncCtrl->input_quality);
  }
  if (voiced) {
    psEnc->sCmn.indices.quantOffsetType = 0;
  } else {
    nSamples = 2 * psEnc->sCmn.fs_kHz;
    energy_variation = 0.0f;
    log_energy_prev = 0.0f;
    pitch_res_ptr = pitch_res;
    nSegs =
        (static_cast<opus_int32>(static_cast<opus_int16>(5)) * static_cast<opus_int32>(static_cast<opus_int16>(psEnc->sCmn.nb_subfr))) / 2;
    for (k = 0; k < nSegs; k++) {
      nrg = static_cast<float>(nSamples) + static_cast<float>(silk_energy_FLP(pitch_res_ptr, nSamples));
      log_energy = silk_log2(nrg);
      if (k > 0) {
        energy_variation += static_cast<float>(std::fabs(static_cast<double>(log_energy - log_energy_prev)));
      }
      log_energy_prev = log_energy;
      pitch_res_ptr += nSamples;
    }
    psEnc->sCmn.indices.quantOffsetType = energy_variation > 0.6f * (nSegs - 1) ? 0 : 1;
  }
  strength = 1e-3f * psEncCtrl->predGain;
  BWExp = 0.94f / (1.0f + strength * strength);
  warping = static_cast<float>(psEnc->sCmn.warping_Q16) / 65536.0f + 0.01f * psEncCtrl->coding_quality;
  gain_mult = silk_pow_reference(2.0f, -0.16f * SNR_adj_dB);
  gain_add = silk_pow_reference(2.0f, 0.16f * 2);
  const float lf_strength = 4.0f * (1.0f + 0.5f * (psEnc->sCmn.input_quality_bands_Q15[0] * (1.0f / 32768.0f) - 1.0f)) *
                            psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f);
  const float unvoiced_b = 1.3f / psEnc->sCmn.fs_kHz;
  Tilt = voiced ? -0.25f - (1 - 0.25f) * 0.35f * psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f) : -0.25f;
  HarmShapeGain = voiced ? 0.3f + 0.2f * (1.0f - (1.0f - psEncCtrl->coding_quality) * psEncCtrl->input_quality) : 0.0f;
  if (voiced) {
    HarmShapeGain *= silk_sqrt_reference(psEnc->LTPCorr);
  }
  for (k = 0; k < psEnc->sCmn.nb_subfr; k++) {
    int shift, slope_part, flat_part;
    flat_part = psEnc->sCmn.fs_kHz * 3;
    slope_part = (psEnc->sCmn.shapeWinLength - flat_part) / 2;
    silk_apply_sine_window_FLP(std::span<float>{x_windowed, static_cast<std::size_t>(slope_part)},
                               std::span<const float>{x_ptr, static_cast<std::size_t>(slope_part)}, 1);
    shift = slope_part;
    copy_n_bytes(x_ptr + shift, static_cast<std::size_t>(flat_part * sizeof(float)), x_windowed + shift);
    shift += flat_part;
    silk_apply_sine_window_FLP(std::span<float>{x_windowed + shift, static_cast<std::size_t>(slope_part)},
                               std::span<const float>{x_ptr + shift, static_cast<std::size_t>(slope_part)}, 2);
    x_ptr += psEnc->sCmn.subfr_length;
    if (psEnc->sCmn.warping_Q16 > 0) {
      silk_warped_autocorrelation_FLP(auto_corr, x_windowed, warping, psEnc->sCmn.shapeWinLength, psEnc->sCmn.shapingLPCOrder);
    } else {
      silk_autocorrelation_FLP(auto_corr, x_windowed, psEnc->sCmn.shapeWinLength, psEnc->sCmn.shapingLPCOrder + 1);
    }
    auto_corr[0] += auto_corr[0] * 3e-5f + 1.0f;
    nrg = silk_schur_FLP(rc, auto_corr, psEnc->sCmn.shapingLPCOrder);
    silk_k2a_FLP(&psEncCtrl->AR[k * 24], rc, psEnc->sCmn.shapingLPCOrder);
    psEncCtrl->Gains[k] = silk_sqrt_reference(nrg);
    if (psEnc->sCmn.warping_Q16 > 0) {
      psEncCtrl->Gains[k] *=
          warped_gain(std::span<const float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, warping);
    }
    silk_bwexpander_FLP(std::span<float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, BWExp);
    if (psEnc->sCmn.warping_Q16 > 0) {
      warped_true2monic_coefs(std::span<float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, warping,
                              3.999f);
    } else {
      limit_coefs(std::span<float>{&psEncCtrl->AR[k * 24], static_cast<std::size_t>(psEnc->sCmn.shapingLPCOrder)}, 3.999f);
    }
    psEncCtrl->Gains[k] = psEncCtrl->Gains[k] * gain_mult + gain_add;
    if (voiced) {
      b = 0.2f / psEnc->sCmn.fs_kHz + 3.0f / psEncCtrl->pitchL[k];
      psEncCtrl->LF_MA_shp[k] = -1.0f + b;
      psEncCtrl->LF_AR_shp[k] = 1.0f - b - b * lf_strength;
    } else {
      psEncCtrl->LF_MA_shp[k] = -1.0f + unvoiced_b;
      psEncCtrl->LF_AR_shp[k] = 1.0f - unvoiced_b - unvoiced_b * lf_strength * 0.6f;
    }
    psShapeSt->HarmShapeGain_smth += 0.4f * (HarmShapeGain - psShapeSt->HarmShapeGain_smth);
    psEncCtrl->HarmShapeGain[k] = psShapeSt->HarmShapeGain_smth;
    psShapeSt->Tilt_smth += 0.4f * (Tilt - psShapeSt->Tilt_smth);
    psEncCtrl->Tilt[k] = psShapeSt->Tilt_smth;
  }
}

void silk_process_gains_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, int condCoding) {
  silk_shape_state_FLP* psShapeSt = &psEnc->sShape;
  int k;
  opus_int32 pGains_Q16[4]{};
  float s, InvMaxSqrVal, gain, quant_offset;
  if (psEnc->sCmn.indices.signalType == 2) {
    s = 1.0f - 0.5f * silk_sigmoid(0.25f * (psEncCtrl->LTPredCodGain - 12.0f));
    for (int i = 0; i < psEnc->sCmn.nb_subfr; ++i) {
      psEncCtrl->Gains[i] *= s;
    }
  }
  InvMaxSqrVal = (std::pow(2.0f, 0.33f * (21.0f - psEnc->sCmn.SNR_dB_Q7 * (1 / 128.0f))) / psEnc->sCmn.subfr_length);
  for (k = 0; k < psEnc->sCmn.nb_subfr; k++) {
    gain = std::sqrt(psEncCtrl->Gains[k] * psEncCtrl->Gains[k] + psEncCtrl->ResNrg[k] * InvMaxSqrVal);
    psEncCtrl->Gains[k] = std::min(gain, 32767.0f);
  }
  for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) {
    pGains_Q16[index] = static_cast<opus_int32>(psEncCtrl->Gains[index] * 65536.0f);
  }
  copy_n_bytes(pGains_Q16, static_cast<std::size_t>(psEnc->sCmn.nb_subfr * sizeof(opus_int32)), psEncCtrl->GainsUnq_Q16);
  psEncCtrl->lastGainIndexPrev = psShapeSt->LastGainIndex;
  silk_gains_quant(psEnc->sCmn.indices.GainsIndices, pGains_Q16, &psShapeSt->LastGainIndex, condCoding == 2, psEnc->sCmn.nb_subfr);
  for (int index = 0; index < psEnc->sCmn.nb_subfr; ++index) {
    psEncCtrl->Gains[index] = pGains_Q16[index] / 65536.0f;
  }
  if (psEnc->sCmn.indices.signalType == 2) {
    psEnc->sCmn.indices.quantOffsetType = psEncCtrl->LTPredCodGain + psEnc->sCmn.input_tilt_Q15 * (1.0f / 32768.0f) > 1.0f ? 0 : 1;
  }
  quant_offset = silk_Quantization_Offsets_Q10[psEnc->sCmn.indices.signalType >> 1][psEnc->sCmn.indices.quantOffsetType] / 1024.0f;
  psEncCtrl->Lambda = 1.2f + -0.05f * psEnc->sCmn.nStatesDelayedDecision + -0.2f * psEnc->sCmn.speech_activity_Q8 * (1.0f / 256.0f) +
                      -0.1f * psEncCtrl->input_quality + -0.2f * psEncCtrl->coding_quality + 0.8f * quant_offset;
}

void silk_residual_energy_FLP(float nrgs[4], const float x[], float a[2][16], const float gains[], const int subfr_length,
                              const int nb_subfr, const int LPC_order) {
  float LPC_res[(((5 * 4) * 16) + 4 * 16) / 2]{};
  auto* LPC_res_ptr = LPC_res + LPC_order;
  const int shift = LPC_order + subfr_length;
  silk_LPC_analysis_filter_FLP(LPC_res, a[0], x + 0 * shift, 2 * shift, LPC_order);
  nrgs[0] = static_cast<float>(gains[0] * gains[0] * silk_energy_FLP(LPC_res_ptr + 0 * shift, subfr_length));
  nrgs[1] = static_cast<float>(gains[1] * gains[1] * silk_energy_FLP(LPC_res_ptr + 1 * shift, subfr_length));
  if (nb_subfr == 4) {
    silk_LPC_analysis_filter_FLP(LPC_res, a[1], x + 2 * shift, 2 * shift, LPC_order);
    nrgs[2] = static_cast<float>(gains[2] * gains[2] * silk_energy_FLP(LPC_res_ptr + 0 * shift, subfr_length));
    nrgs[3] = static_cast<float>(gains[3] * gains[3] * silk_energy_FLP(LPC_res_ptr + 1 * shift, subfr_length));
  }
}

void silk_warped_autocorrelation_FLP(float* corr, const float* input, const float warping, const int length, const int order) {
  std::array<double, 24 + 1> state{};
  std::array<double, 24 + 1> C{};
  for (int n = 0; n < length; n++) {
    double tmp1 = input[n];
    for (int i = 0; i < order; i += 2) {
      double tmp2 = state[i] + warping * state[i + 1] - warping * tmp1;
      state[i] = tmp1;
      C[i] += state[0] * tmp1;
      tmp1 = state[i + 1] + warping * state[i + 2] - warping * tmp2;
      state[i + 1] = tmp2;
      C[i + 1] += state[0] * tmp2;
    }
    state[order] = tmp1;
    C[order] += state[0] * tmp1;
  }
  for (int index = 0; index <= order; ++index) {
    corr[index] = static_cast<float>(C[index]);
  }
}

void silk_A2NLSF_FLP(opus_int16* NLSF_Q15, const float* pAR, const int LPC_order) {
  std::array<opus_int32, 16> a_fix_Q16{};
  for (int index = 0; index < LPC_order; ++index) {
    a_fix_Q16[index] = float2int(pAR[index] * 65536.0f);
  }
  silk_A2NLSF(NLSF_Q15, a_fix_Q16.data(), LPC_order);
}

void silk_NLSF2A_FLP(float* pAR, const opus_int16* NLSF_Q15, const int LPC_order) {
  std::array<opus_int16, 16> a_fix_Q12{};
  silk_NLSF2A(a_fix_Q12.data(), NLSF_Q15, LPC_order);
  for (int index = 0; index < LPC_order; ++index) {
    pAR[index] = static_cast<float>(a_fix_Q12[index]) * (1.0f / 4096.0f);
  }
}

void silk_process_NLSFs_FLP(silk_encoder_state* psEncC, float PredCoef[2][16], opus_int16 NLSF_Q15[16],
                            const opus_int16 prev_NLSF_Q15[16]) {
  opus_int16 PredCoef_Q12[2][16];
  silk_process_NLSFs(psEncC, PredCoef_Q12, NLSF_Q15, prev_NLSF_Q15);
  for (int j = 0; j < 2; j++) {
    for (int index = 0; index < psEncC->predictLPCOrder; ++index) {
      PredCoef[j][index] = static_cast<float>(PredCoef_Q12[j][index]) * (1.0f / 4096.0f);
    }
  }
}

void silk_NSQ_wrapper_FLP(silk_encoder_state_FLP* psEnc, silk_encoder_control_FLP* psEncCtrl, SideInfoIndices* psIndices,
                          silk_nsq_state* psNSQ, opus_int8 pulses[], const opus_int16 samples[]) {
  std::array<opus_int32, 4> gains{};
  opus_int16 prediction[2][16]{};
  opus_int16 ltp[5 * 4]{};
  opus_int16 shaping[4 * 24]{};
  opus_int32 low_frequency[4]{};
  int tilt[4]{}, harmonic[4]{};
  for (int subframe = 0; subframe < psEnc->sCmn.nb_subfr; ++subframe) {
    for (int index = 0; index < psEnc->sCmn.shapingLPCOrder; ++index) {
      shaping[static_cast<std::size_t>(subframe * 24 + index)] =
          static_cast<opus_int16>(float2int(psEncCtrl->AR[subframe * 24 + index] * 8192.0f));
    }
    low_frequency[subframe] = wrap_shift_left(float2int(psEncCtrl->LF_AR_shp[subframe] * 16384.0f), 16) |
                              static_cast<opus_uint16>(float2int(psEncCtrl->LF_MA_shp[subframe] * 16384.0f));
    tilt[subframe] = float2int(psEncCtrl->Tilt[subframe] * 16384.0f);
    harmonic[subframe] = float2int(psEncCtrl->HarmShapeGain[subframe] * 16384.0f);
    gains[subframe] = float2int(psEncCtrl->Gains[subframe] * 65536.0f);
  }
  if (psIndices->signalType == 2) {
    for (int index = 0; index < psEnc->sCmn.nb_subfr * 5; ++index) {
      ltp[index] = static_cast<opus_int16>(float2int(psEncCtrl->LTPCoef[index] * 16384.0f));
    }
  }
  const int first_prediction = psIndices->NLSFInterpCoef_Q2 == 4 ? 1 : 0;
  for (int row = first_prediction; row < 2; ++row) {
    for (int index = 0; index < psEnc->sCmn.predictLPCOrder; ++index) {
      prediction[row][index] = static_cast<opus_int16>(float2int(psEncCtrl->PredCoef[row][index] * 4096.0f));
    }
  }
  const int ltp_scale = psIndices->signalType == 2 ? silk_LTPScales_table_Q14[psIndices->LTP_scaleIndex] : 0;
  const auto nsq = psEnc->sCmn.nStatesDelayedDecision > 1 || psEnc->sCmn.warping_Q16 > 0 ? &silk_NSQ<true> : &silk_NSQ<false>;
  nsq(&psEnc->sCmn, psNSQ, psIndices, samples, pulses, prediction[0], ltp, shaping, harmonic, tilt, low_frequency, gains.data(),
      psEncCtrl->pitchL, float2int(psEncCtrl->Lambda * 1024.0f), ltp_scale);
}

void silk_quant_LTP_gains_FLP(float B[4 * 5], opus_uint8 cbk_index[4], opus_uint8* periodicity_index, opus_int32* sum_log_gain_Q7,
                              float* pred_gain_dB, const float XX[4 * 5 * 5], const float xX[4 * 5], const int subfr_len,
                              const int nb_subfr) {
  int pred_gain_dB_Q7;
  opus_int16 B_Q14[4 * 5]{};
  opus_int32 XX_Q17[4 * 5 * 5]{}, xX_Q17[4 * 5]{};
  for (int index = 0; index < nb_subfr * 5 * 5; ++index) {
    XX_Q17[index] = static_cast<opus_int32>(float2int(XX[index] * 131072.0f));
  }
  for (int index = 0; index < nb_subfr * 5; ++index) {
    xX_Q17[index] = static_cast<opus_int32>(float2int(xX[index] * 131072.0f));
  }
  silk_quant_LTP_gains(B_Q14, cbk_index, periodicity_index, sum_log_gain_Q7, &pred_gain_dB_Q7, XX_Q17, xX_Q17, subfr_len, nb_subfr);
  for (int index = 0; index < nb_subfr * 5; ++index) {
    B[index] = static_cast<float>(B_Q14[index]) * (1.0f / 16384.0f);
  }
  *pred_gain_dB = static_cast<float>(pred_gain_dB_Q7) * (1.0f / 128.0f);
}

void silk_autocorrelation_FLP(float* results, const float* inputData, int inputDataSize, int correlationCount) {
  if (correlationCount > inputDataSize) {
    correlationCount = inputDataSize;
  }
  for (int i = 0; i < correlationCount; i++) {
    results[i] = static_cast<float>(silk_inner_product_FLP_c(inputData, inputData + i, inputDataSize - i));
  }
}
float silk_burg_modified_FLP(float A[], const float x[], const float minInvGain, const int subfr_length, const int nb_subfr, const int D) {
  std::array<double, silk_nlsf_max_order> C_first_row;
  std::array<double, silk_nlsf_max_order> C_last_row;
  std::array<double, silk_nlsf_max_order + 1> CAf;
  std::array<double, silk_nlsf_max_order + 1> CAb;
  std::array<double, silk_nlsf_max_order> Af;
  double C0 = silk_energy_FLP(x, nb_subfr * subfr_length);
  zero_n_items(C_first_row.data(), static_cast<std::size_t>(D));
  for (int s = 0; s < nb_subfr; ++s) {
    const auto* x_ptr = x + s * subfr_length;
    for (int n = 1; n <= D; ++n) {
      C_first_row[n - 1] += silk_inner_product_FLP_c(x_ptr, x_ptr + n, subfr_length - n);
    }
  }
  std::copy_n(C_first_row.begin(), D, C_last_row.begin());
  CAb[0] = CAf[0] = C0 + 1e-5f * C0 + 1e-9f;
  double invGain = 1.0f;
  double nrg_f = 0;
  bool reached_max_gain = false;
  for (int n = 0; n < D; ++n) {
    for (int s = 0; s < nb_subfr; ++s) {
      const auto* x_ptr = x + s * subfr_length;
      double tmp1 = x_ptr[n];
      double tmp2 = x_ptr[subfr_length - n - 1];
      for (int k = 0; k < n; ++k) {
        C_first_row[k] -= x_ptr[n] * x_ptr[n - k - 1];
        C_last_row[k] -= x_ptr[subfr_length - n - 1] * x_ptr[subfr_length - n + k];
        const double Atmp = Af[k];
        tmp1 += x_ptr[n - k - 1] * Atmp;
        tmp2 += x_ptr[subfr_length - n + k] * Atmp;
      }
      for (int k = 0; k <= n; ++k) {
        CAf[k] -= tmp1 * x_ptr[n - k];
        CAb[k] -= tmp2 * x_ptr[subfr_length - n + k - 1];
      }
    }
    double tmp1 = C_first_row[n];
    double tmp2 = C_last_row[n];
    for (int k = 0; k < n; ++k) {
      const double Atmp = Af[k];
      tmp1 += C_last_row[n - k - 1] * Atmp;
      tmp2 += C_first_row[n - k - 1] * Atmp;
    }
    CAf[n + 1] = tmp1;
    CAb[n + 1] = tmp2;
    double num = CAb[n + 1];
    double nrg_b = CAb[0];
    nrg_f = CAf[0];
    for (int k = 0; k < n; ++k) {
      const double Atmp = Af[k];
      num += CAb[n - k] * Atmp;
      nrg_b += CAb[k + 1] * Atmp;
      nrg_f += CAf[k + 1] * Atmp;
    }
    double rc = -2.0 * num / (nrg_f + nrg_b);
    tmp1 = invGain * (1.0 - rc * rc);
    if (tmp1 <= minInvGain) {
      rc = std::sqrt(1.0 - minInvGain / invGain);
      if (num > 0) {
        rc = -rc;
      }
      invGain = minInvGain;
      reached_max_gain = true;
    } else {
      invGain = tmp1;
    }
    for (int k = 0; k < (n + 1) >> 1; ++k) {
      tmp1 = Af[k];
      tmp2 = Af[n - k - 1];
      Af[k] = tmp1 + rc * tmp2;
      Af[n - k - 1] = tmp2 + rc * tmp1;
    }
    Af[n] = rc;
    if (reached_max_gain) {
      std::fill_n(Af.data() + n + 1, static_cast<std::size_t>(D - n - 1), 0.0);
      break;
    }
    for (int k = 0; k <= n + 1; ++k) {
      tmp1 = CAf[k];
      CAf[k] += rc * CAb[n - k + 1];
      CAb[n - k + 1] += rc * tmp1;
    }
  }
  if (reached_max_gain) {
    for (int index = 0; index < D; ++index) {
      A[index] = static_cast<float>(-Af[index]);
    }
    for (int s = 0; s < nb_subfr; ++s) {
      C0 -= silk_energy_FLP(x + s * subfr_length, D);
    }
    nrg_f = C0 * invGain;
  } else {
    nrg_f = CAf[0];
    double tmp1 = 1.0;
    for (int k = 0; k < D; ++k) {
      const double Atmp = Af[k];
      nrg_f += CAf[k + 1] * Atmp;
      tmp1 += Atmp * Atmp;
      A[k] = static_cast<float>(-Atmp);
    }
    nrg_f -= 1e-5f * C0 * tmp1;
  }
  return static_cast<float>(nrg_f);
}

void silk_bwexpander_FLP(std::span<float> ar, const float chirp) {
  float cfac = chirp;
  for (auto index = std::size_t{}; index < ar.size(); ++index) {
    ar[index] *= cfac;
    cfac *= chirp;
  }
}

static double silk_energy_FLP(const float* data, int dataSize) {
  return silk_inner_product_FLP_c(data, data, dataSize);
}
double silk_inner_product_FLP_c(const float* data1, const float* data2, int dataSize) {
  int i;
  double result = 0.0;
  for (i = 0; i < dataSize - 3; i += 4) {
    result += data1[i] * static_cast<double>(data2[i]) + data1[i + 1] * static_cast<double>(data2[i + 1]) +
              data1[i + 2] * static_cast<double>(data2[i + 2]) + data1[i + 3] * static_cast<double>(data2[i + 3]);
  }
  for (; i < dataSize; i++) {
    result += data1[i] * static_cast<double>(data2[i]);
  }
  return result;
}

void silk_k2a_FLP(float* A, const float* rc, opus_int32 order) {
  for (int k = 0; k < order; k++) {
    const float rck = rc[k];
    for (int n = 0; n < (k + 1) >> 1; n++) {
      const float t1 = A[n], t2 = A[k - n - 1];
      A[n] = t1 + t2 * rck;
      A[k - n - 1] = t2 + t1 * rck;
    }
    A[k] = -rck;
  }
}

static auto silk_pitch_search_stage3(const float frame[], int lag, int min_lag, int max_lag, int sf_length, int nb_subfr, int complexity)
    -> std::array<int, 2>;

static void silk_prepare_pitch_frames(const float* frame, int frame_length, int Fs_kHz, std::span<float> frame_8kHz,
                                      std::span<float> frame_4kHz, std::span<opus_int16> resample_workspace) {
  std::array<opus_int32, 6> filter_state;
  silk_float2short_array(resample_workspace.data(), frame, frame_length);
  if (Fs_kHz != 8) {
    zero_n_items(filter_state.data(), Fs_kHz == 16 ? 2 : 6);
    if (Fs_kHz == 16) {
      silk_resampler_down2(filter_state.data(), resample_workspace.data(), resample_workspace.data(), frame_length);
    } else {
      silk_resampler_down2_3(filter_state.data(), resample_workspace.data(), resample_workspace.data(), frame_length);
    }
    silk_short2float_array(frame_8kHz.data(), resample_workspace.data(), static_cast<int>(frame_8kHz.size()));
  }
  zero_n_items(filter_state.data(), 2);
  silk_resampler_down2(filter_state.data(), resample_workspace.data(), resample_workspace.data(), static_cast<int>(frame_8kHz.size()));
  silk_short2float_array(frame_4kHz.data(), resample_workspace.data(), static_cast<int>(frame_4kHz.size()));
}

auto silk_pitch_analysis_core_FLP(const float* frame, float previous_correlation, int prevLag, const float search_thres1,
                                  const float search_thres2, const int Fs_kHz, const int complexity, const int nb_subfr)
    -> silk_pitch_analysis_result {
  silk_pitch_analysis_result result;
  const int sf_length = 5 * Fs_kHz;
  const int min_lag = 2 * Fs_kHz;
  const int max_lag = 18 * Fs_kHz - 1;
  constexpr int min_lag_4kHz = 8, min_lag_8kHz = 16;
  constexpr int max_lag_4kHz = 72, max_lag_8kHz = 143;
  float best_correlation = 0.0f;
  int best_contour = 0;
  int lag = -1;
  const auto stage2_codebook = silk_stage2_pitch_codebook_view(Fs_kHz, nb_subfr, complexity);
  {
    constexpr int pitch_stage2_cols = ((18 * 16) >> 1) + 5;
    std::array<opus_val32, 18 * 4 - 2 * 4 + 1> coarse_cross_correlations;
    std::array<int, 24> lag_candidates;
    const int frame_length = (20 + 5 * nb_subfr) * Fs_kHz;
    const int frame_length_4kHz = (20 + 5 * nb_subfr) * 4;
    const int frame_length_8kHz = (20 + 5 * nb_subfr) * 8;
    constexpr int sf_length_4kHz = 20, sf_length_8kHz = 40;
    std::array<float, 40 * 8> frame_8kHz;
    std::array<opus_int16, 40 * silk_max_fs_kHz> resample_workspace;
    auto* candidate_map = resample_workspace.data();
    std::array<float, 4 * pitch_stage2_cols> lag_correlations;
    const auto frame_4kHz = std::span{lag_correlations}.subspan(pitch_stage2_cols, frame_length_4kHz);
    silk_prepare_pitch_frames(frame, frame_length, Fs_kHz, std::span{frame_8kHz}.first(frame_length_8kHz), frame_4kHz, resample_workspace);
    for (int i = frame_length_4kHz - 1; i > 0; --i) {
      frame_4kHz[i] = saturate_int16_from_int32(static_cast<opus_int32>(frame_4kHz[i]) + frame_4kHz[i - 1]);
    }
    zero_n_items(lag_correlations.data(), pitch_stage2_cols);
    auto* target_4k = &frame_4kHz[wrap_shift_left(sf_length_4kHz, 2)];
    for (int k = 0; k < nb_subfr >> 1; ++k) {
      auto* basis = target_4k - min_lag_4kHz;
      celt_pitch_xcorr_c(target_4k, target_4k - max_lag_4kHz, coarse_cross_correlations.data(), sf_length_8kHz,
                         max_lag_4kHz - min_lag_4kHz + 1);
      auto cross_corr = static_cast<double>(coarse_cross_correlations[max_lag_4kHz - min_lag_4kHz]);
      auto normalizer = silk_energy_FLP(target_4k, sf_length_8kHz) + silk_energy_FLP(basis, sf_length_8kHz) + sf_length_8kHz * 4000.0f;
      lag_correlations[min_lag_4kHz] += static_cast<float>(2 * cross_corr / normalizer);
      for (int d = min_lag_4kHz + 1; d <= max_lag_4kHz; ++d) {
        --basis;
        cross_corr = coarse_cross_correlations[max_lag_4kHz - d];
        normalizer += basis[0] * static_cast<double>(basis[0]) - basis[sf_length_8kHz] * static_cast<double>(basis[sf_length_8kHz]);
        lag_correlations[d] += static_cast<float>(2 * cross_corr / normalizer);
      }
      target_4k += sf_length_8kHz;
    }
    for (int i = max_lag_4kHz; i >= min_lag_4kHz; --i) {
      lag_correlations[i] -= lag_correlations[i] * i / 4096.0f;
    }
    int candidate_count = 4 + 2 * complexity;
    silk_insertion_sort_top_k<float, false>(lag_correlations.data() + min_lag_4kHz, lag_candidates.data(), max_lag_4kHz - min_lag_4kHz + 1,
                                            candidate_count);
    const float strongest_coarse_correlation = lag_correlations[min_lag_4kHz];
    if (strongest_coarse_correlation < 0.2f) {
      return result;
    }
    const float threshold = search_thres1 * strongest_coarse_correlation;
    for (int i = 0; i < candidate_count; ++i) {
      if (lag_correlations[min_lag_4kHz + i] > threshold) {
        lag_candidates[i] = wrap_shift_left(lag_candidates[i] + min_lag_4kHz, 1);
      } else {
        candidate_count = i;
        break;
      }
    }
    auto* const expanded_map = candidate_map + pitch_stage2_cols;
    zero_n_items(candidate_map, static_cast<std::size_t>(2 * pitch_stage2_cols));
    for (int i = 0; i < candidate_count; ++i) {
      const int center = lag_candidates[i];
      std::fill_n(candidate_map + center - 1, 3, static_cast<opus_int16>(1));
      std::fill_n(expanded_map + center - 2, 6, static_cast<opus_int16>(1));
    }
    candidate_count = 0;
    for (int i = min_lag_8kHz; i <= max_lag_8kHz; ++i) {
      if (candidate_map[i] != 0) {
        lag_candidates[candidate_count++] = i;
      }
    }
    int expanded_count = 0;
    for (int i = min_lag_8kHz - 2; i <= max_lag_8kHz + 1; ++i) {
      if (expanded_map[i] != 0) {
        candidate_map[expanded_count++] = static_cast<opus_int16>(i);
      }
    }
    zero_n_items(lag_correlations.data(), lag_correlations.size());
    auto* target_8k = Fs_kHz == 8 ? &frame[(4 * 5) * 8] : &frame_8kHz[(4 * 5) * 8];
    for (int k = 0; k < nb_subfr; ++k) {
      const double target_energy = silk_energy_FLP(target_8k, sf_length_8kHz) + 1.0;
      for (int j = 0; j < expanded_count; ++j) {
        const int d = candidate_map[j];
        const auto* basis = target_8k - d;
        const double cross_corr = silk_inner_product_FLP_c(basis, target_8k, sf_length_8kHz);
        if (cross_corr > 0.0f) {
          const double basis_energy = silk_energy_FLP(basis, sf_length_8kHz);
          lag_correlations[k * pitch_stage2_cols + d] = static_cast<float>(2 * cross_corr / (basis_energy + target_energy));
        } else {
          lag_correlations[k * pitch_stage2_cols + d] = 0.0f;
        }
      }
      target_8k += sf_length_8kHz;
    }
    float best_biased_correlation = -1000.0f;
    float previous_lag_log2 = 0.0f;
    if (prevLag > 0) {
      if (Fs_kHz == 12) {
        prevLag = wrap_shift_left(prevLag, 1) / 3;
      } else if (Fs_kHz == 16) {
        prevLag = ((prevLag) >> (1));
      }
      previous_lag_log2 = silk_log2(static_cast<float>(prevLag));
    }
    for (int k = 0; k < candidate_count; ++k) {
      const int d = lag_candidates[k];
      float candidate_correlation = -1000.0f;
      int candidate_contour = 0;
      for (int j = 0; j < stage2_codebook.nb_cbk_search; ++j) {
        float contour_score = 0.0f;
        for (int i = 0; i < nb_subfr; ++i) {
          contour_score += lag_correlations[i * pitch_stage2_cols + d + stage2_codebook.at(i, j)];
        }
        if (contour_score > candidate_correlation) {
          candidate_correlation = contour_score;
          candidate_contour = j;
        }
      }
      const float lag_log2 = silk_log2(static_cast<float>(d));
      float biased_correlation = candidate_correlation - 0.2f * nb_subfr * lag_log2;
      if (prevLag > 0) {
        float delta_lag_log2_sqr = lag_log2 - previous_lag_log2;
        delta_lag_log2_sqr *= delta_lag_log2_sqr;
        biased_correlation -= 0.2f * nb_subfr * previous_correlation * delta_lag_log2_sqr / (delta_lag_log2_sqr + 0.5f);
      }
      if (biased_correlation > best_biased_correlation && candidate_correlation > nb_subfr * search_thres2) {
        best_biased_correlation = biased_correlation;
        best_correlation = candidate_correlation;
        lag = d;
        best_contour = candidate_contour;
      }
    }
    if (lag == -1) {
      return result;
    }
  }
  result.correlation = static_cast<float>(best_correlation / nb_subfr);
  if (Fs_kHz > 8) {
    lag = Fs_kHz == 12 ? rounded_i16_product_shift<1>(lag, 3) : wrap_shift_left(lag, 1);
    lag = std::clamp(lag, min_lag, max_lag);
    const auto stage3 = silk_pitch_search_stage3(frame, lag, min_lag, max_lag, sf_length, nb_subfr, complexity);
    const int lag_new = stage3[0];
    best_contour = stage3[1];
    const auto stage3_codebook = silk_stage3_pitch_codebook_view(nb_subfr, complexity);
    for (int k = 0; k < nb_subfr; ++k) {
      result.lags[k] = std::clamp(lag_new + stage3_codebook.at(k, best_contour), min_lag, 18 * Fs_kHz);
    }
    result.lag_index = static_cast<opus_int16>(lag_new - min_lag);
    result.contour_index = static_cast<opus_uint8>(best_contour);
  } else {
    for (int k = 0; k < nb_subfr; ++k) {
      result.lags[k] = std::clamp(lag + stage2_codebook.at(k, best_contour), min_lag_8kHz, 18 * 8);
    }
    result.lag_index = static_cast<opus_int16>(lag - min_lag_8kHz);
    result.contour_index = static_cast<opus_uint8>(best_contour);
  }
  result.voiced = true;
  return result;
}

auto silk_pitch_search_stage3(const float frame[], const int lag, const int min_lag, const int max_lag, const int sf_length,
                              const int nb_subfr, const int complexity) -> std::array<int, 2> {
  constexpr int lag_span = 5;
  const int start_lag = std::max(lag - 2, min_lag);
  const int end_lag = std::min(lag + 2, max_lag);
  const auto lag_ranges = silk_stage3_lag_range_view(nb_subfr, complexity);
  const auto codebook = silk_stage3_pitch_codebook_view(nb_subfr, complexity);
  std::array<std::array<double, lag_span>, 34> correlation_sums{};
  std::array<std::array<double, lag_span>, 34> energy_sums;
  const double target_energy = silk_energy_FLP(frame + 4 * sf_length, nb_subfr * sf_length) + 1.0;
  for (auto& energies : energy_sums) {
    energies.fill(target_energy);
  }
  const float* target_ptr = &frame[wrap_shift_left(sf_length, 2)];
  for (int k = 0; k < nb_subfr; k++) {
    const int lag_low = lag_ranges.low(k);
    const int lag_high = lag_ranges.high(k);
    const int lag_count = lag_high - lag_low + 1;
    std::array<opus_val32, 22> xcorr;
    celt_pitch_xcorr_c(target_ptr, target_ptr - start_lag - lag_high, xcorr.data(), sf_length, lag_count);
    std::array<float, 22> energies;
    const float* basis_ptr = target_ptr - (start_lag + lag_low);
    double energy = silk_energy_FLP(basis_ptr, sf_length) + 1e-3;
    energies[0] = static_cast<float>(energy);
    for (int i = 1; i < lag_count; i++) {
      energy -= basis_ptr[sf_length - i] * static_cast<double>(basis_ptr[sf_length - i]);
      energy += basis_ptr[-i] * static_cast<double>(basis_ptr[-i]);
      energies[i] = static_cast<float>(energy);
    }
    for (int i = 0; i < codebook.nb_cbk_search; i++) {
      const int index = codebook.at(k, i) - lag_low;
      for (int offset = 0; offset < lag_span; ++offset) {
        correlation_sums[i][offset] += xcorr[lag_count - 1 - index - offset];
        energy_sums[i][offset] += energies[index + offset];
      }
    }
    target_ptr += sf_length;
  }
  int best_lag = lag;
  int best_contour = 0;
  float best_correlation = -1000.0f;
  const float contour_bias = 0.05f / lag;
  for (int candidate_lag = start_lag; candidate_lag <= end_lag; ++candidate_lag) {
    const int offset = candidate_lag - start_lag;
    for (int contour = 0; contour < codebook.nb_cbk_search; ++contour) {
      const double cross_correlation = correlation_sums[contour][offset];
      const float correlation = cross_correlation > 0.0 ? static_cast<float>(2 * cross_correlation / energy_sums[contour][offset]) *
                                                              (1.0f - contour_bias * contour)
                                                        : 0.0f;
      if (correlation > best_correlation && candidate_lag + static_cast<int>(codebook.at(0, contour)) <= max_lag) {
        best_correlation = correlation;
        best_lag = candidate_lag;
        best_contour = contour;
      }
    }
  }
  return {best_lag, best_contour};
}

void silk_scale_copy_vector_FLP(float* data_out, const float* data_in, float gain, int dataSize) {
  const auto count = static_cast<std::size_t>(dataSize > 0 ? dataSize : 0);
  for (auto index = std::size_t{}; index < count; ++index)
    data_out[index] = data_in[index] * gain;
}

float silk_schur_FLP(float refl_coef[], const float auto_corr[], int order) {
  std::array<std::array<double, 2>, 24 + 1> C;
  for (int i = 0; i <= order; ++i) {
    C[i][0] = C[i][1] = auto_corr[i];
  }
  for (int k = 0; k < order; k++) {
    const double rc_tmp = -C[k + 1][0] / std::max(C[0][1], 1e-9);
    refl_coef[k] = static_cast<float>(rc_tmp);
    for (int n = 0; n < order - k; n++) {
      const double c0 = C[n + k + 1][0], c1 = C[n][1];
      C[n + k + 1][0] = c0 + c1 * rc_tmp;
      C[n][1] = c1 + c0 * rc_tmp;
    }
  }
  return static_cast<float>(C[0][1]);
}

namespace {
void assign_error(int* error, int value) noexcept {
  if (error != nullptr) {
    *error = value;
  }
}

template <bool Encoder>
[[nodiscard]] auto create_codec_state(int Fs, int channels, int application, int* error) noexcept
    -> std::conditional_t<Encoder, OpusEncoder, OpusDecoder>* {
  const bool supported_application =
      application == OPUS_APPLICATION_VOIP || application == OPUS_APPLICATION_AUDIO || application == OPUS_APPLICATION_RESTRICTED_LOWDELAY;
  if (!is_supported_sample_rate(Fs) || (Encoder && !supported_application)) {
    assign_error(error, OPUS_BAD_ARG);
    return nullptr;
  }
  const int state_size = [&] {
    if constexpr (Encoder) {
      if (!is_supported_channel_count(channels)) {
        return 0;
      }
      const int silk_size = encoder_uses_silk(application) ? align(silk_encoder_get_size(channels)) : 0;
      const int base_size =
          align(sizeof(OpusEncoder)) + align(static_cast<int>(encoder_delay_buffer_count(channels, application) * sizeof(opus_res)));
      return base_size + silk_size +
             static_cast<int>(sizeof(CeltEncoderInternal) + celt_encoder_storage_count(channels) * sizeof(celt_sig));
    }
    return !is_supported_channel_count(channels)
               ? 0
               : align(sizeof(OpusDecoder)) + align(silk_decoder_get_size()) +
                     static_cast<int>(sizeof(CeltDecoderInternal) + celt_decoder_storage_count(channels) * sizeof(celt_sig));
  }();
  if (state_size <= 0) {
    assign_error(error, OPUS_BAD_ARG);
    return nullptr;
  }
  auto* allocation = std::malloc(static_cast<std::size_t>(state_size));
  if (allocation == nullptr) {
    assign_error(error, OPUS_ALLOC_FAIL);
    return nullptr;
  }
  using state_type = std::conditional_t<Encoder, OpusEncoder, OpusDecoder>;
  auto* state = std::construct_at(static_cast<state_type*>(allocation));
  if constexpr (Encoder) {
    ref_opus_encoder_init(state, Fs, channels, application);
  } else {
    ref_opus_decoder_init(state, Fs, channels);
  }
  assign_error(error, OPUS_OK);
  return state;
}

template <typename T> [[nodiscard]] static inline auto ctl_write_value(va_list& ap, T value) noexcept -> int {
  auto* out = va_arg(ap, T*);
  if (out == nullptr) {
    return OPUS_BAD_ARG;
  }
  *out = value;
  return OPUS_OK;
}

[[nodiscard]] static inline auto ctl_read_boolean(va_list& ap, int& destination) noexcept -> int {
  const auto value = va_arg(ap, opus_int32);
  if (value < 0 || value > 1) {
    return OPUS_BAD_ARG;
  }
  destination = value;
  return OPUS_OK;
}

[[nodiscard]] auto dispatch_encoder_control(OpusEncoder* st, int request, va_list& ap) noexcept -> int {
  auto* celt_enc = encoder_celt_state(st);
  switch (request) {
  case OPUS_SET_BITRATE_REQUEST: {
    auto value = va_arg(ap, opus_int32);
    if (value != -1000 && value != -1) {
      if (value <= 0) {
        return OPUS_BAD_ARG;
      }
      value = clamp_value(value, static_cast<opus_int32>(500), static_cast<opus_int32>(750000 * st->channels));
    }
    st->user_bitrate_bps = value;
    reset_vbr_budget(st);
    return OPUS_OK;
  }
  case OPUS_GET_BITRATE_REQUEST:
    return ctl_write_value(ap, user_bitrate_to_bitrate(st, st->prev_framesize ? st->Fs / st->prev_framesize : 0, 1276));
  case OPUS_SET_VBR_REQUEST: {
    if (const auto error = ctl_read_boolean(ap, st->use_vbr); error != OPUS_OK) {
      return error;
    }
    reset_vbr_budget(st);
    return OPUS_OK;
  }
  case OPUS_GET_VBR_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->use_vbr));
  case OPUS_SET_DTX_REQUEST: {
    if (const auto error = ctl_read_boolean(ap, st->use_dtx); error != OPUS_OK) {
      return error;
    }
    st->nb_no_activity_ms_Q1 = 0;
    st->dtx_smoothed_energy = 0;
    return OPUS_OK;
  }
  case OPUS_GET_DTX_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->use_dtx));
  case OPUS_SET_VBR_CONSTRAINT_REQUEST: {
    if (const auto error = ctl_read_boolean(ap, st->vbr_constraint); error != OPUS_OK) {
      return error;
    }
    reset_vbr_budget(st);
    return OPUS_OK;
  }
  case OPUS_GET_VBR_CONSTRAINT_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->vbr_constraint));
  case OPUS_SET_COMPLEXITY_REQUEST: {
    const auto value = va_arg(ap, opus_int32);
    if (value < 0 || value > 10) {
      return OPUS_BAD_ARG;
    }
    st->silk_mode.complexity = value;
    celt_enc->complexity = value;
    return OPUS_OK;
  }
  case OPUS_GET_COMPLEXITY_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->silk_mode.complexity));
  case OPUS_SET_INBAND_FEC_REQUEST: {
    int value = st->silk_mode.useInBandFEC;
    if (const auto error = ctl_read_boolean(ap, value); error != OPUS_OK) {
      return error;
    }
    if (value && !ensure_encoder_lbrr_state(st)) {
      return OPUS_ALLOC_FAIL;
    }
    st->silk_mode.useInBandFEC = value;
    if (!value) {
      st->silk_mode.LBRR_coded = 0;
    }
    return OPUS_OK;
  }
  case OPUS_GET_INBAND_FEC_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->silk_mode.useInBandFEC));
  case OPUS_SET_PACKET_LOSS_PERC_REQUEST: {
    const auto value = va_arg(ap, opus_int32);
    if (value < 0 || value > 100) {
      return OPUS_BAD_ARG;
    }
    st->silk_mode.packetLossPercentage = value;
    return OPUS_OK;
  }
  case OPUS_GET_PACKET_LOSS_PERC_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->silk_mode.packetLossPercentage));
  case OPUS_GET_LOOKAHEAD_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->Fs / 400 + encoder_delay_compensation(st)));
  case OPUS_GET_FINAL_RANGE_REQUEST:
    return ctl_write_value(ap, st->rangeFinal);
  case OPUS_GET_IN_DTX_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(encoder_is_in_dtx(st)));
  case OPUS_RESET_STATE:
    reset_ref_encoder_state(st, celt_enc);
    return OPUS_OK;
  default:
    return OPUS_UNIMPLEMENTED;
  }
}

[[nodiscard]] auto dispatch_decoder_control(OpusDecoder* st, int request, va_list& ap) noexcept -> int {
  switch (request) {
  case OPUS_GET_LAST_PACKET_DURATION_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(st->last_packet_duration));
  case OPUS_GET_FINAL_RANGE_REQUEST:
    return ctl_write_value(ap, st->rangeFinal);
  case OPUSCPP_SET_DECODE_POSTFILTER_REQUEST: {
    const auto value = va_arg(ap, opus_int32);
    if (value < 0 || value > 3) {
      return OPUS_BAD_ARG;
    }
    auto* celt_dec = decoder_celt_state(st);
    if (celt_dec->output_postfilter_level != value) {
      celt_dec->output_postfilter_auto_hold = 0;
      celt_dec->output_postfilter_average_bitrate = 0;
      celt_dec->output_postfilter_smoothed_gain = 0;
    }
    celt_dec->output_postfilter_level = value;
    return OPUS_OK;
  }
  case OPUSCPP_GET_DECODE_POSTFILTER_REQUEST:
    return ctl_write_value(ap, static_cast<opus_int32>(decoder_celt_state(st)->output_postfilter_level));
  case OPUS_RESET_STATE: {
    auto* silk_dec = decoder_silk_state(st);
    auto* celt_dec = decoder_celt_state(st);
    celt_decoder_reset_state(celt_dec);
    silk_ResetDecoder(silk_dec);
    zero_object_tail(*st, offsetof(OpusDecoder, stream_channels));
    st->stream_channels = st->channels;
    st->frame_size = st->Fs / 400;
    return OPUS_OK;
  }
  default:
    return OPUS_UNIMPLEMENTED;
  }
}
} // namespace

OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error) noexcept {
  return create_codec_state<true>(Fs, channels, application, error);
}

void opus_encoder_destroy(OpusEncoder* st) noexcept {
  release_encoder_silk_state(st);
  std::free(st);
}

int opus_encoder_ctl(OpusEncoder* st, int request, ...) noexcept {
  va_list ap;
  va_start(ap, request);
  const int ret = st == nullptr ? OPUS_BAD_ARG : dispatch_encoder_control(st, request, ap);
  va_end(ap);
  return ret;
}

OpusDecoder* opus_decoder_create(int Fs, int channels, int* error) noexcept {
  return create_codec_state<false>(Fs, channels, 0, error);
}

void opus_decoder_destroy(OpusDecoder* st) noexcept {
  if (st != nullptr) {
    silk_destroy_decoder(decoder_silk_state(st));
  }
  std::free(st);
}

int opus_decoder_ctl(OpusDecoder* st, int request, ...) noexcept {
  va_list ap;
  va_start(ap, request);
  const int ret = st == nullptr ? OPUS_BAD_ARG : dispatch_decoder_control(st, request, ap);
  va_end(ap);
  return ret;
}

[[nodiscard]] auto opus_strerror(int error) noexcept -> const char* {
  static constexpr char messages[] =
      "success\0invalid argument\0buffer too small\0internal error\0corrupted stream\0request not implemented\0invalid "
      "state\0memory allocation failed\0unknown error";
  static constexpr std::array<unsigned char, 9> offsets{0, 8, 25, 42, 57, 74, 98, 112, 137};
  const auto index = (error <= 0 && error >= -7) ? static_cast<unsigned>(-error) : 8U;
  return messages + offsets[index];
}
