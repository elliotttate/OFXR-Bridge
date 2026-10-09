#include "xrfg/d3d12_history.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include <filesystem>
#include <fstream>
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 7;
constexpr UINT kHeight = 3;
constexpr UINT kEyeCount = 2;
constexpr UINT kBytesPerPixel = 4;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

using StereoPattern = std::array<std::vector<std::uint8_t>, kEyeCount>;
using RgbaBytes = std::array<std::uint8_t, kBytesPerPixel>;
using ReprojectionViews =
    std::array<xrfg::D3D12ReprojectionView, kEyeCount>;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void require_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        fail(std::string(operation) + " failed with HRESULT " +
             std::to_string(static_cast<std::int32_t>(result)));
    }
}

[[nodiscard]] bool operation_succeeded(HRESULT result) noexcept {
    return SUCCEEDED(result);
}

void require_frame_start_gate(
    xrfg::D3D12FrameSynthesizer& synthesizer,
    const std::string& label) {
    const HRESULT result = synthesizer.wait_for_previous_submission(500);
    require(
        operation_succeeded(result),
        label + " failed with HRESULT " +
            std::to_string(static_cast<std::int32_t>(result)));
}

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

[[nodiscard]] bool same_description(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension &&
           left.Alignment == right.Alignment &&
           left.Width == right.Width &&
           left.Height == right.Height &&
           left.DepthOrArraySize == right.DepthOrArraySize &&
           left.MipLevels == right.MipLevels &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Layout == right.Layout &&
           left.Flags == right.Flags;
}

class D3D12WarpFixture final {
public:
    explicit D3D12WarpFixture(bool use_nvidia_hardware = false) {
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_.GetAddressOf())))) {
            debug_->EnableDebugLayer();
            debug_enabled_ = true;
        }

        UINT factory_flags = debug_enabled_ ? DXGI_CREATE_FACTORY_DEBUG : 0;
        HRESULT factory_result =
            CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(factory_.GetAddressOf()));
        if (FAILED(factory_result) && factory_flags != 0) {
            factory_.Reset();
            factory_flags = 0;
            factory_result =
                CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(factory_.GetAddressOf()));
        }
        require_hresult(factory_result, "CreateDXGIFactory2");

        if (use_nvidia_hardware) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const HRESULT enumeration = factory_->EnumAdapterByGpuPreference(
                    index,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(candidate.GetAddressOf()));
                if (enumeration == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                require_hresult(
                    enumeration,
                    "IDXGIFactory6::EnumAdapterByGpuPreference");
                DXGI_ADAPTER_DESC1 description{};
                require_hresult(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
                if (description.VendorId == 0x10deU &&
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    adapter_ = candidate;
                    break;
                }
            }
            require(adapter_ != nullptr, "no NVIDIA hardware adapter found");
        } else {
            require_hresult(
                factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter_.GetAddressOf())),
                "IDXGIFactory::EnumWarpAdapter");
        }
        require_hresult(
            D3D12CreateDevice(
                adapter_.Get(),
                use_nvidia_hardware
                    ? D3D_FEATURE_LEVEL_12_0
                    : D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device_.GetAddressOf())),
            use_nvidia_hardware
                ? "D3D12CreateDevice(NVIDIA)"
                : "D3D12CreateDevice(WARP)");

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queue_description.NodeMask = 0;
        require_hresult(
            device_->CreateCommandQueue(
                &queue_description,
                IID_PPV_ARGS(queue_.GetAddressOf())),
            "ID3D12Device::CreateCommandQueue");
        require_hresult(
            device_->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence_.GetAddressOf())),
            "ID3D12Device::CreateFence");

        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(fence_event_ != nullptr, "CreateEventW failed");

        if (debug_enabled_) {
            (void)device_.As(&info_queue_);
            if (info_queue_ != nullptr) {
                info_queue_->ClearStoredMessages();
            }
        }
    }

    ~D3D12WarpFixture() {
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
        }
    }

    D3D12WarpFixture(const D3D12WarpFixture&) = delete;
    D3D12WarpFixture& operator=(const D3D12WarpFixture&) = delete;

    [[nodiscard]] ID3D12Device* device() const noexcept {
        return device_.Get();
    }

    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept {
        return queue_.Get();
    }

    template <typename Recorder>
    void execute_and_wait(Recorder&& recorder) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        require_hresult(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator.GetAddressOf())),
            "ID3D12Device::CreateCommandAllocator");
        require_hresult(
            device_->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(),
                nullptr,
                IID_PPV_ARGS(command_list.GetAddressOf())),
            "ID3D12Device::CreateCommandList");

        recorder(command_list.Get());
        require_hresult(command_list->Close(), "ID3D12GraphicsCommandList::Close");
        ID3D12CommandList* command_lists[] = {command_list.Get()};
        queue_->ExecuteCommandLists(1, command_lists);

        const std::uint64_t fence_value = next_fence_value_++;
        require_hresult(queue_->Signal(fence_.Get(), fence_value), "ID3D12CommandQueue::Signal");
        wait_for_fence(fence_.Get(), fence_value);
    }

    void wait_for_fence(ID3D12Fence* fence, std::uint64_t value) {
        require(fence != nullptr, "cannot wait on a null fence");
        const std::uint64_t completed = fence->GetCompletedValue();
        require(
            completed != std::numeric_limits<std::uint64_t>::max(),
            "D3D12 fence reports device removal");
        if (completed >= value) {
            return;
        }

        require_hresult(
            fence->SetEventOnCompletion(value, fence_event_),
            "ID3D12Fence::SetEventOnCompletion");
        require(
            WaitForSingleObject(fence_event_, 10'000) == WAIT_OBJECT_0,
            "timed out waiting for WARP command completion");
        require(
            fence->GetCompletedValue() >= value,
            "D3D12 fence event fired before the requested value completed");
    }

    void require_no_debug_errors() const {
        if (!debug_enabled_ || info_queue_ == nullptr) {
            return;
        }

        const std::uint64_t message_count =
            info_queue_->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (std::uint64_t index = 0; index < message_count; ++index) {
            SIZE_T message_size = 0;
            require_hresult(
                info_queue_->GetMessage(index, nullptr, &message_size),
                "ID3D12InfoQueue::GetMessage(size)");
            std::vector<std::byte> storage(message_size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            require_hresult(
                info_queue_->GetMessage(index, message, &message_size),
                "ID3D12InfoQueue::GetMessage");
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                fail(std::string("D3D12 debug-layer error: ") +
                     (message->pDescription == nullptr ? "<no description>"
                                                       : message->pDescription));
            }
        }
    }

private:
    ComPtr<ID3D12Debug> debug_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    HANDLE fence_event_{};
    std::uint64_t next_fence_value_{1};
    bool debug_enabled_{};
};

[[nodiscard]] D3D12_RESOURCE_DESC stereo_texture_description(
    UINT width = kWidth,
    UINT height = kHeight) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = static_cast<UINT16>(kEyeCount);
    description.MipLevels = 1;
    description.Format = kFormat;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_source_texture(
    D3D12WarpFixture& fixture,
    UINT width = kWidth,
    UINT height = kHeight,
    // The layer's private swapchains rest in COMMON when the synthesis runs
    // on a queue of its own, because that is the state D3D12 wants at a queue
    // ownership transfer. A destination created in any other state would not
    // match the first barrier those command lists record.
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
    UINT16 slices = kEyeCount,
    DXGI_FORMAT format = kFormat) {
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.DepthOrArraySize = slices;
    description.Format = format;
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            initial_state,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(source texture)");
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_buffer(
    D3D12WarpFixture& fixture,
    D3D12_HEAP_TYPE heap_type,
    std::uint64_t size,
    D3D12_RESOURCE_STATES initial_state);

[[nodiscard]] ComPtr<ID3D12Resource> create_single_slice_texture(
    D3D12WarpFixture& fixture,
    UINT width,
    UINT height) {
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.DepthOrArraySize = 1;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(single-slice texture)");
    return texture;
}

void clear_double_wide_pattern(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    UINT half_width,
    UINT height) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require_hresult(
        fixture.device()->CreateDescriptorHeap(
            &heap_description,
            IID_PPV_ARGS(rtv_heap.GetAddressOf())),
        "ID3D12Device::CreateDescriptorHeap(double-wide RTV)");

    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = kFormat;
    rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv_description.Texture2DArray.MipSlice = 0;
    rtv_description.Texture2DArray.FirstArraySlice = 0;
    rtv_description.Texture2DArray.ArraySize = 1;
    fixture.device()->CreateRenderTargetView(
        texture,
        &rtv_description,
        rtv_heap->GetCPUDescriptorHandleForHeapStart());

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        constexpr FLOAT kLeftColor[4] = {1.0F, 0.0F, 0.0F, 1.0F};
        constexpr FLOAT kRightColor[4] = {0.0F, 0.0F, 1.0F, 1.0F};
        const D3D12_RECT left_rect{
            0, 0, static_cast<LONG>(half_width), static_cast<LONG>(height)};
        const D3D12_RECT right_rect{
            static_cast<LONG>(half_width),
            0,
            static_cast<LONG>(half_width * 2U),
            static_cast<LONG>(height)};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        command_list->ClearRenderTargetView(rtv, kLeftColor, 1, &left_rect);
        command_list->ClearRenderTargetView(rtv, kRightColor, 1, &right_rect);
    });
}

[[nodiscard]] std::vector<std::uint8_t> readback_single_slice(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES initial_state) {
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    require(
        description.DepthOrArraySize == 1 &&
            description.Width <= std::numeric_limits<UINT>::max(),
        "single-slice readback texture shape is unsupported");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &description,
        0,
        1,
        0,
        &layout,
        &row_count,
        &row_size,
        &total_size);
    require(total_size != 0, "single-slice readback layout is empty");
    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            texture, initial_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;
        command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        const D3D12_RESOURCE_BARRIER restore = transition_barrier(
            texture, D3D12_RESOURCE_STATE_COPY_SOURCE, initial_state);
        command_list->ResourceBarrier(1, &restore);
    });

    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    require(
        row_count == height && row_size == width * kBytesPerPixel,
        "single-slice readback layout is unexpected");
    std::vector<std::uint8_t> output(
        static_cast<std::size_t>(width) * height * kBytesPerPixel);
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped),
        "ID3D12Resource::Map(single-slice readback)");
    const auto* bytes = static_cast<const std::byte*>(mapped);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(
            output.data() + static_cast<std::size_t>(y) * width * kBytesPerPixel,
            bytes + layout.Offset +
                static_cast<std::size_t>(y) * layout.Footprint.RowPitch,
            static_cast<std::size_t>(row_size));
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return output;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_depth_source_texture(
    D3D12WarpFixture& fixture,
    D3D12_RESOURCE_FLAGS additional_flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC description = stereo_texture_description();
    description.Format = kDepthFormat;
    description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | additional_flags);

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = kDepthFormat;
    clear_value.DepthStencil.Depth = 1.0F;
    clear_value.DepthStencil.Stencil = 0;

    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear_value,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(depth source texture)");
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_buffer(
    D3D12WarpFixture& fixture,
    D3D12_HEAP_TYPE heap_type,
    std::uint64_t size,
    D3D12_RESOURCE_STATES initial_state) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES properties = heap_properties(heap_type);
    ComPtr<ID3D12Resource> buffer;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            initial_state,
            nullptr,
            IID_PPV_ARGS(buffer.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(buffer)");
    return buffer;
}

[[nodiscard]] StereoPattern make_pattern(std::uint8_t seed) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kWidth + x) * kBytesPerPixel;
                bytes[offset + 0] = static_cast<std::uint8_t>(seed + eye * 37U + x * 11U + y * 3U);
                bytes[offset + 1] = static_cast<std::uint8_t>(seed + eye * 19U + x * 5U + y * 17U);
                bytes[offset + 2] = static_cast<std::uint8_t>(seed + eye * 53U + x * 7U + y * 13U);
                bytes[offset + 3] = static_cast<std::uint8_t>(255U - eye * 7U - x - y);
            }
        }
    }
    return pattern;
}

[[nodiscard]] ReprojectionViews make_reprojection_views(float yaw_radians = 0.0F) {
    ReprojectionViews views{};
    const float half_yaw = yaw_radians * 0.5F;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        views[eye].pose.orientation.y = std::sin(half_yaw);
        views[eye].pose.orientation.w = std::cos(half_yaw);
        views[eye].pose.position.x = eye == 0 ? -0.032F : 0.032F;
        views[eye].fov.angle_left = eye == 0 ? -0.7853981633974483F
                                             : -0.7504915783575616F;
        views[eye].fov.angle_right = eye == 0 ? 0.7504915783575616F
                                              : 0.7853981633974483F;
        views[eye].fov.angle_up = eye == 0 ? 0.6108652381980153F
                                           : 0.5934119456780721F;
        views[eye].fov.angle_down = eye == 0 ? -0.5934119456780721F
                                             : -0.6108652381980153F;
    }
    return views;
}

[[nodiscard]] StereoPattern make_solid_pattern(
    const std::array<RgbaBytes, kEyeCount>& colors) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (std::size_t offset = 0; offset < bytes.size(); offset += kBytesPerPixel) {
            std::copy(colors[eye].begin(), colors[eye].end(), bytes.begin() + offset);
        }
    }
    return pattern;
}

[[nodiscard]] StereoPattern midpoint_pattern(
    const StereoPattern& previous,
    const StereoPattern& current) {
    StereoPattern midpoint;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(
            previous[eye].size() == current[eye].size(),
            "cannot compute a midpoint for differently sized stereo patterns");
        midpoint[eye].resize(previous[eye].size());
        for (std::size_t index = 0; index < previous[eye].size(); ++index) {
            midpoint[eye][index] = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(previous[eye][index]) +
                 static_cast<std::uint16_t>(current[eye][index])) /
                2U);
        }
    }
    return midpoint;
}

[[nodiscard]] RgbaBytes motion_texel(UINT x, UINT y, UINT eye) noexcept {
    const float xf = static_cast<float>(x);
    const float yf = static_cast<float>(y);
    const float eye_phase = static_cast<float>(eye) * 0.7F;
    const auto channel = [](float value) noexcept {
        return static_cast<std::uint8_t>(std::lround(
            std::clamp(value, 0.0F, 255.0F)));
    };
    return {
        channel(128.0F + 82.0F * std::sin(xf * 0.061F + eye_phase) +
                31.0F * std::cos(yf * 0.093F)),
        channel(126.0F + 76.0F * std::sin((xf + yf) * 0.047F + eye_phase) +
                34.0F * std::cos((xf - yf) * 0.037F)),
        channel(124.0F + 69.0F * std::cos(xf * 0.041F - eye_phase) +
                41.0F * std::sin(yf * 0.071F)),
        255,
    };
}

[[nodiscard]] StereoPattern translated_motion_pattern(
    UINT width,
    UINT height,
    const std::array<int, kEyeCount>& translations) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(
            static_cast<std::size_t>(width) * height * kBytesPerPixel,
            0);
        for (std::size_t offset = 3; offset < bytes.size(); offset += kBytesPerPixel) {
            bytes[offset] = 255;
        }
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const int target_x = static_cast<int>(x) + translations[eye];
                if (target_x < 0 || target_x >= static_cast<int>(width)) {
                    continue;
                }
                const RgbaBytes texel = motion_texel(x, y, eye);
                const std::size_t destination =
                    (static_cast<std::size_t>(y) * width +
                     static_cast<std::size_t>(target_x)) *
                    kBytesPerPixel;
                std::copy(texel.begin(), texel.end(), bytes.begin() + destination);
            }
        }
    }
    return pattern;
}

[[nodiscard]] xrfg::Vec3 cross_product(
    const xrfg::Vec3& left,
    const xrfg::Vec3& right) noexcept {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] xrfg::Vec3 rotate_vector(
    const xrfg::Quaternion& quaternion,
    const xrfg::Vec3& input) noexcept {
    const xrfg::Vec3 axis{quaternion.x, quaternion.y, quaternion.z};
    const xrfg::Vec3 inner = cross_product(axis, input);
    const xrfg::Vec3 adjusted{
        inner.x + quaternion.w * input.x,
        inner.y + quaternion.w * input.y,
        inner.z + quaternion.w * input.z,
    };
    const xrfg::Vec3 outer = cross_product(axis, adjusted);
    return {
        input.x + 2.0F * outer.x,
        input.y + 2.0F * outer.y,
        input.z + 2.0F * outer.z,
    };
}

[[nodiscard]] std::uint8_t direction_channel(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
}

[[nodiscard]] StereoPattern ray_direction_pattern(
    UINT width,
    UINT height,
    const ReprojectionViews& views) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
        const auto& view = views[eye];
        const float left = std::tan(view.fov.angle_left);
        const float right = std::tan(view.fov.angle_right);
        const float top = std::tan(view.fov.angle_up);
        const float bottom = std::tan(view.fov.angle_down);
        const xrfg::D3D12ImageRect rect =
            view.image_rect.width == 0 && view.image_rect.height == 0
                ? xrfg::D3D12ImageRect{0, 0, width, height}
                : view.image_rect;
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
                if (x < rect.offset_x || y < rect.offset_y ||
                    x >= rect.offset_x + rect.width ||
                    y >= rect.offset_y + rect.height) {
                    bytes[offset + 0] = 0;
                    bytes[offset + 1] = 0;
                    bytes[offset + 2] = 0;
                    bytes[offset + 3] = 255;
                    continue;
                }
                const float u =
                    (static_cast<float>(x - rect.offset_x) + 0.5F) /
                    static_cast<float>(rect.width);
                const float v =
                    (static_cast<float>(y - rect.offset_y) + 0.5F) /
                    static_cast<float>(rect.height);
                const float tangent_x = left + (right - left) * u;
                const float tangent_y = top + (bottom - top) * v;
                const xrfg::Vec3 world_ray = rotate_vector(
                    view.pose.orientation,
                    {tangent_x, tangent_y, -1.0F});
                const float azimuth = std::atan2(world_ray.x, -world_ray.z);
                const float elevation = std::atan2(
                    world_ray.y,
                    std::sqrt(world_ray.x * world_ray.x +
                              world_ray.z * world_ray.z));
                const float red =
                    127.5F + 74.0F * std::sin(azimuth * 7.0F + elevation * 2.0F) +
                    31.0F * std::cos(azimuth * 3.0F - elevation * 5.0F);
                const float green =
                    127.5F + 69.0F * std::cos(azimuth * 5.0F + elevation * 4.0F) +
                    27.0F * std::sin(azimuth * 11.0F - elevation * 2.0F);
                const float blue =
                    127.5F + 71.0F * std::sin(azimuth * 9.0F - elevation * 3.0F) +
                    29.0F * std::cos(azimuth * 4.0F + elevation * 6.0F);
                bytes[offset + 0] = direction_channel(red);
                bytes[offset + 1] = direction_channel(green);
                bytes[offset + 2] = direction_channel(blue);
                bytes[offset + 3] = 255;
            }
        }
    }
    return pattern;
}

[[nodiscard]] bool target_pixel_maps_to_source(
    const xrfg::D3D12ReprojectionView& source,
    const xrfg::D3D12ReprojectionView& target,
    UINT width,
    UINT height,
    UINT x,
    UINT y,
    float* mapped_x = nullptr,
    float* mapped_y = nullptr) noexcept {
    const xrfg::D3D12ImageRect source_rect =
        source.image_rect.width == 0 && source.image_rect.height == 0
            ? xrfg::D3D12ImageRect{0, 0, width, height}
            : source.image_rect;
    const xrfg::D3D12ImageRect target_rect =
        target.image_rect.width == 0 && target.image_rect.height == 0
            ? xrfg::D3D12ImageRect{0, 0, width, height}
            : target.image_rect;
    if (x < target_rect.offset_x || y < target_rect.offset_y ||
        x >= target_rect.offset_x + target_rect.width ||
        y >= target_rect.offset_y + target_rect.height) {
        return false;
    }
    const float u =
        (static_cast<float>(x - target_rect.offset_x) + 0.5F) /
        static_cast<float>(target_rect.width);
    const float v =
        (static_cast<float>(y - target_rect.offset_y) + 0.5F) /
        static_cast<float>(target_rect.height);
    const float target_x = std::tan(target.fov.angle_left) +
                           (std::tan(target.fov.angle_right) -
                            std::tan(target.fov.angle_left)) *
                               u;
    const float target_y = std::tan(target.fov.angle_up) +
                           (std::tan(target.fov.angle_down) -
                            std::tan(target.fov.angle_up)) *
                               v;
    const xrfg::Vec3 world_ray = rotate_vector(
        target.pose.orientation,
        {target_x, target_y, -1.0F});
    const xrfg::Quaternion inverse_source{
        -source.pose.orientation.x,
        -source.pose.orientation.y,
        -source.pose.orientation.z,
        source.pose.orientation.w,
    };
    const xrfg::Vec3 source_ray = rotate_vector(inverse_source, world_ray);
    if (source_ray.z >= -1.0e-5F) {
        return false;
    }
    const float tangent_x = source_ray.x / -source_ray.z;
    const float tangent_y = source_ray.y / -source_ray.z;
    const float left = std::tan(source.fov.angle_left);
    const float right = std::tan(source.fov.angle_right);
    const float top = std::tan(source.fov.angle_up);
    const float bottom = std::tan(source.fov.angle_down);
    const float source_u = (tangent_x - left) / (right - left);
    const float source_v = (tangent_y - top) / (bottom - top);
    const float source_x = static_cast<float>(source_rect.offset_x) +
                           source_u * static_cast<float>(source_rect.width) -
                           0.5F;
    const float source_y = static_cast<float>(source_rect.offset_y) +
                           source_v * static_cast<float>(source_rect.height) -
                           0.5F;
    if (mapped_x != nullptr) {
        *mapped_x = source_x;
    }
    if (mapped_y != nullptr) {
        *mapped_y = source_y;
    }
    return source_x >= static_cast<float>(source_rect.offset_x) - 0.5F &&
           source_y >= static_cast<float>(source_rect.offset_y) - 0.5F &&
           source_x <= static_cast<float>(source_rect.offset_x + source_rect.width) - 0.5F &&
           source_y <= static_cast<float>(source_rect.offset_y + source_rect.height) - 0.5F;
}

[[nodiscard]] double mean_absolute_rgb_error_rect(
    const StereoPattern& actual,
    const StereoPattern& expected,
    UINT texture_width,
    UINT eye,
    const xrfg::D3D12ImageRect& rect,
    UINT margin) {
    require(
        eye < kEyeCount && margin * 2U < rect.width &&
            margin * 2U < rect.height &&
            actual[eye].size() == expected[eye].size(),
        "invalid viewport MAE input");
    double total = 0.0;
    std::size_t sample_count = 0;
    for (UINT y = rect.offset_y + margin;
         y + margin < rect.offset_y + rect.height;
         ++y) {
        for (UINT x = rect.offset_x + margin;
             x + margin < rect.offset_x + rect.width;
             ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * texture_width + x) *
                kBytesPerPixel;
            for (UINT channel = 0; channel < 3; ++channel) {
                total += std::abs(
                    static_cast<int>(actual[eye][offset + channel]) -
                    static_cast<int>(expected[eye][offset + channel]));
                ++sample_count;
            }
        }
    }
    require(sample_count != 0, "viewport MAE has no samples");
    return total / static_cast<double>(sample_count);
}

[[nodiscard]] double mean_absolute_rgb_error(
    const StereoPattern& actual,
    const StereoPattern& expected,
    UINT width,
    UINT height,
    UINT eye,
    UINT margin) {
    require(
        eye < kEyeCount && margin * 2U < width && margin * 2U < height &&
            actual[eye].size() == expected[eye].size(),
        "invalid stereo MAE input");
    double total = 0.0;
    std::size_t sample_count = 0;
    for (UINT y = margin; y + margin < height; ++y) {
        for (UINT x = margin; x + margin < width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
            for (UINT channel = 0; channel < 3; ++channel) {
                total += std::abs(
                    static_cast<int>(actual[eye][offset + channel]) -
                    static_cast<int>(expected[eye][offset + channel]));
                ++sample_count;
            }
        }
    }
    require(sample_count != 0, "stereo MAE has no samples");
    return total / static_cast<double>(sample_count);
}

void get_copy_layouts(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& description,
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount>* layouts,
    std::array<UINT, kEyeCount>* row_counts,
    std::array<UINT64, kEyeCount>* row_sizes,
    UINT64* total_size) {
    device->GetCopyableFootprints(
        &description,
        0,
        kEyeCount,
        0,
        layouts->data(),
        row_counts->data(),
        row_sizes->data(),
        total_size);
    require(*total_size > 0, "GetCopyableFootprints returned an empty layout");
}

void upload_pattern(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const StereoPattern& pattern) {
    require(texture != nullptr, "upload target is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_UPLOAD,
        total_size,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped_memory = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped_memory), "ID3D12Resource::Map(upload)");
    auto* mapped_bytes = static_cast<std::byte*>(mapped_memory);
    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == height, "unexpected upload row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(width) * kBytesPerPixel,
            "unexpected upload row size");
        require(
            pattern[eye].size() ==
                static_cast<std::size_t>(width) * height * kBytesPerPixel,
            "upload pattern dimensions do not match the texture");
        for (UINT y = 0; y < height; ++y) {
            std::byte* destination =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            const std::uint8_t* source =
                pattern[eye].data() +
                static_cast<std::size_t>(y) * width * kBytesPerPixel;
            std::memcpy(destination, source, static_cast<std::size_t>(row_sizes[eye]));
        }
    }
    upload->Unmap(0, nullptr);

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_destination = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_DEST);
        command_list->ResourceBarrier(1, &to_copy_destination);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[eye];

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = eye;
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER to_render_target = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        command_list->ResourceBarrier(1, &to_render_target);
    });
}

template <typename MotionForPixel>
[[nodiscard]] ComPtr<ID3D12Resource> create_and_upload_game_motion_field(
    D3D12WarpFixture& fixture,
    UINT width,
    UINT height,
    MotionForPixel&& motion_for_pixel) {
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.Format = DXGI_FORMAT_R32G32_FLOAT;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(game motion)");

    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);
    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_UPLOAD,
        total_size,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped_memory = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(
        upload->Map(0, &no_read, &mapped_memory),
        "ID3D12Resource::Map(game motion upload)");
    auto* mapped_bytes = static_cast<std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == height, "unexpected game-motion row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(width) * sizeof(float) * 2U,
            "unexpected game-motion row size");
        for (UINT y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<float*>(
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch);
            for (UINT x = 0; x < width; ++x) {
                const auto motion = motion_for_pixel(eye, x, y);
                row[x * 2U] = motion[0];
                row[x * 2U + 1U] = motion[1];
            }
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = eye;
            command_list->CopyTextureRegion(
                &destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER ready = transition_barrier(
            texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        command_list->ResourceBarrier(1, &ready);
    });
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_and_upload_game_motion(
    D3D12WarpFixture& fixture,
    UINT width,
    UINT height,
    const std::array<float, kEyeCount>& horizontal_motion) {
    return create_and_upload_game_motion_field(
        fixture,
        width,
        height,
        [&](UINT eye, UINT, UINT) {
            return std::array<float, 2>{horizontal_motion[eye], 0.0F};
        });
}

[[nodiscard]] StereoPattern readback_pattern(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES initial_state) {
    require(texture != nullptr, "readback source is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    require(
        description.Width <= std::numeric_limits<UINT>::max() &&
            description.DepthOrArraySize == kEyeCount,
        "readback texture shape is unsupported");
    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            texture,
            initial_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy_source);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = eye;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[eye];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER restore_state = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            initial_state);
        command_list->ResourceBarrier(1, &restore_state);
    });

    StereoPattern result;
    void* mapped_memory = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped_memory),
        "ID3D12Resource::Map(pattern readback)");
    const auto* mapped_bytes = static_cast<const std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(
            row_counts[eye] == height &&
                row_sizes[eye] == static_cast<UINT64>(width) * kBytesPerPixel,
            "unexpected pattern readback layout");
        result[eye].resize(
            static_cast<std::size_t>(width) * height * kBytesPerPixel);
        for (UINT y = 0; y < height; ++y) {
            const std::byte* source =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            std::uint8_t* destination =
                result[eye].data() +
                static_cast<std::size_t>(y) * width * kBytesPerPixel;
            std::memcpy(destination, source, static_cast<std::size_t>(row_sizes[eye]));
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return result;
}

void require_readback_matches(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const StereoPattern& expected,
    const char* label,
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
    require(texture != nullptr, std::string(label) + " resource is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            texture,
            initial_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy_source);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = eye;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[eye];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER restore_state = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            initial_state);
        command_list->ResourceBarrier(1, &restore_state);
    });

    void* mapped_memory = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped_memory),
        "ID3D12Resource::Map(readback)");
    const auto* mapped_bytes = static_cast<const std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (UINT y = 0; y < kHeight; ++y) {
            const std::byte* actual =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            const std::uint8_t* wanted =
                expected[eye].data() +
                static_cast<std::size_t>(y) * kWidth * kBytesPerPixel;
            if (std::memcmp(actual, wanted, static_cast<std::size_t>(row_sizes[eye])) != 0) {
                std::size_t first_difference = 0;
                while (first_difference < row_sizes[eye] &&
                       actual[first_difference] ==
                           static_cast<std::byte>(wanted[first_difference])) {
                    ++first_difference;
                }
                const unsigned actual_value = first_difference < row_sizes[eye]
                    ? std::to_integer<unsigned>(actual[first_difference])
                    : 0U;
                const unsigned wanted_value = first_difference < row_sizes[eye]
                    ? wanted[first_difference]
                    : 0U;
                readback->Unmap(0, nullptr);
                fail(std::string(label) + " pixel mismatch at eye " +
                     std::to_string(eye) + ", row " + std::to_string(y) +
                     ", byte " + std::to_string(first_difference) +
                     ": actual=" + std::to_string(actual_value) +
                     " expected=" + std::to_string(wanted_value));
            }
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
}

void require_source_is_render_target(
    D3D12WarpFixture& fixture,
    ID3D12Resource* source) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = 1;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require_hresult(
        fixture.device()->CreateDescriptorHeap(
            &heap_description,
            IID_PPV_ARGS(rtv_heap.GetAddressOf())),
        "ID3D12Device::CreateDescriptorHeap(RTV)");

    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = kFormat;
    rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv_description.Texture2DArray.MipSlice = 0;
    rtv_description.Texture2DArray.FirstArraySlice = 0;
    rtv_description.Texture2DArray.ArraySize = kEyeCount;
    rtv_description.Texture2DArray.PlaneSlice = 0;
    fixture.device()->CreateRenderTargetView(
        source,
        &rtv_description,
        rtv_heap->GetCPUDescriptorHandleForHeapStart());

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        constexpr FLOAT clear_color[4] = {0.125F, 0.25F, 0.5F, 1.0F};
        command_list->ClearRenderTargetView(
            rtv_heap->GetCPUDescriptorHandleForHeapStart(),
            clear_color,
            0,
            nullptr);
    });
}

void clear_depth_slices(
    D3D12WarpFixture& fixture,
    ID3D12Resource* source,
    const std::array<float, kEyeCount>& depths) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_description.NumDescriptors = kEyeCount;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    require_hresult(
        fixture.device()->CreateDescriptorHeap(
            &heap_description,
            IID_PPV_ARGS(dsv_heap.GetAddressOf())),
        "ID3D12Device::CreateDescriptorHeap(DSV)");

    const UINT descriptor_size =
        fixture.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const D3D12_CPU_DESCRIPTOR_HANDLE first_handle =
        dsv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_description{};
        dsv_description.Format = kDepthFormat;
        dsv_description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv_description.Flags = D3D12_DSV_FLAG_NONE;
        dsv_description.Texture2DArray.MipSlice = 0;
        dsv_description.Texture2DArray.FirstArraySlice = eye;
        dsv_description.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = first_handle;
        handle.ptr += static_cast<SIZE_T>(eye) * descriptor_size;
        fixture.device()->CreateDepthStencilView(source, &dsv_description, handle);
    }

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = first_handle;
            handle.ptr += static_cast<SIZE_T>(eye) * descriptor_size;
            command_list->ClearDepthStencilView(
                handle,
                D3D12_CLEAR_FLAG_DEPTH,
                depths[eye],
                0,
                0,
                nullptr);
        }
    });
}

void require_depth_readback_matches(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const std::array<float, kEyeCount>& expected,
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
    require(texture != nullptr, "depth history resource is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            texture,
            initial_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy_source);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = eye;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[eye];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER restore_state = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            initial_state);
        command_list->ResourceBarrier(1, &restore_state);
    });

    void* mapped_memory = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped_memory),
        "ID3D12Resource::Map(depth readback)");
    const auto* mapped_bytes = static_cast<const std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == kHeight, "unexpected depth readback row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(kWidth) * sizeof(float),
            "unexpected depth readback row size");
        for (UINT y = 0; y < kHeight; ++y) {
            const std::byte* row =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            for (UINT x = 0; x < kWidth; ++x) {
                float actual = 0.0F;
                std::memcpy(&actual, row + static_cast<std::size_t>(x) * sizeof(float), sizeof(actual));
                if (std::fabs(actual - expected[eye]) > 1.0e-6F) {
                    readback->Unmap(0, nullptr);
                    fail(
                        "depth pixel mismatch at eye " + std::to_string(eye) +
                        ", x " + std::to_string(x) + ", y " + std::to_string(y));
                }
            }
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
}

struct CaptureResources {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Fence> fence;
    xrfg::D3D12HistoryConsumerLease lease{};
};

[[nodiscard]] CaptureResources acquire_capture_for_inspection(
    xrfg::D3D12SwapchainHistory& history,
    const xrfg::D3D12HistoryCaptureTicket& ticket) {
    ID3D12Resource* raw_resource = nullptr;
    ID3D12Fence* raw_fence = nullptr;
    xrfg::D3D12HistoryConsumerLease lease{};
    require(
        operation_succeeded(history.acquire_consumer(
            ticket,
            &lease,
            &raw_resource,
            &raw_fence)),
        "acquire_consumer rejected a current ticket");
    require(raw_resource != nullptr, "acquire_consumer returned a null texture");
    require(raw_fence != nullptr, "acquire_consumer returned a null fence");

    CaptureResources resources;
    resources.resource.Attach(raw_resource);
    resources.fence.Attach(raw_fence);
    resources.lease = lease;
    return resources;
}

void retire_completed_inspection(
    D3D12WarpFixture& fixture,
    xrfg::D3D12SwapchainHistory& history,
    const CaptureResources& resources) {
    ComPtr<ID3D12Fence> completed_fence;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(completed_fence.GetAddressOf())),
        "ID3D12Device::CreateFence(completed consumer)");
    require_hresult(
        completed_fence->Signal(1),
        "ID3D12Fence::Signal(completed consumer)");
    require(
        operation_succeeded(history.retire_consumer(resources.lease, completed_fence.Get(), 1)),
        "completed consumer lease retirement failed");
}

void require_private_resource_shape(
    ID3D12Resource* source,
    ID3D12Resource* history_resource) {
    require(source != nullptr && history_resource != nullptr, "cannot compare null resources");
    require(source != history_resource, "history aliases an application swapchain image");
    require(
        same_description(source->GetDesc(), history_resource->GetDesc()),
        "private history texture description does not match the source");

    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    require_hresult(
        history_resource->GetHeapProperties(&properties, &flags),
        "ID3D12Resource::GetHeapProperties(history)");
    require(properties.Type == D3D12_HEAP_TYPE_DEFAULT, "history texture is not on a DEFAULT heap");
    constexpr D3D12_HEAP_FLAGS kExternallyVisibleHeapFlags =
        D3D12_HEAP_FLAG_SHARED |
        D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
        D3D12_HEAP_FLAG_HARDWARE_PROTECTED;
    require(
        (flags & kExternallyVisibleHeapFlags) == D3D12_HEAP_FLAG_NONE,
        "history texture is shared, cross-adapter, or hardware-protected");
}

void test_stereo_capture_ring(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
        sources[2].Get(),
    };

    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "D3D12SwapchainHistory::initialize failed");
    require(history.initialized(), "history did not report initialized state");
    require(xrfg::D3D12SwapchainHistory::kSlotCount == 3, "history ring is not three slots");

    const StereoPattern pattern_a = make_pattern(11);
    const StereoPattern pattern_b = make_pattern(73);
    const StereoPattern pattern_c = make_pattern(149);

    upload_pattern(fixture, sources[2].Get(), pattern_a);
    xrfg::D3D12HistoryCaptureTicket ticket_a{};
    require(
        operation_succeeded(history.capture(2, &ticket_a)),
        "capture A failed");
    require(
        ticket_a.serial == 1 && ticket_a.slot == 0 && ticket_a.source_index == 2 &&
            ticket_a.fence_value != 0,
        "capture A ticket is incorrect");
    require(operation_succeeded(history.commit(ticket_a)), "commit A failed");
    CaptureResources capture_a = acquire_capture_for_inspection(history, ticket_a);
    fixture.wait_for_fence(capture_a.fence.Get(), ticket_a.fence_value);
    require_private_resource_shape(sources[2].Get(), capture_a.resource.Get());
    require_readback_matches(fixture, capture_a.resource.Get(), pattern_a, "capture A");
    retire_completed_inspection(fixture, history, capture_a);

    upload_pattern(fixture, sources[0].Get(), pattern_b);
    xrfg::D3D12HistoryCaptureTicket ticket_b{};
    require(
        operation_succeeded(history.capture(0, &ticket_b)),
        "capture B failed");
    require(
        ticket_b.serial == 2 && ticket_b.slot == 1 && ticket_b.source_index == 0 &&
            ticket_b.fence_value > ticket_a.fence_value,
        "capture B ticket is incorrect");
    require(operation_succeeded(history.commit(ticket_b)), "commit B failed");
    CaptureResources capture_b = acquire_capture_for_inspection(history, ticket_b);
    fixture.wait_for_fence(capture_b.fence.Get(), ticket_b.fence_value);
    require(capture_a.resource.Get() != capture_b.resource.Get(), "A and B use the same history slot");
    require_private_resource_shape(sources[0].Get(), capture_b.resource.Get());
    require_readback_matches(fixture, capture_b.resource.Get(), pattern_b, "capture B");
    retire_completed_inspection(fixture, history, capture_b);
    CaptureResources capture_a_after_b = acquire_capture_for_inspection(history, ticket_a);
    fixture.wait_for_fence(capture_a_after_b.fence.Get(), ticket_a.fence_value);
    require_readback_matches(
        fixture,
        capture_a_after_b.resource.Get(),
        pattern_a,
        "capture A after B");
    retire_completed_inspection(fixture, history, capture_a_after_b);

    const StereoPattern discarded_pattern = make_pattern(103);
    upload_pattern(fixture, sources[1].Get(), discarded_pattern);
    xrfg::D3D12HistoryCaptureTicket discarded_ticket{};
    require(
        operation_succeeded(history.capture(1, &discarded_ticket)),
        "discard candidate capture failed");
    require(
        discarded_ticket.serial == 3 && discarded_ticket.slot == 2 &&
            discarded_ticket.source_index == 1 &&
            discarded_ticket.fence_value > ticket_b.fence_value,
        "discard candidate ticket is incorrect");
    history.discard(discarded_ticket);

    xrfg::D3D12HistoryConsumerLease discarded_lease{};
    ID3D12Resource* discarded_resource = nullptr;
    ID3D12Fence* discarded_fence = nullptr;
    require(
        !operation_succeeded(history.acquire_consumer(
            discarded_ticket,
            &discarded_lease,
            &discarded_resource,
            &discarded_fence)),
        "discarded ticket remained valid");
    require(
        discarded_resource == nullptr && discarded_fence == nullptr,
        "discarded ticket returned COM objects");
    require(
        operation_succeeded(history.wait_for_idle()),
        "discarded capture did not drain");

    upload_pattern(fixture, sources[2].Get(), pattern_c);
    xrfg::D3D12HistoryCaptureTicket ticket_c{};
    require(
        operation_succeeded(history.capture(2, &ticket_c)),
        "capture C failed");
    require(
        ticket_c.serial == 3 && ticket_c.slot == 2 && ticket_c.source_index == 2 &&
            ticket_c.fence_value > discarded_ticket.fence_value,
        "capture C ticket is incorrect");
    require(
        ticket_c.serial == discarded_ticket.serial && ticket_c.slot == discarded_ticket.slot,
        "capture after discard did not reuse the same serial and slot");
    require(operation_succeeded(history.commit(ticket_c)), "commit C failed");
    CaptureResources capture_c = acquire_capture_for_inspection(history, ticket_c);
    fixture.wait_for_fence(capture_c.fence.Get(), ticket_c.fence_value);
    require(
        capture_c.resource.Get() != capture_a.resource.Get() &&
            capture_c.resource.Get() != capture_b.resource.Get(),
        "capture C did not use the independent third history slot");
    require_readback_matches(fixture, capture_c.resource.Get(), pattern_c, "capture C");
    retire_completed_inspection(fixture, history, capture_c);
    CaptureResources capture_b_after_c = acquire_capture_for_inspection(history, ticket_b);
    fixture.wait_for_fence(capture_b_after_c.fence.Get(), ticket_b.fence_value);
    require_readback_matches(
        fixture,
        capture_b_after_c.resource.Get(),
        pattern_b,
        "capture B after C");
    retire_completed_inspection(fixture, history, capture_b_after_c);
    CaptureResources capture_a_after_c = acquire_capture_for_inspection(history, ticket_a);
    fixture.wait_for_fence(capture_a_after_c.fence.Get(), ticket_a.fence_value);
    require_readback_matches(
        fixture,
        capture_a_after_c.resource.Get(),
        pattern_a,
        "capture A retained across the third slot");
    retire_completed_inspection(fixture, history, capture_a_after_c);

    const HRESULT reinitialize_result = history.initialize(
        fixture.device(),
        fixture.queue(),
        std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(FAILED(reinitialize_result), "second initialize unexpectedly succeeded");
    CaptureResources capture_b_after_reinitialize =
        acquire_capture_for_inspection(history, ticket_b);
    CaptureResources capture_c_after_reinitialize =
        acquire_capture_for_inspection(history, ticket_c);
    require(
        capture_b_after_reinitialize.resource.Get() == capture_b.resource.Get() &&
            capture_c_after_reinitialize.resource.Get() == capture_c.resource.Get(),
        "failed reinitialize invalidated or replaced committed captures");
    history.cancel_consumer(capture_b_after_reinitialize.lease);
    history.cancel_consumer(capture_c_after_reinitialize.lease);

    require(operation_succeeded(history.invalidate()), "history invalidate failed");
    for (const auto& invalidated_ticket : {ticket_b, ticket_c}) {
        xrfg::D3D12HistoryConsumerLease invalidated_lease{};
        ID3D12Resource* invalidated_resource = nullptr;
        ID3D12Fence* invalidated_fence = nullptr;
        require(
            !operation_succeeded(history.acquire_consumer(
                invalidated_ticket,
                &invalidated_lease,
                &invalidated_resource,
                &invalidated_fence)),
            "invalidate left an old committed ticket resolvable");
        require(
            invalidated_resource == nullptr && invalidated_fence == nullptr,
            "invalidated ticket returned COM objects");
    }

    const StereoPattern pattern_d = make_pattern(181);
    upload_pattern(fixture, sources[1].Get(), pattern_d);
    xrfg::D3D12HistoryCaptureTicket ticket_d{};
    require(
        operation_succeeded(history.capture(1, &ticket_d)),
        "capture after invalidate failed");
    require(
        ticket_d.serial > ticket_c.serial && ticket_d.serial == 4 &&
            ticket_d.slot == 0 && ticket_d.source_index == 1 &&
            ticket_d.fence_value > ticket_c.fence_value,
        "capture after invalidate reset serial/fence identity");
    require(
        operation_succeeded(history.commit(ticket_d)),
        "capture after invalidate commit failed");
    CaptureResources capture_d = acquire_capture_for_inspection(history, ticket_d);
    fixture.wait_for_fence(capture_d.fence.Get(), ticket_d.fence_value);
    require_readback_matches(fixture, capture_d.resource.Get(), pattern_d, "capture D after invalidate");
    retire_completed_inspection(fixture, history, capture_d);

    xrfg::D3D12HistoryConsumerLease stale_lease{};
    ID3D12Resource* stale_resource = nullptr;
    ID3D12Fence* stale_fence = nullptr;
    require(
        !operation_succeeded(history.acquire_consumer(
            ticket_a,
            &stale_lease,
            &stale_resource,
            &stale_fence)),
        "overwritten capture A ticket remained valid");
    require(
        stale_resource == nullptr && stale_fence == nullptr,
        "rejected stale ticket returned COM objects");

    require_source_is_render_target(fixture, sources[1].Get());
    require(operation_succeeded(history.wait_for_idle()), "history wait_for_idle failed");
}

void test_capture_is_async_and_consumer_fence_blocks_reuse(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
        sources[2].Get(),
    };
    const StereoPattern pattern = make_pattern(211);
    upload_pattern(fixture, sources[0].Get(), pattern);

    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "gate history initialization failed");

    ComPtr<ID3D12Fence> gate;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(gate.GetAddressOf())),
        "ID3D12Device::CreateFence(gate)");
    require_hresult(fixture.queue()->Wait(gate.Get(), 1), "ID3D12CommandQueue::Wait(gate)");

    struct CaptureOutcome {
        HRESULT result{E_FAIL};
        xrfg::D3D12HistoryCaptureTicket ticket{};
    };
    std::promise<void> capture_entered;
    std::future<void> entered = capture_entered.get_future();
    std::future<CaptureOutcome> capture = std::async(
        std::launch::async,
        [&history, promise = std::move(capture_entered)]() mutable {
            promise.set_value();
            CaptureOutcome outcome{};
            outcome.result = history.capture(0, &outcome.ticket);
            return outcome;
        });
    require(
        entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "capture worker did not start");

    const bool returned_while_gpu_blocked =
        capture.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    if (!returned_while_gpu_blocked) {
        require_hresult(gate->Signal(1), "ID3D12Fence::Signal(gate cleanup)");
        fail("capture blocked the CPU on its producer fence");
    }

    const CaptureOutcome outcome = capture.get();
    require(operation_succeeded(outcome.result), "asynchronous gated capture failed");
    require(operation_succeeded(history.commit(outcome.ticket)), "gated capture commit failed");

    xrfg::D3D12HistoryConsumerLease lease{};
    ID3D12Resource* raw_resource = nullptr;
    ID3D12Fence* raw_producer_fence = nullptr;
    require(
        operation_succeeded(history.acquire_consumer(
            outcome.ticket,
            &lease,
            &raw_resource,
            &raw_producer_fence)),
        "failed to acquire the asynchronous history consumer");
    ComPtr<ID3D12Resource> leased_resource;
    ComPtr<ID3D12Fence> producer_fence;
    leased_resource.Attach(raw_resource);
    producer_fence.Attach(raw_producer_fence);
    require(
        producer_fence->GetCompletedValue() < outcome.ticket.fence_value,
        "capture producer fence completed before the gated assertion");

    ComPtr<ID3D12Fence> consumer_completion;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(consumer_completion.GetAddressOf())),
        "ID3D12Device::CreateFence(consumer completion)");
    require(
        operation_succeeded(history.retire_consumer(lease, consumer_completion.Get(), 1)),
        "consumer lease retirement failed");

    xrfg::D3D12HistoryCaptureTicket ticket_b{};
    require(
        operation_succeeded(history.capture(1, &ticket_b)) &&
            operation_succeeded(history.commit(ticket_b)),
        "the independent second ring slot did not accept an asynchronous capture");

    xrfg::D3D12HistoryCaptureTicket ticket_c{};
    require(
        operation_succeeded(history.capture(2, &ticket_c)) &&
            operation_succeeded(history.commit(ticket_c)),
        "the independent third ring slot did not accept an asynchronous capture");
    require(
        ticket_c.serial == 3 && ticket_c.slot == 2,
        "third asynchronous capture did not preserve rolling chronology");

    xrfg::D3D12HistoryCaptureTicket blocked_ticket{};
    require(
        history.capture(0, &blocked_ticket) == HRESULT_FROM_WIN32(ERROR_BUSY) &&
            blocked_ticket.serial == 0,
        "history overwrote a slot protected by an incomplete consumer fence");

    require_hresult(gate->Signal(1), "ID3D12Fence::Signal(gate)");
    fixture.wait_for_fence(producer_fence.Get(), ticket_c.fence_value);
    require(
        history.capture(0, &blocked_ticket) == HRESULT_FROM_WIN32(ERROR_BUSY),
        "producer completion bypassed the independent consumer fence");

    require_hresult(
        consumer_completion->Signal(1),
        "ID3D12Fence::Signal(consumer completion)");
    xrfg::D3D12HistoryCaptureTicket ticket_d{};
    require(
        operation_succeeded(history.capture(0, &ticket_d)) &&
            operation_succeeded(history.commit(ticket_d)),
        "history slot did not become reusable after consumer completion");
    require(
        ticket_d.slot == outcome.ticket.slot && ticket_d.serial == 4,
        "history did not preserve chronological ring identity after lease retirement");
    require(operation_succeeded(history.wait_for_idle()), "asynchronous history drain failed");
    require_source_is_render_target(fixture, sources[0].Get());
}

void test_depth_capture_path(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_depth_source_texture(fixture),
        create_depth_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
    };
    constexpr std::array<float, kEyeCount> kCapturedDepths{0.25F, 0.75F};
    clear_depth_slices(fixture, sources[1].Get(), kCapturedDepths);

    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_DEPTH_WRITE)),
        "depth history initialization failed");

    xrfg::D3D12HistoryCaptureTicket ticket{};
    require(
        operation_succeeded(history.capture(1, &ticket)),
        "depth capture failed");
    require(
        ticket.serial == 1 && ticket.slot == 0 && ticket.source_index == 1 &&
            ticket.fence_value != 0,
        "depth capture ticket is incorrect");
    require(operation_succeeded(history.commit(ticket)), "depth capture commit failed");

    CaptureResources capture = acquire_capture_for_inspection(history, ticket);
    fixture.wait_for_fence(capture.fence.Get(), ticket.fence_value);
    require_private_resource_shape(sources[1].Get(), capture.resource.Get());
    require_depth_readback_matches(fixture, capture.resource.Get(), kCapturedDepths);
    retire_completed_inspection(fixture, history, capture);

    // No transition is recorded here: the debug layer validates that capture restored
    // the OpenXR-required DEPTH_WRITE state before returning.
    constexpr std::array<float, kEyeCount> kPostCaptureDepths{0.5F, 0.625F};
    clear_depth_slices(fixture, sources[1].Get(), kPostCaptureDepths);
    require(
        operation_succeeded(history.wait_for_idle()),
        "depth history wait_for_idle failed");
}

void test_depth_private_history_allows_shader_resource_views(D3D12WarpFixture& fixture) {
    ComPtr<ID3D12Resource> source = create_depth_source_texture(
        fixture,
        D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    std::array<ID3D12Resource*, 1> source_pointers{source.Get()};
    require(
        (source->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0,
        "DENY_SHADER_RESOURCE source fixture is missing its flag");

    constexpr std::array<float, kEyeCount> kDepths{0.375F, 0.875F};
    clear_depth_slices(fixture, source.Get(), kDepths);
    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_DEPTH_WRITE)),
        "DENY_SHADER_RESOURCE history initialization failed");

    xrfg::D3D12HistoryCaptureTicket ticket{};
    require(
        operation_succeeded(history.capture(0, &ticket)),
        "DENY_SHADER_RESOURCE capture failed");
    require(
        operation_succeeded(history.commit(ticket)),
        "DENY_SHADER_RESOURCE capture commit failed");
    CaptureResources capture = acquire_capture_for_inspection(history, ticket);
    fixture.wait_for_fence(capture.fence.Get(), ticket.fence_value);
    require(
        (capture.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0,
        "private history retained DENY_SHADER_RESOURCE");
    require(
        (capture.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0,
        "private history lost depth-stencil compatibility");
    require_depth_readback_matches(fixture, capture.resource.Get(), kDepths);
    retire_completed_inspection(fixture, history, capture);
    constexpr std::array<float, kEyeCount> kPostCaptureDepths{0.125F, 0.625F};
    clear_depth_slices(fixture, source.Get(), kPostCaptureDepths);
    require(
        operation_succeeded(history.wait_for_idle()),
        "shader-readable history wait_for_idle failed");
}

// Synthesis on a queue of the layer's own, which is what the layer does for a
// native D3D12 application. The debug layer is on here, so a cross-queue
// ownership violation is reported rather than silently producing the wrong
// pixels - the failure this configuration actually had, where every timing
// record said the frame was generated, paced and complete and the runtime
// still displayed something else.
void test_synthesis_on_a_dedicated_queue(D3D12WarpFixture& fixture) {
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    ComPtr<ID3D12CommandQueue> synthesis_queue;
    require_hresult(
        fixture.device()->CreateCommandQueue(
            &queue_description, IID_PPV_ARGS(synthesis_queue.GetAddressOf())),
        "dedicated synthesis queue creation");

    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ComPtr<ID3D12Resource>, 2> synthetic_destinations{
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(), sources[1].Get(), sources[2].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 2> synthetic_pointers{
        synthetic_destinations[0].Get(), synthetic_destinations[1].Get()};

    upload_pattern(fixture, sources[0].Get(), make_solid_pattern({
        RgbaBytes{16, 40, 72, 255}, RgbaBytes{32, 56, 88, 255}}));
    upload_pattern(fixture, sources[1].Get(), make_solid_pattern({
        RgbaBytes{80, 104, 136, 255}, RgbaBytes{96, 120, 152, 255}}));

    // The history stays on the application's queue. Its capture has to be
    // ordered after the application's own rendering, which is exactly what
    // that queue gives it.
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(
                source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "dedicated-queue history initialization failed");

    // COMMON rather than RENDER_TARGET: these stand in for the layer's own
    // private swapchains, and COMMON is the state D3D12 requires at the point
    // queue ownership transfers.
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            synthesis_queue.Get(),
            history,
            std::span<ID3D12Resource* const>(
                current_pointers.data(), current_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_pointers.data(), synthetic_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "dedicated-queue synthesizer initialization failed");

    const ReprojectionViews views = make_reprojection_views();

    // A join against the queue the synthesis already runs on is redundant
    // rather than wrong, and has to say so instead of queueing a wait.
    require(
        synthesizer.synchronize_producer_queue(synthesis_queue.Get()) == S_FALSE,
        "producer join did not report the same queue as already ordered");
    require(
        operation_succeeded(
            synthesizer.synchronize_producer_queue(fixture.queue())),
        "producer join across queues failed");

    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "dedicated-queue capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime_ticket{};
    require(
        operation_succeeded(synthesizer.submit_prime(
            capture_a, views, 0, &prime_ticket)),
        "prime on the dedicated queue failed");

    // The prime is still queued; the pair tests the same submission slot, so
    // let it land first the way the frame-start poll does in the layer.
    require(
        operation_succeeded(synthesizer.wait_for_previous_submission(500)),
        "waiting out the dedicated-queue prime failed");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "dedicated-queue capture B failed");

    // Deferred, as the native D3D12 path always is. The pair then reserves two
    // fence values: the synthetic's, signalled now, and the pair's, which the
    // held-back copy signals only once flush_current_copy submits it.
    xrfg::D3D12FrameSynthesisTicket pair_ticket{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b,
            views,
            views,
            0,
            1,
            &pair_ticket,
            std::nullopt,
            {},
            true)),
        "deferred pair on the dedicated queue failed");
    require(
        pair_ticket.synthetic_fence_value != 0,
        "deferred pair reported no value for the synthetic");
    require(
        pair_ticket.synthetic_fence_value < pair_ticket.fence_value,
        "deferred pair put the synthetic at or after the pair's own value; a "
        "consumer joined on that would park until the copy is flushed a "
        "display period later");

    // The consumer join carries the synthesis back to the queue the runtime
    // orders its own reads against.
    require(
        operation_succeeded(
            synthesizer.synchronize_consumer_queue(fixture.queue(), pair_ticket)),
        "consumer join across queues failed");
    require(
        synthesizer.synchronize_consumer_queue(
            synthesis_queue.Get(), pair_ticket) == S_FALSE,
        "consumer join did not report the same queue as already ordered");

    require(
        operation_succeeded(synthesizer.flush_current_copy(
            fixture.queue(), pair_ticket.fence_value)),
        "flushing the held-back copy across queues failed");
    require(
        operation_succeeded(synthesizer.wait_for_idle()),
        "dedicated-queue synthesizer did not go idle");

    // The synthesizer falls out of scope here. Its destructor has to drain the
    // synthesis queue itself, because nothing outside waits on it. Without
    // that the slot allocators are released with work still in flight, which
    // crashed six call-chain tests at process teardown.
}

void test_rolling_frame_synthesizer(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ComPtr<ID3D12Resource>, 3> current_destinations{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ComPtr<ID3D12Resource>, 3> synthetic_destinations{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
        sources[2].Get(),
    };
    std::array<ID3D12Resource*, 3> current_destination_pointers{
        current_destinations[0].Get(),
        current_destinations[1].Get(),
        current_destinations[2].Get(),
    };
    std::array<ID3D12Resource*, 3> synthetic_destination_pointers{
        synthetic_destinations[0].Get(),
        synthetic_destinations[1].Get(),
        synthetic_destinations[2].Get(),
    };

    const StereoPattern pattern_a = make_solid_pattern({
        RgbaBytes{16, 40, 72, 255},
        RgbaBytes{32, 56, 88, 255},
    });
    const StereoPattern pattern_b = make_solid_pattern({
        RgbaBytes{80, 104, 136, 255},
        RgbaBytes{96, 120, 152, 255},
    });
    const StereoPattern pattern_c = make_solid_pattern({
        RgbaBytes{160, 184, 208, 255},
        RgbaBytes{176, 200, 224, 255},
    });
    const StereoPattern pattern_d = make_solid_pattern({
        RgbaBytes{24, 112, 200, 255},
        RgbaBytes{48, 136, 224, 255},
    });
    upload_pattern(fixture, sources[0].Get(), pattern_a);
    upload_pattern(fixture, sources[1].Get(), pattern_b);
    upload_pattern(fixture, sources[2].Get(), pattern_c);

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "rolling synthesizer history initialization failed");

    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            std::span<ID3D12Resource* const>(
                current_destination_pointers.data(),
                current_destination_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_destination_pointers.data(),
             synthetic_destination_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "rolling frame synthesizer initialization failed");
    require(synthesizer.initialized(), "rolling frame synthesizer did not report initialized");
    const ReprojectionViews static_views = make_reprojection_views();

    xrfg::D3D12FrameSynthesizer invalid_synthesizer;
    require(
        FAILED(invalid_synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            {},
            std::span<ID3D12Resource* const>(
                synthetic_destination_pointers.data(),
                synthetic_destination_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "synthesizer accepted an empty current-destination set");

    ComPtr<ID3D12Fence> gate;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(gate.GetAddressOf())),
        "ID3D12Device::CreateFence(synthesis gate)");
    require_hresult(
        fixture.queue()->Wait(gate.Get(), 1),
        "ID3D12CommandQueue::Wait(synthesis gate)");

    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "prime capture A failed");
    require(
        capture_a.serial == 1 && capture_a.slot == 0 && capture_a.source_index == 0,
        "prime capture A chronology is incorrect");

    xrfg::D3D12FrameSynthesisTicket invalid_prime{};
    require(
        FAILED(synthesizer.submit_prime(
            capture_a,
            std::span<const xrfg::D3D12ReprojectionView>(static_views.data(), 1),
            0,
            &invalid_prime)) &&
            invalid_prime.current_serial == 0 && invalid_prime.fence_value == 0,
        "prime accepted incomplete per-eye camera metadata");
    require(
        FAILED(synthesizer.submit_prime(
            capture_a,
            static_views,
            static_cast<std::uint32_t>(current_destinations.size()),
            &invalid_prime)) &&
            invalid_prime.current_serial == 0 && invalid_prime.fence_value == 0,
        "out-of-range prime submission was not rejected atomically");

    struct SynthesisOutcome {
        HRESULT result{E_FAIL};
        xrfg::D3D12FrameSynthesisTicket ticket{};
    };
    std::future<SynthesisOutcome> prime_future = std::async(
        std::launch::async,
        [&synthesizer, &capture_a, &static_views] {
            SynthesisOutcome outcome{};
            outcome.result =
                synthesizer.submit_prime(capture_a, static_views, 2, &outcome.ticket);
            return outcome;
        });
    if (prime_future.wait_for(std::chrono::milliseconds(500)) !=
        std::future_status::ready) {
        require_hresult(gate->Signal(1), "ID3D12Fence::Signal(prime timeout cleanup)");
        (void)prime_future.get();
        fail("prime submission blocked the CPU on GPU completion");
    }
    const SynthesisOutcome prime = prime_future.get();
    require(operation_succeeded(prime.result), "asynchronous prime submission failed");
    require(
        prime.ticket.previous_serial == 0 &&
            prime.ticket.current_serial == capture_a.serial &&
            prime.ticket.fence_value != 0 &&
            prime.ticket.synthetic_destination_index ==
                std::numeric_limits<std::uint32_t>::max() &&
            prime.ticket.current_destination_index == 2,
        "prime synthesis ticket is incorrect");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "pair capture B failed while prime work was queued");
    require(
        capture_b.serial == 2 && capture_b.slot == 1 && capture_b.source_index == 1 &&
            capture_b.fence_value > capture_a.fence_value,
        "capture B chronology is incorrect");

    xrfg::D3D12FrameSynthesisTicket invalid_pair{};
    const ReprojectionViews mismatched_target_views = make_reprojection_views(0.05F);
    require(
        FAILED(synthesizer.submit_pair(
            capture_b,
            static_views,
            mismatched_target_views,
            0,
            0,
            &invalid_pair)) &&
            invalid_pair.current_serial == 0 && invalid_pair.fence_value == 0,
        "synthesizer accepted a target camera different from render pose B");
    require(
        FAILED(synthesizer.submit_pair(
            capture_b,
            static_views,
            static_views,
            static_cast<std::uint32_t>(synthetic_destinations.size()),
            0,
            &invalid_pair)) &&
            invalid_pair.current_serial == 0 && invalid_pair.fence_value == 0,
        "out-of-range pair submission was not rejected atomically");

    // Release the deliberately blocked queue, then model V090's frame-start
    // gate before advancing the temporal pair. Submission itself remains
    // nonblocking and cannot accumulate another complete transaction.
    require_hresult(gate->Signal(1), "ID3D12Fence::Signal(synthesis gate)");
    require(
        operation_succeeded(synthesizer.wait_for_previous_submission(500)),
        "frame-start synthesis gate did not settle the asynchronous prime");

    std::future<SynthesisOutcome> pair_future = std::async(
        std::launch::async,
        [&synthesizer, &capture_b, &static_views] {
            SynthesisOutcome outcome{};
            outcome.result = synthesizer.submit_pair(
                capture_b,
                static_views,
                static_views,
                2,
                1,
                &outcome.ticket);
            return outcome;
        });
    if (pair_future.wait_for(std::chrono::milliseconds(500)) !=
        std::future_status::ready) {
        require_hresult(gate->Signal(1), "ID3D12Fence::Signal(pair timeout cleanup)");
        (void)pair_future.get();
        fail("pair submission blocked the CPU on GPU completion");
    }
    const SynthesisOutcome pair_ab = pair_future.get();
    require(operation_succeeded(pair_ab.result), "asynchronous A/B pair submission failed");
    require(
        pair_ab.ticket.previous_serial == capture_a.serial &&
            pair_ab.ticket.current_serial == capture_b.serial &&
            pair_ab.ticket.fence_value > prime.ticket.fence_value &&
            pair_ab.ticket.work_slot != prime.ticket.work_slot &&
            pair_ab.ticket.synthetic_destination_index == 2 &&
            pair_ab.ticket.current_destination_index == 1,
        "A/B synthesis ticket is incorrect");

    require(
        operation_succeeded(synthesizer.wait_for_previous_submission(500)),
        "frame-start synthesis gate did not settle the A/B pair");
    xrfg::D3D12FrameSynthesisTicket repeated_pair{};
    const ReprojectionViews repeated_views = make_reprojection_views(0.025F);
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b,
            repeated_views,
            repeated_views,
            0,
            0,
            &repeated_pair)) &&
            repeated_pair.previous_serial == capture_b.serial &&
            repeated_pair.current_serial == capture_b.serial &&
            repeated_pair.fence_value > pair_ab.ticket.fence_value &&
            repeated_pair.synthetic_destination_index == 0 &&
            repeated_pair.current_destination_index == 0,
        "rolling synthesizer did not reuse an unchanged eye capture");

    xrfg::D3D12HistoryCaptureTicket capture_c{};
    require(
        operation_succeeded(history->capture(2, &capture_c)) &&
            operation_succeeded(history->commit(capture_c)),
        "third rolling slot did not accept C while A/B work was in flight");
    require(
        capture_c.serial == 3 && capture_c.slot == 2 && capture_c.source_index == 2 &&
            capture_c.fence_value > capture_b.fence_value,
        "capture C did not preserve A0/B1/C2 rolling chronology");

    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});

    require_readback_matches(
        fixture,
        current_destinations[2].Get(),
        pattern_a,
        "prime current A",
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require_readback_matches(
        fixture,
        current_destinations[1].Get(),
        pattern_b,
        "pair current B",
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern synthetic_ab = readback_pattern(
        fixture,
        synthetic_destinations[2].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(
        synthetic_ab != pattern_a && synthetic_ab != pattern_b,
        "A/B synthetic output did not remain temporally distinct");
    require_source_is_render_target(fixture, sources[0].Get());
    require_source_is_render_target(fixture, sources[1].Get());
    require_readback_matches(
        fixture,
        current_destinations[0].Get(),
        pattern_b,
        "repeated current B",
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    xrfg::D3D12FrameSynthesisTicket pair_bc{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_c,
            static_views,
            static_views,
            0,
            0,
            &pair_bc)),
        "B/C rolling pair submission failed");
    require(
        pair_bc.previous_serial == capture_b.serial &&
            pair_bc.current_serial == capture_c.serial &&
            pair_bc.fence_value > pair_ab.ticket.fence_value &&
            pair_bc.synthetic_destination_index == 0 &&
            pair_bc.current_destination_index == 0,
        "B/C synthesis ticket is incorrect");

    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    require_readback_matches(
        fixture,
        current_destinations[0].Get(),
        pattern_c,
        "pair current C",
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern synthetic_bc = readback_pattern(
        fixture,
        synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(
        synthetic_bc != pattern_b && synthetic_bc != pattern_c,
        "B/C synthetic output did not remain temporally distinct");
    require_source_is_render_target(fixture, sources[2].Get());

    upload_pattern(fixture, sources[0].Get(), pattern_d);
    xrfg::D3D12HistoryCaptureTicket capture_d{};
    require(
        operation_succeeded(history->capture(0, &capture_d)) &&
            operation_succeeded(history->commit(capture_d)),
        "capture D did not reuse slot zero after synthesis completion");
    require(
        capture_d.serial == 4 && capture_d.slot == 0 && capture_d.source_index == 0 &&
            capture_d.fence_value > capture_c.fence_value,
        "capture D did not preserve A0/B1/C2/D0 rolling chronology");

    xrfg::D3D12FrameSynthesisTicket repeated_c{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_c,
            static_views,
            static_views,
            1,
            1,
            &repeated_c)) &&
            repeated_c.previous_serial == capture_c.serial &&
            repeated_c.current_serial == capture_c.serial &&
            repeated_c.fence_value > pair_bc.fence_value,
        "rolling synthesizer did not retain a repeated capture after an advancing pair");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    require_readback_matches(
        fixture,
        current_destinations[1].Get(),
        pattern_c,
        "repeated current C",
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    require(
        operation_succeeded(synthesizer.retire_previous()),
        "rolling previous retirement failed");
    require(
        operation_succeeded(synthesizer.retire_previous()),
        "rolling previous retirement was not idempotent");
    require(operation_succeeded(synthesizer.wait_for_idle()), "synthesizer drain failed");

    CaptureResources history_b = acquire_capture_for_inspection(*history, capture_b);
    fixture.wait_for_fence(history_b.fence.Get(), capture_b.fence_value);
    require_readback_matches(
        fixture,
        history_b.resource.Get(),
        pattern_b,
        "history B after rolling synthesis");
    retire_completed_inspection(fixture, *history, history_b);

    CaptureResources history_c = acquire_capture_for_inspection(*history, capture_c);
    fixture.wait_for_fence(history_c.fence.Get(), capture_c.fence_value);
    require_readback_matches(
        fixture,
        history_c.resource.Get(),
        pattern_c,
        "history C after rolling synthesis");
    retire_completed_inspection(fixture, *history, history_c);
    CaptureResources history_d = acquire_capture_for_inspection(*history, capture_d);
    fixture.wait_for_fence(history_d.fence.Get(), capture_d.fence_value);
    require_readback_matches(
        fixture,
        history_d.resource.Get(),
        pattern_d,
        "history D after rolling synthesis");
    retire_completed_inspection(fixture, *history, history_d);
    require(operation_succeeded(history->wait_for_idle()), "synthesis leases did not retire");

    xrfg::D3D12HistoryConsumerLease stale_lease{};
    ID3D12Resource* stale_resource = nullptr;
    ID3D12Fence* stale_fence = nullptr;
    require(
        FAILED(history->acquire_consumer(
            capture_a,
            &stale_lease,
            &stale_resource,
            &stale_fence)) &&
            stale_resource == nullptr && stale_fence == nullptr,
        "overwritten capture A remained consumable after serial 3");

    require(operation_succeeded(history->invalidate()), "synthesis history invalidate failed");
}

void test_stereo_motion_synthesis_beats_same_pixel_blend(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    bool validate_repeated_capture = false,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT kMotionWidth = 1024;
    constexpr UINT kMotionHeight = 512;
    constexpr UINT kEvaluationMargin = 32;

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
    };
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
    };
    std::array<ID3D12Resource*, 2> current_destination_pointers{
        current_destinations[0].Get(),
        current_destinations[1].Get(),
    };
    std::array<ID3D12Resource*, 1> synthetic_destination_pointers{
        synthetic_destinations[0].Get(),
    };

    const StereoPattern previous = translated_motion_pattern(
        kMotionWidth,
        kMotionHeight,
        {0, 0});
    const StereoPattern current = translated_motion_pattern(
        kMotionWidth,
        kMotionHeight,
        {16, -16});
    const StereoPattern expected_midpoint = translated_motion_pattern(
        kMotionWidth,
        kMotionHeight,
        {8, -8});
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "motion synthesis history initialization failed");

    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            std::span<ID3D12Resource* const>(
                current_destination_pointers.data(),
                current_destination_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_destination_pointers.data(),
                synthetic_destination_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            nvidia_options,
            backend == xrfg::D3D12OpticalFlowBackend::nvidia)),
        "motion frame synthesizer initialization failed");
    const ReprojectionViews static_views = make_reprojection_views();

    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "motion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(
            synthesizer.submit_prime(capture_a, static_views, 0, &prime)),
        "motion prime submission failed");
    require_frame_start_gate(synthesizer, "motion frame-start gate");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "motion capture B failed");
    require(
        capture_a.serial == 1 && capture_a.slot == 0 &&
            capture_b.serial == 2 && capture_b.slot == 1,
        "motion pair history chronology is incorrect");
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b,
            static_views,
            static_views,
            0,
            1,
            &pair)),
        "motion A/B pair submission failed");
    require(
        pair.previous_serial == capture_a.serial &&
            pair.current_serial == capture_b.serial,
        "motion pair synthesis ticket is incorrect");
    const StereoPattern actual_current = readback_pattern(
        fixture,
        current_destinations[1].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern actual_synthetic = readback_pattern(
        fixture,
        synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(actual_current == current, "motion current output is not a bit-exact copy of B");
    require(
        actual_synthetic != previous && actual_synthetic != current,
        "motion synthetic output duplicates one of its inputs");
    require(
        actual_synthetic != same_pixel_blend,
        "motion synthetic output ignored the optical-flow field");

    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double synthetic_error = mean_absolute_rgb_error(
            actual_synthetic,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        const double blend_error = mean_absolute_rgb_error(
            same_pixel_blend,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        const double previous_duplicate_error = mean_absolute_rgb_error(
            previous,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        const double current_duplicate_error = mean_absolute_rgb_error(
            current,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        std::cout << "motion eye=" << eye
                  << " synthetic_mae=" << synthetic_error
                  << " blend_mae=" << blend_error
                  << " previous_mae=" << previous_duplicate_error
                  << " current_mae=" << current_duplicate_error << '\n';
        require(
            synthetic_error <= blend_error * 1.10 &&
                synthetic_error < previous_duplicate_error &&
                synthetic_error < current_duplicate_error,
            "motion midpoint is less stable than blend/duplicates for eye " +
                std::to_string(eye) +
                ": synthetic=" + std::to_string(synthetic_error) +
                " blend=" + std::to_string(blend_error));
    }

    require_source_is_render_target(fixture, sources[0].Get());
    require_source_is_render_target(fixture, sources[1].Get());
    if (validate_repeated_capture) {
        const ReprojectionViews repeated_views = static_views;
        xrfg::D3D12FrameSynthesisTicket repeated{};
        require(
            operation_succeeded(synthesizer.submit_pair(
                capture_b,
                repeated_views,
                repeated_views,
                0,
                0,
                &repeated)) &&
                repeated.previous_serial == capture_b.serial &&
                repeated.current_serial == capture_b.serial &&
                repeated.fence_value > pair.fence_value,
            "motion synthesizer did not accept an unchanged capture");
        require(
            operation_succeeded(synthesizer.wait_for_idle()),
            "repeated motion synthesis drain failed");
        fixture.require_no_debug_errors();
        const StereoPattern repeated_current = readback_pattern(
            fixture,
            current_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(
            repeated_current == current,
            "repeated motion current output is not a bit-exact copy of B");
        const StereoPattern repeated_synthetic = readback_pattern(
            fixture,
            synthetic_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            const double repeated_error = mean_absolute_rgb_error(
                repeated_synthetic,
                current,
                kMotionWidth,
                kMotionHeight,
                eye,
                kEvaluationMargin);
            require(
                repeated_error < 0.5,
                "repeated capture pose synthesis is unstable for eye " +
                    std::to_string(eye) + ": error=" +
                    std::to_string(repeated_error));
        }
    } else {
        require(
            operation_succeeded(synthesizer.wait_for_idle()),
            "motion synthesis drain failed");
    }
    if (backend == xrfg::D3D12OpticalFlowBackend::nvidia) {
        xrfg::D3D12NvidiaGpuTiming timing{};
        const HRESULT consumed = synthesizer.consume_nvidia_gpu_timing(&timing);
        std::cout << "NVIDIA timing consumed=0x" << std::hex << unsigned(consumed) << std::dec
                  << " serials=" << timing.previous_serial << "->" << timing.current_serial
                  << " (expected " << capture_a.serial << "->" << capture_b.serial << ")"
                  << " eyes=" << timing.eye_count << " total=" << timing.total_microseconds
                  << " pack=" << timing.pack_microseconds
                  << " composition=" << timing.composition_microseconds << '\n';
        require(
            consumed == S_OK &&
                timing.previous_serial == capture_a.serial &&
                timing.current_serial == capture_b.serial &&
                timing.eye_count == kEyeCount &&
                timing.total_microseconds >=
                    timing.pack_microseconds +
                        timing.composition_microseconds,
            "NVIDIA GPU timing did not resolve the measured stereo pair");
        std::cout << "nvidia gpu pack_us=" << timing.pack_microseconds
                  << " eye0_us=" << timing.eye0_microseconds
                  << " eye1_us=" << timing.eye1_microseconds
                  << " composition_us=" << timing.composition_microseconds
                  << " total_us=" << timing.total_microseconds << '\n';
    }
    require(operation_succeeded(history->invalidate()), "motion history invalidate failed");
}

void test_dlss_motion_vector_stereo_stream_pairing(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(true);
    constexpr UINT width = 64;
    constexpr UINT height = 32;
    const auto create_texture = [&](UINT texture_width, UINT texture_height, UINT16 slices) {
        D3D12_RESOURCE_DESC description =
            stereo_texture_description(texture_width, texture_height);
        description.DepthOrArraySize = slices;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        require_hresult(fixture.device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
            "create DLSS provenance texture");
        return texture;
    };
    const auto left_output = create_texture(width, height, 1);
    const auto right_output = create_texture(width, height, 1);
    const auto motion = create_texture(width / 2, height / 2, 1);
    const auto xr_output = create_texture(width, height, 2);

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::publish_dlss_motion_vectors({
            101, left_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        xrfg::publish_dlss_motion_vectors({
            202, right_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });

    const auto resolved = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), fixture.queue());
    require(resolved && resolved->eye_count == 2 && resolved->eyes[0] &&
            resolved->eyes[1] && resolved->eyes[0]->stream == 101 &&
            resolved->eyes[1]->stream == 202 &&
            resolved->eyes[0]->motion_vectors.Get() != motion.Get() &&
            resolved->eyes[1]->motion_vectors.Get() != motion.Get() &&
            resolved->eyes[0]->motion_vectors.Get() !=
                resolved->eyes[1]->motion_vectors.Get() &&
            resolved->eyes[0]->output_slice == 0 &&
            resolved->eyes[1]->output_slice == 0,
        "two DLSS streams were not associated with the XR stereo image");

    const auto packed_xr_output = create_texture(width*2, height, 1);
    const auto packed = xrfg::resolve_dlss_motion_vectors(packed_xr_output.Get(),fixture.queue());
    require(packed && packed->eye_count==2 && packed->eyes[0]->stream==101 &&
        packed->eyes[1]->stream==202 && packed->eyes[0]->output_x==0 &&
        packed->eyes[1]->output_x==width && packed->eyes[1]->output_width==width &&
        packed->eyes[1]->motion_x==0 && packed->eyes[1]->motion_width==width/2 &&
        resolved->eyes[1]->output_x==0,
        "double-wide UEVR colour did not retain independent mono eye guide mappings");

    // Publish in the opposite order. Eye assignment must remain tied to the
    // stable first-seen stream order, not to this frame's evaluation order.
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::publish_dlss_motion_vectors({
            202, right_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        xrfg::publish_dlss_motion_vectors({
            101, left_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });
    const auto reordered = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), fixture.queue());
    require(reordered && reordered->eyes[0]->stream == 101 &&
            reordered->eyes[1]->stream == 202 &&
            reordered->eyes[0]->serial == 2 &&
            reordered->eyes[1]->serial == 2,
        "DLSS stream pairing changed eye assignment with evaluation order");

    const auto tracked = xrfg::dlss_motion_vector_statistics();
    require(tracked.published == 4 && tracked.snapshot_copies == 4 &&
            tracked.snapshot_failures == 0 && tracked.matched == 3 &&
            tracked.resolve_missing_streams == 0 &&
            tracked.resolve_stale_pairs == 0,
        "DLSS stream diagnostics did not account for the stereo pair");
    const auto unrelated_ui = create_texture(width/2,height/2,1);
    require(!xrfg::resolve_dlss_motion_vectors(unrelated_ui.Get(),fixture.queue()),
        "scene depth/motion were incorrectly associated with an unrelated UI swapchain");
    xrfg::report_dlss_motion_vector_use();
    const auto used = xrfg::dlss_motion_vector_statistics();
    require(used.status == xrfg::DlssMotionVectorStatus::used && used.used == 1 &&
            used.last_used_publication == used.published,
        "DLSS stream diagnostics did not retain the last successful use");
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::publish_dlss_motion_vectors({
            101, left_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });
    require(xrfg::dlss_motion_vector_statistics().status ==
                xrfg::DlssMotionVectorStatus::used,
        "a new DLSS publication falsely demoted a recently used vector route");
    xrfg::configure_dlss_motion_vector_tracking(false);
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), fixture.queue()),
        "disabled DLSS vector tracking still resolved a pair");
    xrfg::configure_dlss_motion_vector_tracking(true);
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), fixture.queue()),
        "re-enabled DLSS vector tracking exposed a stale pair");

    const auto publish_alternating_eye = [&](ID3D12Resource* eye_output) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
            xrfg::publish_dlss_motion_vectors({
                303, eye_output, motion.Get(), fixture.queue(), 0, 0, width, height,
                0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
                D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        });
    };
    publish_alternating_eye(left_output.Get());
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), fixture.queue()),
        "single DLSS stream resolved before the second eye arrived");
    publish_alternating_eye(right_output.Get());
    const auto alternating_ab = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), fixture.queue());
    require(alternating_ab && alternating_ab->eyes[0] && alternating_ab->eyes[1] &&
            alternating_ab->eyes[0]->stream == 303 &&
            alternating_ab->eyes[1]->stream == 303 &&
            alternating_ab->eyes[0]->serial == 1 &&
            alternating_ab->eyes[1]->serial == 2 &&
            alternating_ab->eyes[0]->previous_serial == 0 &&
            alternating_ab->eyes[1]->previous_serial == 0,
        "single alternating DLSS stream did not form its first stereo pair");
    publish_alternating_eye(left_output.Get());
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), fixture.queue()),
        "single alternating DLSS stream exposed a half-updated stereo pair");
    publish_alternating_eye(right_output.Get());
    const auto alternating_cd = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), fixture.queue());
    require(alternating_cd && alternating_cd->eyes[0] && alternating_cd->eyes[1] &&
            alternating_cd->eyes[0]->serial == 3 &&
            alternating_cd->eyes[1]->serial == 4 &&
            alternating_cd->eyes[0]->previous_serial == 1 &&
            alternating_cd->eyes[1]->previous_serial == 2,
        "single alternating DLSS stream lost per-eye temporal continuity");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_dlss_motion_vector_stream(101);
    xrfg::retire_dlss_motion_vector_stream(202);
    xrfg::retire_dlss_motion_vector_stream(303);

    // A game can publish a typed depth/stencil resource with shader reads
    // disabled. Its retained snapshot must be both bit-exact and SRV-readable.
    xrfg::configure_dlss_motion_vector_tracking(true);
    auto typed_depth=create_depth_source_texture(fixture,D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    auto depth_left=create_texture(kWidth,kHeight,1),depth_right=create_texture(kWidth,kHeight,1);
    auto depth_xr=create_texture(kWidth,kHeight,2);
    auto depth_motion=create_and_upload_game_motion(fixture,kWidth,kHeight,{0,0});
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> dsv;
    require_hresult(fixture.device()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)),"typed depth DSV heap");
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format=DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY; dd.Texture2DArray.ArraySize=2;
    fixture.device()->CreateDepthStencilView(typed_depth.Get(),&dd,dsv->GetCPUDescriptorHandleForHeapStart());
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        list->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,0.5F,0,0,nullptr);
        for(UINT eye=0;eye<2;++eye) {
            xrfg::DlssMotionVectorPublication pub{};
            pub.stream=901+eye; pub.output=eye?depth_right.Get():depth_left.Get();
            pub.motion_vectors=depth_motion.Get(); pub.producer_queue=fixture.queue(); pub.producer_command_list=list;
            pub.output_width=pub.motion_width=pub.depth_width=kWidth;
            pub.output_height=pub.motion_height=pub.depth_height=kHeight;
            pub.resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            pub.depth=typed_depth.Get(); pub.depth_resource_state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
            xrfg::publish_dlss_motion_vectors(pub);
        }
    });
    const auto depth_set=xrfg::resolve_dlss_motion_vectors(depth_xr.Get(),fixture.queue());
    require(depth_set && depth_set->eye_count==2,"typed depth guides did not resolve");
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    require_hresult(fixture.device()->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srv_heap)),"typed depth SRV heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.ArraySize=2; sd.Texture2DArray.MipLevels=1;
    sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    for(const auto& frame:depth_set->eyes) {
        require(frame && frame->depth && frame->depth.Get()!=typed_depth.Get() &&
            frame->depth->GetDesc().Format==DXGI_FORMAT_R32_TYPELESS &&
            !(frame->depth->GetDesc().Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),"typed depth snapshot cannot be sampled");
        fixture.device()->CreateShaderResourceView(frame->depth.Get(),&sd,srv_heap->GetCPUDescriptorHandleForHeapStart());
        const auto copied=readback_pattern(fixture,frame->depth.Get(),frame->depth_resource_state);
        for(const auto& eye:copied) for(SIZE_T byte=0;byte<eye.size();byte+=4) {
            float value{}; std::memcpy(&value,eye.data()+byte,4);
            require(value==0.5F,"typed depth snapshot changed the depth values");
        }
    }
    xrfg::retire_dlss_motion_vector_stream(901); xrfg::retire_dlss_motion_vector_stream(902);
    xrfg::configure_dlss_motion_vector_tracking(false);
}

// Guide snapshots keep only what an evaluation reads: the motion rectangle of a
// single-subresource target, and the depth plane of a depth-stencil target
// (D3D12 copies depth-stencil subresources only whole, so that is not cropped).
void test_dlss_guide_snapshots_copy_only_the_read_region(D3D12WarpFixture& fixture) {
    constexpr UINT width = 192, height = 96;
    constexpr UINT rx = 40, ry = 20, rw = 64, rh = 32;
    const auto make = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                          D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
        D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
        description.DepthOrArraySize = 1;
        description.Format = format;
        description.Flags = flags;
        const auto properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> resource;
        require_hresult(fixture.device()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                            &description, state, clear, IID_PPV_ARGS(&resource)),
                        "guide snapshot texture");
        return resource;
    };
    const auto footprint = [&](ID3D12Resource* texture, UINT64* total) {
        const auto description = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows{};
        UINT64 row_size{};
        fixture.device()->GetCopyableFootprints(&description, 0, 1, 0, &layout, &rows, &row_size, total);
        return layout;
    };
    const auto read = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
        const auto description = texture->GetDesc();
        UINT64 total{};
        const auto layout = footprint(texture, &total);
        auto buffer = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            auto barrier = transition_barrier(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = buffer.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layout;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        });
        std::vector<std::uint32_t> texels(static_cast<std::size_t>(description.Width) * description.Height);
        void* mapped{};
        require_hresult(buffer->Map(0, nullptr, &mapped), "guide snapshot readback");
        for (UINT y = 0; y < description.Height; ++y) {
            std::memcpy(texels.data() + static_cast<std::size_t>(y) * description.Width,
                        static_cast<const std::byte*>(mapped) + layout.Offset +
                            static_cast<std::size_t>(y) * layout.Footprint.RowPitch,
                        static_cast<std::size_t>(description.Width) * 4);
        }
        buffer->Unmap(0, nullptr);
        return texels;
    };
    const auto texel = [](UINT x, UINT y) { return x | (y << 8) | ((x ^ y) << 16) | 0xFF000000U; };

    auto output = make(kFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
    auto motion = make(kFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    UINT64 total{};
    const auto layout = footprint(motion.Get(), &total);
    auto upload = create_buffer(fixture, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{};
    require_hresult(upload->Map(0, nullptr, &mapped), "guide snapshot upload");
    for (UINT y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(mapped) + layout.Offset +
                                                     static_cast<std::size_t>(y) * layout.Footprint.RowPitch);
        for (UINT x = 0; x < width; ++x) row[x] = texel(x, y);
    }
    upload->Unmap(0, nullptr);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    clear.DepthStencil = {0.25F, 7};
    auto depth = make(DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_description.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsv;
    require_hresult(fixture.device()->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&dsv)),
                    "guide snapshot DSV heap");
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    fixture.device()->CreateDepthStencilView(depth.Get(), &view, dsv->GetCPUDescriptorHandleForHeapStart());

    xrfg::configure_dlss_motion_vector_tracking(true);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = motion.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layout;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        const auto ready = transition_barrier(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &ready);
        list->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),
                                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25F, 7, 0, nullptr);
        xrfg::DlssMotionVectorPublication publication{};
        publication.stream = 951;
        publication.output = output.Get();
        publication.motion_vectors = motion.Get();
        publication.depth = depth.Get();
        publication.producer_queue = fixture.queue();
        publication.producer_command_list = list;
        publication.output_width = width;
        publication.output_height = height;
        publication.motion_x = publication.depth_x = rx;
        publication.motion_y = publication.depth_y = ry;
        publication.motion_width = publication.depth_width = rw;
        publication.motion_height = publication.depth_height = rh;
        publication.resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        publication.depth_resource_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        xrfg::publish_dlss_motion_vectors(publication);
    });
    const auto set = xrfg::resolve_dlss_motion_vectors(output.Get(), fixture.queue());
    require(set && set->eye_count == 1 && set->eyes[0], "cropped guide snapshot did not resolve");
    const auto& frame = *set->eyes[0];
    const auto motion_description = frame.motion_vectors->GetDesc();
    require(motion_description.Width == rw && motion_description.Height == rh &&
                frame.motion_x == 0 && frame.motion_y == 0 && frame.motion_width == rw,
            "motion snapshot was not cropped to the read rectangle");
    const auto copied = read(frame.motion_vectors.Get(), frame.resource_state);
    for (UINT y = 0; y < rh; ++y) {
        for (UINT x = 0; x < rw; ++x) {
            require(copied[static_cast<std::size_t>(y) * rw + x] == texel(rx + x, ry + y),
                    "cropped motion snapshot moved or changed the vectors");
        }
    }
    const auto depth_description = frame.depth->GetDesc();
    require(depth_description.Format == DXGI_FORMAT_R32_TYPELESS && depth_description.Width == width &&
                !(depth_description.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
                frame.depth_x == rx && frame.depth_y == ry,
            "depth snapshot did not keep the whole depth plane alone");
    for (const auto value : read(frame.depth.Get(), frame.depth_resource_state)) {
        float depth_value{};
        std::memcpy(&depth_value, &value, sizeof(depth_value));
        require(depth_value == 0.25F, "depth plane snapshot changed the depth values");
    }
    xrfg::retire_dlss_motion_vector_stream(951);
    xrfg::configure_dlss_motion_vector_tracking(false);
    fixture.require_no_debug_errors();
}

[[nodiscard]] ComPtr<ID3D12Resource> create_native_test_depth(D3D12WarpFixture& fixture, UINT width, UINT height,
    float (*depth_at)(UINT eye, UINT x, UINT y) = nullptr) {
    ComPtr<ID3D12Resource> native_depth;
    auto description = stereo_texture_description(width,height);
    description.Format=DXGI_FORMAT_R32_FLOAT; description.Flags=D3D12_RESOURCE_FLAG_NONE;
    const auto hp=heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    require_hresult(fixture.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,
        &description,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&native_depth)),"native depth creation");
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,kEyeCount> fp{};
    std::array<UINT,kEyeCount> rows{}; std::array<UINT64,kEyeCount> sizes{}; UINT64 total{};
    fixture.device()->GetCopyableFootprints(&description,0,kEyeCount,0,fp.data(),rows.data(),sizes.data(),&total);
    auto upload=create_buffer(fixture,D3D12_HEAP_TYPE_UPLOAD,total,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped{}; require_hresult(upload->Map(0,nullptr,&mapped),"map native depth");
    for(UINT eye=0;eye<kEyeCount;++eye) for(UINT y=0;y<height;++y) {
        auto* row=reinterpret_cast<float*>(static_cast<std::byte*>(mapped)+fp[eye].Offset+SIZE_T(y)*fp[eye].Footprint.RowPitch);
        for(UINT x=0;x<width;++x) row[x]=depth_at?depth_at(eye,x,y):0.5F;
    }
    upload->Unmap(0,nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){
        for(UINT eye=0;eye<kEyeCount;++eye) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=native_depth.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=eye;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp[eye];
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        auto barrier=transition_barrier(native_depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1,&barrier);
    });
    return native_depth;
}

void test_dlss_motion_vector_gpu_ingress(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    bool native_dlss = false, bool triple = false, bool cropped = false, bool deferred = false) {
    constexpr UINT width = 256;
    constexpr UINT height = 128;
    constexpr UINT motion_width = width / 2;
    constexpr UINT motion_height = height / 2;
    constexpr UINT margin = 16;
    // 3X moves 12 pixels so its thirds, like the 2X midpoint, are whole pixels.
    const std::array<int, kEyeCount> translation =
        triple ? std::array<int, kEyeCount>{12, -12} : std::array<int, kEyeCount>{8, -8};

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, width, height),
    };
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_destination_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_destination_pointers{
        synthetic_destinations[0].Get()};

    const StereoPattern previous = translated_motion_pattern(width, height, {0, 0});
    const StereoPattern current = translated_motion_pattern(width, height, translation);
    const StereoPattern expected = translated_motion_pattern(width, height, {4, -4});
    const StereoPattern unscaled_expected = translated_motion_pattern(width, height, {2, -2});
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    if (deferred) upload_pattern(fixture, current_destinations[1].Get(), previous);
    ComPtr<ID3D12Resource> game_motion = create_and_upload_game_motion(
        fixture,
        motion_width,
        motion_height,
        {-static_cast<float>(translation[0]) * motion_width / width,
         -static_cast<float>(translation[1]) * motion_width / width});

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(), fixture.queue(), source_pointers,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "DLSS-motion history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    if (native_dlss) options.frame_generation = xrfg::D3D12FrameGeneration::native_dlss;
    auto extra_image = triple ? create_source_texture(fixture,width,height) : ComPtr<ID3D12Resource>{};
    std::vector<ID3D12Resource*> output_images{synthetic_destination_pointers[0]};
    if(triple) output_images.push_back(extra_image.Get());
    const auto extra_output = triple ? std::optional<xrfg::D3D12ExtraSynthetic>({1,2.0F/3.0F}) : std::nullopt;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(), fixture.queue(), history,
            current_destination_pointers, output_images,
            kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET, backend, options, native_dlss)),
        "DLSS-motion synthesizer initialization failed");

    auto frame_a = std::make_shared<xrfg::DlssMotionVectorFrame>();
    frame_a->stream = 17;
    frame_a->epoch = 3;
    frame_a->serial = 1;
    frame_a->motion_vectors = game_motion;
    frame_a->producer_queue = fixture.queue();
    frame_a->output_width = width;
    frame_a->output_height = height;
    frame_a->motion_width = motion_width;
    frame_a->motion_height = motion_height;
    frame_a->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (native_dlss) {
        frame_a->depth=create_native_test_depth(fixture,width,height);
        frame_a->depth_width=width; frame_a->depth_height=height;
        frame_a->depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
    }
    auto frame_b = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_a);
    frame_b->previous_serial = 1;
    frame_b->serial = 2;
    auto frame_a_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_a);
    frame_a_right->motion_slice = 1;
    frame_a_right->output_slice = 1;
    auto frame_b_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b);
    frame_b_right->motion_slice = 1;
    frame_b_right->output_slice = 1;
    auto set_a = std::make_shared<xrfg::DlssMotionVectorSet>();
    set_a->eye_count = kEyeCount;
    set_a->eyes = {frame_a, frame_a_right};
    auto set_b = std::make_shared<xrfg::DlssMotionVectorSet>();
    set_b->eye_count = kEyeCount;
    set_b->eyes = {frame_b, frame_b_right};

    ReprojectionViews views = make_reprojection_views();
    if(cropped) for(auto& view:views) view.image_rect={16,8,width-32,height-16};
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "DLSS-motion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(synthesizer.submit_prime(
            capture_a, views, 0, &prime, set_a)),
        "DLSS-motion prime failed");
    require_frame_start_gate(synthesizer, "DLSS-motion frame-start gate");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "DLSS-motion capture B failed");
    const auto statistics_before = xrfg::dlss_motion_vector_statistics();
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b, views, views, 0, 1, &pair, std::nullopt, set_b, deferred,
            triple ? 1.0F/3.0F : 0.5F, extra_output)),
        "DLSS-motion pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    if (deferred) {
        require(readback_pattern(fixture, current_destinations[1].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET) == previous,
            "native real copy executed before deferred submission");
        require_hresult(synthesizer.flush_current_copy(fixture.queue(), pair.fence_value),
            "native deferred real-copy flush");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    }
    const auto statistics_after = xrfg::dlss_motion_vector_statistics();
    require(
        statistics_after.used == statistics_before.used + 1 &&
            statistics_after.status == xrfg::DlssMotionVectorStatus::used,
        "DLSS-motion path was not selected by the GPU synthesizer");

    const StereoPattern actual = readback_pattern(
        fixture, synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (native_dlss) {
        xrfg::D3D12NvidiaGpuTiming timing{};
        require(synthesizer.consume_nvidia_gpu_timing(&timing) == S_OK &&
            timing.previous_serial == capture_a.serial && timing.current_serial == capture_b.serial &&
            timing.eye_count == kEyeCount && timing.total_microseconds > 0 &&
            timing.gpu_end_qpc > timing.gpu_begin_qpc,
            "native GPU timing did not resolve the completed stereo pair");
        std::cout << "native stereo gpu total_us=" << timing.total_microseconds << '\n';
        require(actual != current && actual != previous, "native DLSS did not produce an intermediate frame");
        require(readback_pattern(fixture,current_destinations[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==current,
            "native DLSS changed the real output");
        if(triple) {
            const auto second=readback_pattern(fixture,extra_image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
            require(second!=actual && second!=current && second!=previous,"native 3X did not produce two distinct intermediate frames");
            const auto second_expected = translated_motion_pattern(width, height, {8, -8});
            for(UINT eye=0;eye<kEyeCount;++eye) {
                const double second_error = mean_absolute_rgb_error(second, second_expected, width, height, eye, margin);
                std::cout << "native 3X second eye=" << eye << " synthetic_mae=" << second_error << '\n';
                require(second_error < 0.02, "native 3X second frame was not at two thirds for eye " + std::to_string(eye));
            }
            for(UINT eye=0;eye<kEyeCount;++eye) {
                require(mean_absolute_rgb_error(actual,previous,width,height,eye,margin) < mean_absolute_rgb_error(second,previous,width,height,eye,margin),
                    "native 3X output order incorrect");
            }
        }
        if(cropped) for(UINT eye=0;eye<kEyeCount;++eye) for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
            if(x>=16 && x<width-16 && y>=8 && y<height-8) continue;
            const SIZE_T offset=(SIZE_T(y)*width+x)*4;
            require(std::equal(actual[eye].begin()+offset,actual[eye].begin()+offset+4,current[eye].begin()+offset),
                "native DLSS wrote outside the projection viewport");
        }
    }
    // The normal backend invocation above proves shader sampling. The NVIDIA
    // fixture reuses this case specifically to exercise its extra work-list
    // lifetime while game motion bypasses OFA; rounding can make its confidence
    // result byte-identical to the smooth same-pixel reference.
    if (backend != xrfg::D3D12OpticalFlowBackend::nvidia) {
        require(actual != same_pixel_blend,
            "DLSS-motion texture was not consumed by the synthesis shader");
    }
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double actual_error = mean_absolute_rgb_error(
            actual, expected, width, height, eye, margin);
        const double blend_error = mean_absolute_rgb_error(
            same_pixel_blend, expected, width, height, eye, margin);
        const double unscaled_error = mean_absolute_rgb_error(
            actual, unscaled_expected, width, height, eye, margin);
        const double previous_error = mean_absolute_rgb_error(
            previous, expected, width, height, eye, margin);
        const double current_error = mean_absolute_rgb_error(
            current, expected, width, height, eye, margin);
        std::cout << "DLSS motion eye=" << eye
                  << " synthetic_mae=" << actual_error
                  << " unscaled_mae=" << unscaled_error
                  << " blend_mae=" << blend_error << '\n';
        require(
            actual_error < (native_dlss ? (cropped ? 0.15 : 0.02) : 0.05) && actual_error < unscaled_error &&
                actual_error < previous_error && actual_error < current_error,
            "DLSS-motion low-resolution vectors were not exactly converted to output space for eye " +
                std::to_string(eye));
        if (native_dlss) require(actual_error < blend_error,
            "native DLSS did not improve on same-pixel blending for eye " + std::to_string(eye));
    }

    if (native_dlss) {
        require_hresult(synthesizer.submit_pair(capture_b, views, views, 0, 1, &pair,
            std::nullopt, set_b, false, triple ? 1.0F/3.0F : 0.5F, extra_output),
            "native repeated-frame pair");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
        require(readback_pattern(fixture, synthetic_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET) == current,
            "native repeated frame depended on unused optical-flow resources");
    }
    // Exercise enough advancing pairs to reuse a work slot.
    std::uint64_t guide_serial = frame_b->serial;
    for (UINT iteration = 0; iteration < 3; ++iteration) {
        const std::uint32_t source_index = iteration % 2U;
        xrfg::D3D12HistoryCaptureTicket capture{};
        require(
            operation_succeeded(history->capture(source_index, &capture)) &&
                operation_succeeded(history->commit(capture)),
            "DLSS-motion slot-reuse capture failed");
        auto guide_left = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b);
        guide_left->previous_serial = guide_serial;
        guide_left->serial = ++guide_serial;
        auto guide_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*guide_left);
        guide_right->motion_slice = 1;
        guide_right->output_slice = 1;
        auto guide = std::make_shared<xrfg::DlssMotionVectorSet>();
        guide->eye_count = kEyeCount;
        guide->eyes = {guide_left, guide_right};
        xrfg::D3D12FrameSynthesisTicket advancing{};
        require(
            operation_succeeded(synthesizer.submit_pair(
                capture, views, views, 0, source_index, &advancing,
                std::nullopt, guide, false, triple ? 1.0F/3.0F : 0.5F, extra_output)),
            "DLSS-motion work-slot reuse failed");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    }
    if (native_dlss) {
        require_frame_start_gate(synthesizer,"native DLSS second pair gate");
        upload_pattern(fixture,sources[0].Get(),current);
        xrfg::D3D12HistoryCaptureTicket capture_c{};
        require_hresult(history->capture(0,&capture_c),"native reset capture");
        require_hresult(history->commit(capture_c),"native reset commit");
        auto cut_left=std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b);
        auto cut_right=std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b_right);
        cut_left->serial=cut_right->serial=guide_serial+1;
        cut_left->previous_serial=cut_right->previous_serial=guide_serial;
        cut_left->reset=cut_right->reset=true;
        auto cut=std::make_shared<xrfg::DlssMotionVectorSet>(); cut->eye_count=kEyeCount; cut->eyes={cut_left,cut_right};
        require_hresult(synthesizer.submit_pair(capture_c,views,views,0,0,&pair,std::nullopt,cut,false,0.5F,extra_output),"native reset pair");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*){});
        require(readback_pattern(fixture,synthetic_destinations[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==current,
            "native explicit reset did not show the current frame");
        require_frame_start_gate(synthesizer,"native missing-depth gate");
        xrfg::D3D12HistoryCaptureTicket capture_d{};
        require_hresult(history->capture(1,&capture_d),"native missing-depth capture");
        require_hresult(history->commit(capture_d),"native missing-depth commit");
        auto missing_left=std::make_shared<xrfg::DlssMotionVectorFrame>(*cut_left);
        auto missing_right=std::make_shared<xrfg::DlssMotionVectorFrame>(*cut_right);
        missing_left->reset=missing_right->reset=false;
        missing_left->previous_serial=missing_right->previous_serial=guide_serial+1;
        missing_left->serial=missing_right->serial=guide_serial+2; missing_left->depth.Reset(); missing_right->depth.Reset();
        auto missing=std::make_shared<xrfg::DlssMotionVectorSet>(); missing->eye_count=kEyeCount; missing->eyes={missing_left,missing_right};
        require_hresult(synthesizer.submit_pair(capture_d,views,views,0,1,&pair,std::nullopt,missing,false,0.5F,extra_output),"native missing-depth pair");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*){});
        require(readback_pattern(fixture,synthetic_destinations[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==current,
            "native missing depth fabricated an interpolated frame");
        require(xrfg::dlss_motion_vector_statistics().status==xrfg::DlssMotionVectorStatus::waiting_for_depth,
            "native missing depth did not report the waiting state");
    }
    require(operation_succeeded(synthesizer.wait_for_idle()), "DLSS-motion final drain failed");
    require(operation_succeeded(history->invalidate()), "DLSS-motion history invalidate failed");
}

#ifdef XRFG_NATIVE_DLSSG
// An sRGB format exercises the encode before NGX and the decode after it:
// the generated bytes must still match the byte-space expectation.
// Copies each eye slice of an array texture side by side into a single-slice
// texture twice as wide, or back again.
void copy_stereo_layout(D3D12WarpFixture& fixture, ID3D12Resource* array,
                        D3D12_RESOURCE_STATES array_state, ID3D12Resource* packed,
                        D3D12_RESOURCE_STATES packed_state, UINT width, UINT height,
                        bool pack) {
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        std::array barriers{
            transition_barrier(array, array_state, pack ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                                        : D3D12_RESOURCE_STATE_COPY_DEST),
            transition_barrier(packed, packed_state, pack ? D3D12_RESOURCE_STATE_COPY_DEST
                                                          : D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION slice{}, wide{};
            slice.pResource = array;
            slice.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            slice.SubresourceIndex = eye;
            wide.pResource = packed;
            wide.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            if (pack) {
                list->CopyTextureRegion(&wide, eye * width, 0, 0, &slice, nullptr);
            } else {
                const D3D12_BOX box{eye * width, 0, 0, (eye + 1) * width, height, 1};
                list->CopyTextureRegion(&slice, 0, 0, 0, &wide, &box);
            }
        }
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    });
}

// UEVR submits both eyes side by side in one texture while the game's DLSS
// guides arrive per eye. OFXR's own synthesis must pair each view with its
// own guide, as native generation does, instead of rejecting the pair.
void test_dlss_motion_vector_side_by_side(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx) {
    constexpr UINT width = 256, height = 128, margin = 16;
    const auto previous = translated_motion_pattern(width, height, {0, 0});
    const auto current = translated_motion_pattern(width, height, {8, -8});
    const auto expected = translated_motion_pattern(width, height, {4, -4});
    const auto blend = midpoint_pattern(previous, current);
    std::array<ComPtr<ID3D12Resource>, 2> stereo{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    upload_pattern(fixture, stereo[0].Get(), previous);
    upload_pattern(fixture, stereo[1].Get(), current);
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1),
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1)};
    for (UINT frame = 0; frame < 2; ++frame) {
        copy_stereo_layout(fixture, stereo[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           sources[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, width,
                           height, true);
    }
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1),
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1)};
    auto synthetic =
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{current_destinations[0].Get(),
                                                    current_destinations[1].Get()};
    std::vector<ID3D12Resource*> outputs{synthetic.Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                                                    source_pointers,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET)),
            "side-by-side DLSS-motion history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(operation_succeeded(synthesizer.initialize(
                fixture.device(), fixture.queue(), history, current_pointers, outputs, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET, backend, {})),
            "side-by-side DLSS-motion synthesizer initialization failed");
    // One motion texture with a slice per eye, at half resolution.
    auto motion = create_and_upload_game_motion(fixture, width / 2, height / 2, {-4, 4});
    std::array<std::shared_ptr<xrfg::DlssMotionVectorSet>, 2> sets{
        std::make_shared<xrfg::DlssMotionVectorSet>(),
        std::make_shared<xrfg::DlssMotionVectorSet>()};
    ReprojectionViews views = make_reprojection_views();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        for (UINT frame = 0; frame < 2; ++frame) {
            auto guide = std::make_shared<xrfg::DlssMotionVectorFrame>();
            guide->stream = 41 + eye;
            guide->epoch = 1;
            guide->serial = frame + 1;
            guide->previous_serial = frame;
            guide->motion_vectors = motion;
            guide->producer_queue = fixture.queue();
            guide->output_x = eye * width;
            guide->output_width = width;
            guide->output_height = height;
            guide->motion_width = width / 2;
            guide->motion_height = height / 2;
            guide->motion_slice = eye;
            guide->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            sets[frame]->eye_count = kEyeCount;
            sets[frame]->eyes[eye] = guide;
        }
        views[eye].array_slice = 0;
        views[eye].image_rect = {eye * width, 0, width, height};
    }
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
                operation_succeeded(history->commit(capture_a)),
            "side-by-side DLSS-motion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(operation_succeeded(synthesizer.submit_prime(capture_a, views, 0, &prime, sets[0])),
            "side-by-side DLSS-motion prime failed");
    require_frame_start_gate(synthesizer, "side-by-side DLSS-motion frame-start gate");
    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(operation_succeeded(history->capture(1, &capture_b)) &&
                operation_succeeded(history->commit(capture_b)),
            "side-by-side DLSS-motion capture B failed");
    const auto before = xrfg::dlss_motion_vector_statistics();
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(operation_succeeded(
                synthesizer.submit_pair(capture_b, views, views, 0, 1, &pair, std::nullopt, sets[1])),
            "side-by-side DLSS-motion pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    const auto after = xrfg::dlss_motion_vector_statistics();
    require(after.used == before.used + 1 && after.status == xrfg::DlssMotionVectorStatus::used,
            "side-by-side DLSS guides were not used by OFXR synthesis");
    auto unpacked = create_source_texture(fixture, width, height);
    copy_stereo_layout(fixture, unpacked.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       synthetic.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, width, height, false);
    const auto actual =
        readback_pattern(fixture, unpacked.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double error = mean_absolute_rgb_error(actual, expected, width, height, eye, margin);
        const double blend_error = mean_absolute_rgb_error(blend, expected, width, height, eye, margin);
        std::cout << "side-by-side DLSS motion eye=" << eye << " synthetic_mae=" << error
                  << " blend_mae=" << blend_error << '\n';
        require(error < 0.05 && error < blend_error,
                "side-by-side DLSS motion did not use eye " + std::to_string(eye) + "'s guide");
    }
    require(synthesizer.wait_for_idle() == S_OK, "side-by-side DLSS-motion drain failed");
    fixture.require_no_debug_errors();
}

void test_native_dlss_packed_stereo(D3D12WarpFixture& fixture, bool cropped,
                                    DXGI_FORMAT format = kFormat) {
    constexpr UINT width = 256, height = 128, margin = 16;
    const auto previous = translated_motion_pattern(width, height, {0, 0});
    const auto current = translated_motion_pattern(width, height, {8, -8});
    const auto expected = translated_motion_pattern(width, height, {4, -4});
    const auto blend = midpoint_pattern(previous, current);
    std::array<ComPtr<ID3D12Resource>, 2> stereo{
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                              kEyeCount, format),
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                              kEyeCount, format)};
    upload_pattern(fixture, stereo[0].Get(), previous);
    upload_pattern(fixture, stereo[1].Get(), current);
    std::array<ComPtr<ID3D12Resource>, 2> packed{
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_COMMON, 1, format),
        create_source_texture(fixture, width * 2, height, D3D12_RESOURCE_STATE_COMMON, 1, format)};
    auto output = create_source_texture(
        fixture, width * 2, height, D3D12_RESOURCE_STATE_RENDER_TARGET, 1, format);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        for (UINT frame = 0; frame < 2; ++frame) {
            std::array barriers{
                transition_barrier(stereo[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(packed[frame].Get(), D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            for (UINT eye = 0; eye < kEyeCount; ++eye) {
                D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                src.pResource = stereo[frame].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = eye;
                dst.pResource = packed[frame].Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, eye * width, 0, 0, &src, nullptr);
            }
            for (auto& barrier : barriers)
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        }
        // Native composition preserves pixels outside each projection rectangle.
        auto source = transition_barrier(packed[1].Get(), D3D12_RESOURCE_STATE_COMMON,
                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto destination = transition_barrier(output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &source);
        list->ResourceBarrier(1, &destination);
        list->CopyResource(output.Get(), packed[1].Get());
        std::swap(source.Transition.StateBefore, source.Transition.StateAfter);
        std::swap(destination.Transition.StateBefore, destination.Transition.StateAfter);
        list->ResourceBarrier(1, &source);
        list->ResourceBarrier(1, &destination);
    });
    auto motion = create_and_upload_game_motion(fixture, width / 2, height / 2, {-4, 4});
    auto depth = create_native_test_depth(fixture, width, height);
    xrfg::DlssMotionVectorSet a_guides{}, b_guides{};
    a_guides.eye_count = b_guides.eye_count = kEyeCount;
    auto views = make_reprojection_views();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto a = std::make_shared<xrfg::DlssMotionVectorFrame>();
        a->stream = 17 + eye;
        a->epoch = 3;
        a->serial = 1;
        a->motion_vectors = motion;
        a->depth = depth;
        a->producer_queue = fixture.queue();
        a->output_x = eye * width;
        a->output_width = a->depth_width = width;
        a->output_height = a->depth_height = height;
        a->motion_width = width / 2;
        a->motion_height = height / 2;
        a->motion_slice = a->output_slice = eye;
        a->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
        auto b = std::make_shared<xrfg::DlssMotionVectorFrame>(*a);
        b->serial = 2;
        b->previous_serial = 1;
        a_guides.eyes[eye] = a;
        b_guides.eyes[eye] = b;
        views[eye].array_slice = 0;
        views[eye].image_rect = {
            eye * width + (cropped ? 3U : 0U), cropped ? 1U : 0U,
            cropped ? width - 6 : width, cropped ? height - 2 : height};
    }
    xrfg::D3D12NativeDlssG native;
    require_hresult(native.initialize(fixture.device(), fixture.queue(), packed[0]->GetDesc(),
                                      format), "packed native initialization");
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        require(native.record(list, 0, packed[0].Get(), packed[1].Get(), views, views,
                              &a_guides, &b_guides, outputs,
                              D3D12_RESOURCE_STATE_RENDER_TARGET) == S_OK,
                "packed native generation did not consume both eye guides");
    });
    auto unpacked = create_source_texture(fixture, width, height,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, kEyeCount, format);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        std::array barriers{
            transition_barrier(output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(unpacked.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = output.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = unpacked.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = eye;
            const D3D12_BOX box{eye * width, 0, 0, (eye + 1) * width, height, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    });
    const auto actual = readback_pattern(fixture, unpacked.Get(),
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double error = mean_absolute_rgb_error(actual, expected, width, height, eye, margin);
        const double blend_error = mean_absolute_rgb_error(blend, expected, width, height, eye, margin);
        std::cout << "Native packed stereo eye=" << eye << " cropped=" << cropped
                  << " srgb=" << (format != kFormat) << " synthetic_mae=" << error
                  << " blend_mae=" << blend_error << '\n';
        require(error < 0.02 && error < blend_error,
                "native packed stereo did not interpolate eye " + std::to_string(eye));
        if (cropped) for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            if (x >= 3 && x < width - 3 && y >= 1 && y < height - 1) continue;
            const SIZE_T offset = (SIZE_T(y) * width + x) * 4;
            require(std::equal(actual[eye].begin() + offset, actual[eye].begin() + offset + 4,
                               current[eye].begin() + offset),
                    "native packed stereo wrote outside the eye viewport");
        }
    }
    fixture.require_no_debug_errors();
}

// The native history is reseeded only when aligning A into B's camera moves a
// pixel: not for a still head, a translation or a sub-pixel tracking change.
void test_native_dlss_reseeds_only_for_camera_motion(D3D12WarpFixture& fixture) {
    constexpr UINT width = 256, height = 128;
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_COMMON),
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_COMMON)};
    auto output = create_source_texture(fixture, width, height);
    auto motion = create_and_upload_game_motion(fixture, width / 2, height / 2, {0, 0});
    auto depth = create_native_test_depth(fixture, width, height);
    const std::uint32_t frames = xrfg::native_dlssg_max_generated_frames(fixture.device());
    std::cout << "native max generated frames=" << frames << '\n';
    require(frames >= 1, "native frame generation reported unavailable");
    xrfg::D3D12NativeDlssG native;
    require_hresult(native.initialize(fixture.device(), fixture.queue(), sources[0]->GetDesc(),
                                      kFormat), "reseed native initialization");
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    std::uint64_t serial = 1;
    const auto pair = [&](const ReprojectionViews& a_views, const ReprojectionViews& b_views) {
        xrfg::DlssMotionVectorSet a_guides{}, b_guides{};
        a_guides.eye_count = b_guides.eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            for (const UINT next : {0U, 1U}) {
                auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
                f->stream = 61 + eye;
                f->epoch = 1;
                f->serial = serial + next;
                f->previous_serial = serial + next - 1;
                f->motion_vectors = motion;
                f->depth = depth;
                f->producer_queue = fixture.queue();
                f->output_width = f->depth_width = width;
                f->output_height = f->depth_height = height;
                f->motion_width = width / 2;
                f->motion_height = height / 2;
                f->motion_slice = f->output_slice = eye;
                f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
                (next ? b_guides : a_guides).eyes[eye] = f;
            }
        }
        ++serial;
        HRESULT recorded = E_FAIL;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            recorded = native.record(list, 0, sources[0].Get(), sources[1].Get(), a_views,
                                     b_views, &a_guides, &b_guides, outputs,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        require(recorded == S_OK, "reseed native pair was not generated");
        return native.reseeds();
    };
    const auto still = make_reprojection_views();
    require(pair(still, still) == kEyeCount, "the first native pair did not seed each eye");
    require(pair(still, still) == kEyeCount, "a still camera reseeded the native history");
    auto moved = still;
    for (auto& view : moved) view.pose.position.z += 0.05F;
    require(pair(still, moved) == kEyeCount, "a translation alone reseeded the native history");
    require(pair(still, make_reprojection_views(0.000001F)) == kEyeCount,
            "a sub-pixel rotation reseeded the native history");
    require(pair(still, make_reprojection_views(0.01F)) == 2 * kEyeCount,
            "a turning camera did not reseed the native history");
    fixture.require_no_debug_errors();
}

// A static scene with fine detail and hard edges, seen through each view.
[[nodiscard]] StereoPattern detailed_world_pattern(UINT width, UINT height,
                                                   const ReprojectionViews& views) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
        const auto& view = views[eye];
        const float left = std::tan(view.fov.angle_left), right = std::tan(view.fov.angle_right);
        const float top = std::tan(view.fov.angle_up), bottom = std::tan(view.fov.angle_down);
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const float u = (x + 0.5F) / width, v = (y + 0.5F) / height;
                const xrfg::Vec3 ray = rotate_vector(
                    view.pose.orientation,
                    {left + (right - left) * u, top + (bottom - top) * v, -1.0F});
                const float az = std::atan2(ray.x, -ray.z);
                const float el = std::atan2(ray.y, std::sqrt(ray.x * ray.x + ray.z * ray.z));
                const auto edge = [](float s) { return s >= 0 ? 1.0F : -1.0F; };
                const float red = 127.5F + 60 * std::sin(az * 41 + el * 7) +
                                  40 * edge(std::sin(az * 23) * std::sin(el * 19));
                const float green = 127.5F + 55 * std::cos(az * 59 - el * 13) +
                                    40 * edge(std::sin(az * 17 + 1) * std::sin(el * 29));
                const float blue = 127.5F + 50 * std::sin(az * 73 + el * 31) +
                                   35 * std::cos(el * 47);
                const std::size_t offset = (std::size_t(y) * width + x) * kBytesPerPixel;
                bytes[offset + 0] = direction_channel(red);
                bytes[offset + 1] = direction_channel(green);
                bytes[offset + 2] = direction_channel(blue);
                bytes[offset + 3] = 255;
            }
        }
    }
    return pattern;
}

void set_texture_state(D3D12WarpFixture& fixture, ID3D12Resource* texture,
                       D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        const auto barrier = transition_barrier(texture, from, to);
        list->ResourceBarrier(1, &barrier);
    });
}

// A foreground square moves three times faster than the background, with
// half-resolution vectors and edges between their texels, so pixels at its
// edges need the nearer surface's motion. NGX dilates the vectors itself.
void test_native_dlss_depth_edges(D3D12WarpFixture& fixture) {
    constexpr UINT width = 256, height = 128;
    constexpr int box_x = 97, box_y = 33, box_size = 63, fg_motion = 12, bg_motion = 4;
    const auto inside = [](int x, int y, int shift) {
        return x >= box_x + shift && x < box_x + box_size + shift && y >= box_y &&
               y < box_y + box_size;
    };
    const auto scene = [&](int bg_shift, int fg_shift) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.assign(std::size_t(width) * height * kBytesPerPixel, 0);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    RgbaBytes texel{0, 0, 0, 255};
                    if (inside(int(x), int(y), fg_shift)) {
                        const bool stripe = ((int(x) - fg_shift) / 4 + int(y) / 4) % 2 != 0;
                        texel = stripe ? RgbaBytes{240, 210, 40, 255} : RgbaBytes{30, 60, 200, 255};
                    } else if (int(x) >= bg_shift) {
                        texel = motion_texel(UINT(int(x) - bg_shift), y, eye);
                    }
                    std::copy(texel.begin(), texel.end(),
                              bytes.begin() + (std::size_t(y) * width + x) * kBytesPerPixel);
                }
            }
        }
        return pattern;
    };
    const auto a_pattern = scene(0, 0), b_pattern = scene(bg_motion, fg_motion);
    const auto expected = scene(bg_motion / 2, fg_motion / 2);
    std::array<ComPtr<ID3D12Resource>, 2> sources{create_source_texture(fixture, width, height),
                                                  create_source_texture(fixture, width, height)};
    upload_pattern(fixture, sources[0].Get(), a_pattern);
    upload_pattern(fixture, sources[1].Get(), b_pattern);
    // Native generation takes its sources at rest in COMMON.
    for (auto& source : sources) {
        set_texture_state(fixture, source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COMMON);
    }
    auto output = create_source_texture(fixture, width, height);
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    // Nearer surfaces have smaller depth here (not reversed).
    auto depth_a = create_native_test_depth(fixture, width, height, [](UINT, UINT x, UINT y) {
        return x >= box_x && x < box_x + box_size && y >= box_y && y < box_y + box_size ? 0.2F
                                                                                       : 0.8F;
    });
    auto depth_b = create_native_test_depth(fixture, width, height, [](UINT, UINT x, UINT y) {
        return x >= box_x + fg_motion && x < box_x + box_size + fg_motion && y >= box_y &&
                       y < box_y + box_size
                   ? 0.2F
                   : 0.8F;
    });
    auto still = create_and_upload_game_motion(fixture, width / 2, height / 2, {0, 0});
    auto motion = create_and_upload_game_motion_field(
        fixture, width / 2, height / 2, [&](UINT, UINT mx, UINT my) {
            const bool fg = inside(int(mx * 2 + 1), int(my * 2 + 1), fg_motion);
            return std::array<float, 2>{-float(fg ? fg_motion : bg_motion) / 2, 0.0F};
        });
    const auto guides = [&](std::uint64_t serial, const ComPtr<ID3D12Resource>& vectors,
                            const ComPtr<ID3D12Resource>& depth) {
        xrfg::DlssMotionVectorSet set{};
        set.eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
            f->stream = 91 + eye;
            f->epoch = 1;
            f->serial = serial;
            f->previous_serial = serial - 1;
            f->motion_vectors = vectors;
            f->depth = depth;
            f->producer_queue = fixture.queue();
            f->output_width = f->depth_width = width;
            f->output_height = f->depth_height = height;
            f->motion_width = width / 2;
            f->motion_height = height / 2;
            f->motion_slice = f->output_slice = eye;
            f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
            set.eyes[eye] = f;
        }
        return set;
    };
    const auto views = make_reprojection_views();
    const auto band_error = [&](const StereoPattern& actual, UINT eye) {
        // Pixels within six of the square's interpolated left or right edge.
        double total = 0;
        std::size_t count = 0;
        for (UINT y = box_y; y < box_y + box_size; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const int left = box_x + fg_motion / 2, right = left + box_size;
                if (std::abs(int(x) - left) >= 6 && std::abs(int(x) - right) >= 6) continue;
                const std::size_t offset = (std::size_t(y) * width + x) * kBytesPerPixel;
                for (UINT c = 0; c < 3; ++c, ++count)
                    total += std::abs(int(actual[eye][offset + c]) - int(expected[eye][offset + c]));
            }
        }
        return total / double(count);
    };
    {
        xrfg::D3D12NativeDlssG native;
        require_hresult(native.initialize(fixture.device(), fixture.queue(),
                                          sources[0]->GetDesc(), kFormat),
                        "native depth-edge initialization");
        const auto seed_a = guides(1, still, depth_a), seed_b = guides(2, still, depth_a);
        const auto moved = guides(3, motion, depth_b);
        HRESULT seeded = E_FAIL, generated = E_FAIL;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            seeded = native.record(list, 0, sources[0].Get(), sources[0].Get(), views, views,
                                   &seed_a, &seed_b, outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            generated = native.record(list, 0, sources[0].Get(), sources[1].Get(), views, views,
                                      &seed_b, &moved, outputs,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        require(seeded == S_OK && generated == S_OK, "native depth-edge pair failed");
        const auto actual = readback_pattern(fixture, output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto blend = midpoint_pattern(a_pattern, b_pattern);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            const double error = mean_absolute_rgb_error(actual, expected, width, height, eye, 16);
            const double edge = band_error(actual, eye), blend_edge = band_error(blend, eye);
            std::cout << "native depth edges eye=" << eye << " mae=" << error
                      << " edge_mae=" << edge << " blend_edge_mae=" << blend_edge << '\n';
            require(error < 0.5 && edge < 3 && edge < blend_edge * 0.05,
                    "native generation misplaced a moving foreground edge for eye " +
                        std::to_string(eye));
        }
    }
    fixture.require_no_debug_errors();
}

// The square's slide while a depth texture is built: create_native_test_depth
// takes a plain function, which cannot capture it.
float g_scale_quality_shift = 0;

// Opt-in with XRFG_TEST_NATIVE_DLSSG_SCALE_QUALITY: what a reduced-resolution
// feature costs in quality. A detailed background slides behind a striped
// foreground square, with DLSS Quality's two-thirds guides, at a headset-like
// size. Four pairs of slides, every one a fraction of a pixel on each feature
// scale's grid, are averaged: whole-pixel motion there, or one scene's
// periodic detail beating against one scale, would flatter that scale. The
// scene is rendered analytically with 4x4 supersampling, so the true midpoint
// frame is exact. The error is against it: overall, in the middle half of
// each axis and outside it, and in the band around the square's edges.
void bench_native_dlss_scale_quality(D3D12WarpFixture& fixture) {
    constexpr UINT width = 1024, height = 768, margin = 24;
    constexpr UINT gw = width * 2 / 3, gh = height * 2 / 3;
    constexpr float box_x = 301, box_y = 211, box_size = 301;
    const std::array<std::array<float, 2>, 4> slides{{
        {6.6F, 15.4F}, {3.3F, 9.8F}, {9.4F, 21.2F}, {5.1F, 12.6F}}};
    const auto inside = [](float x, float y, float shift) {
        return x >= box_x + shift && x < box_x + box_size + shift && y >= box_y &&
               y < box_y + box_size;
    };
    const auto background = [](float x, float y, UINT eye) {
        const auto edge = [](float s) { return s >= 0 ? 1.0F : -1.0F; };
        const float phase = float(eye) * 0.7F;
        return std::array<float, 3>{
            127.5F + 50 * std::sin(x * 0.031F + y * 0.017F + phase) +
                35 * edge(std::sin(x * 0.11F) * std::sin(y * 0.093F)) +
                20 * std::sin(x * 0.83F + y * 0.37F),
            127.5F + 45 * std::cos(x * 0.047F - y * 0.029F) +
                35 * edge(std::sin(x * 0.071F + 1) * std::sin(y * 0.13F)) +
                18 * std::cos(x * 0.61F - y * 0.97F),
            127.5F + 45 * std::sin(x * 0.021F + y * 0.053F) + 30 * std::cos(y * 0.19F) +
                16 * std::sin(x * 1.1F + y * 0.7F)};
    };
    const auto scene = [&](float bg_shift, float fg_shift) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.resize(std::size_t(width) * height * kBytesPerPixel);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    std::array<float, 3> sum{};
                    for (UINT j = 0; j < 4; ++j) {
                        for (UINT i = 0; i < 4; ++i) {
                            const float sx = x + (i + 0.5F) / 4, sy = y + (j + 0.5F) / 4;
                            std::array<float, 3> c = background(sx - bg_shift, sy, eye);
                            if (inside(sx, sy, fg_shift)) {
                                const bool stripe =
                                    (int(std::floor((sx - fg_shift) / 6)) + int(sy / 6)) % 2 != 0;
                                c = stripe ? std::array<float, 3>{240, 210, 40}
                                           : std::array<float, 3>{30, 60, 200};
                            }
                            for (UINT k = 0; k < 3; ++k) sum[k] += c[k] / 16;
                        }
                    }
                    const RgbaBytes texel{direction_channel(sum[0]), direction_channel(sum[1]),
                                          direction_channel(sum[2]), 255};
                    std::copy(texel.begin(), texel.end(),
                              bytes.begin() + (std::size_t(y) * width + x) * kBytesPerPixel);
                }
            }
        }
        return pattern;
    };
    // Nearer surfaces have smaller depth here (not reversed).
    const auto box_depth = [&](float shift) {
        g_scale_quality_shift = shift;
        return create_native_test_depth(fixture, gw, gh, [](UINT, UINT x, UINT y) {
            const float px = (x + 0.5F) * width / gw - g_scale_quality_shift,
                        py = (y + 0.5F) * height / gh;
            return px >= box_x && px < box_x + box_size && py >= box_y && py < box_y + box_size
                ? 0.2F : 0.8F;
        });
    };
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_COMMON),
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_COMMON)};
    auto output = create_source_texture(fixture, width, height);
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    auto depth_a = box_depth(0);
    auto still = create_and_upload_game_motion(fixture, gw, gh, {0, 0});
    const auto guides = [&](std::uint64_t serial, const ComPtr<ID3D12Resource>& vectors,
                            const ComPtr<ID3D12Resource>& depth) {
        xrfg::DlssMotionVectorSet set{};
        set.eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
            f->stream = 95 + eye;
            f->epoch = 1;
            f->serial = serial;
            f->previous_serial = serial - 1;
            f->motion_vectors = vectors;
            f->depth = depth;
            f->producer_queue = fixture.queue();
            f->output_width = width;
            f->output_height = height;
            f->motion_width = f->depth_width = gw;
            f->motion_height = f->depth_height = gh;
            f->motion_slice = f->output_slice = eye;
            f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
            set.eyes[eye] = f;
        }
        return set;
    };
    const auto views = make_reprojection_views();
    struct Setting { const char *scale, *detail; };
    // Without the detail restore, and with tolerances of 1/2 (the default) and 1/4.
    std::vector<Setting> settings{{"100", "0"}};
    for (const char* scale : {"85", "75", "67", "50"}) {
        for (const char* detail : {"0", "2", "4"}) settings.push_back({scale, detail});
    }
    // Per setting: overall, centre, outer and edge error, summed over eyes and slides.
    std::vector<std::array<double, 4>> totals(settings.size());
    double blend_total = 0;
    // 3X: each of the two generated frames restores detail from its own point
    // along the motion, a third and two thirds of the way from A.
    const std::array<const char*, 2> triple_scales{"100", "67"};
    std::array<std::array<double, 2>, 2> triple_totals{};
    auto second_output = create_source_texture(fixture, width, height);
    const std::array<xrfg::D3D12NativeDlssG::Output, 2> triple_outputs{
        {{output.Get()}, {second_output.Get()}}};
    const bool triple = xrfg::native_dlssg_max_generated_frames(fixture.device()) >= 2;
    for (const auto& slide : slides) {
        const float bg_motion = slide[0], fg_motion = slide[1];
        const auto a_pattern = scene(0, 0), b_pattern = scene(bg_motion, fg_motion);
        const auto expected = scene(bg_motion / 2, fg_motion / 2);
        for (UINT frame = 0; frame < 2; ++frame) {
            set_texture_state(fixture, sources[frame].Get(), D3D12_RESOURCE_STATE_COMMON,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
            upload_pattern(fixture, sources[frame].Get(), frame ? b_pattern : a_pattern);
            set_texture_state(fixture, sources[frame].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COMMON);
        }
        auto depth_b = box_depth(fg_motion);
        auto motion = create_and_upload_game_motion_field(fixture, gw, gh, [&](UINT, UINT mx, UINT my) {
            const bool fg = inside((mx + 0.5F) * width / gw, (my + 0.5F) * height / gh, fg_motion);
            return std::array<float, 2>{-(fg ? fg_motion : bg_motion) * gw / width, 0.0F};
        });
        const auto error = [&](const StereoPattern& actual, UINT eye, int region) {
            double total = 0;
            std::size_t count = 0;
            const float left = box_x + fg_motion / 2, right = left + box_size;
            for (UINT y = margin; y < height - margin; ++y) {
                for (UINT x = margin; x < width - margin; ++x) {
                    const bool inner = x >= width / 4 && x < width * 3 / 4 && y >= height / 4 &&
                                       y < height * 3 / 4;
                    const bool band = y >= box_y && y < box_y + box_size &&
                        (std::abs(float(x) + 0.5F - left) < 12 ||
                         std::abs(float(x) + 0.5F - right) < 12);
                    if ((region == 1 && !inner) || (region == 2 && inner) || (region == 3 && !band))
                        continue;
                    const std::size_t offset = (std::size_t(y) * width + x) * kBytesPerPixel;
                    for (UINT c = 0; c < 3; ++c, ++count)
                        total += std::abs(int(actual[eye][offset + c]) - int(expected[eye][offset + c]));
                }
            }
            return total / double(count);
        };
        const auto blend = midpoint_pattern(a_pattern, b_pattern);
        for (UINT eye = 0; eye < kEyeCount; ++eye) blend_total += error(blend, eye, 0);
        for (std::size_t s = 0; s < settings.size(); ++s) {
            SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", settings[s].scale);
            SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_DETAIL", settings[s].detail);
            xrfg::D3D12NativeDlssG native;
            require_hresult(native.initialize(fixture.device(), fixture.queue(),
                                              sources[0]->GetDesc(), kFormat),
                            "scale quality initialization");
            const auto seed_a = guides(1, still, depth_a), seed_b = guides(2, still, depth_a);
            const auto moved = guides(3, motion, depth_b);
            HRESULT seeded = E_FAIL, generated = E_FAIL;
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                seeded = native.record(list, 0, sources[0].Get(), sources[0].Get(), views, views,
                                       &seed_a, &seed_b, outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
            });
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                generated = native.record(list, 0, sources[0].Get(), sources[1].Get(), views,
                                          views, &seed_b, &moved, outputs,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            });
            require(seeded == S_OK && generated == S_OK, "scale quality pair failed");
            const auto actual =
                readback_pattern(fixture, output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            for (UINT eye = 0; eye < kEyeCount; ++eye) {
                for (int region = 0; region < 4; ++region) {
                    totals[s][region] += error(actual, eye, region);
                }
            }
        }
        if (!triple) continue;
        const std::array<StereoPattern, 2> thirds{scene(bg_motion / 3, fg_motion / 3),
                                                  scene(bg_motion * 2 / 3, fg_motion * 2 / 3)};
        for (std::size_t s = 0; s < triple_scales.size(); ++s) {
            SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", triple_scales[s]);
            SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_DETAIL", nullptr);
            xrfg::D3D12NativeDlssG native;
            require_hresult(native.initialize(fixture.device(), fixture.queue(),
                                              sources[0]->GetDesc(), kFormat),
                            "scale quality 3X initialization");
            const auto seed_a = guides(1, still, depth_a), seed_b = guides(2, still, depth_a);
            const auto moved = guides(3, motion, depth_b);
            HRESULT seeded = E_FAIL, generated = E_FAIL;
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                seeded = native.record(list, 0, sources[0].Get(), sources[0].Get(), views, views,
                                       &seed_a, &seed_b, triple_outputs,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            });
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                generated = native.record(list, 0, sources[0].Get(), sources[1].Get(), views,
                                          views, &seed_b, &moved, triple_outputs,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            });
            require(seeded == S_OK && generated == S_OK, "scale quality 3X pair failed");
            for (UINT index = 0; index < 2; ++index) {
                const auto actual = readback_pattern(fixture, triple_outputs[index].image,
                                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
                for (UINT eye = 0; eye < kEyeCount; ++eye) {
                    triple_totals[s][index] +=
                        mean_absolute_rgb_error(actual, thirds[index], width, height, eye, margin);
                }
            }
        }
    }
    SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", nullptr);
    SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_DETAIL", nullptr);
    const double runs = double(slides.size() * kEyeCount);
    std::cout << "scale quality blend mae=" << blend_total / runs << '\n';
    for (std::size_t s = 0; s < settings.size(); ++s) {
        std::cout << "scale quality scale=" << settings[s].scale << " detail=" << settings[s].detail
                  << " mae=" << totals[s][0] / runs << " centre=" << totals[s][1] / runs
                  << " outer=" << totals[s][2] / runs << " edge=" << totals[s][3] / runs << '\n';
    }
    for (std::size_t s = 0; triple && s < triple_scales.size(); ++s) {
        std::cout << "scale quality 3X scale=" << triple_scales[s]
                  << " third_mae=" << triple_totals[s][0] / runs
                  << " two_thirds_mae=" << triple_totals[s][1] / runs << '\n';
    }
    fixture.require_no_debug_errors();
}

// Opt-in with XRFG_TEST_NATIVE_DLSSG_ROTATION_SWEEP: how well native
// generation reproduces a static, detailed scene after a small head rotation
// (about 0.25 to 16 pixels). Every rotation reseeds the history with the
// aligned previous frame, so the error is mostly that frame's resampling.
// Keeping the history instead and aligning the generated image in the
// compose pass measured two to five times worse, even with Catmull-Rom.
void bench_native_dlss_rotation_sweep(D3D12WarpFixture& fixture) {
    constexpr UINT width = 512, height = 512, margin = 24;
    const std::array<float, 7> yaws_degrees{0.05F, 0.1F, 0.2F, 0.4F, 0.8F, 1.6F, 3.2F};
    auto depth = create_native_test_depth(fixture, width, height);
    auto still_motion = create_and_upload_game_motion(fixture, width, height, {0, 0});
    const auto still = make_reprojection_views();
    const auto still_pattern = detailed_world_pattern(width, height, still);
    std::array<ComPtr<ID3D12Resource>, 2> still_sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    for (auto& source : still_sources) {
        upload_pattern(fixture, source.Get(), still_pattern);
        set_texture_state(fixture, source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COMMON);
    }
    auto turned_source = create_source_texture(fixture, width, height);
    auto output = create_source_texture(fixture, width, height);
    const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
    const auto guides = [&](std::uint64_t serial, const ComPtr<ID3D12Resource>& motion) {
        xrfg::DlssMotionVectorSet set{};
        set.eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
            f->stream = 81 + eye;
            f->epoch = 1;
            f->serial = serial;
            f->previous_serial = serial - 1;
            f->motion_vectors = motion;
            f->depth = depth;
            f->producer_queue = fixture.queue();
            f->output_width = f->depth_width = f->motion_width = width;
            f->output_height = f->depth_height = f->motion_height = height;
            f->motion_slice = f->output_slice = eye;
            f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
            set.eyes[eye] = f;
        }
        return set;
    };
    for (const float yaw_degrees : yaws_degrees) {
        const auto turned = make_reprojection_views(yaw_degrees * 3.14159265F / 180.0F);
        const auto turned_pattern = detailed_world_pattern(width, height, turned);
        upload_pattern(fixture, turned_source.Get(), turned_pattern);
        set_texture_state(fixture, turned_source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COMMON);
        auto motion = create_and_upload_game_motion_field(
            fixture, width, height, [&](UINT eye, UINT x, UINT y) {
                float sx = float(x), sy = float(y);
                (void)target_pixel_maps_to_source(still[eye], turned[eye], width, height, x,
                                                  y, &sx, &sy);
                return std::array<float, 2>{sx - float(x), sy - float(y)};
            });
        xrfg::D3D12NativeDlssG native;
        require_hresult(native.initialize(fixture.device(), fixture.queue(),
                                          still_sources[0]->GetDesc(), kFormat),
                        "rotation sweep native initialization");
        const auto seed_a = guides(1, still_motion), seed_b = guides(2, still_motion);
        const auto turn_b = guides(3, motion);
        HRESULT seeded = E_FAIL, turned_pair = E_FAIL;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            seeded = native.record(list, 0, still_sources[0].Get(), still_sources[1].Get(),
                                   still, still, &seed_a, &seed_b, outputs,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        const std::uint64_t reseeds_before = native.reseeds();
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            turned_pair = native.record(list, 0, still_sources[1].Get(), turned_source.Get(),
                                        still, turned, &seed_b, &turn_b, outputs,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        require(seeded == S_OK && turned_pair == S_OK, "rotation sweep pair failed");
        const auto actual =
            readback_pattern(fixture, output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        std::cout << "rotation sweep yaw_deg=" << yaw_degrees
                  << " reseeded=" << (native.reseeds() != reseeds_before);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            std::cout << " eye" << eye << "_mae="
                      << mean_absolute_rgb_error(actual, turned_pattern, width, height, eye,
                                                 margin)
                      << " eye" << eye << "_moved_mae="
                      << mean_absolute_rgb_error(still_pattern, turned_pattern, width,
                                                 height, eye, margin);
        }
        std::cout << '\n';
        set_texture_state(fixture, turned_source.Get(), D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    fixture.require_no_debug_errors();
}

// Opt-in with XRFG_TEST_NATIVE_DLSSG_LAYOUT_BENCH: GPU time of NGX for one
// eye, two separate eyes, and both eyes as one side-by-side feature. NGX's
// cost is sublinear in pixels, so the layouts show its fixed per-evaluation
// share. Still head, 2X, so no reseeding.
void bench_native_dlss_layouts(D3D12WarpFixture& fixture) {
    constexpr UINT eye_width = 2004, height = 2004, warmup = 8, measured = 32;
    struct Layout {
        const char* name;
        UINT width, slices, views;
    };
    const std::array<Layout, 3> layouts{{{"one eye, one feature", eye_width, 1, 1},
                                         {"two eyes, two features", eye_width, 2, 2},
                                         {"two eyes side by side, one feature", eye_width * 2, 1, 1}}};
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = 2;
    require_hresult(fixture.device()->CreateQueryHeap(&query_description, IID_PPV_ARGS(&queries)),
                    "layout bench query heap");
    auto readback = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, 2 * sizeof(UINT64),
                                  D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency{};
    require_hresult(fixture.queue()->GetTimestampFrequency(&frequency), "layout bench frequency");
    for (const auto& layout : layouts) {
        std::array<ComPtr<ID3D12Resource>, 2> sources{
            create_source_texture(fixture, layout.width, height, D3D12_RESOURCE_STATE_COMMON, UINT16(layout.slices)),
            create_source_texture(fixture, layout.width, height, D3D12_RESOURCE_STATE_COMMON, UINT16(layout.slices))};
        auto image = create_source_texture(fixture, layout.width, height,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET, UINT16(layout.slices));
        auto motion = create_and_upload_game_motion(fixture, layout.width * 2 / 3, height * 2 / 3, {-3, 3});
        auto depth = create_native_test_depth(fixture, layout.width * 2 / 3, height * 2 / 3);
        xrfg::D3D12NativeDlssG native;
        require_hresult(native.initialize(fixture.device(), fixture.queue(), sources[0]->GetDesc(), kFormat),
                        "layout bench initialization");
        const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{image.Get()}}};
        auto views = make_reprojection_views();
        for (UINT view = 0; view < layout.views; ++view) {
            views[view].image_rect = {0, 0, layout.width, height};
            views[view].array_slice = view;
        }
        std::vector<double> samples;
        for (UINT pair = 1; pair <= warmup + measured; ++pair) {
            xrfg::DlssMotionVectorSet a_guides{}, b_guides{};
            a_guides.eye_count = b_guides.eye_count = layout.views;
            for (UINT eye = 0; eye < layout.views; ++eye) {
                for (const UINT next : {0U, 1U}) {
                    auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
                    f->stream = 91 + eye;
                    f->epoch = 1;
                    f->serial = pair + next;
                    f->previous_serial = pair + next - 1;
                    f->motion_vectors = motion;
                    f->depth = depth;
                    f->producer_queue = fixture.queue();
                    f->output_width = layout.width;
                    f->output_height = height;
                    f->motion_width = f->depth_width = layout.width * 2 / 3;
                    f->motion_height = f->depth_height = height * 2 / 3;
                    f->motion_slice = f->output_slice = eye;
                    f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
                    (next ? b_guides : a_guides).eyes[eye] = f;
                }
            }
            HRESULT recorded = E_FAIL;
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                recorded = native.record(list, 0, sources[pair & 1].Get(), sources[(pair + 1) & 1].Get(),
                                         std::span(views.data(), layout.views),
                                         std::span(views.data(), layout.views), &a_guides, &b_guides,
                                         outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
            });
            require(recorded == S_OK, std::string("layout bench pair not generated: ") + layout.name);
            void* mapped{};
            const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
            require_hresult(readback->Map(0, &range, &mapped), "layout bench readback");
            const auto* stamps = static_cast<const UINT64*>(mapped);
            if (pair > warmup) samples.push_back(double(stamps[1] - stamps[0]) * 1e6 / double(frequency));
            const D3D12_RANGE no_write{0, 0};
            readback->Unmap(0, &no_write);
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "layout bench " << layout.name << " (" << layout.width << "x" << height
                  << "): p10_us=" << samples[samples.size() / 10]
                  << " median_us=" << samples[samples.size() / 2] << '\n';
    }
    fixture.require_no_debug_errors();
}

// Opt-in with XRFG_TEST_NATIVE_DLSSG_BENCH: GPU time of one native stereo pair
// at a headset-class eye size, with engine guides at a DLSS-quality render
// size. A still head keeps NGX history; a turning head changes the camera on
// every pair. Each case runs at 2X and 3X.
// XRFG_TEST_BENCH_EYE=WxH replaces a benchmark's per-eye size, such as a
// Steam Frame's 3004x3004; the guides stay at two thirds of it.
std::array<UINT, 2> bench_eye_size(UINT width, UINT height) {
    if (const char* value = std::getenv("XRFG_TEST_BENCH_EYE")) {
        UINT w = 0, h = 0;
        if (std::sscanf(value, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64) return {w, h};
    }
    return {width, height};
}

void bench_native_dlss_pairs(D3D12WarpFixture& fixture) {
    const auto eye = bench_eye_size(2064, 2208);
    const UINT width = eye[0], height = eye[1];
    const UINT render_width = width * 2 / 3, render_height = height * 2 / 3;
    constexpr UINT warmup = 8, measured = 96;
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_COMMON),
        create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_COMMON)};
    std::array<ComPtr<ID3D12Resource>, 2> images{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    // The frame-generation benchmark's scene. Blank frames would compress to
    // almost nothing in GPU memory, and NGX measures 20-25% faster on them.
    constexpr int shift = 12;
    auto motion = create_and_upload_game_motion(fixture, render_width, render_height,
        {-float(shift) * render_width / width, float(shift) * render_width / width});
    for (UINT i = 0; i < 2; ++i) {
        const int moved = i ? shift : 0;
        set_texture_state(fixture, sources[i].Get(), D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
        upload_pattern(fixture, sources[i].Get(), translated_motion_pattern(width, height, {moved, -moved}));
        set_texture_state(fixture, sources[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COMMON);
    }
    auto depth = create_native_test_depth(fixture, render_width, render_height);
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = 2;
    require_hresult(fixture.device()->CreateQueryHeap(&query_description, IID_PPV_ARGS(&queries)),
                    "native bench query heap");
    auto readback = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, 2 * sizeof(UINT64),
                                  D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency{};
    require_hresult(fixture.queue()->GetTimestampFrequency(&frequency), "native bench frequency");
    const auto frame = [&](UINT eye, std::uint64_t serial) {
        auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
        f->stream = 41 + eye;
        f->epoch = 1;
        f->serial = serial;
        f->previous_serial = serial - 1;
        f->motion_vectors = motion;
        f->depth = depth;
        f->producer_queue = fixture.queue();
        f->output_width = width;
        f->output_height = height;
        f->motion_width = f->depth_width = render_width;
        f->motion_height = f->depth_height = render_height;
        f->motion_slice = f->output_slice = eye;
        f->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
        return f;
    };
    for (const UINT output_count : {1U, 2U}) {
        for (const bool turning : {false, true}) {
            xrfg::D3D12NativeDlssG native;
            require_hresult(native.initialize(fixture.device(), fixture.queue(),
                                              sources[0]->GetDesc(), kFormat),
                            "native bench initialization");
            std::vector<xrfg::D3D12NativeDlssG::Output> outputs;
            for (UINT output = 0; output < output_count; ++output)
                outputs.push_back({images[output].Get()});
            std::vector<double> samples;
            for (UINT pair = 1; pair <= warmup + measured; ++pair) {
                xrfg::DlssMotionVectorSet a_guides{}, b_guides{};
                a_guides.eye_count = b_guides.eye_count = kEyeCount;
                for (UINT eye = 0; eye < kEyeCount; ++eye) {
                    a_guides.eyes[eye] = frame(eye, pair);
                    b_guides.eyes[eye] = frame(eye, pair + 1);
                }
                const auto a_views = make_reprojection_views(turning ? 0.01F * pair : 0.0F);
                const auto b_views = make_reprojection_views(turning ? 0.01F * (pair + 1) : 0.0F);
                HRESULT recorded = E_FAIL;
                fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                    recorded = native.record(list, 0, sources[pair & 1].Get(),
                                             sources[(pair + 1) & 1].Get(), a_views, b_views,
                                             &a_guides, &b_guides, outputs,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
                    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                           readback.Get(), 0);
                });
                require(recorded == S_OK, "native bench pair was not generated");
                void* mapped{};
                const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
                require_hresult(readback->Map(0, &range, &mapped), "native bench readback");
                const auto* stamps = static_cast<const UINT64*>(mapped);
                const double microseconds =
                    double(stamps[1] - stamps[0]) * 1000000.0 / double(frequency);
                const D3D12_RANGE no_write{0, 0};
                readback->Unmap(0, &no_write);
                if (pair > warmup) samples.push_back(microseconds);
            }
            // Other GPU work inflates single samples; the low percentile is
            // the steadier comparison between runs.
            std::sort(samples.begin(), samples.end());
            std::cout << "native bench " << width << "x" << height << " per eye, "
                      << (output_count + 1) << "X, " << (turning ? "turning" : "still")
                      << " head: p10_us=" << samples[samples.size() / 10]
                      << " median_us=" << samples[samples.size() / 2] << '\n';
        }
    }
    fixture.require_no_debug_errors();
}
#endif

void test_dlss_motion_vector_strafe_rejects_double_edges(
    D3D12WarpFixture& fixture) {
    constexpr UINT width = 256;
    constexpr UINT height = 128;
    constexpr UINT object_width = 64;
    constexpr UINT object_top = 32;
    constexpr UINT object_bottom = 96;
    const std::array<int, kEyeCount> previous_left{48, 144};
    const std::array<int, kEyeCount> displacement{32, -32};
    const std::array<RgbaBytes, kEyeCount> background{
        RgbaBytes{12, 20, 28, 255},
        RgbaBytes{20, 12, 32, 255}};
    const RgbaBytes foreground{238, 222, 56, 255};

    const auto make_scene = [&](bool current) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
            const int left = previous_left[eye] +
                (current ? displacement[eye] : 0);
            const int right = left + static_cast<int>(object_width);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    const bool object = y >= object_top && y < object_bottom &&
                        static_cast<int>(x) >= left && static_cast<int>(x) < right;
                    const auto& color = object ? foreground : background[eye];
                    const std::size_t offset =
                        (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
                    std::copy(color.begin(), color.end(), bytes.begin() + offset);
                }
            }
        }
        return pattern;
    };

    const StereoPattern previous = make_scene(false);
    const StereoPattern current = make_scene(true);
    auto game_motion = create_and_upload_game_motion_field(
        fixture, width, height, [&](UINT eye, UINT x, UINT y) {
            const int left = previous_left[eye] + displacement[eye];
            const int right = left + static_cast<int>(object_width);
            const bool object = y >= object_top && y < object_bottom &&
                static_cast<int>(x) >= left && static_cast<int>(x) < right;
            return std::array<float, 2>{
                object ? static_cast<float>(-displacement[eye]) : 0.0F,
                0.0F};
        });

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, width, height)};
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{
        synthetic_destinations[0].Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(
                fixture.device(), fixture.queue(), source_pointers,
                D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "strafe history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(operation_succeeded(synthesizer.initialize(
                fixture.device(), fixture.queue(), history, current_pointers,
                synthetic_pointers, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                xrfg::D3D12OpticalFlowBackend::fidelity_fx)),
        "strafe synthesizer initialization failed");

    auto frame_a = std::make_shared<xrfg::DlssMotionVectorFrame>();
    frame_a->stream = 301;
    frame_a->epoch = 7;
    frame_a->serial = 1;
    frame_a->motion_vectors = game_motion;
    frame_a->producer_queue = fixture.queue();
    frame_a->output_width = width;
    frame_a->output_height = height;
    frame_a->motion_width = width;
    frame_a->motion_height = height;
    frame_a->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto frame_b = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_a);
    frame_b->previous_serial = 1;
    frame_b->serial = 2;
    auto frame_a_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_a);
    frame_a_right->stream = 302;
    frame_a_right->motion_slice = 1;
    auto frame_b_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b);
    frame_b_right->stream = 302;
    frame_b_right->motion_slice = 1;
    auto set_a = std::make_shared<xrfg::DlssMotionVectorSet>();
    set_a->eye_count = kEyeCount;
    set_a->eyes = {frame_a, frame_a_right};
    auto set_b = std::make_shared<xrfg::DlssMotionVectorSet>();
    set_b->eye_count = kEyeCount;
    set_b->eyes = {frame_b, frame_b_right};

    const ReprojectionViews views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "strafe capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(operation_succeeded(synthesizer.submit_prime(
                capture_a, views, 0, &prime, set_a)),
        "strafe prime failed");
    require_frame_start_gate(synthesizer, "strafe frame-start gate");
    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "strafe capture B failed");
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(operation_succeeded(synthesizer.submit_pair(
                capture_b, views, views, 0, 1, &pair, std::nullopt, set_b)),
        "strafe pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});

    const StereoPattern actual = readback_pattern(
        fixture, synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);
    std::array<std::size_t, kEyeCount> double_edge_pixels{};
    std::array<std::size_t, kEyeCount> baseline_double_edge_pixels{};
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const int minimum_x = std::min(previous_left[eye],
            previous_left[eye] + displacement[eye]) - 2;
        const int maximum_x = std::max(previous_left[eye],
            previous_left[eye] + displacement[eye]) +
            static_cast<int>(object_width) + 2;
        for (UINT y = object_top + 4; y < object_bottom - 4; ++y) {
            for (int x = minimum_x; x < maximum_x; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width +
                        static_cast<UINT>(x)) * kBytesPerPixel;
                const auto distance = [&](const RgbaBytes& color) {
                    return std::abs(static_cast<int>(actual[eye][offset]) - color[0]) +
                        std::abs(static_cast<int>(actual[eye][offset + 1]) - color[1]) +
                        std::abs(static_cast<int>(actual[eye][offset + 2]) - color[2]);
                };
                if (std::min(distance(background[eye]), distance(foreground)) > 24) {
                    ++double_edge_pixels[eye];
                }
                const auto baseline_distance = [&](const RgbaBytes& color) {
                    return std::abs(static_cast<int>(same_pixel_blend[eye][offset]) - color[0]) +
                        std::abs(static_cast<int>(same_pixel_blend[eye][offset + 1]) - color[1]) +
                        std::abs(static_cast<int>(same_pixel_blend[eye][offset + 2]) - color[2]);
                };
                if (std::min(baseline_distance(background[eye]),
                        baseline_distance(foreground)) > 24) {
                    ++baseline_double_edge_pixels[eye];
                }
            }
        }
        std::cout << "DLSS strafe eye=" << eye
                  << " double_edge_pixels=" << double_edge_pixels[eye]
                  << " same_pixel_baseline=" << baseline_double_edge_pixels[eye] << '\n';
        require(double_edge_pixels[eye] * 2 <=
                baseline_double_edge_pixels[eye] + 64,
            "DLSS strafe did not remove the motion-compensable double edge for eye " +
                std::to_string(eye));
    }
    require(operation_succeeded(synthesizer.wait_for_idle()),
        "strafe final drain failed");
    require(operation_succeeded(history->invalidate()),
        "strafe history invalidate failed");
}

void test_rotation_aware_synthesis_beats_uncompensated_flow(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT kRotationWidth = 160;
    constexpr UINT kRotationHeight = 80;
    constexpr UINT kEvaluationMargin = 20;
    constexpr float kYawRadians = 0.2094395102393195F;

    const ReprojectionViews views_a = make_reprojection_views(0.0F);
    const ReprojectionViews views_b = make_reprojection_views(kYawRadians);
    const ReprojectionViews identity_views = make_reprojection_views(0.0F);
    const StereoPattern previous =
        ray_direction_pattern(kRotationWidth, kRotationHeight, views_a);
    const StereoPattern current =
        ray_direction_pattern(kRotationWidth, kRotationHeight, views_b);
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);

    const auto synthesize = [&](const char* label,
                                const ReprojectionViews& source_views_a,
                                const ReprojectionViews& source_views_b,
                                const ReprojectionViews& target_views,
                                const StereoPattern& input_previous,
                                const StereoPattern& input_current,
                                std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_a = {},
                                std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_b = {}) {
        std::array<ComPtr<ID3D12Resource>, 2> sources{
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
        };
        std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
        };
        std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
        };
        std::array<ID3D12Resource*, 2> source_pointers{
            sources[0].Get(),
            sources[1].Get(),
        };
        std::array<ID3D12Resource*, 2> current_destination_pointers{
            current_destinations[0].Get(),
            current_destinations[1].Get(),
        };
        std::array<ID3D12Resource*, 1> synthetic_destination_pointers{
            synthetic_destinations[0].Get(),
        };
        upload_pattern(fixture, sources[0].Get(), input_previous);
        upload_pattern(fixture, sources[1].Get(), input_current);

        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        require(
            operation_succeeded(history->initialize(
                fixture.device(),
                fixture.queue(),
                std::span<ID3D12Resource* const>(
                    source_pointers.data(),
                    source_pointers.size()),
                D3D12_RESOURCE_STATE_RENDER_TARGET)),
            std::string(label) + " history initialization failed");
        xrfg::D3D12FrameSynthesizer synthesizer;
        require(
            operation_succeeded(synthesizer.initialize(
                fixture.device(),
                fixture.queue(),
                history,
                std::span<ID3D12Resource* const>(
                    current_destination_pointers.data(),
                    current_destination_pointers.size()),
                std::span<ID3D12Resource* const>(
                    synthetic_destination_pointers.data(),
                    synthetic_destination_pointers.size()),
                kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                backend,
                motion_a && motion_b ? nvidia_options : xrfg::D3D12NvidiaOpticalFlowOptions{
                    nvidia_options.preset,nvidia_options.input_scale,nvidia_options.bidirectional})),
            std::string(label) + " synthesizer initialization failed");
        xrfg::D3D12HistoryCaptureTicket capture_a{};
        require(
            operation_succeeded(history->capture(0, &capture_a)) &&
                operation_succeeded(history->commit(capture_a)),
            std::string(label) + " capture A failed");
        xrfg::D3D12FrameSynthesisTicket prime{};
        require(
            operation_succeeded(synthesizer.submit_prime(
                capture_a,
                source_views_a,
                0,
                &prime,
                std::move(motion_a))),
            std::string(label) + " prime submission failed");
        require_frame_start_gate(
            synthesizer, std::string(label) + " frame-start gate");

        xrfg::D3D12HistoryCaptureTicket capture_b{};
        require(
            operation_succeeded(history->capture(1, &capture_b)) &&
                operation_succeeded(history->commit(capture_b)),
            std::string(label) + " capture B failed");
        xrfg::D3D12FrameSynthesisTicket pair{};
        require(
            operation_succeeded(synthesizer.submit_pair(
                capture_b,
                source_views_b,
                target_views,
                0,
                1,
                &pair,
                std::nullopt,
                std::move(motion_b))),
            std::string(label) + " pair submission failed");
        require(
            pair.previous_serial == capture_a.serial &&
                pair.current_serial == capture_b.serial,
            std::string(label) + " pair chronology is incorrect");
        require(
            operation_succeeded(synthesizer.wait_for_idle()),
            std::string(label) + " synthesis drain failed");

        const StereoPattern actual_current = readback_pattern(
            fixture,
            current_destinations[1].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const StereoPattern actual_synthetic = readback_pattern(
            fixture,
            synthetic_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(
            actual_current == input_current,
            std::string(label) + " current output is not a bit-exact copy of B");
        require_source_is_render_target(fixture, sources[0].Get());
        require_source_is_render_target(fixture, sources[1].Get());
        require(
            operation_succeeded(history->invalidate()),
            std::string(label) + " history invalidate failed");
        return actual_synthetic;
    };

    const auto validate_cropped_viewport = [&] {
        constexpr xrfg::D3D12ImageRect kCroppedRect{16, 20, 128, 40};
        ReprojectionViews cropped_views_a = make_reprojection_views(0.0F);
        ReprojectionViews cropped_views_b = make_reprojection_views(kYawRadians);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            cropped_views_a[eye].image_rect = kCroppedRect;
            cropped_views_b[eye].image_rect = kCroppedRect;
        }
        const StereoPattern cropped_previous = ray_direction_pattern(
            kRotationWidth, kRotationHeight, cropped_views_a);
        const StereoPattern cropped_current = ray_direction_pattern(
            kRotationWidth, kRotationHeight, cropped_views_b);
        const StereoPattern cropped_synthetic = synthesize(
            "cropped rotation-aware",
            cropped_views_a,
            cropped_views_b,
            cropped_views_b,
            cropped_previous,
            cropped_current);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            const double cropped_error = mean_absolute_rgb_error_rect(
                cropped_synthetic,
                cropped_current,
                kRotationWidth,
                eye,
                kCroppedRect,
                8);
            std::cout << "cropped rotation eye=" << eye
                      << " aware_mae=" << cropped_error << '\n';
            require(
                cropped_error <= 1.0,
                "cropped viewport camera mapping is inaccurate for eye " +
                    std::to_string(eye) + ": aware=" +
                    std::to_string(cropped_error));
        }
    };

    if (backend == xrfg::D3D12OpticalFlowBackend::nvidia) {
        validate_cropped_viewport();
        return;
    }

    const StereoPattern rotation_aware = synthesize(
        "rotation-aware",
        views_a,
        views_b,
        views_b,
        previous,
        current);
    // Feed a dense B-to-A field that already includes the complete projective
    // head rotation. Subtracting the pose as a linear pixel displacement is
    // only an approximation and leaves rotation residue. The shader must map
    // the source endpoint back through the exact inverse OpenXR camera map.
    ComPtr<ID3D12Resource> pose_inclusive_game_motion =
        create_and_upload_game_motion_field(
            fixture,
            kRotationWidth,
            kRotationHeight,
            [&](UINT eye, UINT x, UINT y) {
                // Like an engine, point uncovered pixels at their source
                // outside A's view rather than claiming they did not move.
                float source_x = static_cast<float>(x);
                float source_y = static_cast<float>(y);
                (void)target_pixel_maps_to_source(
                    views_a[eye],
                    views_b[eye],
                    kRotationWidth,
                    kRotationHeight,
                    x,
                    y,
                    &source_x,
                    &source_y);
                return std::array<float, 2>{
                    source_x - static_cast<float>(x),
                    source_y - static_cast<float>(y)};
            });
    auto game_frame_a = std::make_shared<xrfg::DlssMotionVectorFrame>();
    game_frame_a->stream = 31;
    game_frame_a->epoch = 1;
    game_frame_a->serial = 1;
    game_frame_a->motion_vectors = pose_inclusive_game_motion;
    game_frame_a->producer_queue = fixture.queue();
    game_frame_a->output_width = kRotationWidth;
    game_frame_a->output_height = kRotationHeight;
    game_frame_a->motion_width = kRotationWidth;
    game_frame_a->motion_height = kRotationHeight;
    game_frame_a->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if(nvidia_options.frame_generation==xrfg::D3D12FrameGeneration::native_dlss) {
        game_frame_a->depth=create_native_test_depth(fixture,kRotationWidth,kRotationHeight);
        game_frame_a->depth_width=kRotationWidth; game_frame_a->depth_height=kRotationHeight;
        game_frame_a->depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
    }
    auto game_frame_b = std::make_shared<xrfg::DlssMotionVectorFrame>(*game_frame_a);
    game_frame_b->previous_serial = 1;
    game_frame_b->serial = 2;
    auto game_frame_a_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*game_frame_a);
    game_frame_a_right->stream = 32;
    game_frame_a_right->motion_slice = 1;
    game_frame_a_right->output_slice = 1;
    auto game_frame_b_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*game_frame_b);
    game_frame_b_right->stream = 32;
    game_frame_b_right->motion_slice = 1;
    game_frame_b_right->output_slice = 1;
    auto game_set_a = std::make_shared<xrfg::DlssMotionVectorSet>();
    game_set_a->eye_count = kEyeCount;
    game_set_a->eyes = {game_frame_a, game_frame_a_right};
    auto game_set_b = std::make_shared<xrfg::DlssMotionVectorSet>();
    game_set_b->eye_count = kEyeCount;
    game_set_b->eyes = {game_frame_b, game_frame_b_right};
    const StereoPattern game_pose_separated = synthesize(
        "DLSS iterative endpoint inversion",
        views_a,
        views_b,
        views_b,
        previous,
        current,
        game_set_a,
        game_set_b);
    const StereoPattern uncompensated = synthesize(
        "uncompensated-flow baseline",
        identity_views,
        identity_views,
        identity_views,
        previous,
        current);

    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double aware_error = mean_absolute_rgb_error(
            rotation_aware,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double uncompensated_error = mean_absolute_rgb_error(
            uncompensated,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double blend_error = mean_absolute_rgb_error(
            same_pixel_blend,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double previous_error = mean_absolute_rgb_error(
            previous,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double game_pose_error = mean_absolute_rgb_error(
            game_pose_separated,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        double uncovered_total = 0.0;
        std::size_t uncovered_samples = 0;
        for (UINT y = 0; y < kRotationHeight; ++y) {
            for (UINT x = 0; x < kRotationWidth; ++x) {
                if (target_pixel_maps_to_source(
                        views_a[eye],
                        views_b[eye],
                        kRotationWidth,
                        kRotationHeight,
                        x,
                        y)) {
                    continue;
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kRotationWidth + x) *
                    kBytesPerPixel;
                for (UINT channel = 0; channel < 3; ++channel) {
                    uncovered_total += std::abs(
                        static_cast<int>(rotation_aware[eye][offset + channel]) -
                        static_cast<int>(current[eye][offset + channel]));
                    ++uncovered_samples;
                }
            }
        }
        require(uncovered_samples != 0, "rotation fixture has no uncovered target edge");
        const double uncovered_error =
            uncovered_total / static_cast<double>(uncovered_samples);
        std::cout << "rotation eye=" << eye
                  << " aware_mae=" << aware_error
                  << " game_pose_mae=" << game_pose_error
                  << " uncompensated_flow_mae=" << uncompensated_error
                  << " blend_mae=" << blend_error
                  << " previous_mae=" << previous_error
                  << " uncovered_mae=" << uncovered_error << '\n';
        require(
            aware_error < uncompensated_error * 0.35 &&
                aware_error < blend_error * 0.35 &&
                aware_error < previous_error * 0.25 &&
                uncovered_error <= 0.5,
            "rotation-aware synthesis does not materially beat uncompensated flow for eye " +
                std::to_string(eye) + ": aware=" + std::to_string(aware_error) +
                " uncompensated=" + std::to_string(uncompensated_error) +
                " uncovered=" + std::to_string(uncovered_error));
        require(
            game_pose_error <= 1.0,
            "DLSS vectors retained projective OpenXR rotation residue for eye " +
                std::to_string(eye) + ": game_pose=" +
                std::to_string(game_pose_error));
    }

    validate_cropped_viewport();
}

void test_double_wide_single_slice_views(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT kHalfWidth = 64;
    constexpr UINT kDoubleWidth = kHalfWidth * 2U;
    constexpr UINT kDoubleHeight = 64;

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
    };
    clear_double_wide_pattern(
        fixture, sources[0].Get(), kHalfWidth, kDoubleHeight);
    clear_double_wide_pattern(
        fixture, sources[1].Get(), kHalfWidth, kDoubleHeight);

    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{
        synthetic_destinations[0].Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(
                source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "double-wide history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            std::span<ID3D12Resource* const>(
                current_pointers.data(), current_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_pointers.data(), synthetic_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            nvidia_options)),
        "double-wide synthesizer initialization failed");

    ReprojectionViews views = make_reprojection_views();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        views[eye].image_rect = {
            eye * kHalfWidth, 0, kHalfWidth, kDoubleHeight};
        views[eye].array_slice = 0;
    }

    xrfg::D3D12HistoryCaptureTicket capture{};
    require(
        operation_succeeded(history->capture(0, &capture)) &&
            operation_succeeded(history->commit(capture)),
        "double-wide capture failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(synthesizer.submit_prime(
            capture, views, 0, &prime)),
        "double-wide prime failed");
    require_frame_start_gate(synthesizer, "double-wide frame-start gate");
    xrfg::D3D12FrameSynthesisTicket repeated{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture, views, views, 0, 1, &repeated)),
        "double-wide repeated pair failed");
    const auto validate_pattern = [&](ID3D12Resource* resource,
                                      const char* label,
                                      double maximum_mae) {
        const std::vector<std::uint8_t> pixels = readback_single_slice(
            fixture, resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        double total_error = 0.0;
        std::size_t sample_count = 0;
        for (UINT y = 0; y < kDoubleHeight; ++y) {
            for (UINT x = 0; x < kDoubleWidth; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kDoubleWidth + x) *
                    kBytesPerPixel;
                const RgbaBytes expected = x < kHalfWidth
                    ? RgbaBytes{255, 0, 0, 255}
                    : RgbaBytes{0, 0, 255, 255};
                for (UINT channel = 0; channel < 3; ++channel) {
                    total_error += std::abs(
                        static_cast<int>(pixels[offset + channel]) -
                        static_cast<int>(expected[channel]));
                    ++sample_count;
                }
            }
        }
        const double mae = total_error / static_cast<double>(sample_count);
        if (maximum_mae > 0.0) {
            std::cout << label << " mae=" << mae << '\n';
        }
        require(
            mae <= maximum_mae,
            std::string(label) +
                " does not preserve both single-slice eye viewports: mae=" +
                std::to_string(mae));
    };
    validate_pattern(
        current_destinations[1].Get(), "double-wide current output", 0.0);
    validate_pattern(
        synthetic_destinations[0].Get(), "double-wide synthetic output", 0.0);

    xrfg::D3D12HistoryCaptureTicket advanced_capture{};
    require(
        operation_succeeded(history->capture(1, &advanced_capture)) &&
            operation_succeeded(history->commit(advanced_capture)),
        "double-wide advancing capture failed");
    xrfg::D3D12FrameSynthesisTicket advanced_pair{};
    const HRESULT advanced_pair_result = synthesizer.submit_pair(
        advanced_capture, views, views, 0, 0, &advanced_pair);
    require(
        operation_succeeded(advanced_pair_result),
        "double-wide advancing pair failed with HRESULT " +
            std::to_string(static_cast<std::int32_t>(advanced_pair_result)));
    require(
        operation_succeeded(synthesizer.wait_for_idle()),
        "double-wide advancing synthesis drain failed");
    validate_pattern(
        current_destinations[0].Get(),
        "double-wide advancing current output",
        0.0);
    validate_pattern(
        synthetic_destinations[0].Get(),
        "double-wide advancing synthetic output",
        backend == xrfg::D3D12OpticalFlowBackend::nvidia ? 2.0 : 0.0);
    require(
        operation_succeeded(history->invalidate()),
        "double-wide history invalidate failed");
}

void test_submission_backpressure_and_recovery(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT width = 1024;
    constexpr UINT height = 512;
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 3> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 2> synthetic_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(), sources[1].Get(), sources[2].Get()};
    std::array<ID3D12Resource*, 3> current_destination_pointers{
        current_destinations[0].Get(), current_destinations[1].Get(),
        current_destinations[2].Get()};
    std::array<ID3D12Resource*, 2> synthetic_destination_pointers{
        synthetic_destinations[0].Get(), synthetic_destinations[1].Get()};

    upload_pattern(
        fixture, sources[0].Get(),
        translated_motion_pattern(width, height, {0, 0}));
    upload_pattern(
        fixture, sources[1].Get(),
        translated_motion_pattern(width, height, {16, -16}));
    upload_pattern(
        fixture, sources[2].Get(),
        translated_motion_pattern(width, height, {32, -32}));

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(), fixture.queue(), source_pointers,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "backpressure history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(), fixture.queue(), history,
            current_destination_pointers, synthetic_destination_pointers,
            kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend, nvidia_options)),
        "backpressure synthesizer initialization failed");

    ComPtr<ID3D12Fence> gate;
    require_hresult(
        fixture.device()->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gate.GetAddressOf())),
        "ID3D12Device::CreateFence(backpressure gate)");
    require_hresult(
        fixture.queue()->Wait(gate.Get(), 1),
        "ID3D12CommandQueue::Wait(backpressure gate)");

    const ReprojectionViews views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "backpressure capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(
            synthesizer.submit_prime(capture_a, views, 0, &prime)),
        "backpressure prime failed");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "backpressure capture B failed");
    // Backpressure is per-resource, not global. A pair that reuses the
    // destination the prime is still writing - current index 0 here, the
    // prime's - must be refused, and refused without blocking: the caller
    // fails open on ERROR_BUSY and cannot afford a stall to learn that.
    xrfg::D3D12FrameSynthesisTicket collided{};
    const auto immediate_started = std::chrono::steady_clock::now();
    const HRESULT immediate_result = synthesizer.submit_pair(
        capture_b, views, views, 1, 0, &collided);
    const auto immediate_elapsed =
        std::chrono::steady_clock::now() - immediate_started;
    require(
        immediate_result == HRESULT_FROM_WIN32(ERROR_BUSY) &&
            collided.fence_value == 0 &&
            immediate_elapsed < std::chrono::milliseconds(50),
        "a pair reusing the prime's destination was not refused immediately");

    // A pair whose resources do not collide is admitted while the prime is
    // still outstanding, and its work queues behind the prime's on the GPU.
    // The synthesiser holds three work slots and alternating destinations so
    // that this is legal; there is deliberately no global gate that would
    // collapse it to one submission in flight.
    xrfg::D3D12FrameSynthesisTicket pair_ab{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b, views, views, 0, 1, &pair_ab)) &&
            pair_ab.previous_serial == capture_a.serial &&
            pair_ab.current_serial == capture_b.serial &&
            pair_ab.fence_value > prime.fence_value,
        "a non-colliding pair was refused while earlier work was outstanding");

    const auto blocked_started = std::chrono::steady_clock::now();
    const HRESULT blocked_result =
        synthesizer.wait_for_previous_submission(100);
    const auto blocked_elapsed = std::chrono::steady_clock::now() - blocked_started;
    require(
        blocked_result == HRESULT_FROM_WIN32(ERROR_BUSY) &&
            blocked_elapsed >= std::chrono::milliseconds(80),
        "the drain wait did not block on unfinished bridge work");

    require_hresult(
        gate->Signal(1),
        "ID3D12Fence::Signal(backpressure release)");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    require(
        operation_succeeded(synthesizer.wait_for_previous_submission(100)),
        "the drain wait did not recover after the queue advanced");
    const HRESULT drain_result = synthesizer.wait_for_idle();
    require(
        operation_succeeded(drain_result),
        "backpressure final drain failed with HRESULT " +
            std::to_string(static_cast<std::int32_t>(drain_result)));
    require(
        operation_succeeded(history->invalidate()),
        "backpressure history invalidate failed");
}

void test_nvidia_serialized_eye_context_stress(
    D3D12WarpFixture& fixture,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options) {
    constexpr UINT width = 1024;
    constexpr UINT height = 512;
    constexpr std::uint32_t pair_count = 1000;
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 3> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 3> synthetic_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(), sources[1].Get(), sources[2].Get()};
    std::array<ID3D12Resource*, 3> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get(),
        current_destinations[2].Get()};
    std::array<ID3D12Resource*, 3> synthetic_pointers{
        synthetic_destinations[0].Get(), synthetic_destinations[1].Get(),
        synthetic_destinations[2].Get()};
    for (UINT index = 0; index < sources.size(); ++index) {
        upload_pattern(
            fixture,
            sources[index].Get(),
            translated_motion_pattern(
                width,
                height,
                {static_cast<int>(index * 16),
                 -static_cast<int>(index * 16)}));
    }

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(), fixture.queue(), source_pointers,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "NVIDIA context stress history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(), fixture.queue(), history, current_pointers,
            synthetic_pointers, kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            xrfg::D3D12OpticalFlowBackend::nvidia, nvidia_options)),
        "NVIDIA context stress synthesizer initialization failed");

    const ReprojectionViews views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket first{};
    require(
        operation_succeeded(history->capture(0, &first)) &&
            operation_succeeded(history->commit(first)),
        "NVIDIA context stress prime capture failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(
            synthesizer.submit_prime(first, views, 0, &prime)),
        "NVIDIA context stress prime failed");

    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t pair = 1; pair <= pair_count; ++pair) {
        const std::uint32_t index = static_cast<std::uint32_t>(
            pair % sources.size());
        xrfg::D3D12HistoryCaptureTicket capture{};
        HRESULT capture_result = E_FAIL;
        const auto capture_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            capture_result = history->capture(index, &capture);
            if (capture_result == HRESULT_FROM_WIN32(ERROR_BUSY)) {
                Sleep(0);
            }
        } while (capture_result == HRESULT_FROM_WIN32(ERROR_BUSY) &&
                 std::chrono::steady_clock::now() < capture_deadline);
        require(
            operation_succeeded(capture_result) &&
                operation_succeeded(history->commit(capture)),
            "NVIDIA context stress capture failed at pair " +
                std::to_string(pair) + " HRESULT " +
                std::to_string(static_cast<std::int32_t>(capture_result)));
        xrfg::D3D12FrameSynthesisTicket output{};
        HRESULT submit_result = E_FAIL;
        const auto submit_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            submit_result = synthesizer.submit_pair(
                capture, views, views, index, index, &output);
            if (submit_result == HRESULT_FROM_WIN32(ERROR_BUSY)) {
                Sleep(0);
            }
        } while (submit_result == HRESULT_FROM_WIN32(ERROR_BUSY) &&
                 std::chrono::steady_clock::now() < submit_deadline);
        require(
            operation_succeeded(submit_result),
            "NVIDIA context stress submit failed at pair " +
                std::to_string(pair) + " HRESULT " +
                std::to_string(static_cast<std::int32_t>(submit_result)) +
                " serial " + std::to_string(capture.serial) + " slot " +
                std::to_string(capture.slot));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    require(
        operation_succeeded(synthesizer.wait_for_idle()) &&
            operation_succeeded(history->invalidate()),
        "NVIDIA context stress retirement failed");
    fixture.require_no_debug_errors();
    std::cout << "NVIDIA serialized eye-context stress passed "
              << pair_count << " pairs in " << elapsed.count() << " ms\n";
}

// Opt-in with XRFG_TEST_FG_BENCH: GPU time per stereo pair for each frame
// generation method, through the synthesizer the layer uses, at Galactic
// Racer's per-eye size with a turning head. The span covers synthesis and the
// real-frame copy. The second part times the game-side guide snapshots that
// the DLSS-vector and native methods add to every game frame.
void bench_frame_generation_methods() {
    D3D12WarpFixture fixture(true);
    const auto eye = bench_eye_size(2004, 2004);
    const UINT width = eye[0], height = eye[1];
    const UINT render_width = width * 2 / 3, render_height = height * 2 / 3;
    std::cout << "fg bench " << width << "x" << height << " per eye\n";
    constexpr UINT warmup = 8, measured = 48;
    constexpr int shift = 12;
    using Backend = xrfg::D3D12OpticalFlowBackend;
    using Preset = xrfg::D3D12NvidiaPerformancePreset;
    using Scale = xrfg::D3D12OpticalFlowInputScale;
    struct Method {
        const char* name;
        Backend backend;
        Preset preset;
        Scale scale;
        bool game_motion, native, triple;
    };
    const std::vector<Method> methods{
        {"OFXR FidelityFX, half-res flow", Backend::fidelity_fx, Preset::medium, Scale::half, false, false, false},
        {"OFXR FidelityFX, full-res flow", Backend::fidelity_fx, Preset::medium, Scale::full, false, false, false},
        {"OFXR NVIDIA OFA fast", Backend::nvidia, Preset::fast, Scale::half, false, false, false},
        {"OFXR NVIDIA OFA medium", Backend::nvidia, Preset::medium, Scale::half, false, false, false},
        {"OFXR NVIDIA OFA slow", Backend::nvidia, Preset::slow, Scale::half, false, false, false},
        {"OFXR FidelityFX + DLSS vectors", Backend::fidelity_fx, Preset::medium, Scale::half, true, false, false},
        {"OFXR NVIDIA + DLSS vectors", Backend::nvidia, Preset::medium, Scale::half, true, false, false},
        {"OFXR FidelityFX 3X", Backend::fidelity_fx, Preset::medium, Scale::half, false, false, true},
        {"OFXR NVIDIA OFA medium 3X", Backend::nvidia, Preset::medium, Scale::half, false, false, true},
#ifdef XRFG_NATIVE_DLSSG
        {"Native DLSS FG 2X", Backend::nvidia, Preset::medium, Scale::half, true, true, false},
        {"Native DLSS FG 3X", Backend::nvidia, Preset::medium, Scale::half, true, true, true},
#endif
    };
    const auto a = translated_motion_pattern(width, height, {0, 0});
    const auto b = translated_motion_pattern(width, height, {shift, -shift});
    auto motion = create_and_upload_game_motion(fixture, render_width, render_height,
        {-float(shift) * render_width / width, float(shift) * render_width / width});
    auto depth = create_native_test_depth(fixture, render_width, render_height);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto report = [](const char* name, std::vector<double> samples, const std::string& note) {
        std::sort(samples.begin(), samples.end());
        std::cout << "fg bench " << name << ": p10_us=" << samples[samples.size() / 10]
                  << " median_us=" << samples[samples.size() / 2] << note << '\n';
    };
    for (const auto& method : methods) {
        std::array<ComPtr<ID3D12Resource>, 2> sources{
            create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
        std::array<ComPtr<ID3D12Resource>, 2> currents{
            create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
        std::array<ComPtr<ID3D12Resource>, 2> synthetics{
            create_source_texture(fixture, width, height), create_source_texture(fixture, width, height)};
        upload_pattern(fixture, sources[0].Get(), a);
        upload_pattern(fixture, sources[1].Get(), b);
        std::array<ID3D12Resource*, 2> input{sources[0].Get(), sources[1].Get()};
        std::array<ID3D12Resource*, 2> current_out{currents[0].Get(), currents[1].Get()};
        std::vector<ID3D12Resource*> synthetic_out{synthetics[0].Get()};
        if (method.triple) synthetic_out.push_back(synthetics[1].Get());
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        require_hresult(history->initialize(fixture.device(), fixture.queue(), input,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET), "bench history");
        xrfg::D3D12NvidiaOpticalFlowOptions options;
        options.preset = method.preset;
        options.input_scale = method.scale;
        if (method.native) options.frame_generation = xrfg::D3D12FrameGeneration::native_dlss;
        xrfg::D3D12FrameSynthesizer synthesizer;
        require_hresult(synthesizer.initialize(fixture.device(), fixture.queue(), history,
                                               current_out, synthetic_out, kFormat,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, method.backend,
                                               options, true), "bench initialize");
        const auto guides = [&](std::uint64_t serial) -> std::shared_ptr<const xrfg::DlssMotionVectorSet> {
            if (!method.game_motion) return {};
            auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
            set->eye_count = kEyeCount;
            for (UINT eye = 0; eye < kEyeCount; ++eye) {
                auto frame = std::make_shared<xrfg::DlssMotionVectorFrame>();
                frame->stream = 71 + eye;
                frame->epoch = 1;
                frame->serial = serial;
                frame->previous_serial = serial - 1;
                frame->motion_vectors = motion;
                frame->producer_queue = fixture.queue();
                frame->output_width = width;
                frame->output_height = height;
                frame->motion_width = render_width;
                frame->motion_height = render_height;
                frame->motion_slice = frame->output_slice = eye;
                frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                if (method.native) {
                    frame->depth = depth;
                    frame->depth_width = render_width;
                    frame->depth_height = render_height;
                    frame->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
                }
                set->eyes[eye] = frame;
            }
            return set;
        };
        xrfg::D3D12HistoryCaptureTicket capture{};
        xrfg::D3D12FrameSynthesisTicket ticket{};
        require_hresult(history->capture(0, &capture), "bench capture");
        require_hresult(history->commit(capture), "bench commit");
        require_hresult(synthesizer.submit_prime(capture, make_reprojection_views(0.0F), 0, &ticket,
                                                 guides(1)), "bench prime");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
        const auto used_before = xrfg::dlss_motion_vector_statistics().used;
        std::vector<double> samples;
        std::array<std::vector<double>, 4> stages;  // NVIDIA flow: pack, eye 0, eye 1, composition
        for (UINT pair = 1; pair <= warmup + measured; ++pair) {
            require_frame_start_gate(synthesizer, "bench frame gate");
            require_hresult(history->capture(pair % 2, &capture), "bench capture");
            require_hresult(history->commit(capture), "bench commit");
            const auto views = make_reprojection_views(0.01F * float(pair));
            const auto extra = method.triple
                ? std::optional<xrfg::D3D12ExtraSynthetic>(xrfg::D3D12ExtraSynthetic{1, 2.0F / 3.0F})
                : std::nullopt;
            require_hresult(synthesizer.submit_pair(capture, views, views, 0, pair % 2, &ticket,
                                                    std::nullopt, guides(pair + 1), false,
                                                    method.triple ? 1.0F / 3.0F : 0.5F, extra),
                            "bench pair");
            fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
            xrfg::D3D12NvidiaGpuTiming timing{};
            require(synthesizer.consume_nvidia_gpu_timing(&timing) == S_OK,
                    std::string("bench timing missing for ") + method.name);
            const double microseconds = timing.gpu_end_qpc > timing.gpu_begin_qpc
                ? double(timing.gpu_end_qpc - timing.gpu_begin_qpc) * 1e6 / double(frequency.QuadPart)
                : double(timing.total_microseconds);
            if (pair > warmup) {
                samples.push_back(microseconds);
                const std::array<std::uint64_t, 4> stage{timing.pack_microseconds, timing.eye0_microseconds,
                                                         timing.eye1_microseconds,
                                                         timing.composition_microseconds};
                for (std::size_t i = 0; i < stage.size(); ++i) stages[i].push_back(double(stage[i]));
            }
        }
        require_hresult(synthesizer.wait_for_idle(), "bench drain");
        const auto used = xrfg::dlss_motion_vector_statistics().used - used_before;
        std::string note = method.game_motion ? " vector_pairs=" + std::to_string(used) + "/" +
                                                    std::to_string(warmup + measured)
                                              : std::string();
        if (method.backend == Backend::nvidia && !method.game_motion) {
            const char* names[]{" pack", " eye0", " eye1", " composition"};
            for (std::size_t i = 0; i < stages.size(); ++i) {
                std::sort(stages[i].begin(), stages[i].end());
                note += std::string(names[i]) + "_us=" + std::to_string(int(stages[i][stages[i].size() / 2]));
            }
        }
        report(method.name, samples, note);
    }

    // Game side: one DLSS evaluation per eye per game frame, each followed by
    // a snapshot of its motion and depth. Sizes are Galactic Racer's: both
    // eyes share 4012x3004 targets, each reading a 2004x2004 rectangle.
    constexpr UINT target_width = 4012, target_height = 3004;
    const auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                             D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC description = stereo_texture_description(target_width, target_height);
        description.DepthOrArraySize = 1;
        description.Format = format;
        description.Flags = flags;
        const auto properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> resource;
        require_hresult(fixture.device()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                            &description, state, nullptr, IID_PPV_ARGS(&resource)),
                        "bench game texture");
        return resource;
    };
    auto game_output = texture(kFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto game_motion = texture(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto game_depth = texture(DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = 2;
    require_hresult(fixture.device()->CreateQueryHeap(&query_description, IID_PPV_ARGS(&queries)),
                    "bench query heap");
    auto readback = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, 2 * sizeof(UINT64),
                                  D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 ticks{};
    require_hresult(fixture.queue()->GetTimestampFrequency(&ticks), "bench timestamp frequency");
    xrfg::configure_dlss_motion_vector_tracking(true);
    std::vector<double> snapshot_samples;
    for (UINT frame = 0; frame < warmup + measured; ++frame) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            for (UINT eye = 0; eye < kEyeCount; ++eye) {
                xrfg::DlssMotionVectorPublication publication{};
                publication.stream = 81 + eye;
                publication.output = game_output.Get();
                publication.motion_vectors = game_motion.Get();
                publication.depth = game_depth.Get();
                publication.producer_queue = fixture.queue();
                publication.producer_command_list = list;
                publication.verified_producer_device = fixture.device();
                publication.output_width = publication.motion_width = publication.depth_width = width;
                publication.output_height = publication.motion_height = publication.depth_height = height;
                publication.depth_inverted = true;
                publication.depth_infinite = true;
                xrfg::publish_dlss_motion_vectors(publication);
            }
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        });
        void* mapped{};
        const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
        require_hresult(readback->Map(0, &range, &mapped), "bench readback");
        const auto* stamps = static_cast<const UINT64*>(mapped);
        if (frame >= warmup) snapshot_samples.push_back(double(stamps[1] - stamps[0]) * 1e6 / double(ticks));
        const D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
    }
    xrfg::retire_dlss_motion_vector_stream(81);
    xrfg::retire_dlss_motion_vector_stream(82);
    xrfg::configure_dlss_motion_vector_tracking(false);
    report("game-side guide snapshots per frame (2 eyes, motion + depth)", snapshot_samples,
           " snapshot_failures=" +
               std::to_string(xrfg::dlss_motion_vector_statistics().snapshot_failures));
    fixture.require_no_debug_errors();
}

#include "nvidia_fast_patterns.inc"

}  // namespace

int main() {
    const bool trace_test = std::getenv("XRFG_TEST_GPU_TRACE") != nullptr;
    const auto trace_dir = std::filesystem::temp_directory_path() /
        ("xrfg-v073-gpu-" + std::to_string(GetCurrentProcessId()));
    if (trace_test) {
        std::filesystem::create_directories(trace_dir);
        std::ofstream(trace_dir / "ofxr_bridge.ini") <<
            "[diagnostics]\nlogging_enabled=1\nmax_file_mb=32\nflush_each_event=0\n";
        xrfg::bridge_flight_logger().initialize(trace_dir);
    }
    try {
        if (std::getenv("XRFG_TEST_FG_BENCH")) {
            bench_frame_generation_methods();
            return 0;
        }
#ifdef XRFG_NATIVE_DLSSG
        if (std::getenv("XRFG_TEST_NATIVE_DLSSG_ROTATION_SWEEP")) {
            D3D12WarpFixture bench_fixture(true);
            bench_native_dlss_rotation_sweep(bench_fixture);
            return 0;
        }
        if (std::getenv("XRFG_TEST_NATIVE_DLSSG_SCALE_QUALITY")) {
            D3D12WarpFixture bench_fixture(true);
            bench_native_dlss_scale_quality(bench_fixture);
            return 0;
        }
        if (std::getenv("XRFG_TEST_NATIVE_DLSSG_LAYOUT_BENCH")) {
            D3D12WarpFixture bench_fixture(true);
            bench_native_dlss_layouts(bench_fixture);
            return 0;
        }
        if (std::getenv("XRFG_TEST_NATIVE_DLSSG_BENCH")) {
            D3D12WarpFixture bench_fixture(true);
            bench_native_dlss_pairs(bench_fixture);
            return 0;
        }
        if (std::getenv("XRFG_TEST_NATIVE_DLSSG")) {
            D3D12WarpFixture native_fixture(true);
            test_native_dlss_depth_edges(native_fixture);
            test_native_dlss_packed_stereo(native_fixture, false);
            test_native_dlss_packed_stereo(native_fixture, true);
            test_native_dlss_packed_stereo(native_fixture, false, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            test_native_dlss_packed_stereo(native_fixture, true, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            test_native_dlss_reseeds_only_for_camera_motion(native_fixture);
            test_dlss_motion_vector_gpu_ingress(native_fixture,xrfg::D3D12OpticalFlowBackend::fidelity_fx,true);
            test_dlss_motion_vector_gpu_ingress(native_fixture,xrfg::D3D12OpticalFlowBackend::fidelity_fx,true,true);
            test_dlss_motion_vector_gpu_ingress(native_fixture,xrfg::D3D12OpticalFlowBackend::fidelity_fx,true,false,true);
            test_dlss_motion_vector_gpu_ingress(native_fixture,xrfg::D3D12OpticalFlowBackend::nvidia,true);
            test_dlss_motion_vector_gpu_ingress(native_fixture,xrfg::D3D12OpticalFlowBackend::fidelity_fx,true,false,false,true);
            test_dlss_motion_vector_gpu_ingress(native_fixture,xrfg::D3D12OpticalFlowBackend::nvidia,true,false,true,true);
            xrfg::D3D12NvidiaOpticalFlowOptions native_options;
            native_options.frame_generation=xrfg::D3D12FrameGeneration::native_dlss;
            test_rotation_aware_synthesis_beats_uncompensated_flow(native_fixture,
                xrfg::D3D12OpticalFlowBackend::fidelity_fx,native_options);
            native_fixture.require_no_debug_errors();
            std::cout << "Native DLSS FG stereo, real-copy, reset and missing-depth tests passed\n";
            return 0;
        }
#endif
        if (GetEnvironmentVariableA("XRFG_TEST_NVIDIA_QUALITY", nullptr, 0)) {
            run_nvidia_fast_controls();
            return 0;
        }
        D3D12WarpFixture fixture;
        test_stereo_capture_ring(fixture);
        test_capture_is_async_and_consumer_fence_blocks_reuse(fixture);
        test_depth_capture_path(fixture);
        test_depth_private_history_allows_shader_resource_views(fixture);
        test_rolling_frame_synthesizer(fixture);
        test_synthesis_on_a_dedicated_queue(fixture);
        test_double_wide_single_slice_views(fixture);
        test_stereo_motion_synthesis_beats_same_pixel_blend(fixture);
        test_dlss_motion_vector_stereo_stream_pairing(fixture);
        test_dlss_guide_snapshots_copy_only_the_read_region(fixture);
        test_dlss_motion_vector_gpu_ingress(fixture);
        test_dlss_motion_vector_side_by_side(fixture);
        test_dlss_motion_vector_strafe_rejects_double_edges(fixture);
        test_rotation_aware_synthesis_beats_uncompensated_flow(fixture);
        test_submission_backpressure_and_recovery(
            fixture,
            xrfg::D3D12OpticalFlowBackend::fidelity_fx);
        fixture.require_no_debug_errors();
        std::array<char, 8> nvidia_test{};
        const DWORD nvidia_test_length = GetEnvironmentVariableA(
            "XRFG_TEST_NVIDIA",
            nvidia_test.data(),
            static_cast<DWORD>(nvidia_test.size()));
        if (nvidia_test_length == 1 && nvidia_test[0] == '1') {
            xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options;
            std::array<char, 16> preset{};
            const DWORD preset_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_PRESET",
                preset.data(),
                static_cast<DWORD>(preset.size()));
            if (preset_length == 4 && _stricmp(preset.data(), "slow") == 0) {
                nvidia_options.preset =
                    xrfg::D3D12NvidiaPerformancePreset::slow;
            } else if (preset_length == 4 && _stricmp(preset.data(), "fast") == 0) {
                nvidia_options.preset =
                    xrfg::D3D12NvidiaPerformancePreset::fast;
            }
            std::array<char, 8> input_scale{};
            const DWORD input_scale_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_INPUT_SCALE",
                input_scale.data(),
                static_cast<DWORD>(input_scale.size()));
            if (input_scale_length == 3 &&
                std::string_view(input_scale.data(), input_scale_length) == "100") {
                nvidia_options.input_scale = xrfg::D3D12NvidiaInputScale::full;
            } else if (input_scale_length == 2 &&
                std::string_view(input_scale.data(), input_scale_length) ==
                    "75") {
                nvidia_options.input_scale =
                    xrfg::D3D12NvidiaInputScale::three_quarter;
            } else if (input_scale_length == 2 &&
                       std::string_view(
                           input_scale.data(), input_scale_length) == "50") {
                nvidia_options.input_scale =
                    xrfg::D3D12NvidiaInputScale::half;
            }
            std::array<char, 8> bidirectional{};
            const DWORD bidirectional_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_BIDIRECTIONAL",
                bidirectional.data(),
                static_cast<DWORD>(bidirectional.size()));
            nvidia_options.bidirectional = bidirectional_length == 1 &&
                bidirectional[0] == '1';
            D3D12WarpFixture nvidia_fixture(true);
            test_stereo_motion_synthesis_beats_same_pixel_blend(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                true,
                nvidia_options);
            test_double_wide_single_slice_views(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                nvidia_options);
            test_rotation_aware_synthesis_beats_uncompensated_flow(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                nvidia_options);
            test_dlss_motion_vector_gpu_ingress(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia);
            test_dlss_motion_vector_side_by_side(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia);
            test_submission_backpressure_and_recovery(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                nvidia_options);
            if (GetEnvironmentVariableA(
                    "XRFG_TEST_NVIDIA_CONTEXT_STRESS", nullptr, 0)) {
                test_nvidia_serialized_eye_context_stress(
                    nvidia_fixture,
                    nvidia_options);
            }
            nvidia_fixture.require_no_debug_errors();
            std::cout << "D3D12 NVIDIA OFA synthesis test passed\n";
        }
        std::array<char, 8> nvidia_cropped_test{};
        const DWORD nvidia_cropped_test_length = GetEnvironmentVariableA(
            "XRFG_TEST_NVIDIA_CROPPED",
            nvidia_cropped_test.data(),
            static_cast<DWORD>(nvidia_cropped_test.size()));
        if (nvidia_cropped_test_length == 1 &&
            nvidia_cropped_test[0] == '1') {
            D3D12WarpFixture nvidia_cropped_fixture(true);
            test_rotation_aware_synthesis_beats_uncompensated_flow(
                nvidia_cropped_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia);
            nvidia_cropped_fixture.require_no_debug_errors();
            std::cout << "D3D12 NVIDIA OFA cropped synthesis test passed\n";
        }
        std::array<char, 8> hardware_repeat_test{};
        const DWORD hardware_repeat_test_length = GetEnvironmentVariableA(
            "XRFG_TEST_HARDWARE_REPEAT",
            hardware_repeat_test.data(),
            static_cast<DWORD>(hardware_repeat_test.size()));
        if (hardware_repeat_test_length == 1 &&
            hardware_repeat_test[0] == '1') {
            D3D12WarpFixture hardware_repeat_fixture(true);
            test_stereo_motion_synthesis_beats_same_pixel_blend(
                hardware_repeat_fixture,
                xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                true);
            hardware_repeat_fixture.require_no_debug_errors();
            std::cout << "D3D12 hardware repeated-capture test passed\n";
        }
        std::cout << "D3D12 WARP asynchronous history/synthesis tests passed\n";
        if (trace_test) {
            const auto path = xrfg::bridge_flight_logger().log_path();
            std::ifstream input(path);
            const std::string contents{std::istreambuf_iterator<char>(input), {}};
            require(
                contents.find(
                    "op=synthesis_frame_start_wait result=-2147024726") !=
                    std::string::npos,
                "transient frame-start synthesis wait breadcrumb missing");
            xrfg::bridge_flight_logger().shutdown();
            input.close();
            std::filesystem::remove_all(trace_dir);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 WARP asynchronous history/synthesis test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
