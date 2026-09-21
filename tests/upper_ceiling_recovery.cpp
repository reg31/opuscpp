// Regression test for upper-CELT-boundary recovery when SILK holds its bandwidth switch.
//
// Scenario: hybrid at 24 kbps (above the legacy fullband cap threshold) -> low SILK rate -> back to
// 24 kbps with sustained voiced + high-band input while SILK keeps its switch gate held. The SWB/FB
// ceiling must be refreshed from the cached ordinary fullband choice, producing sustained FB packets
// once SILK is WB/LP ready. Decoding every packet must remain valid. A 24 kHz input must never
// select FB.
//
// Source under test is the codec translation unit itself (internal-test style):
//   production:            tests/upper_ceiling_recovery.cpp -> #include "../src/opus_codec.cpp"
//   private candidate run: -DOPUSCPP_UPPER_CEILING_SRC='"C/opus_codec.cpp"'
//
// Build and run standalone (there is no CMake registration here):
//   g++ -std=c++23 -O2 -DNDEBUG -Isrc tests/upper_ceiling_recovery.cpp -o upper_ceiling_recovery_test
//   ./upper_ceiling_recovery_test
#ifndef OPUSCPP_UPPER_CEILING_SRC
#define OPUSCPP_UPPER_CEILING_SRC "../src/opus_codec.cpp"
#endif
#include OPUSCPP_UPPER_CEILING_SRC

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr double pi = 3.14159265358979323846;
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what, int value = 0) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("upper_ceiling_recovery FAIL: %s value=%d\n", what, value);
  }
}

void check_ctl(int status, const char* what) {
  check(status == OPUS_OK, what, status);
}

void make_frame(std::vector<std::int16_t>& out, int frame_size, int sample_rate, int index, bool high) {
  out.assign(static_cast<std::size_t>(frame_size), 0);
  const double high_tone_hz = sample_rate >= 48000 ? 14000.0 : 9500.0;
  for (int i = 0; i < frame_size; ++i) {
    const double t = (index * frame_size + i) / static_cast<double>(sample_rate);
    double v = 0.18 * std::sin(2 * pi * 400 * t) + 0.12 * std::sin(2 * pi * 1700 * t);
    if (high) {
      v += 0.15 * std::sin(2 * pi * high_tone_hz * t);
    }
    out[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::clamp<long>(std::lrint(v * 32767.0), -32768, 32767));
  }
}

struct packet_info {
  int config = 0;
  int bandwidth = 0;  // 1104 SWB, 1105 FB, 0 otherwise
  bool hybrid = false;
  int pre_bw = 0;
  int pre_auto = 0;
  int pre_mode = 0;
  bool pre_wb_lp = false;
  bool pre_allow = false;
  bool held_eligible = false;
};

// 24 kbps -> 8 kbps -> 24 kbps; high content throughout. Returns per-packet TOC info; decodes every
// packet when a decoder is available.
std::vector<packet_info> run_recovery_sequence() {
  int error = 0;
  auto* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
  auto* dec = opus_decoder_create(48000, 1, &error);
  check(enc != nullptr && dec != nullptr, "create");
  if (enc == nullptr || dec == nullptr) {
    if (enc != nullptr) opus_encoder_destroy(enc);
    if (dec != nullptr) opus_decoder_destroy(dec);
    return {};
  }
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_BITRATE_REQUEST, 24000), "set initial bitrate");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY_REQUEST, 10), "set complexity");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_VBR_REQUEST, 1), "set vbr");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC_REQUEST, 0), "disable inband fec");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC_REQUEST, 0), "disable packet loss");
  std::vector<std::int16_t> pcm;
  std::vector<unsigned char> packet(1500);
  std::vector<std::int16_t> decoded(960);
  auto* st = reinterpret_cast<OpusEncoder*>(enc);
  std::vector<packet_info> out;
  int requested_bitrate = 24000; // the configured test setting for the current phase
  for (int frame = 0; frame < 64; ++frame) {
    if (frame == 12 || frame == 34) {
      check_ctl(opus_encoder_ctl(enc, OPUS_SET_BITRATE_REQUEST, 8000), "set low bitrate");
      requested_bitrate = 8000;
    }
    if (frame == 22 || frame == 44) {
      check_ctl(opus_encoder_ctl(enc, OPUS_SET_BITRATE_REQUEST, 24000), "set recovery bitrate");
      requested_bitrate = 24000;
    }
    make_frame(pcm, 960, 48000, frame, true);
    packet_info info;
    info.pre_bw = st->bandwidth;
    info.pre_auto = st->auto_bandwidth;
    info.pre_mode = st->prev_mode;
    info.pre_wb_lp = st->silk_mode.inWBmodeWithoutVariableLP;
    info.pre_allow = st->silk_mode.allowBandwidthSwitch;
    const int size = opus_encode(enc, pcm.data(), 960, packet.data(), static_cast<int>(packet.size()));
    check(size > 0, "encode", frame);
    if (size <= 0) {
      break;
    }
    info.config = packet[0] >> 3;
    info.hybrid = info.config >= 12 && info.config < 16;
    if (info.hybrid) {
      info.bandwidth = info.config < 14 ? 1104 : 1105;
    }
    // Eligible only in the configured 24 kbps phase, with the previous resolved mode hybrid, the
    // returned packet actually hybrid, SWB stalled under a cached fullband ceiling, SILK WB/LP ready
    // and the switch gate held. A rate decrease or a mode reset therefore cannot be called eligible.
    info.held_eligible = requested_bitrate == 24000 && info.pre_mode == opus_mode_hybrid && info.pre_bw == 1104 &&
                         info.pre_auto == 1105 && info.pre_wb_lp && !info.pre_allow && info.hybrid;
    std::printf(
        "upper_ceiling_recovery frame=%d req=%d config=%d bw=%d pre_bw=%d pre_auto=%d pre_mode=%d wbLP=%d allow=%d eligible=%d\n",
        frame, requested_bitrate, info.config, info.bandwidth, info.pre_bw, info.pre_auto, info.pre_mode,
        info.pre_wb_lp ? 1 : 0, info.pre_allow ? 1 : 0, info.held_eligible ? 1 : 0);
    out.push_back(info);
    const int got = opus_decode(dec, packet.data(), size, decoded.data(), 960, 0);
    check(got == 960, "decode", frame);
  }
  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);
  return out;
}

void check_recovery_sequence() {
  const auto packets = run_recovery_sequence();
  check(packets.size() == 64, "packet count", static_cast<int>(packets.size()));
  if (packets.size() != 64) {
    return;
  }
  // Both low-rate segments must be SILK (config < 12), not merely non-hybrid.
  int low_silk = 0;
  for (int i = 12; i < 22; ++i) {
    low_silk += packets[static_cast<std::size_t>(i)].config < 12 ? 1 : 0;
  }
  for (int i = 34; i < 44; ++i) {
    low_silk += packets[static_cast<std::size_t>(i)].config < 12 ? 1 : 0;
  }
  check(low_silk == 20, "low-rate segments should be SILK", low_silk);
  // The intended recovery paths must run in hybrid mode.
  int recovery_hybrid = 0;
  for (int i = 22; i < 34; ++i) {
    recovery_hybrid += packets[static_cast<std::size_t>(i)].hybrid ? 1 : 0;
  }
  for (int i = 44; i < 64; ++i) {
    recovery_hybrid += packets[static_cast<std::size_t>(i)].hybrid ? 1 : 0;
  }
  check(recovery_hybrid >= 16, "recovery windows should be hybrid", recovery_hybrid);
  // Upper-boundary contract: a held-but-eligible frame (SWB stalled while the cached ordinary ceiling
  // is fullband, SILK already WB/LP ready, switch gate held) must be fullband once the current rate
  // ceiling permits it. No fixed minimum run length is used as the pass criterion.
  int eligible = 0;
  int eligible_not_fb = 0;
  for (int i = 0; i < 64; ++i) {
    const auto& info = packets[static_cast<std::size_t>(i)];
    if (!info.held_eligible) {
      continue;
    }
    ++eligible;
    if (info.bandwidth != 1105) {
      ++eligible_not_fb;
    }
  }
  check(eligible >= 1, "held-but-eligible state exercised", eligible);
  check(eligible_not_fb == 0, "held-but-eligible frames must be fullband", eligible_not_fb);
  std::printf("upper_ceiling_recovery eligible=%d eligible_not_fb=%d recovery_hybrid=%d low_silk=%d\n", eligible,
              eligible_not_fb, recovery_hybrid, low_silk);
}

void check_24k_input_ceiling() {
  int error = 0;
  auto* enc = opus_encoder_create(24000, 1, OPUS_APPLICATION_VOIP, &error);
  auto* dec = opus_decoder_create(24000, 1, &error);
  check(enc != nullptr && dec != nullptr, "24k create");
  if (enc == nullptr || dec == nullptr) {
    if (enc != nullptr) opus_encoder_destroy(enc);
    if (dec != nullptr) opus_decoder_destroy(dec);
    return;
  }
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_BITRATE_REQUEST, 24000), "24k set bitrate");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY_REQUEST, 10), "24k set complexity");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_VBR_REQUEST, 1), "24k set vbr");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC_REQUEST, 0), "24k disable inband fec");
  check_ctl(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC_REQUEST, 0), "24k disable packet loss");
  std::vector<std::int16_t> pcm;
  std::vector<unsigned char> packet(1500);
  std::vector<std::int16_t> decoded(480);
  int hybrid_frames = 0;
  int fb_frames = 0;
  for (int frame = 0; frame < 20; ++frame) {
    make_frame(pcm, 480, 24000, frame, true);
    const int size = opus_encode(enc, pcm.data(), 480, packet.data(), static_cast<int>(packet.size()));
    check(size > 0, "24k encode", frame);
    if (size <= 0) {
      break;
    }
    const int config = packet[0] >> 3;
    if (config >= 12 && config < 16) {
      ++hybrid_frames;
    }
    // Count fullband through the canonical TOC parser so a CELT-only fullband packet cannot be missed.
    if (ref_opus_packet_get_bandwidth(packet.data()) == 1105) {
      ++fb_frames;
    }
    const int got = opus_decode(dec, packet.data(), size, decoded.data(), 480, 0);
    check(got == 480, "24k decode", frame);
  }
  check(hybrid_frames >= 5, "24k hybrid frames", hybrid_frames);
  check(fb_frames == 0, "24k must never select fullband", fb_frames);
  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);
}
} // namespace

int main() {
  check_recovery_sequence();
  check_24k_input_ceiling();
  std::printf("upper_ceiling_recovery checks=%d failures=%d %s\n", g_checks, g_failures,
              g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
