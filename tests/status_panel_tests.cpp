#include "xrfg/status_panel_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }

xrfg::Quaternion about(float x, float y, float z, float degrees) {
    const float half = degrees * 3.14159265358979323846f / 360.0f;
    return {x * std::sin(half), y * std::sin(half), z * std::sin(half), std::cos(half)};
}

xrfg::Quaternion times(const xrfg::Quaternion& a, const xrfg::Quaternion& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

xrfg::Vec3 turn(const xrfg::Quaternion& q, xrfg::Vec3 v) {
    const xrfg::Quaternion p{v.x, v.y, v.z, 0.0f};
    const auto r = times(times(q, p), {-q.x, -q.y, -q.z, q.w});
    return {r.x, r.y, r.z};
}

bool near(float a, float b, float tolerance = 1.0e-4f) { return std::abs(a - b) < tolerance; }

bool has_line(const xrfg::StatusPanelText& text, const std::string& fragment, xrfg::PanelTone tone) {
    return std::any_of(text.lines.begin(), text.lines.end(), [&](const xrfg::PanelLine& line) {
        return line.text.find(fragment) != std::string::npos && line.tone == tone;
    });
}

const xrfg::PanelLine& line_labelled(const xrfg::StatusPanelText& text, const std::string& label) {
    for (const auto& line : text.lines) if (line.label == label) return line;
    throw std::runtime_error(("no line labelled " + label).c_str());
}

// What the tray's defaults ask for: the hybrid, 2X, the deeper pipeline,
// NVIDIA medium flow at half resolution for games without vectors.
xrfg::PanelMethod tray_default() {
    xrfg::PanelMethod method;
    method.flow = xrfg::PanelFlow::nvidia;
    method.preset = xrfg::PanelPreset::medium;
    method.flow_scale = 50;
    method.game_vectors = true;
    method.hybrid = true;
    method.deep = true;
    return method;
}

xrfg::StatusPanelInput generating(const xrfg::PanelMethod& asked, const xrfg::PanelMethod& running) {
    xrfg::StatusPanelInput input;
    input.asked = asked;
    input.session = asked;
    input.running = running;
    input.rates = {90.0f, 45.0f, 45.0f, 0.0f, 90.0f, -1.0f, true};
    input.vectors_published = true;
    input.vectors.published = 4000;
    input.vectors.status = xrfg::PanelVectorStatus::used;
    input.vectors.used = 11;
    input.presenter = true;
    input.runtime = "SteamVR/OpenXR";
    return input;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        using namespace xrfg;
        // The setting, and what an unknown value means: the default.
        for (auto mode : {StatusPanelMode::off, StatusPanelMode::gesture, StatusPanelMode::always})
            require(parse_status_panel_mode(status_panel_mode_name(mode)) == mode, "mode round trip");
        require(parse_status_panel_mode("") == StatusPanelMode::gesture &&
                parse_status_panel_mode("sideways") == StatusPanelMode::gesture, "unknown mode is the default");

        // Upside down is the grip's up axis past 120 degrees from straight
        // up, whichever way the controller got there, and only that.
        require(!grip_upside_down({}), "upright is not flipped");
        require(grip_upside_down(about(0, 0, 1, 180)), "rolled over is flipped");
        require(grip_upside_down(about(1, 0, 0, 180)), "pitched over is flipped");
        require(!grip_upside_down(about(0, 0, 1, 119)) && grip_upside_down(about(0, 0, 1, 121)),
                "the 120 degree threshold");
        require(!grip_upside_down(about(1, 0, 0, 90)) && !grip_upside_down(about(1, 0, 0, -90)),
                "pointing straight up or down is not upside down");
        require(!grip_upside_down(about(0, 1, 0, 180)), "turning around is not upside down");
        auto scaled = about(0, 0, 1, 150);
        scaled.x *= 3; scaled.y *= 3; scaled.z *= 3; scaled.w *= 3;
        require(grip_upside_down(scaled), "an unnormalized pose still reads");
        require(!grip_upside_down({0, 0, 0, 0}), "a zero quaternion is not flipped");

        // The hysteresis: a quarter second turned over to show, half a
        // second back to hide, and a break in either restarts it.
        constexpr std::int64_t ms = 1'000'000;
        FlipGesture gesture;
        require(!gesture.observe(0, true, false) && gesture.pending(), "shown at once");
        require(!gesture.observe(240 * ms, true, false), "shown before a quarter second");
        require(gesture.observe(250 * ms, true, false) && !gesture.pending() && gesture.hand() == 0,
                "not shown after a quarter second on the left hand");
        require(gesture.observe(300 * ms, false, false), "hidden at once when turned back");
        require(gesture.observe(790 * ms, false, false), "hidden before half a second");
        require(gesture.observe(795 * ms, true, false), "turned over again during the hide delay");
        require(gesture.observe(1200 * ms, false, false) && gesture.observe(1690 * ms, false, false),
                "the hide delay did not restart");
        require(!gesture.observe(1700 * ms, false, false) && gesture.hand() == -1,
                "not hidden after half a second back up");
        gesture.reset();
        gesture.observe(0, false, true);
        gesture.observe(100 * ms, false, true);
        gesture.observe(120 * ms, false, false);
        require(!gesture.observe(130 * ms, false, true) && !gesture.observe(370 * ms, false, true),
                "a break did not restart the show delay");
        require(gesture.observe(380 * ms, false, true) && gesture.hand() == 1, "right hand");
        require(gesture.observe(400 * ms, true, true) && gesture.hand() == 1,
                "the panel moved hands while its own stayed turned");
        require(gesture.observe(420 * ms, true, false) && gesture.hand() == 0,
                "the other turned hand did not take it over");
        require(gesture.observe(450 * ms, false, false) && gesture.hand() == 0,
                "the hand is kept through the hide delay");
        FlipGesture untracked;
        for (std::int64_t t = 0; t < 2000; t += 11) untracked.observe(t * ms, false, false);
        require(!untracked.visible() && !untracked.pending(), "nothing turned, nothing shown");

        // On the flipped controller: upright, facing back along it, lifted
        // clear of it - whatever way the controller points.
        for (float yaw : {0.0f, 90.0f, -135.0f}) {
            Pose grip{times(about(0, 1, 0, yaw), about(0, 0, 1, 180)), {0.1f, 1.0f, -0.3f}};
            const Pose panel = panel_pose_on_grip(grip, 0.2f);
            const Vec3 up = turn(panel.orientation, {0, 1, 0});
            const Vec3 normal = turn(panel.orientation, {0, 0, 1});
            const Vec3 back = turn(grip.orientation, {0, 0, 1});
            require(near(up.y, 1.0f), "panel is not upright on a flipped controller");
            require(near(normal.x, back.x) && near(normal.z, back.z), "panel does not face back along the grip");
            require(near(panel.position.x, 0.1f) && near(panel.position.z, -0.3f) &&
                    near(panel.position.y, 1.0f + 0.15f), "panel not lifted half its height and 5 cm");
        }

        // What is asked against what runs. The tray's default in a game
        // that has given no DLSS vectors yet: the hybrid waits, and the
        // chosen flow runs meanwhile.
        auto running = tray_default();
        running.hybrid = false;
        auto input = generating(tray_default(), running);
        input.vectors_published = false;
        input.vectors = {PanelVectorStatus::waiting_for_dlss, 0, 0, 0, 0};
        auto text = status_panel_text(input);
        require(text.state == "FALLBACK" && text.state_tone == PanelTone::warn, "waiting hybrid is a fallback");
        require(line_labelled(text, "Asked").text == "Hybrid: DLSS vectors + FidelityFX flow, 2X, deep",
                "asked hybrid text");
        require(line_labelled(text, "Running").text == "NVIDIA flow medium 50%, 2X, deep" &&
                line_labelled(text, "Running").tone == PanelTone::warn, "running flow text");
        require(line_labelled(text, "Why").text == "The hybrid waits for the game's first DLSS vectors",
                "why the hybrid is not running");
        require(line_labelled(text, "Vectors").text == "none from this game yet", "no vectors yet");

        // Once the game's vectors arrive, the hybrid runs at a quarter.
        running = tray_default();
        running.flow = PanelFlow::fidelity_fx;
        running.flow_scale = 25;
        input = generating(tray_default(), running);
        input.vectors.used = 30;
        text = status_panel_text(input);
        require(text.state == "GENERATING" && text.state_tone == PanelTone::good, "hybrid running is fine");
        require(line_labelled(text, "Running").text == "Hybrid: DLSS vectors + FidelityFX flow, 2X, deep" &&
                line_labelled(text, "Running").tone == PanelTone::good, "hybrid running text");
        require(std::none_of(text.lines.begin(), text.lines.end(),
                             [](const PanelLine& line) { return line.label == "Why"; }),
                "a reason given while nothing differs");
        require(line_labelled(text, "Vectors").text == "used on 100% of pairs", "vector share");
        require(line_labelled(text, "Latency").text == "+2 frames (22 ms) added", "deep 2X latency at 90 Hz");
        require(line_labelled(text, "Pipeline").text == "deep, presenter thread", "pipeline text");
        require(line_labelled(text, "GPU").text == "shown while the flight recorder is on" &&
                line_labelled(text, "GPU").tone == PanelTone::dim, "GPU without timing");
        input.gpu_ms = 1.843f;
        input.gpu_timing = true;
        require(line_labelled(status_panel_text(input), "GPU").text == "1.84 ms a pair", "GPU time");
        input.vectors.used = 27;
        input.vectors.temporal_rejections = 3;
        text = status_panel_text(input);
        require(line_labelled(text, "Vectors").text == "used on 90% of pairs, 3 refused" &&
                line_labelled(text, "Vectors").tone == PanelTone::good, "partial vector share");

        // Vectors that stop matching: the interpolation falls back on flow.
        auto asked = tray_default();
        asked.hybrid = false;
        running = asked;
        input = generating(asked, running);
        input.vectors = {PanelVectorStatus::temporal_mismatch, 4000, 0, 12, 0};
        text = status_panel_text(input);
        require(line_labelled(text, "Asked").text == "DLSS vectors, 2X, deep", "asked vectors text");
        require(line_labelled(text, "Running").text == "NVIDIA flow medium 50%, 2X, deep", "fallen back to flow");
        require(line_labelled(text, "Why").text == "The game's DLSS vectors did not match the frames" &&
                line_labelled(text, "Why").tone == PanelTone::warn, "temporal mismatch reason");
        require(line_labelled(text, "Vectors").tone == PanelTone::warn, "refusals are a warning");

        // NVIDIA optical flow asked on a machine without it.
        asked = PanelMethod{};
        asked.flow = PanelFlow::nvidia;
        asked.preset = PanelPreset::fast;
        running = PanelMethod{};
        input = generating(asked, running);
        input.nvidia_unavailable = true;
        text = status_panel_text(input);
        require(line_labelled(text, "Asked").text == "NVIDIA flow fast 50%, 2X", "asked NVIDIA");
        require(line_labelled(text, "Running").text == "FidelityFX flow 50%, 2X", "running FidelityFX");
        require(has_line(text, "NVIDIA optical flow is unavailable here", PanelTone::warn), "NVIDIA fallback");
        require(std::none_of(text.lines.begin(), text.lines.end(),
                             [](const PanelLine& line) { return line.label == "Vectors"; }),
                "a vector line without vectors asked for");

        // The deeper pipeline lost to the runtime's swapchain limit, and 3X.
        asked = tray_default();
        running = asked;
        running.deep = false;
        input = generating(asked, running);
        input.shallow_fallback = true;
        text = status_panel_text(input);
        require(has_line(text, "refused a fourth swapchain: shallow pipeline", PanelTone::warn), "shallow fallback");
        require(text.state == "FALLBACK", "shallow fallback is a fallback");
        // The session started shallow and the tray now asks for deep: a
        // change that waits for the next start, not a fallback.
        input.shallow_fallback = false;
        input.session.deep = false;
        require(has_line(status_panel_text(input), "newer choice starts with the next game start", PanelTone::dim) &&
                status_panel_text(input).state == "GENERATING", "a changed setting is not a fallback");
        asked.frames = 3;
        running = asked;
        running.frames = 2;
        input = generating(asked, running);
        input.triple_fixed = true;
        text = status_panel_text(input);
        require(line_labelled(text, "Asked").text == "Hybrid: DLSS vectors + FidelityFX flow, 3X",
                "3X runs shallow whatever the setting says");
        require(has_line(text, "3X needs a game restart", PanelTone::warn), "3X fixed");
        input.triple_fixed = false;
        input.graphics = PanelGraphics::d3d11_interop;
        require(has_line(status_panel_text(input), "not available through the D3D11 interop", PanelTone::warn),
                "3X through the interop");

        // Native DLSS Frame Generation.
        asked = PanelMethod{};
        asked.native = true;
        asked.native_scale = 67;
        asked.frames = 3;
        running = asked;
        running.frames = 2;
        input = generating(asked, running);
        input.native_single_frame = true;
        text = status_panel_text(input);
        require(line_labelled(text, "Running").text == "DLSS Frame Generation 67%, 2X", "native running");
        require(has_line(text, "makes one frame: 2X", PanelTone::warn), "native single frame");
        require(line_labelled(text, "Guides").text == "used on 100% of pairs", "native guides");
        input.native_available = false;
        input.vectors = {PanelVectorStatus::native_unavailable, 4000, 0, 0, 0};
        text = status_panel_text(input);
        require(text.state == "NOT GENERATING" && line_labelled(text, "Running").text == "Real frames only",
                "native that makes nothing is not generating");
        require(has_line(text, "unavailable on this GPU or driver", PanelTone::bad), "native unavailable");
        input.native_available = true;
        input.vectors = {PanelVectorStatus::waiting_for_depth, 4000, 0, 0, 0};
        require(has_line(status_panel_text(input), "Waiting for the game's depth", PanelTone::warn), "depth wait");

        // Every pair skipped for guides that do not fit (reported in Star Wars
        // Jedi: Survivor under UEVR on Virtual Desktop at 90 Hz): the pairs
        // still go out, a copy of each real frame before it, so the counter
        // has them as repeats, and the real frames are held as long as when
        // they are generated. The panel said "Waiting for the game's DLSS
        // guides", 45 generated a second and no latency added.
        asked = PanelMethod{};
        asked.native = true;
        asked.native_scale = 67;
        asked.deep = true;
        input = generating(asked, asked);
        input.runtime = "VirtualDesktopXR";
        input.rates = {45.0f, 45.0f, 0.0f, 45.0f, 90.0f, -1.0f, true};
        input.vectors = {PanelVectorStatus::temporal_mismatch, 4000, 0, 0, 0};
        text = status_panel_text(input);
        require(text.state == "NOT GENERATING" && line_labelled(text, "Running").text == "Real frames only",
                "native pairs with refused guides are not generating");
        require(line_labelled(text, "Why").text == "The game's DLSS guides did not match the frames" &&
                    line_labelled(text, "Why").tone == PanelTone::warn,
                "refused native guides read as still awaited");
        require(line_labelled(text, "Guides").text == "refused: they did not match the frames", "refused guides");
        require(line_labelled(text, "Latency").text == "+2 frames (22 ms) added",
                "skipped native pairs still hold the real frame");
        require(line_labelled(text, "Frames").text == "45.0 repeats/s", "the copies are repeats");
        input.vectors = {PanelVectorStatus::no_matching_output, 4000, 0, 0, 0};
        text = status_panel_text(input);
        require(line_labelled(text, "Why").text == "The game's DLSS output is not the eye images' size" &&
                    line_labelled(text, "Guides").text == "unused: no DLSS output is the eye images' size",
                "eye images no DLSS output fits");
        input.vectors = {PanelVectorStatus::queue_mismatch, 4000, 0, 0, 0};
        require(line_labelled(status_panel_text(input), "Why").text == "The game's DLSS ran on another queue",
                "native guides on another queue");
        input.vectors = {PanelVectorStatus::output_not_direct, 4000, 0, 0, 0};
        require(line_labelled(status_panel_text(input), "Why").text == "Waiting for the game's DLSS guides",
                "native guides not used yet");
        // OFXR's own methods name the size mismatch too.
        input = generating(tray_default(), tray_default());
        input.running.hybrid = false;
        input.session.hybrid = input.asked.hybrid = false;
        input.running.game_vectors = true;
        input.vectors = {PanelVectorStatus::no_matching_output, 4000, 0, 0, 0};
        require(line_labelled(status_panel_text(input), "Why").text ==
                    "DLSS vectors unused: no DLSS output is the eye images' size",
                "OFXR vectors with no DLSS output of the eye images' size");

        // Extrapolation: no latency added, and FidelityFX flow without vectors.
        asked = tray_default();
        asked.extrapolate = 1;
        asked.hybrid = false;
        running = asked;
        running.deep = false;
        running.flow = PanelFlow::fidelity_fx;
        input = generating(asked, running);
        text = status_panel_text(input);
        require(line_labelled(text, "Running").text == "Extrapolate, DLSS vectors and depth, 2X" &&
                text.state == "GENERATING", "extrapolation running");
        require(line_labelled(text, "Latency").text == "none added", "extrapolation adds no latency");
        input.vectors = {PanelVectorStatus::waiting_for_dlss, 0, 0, 0, 0};
        text = status_panel_text(input);
        require(line_labelled(text, "Running").text == "Extrapolate, FidelityFX flow, 2X" &&
                has_line(text, "No DLSS vectors from this game", PanelTone::warn), "extrapolation from flow");
        input.asked.extrapolate = 0;
        require(has_line(status_panel_text(input), "newer choice starts with the next game start", PanelTone::dim),
                "extrapolation is fixed for the session");

        // The tray switched to DLSS Frame Generation mid-game: the session
        // runs on what it started with, and says the change is pending.
        asked = PanelMethod{};
        asked.native = true;
        input = generating(asked, tray_default());
        input.session = tray_default();
        input.running.flow = PanelFlow::fidelity_fx;
        input.running.flow_scale = 25;
        text = status_panel_text(input);
        require(text.state == "GENERATING" && line_labelled(text, "Asked").text == "DLSS Frame Generation 67%, 2X" &&
                line_labelled(text, "Running").text == "Hybrid: DLSS vectors + FidelityFX flow, 2X, deep" &&
                has_line(text, "newer choice starts with the next game start", PanelTone::dim) &&
                line_labelled(text, "Vectors").tone == PanelTone::good, "a pending method is not a fallback");
        // 3X follows the tray at once: no pending change for it alone.
        asked = tray_default();
        asked.frames = 3;
        input = generating(asked, asked);
        input.session.frames = 2;
        text = status_panel_text(input);
        require(text.state == "GENERATING" &&
                std::none_of(text.lines.begin(), text.lines.end(),
                             [](const PanelLine& line) { return line.text.find("newer") != std::string::npos; }),
                "3X counted as a pending change");

        // Held back: paused, off, out of swapchains, or passing frames through.
        input = generating(tray_default(), tray_default());
        input.paused = true;
        input.rates.generating = false;
        input.rates.generated = 0;
        text = status_panel_text(input);
        require(text.state == "PAUSED" && has_line(text, "Paused from the tray", PanelTone::warn), "paused");
        require(line_labelled(text, "Latency").text == "none added", "paused adds no latency");
        input.paused = false;
        input.enabled = false;
        input.settings_failed = true;
        text = status_panel_text(input);
        require(text.state == "OFF" && has_line(text, "settings change failed", PanelTone::bad), "failed settings");
        input.enabled = true;
        input.budget_exhausted = true;
        require(status_panel_text(input).state == "OFF" &&
                has_line(status_panel_text(input), "refused a swapchain", PanelTone::bad), "budget exhausted");
        input.budget_exhausted = false;
        input.bypass = PanelBypass::quarantine;
        text = status_panel_text(input);
        require(text.state == "NOT GENERATING" && text.state_tone == PanelTone::bad &&
                has_line(text, "Holding off for a second", PanelTone::warn), "quarantine");
        input.declined_images = 2;
        require(has_line(status_panel_text(input), "2 eye images could not be set up", PanelTone::warn),
                "declined images");

        // Frames and session lines.
        input = generating(tray_default(), tray_default());
        input.rates.repeats = 3.3f;
        input.rates.delivered = 88.6f;
        input.promise_periods = 1;
        input.graphics = PanelGraphics::d3d11_bridge;
        text = status_panel_text(input);
        require(line_labelled(text, "Frames").text == "3.3 repeats/s, headset got 88.6/s", "frames line");
        require(line_labelled(text, "Latency").text == "+2 frames (22 ms) added, promise +1", "promise");
        require(line_labelled(text, "Session").text == "D3D11 through the D3D12 bridge on SteamVR/OpenXR",
                "session line");

        // The pixels: the panel's size only, cut to its lines, premultiplied,
        // rounded corners, and the colours converted for a linear swapchain.
        require(rasterize_status_panel(100, 50, input, false, false).empty(), "a size it cannot lay out");
        std::uint32_t used = 0;
        const auto image =
            rasterize_status_panel(kStatusPanelWidth, kStatusPanelHeight, input, false, false, &used);
        require(image.size() == static_cast<std::size_t>(kStatusPanelWidth) * kStatusPanelHeight, "panel size");
        // Eight lines: 136 to the first, 22 a line, 8 below the last.
        require(used == 136 + 8 * 22 + 8, "panel not cut to its lines");
        const auto used_end = image.begin() + static_cast<std::ptrdiff_t>(used) * kStatusPanelWidth;
        require(std::all_of(used_end, image.end(), [](std::uint32_t pixel) { return pixel == 0; }),
                "below the panel must be transparent");
        require(image.front() == 0 && *(used_end - 1) == 0, "corners must be transparent");
        require((image[(used - 1) * kStatusPanelWidth + 600] >> 24) == 255, "the border must close the panel");
        const auto opaque = std::count_if(image.begin(), used_end, [](std::uint32_t pixel) {
            return (pixel >> 24) >= 200;
        });
        require(opaque > (used_end - image.begin()) * 95 / 100, "background must cover the panel");
        auto busy = input;
        busy.paused = true;
        busy.declined_images = 1;
        busy.rates.generating = false;
        std::uint32_t busy_used = 0;
        static_cast<void>(rasterize_status_panel(kStatusPanelWidth, kStatusPanelHeight, busy, false, false,
                                                 &busy_used));
        require(busy_used == used + 2 * 22, "two reasons must add two lines");
        for (const std::uint32_t pixel : image) {
            const std::uint32_t alpha = pixel >> 24;
            require((pixel & 0xffu) <= alpha && ((pixel >> 8) & 0xffu) <= alpha &&
                    ((pixel >> 16) & 0xffu) <= alpha, "colour exceeds alpha: not premultiplied");
        }
        // The headline's green (92, 224, 142) is drawn somewhere.
        const std::uint32_t green = 0xff000000u | 92u | (224u << 8) | (142u << 16);
        require(std::count(image.begin(), image.end(), green) > 100, "GENERATING is not drawn in green");
        const auto bgra = rasterize_status_panel(kStatusPanelWidth, kStatusPanelHeight, input, true, false);
        const std::uint32_t green_bgra = 0xff000000u | 142u | (224u << 8) | (92u << 16);
        require(std::count(bgra.begin(), bgra.end(), green_bgra) ==
                std::count(image.begin(), image.end(), green), "BGRA order");
        const auto linear = rasterize_status_panel(kStatusPanelWidth, kStatusPanelHeight, input, false, true);
        const std::size_t middle = static_cast<std::size_t>(used - 4) * kStatusPanelWidth + 600;
        require((image[middle] >> 24) == (linear[middle] >> 24) &&
                (linear[middle] & 0xffu) < (image[middle] & 0xffu), "linear background must be darker in value");
        // Long text wraps rather than running off the panel.
        auto wordy = input;
        wordy.runtime = std::string(80, 'x');
        require(rasterize_status_panel(kStatusPanelWidth, kStatusPanelHeight, wordy, false, false).size() ==
                image.size(), "long text");

        // Optional preview of a fallback and of the default running well.
        if (argc > 1) {
            auto fallback = generating(tray_default(), tray_default());
            fallback.running.hybrid = false;
            fallback.vectors_published = false;
            fallback.vectors = {PanelVectorStatus::waiting_for_dlss, 0, 0, 0, 0};
            fallback.rates = {86.0f, 43.0f, 43.0f, 1.2f, 90.0f, 85.9f, true};
            const auto first = rasterize_status_panel(kStatusPanelWidth, kStatusPanelHeight, fallback, false, false);
            std::ofstream file(argv[1], std::ios::binary);
            file << "P6\n" << kStatusPanelWidth << " " << kStatusPanelHeight * 2 << "\n255\n";
            for (const auto* pixels : {&first, &image}) {
                for (std::size_t index = 0; index < pixels->size(); ++index) {
                    // Over a mid grey, to show the panel's transparency.
                    const std::uint32_t pixel = (*pixels)[index];
                    const std::uint32_t alpha = pixel >> 24;
                    const char rgb[]{
                        static_cast<char>((pixel & 0xffu) + (96u * (255u - alpha)) / 255u),
                        static_cast<char>(((pixel >> 8) & 0xffu) + (96u * (255u - alpha)) / 255u),
                        static_cast<char>(((pixel >> 16) & 0xffu) + (96u * (255u - alpha)) / 255u)};
                    file.write(rgb, 3);
                }
            }
        }
        std::cout << "Status panel gesture, placement, requested-against-running text and raster tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
