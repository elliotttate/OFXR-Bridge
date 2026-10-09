#include "xrfg/fg_benchmark.hpp"

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xrfg::fg_benchmark {
namespace {

using Microsoft::WRL::ComPtr;
using benchmark::CaseKind;
using benchmark::CaseResult;
using benchmark::CaseSpec;
using benchmark::CaseStatus;

constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT kEyes = 2;
// How far the scene moves between the two frames, in pixels.
constexpr int kShift = 12;

// Ends the whole run: the device is gone, hung or out of memory.
struct Fatal {
    HRESULT result;
    std::wstring message;
};
// Ends one case, which then reports failed.
struct CaseError {
    std::string message;
};

[[nodiscard]] std::string hresult_text(HRESULT result) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(result));
    return buffer;
}

void require(HRESULT result, const char* what) {
    if (FAILED(result)) throw CaseError{std::string(what) + " failed (" + hresult_text(result) + ")"};
}

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
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

// One device and queue, with a reusable list and a fence to wait on.
class Gpu {
public:
    explicit Gpu(std::uint32_t timeout_ms) : timeout_ms_(timeout_ms) {}
    ~Gpu() {
        if (event_ != nullptr) CloseHandle(event_);
    }
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    void create(bool warp, AdapterInfo* info) {
        HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.GetAddressOf()));
        if (FAILED(result)) throw Fatal{result, L"DXGI is not available."};
        if (warp) {
            result = factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter_.GetAddressOf()));
        } else {
            result = DXGI_ERROR_NOT_FOUND;
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory_->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                        IID_PPV_ARGS(candidate.GetAddressOf())) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(candidate->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
                adapter_ = candidate;
                result = S_OK;
                break;
            }
        }
        if (FAILED(result) || !adapter_) throw Fatal{result, L"No graphics card was found."};
        DXGI_ADAPTER_DESC1 description{};
        adapter_->GetDesc1(&description);
        info->name = description.Description;
        info->vendor_id = description.VendorId;
        info->device_id = description.DeviceId;
        info->dedicated_video_memory = description.DedicatedVideoMemory;
        info->software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter_->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
            info->driver_version = static_cast<std::uint64_t>(version.QuadPart);
        }
        result = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(device_.GetAddressOf()));
        if (FAILED(result)) throw Fatal{result, L"Direct3D 12 could not start on " + info->name + L"."};
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(result = device_->CreateCommandQueue(&queue_description,
                       IID_PPV_ARGS(queue_.GetAddressOf()))) ||
            FAILED(result = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                       IID_PPV_ARGS(fence_.GetAddressOf()))) ||
            FAILED(result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                       IID_PPV_ARGS(allocator_.GetAddressOf()))) ||
            FAILED(result = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                       allocator_.Get(), nullptr, IID_PPV_ARGS(list_.GetAddressOf()))) ||
            FAILED(result = list_->Close())) {
            throw Fatal{result, L"Direct3D 12 could not create its queue."};
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_ == nullptr) throw Fatal{E_OUTOFMEMORY, L"CreateEvent failed."};
    }

    [[nodiscard]] ID3D12Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept { return queue_.Get(); }

    template <typename Recorder>
    void execute(Recorder&& record) {
        HRESULT result = allocator_->Reset();
        if (SUCCEEDED(result)) result = list_->Reset(allocator_.Get(), nullptr);
        if (FAILED(result)) throw Fatal{result, L"A command list could not be reset."};
        record(list_.Get());
        if (FAILED(result = list_->Close())) {
            throw Fatal{result, L"A command list could not be closed."};
        }
        ID3D12CommandList* lists[]{list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        flush();
    }

    // Waits for everything queued so far.
    void flush() {
        const std::uint64_t value = ++fence_value_;
        HRESULT result = queue_->Signal(fence_.Get(), value);
        if (FAILED(result)) throw Fatal{result, L"The GPU queue could not be signalled."};
        if (fence_->GetCompletedValue() < value) {
            if (FAILED(result = fence_->SetEventOnCompletion(value, event_))) {
                throw Fatal{result, L"The GPU fence could not be waited on."};
            }
            if (WaitForSingleObject(event_, timeout_ms_) != WAIT_OBJECT_0) {
                throw Fatal{HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                            L"The graphics card stopped responding during the benchmark."};
            }
        }
        check_device();
    }

    void check_device() const {
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            wchar_t message[128]{};
            std::swprintf(message, std::size(message),
                          L"The graphics driver reset the GPU (device removed, 0x%08X).",
                          static_cast<unsigned>(removed));
            throw Fatal{removed, message};
        }
    }

private:
    std::uint32_t timeout_ms_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    HANDLE event_{};
    std::uint64_t fence_value_{};
};

[[nodiscard]] ComPtr<ID3D12Resource> create_texture(Gpu& gpu, UINT width, UINT height, UINT16 slices,
                                                    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                                    D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = slices;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    const auto properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    const HRESULT result = gpu.device()->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
        IID_PPV_ARGS(texture.GetAddressOf()));
    if (FAILED(result)) {
        throw Fatal{result, L"Not enough video memory for this resolution; close other "
                            L"programs or pick a smaller one."};
    }
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_buffer(Gpu& gpu, D3D12_HEAP_TYPE type, UINT64 size,
                                                   D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto properties = heap_properties(type);
    ComPtr<ID3D12Resource> buffer;
    const HRESULT result = gpu.device()->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(result)) throw Fatal{result, L"Not enough memory for the benchmark's upload."};
    return buffer;
}

// Fills each slice of `texture` through an upload buffer. `fill` gets the
// slice, a row pointer and the row's y.
template <typename Fill>
void upload(Gpu& gpu, ID3D12Resource* texture, D3D12_RESOURCE_STATES before,
            D3D12_RESOURCE_STATES after, Fill&& fill) {
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    const UINT slices = description.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(slices);
    std::vector<UINT> rows(slices);
    std::vector<UINT64> row_sizes(slices);
    UINT64 total = 0;
    gpu.device()->GetCopyableFootprints(&description, 0, slices, 0, layouts.data(), rows.data(),
                                        row_sizes.data(), &total);
    auto staging = create_buffer(gpu, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(staging->Map(0, &no_read, &mapped))) {
        throw Fatal{E_FAIL, L"The benchmark's upload could not be mapped."};
    }
    auto* bytes = static_cast<std::byte*>(mapped);
    for (UINT slice = 0; slice < slices; ++slice) {
        for (UINT y = 0; y < rows[slice]; ++y) {
            fill(slice, bytes + layouts[slice].Offset +
                            static_cast<std::size_t>(y) * layouts[slice].Footprint.RowPitch, y);
        }
    }
    staging->Unmap(0, nullptr);
    gpu.execute([&](ID3D12GraphicsCommandList* list) {
        if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
            const auto barrier = transition(texture, before, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &barrier);
        }
        for (UINT slice = 0; slice < slices; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = staging.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[slice];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = slice;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const auto barrier = transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
        list->ResourceBarrier(1, &barrier);
    });
}

// The frame-generation bench's scene: smooth detail in every channel, so
// neither the flow nor GPU memory compression gets an easy frame, moved by
// `translation` pixels per eye with black where nothing moves in.
void upload_scene(Gpu& gpu, ID3D12Resource* texture, UINT width, UINT height,
                  const std::array<int, kEyes>& translation) {
    // The texel at (x, y) is a sum of per-axis waves; tabulate each term.
    std::array<std::vector<float>, kEyes> red_x, green_xy, blue_x;
    std::vector<float> red_y(height), green_x_minus_y(static_cast<std::size_t>(width) + height),
        blue_y(height);
    for (UINT eye = 0; eye < kEyes; ++eye) {
        const float phase = static_cast<float>(eye) * 0.7F;
        red_x[eye].resize(width);
        blue_x[eye].resize(width);
        green_xy[eye].resize(static_cast<std::size_t>(width) + height);
        for (UINT x = 0; x < width; ++x) {
            red_x[eye][x] = 82.0F * std::sin(static_cast<float>(x) * 0.061F + phase);
            blue_x[eye][x] = 69.0F * std::cos(static_cast<float>(x) * 0.041F - phase);
        }
        for (UINT s = 0; s < width + height; ++s) {
            green_xy[eye][s] = 76.0F * std::sin(static_cast<float>(s) * 0.047F + phase);
        }
    }
    for (UINT y = 0; y < height; ++y) {
        red_y[y] = 31.0F * std::cos(static_cast<float>(y) * 0.093F);
        blue_y[y] = 41.0F * std::sin(static_cast<float>(y) * 0.071F);
    }
    for (UINT d = 0; d < width + height; ++d) {
        green_x_minus_y[d] = 34.0F * std::cos((static_cast<float>(d) - static_cast<float>(height)) * 0.037F);
    }
    const auto channel = [](float value) noexcept {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
    };
    upload(gpu, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
           [&](UINT eye, std::byte* row, UINT y) {
               auto* texels = reinterpret_cast<std::uint8_t*>(row);
               for (UINT x = 0; x < width; ++x) {
                   const int source = static_cast<int>(x) - translation[eye];
                   std::uint8_t* texel = texels + static_cast<std::size_t>(x) * 4;
                   if (source < 0 || source >= static_cast<int>(width)) {
                       texel[0] = texel[1] = texel[2] = 0;
                       texel[3] = 255;
                       continue;
                   }
                   const auto sx = static_cast<UINT>(source);
                   texel[0] = channel(128.0F + red_x[eye][sx] + red_y[y]);
                   texel[1] = channel(126.0F + green_xy[eye][sx + y] +
                                      green_x_minus_y[sx + height - y]);
                   texel[2] = channel(124.0F + blue_x[eye][sx] + blue_y[y]);
                   texel[3] = 255;
               }
           });
}

// The game's vectors for the scene's motion, except in the bottom fifth,
// which moves with no vector at all, as shadows, reflections and particles
// do in games. Exact vectors everywhere would be the vector methods' best
// case; the bench's own, which disagree with the frames almost everywhere,
// their worst.
[[nodiscard]] ComPtr<ID3D12Resource> create_motion(Gpu& gpu, UINT width, UINT height,
                                                   const std::array<float, kEyes>& motion) {
    auto texture = create_texture(gpu, width, height, kEyes, DXGI_FORMAT_R32G32_FLOAT,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT vectored_rows = height - height / 5;
    upload(gpu, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, [&](UINT eye, std::byte* row, UINT y) {
               auto* values = reinterpret_cast<float*>(row);
               const float horizontal = y < vectored_rows ? motion[eye] : 0.0F;
               for (UINT x = 0; x < width; ++x) {
                   values[x * 2] = horizontal;
                   values[x * 2 + 1] = 0.0F;
               }
           });
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_depth(Gpu& gpu, UINT width, UINT height) {
    auto texture = create_texture(gpu, width, height, kEyes, DXGI_FORMAT_R32_FLOAT,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload(gpu, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON,
           [&](UINT, std::byte* row, UINT) {
               auto* values = reinterpret_cast<float*>(row);
               std::fill_n(values, width, 0.5F);
           });
    return texture;
}

// A headset's two views turned by `yaw` radians, as the tests make them.
[[nodiscard]] std::array<D3D12ReprojectionView, kEyes> make_views(float yaw) {
    std::array<D3D12ReprojectionView, kEyes> views{};
    const float half = yaw * 0.5F;
    for (UINT eye = 0; eye < kEyes; ++eye) {
        views[eye].pose.orientation.y = std::sin(half);
        views[eye].pose.orientation.w = std::cos(half);
        views[eye].pose.position.x = eye == 0 ? -0.032F : 0.032F;
        views[eye].fov.angle_left = eye == 0 ? -0.7853981633974483F : -0.7504915783575616F;
        views[eye].fov.angle_right = eye == 0 ? 0.7504915783575616F : 0.7853981633974483F;
        views[eye].fov.angle_up = eye == 0 ? 0.6108652381980153F : 0.5934119456780721F;
        views[eye].fov.angle_down = eye == 0 ? -0.5934119456780721F : -0.6108652381980153F;
    }
    return views;
}

// Everything the cases share: the two frames, the outputs, and the game's
// guides for each direction of motion.
struct Scene {
    UINT width{}, height{}, render_width{}, render_height{};
    std::array<ComPtr<ID3D12Resource>, 2> sources, currents, synthetics;
    // Motion into frame 1 (from frame 0) and into frame 0 (from frame 1):
    // DLSS vectors point from a pixel back to where it was.
    std::array<ComPtr<ID3D12Resource>, 2> motion;
    ComPtr<ID3D12Resource> depth;
};

void create_scene(Gpu& gpu, Scene& scene, UINT width, UINT height) {
    scene.width = width;
    scene.height = height;
    scene.render_width = std::max(width * 2 / 3, 1U);
    scene.render_height = std::max(height * 2 / 3, 1U);
    for (auto* set : {&scene.sources, &scene.currents, &scene.synthetics}) {
        for (auto& texture : *set) {
            texture = create_texture(gpu, width, height, kEyes, kFormat,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }
    upload_scene(gpu, scene.sources[0].Get(), width, height, {0, 0});
    upload_scene(gpu, scene.sources[1].Get(), width, height, {kShift, -kShift});
    const float render_shift = static_cast<float>(kShift) * static_cast<float>(scene.render_width) /
        static_cast<float>(width);
    scene.motion[1] = create_motion(gpu, scene.render_width, scene.render_height,
                                    {-render_shift, render_shift});
    scene.motion[0] = create_motion(gpu, scene.render_width, scene.render_height,
                                    {render_shift, -render_shift});
    scene.depth = create_depth(gpu, scene.render_width, scene.render_height);
}

[[nodiscard]] CaseResult unavailable(const CaseSpec& spec, std::string note) {
    CaseResult result;
    result.key = std::string(spec.key);
    result.status = CaseStatus::unavailable;
    result.note = std::move(note);
    return result;
}

[[nodiscard]] CaseResult failed(const CaseSpec& spec, std::string note) {
    CaseResult result;
    result.key = std::string(spec.key);
    result.status = CaseStatus::failed;
    result.note = std::move(note);
    return result;
}

[[nodiscard]] CaseResult measured(const CaseSpec& spec, std::vector<double> samples) {
    CaseResult result;
    result.key = std::string(spec.key);
    std::sort(samples.begin(), samples.end());
    result.status = CaseStatus::ok;
    result.samples = static_cast<std::uint32_t>(samples.size());
    result.median_us = samples[samples.size() / 2];
    result.p10_us = samples[samples.size() / 10];
    return result;
}

struct Context {
    Gpu& gpu;
    Scene& scene;
    const Options& options;
    const AdapterInfo& adapter;
    // Frames native generation makes per pair on this adapter, asked once.
    std::optional<std::uint32_t> native_frames;
    std::string native_reason;
};

// Why native generation cannot run, or nothing when it can make `frames`.
[[nodiscard]] std::optional<std::string> native_unavailable(Context& context, int frames) {
    if (!context.native_frames) {
#ifndef XRFG_NATIVE_DLSSG
        context.native_frames = 0;
        context.native_reason = "This build of OFXR Bridge has no DLSS Frame Generation.";
#else
        std::error_code ignored;
        if (context.adapter.vendor_id != kNvidiaVendorId) {
            context.native_frames = 0;
            context.native_reason = "DLSS Frame Generation needs an NVIDIA GeForce RTX 40 or 50 series card.";
        } else if (!std::filesystem::exists(context.options.module_directory / L"nvngx_dlssg.dll", ignored)) {
            context.native_frames = 0;
            context.native_reason = "nvngx_dlssg.dll is missing from the ofxr folder.";
        } else {
            context.native_frames = native_dlssg_max_generated_frames(context.gpu.device());
            if (*context.native_frames == 0) {
                context.native_reason = "This GPU or driver cannot run DLSS Frame Generation "
                                        "(it needs an RTX 40 or 50 series card and a current driver).";
            }
        }
#endif
    }
    if (*context.native_frames == 0) return context.native_reason;
    if (static_cast<std::uint32_t>(frames) > *context.native_frames) {
        return std::string("3X needs a GPU that generates two frames at a time (RTX 50 series).");
    }
    return std::nullopt;
}

[[nodiscard]] CaseResult run_synthesizer_case(Context& context, const CaseSpec& spec) {
    Gpu& gpu = context.gpu;
    Scene& scene = context.scene;
    const bool nvidia_flow = spec.kind == CaseKind::flow && spec.backend == benchmark::CaseBackend::nvidia;
    if (nvidia_flow && context.adapter.vendor_id != kNvidiaVendorId) {
        return unavailable(spec, "NVIDIA optical flow needs an NVIDIA GeForce RTX card.");
    }
    if (spec.kind == CaseKind::native) {
        if (const auto reason = native_unavailable(context, spec.generated_frames)) {
            return unavailable(spec, *reason);
        }
    }
    const bool triple = spec.generated_frames >= 2;
    const bool game_motion = spec.kind != CaseKind::flow;

    auto history = std::make_shared<D3D12SwapchainHistory>();
    const std::array<ID3D12Resource*, 2> sources{scene.sources[0].Get(), scene.sources[1].Get()};
    const std::array<ID3D12Resource*, 2> currents{scene.currents[0].Get(), scene.currents[1].Get()};
    const std::array<ID3D12Resource*, 2> synthetics{scene.synthetics[0].Get(), scene.synthetics[1].Get()};
    require(history->initialize(gpu.device(), gpu.queue(), sources, D3D12_RESOURCE_STATE_RENDER_TARGET),
            "The frame history");

    D3D12NvidiaOpticalFlowOptions options;
    options.preset = spec.preset == benchmark::CasePreset::fast ? D3D12NvidiaPerformancePreset::fast
        : spec.preset == benchmark::CasePreset::slow ? D3D12NvidiaPerformancePreset::slow
        : D3D12NvidiaPerformancePreset::medium;
    options.input_scale = spec.input_scale >= 100 ? D3D12OpticalFlowInputScale::full
        : spec.input_scale >= 75 ? D3D12OpticalFlowInputScale::three_quarter
        : D3D12OpticalFlowInputScale::half;
    options.bidirectional = spec.bidirectional;
    options.hybrid = spec.kind == CaseKind::hybrid;
    options.extrapolate = spec.kind == CaseKind::extrapolate;
    if (spec.kind == CaseKind::native) {
        options.frame_generation = D3D12FrameGeneration::native_dlss;
        options.native_scale = static_cast<std::uint32_t>(spec.native_scale);
    }
    const auto backend = nvidia_flow ? D3D12OpticalFlowBackend::nvidia : D3D12OpticalFlowBackend::fidelity_fx;
    D3D12FrameSynthesizer synthesizer;
    const HRESULT initialized = synthesizer.initialize(
        gpu.device(), gpu.queue(), history, currents,
        std::span<ID3D12Resource* const>(synthetics.data(), triple ? 2U : 1U), kFormat,
        D3D12_RESOURCE_STATE_RENDER_TARGET, backend, options, true);
    if (FAILED(initialized)) {
        gpu.check_device();
        if (nvidia_flow) {
            return unavailable(spec, "NVIDIA optical flow could not start (" + hresult_text(initialized) +
                                         "); it needs an RTX 20 series or newer card and a current driver.");
        }
        if (spec.kind == CaseKind::native) {
            return unavailable(spec, "DLSS Frame Generation could not start (" +
                                         hresult_text(initialized) + ").");
        }
        return failed(spec, "The synthesizer could not start (" + hresult_text(initialized) + ").");
    }

    const float frame_ms = static_cast<float>(2000.0 / std::max(context.options.refresh_hz, 1.0));
    // The guides of the frame captured from `source`: its motion back to the other one.
    const auto guides = [&](std::uint64_t serial, UINT source) -> std::shared_ptr<const DlssMotionVectorSet> {
        if (!game_motion) return {};
        auto set = std::make_shared<DlssMotionVectorSet>();
        set->eye_count = kEyes;
        for (UINT eye = 0; eye < kEyes; ++eye) {
            auto frame = std::make_shared<DlssMotionVectorFrame>();
            frame->stream = 71 + eye;
            frame->epoch = 1;
            frame->serial = serial;
            frame->previous_serial = serial - 1;
            frame->motion_vectors = scene.motion[source];
            frame->producer_queue = gpu.queue();
            frame->output_width = scene.width;
            frame->output_height = scene.height;
            frame->motion_width = scene.render_width;
            frame->motion_height = scene.render_height;
            frame->motion_slice = frame->output_slice = eye;
            frame->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            frame->depth = scene.depth;
            frame->depth_width = scene.render_width;
            frame->depth_height = scene.render_height;
            frame->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
            frame->depth_inverted = true;
            frame->frame_time_delta_ms = frame_ms;
            set->eyes[eye] = frame;
        }
        return set;
    };
    const auto capture = [&](UINT source, D3D12HistoryCaptureTicket* ticket) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        HRESULT result = S_OK;
        do {
            result = history->capture(source, ticket);
            if (result != HRESULT_FROM_WIN32(ERROR_BUSY)) break;
            Sleep(1);
        } while (std::chrono::steady_clock::now() < deadline);
        require(result, "Capturing a frame");
        require(history->commit(*ticket), "Committing a frame");
    };

    D3D12HistoryCaptureTicket ticket{};
    D3D12FrameSynthesisTicket synthesis{};
    capture(0, &ticket);
    const auto first_views = make_views(0.0F);
    require(synthesizer.submit_prime(ticket, first_views, 0, &synthesis, guides(1, 0)), "The first frame");
    gpu.flush();

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const std::uint64_t used_before = dlss_motion_vector_statistics().used;
    const UINT warmup = context.options.warmup_pairs;
    const UINT total = warmup + std::max(context.options.measured_pairs, 4U);
    // The first frame after the prime extrapolates past; interpolation sits
    // halfway, or at thirds for 3X.
    const float fraction = spec.kind == CaseKind::extrapolate ? 1.5F : triple ? 1.0F / 3.0F : 0.5F;
    std::vector<double> samples;
    for (UINT pair = 1; pair <= total; ++pair) {
        HRESULT gate = synthesizer.wait_for_previous_submission(2000);
        if (FAILED(gate)) {
            gpu.check_device();
            require(gate, "Waiting for the previous pair");
        }
        const UINT source = pair % 2;
        capture(source, &ticket);
        const auto views = make_views(0.01F * static_cast<float>(pair));
        const auto extra = triple
            ? std::optional<D3D12ExtraSynthetic>(D3D12ExtraSynthetic{1, 2.0F / 3.0F})
            : std::nullopt;
        HRESULT submitted = E_FAIL;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            submitted = synthesizer.submit_pair(ticket, views, views, 0, source, &synthesis,
                                                std::nullopt, guides(pair + 1, source), false,
                                                fraction, extra);
            if (submitted != HRESULT_FROM_WIN32(ERROR_BUSY)) break;
            Sleep(1);
        } while (std::chrono::steady_clock::now() < deadline);
        if (FAILED(submitted)) {
            gpu.check_device();
            require(submitted, "Generating a frame");
        }
        gpu.flush();
        D3D12NvidiaGpuTiming timing{};
        if (synthesizer.consume_nvidia_gpu_timing(&timing) == S_OK && pair > warmup) {
            const double microseconds = timing.gpu_end_qpc > timing.gpu_begin_qpc && frequency.QuadPart > 0
                ? static_cast<double>(timing.gpu_end_qpc - timing.gpu_begin_qpc) * 1e6 /
                      static_cast<double>(frequency.QuadPart)
                : static_cast<double>(timing.total_microseconds);
            if (microseconds > 0.0) samples.push_back(microseconds);
        }
    }
    require(synthesizer.wait_for_idle(), "Finishing the case");
    const std::uint64_t used = dlss_motion_vector_statistics().used - used_before;
    if (game_motion && used == 0) {
        if (spec.kind == CaseKind::native) {
            return unavailable(spec, "DLSS Frame Generation generated no frames on this PC.");
        }
        return failed(spec, "The game's motion vectors were not used.");
    }
    if (samples.size() < std::max<std::size_t>(context.options.measured_pairs / 2, 2)) {
        if (spec.kind == CaseKind::native) {
            return unavailable(spec, "DLSS Frame Generation skipped most frames on this PC.");
        }
        return failed(spec, "The GPU reported no timings.");
    }
    return measured(spec, std::move(samples));
}

// The game side of the DLSS-vector and native methods: every game frame,
// each eye's motion and depth are copied once DLSS has read them.
[[nodiscard]] CaseResult run_guide_snapshot_case(Context& context, const CaseSpec& spec) {
    Gpu& gpu = context.gpu;
    const Scene& scene = context.scene;
    const auto output = create_texture(gpu, scene.width, scene.height, 1, kFormat,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto motion = create_texture(gpu, scene.render_width, scene.render_height, 1,
                                       DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto depth = create_texture(gpu, scene.render_width, scene.render_height, 1,
                                      DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = 2;
    require(gpu.device()->CreateQueryHeap(&query_description, IID_PPV_ARGS(queries.GetAddressOf())),
            "The timestamp queries");
    auto readback = create_buffer(gpu, D3D12_HEAP_TYPE_READBACK, 2 * sizeof(UINT64),
                                  D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 ticks = 0;
    require(gpu.queue()->GetTimestampFrequency(&ticks), "The timestamp frequency");
    if (ticks == 0) return failed(spec, "The GPU reported no timestamp frequency.");
    configure_dlss_motion_vector_tracking(true);
    struct Untrack {
        ~Untrack() {
            retire_dlss_motion_vector_stream(81);
            retire_dlss_motion_vector_stream(82);
            configure_dlss_motion_vector_tracking(false);
        }
    } untrack;
    std::vector<double> samples;
    const UINT warmup = context.options.warmup_pairs;
    const UINT total = warmup + std::max(context.options.measured_pairs, 4U);
    for (UINT frame = 0; frame < total; ++frame) {
        gpu.execute([&](ID3D12GraphicsCommandList* list) {
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            for (UINT eye = 0; eye < kEyes; ++eye) {
                DlssMotionVectorPublication publication{};
                publication.stream = 81 + eye;
                publication.output = output.Get();
                publication.motion_vectors = motion.Get();
                publication.depth = depth.Get();
                publication.producer_queue = gpu.queue();
                publication.producer_command_list = list;
                publication.verified_producer_device = gpu.device();
                publication.output_width = scene.width;
                publication.output_height = scene.height;
                publication.motion_width = publication.depth_width = scene.render_width;
                publication.motion_height = publication.depth_height = scene.render_height;
                publication.depth_inverted = true;
                publication.depth_infinite = true;
                publish_dlss_motion_vectors(publication);
            }
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
        });
        void* mapped = nullptr;
        const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
        if (FAILED(readback->Map(0, &range, &mapped))) return failed(spec, "Reading the timestamps failed.");
        const auto* stamps = static_cast<const UINT64*>(mapped);
        if (frame >= warmup && stamps[1] > stamps[0]) {
            samples.push_back(static_cast<double>(stamps[1] - stamps[0]) * 1e6 / static_cast<double>(ticks));
        }
        const D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
    }
    if (dlss_motion_vector_statistics().snapshot_copies == 0 || samples.size() < 2) {
        return failed(spec, "The guide copies did not run.");
    }
    return measured(spec, std::move(samples));
}

[[nodiscard]] CaseResult run_case(Context& context, const CaseSpec& spec) {
    try {
        if (spec.kind == CaseKind::guide_snapshot) return run_guide_snapshot_case(context, spec);
        return run_synthesizer_case(context, spec);
    } catch (const CaseError& error) {
        context.gpu.check_device();
        return failed(spec, error.message);
    }
}

} // namespace

std::string driver_version_text(std::uint64_t version) {
    char buffer[48]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
                  static_cast<unsigned>((version >> 48) & 0xFFFF), static_cast<unsigned>((version >> 32) & 0xFFFF),
                  static_cast<unsigned>((version >> 16) & 0xFFFF), static_cast<unsigned>(version & 0xFFFF));
    return buffer;
}

std::string nvidia_driver_text(std::uint32_t vendor_id, std::uint64_t version) {
    if (vendor_id != kNvidiaVendorId || version == 0) return {};
    // 32.0.15.8180 is 581.80: the last digit of the third part and the fourth.
    const unsigned number = static_cast<unsigned>(((version >> 16) & 0xFFFF) % 10) * 10000U +
                            static_cast<unsigned>(version & 0xFFFF);
    char buffer[24]{};
    std::snprintf(buffer, sizeof(buffer), "%u.%02u", number / 100, number % 100);
    return buffer;
}

HRESULT high_performance_adapter(AdapterInfo* info) noexcept {
    if (info == nullptr) return E_POINTER;
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) return result;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     IID_PPV_ARGS(adapter.GetAddressOf()));
        if (FAILED(result)) return result;
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        *info = {};
        info->name = description.Description;
        info->vendor_id = description.VendorId;
        info->device_id = description.DeviceId;
        info->dedicated_video_memory = description.DedicatedVideoMemory;
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
            info->driver_version = static_cast<std::uint64_t>(version.QuadPart);
        }
        return S_OK;
    }
}

HRESULT run(const Options& options, const Callbacks& callbacks, std::wstring* error) noexcept {
    try {
        if (options.eye_width < 64 || options.eye_height < 64 || options.eye_width > 8192 ||
            options.eye_height > 8192) {
            if (error) *error = L"The per-eye resolution must be between 64 and 8192 pixels.";
            return E_INVALIDARG;
        }
        std::vector<const CaseSpec*> cases;
        if (options.keys.empty()) {
            for (const auto& spec : benchmark::standard_cases()) cases.push_back(&spec);
        } else {
            for (const auto& key : options.keys) {
                const CaseSpec* spec = benchmark::find_case(key);
                if (spec == nullptr) {
                    if (error) *error = L"Unknown benchmark case: " + std::wstring(key.begin(), key.end());
                    return E_INVALIDARG;
                }
                cases.push_back(spec);
            }
        }
        Gpu gpu(options.gpu_timeout_ms);
        AdapterInfo adapter;
        gpu.create(options.use_warp, &adapter);
        if (callbacks.adapter) callbacks.adapter(adapter);
        Scene scene;
        create_scene(gpu, scene, options.eye_width, options.eye_height);
        Context context{gpu, scene, options, adapter, std::nullopt, {}};
        for (std::size_t index = 0; index < cases.size(); ++index) {
            if (callbacks.begin) callbacks.begin(index, cases.size(), *cases[index]);
            const CaseResult result = run_case(context, *cases[index]);
            if (callbacks.result) callbacks.result(result);
        }
        return S_OK;
    } catch (const Fatal& fatal) {
        if (error) *error = fatal.message;
        return FAILED(fatal.result) ? fatal.result : E_FAIL;
    } catch (const CaseError& failure) {
        if (error) *error = std::wstring(failure.message.begin(), failure.message.end());
        return E_FAIL;
    } catch (const std::bad_alloc&) {
        if (error) *error = L"Out of memory.";
        return E_OUTOFMEMORY;
    } catch (...) {
        if (error) *error = L"The benchmark stopped unexpectedly.";
        return E_FAIL;
    }
}

} // namespace xrfg::fg_benchmark
