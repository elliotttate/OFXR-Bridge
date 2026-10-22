#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <array>
#include <memory>

namespace xrfg {

struct DlssMotionVectorPublication {
    std::uint64_t stream{};
    ID3D12Resource* output{};
    ID3D12Resource* motion_vectors{};
    ID3D12CommandQueue* producer_queue{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    D3D12_RESOURCE_STATES resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bool jittered{};
    bool reset{};
    ID3D12GraphicsCommandList* producer_command_list{};
    // Optional frame-generation guides. The established DLSS-vector route
    // remains valid when these fields are absent.
    ID3D12Resource* depth{};
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    D3D12_RESOURCE_STATES depth_resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    float frame_time_delta_ms{16.6667F};
    float camera_near{0.1F};
    float camera_far{1000.0F};
    bool depth_inverted{};
    bool depth_infinite{};
    // A cooperating producer may supply the native D3D12 device after it has
    // unwrapped and identity-checked every object in the publication.
    ID3D12Device* verified_producer_device{};
    // Whether the game gave DLSS its frame time (frame_time_delta_ms is
    // otherwise a default).
    bool frame_time_known{};
    // Whether to copy a tile of the output with the guides, for telling which
    // eye's image the evaluation made (DlssMotionVectorFrame::output_tile),
    // and the output's state as the evaluation leaves it: NGX writes its
    // output as an unordered-access view, and the capture publishes right
    // after the evaluation, before the game moves it on. A producer that
    // does not know the state leaves the tile off.
    bool copy_output_tile{};
    D3D12_RESOURCE_STATES output_resource_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
};

struct DlssMotionVectorFrame {
    std::uint64_t stream{};
    std::uint64_t epoch{};
    std::uint64_t serial{};
    std::uint64_t previous_serial{};
    std::uint64_t publication{};
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> producer_queue;
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t output_slice{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    std::uint32_t motion_slice{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    float previous_jitter_x{};
    float previous_jitter_y{};
    D3D12_RESOURCE_STATES resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bool jittered{};
    bool reset{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    D3D12_RESOURCE_STATES depth_resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    float frame_time_delta_ms{16.6667F};
    float camera_near{0.1F};
    float camera_far{1000.0F};
    bool depth_inverted{};
    bool depth_infinite{};
    // Where the game's motion and depth rectangles started before the
    // snapshot cropped them: what tells eyes sharing one target apart.
    std::uint32_t source_motion_x{};
    std::uint32_t source_depth_x{};
    // A tile of the output as this evaluation wrote it, copied with the
    // guides, for telling which eye's image the evaluation made
    // (D3D12EyeCheck): the lower middle of the output rectangle, where a
    // first-person game's hands and ground are. Its origin is in the output
    // rectangle's own coordinates; the tile holds that region at 0,0. Empty
    // where the output could not be copied.
    Microsoft::WRL::ComPtr<ID3D12Resource> output_tile;
    std::uint32_t output_tile_x{};
    std::uint32_t output_tile_y{};
    std::uint32_t output_tile_width{};
    std::uint32_t output_tile_height{};
    // Whether the game gave DLSS its frame time.
    bool frame_time_known{};
    // For an eye whose DLSS stream also evaluates the other eye: the stream's
    // evaluation just before this one, the other eye's. A game that renders
    // both eyes through one view - UEVR's Synchronized Sequential and AFR
    // without its ghosting fix - writes each eye's vectors from the view's
    // previous frame, which is that other eye's, not this eye's previous image;
    // the bridge's own vectors lead on from there to it
    // (D3D12GuideComposer). Never set on the bridge itself.
    std::shared_ptr<const DlssMotionVectorFrame> bridge;
};

constexpr std::uint32_t kDlssMotionVectorEyeCount = 2;

struct DlssMotionVectorSet {
    std::array<std::shared_ptr<const DlssMotionVectorFrame>,
               kDlssMotionVectorEyeCount>
        eyes;
    std::uint32_t eye_count{};
};

enum class DlssMotionVectorStatus : std::uint32_t {
    disabled,
    waiting_for_dlss,
    output_not_direct,
    queue_mismatch,
    invalid_input,
    temporal_mismatch,
    used,
    waiting_for_depth,
    // Native DLSS FG could not create or evaluate its NGX feature.
    native_unavailable,
    // 3X was requested from a native feature that generates one frame.
    multi_frame_unsupported,
    // The game evaluates DLSS on the image's queue, but no evaluation's
    // output has the image's shape: an eye image of another size than
    // every DLSS output, say.
    no_matching_output,
    // Both submitted frames refer to the same DLSS evaluation.
    stale_guides,
};

struct DlssMotionVectorStatistics {
    std::uint64_t published{};
    std::uint64_t depth_publications{};
    std::uint64_t invalid_publications{};
    std::uint64_t snapshot_copies{};
    std::uint64_t snapshot_failures{};
    std::uint64_t resolve_calls{};
    std::uint64_t resolve_missing_streams{};
    std::uint64_t resolve_stale_pairs{};
    std::uint64_t resolve_queue_mismatches{};
    std::uint64_t matched{};
    std::uint64_t temporal_rejections{};
    std::uint64_t invalid_rejections{};
    std::uint64_t used{};
    std::uint64_t last_used_publication{};
    DlssMotionVectorStatus status{DlssMotionVectorStatus::waiting_for_dlss};
    // Latest queue-compatible DLSS output and requested eye image, for a
    // size-mismatch diagnostic. Internal only; the public getter ABI stays fixed.
    std::uint32_t dlss_width{}, dlss_height{}, eye_width{}, eye_height{};
    // Eye 0 of two streams at one input position, decided from their
    // evaluation order (resolve): decisions so far, the stream the last gave
    // eye 0, how long that order had held when it was decided and how long
    // the pair had been seen by then, both in milliseconds. Internal only.
    std::uint64_t eye_order_decisions{}, eye_order_stream{}, eye_order_run_ms{},
        eye_order_pair_ms{};
    // The same decided from the images (report_dlss_eye_order_check): how
    // many, the stream the last gave eye 0, and how long the pair had been
    // seen by then in milliseconds. Internal only.
    std::uint64_t eye_check_decisions{}, eye_check_stream{}, eye_check_pair_ms{};
    // Output tiles copied for that check.
    std::uint64_t tile_copies{};
    // How many times the counts above have started over (tracking configured
    // again, or its resources retired): a reader that remembers a count keys
    // it on this too. Internal only.
    std::uint64_t generation{};
};

void publish_dlss_motion_vectors(
    const DlssMotionVectorPublication& publication) noexcept;
void configure_dlss_motion_vector_tracking(bool enabled) noexcept;
// Call only after all queue work that may reference published snapshots has
// completed. A live disable intentionally retains them until this safe point.
void retire_disabled_dlss_motion_vector_resources() noexcept;
void retire_dlss_motion_vector_stream(std::uint64_t stream) noexcept;
// eye: for an image that holds one eye of a two-view projection - a game with
// a swapchain per eye - which eye it is (0 left, 1 right), or -1 when unknown
// or when the image holds both. released_at and other_released_at: how many
// evaluations had been published (dlss_motion_vector_publications) when this
// eye's image and the other eye's were last released, 0 when not known.
std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    ID3D12Resource* output,
    ID3D12CommandQueue* consumer_queue,
    int eye = -1,
    std::uint64_t released_at = 0,
    std::uint64_t other_released_at = 0) noexcept;
// How many evaluations have been published so far.
std::uint64_t dlss_motion_vector_publications() noexcept;
// The two streams' eyes as the images show them (D3D12EyeCheck): the stream
// whose output matched eye 0's image, and the one that matched eye 1's. It
// outranks the evaluation order and first-seen, below only the inputs'
// positions, until the evaluation order next settles the other way.
void report_dlss_eye_order_check(std::uint64_t eye0_stream, std::uint64_t eye1_stream) noexcept;
void report_dlss_motion_vector_status(DlssMotionVectorStatus status) noexcept;
void report_dlss_motion_vector_use() noexcept;
DlssMotionVectorStatistics dlss_motion_vector_statistics() noexcept;

}  // namespace xrfg
