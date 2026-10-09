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
    std::shared_ptr<const DlssMotionVectorFrame> previous;
    std::shared_ptr<const DlssMotionVectorFrame> latest;
    std::array<SnapshotSlot, kSnapshotSlotCount> snapshots;
    std::size_t next_snapshot{};
    std::array<SnapshotSlot, kSnapshotSlotCount> depth_snapshots;
    std::size_t next_depth_snapshot{};
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, StreamState> streams;
    DlssMotionVectorStatistics statistics;
};

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
};

// Where an eye's DLSS inputs sit in their render targets. Both eyes often
// share one double-wide target with the left eye on the left - UEVR's depth,
// for one - so the eye further left is eye 0 whichever evaluates first.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> input_offset(
    const DlssMotionVectorFrame& frame) noexcept {
    return {frame.depth ? frame.source_depth_x : 0U, frame.source_motion_x};
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
        frame->frame_time_delta_ms = publication.frame_time_delta_ms;
        frame->camera_near = publication.camera_near;
        frame->camera_far = publication.camera_far;
        frame->depth_inverted = publication.depth_inverted;
        frame->depth_infinite = publication.depth_infinite;
        stream.jitter_x = publication.jitter_x;
        stream.jitter_y = publication.jitter_y;
        if (stream.first_publication == 0) {
            stream.first_publication = frame->publication;
        }
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
            stream.previous.reset();
            stream.latest.reset();
        }
        state.statistics = {};
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
        state.statistics = {};
        state.statistics.status = DlssMotionVectorStatus::disabled;
    } catch (...) {
    }
}

void retire_dlss_motion_vector_stream(std::uint64_t stream) noexcept {
    try {
        auto& state = registry();
        std::scoped_lock lock(state.mutex);
        state.streams.erase(stream);
    } catch (...) {
    }
}

std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    ID3D12Resource* output,
    ID3D12CommandQueue* consumer_queue) noexcept {
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
                    {stream.first_publication, stream.latest, stream.previous});
            }
        }

        // UEVR submits double-wide colour after independent mono DLSS calls.
        // Preserve both eye streams and remap their output coordinate systems
        // onto the packed XR image. Motion/depth stay in each NGX input's own
        // coordinates; the native pack shader applies this output mapping.
        bool packed_stereo = false;
        if (eye_count == 1 && description.Width % 2 == 0) {
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

        if (candidates.size() < eye_count) {
            if (streams_with_frames >= eye_count) {
                ++state.statistics.resolve_queue_mismatches;
                state.statistics.status = DlssMotionVectorStatus::queue_mismatch;
            } else {
                ++state.statistics.resolve_missing_streams;
                state.statistics.status = state.statistics.published == 0
                    ? DlssMotionVectorStatus::waiting_for_dlss
                    : DlssMotionVectorStatus::output_not_direct;
            }
            return {};
        }

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
        // final colour resource, which survives any post-DLSS shader passes:
        // by where each stream's inputs sit, then by first-seen order. First
        // seen alone gave Galactic Racer under UEVR each eye the other's
        // guides whenever the right eye evaluated first.
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                           const Candidate& b) {
            const auto ao = input_offset(*a.frame), bo = input_offset(*b.frame);
            if (ao != bo) return ao < bo;
            return a.first_publication < b.first_publication;
        });

        auto result = std::make_shared<DlssMotionVectorSet>();
        result->eye_count = eye_count;
        for (std::uint32_t eye = 0; eye < eye_count; ++eye) {
            if (packed_stereo) {
                auto frame = std::make_shared<DlssMotionVectorFrame>(*candidates[eye].frame);
                frame->output_x = eye * frame->output_width;
                result->eyes[eye] = std::move(frame);
            } else {
                result->eyes[eye] = candidates[eye].frame;
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
        if (status == DlssMotionVectorStatus::temporal_mismatch) {
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
