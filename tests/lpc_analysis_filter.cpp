#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "../src/opus_codec.cpp"

void* lpc_official_decoder_create(int rate, int channels, int* error);
void lpc_official_decoder_destroy(void* decoder);
int lpc_official_decode(void* decoder, const unsigned char* packet, int bytes, opus_int16* output, int capacity);
int lpc_official_final_range(void* decoder, opus_uint32* range);

static bool write_pcm16(std::ofstream& file, std::span<const opus_int16> pcm) {
  std::array<unsigned char, 3840> bytes;
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    const auto value = static_cast<std::uint16_t>(pcm[i]);
    bytes[2 * i] = static_cast<unsigned char>(value);
    bytes[2 * i + 1] = static_cast<unsigned char>(value >> 8);
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), 2 * pcm.size());
  return static_cast<bool>(file);
}

static bool check_public_packets(const std::filesystem::path& root) {
  unsigned silk_layer_frames = 0;
  for (const int complexity : {0, 5, 9, 10}) {
    for (const int bitrate : {16000, 24000, 32000, 48000}) {
      const auto path = root / ("c" + std::to_string(complexity) + "_b" + std::to_string(bitrate));
      std::filesystem::create_directories(path);
      std::ofstream native_file(path / "native16_mono.pcm", std::ios::binary);
      std::ofstream reference_file(path / "official48_stereo.pcm", std::ios::binary);
      std::ofstream packet_file(path / "packets.bin", std::ios::binary);
      std::ofstream ranges(path / "ranges.csv");
      ranges << "frame,native16,official16,official48\n";
      int error = OPUS_OK;
      const auto encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error), opus_encoder_destroy};
      const auto decoder = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>{opus_decoder_create(16000, 1, &error), opus_decoder_destroy};
      const auto matching = std::unique_ptr<void, decltype(&lpc_official_decoder_destroy)>{lpc_official_decoder_create(16000, 1, &error), lpc_official_decoder_destroy};
      const auto reference = std::unique_ptr<void, decltype(&lpc_official_decoder_destroy)>{lpc_official_decoder_create(48000, 2, &error), lpc_official_decoder_destroy};
      if (!encoder || !decoder || !matching || !reference || error || !native_file || !reference_file || !packet_file || !ranges ||
          opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate)) || opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(complexity))) {
        std::cerr << "Public oracle setup failed complexity=" << complexity << " bitrate=" << bitrate << '\n';
        return false;
      }
      std::array<opus_int16, 320> input, output, official16;
      std::array<opus_int16, 1920> official48;
      std::array<unsigned char, 1500> packet;
      opus_uint32 random = 1;
      int silk_frames = 0, hybrid_frames = 0, celt_frames = 0;
      for (int frame = 0; frame < 200; ++frame) {
        for (int i = 0; i < 320; ++i) {
          const double t = (frame * 320 + i) / 16000.;
          random = random * 1664525U + 1013904223U;
          const double noise = (static_cast<int>(random >> 16) - 32768) * .005;
          const double phase = 2 * 3.141592653589793 * (140 * t + 3 * std::sin(2 * t));
          input[i] = static_cast<opus_int16>((.6 + .4 * std::sin(11 * t)) * (7000 * std::sin(phase) + 2500 * std::sin(2 * phase) + 1500 * std::sin(3 * phase)) + noise);
        }
        const int bytes = opus_encode(encoder.get(), input.data(), 320, packet.data(), packet.size());
        if (bytes <= 0) {
          std::cerr << "Public oracle encode failed complexity=" << complexity << " bitrate=" << bitrate << " frame=" << frame << '\n';
          return false;
        }
        const int native_samples = opus_decode(decoder.get(), packet.data(), bytes, output.data(), 320, 0);
        const int matching_samples = lpc_official_decode(matching.get(), packet.data(), bytes, official16.data(), 320);
        const int reference_samples = lpc_official_decode(reference.get(), packet.data(), bytes, official48.data(), 960);
        if (native_samples != 320 || matching_samples != 320 || reference_samples != 960) {
          std::cerr << "Public oracle return failed complexity=" << complexity << " bitrate=" << bitrate << " frame=" << frame << " native=" << native_samples << " official16=" << matching_samples << " official48=" << reference_samples << '\n';
          return false;
        }
        opus_uint32 native_range = 0, matching_range = 0, reference_range = 0;
        if (opus_decoder_ctl(decoder.get(), OPUS_GET_FINAL_RANGE(&native_range)) || lpc_official_final_range(matching.get(), &matching_range) ||
            lpc_official_final_range(reference.get(), &reference_range) || native_range != matching_range || native_range != reference_range) {
          std::cerr << "Public oracle range failed complexity=" << complexity << " bitrate=" << bitrate << " frame=" << frame << '\n';
          return false;
        }
        if (packet[0] & 0x80) ++celt_frames;
        else if ((packet[0] & 0x60) == 0x60) ++hybrid_frames;
        else ++silk_frames;
        ranges << frame << ',' << native_range << ',' << matching_range << ',' << reference_range << '\n';
        const std::array<unsigned char, 4> length{static_cast<unsigned char>(bytes), static_cast<unsigned char>(bytes >> 8), static_cast<unsigned char>(bytes >> 16), static_cast<unsigned char>(bytes >> 24)};
        packet_file.write(reinterpret_cast<const char*>(length.data()), length.size());
        packet_file.write(reinterpret_cast<const char*>(packet.data()), bytes);
        if (!write_pcm16(native_file, output) || !write_pcm16(reference_file, official48) || !packet_file || !ranges) return false;
      }
      native_file.close(); reference_file.close(); packet_file.close(); ranges.close();
      if (!native_file || !reference_file || !packet_file || !ranges) return false;
      silk_layer_frames += silk_frames + hybrid_frames;
      std::cout << "lpc_case complexity=" << complexity << " bitrate=" << bitrate << " frames=200 silk_frames=" << silk_frames << " hybrid_frames=" << hybrid_frames << " celt_frames=" << celt_frames << '\n';
    }
  }
  if (silk_layer_frames == 0) {
    std::cerr << "Public oracle has no actual SILK-layer packets\n";
    return false;
  }
  std::cout << "lpc_silk_layer_frames=" << silk_layer_frames << '\n';
  return true;
}

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  const bool packets_ok = check_public_packets(argv[1]);
  if (!packets_ok) std::cerr << "Public oracle packet/range gate failed\n";
  const auto check_fraction = [](opus_uint32 bits) {
    opus_int32 leading = 0, fraction = 0;
    silk_CLZ_FRAC(std::bit_cast<opus_int32>(bits), &leading, &fraction);
    int expected_leading = 32;
    for (auto value = bits; value != 0; value >>= 1)
      --expected_leading;
    const auto expected_fraction = ((static_cast<std::uint64_t>(bits) << expected_leading) >> 24) & 127;
    return leading == expected_leading && static_cast<std::uint64_t>(fraction) == expected_fraction;
  };
  opus_uint32 mask = 0, random = 1;
  if (!check_fraction(0))
    return 1;
  for (unsigned bits = 0; bits < 32; ++bits) {
    if (low_bits_mask(bits) != mask || !check_fraction(opus_uint32{1} << bits) || !check_fraction(mask))
      return 1;
    mask = (mask << 1) | 1;
  }
  for (int i = 0; i < 200000; ++i) {
    random = random * 1664525U + 1013904223U;
    if (!check_fraction(random))
      return 1;
  }
  int status = OPUS_OK;
  auto encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>{opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &status), opus_encoder_destroy};
  if (!encoder || status != OPUS_OK || opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(16000)) != OPUS_OK)
    return 1;
  std::array<opus_int16, 960> speech{};
  std::array<unsigned char, 1500> packet{};
  for (int i = 0; i < 960; ++i)
    speech[i] = static_cast<opus_int16>((i % 50 - 25) * 200);
  if (opus_encode(encoder.get(), speech.data(), 960, packet.data(), packet.size()) <= 0 || encoder->mode == opus_mode_celt_only)
    return 1;
  auto& state = silk_encoder_channel_states(static_cast<silk_encoder*>(encoder_silk_state(encoder.get())))[0].sCmn;
  state.frameCounter = std::numeric_limits<decltype(state.frameCounter)>::max();
  for (const auto expected : {opus_uint32{0}, opus_uint32{1}}) {
    if (opus_encode(encoder.get(), speech.data(), 960, packet.data(), packet.size()) <= 0 || state.frameCounter != expected)
      return 1;
  }
  for (int order : {0, 2, 4, 6, 8, 10, 12, 16, 24}) {
    std::array<float, 25> correlation;
    std::array<float, 24> reflection;
    correlation.fill(std::numeric_limits<float>::quiet_NaN());
    reflection.fill(1.f);
    correlation[0] = .75f;
    std::fill_n(correlation.begin() + 1, order, 0.f);
    if (silk_schur_FLP(reflection.data(), correlation.data(), order) != .75f)
      return 1;
    for (int i = 0; i < 24; ++i) {
      if (reflection[i] != (i < order ? 0.f : 1.f))
        return 1;
    }
  }
  std::array<float, 96> input;
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<float>(static_cast<int>(i * i % 101) - 50) * .125f;
  }
  for (int order : {6, 8, 10, 12, 16}) {
    std::array<float, 16> coefficients;
    coefficients.fill(std::numeric_limits<float>::quiet_NaN());
    for (int i = 0; i < order; ++i) {
      coefficients[i] = static_cast<float>(i - 3) * .015625f;
    }
    for (int length : {order, order + 1, static_cast<int>(input.size())}) {
      std::array<float, 96> actual;
      actual.fill(std::numeric_limits<float>::quiet_NaN());
      silk_LPC_analysis_filter_FLP(actual.data(), coefficients.data(), input.data(), length, order);
      for (int i = 0; i < length; ++i) {
        float expected = 0;
        if (i >= order) {
          float prediction = 0;
          for (int tap = 0; tap < order; ++tap) {
            prediction += input[i - tap - 1] * coefficients[tap];
          }
          expected = input[i] - prediction;
        }
        if (actual[i] != expected) {
          std::cerr << "LPC filter mismatch order=" << order << " length=" << length << " sample=" << i << '\n';
          return 1;
        }
      }
    }
  }
  for (int samples : {240, 360, 600, 1080}) {
    std::array<float, celt_max_frame_samples + celt_default_overlap> pcm{};
    std::array<float, celt_lpc_order> denominator{}, memory{};
    denominator[0] = -.5f;
    memory[0] = .25f;
    for (int i = 0; i < samples; ++i)
      pcm[i] = static_cast<float>(i % 17 - 8) * .125f;
    const auto original = pcm;
    celt_iir(pcm.data(), denominator.data(), pcm.data(), samples, memory.data());
    float previous = .25f;
    for (int i = 0; i < samples; ++i) {
      const float expected = original[i] + .5f * previous;
      if (pcm[i] != expected) {
        std::cerr << "CELT IIR mismatch length=" << samples << " sample=" << i << '\n';
        return 1;
      }
      previous = expected;
    }
  }
  std::cout << "lpc_helper_checks=PASS (bit helpers; seed wrap; Schur initialization; SILK orders 6/8/10/12/16; CELT PLC including overlap)\n";
  if (!packets_ok) return 1;
  std::cout << "lpc_packet_checks=PASS (16 cases; 3200 frames; exact public returns/final ranges; actual TOC coverage)\n";
}
