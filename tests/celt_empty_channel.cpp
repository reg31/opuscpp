// Corrected ordinary regression check for the absent-channel energy/width problem.
// Alignment mirrors the validated width_diag harness: flush frames are encoded, the decoder output has
// lookahead*channels erased, and the comparison is truncated to the true (unpadded) reference length.
// Fresh codecs per segment; reset is tested as an explicit fresh-vs-reset packet identity check.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "opus_codec.h"

namespace {
constexpr int fs = 48000;
constexpr int window = 480;
constexpr int set_bitrate = 4002;
constexpr int set_complexity = 4010;
constexpr int get_lookahead = 4027;
constexpr int reset_state = 4028;
constexpr double pi = 3.14159265358979323846;

using sample_fn = void (*)(int16_t&, int16_t&, double, int);

struct outcome {
  bool ok;
  double width_err;
  double empty_ratio;
  std::uint64_t hash;
  int toc;
};

bool ctl_ok(OpusEncoder* st, int request, auto&&... args) { return opus_encoder_ctl(st, request, args...) == 0; }

auto measure(int frame_size, int application, int bitrate, int frames, sample_fn fn) -> outcome {
  int error = 0;
  std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)> enc{opus_encoder_create(fs, 2, application, &error), opus_encoder_destroy};
  std::unique_ptr<OpusDecoder, void (*)(OpusDecoder*)> dec{opus_decoder_create(fs, 2, &error), opus_decoder_destroy};
  if (!enc || !dec) return {false, 0, 0, 0, -1};
  if (!ctl_ok(enc.get(), set_bitrate, bitrate) || !ctl_ok(enc.get(), set_complexity, 10)) return {false, 0, 0, 0, -1};
  int lookahead = 0;
  if (!ctl_ok(enc.get(), get_lookahead, &lookahead) || lookahead < 0) return {false, 0, 0, 0, -1};

  const std::size_t signal_samples = static_cast<std::size_t>(frames * frame_size * 2);
  const int flush_frames = std::max(1, (lookahead + frame_size - 1) / frame_size);
  std::vector<int16_t> pcm(signal_samples + static_cast<std::size_t>(flush_frames * frame_size * 2), 0);
  for (int f = 0; f < frames; ++f) {
    for (int i = 0; i < frame_size; ++i) {
      const auto t = static_cast<double>(f * frame_size + i) / fs;
      const auto idx = (static_cast<std::size_t>(f) * frame_size + i) * 2;
      fn(pcm[idx], pcm[idx + 1], t, i);
    }
  }
  std::vector<std::vector<unsigned char>> packets;
  std::array<unsigned char, 1276> pkt{};
  std::uint64_t hash = 1469598103934665603ull;
  const int total_frames = frames + flush_frames;
  for (int f = 0; f < total_frames; ++f) {
    const int len = opus_encode(enc.get(), pcm.data() + static_cast<std::size_t>(f) * frame_size * 2, frame_size, pkt.data(), 1276);
    if (len <= 0) return {false, 0, 0, 0, -1};
    for (int b = 0; b < len; ++b) {
      hash ^= pkt[static_cast<std::size_t>(b)];
      hash *= 1099511628211ull;
    }
    packets.emplace_back(pkt.begin(), pkt.begin() + len);
  }
  std::vector<float> decoded;
  std::vector<float> scratch(static_cast<std::size_t>(frame_size * 2));
  for (const auto& p : packets) {
    if (opus_decode_float(dec.get(), p.data(), static_cast<int>(p.size()), scratch.data(), frame_size, 0) != frame_size) {
      return {false, 0, 0, 0, -1};
    }
    decoded.insert(decoded.end(), scratch.begin(), scratch.end());
  }
  const std::size_t erase_n = static_cast<std::size_t>(lookahead) * 2;
  if (decoded.size() < erase_n + signal_samples) return {false, 0, 0, 0, -1};
  decoded.erase(decoded.begin(), decoded.begin() + static_cast<std::ptrdiff_t>(erase_n));
  decoded.resize(signal_samples);  // truncate to the true reference, no synthetic tail

  std::vector<int16_t> ref(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(signal_samples));
  double e0 = 0, e1 = 0, agg = 0;
  for (std::size_t i = 0; i + 1 < decoded.size(); i += 2) {
    e0 += static_cast<double>(decoded[i]) * decoded[i];
    e1 += static_cast<double>(decoded[i + 1]) * decoded[i + 1];
  }
  int windows = 0;
  const std::size_t total = decoded.size() / 2;
  for (std::size_t w = window; w + window <= total; w += window) {
    double rm = 0, rs = 0, dm = 0, ds = 0;
    for (int i = 0; i < window; ++i) {
      const auto idx = (w + static_cast<std::size_t>(i)) * 2;
      const double a0 = static_cast<double>(ref[idx]) / 32768.0, a1 = static_cast<double>(ref[idx + 1]) / 32768.0;
      const double b0 = decoded[idx], b1 = decoded[idx + 1];
      rm += 0.25 * (a0 + a1) * (a0 + a1);
      rs += 0.25 * (a0 - a1) * (a0 - a1);
      dm += 0.25 * (b0 + b1) * (b0 + b1);
      ds += 0.25 * (b0 - b1) * (b0 - b1);
    }
    const double rw = std::sqrt(rs / std::max(rm + rs, 1e-12));
    const double dw = std::sqrt(ds / std::max(dm + ds, 1e-12));
    agg += std::fabs(rw - dw);
    ++windows;
  }
  const double ratio = (std::max(e0, e1) <= 0.0) ? 0.0 : std::min(e0, e1) / std::max(e0, e1);
  return {true, windows ? agg / windows : 0.0, ratio, hash, packets.front().empty() ? -1 : packets.front()[0]};
}

// reset-vs-fresh: encode segment, reset, encode again; the post-reset packets must equal a fresh encode
auto reset_identity(int frame_size, int application, int bitrate, int frames, sample_fn fn) -> bool {
  int error = 0;
  std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)> enc{opus_encoder_create(fs, 2, application, &error), opus_encoder_destroy};
  if (!enc) return false;
  if (!ctl_ok(enc.get(), set_bitrate, bitrate) || !ctl_ok(enc.get(), set_complexity, 10)) return false;
  std::vector<int16_t> pcm(static_cast<std::size_t>(frames * frame_size * 2));
  for (int f = 0; f < frames; ++f)
    for (int i = 0; i < frame_size; ++i) {
      const auto t = static_cast<double>(f * frame_size + i) / fs;
      const auto idx = (static_cast<std::size_t>(f) * frame_size + i) * 2;
      fn(pcm[idx], pcm[idx + 1], t, i);
    }
  std::array<unsigned char, 1276> a{}, b{};
  std::vector<std::vector<unsigned char>> first;
  for (int f = 0; f < frames; ++f) {
    const int len = opus_encode(enc.get(), pcm.data() + static_cast<std::size_t>(f) * frame_size * 2, frame_size, a.data(), 1276);
    if (len <= 0) return false;
    first.emplace_back(a.begin(), a.begin() + len);
  }
  if (!ctl_ok(enc.get(), reset_state)) return false;
  std::vector<std::vector<unsigned char>> second;
  for (int f = 0; f < frames; ++f) {
    const int len = opus_encode(enc.get(), pcm.data() + static_cast<std::size_t>(f) * frame_size * 2, frame_size, b.data(), 1276);
    if (len <= 0) return false;
    second.emplace_back(b.begin(), b.begin() + len);
  }
  return first == second;
}

void left_only(int16_t& l, int16_t& r, double t, int) {
  const double v = 12000.0 * std::sin(2.0 * pi * 220.0 * t) * (0.6 + 0.4 * std::sin(2.0 * pi * 3.0 * t));
  l = static_cast<int16_t>(std::lround(v));
  r = 0;
}
void right_only(int16_t& l, int16_t& r, double t, int) {
  left_only(r, l, t, 0);
}
void both_active(int16_t& l, int16_t& r, double t, int) {
  const double v = 12000.0 * std::sin(2.0 * pi * 220.0 * t);
  l = static_cast<int16_t>(std::lround(v));
  r = static_cast<int16_t>(std::lround(9000.0 * std::sin(2.0 * pi * 330.0 * t + 0.7)));
}
void alternating(int16_t& l, int16_t& r, double t, int) {  // both -> right absent -> both
  const double v = 12000.0 * std::sin(2.0 * pi * 220.0 * t);
  const bool right_absent = std::fmod(t, 0.4) < 0.2;
  l = static_cast<int16_t>(std::lround(v));
  r = right_absent ? 0 : static_cast<int16_t>(std::lround(9000.0 * std::sin(2.0 * pi * 330.0 * t + 0.7)));
}
void alternating_mirror(int16_t& l, int16_t& r, double t, int i) {
  alternating(r, l, t, i);
}
void near_silent_right(int16_t& l, int16_t& r, double t, int i) {
  l = static_cast<int16_t>(std::lround(12000.0 * std::sin(2.0 * pi * 220.0 * t)));
  r = static_cast<int16_t>((i & 1) ? 1 : -1);
}
void both_silent(int16_t& l, int16_t& r, double, int) {
  l = 0;
  r = 0;
}
} // namespace

int main() {
  int failures = 0;
  auto check = [&](const char* name, int frame_size, int application, int bitrate, int frames, sample_fn fn, bool eligible) {
    const auto r = measure(frame_size, application, bitrate, frames, fn);
    if (!r.ok) {
      std::printf("case=%-22s frame=%4d rc=FAIL_DECODE\n", name, frame_size);
      ++failures;
      return;
    }
    bool pass = true;
    if (eligible) {
      const bool eligible_pass = r.width_err < 1e-11;
      if (!eligible_pass) {
        pass = false;
      }
    }
    std::printf("case=%-22s frame=%4d width_err=%.12e empty_ratio=%.12e toc=0x%02x hash=%016llx eligible=%d verdict=%s%s\n",
                name, frame_size, r.width_err, r.empty_ratio, r.toc, static_cast<unsigned long long>(r.hash),
                static_cast<int>(eligible), pass ? "PASS" : "FAIL", pass ? "" : " reason=width above gate");
    if (!pass) ++failures;
  };

  check("left_only_20ms", 960, 2049, 64000, 40, left_only, true);
  check("right_only_20ms", 960, 2049, 64000, 40, right_only, true);
  check("left_only_10ms", 480, 2049, 64000, 80, left_only, true);
  check("left_only_5ms", 240, 2049, 64000, 160, left_only, true);
  check("left_only_2p5ms", 120, 2049, 64000, 320, left_only, false);
  check("both_active_20ms", 960, 2049, 64000, 40, both_active, false);
  check("alternating_20ms", 960, 2049, 64000, 40, alternating, false);
  check("alternating_mirror_20ms", 960, 2049, 64000, 40, alternating_mirror, false);
  check("near_silent_right_20ms", 960, 2049, 64000, 40, near_silent_right, false);
  check("both_silent_20ms", 960, 2049, 64000, 40, both_silent, false);
  check("voip_24k_20ms", 960, 2048, 24000, 40, left_only, false);
  check("voip_32k_20ms",  960, 2048, 32000, 40, left_only, false);

  const bool reset_ok = reset_identity(960, 2049, 64000, 20, left_only);
  std::printf("reset_fresh_identity=%s\n", reset_ok ? "PASS" : "FAIL");
  if (!reset_ok) ++failures;
  std::printf("SUMMARY failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
