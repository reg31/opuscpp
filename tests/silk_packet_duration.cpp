#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
#include "../src/opus_codec.cpp"

namespace {

bool failed = false;

void expect(bool ok, const char* name, int rate, int ms) {
  if (!ok) {
    failed = true;
    std::cout << "FAIL " << name << " rate=" << rate << " ms=" << ms << '\n';
  }
}

int child_ms(int outer_ms) {
  if (outer_ms == 80)
    return 40;
  if (outer_ms == 100)
    return 20;
  if (outer_ms == 120)
    return 60;
  return outer_ms;
}

std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> make_enc(int rate, int* status) {
  return std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, status), opus_encoder_destroy};
}

void run_case(int rate, int outer_ms, bool float_api) {
  int status = OPUS_OK;
  auto enc = make_enc(rate, &status);
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> dec{opus_decoder_create(rate, 1, &status), opus_decoder_destroy};
  expect(enc != nullptr && dec != nullptr && status == OPUS_OK, "create", rate, outer_ms);
  if (!enc || !dec)
    return;
  if (opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(rate == 8000 ? 6000 : 8000)) != OPUS_OK ||
      opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY(10)) != OPUS_OK) {
    expect(false, "ctl", rate, outer_ms);
    return;
  }
  const int frame_size = rate * outer_ms / 1000;
  std::vector<opus_int16> in(static_cast<std::size_t>(frame_size)), out(static_cast<std::size_t>(frame_size) + rate / 100);
  for (int i = 0; i < frame_size; ++i)
    in[static_cast<std::size_t>(i)] = static_cast<opus_int16>(1200 * std::sin(0.05 * i) + 600 * std::sin(0.31 * i));
  std::array<unsigned char, 4000> packet{};
  int encoded = -1;
  if (float_api) {
    std::vector<float> fin(static_cast<std::size_t>(frame_size));
    for (int i = 0; i < frame_size; ++i)
      fin[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] / 32768.0f;
    encoded = opus_encode_float(enc.get(), fin.data(), frame_size, packet.data(), static_cast<int>(packet.size()));
  } else {
    encoded = opus_encode(enc.get(), in.data(), frame_size, packet.data(), static_cast<int>(packet.size()));
  }
  expect(encoded > 0, "encode", rate, outer_ms);
  expect(enc->mode == opus_mode_silk_only, "silk_mode", rate, outer_ms);
  expect(enc->silk_mode.payloadSize_ms == child_ms(outer_ms), "payloadSize_ms", rate, outer_ms);
  expect(encoded > 0 && opus_packet_get_nb_samples(packet.data(), encoded, rate) == frame_size, "toc_duration", rate, outer_ms);
  const int decoded = encoded > 0 ? opus_decode(dec.get(), packet.data(), encoded, out.data(), frame_size, 0) : -1;
  expect(decoded == frame_size, "decode_duration", rate, outer_ms);
}

void run_repeat_60ms(int rate) {
  int status = OPUS_OK;
  auto enc = make_enc(rate, &status);
  std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> dec{opus_decoder_create(rate, 1, &status), opus_decoder_destroy};
  expect(enc != nullptr && dec != nullptr && status == OPUS_OK, "create_repeat", rate, 60);
  if (!enc || !dec)
    return;
  if (opus_encoder_ctl(enc.get(), OPUS_SET_BITRATE(rate == 8000 ? 6000 : 8000)) != OPUS_OK ||
      opus_encoder_ctl(enc.get(), OPUS_SET_COMPLEXITY(10)) != OPUS_OK) {
    expect(false, "ctl_repeat", rate, 60);
    return;
  }
  const int frame_size = rate * 60 / 1000;
  std::vector<opus_int16> in(static_cast<std::size_t>(frame_size)), out(static_cast<std::size_t>(frame_size) + rate / 100);
  std::array<unsigned char, 4000> packet{};
  for (int repeat = 0; repeat < 3; ++repeat) {
    for (int i = 0; i < frame_size; ++i)
      in[static_cast<std::size_t>(i)] = static_cast<opus_int16>(900 * std::sin(0.037 * (i + repeat * 7)) + 500 * std::sin(0.23 * i));
    const int encoded = opus_encode(enc.get(), in.data(), frame_size, packet.data(), static_cast<int>(packet.size()));
    expect(encoded > 0, "encode_repeat", rate, 60);
    expect(enc->mode == opus_mode_silk_only, "silk_mode_repeat", rate, 60);
    expect(enc->silk_mode.payloadSize_ms == 60, "payloadSize_ms_repeat", rate, 60);
    expect(encoded > 0 && opus_packet_get_nb_samples(packet.data(), encoded, rate) == frame_size, "toc_repeat", rate, 60);
    const int decoded = encoded > 0 ? opus_decode(dec.get(), packet.data(), encoded, out.data(), frame_size, 0) : -1;
    expect(decoded == frame_size, "decode_repeat", rate, 60);
  }
}

}

int main() {
  for (const int rate : {8000, 48000}) {
    for (const int outer_ms : {10, 20, 40, 60, 80, 100, 120}) {
      run_case(rate, outer_ms, false);
      if (outer_ms == 60)
        run_case(rate, outer_ms, true);
    }
    run_repeat_60ms(rate);
  }
  std::cout << (failed ? "FAILED" : "PASSED") << '\n';
  return failed ? 1 : 0;
}
