#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "encode_conformance_shared.h"
#include "official_opus_abi.h"

namespace {

using namespace encode_conformance;

struct validation_options final {
  std::optional<std::size_t> max_frame_index;
  std::string_view clip_filter;
  std::string_view scenario_filter;
};

[[nodiscard]] auto parse_int_argument(std::string_view value, std::string_view name) -> int {
  auto parsed = 0;
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto [ptr, error] = std::from_chars(begin, end, parsed);
  if (error != std::errc{} || ptr != end) {
    throw std::runtime_error("invalid " + std::string{name} + ": " + std::string{value});
  }
  return parsed;
}

[[nodiscard]] auto parse_optional_value(std::string_view argument, std::string_view name, int& index, int argc, char** argv)
    -> std::string_view {
  if (argument == name) {
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value for " + std::string{name});
    }
    ++index;
    return argv[index];
  }

  const auto prefix = std::string{name} + "=";
  if (argument.starts_with(prefix)) {
    return argument.substr(prefix.size());
  }

  return {};
}

[[nodiscard]] auto parse_validation_options(int argc, char** argv, int first_index) -> validation_options {
  auto options = validation_options{};
  for (int index = first_index; index < argc; ++index) {
    const auto argument = std::string_view{argv[index]};
    if (const auto value = parse_optional_value(argument, "--max-frame-index", index, argc, argv); !value.empty()) {
      const auto parsed = parse_int_argument(value, "max frame index");
      if (parsed < 0) {
        throw std::runtime_error("max frame index must be non-negative");
      }
      options.max_frame_index = static_cast<std::size_t>(parsed);
    } else if (const auto value = parse_optional_value(argument, "--clip", index, argc, argv); !value.empty()) {
      options.clip_filter = value;
    } else if (const auto value = parse_optional_value(argument, "--clip-filter", index, argc, argv); !value.empty()) {
      options.clip_filter = value;
    } else if (const auto value = parse_optional_value(argument, "--scenario", index, argc, argv); !value.empty()) {
      options.scenario_filter = value;
    } else if (const auto value = parse_optional_value(argument, "--scenario-filter", index, argc, argv); !value.empty()) {
      options.scenario_filter = value;
    } else {
      throw std::runtime_error("unknown option: " + std::string{argument});
    }
  }
  return options;
}

[[nodiscard]] auto make_encoder(int channels, const scenario& current) -> std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> {
  int error = OPUS_OK;
  auto* encoder = opus_encoder_create(48000, channels, current.application, &error);
  if (error != OPUS_OK || encoder == nullptr) {
    throw std::runtime_error("failed to create official encoder");
  }
  return {encoder, &opus_encoder_destroy};
}

void configure_encoder(OpusEncoder* encoder, int channels, const scenario& current) {
  if (const auto ret = opus_encoder_ctl(encoder, OPUS_SET_BITRATE_REQUEST, scenario_bitrate(current, channels)); ret != OPUS_OK) {
    throw std::runtime_error("failed to set official bitrate");
  }
  if (const auto ret = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY_REQUEST, current.complexity); ret != OPUS_OK) {
    throw std::runtime_error("failed to set official complexity");
  }
  if (const auto ret = opus_encoder_ctl(encoder, OPUS_SET_VBR_REQUEST, static_cast<int>(current.vbr)); ret != OPUS_OK) {
    throw std::runtime_error("failed to set official VBR");
  }
}

[[nodiscard]] auto encode_case(const clip_input& clip, const scenario& current, const validation_options& options) -> case_record {
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
    if (options.max_frame_index.has_value() && frame_index > *options.max_frame_index) {
      break;
    }
    const auto frame_offset = frame_index * samples_per_frame;
    std::array<unsigned char, 1500> packet{};
    const int packet_size =
        opus_encode(encoder.get(), samples.data() + frame_offset, current.frame_size, packet.data(), static_cast<int>(packet.size()));
    if (packet_size < 0) {
      throw std::runtime_error("official encoder returned an error");
    }

    std::uint32_t final_range = 0;
    if (const auto ret = opus_encoder_ctl(encoder.get(), OPUS_GET_FINAL_RANGE_REQUEST, &final_range); ret != OPUS_OK) {
      throw std::runtime_error("failed to read official final range");
    }

    result.frames.push_back(frame_record{.final_range = final_range, .packet = {packet.begin(), packet.begin() + packet_size}});
  }

  std::cout << "VALIDATION clip=" << clip.label << " scenario=" << current.name << " frames=" << frame_count << '\n';
  return result;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto vector_path = argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"opus_newvectors"};
    const auto validation_path = argc > 2 ? std::filesystem::path{argv[2]} : std::filesystem::path{"encode_validation.bin"};
    const auto options = parse_validation_options(argc, argv, 3);

    const auto clips = discover_clips(vector_path);
    if (clips.empty()) {
      std::cerr << "No .dec inputs found in " << vector_path << '\n';
      return 1;
    }

    auto cases = std::vector<case_record>{};
    cases.reserve(clips.size() * scenarios.size());
    for (const auto& clip : clips) {
      if (!options.clip_filter.empty() && clip.label != options.clip_filter) {
        continue;
      }
      for (const auto& current : scenarios) {
        if (!options.scenario_filter.empty() && current.name != options.scenario_filter) {
          continue;
        }
        cases.push_back(encode_case(clip, current, options));
      }
    }

    write_validation(validation_path, cases);
    std::cout << "Official encode validation written to " << validation_path << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Official encode validation failed: " << ex.what() << '\n';
    return 1;
  }
}
