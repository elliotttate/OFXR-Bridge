#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <nvOpticalFlowD3D12.h>

#include "fullscreen_vertex_shader.hpp"
#include "game_motion_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_bidirectional_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_fast_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_pack_flow_input_shader.hpp"
#include "nvidia_pack_luma_input_shader.hpp"
#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
#include "pack_flow_input_shader.hpp"
#include "synthesize_midpoint_pixel_shader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

// Diagnostic-only XRFG-V092 markers. Keep them on the same explicit switch as
// the flight recorder so a normal run records no diagnostic GPU commands.
void dred_marker(ID3D12GraphicsCommandList* command_list, const char* label) noexcept {
    if (command_list != nullptr && label != nullptr &&
        bridge_flight_logger().enabled()) {
        command_list->SetMarker(1U, label, static_cast<UINT>(std::strlen(label)));
    }
}

constexpr std::uint32_t kWorkSlotCount = D3D12SwapchainHistory::kSlotCount;
constexpr UINT kFidelityFxFlowBlockSize = 8;
constexpr UINT kNvidiaFlowBlockSize = 4;
constexpr UINT kEyeGapPixels = 64;
constexpr UINT kMinimumFlowDimension = 64;
// Keep only one complete OFXR transaction resident on the graphics queue.  A
struct NvidiaInputScaleRatio {
    UINT numerator;
    UINT denominator;
};

[[nodiscard]] constexpr NvidiaInputScaleRatio nvidia_input_scale_ratio(
    D3D12NvidiaInputScale scale) noexcept {
    switch (scale) {
    case D3D12NvidiaInputScale::three_quarter:
        return {3U, 4U};
    case D3D12NvidiaInputScale::half:
        return {1U, 2U};
    case D3D12NvidiaInputScale::full:
    default:
        return {1U, 1U};
    }
}
constexpr UINT kMaxReprojectionViews = 2;
constexpr UINT kSrvDescriptorCount = 7;
constexpr UINT kUavDescriptorCount = 3;
constexpr UINT kDescriptorBlockSize =
    kSrvDescriptorCount + kUavDescriptorCount;
constexpr UINT kDescriptorCount =
    kDescriptorBlockSize * kMaxReprojectionViews;
constexpr UINT kNvidiaTimestampCount = 5;
constexpr UINT kNvidiaTimestampPackBegin = 0;
constexpr UINT kNvidiaTimestampPackEnd = 1;
constexpr UINT kNvidiaTimestampEye0End = 2;
constexpr UINT kNvidiaTimestampCompositionBegin = 3;
constexpr UINT kNvidiaTimestampCompositionEnd = 4;
// Two more, written by both backends, bracketing all of a pair's GPU work.
// The NVIDIA stage marks above stay as they are; these exist so the
// FidelityFX path is measurable too, and so the span can be reported as
// absolute times rather than only as a duration.
constexpr UINT kSpanBegin = 5;
constexpr UINT kSpanEnd = 6;
constexpr UINT kTimestampCount = 7;
constexpr float kPositionToleranceMeters = 1.0e-4F;
constexpr float kCameraTolerance = 1.0e-5F;
constexpr D3D12_RESOURCE_STATES kShaderReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

[[nodiscard]] bool same_device(
    ID3D12Device* expected,
    ID3D12DeviceChild* object) noexcept {
    if (expected == nullptr || object == nullptr) {
        return false;
    }

    ComPtr<ID3D12Device> actual;
    if (FAILED(object->GetDevice(IID_PPV_ARGS(actual.GetAddressOf())))) {
        return false;
    }

    ComPtr<IUnknown> expected_identity;
    ComPtr<IUnknown> actual_identity;
    return SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(expected_identity.GetAddressOf()))) &&
           SUCCEEDED(actual->QueryInterface(IID_PPV_ARGS(actual_identity.GetAddressOf()))) &&
           expected_identity.Get() == actual_identity.Get();
}

[[nodiscard]] bool same_adapter(
    ID3D12Device* expected,
    ID3D12DeviceChild* object) noexcept {
    if (expected == nullptr || object == nullptr) {
        return false;
    }
    ComPtr<ID3D12Device> actual;
    if (FAILED(object->GetDevice(IID_PPV_ARGS(actual.GetAddressOf())))) {
        return false;
    }
    const LUID expected_luid = expected->GetAdapterLuid();
    const LUID actual_luid = actual->GetAdapterLuid();
    return expected_luid.HighPart == actual_luid.HighPart &&
           expected_luid.LowPart == actual_luid.LowPart;
}

[[nodiscard]] bool copy_compatible(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension &&
           left.Width == right.Width &&
           left.Height == right.Height &&
           left.DepthOrArraySize == right.DepthOrArraySize &&
           left.MipLevels == right.MipLevels &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Layout == right.Layout;
}

[[nodiscard]] bool compatible_view_format(
    DXGI_FORMAT resource_format,
    DXGI_FORMAT view_format) noexcept {
    switch (view_format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                   resource_format == view_format;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return resource_format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                   resource_format == view_format;
        default:
            return false;
    }
}

[[nodiscard]] DXGI_FORMAT motion_vector_view_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
        return format;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
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

[[nodiscard]] D3D12_RESOURCE_BARRIER uav_barrier(
    ID3D12Resource* resource) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    return barrier;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES default_heap_properties(
    UINT node_mask) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = node_mask == 0 ? 1U : node_mask;
    properties.VisibleNodeMask = properties.CreationNodeMask;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_DESC texture2d_description(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return description;
}

[[nodiscard]] HRESULT ffx_result(FfxErrorCode result) noexcept {
    return result == FFX_OK ? S_OK : E_FAIL;
}

[[nodiscard]] HRESULT nvidia_result(NV_OF_STATUS result) noexcept {
    return result == NV_OF_SUCCESS
               ? S_OK
               : MAKE_HRESULT(
                     SEVERITY_ERROR,
                     FACILITY_ITF,
                     static_cast<unsigned>(result) & 0xffffU);
}

[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu_handle(
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    UINT index,
    UINT increment) noexcept {
    handle.ptr += static_cast<SIZE_T>(index) * increment;
    return handle;
}

[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu_handle(
    D3D12_GPU_DESCRIPTOR_HANDLE handle,
    UINT index,
    UINT increment) noexcept {
    handle.ptr += static_cast<UINT64>(index) * increment;
    return handle;
}

struct CameraMapping {
    std::array<float, 4> target_to_source_rotation{};
    std::array<float, 4> source_tangents{};
    std::array<float, 4> target_tangents{};
    std::array<float, 4> source_rect{};
    std::array<float, 4> target_rect{};
};

struct SynthesisParameters {
    UINT width{};
    UINT height{};
    UINT array_size{};
    UINT packed_width{};
    UINT packed_height{};
    UINT packed_eye_stride{};
    UINT flow_width{};
    UINT flow_height{};
    UINT flow_block_size{};
    UINT view_index{};
    UINT use_game_motion{};
    UINT game_motion_padding{};
    std::array<float, 4> game_motion_rect{};
    std::array<float, 2> game_motion_scale{};
    std::array<float, 2> game_motion_jitter_delta{};
    std::array<CameraMapping, kMaxReprojectionViews> previous_mappings{};
    UINT slice{};
    // Bit 0 is the repeated-capture flag; bit 1 asks the pack shaders to
    // sRGB-encode the flow input; bits 8-23 carry the synthetic's position
    // between the two captures in 1/65535ths. They share a slot because the
    // root signature is full - see the assert below.
    UINT synthesis_flags{};
};

// The source views are created in the swapchain's own format, so on an sRGB
// swapchain the pack shaders read linear light. The composition needs that -
// its render target is the same sRGB format and encodes on write - but the
// 8-bit flow input does not go through a render target, and written linear
// it leaves the darkest tenth of the perceptual range in two or three codes:
// dim walls turn flat to the matcher, which ghosts them under head turns. The
// pack encodes on the way out instead; the views stay shared. FidelityFX is
// told its input is sRGB (FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB) and receives
// it that way.
constexpr UINT kEncodeSrgbFlowInputFlag = 2U;

[[nodiscard]] constexpr bool srgb_view_format(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// The flag bits every pack dispatch carries; the composition ignores them.
[[nodiscard]] constexpr UINT pack_input_flags(DXGI_FORMAT view_format) noexcept {
    return srgb_view_format(view_format) ? kEncodeSrgbFlowInputFlag : 0U;
}

// Packs the synthetic's position between the two captures into the spare
// bits of synthesis_flags. Zero is reserved for "not set", which the shader
// decodes as the old fixed midpoint, so a half that rounds to zero is nudged
// rather than silently meaning something else.
[[nodiscard]] constexpr UINT packed_synthesis_fraction(float fraction) noexcept {
    const float clamped = fraction < 0.0F ? 0.0F : (fraction > 1.0F ? 1.0F : fraction);
    UINT packed = static_cast<UINT>(clamped * 65535.0F + 0.5F);
    if (packed == 0U) {
        packed = 1U;
    }
    return packed << 8U;
}

constexpr UINT kSynthesisConstantCount =
    static_cast<UINT>(sizeof(SynthesisParameters) / sizeof(UINT));
static_assert(sizeof(CameraMapping) == sizeof(float) * 20U);
static_assert(sizeof(SynthesisParameters) == sizeof(UINT) * 62U);
static_assert(offsetof(SynthesisParameters, game_motion_rect) == sizeof(UINT) * 12U);
static_assert(offsetof(SynthesisParameters, game_motion_scale) == sizeof(UINT) * 16U);
static_assert(offsetof(SynthesisParameters, game_motion_jitter_delta) == sizeof(UINT) * 18U);
static_assert(offsetof(SynthesisParameters, previous_mappings) == sizeof(UINT) * 20U);
static_assert(offsetof(SynthesisParameters, slice) == sizeof(UINT) * 60U);
static_assert(kSynthesisConstantCount + 2U <= 64U);

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] float quaternion_length_squared(const Quaternion& value) noexcept {
    return value.x * value.x + value.y * value.y +
           value.z * value.z + value.w * value.w;
}

[[nodiscard]] bool valid_view(const D3D12ReprojectionView& view) noexcept {
    constexpr float kHalfPi = 1.57079632679489661923F;
    const Quaternion& orientation = view.pose.orientation;
    const Vec3& position = view.pose.position;
    const D3D12FieldOfView& fov = view.fov;
    const float length_squared = quaternion_length_squared(orientation);
    return finite(orientation.x) && finite(orientation.y) &&
           finite(orientation.z) && finite(orientation.w) &&
           finite(position.x) && finite(position.y) && finite(position.z) &&
           finite(fov.angle_left) && finite(fov.angle_right) &&
           finite(fov.angle_up) && finite(fov.angle_down) &&
           length_squared > 1.0e-12F && finite(length_squared) &&
           fov.angle_left < fov.angle_right &&
           // OpenXR encodes a vertically flipped image by reversing Up/Down.
           // Preserve that signed mapping; only an empty/out-of-range FOV is invalid.
           fov.angle_down != fov.angle_up &&
           fov.angle_left > -kHalfPi && fov.angle_right < kHalfPi &&
           std::min(fov.angle_down, fov.angle_up) > -kHalfPi &&
           std::max(fov.angle_down, fov.angle_up) < kHalfPi &&
           ((view.image_rect.width == 0 && view.image_rect.height == 0) ||
            (view.image_rect.width != 0 && view.image_rect.height != 0));
}

[[nodiscard]] bool rect_within_resource(
    const D3D12ImageRect& rect,
    UINT width,
    UINT height) noexcept {
    if (rect.width == 0 && rect.height == 0) {
        return true;
    }
    return rect.width != 0 && rect.height != 0 &&
           rect.offset_x < width && rect.offset_y < height &&
           static_cast<std::uint64_t>(rect.offset_x) + rect.width <= width &&
           static_cast<std::uint64_t>(rect.offset_y) + rect.height <= height;
}

[[nodiscard]] UINT resolved_array_slice(
    const D3D12ReprojectionView& view,
    std::size_t view_index,
    std::size_t view_count,
    UINT array_size) noexcept {
    if (view.array_slice != std::numeric_limits<std::uint32_t>::max()) {
        return view.array_slice < array_size
            ? static_cast<UINT>(view.array_slice)
            : std::numeric_limits<UINT>::max();
    }
    return view_count == array_size && view_index < array_size
        ? static_cast<UINT>(view_index)
        : std::numeric_limits<UINT>::max();
}

[[nodiscard]] bool rectangles_overlap(
    const D3D12ImageRect& left,
    const D3D12ImageRect& right) noexcept {
    const std::uint64_t left_right =
        static_cast<std::uint64_t>(left.offset_x) + left.width;
    const std::uint64_t left_bottom =
        static_cast<std::uint64_t>(left.offset_y) + left.height;
    const std::uint64_t right_right =
        static_cast<std::uint64_t>(right.offset_x) + right.width;
    const std::uint64_t right_bottom =
        static_cast<std::uint64_t>(right.offset_y) + right.height;
    return left.offset_x < right_right && right.offset_x < left_right &&
           left.offset_y < right_bottom && right.offset_y < left_bottom;
}

[[nodiscard]] bool valid_view_layout(
    std::span<const D3D12ReprojectionView> views,
    UINT width,
    UINT height,
    UINT array_size) noexcept {
    if (views.empty() || views.size() > kMaxReprojectionViews ||
        array_size == 0 || array_size > kMaxReprojectionViews) {
        return false;
    }
    std::array<bool, kMaxReprojectionViews> covered_slices{};
    for (std::size_t view_index = 0; view_index < views.size(); ++view_index) {
        const D3D12ReprojectionView& view = views[view_index];
        const UINT slice = resolved_array_slice(
            view,
            view_index,
            views.size(),
            array_size);
        if (slice == std::numeric_limits<UINT>::max() ||
            !valid_view(view) ||
            !rect_within_resource(view.image_rect, width, height)) {
            return false;
        }
        covered_slices[slice] = true;
        for (std::size_t preceding = 0; preceding < view_index; ++preceding) {
            if (resolved_array_slice(
                    views[preceding],
                    preceding,
                    views.size(),
                    array_size) != slice) {
                continue;
            }
            if (view.image_rect.width == 0 || view.image_rect.height == 0 ||
                views[preceding].image_rect.width == 0 ||
                views[preceding].image_rect.height == 0 ||
                rectangles_overlap(view.image_rect, views[preceding].image_rect)) {
                return false;
            }
        }
    }
    return std::all_of(
        covered_slices.begin(),
        covered_slices.begin() + array_size,
        [](bool covered) { return covered; });
}

[[nodiscard]] bool same_view_layout(
    std::span<const D3D12ReprojectionView> left,
    std::span<const D3D12ReprojectionView> right,
    UINT array_size) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t view_index = 0; view_index < left.size(); ++view_index) {
        if (resolved_array_slice(
                left[view_index], view_index, left.size(), array_size) !=
                resolved_array_slice(
                    right[view_index], view_index, right.size(), array_size) ||
            left[view_index].image_rect.offset_x !=
                right[view_index].image_rect.offset_x ||
            left[view_index].image_rect.offset_y !=
                right[view_index].image_rect.offset_y ||
            left[view_index].image_rect.width !=
                right[view_index].image_rect.width ||
            left[view_index].image_rect.height !=
                right[view_index].image_rect.height) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] D3D12ImageRect resolved_rect(
    const D3D12ImageRect& rect,
    UINT width,
    UINT height) noexcept {
    return rect.width == 0 && rect.height == 0
        ? D3D12ImageRect{0, 0, width, height}
        : rect;
}

[[nodiscard]] bool views_cover_resource(
    std::span<const D3D12ReprojectionView> views,
    UINT width, UINT height, UINT array_size) noexcept {
    if (!valid_view_layout(views, width, height, array_size)) return false;
    std::array<std::uint64_t, kMaxReprojectionViews> area{};
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto r = resolved_rect(views[i].image_rect, width, height);
        area[resolved_array_slice(views[i], i, views.size(), array_size)] +=
            std::uint64_t(r.width) * r.height;
    }
    // The layout validation establishes bounded, non-overlapping rectangles.
    // Equal area therefore establishes coverage of every physical slice.
    return std::all_of(area.begin(), area.begin() + array_size,
        [&](std::uint64_t value) { return value == std::uint64_t(width) * height; });
}

void set_viewport_and_scissor(
    ID3D12GraphicsCommandList* command_list,
    const D3D12ImageRect& image_rect,
    UINT width,
    UINT height) noexcept {
    const D3D12ImageRect rect = resolved_rect(image_rect, width, height);
    const D3D12_VIEWPORT viewport{
        static_cast<float>(rect.offset_x),
        static_cast<float>(rect.offset_y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height),
        0.0F,
        1.0F,
    };
    const D3D12_RECT scissor{
        static_cast<LONG>(rect.offset_x),
        static_cast<LONG>(rect.offset_y),
        static_cast<LONG>(rect.offset_x + rect.width),
        static_cast<LONG>(rect.offset_y + rect.height),
    };
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
}

void clear_synthetic_marker(ID3D12GraphicsCommandList* commands,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, const D3D12ReprojectionView& view,
    UINT width, UINT height, const std::optional<OverlayPlacement>& marker,
    const float* colour = nullptr, float x_shift = 0.0F) noexcept {
    if (!marker) return; // No extra GPU work with diagnostics off.
    const auto& box = *marker;
    if (!finite(box.x) || !finite(box.y) || !finite(box.z) ||
        !finite(box.width) || !finite(box.height) || box.z >= -1.0e-5F ||
        box.width <= 0 || box.height <= 0) return;
    const auto rect = resolved_rect(view.image_rect, width, height);
    const float left = std::tan(view.fov.angle_left);
    const float right = std::tan(view.fov.angle_right);
    const float up = std::tan(view.fov.angle_up);
    const float down = std::tan(view.fov.angle_down);
    const float depth = -box.z;
    const float shifted = box.x + x_shift * box.width;
    const float x0 = ((shifted - box.width * 0.5F) / depth - left) / (right - left);
    const float x1 = ((shifted + box.width * 0.5F) / depth - left) / (right - left);
    const float y0 = ((box.y - box.height * 0.5F) / depth - up) / (down - up);
    const float y1 = ((box.y + box.height * 0.5F) / depth - up) / (down - up);
    if (!finite(x0) || !finite(x1) || !finite(y0) || !finite(y1)) return;
    const auto low = [](float a, float b, UINT offset, UINT extent) {
        return static_cast<LONG>(offset + std::floor(std::clamp(std::min(a, b), 0.0F, 1.0F) * extent));
    };
    const auto high = [](float a, float b, UINT offset, UINT extent) {
        return static_cast<LONG>(offset + std::ceil(std::clamp(std::max(a, b), 0.0F, 1.0F) * extent));
    };
    const D3D12_RECT mark{low(x0, x1, rect.offset_x, rect.width), low(y0, y1, rect.offset_y, rect.height),
                          high(x0, x1, rect.offset_x, rect.width), high(y0, y1, rect.offset_y, rect.height)};
    if (mark.left >= mark.right || mark.top >= mark.bottom) return;
    // Only the bridge-owned S render target is touched, AFTER interpolation.
    // It then follows the same fence, D3D11 publication and XR release as S.
    constexpr float purple[4]{0.75F, 0.0F, 1.0F, 1.0F};
    commands->ClearRenderTargetView(rtv, colour ? colour : purple, 1, &mark);
}

[[nodiscard]] bool same_position(
    const Vec3& left,
    const Vec3& right) noexcept {
    const float delta_x = left.x - right.x;
    const float delta_y = left.y - right.y;
    const float delta_z = left.z - right.z;
    return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <=
           kPositionToleranceMeters * kPositionToleranceMeters;
}

[[nodiscard]] float quaternion_dot(
    const Quaternion& left,
    const Quaternion& right) noexcept {
    return left.x * right.x + left.y * right.y +
           left.z * right.z + left.w * right.w;
}

[[nodiscard]] bool same_orientation(
    Quaternion left,
    Quaternion right) noexcept {
    left = normalize(left);
    right = normalize(right);
    return std::abs(std::abs(quaternion_dot(left, right)) - 1.0F) <=
           kCameraTolerance;
}

[[nodiscard]] bool same_fov(
    const D3D12FieldOfView& left,
    const D3D12FieldOfView& right) noexcept {
    return std::abs(left.angle_left - right.angle_left) <= kCameraTolerance &&
           std::abs(left.angle_right - right.angle_right) <= kCameraTolerance &&
           std::abs(left.angle_up - right.angle_up) <= kCameraTolerance &&
           std::abs(left.angle_down - right.angle_down) <= kCameraTolerance;
}

[[nodiscard]] bool same_camera(
    const D3D12ReprojectionView& left,
    const D3D12ReprojectionView& right) noexcept {
    return same_position(left.pose.position, right.pose.position) &&
           same_orientation(left.pose.orientation, right.pose.orientation) &&
           same_fov(left.fov, right.fov) &&
           left.image_rect.offset_x == right.image_rect.offset_x &&
           left.image_rect.offset_y == right.image_rect.offset_y &&
           left.image_rect.width == right.image_rect.width &&
           left.image_rect.height == right.image_rect.height;
}

[[nodiscard]] Quaternion multiply(
    const Quaternion& left,
    const Quaternion& right) noexcept {
    return {
        left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z,
    };
}

[[nodiscard]] std::array<float, 4> fov_tangents(
    const D3D12FieldOfView& fov) noexcept {
    return {
        std::tan(fov.angle_left),
        std::tan(fov.angle_right),
        std::tan(fov.angle_up),
        std::tan(fov.angle_down),
    };
}

[[nodiscard]] CameraMapping make_camera_mapping(
    const D3D12ReprojectionView& source,
    const D3D12ReprojectionView& target,
    UINT width,
    UINT height) noexcept {
    const Quaternion source_orientation = normalize(source.pose.orientation);
    const Quaternion target_orientation = normalize(target.pose.orientation);
    const Quaternion inverse_source{
        -source_orientation.x,
        -source_orientation.y,
        -source_orientation.z,
        source_orientation.w,
    };
    const Quaternion target_to_source =
        normalize(multiply(inverse_source, target_orientation));
    const D3D12ImageRect source_rect =
        resolved_rect(source.image_rect, width, height);
    const D3D12ImageRect target_rect =
        resolved_rect(target.image_rect, width, height);
    return {
        {
            target_to_source.x,
            target_to_source.y,
            target_to_source.z,
            target_to_source.w,
        },
        fov_tangents(source.fov),
        fov_tangents(target.fov),
        {
            static_cast<float>(source_rect.offset_x),
            static_cast<float>(source_rect.offset_y),
            static_cast<float>(source_rect.width),
            static_cast<float>(source_rect.height),
        },
        {
            static_cast<float>(target_rect.offset_x),
            static_cast<float>(target_rect.offset_y),
            static_cast<float>(target_rect.width),
            static_cast<float>(target_rect.height),
        },
    };
}

}  // namespace

struct D3D12FrameSynthesizer::Impl {
    struct Destination {
        ComPtr<ID3D12Resource> resource;
        std::uint64_t fence_value{};
    };

    struct NvidiaEyeResources {
        ComPtr<ID3D12Resource> previous_input;
        ComPtr<ID3D12Resource> current_input;
        ComPtr<ID3D12Resource> flow;
        ComPtr<ID3D12Resource> cost;
        ComPtr<ID3D12Resource> backward_flow;
        ComPtr<ID3D12Resource> backward_cost;
        NvOFGPUBufferHandle previous_input_handle{};
        NvOFGPUBufferHandle current_input_handle{};
        NvOFGPUBufferHandle flow_handle{};
        NvOFGPUBufferHandle cost_handle{};
        NvOFGPUBufferHandle backward_flow_handle{};
        NvOFGPUBufferHandle backward_cost_handle{};
    };

    struct WorkSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        ComPtr<ID3D12CommandAllocator> synthesis_allocator;
        ComPtr<ID3D12GraphicsCommandList> synthesis_command_list;
        // The bit-exact copy of B into the current output. Recorded apart from
        // the synthesis and submitted only once the synthetic frame has been
        // handed to the runtime, so it is not queued ahead of it.
        ComPtr<ID3D12CommandAllocator> current_copy_allocator;
        ComPtr<ID3D12GraphicsCommandList> current_copy_command_list;
        ComPtr<ID3D12CommandAllocator> timing_marker_allocator;
        ComPtr<ID3D12GraphicsCommandList> timing_marker_command_list;
        ComPtr<ID3D12DescriptorHeap> descriptor_heap;
        std::array<NvidiaEyeResources, kMaxReprojectionViews> nvidia_eyes;
        D3D12NvidiaGpuTiming timing_metadata{};
        D3D12NvidiaGpuTiming cached_timing{};
        std::uint64_t fence_value{};
        UINT timing_query_base{};
        bool timing_pending{};
        bool timing_cached{};
        // The pair wrote only the span marks: FidelityFX, game motion vectors
        // and native generation never run the NVIDIA optical-flow stages.
        bool timing_span_only{};
        // A span end was written and resolved since the slot was reset.
        bool timing_resolved{};
    };

    struct RollingSource {
        D3D12HistoryCaptureTicket ticket{};
        D3D12HistoryConsumerLease lease{};
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12Fence> producer_fence;
        std::array<D3D12ReprojectionView, kMaxReprojectionViews> views{};
        UINT view_count{};
        std::uint64_t last_use_fence_value{};
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors;

        [[nodiscard]] bool active() const noexcept {
            return lease.lease_serial != 0;
        }

        void clear() noexcept {
            ticket = {};
            lease = {};
            resource.Reset();
            producer_fence.Reset();
            views = {};
            view_count = 0;
            last_use_fence_value = 0;
            motion_vectors.reset();
        }
    };

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::shared_ptr<D3D12SwapchainHistory> history;
    std::vector<Destination> current_destinations;
    std::vector<Destination> synthetic_destinations;
    std::array<WorkSlot, kWorkSlotCount> work_slots;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT current_rtv_base{};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pack_pipeline;
    ComPtr<ID3D12PipelineState> graphics_pipeline;
    ComPtr<ID3D12PipelineState> game_motion_graphics_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_pack_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_luma_pack_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_graphics_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_bidirectional_graphics_pipeline;
    ComPtr<ID3D12Resource> packed_color;
    ComPtr<ID3D12Resource> optical_flow_vector;
    ComPtr<ID3D12Resource> optical_flow_scene_change;
    std::vector<std::byte> ffx_scratch;
    FfxOpticalflowContext ffx_context{};
    HMODULE nvidia_module{};
    NV_OF_D3D12_API_FUNCTION_LIST nvidia_api{};
    std::array<NvOFHandle, kMaxReprojectionViews> nvidia_contexts{};
    // What the OFA input textures are: R8 where the driver accepts grayscale
    // input, which every engine since Turing does and which is a quarter of
    // the bytes to write and to read for the same vectors; BGRA8 otherwise.
    // Decided against the first context, before any input is created.
    DXGI_FORMAT nvidia_input_format{DXGI_FORMAT_B8G8R8A8_UNORM};
    ComPtr<ID3D12Fence> fence;
    // Carries the runtime's acquire guarantee from the queue it knows about
    // onto the one it does not. Kept apart from `fence` so it cannot disturb
    // the submission values the rest of the pipeline tracks against that one.
    ComPtr<ID3D12Fence> acquire_fence;
    std::uint64_t next_acquire_fence_value{1};
    ComPtr<ID3D12Fence> nvidia_fence;
    ComPtr<ID3D12QueryHeap> nvidia_timestamp_heap;
    // Maps GPU ticks onto QueryPerformanceCounter, sampled once when the
    // heap is created. Without it a timestamp is only a duration; with it
    // it is a moment that can be compared to when the frame was submitted.
    std::uint64_t calibration_gpu_ticks{};
    std::uint64_t calibration_qpc_ticks{};
    std::uint64_t qpc_frequency{};
    ComPtr<ID3D12Resource> nvidia_timestamp_readback;
    HANDLE fence_event{};
    RollingSource previous;
    RollingSource untracked_source;
    D3D12_RESOURCE_DESC image_description{};
    DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN};
    D3D12_RESOURCE_STATES release_state{};
    UINT descriptor_increment{};
    UINT rtv_increment{};
    UINT packed_width{};
    UINT packed_height{};
    UINT packed_eye_stride{};
    UINT flow_width{};
    UINT flow_height{};
    UINT flow_block_size{};
    std::uint64_t next_fence_value{1};
    std::uint64_t last_submitted_fence_value{};
    // Set by each submit_pair from the interval its two captures span. Half
    // is only correct when the application runs at exactly half the display
    // rate; away from that the synthetic belongs somewhere else between them.
    float synthetic_fraction{0.5F};
    // The pair's second synthetic, when the caller asked for one: set and
    // cleared by each submit_pair alongside the fraction above. Every
    // composition loop draws once per output, so the flow a pair computed
    // serves both.
    std::optional<std::uint32_t> extra_synthetic_destination;
    float extra_synthetic_fraction{0.5F};

    [[nodiscard]] UINT synthetic_output_count() const noexcept {
        return extra_synthetic_destination ? 2U : 1U;
    }
    [[nodiscard]] float synthetic_output_fraction(UINT output) const noexcept {
        return output == 0U ? synthetic_fraction : extra_synthetic_fraction;
    }
    [[nodiscard]] std::uint32_t synthetic_output_destination(
        UINT output,
        std::uint32_t first_destination) const noexcept {
        return output == 0U ? first_destination : *extra_synthetic_destination;
    }
    // The work slot whose current copy has been recorded and closed but not
    // yet submitted, and the fence value that submission will signal. The
    // slot cannot be recycled and the history lease cannot retire until it
    // has, so every entry point that needs either flushes first.
    static constexpr std::uint32_t kNoPendingCopy =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t pending_copy_slot{kNoPendingCopy};
    std::uint64_t pending_copy_fence_value{};
    // The value the most recent submission signalled once the synthetic's
    // pixels existed, as opposed to the value the whole pair completes at.
    std::uint64_t last_synthetic_fence_value{};
    std::uint64_t next_nvidia_fence_value{1};
    std::uint64_t last_nvidia_fence_value{};
    std::uint64_t nvidia_timestamp_frequency{};
    std::uint32_t next_work_slot{};
    std::uint32_t next_timing_slot{};
    D3D12OpticalFlowBackend backend{D3D12OpticalFlowBackend::fidelity_fx};
    D3D12NvidiaOpticalFlowOptions nvidia_options{};
    std::unique_ptr<D3D12NativeDlssG> native_dlss;
    bool native_pair_generated{};
    bool synthesis_enabled{};
    bool completion_unknown{};
    bool ffx_context_created{};
    bool flow_output_consumed{};
    bool nvidia_gpu_timing_enabled{};

    ~Impl() {
        // Nothing outside this object waits on the synthesis queue. Its work
        // writes into the slot command allocators and reads the history
        // slots, so releasing them with submissions still in flight is a
        // use-after-free - which the debug layer reports at destruction
        // rather than where it happened. When the synthesis runs on the
        // application's queue the application drains it; on a queue of the
        // layer's own, nobody does.
        //
        // Signal a fresh value rather than waiting on the tracked one. A
        // deferred current copy that was never flushed has already reserved
        // last_submitted_fence_value, and nothing will ever signal it, so
        // waiting there would hang instead of draining. The acquire fence is
        // used because its values are not part of that bookkeeping.
        if (queue != nullptr && acquire_fence != nullptr &&
            fence_event != nullptr) {
            const std::uint64_t drain = next_acquire_fence_value++;
            if (SUCCEEDED(queue->Signal(acquire_fence.Get(), drain))) {
                static_cast<void>(wait_for_fence(acquire_fence.Get(), drain));
            }
        }
        if (ffx_context_created) {
            static_cast<void>(ffxOpticalflowContextDestroy(&ffx_context));
        }
        if (nvidia_fence != nullptr && last_nvidia_fence_value != 0) {
            static_cast<void>(wait_for_fence(
                nvidia_fence.Get(),
                last_nvidia_fence_value));
        }
        if (nvidia_api.nvOFUnregisterResourceD3D12 != nullptr) {
            for (WorkSlot& slot : work_slots) {
                for (NvidiaEyeResources& eye : slot.nvidia_eyes) {
                    const std::array<NvOFGPUBufferHandle*, 6> handles{
                        &eye.backward_cost_handle,
                        &eye.backward_flow_handle,
                        &eye.cost_handle,
                        &eye.flow_handle,
                        &eye.current_input_handle,
                        &eye.previous_input_handle,
                    };
                    for (NvOFGPUBufferHandle* handle : handles) {
                        if (*handle == nullptr) {
                            continue;
                        }
                        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 parameters{};
                        parameters.hOFGpuBuffer = *handle;
                        static_cast<void>(
                            nvidia_api.nvOFUnregisterResourceD3D12(&parameters));
                        *handle = nullptr;
                    }
                }
            }
        }
        for (WorkSlot& slot : work_slots) {
            for (NvidiaEyeResources& eye : slot.nvidia_eyes) {
                eye.backward_cost.Reset();
                eye.backward_flow.Reset();
                eye.cost.Reset();
                eye.flow.Reset();
                eye.current_input.Reset();
                eye.previous_input.Reset();
            }
        }
        if (nvidia_api.nvOFDestroy != nullptr) {
            for (NvOFHandle& context : nvidia_contexts) {
                if (context == nullptr) {
                    continue;
                }
                static_cast<void>(nvidia_api.nvOFDestroy(context));
                context = nullptr;
            }
        }
        if (nvidia_module != nullptr) {
            FreeLibrary(nvidia_module);
            nvidia_module = nullptr;
        }
        if (fence_event != nullptr) {
            CloseHandle(fence_event);
        }
    }

    [[nodiscard]] HRESULT create_root_signature_and_pipelines(UINT node_mask) noexcept {
        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = kSrvDescriptorCount;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = kUavDescriptorCount;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0;
        parameters[2].Constants.RegisterSpace = 0;
        parameters[2].Constants.Num32BitValues = kSynthesisConstantCount;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        root_description.NumStaticSamplers = 0;
        root_description.pStaticSamplers = nullptr;
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            error.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateRootSignature(
            node_mask,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(root_signature.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC compute_description{};
        compute_description.pRootSignature = root_signature.Get();
        compute_description.CS = {
            g_xrfg_pack_flow_input_shader,
            sizeof(g_xrfg_pack_flow_input_shader),
        };
        compute_description.NodeMask = node_mask;
        result = device->CreateComputePipelineState(
            &compute_description,
            IID_PPV_ARGS(pack_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        compute_description.CS = {
            g_xrfg_nvidia_pack_flow_input_shader,
            sizeof(g_xrfg_nvidia_pack_flow_input_shader),
        };
        result = device->CreateComputePipelineState(
            &compute_description,
            IID_PPV_ARGS(nvidia_pack_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        compute_description.CS = {
            g_xrfg_nvidia_pack_luma_input_shader,
            sizeof(g_xrfg_nvidia_pack_luma_input_shader),
        };
        result = device->CreateComputePipelineState(
            &compute_description,
            IID_PPV_ARGS(nvidia_luma_pack_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_description{};
        graphics_description.pRootSignature = root_signature.Get();
        graphics_description.VS = {
            g_xrfg_fullscreen_vertex_shader,
            sizeof(g_xrfg_fullscreen_vertex_shader),
        };
        graphics_description.PS = {
            g_xrfg_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_synthesize_midpoint_pixel_shader),
        };
        graphics_description.BlendState.AlphaToCoverageEnable = FALSE;
        graphics_description.BlendState.IndependentBlendEnable = FALSE;
        D3D12_RENDER_TARGET_BLEND_DESC& target_blend =
            graphics_description.BlendState.RenderTarget[0];
        target_blend.BlendEnable = FALSE;
        target_blend.LogicOpEnable = FALSE;
        target_blend.SrcBlend = D3D12_BLEND_ONE;
        target_blend.DestBlend = D3D12_BLEND_ZERO;
        target_blend.BlendOp = D3D12_BLEND_OP_ADD;
        target_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        target_blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        target_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target_blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        target_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        graphics_description.SampleMask = std::numeric_limits<UINT>::max();
        graphics_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        graphics_description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        graphics_description.RasterizerState.FrontCounterClockwise = FALSE;
        graphics_description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        graphics_description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        graphics_description.RasterizerState.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        graphics_description.RasterizerState.DepthClipEnable = TRUE;
        graphics_description.RasterizerState.MultisampleEnable = FALSE;
        graphics_description.RasterizerState.AntialiasedLineEnable = FALSE;
        graphics_description.RasterizerState.ForcedSampleCount = 0;
        graphics_description.RasterizerState.ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        graphics_description.DepthStencilState.DepthEnable = FALSE;
        graphics_description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        graphics_description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        graphics_description.DepthStencilState.StencilEnable = FALSE;
        graphics_description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        graphics_description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        graphics_description.InputLayout = {nullptr, 0};
        graphics_description.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        graphics_description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics_description.NumRenderTargets = 1;
        graphics_description.RTVFormats[0] = view_format;
        graphics_description.DSVFormat = DXGI_FORMAT_UNKNOWN;
        graphics_description.SampleDesc.Count = 1;
        graphics_description.SampleDesc.Quality = 0;
        graphics_description.NodeMask = node_mask;
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        graphics_description.PS = {
            g_xrfg_game_motion_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_game_motion_synthesize_midpoint_pixel_shader),
        };
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(game_motion_graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        graphics_description.PS = {
            g_xrfg_nvidia_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_nvidia_synthesize_midpoint_pixel_shader),
        };
        if (nvidia_options.preset == D3D12NvidiaPerformancePreset::fast) {
            graphics_description.PS = {
                g_xrfg_nvidia_fast_synthesize_midpoint_pixel_shader,
                sizeof(g_xrfg_nvidia_fast_synthesize_midpoint_pixel_shader),
            };
        }
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(nvidia_graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        graphics_description.PS = {
            g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader),
        };
        if (nvidia_options.preset == D3D12NvidiaPerformancePreset::fast) {
            graphics_description.PS = {
                g_xrfg_nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader,
                sizeof(g_xrfg_nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader),
            };
        }
        return device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(
                nvidia_bidirectional_graphics_pipeline.GetAddressOf()));
    }

    [[nodiscard]] HRESULT create_fidelityfx_resources(UINT node_mask) {
        const D3D12_HEAP_PROPERTIES heap_properties =
            default_heap_properties(node_mask);

        const D3D12_RESOURCE_DESC packed_description = texture2d_description(
            packed_width,
            packed_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        HRESULT result = device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &packed_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(packed_color.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        const size_t scratch_size =
            ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT);
        if (scratch_size == 0) {
            return E_FAIL;
        }
        ffx_scratch.resize(scratch_size);

        FfxOpticalflowContextDescription context_description{};
        result = ffx_result(ffxGetInterfaceDX12(
            &context_description.backendInterface,
            ffxGetDeviceDX12(device.Get()),
            ffx_scratch.data(),
            ffx_scratch.size(),
            FFX_OPTICALFLOW_CONTEXT_COUNT));
        if (FAILED(result)) {
            return result;
        }
        context_description.flags = 0;
        context_description.resolution = {packed_width, packed_height};
        result = ffx_result(ffxOpticalflowContextCreate(
            &ffx_context,
            &context_description));
        if (FAILED(result)) {
            return result;
        }
        ffx_context_created = true;

        FfxOpticalflowSharedResourceDescriptions shared{};
        result = ffx_result(ffxOpticalflowGetSharedResourceDescriptions(
            &ffx_context,
            &shared));
        if (FAILED(result)) {
            return result;
        }
        flow_width = shared.opticalFlowVector.resourceDescription.width;
        flow_height = shared.opticalFlowVector.resourceDescription.height;
        if (flow_width == 0 || flow_height == 0 ||
            shared.opticalFlowVector.resourceDescription.depth != 1 ||
            shared.opticalFlowVector.resourceDescription.mipCount != 1 ||
            shared.opticalFlowSCD.resourceDescription.width != 3 ||
            shared.opticalFlowSCD.resourceDescription.height != 1) {
            return E_FAIL;
        }

        const D3D12_RESOURCE_DESC flow_description = texture2d_description(
            flow_width,
            flow_height,
            ffxGetDX12FormatFromSurfaceFormat(
                shared.opticalFlowVector.resourceDescription.format),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result = device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &flow_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(optical_flow_vector.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        const D3D12_RESOURCE_DESC scene_change_description =
            texture2d_description(
                shared.opticalFlowSCD.resourceDescription.width,
                shared.opticalFlowSCD.resourceDescription.height,
                ffxGetDX12FormatFromSurfaceFormat(
                    shared.opticalFlowSCD.resourceDescription.format),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &scene_change_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(optical_flow_scene_change.GetAddressOf()));
    }

    [[nodiscard]] bool nvidia_surface_format_supported(
        NvOFHandle context,
        NV_OF_BUFFER_USAGE usage,
        DXGI_FORMAT required_format) const noexcept {
        std::uint32_t count = 0;
        if (nvidia_api.nvOFGetSurfaceFormatCountD3D12(
                context,
                usage,
                NV_OF_MODE_OPTICALFLOW,
                &count) != NV_OF_SUCCESS ||
            count == 0) {
            return false;
        }
        try {
            std::vector<DXGI_FORMAT> formats(count);
            if (nvidia_api.nvOFGetSurfaceFormatD3D12(
                    context,
                    usage,
                    NV_OF_MODE_OPTICALFLOW,
                    formats.data()) != NV_OF_SUCCESS) {
                return false;
            }
            return std::find(formats.begin(), formats.end(), required_format) !=
                   formats.end();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool nvidia_capability_contains(
        NvOFHandle context,
        NV_OF_CAPS capability,
        std::uint32_t required_value) const noexcept {
        std::uint32_t count = 0;
        if (nvidia_api.nvOFGetCaps(
                context,
                capability,
                nullptr,
                &count) != NV_OF_SUCCESS ||
            count == 0) {
            return false;
        }
        try {
            std::vector<std::uint32_t> values(count);
            if (nvidia_api.nvOFGetCaps(
                    context,
                    capability,
                    values.data(),
                    &count) != NV_OF_SUCCESS) {
                return false;
            }
            return std::find(values.begin(), values.begin() + count, required_value) !=
                   values.begin() + count;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool nvidia_dimension_supported(
        NvOFHandle context,
        NV_OF_CAPS minimum_capability,
        NV_OF_CAPS maximum_capability,
        std::uint32_t value) const noexcept {
        std::uint32_t minimum_count = 0;
        std::uint32_t maximum_count = 0;
        if (nvidia_api.nvOFGetCaps(
                context,
                minimum_capability,
                nullptr,
                &minimum_count) != NV_OF_SUCCESS ||
            nvidia_api.nvOFGetCaps(
                context,
                maximum_capability,
                nullptr,
                &maximum_count) != NV_OF_SUCCESS ||
            minimum_count != 1 || maximum_count != 1) {
            return false;
        }
        std::uint32_t minimum = 0;
        std::uint32_t maximum = 0;
        return nvidia_api.nvOFGetCaps(
                   context,
                   minimum_capability,
                   &minimum,
                   &minimum_count) == NV_OF_SUCCESS &&
               nvidia_api.nvOFGetCaps(
                   context,
                   maximum_capability,
                   &maximum,
                   &maximum_count) == NV_OF_SUCCESS &&
               value >= minimum && value <= maximum;
    }

    [[nodiscard]] HRESULT register_nvidia_resource(
        NvOFHandle context,
        ID3D12Resource* resource,
        NvOFGPUBufferHandle* output_handle) noexcept {
        if (resource == nullptr || output_handle == nullptr ||
            *output_handle != nullptr || fence == nullptr ||
            nvidia_fence == nullptr || next_nvidia_fence_value == 0 ||
            next_nvidia_fence_value ==
                std::numeric_limits<std::uint64_t>::max()) {
            return E_INVALIDARG;
        }
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 parameters{};
        parameters.resource = resource;
        parameters.inputFencePoint.fence = fence.Get();
        parameters.inputFencePoint.value = 0;
        parameters.hOFGpuBuffer = output_handle;
        parameters.outputFencePoint.fence = nvidia_fence.Get();
        parameters.outputFencePoint.value = next_nvidia_fence_value;
        HRESULT result = nvidia_result(
            nvidia_api.nvOFRegisterResourceD3D12(
                context,
                &parameters));
        if (FAILED(result)) {
            return result;
        }
        last_nvidia_fence_value = next_nvidia_fence_value;
        ++next_nvidia_fence_value;
        // Resource registration is asynchronous in D3D12. V080 uses one OFA
        // context per independent eye stream, so do not let registration
        // signals from different contexts race on the shared ordered fence.
        // This is initialization-only and cannot affect steady-state pacing.
        result = wait_for_fence(
            nvidia_fence.Get(),
            last_nvidia_fence_value);
        return result;
    }

    [[nodiscard]] HRESULT create_nvidia_resources(UINT node_mask) noexcept {
        nvidia_module = LoadLibraryExW(
            L"nvofapi64.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (nvidia_module == nullptr) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        using CreateInstanceFunction = NV_OF_STATUS(NVOFAPI*)(
            std::uint32_t,
            NV_OF_D3D12_API_FUNCTION_LIST*);
        const auto create_instance = reinterpret_cast<CreateInstanceFunction>(
            GetProcAddress(nvidia_module, "NvOFAPICreateInstanceD3D12"));
        if (create_instance == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        HRESULT result = nvidia_result(
            create_instance(NV_OF_API_VERSION, &nvidia_api));
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(nvidia_fence.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        flow_width =
            (packed_width + kNvidiaFlowBlockSize - 1U) /
            kNvidiaFlowBlockSize;
        flow_height =
            (packed_height + kNvidiaFlowBlockSize - 1U) /
            kNvidiaFlowBlockSize;
        const D3D12_HEAP_PROPERTIES heap_properties =
            default_heap_properties(node_mask);
        // Settled below against the first context, which is the first point
        // the driver can be asked what it accepts.
        D3D12_RESOURCE_DESC input_description = texture2d_description(
            packed_width,
            packed_height,
            nvidia_input_format,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC flow_description = texture2d_description(
            flow_width,
            flow_height,
            DXGI_FORMAT_R16G16_SINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC cost_description = texture2d_description(
            flow_width,
            flow_height,
            DXGI_FORMAT_R8_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        NV_OF_INIT_PARAMS initialization{};
        initialization.width = packed_width;
        initialization.height = packed_height;
        initialization.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        initialization.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        initialization.mode = NV_OF_MODE_OPTICALFLOW;
        switch (nvidia_options.preset) {
        case D3D12NvidiaPerformancePreset::fast:
            initialization.perfLevel = NV_OF_PERF_LEVEL_FAST;
            break;
        case D3D12NvidiaPerformancePreset::slow:
            initialization.perfLevel = NV_OF_PERF_LEVEL_SLOW;
            break;
        case D3D12NvidiaPerformancePreset::medium:
        default:
            initialization.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
            break;
        }
        initialization.enableExternalHints = NV_OF_FALSE;
        initialization.enableOutputCost = NV_OF_TRUE;
        initialization.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
        initialization.enableRoi = NV_OF_FALSE;
        initialization.predDirection = nvidia_options.bidirectional
            ? NV_OF_PRED_DIRECTION_BOTH
            : NV_OF_PRED_DIRECTION_FORWARD;
        initialization.enableGlobalFlow = NV_OF_FALSE;
        initialization.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvOFHandle& context = nvidia_contexts[eye_index];
            result = nvidia_result(nvidia_api.nvCreateOpticalFlowD3D12(
                device.Get(),
                &context));
            if (FAILED(result) || context == nullptr) {
                return FAILED(result) ? result : E_FAIL;
            }
            if (eye_index == 0) {
                nvidia_input_format = nvidia_surface_format_supported(
                        context,
                        NV_OF_BUFFER_USAGE_INPUT,
                        DXGI_FORMAT_R8_UNORM)
                    ? DXGI_FORMAT_R8_UNORM
                    : DXGI_FORMAT_B8G8R8A8_UNORM;
                initialization.inputBufferFormat =
                    nvidia_input_format == DXGI_FORMAT_R8_UNORM
                        ? NV_OF_BUFFER_FORMAT_GRAYSCALE8
                        : NV_OF_BUFFER_FORMAT_ABGR8;
                input_description.Format = nvidia_input_format;
            }
            if (!nvidia_surface_format_supported(
                    context,
                    NV_OF_BUFFER_USAGE_INPUT,
                    nvidia_input_format) ||
                !nvidia_surface_format_supported(
                    context,
                    NV_OF_BUFFER_USAGE_OUTPUT,
                    DXGI_FORMAT_R16G16_SINT) ||
                !nvidia_surface_format_supported(
                    context,
                    NV_OF_BUFFER_USAGE_COST,
                    DXGI_FORMAT_R8_UINT) ||
                !nvidia_capability_contains(
                    context,
                    NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
                    kNvidiaFlowBlockSize) ||
                !nvidia_dimension_supported(
                    context,
                    NV_OF_CAPS_WIDTH_MIN,
                    NV_OF_CAPS_WIDTH_MAX,
                    packed_width) ||
                !nvidia_dimension_supported(
                    context,
                    NV_OF_CAPS_HEIGHT_MIN,
                    NV_OF_CAPS_HEIGHT_MAX,
                    packed_height)) {
                return E_NOTIMPL;
            }
            result = nvidia_result(
                nvidia_api.nvOFInit(context, &initialization));
            if (FAILED(result)) {
                return result;
            }

            // Each command-list work slot owns a complete OFA surface set.
            // A later application frame can therefore be queued while the
            // previous slot is still executing without overwriting registered
            // input, flow or cost resources.  The per-eye contexts remain
            // independent and fence points preserve submission order.
            for (WorkSlot& slot : work_slots) {
                NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
                const std::array<std::pair<const D3D12_RESOURCE_DESC*,
                                           ComPtr<ID3D12Resource>*>, 4>
                    resources{{
                        {&input_description, &eye.previous_input},
                        {&input_description, &eye.current_input},
                        {&flow_description, &eye.flow},
                        {&cost_description, &eye.cost},
                    }};
                for (const auto& [description, resource] : resources) {
                    result = device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(resource->GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                }

                if (nvidia_options.bidirectional) {
                    result = device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        &flow_description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(eye.backward_flow.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                    result = device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        &cost_description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(eye.backward_cost.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                }

                const std::array<
                    std::pair<ID3D12Resource*, NvOFGPUBufferHandle*>, 4>
                    registrations{{
                        {eye.previous_input.Get(), &eye.previous_input_handle},
                        {eye.current_input.Get(), &eye.current_input_handle},
                        {eye.flow.Get(), &eye.flow_handle},
                        {eye.cost.Get(), &eye.cost_handle},
                    }};
                for (const auto& [resource, handle] : registrations) {
                    result = register_nvidia_resource(context, resource, handle);
                    if (FAILED(result)) {
                        return result;
                    }
                }
                if (nvidia_options.bidirectional) {
                    result = register_nvidia_resource(
                        context,
                        eye.backward_flow.Get(),
                        &eye.backward_flow_handle);
                    if (FAILED(result)) {
                        return result;
                    }
                    result = register_nvidia_resource(
                        context,
                        eye.backward_cost.Get(),
                        &eye.backward_cost_handle);
                    if (FAILED(result)) {
                        return result;
                    }
                }
            }
        }
        return S_OK;
    }

    // A GPU timestamp as a QueryPerformanceCounter value, using the
    // calibration sampled at creation. Both clocks run at a fixed rate, so
    // one simultaneous sample plus the two frequencies is enough.
    [[nodiscard]] std::uint64_t timestamp_qpc(
        std::uint64_t gpu_ticks) const noexcept {
        if (calibration_gpu_ticks == 0 || qpc_frequency == 0 ||
            nvidia_timestamp_frequency == 0) {
            return 0;
        }
        const double gpu_delta =
            static_cast<double>(gpu_ticks) -
            static_cast<double>(calibration_gpu_ticks);
        const double qpc_delta = gpu_delta *
            static_cast<double>(qpc_frequency) /
            static_cast<double>(nvidia_timestamp_frequency);
        return static_cast<std::uint64_t>(
            static_cast<double>(calibration_qpc_ticks) + qpc_delta);
    }

    [[nodiscard]] HRESULT create_nvidia_timing_resources(
        UINT node_mask) noexcept {
        if (!nvidia_gpu_timing_enabled) {
            return S_OK;
        }
        HRESULT result = queue->GetTimestampFrequency(
            &nvidia_timestamp_frequency);
        if (FAILED(result) || nvidia_timestamp_frequency == 0) {
            return FAILED(result) ? result : E_FAIL;
        }

        D3D12_QUERY_HEAP_DESC query_description{};
        query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query_description.Count =
            kTimestampCount * static_cast<UINT>(work_slots.size());
        query_description.NodeMask = node_mask;
        LARGE_INTEGER qpc_hz{};
        if (QueryPerformanceFrequency(&qpc_hz) && qpc_hz.QuadPart > 0) {
            qpc_frequency = static_cast<std::uint64_t>(qpc_hz.QuadPart);
        }
        if (FAILED(queue->GetClockCalibration(
                &calibration_gpu_ticks, &calibration_qpc_ticks))) {
            calibration_gpu_ticks = 0;
            calibration_qpc_ticks = 0;
        }
        result = device->CreateQueryHeap(
            &query_description,
            IID_PPV_ARGS(nvidia_timestamp_heap.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        readback_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readback_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readback_heap.CreationNodeMask = node_mask;
        readback_heap.VisibleNodeMask = node_mask;
        D3D12_RESOURCE_DESC readback_description{};
        readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_description.Alignment = 0;
        readback_description.Width =
            static_cast<UINT64>(query_description.Count) * sizeof(std::uint64_t);
        readback_description.Height = 1;
        readback_description.DepthOrArraySize = 1;
        readback_description.MipLevels = 1;
        readback_description.Format = DXGI_FORMAT_UNKNOWN;
        readback_description.SampleDesc.Count = 1;
        readback_description.SampleDesc.Quality = 0;
        readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readback_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        return device->CreateCommittedResource(
            &readback_heap,
            D3D12_HEAP_FLAG_NONE,
            &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(nvidia_timestamp_readback.GetAddressOf()));
    }

    [[nodiscard]] HRESULT create_work_slots(UINT node_mask) noexcept {

        descriptor_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (descriptor_increment == 0) {
            return E_FAIL;
        }

        for (WorkSlot& slot : work_slots) {
            slot.timing_query_base = static_cast<UINT>(
                &slot - work_slots.data()) * kTimestampCount;
            HRESULT result = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(slot.allocator.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandList(
                node_mask,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(),
                nullptr,
                IID_PPV_ARGS(slot.command_list.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            // Stable DRED names let the host identify which bridge phase was executing when
            // a GPU device removal occurred. They have no effect on command recording/order.
            slot.command_list->SetName(L"OFXR Pack and Composite");
            result = slot.command_list->Close();
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(slot.current_copy_allocator.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandList(
                node_mask,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.current_copy_allocator.Get(),
                nullptr,
                IID_PPV_ARGS(slot.current_copy_command_list.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = slot.current_copy_command_list->Close();
            if (FAILED(result)) {
                return result;
            }
            if (backend == D3D12OpticalFlowBackend::nvidia &&
                nvidia_options.frame_generation != D3D12FrameGeneration::native_dlss) {
                result = device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(slot.synthesis_allocator.GetAddressOf()));
                if (FAILED(result)) {
                    return result;
                }
                result = device->CreateCommandList(
                    node_mask,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    slot.synthesis_allocator.Get(),
                    nullptr,
                    IID_PPV_ARGS(slot.synthesis_command_list.GetAddressOf()));
                if (FAILED(result)) {
                    return result;
                }
                slot.synthesis_command_list->SetName(L"OFXR NVIDIA Synthesis");
                result = slot.synthesis_command_list->Close();
                if (FAILED(result)) {
                    return result;
                }
                if (nvidia_gpu_timing_enabled) {
                    result = device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(
                            slot.timing_marker_allocator.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                    result = device->CreateCommandList(
                        node_mask,
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        slot.timing_marker_allocator.Get(),
                        nullptr,
                        IID_PPV_ARGS(
                            slot.timing_marker_command_list.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                    slot.timing_marker_command_list->SetName(L"OFXR NVIDIA Timing");
                    result = slot.timing_marker_command_list->Close();
                    if (FAILED(result)) {
                        return result;
                    }
                }
            }

            D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
            descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptor_description.NumDescriptors = kDescriptorCount;
            descriptor_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            descriptor_description.NodeMask = node_mask;
            result = device->CreateDescriptorHeap(
                &descriptor_description,
                IID_PPV_ARGS(slot.descriptor_heap.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu_start =
                slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();

            D3D12_SHADER_RESOURCE_VIEW_DESC srv_description{};
            srv_description.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_description.Texture2D.MostDetailedMip = 0;
            srv_description.Texture2D.MipLevels = 1;
            srv_description.Texture2D.PlaneSlice = 0;
            srv_description.Texture2D.ResourceMinLODClamp = 0.0F;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav_description{};
            uav_description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav_description.Texture2D.MipSlice = 0;
            uav_description.Texture2D.PlaneSlice = 0;
            if (backend == D3D12OpticalFlowBackend::nvidia) {
                for (UINT eye_index = 0;
                     eye_index < image_description.DepthOrArraySize;
                     ++eye_index) {
                    const UINT base = eye_index * kDescriptorBlockSize;
                    NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
                    srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                    device->CreateShaderResourceView(
                        eye.flow.Get(),
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 2,
                            descriptor_increment));
                    srv_description.Format = DXGI_FORMAT_R8_UINT;
                    device->CreateShaderResourceView(
                        eye.cost.Get(),
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 3,
                            descriptor_increment));
                    srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                    device->CreateShaderResourceView(
                        nvidia_options.bidirectional
                            ? eye.backward_flow.Get()
                            : nullptr,
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 4,
                            descriptor_increment));
                    srv_description.Format = DXGI_FORMAT_R8_UINT;
                    device->CreateShaderResourceView(
                        nvidia_options.bidirectional
                            ? eye.backward_cost.Get()
                            : nullptr,
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 5,
                            descriptor_increment));
                    uav_description.Format = nvidia_input_format;
                    device->CreateUnorderedAccessView(
                        nullptr,
                        nullptr,
                        &uav_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + kSrvDescriptorCount,
                            descriptor_increment));
                    device->CreateUnorderedAccessView(
                        eye.previous_input.Get(),
                        nullptr,
                        &uav_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + kSrvDescriptorCount + 1,
                            descriptor_increment));
                    device->CreateUnorderedAccessView(
                        eye.current_input.Get(),
                        nullptr,
                        &uav_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + kSrvDescriptorCount + 2,
                            descriptor_increment));
                }
            } else {
                srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                device->CreateShaderResourceView(
                    optical_flow_vector.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 2, descriptor_increment));
                srv_description.Format = DXGI_FORMAT_R32_UINT;
                device->CreateShaderResourceView(
                    optical_flow_scene_change.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 3, descriptor_increment));
                srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                device->CreateShaderResourceView(
                    optical_flow_vector.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 4, descriptor_increment));
                srv_description.Format = DXGI_FORMAT_R32_UINT;
                device->CreateShaderResourceView(
                    optical_flow_scene_change.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 5, descriptor_increment));
                uav_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                device->CreateUnorderedAccessView(
                    packed_color.Get(),
                    nullptr,
                    &uav_description,
                    offset_cpu_handle(
                        cpu_start,
                        kSrvDescriptorCount,
                        descriptor_increment));
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC null_motion{};
            null_motion.Format = DXGI_FORMAT_R16G16_FLOAT;
            null_motion.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            null_motion.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            null_motion.Texture2DArray.MostDetailedMip = 0;
            null_motion.Texture2DArray.MipLevels = 1;
            null_motion.Texture2DArray.FirstArraySlice = 0;
            null_motion.Texture2DArray.ArraySize = 1;
            const UINT block_count = backend == D3D12OpticalFlowBackend::nvidia
                ? image_description.DepthOrArraySize
                : 1U;
            for (UINT block = 0; block < block_count; ++block) {
                device->CreateShaderResourceView(nullptr, &null_motion,
                    offset_cpu_handle(cpu_start,
                        block * kDescriptorBlockSize + 6U,
                        descriptor_increment));
            }
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT create_rtv_heap(UINT node_mask) noexcept {
        // Synthetic destinations first, then current: the current ones exist
        // only so the diagnostic marker can be drawn on the real frame, which
        // is otherwise indistinguishable from a frame that never arrived.
        const UINT64 descriptor_count =
            static_cast<UINT64>(synthetic_destinations.size() +
                current_destinations.size()) *
            image_description.DepthOrArraySize;
        if (descriptor_count == 0 ||
            descriptor_count > std::numeric_limits<UINT>::max()) {
            return E_INVALIDARG;
        }

        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        description.NumDescriptors = static_cast<UINT>(descriptor_count);
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        description.NodeMask = node_mask;
        HRESULT result = device->CreateDescriptorHeap(
            &description,
            IID_PPV_ARGS(rtv_heap.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        rtv_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (rtv_increment == 0) {
            return E_FAIL;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
        rtv_description.Format = view_format;
        rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv_description.Texture2DArray.MipSlice = 0;
        rtv_description.Texture2DArray.ArraySize = 1;
        rtv_description.Texture2DArray.PlaneSlice = 0;
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        UINT descriptor_index = 0;
        for (const Destination& destination : synthetic_destinations) {
            for (UINT slice = 0; slice < image_description.DepthOrArraySize;
                 ++slice) {
                rtv_description.Texture2DArray.FirstArraySlice = slice;
                device->CreateRenderTargetView(
                    destination.resource.Get(),
                    &rtv_description,
                    offset_cpu_handle(start, descriptor_index, rtv_increment));
                ++descriptor_index;
            }
        }
        current_rtv_base = descriptor_index;
        for (const Destination& destination : current_destinations) {
            for (UINT slice = 0; slice < image_description.DepthOrArraySize;
                 ++slice) {
                rtv_description.Texture2DArray.FirstArraySlice = slice;
                device->CreateRenderTargetView(
                    destination.resource.Get(),
                    &rtv_description,
                    offset_cpu_handle(start, descriptor_index, rtv_increment));
                ++descriptor_index;
            }
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT initialize(
        ID3D12Device* input_device,
        ID3D12CommandQueue* input_queue,
        std::shared_ptr<D3D12SwapchainHistory> input_history,
        std::span<ID3D12Resource* const> input_current_images,
        std::span<ID3D12Resource* const> input_synthetic_images,
        DXGI_FORMAT input_view_format,
        D3D12_RESOURCE_STATES input_release_state,
        D3D12OpticalFlowBackend input_backend,
        D3D12NvidiaOpticalFlowOptions input_nvidia_options,
        bool enable_nvidia_gpu_timing) {
        if (input_device == nullptr || input_queue == nullptr ||
            input_history == nullptr || !input_history->initialized() ||
            input_current_images.empty() || input_synthetic_images.empty() ||
            (input_release_state != D3D12_RESOURCE_STATE_RENDER_TARGET &&
             input_release_state != D3D12_RESOURCE_STATE_COMMON) ||
            input_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !same_device(input_device, input_queue) ||
            (input_backend != D3D12OpticalFlowBackend::fidelity_fx &&
             input_backend != D3D12OpticalFlowBackend::nvidia) ||
            (input_nvidia_options.frame_generation != D3D12FrameGeneration::ofxr &&
             input_nvidia_options.frame_generation != D3D12FrameGeneration::native_dlss)) {
            return E_INVALIDARG;
        }

        ID3D12Resource* const first = input_current_images.front();
        if (first == nullptr || !same_adapter(input_device, first)) {
            return E_INVALIDARG;
        }
        const D3D12_RESOURCE_DESC description = first->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Width == 0 ||
            description.Width > static_cast<UINT64>(std::numeric_limits<LONG>::max()) ||
            description.Height == 0 ||
            description.Height > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
            description.DepthOrArraySize == 0 ||
            description.DepthOrArraySize > kMaxReprojectionViews ||
            description.MipLevels != 1 ||
            description.SampleDesc.Count != 1 ||
            description.SampleDesc.Quality != 0 ||
            (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
            !compatible_view_format(description.Format, input_view_format)) {
            return E_INVALIDARG;
        }

        D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support{};
        format_support.Format = input_view_format;
        HRESULT result = input_device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT,
            &format_support,
            sizeof(format_support));
        constexpr D3D12_FORMAT_SUPPORT1 kRequiredSupport =
            D3D12_FORMAT_SUPPORT1_TEXTURE2D |
            D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
            D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if (FAILED(result) ||
            (format_support.Support1 & kRequiredSupport) != kRequiredSupport) {
            return FAILED(result) ? result : E_NOTIMPL;
        }

        D3D12_FEATURE_DATA_FORMAT_SUPPORT packed_format_support{};
        packed_format_support.Format =
            input_backend == D3D12OpticalFlowBackend::nvidia
                ? DXGI_FORMAT_B8G8R8A8_UNORM
                : DXGI_FORMAT_R8G8B8A8_UNORM;
        result = input_device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT,
            &packed_format_support,
            sizeof(packed_format_support));
        if (FAILED(result) ||
            (packed_format_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) == 0 ||
            (packed_format_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
            return FAILED(result) ? result : E_NOTIMPL;
        }

        const auto contains_resource = [](
            const std::vector<Destination>& retained,
            ID3D12Resource* resource) noexcept {
            return std::any_of(
                retained.begin(),
                retained.end(),
                [resource](const Destination& destination) noexcept {
                    return destination.resource.Get() == resource;
                });
        };

        current_destinations.reserve(input_current_images.size());
        for (ID3D12Resource* image : input_current_images) {
            if (image == nullptr || !same_adapter(input_device, image) ||
                !copy_compatible(description, image->GetDesc()) ||
                (image->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
                !compatible_view_format(image->GetDesc().Format, input_view_format) ||
                contains_resource(current_destinations, image)) {
                return E_INVALIDARG;
            }
            Destination destination{};
            destination.resource = image;
            current_destinations.push_back(std::move(destination));
        }

        synthetic_destinations.reserve(input_synthetic_images.size());
        for (ID3D12Resource* image : input_synthetic_images) {
            if (image == nullptr || !same_adapter(input_device, image) ||
                !copy_compatible(description, image->GetDesc()) ||
                !compatible_view_format(image->GetDesc().Format, input_view_format) ||
                (image->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
                contains_resource(current_destinations, image) ||
                contains_resource(synthetic_destinations, image)) {
                return E_INVALIDARG;
            }
            Destination destination{};
            destination.resource = image;
            synthetic_destinations.push_back(std::move(destination));
        }

        const UINT width = static_cast<UINT>(description.Width);
        flow_block_size =
            input_backend == D3D12OpticalFlowBackend::nvidia
                ? kNvidiaFlowBlockSize
                : kFidelityFxFlowBlockSize;
        const bool separate_nvidia_eyes =
            input_backend == D3D12OpticalFlowBackend::nvidia;
        // One input scale for whichever backend is running. The FidelityFX
        // path packs both eyes into one texture, so its stride and packed
        // height come from the scaled per-eye height, not the source.
        const NvidiaInputScaleRatio scale =
            nvidia_input_scale_ratio(input_nvidia_options.input_scale);
        const auto scaled = [&](UINT64 value) -> UINT64 {
            return (value * scale.numerator + scale.denominator - 1U) /
                scale.denominator;
        };
        const UINT64 flow_input_width = scaled(width);
        const UINT64 flow_input_height =
            scaled(description.Height);
        const UINT64 stride = separate_nvidia_eyes
            ? 0U
            : (flow_input_height + kEyeGapPixels +
               flow_block_size - 1U) /
                  flow_block_size * flow_block_size;
        const UINT64 unaligned_packed_height = separate_nvidia_eyes
            ? flow_input_height
            : stride * (description.DepthOrArraySize - 1U) +
                  flow_input_height;
        const UINT64 packed_height_value = std::max<UINT64>(
            (unaligned_packed_height + flow_block_size - 1U) /
                flow_block_size * flow_block_size,
            kMinimumFlowDimension);
        const UINT64 packed_width_value = std::max<UINT64>(
            (flow_input_width + flow_block_size - 1U) /
                flow_block_size * flow_block_size,
            kMinimumFlowDimension);
        if ((!separate_nvidia_eyes &&
             description.Height >
                 D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION - kEyeGapPixels) ||
            stride > std::numeric_limits<UINT>::max() ||
            unaligned_packed_height > std::numeric_limits<UINT>::max() ||
            packed_width_value > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            packed_height_value > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            return E_INVALIDARG;
        }
        packed_width = static_cast<UINT>(packed_width_value);
        packed_height = static_cast<UINT>(packed_height_value);
        packed_eye_stride = static_cast<UINT>(stride);

        device = input_device;
        queue = input_queue;
        history = std::move(input_history);
        image_description = description;
        view_format = input_view_format;
        release_state = input_release_state;
        backend = input_backend;
        nvidia_options = input_nvidia_options;
        // Timing is no longer NVIDIA only: the span marks below are written by
        // both backends, and FidelityFX is the one that had no measurement.
        nvidia_gpu_timing_enabled =
            enable_nvidia_gpu_timing;
        const UINT node_mask = queue->GetDesc().NodeMask;

        result = create_root_signature_and_pipelines(node_mask);
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(acquire_fence.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(
                error == ERROR_SUCCESS ? ERROR_NOT_ENOUGH_MEMORY : error);
        }
        // Native NGX owns its own optical flow. Retain the shared composition
        // pipelines for repeated frames, but allocate no OFA/FFX contexts or
        // their otherwise-unused packed inputs, flow surfaces and scratch.
        result = nvidia_options.frame_generation == D3D12FrameGeneration::native_dlss
                     ? S_OK
                     : backend == D3D12OpticalFlowBackend::nvidia
                           ? create_nvidia_resources(node_mask)
                           : create_fidelityfx_resources(node_mask);
        if (FAILED(result)) {
            return result;
        }
        result = create_nvidia_timing_resources(node_mask);
        if (FAILED(result)) {
            nvidia_gpu_timing_enabled = false;
            nvidia_timestamp_frequency = 0;
            nvidia_timestamp_readback.Reset();
            nvidia_timestamp_heap.Reset();
        }
        result = create_work_slots(node_mask);
        if (FAILED(result)) {
            return result;
        }
        result = create_rtv_heap(node_mask);
        if (FAILED(result)) {
            return result;
        }
        if (nvidia_options.frame_generation == D3D12FrameGeneration::native_dlss) {
            native_dlss = std::make_unique<D3D12NativeDlssG>();
            result = native_dlss->initialize(device.Get(), queue.Get(), image_description, view_format);
            if (FAILED(result)) return result;
        }
        synthesis_enabled = true;
        return S_OK;
    }

    [[nodiscard]] static HRESULT fence_status(
        ID3D12Fence* input_fence,
        std::uint64_t value) noexcept {
        if (value == 0) {
            return S_OK;
        }
        if (input_fence == nullptr) {
            return E_UNEXPECTED;
        }
        const std::uint64_t completed = input_fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            return E_FAIL;
        }
        return completed >= value ? S_OK : S_FALSE;
    }

    [[nodiscard]] std::uint64_t timestamp_microseconds(
        std::uint64_t begin,
        std::uint64_t end) const noexcept {
        if (end < begin || nvidia_timestamp_frequency == 0) {
            return 0;
        }
        const long double microseconds =
            static_cast<long double>(end - begin) * 1'000'000.0L /
            static_cast<long double>(nvidia_timestamp_frequency);
        return microseconds >=
                static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(microseconds + 0.5L);
    }

    [[nodiscard]] HRESULT harvest_nvidia_gpu_timing(
        WorkSlot& slot) noexcept {
        if (!nvidia_gpu_timing_enabled || !slot.timing_pending) {
            return S_FALSE;
        }
        const HRESULT status = fence_status(fence.Get(), slot.fence_value);
        if (status != S_OK) {
            return status;
        }

        const SIZE_T byte_begin =
            static_cast<SIZE_T>(slot.timing_query_base) *
            sizeof(std::uint64_t);
        const SIZE_T byte_end = byte_begin +
            kTimestampCount * sizeof(std::uint64_t);
        const D3D12_RANGE read_range{byte_begin, byte_end};
        void* mapped = nullptr;
        HRESULT result = nvidia_timestamp_readback->Map(
            0, &read_range, &mapped);
        if (FAILED(result) || mapped == nullptr) {
            slot.timing_pending = false;
            return FAILED(result) ? result : E_FAIL;
        }
        const auto* timestamps = static_cast<const std::uint64_t*>(mapped) +
            slot.timing_query_base;
        std::array<std::uint64_t, kTimestampCount> values{};
        std::copy_n(timestamps, values.size(), values.begin());
        const D3D12_RANGE no_write{0, 0};
        nvidia_timestamp_readback->Unmap(0, &no_write);

        // The span marks are written by both backends; the stage marks only
        // by NVIDIA, so their ordering is checked only where they exist.
        slot.cached_timing = slot.timing_metadata;
        slot.cached_timing.gpu_begin_qpc = timestamp_qpc(values[kSpanBegin]);
        slot.cached_timing.gpu_end_qpc = timestamp_qpc(values[kSpanEnd]);
        slot.cached_timing.total_microseconds = timestamp_microseconds(
            values[kSpanBegin], values[kSpanEnd]);
        if (slot.timing_span_only) {
            slot.timing_pending = false;
            slot.timing_cached = true;
            return S_OK;
        }
        if (values[kNvidiaTimestampPackBegin] >
                values[kNvidiaTimestampPackEnd] ||
            values[kNvidiaTimestampPackEnd] >
                values[kNvidiaTimestampEye0End] ||
            values[kNvidiaTimestampEye0End] >
                values[kNvidiaTimestampCompositionBegin] ||
            values[kNvidiaTimestampCompositionBegin] >
                values[kNvidiaTimestampCompositionEnd]) {
            slot.timing_pending = false;
            return E_FAIL;
        }

        slot.cached_timing.pack_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampPackBegin],
            values[kNvidiaTimestampPackEnd]);
        slot.cached_timing.eye0_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampPackEnd],
            values[kNvidiaTimestampEye0End]);
        slot.cached_timing.eye1_microseconds =
            slot.cached_timing.eye_count > 1
                ? timestamp_microseconds(
                      values[kNvidiaTimestampEye0End],
                      values[kNvidiaTimestampCompositionBegin])
                : 0;
        slot.cached_timing.composition_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampCompositionBegin],
            values[kNvidiaTimestampCompositionEnd]);
        slot.cached_timing.total_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampPackBegin],
            values[kNvidiaTimestampCompositionEnd]);
        slot.timing_pending = false;
        slot.timing_cached = true;
        return S_OK;
    }

    [[nodiscard]] HRESULT consume_nvidia_gpu_timing(
        D3D12NvidiaGpuTiming* output_timing) noexcept {
        if (output_timing == nullptr) {
            return E_POINTER;
        }
        *output_timing = {};
        if (!nvidia_gpu_timing_enabled) {
            return S_FALSE;
        }
        for (std::uint32_t offset = 0; offset < work_slots.size(); ++offset) {
            const std::uint32_t index =
                (next_timing_slot + offset) %
                static_cast<std::uint32_t>(work_slots.size());
            WorkSlot& slot = work_slots[index];
            if (!slot.timing_cached) {
                const HRESULT result = harvest_nvidia_gpu_timing(slot);
                if (FAILED(result)) {
                    return result;
                }
            }
            if (slot.timing_cached) {
                *output_timing = slot.cached_timing;
                slot.cached_timing = {};
                slot.timing_cached = false;
                next_timing_slot = (index + 1U) %
                    static_cast<std::uint32_t>(work_slots.size());
                return S_OK;
            }
        }
        return S_FALSE;
    }

    [[nodiscard]] HRESULT wait_for_fence(
        ID3D12Fence* input_fence,
        std::uint64_t value) noexcept {
        if (fence_event == nullptr) {
            return E_UNEXPECTED;
        }
        for (;;) {
            const HRESULT status = fence_status(input_fence, value);
            if (status != S_FALSE) {
                return status;
            }
            HRESULT result = input_fence->SetEventOnCompletion(value, fence_event);
            if (FAILED(result)) {
                return result;
            }
            if (WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0) {
                const DWORD error = GetLastError();
                return HRESULT_FROM_WIN32(
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            }
            // A prior bounded wait may still own a registration on this
            // reusable event. Its earlier fence can therefore wake us before
            // the target above; validate and re-arm instead of reporting a
            // false synchronization failure.
        }
    }

    [[nodiscard]] HRESULT wait_for_previous_submission(
        std::uint32_t timeout_milliseconds) noexcept {
        if (last_submitted_fence_value == 0) {
            return S_OK;
        }

        // last_submitted_fence_value is the value a deferred copy will signal,
        // so waiting for it without submitting that copy can only time out.
        // Waiting for the synthetic alone instead was built as V160 and again
        // here: it leaves the previous copy in flight at the next capture, the
        // real frame's hand-over then blocks on it (realCall 0.677 -> 1.792 ms)
        // and both halves lose scanouts. This wait is what gives the copy its
        // time; do not take it away without giving it back elsewhere.
        static_cast<void>(flush_pending_copy());

        const HRESULT initial_status =
            fence_status(fence.Get(), last_submitted_fence_value);
        if (initial_status == S_OK) {
            return S_OK;
        }
        if (FAILED(initial_status)) {
            synthesis_enabled = false;
            return initial_status;
        }
        if (fence_event == nullptr) {
            synthesis_enabled = false;
            return E_UNEXPECTED;
        }

        auto& flight = bridge_flight_logger();
        const auto token = flight.begin(
            BridgeFlightOperation::synthesis_frame_start_wait,
            reinterpret_cast<std::uintptr_t>(fence.Get()),
            last_submitted_fence_value,
            fence->GetCompletedValue());

        HRESULT result = S_FALSE;
        const ULONGLONG deadline =
            GetTickCount64() + timeout_milliseconds;
        while (result == S_FALSE) {
            result = fence_status(fence.Get(), last_submitted_fence_value);
            if (result != S_FALSE) {
                break;
            }
            result = fence->SetEventOnCompletion(
                last_submitted_fence_value,
                fence_event);
            if (FAILED(result)) {
                break;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                result = HRESULT_FROM_WIN32(ERROR_BUSY);
                break;
            }
            const DWORD remaining = static_cast<DWORD>(deadline - now);
            const DWORD wait_result = WaitForSingleObject(fence_event, remaining);
            if (wait_result == WAIT_TIMEOUT) {
                result = HRESULT_FROM_WIN32(ERROR_BUSY);
            } else if (wait_result != WAIT_OBJECT_0) {
                const DWORD error = GetLastError();
                result = HRESULT_FROM_WIN32(
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            } else {
                // May be a stale registration left by an earlier bounded
                // wait. The loop checks the actual fence and re-arms while
                // preserving the original deadline.
                result = S_FALSE;
                if (GetTickCount64() >= deadline) {
                    result = HRESULT_FROM_WIN32(ERROR_BUSY);
                }
            }
        }

        flight.end(
            token,
            BridgeFlightOperation::synthesis_frame_start_wait,
            result,
            reinterpret_cast<std::uintptr_t>(fence.Get()),
            last_submitted_fence_value,
            fence->GetCompletedValue());
        if (FAILED(result) &&
            result != HRESULT_FROM_WIN32(ERROR_BUSY)) {
            synthesis_enabled = false;
        }
        return result;
    }

    [[nodiscard]] HRESULT destination_available(
        const Destination& destination) noexcept {
        const HRESULT status = fence_status(fence.Get(), destination.fence_value);
        if (status == S_FALSE) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        if (FAILED(status)) {
            synthesis_enabled = false;
        }
        return status;
    }

    [[nodiscard]] HRESULT acquire_work_slot(std::uint32_t* output_index) noexcept {
        if (output_index == nullptr) {
            return E_POINTER;
        }
        for (std::uint32_t offset = 0; offset < work_slots.size(); ++offset) {
            const std::uint32_t index =
                (next_work_slot + offset) %
                static_cast<std::uint32_t>(work_slots.size());
            const HRESULT status =
                fence_status(fence.Get(), work_slots[index].fence_value);
            if (status == S_OK) {
                const HRESULT timing_result =
                    harvest_nvidia_gpu_timing(work_slots[index]);
                if (FAILED(timing_result)) {
                    work_slots[index].timing_pending = false;
                }
                *output_index = index;
                return S_OK;
            }
            if (FAILED(status)) {
                synthesis_enabled = false;
                return status;
            }
        }
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    [[nodiscard]] HRESULT acquire_source(
        const D3D12HistoryCaptureTicket& ticket,
        std::span<const D3D12ReprojectionView> source_views,
        RollingSource* output_source) noexcept {
        if (output_source == nullptr) {
            return E_POINTER;
        }
        output_source->clear();
        if (!valid_view_layout(
                source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize)) {
            return E_INVALIDARG;
        }
        ID3D12Resource* raw_resource = nullptr;
        ID3D12Fence* raw_fence = nullptr;
        HRESULT result = history->acquire_consumer(
            ticket,
            &output_source->lease,
            &raw_resource,
            &raw_fence);
        if (FAILED(result)) {
            return result;
        }
        output_source->ticket = ticket;
        output_source->resource.Attach(raw_resource);
        output_source->producer_fence.Attach(raw_fence);
        output_source->view_count = static_cast<UINT>(source_views.size());
        std::copy(
            source_views.begin(),
            source_views.end(),
            output_source->views.begin());
        if (!same_adapter(device.Get(), output_source->resource.Get()) ||
            !same_adapter(device.Get(), output_source->producer_fence.Get()) ||
            !copy_compatible(
                image_description,
                output_source->resource->GetDesc()) ||
            !compatible_view_format(
                output_source->resource->GetDesc().Format,
                view_format)) {
            history->cancel_consumer(output_source->lease);
            output_source->clear();
            return E_INVALIDARG;
        }
        return S_OK;
    }

    void create_source_views(
        WorkSlot& slot,
        ID3D12Resource* previous_resource,
        ID3D12Resource* current_resource,
        UINT blocks = 0) noexcept {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.Format = view_format;
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Texture2DArray.MostDetailedMip = 0;
        description.Texture2DArray.MipLevels = 1;
        description.Texture2DArray.FirstArraySlice = 0;
        description.Texture2DArray.ArraySize = image_description.DepthOrArraySize;
        description.Texture2DArray.PlaneSlice = 0;
        description.Texture2DArray.ResourceMinLODClamp = 0.0F;
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        const UINT block_count = std::max<UINT>(blocks, image_description.DepthOrArraySize);
        for (UINT block = 0; block < block_count; ++block) {
            const UINT base = block * kDescriptorBlockSize;
            device->CreateShaderResourceView(
                previous_resource,
                &description,
                offset_cpu_handle(start, base, descriptor_increment));
            device->CreateShaderResourceView(
                current_resource,
                &description,
                offset_cpu_handle(start, base + 1, descriptor_increment));
        }
    }

    [[nodiscard]] HRESULT create_game_motion_views(
        WorkSlot& slot,
        const DlssMotionVectorSet& frames,
        UINT view_count) noexcept {
        if (frames.eye_count == 0 ||
            !game_motion_eyes_match(frames.eye_count, view_count) ||
            frames.eye_count > kDlssMotionVectorEyeCount) {
            return E_INVALIDARG;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT eye = 0; eye < frames.eye_count; ++eye) {
            const auto& frame = frames.eyes[eye];
            if (!frame || !frame->motion_vectors ||
                !same_device(device.Get(), frame->motion_vectors.Get())) {
                return E_INVALIDARG;
            }
            const D3D12_RESOURCE_DESC resource = frame->motion_vectors->GetDesc();
            const DXGI_FORMAT format = motion_vector_view_format(resource.Format);
            if (format == DXGI_FORMAT_UNKNOWN ||
                resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                resource.MipLevels == 0 ||
                frame->motion_slice >= resource.DepthOrArraySize ||
                frame->motion_width == 0 || frame->motion_height == 0 ||
                static_cast<std::uint64_t>(frame->motion_x) +
                        frame->motion_width > resource.Width ||
                static_cast<std::uint64_t>(frame->motion_y) +
                        frame->motion_height > resource.Height) {
                return E_INVALIDARG;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC description{};
            description.Format = format;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            description.Texture2DArray.MostDetailedMip = 0;
            description.Texture2DArray.MipLevels = 1;
            description.Texture2DArray.FirstArraySlice = frame->motion_slice;
            description.Texture2DArray.ArraySize = 1;
            description.Texture2DArray.PlaneSlice = 0;
            device->CreateShaderResourceView(frame->motion_vectors.Get(), &description,
                offset_cpu_handle(start,
                    eye * kDescriptorBlockSize + 6U,
                    descriptor_increment));
        }
        return S_OK;
    }

    // NOTE: the synthetic destination is drawn into while the private
    // swapchains rest in COMMON, which is not a state D3D12 promotes to
    // RENDER_TARGET implicitly - it promotes into the copy and shader-read
    // states, not that one. Bracketing the draws with an explicit
    // COMMON -> RENDER_TARGET -> COMMON pair is correct by the spec and was
    // tried in V148: it cost about a third of the frame rate and made frame
    // times erratic, which is what a barrier through COMMON does to a large
    // render target - a full decompress, and on this path a serialisation
    // against the other queue that gives back the overlap the private queue
    // exists to win.
    //
    // So the draws are left as they are. This is undefined behaviour that
    // NVIDIA tolerates, and the whole dedicated-queue path is NVIDIA-only by
    // decision, but it is undefined behaviour and it is written down here
    // rather than discovered again. The way out is not a cheaper barrier: it
    // is to stop writing runtime-owned images from the private queue at all,
    // compositing into layer-owned intermediates that can carry
    // ALLOW_SIMULTANEOUS_ACCESS and copying across on the application's
    // queue - which is what the D3D11 interop path already does.

    // One texture can hold the views side by side while the producer
    // publishes a guide per view; each view then uses its own guide, as
    // native generation maps them.
    [[nodiscard]] bool side_by_side_guides(UINT eye_count, UINT view_count) const noexcept {
        return image_description.DepthOrArraySize == 1 && view_count > 1 &&
            eye_count == view_count;
    }
    [[nodiscard]] bool game_motion_eyes_match(UINT eye_count, UINT view_count) const noexcept {
        return eye_count == image_description.DepthOrArraySize ||
            side_by_side_guides(eye_count, view_count);
    }
    // The guide eye, and so the descriptor block, a view of the pair uses.
    [[nodiscard]] UINT game_motion_guide(std::span<const D3D12ReprojectionView> views,
        UINT view_index, UINT eye_count) const noexcept {
        return side_by_side_guides(eye_count, static_cast<UINT>(views.size()))
            ? view_index
            : resolved_array_slice(views[view_index], view_index, views.size(),
                  image_description.DepthOrArraySize);
    }

    [[nodiscard]] bool valid_game_motion_pair(
        const RollingSource& previous_source,
        const RollingSource& current_source) const noexcept {
        const auto& previous_vectors = previous_source.motion_vectors;
        const auto& current = current_source.motion_vectors;
        if (!previous_vectors || !current || previous_vectors->eye_count == 0 ||
            previous_vectors->eye_count != current->eye_count ||
            !game_motion_eyes_match(current->eye_count, current_source.view_count) ||
            current->eye_count > kDlssMotionVectorEyeCount) {
            return false;
        }
        for (UINT eye = 0; eye < current->eye_count; ++eye) {
            const auto& a = previous_vectors->eyes[eye];
            const auto& b = current->eyes[eye];
            if (!a || !b || b->reset || a->stream != b->stream ||
                a->epoch != b->epoch || a->serial == 0 ||
                b->previous_serial != a->serial || b->serial <= a->serial ||
                !b->motion_vectors || !b->producer_queue ||
                !same_device(device.Get(), b->motion_vectors.Get())) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool has_complete_game_motion_set(
        const RollingSource& source) const noexcept {
        const auto& frames = source.motion_vectors;
        if (!frames || frames->eye_count == 0 ||
            frames->eye_count != image_description.DepthOrArraySize ||
            frames->eye_count > kDlssMotionVectorEyeCount) {
            return false;
        }
        for (UINT eye = 0; eye < frames->eye_count; ++eye) {
            const auto& frame = frames->eyes[eye];
            if (!frame || frame->stream == 0 || frame->epoch == 0 ||
                frame->serial == 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool unchanged_game_motion_pair(
        const RollingSource& previous_source,
        const RollingSource& current_source) const noexcept {
        const auto& previous_vectors = previous_source.motion_vectors;
        const auto& current = current_source.motion_vectors;
        if (!previous_vectors || !current || previous_vectors->eye_count == 0 ||
            previous_vectors->eye_count != current->eye_count ||
            current->eye_count != image_description.DepthOrArraySize ||
            current->eye_count > kDlssMotionVectorEyeCount) {
            return false;
        }
        for (UINT eye = 0; eye < current->eye_count; ++eye) {
            const auto& a = previous_vectors->eyes[eye];
            const auto& b = current->eyes[eye];
            if (!a || !b || a->reset || b->reset || a->stream != b->stream ||
                a->epoch != b->epoch || a->serial == 0 ||
                a->serial != b->serial ||
                a->previous_serial != b->previous_serial) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] HRESULT record_game_motion_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        const auto& guides = current_source.motion_vectors;
        if (!guides || !valid_game_motion_pair(previous_source, current_source)) {
            return E_INVALIDARG;
        }
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector begin");
        ID3D12Resource* const previous_resource = previous_source.resource.Get();
        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        ID3D12Resource* const synthetic_destination =
            synthetic_destinations[synthetic_destination_index].resource.Get();
        for (UINT eye = 0; eye < guides->eye_count; ++eye) {
            ID3D12Resource* const motion_resource = guides->eyes[eye]->motion_vectors.Get();
            if (motion_resource == previous_resource || motion_resource == current_resource ||
                motion_resource == current_destination ||
                motion_resource == synthetic_destination) {
                return E_INVALIDARG;
            }
        }
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            const UINT eye = game_motion_guide(target_views, view_index, guides->eye_count);
            if (eye >= guides->eye_count) return E_INVALIDARG;
            const auto& guide = guides->eyes[eye];
            const D3D12ImageRect rect = resolved_rect(
                target_views[view_index].image_rect,
                static_cast<UINT>(image_description.Width), image_description.Height);
            const std::uint64_t rect_right =
                static_cast<std::uint64_t>(rect.offset_x) + rect.width;
            const std::uint64_t rect_bottom =
                static_cast<std::uint64_t>(rect.offset_y) + rect.height;
            const std::uint64_t guide_right =
                static_cast<std::uint64_t>(guide->output_x) + guide->output_width;
            const std::uint64_t guide_bottom =
                static_cast<std::uint64_t>(guide->output_y) + guide->output_height;
            if (rect.width == 0 || rect.height == 0 ||
                rect.offset_x < guide->output_x || rect.offset_y < guide->output_y ||
                rect_right > guide_right || rect_bottom > guide_bottom) {
                return E_INVALIDARG;
            }
        }
        create_source_views(slot, previous_resource, current_resource, guides->eye_count);
        HRESULT result = create_game_motion_views(slot, *guides,
            static_cast<UINT>(target_views.size()));
        if (FAILED(result)) return result;
        // NVIDIA work slots also own a second command list. This route bypasses
        // OFA, but the list was reset with the slot and must still be closed
        // before the slot can be reused on a later frame.
        if (backend == D3D12OpticalFlowBackend::nvidia) {
            result = slot.synthesis_command_list->Close();
            if (FAILED(result)) return result;
        }

        std::array<D3D12_RESOURCE_BARRIER, 4> before{};
        UINT before_count = 0;
        before[before_count++] = transition_barrier(previous_resource,
            D3D12_RESOURCE_STATE_COMMON, kShaderReadState);
        before[before_count++] = transition_barrier(current_resource,
            D3D12_RESOURCE_STATE_COMMON, kShaderReadState);
        for (UINT eye = 0; eye < guides->eye_count; ++eye) {
            const auto& guide = guides->eyes[eye];
            if (guide->resource_state != kShaderReadState &&
                (eye == 0 || guide->motion_vectors.Get() !=
                                 guides->eyes[0]->motion_vectors.Get())) {
                before[before_count++] = transition_barrier(guide->motion_vectors.Get(),
                    guide->resource_state, kShaderReadState);
            }
        }
        slot.command_list->ResourceBarrier(before_count, before.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(root_signature.Get());
        const auto gpu_start = slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetPipelineState(game_motion_graphics_pipeline.Get());
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector compose");
        slot.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.flow_block_size = 1;
        parameters.use_game_motion = 1;
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                previous_source.views[view_index], target_views[view_index],
                static_cast<UINT>(image_description.Width), image_description.Height);
        }

        const auto rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv =
                synthetic_output_destination(output, synthetic_destination_index) * image_description.DepthOrArraySize;
            for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
                const UINT slice = resolved_array_slice(target_views[view_index], view_index,
                    target_views.size(), image_description.DepthOrArraySize);
                const UINT eye = game_motion_guide(target_views, view_index, guides->eye_count);
                if (eye >= guides->eye_count) return E_INVALIDARG;
                const auto& guide = guides->eyes[eye];
                const UINT descriptor_base = eye * kDescriptorBlockSize;
                slot.command_list->SetGraphicsRootDescriptorTable(0,
                    offset_gpu_handle(gpu_start, descriptor_base, descriptor_increment));
                slot.command_list->SetGraphicsRootDescriptorTable(1,
                    offset_gpu_handle(gpu_start, descriptor_base + kSrvDescriptorCount,
                        descriptor_increment));
                parameters.slice = slice;
                parameters.view_index = view_index;
                // The optical-flow fields are unused by the game-motion branch.
                // Reuse them to carry the DLSS output rectangle associated with
                // the stable eye stream without growing the root constants.
                parameters.flow_width = guide->output_x;
                parameters.flow_height = guide->output_y;
                parameters.flow_block_size = guide->output_width;
                parameters.game_motion_padding = guide->output_height;
                parameters.game_motion_rect = {
                    static_cast<float>(guide->motion_x),
                    static_cast<float>(guide->motion_y),
                    static_cast<float>(guide->motion_width),
                    static_cast<float>(guide->motion_height)};
                // DLSS low-resolution vectors are expressed in render-space pixel
                // units after applying MV_Scale. Synthesis and pose reprojection
                // operate in the final output rectangle's pixel space, so convert
                // both vector components into that same coordinate system.
                parameters.game_motion_scale = {
                    guide->scale_x * static_cast<float>(guide->output_width) /
                        static_cast<float>(guide->motion_width),
                    guide->scale_y * static_cast<float>(guide->output_height) /
                        static_cast<float>(guide->motion_height)};
                parameters.game_motion_jitter_delta = guide->jittered
                    ? std::array<float, 2>{
                          guide->jitter_x - guide->previous_jitter_x,
                          guide->jitter_y - guide->previous_jitter_y}
                    : std::array<float, 2>{};
                slot.command_list->SetGraphicsRoot32BitConstants(2,
                    kSynthesisConstantCount, &parameters, 0);
                const auto rtv = offset_cpu_handle(rtv_start, first_rtv + slice, rtv_increment);
                slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(slot.command_list.Get(), target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width), image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(slot.command_list.Get(), rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
        }

        // As the other paths: the span ends before the current copy.
        end_timing_span(slot);
        std::array<D3D12_RESOURCE_BARRIER, 5> after{};
        UINT after_count = 0;
        after[after_count++] = transition_barrier(previous_resource,
            kShaderReadState, D3D12_RESOURCE_STATE_COMMON);
        after[after_count++] = transition_barrier(current_resource,
            kShaderReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        after[after_count++] = transition_barrier(current_destination,
            release_state, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT eye = 0; eye < guides->eye_count; ++eye) {
            const auto& guide = guides->eyes[eye];
            if (guide->resource_state != kShaderReadState &&
                (eye == 0 || guide->motion_vectors.Get() !=
                                 guides->eyes[0]->motion_vectors.Get())) {
                after[after_count++] = transition_barrier(guide->motion_vectors.Get(),
                    kShaderReadState, guide->resource_state);
            }
        }
        slot.command_list->ResourceBarrier(after_count, after.data());
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector current copy");
        slot.command_list->CopyResource(current_destination, current_resource);
        const std::array<D3D12_RESOURCE_BARRIER, 2> finish{
            transition_barrier(current_resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(current_destination, D3D12_RESOURCE_STATE_COPY_DEST,
                release_state)};
        slot.command_list->ResourceBarrier(static_cast<UINT>(finish.size()), finish.data());
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector complete");
        return S_OK;
    }

    // Ends the pair's GPU span and resolves its two marks for readback. The
    // NVIDIA flow path resolves its own stage marks on its synthesis list.
    void end_timing_span(WorkSlot& slot) noexcept {
        if (!nvidia_gpu_timing_enabled || !nvidia_timestamp_heap) return;
        slot.command_list->EndQuery(nvidia_timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            slot.timing_query_base + kSpanEnd);
        slot.command_list->ResolveQueryData(nvidia_timestamp_heap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP, slot.timing_query_base + kSpanBegin, 2,
            nvidia_timestamp_readback.Get(),
            UINT64(slot.timing_query_base + kSpanBegin) * sizeof(std::uint64_t));
        slot.timing_resolved = true;
    }

    [[nodiscard]] HRESULT reset_work_slot(WorkSlot& slot) noexcept {
        slot.timing_resolved = false;
        HRESULT result = slot.allocator->Reset();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.command_list->Reset(slot.allocator.Get(), nullptr);
        if (SUCCEEDED(result) && nvidia_gpu_timing_enabled &&
            nvidia_timestamp_heap) {
            // First mark on the list both backends record into, so the span
            // starts where the GPU starts this pair's work.
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kSpanBegin);
        }
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_allocator->Reset();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_command_list->Reset(
            slot.current_copy_allocator.Get(),
            nullptr);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        if (slot.synthesis_allocator) {
            result = slot.synthesis_allocator->Reset();
            if (FAILED(result)) {
                synthesis_enabled = false;
                return result;
            }
            result = slot.synthesis_command_list->Reset(
                slot.synthesis_allocator.Get(),
                nullptr);
            if (FAILED(result)) {
                synthesis_enabled = false;
            }
        }
        return result;
    }

    [[nodiscard]] HRESULT record_prime(
        WorkSlot& slot,
        const RollingSource& source,
        ID3D12Resource* destination) noexcept {
        if (native_dlss) {
            native_pair_generated = false;
            native_dlss->reset();
        }
        if (backend == D3D12OpticalFlowBackend::nvidia || native_dlss) {
            dred_marker(slot.command_list.Get(), "OFXR NVIDIA prime copy");
            const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{
                transition_barrier(
                    source.resource.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(
                    destination,
                    release_state,
                    D3D12_RESOURCE_STATE_COPY_DEST),
            };
            slot.command_list->ResourceBarrier(
                static_cast<UINT>(before_copy.size()),
                before_copy.data());
            slot.command_list->CopyResource(destination, source.resource.Get());
            const std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
                transition_barrier(
                    source.resource.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COMMON),
                transition_barrier(
                    destination,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    release_state),
            };
            slot.command_list->ResourceBarrier(
                static_cast<UINT>(after_copy.size()),
                after_copy.data());
            dred_marker(slot.command_list.Get(), "OFXR NVIDIA prime complete");
            return slot.synthesis_command_list ? slot.synthesis_command_list->Close() : S_OK;
        }

        dred_marker(slot.command_list.Get(), "OFXR FidelityFX prime begin");

        create_source_views(
            slot,
            source.resource.Get(),
            source.resource.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{
            transition_barrier(
                source.resource.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState),
            transition_barrier(
                destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_copy.size()),
            before_copy.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetComputeRootSignature(root_signature.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetComputeRootDescriptorTable(0, gpu_start);
        slot.command_list->SetComputeRootDescriptorTable(
            1,
            offset_gpu_handle(
                gpu_start,
                kSrvDescriptorCount,
                descriptor_increment));
        slot.command_list->SetPipelineState(pack_pipeline.Get());

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction) |
            pack_input_flags(view_format);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        for (UINT view_index = 0; view_index < source.view_count;
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                source.views[view_index],
                source.views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        slot.command_list->SetComputeRoot32BitConstants(
            2,
            kSynthesisConstantCount,
            &parameters,
            0);
        slot.command_list->Dispatch(
            (packed_width + 7U) / 8U,
            (packed_height + 7U) / 8U,
            1);

        dred_marker(slot.command_list.Get(), "OFXR FidelityFX prime optical-flow");

        const D3D12_RESOURCE_BARRIER before_fidelityfx = transition_barrier(
            packed_color.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot.command_list->ResourceBarrier(1, &before_fidelityfx);

        // FidelityFX suppresses optical flow through frame index five. Seed
        // its temporal history with the prime frame so the first real A/B pair
        // reaches the first usable B dispatch without presenting warm-up flow.
        for (UINT seed_index = 0; seed_index < 6U; ++seed_index) {
            const HRESULT seed_result = dispatch_fidelityfx(
                slot.command_list.Get(),
                FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            if (FAILED(seed_result)) {
                return seed_result;
            }
        }
        const D3D12_RESOURCE_BARRIER before_source_copy = transition_barrier(
            source.resource.Get(),
            kShaderReadState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot.command_list->ResourceBarrier(1, &before_source_copy);
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX prime current copy");
        slot.command_list->CopyResource(destination, source.resource.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
            transition_barrier(
                source.resource.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(
                destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(after_copy.size()),
            after_copy.data());
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX prime complete");
        return S_OK;
    }

    [[nodiscard]] HRESULT dispatch_fidelityfx(
        ID3D12GraphicsCommandList* command_list,
        FfxResourceStates output_state) noexcept {
        FfxOpticalflowDispatchDescription dispatch{};
        dispatch.commandList = ffxGetCommandListDX12(command_list);
        dispatch.color = ffxGetResourceDX12(
            packed_color.Get(),
            ffxGetResourceDescriptionDX12(packed_color.Get()),
            L"XRFG packed color",
            FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.opticalFlowVector = ffxGetResourceDX12(
            optical_flow_vector.Get(),
            ffxGetResourceDescriptionDX12(optical_flow_vector.Get()),
            L"XRFG FidelityFX optical flow",
            output_state);
        dispatch.opticalFlowSCD = ffxGetResourceDX12(
            optical_flow_scene_change.Get(),
            ffxGetResourceDescriptionDX12(optical_flow_scene_change.Get()),
            L"XRFG FidelityFX scene change",
            output_state);
        dispatch.reset = false;
        dispatch.backbufferTransferFunction =
            FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance = {0.0F, 1.0F};
        return ffx_result(ffxOpticalflowContextDispatch(
            &ffx_context,
            &dispatch));
    }

    [[nodiscard]] HRESULT record_nvidia_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        if (slot.synthesis_command_list == nullptr) {
            return E_UNEXPECTED;
        }
        dred_marker(slot.command_list.Get(), "OFXR NVIDIA pack begin");
        ID3D12Resource* const previous_resource = previous_source.resource.Get();
        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        create_source_views(slot, previous_resource, current_resource);
        if (nvidia_gpu_timing_enabled) {
            slot.timing_metadata = {};
            slot.timing_metadata.previous_serial = previous_source.ticket.serial;
            slot.timing_metadata.current_serial = current_source.ticket.serial;
            slot.timing_metadata.eye_count = image_description.DepthOrArraySize;
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampPackBegin);
        }

        std::array<D3D12_RESOURCE_BARRIER,
                   2U + 2U * kMaxReprojectionViews> before_pack{};
        UINT before_pack_count = 0;
        before_pack[before_pack_count++] = transition_barrier(
            previous_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        before_pack[before_pack_count++] = transition_barrier(
            current_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            before_pack[before_pack_count++] = transition_barrier(
                eye.previous_input.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            before_pack[before_pack_count++] = transition_barrier(
                eye.current_input.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        slot.command_list->ResourceBarrier(before_pack_count, before_pack.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetComputeRootSignature(root_signature.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetPipelineState(
            nvidia_input_format == DXGI_FORMAT_R8_UNORM
                ? nvidia_luma_pack_pipeline.Get()
                : nvidia_pack_pipeline.Get());

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction) |
            pack_input_flags(view_format);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        for (UINT view_index = 0;
             view_index < static_cast<UINT>(target_views.size());
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                previous_source.views[view_index],
                target_views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            parameters.slice = eye_index;
            const D3D12_GPU_DESCRIPTOR_HANDLE block = offset_gpu_handle(
                gpu_start,
                eye_index * kDescriptorBlockSize,
                descriptor_increment);
            slot.command_list->SetComputeRootDescriptorTable(0, block);
            slot.command_list->SetComputeRootDescriptorTable(
                1,
                offset_gpu_handle(
                    block,
                    kSrvDescriptorCount,
                    descriptor_increment));
            slot.command_list->SetComputeRoot32BitConstants(
                2,
                kSynthesisConstantCount,
                &parameters,
                0);
            slot.command_list->Dispatch(
                (packed_width + 7U) / 8U,
                (packed_height + 7U) / 8U,
                1);
        }

        std::array<D3D12_RESOURCE_BARRIER,
                   2U * kMaxReprojectionViews> uav_barriers{};
        UINT uav_barrier_count = 0;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            uav_barriers[uav_barrier_count++] =
                uav_barrier(eye.previous_input.Get());
            uav_barriers[uav_barrier_count++] =
                uav_barrier(eye.current_input.Get());
        }
        slot.command_list->ResourceBarrier(
            uav_barrier_count,
            uav_barriers.data());

        std::array<D3D12_RESOURCE_BARRIER,
                   2U + 2U * kMaxReprojectionViews> after_pack{};
        UINT after_pack_count = 0;
        after_pack[after_pack_count++] = transition_barrier(
            previous_resource,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        after_pack[after_pack_count++] = transition_barrier(
            current_resource,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            after_pack[after_pack_count++] = transition_barrier(
                eye.previous_input.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
            after_pack[after_pack_count++] = transition_barrier(
                eye.current_input.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
        }
        slot.command_list->ResourceBarrier(after_pack_count, after_pack.data());
        dred_marker(slot.command_list.Get(), "OFXR NVIDIA pack complete");
        if (nvidia_gpu_timing_enabled) {
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampPackEnd);
        }

        ID3D12GraphicsCommandList* const synthesis =
            slot.synthesis_command_list.Get();
        dred_marker(synthesis, "OFXR NVIDIA composition begin");
        if (nvidia_gpu_timing_enabled) {
            synthesis->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base +
                    kNvidiaTimestampCompositionBegin);
        }
        std::array<D3D12_RESOURCE_BARRIER,
                   2U + 4U * kMaxReprojectionViews> before_synthesis{};
        UINT before_synthesis_count = 0;
        before_synthesis[before_synthesis_count++] = transition_barrier(
            previous_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        before_synthesis[before_synthesis_count++] = transition_barrier(
            current_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            before_synthesis[before_synthesis_count++] = transition_barrier(
                eye.flow.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            before_synthesis[before_synthesis_count++] = transition_barrier(
                eye.cost.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (nvidia_options.bidirectional) {
                before_synthesis[before_synthesis_count++] = transition_barrier(
                    eye.backward_flow.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                before_synthesis[before_synthesis_count++] = transition_barrier(
                    eye.backward_cost.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        synthesis->ResourceBarrier(
            before_synthesis_count,
            before_synthesis.data());

        synthesis->SetDescriptorHeaps(1, heaps);
        synthesis->SetGraphicsRootSignature(root_signature.Get());
        synthesis->SetPipelineState(
            nvidia_options.bidirectional
                ? nvidia_bidirectional_graphics_pipeline.Get()
                : nvidia_graphics_pipeline.Get());
        dred_marker(synthesis, "OFXR NVIDIA composition draw");
        synthesis->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv =
                synthetic_output_destination(output, synthetic_destination_index) *
                image_description.DepthOrArraySize;
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size());
                 ++view_index) {
                const UINT slice = resolved_array_slice(
                    target_views[view_index],
                    view_index,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                parameters.slice = slice;
                parameters.view_index = view_index;
                const D3D12_GPU_DESCRIPTOR_HANDLE block = offset_gpu_handle(
                    gpu_start,
                    slice * kDescriptorBlockSize,
                    descriptor_increment);
                synthesis->SetGraphicsRootDescriptorTable(0, block);
                synthesis->SetGraphicsRootDescriptorTable(
                    1,
                    offset_gpu_handle(
                        block,
                        kSrvDescriptorCount,
                        descriptor_increment));
                synthesis->SetGraphicsRoot32BitConstants(
                    2,
                    kSynthesisConstantCount,
                    &parameters,
                    0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset_cpu_handle(
                    rtv_start,
                    first_rtv + slice,
                    rtv_increment);
                synthesis->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(
                    synthesis,
                    target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                synthesis->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(synthesis, rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
        }

        std::array<D3D12_RESOURCE_BARRIER,
                   3U + 4U * kMaxReprojectionViews> before_current_copy{};
        UINT before_current_copy_count = 0;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            before_current_copy[before_current_copy_count++] =
                transition_barrier(
                    eye.flow.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            before_current_copy[before_current_copy_count++] =
                transition_barrier(
                    eye.cost.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            if (nvidia_options.bidirectional) {
                before_current_copy[before_current_copy_count++] =
                    transition_barrier(
                        eye.backward_flow.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
                before_current_copy[before_current_copy_count++] =
                    transition_barrier(
                        eye.backward_cost.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
            }
        }
        before_current_copy[before_current_copy_count++] = transition_barrier(
            previous_resource,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        synthesis->ResourceBarrier(
            before_current_copy_count,
            before_current_copy.data());
        if (nvidia_gpu_timing_enabled && nvidia_timestamp_heap) {
            // Last mark of the pair's synthesis, before the current copy
            // that is submitted separately. The span therefore covers
            // exactly the work the synthetic frame is waiting on.
            synthesis->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kSpanEnd);
        }
        // The copy's own transitions travel with the copy, so the resources
        // are left in their normal states while it waits to be submitted.
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy_states{
            transition_barrier(
                current_resource,
                kShaderReadState,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(
                current_destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(before_copy_states.size()),
            before_copy_states.data());
        // Recorded here but submitted only after the synthetic frame has gone
        // to the runtime; see flush_current_copy.
        dred_marker(slot.current_copy_command_list.Get(), "OFXR NVIDIA current copy");
        slot.current_copy_command_list->CopyResource(
            current_destination,
            current_resource);
        // DIAGNOSTIC. The real frame is otherwise indistinguishable from one
        // that never arrived: the purple marker rides only on the synthetic,
        // so a steady purple square means either "only synthetics are shown"
        // or "both are shown and 45 Hz reads as steady". Green, offset to the
        // right of the purple one, separates those two readings in one look.
        if (debug_marker) {
            constexpr float green[4]{0.0F, 0.9F, 0.2F, 1.0F};
            const auto marker_to_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_to_rt);
            const D3D12_CPU_DESCRIPTOR_HANDLE marker_rtv_start =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const UINT marker_first_rtv = current_rtv_base +
                current_destination_index * image_description.DepthOrArraySize;
            for (UINT marker_view = 0;
                 marker_view < static_cast<UINT>(target_views.size());
                 ++marker_view) {
                const UINT marker_slice = resolved_array_slice(
                    target_views[marker_view],
                    marker_view,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                clear_synthetic_marker(
                    slot.current_copy_command_list.Get(),
                    offset_cpu_handle(
                        marker_rtv_start,
                        marker_first_rtv + marker_slice,
                        rtv_increment),
                    target_views[marker_view],
                    static_cast<UINT>(image_description.Width),
                    image_description.Height,
                    debug_marker,
                    green,
                    1.6F);
            }
            const auto marker_from_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_from_rt);
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_current_copy{
            transition_barrier(
                current_resource,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(after_current_copy.size()),
            after_current_copy.data());
        if (nvidia_gpu_timing_enabled) {
            synthesis->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampCompositionEnd);
            synthesis->ResolveQueryData(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base,
                kTimestampCount,
                nvidia_timestamp_readback.Get(),
                static_cast<UINT64>(slot.timing_query_base) *
                    sizeof(std::uint64_t));
        }
        dred_marker(synthesis, "OFXR NVIDIA composition complete");
        return S_OK;
    }

    [[nodiscard]] HRESULT record_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker,
        bool* used_game_motion) noexcept {
        if (used_game_motion == nullptr) return E_POINTER;
        *used_game_motion = false;
        slot.timing_span_only = backend != D3D12OpticalFlowBackend::nvidia || native_dlss;
        if (native_dlss) {
            native_pair_generated = false;
            if (nvidia_gpu_timing_enabled) {
                slot.timing_metadata = {};
                slot.timing_metadata.previous_serial = previous_source.ticket.serial;
                slot.timing_metadata.current_serial = current_source.ticket.serial;
                slot.timing_metadata.eye_count = current_source.view_count;
            }
            const auto current_views = std::span(current_source.views.data(), current_source.view_count);
            const bool full_coverage = views_cover_resource(current_views,
                static_cast<UINT>(image_description.Width), image_description.Height,
                image_description.DepthOrArraySize);
            // Cropped outputs need B outside the submitted rectangles. Fully
            // covered outputs are written by composition and need no pre-copy.
            auto* b = current_source.resource.Get();
            const auto copy = [&](ID3D12GraphicsCommandList* list, ID3D12Resource* destination) {
                const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                    transition_barrier(b, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition_barrier(destination, release_state, D3D12_RESOURCE_STATE_COPY_DEST)};
                list->ResourceBarrier(2, before.data());
                list->CopyResource(destination, b);
                const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                    transition_barrier(b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
                    transition_barrier(destination, D3D12_RESOURCE_STATE_COPY_DEST, release_state)};
                list->ResourceBarrier(2, after.data());
            };
            std::array<D3D12NativeDlssG::Output, 2> outputs{};
            for (UINT output=0; output<synthetic_output_count(); ++output) {
                outputs[output].image = synthetic_destinations[
                    synthetic_output_destination(output, synthetic_destination_index)].resource.Get();
                if (!full_coverage) copy(slot.command_list.Get(), outputs[output].image);
            }
            const UINT work_slot = static_cast<UINT>(&slot - work_slots.data());
            const HRESULT generated = native_dlss->record(slot.command_list.Get(), work_slot,
                previous_source.resource.Get(), b,
                {previous_source.views.data(), previous_source.view_count},
                {current_source.views.data(), current_source.view_count},
                previous_source.motion_vectors.get(), current_source.motion_vectors.get(),
                {outputs.data(), synthetic_output_count()}, release_state);
            if (FAILED(generated)) return generated;
            native_pair_generated = generated == S_OK;
            if (!native_pair_generated) {
                // A skipped pair never writes the outputs; show the current frame.
                if (full_coverage) for (UINT output = 0; output < synthetic_output_count(); ++output)
                    copy(slot.command_list.Get(), outputs[output].image);
                switch (native_dlss->last_skip()) {
                case D3D12NativeDlssG::Skip::resizing:
                    break;
                case D3D12NativeDlssG::Skip::multi_frame_unsupported:
                    report_dlss_motion_vector_status(DlssMotionVectorStatus::multi_frame_unsupported);
                    break;
                case D3D12NativeDlssG::Skip::feature_unavailable:
                case D3D12NativeDlssG::Skip::evaluate_failed:
                    report_dlss_motion_vector_status(DlssMotionVectorStatus::native_unavailable);
                    break;
                default: {
                    bool missing_depth = false;
                    for (const auto* guides : {previous_source.motion_vectors.get(), current_source.motion_vectors.get()}) {
                        if (!guides) continue;
                        for (UINT eye=0; eye<std::min(guides->eye_count,kDlssMotionVectorEyeCount); ++eye)
                            missing_depth |= guides->eyes[eye] && !guides->eyes[eye]->depth;
                    }
                    report_dlss_motion_vector_status(missing_depth ? DlssMotionVectorStatus::waiting_for_depth :
                        current_source.motion_vectors ? DlssMotionVectorStatus::invalid_input :
                            DlssMotionVectorStatus::waiting_for_dlss);
                    break;
                }
                }
            }
            // A skipped pair generated nothing, so it reports no time.
            if (native_pair_generated) end_timing_span(slot);
            // This list is submitted after the synthetic when requested by the
            // presenter. Keeping the real copy out of the synthesis span also
            // makes its timing and completion fence reflect the actual order.
            copy(slot.current_copy_command_list.Get(), current_destinations[current_destination_index].resource.Get());
            if (slot.synthesis_command_list) {
                const HRESULT closed=slot.synthesis_command_list->Close();
                if (FAILED(closed)) return closed;
            }
            *used_game_motion = true; // execute the direct list, bypass OFA
            return S_OK;
        }
        if (valid_game_motion_pair(previous_source, current_source)) {
            const HRESULT result = record_game_motion_pair(slot, previous_source,
                current_source, target_views, synthetic_destination_index,
                current_destination_index, debug_marker);
            if (SUCCEEDED(result)) {
                *used_game_motion = true;
                slot.timing_span_only = true;
                if (nvidia_gpu_timing_enabled) {
                    slot.timing_metadata = {};
                    slot.timing_metadata.previous_serial = previous_source.ticket.serial;
                    slot.timing_metadata.current_serial = current_source.ticket.serial;
                    slot.timing_metadata.eye_count = current_source.view_count;
                }
                return result;
            }
            report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        } else if (current_source.motion_vectors) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::temporal_mismatch);
        }
        if (backend == D3D12OpticalFlowBackend::nvidia) {
            return record_nvidia_pair(
                slot,
                previous_source,
                current_source,
                target_views,
                synthetic_destination_index,
                current_destination_index,
                debug_marker);
        }
        ID3D12Resource* const previous_resource = previous_source.resource.Get();
        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        create_source_views(slot, previous_resource, current_resource);
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX pair begin");

        const std::array<D3D12_RESOURCE_BARRIER, 2> before_synthesis{
            transition_barrier(
                previous_resource,
                D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState),
            transition_barrier(
                current_resource,
                D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_synthesis.size()),
            before_synthesis.data());

        const D3D12_RESOURCE_BARRIER before_pack = transition_barrier(
            packed_color.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.command_list->ResourceBarrier(1, &before_pack);

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetComputeRootSignature(root_signature.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetComputeRootDescriptorTable(0, gpu_start);
        slot.command_list->SetComputeRootDescriptorTable(
            1,
            offset_gpu_handle(
                gpu_start,
                kSrvDescriptorCount,
                descriptor_increment));
        slot.command_list->SetPipelineState(pack_pipeline.Get());

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction) |
            pack_input_flags(view_format);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        parameters.slice = 0;
        for (UINT view_index = 0;
             view_index < static_cast<UINT>(target_views.size());
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                previous_source.views[view_index],
                target_views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        slot.command_list->SetComputeRoot32BitConstants(
            2,
            kSynthesisConstantCount,
            &parameters,
            0);
        slot.command_list->Dispatch(
            (packed_width + 7U) / 8U,
            (packed_height + 7U) / 8U,
            1);
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX packed input complete");
        const D3D12_RESOURCE_BARRIER before_fidelityfx = transition_barrier(
            packed_color.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot.command_list->ResourceBarrier(1, &before_fidelityfx);

        const FfxResourceStates first_output_state = flow_output_consumed
            ? FFX_RESOURCE_STATE_PIXEL_READ
            : FFX_RESOURCE_STATE_UNORDERED_ACCESS;
        HRESULT result = dispatch_fidelityfx(
            slot.command_list.Get(),
            first_output_state);
        if (FAILED(result)) {
            return result;
        }
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX optical-flow complete");

        if (!flow_output_consumed) {
            const std::array<D3D12_RESOURCE_BARRIER, 2> before_pixel{
                transition_barrier(
                    optical_flow_vector.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                transition_barrier(
                    optical_flow_scene_change.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            slot.command_list->ResourceBarrier(
                static_cast<UINT>(before_pixel.size()),
                before_pixel.data());
        }
        flow_output_consumed = true;

        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(root_signature.Get());
        slot.command_list->SetGraphicsRootDescriptorTable(0, gpu_start);
        slot.command_list->SetGraphicsRootDescriptorTable(
            1,
            offset_gpu_handle(
                gpu_start,
                kSrvDescriptorCount,
                descriptor_increment));
        slot.command_list->SetPipelineState(graphics_pipeline.Get());
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX composition draw");
        slot.command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv =
                synthetic_output_destination(output, synthetic_destination_index) *
                image_description.DepthOrArraySize;
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size());
                 ++view_index) {
                const UINT slice = resolved_array_slice(
                    target_views[view_index],
                    view_index,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                parameters.slice = slice;
                parameters.view_index = view_index;
                slot.command_list->SetGraphicsRoot32BitConstants(
                    2,
                    kSynthesisConstantCount,
                    &parameters,
                    0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset_cpu_handle(
                    rtv_start,
                    first_rtv + slice,
                    rtv_increment);
                slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(
                    slot.command_list.Get(),
                    target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(slot.command_list.Get(), rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
        }

        const std::array<D3D12_RESOURCE_BARRIER, 1> before_current_copy{
            transition_barrier(
                previous_resource,
                kShaderReadState,
                D3D12_RESOURCE_STATE_COMMON),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_current_copy.size()),
            before_current_copy.data());
        // Last mark of the pair's synthesis, before the current copy that is
        // submitted separately. The span therefore covers exactly the work
        // the synthetic frame is waiting on.
        end_timing_span(slot);
        // The copy's own transitions travel with the copy, so the resources
        // are left in their normal states while it waits to be submitted.
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy_states{
            transition_barrier(
                current_resource,
                kShaderReadState,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(
                current_destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(before_copy_states.size()),
            before_copy_states.data());
        // Recorded here but submitted only after the synthetic frame has gone
        // to the runtime; see flush_current_copy.
        dred_marker(slot.current_copy_command_list.Get(), "OFXR FidelityFX current copy");
        slot.current_copy_command_list->CopyResource(
            current_destination,
            current_resource);
        // DIAGNOSTIC. The real frame is otherwise indistinguishable from one
        // that never arrived: the purple marker rides only on the synthetic,
        // so a steady purple square means either "only synthetics are shown"
        // or "both are shown and 45 Hz reads as steady". Green, offset to the
        // right of the purple one, separates those two readings in one look.
        if (debug_marker) {
            constexpr float green[4]{0.0F, 0.9F, 0.2F, 1.0F};
            const auto marker_to_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_to_rt);
            const D3D12_CPU_DESCRIPTOR_HANDLE marker_rtv_start =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const UINT marker_first_rtv = current_rtv_base +
                current_destination_index * image_description.DepthOrArraySize;
            for (UINT marker_view = 0;
                 marker_view < static_cast<UINT>(target_views.size());
                 ++marker_view) {
                const UINT marker_slice = resolved_array_slice(
                    target_views[marker_view],
                    marker_view,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                clear_synthetic_marker(
                    slot.current_copy_command_list.Get(),
                    offset_cpu_handle(
                        marker_rtv_start,
                        marker_first_rtv + marker_slice,
                        rtv_increment),
                    target_views[marker_view],
                    static_cast<UINT>(image_description.Width),
                    image_description.Height,
                    debug_marker,
                    green,
                    1.6F);
            }
            const auto marker_from_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_from_rt);
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_current_copy{
            transition_barrier(
                current_resource,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(after_current_copy.size()),
            after_current_copy.data());
        dred_marker(slot.command_list.Get(), "OFXR FidelityFX pair complete");
        return S_OK;
    }

    [[nodiscard]] HRESULT record_repeated_pair(
        WorkSlot& slot,
        const RollingSource& retained_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        if (native_dlss) native_pair_generated = false;
        slot.timing_span_only = true;
        if (nvidia_gpu_timing_enabled) {
            slot.timing_metadata = {};
            slot.timing_metadata.previous_serial = retained_source.ticket.serial;
            slot.timing_metadata.current_serial = retained_source.ticket.serial;
            slot.timing_metadata.eye_count = retained_source.view_count;
        }
        ID3D12Resource* const source = retained_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        create_source_views(slot, source, source);
        dred_marker(slot.command_list.Get(), "OFXR repeated pair begin");

        const std::array<D3D12_RESOURCE_BARRIER, 2> before_current_copy{
            transition_barrier(
                source,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(
                current_destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_current_copy.size()),
            before_current_copy.data());
        slot.command_list->CopyResource(current_destination, source);
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_synthesis{
            transition_barrier(
                source,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                kShaderReadState),
            transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_synthesis.size()),
            before_synthesis.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(root_signature.Get());
        slot.command_list->SetPipelineState(
            backend == D3D12OpticalFlowBackend::nvidia
                ? (nvidia_options.bidirectional
                       ? nvidia_bidirectional_graphics_pipeline.Get()
                       : nvidia_graphics_pipeline.Get())
                : graphics_pipeline.Get());
        slot.command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        parameters.synthesis_flags |= 1U;
        for (UINT view_index = 0;
             view_index < static_cast<UINT>(target_views.size());
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                retained_source.views[view_index],
                target_views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }

        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv =
                synthetic_output_destination(output, synthetic_destination_index) *
                image_description.DepthOrArraySize;
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size());
                 ++view_index) {
                const UINT slice = resolved_array_slice(
                    target_views[view_index],
                    view_index,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                parameters.slice = slice;
                parameters.view_index = view_index;
                const UINT descriptor_base =
                    backend == D3D12OpticalFlowBackend::nvidia
                        ? slice * kDescriptorBlockSize
                        : 0;
                const D3D12_GPU_DESCRIPTOR_HANDLE block = offset_gpu_handle(
                    gpu_start,
                    descriptor_base,
                    descriptor_increment);
                slot.command_list->SetGraphicsRootDescriptorTable(0, block);
                slot.command_list->SetGraphicsRootDescriptorTable(
                    1,
                    offset_gpu_handle(
                        block,
                        kSrvDescriptorCount,
                        descriptor_increment));
                slot.command_list->SetGraphicsRoot32BitConstants(
                    2,
                    kSynthesisConstantCount,
                    &parameters,
                    0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset_cpu_handle(
                    rtv_start,
                    first_rtv + slice,
                    rtv_increment);
                slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(
                    slot.command_list.Get(),
                    target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(slot.command_list.Get(), rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
        }

        const D3D12_RESOURCE_BARRIER after_synthesis = transition_barrier(
            source,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        slot.command_list->ResourceBarrier(1, &after_synthesis);
        // This path copies the current frame first, so its span includes it.
        end_timing_span(slot);
        dred_marker(slot.command_list.Get(), "OFXR repeated pair complete");
        return S_OK;
    }

    // Submits the recorded work. The current output's copy is closed either
    // way, but with defer_current_copy it is held back for flush_current_copy
    // to submit once the synthetic frame has reached the runtime. Two fence
    // values are reserved in that case: the first is signalled here and marks
    // the synthetic's pixels done, the second is what the copy will signal and
    // is what the slot, the destination and the history lease are tracked
    // against, so none of them come free until the copy has actually run.
    [[nodiscard]] HRESULT execute_and_signal(
        WorkSlot& slot,
        std::uint32_t slot_index,
        RollingSource* newly_acquired,
        std::uint64_t* output_fence_value,
        bool defer_current_copy) noexcept {
        if (newly_acquired == nullptr || output_fence_value == nullptr ||
            slot.current_copy_command_list == nullptr ||
            next_fence_value == 0 ||
            next_fence_value >=
                std::numeric_limits<std::uint64_t>::max() - 1U) {
            synthesis_enabled = false;
            return E_FAIL;
        }

        HRESULT result = slot.command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        ID3D12CommandList* lists[] = {slot.command_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        completion_unknown = true;

        std::uint64_t value = 0;
        if (defer_current_copy) {
            // Signal the synthetic first: its pixels exist now, and it goes
            // to the runtime a display period before the current frame.
            const std::uint64_t synthetic_value = next_fence_value;
            HRESULT signal_result =
                queue->Signal(fence.Get(), synthetic_value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
            ++next_fence_value;
            last_synthetic_fence_value = synthetic_value;
            value = next_fence_value;
            ++next_fence_value;
            pending_copy_slot = slot_index;
            pending_copy_fence_value = value;
        } else {
            ID3D12CommandList* copy_lists[] = {
                slot.current_copy_command_list.Get()};
            queue->ExecuteCommandLists(1, copy_lists);
            value = next_fence_value;
            ++next_fence_value;
            last_synthetic_fence_value = value;
            HRESULT signal_result = queue->Signal(fence.Get(), value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
        }

        completion_unknown = false;
        slot.fence_value = value;
        // Only a slot that resolved a span has timing to read back: a prime,
        // or a skipped native pair, wrote a begin mark and nothing else.
        slot.timing_pending = nvidia_gpu_timing_enabled && slot.timing_resolved;
        last_submitted_fence_value = value;
        *output_fence_value = value;
        return S_OK;
    }

    // Submits a copy left pending by execute_and_signal and signals the value
    // the rest of the pipeline is already tracking against it.
    [[nodiscard]] HRESULT flush_pending_copy() noexcept {
        if (pending_copy_slot == kNoPendingCopy) {
            return S_OK;
        }
        if (pending_copy_slot >= work_slots.size() || !synthesis_enabled) {
            pending_copy_slot = kNoPendingCopy;
            return E_FAIL;
        }
        WorkSlot& slot = work_slots[pending_copy_slot];
        const std::uint64_t value = pending_copy_fence_value;
        pending_copy_slot = kNoPendingCopy;
        ID3D12CommandList* copy_lists[] = {
            slot.current_copy_command_list.Get()};
        queue->ExecuteCommandLists(1, copy_lists);
        const HRESULT result = queue->Signal(fence.Get(), value);
        if (FAILED(result)) {
            synthesis_enabled = false;
        }
        return result;
    }

    [[nodiscard]] HRESULT execute_nvidia_and_signal(
        WorkSlot& slot,
        std::uint32_t slot_index,
        RollingSource* newly_acquired,
        std::uint64_t* output_fence_value,
        bool defer_current_copy) noexcept {
        if (newly_acquired == nullptr || output_fence_value == nullptr ||
            slot.synthesis_command_list == nullptr ||
            slot.current_copy_command_list == nullptr ||
            next_fence_value == 0 ||
            next_fence_value >=
                std::numeric_limits<std::uint64_t>::max() - 1U ||
            next_nvidia_fence_value == 0 ||
            next_nvidia_fence_value >
                std::numeric_limits<std::uint64_t>::max() -
                    image_description.DepthOrArraySize) {
            synthesis_enabled = false;
            return E_FAIL;
        }

        HRESULT result = slot.command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.synthesis_command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = queue->Wait(nvidia_fence.Get(), last_nvidia_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }

        ID3D12CommandList* pack_lists[] = {slot.command_list.Get()};
        queue->ExecuteCommandLists(1, pack_lists);
        completion_unknown = true;

        const std::uint64_t pack_ready_value = next_fence_value;
        result = queue->Signal(fence.Get(), pack_ready_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            untracked_source = std::move(*newly_acquired);
            return result;
        }
        ++next_fence_value;

        if (nvidia_gpu_timing_enabled) {
            result = slot.timing_marker_allocator->Reset();
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
            result = slot.timing_marker_command_list->Reset(
                slot.timing_marker_allocator.Get(),
                nullptr);
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
            slot.timing_marker_command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampEye0End);
            result = slot.timing_marker_command_list->Close();
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
        }

        std::uint64_t preceding_eye_value = 0;
        std::uint64_t eye0_fence_value = 0;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            std::array<NV_OF_FENCE_POINT, 2> input_fence_points{};
            input_fence_points[0].fence = fence.Get();
            input_fence_points[0].value = pack_ready_value;
            UINT input_fence_count = 1;
            if (preceding_eye_value != 0) {
                input_fence_points[input_fence_count].fence =
                    nvidia_fence.Get();
                input_fence_points[input_fence_count].value =
                    preceding_eye_value;
                ++input_fence_count;
            }

            NV_OF_EXECUTE_INPUT_PARAMS_D3D12 input{};
            input.inputFrame = eye.current_input_handle;
            input.referenceFrame = eye.previous_input_handle;
            input.disableTemporalHints = NV_OF_TRUE;
            input.numFencePoints = input_fence_count;
            input.fencePoint = input_fence_points.data();

            NV_OF_FENCE_POINT output_fence_point{};
            output_fence_point.fence = nvidia_fence.Get();
            output_fence_point.value = next_nvidia_fence_value;
            NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 output{};
            output.outputBuffer = eye.flow_handle;
            output.outputCostBuffer = eye.cost_handle;
            if (nvidia_options.bidirectional) {
                output.bwdOutputBuffer = eye.backward_flow_handle;
                output.bwdOutputCostBuffer = eye.backward_cost_handle;
            }
            output.fencePoint = &output_fence_point;
            result = nvidia_result(nvidia_api.nvOFExecuteD3D12(
                nvidia_contexts[eye_index],
                &input,
                &output));
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
            preceding_eye_value = next_nvidia_fence_value;
            if (eye_index == 0) {
                eye0_fence_value = next_nvidia_fence_value;
                if (nvidia_gpu_timing_enabled) {
                    result = queue->Wait(
                        nvidia_fence.Get(), eye0_fence_value);
                    if (FAILED(result)) {
                        synthesis_enabled = false;
                        untracked_source = std::move(*newly_acquired);
                        return result;
                    }
                    ID3D12CommandList* timing_lists[] = {
                        slot.timing_marker_command_list.Get()};
                    queue->ExecuteCommandLists(1, timing_lists);
                }
            }
            last_nvidia_fence_value = next_nvidia_fence_value;
            ++next_nvidia_fence_value;
        }
        result = queue->Wait(nvidia_fence.Get(), last_nvidia_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            untracked_source = std::move(*newly_acquired);
            return result;
        }
        ID3D12CommandList* synthesis_lists[] = {
            slot.synthesis_command_list.Get()};
        queue->ExecuteCommandLists(1, synthesis_lists);

        // The synthetic's pixels exist once this signals. The current copy
        // is held back so it is not queued ahead of the synthetic frame.
        std::uint64_t completion_value = 0;
        if (defer_current_copy) {
            // Signal the synthetic first: its pixels exist now, and it goes
            // to the runtime a display period before the current frame.
            const std::uint64_t synthetic_value = next_fence_value;
            HRESULT signal_result =
                queue->Signal(fence.Get(), synthetic_value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
            ++next_fence_value;
            last_synthetic_fence_value = synthetic_value;
            completion_value = next_fence_value;
            ++next_fence_value;
            pending_copy_slot = slot_index;
            pending_copy_fence_value = completion_value;
        } else {
            ID3D12CommandList* copy_lists[] = {
                slot.current_copy_command_list.Get()};
            queue->ExecuteCommandLists(1, copy_lists);
            completion_value = next_fence_value;
            ++next_fence_value;
            last_synthetic_fence_value = completion_value;
            HRESULT signal_result = queue->Signal(fence.Get(), completion_value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
        }

        completion_unknown = false;
        slot.fence_value = completion_value;
        slot.timing_pending = nvidia_gpu_timing_enabled;
        last_submitted_fence_value = completion_value;
        *output_fence_value = completion_value;
        return S_OK;
    }

    [[nodiscard]] HRESULT submit_prime(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* output_ticket,
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors) noexcept {
        if (output_ticket == nullptr) {
            return E_POINTER;
        }
        *output_ticket = {};
        if (!synthesis_enabled || previous.active() || untracked_source.active() ||
            current.serial == 0 || current.fence_value == 0 ||
            !valid_view_layout(
                current_source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            current_destination_index >= current_destinations.size()) {
            return E_INVALIDARG;
        }
        // Before anything is asked about outstanding work. A copy still
        // pending holds its work slot and history lease, and the value it
        // will signal is what wait_for_previous_submission tests, so a caller
        // that never flushed would otherwise read as permanently busy rather
        // than costing a frame of latency.
        static_cast<void>(flush_pending_copy());
        HRESULT result =
            destination_available(current_destinations[current_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        std::uint32_t work_slot_index = 0;
        result = acquire_work_slot(&work_slot_index);
        if (FAILED(result)) {
            return result;
        }

        RollingSource next{};
        result = acquire_source(current, current_source_views, &next);
        if (FAILED(result)) {
            return result;
        }
        next.motion_vectors = std::move(motion_vectors);
        const auto cancel_next = [this, &next]() noexcept {
            history->cancel_consumer(next.lease);
            next.clear();
        };
        if (next.resource.Get() ==
            current_destinations[current_destination_index].resource.Get()) {
            cancel_next();
            return E_INVALIDARG;
        }

        WorkSlot& slot = work_slots[work_slot_index];
        result = reset_work_slot(slot);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }
        result = record_prime(
            slot,
            next,
            current_destinations[current_destination_index].resource.Get());
        if (FAILED(result)) {
            synthesis_enabled = false;
            cancel_next();
            return result;
        }
        result = queue->Wait(next.producer_fence.Get(), current.fence_value);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }

        std::uint64_t fence_value = 0;
        result = execute_and_signal(
            slot, work_slot_index, &next, &fence_value, false);
        if (FAILED(result)) {
            if (next.active()) {
                cancel_next();
            }
            return result;
        }
        next.last_use_fence_value = fence_value;
        previous = std::move(next);
        current_destinations[current_destination_index].fence_value = fence_value;
        next_work_slot = (work_slot_index + 1U) %
            static_cast<std::uint32_t>(work_slots.size());

        output_ticket->previous_serial = 0;
        output_ticket->current_serial = current.serial;
        output_ticket->fence_value = fence_value;
        output_ticket->synthetic_fence_value = last_synthetic_fence_value;
        output_ticket->work_slot = work_slot_index;
        output_ticket->synthetic_destination_index =
            std::numeric_limits<std::uint32_t>::max();
        output_ticket->current_destination_index = current_destination_index;
        return S_OK;
    }

    [[nodiscard]] HRESULT submit_pair(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::span<const D3D12ReprojectionView> synthetic_target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* output_ticket,
        const std::optional<OverlayPlacement>& debug_marker,
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors,
        bool defer_current_copy,
        float interpolation_fraction,
        const std::optional<D3D12ExtraSynthetic>& extra_synthetic) noexcept {
        // A degenerate interval says the pairing is not in a steady cadence;
        // half is the safe answer there, not an extrapolation.
        const auto usable_fraction = [](float fraction) noexcept {
            return fraction > 0.05F && fraction < 0.95F ? fraction : 0.5F;
        };
        synthetic_fraction = usable_fraction(interpolation_fraction);
        extra_synthetic_destination.reset();
        if (output_ticket == nullptr) {
            return E_POINTER;
        }
        *output_ticket = {};
        if (extra_synthetic) {
            if (extra_synthetic->destination_index >= synthetic_destinations.size() ||
                extra_synthetic->destination_index == synthetic_destination_index) {
                return E_INVALIDARG;
            }
            extra_synthetic_destination = extra_synthetic->destination_index;
            extra_synthetic_fraction =
                usable_fraction(extra_synthetic->interpolation_fraction);
        }
        const bool repeated_capture =
            previous.active() && current.serial == previous.ticket.serial &&
            current.fence_value == previous.ticket.fence_value &&
            current.slot == previous.ticket.slot &&
            current.source_index == previous.ticket.source_index;
        const bool advanced_capture =
            previous.active() &&
            previous.ticket.serial != std::numeric_limits<std::uint64_t>::max() &&
            current.serial == previous.ticket.serial + 1U &&
            current.slot != previous.ticket.slot;
        if (!synthesis_enabled || !previous.active() || untracked_source.active() ||
            current.serial == 0 || current.fence_value == 0 ||
            previous.ticket.serial == std::numeric_limits<std::uint64_t>::max() ||
            (!repeated_capture && !advanced_capture) ||
            previous.view_count !=
                static_cast<UINT>(current_source_views.size()) ||
            synthetic_target_views.size() != current_source_views.size() ||
            !valid_view_layout(
                current_source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            !valid_view_layout(
                synthetic_target_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            synthetic_destination_index >= synthetic_destinations.size() ||
            current_destination_index >= current_destinations.size()) {
            return E_INVALIDARG;
        }
        const std::span<const D3D12ReprojectionView> previous_views(
            previous.views.data(),
            previous.view_count);
        if (!same_view_layout(
                previous_views,
                current_source_views,
                image_description.DepthOrArraySize) ||
            !same_view_layout(
                current_source_views,
                synthetic_target_views,
                image_description.DepthOrArraySize)) {
            return E_INVALIDARG;
        }
        for (std::size_t view_index = 0;
             view_index < current_source_views.size();
             ++view_index) {
            const D3D12ReprojectionView& current_view =
                current_source_views[view_index];
            const D3D12ReprojectionView& target_view =
                synthetic_target_views[view_index];
            if (!valid_view(current_view) || !valid_view(target_view) ||
                !rect_within_resource(
                    current_view.image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height) ||
                !rect_within_resource(
                    target_view.image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height) ||
                !same_camera(current_view, target_view)) {
                return E_INVALIDARG;
            }
        }
        // Before anything is asked about outstanding work. A copy still
        // pending holds its work slot and history lease, and the value it
        // will signal is what wait_for_previous_submission tests, so a caller
        // that never flushed would otherwise read as permanently busy rather
        // than costing a frame of latency.
        static_cast<void>(flush_pending_copy());
        HRESULT result =
            destination_available(current_destinations[current_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        result = destination_available(
            synthetic_destinations[synthetic_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        if (extra_synthetic_destination) {
            result = destination_available(
                synthetic_destinations[*extra_synthetic_destination]);
            if (FAILED(result)) {
                return result;
            }
        }
        ID3D12Resource* const extra_destination = extra_synthetic_destination
            ? synthetic_destinations[*extra_synthetic_destination].resource.Get()
            : nullptr;
        std::uint32_t work_slot_index = 0;
        result = acquire_work_slot(&work_slot_index);
        if (FAILED(result)) {
            return result;
        }

        if (repeated_capture) {
            ID3D12Resource* const repeated_resource = previous.resource.Get();
            ID3D12Resource* const synthetic_destination =
                synthetic_destinations[synthetic_destination_index].resource.Get();
            ID3D12Resource* const current_destination =
                current_destinations[current_destination_index].resource.Get();
            if (repeated_resource == synthetic_destination ||
                repeated_resource == current_destination ||
                repeated_resource == extra_destination) {
                return E_INVALIDARG;
            }

            WorkSlot& slot = work_slots[work_slot_index];
            result = reset_work_slot(slot);
            if (FAILED(result)) {
                return result;
            }
            result = record_repeated_pair(
                slot,
                previous,
                synthetic_target_views,
                synthetic_destination_index,
                current_destination_index,
                debug_marker);
            if (FAILED(result)) {
                synthesis_enabled = false;
                return result;
            }
            if (slot.synthesis_command_list != nullptr) {
                result = slot.synthesis_command_list->Close();
                if (FAILED(result)) {
                    synthesis_enabled = false;
                    return result;
                }
            }

            RollingSource no_new_source{};
            std::uint64_t fence_value = 0;
            result = execute_and_signal(
                slot, work_slot_index, &no_new_source, &fence_value, false);
            if (FAILED(result)) {
                return result;
            }

            previous.ticket = current;
            previous.view_count = static_cast<UINT>(current_source_views.size());
            std::copy(
                current_source_views.begin(),
                current_source_views.end(),
                previous.views.begin());
            previous.last_use_fence_value = fence_value;
            current_destinations[current_destination_index].fence_value = fence_value;
            synthetic_destinations[synthetic_destination_index].fence_value = fence_value;
            if (extra_synthetic_destination) {
                synthetic_destinations[*extra_synthetic_destination].fence_value =
                    fence_value;
            }
            next_work_slot = (work_slot_index + 1U) %
                static_cast<std::uint32_t>(work_slots.size());

            output_ticket->previous_serial = current.serial;
            output_ticket->current_serial = current.serial;
            output_ticket->fence_value = fence_value;
            output_ticket->work_slot = work_slot_index;
            output_ticket->synthetic_destination_index =
                synthetic_destination_index;
            output_ticket->current_destination_index = current_destination_index;
            return S_OK;
        }

        RollingSource next{};
        result = acquire_source(current, current_source_views, &next);
        if (FAILED(result)) {
            return result;
        }
        next.motion_vectors = std::move(motion_vectors);
        const auto cancel_next = [this, &next]() noexcept {
            history->cancel_consumer(next.lease);
            next.clear();
        };
        ID3D12Resource* const synthetic_destination =
            synthetic_destinations[synthetic_destination_index].resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        if (previous.resource.Get() == next.resource.Get() ||
            previous.resource.Get() == synthetic_destination ||
            previous.resource.Get() == current_destination ||
            next.resource.Get() == synthetic_destination ||
            next.resource.Get() == current_destination ||
            (extra_destination != nullptr &&
             (previous.resource.Get() == extra_destination ||
              next.resource.Get() == extra_destination))) {
            cancel_next();
            return E_INVALIDARG;
        }

        WorkSlot& slot = work_slots[work_slot_index];
        result = reset_work_slot(slot);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }
        bool used_game_motion = false;
        result = record_pair(
            slot,
            previous,
            next,
            synthetic_target_views,
            synthetic_destination_index,
            current_destination_index,
            debug_marker,
            &used_game_motion);
        if (FAILED(result)) {
            synthesis_enabled = false;
            cancel_next();
            return result;
        }
        result = queue->Wait(
            previous.producer_fence.Get(),
            previous.ticket.fence_value);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }
        result = queue->Wait(next.producer_fence.Get(), current.fence_value);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }

        std::uint64_t fence_value = 0;
        result = backend == D3D12OpticalFlowBackend::nvidia && !used_game_motion
                     ? execute_nvidia_and_signal(
                           slot, work_slot_index, &next, &fence_value,
                           defer_current_copy)
                     : execute_and_signal(
                           slot, work_slot_index, &next, &fence_value,
                           defer_current_copy);
        if (FAILED(result)) {
            if (next.active()) {
                cancel_next();
            }
            return result;
        }
        // A skipped pair can still hold NGX history work, so every native
        // pair's fence guards the eye resources a later resize releases.
        if (native_dlss) native_dlss->submitted(fence.Get(), fence_value);
        if (used_game_motion && (!native_dlss || native_pair_generated)) report_dlss_motion_vector_use();

        next.last_use_fence_value = fence_value;
        result = history->retire_consumer(
            previous.lease,
            fence.Get(),
            fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            untracked_source = std::move(next);
            return result;
        }

        const std::uint64_t previous_serial = previous.ticket.serial;
        previous = std::move(next);
        current_destinations[current_destination_index].fence_value = fence_value;
        synthetic_destinations[synthetic_destination_index].fence_value = fence_value;
        if (extra_synthetic_destination) {
            synthetic_destinations[*extra_synthetic_destination].fence_value =
                fence_value;
        }
        next_work_slot = (work_slot_index + 1U) %
            static_cast<std::uint32_t>(work_slots.size());

        output_ticket->previous_serial = previous_serial;
        output_ticket->current_serial = current.serial;
        output_ticket->fence_value = fence_value;
        output_ticket->synthetic_fence_value = last_synthetic_fence_value;
        output_ticket->work_slot = work_slot_index;
        output_ticket->synthetic_destination_index = synthetic_destination_index;
        output_ticket->current_destination_index = current_destination_index;
        return S_OK;
    }

    [[nodiscard]] HRESULT retire_previous() noexcept {
        if (completion_unknown || untracked_source.active()) {
            synthesis_enabled = false;
            return HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);
        }
        if (!previous.active()) return S_OK;
        if (previous.last_use_fence_value == 0 || fence == nullptr) {
            synthesis_enabled = false;
            return E_UNEXPECTED;
        }
        const HRESULT result = history->retire_consumer(
            previous.lease,
            fence.Get(),
            previous.last_use_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        previous.clear();
        return S_OK;
    }

    [[nodiscard]] HRESULT wait_for_idle() noexcept {
        // last_submitted_fence_value is the value a deferred copy will signal,
        // so waiting for it without submitting that copy would never return.
        static_cast<void>(flush_pending_copy());
        HRESULT result = retire_previous();
        if (FAILED(result)) {
            return result;
        }
        result = wait_for_fence(fence.Get(), last_submitted_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        if (backend == D3D12OpticalFlowBackend::nvidia) {
            result = wait_for_fence(
                nvidia_fence.Get(),
                last_nvidia_fence_value);
            if (FAILED(result)) {
                synthesis_enabled = false;
                return result;
            }
        }
        result = history->wait_for_idle();
        if (FAILED(result)) {
            synthesis_enabled = false;
        }
        return result;
    }
};

D3D12FrameSynthesizer::D3D12FrameSynthesizer() noexcept = default;

D3D12FrameSynthesizer::~D3D12FrameSynthesizer() {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr && FAILED(impl_->wait_for_idle())) {
            // Keep every resource and active history lease alive when an
            // executed submission cannot be proven complete.
            static_cast<void>(impl_.release());
        }
    } catch (...) {
        // Destruction must not let synchronization failures escape.
    }
}

HRESULT D3D12FrameSynthesizer::initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    std::shared_ptr<D3D12SwapchainHistory> history,
    std::span<ID3D12Resource* const> current_destination_images,
    std::span<ID3D12Resource* const> synthetic_destination_images,
    DXGI_FORMAT view_format,
    D3D12_RESOURCE_STATES release_state,
    D3D12OpticalFlowBackend backend,
    D3D12NvidiaOpticalFlowOptions nvidia_options,
    bool enable_nvidia_gpu_timing) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        auto candidate = std::make_unique<Impl>();
        const HRESULT result = candidate->initialize(
            device,
            queue,
            std::move(history),
            current_destination_images,
            synthetic_destination_images,
            view_format,
            release_state,
            backend,
            nvidia_options,
            enable_nvidia_gpu_timing);
        if (FAILED(result)) {
            return result;
        }
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::reconfigure(D3D12OpticalFlowBackend backend,
    D3D12NvidiaOpticalFlowOptions options) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_) return E_UNEXPECTED;
        const HRESULT idle = impl_->wait_for_idle();
        if (FAILED(idle)) return idle; // never free pending GPU work
        std::vector<ID3D12Resource*> current, synthetic;
        for (const auto& d : impl_->current_destinations) current.push_back(d.resource.Get());
        for (const auto& d : impl_->synthetic_destinations) synthetic.push_back(d.resource.Get());
        auto candidate = std::make_unique<Impl>();
        const HRESULT result = candidate->initialize(impl_->device.Get(), impl_->queue.Get(),
            impl_->history, current, synthetic, impl_->view_format, impl_->release_state,
            backend, options, impl_->nvidia_gpu_timing_enabled);
        if (FAILED(result)) return result;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT D3D12FrameSynthesizer::reconfigure_gpu_timing(bool enabled) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_) return E_UNEXPECTED;
        const HRESULT idle = impl_->wait_for_idle();
        if (FAILED(idle)) return idle; // never free pending GPU work
        std::vector<ID3D12Resource*> current, synthetic;
        for (const auto& d : impl_->current_destinations) current.push_back(d.resource.Get());
        for (const auto& d : impl_->synthetic_destinations) synthetic.push_back(d.resource.Get());
        auto candidate = std::make_unique<Impl>();
        const HRESULT result = candidate->initialize(impl_->device.Get(), impl_->queue.Get(),
            impl_->history, current, synthetic, impl_->view_format, impl_->release_state,
            impl_->backend, impl_->nvidia_options, enabled);
        if (FAILED(result)) return result;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

bool D3D12FrameSynthesizer::gpu_timing_enabled() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ && impl_->nvidia_gpu_timing_enabled;
    } catch (...) {
        return false;
    }
}

HRESULT D3D12FrameSynthesizer::submit_prime(
    const D3D12HistoryCaptureTicket& current,
    std::span<const D3D12ReprojectionView> current_source_views,
    std::uint32_t current_destination_index,
    D3D12FrameSynthesisTicket* ticket,
    std::shared_ptr<const DlssMotionVectorSet> motion_vectors) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (ticket != nullptr) {
                *ticket = {};
            }
            return E_UNEXPECTED;
        }
        return impl_->submit_prime(
            current,
            current_source_views,
            current_destination_index,
            ticket,
            std::move(motion_vectors));
    } catch (...) {
        if (ticket != nullptr) {
            *ticket = {};
        }
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::submit_pair(
    const D3D12HistoryCaptureTicket& current,
    std::span<const D3D12ReprojectionView> current_source_views,
    std::span<const D3D12ReprojectionView> synthetic_target_views,
    std::uint32_t synthetic_destination_index,
    std::uint32_t current_destination_index,
    D3D12FrameSynthesisTicket* ticket,
    std::optional<OverlayPlacement> debug_marker,
    std::shared_ptr<const DlssMotionVectorSet> motion_vectors,
    bool defer_current_copy,
    float interpolation_fraction,
    std::optional<D3D12ExtraSynthetic> extra_synthetic) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (ticket != nullptr) {
                *ticket = {};
            }
            return E_UNEXPECTED;
        }
        return impl_->submit_pair(
            current,
            current_source_views,
            synthetic_target_views,
            synthetic_destination_index,
            current_destination_index,
            ticket,
            debug_marker,
            std::move(motion_vectors),
            defer_current_copy,
            interpolation_fraction,
            extra_synthetic);
    } catch (...) {
        if (ticket != nullptr) {
            *ticket = {};
        }
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::flush_current_copy(
    ID3D12CommandQueue* consumer_queue,
    std::uint64_t copy_fence_value) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            return S_OK;
        }
        const HRESULT result = impl_->flush_pending_copy();
        // Now that the copy is on the queue, the value it will signal is a
        // value something is actually working towards, so a consumer can
        // wait on it without parking.
        //
        // This pair's value, never the synthesiser's latest. Several pairs
        // can be outstanding at once, and the copy is usually already
        // submitted by the time this runs, so flush_pending_copy above is a
        // no-op and the latest value belongs to a *later* pair whose
        // synthesis has not started. Joining the application's queue to that
        // blocks the real frame inside xrEndFrame for a whole synthesis
        // cycle, and the compositor keeps the synthetic and discards the real
        // frame - measured as the real half's hand-over p90 going 1.0 to
        // 4.8 ms with a 14 ms tail while the synthetic's stayed flat, costing
        // 12-18 real frames a second at 13.6 megapixels an eye.
        if (SUCCEEDED(result) && consumer_queue != nullptr &&
            consumer_queue != impl_->queue.Get() &&
            impl_->fence != nullptr &&
            copy_fence_value != 0) {
            static_cast<void>(consumer_queue->Wait(
                impl_->fence.Get(),
                copy_fence_value));
        }
        return result;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::retire_previous() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? S_OK : impl_->retire_previous();
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::wait_for_idle() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? S_OK : impl_->wait_for_idle();
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::wait_for_previous_submission(
    std::uint32_t timeout_milliseconds) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr
            ? S_OK
            : impl_->wait_for_previous_submission(timeout_milliseconds);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::consume_nvidia_gpu_timing(
    D3D12NvidiaGpuTiming* timing) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (timing != nullptr) {
                *timing = {};
            }
            return E_UNEXPECTED;
        }
        return impl_->consume_nvidia_gpu_timing(timing);
    } catch (...) {
        if (timing != nullptr) {
            *timing = {};
        }
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::synchronize_producer_queue(
    ID3D12CommandQueue* queue) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr || queue == nullptr) {
            return E_INVALIDARG;
        }
        if (impl_->queue == nullptr || impl_->acquire_fence == nullptr) {
            return E_UNEXPECTED;
        }
        // Same queue on both sides means the acquire already applies; the
        // signal and wait would be redundant rather than wrong.
        if (queue == impl_->queue.Get()) {
            return S_FALSE;
        }
        const std::uint64_t value = impl_->next_acquire_fence_value;
        const HRESULT signal_result =
            queue->Signal(impl_->acquire_fence.Get(), value);
        if (FAILED(signal_result)) {
            return signal_result;
        }
        ++impl_->next_acquire_fence_value;
        return impl_->queue->Wait(impl_->acquire_fence.Get(), value);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::synchronize_consumer_queue(
    ID3D12CommandQueue* queue,
    const D3D12FrameSynthesisTicket& ticket) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr || queue == nullptr) {
            return E_INVALIDARG;
        }
        // The synthetic's value, never the pair's, and it only matters while
        // the current copy is deferred. Deferred, fence_value is what that
        // copy will signal and the copy is not submitted until the presenter
        // flushes it a display period later, so waiting on it here would park
        // the application's own queue for that whole window, every frame, and
        // the game could not start rendering until it cleared.
        //
        // Submitted inline - every D3D11 title, and any session running the
        // deeper pipeline - the two values are the same, because the synthetic
        // and the copy are signalled together. The choice is then moot, and
        // the application's queue waits for the copy as well as the synthesis:
        // one resource copy more than it used to, against a synthesis cycle
        // saved. Keep preferring the synthetic's value, because the deferred
        // path is still what native D3D12 runs with the depth off.
        const std::uint64_t value = ticket.synthetic_fence_value != 0
            ? ticket.synthetic_fence_value
            : ticket.fence_value;
        if (value == 0 || impl_->fence == nullptr) {
            return E_UNEXPECTED;
        }
        // Same queue on both sides means the submission is already ordered;
        // the wait would be redundant rather than wrong.
        if (queue == impl_->queue.Get()) {
            return S_FALSE;
        }
        return queue->Wait(impl_->fence.Get(), value);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::completion_fence(
    ID3D12Fence** output_fence) const noexcept {
    try {
        if (output_fence == nullptr) {
            return E_POINTER;
        }
        *output_fence = nullptr;
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr || impl_->fence == nullptr) {
            return E_UNEXPECTED;
        }
        *output_fence = impl_->fence.Get();
        (*output_fence)->AddRef();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

bool D3D12FrameSynthesizer::initialized() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ != nullptr && impl_->synthesis_enabled;
    } catch (...) {
        return false;
    }
}

}  // namespace xrfg
