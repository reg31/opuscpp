#include "../src/opus_codec.cpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace {
constexpr std::int32_t W = SignalBwTemporal::window_samples;
constexpr std::int32_t B = SignalBwTemporal::bootstrap_samples;

struct Model {
  long long t = 0;
  std::vector<std::pair<long long, int>> ev;
  void reset() {
    t = 0;
    ev.clear();
  }
  int step(bool active, int source, std::int32_t n, int endm1) {
    if (!active)
      return source;
    const bool boot = t < B;
    int expanded = source;
    if (boot) {
      ev.push_back({t, endm1});
      expanded = endm1;
    } else {
      for (auto& [ts, b] : ev)
        if (t - ts <= W && b > expanded)
          expanded = b;
      if (source >= 0)
        ev.push_back({t, source});
    }
    t += n;
    std::vector<std::pair<long long, int>> keep;
    for (auto& e : ev)
      if (t - e.first <= W + 1)
        keep.push_back(e);
    ev.swap(keep);
    return expanded;
  }
};

int step(SignalBwTemporal& st, Model& m, bool active, int source, std::int32_t n, int endm1) {
  const int got = st.advance(active, source, n, endm1);
  const int want = m.step(active, source, n, endm1);
  assert(got == want);
  return got;
}

void constant_20ms_boundary() {
  SignalBwTemporal st;
  Model m;
  const int endm1 = 20;
  for (int f = 0; f < 3; ++f)
    assert(step(st, m, true, -1, 960, endm1) == 20);
  assert(st.active_samples == B);
  const int seq[8] = {20, 20, 20, 20, 20, 20, 14, 14};
  for (int f = 0; f < 8; ++f)
    assert(step(st, m, true, 14, 960, endm1) == seq[f]);
  std::puts("constant_20ms_boundary PASS");
}

void variable_spans() {
  SignalBwTemporal st;
  Model m;
  const std::int32_t spans[4] = {120, 240, 480, 960};
  const int srcs[9] = {-1, 14, 16, 20, 18, 17, 15, 20, 14};
  const int endm1 = 20;
  for (int f = 0; f < 400; ++f) {
    const auto n = spans[f % 4];
    const int s = srcs[(f * 7 + 3) % 9];
    step(st, m, true, s, n, endm1);
  }
  std::puts("variable_spans PASS");
}

void silence_and_aux_exclusion() {
  SignalBwTemporal st;
  Model m;
  const int endm1 = 20;
  for (int f = 0; f < 5; ++f)
    step(st, m, true, -1, 960, endm1);
  for (int gap = 0; gap < 5; ++gap) {
    const auto a0 = st.active_samples, l0 = st.last_frame_samples;
    int r0[7];
    for (int i = 0; i < 7; ++i)
      r0[i] = st.remaining[i];
    for (int k = 0; k < 50; ++k) {
      assert(st.advance(false, 18, 960, endm1) == 18);
      assert(m.step(false, 18, 960, endm1) == 18);
    }
    assert(st.active_samples == a0 && st.last_frame_samples == l0);
    for (int i = 0; i < 7; ++i)
      assert(st.remaining[i] == r0[i]);
    step(st, m, true, 18, 960, endm1);
  }
  std::puts("silence_and_aux_exclusion PASS");
}

void warmup_boundaries() {
  const std::int32_t spans[4] = {120, 240, 480, 960};
  for (auto n : spans) {
    SignalBwTemporal st;
    Model m;
    int bootstrap_frames = 0;
    while (st.active_samples < B) {
      step(st, m, true, -1, n, 20);
      ++bootstrap_frames;
      assert(bootstrap_frames <= (B + n - 1) / n + 1);
    }
    assert(bootstrap_frames == (B + n - 1) / n);
    assert(st.active_samples >= B);
    step(st, m, true, -1, n, 20);
  }
  std::puts("warmup_boundaries PASS");
}

void downward_expiry_and_widening() {
  SignalBwTemporal st;
  Model m;
  const int endm1 = 20;
  for (int f = 0; f < 3; ++f)
    step(st, m, true, -1, 960, endm1);
  assert(step(st, m, true, 20, 960, endm1) == 20);
  assert(step(st, m, true, 14, 960, endm1) == 20);
  for (int f = 0; f < 8; ++f)
    step(st, m, true, 14, 960, endm1);
  assert(step(st, m, true, 16, 960, endm1) == 16);
  assert(step(st, m, true, 14, 960, endm1) == 16);
  std::puts("downward_expiry_and_widening PASS");
}

void saturation() {
  SignalBwTemporal st;
  st.active_samples = std::numeric_limits<std::int32_t>::max() - 5;
  st.advance(true, -1, 960, 20);
  assert(st.active_samples == B);
  for (int i = 0; i < 10; ++i) {
    st.advance(true, -1, 960, 20);
    assert(st.active_samples == B);
  }
  SignalBwTemporal near;
  near.active_samples = B - 1;
  assert(near.advance(true, -1, 960, 20) == 20);
  assert(near.active_samples == B);
  std::puts("saturation PASS");
}

void rate_change_retention() {
  SignalBwTemporal st;
  for (int f = 0; f < 3; ++f)
    st.advance(true, -1, 960, 20);
  for (int f = 0; f < 7; ++f)
    st.advance(true, -1, 960, 20);
  int floor = 18;
  int expanded = st.advance(true, 15, 960, 20);
  int applied = std::min(std::max(floor, expanded), 20);
  assert(applied == 18);
  assert(st.remaining[15 - 14] > 0);
  assert(st.remaining[18 - 14] == 0);
  floor = 13;
  expanded = st.advance(true, -1, 960, 20);
  applied = std::min(std::max(floor, expanded), 20);
  assert(applied == 15);
  st.reset();
  assert(st.active_samples == 0 && st.remaining[15 - 14] == 0);
  std::puts("rate_change_retention PASS");
}

void resets() {
  SignalBwTemporal st;
  Model m;
  for (int f = 0; f < 30; ++f)
    step(st, m, true, 18, 960, 20);
  st.reset();
  m.reset();
  assert(st.active_samples == 0 && st.last_frame_samples == 0);
  for (int i = 0; i < 7; ++i)
    assert(st.remaining[i] == 0);
  for (int f = 0; f < 10; ++f)
    step(st, m, true, -1, 480, 20);
  SignalBwTemporal zero{};
  assert(zero.advance(true, -1, 960, 20) == 20);
  std::puts("resets PASS");
}
}

int main() {
  constant_20ms_boundary();
  variable_spans();
  silence_and_aux_exclusion();
  warmup_boundaries();
  downward_expiry_and_widening();
  saturation();
  rate_change_retention();
  resets();
  std::puts("signal_bw_temporal_helper=PASS");
  return 0;
}
