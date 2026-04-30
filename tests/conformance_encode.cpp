#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define OpusEncoder codex_OpusEncoder
#define OpusDecoder codex_OpusDecoder
#define opus_encoder_create codex_opus_encoder_create
#define opus_encoder_destroy codex_opus_encoder_destroy
#define opus_encoder_ctl codex_opus_encoder_ctl
#define opus_encode codex_opus_encode
#define opus_encode_float codex_opus_encode_float
#define opus_decoder_create codex_opus_decoder_create
#define opus_decoder_destroy codex_opus_decoder_destroy
#define opus_decoder_ctl codex_opus_decoder_ctl
#define opus_decode codex_opus_decode
#define opus_decode_float codex_opus_decode_float
#define opus_packet_get_nb_samples codex_opus_packet_get_nb_samples
#define opus_strerror codex_opus_strerror
#if defined(CODEX_EXTERN_C)
extern "C" {
#endif
#include "opus_codec.h"
#if defined(CODEX_EXTERN_C)
}
#endif
#undef OpusEncoder
#undef OpusDecoder
#undef opus_encoder_create
#undef opus_encoder_destroy
#undef opus_encoder_ctl
#undef opus_encode
#undef opus_encode_float
#undef opus_decoder_create
#undef opus_decoder_destroy
#undef opus_decoder_ctl
#undef opus_decode
#undef opus_decode_float
#undef opus_packet_get_nb_samples
#undef opus_strerror

#include "encode_conformance_shared.h"

extern "C" {
struct OpusDecoder;
OpusDecoder* opus_decoder_create(int Fs, int channels, int* error);
void opus_decoder_destroy(OpusDecoder* st);
int opus_decode(OpusDecoder* st, const unsigned char* data, int len, opus_int16* pcm, int frame_size, int decode_fec);
}

namespace {

using namespace encode_conformance;

struct worker_slice {
    std::size_t begin = 0;
    std::size_t end = 0;
    int index = 0;
    int count = 1;
};

struct worker_result {
    std::vector<std::string> pass_lines;
    std::size_t completed_cases = 0;
};

[[nodiscard]] auto parse_int_argument(const char* text, std::string_view name) -> int {
    if (text == nullptr) {
        throw std::runtime_error("missing " + std::string{name});
    }

    int value = 0;
    const char* end = text + std::char_traits<char>::length(text);
    const auto [ptr, error] = std::from_chars(text, end, value);
    if (error != std::errc{} || ptr != end) {
        throw std::runtime_error("invalid " + std::string{name} + ": " + text);
    }
    return value;
}

[[nodiscard]] constexpr auto make_worker_slice(
    const std::size_t total_cases,
    const int worker_index,
    const int worker_count) -> worker_slice {
    return worker_slice{
        .begin = (total_cases * static_cast<std::size_t>(worker_index)) / static_cast<std::size_t>(worker_count),
        .end = (total_cases * static_cast<std::size_t>(worker_index + 1)) / static_cast<std::size_t>(worker_count),
        .index = worker_index,
        .count = worker_count,
    };
}

[[nodiscard]] auto find_clip(
    const std::vector<clip_input>& clips,
    std::string_view label,
    int channels) -> const clip_input* {
    for (const auto& clip : clips) {
        if (clip.label == label && clip.channels == channels) {
            return &clip;
        }
    }
    return nullptr;
}

[[nodiscard]] auto make_encoder(int channels, const scenario& current)
    -> std::unique_ptr<codex_OpusEncoder, decltype(&codex_opus_encoder_destroy)> {
    int error = OPUS_OK;
    auto* encoder = codex_opus_encoder_create(48000, channels, current.application, &error);
    if (error != OPUS_OK || encoder == nullptr) {
        throw std::runtime_error("failed to create codex encoder");
    }
    return {encoder, &codex_opus_encoder_destroy};
}

[[nodiscard]] auto make_official_decoder(int channels)
    -> std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> {
    int error = OPUS_OK;
    auto* decoder = opus_decoder_create(48000, channels, &error);
    if (error != OPUS_OK || decoder == nullptr) {
        throw std::runtime_error("failed to create official decoder");
    }
    return {decoder, &opus_decoder_destroy};
}

[[nodiscard]] auto describe_packet_mismatch(
    std::span<const unsigned char> expected,
    std::span<const unsigned char> actual,
    std::uint32_t expected_range,
    std::uint32_t actual_range,
    const clip_input& clip,
    const scenario& current,
    std::size_t frame_index) -> std::string {
    const auto mismatch =
        std::ranges::mismatch(expected, actual, std::ranges::equal_to{}, [](unsigned char value) { return value; },
                              [](unsigned char value) { return value; });
    const auto mismatch_index = static_cast<std::size_t>(mismatch.in1 - expected.begin());
    std::size_t mismatch_count = 0;
    std::size_t last_mismatch_index = mismatch_index;
    unsigned last_expected = mismatch.in1 != expected.end() ? *mismatch.in1 : 0U;
    unsigned last_actual = mismatch.in2 != actual.end() ? *mismatch.in2 : 0U;
    auto mismatch_indices = std::string{};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] != actual[index]) {
            ++mismatch_count;
            last_mismatch_index = index;
            last_expected = expected[index];
            last_actual = actual[index];
            if (mismatch_indices.size() < 80) {
                if (!mismatch_indices.empty()) {
                    mismatch_indices += ',';
                }
                mismatch_indices += std::to_string(index);
            }
        }
    }

    auto message = std::string{};
    auto append_slice = [&message](std::string_view label, std::span<const unsigned char> bytes, std::size_t begin,
                                   std::size_t end) {
        message += " ";
        message += label;
        message += "=";
        for (std::size_t index = begin; index < end && index < bytes.size(); ++index) {
            if (index > begin) {
                message += ':';
            }
            message += std::to_string(static_cast<unsigned>(bytes[index]));
        }
    };
    message += "packet mismatch for ";
    message += clip.label;
    message += " / ";
    message += current.name;
    message += " frame ";
    message += std::to_string(frame_index);
    message += " at byte ";
    message += std::to_string(mismatch_index);
    message += " expected=";
    message += mismatch.in1 != expected.end() ? std::to_string(static_cast<unsigned>(*mismatch.in1)) : "eof";
    message += " actual=";
    message += mismatch.in2 != actual.end() ? std::to_string(static_cast<unsigned>(*mismatch.in2)) : "eof";
    message += " packet_size=";
    message += std::to_string(expected.size());
    message += " mismatch_count=";
    message += std::to_string(mismatch_count);
    message += " mismatch_indices=";
    message += mismatch_indices;
    message += " last_byte=";
    message += std::to_string(last_mismatch_index);
    message += " last_expected=";
    message += std::to_string(last_expected);
    message += " last_actual=";
    message += std::to_string(last_actual);
    message += " expected_range=";
    message += std::to_string(expected_range);
    message += " actual_range=";
    message += std::to_string(actual_range);
    append_slice("expected_24_40", expected, 24, 40);
    append_slice("actual_24_40", actual, 24, 40);
    append_slice("expected_68_80", expected, 68, 80);
    append_slice("actual_68_80", actual, 68, 80);
    return message;
}

void configure_encoder(codex_OpusEncoder* encoder, int channels, const scenario& current) {
    if (const auto ret =
            codex_opus_encoder_ctl(encoder, OPUS_SET_BITRATE_REQUEST, scenario_bitrate(current, channels));
        ret != OPUS_OK) {
        throw std::runtime_error("failed to set codex bitrate");
    }
    if (const auto ret = codex_opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY_REQUEST, current.complexity);
        ret != OPUS_OK) {
        throw std::runtime_error("failed to set codex complexity");
    }
    if (const auto ret = codex_opus_encoder_ctl(encoder, OPUS_SET_VBR_REQUEST, static_cast<int>(current.vbr));
        ret != OPUS_OK) {
        throw std::runtime_error("failed to set codex VBR");
    }
}

[[nodiscard]] auto compare_case(const case_record& oracle_case, const clip_input& clip, const scenario& current)
    -> std::string {
    const auto max_frame_index = []() -> std::optional<std::size_t> {
        if (const auto* value = std::getenv("CODEX_MAX_FRAME_INDEX")) {
            const auto parsed = parse_int_argument(value, "max frame index");
            if (parsed < 0) {
                throw std::runtime_error("max frame index must be non-negative");
            }
            return static_cast<std::size_t>(parsed);
        }
        return std::nullopt;
    }();
    const auto require_bitexact = [] {
        if (const auto* value = std::getenv("CODEX_REQUIRE_BITEXACT")) {
            return std::string_view{value} != "0" && std::string_view{value} != "false";
        }
        return false;
    }();
    auto encoder = make_encoder(clip.channels, current);
    configure_encoder(encoder.get(), clip.channels, current);
    auto official_decoder = make_official_decoder(clip.channels);

    const auto samples = load_pcm_samples(clip.path);
    const auto samples_per_frame = static_cast<std::size_t>(current.frame_size * clip.channels);
    auto decoded = std::vector<opus_int16>(samples_per_frame);
    const auto expected_frames = samples.size() / samples_per_frame;
    const auto expected_oracle_frames =
        max_frame_index.has_value() ? std::min(expected_frames, *max_frame_index + 1U) : expected_frames;
    if (expected_oracle_frames != oracle_case.frames.size()) {
        throw std::runtime_error("frame count mismatch for " + clip.label + " / " + std::string{current.name});
    }

    for (std::size_t frame_index = 0; frame_index < oracle_case.frames.size(); ++frame_index) {
        if (max_frame_index.has_value() && frame_index > *max_frame_index) {
            break;
        }
        const auto frame_offset = frame_index * samples_per_frame;
        std::array<unsigned char, 1500> packet{};
        const int packet_size = codex_opus_encode(
            encoder.get(),
            samples.data() + frame_offset,
            current.frame_size,
            packet.data(),
            static_cast<int>(packet.size()));

        if (packet_size < 0) {
            throw std::runtime_error("codex encoder returned an error");
        }

        const auto& oracle_frame = oracle_case.frames[frame_index];
        std::uint32_t final_range = 0;
        if (const auto ret = codex_opus_encoder_ctl(encoder.get(), OPUS_GET_FINAL_RANGE_REQUEST, &final_range);
            ret != OPUS_OK) {
            throw std::runtime_error("failed to read codex final range");
        }

        if (require_bitexact) {
            if (packet_size != static_cast<int>(oracle_frame.packet.size())) {
                throw std::runtime_error(
                    "packet size mismatch for " + clip.label + " / " + std::string{current.name} + " frame "
                    + std::to_string(frame_index));
            }
            if (!std::ranges::equal(
                    std::span<const unsigned char>{packet.data(), static_cast<std::size_t>(packet_size)},
                    std::span<const unsigned char>{oracle_frame.packet})) {
                throw std::runtime_error(describe_packet_mismatch(
                    oracle_frame.packet,
                    std::span<const unsigned char>{packet.data(), static_cast<std::size_t>(packet_size)},
                    oracle_frame.final_range,
                    final_range,
                    clip,
                    current,
                    frame_index));
            }
            if (final_range != oracle_frame.final_range) {
                throw std::runtime_error(
                    "final range mismatch for " + clip.label + " / " + std::string{current.name} + " frame "
                    + std::to_string(frame_index));
            }
        } else {
            const int decoded_samples = opus_decode(
                official_decoder.get(),
                packet.data(),
                packet_size,
                decoded.data(),
                current.frame_size,
                0);
            if (decoded_samples != current.frame_size) {
                throw std::runtime_error(
                    "official decoder rejected codex packet for " + clip.label + " / "
                    + std::string{current.name} + " frame " + std::to_string(frame_index));
            }
        }
    }

    return "PASS clip=" + clip.label + " scenario=" + std::string{current.name}
         + " frames=" + std::to_string(oracle_case.frames.size());
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto vector_path = argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"opus_newvectors"};
        const auto oracle_path = argc > 2 ? std::filesystem::path{argv[2]} : std::filesystem::path{"encode_oracle.bin"};
        const auto worker_index = argc > 3 ? parse_int_argument(argv[3], "worker index") : 0;
        const auto worker_count = argc > 4 ? parse_int_argument(argv[4], "worker count") : 1;
        const auto clip_filter = []() -> std::string_view {
            if (const auto* value = std::getenv("CODEX_CLIP_FILTER")) {
                return value;
            }
            return {};
        }();
        const auto scenario_filter = []() -> std::string_view {
            if (const auto* value = std::getenv("CODEX_SCENARIO_FILTER")) {
                return value;
            }
            return {};
        }();
        if (worker_count <= 0) {
            throw std::runtime_error("worker count must be positive");
        }
        if (worker_index < 0 || worker_index >= worker_count) {
            throw std::runtime_error("worker index out of range");
        }
        if (argc == 4) {
            throw std::runtime_error("worker count is required when worker index is provided");
        }

        const auto clips = discover_clips(vector_path);
        const auto oracle_cases = read_oracle(oracle_path);
        if (oracle_cases.empty()) {
            throw std::runtime_error("oracle file did not contain any cases");
        }

        const auto run_slice = [&](const worker_slice& slice_to_run) -> worker_result {
            auto result = worker_result{};
            for (const auto case_index : std::views::iota(slice_to_run.begin, slice_to_run.end)) {
                const auto& oracle_case = oracle_cases[case_index];
                if (!clip_filter.empty() && oracle_case.clip_label != clip_filter) {
                    continue;
                }
                if (!scenario_filter.empty() && oracle_case.scenario_name != scenario_filter) {
                    continue;
                }
                const auto* clip = find_clip(clips, oracle_case.clip_label, oracle_case.channels);
                if (clip == nullptr) {
                    throw std::runtime_error("missing input clip " + oracle_case.clip_label);
                }
                const auto* current = find_scenario(oracle_case.scenario_name);
                if (current == nullptr) {
                    throw std::runtime_error("unknown scenario " + oracle_case.scenario_name);
                }
                result.pass_lines.push_back(compare_case(oracle_case, *clip, *current));
                ++result.completed_cases;
            }
            return result;
        };

        const auto can_use_internal_parallelism = argc <= 3 && worker_count == 1;
        if (can_use_internal_parallelism) {
            const auto available_threads = static_cast<int>(std::thread::hardware_concurrency());
            const auto internal_worker_count = std::clamp(available_threads > 0 ? available_threads : 1, 1, 8);
            if (internal_worker_count > 1) {
                std::vector<std::future<worker_result>> futures;
                futures.reserve(static_cast<std::size_t>(internal_worker_count));
                for (int internal_worker = 0; internal_worker < internal_worker_count; ++internal_worker) {
                    futures.push_back(std::async(
                        std::launch::async,
                        [&, internal_worker] {
                            return run_slice(make_worker_slice(oracle_cases.size(), internal_worker, internal_worker_count));
                        }));
                }

                std::size_t completed_cases = 0;
                for (auto& future : futures) {
                    auto result = future.get();
                    for (const auto& line : result.pass_lines) {
                        std::cout << line << '\n';
                    }
                    completed_cases += result.completed_cases;
                }
                std::cout << "Encode conformance completed (" << completed_cases << " case checks passed)\n";
                return 0;
            }
        }

        const auto slice = make_worker_slice(oracle_cases.size(), worker_index, worker_count);
        auto result = run_slice(slice);
        for (const auto& line : result.pass_lines) {
            std::cout << line << '\n';
        }

        if (worker_count > 1) {
            std::cout << "Encode conformance worker completed (worker " << (worker_index + 1) << '/' << worker_count
                      << ", " << result.completed_cases << " case checks passed)\n";
        } else {
            std::cout << "Encode conformance completed (" << result.completed_cases << " case checks passed)\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Encode conformance failed: " << ex.what() << '\n';
        return 1;
    }
}
