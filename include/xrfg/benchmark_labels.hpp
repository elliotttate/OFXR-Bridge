#pragma once

// The tray's view of its settings as one list of methods and one choice of
// how OFXR makes frames, and what each costs according to the last
// "Benchmark this PC" run.

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

// How OFXR makes frames, as the layer reads dlss_flow_hybrid and extrapolate
// at session start:
// - interpolate: between two real frames, from the game's DLSS vectors where
//   it has them and from the chosen optical flow where it does not.
// - hybrid: the same, with FidelityFX flow beside the game's vectors; the
//   layer then runs FidelityFX in every game.
// - extrapolate: each real frame shown at once and the next predicted from
//   it, SpaceWarp-style, from the game's vectors or else FidelityFX flow;
//   the layer runs FidelityFX and turns the deeper pipeline off.
// Extrapolation wins when both flags are set, as in the layer.
enum class OfxrMode { interpolate, hybrid, extrapolate };
[[nodiscard]] OfxrMode ofxr_mode(const LauncherSettings& settings) noexcept;
void apply_ofxr_mode(LauncherSettings& settings, OfxrMode mode) noexcept;
// Whether the mode runs FidelityFX flow whatever engine the list has chosen:
// extrapolation. The hybrid takes FidelityFX only in games with DLSS vectors,
// where the list's engine does not run in any mode.
[[nodiscard]] bool mode_forces_fidelity_fx(OfxrMode mode) noexcept;

[[nodiscard]] int input_scale_percent(NvidiaInputScale scale) noexcept;
[[nodiscard]] std::wstring method_name(Method method);
[[nodiscard]] std::wstring ofxr_mode_name(OfxrMode mode);
// The menu's status line: "NVIDIA optical flow, medium 50%", a middle dot,
// then "2X" or "3X".
[[nodiscard]] std::wstring active_method_summary(const LauncherSettings& settings);

// GPU time per generated pair (or triple, at 3X) from the benchmark. A
// method on the game's DLSS vectors includes the per-frame copy of the
// game's vectors and depth. At 3X an OFXR method is estimated as its 2X time
// plus one more composition pass, measured on FidelityFX flow.
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
// What `method` costs in a game without DLSS vectors under the current OFXR
// mode: its own flow when interpolating, FidelityFX flow under the hybrid,
// FidelityFX extrapolation when extrapolating. Native generation is the same
// in every mode.
[[nodiscard]] ModeCost method_cost(const benchmark::Results& results,
                                   const LauncherSettings& settings, Method method,
                                   const CostQuery& query = {});
// What `mode` costs in a game with DLSS vectors.
[[nodiscard]] ModeCost ofxr_mode_cost(const benchmark::Results& results,
                                      const LauncherSettings& settings, OfxrMode mode,
                                      const CostQuery& query = {});
// The menu's right-hand column: "0.6 ms", a middle dot and "up to +95%";
// "not available on this PC", "failed in the benchmark", or nothing when the
// case did not run.
[[nodiscard]] std::wstring menu_annotation(const ModeCost& cost, double refresh_hz,
                                           bool triple);

} // namespace xrfg::standalone
