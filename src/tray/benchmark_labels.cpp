#include "xrfg/benchmark_labels.hpp"

#include <algorithm>

namespace xrfg::standalone {
namespace {

using benchmark::CaseStatus;

[[nodiscard]] benchmark::CasePreset case_preset(Method method) noexcept {
    switch (method) {
    case Method::nvidia_fast: return benchmark::CasePreset::fast;
    case Method::nvidia_slow: return benchmark::CasePreset::slow;
    default: return benchmark::CasePreset::medium;
    }
}

// The measured result for `key`, with the guide copy added when `guides`.
[[nodiscard]] ModeCost lookup(const benchmark::Results& results, std::string_view key,
                              benchmark::CaseKind kind, bool guides) {
    ModeCost cost;
    cost.kind = kind;
    const benchmark::CaseResult* result = results.find(key);
    if (result == nullptr) return cost;
    cost.status = result->status;
    cost.note = result->note;
    if (result->status != CaseStatus::ok) return cost;
    cost.cost_ms = result->median_us / 1000.0;
    if (guides) {
        if (const auto snapshot = results.cost_ms(benchmark::kGuideSnapshotKey)) {
            cost.cost_ms += *snapshot;
        }
    }
    return cost;
}

// One more OFXR composition pass, for 3X.
[[nodiscard]] std::optional<double> triple_extra_ms(const benchmark::Results& results) {
    const auto triple = results.cost_ms(benchmark::kOfxrTripleReferenceKey);
    const auto base = results.cost_ms(benchmark::kOfxrTripleBaseKey);
    if (!triple || !base) return std::nullopt;
    return std::max(*triple - *base, 0.0);
}

void add_triple(const benchmark::Results& results, ModeCost& cost) {
    if (cost.status != CaseStatus::ok) return;
    const auto extra = triple_extra_ms(results);
    if (!extra) {
        cost.status = CaseStatus::not_run;
        return;
    }
    cost.cost_ms += *extra;
    cost.estimated = true;
}

} // namespace

Method current_method(const LauncherSettings& settings) noexcept {
    if (settings.frame_generation == FrameGeneration::native_dlss) return Method::native_dlss;
    if (settings.backend == FlowBackend::fidelity_fx) return Method::fidelity_fx;
    switch (settings.nvidia_preset) {
    case NvidiaPerformancePreset::fast: return Method::nvidia_fast;
    case NvidiaPerformancePreset::slow: return Method::nvidia_slow;
    case NvidiaPerformancePreset::medium:
    default: return Method::nvidia_medium;
    }
}

void apply_method(LauncherSettings& settings, Method method) noexcept {
    switch (method) {
    case Method::native_dlss:
        settings.frame_generation = FrameGeneration::native_dlss;
        return;
    case Method::fidelity_fx:
        settings.frame_generation = FrameGeneration::ofxr;
        settings.backend = FlowBackend::fidelity_fx;
        return;
    case Method::nvidia_fast:
    case Method::nvidia_medium:
    case Method::nvidia_slow:
        settings.frame_generation = FrameGeneration::ofxr;
        settings.backend = FlowBackend::nvidia;
        settings.nvidia_preset = method == Method::nvidia_fast ? NvidiaPerformancePreset::fast
            : method == Method::nvidia_slow ? NvidiaPerformancePreset::slow
            : NvidiaPerformancePreset::medium;
        return;
    }
}

DlssGameMode dlss_game_mode(const LauncherSettings& settings) noexcept {
    if (settings.dlss_flow_hybrid) return DlssGameMode::hybrid;
    if (settings.extrapolate) return DlssGameMode::extrapolate;
    return DlssGameMode::vectors;
}

void apply_dlss_game_mode(LauncherSettings& settings, DlssGameMode mode) noexcept {
    settings.dlss_flow_hybrid = mode == DlssGameMode::hybrid;
    settings.extrapolate = mode == DlssGameMode::extrapolate;
}

int input_scale_percent(NvidiaInputScale scale) noexcept {
    switch (scale) {
    case NvidiaInputScale::full: return 100;
    case NvidiaInputScale::three_quarter: return 75;
    case NvidiaInputScale::half:
    default: return 50;
    }
}

std::wstring method_name(Method method) {
    switch (method) {
    case Method::fidelity_fx: return L"FidelityFX optical flow";
    case Method::nvidia_fast: return L"NVIDIA optical flow, fast";
    case Method::nvidia_medium: return L"NVIDIA optical flow, medium";
    case Method::nvidia_slow: return L"NVIDIA optical flow, slow";
    case Method::native_dlss:
    default: return L"NVIDIA DLSS Frame Generation";
    }
}

std::wstring dlss_game_mode_name(DlssGameMode mode) {
    switch (mode) {
    case DlssGameMode::hybrid: return L"game's motion vectors + FidelityFX flow";
    case DlssGameMode::extrapolate: return L"extrapolation from the game's motion vectors";
    case DlssGameMode::vectors:
    default: return L"the game's motion vectors";
    }
}

std::wstring active_method_summary(const LauncherSettings& settings) {
    const Method method = current_method(settings);
    std::wstring text = method_name(method);
    if (method == Method::native_dlss) {
        text += L" " + std::to_wstring(settings.native_scale) + L"%";
    } else {
        text += L" " + std::to_wstring(input_scale_percent(settings.nvidia_input_scale)) + L"%";
    }
    text += settings.triple_frame_gen ? L" · 3X" : L" · 2X";
    return text;
}

ModeCost method_cost(const benchmark::Results& results, const LauncherSettings& settings,
                     Method method, const CostQuery& query) {
    const bool triple = query.triple.value_or(settings.triple_frame_gen);
    if (method == Method::native_dlss) {
        const int scale = query.native_scale.value_or(settings.native_scale);
        return lookup(results, benchmark::native_case_key(scale, triple),
                      benchmark::CaseKind::native, true);
    }
    const int scale = query.input_scale.value_or(input_scale_percent(settings.nvidia_input_scale));
    const bool nvidia = method != Method::fidelity_fx;
    const bool bidirectional = nvidia && query.bidirectional.value_or(settings.nvidia_bidirectional);
    ModeCost cost = lookup(results,
        benchmark::flow_case_key(nvidia ? benchmark::CaseBackend::nvidia
                                        : benchmark::CaseBackend::fidelity_fx,
                                 case_preset(method), scale, bidirectional),
        benchmark::CaseKind::flow, false);
    if (triple) add_triple(results, cost);
    return cost;
}

ModeCost dlss_game_cost(const benchmark::Results& results, const LauncherSettings& settings,
                        DlssGameMode mode, const CostQuery& query) {
    const bool triple = query.triple.value_or(settings.triple_frame_gen);
    ModeCost cost;
    switch (mode) {
    case DlssGameMode::hybrid:
        cost = lookup(results,
            benchmark::hybrid_case_key(query.input_scale.value_or(
                input_scale_percent(settings.nvidia_input_scale))),
            benchmark::CaseKind::hybrid, true);
        break;
    case DlssGameMode::extrapolate:
        cost = lookup(results, benchmark::kExtrapolateKey, benchmark::CaseKind::extrapolate, true);
        break;
    case DlssGameMode::vectors:
    default:
        cost = lookup(results, benchmark::kVectorsKey, benchmark::CaseKind::vectors, true);
        break;
    }
    if (triple) add_triple(results, cost);
    return cost;
}

std::wstring menu_annotation(const ModeCost& cost, double refresh_hz, bool triple) {
    switch (cost.status) {
    case CaseStatus::ok:
        return benchmark::short_annotation(
            benchmark::estimate(cost.cost_ms, refresh_hz, triple ? 3 : 2));
    case CaseStatus::unavailable:
        return L"not available on this PC";
    case CaseStatus::failed:
        return L"failed in the benchmark";
    case CaseStatus::not_run:
    default:
        return {};
    }
}

} // namespace xrfg::standalone
