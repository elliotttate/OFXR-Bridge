#pragma once
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace xrfg {

// Test-only: XRFG_TEST_CAPTURE_FRAMES=<directory> records runs of consecutive
// synthesis sources - the game's colour, its views and its DLSS guides - so a
// replay can generate a recorded frame from its neighbours and compare.
// XRFG_TEST_CAPTURE_SKIP frames pass first (600), then XRFG_TEST_CAPTURE_SEQUENCES
// runs (4) of XRFG_TEST_CAPTURE_COUNT frames (3) are written,
// XRFG_TEST_CAPTURE_GAP frames (600) apart, under seq<N>/frame<M>. A run's
// frames are copied on the GPU as they come, and read back and written only
// once the run is complete, so the game's frame timing within a run is
// barely disturbed. For measurement only. With
// XRFG_TEST_CAPTURE_WAIT=1 the runs start once a file named go appears in the
// directory instead.
class FrameDump {
public:
    // Called once per source; true when this one is to be recorded.
    [[nodiscard]] bool wanted() noexcept;
    // generated, if given, is the frame generated before this one, as the
    // headset is sent it; it rests in generated_state.
    void capture(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* colour,
                 DXGI_FORMAT view_format, std::span<const D3D12ReprojectionView> views,
                 const DlssMotionVectorSet* guides, ID3D12Resource* generated = nullptr,
                 D3D12_RESOURCE_STATES generated_state = D3D12_RESOURCE_STATE_COMMON) noexcept;

private:
    bool configured_{}, waiting_{};
    std::filesystem::path directory_;
    std::uint64_t frame_{};
    std::uint32_t skip_{600}, count_{3}, sequences_{4}, gap_{600};
    std::uint32_t sequence_{}, written_{};
    // The run so far: GPU copies waiting to be written, and each frame's
    // metadata.
    struct Held {
        std::filesystem::path path;
        Microsoft::WRL::ComPtr<ID3D12Resource> copy;
    };
    std::vector<Held> held_;
    std::vector<std::pair<std::filesystem::path, std::string>> metas_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> allocators_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>> lists_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    std::uint64_t fence_value_{};
    void flush(ID3D12Device* device, ID3D12CommandQueue* queue);
    [[nodiscard]] bool wait(std::uint64_t value);
};

// A recorded texture: its description and each subresource's rows, packed.
struct DumpedTexture {
    std::uint32_t width{}, height{}, array_size{}, format{};
    struct Subresource {
        std::uint32_t row_bytes{}, rows{};
        std::vector<std::uint8_t> data;
    };
    std::vector<Subresource> subresources;
};
[[nodiscard]] bool read_dumped_texture(const std::filesystem::path& path, DumpedTexture* texture);

}  // namespace xrfg
