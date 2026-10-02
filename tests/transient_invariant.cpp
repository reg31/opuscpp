#include "../src/opus_codec.cpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFrame = 960;
constexpr int kLen = kFrame + celt_default_overlap;
constexpr int kCapacity = 80;
constexpr int kBands = celt_default_nb_ebands;
constexpr float kPreLog = -10.0f;
constexpr float kPreLog2 = -20.0f;

std::vector<opus_val32> make_noise(unsigned seed, float quiet_level, float loud_level) {
  std::vector<opus_val32> pcm(static_cast<std::size_t>(kLen));
  unsigned r = seed;
  for (int i = 0; i < kLen; ++i) {
    r = r * 1664525u + 1013904223u;
    const auto noise = static_cast<float>(static_cast<int>(r >> 16) - 32768) / 32768.0f;
    pcm[static_cast<std::size_t>(i)] = (i < kLen / 2 ? quiet_level : loud_level) * noise;
  }
  return pcm;
}

std::vector<opus_int16> to_i16(const std::vector<opus_val32>& src) {
  std::vector<opus_int16> out(static_cast<std::size_t>(kFrame));
  for (int i = 0; i < kFrame; ++i)
    out[static_cast<std::size_t>(i)] =
        static_cast<opus_int16>(std::lrint(std::clamp(src[static_cast<std::size_t>(i)], -0.95f, 0.95f) * 32767.0f));
  return out;
}

bool all_equal(const celt_glog* a, float value, int start, int end) {
  for (int i = start; i < end; ++i)
    if (a[i] != value)
      return false;
  return true;
}

void run_case(const char* name, bool policy, bool saturated, float prime_level, float quiet_level, float loud_level,
              int* failures) {
  int error = OPUS_OK;
  std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> enc{
      opus_encoder_create(kRate, 1, OPUS_APPLICATION_AUDIO, &error), opus_encoder_destroy};
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> dec{
      opus_decoder_create(kRate, 1, &error), opus_decoder_destroy};
  if (!enc || !dec || error != OPUS_OK) {
    std::printf("%s ERROR create\n", name);
    ++*failures;
    return;
  }
  opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(64000));
  opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY(10));
  opus_encoder_ctl(enc.get(), OPUS_SET_VBR(1));
  opus_encoder_ctl(enc.get(), OPUS_SET_VBR_CONSTRAINT(1));
  auto* celt = encoder_celt_state(enc.get());

  std::array<unsigned char, 4000> scratch{};
  std::array<opus_int16, kFrame> prime_out{};
  for (int p = 0; p < 2; ++p) {
    const auto prime = to_i16(make_noise(11u + static_cast<unsigned>(p) * 977u, prime_level, prime_level));
    const int bytes = opus_encode(enc.get(), prime.data(), kFrame, scratch.data(), static_cast<opus_int32>(scratch.size()));
    if (bytes <= 0 || opus_decode(dec.get(), scratch.data(), bytes, prime_out.data(), kFrame, 0) != kFrame) {
      std::printf("%s ERROR prime\n", name);
      ++*failures;
      return;
    }
  }

  celt->stereo_policy_celt = policy ? 1 : 0;
  const auto pcm = make_noise(97u, quiet_level, loud_level);
  float tf_estimate = 0;
  int tf_chan = 0;
  bool weak = false;
  const int detected = celt_transient_analysis(pcm.data(), kLen, 1, &tf_estimate, &tf_chan, false, &weak, -1.0f, 0.0f);

  auto* celt_dec = decoder_celt_state(dec.get());
  const auto views = make_celt_decoder_views(celt_dec, kFrame);
  const int start = celt->start, end = celt->end;
  if (start != celt_dec->start || end != celt_dec->end || start >= end || end > kBands) {
    std::printf("%s ERROR decoder context\n", name);
    ++*failures;
    return;
  }
  for (int i = start; i < end; ++i) {
    views.oldLogE[i] = kPreLog;
    views.oldLogE2[i] = kPreLog2;
  }

  std::array<unsigned char, 4096> buf{};
  ec_enc coder;
  ec_enc_init(&coder, buf.data(), static_cast<int>(buf.size()));
  if (saturated)
    while (ec_tell(&coder) + 3 <= kCapacity * 8)
      ec_enc_bits(&coder, 0, 32);
  const bool flag_space = ec_tell(&coder) + 3 <= kCapacity * 8;
  const int ret = celt_encode_candidate(celt, pcm.data(), kFrame, buf.data(), kCapacity, &coder, false);

  const bool overflow = ec_tell(&coder) > static_cast<int>(coder.storage) * 8;
  const bool rejected = ret == OPUS_INTERNAL_ERROR && coder.error != 0 && overflow;
  bool wire_transient = false, range_match = false;
  int decoded = OPUS_INVALID_PACKET;
  if (!saturated && ret > 0 && coder.error == 0) {
    ec_dec reader;
    ec_dec_init(&reader, buf.data(), ret);
    std::array<opus_res, kFrame> output{};
    decoded = celt_decode_with_ec(celt_dec, buf.data(), ret, output.data(), kFrame, &reader);
    range_match = reader.error == 0 && reader.rng == coder.rng;
    wire_transient = all_equal(views.oldLogE2, kPreLog2, start, end) &&
                     !all_equal(views.oldLogE2, kPreLog, start, end) &&
                     std::all_of(output.begin(), output.end(), [](float value) {
                       return std::isfinite(value);
                     });
  }
  const bool ok = (detected != 0) == (loud_level != quiet_level) && flag_space != saturated &&
                  (saturated ? rejected : ret > 0 && decoded == kFrame && range_match && wire_transient);
  if (!ok)
    ++*failures;
  std::printf("%s policy=%d saturated=%d detected=%d flag_space=%d overflow=%d rejected=%d ret=%d",
              name, policy ? 1 : 0, saturated ? 1 : 0, detected, flag_space ? 1 : 0, overflow ? 1 : 0,
              rejected ? 1 : 0, ret);
  if (!saturated)
    std::printf(" range_match=%d wire_transient=%d decoded=%d", range_match ? 1 : 0, wire_transient ? 1 : 0, decoded);
  std::printf(" %s\n", ok ? "PASS" : "FAIL");
  std::fflush(stdout);
}

}

int main() {
  int failures = 0;
  run_case("focused", false, true, 0.0001f, 0.0001f, 1.0f, &failures);
  run_case("normal_attack", false, false, 0.0001f, 0.0001f, 1.0f, &failures);
  run_case("saturated_steady", false, true, 0.5f, 0.5f, 0.5f, &failures);
  run_case("policy_true", true, true, 0.0001f, 0.0001f, 1.0f, &failures);
  std::printf("celt_unsignaled_transient_invariant checks=4 failures=%d\n", failures);
  return failures != 0;
}
