#include "opus_codec.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

uint32_t read_be32(FILE* f, bool& ok) {
    unsigned char b[4];
    size_t n = std::fread(b, 1, 4, f);
    if (n != 4) {
        ok = false;
        return 0;
    }
    ok = true;
    return (static_cast<uint32_t>(b[0]) << 24) |
           (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) |
           static_cast<uint32_t>(b[3]);
}

int16_t f32_to_s16(float x) {
    int v = static_cast<int>(x * 32768.0f);
    v = std::clamp(v, -32768, 32767);
    return static_cast<int16_t>(v);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc != 6) {
        std::fprintf(stderr, "usage: conformance_decode <rate> <channels> <in.bit> <out.pcm> <check_range 0|1>\n");
        return 1;
    }

    int rate = std::atoi(argv[1]);
    int channels = std::atoi(argv[2]);
    const char* in_path = argv[3];
    const char* out_path = argv[4];
    int check_range = std::atoi(argv[5]);

    if ((rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        (channels != 1 && channels != 2)) {
        std::fprintf(stderr, "Invalid rate/channels.\n");
        return 1;
    }

    FILE* fin = std::fopen(in_path, "rb");
    if (!fin) {
        std::fprintf(stderr, "Cannot open input: %s\n", in_path);
        return 1;
    }
    FILE* fout = std::fopen(out_path, "wb");
    if (!fout) {
        std::fprintf(stderr, "Cannot open output: %s\n", out_path);
        std::fclose(fin);
        return 1;
    }

    int err = 0;
    OpusDecoder* dec = opus_decoder_create(rate, channels, &err);
    if (!dec || err != OPUS_OK) {
        std::fprintf(stderr, "opus_decoder_create failed: %d\n", err);
        std::fclose(fin);
        std::fclose(fout);
        return 1;
    }

    const int max_frame = rate * 2; // 120 ms max
    std::vector<unsigned char> packet(1275);
    std::vector<float> out(max_frame * channels);
    std::vector<int16_t> out16(max_frame * channels);

    int frame_index = 0;
    while (true) {
        bool ok = false;
        uint32_t len = read_be32(fin, ok);
        if (!ok) break;
        uint32_t enc_final = read_be32(fin, ok);
        if (!ok) break;

        if (len > packet.size()) {
            packet.resize(len);
        }

        if (len > 0u) {
            if (std::fread(packet.data(), 1, len, fin) != len) break;
        }

        int out_samples = 0;
        if (len == 0u) {
            int last = 0;
            if (opus_decoder_ctl(dec, OPUS_GET_LAST_PACKET_DURATION(&last)) != OPUS_OK || last <= 0) {
                last = rate / 50;
            }
            out_samples = opus_decode_float(dec, nullptr, 0, out.data(), std::min(last, max_frame), 0);
        } else {
            out_samples = opus_decode_float(dec, packet.data(), static_cast<int>(len), out.data(), max_frame, 0);
        }

        if (out_samples < 0) {
            std::fprintf(stderr, "Decode failed in frame %d: %d\n", frame_index, out_samples);
            opus_decoder_destroy(dec);
            std::fclose(fin);
            std::fclose(fout);
            return 3;
        }
        if (out_samples > max_frame) {
            std::fprintf(stderr, "Decoder returned too many samples in frame %d: %d > %d\n",
                         frame_index, out_samples, max_frame);
            opus_decoder_destroy(dec);
            std::fclose(fin);
            std::fclose(fout);
            return 7;
        }

        if (check_range && len > 0u && enc_final != 0u) {
            uint32_t dec_final = 0;
            if (opus_decoder_ctl(dec, OPUS_GET_FINAL_RANGE(&dec_final)) != OPUS_OK) {
                std::fprintf(stderr, "OPUS_GET_FINAL_RANGE failed in frame %d\n", frame_index);
                opus_decoder_destroy(dec);
                std::fclose(fin);
                std::fclose(fout);
                return 4;
            }
            if (dec_final != enc_final) {
                std::fprintf(stderr, "Error: Range coder state mismatch in frame %d: 0x%08x vs 0x%08x\n",
                             frame_index, enc_final, dec_final);
                opus_decoder_destroy(dec);
                std::fclose(fin);
                std::fclose(fout);
                return 5;
            }
        }

        int samples_total = out_samples * channels;
        for (int i = 0; i < samples_total; ++i) out16[i] = f32_to_s16(out[i]);
        if (std::fwrite(out16.data(), sizeof(int16_t), samples_total, fout) != static_cast<size_t>(samples_total)) {
            std::fprintf(stderr, "Write failed.\n");
            opus_decoder_destroy(dec);
            std::fclose(fin);
            std::fclose(fout);
            return 6;
        }

        frame_index++;
    }

    opus_decoder_destroy(dec);
    std::fclose(fin);
    std::fclose(fout);
    std::fprintf(stderr, "Decoded %d frames.\n", frame_index);
    return 0;
}
