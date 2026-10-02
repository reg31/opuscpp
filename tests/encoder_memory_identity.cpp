#include "opus_codec.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  std::ofstream output(argv[1], std::ios::binary);
  if (!output)
    return 2;
  auto write = [&](const auto* data, std::size_t count) {
    output.write(reinterpret_cast<const char*>(data), count * sizeof(*data));
  };
  std::uint64_t frames = 0, dtx = 0;
  std::array<std::uint64_t, 3> modes{};
  for (int fs : {8000, 12000, 16000, 24000, 48000})
    for (int channels : {1, 2})
      for (int application : {OPUS_APPLICATION_VOIP, OPUS_APPLICATION_AUDIO, OPUS_APPLICATION_RESTRICTED_LOWDELAY})
        for (int duration : {5, 10, 20, 40, 80, 120, 160, 200, 240})
          for (int profile : {0, 1, 2})
            for (int float_api : {0, 1}) {
              const int samples = fs * duration / 2000;
              auto encoder = make_opus_encoder(fs, channels, application, nullptr);
              auto decoder = make_opus_decoder(fs, channels, nullptr);
              auto fec_decoder = make_opus_decoder(fs, channels, nullptr);
              if (!encoder || !decoder || !fec_decoder)
                return 3;
              if (opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(profile != 0)) ||
                  opus_encoder_ctl(encoder.get(), OPUS_SET_DTX(profile == 2)) ||
                  opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(profile == 2)) ||
                  opus_encoder_ctl(encoder.get(), OPUS_SET_PACKET_LOSS_PERC(profile == 2 ? 20 : 0)))
                return 4;
              std::vector<opus_int16> pcm(samples * channels), decoded(5760 * channels);
              std::vector<float> pcm_float(pcm.size()), decoded_float(decoded.size());
              std::array<unsigned char, 7656> packet;
              std::uint32_t random = 19;
              for (int frame = 0; frame < 48; ++frame) {
                const int bitrate = profile == 2 ? 24000 * channels : frame < 4 ? 8000 * channels
                                                                  : frame < 8   ? 32000 * channels
                                                                                : 96000 * channels;
                if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)))
                  return 5;
                for (int i = 0; i < samples; ++i)
                  for (int c = 0; c < channels; ++c) {
                    random = random * 1664525u + 1013904223u;
                    const double phase = 6.283185307179586 * (frame * samples + i) / fs;
                    const double signal = frame >= 12 ? 0.0 : 7000 * std::sin((frame < 4 ? 183 : 443 + 127 * c) * phase) + (frame >= 8 ? static_cast<int>(random >> 17) - 16384 : 0);
                    pcm[i * channels + c] = static_cast<opus_int16>(signal);
                    pcm_float[i * channels + c] = static_cast<float>(signal / 32768.0);
                  }
                const int size = float_api ? opus_encode_float(encoder.get(), pcm_float.data(), samples, packet.data(), packet.size()) : opus_encode(encoder.get(), pcm.data(), samples, packet.data(), packet.size());
                if (size <= 0) {
                  std::cerr << "encode failed " << fs << ' ' << channels << ' ' << duration << ' ' << size << '\n';
                  return 6;
                }
                const int mode = packet[0] >= 128 ? 2 : packet[0] >= 96 ? 1
                                                                        : 0;
                ++modes[mode];
                ++frames;
                dtx += size == 1;
                write(&size, 1);
                write(packet.data(), size);
                opus_uint32 range = 0;
                if (opus_encoder_ctl(encoder.get(), OPUS_GET_FINAL_RANGE(&range)))
                  return 7;
                write(&range, 1);
                const int decoded_samples = float_api ? opus_decode_float(decoder.get(), packet.data(), size, decoded_float.data(), 5760, 0) : opus_decode(decoder.get(), packet.data(), size, decoded.data(), 5760, 0);
                if (decoded_samples != samples)
                  return 8;
                write(&decoded_samples, 1);
                if (float_api)
                  write(decoded_float.data(), decoded_samples * channels);
                else
                  write(decoded.data(), decoded_samples * channels);
                if (profile == 2) {
                  const int fec_samples = opus_decode(fec_decoder.get(), packet.data(), size, decoded.data(), samples, 1);
                  if (fec_samples != samples)
                    return 9;
                  write(decoded.data(), fec_samples * channels);
                }
              }
            }
  std::cout << "frames=" << frames << " silk=" << modes[0] << " hybrid=" << modes[1]
            << " celt=" << modes[2] << " dtx=" << dtx << '\n';
  return output ? 0 : 10;
}
