#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace xrfg {

// Native NGX integration. The caller drains the queue before destroying this
// object and owns the frame/guide leases until its completion fence passes.
class D3D12NativeDlssG final {
  public:
    struct Output {
        ID3D12Resource *image{};
    };
    // Why the last record() returned S_FALSE.
    enum class Skip {
        none,
        guides,                  // missing, reset, discontinuous or invalid guides
        resizing,                // a resized eye waits for its previous submission
        multi_frame_unsupported, // more outputs than the adapter's feature generates
        feature_unavailable,     // NGX could not create the feature; retried later
        evaluate_failed,         // NGX rejected an evaluation; the history reseeds
    };
    D3D12NativeDlssG();
    ~D3D12NativeDlssG();
    HRESULT initialize(ID3D12Device *device, ID3D12CommandQueue *queue,
                       const D3D12_RESOURCE_DESC &source, DXGI_FORMAT view_format) noexcept;
    // S_FALSE means the pair was not generated and no output was written. The
    // list may still hold NGX history work, so call submitted() for any list
    // that is executed after record(), whatever it returned.
    HRESULT record(ID3D12GraphicsCommandList *list, UINT work_slot, ID3D12Resource *a,
                   ID3D12Resource *b, std::span<const D3D12ReprojectionView> a_views,
                   std::span<const D3D12ReprojectionView> b_views,
                   const DlssMotionVectorSet *a_guides, const DlssMotionVectorSet *b_guides,
                   std::span<const Output> outputs, D3D12_RESOURCE_STATES release_state) noexcept;
    void reset() noexcept;
    void submitted(ID3D12Fence *completion, std::uint64_t value) noexcept;
    [[nodiscard]] Skip last_skip() const noexcept;
    // Eye evaluations that reseeded NGX's history with the aligned previous frame.
    [[nodiscard]] std::uint64_t reseeds() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Frames NGX can generate between two rendered frames on this device's
// adapter: 0 when native frame generation is unavailable, 1 for 2X only, and
// 2 or more when 3X is possible. The first call per adapter queries NGX.
[[nodiscard]] std::uint32_t native_dlssg_max_generated_frames(ID3D12Device *device) noexcept;

} // namespace xrfg
