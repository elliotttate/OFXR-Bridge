#pragma once

// What "Benchmark this PC" measures and how its numbers become the tray's
// labels. Pure data and arithmetic, shared by the OFXRBenchmark tool that
// measures and the tray that shows the results; no Windows or D3D12 here.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xrfg::benchmark {

// One measured configuration of the synthesizer.
enum class CaseKind {
    flow,             // OFXR interpolating from optical flow alone: any game
    flow_extrapolate, // SpaceWarp-style prediction from FidelityFX flow: no vectors
    vectors,          // OFXR interpolating from the game's DLSS motion vectors
    hybrid,           // the game's vectors and FidelityFX flow, per pixel
    extrapolate,      // SpaceWarp-style prediction from the vectors and depth
    native,           // NVIDIA DLSS Frame Generation through NGX
    guide_snapshot,   // the copy of the game's vectors and depth, per game frame
};
// Whether a kind runs on the game's DLSS vectors (and so needs their copy).
[[nodiscard]] bool uses_game_guides(CaseKind kind) noexcept;
// Whether a kind predicts past the real frame rather than interpolating.
[[nodiscard]] bool extrapolates(CaseKind kind) noexcept;
enum class CaseBackend { fidelity_fx, nvidia };
enum class CasePreset { fast, medium, slow };

struct CaseSpec {
    std::string_view key;
    std::string_view name;
    CaseKind kind{};
    CaseBackend backend{CaseBackend::fidelity_fx};
    CasePreset preset{CasePreset::medium};
    int input_scale{50};       // percent per axis; flow and hybrid only
    bool bidirectional{};      // NVIDIA flow only
    int native_scale{100};     // percent per axis; native only
    int generated_frames{1};   // 1 for 2X, 2 for 3X
};

// Every case, in the order the tool runs them: the flow first, NGX last,
// since a driver fault there should cost the fewest results.
[[nodiscard]] std::span<const CaseSpec> standard_cases() noexcept;
[[nodiscard]] const CaseSpec* find_case(std::string_view key) noexcept;

// The keys the tray looks results up by.
[[nodiscard]] std::string flow_case_key(
    CaseBackend backend, CasePreset preset, int input_scale, bool bidirectional);
[[nodiscard]] std::string hybrid_case_key(int input_scale);
[[nodiscard]] std::string flow_extrapolate_case_key(int input_scale);
[[nodiscard]] std::string native_case_key(int native_scale, bool triple);
inline constexpr std::string_view kVectorsKey = "vectors";
inline constexpr std::string_view kExtrapolateKey = "extrapolate";
inline constexpr std::string_view kGuideSnapshotKey = "guide_snapshot";
// FidelityFX flow at 50% for 3X: its difference from ffx_50 is what one more
// composition pass costs, which is how 3X is estimated for the other OFXR
// methods.
inline constexpr std::string_view kOfxrTripleReferenceKey = "ffx_50_3x";
inline constexpr std::string_view kOfxrTripleBaseKey = "ffx_50";

enum class CaseStatus { not_run, ok, unavailable, failed };
[[nodiscard]] std::string_view status_name(CaseStatus status) noexcept;
[[nodiscard]] CaseStatus parse_status(std::string_view text) noexcept;

struct CaseResult {
    std::string key;
    CaseStatus status{CaseStatus::not_run};
    double median_us{};
    double p10_us{};
    std::uint32_t samples{};
    // Why a case is unavailable or failed, in words for the user.
    std::string note;
};

struct Results {
    // Bumped when the meaning of a stored number changes; older files are
    // then offered for a re-run instead of read.
    static constexpr int kFormat = 1;
    int format{kFormat};
    // Whether every case ran to an answer. A run the tool did not finish
    // keeps what it measured; last_started names the case it was in.
    bool complete{};
    std::string last_started;
    std::string gpu;
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::string driver;
    std::uint32_t eye_width{};
    std::uint32_t eye_height{};
    // The headset's refresh rate the estimates are for. Not part of the
    // measurement: changing it only changes the arithmetic.
    double refresh_hz{90.0};
    std::string date;
    // The first case measured again at the end, over its first time: well
    // away from 1 means something else used the GPU during the run. 0 when
    // it was not checked.
    double drift{};
    std::vector<CaseResult> cases;

    // Whether the GPU's load changed enough during the run to doubt it.
    [[nodiscard]] bool drifted() const noexcept;

    [[nodiscard]] const CaseResult* find(std::string_view key) const noexcept;
    // The case's median in milliseconds, when it ran.
    [[nodiscard]] std::optional<double> cost_ms(std::string_view key) const noexcept;
    void set(CaseResult result);
};

[[nodiscard]] Results parse_results(std::string_view text);
[[nodiscard]] std::string serialize_results(const Results& results);

// The speed-up model. With frame generation the headset shows
// `frames_per_game_frame` frames (2 for 2X, 3 for 3X) for every frame the
// game renders, so at R Hz the game has frames_per_game_frame / R seconds per
// frame. Generation runs on the same GPU, so its cost comes out of that
// budget. The game reaches full refresh when its own GPU time per frame is at
// most budget - cost; the frame rate it needs on its own is the inverse of
// that, and showing R frames from it is the best speed-up the method gives.
// A game faster than that gains less (nothing passes R), and one slower
// cannot hold R with this method.
struct Estimate {
    double cost_ms{};
    double refresh_hz{};
    int frames_per_game_frame{2};
    double display_period_ms{};
    double budget_ms{};
    // cost_ms / budget_ms.
    double budget_share{};
    double max_game_ms{};
    // The game's own frame rate, without frame generation, that reaches R.
    double min_game_fps{};
    // R / min_game_fps - 1: the speed-up for a game running at exactly
    // min_game_fps. frames_per_game_frame - 1 - cost / period.
    double max_gain{};
    // Whether any game can reach R at all: budget > cost.
    bool fits{};
};
[[nodiscard]] Estimate estimate(double cost_ms, double refresh_hz, int frames_per_game_frame);

// Average error where the scene moved, on 42 recorded Galactic Racer frame
// triplets (lower is better), for the configurations it was measured at.
// Nothing for configurations it was not measured at, and nothing for
// extrapolation, which predicts the next frame instead of interpolating one.
[[nodiscard]] std::optional<double> recorded_error(const CaseSpec& spec) noexcept;
inline constexpr double kRecordedBlendError = 13.0;
// "best", "very good", "good", "fair" for an error from recorded_error.
[[nodiscard]] std::wstring quality_word(double error);

// The headset's per-eye render resolution and refresh rate, as far as a
// source tells them.
struct HeadsetGuess {
    std::uint32_t eye_width{};
    std::uint32_t eye_height{};
    std::optional<double> refresh_hz;
};
// From a flight-recorder log: the runtime's recommended per-eye size (the
// view_configuration record) and the shortest display period the runtime
// reported to the layer's own waits (internal_wait_frame), which at worst is
// throttled to half and is otherwise the refresh. The log's head holds the
// session's start-up records, so its first megabytes are enough.
[[nodiscard]] std::optional<HeadsetGuess> headset_from_flight_log(std::string_view text);
// From [headset] eye_width, eye_height and refresh_hz.
[[nodiscard]] std::optional<HeadsetGuess> headset_from_ini(std::string_view text);
// To the nearest whole rate, or to the nearest common one within 1 Hz.
[[nodiscard]] double snap_refresh(double hz) noexcept;

inline constexpr std::uint32_t kNvidiaVendorId = 0x10de;
// IDXGIAdapter::CheckInterfaceSupport's driver version: "32.0.15.8180".
[[nodiscard]] std::string driver_version_text(std::uint64_t version);
// NVIDIA's own number for it, "581.80" from 32.0.15.8180: the last digit of
// the third part and the fourth. Empty for another vendor.
[[nodiscard]] std::string nvidia_driver_text(std::uint32_t vendor_id, std::string_view version);

// "0.59 ms", "12.4 ms": two significant decimals under 1 ms, one above.
[[nodiscard]] std::wstring format_ms(double milliseconds);
// "0.6 ms", a middle dot and "up to +95%"; or "too slow for 90 Hz" after the
// dot when no game can gain at that rate.
[[nodiscard]] std::wstring short_annotation(const Estimate& estimate);
// Latency a method adds to the real frames, in display frames: 0 for
// extrapolation, frames_per_game_frame - 1 for interpolation.
[[nodiscard]] int added_latency_frames(CaseKind kind, int frames_per_game_frame) noexcept;

} // namespace xrfg::benchmark
