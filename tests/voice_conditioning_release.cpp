











#ifndef OPUSCPP_VOICE_CONDITIONING_SRC
#define OPUSCPP_VOICE_CONDITIONING_SRC "../src/opus_codec.cpp"
#endif
#include OPUSCPP_VOICE_CONDITIONING_SRC

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond, msg)                                   \
  do {                                                     \
    ++g_checks;                                            \
    if (!(cond)) {                                         \
      ++g_failures;                                        \
      std::printf("FAIL %s (line %d)\n", (msg), __LINE__); \
    }                                                      \
  } while (0)

namespace {
constexpr int test_fs = 48000;
constexpr int test_frame = 960;
constexpr float test_pi = 3.14159265358979323846f;

void make_tone(std::vector<float>& pcm, float freq_hz, float amplitude, float& phase) {
  pcm.resize(static_cast<std::size_t>(test_frame));
  const float step = 2.f * test_pi * freq_hz / static_cast<float>(test_fs);
  for (int i = 0; i < test_frame; ++i) {
    pcm[static_cast<std::size_t>(i)] = amplitude * std::sin(phase);
    phase += step;
    if (phase > 2.f * test_pi)
      phase -= 2.f * test_pi;
  }
}

void make_dc_with_ac(std::vector<float>& pcm, float dc, float freq_hz, float amplitude, float& phase) {
  make_tone(pcm, freq_hz, amplitude, phase);
  for (auto& sample : pcm)
    sample += dc;
}




void start_state(voice_conditioning_channel& state, float score, int provisional) {
  state = voice_conditioning_channel{};
  std::vector<float> warmup;
  float phase = 0.f;
  make_tone(warmup, 1000.f, 0.2f, phase);
  update_voice_conditioning(state, warmup.data(), test_frame, 1, test_fs);
  state.cond_started = 1;
  state.cond_score = score;
  state.cond_mix = score;
  state.cue_dirty = 0;
  state.cue_released = 1;
  state.cue_provisional = provisional;
  state.cue_provisional_run = 0;
  state.cue_run = 0;
  state.cue_consec = 0;
  state.cue_rumble_consec = 0;
  state.cue_unknown_run = 0;
  state.cue_floor_hold = 0;
  state.cue_strict_run = 0;
  state.cue_adapt_run = 0;
  state.cue_adapt_count = 0;
  state.cue_adapt_sum = 0;
  state.cue_lf_ref = 0.f;
}

double run_clean_frames(voice_conditioning_channel& state, int frames, std::vector<double>* trace = nullptr) {
  std::vector<float> pcm;
  float phase = 0.f;
  for (int f = 0; f < frames; ++f) {
    make_tone(pcm, 1000.f, 0.2f, phase);
    update_voice_conditioning(state, pcm.data(), test_frame, 1, test_fs);
    if (trace)
      trace->push_back(static_cast<double>(state.cond_score));
  }
  return static_cast<double>(state.cond_score);
}
}

int main() {




  {
    voice_conditioning_channel normal, provisional;
    start_state(normal, 0.8f, 0);
    start_state(provisional, 0.8f, 1);
    std::vector<double> tn, tp;
    run_clean_frames(normal, 60, &tn);
    run_clean_frames(provisional, 60, &tp);

    CHECK(normal.cue_dirty == 0 && provisional.cue_dirty == 0, "clean input must not latch dirty");
    for (std::size_t i = 0; i < tn.size(); ++i) {
      CHECK(std::fabs(tn[i] - tp[i]) <= 0.02, "provisional and normal decline must match");
    }
    CHECK(tn[19] >= 0.30 && tn[19] <= 0.70, "normal release envelope after 20 frames");
    CHECK(tp[19] >= 0.30 && tp[19] <= 0.70, "provisional release envelope after 20 frames");
    CHECK(tn[59] < 0.25 && tp[59] < 0.25, "both states converge toward zero");
    for (std::size_t i = 1; i < tn.size(); ++i) {
      CHECK(tn[i] <= tn[i - 1] + 1e-6 && tp[i] <= tp[i - 1] + 1e-6, "clean decline is monotone");
    }
    std::printf("A: normal_f20=%.3f provisional_f20=%.3f\n", tn[19], tp[19]);
  }


  {
    voice_conditioning_channel state;
    start_state(state, 0.8f, 1);
    run_clean_frames(state, 30);
    CHECK(state.cue_provisional == 0, "provisional expires under clean evidence");
    CHECK(state.cond_score < 0.6, "provisional expiry does not hold the score");
  }


  {
    voice_conditioning_channel state;
    start_state(state, 0.2f, 0);
    std::vector<float> rumble;
    float phase = 0.f;
    const int attack_frames = 20;
    double rumble6 = 0.0;
    for (int f = 0; f < attack_frames; ++f) {
      make_tone(rumble, 50.f, 0.3f, phase);
      update_voice_conditioning(state, rumble.data(), test_frame, 1, test_fs);
      if (f == 5) {
        rumble6 = state.cond_score;


        CHECK(state.cond_score > 0.5, "rumble attacks within six frames");
      }
    }
    CHECK(state.cond_score > 0.85, "rumble reaches strong protection");
    CHECK(state.cue_dirty == 1, "rumble latches the dirty state");
    std::printf("rumble f6=%.3f f20=%.3f\n", rumble6, static_cast<double>(state.cond_score));
  }


  {
    voice_conditioning_channel state;
    start_state(state, 0.2f, 0);
    std::vector<float> mixed;
    float phase = 0.f;
    make_dc_with_ac(mixed, 0.15f, 40.f, 0.1f, phase);
    update_voice_conditioning(state, mixed.data(), test_frame, 1, test_fs);
    CHECK(state.cond_score > 0.6, "DC evidence attacks within one frame");
    std::printf("C2: dc_f1=%.3f\n", static_cast<double>(state.cond_score));
  }


  {
    voice_conditioning_channel state;
    start_state(state, 0.2f, 0);
    std::vector<float> rumble;
    float phase = 0.f;
    for (int f = 0; f < 20; ++f) {
      make_tone(rumble, 50.f, 0.3f, phase);
      update_voice_conditioning(state, rumble.data(), test_frame, 1, test_fs);
    }
    CHECK(state.cue_dirty == 1, "recovery scenario requires a latched dirty state");
    run_clean_frames(state, 240);
    CHECK(state.cue_dirty == 0 && state.cue_released == 1, "clean input releases the dirty state");
    CHECK(state.cond_score < 0.2, "score recovers toward the raw endpoint");
    CHECK(state.cue_provisional == 0, "no provisional residue after recovery");
    std::printf("D: score_f240=%.3f\n", static_cast<double>(state.cond_score));
  }

  std::printf("voice_conditioning_release checks=%d failures=%d\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
