#pragma once

// "Benchmark this PC": GPU time of each frame-generation method through the
// synthesizer the layer uses, on this PC's GPU at a headset's per-eye size.
// The scene is the frame-generation bench's (tests/d3d12_history_tests.cpp,
// XRFG_TEST_FG_BENCH): a detailed stereo pattern moving 12 pixels a frame
// under a turning head, with the game's guides at two thirds of the eye
// (DLSS Quality). Unlike the bench's, the guides follow the frames' motion,
// except over a fifth of the frame that moves without vectors. Each case's
// time is the median of its pairs' GPU spans, synthesis and the real frame's
// copy together.

#include "xrfg/benchmark_model.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xrfg::fg_benchmark {

struct AdapterInfo {
    std::wstring name;
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    // IDXGIAdapter::CheckInterfaceSupport's user-mode driver version.
    std::uint64_t driver_version{};
    std::uint64_t dedicated_video_memory{};
    bool software{};
};
inline constexpr std::uint32_t kNvidiaVendorId = 0x10de;

// "32.0.15.8180".
[[nodiscard]] std::string driver_version_text(std::uint64_t version);
// "581.80": NVIDIA's own number, from the last five digits. Empty for others.
[[nodiscard]] std::string nvidia_driver_text(std::uint32_t vendor_id, std::uint64_t version);
// The adapter games run on: the high-performance one, as games pick it.
[[nodiscard]] HRESULT high_performance_adapter(AdapterInfo* info) noexcept;

struct Options {
    std::uint32_t eye_width{2064};
    std::uint32_t eye_height{2208};
    std::uint32_t warmup_pairs{8};
    std::uint32_t measured_pairs{32};
    // WARP instead of the hardware adapter, for tests without a GPU.
    bool use_warp{};
    // Empty runs every standard case, in their order.
    std::vector<std::string> keys;
    // The game's frame interval the guides report (2X at this refresh).
    double refresh_hz{90.0};
    // Where nvngx_dlssg.dll must be for native generation: NGX searches the
    // folder of the module that initialises it, which is this tool's own.
    std::filesystem::path module_directory;
    // How long one GPU wait may take before the GPU counts as hung.
    std::uint32_t gpu_timeout_ms{15000};
};

struct Callbacks {
    std::function<void(const AdapterInfo&)> adapter;
    std::function<void(std::size_t index, std::size_t count, const benchmark::CaseSpec&)> begin;
    std::function<void(const benchmark::CaseResult&)> result;
};

// Runs each case and reports its answer: ok with times, unavailable with a
// reason (no NVIDIA GPU, no DLSS FG on this one, ...) or failed. Fails as a
// whole only when the device cannot be created or stops responding.
[[nodiscard]] HRESULT run(const Options& options, const Callbacks& callbacks,
                          std::wstring* error) noexcept;

} // namespace xrfg::fg_benchmark
