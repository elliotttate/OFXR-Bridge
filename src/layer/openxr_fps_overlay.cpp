#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/steamvr_delivery.hpp"
#include <openxr/openxr_platform.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>

namespace xrfg {
using Microsoft::WRL::ComPtr;
namespace {
std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<class T> bool load(PFN_xrGetInstanceProcAddr get, XrInstance instance,
                          const char* name, T& output) {
    PFN_xrVoidFunction function{};
    if (!get || XR_FAILED(get(instance, name, &function)) || !function) return false;
    output = reinterpret_cast<T>(function);
    return true;
}
std::string read_overlay_setting(const std::filesystem::path& ini, const wchar_t* key,
                                 const wchar_t* fallback) {
    std::array<wchar_t, 32> setting{};
    GetPrivateProfileStringW(L"overlay", key, fallback,
        setting.data(), static_cast<DWORD>(setting.size()), ini.c_str());
    std::string value;
    for (wchar_t c : setting) { if (!c) break; value += c < 128 ? static_cast<char>(c) : '?'; }
    return value;
}

// The status panel's size in the world. On a flipped controller it is held
// at about forearm's length, where 42 cm across puts a line of its text at
// the size of print at reading distance; in the view it is a metre away, so
// a little wider. Its height follows the lines it has.
constexpr float kPanelOnControllerWidth = 0.42F;
constexpr float kPanelInViewWidth = 0.56F;
// Low in the view, tilted back to face the eye: below the line of sight,
// where it covers the least of the game.
constexpr float kPanelInViewDrop = 0.30F;
constexpr float kPanelInViewDistance = 1.0F;
}

struct OpenXrFpsOverlay::Impl {
    XrInstance instance{};
    XrSession session{};
    XrSystemId system{};
    PFN_xrGetInstanceProcAddr get{};
    PFN_xrEndFrame downstream_end{};
    PFN_xrCreateReferenceSpace create_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainFormats enumerate_formats{};
    PFN_xrEnumerateSwapchainImages enumerate_images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrGetSystemProperties system_properties{};
    // Optional: only the flip gesture needs it, and its absence must not
    // take the counter with it.
    PFN_xrLocateSpace locate_space{};
    std::filesystem::path ini;
    std::mutex mutex;
    FpsCounter counter;
    FpsOverlayPosition position{FpsOverlayPosition::upper_right};
    std::int64_t next_refresh{};
    bool attempted{}, initialized{}, disabled{};
    bool paused{};
    std::atomic<std::int64_t> display_period_ns{0};
    // The number the overlay draws is what the headset received where that
    // can be known, and what was submitted everywhere else. Only SteamVR
    // reports the former. Borrowed: the session owns it, because the presenter
    // reads the vsync anchor from the same connection.
    SteamVrDelivery* delivery{};
    XrSpace space{};
    std::uint32_t max_layers{}, max_width{}, max_height{};
    std::vector<std::int64_t> formats;
    OverlayPlacement placement{};
    bool placement_valid{};
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Multithread> multithread11;
    ComPtr<ID3D11Fence> fence11;
    std::uint64_t next_fence{1}, last_fence{};
    HANDLE event{};
    struct Image {
        ComPtr<ID3D12Resource> texture12, upload;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D11Texture2D> texture11;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint64_t fence_value{};
    };
    // One quad's swapchain and its upload state. The counter and the status
    // panel have one each; a failure in either disables that one alone.
    struct Surface {
        XrSwapchain swapchain{};
        std::uint32_t index{}, width{}, height{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        bool attempted{}, initialized{}, image_valid{}, acquired{}, waited{}, failed{};
        std::vector<Image> images;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        [[nodiscard]] bool bgra() const noexcept {
            return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
        [[nodiscard]] bool srgb() const noexcept {
            return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        }
    };
    Surface number;

    // The status panel (`[overlay] panel`). Its image is repainted from the
    // application's thread; where it is placed is decided per submission, so
    // a panel on a controller follows the hand at the display's rate rather
    // than the game's.
    Surface panel;
    StatusPanelMode panel_mode{StatusPanelMode::gesture};
    std::int64_t next_panel_refresh{};
    std::uint32_t panel_rows{};
    std::array<XrSpace, 2> grips{};
    XrSpace local{};
    bool local_attempted{};
    FlipGesture gesture;
    bool panel_logged_visible{};

    ~Impl() {
        // Only teardown waits for this overlay's last upload. No global queue
        // drain, no runtime frame calls, no application-owned handle retention.
        if (last_fence && event) {
            HRESULT result = S_OK;
            if (fence12 && fence12->GetCompletedValue() < last_fence)
                result = fence12->SetEventOnCompletion(last_fence, event);
            else if (fence11 && fence11->GetCompletedValue() < last_fence)
                result = fence11->SetEventOnCompletion(last_fence, event);
            else result = S_FALSE;
            if (result == S_OK) WaitForSingleObject(event, INFINITE);
        }
        for (Surface* surface : {&number, &panel})
            if (surface->swapchain && destroy_swapchain) destroy_swapchain(surface->swapchain);
        if (space && destroy_space) destroy_space(space);
        if (local && destroy_space) destroy_space(local);
        if (event) CloseHandle(event);
    }

    // What both quads need: the runtime's entry points and limits, the VIEW
    // space and the upload fence. Attempted once.
    bool initialize() {
        if (attempted) return initialized;
        attempted = true;
        if ((!device11 && (!device12 || !queue12)) || !system) return false;
        if (!load(get, instance, "xrCreateReferenceSpace", create_space) ||
            !load(get, instance, "xrDestroySpace", destroy_space) ||
            !load(get, instance, "xrCreateSwapchain", create_swapchain) ||
            !load(get, instance, "xrDestroySwapchain", destroy_swapchain) ||
            !load(get, instance, "xrEnumerateSwapchainFormats", enumerate_formats) ||
            !load(get, instance, "xrEnumerateSwapchainImages", enumerate_images) ||
            !load(get, instance, "xrAcquireSwapchainImage", acquire) ||
            !load(get, instance, "xrWaitSwapchainImage", wait) ||
            !load(get, instance, "xrReleaseSwapchainImage", release) ||
            !load(get, instance, "xrGetSystemProperties", system_properties)) return false;
        static_cast<void>(load(get, instance, "xrLocateSpace", locate_space));
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_FAILED(system_properties(instance, system, &properties))) return false;
        max_layers = std::min(properties.graphicsProperties.maxLayerCount, 128u);
        max_width = properties.graphicsProperties.maxSwapchainImageWidth;
        max_height = properties.graphicsProperties.maxSwapchainImageHeight;
        if (max_layers < 2) return false;
        std::uint32_t count{};
        if (XR_FAILED(enumerate_formats(session, 0, &count, nullptr)) || !count || count > 1024) return false;
        formats.resize(count);
        if (XR_FAILED(enumerate_formats(session, count, &count, formats.data()))) return false;
        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_info.poseInReferenceSpace.orientation.w = 1;
        if (XR_FAILED(create_space(session, &space_info, &space))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        if (device11) {
            ComPtr<ID3D11DeviceContext> immediate;
            device11->GetImmediateContext(&immediate);
            if (!immediate || FAILED(immediate.As(&context11))) return false;
            immediate.As(&multithread11);
            ComPtr<ID3D11Device5> device5;
            if (FAILED(device11.As(&device5)) || FAILED(device5->CreateFence(
                0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence11)))) return false;
        } else if (FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12)))) {
            return false;
        }
        initialized = true;
        return true;
    }

    // A swapchain for one quad, in the first of `preferred` the runtime has.
    bool create_surface(Surface& surface, std::uint32_t width, std::uint32_t height,
                        std::initializer_list<DXGI_FORMAT> preferred) {
        if (surface.attempted) return surface.initialized;
        surface.attempted = true;
        if (!initialize()) return false;
        for (auto candidate : preferred) {
            if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
                surface.format = candidate;
                break;
            }
        }
        if (surface.format == DXGI_FORMAT_UNKNOWN || width > max_width || height > max_height) return false;
        surface.width = width;
        surface.height = height;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = surface.format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = width;
        info.height = height;
        if (XR_FAILED(create_swapchain(session, &info, &surface.swapchain))) {
            surface.swapchain = XR_NULL_HANDLE;
            return false;
        }
        std::uint32_t count{};
        if (XR_FAILED(enumerate_images(surface.swapchain, 0, &count, nullptr)) || !count || count > 64) return false;
        auto& images = surface.images;
        images.resize(count);
        if (device11) {
            std::vector<XrSwapchainImageD3D11KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(enumerate_images(surface.swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < images.size(); ++i) {
                images[i].texture11 = buffers[i].texture;
                if (!images[i].texture11) return false;
            }
        } else {
            std::vector<XrSwapchainImageD3D12KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            if (XR_FAILED(enumerate_images(surface.swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < images.size(); ++i) {
                auto& image = images[i];
                image.texture12 = buffers[i].texture;
                if (!image.texture12) return false;
                const auto desc = image.texture12->GetDesc();
                UINT64 bytes{};
                device12->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, nullptr, nullptr, &bytes);
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC upload_desc{};
                upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                upload_desc.Width = bytes;
                upload_desc.Height = upload_desc.DepthOrArraySize = upload_desc.MipLevels = 1;
                upload_desc.SampleDesc.Count = 1;
                upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&image.upload))) ||
                    FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&image.allocator))) ||
                    FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, image.allocator.Get(),
                        nullptr, IID_PPV_ARGS(&image.list))) || FAILED(image.list->Close())) return false;
            }
        }
        surface.quad.space = space;
        surface.quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        surface.quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        surface.quad.subImage.swapchain = surface.swapchain;
        surface.quad.subImage.imageRect.extent = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
        surface.quad.pose.orientation.w = 1;
        surface.initialized = true;
        return true;
    }

    // Uploads the top `rows` rows of `pixels` into the surface's next image.
    // A failure the overlay cannot recover from marks the surface failed.
    bool upload(Surface& surface, const std::vector<std::uint32_t>& pixels, std::uint32_t rows) {
        if (!surface.acquired) {
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(acquire(surface.swapchain, &info, &surface.index))) { surface.failed = true; return false; }
            surface.acquired = true;
            surface.waited = false;
        }
        if (surface.index >= surface.images.size()) { surface.failed = true; return false; }
        if (!surface.waited) {
            XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            info.timeout = 0;
            const auto result = wait(surface.swapchain, &info);
            // XR_TIMEOUT_EXPIRED is positive, but does NOT grant ownership.
            if (result == XR_TIMEOUT_EXPIRED) return false;
            if (result != XR_SUCCESS) { surface.failed = true; return false; }
            surface.waited = true;
        }
        auto& image = surface.images[surface.index];
        const auto completed = fence12 ? fence12->GetCompletedValue() : fence11->GetCompletedValue();
        if (completed == UINT64_MAX) { surface.failed = true; return false; }
        if (completed < image.fence_value) return false;
        rows = std::min(rows, surface.height);
        if (pixels.size() < static_cast<std::size_t>(surface.width) * rows || rows == 0) return false;
        HRESULT result = S_OK;
        const auto value = next_fence++;
        if (device11) {
            // Runs on the same application end-frame thread as bridge capture,
            // never from the asynchronous presenter. No context state changes.
            const D3D11_BOX box{0, 0, 0, surface.width, rows, 1};
            if (multithread11) multithread11->Enter();
            context11->UpdateSubresource(image.texture11.Get(), 0, &box, pixels.data(), surface.width * 4, 0);
            result = context11->Signal(fence11.Get(), value);
            context11->Flush();
            if (multithread11) multithread11->Leave();
        } else {
            if (FAILED(image.allocator->Reset()) || FAILED(image.list->Reset(image.allocator.Get(), nullptr))) {
                surface.failed = true; return false;
            }
            void* mapped{};
            const D3D12_RANGE empty{};
            if (FAILED(image.upload->Map(0, &empty, &mapped))) { surface.failed = true; return false; }
            for (std::uint32_t row = 0; row < rows; ++row)
                std::memcpy(static_cast<std::uint8_t*>(mapped) + image.footprint.Offset +
                    static_cast<std::size_t>(row) * image.footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<std::size_t>(row) * surface.width, surface.width * 4);
            image.upload->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = image.texture12.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            image.list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = image.footprint;
            destination.pResource = image.texture12.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, 0, 0, surface.width, rows, 1};
            image.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            image.list->ResourceBarrier(1, &barrier);
            if (FAILED(image.list->Close())) { surface.failed = true; return false; }
            ID3D12CommandList* lists[]{image.list.Get()};
            queue12->ExecuteCommandLists(1, lists);
            result = queue12->Signal(fence12.Get(), value);
        }
        if (FAILED(result)) { surface.failed = true; return false; }
        last_fence = image.fence_value = value;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(release(surface.swapchain, &info))) { surface.failed = true; return false; }
        surface.acquired = surface.waited = false;
        surface.image_valid = true;
        return true;
    }

    // The rate the counter draws: what the headset received where SteamVR
    // says, less the share that was repeats, and the refresh rate itself
    // within 2% of it.
    float shown_rate(std::int64_t now, const FpsSnapshot& snapshot, float* delivered = nullptr) {
        float rate = snapshot.submitted_fps;
        if (delivery) {
            if (const auto received = delivery->delivered_fps(now)) {
                rate = delivered_new_images(*received, snapshot);
                if (delivered) *delivered = *received;
            }
        }
        if (const auto period = display_period_ns.load(std::memory_order_relaxed); period > 0) {
            rate = displayed_rate(rate, 1.0e9f / static_cast<float>(period));
        }
        return rate;
    }

    void refresh_counter(const XrFrameEndInfo* info, std::int64_t now) {
        if (position == FpsOverlayPosition::off || disabled || number.failed ||
            !info->layerCount || !info->layers) return;
        float left = -1, right = 1, down = -1, up = 1;
        std::uint32_t eye_width = 0;
        for (std::uint32_t i = 0; i < info->layerCount; ++i) {
            const auto* layer = info->layers[i];
            if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            if (!projection->views || projection->viewCount > 16) continue;
            for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
                const auto& view = projection->views[v];
                if (view.subImage.imageRect.extent.width <= 0) continue;
                const auto w = static_cast<std::uint32_t>(view.subImage.imageRect.extent.width);
                eye_width = eye_width ? std::min(eye_width, w) : w;
                // The quad uses physical visible bounds, not the source image's
                // vertical storage direction. Never reorder the submitted FOV.
                const float down_angle = std::min(view.fov.angleDown, view.fov.angleUp);
                const float up_angle = std::max(view.fov.angleDown, view.fov.angleUp);
                if (std::isfinite(view.fov.angleLeft) && std::isfinite(view.fov.angleRight) &&
                    std::isfinite(view.fov.angleDown) && std::isfinite(view.fov.angleUp) &&
                    view.fov.angleLeft < 0 && view.fov.angleRight > 0 &&
                    down_angle < 0 && up_angle > 0 &&
                    down_angle > -1.57079632679489661923F && up_angle < 1.57079632679489661923F) {
                    left = std::max(left, std::tan(view.fov.angleLeft));
                    right = std::min(right, std::tan(view.fov.angleRight));
                    down = std::max(down, std::tan(down_angle));
                    up = std::min(up, std::tan(up_angle));
                }
            }
        }
        if (!eye_width) return;
        placement = overlay_placement(position, left, right, down, up);
        placement_valid = true;
        const std::uint32_t width = overlay_texture_width(eye_width);
        if (!create_surface(number, width, width / 2, {DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})) return;
        // A quad layer in front of the view, which the runtime composites in
        // the order the layer list gives. An application that submits its own
        // layer over the whole view - a HUD, a menu, a loading or fade overlay
        // - is composited on top of this one and hides the counter until the
        // head turns enough to move it off. Reproduced in Callisto Protocol,
        // and reported as the counter vanishing, so rule it out before
        // suspecting the upload path.
        number.quad.pose.position = {placement.x, placement.y, placement.z};
        number.quad.size = {placement.width, placement.height};
        auto snapshot = counter.snapshot(now);
        snapshot.paused = paused;
        snapshot.submitted_fps = shown_rate(now, snapshot);
        const auto pixels = rasterize_fps_overlay(number.width, number.height, snapshot, number.bgra());
        if (pixels.empty()) return;
        // A counter that cannot upload stops the overlay, as it always has:
        // no quad and no diagnostic marker.
        if (!upload(number, pixels, number.height) && number.failed) disabled = true;
    }

    // Whether the panel has to be painted now: it is up, or a controller has
    // just been turned over, and a quarter second has passed since the last.
    [[nodiscard]] bool panel_due(std::int64_t now) const noexcept {
        if (disabled || panel.failed || panel_mode == StatusPanelMode::off) return false;
        const bool wanted = panel_mode == StatusPanelMode::always || gesture.visible() || gesture.pending();
        return wanted && now >= next_panel_refresh;
    }

    void refresh_panel(const StatusPanelInput& status, std::int64_t now) {
        if (!panel_due(now)) return;
        next_panel_refresh = now + 250'000'000;
        if (!create_surface(panel, kStatusPanelWidth, kStatusPanelHeight, {
                // sRGB first: the panel's colours are chosen in it, and on a
                // UNORM swapchain they are converted to linear light.
                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM})) {
            panel.failed = true;
            return;
        }
        StatusPanelInput input = status;
        const auto snapshot = counter.snapshot(now);
        auto& rates = input.rates;
        float delivered = -1.0F;
        rates.shown = shown_rate(now, snapshot, &delivered);
        rates.delivered = delivered;
        rates.generated = snapshot.generated_fps;
        rates.game = std::max(snapshot.submitted_fps - snapshot.generated_fps, 0.0F);
        rates.repeats = snapshot.repeated_fps;
        rates.generating = snapshot.active;
        if (const auto period = display_period_ns.load(std::memory_order_relaxed); period > 0) {
            rates.refresh_hz = 1.0e9F / static_cast<float>(period);
        }
        std::uint32_t rows = 0;
        const auto pixels = rasterize_status_panel(panel.width, panel.height, input, panel.bgra(),
                                                   !panel.srgb(), &rows);
        if (pixels.empty()) return;
        // The upload can wait out an image the runtime still holds; the rows
        // the quad shows are the ones the last upload that went through wrote.
        if (upload(panel, pixels, rows)) panel_rows = rows;
    }

    // Where a grip is at `time`, in the LOCAL space. Only a pose the runtime
    // is tracking counts, as in xrFPS: runtimes keep VALID set on a
    // controller they have lost, serving its last pose, and a panel shown on
    // that would freeze in mid-air.
    bool locate_grip(int hand, XrTime time, Pose& pose) const {
        if (!grips[hand] || !local || !locate_space) return false;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(locate_space(grips[hand], local, time, &location))) return false;
        constexpr XrSpaceLocationFlags kNeeded =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if ((location.locationFlags & kNeeded) != kNeeded) return false;
        const auto& o = location.pose.orientation;
        const auto& p = location.pose.position;
        pose = {{o.x, o.y, o.z, o.w}, {p.x, p.y, p.z}};
        return true;
    }

    // Decides, for one submission, whether the panel goes on and where.
    bool place_panel(XrTime time, std::int64_t now) {
        if (disabled || panel.failed || panel_mode == StatusPanelMode::off) return false;
        float width = kPanelInViewWidth;
        if (panel_mode == StatusPanelMode::always) {
            panel.quad.space = space;
            // Turned about X to face the eye from below the line of sight.
            const float tilt = -std::atan2(kPanelInViewDrop, kPanelInViewDistance);
            panel.quad.pose.orientation = {std::sin(tilt * 0.5F), 0.0F, 0.0F, std::cos(tilt * 0.5F)};
            panel.quad.pose.position = {0.0F, -kPanelInViewDrop, -kPanelInViewDistance};
        } else {
            std::array<Pose, 2> pose{};
            std::array<bool, 2> flipped{};
            for (int hand = 0; hand < 2; ++hand) {
                flipped[hand] = locate_grip(hand, time, pose[hand]) &&
                    grip_upside_down(pose[hand].orientation);
            }
            const bool shown = gesture.observe(now, flipped[0], flipped[1]);
            if (shown != panel_logged_visible) {
                panel_logged_visible = shown;
                // 4 shown, a the hand (1 left, 2 right); 5 hidden.
                bridge_flight_logger().event(BridgeFlightOperation::status_panel, shown ? 4 : 5,
                    static_cast<std::uint64_t>(shown ? gesture.hand() + 1 : 0));
                if (shown) next_panel_refresh = 0;
            }
            const int hand = gesture.hand();
            // Its pose is the controller's now, not where it was turned over:
            // through the hide delay too, which is what lets it follow the
            // hand down as it comes back up.
            if (!shown || hand < 0 || !locate_grip(hand, time, pose[hand])) return false;
            width = kPanelOnControllerWidth;
            const float height = width * static_cast<float>(std::max(panel_rows, 1u)) /
                static_cast<float>(kStatusPanelWidth);
            const Pose placed = panel_pose_on_grip(pose[hand], height);
            panel.quad.space = local;
            panel.quad.pose.orientation = {placed.orientation.x, placed.orientation.y,
                                           placed.orientation.z, placed.orientation.w};
            panel.quad.pose.position = {placed.position.x, placed.position.y, placed.position.z};
        }
        if (!panel.image_valid || panel_rows == 0) return false;
        panel.quad.subImage.imageRect.extent = {static_cast<std::int32_t>(panel.width),
                                                static_cast<std::int32_t>(panel_rows)};
        panel.quad.size = {width, width * static_cast<float>(panel_rows) / static_cast<float>(panel.width)};
        return true;
    }

    // The gesture needs a LOCAL space to read the grips in: made on the
    // application's thread once there are grips to read.
    void prepare_gesture() {
        if (panel_mode != StatusPanelMode::gesture || local_attempted || (!grips[0] && !grips[1])) return;
        local_attempted = true;
        if (!initialize() || !locate_space) return;
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        info.poseInReferenceSpace.orientation.w = 1;
        if (XR_FAILED(create_space(session, &info, &local))) local = XR_NULL_HANDLE;
    }

    void application_frame(const XrFrameEndInfo* info, const StatusPanelInput* status) {
        const auto now = now_ns();
        if (!info || info->type != XR_TYPE_FRAME_END_INFO) return;
        if (now >= next_refresh) {
            next_refresh = now + 250'000'000;
            position = parse_overlay_position(read_overlay_setting(ini, L"position", L"upper_right"));
            const auto mode = parse_status_panel_mode(read_overlay_setting(ini, L"panel", L"gesture"));
            if (mode != panel_mode) {
                panel_mode = mode;
                gesture.reset();
                next_panel_refresh = 0;
            }
            refresh_counter(info, now);
        }
        if (disabled) return;
        prepare_gesture();
        if (status) refresh_panel(*status, now);
    }
};

OpenXrFpsOverlay::OpenXrFpsOverlay(XrInstance instance, XrSession session, XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc, PFN_xrEndFrame end_frame,
    ID3D12Device* device12, ID3D12CommandQueue* queue12,
    ID3D11Device* device11, const std::filesystem::path& ini,
    SteamVrDelivery* delivery) : impl_(std::make_unique<Impl>()) {
    impl_->instance = instance; impl_->session = session; impl_->system = system;
    impl_->get = get_proc; impl_->downstream_end = end_frame;
    impl_->device12 = device12; impl_->queue12 = queue12; impl_->device11 = device11;
    impl_->ini = ini;
    impl_->delivery = delivery;
}
OpenXrFpsOverlay::~OpenXrFpsOverlay() = default;

void OpenXrFpsOverlay::application_frame(
    const XrFrameEndInfo* info, const StatusPanelInput* status) noexcept {
    try { std::scoped_lock lock(impl_->mutex); impl_->application_frame(info, status); }
    catch (...) { /* Overlay is optional: never fail the game's frame. */ }
}

bool OpenXrFpsOverlay::status_wanted() noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->panel_due(now_ns());
}

void OpenXrFpsOverlay::set_grip_spaces(XrSpace left, XrSpace right) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->grips = {left, right};
    impl_->gesture.reset();
}

void OpenXrFpsOverlay::reset_metrics() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->counter.reset();
    impl_->number.image_valid = false;
    impl_->next_refresh = 0;
    impl_->next_panel_refresh = 0;
    impl_->placement_valid = false;
}

void OpenXrFpsOverlay::set_display_period(std::int64_t period_ns) noexcept {
    impl_->display_period_ns.store(period_ns > 0 ? period_ns : 0, std::memory_order_relaxed);
}

void OpenXrFpsOverlay::set_paused(bool paused) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->paused == paused) return;
    impl_->paused = paused;
    impl_->next_refresh = 0; // Redraw now, not at the next quarter second.
    impl_->next_panel_refresh = 0;
}

void OpenXrFpsOverlay::suspend() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->disabled = true;
    impl_->number.image_valid = false;
    impl_->panel.image_valid = false;
    impl_->counter.reset();
}

FpsSnapshot OpenXrFpsOverlay::metrics() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->counter.snapshot(now_ns());
}

std::optional<OverlayPlacement> OpenXrFpsOverlay::marker_placement() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->disabled || impl_->position == FpsOverlayPosition::off || !impl_->placement_valid)
        return std::nullopt;
    return impl_->placement;
}

XrResult OpenXrFpsOverlay::end_frame(
    const XrFrameEndInfo* info, bool synthetic, bool new_content) {
    std::scoped_lock lock(impl_->mutex);
    auto& impl = *impl_;
    std::array<const XrCompositionLayerBaseHeader*, 128> layers{};
    XrFrameEndInfo composed{};
    const bool nonempty = info && info->type == XR_TYPE_FRAME_END_INFO && info->layers && info->layerCount;
    const XrFrameEndInfo* submitted = info;
    // The panel stands in for the counter while it is up: it shows the same
    // number, larger.
    const XrCompositionLayerBaseHeader* added = nullptr;
    if (nonempty && impl.place_panel(info->displayTime, now_ns())) {
        added = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl.panel.quad);
    } else if (nonempty && impl.number.initialized && impl.number.image_valid && !impl.disabled &&
               impl.position != FpsOverlayPosition::off) {
        added = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl.number.quad);
    }
    if (added && info->layerCount < impl.max_layers) {
        std::copy_n(info->layers, info->layerCount, layers.begin());
        layers[info->layerCount] = added;
        composed = *info; // Preserve every app layer, next chain and display time.
        composed.layers = layers.data();
        ++composed.layerCount;
        submitted = &composed;
    }
    const auto result = impl.downstream_end(impl.session, submitted);
    if (result == XR_SUCCESS && nonempty)
        impl.counter.submitted(now_ns(), synthetic, new_content);
    return result;
}

} // namespace xrfg
