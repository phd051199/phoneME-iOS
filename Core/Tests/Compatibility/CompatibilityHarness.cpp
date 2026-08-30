#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/resource.h>
#endif

#include "phoneme/base/Error.hpp"
#include "phoneme/base/Types.hpp"
#include "phoneme/runtime/Runtime.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options final {
    std::string jar;
    std::string main_class;
    std::string runtime_home;
    std::string result_path;
    std::string frame_path;
    std::string native_coverage_path;
    phoneme::i32 width {240};
    phoneme::i32 height {320};
    phoneme::i32 observe_ms {0};
    phoneme::i32 confirm_interval_ms {0};
    phoneme::i32 confirm_count {0};
    bool autoplay {false};
    phoneme::i32 input_start_delay_ms {400};
    phoneme::i32 input_interval_ms {220};
    phoneme::i32 stall_ms {0};
    phoneme::i32 heartbeat_ms {1'000};
    bool skip_teardown {false};
};

struct HarnessResult final {
    bool configured {false};
    bool installed {false};
    bool system_started {false};
    bool midlet_started {false};
    bool destroyed {false};
    std::string install {"failure"};
    std::string app_state {"unknown"};
    phoneme::i32 exit_code {1};
    phoneme::i64 startup_ms {0};
    phoneme::usize frames_produced {0};
    phoneme::usize frame_changes {0};
    phoneme::usize unique_frames {0};
    phoneme::usize nonzero_frame_bytes {0};
    phoneme::usize ui_event_count {0};
    phoneme::usize canvas_event_count {0};
    phoneme::usize lcdui_event_count {0};
    phoneme::usize screens_shown {0};
    phoneme::usize key_presses {0};
    phoneme::usize key_events_sent {0};
    phoneme::usize lcdui_actions_sent {0};
    phoneme::usize heartbeat_count {0};
    phoneme::i64 last_progress_ms {0};
    phoneme::i64 longest_idle_ms {0};
    double process_cpu_ms {0.0};
    double wall_time_ms {0.0};
    double cpu_utilization_pct {0.0};
    double cpu_ms_per_generated_frame {0.0};
    double frame_interval_p50_ms {0.0};
    double frame_interval_p95_ms {0.0};
    double frame_interval_p99_ms {0.0};
    phoneme::usize frame_hitches_over_50ms {0};
    phoneme::usize frame_interval_samples {0};
    bool visual_output_observed {false};
    bool stall_suspected {false};
    phoneme::i32 error_code {0};
    std::string error_message;
    std::string java_exception_class;
    std::string final_frame_hash;
    std::string last_input_action;
    std::vector<std::string> milestones;
    std::vector<std::string> input_actions;
    std::vector<std::string> frame_hashes;
    std::vector<std::string> network_actions;
    std::vector<std::string> media_actions;
};

[[nodiscard]] phoneme::u64 process_cpu_nanoseconds() noexcept {
#if defined(__APPLE__) || defined(__unix__)
    rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0U;
    const auto timeval_ns = [](const timeval& value) noexcept -> phoneme::u64 {
        return static_cast<phoneme::u64>(value.tv_sec) * 1'000'000'000ULL +
               static_cast<phoneme::u64>(value.tv_usec) * 1'000ULL;
    };
    return timeval_ns(usage.ru_utime) + timeval_ns(usage.ru_stime);
#else
    return 0U;
#endif
}

[[nodiscard]] double percentile_ms(std::vector<phoneme::u64> samples_ns,
                                   double percentile) {
    if (samples_ns.empty()) return 0.0;
    std::sort(samples_ns.begin(), samples_ns.end());
    const double clamped = std::clamp(percentile, 0.0, 1.0);
    const auto last = static_cast<double>(samples_ns.size() - 1U);
    const auto index = static_cast<phoneme::usize>(clamped * last + 0.5);
    return static_cast<double>(samples_ns[index]) / 1.0e6;
}

struct CommandCandidate final {
    phoneme::i32 id {0};
    phoneme::i32 type {0};
    phoneme::i32 priority {0};
    std::string label;
};

struct ChoiceCandidate final {
    phoneme::i32 component_id {0};
    phoneme::i32 component_type {0};
    phoneme::i32 index {0};
    std::string label;
};

struct AutoplayState final {
    std::vector<CommandCandidate> commands;
    std::vector<phoneme::i32> selected_command_ids;
    std::vector<ChoiceCandidate> choices;
    std::vector<std::pair<phoneme::i32, phoneme::i32>> selected_choices;
    std::vector<phoneme::i32> activatable_items;
    std::vector<phoneme::i32> activated_items;
    phoneme::usize key_index {0};
    phoneme::i32 active_key {0};
    bool key_down {false};
    Clock::time_point release_at {};
    // Optional pointer tap script parsed from
    // PHONEME_HARNESS_POINTER_TAPS="delay_ms:x,y;delay_ms:x,y;..."
    struct Tap final {
        phoneme::i32 delay_ms {0};
        phoneme::i32 x {0};
        phoneme::i32 y {0};
    };
    std::vector<Tap> taps;
    phoneme::usize tap_index {0};
    Clock::time_point next_action_at {};
};

[[nodiscard]] std::optional<phoneme::i32> parse_i32(std::string_view value) {
    phoneme::i32 parsed = 0;
    const auto result = std::from_chars(value.data(),
                                        value.data() + value.size(),
                                        parsed);
    if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

void parse_pointer_taps(AutoplayState& state) {
    const char* script = std::getenv("PHONEME_HARNESS_POINTER_TAPS");
    if (script == nullptr || script[0] == '\0') return;
    std::string_view text {script};
    while (!text.empty()) {
        const auto entry = text.substr(0, std::min(text.find(';'), text.size()));
        text = text.size() > entry.size() ? text.substr(entry.size() + 1) :
                                           std::string_view {};
        // entry = delay_ms:x,y
        const auto delay_end = entry.find(':');
        const auto coord = delay_end == std::string_view::npos ?
            std::string_view {} : entry.substr(delay_end + 1);
        const auto comma = coord.find(',');
        if (delay_end == std::string_view::npos || comma == std::string_view::npos) {
            continue;
        }
        const auto delay = parse_i32(entry.substr(0, delay_end));
        const auto x = parse_i32(coord.substr(0, comma));
        const auto y = parse_i32(coord.substr(comma + 1));
        if (delay && x && y) {
            state.taps.push_back({*delay, *x, *y});
        }
    }
}

[[nodiscard]] bool parse_options(int argc,
                                 char** argv,
                                 Options& options,
                                 std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument {argv[index]};
        if (argument == "--help") {
            error = "usage: CompatibilityHarness --jar FILE --main CLASS "
                    "--runtime-home DIR --result FILE --frame FILE "
                    "[--native-coverage FILE --width N --height N "
                    "--observe-ms N --confirm-interval-ms N "
                    "--confirm-count N --autoplay 0|1 "
                    "--input-start-delay-ms N --input-interval-ms N "
                    "--stall-ms N --heartbeat-ms N --skip-teardown 0|1]";
            return false;
        }
        if (index + 1 >= argc) {
            error = "missing value for " + std::string(argument);
            return false;
        }
        const std::string value {argv[++index]};
        if (argument == "--jar") {
            options.jar = value;
        } else if (argument == "--main") {
            options.main_class = value;
        } else if (argument == "--runtime-home") {
            options.runtime_home = value;
        } else if (argument == "--result") {
            options.result_path = value;
        } else if (argument == "--frame") {
            options.frame_path = value;
        } else if (argument == "--native-coverage") {
            options.native_coverage_path = value;
        } else if (argument == "--width") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed <= 0 || *parsed > 8192) {
                error = "invalid width: " + value;
                return false;
            }
            options.width = *parsed;
        } else if (argument == "--height") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed <= 0 || *parsed > 8192) {
                error = "invalid height: " + value;
                return false;
            }
            options.height = *parsed;
        } else if (argument == "--observe-ms") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 0 || *parsed > 120'000) {
                error = "invalid observe duration: " + value;
                return false;
            }
            options.observe_ms = *parsed;
        } else if (argument == "--confirm-interval-ms") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 0 || *parsed > 60'000) {
                error = "invalid confirm interval: " + value;
                return false;
            }
            options.confirm_interval_ms = *parsed;
        } else if (argument == "--confirm-count") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 0 || *parsed > 100) {
                error = "invalid confirm count: " + value;
                return false;
            }
            options.confirm_count = *parsed;
        } else if (argument == "--autoplay") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || (*parsed != 0 && *parsed != 1)) {
                error = "invalid autoplay value: " + value;
                return false;
            }
            options.autoplay = *parsed != 0;
        } else if (argument == "--input-start-delay-ms") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 0 || *parsed > 60'000) {
                error = "invalid input start delay: " + value;
                return false;
            }
            options.input_start_delay_ms = *parsed;
        } else if (argument == "--input-interval-ms") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 40 || *parsed > 60'000) {
                error = "invalid input interval: " + value;
                return false;
            }
            options.input_interval_ms = *parsed;
        } else if (argument == "--stall-ms") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 0 || *parsed > 120'000) {
                error = "invalid stall duration: " + value;
                return false;
            }
            options.stall_ms = *parsed;
        } else if (argument == "--heartbeat-ms") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || *parsed < 100 || *parsed > 60'000) {
                error = "invalid heartbeat interval: " + value;
                return false;
            }
            options.heartbeat_ms = *parsed;
        } else if (argument == "--skip-teardown") {
            const auto parsed = parse_i32(value);
            if (!parsed.has_value() || (*parsed != 0 && *parsed != 1)) {
                error = "invalid skip-teardown value: " + value;
                return false;
            }
            options.skip_teardown = *parsed != 0;
        } else {
            error = "unknown argument: " + std::string(argument);
            return false;
        }
    }

    if (options.jar.empty() || options.main_class.empty() ||
        options.runtime_home.empty() || options.result_path.empty() ||
        options.frame_path.empty()) {
        error = "--jar, --main, --runtime-home, --result and --frame are required";
        return false;
    }
    return true;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8U);
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character < 0x20U) {
                static constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(kHex[(character >> 4U) & 0x0FU]);
                escaped.push_back(kHex[character & 0x0FU]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

void add_milestone(HarnessResult& result, std::string milestone) {
    if (std::find(result.milestones.begin(),
                  result.milestones.end(),
                  milestone) == result.milestones.end()) {
        result.milestones.push_back(std::move(milestone));
    }
}

[[nodiscard]] std::string app_state_name(phoneme::runtime::AppState state) {
    switch (state) {
    case phoneme::runtime::AppState::none: return "none";
    case phoneme::runtime::AppState::active: return "active";
    case phoneme::runtime::AppState::paused: return "paused";
    case phoneme::runtime::AppState::destroyed: return "destroyed";
    case phoneme::runtime::AppState::error: return "error";
    }
    return "unknown";
}

void capture_error(HarnessResult& result, const phoneme::Error& error) {
    result.error_code = static_cast<phoneme::i32>(error.code);
    result.error_message = error.message;
    result.java_exception_class = error.java_exception_class;
}

void emit_string_array(std::ostream& output,
                       std::string_view name,
                       std::span<const std::string> values,
                       bool trailing_comma) {
    output << "  \"" << name << "\": [";
    for (phoneme::usize index = 0; index < values.size(); ++index) {
        if (index != 0U) output << ", ";
        output << '"' << json_escape(values[index]) << '"';
    }
    output << ']';
    if (trailing_comma) output << ',';
    output << '\n';
}

[[nodiscard]] bool write_result(const Options& options,
                                const HarnessResult& result) {
    std::error_code directory_error;
    const auto parent = std::filesystem::path(options.result_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            std::cerr << "cannot create result directory: "
                      << directory_error.message() << '\n';
            return false;
        }
    }

    std::ofstream output(options.result_path,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot open result file: " << options.result_path << '\n';
        return false;
    }
    output << "{\n"
           << "  \"configured\": " << (result.configured ? "true" : "false") << ",\n"
           << "  \"installed\": " << (result.installed ? "true" : "false") << ",\n"
           << "  \"system_started\": " << (result.system_started ? "true" : "false") << ",\n"
           << "  \"midlet_started\": " << (result.midlet_started ? "true" : "false") << ",\n"
           << "  \"destroyed\": " << (result.destroyed ? "true" : "false") << ",\n"
           << "  \"install\": \"" << json_escape(result.install) << "\",\n"
           << "  \"app_state\": \"" << json_escape(result.app_state) << "\",\n"
           << "  \"exit_code\": " << result.exit_code << ",\n"
           << "  \"startup_ms\": " << result.startup_ms << ",\n"
           << "  \"frames_produced\": " << result.frames_produced << ",\n"
           << "  \"frame_changes\": " << result.frame_changes << ",\n"
           << "  \"unique_frames\": " << result.unique_frames << ",\n"
           << "  \"nonzero_frame_bytes\": " << result.nonzero_frame_bytes << ",\n"
           << "  \"ui_event_count\": " << result.ui_event_count << ",\n"
           << "  \"canvas_event_count\": " << result.canvas_event_count << ",\n"
           << "  \"lcdui_event_count\": " << result.lcdui_event_count << ",\n"
           << "  \"screens_shown\": " << result.screens_shown << ",\n"
           << "  \"key_presses\": " << result.key_presses << ",\n"
           << "  \"key_events_sent\": " << result.key_events_sent << ",\n"
           << "  \"lcdui_actions_sent\": " << result.lcdui_actions_sent << ",\n"
           << "  \"heartbeat_count\": " << result.heartbeat_count << ",\n"
           << "  \"last_progress_ms\": " << result.last_progress_ms << ",\n"
           << "  \"longest_idle_ms\": " << result.longest_idle_ms << ",\n"
           << "  \"process_cpu_ms\": " << result.process_cpu_ms << ",\n"
           << "  \"wall_time_ms\": " << result.wall_time_ms << ",\n"
           << "  \"cpu_utilization_pct\": "
           << result.cpu_utilization_pct << ",\n"
           << "  \"cpu_ms_per_generated_frame\": "
           << result.cpu_ms_per_generated_frame << ",\n"
           << "  \"frame_interval_p50_ms\": "
           << result.frame_interval_p50_ms << ",\n"
           << "  \"frame_interval_p95_ms\": "
           << result.frame_interval_p95_ms << ",\n"
           << "  \"frame_interval_p99_ms\": "
           << result.frame_interval_p99_ms << ",\n"
           << "  \"frame_hitches_over_50ms\": "
           << result.frame_hitches_over_50ms << ",\n"
           << "  \"frame_interval_samples\": "
           << result.frame_interval_samples << ",\n"
           << "  \"visual_output_observed\": "
           << (result.visual_output_observed ? "true" : "false") << ",\n"
           << "  \"stall_suspected\": "
           << (result.stall_suspected ? "true" : "false") << ",\n"
           << "  \"final_frame_hash\": \""
           << json_escape(result.final_frame_hash) << "\",\n"
           << "  \"last_input_action\": \""
           << json_escape(result.last_input_action) << "\",\n"
           << "  \"error_code\": " << result.error_code << ",\n"
           << "  \"error_message\": \"" << json_escape(result.error_message) << "\",\n"
           << "  \"java_exception_class\": \""
           << json_escape(result.java_exception_class) << "\",\n";
    emit_string_array(output, "milestones", result.milestones, true);
    emit_string_array(output, "input_actions", result.input_actions, true);
    emit_string_array(output, "frame_hashes", result.frame_hashes, true);
    emit_string_array(output, "network_actions", result.network_actions, true);
    emit_string_array(output, "media_actions", result.media_actions, false);
    output << "}\n";
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_native_coverage(
    const Options& options,
    std::span<const phoneme::vm::NativeMethodInvocationCount> counts) {
    if (options.native_coverage_path.empty()) return true;
    std::error_code directory_error;
    const auto parent = std::filesystem::path(
        options.native_coverage_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            std::cerr << "cannot create native coverage directory: "
                      << directory_error.message() << '\n';
            return false;
        }
    }
    std::ofstream output(options.native_coverage_path,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot open native coverage file: "
                  << options.native_coverage_path << '\n';
        return false;
    }
    output << "owner\tname\tdescriptor\tinvocations\n";
    for (const auto& entry : counts) {
        output << entry.signature.owner << '\t'
               << entry.signature.name << '\t'
               << entry.signature.descriptor << '\t'
               << entry.count << '\n';
    }
    return static_cast<bool>(output);
}

[[nodiscard]] bool write_ppm(const Options& options,
                             const phoneme::runtime::FrameSnapshot& frame,
                             HarnessResult& result) {
    if (frame.dimensions.width <= 0 || frame.dimensions.height <= 0 ||
        frame.rgba.empty()) {
        return false;
    }
    const auto pixel_count =
        static_cast<phoneme::usize>(frame.dimensions.width) *
        static_cast<phoneme::usize>(frame.dimensions.height);
    if (frame.rgba.size() != pixel_count * 4U) {
        return false;
    }

    std::error_code directory_error;
    const auto parent = std::filesystem::path(options.frame_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) return false;
    }
    std::ofstream output(options.frame_path,
                         std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "P6\n" << frame.dimensions.width << ' '
           << frame.dimensions.height << "\n255\n";
    for (phoneme::usize offset = 0; offset < frame.rgba.size(); offset += 4U) {
        const char rgb[] {
            static_cast<char>(frame.rgba[offset]),
            static_cast<char>(frame.rgba[offset + 1U]),
            static_cast<char>(frame.rgba[offset + 2U]),
        };
        output.write(rgb, static_cast<std::streamsize>(sizeof(rgb)));
    }
    if (!output) return false;
    result.frames_produced = std::max<phoneme::usize>(
        result.frames_produced, 1U);
    const auto nonzero = static_cast<phoneme::usize>(std::count_if(
        frame.rgba.begin(), frame.rgba.end(), [](phoneme::u8 value) {
            return value != 0U;
        }));
    result.nonzero_frame_bytes = std::max(result.nonzero_frame_bytes, nonzero);
    add_milestone(result, "frame-produced");
    if (nonzero != 0U) {
        result.visual_output_observed = true;
        add_milestone(result, "frame-nonblank");
    }
    return true;
}

[[nodiscard]] std::string milestone_component(std::string_view value) {
    std::string component;
    component.reserve(std::min<phoneme::usize>(value.size(), 64U));
    bool pending_separator = false;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (std::isalnum(character) != 0) {
            if (pending_separator && !component.empty()) component.push_back('-');
            component.push_back(static_cast<char>(std::tolower(character)));
            pending_separator = false;
        } else if (!component.empty()) {
            pending_separator = true;
        }
        if (component.size() >= 64U) break;
    }
    return component;
}

[[nodiscard]] std::string hex_u64(phoneme::u64 value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(16U, '0');
    for (phoneme::usize index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1U) * 4U);
        output[index] = kHex[(value >> shift) & 0x0FU];
    }
    return output;
}

[[nodiscard]] phoneme::u64 frame_fingerprint(
    const phoneme::runtime::FrameSnapshot& frame) {
    phoneme::u64 hash = 1469598103934665603ULL;
    const auto mix = [&hash](phoneme::u8 value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const auto value : frame.rgba) mix(value);
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        mix(static_cast<phoneme::u8>(
            (static_cast<phoneme::u32>(frame.dimensions.width) >> shift) & 0xFFU));
        mix(static_cast<phoneme::u8>(
            (static_cast<phoneme::u32>(frame.dimensions.height) >> shift) & 0xFFU));
    }
    return hash;
}

[[nodiscard]] bool observe_frame(
    const phoneme::runtime::FrameSnapshot& frame,
    HarnessResult& result,
    phoneme::u64& last_hash,
    bool& have_hash) {
    if (frame.dimensions.width <= 0 || frame.dimensions.height <= 0 ||
        frame.rgba.empty()) {
        return false;
    }
    const auto hash = frame_fingerprint(frame);
    const auto encoded = hex_u64(hash);
    result.final_frame_hash = encoded;
    if (std::find(result.frame_hashes.begin(), result.frame_hashes.end(), encoded) ==
        result.frame_hashes.end()) {
        ++result.unique_frames;
        if (result.frame_hashes.size() < 64U) result.frame_hashes.push_back(encoded);
    }
    const bool changed = have_hash && hash != last_hash;
    if (changed) ++result.frame_changes;
    last_hash = hash;
    have_hash = true;
    const auto nonzero = static_cast<phoneme::usize>(std::count_if(
        frame.rgba.begin(), frame.rgba.end(), [](phoneme::u8 value) {
            return value != 0U;
        }));
    result.nonzero_frame_bytes = std::max(result.nonzero_frame_bytes, nonzero);
    if (nonzero != 0U) {
        result.visual_output_observed = true;
        add_milestone(result, "frame-nonblank");
    }
    return changed;
}

void record_input_action(HarnessResult& result, std::string action) {
    result.last_input_action = action;
    if (result.input_actions.size() < 128U) {
        result.input_actions.push_back(std::move(action));
    }
}

[[nodiscard]] bool contains_id(std::span<const phoneme::i32> values,
                               phoneme::i32 value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return value;
}

[[nodiscard]] phoneme::i32 command_score(const CommandCandidate& candidate) {
    phoneme::i32 score = -candidate.priority;
    switch (candidate.type) {
    case 4: score += 140; break; // OK
    case 8: score += 130; break; // ITEM
    case 1: score += 110; break; // SCREEN
    case 5: score += 20; break;  // HELP
    case 2: // BACK
    case 3: // CANCEL
    case 6: // STOP
    case 7: // EXIT
        score -= 240;
        break;
    default: break;
    }
    const auto label = lower_ascii(candidate.label);
    static constexpr std::array<std::string_view, 20> kPositive {
        "start", "play", "continue", "next", "select", "enter",
        "login", "new game", "ok", "yes", "resume", "begin",
        "chơi", "bắt đầu", "tiếp tục", "vào game",
        "chạy ứng dụng", "đồng ý", "chọn", "mở",
    };
    static constexpr std::array<std::string_view, 26> kNegative {
        "exit", "quit", "back", "cancel", "stop",
        "no", "close", "about", "help", "options",
        "thoát", "dừng", "quay lại", "hủy", "xóa",
        "sao lưu", "cài đặt", "trợ giúp", "giới thiệu",
        "nhập khẩu", "khôi phục", "phục hồi", "tùy chọn",
        "thu nhỏ", "độ sáng", "lưu",
    };
    for (const auto token : kPositive) {
        if (label.find(token) != std::string::npos) score += 80;
    }
    for (const auto token : kNegative) {
        if (label.find(token) != std::string::npos) score -= 120;
    }
    return score;
}

[[nodiscard]] const char* key_name(phoneme::i32 key) noexcept {
    switch (key) {
    case -1: return "up";
    case -2: return "down";
    case -3: return "left";
    case -4: return "right";
    case -5: return "fire";
    case -6: return "soft-left";
    case -7: return "soft-right";
    default: return "numeric";
    }
}

[[nodiscard]] bool run_lcdui_action(phoneme::runtime::Runtime& runtime,
                                    AutoplayState& state,
                                    HarnessResult& result) {
    for (const auto& choice : state.choices) {
        // Only an IMPLICIT List (bridge type 2) represents a menu row whose
        // selection dispatches SELECT_COMMAND. Do not mutate ChoiceGroups in
        // forms because those are usually settings rather than navigation.
        if (choice.component_type != 2) continue;
        const auto key = std::pair {choice.component_id, choice.index};
        if (std::find(state.selected_choices.begin(),
                      state.selected_choices.end(), key) !=
            state.selected_choices.end()) {
            continue;
        }
        state.selected_choices.push_back(key);
        runtime.ui_set_choice(choice.component_id, choice.index, true);
        ++result.lcdui_actions_sent;
        record_input_action(result,
                            "lcdui-choice:" + std::to_string(choice.component_id) +
                                ":" + std::to_string(choice.index) + ":" +
                                choice.label);
        add_milestone(result, "autoplay-lcdui-choice");
        return true;
    }

    const CommandCandidate* best = nullptr;
    phoneme::i32 best_score = 0;
    for (const auto& command : state.commands) {
        if (contains_id(state.selected_command_ids, command.id)) continue;
        const auto score = command_score(command);
        if (best == nullptr || score > best_score) {
            best = &command;
            best_score = score;
        }
    }
    if (best != nullptr && best_score > 0) {
        state.selected_command_ids.push_back(best->id);
        runtime.ui_select_command(best->id);
        ++result.lcdui_actions_sent;
        record_input_action(result,
                            "lcdui-command:" + std::to_string(best->id) + ":" +
                                best->label);
        add_milestone(result, "autoplay-lcdui-command");
        return true;
    }

    for (const auto component_id : state.activatable_items) {
        if (contains_id(state.activated_items, component_id)) continue;
        state.activated_items.push_back(component_id);
        runtime.ui_activate_item(component_id);
        ++result.lcdui_actions_sent;
        record_input_action(result,
                            "lcdui-activate:" + std::to_string(component_id));
        add_milestone(result, "autoplay-lcdui-activate");
        return true;
    }
    return false;
}

void autoplay_tick(phoneme::runtime::Runtime& runtime,
                   const Options& options,
                   AutoplayState& state,
                   HarnessResult& result,
                   Clock::time_point now) {
    if (state.key_down && now >= state.release_at) {
        runtime.send_key(state.active_key, false);
        ++result.key_events_sent;
        record_input_action(result,
                            std::string("key-release:") + key_name(state.active_key) +
                                ":" + std::to_string(state.active_key));
        state.key_down = false;
    }
    if (state.key_down || now < state.next_action_at) return;

    if (!state.taps.empty()) {
        if (state.tap_index >= state.taps.size()) return;
        const auto tap = state.taps[state.tap_index++];
        state.next_action_at = now +
            std::chrono::milliseconds(options.input_interval_ms);
        runtime.send_pointer(tap.x, tap.y, 1);
        runtime.send_pointer(tap.x, tap.y, 2);
        result.key_events_sent += 2;
        record_input_action(result,
                            "pointer-tap:" + std::to_string(tap.x) + "," +
                                std::to_string(tap.y) + " (after " +
                                std::to_string(tap.delay_ms) + "ms)");
        add_milestone(result, "autoplay-pointer");
        // honor per-tap delay before the next tap
        state.next_action_at += std::chrono::milliseconds(tap.delay_ms);
        return;
    }

    if (run_lcdui_action(runtime, state, result)) {
        state.next_action_at = now +
            std::chrono::milliseconds(options.input_interval_ms);
        return;
    }

    static constexpr std::array<phoneme::i32, 28> kGameKeys {
        -5, -6, -5, '5', -2, -5, -4, -5, -2, -2, -5, -3, -5, -1,
        -5, '5', '0', '1', '3', '7', '9', '*', '#', -6, -5, -4, -5, -7,
    };
    if (const char* configured_limit =
            std::getenv("PHONEME_HARNESS_AUTOPLAY_KEY_LIMIT");
        configured_limit != nullptr && configured_limit[0] != '\0') {
        char* end = nullptr;
        const unsigned long limit = std::strtoul(configured_limit, &end, 10);
        if (end != configured_limit && *end == '\0' &&
            state.key_index >= static_cast<phoneme::usize>(limit)) {
            return;
        }
    }
    const auto key = kGameKeys[state.key_index % kGameKeys.size()];
    ++state.key_index;
    state.active_key = key;
    state.key_down = true;
    state.release_at = now + std::chrono::milliseconds(35);
    state.next_action_at = now +
        std::chrono::milliseconds(options.input_interval_ms);
    runtime.send_key(key, true);
    ++result.key_presses;
    ++result.key_events_sent;
    record_input_action(result,
                        std::string("key-press:") + key_name(key) + ":" +
                            std::to_string(key));
    add_milestone(result, "autoplay-key");
}

[[nodiscard]] phoneme::usize collect_ui_events(
    phoneme::runtime::Runtime& runtime,
    HarnessResult& result,
    AutoplayState* autoplay = nullptr) {
    phoneme::usize collected = 0;
    while (auto event = runtime.poll_ui_event()) {
        ++collected;
        ++result.ui_event_count;
        add_milestone(result, "ui-event");
        const auto text_component = milestone_component(event->text);
        if (!text_component.empty()) {
            add_milestone(result, "ui-text:" + text_component);
        }
        const auto detail_component = milestone_component(event->detail);
        if (!detail_component.empty()) {
            add_milestone(result, "ui-detail:" + detail_component);
        }
        if (event->component_type == 22) {
            ++result.canvas_event_count;
            add_milestone(result, "canvas-event");
            if (event->kind == 2) add_milestone(result, "canvas-created");
            if (event->kind == 4) add_milestone(result, "canvas-shown");
            if (event->kind == 3 && event->text.starts_with("paint:")) {
                add_milestone(result, "canvas-painted");
            }
        } else {
            ++result.lcdui_event_count;
            add_milestone(result, "lcdui-event");
            if (event->kind == 2) add_milestone(result, "lcdui-component-created");
            if (event->kind == 4) {
                ++result.screens_shown;
                result.visual_output_observed = true;
                add_milestone(result, "lcdui-screen-shown");
            }
        }

        if (autoplay == nullptr) continue;
        if (event->kind == 14) {
            // Reset only what is currently visible. Keep action history across
            // screens so a stable Command object cannot trap autoplay in a
            // Start -> settings -> Start loop.
            autoplay->commands.clear();
            autoplay->choices.clear();
            autoplay->activatable_items.clear();
        } else if (event->kind == 15 && event->component_id != 0) {
            const auto duplicate = std::find_if(
                autoplay->commands.begin(), autoplay->commands.end(),
                [&](const CommandCandidate& command) {
                    return command.id == event->component_id;
                });
            if (duplicate == autoplay->commands.end()) {
                autoplay->commands.push_back(CommandCandidate {
                    .id = event->component_id,
                    .type = event->arguments[0],
                    .priority = event->arguments[1],
                    .label = event->text,
                });
            }
        } else if (event->kind == 12 && event->component_id != 0 &&
                   event->index >= 0) {
            const auto key = std::pair {event->component_id, event->index};
            const auto duplicate = std::find_if(
                autoplay->choices.begin(), autoplay->choices.end(),
                [&](const ChoiceCandidate& choice) {
                    return std::pair {choice.component_id, choice.index} == key;
                });
            if (duplicate == autoplay->choices.end()) {
                autoplay->choices.push_back(ChoiceCandidate {
                    .component_id = event->component_id,
                    .component_type = event->component_type,
                    .index = event->index,
                    .label = event->text,
                });
            }
        } else if ((event->kind == 7 || event->kind == 9) &&
                   event->component_id != 0 &&
                   (event->component_type == 9 || event->component_type == 10 ||
                    event->component_type == 13 || event->component_type == 14)) {
            if (!contains_id(autoplay->activatable_items, event->component_id)) {
                autoplay->activatable_items.push_back(event->component_id);
            }
        }
    }
    return collected;
}

[[nodiscard]] int run(const Options& options) {
    HarnessResult result;
    AutoplayState autoplay;
    parse_pointer_taps(autoplay);
    const auto started_at = Clock::now();
    const phoneme::u64 started_cpu_ns = process_cpu_nanoseconds();
    std::vector<phoneme::u64> frame_intervals_ns;
    std::optional<Clock::time_point> previous_generated_frame_at;
    auto last_progress_at = started_at;
    phoneme::u64 last_frame_hash = 0;
    bool have_frame_hash = false;
    phoneme::runtime::Runtime runtime;
    constexpr phoneme::AppId kAppId {17};

    const auto finish = [&](int code) {
        result.exit_code = static_cast<phoneme::i32>(code);
        const auto finished_at = Clock::now();
        const phoneme::u64 finished_cpu_ns = process_cpu_nanoseconds();
        const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished_at - started_at).count();
        const phoneme::u64 cpu_ns = finished_cpu_ns >= started_cpu_ns
            ? finished_cpu_ns - started_cpu_ns
            : 0U;
        result.process_cpu_ms = static_cast<double>(cpu_ns) / 1.0e6;
        result.wall_time_ms = wall_ns > 0
            ? static_cast<double>(wall_ns) / 1.0e6
            : 0.0;
        result.cpu_utilization_pct = wall_ns > 0
            ? (static_cast<double>(cpu_ns) / static_cast<double>(wall_ns)) * 100.0
            : 0.0;
        result.cpu_ms_per_generated_frame = result.frames_produced != 0U
            ? result.process_cpu_ms /
                  static_cast<double>(result.frames_produced)
            : 0.0;
        result.frame_interval_samples = frame_intervals_ns.size();
        result.frame_interval_p50_ms = percentile_ms(frame_intervals_ns, 0.50);
        result.frame_interval_p95_ms = percentile_ms(frame_intervals_ns, 0.95);
        result.frame_interval_p99_ms = percentile_ms(frame_intervals_ns, 0.99);
        result.frame_hitches_over_50ms = static_cast<phoneme::usize>(std::count_if(
            frame_intervals_ns.begin(), frame_intervals_ns.end(),
            [](phoneme::u64 value) { return value > 50'000'000ULL; }));
        const auto native_counts = runtime.app_native_invocation_counts(kAppId);
        if (!write_native_coverage(options, native_counts)) return 91;
        if (!write_result(options, result)) return 90;
        return code;
    };
    const auto mark_progress = [&](Clock::time_point now) {
        last_progress_at = now;
        result.last_progress_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - started_at).count();
    };

    auto configured = runtime.configure(options.runtime_home);
    if (!configured.has_value()) {
        capture_error(result, configured.error());
        return finish(10);
    }
    result.configured = true;
    add_milestone(result, "runtime-configured");

    const char* enable_jit = std::getenv("PHONEME_HARNESS_JIT");
    if (enable_jit != nullptr && enable_jit[0] != '\0' &&
        std::string_view(enable_jit) == "0") {
        auto jit = runtime.configure_jit(false);
        if (!jit.has_value()) {
            capture_error(result, jit.error());
            return finish(16);
        }
        add_milestone(result, "jit-disabled");
    }

    const char* enable_bing = std::getenv("PHONEME_HARNESS_BING_TRANSLATION");
    if (enable_bing != nullptr && enable_bing[0] != '\0' &&
        std::string_view(enable_bing) != "0") {
        auto translation = runtime.configure_translation(
            true,
            phoneme::translation::TranslationProvider::bing,
            "en",
            "vi");
        if (!translation.has_value()) {
            capture_error(result, translation.error());
            return finish(16);
        }
        add_milestone(result, "bing-translation-enabled");
    }

    auto suite_id = runtime.install_jar(options.jar);
    if (!suite_id.has_value()) {
        capture_error(result, suite_id.error());
        return finish(11);
    }
    result.installed = true;
    result.install = "success";
    add_milestone(result, "jar-installed");

    // The iOS host treats user-imported JARs as trusted compatibility-domain
    // suites before launch. Mirror that production path here so smoke tests
    // reach the MIDlet instead of failing on the first declared network API.
    auto trusted = runtime.set_suite_trust(
        *suite_id, phoneme::security::SuiteTrust::trusted);
    if (!trusted.has_value()) {
        capture_error(result, trusted.error());
        return finish(15);
    }
    add_milestone(result, "suite-trusted");

    auto system_started = runtime.start_system();
    if (!system_started.has_value()) {
        capture_error(result, system_started.error());
        return finish(12);
    }
    result.system_started = true;
    add_milestone(result, "system-started");

    auto midlet_started = runtime.start_midlet(
        *suite_id,
        options.main_class,
        kAppId,
        phoneme::Dimensions {options.width, options.height});
    result.startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started_at).count();
    if (!midlet_started.has_value()) {
        capture_error(result, midlet_started.error());
        result.app_state = app_state_name(runtime.app_state(kAppId));
        (void)collect_ui_events(
            runtime, result, options.autoplay ? &autoplay : nullptr);
        auto frame = runtime.frame_snapshot();
        (void)observe_frame(frame, result, last_frame_hash, have_frame_hash);
        if (!frame.rgba.empty()) (void)write_ppm(options, frame, result);
        const auto console_output = runtime.app_console_output(kAppId);
        if (!console_output.empty()) {
            std::cout << console_output;
            std::cout.flush();
        }
        return finish(13);
    }
    result.midlet_started = true;
    add_milestone(result, "midlet-started");
    result.app_state = app_state_name(runtime.app_state(kAppId));
    if (result.app_state == "active") add_milestone(result, "app-active");
    if (result.app_state == "paused") add_milestone(result, "app-paused");
    if (result.app_state == "destroyed") add_milestone(result, "app-self-destroyed");
    mark_progress(Clock::now());
    // Persist a diagnostic checkpoint so an external timeout can distinguish
    // a launch hang from teardown or frame-collection deadlock.
    (void)write_result(options, result);

    const auto initial_events = collect_ui_events(
        runtime, result, options.autoplay ? &autoplay : nullptr);
    if (initial_events != 0U) mark_progress(Clock::now());
    auto latest_frame = runtime.frame_snapshot();
    (void)observe_frame(latest_frame, result, last_frame_hash, have_frame_hash);
    if (!latest_frame.rgba.empty()) (void)write_ppm(options, latest_frame, result);

    if (options.observe_ms > 0 &&
        runtime.app_state(kAppId) != phoneme::runtime::AppState::destroyed) {
        add_milestone(result, "observation-begin");
        const auto observation_started_at = Clock::now();
        const auto deadline = observation_started_at +
            std::chrono::milliseconds(options.observe_ms);
        auto next_confirm = observation_started_at +
            std::chrono::milliseconds(options.confirm_interval_ms);
        auto next_heartbeat = observation_started_at +
            std::chrono::milliseconds(options.heartbeat_ms);
        autoplay.next_action_at = observation_started_at +
            std::chrono::milliseconds(options.input_start_delay_ms);
        phoneme::i32 confirms_sent = 0;
        phoneme::u64 last_generation = latest_frame.generation;
        auto previous_state = runtime.app_state(kAppId);
        while (Clock::now() < deadline &&
               runtime.app_state(kAppId) !=
                   phoneme::runtime::AppState::destroyed) {
            const auto now = Clock::now();
            if (options.autoplay) {
                autoplay_tick(runtime, options, autoplay, result, now);
            } else if (options.confirm_interval_ms > 0 &&
                       confirms_sent < options.confirm_count &&
                       now >= next_confirm) {
                runtime.send_key(-5, true);
                runtime.send_key(-5, false);
                result.key_presses += 1U;
                result.key_events_sent += 2U;
                record_input_action(result, "confirm-tap:fire:-5");
                ++confirms_sent;
                next_confirm += std::chrono::milliseconds(
                    options.confirm_interval_ms);
                add_milestone(result, "confirm-tap");
            }

            runtime.pump_events();
            const auto current_state = runtime.app_state(kAppId);
            if (current_state != previous_state) {
                previous_state = current_state;
                mark_progress(Clock::now());
            }
            const auto event_count = collect_ui_events(
                runtime, result, options.autoplay ? &autoplay : nullptr);
            if (event_count != 0U) mark_progress(Clock::now());

            auto frame = runtime.frame_snapshot();
            if (frame.generation != last_generation) {
                const auto frame_at = Clock::now();
                if (previous_generated_frame_at.has_value()) {
                    const auto interval =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            frame_at - *previous_generated_frame_at).count();
                    if (interval > 0) {
                        frame_intervals_ns.push_back(
                            static_cast<phoneme::u64>(interval));
                    }
                }
                previous_generated_frame_at = frame_at;
                last_generation = frame.generation;
                latest_frame = std::move(frame);
                ++result.frames_produced;
                (void)observe_frame(latest_frame, result,
                                    last_frame_hash, have_frame_hash);
                // A new framebuffer generation proves that the VM/render loop
                // is still making progress even when a static menu redraws the
                // same pixels. Pixel-hash changes remain a separate metric.
                mark_progress(Clock::now());
            }

            const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - last_progress_at).count();
            result.longest_idle_ms = std::max(result.longest_idle_ms, idle_ms);
            const auto actions_sent = result.key_presses + result.lcdui_actions_sent;
            result.stall_suspected =
                options.stall_ms > 0 && actions_sent >= 3U &&
                result.visual_output_observed && idle_ms >= options.stall_ms;

            if (current_state == phoneme::runtime::AppState::error) break;
            if (Clock::now() >= next_heartbeat) {
                ++result.heartbeat_count;
                result.app_state = app_state_name(current_state);
                (void)write_result(options, result);
                next_heartbeat += std::chrono::milliseconds(options.heartbeat_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (autoplay.key_down) {
            runtime.send_key(autoplay.active_key, false);
            ++result.key_events_sent;
            autoplay.key_down = false;
        }
        runtime.pump_events();
        const auto final_events = collect_ui_events(
            runtime, result, options.autoplay ? &autoplay : nullptr);
        if (final_events != 0U) mark_progress(Clock::now());
        auto final_frame = runtime.frame_snapshot();
        if (final_frame.generation != last_generation) {
            const auto frame_at = Clock::now();
            if (previous_generated_frame_at.has_value()) {
                const auto interval =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        frame_at - *previous_generated_frame_at).count();
                if (interval > 0) {
                    frame_intervals_ns.push_back(
                        static_cast<phoneme::u64>(interval));
                }
            }
            previous_generated_frame_at = frame_at;
            ++result.frames_produced;
            latest_frame = std::move(final_frame);
            (void)observe_frame(latest_frame, result,
                                last_frame_hash, have_frame_hash);
            mark_progress(Clock::now());
        }
        if (!latest_frame.rgba.empty()) {
            (void)write_ppm(options, latest_frame, result);
        }
        const auto final_idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - last_progress_at).count();
        result.longest_idle_ms = std::max(result.longest_idle_ms, final_idle_ms);
        const auto actions_sent = result.key_presses + result.lcdui_actions_sent;
        result.stall_suspected =
            options.stall_ms > 0 && actions_sent >= 3U &&
            result.visual_output_observed && final_idle_ms >= options.stall_ms;
        if (result.stall_suspected) add_milestone(result, "stall-suspected");
        add_milestone(result, "observation-end");
    }

    runtime.pump_events();
    result.app_state = app_state_name(runtime.app_state(kAppId));
    if (result.app_state == "error") {
        result.error_message = runtime.app_error_message(kAppId);
        add_milestone(result, "app-error");
    }
    const auto console_output = runtime.app_console_output(kAppId);
    if (!console_output.empty()) {
        std::cout << console_output;
        std::cout.flush();
    }

    if (result.app_state == "error") return finish(16);

    if (options.skip_teardown) {
        // The process is the isolation boundary for corpus integration tests.
        // Some MIDlets intentionally keep worker loops alive and can block
        // destroyApp/VM shutdown even though launch, rendering and input are
        // healthy. Persist the complete observation result, then terminate
        // without running Runtime's destructor so teardown cannot turn a
        // successful game smoke test into a timeout.
        add_milestone(result, "teardown-skipped");
        const int result_code = finish(0);
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(result_code);
    }

    if (runtime.app_state(kAppId) != phoneme::runtime::AppState::destroyed) {
        add_milestone(result, "destroy-begin");
        (void)write_result(options, result);
        auto destroyed = runtime.destroy_midlet(kAppId);
        if (!destroyed.has_value()) {
            capture_error(result, destroyed.error());
            return finish(14);
        }
        result.destroyed = true;
        add_milestone(result, "midlet-destroyed");
    } else {
        result.destroyed = true;
    }
    const int result_code = finish(0);
    runtime.stop();
    return result_code;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error)) {
        std::cerr << error << '\n';
        return 2;
    }
    return run(options);
}
