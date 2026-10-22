#include "xrfg/dlss_motion_vectors.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t kMaximumStereoPublicationGap = 4;

// D3D12EyeCheck reads the images in bursts: one every kEyeCheckIntervalMs,
// and one at once when a stream appears, resets or the evaluation order
// changes - the signs a swap may leave, though not always (a session began
// with the eyes swapped and no sign at all, which the periodic burst covers).
// The tiles it compares are copied only inside a burst, so outside one
// nothing is copied or compared. A burst is long enough for the check's five
// pairs at twenty frames a second.
constexpr std::uint64_t kEyeCheckIntervalMs = 2000;
constexpr std::uint64_t kEyeCheckBurstMs = 250;
constexpr std::size_t kSnapshotSlotCount = 4;
constexpr D3D12_RESOURCE_STATES kSnapshotReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct SnapshotSlot {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_DESC description{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
};

// The part of a guide resource one publication reads.
struct SnapshotRegion {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct StreamState {
    std::uint64_t epoch{1};
    std::uint64_t serial{};
    std::uint64_t first_publication{};
    float jitter_x{};
    float jitter_y{};
    std::shared_ptr<const DlssMotionVectorFrame> older;
    std::shared_ptr<const DlssMotionVectorFrame> previous;
    std::shared_ptr<const DlssMotionVectorFrame> latest;
    std::array<SnapshotSlot, kSnapshotSlotCount> snapshots;
    std::size_t next_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> depth_snapshots;
    std::size_t next_depth_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> tile_snapshots;
    std::size_t next_tile_snapshot{};
};

// Which of two streams whose inputs sit at the same place is eye 0. First
// seen is the start: a renderer that evaluates the left eye first does so in
// its first frame too. It is not the end: Unreal's DLSS plugin hands its
// features out anew on a level load, and in STALKER 2 under UEVR's Native
// Stereo Fix the two came back the other way round - the same two handles,
// the right eye's now evaluated first on every frame - so from the load on
// each eye took the other's guides. What stays tied to the eyes is the order
// the two are evaluated in, frame after frame: it replaces first seen once
// it has held for kEvaluationOrderFrames frames in a row, and the same run
// takes it back. One reversed frame, or an order that alternates, changes
// nothing, so a game whose order wanders keeps first seen.
constexpr std::uint64_t kEvaluationOrderFrames = 30;

struct EvaluationOrder {
    std::uint64_t streams[2]{};     // the pair learned about, lower id first
    std::uint64_t first{};          // the stream evaluated first in the run
    std::uint64_t run{};            // consecutive frames with that order
    std::uint64_t counted{};        // newest publication counted: one count a frame
    std::uint64_t leader{};         // the stream taken as eye 0, 0 until learned
    std::uint64_t pair_started_ms{};  // when the pair was first counted
    std::uint64_t run_started_ms{};   // when the current order began
    // The stream the images themselves showed to be eye 0
    // (report_dlss_eye_order_check), 0 until they have. It outranks the
    // order: the images are the thing itself, the order a sign of it. Kept
    // until the pair changes, the images say otherwise, or the order settles
    // against it. A change of order opens an eye-check burst at once, so the
    // images usually answer before the new order has held; where they cannot -
    // a dark loading screen after the level load that turned the features
    // round - the order that has since held is the newer word.
    std::uint64_t checked{};
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, StreamState> streams;
    DlssMotionVectorStatistics statistics;
    EvaluationOrder order;
    std::uint64_t burst_started_ms{};  // the last eye-check burst's start
    std::uint64_t burst_until_ms{};    // its end: tiles are copied until then
};

// Opens an eye-check burst now.
void start_eye_check_burst(Registry& state, std::uint64_t now) noexcept {
    state.burst_started_ms = now;
    state.burst_until_ms = now + kEyeCheckBurstMs;
}

Registry& registry() {
    static Registry value;
    return value;
}

std::atomic_bool g_tracking_enabled{false};

ComPtr<IUnknown> identity(IUnknown* object) noexcept {
    ComPtr<IUnknown> value;
    if (object != nullptr) {
        static_cast<void>(object->QueryInterface(IID_PPV_ARGS(value.GetAddressOf())));
    }
    return value;
}

bool same_identity(IUnknown* left, IUnknown* right) noexcept {
    const auto a = identity(left);
    const auto b = identity(right);
    return a && b && a.Get() == b.Get();
}

bool valid(const DlssMotionVectorPublication& publication) noexcept {
    if (publication.stream == 0 || publication.output == nullptr ||
        publication.motion_vectors == nullptr ||
        publication.producer_queue == nullptr ||
        publication.producer_command_list == nullptr || publication.output_width == 0 ||
        publication.output_height == 0 || publication.motion_width == 0 ||
        publication.motion_height == 0 || !std::isfinite(publication.scale_x) ||
        !std::isfinite(publication.scale_y) || !std::isfinite(publication.jitter_x) ||
        !std::isfinite(publication.jitter_y)) {
        return false;
    }
    const auto output = publication.output->GetDesc();
    const auto motion = publication.motion_vectors->GetDesc();
    const bool base_valid = output.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        motion.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        static_cast<std::uint64_t>(publication.output_x) + publication.output_width <=
            output.Width &&
        static_cast<std::uint64_t>(publication.output_y) + publication.output_height <=
            output.Height &&
        static_cast<std::uint64_t>(publication.motion_x) + publication.motion_width <=
            motion.Width &&
        static_cast<std::uint64_t>(publication.motion_y) + publication.motion_height <=
            motion.Height;
    if (!base_valid || publication.depth == nullptr) return base_valid;
    const auto depth = publication.depth->GetDesc();
    return depth.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        publication.depth_width != 0 && publication.depth_height != 0 &&
        static_cast<std::uint64_t>(publication.depth_x) + publication.depth_width <=
            depth.Width &&
        static_cast<std::uint64_t>(publication.depth_y) + publication.depth_height <=
            depth.Height &&
        std::isfinite(publication.frame_time_delta_ms) &&
        publication.frame_time_delta_ms > 0.0F &&
        std::isfinite(publication.camera_near) &&
        std::isfinite(publication.camera_far);
}

bool matching_description(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension && left.Alignment == right.Alignment &&
        left.Width == right.Width && left.Height == right.Height &&
        left.DepthOrArraySize == right.DepthOrArraySize &&
        left.MipLevels == right.MipLevels && left.Format == right.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality &&
        left.Layout == right.Layout && left.Flags == right.Flags;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

HRESULT snapshot_resource(
    std::array<SnapshotSlot, kSnapshotSlotCount>& snapshots,
    std::size_t& next_snapshot,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state,
    ID3D12GraphicsCommandList* producer_command_list,
    ID3D12CommandQueue* producer_queue,
    ID3D12Device* verified_producer_device,
    ComPtr<ID3D12Resource>* snapshot,
    bool depth,
    const SnapshotRegion& region,
    bool* cropped) noexcept {
    if (source == nullptr || producer_command_list == nullptr ||
        producer_queue == nullptr || snapshot == nullptr || cropped == nullptr) return E_POINTER;
    snapshot->Reset();
    *cropped = false;

    ComPtr<ID3D12Device> source_device;
    ComPtr<ID3D12Device> command_device;
    ComPtr<ID3D12Device> queue_device;
    HRESULT result = source->GetDevice(
        IID_PPV_ARGS(source_device.GetAddressOf()));
    if (FAILED(result)) return result;
    result = producer_command_list->GetDevice(
        IID_PPV_ARGS(command_device.GetAddressOf()));
    if (FAILED(result)) return result;
    result = producer_queue->GetDevice(
        IID_PPV_ARGS(queue_device.GetAddressOf()));
    if (FAILED(result)) return result;
    ComPtr<ID3D12Device> allocation_device;
    if (verified_producer_device != nullptr) {
        // The producer has already unwrapped and identity-checked all related
        // objects. Comparing their reported wrapper devices here would
        // recreate the Streamline false mismatch this field avoids.
        allocation_device = verified_producer_device;
    } else {
        if (!same_identity(source_device.Get(), command_device.Get()) ||
            !same_identity(source_device.Get(), queue_device.Get())) {
            return E_INVALIDARG;
        }
        allocation_device = source_device;
    }

    const D3D12_RESOURCE_DESC description = source->GetDesc();
    // Engines size motion and depth targets for their largest view; each DLSS
    // evaluation reads one rectangle of them. A single-subresource texture is
    // copied as that rectangle alone. D3D12 copies depth-stencil subresources
    // only whole, so from a 32-bit depth-stencil target the depth plane alone
    // is copied, and the stencil plane the guides never read is left behind.
    const bool single = description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.DepthOrArraySize == 1 && description.MipLevels == 1 &&
        description.SampleDesc.Count == 1;
    const bool depth_stencil =
        (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const bool crop = single && !depth_stencil && region.width != 0 && region.height != 0 &&
        static_cast<std::uint64_t>(region.x) + region.width <= description.Width &&
        static_cast<std::uint64_t>(region.y) + region.height <= description.Height &&
        (region.width < description.Width || region.height < description.Height);
    const bool depth_plane = depth && single &&
        (description.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
         description.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    auto allocation_description = description;
    if (crop) {
        allocation_description.Width = region.width;
        allocation_description.Height = region.height;
    }
    if (depth) {
        // A typed DSV resource cannot be read through a float SRV. Keep a
        // bit-exact copy in its typeless family and permit shader reads.
        switch (description.Format) {
        case DXGI_FORMAT_D32_FLOAT: allocation_description.Format=DXGI_FORMAT_R32_TYPELESS; break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: allocation_description.Format=DXGI_FORMAT_R24G8_TYPELESS; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: allocation_description.Format=DXGI_FORMAT_R32G8X24_TYPELESS; break;
        case DXGI_FORMAT_D16_UNORM: allocation_description.Format=DXGI_FORMAT_R16_TYPELESS; break;
        default: break;
        }
        allocation_description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            allocation_description.Flags & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        if (depth_plane) {
            allocation_description.Format = DXGI_FORMAT_R32_TYPELESS;
            allocation_description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                allocation_description.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        }
    }
    SnapshotSlot& slot = snapshots[next_snapshot];
    if (!slot.resource || !matching_description(slot.description, allocation_description)) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> resource;
        result = allocation_device->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &allocation_description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(resource.GetAddressOf()));
        if (FAILED(result)) return result;
        slot.resource = std::move(resource);
        slot.description = allocation_description;
        slot.state = D3D12_RESOURCE_STATE_COMMON;
    }

    std::array<D3D12_RESOURCE_BARRIER, 2> before{};
    UINT before_count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        before[before_count++] = transition_barrier(
            source,
            source_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (slot.state != D3D12_RESOURCE_STATE_COPY_DEST) {
        before[before_count++] = transition_barrier(
            slot.resource.Get(), slot.state, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (before_count != 0) {
        producer_command_list->ResourceBarrier(before_count, before.data());
    }
    if (crop || depth_plane) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = slot.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION origin{};
        origin.pResource = source;
        origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;  // plane 0
        const D3D12_BOX box{region.x, region.y, 0, region.x + region.width,
                            region.y + region.height, 1};
        producer_command_list->CopyTextureRegion(&destination, 0, 0, 0, &origin,
                                                 crop ? &box : nullptr);
    } else {
        producer_command_list->CopyResource(slot.resource.Get(), source);
    }
    *cropped = crop;

    std::array<D3D12_RESOURCE_BARRIER, 2> after{};
    UINT after_count = 0;
    if (source_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        after[after_count++] = transition_barrier(
            source,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            source_state);
    }
    after[after_count++] = transition_barrier(
        slot.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSnapshotReadState);
    producer_command_list->ResourceBarrier(after_count, after.data());
    slot.state = kSnapshotReadState;
    *snapshot = slot.resource;
    next_snapshot = (next_snapshot + 1) % kSnapshotSlotCount;
    return S_OK;
}

struct Candidate {
    std::uint64_t first_publication{};
    std::shared_ptr<const DlssMotionVectorFrame> frame;
    std::shared_ptr<const DlssMotionVectorFrame> previous;
    std::shared_ptr<const DlssMotionVectorFrame> older;
};

// Where an eye's DLSS inputs sit in their render targets. Both eyes often
// share one double-wide target with the left eye on the left - UEVR's depth,
// for one - so the eye further left is eye 0 whichever evaluates first.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> input_offset(
    const DlssMotionVectorFrame& frame) noexcept {
    return {frame.depth ? frame.source_depth_x : 0U, frame.source_motion_x};
}

// Counts this frame's evaluation order of two streams and returns the stream
// their order makes eye 0, or 0 while no order has held long enough. Each
// decision is timed into the statistics: how long the order had held, and
// how long the pair had been seen - the frames until then had the eyes'
// guides by the previous rule.
std::uint64_t learn_evaluation_order(Registry& state,
    const DlssMotionVectorFrame& a, const DlssMotionVectorFrame& b) noexcept {
    auto& order = state.order;
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t lower = std::min(a.stream, b.stream);
    const std::uint64_t higher = std::max(a.stream, b.stream);
    if (order.streams[0] != lower || order.streams[1] != higher) {
        order = {};
        order.streams[0] = lower;
        order.streams[1] = higher;
        order.pair_started_ms = now;
    }
    // Both eyes' images resolve against the same pair of evaluations: count
    // the pair once.
    const std::uint64_t newest = std::max(a.publication, b.publication);
    if (newest == order.counted) return order.leader;
    order.counted = newest;
    const std::uint64_t first = a.publication < b.publication ? a.stream : b.stream;
    if (first == order.first) {
        ++order.run;
    } else {
        order.first = first;
        order.run = 1;
        order.run_started_ms = now;
        start_eye_check_burst(state, now);
    }
    if (order.run >= kEvaluationOrderFrames && order.leader != first) {
        order.leader = first;
        if (order.checked != 0 && order.checked != first) order.checked = 0;
        auto& statistics = state.statistics;
        ++statistics.eye_order_decisions;
        statistics.eye_order_stream = first;
        statistics.eye_order_run_ms = now - order.run_started_ms;
        statistics.eye_order_pair_ms = now - order.pair_started_ms;
    }
    return order.leader;
}

// Eye order of two streams: by where their inputs sit, then by what the
// images showed, then by the evaluation order learned above, then by
// first-seen order. First seen alone gave Galactic Racer under UEVR each eye
// the other's guides whenever the right eye evaluated first.
[[nodiscard]] bool eye_before(const Candidate& a, const Candidate& b,
    const EvaluationOrder& order) noexcept {
    const auto ao = input_offset(*a.frame), bo = input_offset(*b.frame);
    if (ao != bo) return ao < bo;
    for (const std::uint64_t eye0 : {order.checked, order.leader}) {
        if (eye0 != 0 && (a.frame->stream == eye0 || b.frame->stream == eye0)) {
            return a.frame->stream == eye0;
        }
    }
    return a.first_publication < b.first_publication;
}

}  // namespace

void publish_dlss_motion_vectors(
    const DlssMotionVectorPublication& publication) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) return;
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (!valid(publication)) {
            ++state.statistics.invalid_publications;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return;
        }

        auto& stream = state.streams[publication.stream];
        if (publication.reset) {
            ++stream.epoch;
            stream.serial = 0;
            stream.older.reset();
            stream.previous.reset();
            stream.latest.reset();
        }

        ComPtr<ID3D12Resource> motion_snapshot;
        bool motion_cropped = false;
        if (FAILED(snapshot_resource(stream.snapshots, stream.next_snapshot,
                publication.motion_vectors, publication.resource_state,
                publication.producer_command_list, publication.producer_queue,
                publication.verified_producer_device,
                &motion_snapshot, false,
                {publication.motion_x, publication.motion_y,
                 publication.motion_width, publication.motion_height},
                &motion_cropped))) {
            ++state.statistics.snapshot_failures;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return;
        }
        ++state.statistics.snapshot_copies;

        ComPtr<ID3D12Resource> depth_snapshot;
        bool depth_cropped = false;
        if (publication.depth != nullptr) {
            if (FAILED(snapshot_resource(stream.depth_snapshots,
                    stream.next_depth_snapshot, publication.depth,
                    publication.depth_resource_state,
                    publication.producer_command_list,
                    publication.producer_queue,
                    publication.verified_producer_device, &depth_snapshot, true,
                    {publication.depth_x, publication.depth_y,
                     publication.depth_width, publication.depth_height},
                    &depth_cropped))) {
                ++state.statistics.snapshot_failures;
                state.statistics.status = DlssMotionVectorStatus::invalid_input;
                return;
            }
            ++state.statistics.snapshot_copies;
            ++state.statistics.depth_publications;
        }

        // The output tile for D3D12EyeCheck: the lower middle of the output
        // rectangle, a quarter of its width by an eighth of its height. From
        // a single 2D output alone, and only as a cropped copy; otherwise the
        // frame goes without and the check stands aside.
        ComPtr<ID3D12Resource> tile_snapshot;
        std::uint32_t tile_x = 0, tile_y = 0, tile_width = 0, tile_height = 0;
        const std::uint64_t now = GetTickCount64();
        if (stream.first_publication == 0 || publication.reset ||
            now - state.burst_started_ms >= kEyeCheckIntervalMs) {
            start_eye_check_burst(state, now);
        }
        if (publication.copy_output_tile && now < state.burst_until_ms) {
            const D3D12_RESOURCE_DESC od = publication.output->GetDesc();
            tile_width = publication.output_width / 4;
            tile_height = publication.output_height / 8;
            tile_x = publication.output_width * 3 / 8;
            tile_y = publication.output_height * 9 / 16;
            bool tile_cropped = false;
            if (od.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                od.DepthOrArraySize == 1 && od.MipLevels == 1 && od.SampleDesc.Count == 1 &&
                tile_width != 0 && tile_height != 0 &&
                std::uint64_t(publication.output_x) + tile_x + tile_width <= od.Width &&
                std::uint64_t(publication.output_y) + tile_y + tile_height <= od.Height &&
                SUCCEEDED(snapshot_resource(stream.tile_snapshots, stream.next_tile_snapshot,
                    publication.output, publication.output_resource_state,
                    publication.producer_command_list, publication.producer_queue,
                    publication.verified_producer_device, &tile_snapshot, false,
                    {publication.output_x + tile_x, publication.output_y + tile_y,
                     tile_width, tile_height},
                    &tile_cropped)) &&
                tile_cropped) {
                ++state.statistics.tile_copies;
            } else {
                tile_snapshot.Reset();
            }
        }

        auto frame = std::make_shared<DlssMotionVectorFrame>();
        frame->stream = publication.stream;
        frame->epoch = stream.epoch;
        frame->previous_serial = stream.serial;
        frame->serial = ++stream.serial;
        frame->publication = ++state.statistics.published;
        frame->output = publication.output;
        frame->motion_vectors = std::move(motion_snapshot);
        frame->producer_queue = publication.producer_queue;
        frame->output_x = publication.output_x;
        frame->output_y = publication.output_y;
        frame->output_width = publication.output_width;
        frame->output_height = publication.output_height;
        frame->output_slice = 0;
        // A cropped snapshot holds the rectangle at its origin.
        frame->motion_x = motion_cropped ? 0 : publication.motion_x;
        frame->source_motion_x = publication.motion_x;
        frame->motion_y = motion_cropped ? 0 : publication.motion_y;
        frame->motion_width = publication.motion_width;
        frame->motion_height = publication.motion_height;
        frame->motion_slice = 0;
        frame->scale_x = publication.scale_x == 0.0F ? 1.0F : publication.scale_x;
        frame->scale_y = publication.scale_y == 0.0F ? 1.0F : publication.scale_y;
        frame->jitter_x = publication.jitter_x;
        frame->jitter_y = publication.jitter_y;
        frame->previous_jitter_x = stream.jitter_x;
        frame->previous_jitter_y = stream.jitter_y;
        frame->resource_state = kSnapshotReadState;
        frame->jittered = publication.jittered;
        frame->reset = publication.reset;
        frame->depth = std::move(depth_snapshot);
        frame->depth_x = depth_cropped ? 0 : publication.depth_x;
        frame->source_depth_x = publication.depth_x;
        frame->depth_y = depth_cropped ? 0 : publication.depth_y;
        frame->depth_width = publication.depth_width;
        frame->depth_height = publication.depth_height;
        frame->depth_resource_state = kSnapshotReadState;
        if (tile_snapshot) {
            frame->output_tile = std::move(tile_snapshot);
            frame->output_tile_x = tile_x;
            frame->output_tile_y = tile_y;
            frame->output_tile_width = tile_width;
            frame->output_tile_height = tile_height;
        }
        frame->frame_time_delta_ms = publication.frame_time_delta_ms;
        frame->frame_time_known = publication.frame_time_known;
        frame->camera_near = publication.camera_near;
        frame->camera_far = publication.camera_far;
        frame->depth_inverted = publication.depth_inverted;
        frame->depth_infinite = publication.depth_infinite;
        stream.jitter_x = publication.jitter_x;
        stream.jitter_y = publication.jitter_y;
        if (stream.first_publication == 0) {
            stream.first_publication = frame->publication;
        }
        stream.older = std::move(stream.previous);
        stream.previous = std::move(stream.latest);
        stream.latest = std::move(frame);
        if (state.statistics.used == 0) {
            state.statistics.status = DlssMotionVectorStatus::output_not_direct;
        }
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
    }
}

void configure_dlss_motion_vector_tracking(bool enabled) noexcept {
    const bool previous = g_tracking_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled == previous) return;
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (!enabled) {
            // Do not release the private resources here. The desired setting is
            // visible to DLSS before the OpenXR layer reaches xrEndFrame and its
            // GPU-idle fence, so snapshots may still be referenced by submitted
            // synthesis work. The layer retires them explicitly after that fence.
            state.statistics.status = DlssMotionVectorStatus::disabled;
            return;
        }
        // Re-enabling without a layer-side retirement must not expose stale
        // vectors. Preserve the slot resources (and therefore any in-flight GPU
        // lifetime), but start fresh stream epochs and temporal ownership.
        for (auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            ++stream.epoch;
            stream.serial = 0;
            stream.first_publication = 0;
            stream.jitter_x = 0.0F;
            stream.jitter_y = 0.0F;
            stream.older.reset();
            stream.previous.reset();
            stream.latest.reset();
        }
        state.order = {};
        state.burst_started_ms = state.burst_until_ms = 0;
        const std::uint64_t generation = state.statistics.generation + 1;
        state.statistics = {};
        state.statistics.generation = generation;
        state.statistics.status = DlssMotionVectorStatus::waiting_for_dlss;
    } catch (...) {
    }
}

void retire_disabled_dlss_motion_vector_resources() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        if (g_tracking_enabled.load(std::memory_order_acquire)) return;
        state.streams.clear();
        state.order = {};
        state.burst_started_ms = state.burst_until_ms = 0;
        const std::uint64_t generation = state.statistics.generation + 1;
        state.statistics = {};
        state.statistics.generation = generation;
        state.statistics.status = DlssMotionVectorStatus::disabled;
    } catch (...) {
    }
}

void retire_dlss_motion_vector_stream(std::uint64_t stream) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        state.streams.erase(stream);
        if (state.order.streams[0] == stream || state.order.streams[1] == stream) {
            state.order = {};
        }
    } catch (...) {
    }
}

std::uint64_t dlss_motion_vector_publications() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        return state.statistics.published;
    } catch (...) {
        return 0;
    }
}

void report_dlss_eye_order_check(std::uint64_t eye0_stream, std::uint64_t eye1_stream) noexcept {
    try {
        if (eye0_stream == 0 || eye1_stream == 0 || eye0_stream == eye1_stream) return;
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        auto& order = state.order;
        const std::uint64_t lower = std::min(eye0_stream, eye1_stream);
        const std::uint64_t higher = std::max(eye0_stream, eye1_stream);
        const std::uint64_t now = GetTickCount64();
        if (order.streams[0] != lower || order.streams[1] != higher) {
            // A pair the order has not counted yet: the images' word starts it.
            order = {};
            order.streams[0] = lower;
            order.streams[1] = higher;
            order.pair_started_ms = now;
        }
        if (order.checked == eye0_stream) return;
        order.checked = eye0_stream;
        ++state.statistics.eye_check_decisions;
        state.statistics.eye_check_stream = eye0_stream;
        state.statistics.eye_check_pair_ms = now - order.pair_started_ms;
    } catch (...) {
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    ID3D12Resource* output,
    ID3D12CommandQueue* consumer_queue,
    int eye,
    std::uint64_t released_at,
    std::uint64_t other_released_at) noexcept {
    try {
        if (!g_tracking_enabled.load(std::memory_order_acquire)) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::disabled);
            return {};
        }
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.resolve_calls;
        if (output == nullptr || consumer_queue == nullptr) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }

        const auto description = output->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            ++state.statistics.invalid_rejections;
            state.statistics.status = DlssMotionVectorStatus::invalid_input;
            return {};
        }
        std::uint32_t eye_count = std::min<std::uint32_t>(
            std::max<std::uint32_t>(description.DepthOrArraySize, 1U),
            kDlssMotionVectorEyeCount);

        std::vector<Candidate> candidates;
        std::uint32_t streams_with_frames = 0;
        candidates.reserve(state.streams.size());
        for (const auto& [unused_stream_id, stream] : state.streams) {
            static_cast<void>(unused_stream_id);
            if (!stream.latest) continue;
            ++streams_with_frames;
            if (same_identity(stream.latest->producer_queue.Get(), consumer_queue)) {
                candidates.push_back(
                    {stream.first_publication, stream.latest, stream.previous, stream.older});
            }
        }
        // The streams evaluated on this image's queue, before any is set aside
        // for its size: what tells an image no evaluation fits from a game
        // whose DLSS ran on another queue.
        const std::size_t streams_on_queue = candidates.size();
        state.statistics.eye_width = static_cast<std::uint32_t>(description.Width);
        state.statistics.eye_height = description.Height;
        state.statistics.dlss_width = state.statistics.dlss_height = 0;
        if (!candidates.empty()) {
            const auto newest = std::max_element(candidates.begin(), candidates.end(),
                [](const Candidate& a, const Candidate& b) {
                    return a.frame->publication < b.frame->publication;
                });
            state.statistics.dlss_width = newest->frame->output_width;
            state.statistics.dlss_height = newest->frame->output_height;
        }

        // An evaluation whose output is this very image belongs to it,
        // whatever else matches its size.
        bool direct_match = false;
        {
            std::vector<Candidate> direct;
            for (const auto& candidate : candidates) {
                if (same_identity(candidate.frame->output.Get(), output)) {
                    direct.push_back(candidate);
                }
            }
            if (!direct.empty()) {
                candidates = std::move(direct);
                direct_match = true;
            }
        }

        // UEVR submits double-wide colour after independent mono DLSS calls.
        // Preserve both eye streams and remap their output coordinate systems
        // onto the packed XR image. Motion/depth stay in each NGX input's own
        // coordinates; the native pack shader applies this output mapping.
        // An image that holds one eye alone - a swapchain per eye, some twice
        // an eye's width - is never two eyes side by side.
        bool packed_stereo = false;
        if (eye_count == 1 && eye != 0 && eye != 1 && description.Width % 2 == 0) {
            std::vector<Candidate> packed;
            for (const auto& candidate : candidates) {
                const auto& f = candidate.frame;
                if (f->output_x == 0 && f->output_y == 0 &&
                    std::uint64_t(f->output_width) * 2 == description.Width &&
                    f->output_height == description.Height) packed.push_back(candidate);
            }
            if (packed.size() >= 2) {
                candidates = std::move(packed);
                eye_count = 2;
                packed_stereo = true;
            }
        }

        if (!packed_stereo && eye_count == 1) {
            // Composition/UI swapchains share the scene queue but are not the
            // upscaler output. Never attach unrelated scene guides to them.
            std::erase_if(candidates, [&](const Candidate& c) {
                return c.frame->output_x != 0 || c.frame->output_y != 0 ||
                    c.frame->output_width != description.Width ||
                    c.frame->output_height != description.Height;
            });
        }
        // Too few streams for the image's eyes. Where this queue evaluated
        // enough of them, none has this image's shape - an eye image of
        // another size than every DLSS output, for one. That is neither a
        // queue mismatch nor a temporal one, which is what the panel used to
        // say: a swapchain per eye said the vectors "did not match the
        // frames" when no evaluation could ever fit the image.
        const auto too_few_streams = [&]() -> std::shared_ptr<const DlssMotionVectorSet> {
            if (streams_on_queue >= eye_count) {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = DlssMotionVectorStatus::no_matching_output;
            } else if (streams_with_frames >= eye_count) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            } else {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = state.statistics.published == 0
                    ? DlssMotionVectorStatus::waiting_for_dlss
                    : DlssMotionVectorStatus::output_not_direct;
            }
            return {};
        };

        // A swapchain per eye, each the size of a DLSS output: every eye's
        // evaluation matches each eye's image by size, and the newest went to
        // both - with the capture at xrEndFrame, after both eyes, always the
        // eye DLSS ran last - so the other eye followed its motion, a double
        // image wherever the two eyes' motion differs. An eye's evaluation
        // comes before its image is released: each eye now takes the newest
        // evaluation published by its own image's release. Lies of P under
        // UEVR evaluates one DLSS feature for each eye in turn, each just
        // before that eye's release. Where both eyes' releases follow the same
        // evaluation - both evaluated before either is released - the eyes go
        // by the order the stereo path gives two streams, and for a single
        // stream by the order they were evaluated, left first.
        if (!packed_stereo && !direct_match && eye_count == 1 &&
            (eye == 0 || eye == 1)) {
            if (candidates.empty()) return too_few_streams();
            // Streams no longer evaluated - Lies of P keeps one of the eyes'
            // size from start-up - are not an eye's.
            std::uint64_t newest_publication = 0;
            for (const auto& candidate : candidates) {
                newest_publication = std::max(newest_publication, candidate.frame->publication);
            }
            std::erase_if(candidates, [&](const Candidate& c) {
                return newest_publication - c.frame->publication > kMaximumStereoPublicationGap;
            });
            std::vector<std::shared_ptr<const DlssMotionVectorFrame>> frames;
            for (const auto& candidate : candidates) {
                frames.push_back(candidate.frame);
                if (candidate.previous && candidate.previous->epoch == candidate.frame->epoch) {
                    frames.push_back(candidate.previous);
                }
            }
            std::sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
                return a->publication > b->publication;
            });
            const auto newest_by = [&](std::uint64_t limit) {
                for (const auto& frame : frames) {
                    if (limit == 0 || frame->publication <= limit) return frame;
                }
                return std::shared_ptr<const DlssMotionVectorFrame>{};
            };
            auto chosen = newest_by(released_at);
            const auto other = other_released_at != 0
                ? newest_by(other_released_at)
                : std::shared_ptr<const DlssMotionVectorFrame>{};
            bool shared_stream = candidates.size() == 1;
            if (chosen && other && chosen == other) {
                if (candidates.size() >= 2) {
                    // Two streams: each stream's newest, in the stereo order.
                    if (candidates.size() == 2) {
                        learn_evaluation_order(state, *candidates[0].frame,
                            *candidates[1].frame);
                    }
                    const EvaluationOrder& order = state.order;
                    std::sort(candidates.begin(), candidates.end(),
                        [&order](const Candidate& a, const Candidate& b) {
                            return eye_before(a, b, order);
                        });
                    chosen = candidates[static_cast<std::size_t>(eye)].frame;
                } else {
                    // One stream: the two newest, in the order evaluated.
                    const auto later = std::find(frames.begin(), frames.end(), chosen);
                    const auto earlier = later + 1;
                    chosen = eye == 1 ? *later
                        : earlier != frames.end() ? *earlier
                                                  : std::shared_ptr<const DlssMotionVectorFrame>{};
                }
            }
            if (!chosen || frames.empty() ||
                frames.front()->publication - chosen->publication > kMaximumStereoPublicationGap) {
                ++state.statistics.resolve_stale_pairs;
                state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
                return {};
            }
            // One stream for both eyes - the other eye's evaluation is that
            // stream's too - so each eye sees every other serial.
            if (other && chosen && other != chosen) {
                shared_stream = other->stream == chosen->stream;
            }
            auto frame = std::make_shared<DlssMotionVectorFrame>(*chosen);
            if (shared_stream) {
                frame->previous_serial = frame->serial > 2 ? frame->serial - 2 : 0;
                // The other eye's evaluation between this eye's two.
                for (const auto& candidate : candidates) {
                    for (const auto& bridge : {candidate.frame, candidate.previous, candidate.older}) {
                        if (bridge && bridge->stream == chosen->stream &&
                            bridge->epoch == chosen->epoch && bridge->serial + 1 == chosen->serial) {
                            frame->bridge = bridge;
                        }
                    }
                }
            }
            auto result = std::make_shared<DlssMotionVectorSet>();
            result->eye_count = 1;
            result->eyes[0] = std::move(frame);
            ++state.statistics.matched;
            return result;
        }

        // Alternating-eye renderers such as Ghost of Tsushima's AER path reuse
        // one DLSS handle for both eyes. A complete stereo guide set therefore
        // consists of two consecutive publications from that single stream.
        // Even local serials close the stable eye-0/eye-1 pair; odd serials are
        // deliberately left pending until the second eye arrives.
        if (eye_count == 2 && candidates.size() == 1) {
            const auto& current = candidates.front().frame;
            const auto& previous = candidates.front().previous;
            if (previous && current->stream == previous->stream &&
                current->epoch == previous->epoch &&
                current->serial == previous->serial + 1 &&
                (current->serial & 1U) == 0 &&
                current->publication > previous->publication &&
                current->publication - previous->publication <=
                    kMaximumStereoPublicationGap &&
                same_identity(previous->producer_queue.Get(), consumer_queue)) {
                auto left = std::make_shared<DlssMotionVectorFrame>(*previous);
                auto right = std::make_shared<DlssMotionVectorFrame>(*current);
                // Keep the intervening raw evaluation, just as the separate
                // eye-swapchain path does. The composer can then recognize and
                // correct cross-eye history instead of merely relabelling it.
                const auto& older = candidates.front().older;
                if (older && older->stream == previous->stream &&
                    older->epoch == previous->epoch && older->serial + 1 == previous->serial) {
                    left->bridge = older;
                }
                right->bridge = previous;
                // The first of the pair is the left eye unless the inputs
                // say otherwise.
                if (input_offset(*right) < input_offset(*left)) std::swap(left, right);
                left->previous_serial = left->serial > 2 ? left->serial - 2 : 0;
                right->previous_serial = right->serial > 2 ? right->serial - 2 : 0;
                auto result = std::make_shared<DlssMotionVectorSet>();
                result->eye_count = 2;
                result->eyes[0] = std::move(left);
                result->eyes[1] = std::move(right);
                ++state.statistics.matched;
                return result;
            }
        }

        if (candidates.size() < eye_count) return too_few_streams();

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            return a.frame->publication > b.frame->publication;
        });
        candidates.resize(eye_count);
        const auto newest = candidates.front().frame->publication;
        const auto oldest = candidates.back().frame->publication;
        if (newest < oldest || newest - oldest > kMaximumStereoPublicationGap) {
            ++state.statistics.resolve_stale_pairs;
            state.statistics.status = DlssMotionVectorStatus::temporal_mismatch;
            return {};
        }

        // Eye order follows the two DLSS evaluation streams rather than the
        // final colour resource, which survives any post-DLSS shader passes.
        if (eye_count == 2) {
            learn_evaluation_order(state, *candidates[0].frame, *candidates[1].frame);
        }
        const EvaluationOrder& order = state.order;
        std::sort(candidates.begin(), candidates.end(),
            [&order](const Candidate& a, const Candidate& b) {
                return eye_before(a, b, order);
            });

        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = eye_count;
        for (std::uint32_t index = 0; index < eye_count; ++index) {
            if (packed_stereo) {
                auto frame = std::make_shared<DlssMotionVectorFrame>(*candidates[index].frame);
                frame->output_x = index * frame->output_width;
                result->eyes[index] = std::move(frame);
            } else {
                result->eyes[index] = candidates[index].frame;
            }
        }
        ++state.statistics.matched;
        return result;
    } catch (...) {
        report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        return {};
    }
}

void report_dlss_motion_vector_status(DlssMotionVectorStatus status) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        // A missing set at synthesis must not erase the resolver's specific
        // explanation of why it could not match a DLSS output to this image.
        if (status == DlssMotionVectorStatus::waiting_for_dlss &&
            (state.statistics.status == DlssMotionVectorStatus::no_matching_output ||
             state.statistics.status == DlssMotionVectorStatus::queue_mismatch ||
             state.statistics.status == DlssMotionVectorStatus::output_not_direct)) return;
        if (status == DlssMotionVectorStatus::temporal_mismatch ||
            status == DlssMotionVectorStatus::stale_guides) {
            ++state.statistics.temporal_rejections;
        } else if (status == DlssMotionVectorStatus::invalid_input) {
            ++state.statistics.invalid_rejections;
        }
        state.statistics.status = status;
    } catch (...) {
    }
}

void report_dlss_motion_vector_use() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        ++state.statistics.used;
        state.statistics.last_used_publication = state.statistics.published;
        state.statistics.status = DlssMotionVectorStatus::used;
    } catch (...) {
    }
}

DlssMotionVectorStatistics dlss_motion_vector_statistics() noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        return state.statistics;
    } catch (...) {
        return {};
    }
}

}  // namespace xrfg
