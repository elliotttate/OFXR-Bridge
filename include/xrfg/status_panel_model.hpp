#pragma once

// The status panel in the headset: what frame generation is running, against
// what the tray asks for, why the two differ when they do, and the frame
// rates. Pure data, text and pixels, so all of it can be tested without a
// headset; the overlay places and uploads it, and the layer fills it in.

#include "xrfg/pose.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xrfg {

// `[overlay] panel`. gesture shows the panel on a controller turned upside
// down, always shows it low in the view, off never shows it.
enum class StatusPanelMode { off, gesture, always };
[[nodiscard]] StatusPanelMode parse_status_panel_mode(std::string_view value) noexcept;
[[nodiscard]] const char* status_panel_mode_name(StatusPanelMode mode) noexcept;

// The flip gesture, as xrFPS has it: a grip whose up axis has tipped more
// than 120 degrees from straight up is upside down, a quarter second of that
// shows the panel and half a second the right way up hides it again. The
// delays keep a wrist turned in passing from flashing the panel up, and a
// controller brought level to read it from hiding it.
inline constexpr float kFlipAngleDegrees = 120.0F;
inline constexpr std::int64_t kFlipShowDelayNs = 250'000'000;
inline constexpr std::int64_t kFlipHideDelayNs = 500'000'000;

// Whether a grip pose, located in a space whose +Y is up (LOCAL or STAGE),
// is upside down: its own +Y, the controller's up, turned past the angle.
[[nodiscard]] bool grip_upside_down(
    const Quaternion& grip, float angle_degrees = kFlipAngleDegrees) noexcept;

class FlipGesture {
public:
    // One look at the controllers: whether each is tracked and upside down.
    // Returns whether the panel is shown.
    bool observe(std::int64_t now_ns, bool left_flipped, bool right_flipped) noexcept;
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    // A controller is upside down and the panel is not up yet: the caller
    // may get its content ready, so it appears with something on it.
    [[nodiscard]] bool pending() const noexcept { return !visible_ && flipped_since_ >= 0; }
    // The hand the panel sits on (0 left, 1 right, -1 none): the one that
    // was turned, kept while it stays turned even if the other one is turned
    // as well, and through the hide delay once it is not.
    [[nodiscard]] int hand() const noexcept { return hand_; }
    void reset() noexcept { *this = FlipGesture{}; }

private:
    std::int64_t flipped_since_{-1};
    std::int64_t upright_since_{-1};
    int hand_{-1};
    bool visible_{};
};

// Where the panel sits on a flipped controller, in the grip's own space
// turned into the space the grip was located in. xrFPS's placement: turned
// half a turn about the controller's length, which stands it upright facing
// back along the controller at whoever holds it, and lifted clear of the
// controller by half its height and a margin.
[[nodiscard]] Pose panel_pose_on_grip(const Pose& grip, float panel_height_m) noexcept;

// Every DlssMotionVectorStatus, in the same order: the layer converts one to
// the other by value, so the core needs no D3D12 header.
enum class PanelVectorStatus : std::uint32_t {
    disabled,
    waiting_for_dlss,
    output_not_direct,
    queue_mismatch,
    invalid_input,
    temporal_mismatch,
    used,
    waiting_for_depth,
    native_unavailable,
    multi_frame_unsupported,
};

enum class PanelFlow : std::uint8_t { fidelity_fx, nvidia };
enum class PanelPreset : std::uint8_t { slow, medium, fast };

// One frame-generation configuration: what the tray asks for, or what the
// session is running.
struct PanelMethod {
    // NVIDIA DLSS Frame Generation, and its resolution in percent of each
    // eye's. Everything below it is OFXR's own.
    bool native{};
    int native_scale{67};
    // The optical flow OFXR falls back on, or runs beside the game's vectors
    // in the hybrid; its input in percent per axis.
    PanelFlow flow{PanelFlow::fidelity_fx};
    PanelPreset preset{PanelPreset::medium};
    int flow_scale{50};
    bool both_ways{};
    // OFXR follows the game's DLSS vectors where it has them, and with the
    // hybrid runs FidelityFX flow beside them.
    bool game_vectors{};
    bool hybrid{};
    // 0 interpolates; 1 extrapolates, from the game's vectors and depth or
    // from FidelityFX flow; 2 extrapolates from both. mesh: Meta's mesh warps
    // rather than OFXR's gather.
    int extrapolate{};
    bool mesh{true};
    // Frames shown per game frame (2X or 3X), and the deeper pipeline.
    std::uint32_t frames{2};
    bool deep{};
};

// Why the layer last passed a frame through without generating.
enum class PanelBypass : std::uint8_t {
    none,
    cooldown,      // a moment after a settings change or a failed frame
    quarantine,    // a second after the game's frames changed shape
    no_projection, // nothing to generate from: no eye images, or arming
    waiting_ahead, // the game's next wait is already with the runtime
    other,
};

enum class PanelGraphics : std::uint8_t {
    d3d12,
    d3d11_bridge,
    d3d11_interop,
    vulkan_bridge,
    vulkan_interop,
    other,
};

// The DLSS guide counters since the panel's previous refresh.
struct PanelVectors {
    PanelVectorStatus status{PanelVectorStatus::waiting_for_dlss};
    // Evaluations the game has published since the process started.
    std::uint64_t published{};
    // Pairs that used the guides, and pairs that refused them, this window.
    std::uint64_t used{};
    std::uint64_t temporal_rejections{};
    std::uint64_t invalid_rejections{};
};

struct PanelRates {
    // Distinct images a second, the number the corner counter draws.
    float shown{};
    // The game's own frames and the generated ones, a second.
    float game{};
    float generated{};
    // Submissions that repeated an image already shown, a second.
    float repeats{};
    float refresh_hz{};
    // What SteamVR's compositor says the headset received, a second; below
    // zero where the runtime does not say.
    float delivered{-1.0F};
    // A generated frame went out in the last half second.
    bool generating{};
};

struct StatusPanelInput {
    PanelMethod asked;
    PanelMethod running;
    // What holds generation back, worst first.
    bool paused{};
    bool enabled{true};
    bool settings_failed{};
    bool budget_exhausted{};
    std::uint32_t declined_images{};
    PanelBypass bypass{PanelBypass::none};
    // Fallbacks the layer latched for the session.
    bool nvidia_unavailable{};
    bool vectors_published{};
    bool native_available{true};
    bool native_single_frame{};
    bool shallow_fallback{};
    bool triple_fixed{};
    PanelVectors vectors;
    PanelRates rates;
    // Synthesis GPU time a pair, smoothed; below zero when not measured,
    // which is whenever the flight recorder is off.
    float gpu_ms{-1.0F};
    bool gpu_timing{};
    // A presenter thread of the layer's own hands the frames to the runtime.
    bool presenter{};
    // Display periods the game's promised display time is moved by.
    std::uint32_t promise_periods{};
    PanelGraphics graphics{PanelGraphics::d3d12};
    std::string runtime;
};

enum class PanelTone : std::uint8_t { normal, dim, good, warn, bad, accent };

struct PanelLine {
    std::string label;
    std::string text;
    PanelTone tone{PanelTone::normal};
};

struct StatusPanelText {
    // One word or two for the panel's corner: GENERATING, FALLBACK, PAUSED,
    // OFF or NOT GENERATING.
    std::string state;
    PanelTone state_tone{PanelTone::good};
    std::vector<PanelLine> lines;
};

[[nodiscard]] std::string describe_asked(const PanelMethod& method);
[[nodiscard]] std::string describe_running(const StatusPanelInput& input);
[[nodiscard]] StatusPanelText status_panel_text(const StatusPanelInput& input);

// The panel's texture. Twice as wide as tall: a 2:1 quad shows it unscaled.
inline constexpr std::uint32_t kStatusPanelWidth = 1024;
inline constexpr std::uint32_t kStatusPanelHeight = 512;

// Premultiplied alpha. `linear` for a UNORM swapchain, which the runtime
// reads as linear light: the colours are converted, so the panel looks the
// same on an sRGB one. The panel is drawn from the top down only as far as
// its lines need, and `used_height` says how far: the quad shows that much
// of the image, so a panel with fewer lines is a smaller one rather than one
// with an empty bottom. Empty for a size it cannot lay out.
[[nodiscard]] std::vector<std::uint32_t> rasterize_status_panel(
    std::uint32_t width,
    std::uint32_t height,
    const StatusPanelInput& input,
    bool bgra,
    bool linear,
    std::uint32_t* used_height = nullptr);

}  // namespace xrfg
