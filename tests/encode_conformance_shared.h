#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace encode_conformance {

namespace fs = std::filesystem;

struct scenario final {
    std::string_view name;
    int frame_size;
    int application;
    int mono_bitrate;
    int stereo_bitrate;
    int complexity;
    bool vbr;
};

struct clip_input final {
    fs::path path;
    std::string label;
    int channels;
};

struct frame_record final {
    std::uint32_t final_range{};
    std::vector<unsigned char> packet{};
};

struct case_record final {
    std::string clip_label;
    std::string scenario_name;
    int channels{};
    std::vector<frame_record> frames{};
};

consteval auto make_scenarios() noexcept {
    return std::array{
        scenario{"voip-20ms-vbr", 960, 2048, 20000, 40000, 10, true},
        scenario{"audio-20ms-cbr", 960, 2049, 32000, 64000, 9, false},
        scenario{"audio-10ms-vbr", 480, 2049, 24000, 48000, 6, true},
        scenario{"lowdelay-5ms-cbr", 240, 2051, 24000, 48000, 10, false},
    };
}

inline constexpr auto scenarios = make_scenarios();
inline constexpr std::uint32_t oracle_magic = 0x454E4346u; // ENCF
inline constexpr std::uint32_t oracle_version = 1u;

[[nodiscard]] constexpr auto scenario_bitrate(const scenario& value, int channels) noexcept -> int {
    return channels == 1 ? value.mono_bitrate : value.stereo_bitrate;
}

[[nodiscard]] constexpr auto find_scenario(std::string_view name) noexcept -> const scenario* {
    for (const auto& value : scenarios) {
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto load_pcm_samples(const fs::path& path) -> std::vector<std::int16_t> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open PCM input");
    }

    input.seekg(0, std::ios::end);
    const auto byte_count = input.tellg();
    input.seekg(0, std::ios::beg);
    if (byte_count < 0 || (byte_count % static_cast<std::streamoff>(sizeof(std::int16_t))) != 0) {
        throw std::runtime_error("PCM input has an invalid byte length");
    }

    std::vector<std::int16_t> samples(static_cast<std::size_t>(byte_count) / sizeof(std::int16_t));
    input.read(reinterpret_cast<char*>(samples.data()), byte_count);
    if (!input) {
        throw std::runtime_error("failed to read PCM input");
    }
    return samples;
}

[[nodiscard]] inline auto discover_clips(const fs::path& vector_path) -> std::vector<clip_input> {
    auto clips = std::vector<clip_input>{};
    for (const auto& entry : fs::directory_iterator(vector_path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".dec") {
            continue;
        }

        const auto file_name = entry.path().filename().string();
        const bool is_stereo = file_name.ends_with("m.dec");
        clips.push_back({entry.path(), file_name, is_stereo ? 2 : 1});
    }

    std::ranges::sort(clips, {}, &clip_input::label);
    return clips;
}

inline void write_u32(std::ostream& output, std::uint32_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw std::runtime_error("failed to write oracle data");
    }
}

inline void write_string(std::ostream& output, std::string_view value) {
    write_u32(output, static_cast<std::uint32_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) {
        throw std::runtime_error("failed to write oracle string");
    }
}

inline void write_blob(std::ostream& output, std::span<const unsigned char> blob) {
    write_u32(output, static_cast<std::uint32_t>(blob.size()));
    output.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (!output) {
        throw std::runtime_error("failed to write oracle packet");
    }
}

[[nodiscard]] inline auto read_u32(std::istream& input) -> std::uint32_t {
    std::uint32_t value = 0;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error("failed to read oracle data");
    }
    return value;
}

[[nodiscard]] inline auto read_string(std::istream& input) -> std::string {
    auto value = std::string(read_u32(input), '\0');
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!input) {
        throw std::runtime_error("failed to read oracle string");
    }
    return value;
}

[[nodiscard]] inline auto read_blob(std::istream& input) -> std::vector<unsigned char> {
    auto blob = std::vector<unsigned char>(read_u32(input));
    input.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    if (!input) {
        throw std::runtime_error("failed to read oracle packet");
    }
    return blob;
}

inline void write_oracle(const fs::path& output_path, std::span<const case_record> cases) {
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to open oracle output");
    }

    write_u32(output, oracle_magic);
    write_u32(output, oracle_version);
    write_u32(output, static_cast<std::uint32_t>(cases.size()));

    for (const auto& case_record : cases) {
        write_string(output, case_record.clip_label);
        write_string(output, case_record.scenario_name);
        write_u32(output, static_cast<std::uint32_t>(case_record.channels));
        write_u32(output, static_cast<std::uint32_t>(case_record.frames.size()));
        for (const auto& frame : case_record.frames) {
            write_u32(output, frame.final_range);
            write_blob(output, frame.packet);
        }
    }
}

[[nodiscard]] inline auto read_oracle(const fs::path& input_path) -> std::vector<case_record> {
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open oracle input");
    }

    if (read_u32(input) != oracle_magic) {
        throw std::runtime_error("invalid oracle magic");
    }
    if (read_u32(input) != oracle_version) {
        throw std::runtime_error("unsupported oracle version");
    }

    auto cases = std::vector<case_record>(read_u32(input));
    for (auto& value : cases) {
        value.clip_label = read_string(input);
        value.scenario_name = read_string(input);
        value.channels = static_cast<int>(read_u32(input));
        value.frames.resize(read_u32(input));
        for (auto& frame : value.frames) {
            frame.final_range = read_u32(input);
            frame.packet = read_blob(input);
        }
    }
    return cases;
}

} // namespace encode_conformance
