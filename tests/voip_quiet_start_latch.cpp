#include "opus_codec.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(OPUSCPP_ENABLE_TEST_HOOKS)
#error "voip_quiet_start_latch requires -DOPUSCPP_ENABLE_TEST_HOOKS"
#endif

int opuscpp_test_preprocess_filter_state(const OpusEncoder* st) noexcept;

namespace {

constexpr int kQuietVoice = 2;
constexpr int kDefaultFilter = 1;
constexpr int kUndecided = 0;

[[nodiscard]] auto make_speech(int frames, int frame_size) -> std::vector<opus_int16> {
  auto pcm = std::vector<opus_int16>(static_cast<std::size_t>(frames * frame_size));
  for (int frame = 0; frame < frames; ++frame) {
    for (int i = 0; i < frame_size; ++i) {
      const double t = static_cast<double>(frame * frame_size + i) / 48000.0;
      const double envelope = 0.55 + 0.35 * std::sin(2.0 * 3.141592653589793 * 3.1 * t);
      const double value = envelope * (0.45 * std::sin(2.0 * 3.141592653589793 * 210.0 * t) +
                                       0.25 * std::sin(2.0 * 3.141592653589793 * 630.0 * t) +
                                       0.15 * std::sin(2.0 * 3.141592653589793 * 1480.0 * t));
      pcm[static_cast<std::size_t>(frame * frame_size + i)] = static_cast<opus_int16>(std::lrint(value * 32767.0 * 0.9));
    }
  }
  return pcm;
}

[[nodiscard]] auto make_level(double amplitude, int frames, int frame_size) -> std::vector<opus_int16> {
  auto pcm = std::vector<opus_int16>(static_cast<std::size_t>(frames * frame_size));
  for (int frame = 0; frame < frames; ++frame) {
    for (int i = 0; i < frame_size; ++i) {
      const double t = static_cast<double>(frame * frame_size + i) / 48000.0;
      const double value = amplitude * std::sin(2.0 * 3.141592653589793 * 190.0 * t);
      pcm[static_cast<std::size_t>(frame * frame_size + i)] = static_cast<opus_int16>(std::lrint(value * 32767.0));
    }
  }
  return pcm;
}

void encode_frames(OpusEncoder* enc, const std::vector<opus_int16>& pcm, int frames, int frame_size) {
  std::array<unsigned char, 1500> packet{};
  for (int frame = 0; frame < frames; ++frame) {
    const auto offset = static_cast<std::size_t>(frame * frame_size);
    const int size = opus_encode(enc, pcm.data() + offset, frame_size, packet.data(), static_cast<int>(packet.size()));
    if (size < 0) {
      throw std::runtime_error("encode failed");
    }
  }
}

[[nodiscard]] auto run_sequence(const std::vector<opus_int16>& opening, int opening_frames, const std::vector<opus_int16>& loud,
                                int loud_frames, int bitrate) -> int {
  constexpr int frame_size = 960;
  int error = OPUS_OK;
  auto enc = make_opus_encoder(48000, 1, OPUS_APPLICATION_VOIP, &error);
  if (!enc || error != OPUS_OK) {
    throw std::runtime_error("encoder create failed");
  }
  if (opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(bitrate)) != OPUS_OK) {
    throw std::runtime_error("set bitrate failed");
  }
  opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY(10));
  encode_frames(enc.get(), opening, opening_frames, frame_size);
  encode_frames(enc.get(), loud, loud_frames, frame_size);
  return opuscpp_test_preprocess_filter_state(enc.get());
}

void require_state(int got, int want, const std::string& what) {
  if (got != want) {
    throw std::runtime_error(what + ": got state " + std::to_string(got) + " want " + std::to_string(want));
  }
}

} // namespace

int main() {
  constexpr int frame_size = 960;
  constexpr int silence_frames = 15;
  constexpr int quiet_frames = 20;
  constexpr int loud_frames = 30;
  const auto silence = make_level(0.0, silence_frames, frame_size);
  const auto quiet = make_level(0.05, quiet_frames, frame_size);
  const auto speech = make_speech(loud_frames, frame_size);
  const auto steady_speech = make_speech(quiet_frames + loud_frames, frame_size);

  for (const int bitrate : std::array{16000, 64000}) {
    // Sanity: steady speech must classify as the default filter.
    require_state(run_sequence(steady_speech, 12, speech, loud_frames, bitrate), kDefaultFilter, "steady speech");
    // Sanity: sustained quiet audio must classify as quiet voice.
    require_state(run_sequence(quiet, quiet_frames, quiet, 1, bitrate), kQuietVoice, "sustained quiet");
    // Regression: leading silence must not latch the quiet path.
    if (run_sequence(silence, silence_frames, speech, loud_frames, bitrate) == kQuietVoice) {
      throw std::runtime_error("silent opening latched quiet voice at " + std::to_string(bitrate));
    }
    // Regression: leading quiet audio must be invalidated by later loud speech.
    if (run_sequence(quiet, quiet_frames, speech, loud_frames, bitrate) == kQuietVoice) {
      throw std::runtime_error("quiet opening was not invalidated by loud speech at " + std::to_string(bitrate));
    }
    // Reset must return the classifier to undecided.
    int error = OPUS_OK;
    auto enc = make_opus_encoder(48000, 1, OPUS_APPLICATION_VOIP, &error);
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(bitrate));
    encode_frames(enc.get(), quiet, quiet_frames, frame_size);
    require_state(opuscpp_test_preprocess_filter_state(enc.get()), kQuietVoice, "pre-reset quiet");
    opus_encoder_ctl(enc.get(), OPUS_RESET_STATE);
    require_state(opuscpp_test_preprocess_filter_state(enc.get()), kUndecided, "reset state");
  }

  // A bitrate change must not prevent the quiet decision from being invalidated.
  {
    int error = OPUS_OK;
    auto enc = make_opus_encoder(48000, 1, OPUS_APPLICATION_VOIP, &error);
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(16000));
    encode_frames(enc.get(), quiet, quiet_frames, frame_size);
    require_state(opuscpp_test_preprocess_filter_state(enc.get()), kQuietVoice, "quiet before bitrate change");
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(64000));
    encode_frames(enc.get(), speech, loud_frames, frame_size);
    if (opuscpp_test_preprocess_filter_state(enc.get()) == kQuietVoice) {
      throw std::runtime_error("bitrate change kept quiet latch");
    }
  }

  // The float input API shares the same classifier.
  {
    int error = OPUS_OK;
    auto enc = make_opus_encoder(48000, 1, OPUS_APPLICATION_VOIP, &error);
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(64000));
    std::vector<float> float_pcm(static_cast<std::size_t>(silence_frames * frame_size));
    for (std::size_t i = 0; i < float_pcm.size(); ++i) {
      float_pcm[i] = static_cast<float>(silence[i]) / 32768.0f;
    }
    std::array<unsigned char, 1500> packet{};
    for (int frame = 0; frame < silence_frames; ++frame) {
      if (opus_encode_float(enc.get(), float_pcm.data() + static_cast<std::size_t>(frame * frame_size), frame_size, packet.data(),
                            static_cast<int>(packet.size())) < 0) {
        throw std::runtime_error("float encode failed");
      }
    }
    std::vector<float> float_speech(static_cast<std::size_t>(loud_frames * frame_size));
    for (std::size_t i = 0; i < float_speech.size(); ++i) {
      float_speech[i] = static_cast<float>(speech[i]) / 32768.0f;
    }
    for (int frame = 0; frame < loud_frames; ++frame) {
      if (opus_encode_float(enc.get(), float_speech.data() + static_cast<std::size_t>(frame * frame_size), frame_size, packet.data(),
                            static_cast<int>(packet.size())) < 0) {
        throw std::runtime_error("float encode failed");
      }
    }
    if (opuscpp_test_preprocess_filter_state(enc.get()) == kQuietVoice) {
      throw std::runtime_error("float API latched quiet voice");
    }
  }

  // The VOIP quiet classifier must not run for AUDIO encoders.
  {
    int error = OPUS_OK;
    auto enc = make_opus_encoder(48000, 1, OPUS_APPLICATION_AUDIO, &error);
    opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(64000));
    encode_frames(enc.get(), quiet, quiet_frames, frame_size);
    if (opuscpp_test_preprocess_filter_state(enc.get()) == kQuietVoice) {
      throw std::runtime_error("AUDIO encoder entered the VOIP quiet state");
    }
  }

  std::cout << "voip_quiet_start_latch=PASS\n";
  return 0;
}
