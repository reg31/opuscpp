#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

struct curr_OpusEncoder;
struct curr_OpusDecoder;
curr_OpusEncoder* curr_opus_encoder_create(int Fs, int channels, int application, int* error) noexcept;
int curr_opus_encoder_ctl(curr_OpusEncoder* st, int request, ...) noexcept;
int curr_opus_encode(curr_OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes) noexcept;
void curr_opus_encoder_destroy(curr_OpusEncoder* st) noexcept;
curr_OpusDecoder* curr_opus_decoder_create(int Fs, int channels, int* error) noexcept;
int curr_opus_decode(curr_OpusDecoder* st, const unsigned char* data, int len, std::int16_t* pcm, int frame_size, int decode_fec) noexcept;
void curr_opus_decoder_destroy(curr_OpusDecoder* st) noexcept;

extern "C" {
struct OpusEncoder;
struct OpusDecoder;
OpusEncoder* opus_encoder_create(int Fs, int channels, int application, int* error);
int opus_encoder_ctl(OpusEncoder* st, int request, ...);
int opus_encode(OpusEncoder* st, const std::int16_t* pcm, int frame_size, unsigned char* data, int max_data_bytes);
void opus_encoder_destroy(OpusEncoder* st);
OpusDecoder* opus_decoder_create(int Fs, int channels, int* error);
int opus_decode(OpusDecoder* st, const unsigned char* data, int len, std::int16_t* pcm, int frame_size, int decode_fec);
void opus_decoder_destroy(OpusDecoder* st);
}

#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE_REQUEST 4002
#define OPUS_SET_COMPLEXITY_REQUEST 4010
#define OPUS_SET_VBR_REQUEST 4006
#define OPUS_SET_INBAND_FEC_REQUEST 4012
#define OPUS_SET_PACKET_LOSS_PERC_REQUEST 4014
#define OPUS_GET_INBAND_FEC_REQUEST 4013
#define OPUS_GET_PACKET_LOSS_PERC_REQUEST 4015
#define OPUS_OK 0

namespace {

constexpr int sample_rate = 48000;
constexpr int packet_count = 12;
constexpr int dropped_packet = 8;
constexpr int boundary_window = 24;
constexpr double pi = 3.14159265358979323846;

using packet_stream = std::vector<std::vector<unsigned char>>;

auto make_voice(int channels, int frame_size, int profile) -> std::vector<std::int16_t> {
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(packet_count * frame_size * channels));
  std::uint32_t noise_state = 1;
  constexpr std::array signal_levels{7600.0, 2600.0, 5600.0, 2600.0};
  constexpr std::array noise_levels{900.0, 450.0, 2200.0, 0.0};
  for (int frame = 0; frame < packet_count; ++frame) {
    for (int sample = 0; sample < frame_size; ++sample) {
      const auto index = frame * frame_size + sample;
      const double time = static_cast<double>(index) / sample_rate;
      const double pitch = 128.0 + 9.0 * std::sin(2.0 * pi * 1.7 * time);
      const double phase = 2.0 * pi * pitch * time;
      const double envelope = .55 + .35 * std::sin(2.0 * pi * 2.3 * time);
      noise_state = noise_state * 1664525u + 1013904223u;
      const double noise = static_cast<double>(static_cast<std::int32_t>(noise_state)) / 2147483648.0;
      const auto left = static_cast<std::int16_t>(std::lround(signal_levels[static_cast<std::size_t>(profile)] * envelope *
                                                              (.75 * std::sin(phase) + .20 * std::sin(2.0 * phase)) +
                                                              noise_levels[static_cast<std::size_t>(profile)] * noise));
      pcm[static_cast<std::size_t>(index * channels)] = left;
      if (channels == 2) {
        pcm[static_cast<std::size_t>(index * channels + 1)] =
            static_cast<std::int16_t>(std::lround(.88 * left + 420.0 * std::sin(2.0 * pi * 211.0 * time)));
      }
    }
  }
  return pcm;
}

auto nrmse(const std::vector<double>& error, const std::vector<double>& reference) -> double {
  double e = 0.0;
  double r = 1.0;
  for (std::size_t i = 0; i < error.size(); ++i) {
    e += error[i] * error[i];
    r += reference[i] * reference[i];
  }
  return std::sqrt(e / r);
}

} // namespace

int main(int argc, char** argv) {
  const int channels = argc > 1 ? std::atoi(argv[1]) : 1;
  const int duration_ms = argc > 2 ? std::atoi(argv[2]) : 20;
  const int bitrate = argc > 3 ? std::atoi(argv[3]) : 24000;
  const int profile = argc > 4 ? std::atoi(argv[4]) : 0;
  const int vbr = argc > 5 ? std::atoi(argv[5]) : 1;
  if (argc != 6 || (channels != 1 && channels != 2) || (duration_ms != 10 && duration_ms != 20) ||
      (bitrate != 16000 && bitrate != 24000 && bitrate != 32000 && bitrate != 48000) ||
      profile < 0 || profile > 2 || (vbr != 0 && vbr != 1)) {
    std::fprintf(stderr, "usage: fec_source_quality channels duration_ms bitrate profile vbr\n");
    return 2;
  }
  const int frame_size = sample_rate * duration_ms / 1000;
  const auto pcm = make_voice(channels, frame_size, profile);

  int error = OPUS_OK;
#ifdef USE_OFFICIAL_ENCODER
  using enc_t = OpusEncoder;
  std::unique_ptr<enc_t, void (*)(enc_t*)> encoder{opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error),
                                                   opus_encoder_destroy};
  const auto ctl = [](enc_t* st, int request, auto&&... args) { return opus_encoder_ctl(st, request, args...); };
  const auto encode = [](enc_t* st, const std::int16_t* p, int fs, unsigned char* d, int mb) { return opus_encode(st, p, fs, d, mb); };
#else
  using enc_t = curr_OpusEncoder;
  std::unique_ptr<enc_t, void (*)(enc_t*)> encoder{curr_opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error),
                                                   curr_opus_encoder_destroy};
  const auto ctl = [](enc_t* st, int request, auto&&... args) { return curr_opus_encoder_ctl(st, request, args...); };
  const auto encode = [](enc_t* st, const std::int16_t* p, int fs, unsigned char* d, int mb) { return curr_opus_encode(st, p, fs, d, mb); };
#endif
  int fec = -1, loss = -1;
  if (!encoder || ctl(encoder.get(), OPUS_GET_INBAND_FEC_REQUEST, &fec) != OPUS_OK ||
      ctl(encoder.get(), OPUS_GET_PACKET_LOSS_PERC_REQUEST, &loss) != OPUS_OK || ctl(encoder.get(), OPUS_SET_BITRATE_REQUEST, bitrate) != OPUS_OK ||
      ctl(encoder.get(), OPUS_SET_COMPLEXITY_REQUEST, 10) != OPUS_OK || ctl(encoder.get(), OPUS_SET_VBR_REQUEST, vbr) != OPUS_OK ||
      ctl(encoder.get(), OPUS_SET_INBAND_FEC_REQUEST, 2) != OPUS_OK || ctl(encoder.get(), OPUS_SET_PACKET_LOSS_PERC_REQUEST, 15) != OPUS_OK ||
      ctl(encoder.get(), OPUS_SET_INBAND_FEC_REQUEST, 1) != OPUS_OK) {
    std::fprintf(stderr, "encoder setup failed\n");
    return 1;
  }
  int lookahead = 0;
  if (ctl(encoder.get(), 4027, &lookahead) != OPUS_OK) {
    std::fprintf(stderr, "lookahead query failed\n");
    return 1;
  }
  packet_stream packets;
  std::array<unsigned char, 1276> packet{};
  for (int frame = 0; frame < packet_count; ++frame) {
    const int length = encode(encoder.get(), pcm.data() + static_cast<std::size_t>(frame * frame_size * channels), frame_size, packet.data(),
                              static_cast<int>(packet.size()));
    if (length <= 0) {
      std::fprintf(stderr, "encode failed at %d\n", frame);
      return 1;
    }
    packets.emplace_back(packet.begin(), packet.begin() + length);
  }
  std::printf("pkt_toc=");
  for (const auto& pkt : packets) {
    std::printf("%02x,", pkt.empty() ? 0 : pkt[0]);
  }
  std::printf("\n");
  std::printf("pkt_bytes=");
  for (const auto& pkt : packets) {
    std::printf("%d,", static_cast<int>(pkt.size()));
  }
  std::printf("\n");

  const auto samples = static_cast<std::size_t>(frame_size * channels);
  std::vector<std::int16_t> scratch(samples);

  // Loss-free branch: normal decode 0..9, keep frame 8 and frame 9 outputs.
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> lossfree{opus_decoder_create(sample_rate, channels, &error),
                                                                         opus_decoder_destroy};
  std::vector<std::int16_t> ref8(samples), refnext(samples);
  for (int frame = 0; frame <= dropped_packet + 1; ++frame) {
    auto* out = frame == dropped_packet ? ref8.data() : (frame == dropped_packet + 1 ? refnext.data() : scratch.data());
    if (opus_decode(lossfree.get(), packets[static_cast<std::size_t>(frame)].data(),
                    static_cast<int>(packets[static_cast<std::size_t>(frame)].size()), out, frame_size, 0) != frame_size) {
      std::fprintf(stderr, "loss-free decode failed\n");
      return 1;
    }
  }

  // Recovery branch: decode 0..7, FEC decode of packet 9 (recovers frame 8), then normal decode of packet 9.
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> rec{opus_decoder_create(sample_rate, channels, &error),
                                                                    opus_decoder_destroy};
  for (int frame = 0; frame < dropped_packet; ++frame) {
    if (opus_decode(rec.get(), packets[static_cast<std::size_t>(frame)].data(),
                    static_cast<int>(packets[static_cast<std::size_t>(frame)].size()), scratch.data(), frame_size, 0) != frame_size) {
      std::fprintf(stderr, "recovery pre-decode failed\n");
      return 1;
    }
  }
  std::vector<std::int16_t> fec8(samples), recnext(samples);
  const auto& recovery_packet = packets[static_cast<std::size_t>(dropped_packet + 1)];
  if (opus_decode(rec.get(), recovery_packet.data(), static_cast<int>(recovery_packet.size()), fec8.data(), frame_size, 1) != frame_size ||
      opus_decode(rec.get(), recovery_packet.data(), static_cast<int>(recovery_packet.size()), recnext.data(), frame_size, 0) != frame_size) {
    std::fprintf(stderr, "recovery decode failed\n");
    return 1;
  }

  // PLC branch: decode 0..7, then conceal frame 8.
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> plc_dec{opus_decoder_create(sample_rate, channels, &error),
                                                                        opus_decoder_destroy};
  for (int frame = 0; frame < dropped_packet; ++frame) {
    if (opus_decode(plc_dec.get(), packets[static_cast<std::size_t>(frame)].data(),
                    static_cast<int>(packets[static_cast<std::size_t>(frame)].size()), scratch.data(), frame_size, 0) != frame_size) {
      std::fprintf(stderr, "plc pre-decode failed\n");
      return 1;
    }
  }
  std::vector<std::int16_t> plc8(samples);
  if (opus_decode(plc_dec.get(), nullptr, 0, plc8.data(), frame_size, 0) != frame_size) {
    std::fprintf(stderr, "plc decode failed\n");
    return 1;
  }

  const auto reference_start = static_cast<std::size_t>(dropped_packet * frame_size > static_cast<std::size_t>(lookahead)
                                                            ? dropped_packet * frame_size - static_cast<std::size_t>(lookahead)
                                                            : 0);
  std::vector<double> orig8(samples), orignext(samples);
  for (int i = 0; i < frame_size; ++i) {
    for (int ch = 0; ch < channels; ++ch) {
      orig8[static_cast<std::size_t>(i * channels + ch)] = pcm[(reference_start + static_cast<std::size_t>(i)) * channels + ch];
      orignext[static_cast<std::size_t>(i * channels + ch)] =
          pcm[(reference_start + static_cast<std::size_t>(frame_size + i)) * channels + ch];
    }
  }
  const auto fidelity = [&](const std::vector<std::int16_t>& x) {
    std::vector<double> e(samples), r(samples);
    for (std::size_t i = 0; i < samples; ++i) {
      e[i] = static_cast<double>(x[i]) - orig8[i];
      r[i] = orig8[i];
    }
    return nrmse(e, r);
  };
  // Transition: boundary-spanning window of the recovered frame 8 tail and next frame head,
  // compared against the same aligned original window (common reference).
  const auto transition = [&](const std::vector<std::int16_t>& prev, const std::vector<std::int16_t>& next) {
    std::vector<double> e, r;
    for (int i = frame_size - boundary_window; i < frame_size; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        const auto idx = static_cast<std::size_t>(i * channels + ch);
        e.push_back(static_cast<double>(prev[idx]) - orig8[idx]);
        r.push_back(orig8[idx]);
      }
    }
    for (int i = 0; i < boundary_window; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        const auto idx = static_cast<std::size_t>(i * channels + ch);
        e.push_back(static_cast<double>(next[idx]) - orignext[idx]);
        r.push_back(orignext[idx]);
      }
    }
    return nrmse(e, r);
  };
  const auto signed_step = [&](const std::vector<std::int16_t>& prev, const std::vector<std::int16_t>& next) {
    double acc = 0.0;
    for (int ch = 0; ch < channels; ++ch) {
      const auto last = static_cast<std::size_t>((frame_size - 1) * channels + ch);
      acc += (static_cast<double>(next[ch]) - static_cast<double>(prev[last])) - (orignext[ch] - orig8[last]);
    }
    return acc / channels;
  };
  std::vector<double> e_sc(samples), r_sc(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    e_sc[i] = static_cast<double>(fec8[i]) - ref8[i];
    r_sc[i] = ref8[i];
  }
  const auto next_error = [&](const std::vector<std::int16_t>& x) {
    std::vector<double> e(samples), r(samples);
    for (std::size_t i = 0; i < samples; ++i) {
      e[i] = static_cast<double>(x[i]) - orignext[i];
      r[i] = orignext[i];
    }
    return nrmse(e, r);
  };
  // Mid/side error shares, weighted against total source energy (stereo only).
  double ms_src = 0.0, lf_mid = 0.0, lf_side = 0.0, fec_mid = 0.0, fec_side = 0.0;
  if (channels == 2) {
    for (int i = 0; i < frame_size; ++i) {
      const double ol = orig8[static_cast<std::size_t>(2 * i)], orr = orig8[static_cast<std::size_t>(2 * i + 1)];
      const double mid = .5 * (ol + orr), side = .5 * (ol - orr);
      ms_src += mid * mid + side * side;
      for (int pass = 0; pass < 2; ++pass) {
        const auto* frame8 = pass == 0 ? ref8.data() : fec8.data();
        const double dl = static_cast<double>(frame8[static_cast<std::size_t>(2 * i)]) - ol;
        const double dr = static_cast<double>(frame8[static_cast<std::size_t>(2 * i + 1)]) - orr;
        const double dm = .5 * (dl + dr), ds = .5 * (dl - dr);
        double& acc_mid = pass == 0 ? lf_mid : fec_mid;
        double& acc_side = pass == 0 ? lf_side : fec_side;
        acc_mid += dm * dm;
        acc_side += ds * ds;
      }
    }
  }
  std::printf("ms_src=%.6f ms_lf_mid=%.6f ms_lf_side=%.6f ms_fec_mid=%.6f ms_fec_side=%.6f ", ms_src, lf_mid, lf_side, fec_mid, fec_side);
  std::printf("ch=%d ms=%d br=%d p=%d vbr=%d lookahead=%d next_fec=%.9f next_ref=%.9f fidelity_fec=%.9f fidelity_ref=%.9f fidelity_plc=%.9f transition_fec=%.9f "
              "transition_ref=%.9f step_fec=%.3f step_ref=%.3f self_consistency=%.9f\n",
              channels, duration_ms, bitrate, profile, vbr, lookahead, next_error(recnext), next_error(refnext), fidelity(fec8), fidelity(ref8), fidelity(plc8), transition(fec8, recnext),
              transition(ref8, refnext), signed_step(fec8, recnext), signed_step(ref8, refnext), nrmse(e_sc, r_sc));
  return 0;
}
