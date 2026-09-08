#include "../src/opus_codec.cpp"
#include <cstdio>
#include <vector>

static void require(bool value) {
  if (!value) {
    std::abort();
  }
}

int main() {
  {
    quality_history_state history;
    quality_frame_work work;
    std::array<opus_int16, 1920> input;
    for (int i = 0; i < 960; ++i)
      input[2 * i] = input[2 * i + 1] = static_cast<opus_int16>((i * 389) % 30000 - 15000);
    history.borrowed_input = input.data();
    history.borrowed_frame_size = 960;
    history.borrowed_channels = 2;
    for (int i = 0; i < 960; ++i)
      for (int c = 0; c < 2; ++c)
        work.output[2 * i + c] = quality_reference_sample(history, i, c);
    const auto perfect = quality_score_decoded(history, work, 1, 2, 960);
    require(perfect[0] == 0 && perfect[1] == 0);
    work.output.back() += .125f;
    const auto changed = quality_score_decoded(history, work, 1, 2, 960);
    require(changed[0] == .015625 && changed[1] == .125);
    for (const int channels : {1, 2}) {
      history.borrowed_channels = channels;
      for (const int samples : {480, 960}) {
        history.borrowed_frame_size = samples;
        for (int frame = 0; frame < 3; ++frame) {
          quality_score_decoded(history, work, 1, channels, samples);
          const auto reference_bands = work.reference_bands;
          const auto decoded_bands = work.decoded_bands;
          work.reference_bands = {};
          work.decoded_bands = {};
          quality_advance_filters(history, work, 1, channels, samples);
          require(work.reference_bands == reference_bands && work.decoded_bands == decoded_bands);
          history.incoming_reference_bands = reference_bands;
          history.incoming_bands = decoded_bands;
        }
      }
    }
  }
  int checked = 0;
  for (const auto channels : {1, 2}) {
    for (const auto duration : {480, 960}) {
      for (const auto complexity : {0, 9, 10}) {
        const auto application = channels == 1 ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
        int error = 0;
        auto* encoder = opus_encoder_create(48000, channels, application, &error);
        auto* control = opus_encoder_create(48000, channels, application, &error);
        auto* decoder = opus_decoder_create(48000, channels, &error);
        require(encoder && control && decoder);
        for (auto* enc : {encoder, control}) {
          require(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity)) == OPUS_OK);
          require(opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000)) == OPUS_OK);
        }
        std::vector<opus_int16> pcm(duration * channels);
        std::vector<float> decoded(duration * channels);
        std::array<unsigned char, 1276> packet{}, reference{};
        for (int frame = 0; frame < 120; ++frame) {
          for (int i = 0; i < duration; ++i) {
            const auto time = (frame * duration + i) / 48000.0;
            for (int c = 0; c < channels; ++c) {
              pcm[i * channels + c] = static_cast<opus_int16>(8000 * std::sin(time * 1177 + c * .2) + 2400 * std::sin(time * 3133 + c * .7));
            }
          }
          if (frame == 40) {
            for (auto* enc : {encoder, control}) {
              require(opus_encoder_ctl(enc, OPUS_RESET_STATE) == OPUS_OK);
            }
            require(opus_decoder_ctl(decoder, OPUS_RESET_STATE) == OPUS_OK);
          }
          if (frame == 60) {
            std::vector<float> invalid(duration * channels, 0.f);
            invalid[duration / 2] = std::numeric_limits<float>::quiet_NaN();
            require(opus_encode_float(encoder, invalid.data(), duration, packet.data(), packet.size()) == OPUS_BAD_ARG);
          }
          if (frame == 80) {
            for (auto* enc : {encoder, control}) {
              require(opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000)) == OPUS_OK);
              require(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1)) == OPUS_OK);
              require(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10)) == OPUS_OK);
            }
          }
          const int length = opus_encode(encoder, pcm.data(), duration, packet.data(), packet.size());
          const int reference_length = opus_encode(control, pcm.data(), duration, reference.data(), reference.size());
          require(length > 0 && length == reference_length);
          require(std::equal(packet.begin(), packet.begin() + length, reference.begin()));
          require(opus_decode_float(decoder, packet.data(), length, decoded.data(), duration, 0) == duration);
          opus_uint32 encode_range = 0, decode_range = 0;
          require(opus_encoder_ctl(encoder, OPUS_GET_FINAL_RANGE(&encode_range)) == OPUS_OK);
          require(opus_decoder_ctl(decoder, OPUS_GET_FINAL_RANGE(&decode_range)) == OPUS_OK);
          require(encode_range == decode_range);
          const auto* history = encoder_celt_state(encoder)->quality_history;
          if (history != nullptr && length > 1)
            require(history->packet_prev_redundancy == (decoder->prev_redundancy != 0));
          if (complexity < 10) {
            require(history == nullptr);
          }
          if (history && history->status == quality_tracking_status::valid && history->ready) {
            auto* actual = decoder_celt_state(decoder);
            auto* model = decoder_celt_state(history->decoder);
            require(std::memcmp(actual, model, sizeof(CeltDecoderInternal)) == 0);
            require(std::equal(celt_decoder_storage(actual), celt_decoder_storage(actual) + celt_decoder_storage_count(channels), celt_decoder_storage(model)));
            ++checked;
          }
        }
        opus_encoder_destroy(encoder);
        opus_encoder_destroy(control);
        opus_decoder_destroy(decoder);
      }
    }
  }
  require(checked > 0);
  std::printf("encoder history: %d exact packet/state checks; reset, invalid input, lower complexity and FEC passed\n", checked);
}
