#include "xrfg/benchmark_model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace xrfg::benchmark {
namespace {

using Kind = CaseKind;
using Backend = CaseBackend;
using Preset = CasePreset;

constexpr Backend kFfx = Backend::fidelity_fx;
constexpr Backend kNv = Backend::nvidia;

// The order is the run order: what any GPU can run first, NVIDIA's optical
// flow next, NGX last.
constexpr std::array<CaseSpec, 37> kCases{{
    {"ffx_50", "FidelityFX optical flow, 50%", Kind::flow, kFfx, Preset::medium, 50},
    {"ffx_75", "FidelityFX optical flow, 75%", Kind::flow, kFfx, Preset::medium, 75},
    {"ffx_100", "FidelityFX optical flow, 100%", Kind::flow, kFfx, Preset::medium, 100},
    {"ffx_50_3x", "FidelityFX optical flow, 50%, 3X", Kind::flow, kFfx, Preset::medium, 50, false, 100, 2},
    {"vectors", "Game's motion vectors", Kind::vectors, kFfx, Preset::medium, 50},
    {"hybrid_50", "Motion vectors + FidelityFX flow, 50%", Kind::hybrid, kFfx, Preset::medium, 50},
    {"hybrid_75", "Motion vectors + FidelityFX flow, 75%", Kind::hybrid, kFfx, Preset::medium, 75},
    {"hybrid_100", "Motion vectors + FidelityFX flow, 100%", Kind::hybrid, kFfx, Preset::medium, 100},
    {"extrapolate", "Extrapolation from motion vectors", Kind::extrapolate, kFfx, Preset::medium, 50},
    {"extrapolate_ffx_50", "Extrapolation from FidelityFX optical flow, 50%", Kind::flow_extrapolate, kFfx, Preset::medium, 50},
    {"extrapolate_ffx_75", "Extrapolation from FidelityFX optical flow, 75%", Kind::flow_extrapolate, kFfx, Preset::medium, 75},
    {"extrapolate_ffx_100", "Extrapolation from FidelityFX optical flow, 100%", Kind::flow_extrapolate, kFfx, Preset::medium, 100},
    {"guide_snapshot", "Copying the game's vectors and depth (per game frame)", Kind::guide_snapshot},
    {"nv_fast_50", "NVIDIA optical flow, fast, 50%", Kind::flow, kNv, Preset::fast, 50},
    {"nv_fast_75", "NVIDIA optical flow, fast, 75%", Kind::flow, kNv, Preset::fast, 75},
    {"nv_fast_100", "NVIDIA optical flow, fast, 100%", Kind::flow, kNv, Preset::fast, 100},
    {"nv_medium_50", "NVIDIA optical flow, medium, 50%", Kind::flow, kNv, Preset::medium, 50},
    {"nv_medium_75", "NVIDIA optical flow, medium, 75%", Kind::flow, kNv, Preset::medium, 75},
    {"nv_medium_100", "NVIDIA optical flow, medium, 100%", Kind::flow, kNv, Preset::medium, 100},
    {"nv_slow_50", "NVIDIA optical flow, slow, 50%", Kind::flow, kNv, Preset::slow, 50},
    {"nv_slow_75", "NVIDIA optical flow, slow, 75%", Kind::flow, kNv, Preset::slow, 75},
    {"nv_slow_100", "NVIDIA optical flow, slow, 100%", Kind::flow, kNv, Preset::slow, 100},
    {"nv_fast_50_bidi", "NVIDIA optical flow, fast, 50%, bidirectional", Kind::flow, kNv, Preset::fast, 50, true},
    {"nv_fast_75_bidi", "NVIDIA optical flow, fast, 75%, bidirectional", Kind::flow, kNv, Preset::fast, 75, true},
    {"nv_fast_100_bidi", "NVIDIA optical flow, fast, 100%, bidirectional", Kind::flow, kNv, Preset::fast, 100, true},
    {"nv_medium_50_bidi", "NVIDIA optical flow, medium, 50%, bidirectional", Kind::flow, kNv, Preset::medium, 50, true},
    {"nv_medium_75_bidi", "NVIDIA optical flow, medium, 75%, bidirectional", Kind::flow, kNv, Preset::medium, 75, true},
    {"nv_medium_100_bidi", "NVIDIA optical flow, medium, 100%, bidirectional", Kind::flow, kNv, Preset::medium, 100, true},
    {"nv_slow_50_bidi", "NVIDIA optical flow, slow, 50%, bidirectional", Kind::flow, kNv, Preset::slow, 50, true},
    {"nv_slow_75_bidi", "NVIDIA optical flow, slow, 75%, bidirectional", Kind::flow, kNv, Preset::slow, 75, true},
    {"nv_slow_100_bidi", "NVIDIA optical flow, slow, 100%, bidirectional", Kind::flow, kNv, Preset::slow, 100, true},
    {"native_100", "NVIDIA DLSS Frame Generation, 100%", Kind::native, kFfx, Preset::medium, 50, false, 100},
    {"native_67", "NVIDIA DLSS Frame Generation, 67%", Kind::native, kFfx, Preset::medium, 50, false, 67},
    {"native_50", "NVIDIA DLSS Frame Generation, 50%", Kind::native, kFfx, Preset::medium, 50, false, 50},
    {"native_100_3x", "NVIDIA DLSS Frame Generation, 100%, 3X", Kind::native, kFfx, Preset::medium, 50, false, 100, 2},
    {"native_67_3x", "NVIDIA DLSS Frame Generation, 67%, 3X", Kind::native, kFfx, Preset::medium, 50, false, 67, 2},
    {"native_50_3x", "NVIDIA DLSS Frame Generation, 50%, 3X", Kind::native, kFfx, Preset::medium, 50, false, 50, 2},
}};

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::optional<double> parse_double(std::string_view text) noexcept {
    text = trim(text);
    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_integer(std::string_view text) noexcept {
    text = trim(text);
    Integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::string format_double(double value, int decimals = 1) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, decimals);
    if (error != std::errc{}) return "0";
    return std::string(buffer.data(), end);
}

// One line per value: a stored text never carries a line break.
[[nodiscard]] std::string single_line(std::string_view value) {
    std::string output(value);
    for (char& character : output) {
        if (character == '\r' || character == '\n') character = ' ';
    }
    return std::string(trim(output));
}

// Calls `line` with each line of `text`, without its line break.
template <typename Function>
void for_each_line(std::string_view text, Function&& line) {
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t newline = text.find('\n', offset);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        if (!line(text.substr(offset, end - offset))) return;
        if (newline == std::string_view::npos) return;
        offset = newline + 1;
    }
}

// The value of "name=" among a flight-log record's space-separated fields.
[[nodiscard]] std::string_view field(std::string_view line, std::string_view name) noexcept {
    std::size_t offset = 0;
    while (offset < line.size()) {
        std::size_t end = line.find(' ', offset);
        if (end == std::string_view::npos) end = line.size();
        const std::string_view token = line.substr(offset, end - offset);
        if (token.size() > name.size() && token.substr(0, name.size()) == name &&
            token[name.size()] == '=') {
            return trim(token.substr(name.size() + 1));
        }
        offset = end + 1;
    }
    return {};
}

[[nodiscard]] bool plausible_eye(std::uint64_t width, std::uint64_t height) noexcept {
    return width >= 64 && height >= 64 && width <= 16384 && height <= 16384;
}

} // namespace

std::span<const CaseSpec> standard_cases() noexcept {
    return kCases;
}

const CaseSpec* find_case(std::string_view key) noexcept {
    for (const auto& spec : kCases) {
        if (spec.key == key) return &spec;
    }
    return nullptr;
}

std::string flow_case_key(CaseBackend backend, CasePreset preset, int input_scale,
                          bool bidirectional) {
    const std::string scale = std::to_string(input_scale);
    if (backend == CaseBackend::fidelity_fx) return "ffx_" + scale;
    const char* name = preset == CasePreset::fast ? "fast"
        : preset == CasePreset::slow ? "slow" : "medium";
    return std::string("nv_") + name + "_" + scale + (bidirectional ? "_bidi" : "");
}

bool uses_game_guides(CaseKind kind) noexcept {
    return kind == CaseKind::vectors || kind == CaseKind::hybrid || kind == CaseKind::extrapolate ||
           kind == CaseKind::native;
}

bool extrapolates(CaseKind kind) noexcept {
    return kind == CaseKind::extrapolate || kind == CaseKind::flow_extrapolate;
}

std::string flow_extrapolate_case_key(int input_scale) {
    return "extrapolate_ffx_" + std::to_string(input_scale);
}

std::string hybrid_case_key(int input_scale) {
    return "hybrid_" + std::to_string(input_scale);
}

std::string native_case_key(int native_scale, bool triple) {
    return "native_" + std::to_string(native_scale) + (triple ? "_3x" : "");
}

std::string_view status_name(CaseStatus status) noexcept {
    switch (status) {
    case CaseStatus::ok: return "ok";
    case CaseStatus::unavailable: return "unavailable";
    case CaseStatus::failed: return "failed";
    case CaseStatus::not_run:
    default: return "not_run";
    }
}

CaseStatus parse_status(std::string_view text) noexcept {
    text = trim(text);
    if (text == "ok") return CaseStatus::ok;
    if (text == "unavailable") return CaseStatus::unavailable;
    if (text == "failed") return CaseStatus::failed;
    return CaseStatus::not_run;
}

bool Results::drifted() const noexcept {
    return drift > 0.0 && (drift > 1.25 || drift < 0.8);
}

const CaseResult* Results::find(std::string_view key) const noexcept {
    for (const auto& result : cases) {
        if (result.key == key) return &result;
    }
    return nullptr;
}

std::optional<double> Results::cost_ms(std::string_view key) const noexcept {
    const CaseResult* result = find(key);
    if (result == nullptr || result->status != CaseStatus::ok || result->median_us <= 0.0) {
        return std::nullopt;
    }
    return result->median_us / 1000.0;
}

void Results::set(CaseResult result) {
    for (auto& existing : cases) {
        if (existing.key == result.key) {
            existing = std::move(result);
            return;
        }
    }
    cases.push_back(std::move(result));
}

Results parse_results(std::string_view text) {
    Results results;
    results.format = 0;
    std::string section;
    CaseResult* current = nullptr;
    for_each_line(text, [&](std::string_view raw) {
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') return true;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(trim(line.substr(1, line.size() - 2)));
            current = nullptr;
            if (section.rfind("case.", 0) == 0 && section.size() > 5) {
                CaseResult result;
                result.key = section.substr(5);
                results.set(std::move(result));
                for (auto& existing : results.cases) {
                    if (existing.key == section.substr(5)) current = &existing;
                }
            }
            return true;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) return true;
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (section == "benchmark") {
            if (key == "format") results.format = parse_integer<int>(value).value_or(0);
            else if (key == "complete") results.complete = value == "1";
            else if (key == "last_started") results.last_started = std::string(value);
            else if (key == "gpu") results.gpu = std::string(value);
            else if (key == "vendor_id") results.vendor_id = parse_integer<std::uint32_t>(value).value_or(0);
            else if (key == "device_id") results.device_id = parse_integer<std::uint32_t>(value).value_or(0);
            else if (key == "driver") results.driver = std::string(value);
            else if (key == "eye_width") results.eye_width = parse_integer<std::uint32_t>(value).value_or(0);
            else if (key == "eye_height") results.eye_height = parse_integer<std::uint32_t>(value).value_or(0);
            else if (key == "refresh_hz") {
                const double hz = parse_double(value).value_or(90.0);
                results.refresh_hz = hz >= 30.0 && hz <= 500.0 ? hz : 90.0;
            } else if (key == "date") results.date = std::string(value);
            else if (key == "drift") results.drift = std::max(parse_double(value).value_or(0.0), 0.0);
        } else if (current != nullptr) {
            if (key == "status") current->status = parse_status(value);
            else if (key == "median_us") current->median_us = parse_double(value).value_or(0.0);
            else if (key == "p10_us") current->p10_us = parse_double(value).value_or(0.0);
            else if (key == "samples") current->samples = parse_integer<std::uint32_t>(value).value_or(0);
            else if (key == "note") current->note = std::string(value);
        }
        return true;
    });
    return results;
}

std::string serialize_results(const Results& results) {
    std::string text = "[benchmark]\r\nformat=" + std::to_string(results.format) +
        "\r\ncomplete=" + (results.complete ? "1" : "0") +
        "\r\nlast_started=" + single_line(results.last_started) +
        "\r\ngpu=" + single_line(results.gpu) +
        "\r\nvendor_id=" + std::to_string(results.vendor_id) +
        "\r\ndevice_id=" + std::to_string(results.device_id) +
        "\r\ndriver=" + single_line(results.driver) +
        "\r\neye_width=" + std::to_string(results.eye_width) +
        "\r\neye_height=" + std::to_string(results.eye_height) +
        "\r\nrefresh_hz=" + format_double(results.refresh_hz) +
        "\r\ndate=" + single_line(results.date) +
        "\r\ndrift=" + format_double(results.drift, 3) + "\r\n";
    for (const auto& result : results.cases) {
        text += "\r\n[case." + single_line(result.key) + "]\r\nstatus=" +
            std::string(status_name(result.status));
        if (result.status == CaseStatus::ok) {
            text += "\r\nmedian_us=" + format_double(result.median_us) +
                "\r\np10_us=" + format_double(result.p10_us) +
                "\r\nsamples=" + std::to_string(result.samples);
        }
        if (!result.note.empty()) text += "\r\nnote=" + single_line(result.note);
        text += "\r\n";
    }
    return text;
}

Estimate estimate(double cost_ms, double refresh_hz, int frames_per_game_frame) {
    Estimate output;
    output.cost_ms = std::max(cost_ms, 0.0);
    output.refresh_hz = refresh_hz > 0.0 ? refresh_hz : 90.0;
    output.frames_per_game_frame = std::clamp(frames_per_game_frame, 2, 3);
    output.display_period_ms = 1000.0 / output.refresh_hz;
    output.budget_ms = output.display_period_ms * output.frames_per_game_frame;
    output.budget_share = output.cost_ms / output.budget_ms;
    output.max_game_ms = output.budget_ms - output.cost_ms;
    output.max_gain = output.frames_per_game_frame - 1.0 -
        output.cost_ms / output.display_period_ms;
    output.fits = output.max_gain > 0.0 && output.max_game_ms > 0.0;
    output.min_game_fps = output.max_game_ms > 0.0
        ? 1000.0 / output.max_game_ms
        : std::numeric_limits<double>::infinity();
    return output;
}

std::optional<double> recorded_error(const CaseSpec& spec) noexcept {
    if (spec.generated_frames != 1) return std::nullopt;
    switch (spec.kind) {
    case CaseKind::flow:
        if (spec.input_scale != 50 || spec.bidirectional) return std::nullopt;
        if (spec.backend == CaseBackend::fidelity_fx) return 7.86;
        if (spec.preset == CasePreset::medium) return 7.67;
        if (spec.preset == CasePreset::slow) return 7.59;
        return std::nullopt;
    case CaseKind::hybrid:
        return spec.input_scale == 50 ? std::optional<double>(7.18) : std::nullopt;
    case CaseKind::vectors:
        return 8.40;
    case CaseKind::native:
        if (spec.native_scale == 67) return 7.96;
        if (spec.native_scale == 100) return 8.04;
        return std::nullopt;
    case CaseKind::extrapolate:
    case CaseKind::guide_snapshot:
    default:
        return std::nullopt;
    }
}

std::wstring quality_word(double error) {
    if (error <= 7.3) return L"best";
    if (error <= 7.75) return L"very good";
    if (error <= 8.1) return L"good";
    if (error <= 9.0) return L"fair";
    return L"poor";
}

std::optional<HeadsetGuess> headset_from_flight_log(std::string_view text) {
    HeadsetGuess guess;
    std::uint64_t shortest_period = 0;
    unsigned periods = 0;
    for_each_line(text, [&](std::string_view line) {
        const std::string_view operation = field(line, "op");
        if (operation == "view_configuration") {
            // a = recommended width << 32 | height, for view `result`.
            if (guess.eye_width == 0 && field(line, "result") == "0") {
                const auto packed = parse_integer<std::uint64_t>(field(line, "a"));
                if (packed) {
                    const std::uint64_t width = *packed >> 32;
                    const std::uint64_t height = *packed & 0xFFFFFFFFULL;
                    if (plausible_eye(width, height)) {
                        guess.eye_width = static_cast<std::uint32_t>(width);
                        guess.eye_height = static_cast<std::uint32_t>(height);
                    }
                }
            }
        } else if (operation == "internal_wait_frame" && field(line, "phase") == "E" &&
                   field(line, "result") == "0") {
            // b = the runtime's predictedDisplayPeriod, in nanoseconds.
            const auto period = parse_integer<std::uint64_t>(field(line, "b"));
            if (period && *period >= 2'000'000ULL && *period <= 100'000'000ULL) {
                if (shortest_period == 0 || *period < shortest_period) shortest_period = *period;
                ++periods;
            }
        }
        // Enough of the session's start to know both.
        return !(guess.eye_width != 0 && periods >= 400);
    });
    if (guess.eye_width == 0) return std::nullopt;
    if (shortest_period != 0) {
        guess.refresh_hz = snap_refresh(1e9 / static_cast<double>(shortest_period));
    }
    return guess;
}

std::optional<HeadsetGuess> headset_from_ini(std::string_view text) {
    HeadsetGuess guess;
    bool in_section = false;
    for_each_line(text, [&](std::string_view raw) {
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') return true;
        if (line.front() == '[' && line.back() == ']') {
            in_section = trim(line.substr(1, line.size() - 2)) == "headset";
            return true;
        }
        if (!in_section) return true;
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) return true;
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key == "eye_width") guess.eye_width = parse_integer<std::uint32_t>(value).value_or(0);
        else if (key == "eye_height") guess.eye_height = parse_integer<std::uint32_t>(value).value_or(0);
        else if (key == "refresh_hz") {
            const auto hz = parse_double(value);
            if (hz && *hz >= 30.0 && *hz <= 500.0) guess.refresh_hz = snap_refresh(*hz);
        }
        return true;
    });
    if (!plausible_eye(guess.eye_width, guess.eye_height)) return std::nullopt;
    return guess;
}

double snap_refresh(double hz) noexcept {
    constexpr std::array<double, 9> common{60, 72, 80, 90, 100, 120, 144, 165, 240};
    for (const double rate : common) {
        if (std::abs(hz - rate) < 1.0) return rate;
    }
    return std::round(hz);
}

std::string driver_version_text(std::uint64_t version) {
    char buffer[48]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
                  static_cast<unsigned>((version >> 48) & 0xFFFF),
                  static_cast<unsigned>((version >> 32) & 0xFFFF),
                  static_cast<unsigned>((version >> 16) & 0xFFFF),
                  static_cast<unsigned>(version & 0xFFFF));
    return buffer;
}

std::string nvidia_driver_text(std::uint32_t vendor_id, std::string_view version) {
    if (vendor_id != kNvidiaVendorId) return {};
    // The last two dot-separated parts.
    const std::size_t last = version.rfind('.');
    if (last == std::string_view::npos || last == 0) return {};
    const std::size_t third = version.rfind('.', last - 1);
    const auto minor = parse_integer<unsigned>(version.substr(last + 1));
    const auto major = parse_integer<unsigned>(
        version.substr(third == std::string_view::npos ? 0 : third + 1,
                       last - (third == std::string_view::npos ? 0 : third + 1)));
    if (!minor || !major || *minor > 9999) return {};
    const unsigned number = (*major % 10) * 10000U + *minor;
    char buffer[24]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%02u", number / 100, number % 100);
    return buffer;
}

std::wstring format_ms(double milliseconds) {
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), milliseconds < 1.0 ? L"%.2f ms" : L"%.1f ms",
                  milliseconds);
    return buffer;
}

std::wstring short_annotation(const Estimate& value) {
    std::wstring text = format_ms(value.cost_ms) + L" \u00B7 ";
    if (!value.fits) {
        wchar_t buffer[48]{};
        std::swprintf(buffer, std::size(buffer), L"too slow for %.0f Hz", value.refresh_hz);
        return text + buffer;
    }
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"up to +%.0f%%", value.max_gain * 100.0);
    return text + buffer;
}

int added_latency_frames(CaseKind kind, int frames_per_game_frame) noexcept {
    if (extrapolates(kind) || kind == CaseKind::guide_snapshot) return 0;
    return std::clamp(frames_per_game_frame, 2, 3) - 1;
}

} // namespace xrfg::benchmark
