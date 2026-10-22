#include "xrfg/d3d12_eye_check.hpp"
#include "xrfg/d3d11_d3d12_interop.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "eye_check_compare_shader.hpp"

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kDescriptors = 4;  // the image, two tiles, the result
constexpr UINT kResultFloats = 10;
constexpr UINT kResultBytes = 64;

// eye_check.hlsl's Params.
struct Params {
    UINT view_rect[2][4];
    UINT view_slice[2];
    UINT tile_size[2];
    UINT tile_origin[4];
    UINT output_size[4];
};
static_assert(sizeof(Params) == 20 * 4);

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// A format a tile can be read through. DLSS outputs are typed colour
// targets; a typeless one takes its usual typed view.
DXGI_FORMAT tile_view_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return format;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace

HRESULT D3D12EyeCheck::initialize(ID3D12Device* device, DXGI_FORMAT view_format) noexcept {
    try {
        if (device == nullptr || view_format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        device_ = device;
        view_format_ = view_format;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        for (UINT i = 0; i < 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i]};
        }
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = {0, 0, sizeof(Params) / 4};
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 3;
        description.pParameters = parameters;
        ComPtr<ID3DBlob> blob, error;
        HRESULT result = serialize_root_signature(device, description, &blob, &error);
        if (FAILED(result)) return result;
        result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root_));
        if (FAILED(result)) return result;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.CS = {g_xrfg_eye_check_compare_shader, sizeof(g_xrfg_eye_check_compare_shader)};
        result = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&compare_));
        if (FAILED(result)) return result;
        increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (auto& slot : slots_) {
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = kDescriptors;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.heap));
            if (FAILED(result)) return result;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = kResultBytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            result = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&slot.result));
            if (FAILED(result)) return result;
            buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            result = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.staging));
            if (FAILED(result)) return result;
        }
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

void D3D12EyeCheck::record(ID3D12GraphicsCommandList* list, UINT slot_index,
    ID3D12Resource* image, std::span<const D3D12ReprojectionView> views,
    const DlssMotionVectorSet& set) noexcept {
    try {
        poll();
        if (!list || !image || !root_ || slot_index >= kSlots || views.size() != 2 ||
            set.eye_count != 2 || !set.eyes[0] || !set.eyes[1]) return;
        const auto& a = *set.eyes[0];
        const auto& b = *set.eyes[1];
        if (!a.output_tile || !b.output_tile || a.stream == b.stream ||
            a.output_tile_width == 0 || a.output_tile_height == 0 ||
            a.output_tile_width != b.output_tile_width ||
            a.output_tile_height != b.output_tile_height ||
            a.output_width == 0 || a.output_height == 0 ||
            b.output_width == 0 || b.output_height == 0) return;
        const DXGI_FORMAT tile_format_a = tile_view_format(a.output_tile->GetDesc().Format);
        const DXGI_FORMAT tile_format_b = tile_view_format(b.output_tile->GetDesc().Format);
        if (tile_format_a == DXGI_FORMAT_UNKNOWN || tile_format_b == DXGI_FORMAT_UNKNOWN) return;
        const D3D12_RESOURCE_DESC image_description = image->GetDesc();
        if (image_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;

        Params p{};
        for (UINT view = 0; view < 2; ++view) {
            const auto& rect = views[view].image_rect;
            const bool whole = rect.width == 0 && rect.height == 0;
            p.view_rect[view][0] = whole ? 0 : rect.offset_x;
            p.view_rect[view][1] = whole ? 0 : rect.offset_y;
            p.view_rect[view][2] = whole ? static_cast<UINT>(image_description.Width) : rect.width;
            p.view_rect[view][3] = whole ? image_description.Height : rect.height;
            const auto slice = views[view].array_slice;
            p.view_slice[view] = slice != std::numeric_limits<std::uint32_t>::max()
                ? slice
                : (image_description.DepthOrArraySize > 1 ? view : 0);
            if (p.view_rect[view][2] == 0 || p.view_rect[view][3] == 0 ||
                p.view_slice[view] >= image_description.DepthOrArraySize ||
                std::uint64_t(p.view_rect[view][0]) + p.view_rect[view][2] > image_description.Width ||
                std::uint64_t(p.view_rect[view][1]) + p.view_rect[view][3] > image_description.Height) {
                return;
            }
        }
        p.tile_size[0] = a.output_tile_width;
        p.tile_size[1] = a.output_tile_height;
        p.tile_origin[0] = a.output_tile_x;
        p.tile_origin[1] = a.output_tile_y;
        p.tile_origin[2] = b.output_tile_x;
        p.tile_origin[3] = b.output_tile_y;
        p.output_size[0] = a.output_width;
        p.output_size[1] = a.output_height;
        p.output_size[2] = b.output_width;
        p.output_size[3] = b.output_height;

        Slot& slot = slots_[slot_index];
        // Still on the GPU, as the read above left it: its result is not
        // written yet. One recorded but never submitted is free to take.
        if (slot.recorded && slot.fence) return;
        auto cpu = slot.heap->GetCPUDescriptorHandleForHeapStart();
        const auto next = [&] { auto h = cpu; cpu.ptr += increment_; return h; };
        D3D12_SHADER_RESOURCE_VIEW_DESC image_view{};
        image_view.Format = view_format_;
        image_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        image_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        image_view.Texture2DArray.MipLevels = 1;
        image_view.Texture2DArray.ArraySize = image_description.DepthOrArraySize;
        device_->CreateShaderResourceView(image, &image_view, next());
        for (const auto& tile : {std::pair{a.output_tile.Get(), tile_format_a},
                                 std::pair{b.output_tile.Get(), tile_format_b}}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC tile_view{};
            tile_view.Format = tile.second;
            tile_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            tile_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            tile_view.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(tile.first, &tile_view, next());
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC result_view{};
        result_view.Format = DXGI_FORMAT_R32_TYPELESS;
        result_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        result_view.Buffer.NumElements = kResultBytes / 4;
        result_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(slot.result.Get(), nullptr, &result_view, next());

        // The image rests in COMMON; the tiles in the snapshots' state.
        const auto in = transition(image, D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &in);
        ID3D12DescriptorHeap* heaps[] = {slot.heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        auto gpu = slot.heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += 3ULL * increment_;
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRoot32BitConstants(2, sizeof(Params) / 4, &p, 0);
        list->SetPipelineState(compare_.Get());
        list->Dispatch(1, 1, 1);
        const std::array<D3D12_RESOURCE_BARRIER, 2> to_copy{
            transition(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COMMON),
            transition(slot.result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(UINT(to_copy.size()), to_copy.data());
        list->CopyBufferRegion(slot.staging.Get(), 0, slot.result.Get(), 0, kResultBytes);
        const auto back = transition(slot.result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
        slot.eye0_stream = a.stream;
        slot.eye1_stream = b.stream;
        slot.fence.Reset();
        slot.value = 0;
        slot.sequence = ++next_sequence_;
        slot.recorded = true;
    } catch (...) {
    }
}

void D3D12EyeCheck::submitted(UINT slot_index, ID3D12Fence* fence, std::uint64_t value) noexcept {
    if (slot_index >= kSlots || fence == nullptr) return;
    Slot& slot = slots_[slot_index];
    if (!slot.recorded) return;
    slot.fence = fence;
    slot.value = value;
}

void D3D12EyeCheck::poll() noexcept {
    try {
        // Slots are taken by the caller's work slot, not in turn, so the
        // finished ones are read by when they were recorded: a run is a run
        // of pairs in the order they happened.
        std::array<Slot*, kSlots> finished{};
        std::size_t count = 0;
        for (auto& slot : slots_) {
            if (slot.recorded && slot.fence && slot.fence->GetCompletedValue() >= slot.value) {
                finished[count++] = &slot;
            }
        }
        std::sort(finished.begin(), finished.begin() + count,
            [](const Slot* a, const Slot* b) { return a->sequence < b->sequence; });
        for (std::size_t index = 0; index < count; ++index) read(*finished[index]);
    } catch (...) {
    }
}

void D3D12EyeCheck::read(Slot& slot) noexcept {
    slot.recorded = false;
    const D3D12_RANGE range{0, kResultBytes};
    void* mapped = nullptr;
    if (FAILED(slot.staging->Map(0, &range, &mapped)) || mapped == nullptr) return;
    float f[kResultFloats]{};
    std::copy_n(static_cast<const float*>(mapped), kResultFloats, f);
    const D3D12_RANGE nothing{0, 0};
    slot.staging->Unmap(0, &nothing);
    ++report_.compared;
    // f[0..3]: the correlation of view 0 with A, view 0 with B, view 1 with A,
    // view 1 with B; f[4..7] the image's variance in each; f[8..9] the tiles'.
    // A is eye 0's stream as resolved, B eye 1's.
    bool finite = true;
    for (const float value : f) finite = finite && std::isfinite(value);
    const bool flat = f[4] < kMinimumVariance || f[5] < kMinimumVariance ||
        f[6] < kMinimumVariance || f[7] < kMinimumVariance ||
        f[8] < kMinimumVariance || f[9] < kMinimumVariance;
    if (!finite || flat) return;
    const float same = f[0] + f[3];
    const float swapped = f[1] + f[2];
    report_.same = same;
    report_.swapped = swapped;
    if (!(std::abs(same - swapped) > kMargin)) return;
    // The vote names streams, not "as resolved" or "the other way": the
    // resolved order can change between two pairs, and the same answer would
    // then read as its opposite. A vote about another two streams, or naming
    // the other one, starts the run over.
    const bool as_resolved = same > swapped;
    const std::uint64_t eye0 = as_resolved ? slot.eye0_stream : slot.eye1_stream;
    const std::uint64_t eye1 = as_resolved ? slot.eye1_stream : slot.eye0_stream;
    const std::uint64_t lower = std::min(eye0, eye1);
    const std::uint64_t higher = std::max(eye0, eye1);
    if (run_streams_[0] != lower || run_streams_[1] != higher || run_eye0_ != eye0) {
        run_streams_[0] = lower;
        run_streams_[1] = higher;
        run_eye0_ = eye0;
        run_ = 0;
    }
    if (++run_ < kVotes) return;
    run_ = 0;
    ++report_.decided;
    if (!as_resolved) ++report_.swaps;
    report_dlss_eye_order_check(eye0, eye1);
}

}  // namespace xrfg
