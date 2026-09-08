#include "../src/opus_codec.cpp"
#include <cstdio>
#include <stdexcept>
#include <vector>

static void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

int main() {
  try {
    int cases = 0;
    for (int channels : {1, 2}) {
      for (int warm : {480, 960}) {
        for (int gap : {120, 240, 480, 960, 1920, 2880}) {
          for (bool floating : {false, true}) {
            int error = 0;
            auto encoder = make_opus_encoder(48000, channels, channels == 1 ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO, &error);
            auto decoder = make_opus_decoder(48000, channels, &error);
            check(encoder && decoder, "allocation");
            check(opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(10)) == OPUS_OK, "complexity");
            check(opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(128000)) == OPUS_OK, "bitrate");
            std::vector<float> source_tail(312 * channels);
            int tick = 0;
            auto encode = [&](int samples, bool as_float) {
              std::vector<opus_int16> input(samples * channels);
              std::vector<float> input_float(input.size()), output(input.size());
              for (std::size_t i = 0; i < input.size(); ++i) {
                input[i] = static_cast<opus_int16>(((++tick * 389) % 30000) - 15000);
                input_float[i] = input[i] * (1.f / 32768);
              }
              std::array<unsigned char, 1500> packet{};
              const int length = as_float ? opus_encode_float(encoder.get(), input_float.data(), samples, packet.data(), packet.size())
                                          : opus_encode(encoder.get(), input.data(), samples, packet.data(), packet.size());
              check(length > 0, "encode");
              check(opus_decode_float(decoder.get(), packet.data(), length, output.data(), samples, 0) == samples, "decode");
              opus_uint32 encode_range = 0, decode_range = 0;
              check(opus_encoder_ctl(encoder.get(), OPUS_GET_FINAL_RANGE(&encode_range)) == OPUS_OK, "encode range");
              check(opus_decoder_ctl(decoder.get(), OPUS_GET_FINAL_RANGE(&decode_range)) == OPUS_OK, "decode range");
              check(encode_range == decode_range, "range parity");
              source_tail.insert(source_tail.end(), input_float.begin(), input_float.end());
              source_tail.erase(source_tail.begin(), source_tail.end() - 312 * channels);
              auto* history = encoder_celt_state(encoder.get())->quality_history;
              check(history != nullptr, "history");
              for (int c = 0; c < channels; ++c)
                for (int i = 0; i < 312; ++i)
                  check(history->input_delay[c][i] == source_tail[i * channels + c], "stale input tail");
              if (history->ready && history->status == quality_tracking_status::valid) {
                auto* actual = decoder_celt_state(decoder.get());
                auto* model = decoder_celt_state(history->decoder);
                check(std::memcmp(actual, model, sizeof(CeltDecoderInternal)) == 0, "decoder header");
                check(std::equal(celt_decoder_storage(actual), celt_decoder_storage(actual) + celt_decoder_storage_count(channels), celt_decoder_storage(model)), "decoder storage");
              }
              return (packet[0] & 0x80) != 0;
            };
            for (int i = 0; i < 4; ++i)
              encode(warm, false);
            encode(gap, floating);
            auto* history = encoder_celt_state(encoder.get())->quality_history;
            const bool missing_reference = gap <= 312 || gap > 960;
            if (missing_reference)
              check(!history->previous_packet_celt, "untracked reference eligible");
            const bool celt = encode(warm, false);
            if (missing_reference)
              check(!history->packet_selection_ready, "first post-gap proposal");
            const bool should_resume = celt && history->ready && decoder_celt_state(history->decoder)->start == 0;
            encode(warm, false);
            if (should_resume)
              check(history->packet_selection_ready, "warmup did not resume");
            ++cases;
          }
        }
      }
    }
    std::printf("reference continuity: %d variable-frame/API sequences passed\n", cases);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
