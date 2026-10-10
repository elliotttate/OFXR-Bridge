#include "xrfg/openxr_fps_overlay.hpp"
#include <openxr/openxr_platform.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
void write_position(const std::filesystem::path& ini, const wchar_t* position) {
    // Match the tray's file replacement, not the Win32 profile writer/cache.
    const std::filesystem::path temporary = ini.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << "[overlay]\r\nposition=";
        for (const wchar_t* c = position; *c; ++c) stream << static_cast<char>(*c);
        stream << "\r\n";
        stream.flush();
        check(static_cast<bool>(stream), "write atomic INI");
    }
    check(MoveFileExW(temporary.c_str(), ini.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "replace atomic INI");
}
template<class T> T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
const auto session = handle<XrSession>(2);
const auto overlay_space = handle<XrSpace>(4);
const auto overlay_chain = handle<XrSwapchain>(5);
// The status panel's swapchain, its LOCAL space, and the layer's two grip
// spaces it reads the flip gesture from.
const auto panel_chain = handle<XrSwapchain>(6);
const auto left_grip = handle<XrSpace>(7);
const auto right_grip = handle<XrSpace>(8);
const auto local_space = handle<XrSpace>(9);
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> queue;
ComPtr<ID3D11Device> device11;
std::vector<ComPtr<ID3D12Resource>> textures, panel_textures;
std::vector<ComPtr<ID3D11Texture2D>> textures11, panel_textures11;
unsigned created{}, destroyed{}, space_created{}, space_destroyed{}, acquires{}, waits{}, releases{}, ends{};
unsigned local_created{}, local_destroyed{}, panel_created{}, panel_destroyed{}, locates{};
unsigned acquired_index{}, last_released{}, width{}, height{};
unsigned panel_acquires{}, panel_acquired_index{}, panel_last_released{}, panel_width{}, panel_height{};
bool timeout_next = true, fail_end{}, unsupported{}, d3d11{};
const XrCompositionLayerBaseHeader* expected_layer{};
const void* expected_next{};
unsigned expected_count{1}, observed_count{};
XrPosef last_pose{};
// The last panel quad the runtime was given, and the grips the fake tracks.
XrCompositionLayerQuad last_panel{};
bool panel_seen{};
XrTime last_locate_time{};
std::array<XrPosef, 2> grip_pose{XrPosef{{0, 0, 0, 1}, {-0.2f, 1.0f, -0.4f}},
                                 XrPosef{{0, 0, 0, 1}, {0.2f, 1.0f, -0.4f}}};
std::array<bool, 2> grip_tracked{true, true};
XrResult XRAPI_CALL create_space(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* output) {
    check((info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ||
           info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) &&
          info->poseInReferenceSpace.orientation.w == 1, "overlay must own a VIEW or LOCAL space");
    if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
        ++local_created; *output = local_space; return XR_SUCCESS;
    }
    ++space_created; *output = overlay_space; return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace space) {
    check(space == overlay_space || space == local_space, "destroyed app space");
    if (space == local_space) ++local_destroyed; else ++space_destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
    check(base == local_space, "grips must be read in the overlay's LOCAL space");
    check(space == left_grip || space == right_grip, "located a space that is not a grip");
    ++locates;
    last_locate_time = time;
    const int hand = space == left_grip ? 0 : 1;
    location->pose = grip_pose[hand];
    // A lost controller keeps VALID with its last pose, as runtimes do.
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
        (grip_tracked[hand] ? XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT : 0);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* output) {
    *count = 1; if (capacity) output[0] = DXGI_FORMAT_R8G8B8A8_UNORM; return XR_SUCCESS;
}
XrResult XRAPI_CALL properties(XrInstance, XrSystemId, XrSystemProperties* output) {
    output->graphicsProperties = {4096, 4096, 2}; return XR_SUCCESS;
}
XrResult XRAPI_CALL create_chain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output) {
    check(info->arraySize == 1 && info->sampleCount == 1 && info->format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "overlay swapchain geometry/format");
    check((info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0, "upload usage missing");
    // The panel's is the 1024-wide one; the counter's is at most 336.
    const bool panel = info->width == xrfg::kStatusPanelWidth;
    (panel ? panel_width : width) = info->width; (panel ? panel_height : height) = info->height;
    if (d3d11) {
        auto& set = panel ? panel_textures11 : textures11;
        set.resize(3);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = info->width; desc.Height = info->height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& texture : set) check(SUCCEEDED(device11->CreateTexture2D(&desc, nullptr, &texture)), "D3D11 texture");
    } else {
        auto& set = panel ? panel_textures : textures;
        set.resize(3);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = info->width; desc.Height = info->height; desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (auto& texture : set) check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture))), "D3D12 texture");
    }
    if (panel) { ++panel_created; *output = panel_chain; return XR_SUCCESS; }
    ++created; *output = overlay_chain; return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_chain(XrSwapchain chain) {
    check(chain == overlay_chain || chain == panel_chain, "destroyed app swapchain");
    if (chain == panel_chain) ++panel_destroyed; else ++destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerate(XrSwapchain chain, std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageBaseHeader* output) {
    *count = 3;
    const bool panel = chain == panel_chain;
    if (capacity) {
        if (d3d11) { auto* images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(output);
            for (unsigned i = 0; i < 3; ++i) images[i].texture = (panel ? panel_textures11 : textures11)[i].Get();
        } else { auto* images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(output);
            for (unsigned i = 0; i < 3; ++i) images[i].texture = (panel ? panel_textures : textures)[i].Get(); }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain chain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    if (chain == panel_chain) { panel_acquired_index = panel_acquires++ % 3; *index = panel_acquired_index; return XR_SUCCESS; }
    acquired_index = acquires++ % 3; *index = acquired_index; return XR_SUCCESS;
}
XrResult XRAPI_CALL wait(XrSwapchain, const XrSwapchainImageWaitInfo* info) {
    check(info->timeout == 0, "overlay adds a blocking image wait"); ++waits;
    if (timeout_next) { timeout_next = false; return XR_TIMEOUT_EXPIRED; } return XR_SUCCESS;
}
XrResult XRAPI_CALL release(XrSwapchain chain, const XrSwapchainImageReleaseInfo*) {
    if (chain == panel_chain) { panel_last_released = panel_acquired_index; return XR_SUCCESS; }
    ++releases; last_released = acquired_index; return XR_SUCCESS;
}
XrResult XRAPI_CALL end(XrSession, const XrFrameEndInfo* info) {
    ++ends;
    observed_count = info->layerCount;
    check(info->next == expected_next, "lost frame next chain");
    if (info->layerCount) check(info->layers[0] == expected_layer, "mutated original projection");
    panel_seen = false;
    if (info->layerCount > expected_count) {
        check(info->layerCount == expected_count + 1, "wrong layer count");
        const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[expected_count]);
        if (quad->subImage.swapchain == panel_chain) {
            // The panel stands in for the counter while it is up.
            check(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH &&
                  quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                  "invalid status panel quad");
            check(quad->subImage.imageRect.extent.width == static_cast<int>(panel_width) &&
                  quad->subImage.imageRect.extent.height > 0 &&
                  quad->subImage.imageRect.extent.height <= static_cast<int>(panel_height),
                  "panel shows rows it does not have");
            check(std::abs(quad->size.height / quad->size.width -
                           static_cast<float>(quad->subImage.imageRect.extent.height) /
                               static_cast<float>(quad->subImage.imageRect.extent.width)) < 1.0e-4f,
                  "panel quad stretched");
            last_panel = *quad;
            panel_seen = true;
            if (fail_end) { fail_end = false; return XR_ERROR_RUNTIME_FAILURE; }
            return XR_SUCCESS;
        }
        check(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad->space == overlay_space &&
            quad->subImage.swapchain == overlay_chain && quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH,
            "invalid overlay quad");
        check(releases > 0 && quad->subImage.imageRect.extent.width == static_cast<int>(width), "unreleased overlay image");
        check(quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              "overlay must blend premultiplied source alpha");
        check(width == height * 2, "numeric overlay aspect ratio");
        last_pose = quad->pose;
    }
    if (fail_end) { fail_end = false; return XR_ERROR_RUNTIME_FAILURE; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL get(XrInstance, const char* name, PFN_xrVoidFunction* output) {
    *output = nullptr;
    if (unsupported) return XR_ERROR_FUNCTION_UNSUPPORTED;
#define FN(n, f) if (std::strcmp(name, n) == 0) { *output = reinterpret_cast<PFN_xrVoidFunction>(f); return XR_SUCCESS; }
    FN("xrCreateReferenceSpace", create_space) FN("xrDestroySpace", destroy_space)
    FN("xrCreateSwapchain", create_chain) FN("xrDestroySwapchain", destroy_chain)
    FN("xrEnumerateSwapchainFormats", formats) FN("xrEnumerateSwapchainImages", enumerate)
    FN("xrAcquireSwapchainImage", acquire) FN("xrWaitSwapchainImage", wait) FN("xrReleaseSwapchainImage", release)
    FN("xrGetSystemProperties", properties) FN("xrLocateSpace", locate)
#undef FN
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// Test-only GPU readback proves that the production overlay actually uploaded
// its red/green bitmap. Production contains no such readbacks or queue drains.
template<class Wanted> std::size_t count_pixels(bool panel, Wanted&& wanted) {
    const unsigned width = panel ? panel_width : ::width;
    const unsigned height = panel ? panel_height : ::height;
    const unsigned last_released = panel ? panel_last_released : ::last_released;
    auto& textures11 = panel ? panel_textures11 : ::textures11;
    auto& textures = panel ? panel_textures : ::textures;
    if (d3d11) {
        auto desc = D3D11_TEXTURE2D_DESC{}; textures11[last_released]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(SUCCEEDED(device11->CreateTexture2D(&desc, nullptr, &staging)), "staging11");
        ComPtr<ID3D11DeviceContext> context; device11->GetImmediateContext(&context);
        context->CopyResource(staging.Get(), textures11[last_released].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map11");
        std::size_t result{};
        for (unsigned y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < width; ++x) result += wanted(row[x]) ? 1 : 0;
        }
        context->Unmap(staging.Get(), 0); return result;
    }
    const auto desc = textures[last_released]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "readback12");
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator12");
    check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "list12");
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = textures[last_released].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}, dest{};
    source.pResource = textures[last_released].Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.pResource = readback.Get(); dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dest.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
    check(SUCCEEDED(list->Close()), "close12");
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "fence12");
    check(SUCCEEDED(queue->Signal(fence.Get(), 1)), "signal12");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event && SUCCEEDED(fence->SetEventOnCompletion(1, event)), "event12");
    check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU readback timed out"); CloseHandle(event);
    void* mapped{}; const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(SUCCEEDED(readback->Map(0, &range, &mapped)), "map12");
    std::size_t result{};
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
        for (unsigned x = 0; x < width; ++x) result += wanted(row[x]) ? 1 : 0;
    }
    const D3D12_RANGE empty{}; readback->Unmap(0, &empty); return result;
}
std::size_t pixel_count(std::uint32_t wanted) {
    return count_pixels(false, [wanted](std::uint32_t pixel) { return pixel == wanted; });
}
void write_overlay(const std::filesystem::path& ini, const char* position, const char* panel) {
    const std::filesystem::path temporary = ini.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << "[overlay]\r\nposition=" << position << "\r\npanel=" << panel << "\r\n";
        stream.flush();
        check(static_cast<bool>(stream), "write atomic INI");
    }
    check(MoveFileExW(temporary.c_str(), ini.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "replace atomic INI");
}
// Half a turn about the controller's length: upside down.
constexpr XrQuaternionf kUpsideDown{0.0f, 0.0f, 1.0f, 0.0f};
constexpr XrQuaternionf kUpright{0.0f, 0.0f, 0.0f, 1.0f};
}

int main(int argc, char** argv) {
    const auto ini = std::filesystem::temp_directory_path() /
        (L"ofxr-overlay-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    try {
        d3d11 = argc > 1 && std::string(argv[1]) == "d3d11";
        if (d3d11) {
            check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device11, nullptr, nullptr)), "WARP D3D11");
        } else {
            ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp;
            check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
            check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP D3D12");
            D3D12_COMMAND_QUEUE_DESC desc{};
            check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue12");
        }
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.fov = {-0.8f, 0.8f, 0.8f, -0.8f};
        view.subImage.imageRect.extent = {2688, 2784};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = handle<XrSpace>(99); projection.viewCount = 1; projection.views = &view;
        expected_layer = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
        std::array<const XrCompositionLayerBaseHeader*, 2> original{expected_layer, expected_layer};
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO}; info.layerCount = 1; info.layers = original.data();
        const XrBaseInStructure extension{XR_TYPE_UNKNOWN}; expected_next = &extension; info.next = expected_next;
        write_position(ini, L"upper_right");
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1), session, 1, get, end,
                device.Get(), queue.Get(), device11.Get(), ini, nullptr);
            overlay.application_frame(&info);
            check(acquires == 1 && waits == 1 && releases == 0, "timeout ownership violated");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 1, "uninitialized quad submitted");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(acquires == 1 && waits == 2 && releases == 1, "must retry same acquired image after timeout");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "fallback missing overlay");
            check(pixel_count(0xff5050ffu) > 100, "inactive bitmap missing red on GPU");
            check(pixel_count(0) > static_cast<std::size_t>(width) * height / 2,
                  "numeric overlay background must be transparent on GPU");
            check(last_pose.position.x > 0 && last_pose.position.y > 0, "default quadrant");
            check(last_pose.position.x > 0.879f && last_pose.position.x < 0.881f &&
                  last_pose.position.y > 0.779f && last_pose.position.y < 0.781f,
                  "runtime quad must move left and down without changing quadrant");
            const unsigned release_count = releases;
            overlay.application_frame(&info);
            check(releases == release_count, "upload unthrottled");
            check(overlay.end_frame(&info, true) == XR_SUCCESS && overlay.metrics().active, "successful synthetic not active");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "original loses overlay");
            check(pixel_count(0xff70eb40u) > 100, "active bitmap missing green on GPU");
            XrFrameEndInfo empty = info; empty.layerCount = 0; empty.layers = nullptr;
            const float before_empty = overlay.metrics().submitted_fps;
            check(overlay.end_frame(&empty, true) == XR_SUCCESS && observed_count == 0, "added overlay to shouldRender=false/empty frame");
            check(overlay.metrics().submitted_fps < before_empty + 0.1f, "counted empty synthetic");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            fail_end = true;
            check(overlay.end_frame(&info, true) == XR_ERROR_RUNTIME_FAILURE && !overlay.metrics().active, "failed end marked active");
            for (auto name : {L"upper_left", L"lower_left", L"lower_right", L"upper_right", L"off"}) {
                write_position(ini, name);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                overlay.application_frame(&info);
                check(overlay.end_frame(&info, false) == XR_SUCCESS, "position frame");
                if (std::wstring(name) == L"off") {
                    check(observed_count == 1, "live Off failed");
                    check(!overlay.marker_placement(), "Off left diagnostic marker enabled");
                }
                else {
                    check(observed_count == 2, "position lost overlay");
                    check(overlay.marker_placement().has_value(), "visible counter has no marker placement");
                    const auto text = std::wstring(name);
                    check((last_pose.position.x < 0) == (text.find(L"left") != text.npos) &&
                          (last_pose.position.y > 0) == (text.find(L"upper") != text.npos), "live quadrant wrong");
                }
            }
            write_position(ini, L"upper_right");
            std::this_thread::sleep_for(std::chrono::milliseconds(260)); overlay.application_frame(&info);
            info.layerCount = expected_count = 2;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "exceeded runtime maxLayerCount");
            overlay.reset_metrics(); check(!overlay.metrics().active && overlay.metrics().submitted_fps == 0, "session reset");
            info.layerCount = expected_count = 1;
            // Asymmetric/narrow FOV catches accidental fallback to the default
            // 90-degree placement. Flipping image storage must not move the quad.
            const auto saved_fov = view.fov;
            view.fov = {-0.65f, 0.55f, 0.42f, -0.61f};
            for (auto name : {L"upper_left", L"upper_right", L"lower_left", L"lower_right"}) {
                write_position(ini, name);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                overlay.application_frame(&info);
                check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "normal FOV placement");
                const auto upright_pose = last_pose;
                std::swap(view.fov.angleUp, view.fov.angleDown);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                overlay.application_frame(&info);
                check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "inverted FOV placement");
                check(std::abs(last_pose.position.x - upright_pose.position.x) < 1.0e-6f &&
                      std::abs(last_pose.position.y - upright_pose.position.y) < 1.0e-6f &&
                      std::abs(last_pose.position.z - upright_pose.position.z) < 1.0e-6f,
                      "vertical FOV inversion moved the overlay");
                check(view.fov.angleDown > view.fov.angleUp, "overlay reordered the application's FOV");
                std::swap(view.fov.angleUp, view.fov.angleDown);
            }
            view.fov = saved_fov;
            write_position(ini, L"upper_right");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, true) == XR_SUCCESS && observed_count == 2, "overlay visible before manual stop");
            const auto uploads_before_stop = releases;
            overlay.suspend();
            check(!overlay.marker_placement(), "manual stop left diagnostic marker enabled");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 1, "manual stop removes quad immediately");
            overlay.reset_metrics();
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 1 && releases == uploads_before_stop,
                  "stopped overlay never uploads/reappears after refresh or metric reset");
        }
        check(created == 1 && destroyed == 1 && space_created == 1 && space_destroyed == 1, "overlay lifetime leak");
        check(panel_created == 0 && local_created == 0 && locates == 0,
              "the status panel made something nobody asked for");

        // The status panel, always on: low in the view, cut to its lines,
        // and in place of the counter while it shows.
        {
            timeout_next = false;
            write_overlay(ini, "upper_right", "always");
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1), session, 1, get, end,
                device.Get(), queue.Get(), device11.Get(), ini, nullptr);
            overlay.set_display_period(11'111'111);
            xrfg::StatusPanelInput status;
            status.asked.game_vectors = status.asked.hybrid = true;
            status.running = status.asked;
            overlay.application_frame(&info);
            check(overlay.status_wanted(), "an always-on panel does not ask for its status");
            overlay.application_frame(&info, &status);
            check(panel_created == 1 && panel_width == xrfg::kStatusPanelWidth &&
                  panel_height == xrfg::kStatusPanelHeight, "panel swapchain");
            check(!overlay.status_wanted(), "panel repaint unthrottled");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2 && panel_seen,
                  "always-on panel missing, or shown beside the counter");
            check(last_panel.space == overlay_space && last_panel.pose.position.z == -1.0f &&
                  last_panel.pose.position.y < 0.0f && std::abs(last_panel.size.width - 0.56f) < 1.0e-4f,
                  "always-on panel is not low in the view");
            check(last_panel.subImage.imageRect.extent.height < static_cast<int>(panel_height),
                  "panel not cut to its lines");
            // The background, converted for this UNORM swapchain, covers the
            // rows the quad shows.
            check(count_pixels(true, [](std::uint32_t pixel) { return (pixel >> 24) == 232; }) >
                      static_cast<std::size_t>(panel_width) * last_panel.subImage.imageRect.extent.height / 2,
                  "panel background missing on GPU");

            write_overlay(ini, "upper_right", "off");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info, &status);
            check(!overlay.status_wanted(), "an off panel asks for its status");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2 && !panel_seen,
                  "an off panel is still shown, or the counter is not back");

            // The flip gesture, read from the layer's grip spaces in a LOCAL
            // space of the overlay's own at each submission's display time.
            write_overlay(ini, "upper_right", "gesture");
            overlay.set_grip_spaces(left_grip, right_grip);
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(local_created == 1, "no LOCAL space for the gesture");
            info.displayTime = 123456;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2 && !panel_seen,
                  "upright controllers show the panel");
            check(locates == 2 && last_locate_time == 123456, "grips not read at the frame's display time");
            check(!overlay.status_wanted(), "the panel asks for its status with nothing turned over");
            grip_pose[1].orientation = kUpsideDown;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && !panel_seen, "shown with no delay");
            check(overlay.status_wanted(), "a turned controller does not get the panel ready");
            overlay.application_frame(&info, &status);
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            check(overlay.end_frame(&info, false) == XR_SUCCESS && panel_seen,
                  "not shown after a quarter second turned over");
            check(last_panel.space == local_space && std::abs(last_panel.size.width - 0.42f) < 1.0e-4f,
                  "panel not on the controller");
            check(std::abs(last_panel.pose.position.x - 0.2f) < 1.0e-4f &&
                  std::abs(last_panel.pose.position.z + 0.4f) < 1.0e-4f &&
                  std::abs(last_panel.pose.position.y - (1.0f + last_panel.size.height / 2 + 0.05f)) < 1.0e-4f &&
                  std::abs(std::abs(last_panel.pose.orientation.w) - 1.0f) < 1.0e-4f,
                  "panel not upright above the turned controller");
            // A controller the runtime has lost keeps its last pose: the
            // panel goes rather than freezing in the air, and comes back
            // with it.
            grip_tracked[1] = false;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && !panel_seen, "panel on a lost controller");
            grip_tracked[1] = true;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && panel_seen, "panel not back with the controller");
            // Back upright: it stays half a second, then goes.
            grip_pose[1].orientation = kUpright;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && panel_seen, "hidden with no delay");
            std::this_thread::sleep_for(std::chrono::milliseconds(510));
            check(overlay.end_frame(&info, false) == XR_SUCCESS && !panel_seen && observed_count == 2,
                  "not hidden half a second after turning back, or the counter not back");
            // Turned over while lost: nothing to show.
            grip_tracked[0] = false;
            grip_pose[0].orientation = kUpsideDown;
            check(overlay.end_frame(&info, false) == XR_SUCCESS, "lost frame");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            check(overlay.end_frame(&info, false) == XR_SUCCESS && !panel_seen, "a lost controller showed the panel");
            grip_tracked[0] = true;
            grip_pose[0].orientation = kUpright;
            info.displayTime = 0;
        }
        check(panel_created == 1 && panel_destroyed == 1 && local_created == 1 && local_destroyed == 1,
              "status panel lifetime leak");
        write_position(ini, L"upper_right");
        unsupported = true;
        info.layerCount = expected_count = 2;
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1), session, 1, get, end,
                device.Get(), queue.Get(), device11.Get(), ini, nullptr);
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "optional API failure broke game");
        }
        std::filesystem::remove(ini);
        std::cout << "Overlay " << (d3d11 ? "D3D11" : "D3D12")
                  << " GPU colors, composition, timeout ownership, live controls, counters, status panel"
                     " and lifetime passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::error_code ignored; std::filesystem::remove(ini, ignored); return 1;
    }
}
