#pragma once

// The tray's view of its settings as one list of methods, and what each of
// them costs according to the last "Benchmark this PC" run.

#include "xrfg/benchmark_model.hpp"
#include "xrfg/standalone_launcher.hpp"

#include <optional>
#include <string>

namespace xrfg::standalone {

// One choice in the menu's method list: OFXR with one of its optical-flow
// engines, or native DLSS Frame Generation.
enum class Method {
    fidelity_fx,
    nvidia_fast,
    nvidia_medium,
    nvidia_slow,
    native_dlss,
};
[[nodiscard]] Method current_method(const LauncherSettings& settings) noexcept;
// frame_generation, and for OFXR the backend and NVIDIA preset.
void apply_method(LauncherSettings& settings, Method method) noexcept;

// What OFXR does in a game that publishes DLSS motion vectors.
enum class DlssGameMode { vectors, hybrid, extrapolate };
// The hybrid when both flags are set, as the synthesizer runs it.
[[nodiscard]] DlssGameMode dlss_game_mode(const LauncherSettings& settings) noexcept;
void apply_dlss_game_mode(LauncherSettings& settings, DlssGameMode mode) noexcept;

[[nodiscard]] int input_scale_percent(NvidiaInputScale scale) noexcept;
[[nodiscard]] std::wstring method_name(Method method);
[[nodiscard]] std::wstring dlss_game_mode_name(DlssGameMode mode);
// "NVIDIA optical flow, medium · 2X", for the menu's status line.
[[nodiscard]] std::wstring active_method_summary(const LauncherSettings& settings);

// A method's GPU time per generated pair (or triple, at 3X) with the given
// settings, from the benchmark. A DLSS-game method includes the per-frame
// copy of the game's vectors and depth. At 3X an OFXR method is estimated as
// its 2X time plus one more composition pass, measured on FidelityFX flow.
struct ModeCost {
    benchmark::CaseStatus status{benchmark::CaseStatus::not_run};
    double cost_ms{};
    bool estimated{};
    benchmark::CaseKind kind{benchmark::CaseKind::flow};
    // Why it is unavailable or failed.
    std::string note;
};
// Settings a submenu entry asks about instead of the current ones.
struct CostQuery {
    std::optional<int> input_scale;
    std::optional<int> native_scale;
    std::optional<bool> triple;
    std::optional<bool> bidirectional;
};
[[nodiscard]] ModeCost method_cost(const benchmark::Results& results,
                                   const LauncherSettings& settings, Method method,
                                   const CostQuery& query = {});
[[nodiscard]] ModeCost dlss_game_cost(const benchmark::Results& results,
                                      const LauncherSettings& settings, DlssGameMode mode,
                                      const CostQuery& query = {});
// The menu's right-hand column: "0.6 ms · up to +95%", "not available on this
// PC", "failed in the benchmark", or nothing when the case did not run.
[[nodiscard]] std::wstring menu_annotation(const ModeCost& cost, double refresh_hz,
                                           bool triple);

} // namespace xrfg::standalone
