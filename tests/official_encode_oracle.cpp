#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <opus.h>

#include "encode_conformance_shared.h"

namespace {

using namespace encode_conformance;

[[nodiscard]] auto make_encoder(int channels, const scenario& current) -> std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> {
    int error = OPUS_OK;
    auto* encoder = opus_encoder_create(48000, channels, current.application, &error);
    if (error != OPUS_OK || encoder == nullptr) {
        throw std::runtime_error("failed to create official encoder");
    }
    return {encoder, &opus_encoder_destroy};
}

void configure_encoder(OpusEncoder* encoder, int channels, const scenario& current) {
    if (const auto ret = opus_encoder_ctl(encoder, OPUS_SET_BITRATE_REQUEST, scenario_bitrate(current, channels));
        ret != OPUS_OK) {
        throw std::runtime_error("failed to set official bitrate");
    }
    if (const auto ret = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY_REQUEST, current.complexity); ret != OPUS_OK) {
        throw std::runtime_error("failed to set official complexity");
    }
    if (const auto ret = opus_encoder_ctl(encoder, OPUS_SET_VBR_REQUEST, static_cast<int>(current.vbr)); ret != OPUS_OK) {
        throw std::runtime_error("failed to set official VBR");
    }
}

[[nodiscard]] auto encode_case(const clip_input& clip, const scenario& current) -> case_record {
    const auto max_frame_index = []() -> std::optional<std::size_t> {
        if (const auto* value = std::getenv("CODEX_MAX_FRAME_INDEX")) {
            const auto parsed = std::strtol(value, nullptr, 10);
            if (parsed < 0) {
                throw std::runtime_error("max frame index must be non-negative");
            }
            return static_cast<std::size_t>(parsed);
        }
        return std::nullopt;
    }();
    auto result = case_record{
        .clip_label = clip.label,
        .scenario_name = std::string{current.name},
        .channels = clip.channels,
    };

    auto encoder = make_encoder(clip.channels, current);
    configure_encoder(encoder.get(), clip.channels, current);

    const auto samples = load_pcm_samples(clip.path);
    const auto samples_per_frame = static_cast<std::size_t>(current.frame_size * clip.channels);
    const auto frame_count = samples.size() / samples_per_frame;
    result.frames.reserve(frame_count);

    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        if (max_frame_index.has_value() && frame_index > *max_frame_index) {
            break;
        }
        const auto frame_offset = frame_index * samples_per_frame;
        std::array<unsigned char, 1500> packet{};
        const int packet_size = opus_encode(
            encoder.get(),
            samples.data() + frame_offset,
            current.frame_size,
            packet.data(),
            static_cast<int>(packet.size()));
        if (packet_size < 0) {
            throw std::runtime_error("official encoder returned an error");
        }

        std::uint32_t final_range = 0;
        if (const auto ret = opus_encoder_ctl(encoder.get(), OPUS_GET_FINAL_RANGE_REQUEST, &final_range);
            ret != OPUS_OK) {
            throw std::runtime_error("failed to read official final range");
        }

        result.frames.push_back(
            frame_record{.final_range = final_range, .packet = {packet.begin(), packet.begin() + packet_size}});
    }

    std::cout << "ORACLE clip=" << clip.label << " scenario=" << current.name << " frames=" << frame_count << '\n';
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto vector_path = argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"opus_newvectors"};
        const auto oracle_path = argc > 2 ? std::filesystem::path{argv[2]} : std::filesystem::path{"encode_oracle.bin"};
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

        const auto clips = discover_clips(vector_path);
        if (clips.empty()) {
            std::cerr << "No .dec inputs found in " << vector_path << '\n';
            return 1;
        }

        auto cases = std::vector<case_record>{};
        cases.reserve(clips.size() * scenarios.size());
        for (const auto& clip : clips) {
            if (!clip_filter.empty() && clip.label != clip_filter) {
                continue;
            }
            for (const auto& current : scenarios) {
                if (!scenario_filter.empty() && current.name != scenario_filter) {
                    continue;
                }
                cases.push_back(encode_case(clip, current));
            }
        }

        write_oracle(oracle_path, cases);
        std::cout << "Official encode oracle written to " << oracle_path << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Official encode oracle failed: " << ex.what() << '\n';
        return 1;
    }
}
