#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <span>

namespace xrfg {

// Tells which of two DLSS streams made which eye's image, from the images: an
// eye's captured image against the tile each evaluation copied of its output
// (DlssMotionVectorFrame::output_tile, eye_check.hlsl). Nothing but the
// images' own agreement goes into it, so it holds whatever order the game
// creates or evaluates its features in, and after a level load has handed
// the two features to each other's eyes. The images are read in bursts of
// pairs - one every two seconds, and one at once when a stream appears,
// resets or the evaluation order changes (dlss_motion_vectors.cpp decides,
// by copying the tiles only then) - so a swap reaches the guide registry
// within a burst; a tile of far things alone decides nothing and leaves the
// current order standing.
class D3D12EyeCheck {
public:
    static constexpr UINT kSlots = 8;
    // The images must name the same stream as eye 0 kVotes pairs in a row,
    // about the same two streams, by at least kMargin of correlation summed
    // over the two eyes (2 at most), before it is told: one pair can be a
    // frame of motion blur or a loading screen. A pair that says nothing - a
    // flat tile or image, or a difference within the margin - neither counts
    // nor breaks the run: it carries no word either way.
    static constexpr UINT kVotes = 5;
    static constexpr float kMargin = 0.2F;
    // Below this variance of the log luminance a tile or an image region is
    // flat - a loading screen, a black frame - and the pair is not counted.
    static constexpr float kMinimumVariance = 1.0e-4F;

    [[nodiscard]] HRESULT initialize(ID3D12Device* device, DXGI_FORMAT view_format) noexcept;

    // Records the comparison of this pair's image with the set's two eyes'
    // tiles into `list`. Every finished comparison is read first (poll), so a
    // slot is never recorded over a result still unread; a slot whose
    // comparison is still on the GPU is left alone, and this pair goes
    // unchecked. The image rests in COMMON and is left there. Nothing is
    // recorded when an eye has no tile or the tiles differ in size.
    void record(ID3D12GraphicsCommandList* list, UINT slot, ID3D12Resource* image,
        std::span<const D3D12ReprojectionView> views,
        const DlssMotionVectorSet& set) noexcept;

    // The fence the slot's list completes on.
    void submitted(UINT slot, ID3D12Fence* fence, std::uint64_t value) noexcept;

    // Reads the comparisons whose lists have completed, in the order they
    // were recorded, and tells the guide registry
    // (report_dlss_eye_order_check) what the images say, once they have said
    // it kVotes pairs in a row.
    void poll() noexcept;

    struct Report {
        std::uint64_t compared{};   // comparisons read
        std::uint64_t decided{};    // verdicts given, confirmations included
        std::uint64_t swaps{};      // verdicts that moved the eyes
        float same{};               // the latest scores: eyes as resolved,
        float swapped{};            // and the other way round
    };
    [[nodiscard]] Report report() const noexcept { return report_; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> result;
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
        std::uint64_t eye0_stream{};
        std::uint64_t eye1_stream{};
        std::uint64_t sequence{};  // when it was recorded, among all slots
        bool recorded{};
    };

    // Reads one completed slot's comparison and counts its vote.
    void read(Slot& slot) noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compare_;
    std::array<Slot, kSlots> slots_;
    DXGI_FORMAT view_format_{DXGI_FORMAT_UNKNOWN};
    UINT increment_{};
    std::uint64_t next_sequence_{};
    // The run of votes: the two streams it is about, lower id first, the
    // stream it names as eye 0, and how many pairs in a row have named it.
    std::uint64_t run_streams_[2]{};
    std::uint64_t run_eye0_{};
    UINT run_{};
    Report report_;
};

}  // namespace xrfg
