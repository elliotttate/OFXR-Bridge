// "Benchmark this PC": the case catalogue, the results file, the speed-up
// arithmetic, the headset guesses and the tray's lookups from its settings.
#include "xrfg/benchmark_labels.hpp"
#include "xrfg/benchmark_model.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double value, double expected, double tolerance = 1e-3) {
    return std::abs(value - expected) <= tolerance;
}

void catalogue() {
    using namespace xrfg::benchmark;
    std::set<std::string_view> keys;
    for (const auto& spec : standard_cases()) {
        require(keys.insert(spec.key).second, "case keys are unique");
        require(find_case(spec.key) == &spec, "find_case finds every case");
        require(!spec.name.empty(), "every case has a name");
    }
    require(find_case("nonsense") == nullptr, "unknown keys are not found");
    // Every key the tray can ask for exists.
    for (const int scale : {50, 75, 100}) {
        require(find_case(flow_case_key(CaseBackend::fidelity_fx, CasePreset::medium, scale, false)),
                "FidelityFX flow at every scale");
        require(find_case(hybrid_case_key(scale)), "hybrid at every scale");
        const auto* extrapolation = find_case(flow_extrapolate_case_key(scale));
        require(extrapolation && extrapolation->kind == CaseKind::flow_extrapolate &&
                    extrapolation->input_scale == scale && !uses_game_guides(extrapolation->kind) &&
                    extrapolates(extrapolation->kind),
                "extrapolation from FidelityFX flow at every scale");
        for (const auto preset : {CasePreset::fast, CasePreset::medium, CasePreset::slow}) {
            for (const bool bidirectional : {false, true}) {
                require(find_case(flow_case_key(CaseBackend::nvidia, preset, scale, bidirectional)),
                        "NVIDIA flow at every preset, scale and direction");
            }
        }
    }
    for (const int scale : {100, 67, 50}) {
        for (const bool triple : {false, true}) {
            const auto* spec = find_case(native_case_key(scale, triple));
            require(spec && spec->kind == CaseKind::native && spec->native_scale == scale &&
                        spec->generated_frames == (triple ? 2 : 1),
                    "native at every scale, 2X and 3X");
        }
    }
    for (const auto key : {kVectorsKey, kExtrapolateKey, kGuideSnapshotKey,
                           kOfxrTripleReferenceKey, kOfxrTripleBaseKey}) {
        require(find_case(key) != nullptr, "named keys exist");
    }
    require(find_case(kOfxrTripleReferenceKey)->generated_frames == 2, "3X reference is 3X");
    require(uses_game_guides(CaseKind::vectors) && uses_game_guides(CaseKind::native) &&
                uses_game_guides(CaseKind::extrapolate) && !uses_game_guides(CaseKind::flow) &&
                extrapolates(CaseKind::extrapolate) && !extrapolates(CaseKind::hybrid),
            "kinds");
    // NGX runs last: a fault there costs the fewest results.
    const auto cases = standard_cases();
    bool seen_native = false;
    for (const auto& spec : cases) {
        if (spec.kind == CaseKind::native) seen_native = true;
        else require(!seen_native, "native cases run last");
    }
}

void results_file() {
    using namespace xrfg::benchmark;
    Results results;
    results.complete = true;
    results.gpu = "NVIDIA GeForce RTX 5090";
    results.vendor_id = 0x10de;
    results.device_id = 0x2b85;
    results.driver = "32.0.15.8180";
    results.eye_width = 3004;
    results.eye_height = 3004;
    results.refresh_hz = 120.0;
    results.date = "2026-10-09 12:00";
    results.last_started = "native_50_3x";
    results.drift = 1.031;
    results.set({"ffx_50", CaseStatus::ok, 590.4, 571.0, 32, ""});
    results.set({"nv_fast_50", CaseStatus::unavailable, 0, 0, 0, "Needs an NVIDIA\r\nGPU"});
    results.set({"native_100", CaseStatus::failed, 0, 0, 0, "NGX = 0xBAD00004"});
    const auto parsed = parse_results(serialize_results(results));
    require(parsed.format == Results::kFormat && parsed.complete, "header round trip");
    require(parsed.gpu == results.gpu && parsed.vendor_id == 0x10de &&
                parsed.device_id == 0x2b85 && parsed.driver == results.driver,
            "adapter round trip");
    require(parsed.eye_width == 3004 && parsed.eye_height == 3004 &&
                near(parsed.refresh_hz, 120.0) && parsed.date == results.date &&
                parsed.last_started == "native_50_3x",
            "resolution and refresh round trip");
    require(parsed.cases.size() == 3, "every case round trips");
    require(near(parsed.drift, 1.031) && !parsed.drifted(), "drift round trips");
    Results busy;
    busy.drift = 1.6;
    require(busy.drifted() && !Results{}.drifted(), "a busy GPU is noticed; unchecked is not drift");
    const auto* ffx = parsed.find("ffx_50");
    require(ffx && ffx->status == CaseStatus::ok && near(ffx->median_us, 590.4, 0.05) &&
                near(ffx->p10_us, 571.0, 0.05) && ffx->samples == 32,
            "a measured case round trips");
    require(parsed.cost_ms("ffx_50") && near(*parsed.cost_ms("ffx_50"), 0.5904, 1e-4),
            "cost in milliseconds");
    const auto* nvidia = parsed.find("nv_fast_50");
    require(nvidia && nvidia->status == CaseStatus::unavailable &&
                nvidia->note == "Needs an NVIDIA  GPU" && !parsed.cost_ms("nv_fast_50"),
            "an unavailable case keeps its reason on one line");
    require(parsed.find("native_100")->note == "NGX = 0xBAD00004", "a note may hold '='");
    // Replacing a case keeps one entry.
    auto changed = parsed;
    changed.set({"ffx_50", CaseStatus::failed, 0, 0, 0, "lost"});
    require(changed.cases.size() == 3 && changed.find("ffx_50")->status == CaseStatus::failed,
            "set replaces");
    // Files with no format, or another one, read as format 0 or that one.
    require(parse_results("[benchmark]\r\ngpu=x\r\n").format == 0, "no format is format 0");
    require(near(parse_results("[benchmark]\r\nrefresh_hz=7\r\n").refresh_hz, 90.0),
            "implausible refresh falls back to 90 Hz");
}

void speed_up_model() {
    using namespace xrfg::benchmark;
    // 90 Hz, 2X, FidelityFX at 0.6 ms: the game gets 22.2 ms per frame and
    // needs 21.6 of them, 46.2 fps, to show 90.
    const auto ffx = estimate(0.6, 90.0, 2);
    require(near(ffx.display_period_ms, 11.111) && near(ffx.budget_ms, 22.222) &&
                near(ffx.max_game_ms, 21.622) && near(ffx.min_game_fps, 46.25, 0.01) &&
                near(ffx.budget_share, 0.027, 0.0005) && ffx.fits,
            "2X budget arithmetic");
    require(near(ffx.max_gain, 90.0 / ffx.min_game_fps - 1.0) && near(ffx.max_gain, 0.946),
            "gain is refresh over the needed frame rate");
    // 3X at 120 Hz: 25 ms per game frame.
    const auto triple = estimate(3.5, 120.0, 3);
    require(near(triple.budget_ms, 25.0) && near(triple.max_game_ms, 21.5) &&
                near(triple.max_gain, 3.0 - 1.0 - 3.5 / (1000.0 / 120.0)) && triple.fits,
            "3X budget arithmetic");
    // A cost of a whole display period or more gains nothing at 2X.
    require(!estimate(11.2, 90.0, 2).fits && estimate(11.0, 90.0, 2).fits, "too slow");
    require(short_annotation(ffx) == L"0.60 ms \u00B7 up to +95%", "menu annotation");
    require(short_annotation(estimate(12.0, 90.0, 2)) == L"12.0 ms \u00B7 too slow for 90 Hz",
            "menu annotation when too slow");
    require(format_ms(2.94) == L"2.9 ms" && format_ms(0.534) == L"0.53 ms", "format_ms");
    const std::uint64_t driver = (32ULL << 48) | (15ULL << 16) | 8180ULL;
    require(driver_version_text(driver) == "32.0.15.8180", "driver version text");
    require(nvidia_driver_text(kNvidiaVendorId, "32.0.15.8180") == "581.80" &&
                nvidia_driver_text(kNvidiaVendorId, "32.0.16.1656") == "616.56" &&
                nvidia_driver_text(0x1002, "32.0.15.8180").empty() &&
                nvidia_driver_text(kNvidiaVendorId, "nonsense").empty(),
            "NVIDIA's driver number");
    require(added_latency_frames(CaseKind::extrapolate, 2) == 0 &&
                added_latency_frames(CaseKind::flow, 2) == 1 &&
                added_latency_frames(CaseKind::native, 3) == 2,
            "interpolation waits for the next frame, extrapolation does not");
    // Quality from recorded frames, where it was measured.
    require(recorded_error(*find_case("hybrid_50")) == 7.18 &&
                recorded_error(*find_case("nv_slow_50")) == 7.59 &&
                recorded_error(*find_case("native_67")) == 7.96 &&
                !recorded_error(*find_case("extrapolate")) &&
                !recorded_error(*find_case("ffx_100")) &&
                !recorded_error(*find_case("native_67_3x")),
            "recorded quality");
    require(quality_word(7.18) == L"best" && quality_word(7.67) == L"very good" &&
                quality_word(7.86) == L"good" && quality_word(8.40) == L"fair",
            "quality words");
}

void headset_guesses() {
    using namespace xrfg::benchmark;
    // A Steam Frame session's start-up records, as the flight recorder writes
    // them: 3004x3004 recommended, the runtime at 120 Hz, at times halved.
    const std::string log =
        "seq=1 ms=0.002 tid=1 phase=I op=logger result=439 dur_us=0 a=32 b=0 c=71432\n"
        "seq=9 ms=2209.309 tid=2 phase=I op=view_configuration result=0 dur_us=0 "
        "a=12902081760188 b=35184372097024 c=1\n"
        "seq=10 ms=2209.313 tid=2 phase=I op=view_configuration result=1 dur_us=0 "
        "a=11355893533268 b=35184372097024 c=1\n"
        "seq=11 ms=3000.1 tid=3 phase=B op=internal_wait_frame result=0 dur_us=0 a=1 b=0 c=0\n"
        "seq=11 ms=3016.7 tid=3 phase=E op=internal_wait_frame result=0 dur_us=7723 "
        "a=130808840116100 b=16666700 c=1\n"
        "seq=12 ms=3025.0 tid=3 phase=E op=internal_wait_frame result=0 dur_us=23 "
        "a=130808856782000 b=8333300 c=1\r\n"
        "seq=13 ms=3025.0 tid=3 phase=E op=internal_wait_frame result=-2 dur_us=23 a=0 b=1000 c=0\n";
    const auto guess = headset_from_flight_log(log);
    require(guess && guess->eye_width == 3004 && guess->eye_height == 3004,
            "the first view's recommended size");
    require(guess->refresh_hz && near(*guess->refresh_hz, 120.0), "the shortest runtime period");
    require(!headset_from_flight_log("seq=1 op=logger a=1\n"), "nothing without a view record");
    const auto no_period = headset_from_flight_log(
        "seq=9 phase=I op=view_configuration result=0 a=8864812501152 b=0 c=1\n");
    require(no_period && no_period->eye_width == 2064 && no_period->eye_height == 2208 &&
                !no_period->refresh_hz,
            "a size alone");
    const auto ini = headset_from_ini("[headset]\r\neye_width=2448\r\neye_height=2448\r\n"
                                      "refresh_hz=89.9\r\n");
    require(ini && ini->eye_width == 2448 && ini->refresh_hz && near(*ini->refresh_hz, 90.0),
            "headset.ini");
    require(!headset_from_ini("[other]\r\neye_width=2448\r\neye_height=2448\r\n"),
            "headset.ini needs its section");
    require(near(snap_refresh(119.88), 120.0) && near(snap_refresh(72.4), 72.0) &&
                near(snap_refresh(85.2), 85.0),
            "refresh snapping");
}

void tray_lookups() {
    using namespace xrfg::standalone;
    using xrfg::benchmark::CaseStatus;
    LauncherSettings settings;
    require(current_method(settings) == Method::nvidia_medium, "the default method");
    for (const auto method : {Method::fidelity_fx, Method::nvidia_fast, Method::nvidia_medium,
                              Method::nvidia_slow, Method::native_dlss}) {
        LauncherSettings changed;
        apply_method(changed, method);
        require(current_method(changed) == method, "apply_method round trips");
    }
    LauncherSettings native;
    apply_method(native, Method::native_dlss);
    apply_method(native, Method::fidelity_fx);
    require(native.frame_generation == FrameGeneration::ofxr &&
                native.backend == FlowBackend::fidelity_fx,
            "a flow method leaves native generation");
    require(ofxr_mode(settings) == OfxrMode::hybrid, "the hybrid by default");
    for (const auto mode : {OfxrMode::interpolate, OfxrMode::hybrid, OfxrMode::extrapolate}) {
        LauncherSettings changed;
        apply_ofxr_mode(changed, mode);
        require(ofxr_mode(changed) == mode && !(changed.dlss_flow_hybrid && changed.extrapolate),
                "one OFXR mode at a time");
        require(mode_forces_fidelity_fx(mode) == (mode == OfxrMode::extrapolate),
                "extrapolation takes FidelityFX, the hybrid only with DLSS vectors");
    }
    LauncherSettings both;
    both.dlss_flow_hybrid = both.extrapolate = true;
    require(ofxr_mode(both) == OfxrMode::extrapolate, "the layer prefers extrapolation");
    require(active_method_summary(both).rfind(L"FidelityFX optical flow 50%", 0) == 0,
            "the status line names the engine that runs");
    require(active_method_summary(settings) == L"NVIDIA optical flow, medium 50% \u00B7 2X",
            "status line");

    xrfg::benchmark::Results results;
    results.set({"ffx_50", CaseStatus::ok, 600, 590, 32, ""});
    results.set({"ffx_50_3x", CaseStatus::ok, 900, 880, 32, ""});
    results.set({"ffx_100", CaseStatus::ok, 1300, 1290, 32, ""});
    results.set({"nv_medium_50", CaseStatus::ok, 2900, 2800, 32, ""});
    results.set({"nv_medium_50_bidi", CaseStatus::ok, 4100, 4000, 32, ""});
    results.set({"nv_slow_50", CaseStatus::unavailable, 0, 0, 0, "No NVIDIA GPU"});
    results.set({"vectors", CaseStatus::ok, 500, 490, 32, ""});
    results.set({"hybrid_50", CaseStatus::ok, 1600, 1590, 32, ""});
    results.set({"guide_snapshot", CaseStatus::ok, 100, 95, 32, ""});
    results.set({"native_67", CaseStatus::ok, 1700, 1650, 32, ""});
    results.set({"native_67_3x", CaseStatus::ok, 2500, 2450, 32, ""});

    const auto medium = method_cost(results, settings, Method::nvidia_medium);
    require(medium.status == CaseStatus::ok && near(medium.cost_ms, 2.9) && !medium.estimated,
            "the current method at the current scale");
    settings.nvidia_bidirectional = true;
    require(near(method_cost(results, settings, Method::nvidia_medium).cost_ms, 4.1),
            "bidirectional is NVIDIA's own case");
    require(near(method_cost(results, settings, Method::fidelity_fx).cost_ms, 0.6),
            "bidirectional does not apply to FidelityFX");
    settings.nvidia_bidirectional = false;
    require(near(method_cost(results, settings, Method::fidelity_fx, {.input_scale = 100}).cost_ms, 1.3),
            "a submenu entry asks about another scale");
    require(method_cost(results, settings, Method::nvidia_slow).status == CaseStatus::unavailable &&
                method_cost(results, settings, Method::nvidia_slow).note == "No NVIDIA GPU",
            "an unavailable method says why");
    require(method_cost(results, settings, Method::nvidia_fast).status == CaseStatus::not_run,
            "a case that did not run");
    const auto triple = method_cost(results, settings, Method::nvidia_medium, {.triple = true});
    require(triple.status == CaseStatus::ok && triple.estimated && near(triple.cost_ms, 3.2),
            "3X adds one composition pass to an OFXR method");
    const auto native67 = method_cost(results, settings, Method::native_dlss);
    require(native67.status == CaseStatus::ok && near(native67.cost_ms, 1.8) && !native67.estimated,
            "native includes the guide copy");
    require(near(method_cost(results, settings, Method::native_dlss, {.triple = true}).cost_ms, 2.6),
            "native 3X is measured");
    require(near(ofxr_mode_cost(results, settings, OfxrMode::interpolate).cost_ms, 0.6) &&
                near(ofxr_mode_cost(results, settings, OfxrMode::hybrid).cost_ms, 1.7),
            "modes in DLSS games include the guide copy");
    require(ofxr_mode_cost(results, settings, OfxrMode::extrapolate).status == CaseStatus::not_run,
            "no extrapolation result");
    // Extrapolation runs FidelityFX whatever the list says; the hybrid only
    // in games with DLSS vectors, so the list keeps its own costs.
    LauncherSettings hybrid = settings;
    apply_ofxr_mode(hybrid, OfxrMode::hybrid);
    require(near(method_cost(results, hybrid, Method::nvidia_medium).cost_ms, 2.9),
            "under the hybrid an NVIDIA choice costs NVIDIA's flow");
    LauncherSettings extrapolating = settings;
    apply_ofxr_mode(extrapolating, OfxrMode::extrapolate);
    results.set({"extrapolate_ffx_50", CaseStatus::ok, 450, 440, 32, ""});
    require(near(method_cost(results, extrapolating, Method::nvidia_slow).cost_ms, 0.45) &&
                near(method_cost(results, extrapolating, Method::fidelity_fx).cost_ms, 0.45),
            "extrapolating, any flow choice costs FidelityFX extrapolation");
    require(method_cost(results, extrapolating, Method::native_dlss).status == CaseStatus::ok,
            "native generation does not change with the OFXR mode");
    require(menu_annotation(medium, 90.0, false) == L"2.9 ms \u00B7 up to +74%",
            "annotation from settings");
    require(menu_annotation(method_cost(results, settings, Method::nvidia_slow), 90.0, false) ==
                L"not available on this PC" &&
                menu_annotation(method_cost(results, settings, Method::nvidia_fast), 90.0, false).empty(),
            "annotations for missing results");
}

} // namespace

int main() {
    try {
        catalogue();
        results_file();
        speed_up_model();
        headset_guesses();
        tray_lookups();
        std::cout << "benchmark model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
