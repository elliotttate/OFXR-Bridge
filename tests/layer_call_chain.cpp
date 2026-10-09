#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// Before openxr_platform.h, which expects the Vulkan types to exist.
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr char kLayerName[] = "XR_APILAYER_XRFrameBridge_diagnostic";
constexpr XrDuration kFakeDisplayPeriod = 10'000'000;
constexpr XrDuration kGenerationCooldownDuration = 1'000'000'000;

template <typename Handle>
[[nodiscard]] Handle fake_handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

XrInstance g_instance = fake_handle<XrInstance>(0x101);
XrSession g_session = fake_handle<XrSession>(0x202);
XrSwapchain g_application_swapchain = fake_handle<XrSwapchain>(0x303);
XrSwapchain g_current_swapchain = fake_handle<XrSwapchain>(0x304);
XrSwapchain g_synthetic_swapchain = fake_handle<XrSwapchain>(0x305);
XrSwapchain g_application_swapchain_right = fake_handle<XrSwapchain>(0x306);
// d3d11-bridge: the application's depth swapchain, in the packed
// depth-stencil format D3D11 will not open shared. The layer keeps its depth
// private and must strip the depth information naming it from every
// submission; the fake counts the ones that reach it.
XrSwapchain g_application_depth_swapchain = fake_handle<XrSwapchain>(0x30d);
std::atomic<std::uint32_t> g_private_depth_submissions{0};
XrSwapchain g_current_swapchain_right = fake_handle<XrSwapchain>(0x307);
XrSwapchain g_synthetic_swapchain_right = fake_handle<XrSwapchain>(0x308);
// The layer alternates its current output between two private swapchains so a
// released frame cannot repoint the previous frame's queued submission, so it
// creates two per application swapchain and this runtime hands out both.
XrSwapchain g_current_swapchain_b = fake_handle<XrSwapchain>(0x309);
XrSwapchain g_current_swapchain_right_b = fake_handle<XrSwapchain>(0x30a);
// The synthetic output alternates across two slots as well, so that the
// application can release the next pair's synthetic while the presenter still
// has the previous one un-retired.
XrSwapchain g_synthetic_swapchain_b = fake_handle<XrSwapchain>(0x30b);
XrSwapchain g_synthetic_swapchain_right_b = fake_handle<XrSwapchain>(0x30c);
XrSpace g_space = fake_handle<XrSpace>(0x404);
std::atomic<XrSpace> g_valid_composition_space{g_space};
std::atomic<bool> g_delay_composition_validation{false};
XrTime g_next_display_time = 100;
bool g_split_eye_mode = false;
// Whether the layer under test runs the deeper pipeline. Read from the same
// ofxr_bridge.ini the layer reads, beside the layer DLL, so the fake runtime
// expects the ring sizes the layer will actually ask for: one synthetic slot
// per application swapchain shallow, two deep.
bool g_deep_pipeline = false;
// Whether the layer runs single-swapchain rings: one private swapchain per
// output, written at the hand-over from staging. Read from the same ini
// (`single_swapchain_rings`, on by default), and never for the interops,
// which keep the rings whatever the ini says.
bool g_single_rings = true;
// Frames the layer hands the runtime per application frame: 3 when the ini
// beside the layer turns "3X Frame Gen" on, and the virtual period the
// application is served follows it.
XrDuration g_frames_per_application_frame = 2;
bool g_cropped_subimage_mode = false;
bool g_double_wide_mode = false;
bool g_uevr_pipelined_display_time_mode = false;
bool g_d3d11_interop_mode = false;
// d3d11-bridge: the application binds a real D3D11 device with the bridge
// on, and the fake runtime is what a D3D12-capable runtime is to it: it
// lists XR_KHR_D3D12_enable, answers the D3D12 requirements call, and expects
// a D3D12 binding at xrCreateSession, on which it creates its images. The
// application then renders into D3D11 textures the layer gave it, and what
// it rendered has to arrive in the runtime's D3D12 images.
bool g_d3d11_bridge_mode = false;
// d3d11-bridge-acquire-ahead: the application keeps one image acquired
// ahead of the one it renders, and the first acquire is outstanding across
// a session restart (Ready or Not's VR mod acquires before xrBeginSession).
bool g_acquire_ahead_mode = false;
std::atomic<bool> g_acquire_ahead_active{false};
std::atomic<std::uint32_t> g_acquire_ahead_calls{0};
std::atomic<bool> g_bridge_session_bound{false};
[[nodiscard]] bool create_fake_d3d12_images();
// vulkan: the application binds a real Vulkan device, so the layer's Vulkan
// interop runs against the driver rather than a stand-in. Importing the D3D12
// textures and the shared fence is the part no fake can vouch for.
bool g_vulkan_mode = false;
// vulkan-bridge: a Vulkan application whose session the layer hands to the
// runtime as a D3D12 one. The runtime side of this fake is then its D3D12
// self; only the application side is Vulkan.
bool g_vulkan_bridge_mode = false;
bool g_vulkan_application = false;
bool g_inverted_vertical_fov = false;
bool g_steamvr_runtime_mode = false;
// Another runtime's name, for the rules the layer keys on it.
const char* g_runtime_name_override = nullptr;
bool g_steamvr_presenter_mode = false;
// promise-shown-time: the presenter path on a runtime whose display time
// advances a whole period per wait, as a real one does, under an application
// that takes a period and a half to render each frame - long enough that its
// real frames go down after the time first promised for them.
bool g_promise_mode = false;
// Set while the application is inside xrEndFrame. The layer runs its inline
// second wait/begin/end cycle from that call on this same thread, which is
// exactly what a throttling SteamVR configuration slows down, so this tells
// the two waits apart by what is happening rather than by counting calls.
std::atomic<bool> g_application_in_end_frame{false};
// d3d11-single-threaded: the application's D3D11 device is created
// D3D11_CREATE_DEVICE_SINGLETHREADED, as Unity does by default, and the fake
// SteamVR runtime throttles the inline second cycle the way that promotes a
// presenter thread on any other session. No frame call may reach the runtime
// from a thread other than the application's: the runtime uses that device
// inside them, and D3D11 does no locking of its own on such a device.
bool g_single_threaded_mode = false;
// dcs-d3d11: the dcs shape on a D3D11 device. Such a session never takes a
// presenter (a driver workaround; see presenter_forbidden in the layer), so
// no frame call may reach the runtime from any thread but the application's
// own two: its render thread and its wait thread.
bool g_dcs_d3d11_mode = false;
std::atomic<DWORD> g_application_wait_thread_id{0};
std::atomic<std::uint32_t> g_off_thread_frame_calls{0};
// swapchain-budget: the runtime allows three private swapchains beside the
// application's, one short of the deeper pipeline's four, and refuses the
// fourth with XR_ERROR_LIMIT_REACHED - what SteamVR does at its cap of 16
// for a UEVR title that creates eight of its own. The layer has to fall back
// to the shallow depth and keep generating. Private swapchains are routed by
// how many are alive rather than by call index, so the second attempt lands
// on the same handles the first one gave back.
bool g_swapchain_budget_mode = false;
std::atomic<std::uint32_t> g_budget_live_privates{0};
std::atomic<std::uint32_t> g_budget_refusals{0};

// dcs: DCS World's frame loop on a throttling SteamVR. One thread calls
// xrWaitFrame and keeps two waits in flight - the next is issued before the
// render thread has begun the current frame and returns once it has - and
// the render thread labels each xrEndFrame with the older of the two. Once
// the session has taken the presenter the application's waits are answered
// virtually and nothing marks them as overlapping, so the label lookup alone
// decides whether the frame pairs, and it has to accept the older pending
// frame. The fake runtime behaves exactly as in steamvr-presenter.
bool g_dcs_mode = false;
bool g_flight_simulator_mode = false;
bool g_destroy_pending_swapchain = false;
bool g_destroy_pending_space = false;
// steamvr-layer-invalid: the runtime refuses one of the presenter's
// submissions for its contents, as Virtual Desktop's runtime refused a
// loading-screen layer naming a swapchain with no released image. The
// session has to carry on: an application sees that error for one frame.
bool g_refuse_layer_mode = false;
std::atomic<std::uint32_t> g_presenter_projection_submissions{0};
std::atomic<std::uint32_t> g_layer_refusals{0};
// steamvr-own-time: MSFS 2024's labelling on the throttling SteamVR. Every
// frame is ended with a display time of the application's own, never the
// one its xrWaitFrame returned, and the loop is sequential, so the session
// is promoted by the SteamVR route and the layer can only tell which wait a
// frame belongs to by elimination. The runtime rejects one inline frame's
// display time as past, which MSFS 2024 produces after a hitch; that wait
// must not stay pending, or no later frame of this title ever pairs again.
bool g_own_display_time_mode = false;
std::atomic<bool> g_reject_end_time_invalid{false};
std::atomic<std::uint32_t> g_time_rejections{0};
std::atomic<bool> g_swapchain_destroyed{false};
std::atomic<unsigned> g_submission_after_destroy{0};
DWORD g_test_application_thread_id{};
void record_frame_call_thread() noexcept {
    const DWORD thread = GetCurrentThreadId();
    if ((g_single_threaded_mode || g_vulkan_mode || g_dcs_d3d11_mode) &&
        thread != g_test_application_thread_id &&
        thread != g_application_wait_thread_id.load(std::memory_order_acquire)) {
        g_off_thread_frame_calls.fetch_add(1, std::memory_order_relaxed);
    }
}
std::atomic<bool> g_concurrent_acquire_mode{false};
std::atomic<int> g_concurrent_acquire_count{0};
std::atomic<bool> g_first_concurrent_acquire_entered{false};
bool g_throw_from_begin_frame = false;
std::atomic<bool> g_fail_next_release{false};
std::atomic<bool> g_fail_next_synthetic_release{false};
std::atomic<bool> g_fail_next_wait_frame{false};
std::atomic<bool> g_next_wait_should_not_render{false};
std::atomic<XrTime> g_delay_end_time{0};
std::atomic<bool> g_block_atomic_end{false};
std::atomic<XrTime> g_block_atomic_end_time{0};
std::atomic<bool> g_atomic_end_entered{false};
std::atomic<bool> g_allow_atomic_end_return{false};
std::atomic<bool> g_wait_begin_handoff_mode{false};
std::atomic<std::uint32_t> g_wait_begin_handoff_calls{0};
std::atomic<bool> g_second_handoff_wait_entered{false};
std::atomic<bool> g_allow_second_handoff_wait_return{false};
std::atomic<std::uint32_t> g_wait_frame_calls{0};
std::atomic<std::uint32_t> g_begin_frame_calls{0};
std::atomic<std::uint32_t> g_end_frame_calls{0};
std::atomic<std::uint32_t> g_locate_views_calls{0};
std::atomic<std::uint32_t> g_create_swapchain_calls{0};
// Split-eye counts the two kinds of creation separately. The layer takes its
// private swapchains when a projection layer first names an application
// swapchain rather than when that swapchain's images are enumerated, so both
// eyes now exist before any private swapchain does and a single call index no
// longer identifies which swapchain is being created.
std::atomic<std::uint32_t> g_split_eye_application_creates{0};
std::atomic<std::uint32_t> g_split_eye_private_creates{0};
std::atomic<std::uint32_t> g_destroy_swapchain_calls{0};
// How many synthetic slots the layer asked for. The count depends on the
// pipeline depth the session runs at - one shallow, two deep - so the
// expectations below are written against it rather than against a literal,
// and hold whichever depth the layer was built or configured for.
std::atomic<std::uint32_t> g_synthetic_private_creates{0};
std::atomic<std::uint32_t> g_application_release_calls{0};
std::atomic<std::uint32_t> g_current_acquire_calls{0};
std::atomic<std::uint32_t> g_current_wait_calls{0};
std::atomic<std::uint32_t> g_current_release_calls{0};
std::atomic<std::uint32_t> g_synthetic_acquire_calls{0};
std::atomic<std::uint32_t> g_synthetic_wait_calls{0};
std::atomic<std::uint32_t> g_synthetic_release_calls{0};
std::atomic<bool> g_current_create_info_valid{false};
std::atomic<bool> g_synthetic_create_info_valid{false};
std::mutex g_frame_loop_mutex;
std::deque<XrTime> g_waited_display_times;
std::optional<XrTime> g_begun_display_time;
// The frame-loop rule: a runtime blocks xrWaitFrame while an earlier waited
// frame has not been begun, and SteamVR hung DCS World on exactly that when
// the layer's presenter issued its first wait over the application's. A
// blocking fake would hang the suite, so the fake fails the call instead
// and counts it; a scenario that provokes one has found a deadlock.
std::atomic<std::uint32_t> g_waits_while_unbegun{0};
// Who waited each queued frame, so a begin can be told apart by thread: the
// application's (its render or wait thread) against the layer's presenter.
// A frame waited by the application and begun by the presenter is one the
// presenter adopted at its start.
std::deque<DWORD> g_waited_by_thread;
std::atomic<std::uint32_t> g_presenter_begun_application_waits{0};

enum class SubmittedTarget {
    none,
    original,
    current,
    synthetic,
    mixed,
};

struct EndFrameRecord {
    XrTime display_time{};
    SubmittedTarget target{SubmittedTarget::none};
    std::uint32_t layer_count{};
    XrSpace space{XR_NULL_HANDLE};
    std::array<XrPosef, 2> poses{};
    std::array<XrFovf, 2> fovs{};
    std::array<XrRect2Di, 2> image_rects{};
    bool passthrough_layer_preserved{};
    std::uint32_t passthrough_layer_index{
        std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t quad_layer_count{};
    std::uint32_t quad_layer_index{
        std::numeric_limits<std::uint32_t>::max()};
};

struct LocateViewsRecord {
    XrTime display_time{};
    XrSpace space{XR_NULL_HANDLE};
    XrViewConfigurationType view_configuration_type{};
};

std::mutex g_end_records_mutex;
std::vector<EndFrameRecord> g_end_records;
std::mutex g_locate_records_mutex;
std::vector<LocateViewsRecord> g_locate_records;
std::string g_log_path;
const XrCompositionLayerBaseHeader* g_expected_passthrough_layer = nullptr;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D11Device> g_d3d11_device;
ComPtr<ID3D11DeviceContext> g_d3d11_context;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_application_swapchain_images;
std::array<VkImage, 3> g_vulkan_application_swapchain_images{};
std::array<VkImage, 3> g_vulkan_current_swapchain_images{};
std::array<VkImage, 3> g_vulkan_current_swapchain_b_images{};
std::array<VkImage, 3> g_vulkan_synthetic_swapchain_images{};
std::array<VkImage, 3> g_vulkan_synthetic_swapchain_b_images{};

// Everything the vulkan mode creates, loaded through vulkan-1.dll the way an
// application does; the test links no Vulkan library either. Nothing is
// destroyed: the layer's own Vulkan objects go with its sessions, and on a
// path that leaves a session behind they go at process exit, which has to
// find the device still there.
struct VulkanTestDevice {
    HMODULE module{};
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t queue_family{};
    VkQueue queue{};
    VkCommandPool command_pool{};
    PFN_vkQueueWaitIdle queue_wait_idle{};
    PFN_vkAllocateCommandBuffers allocate_command_buffers{};
    PFN_vkBeginCommandBuffer begin_command_buffer{};
    PFN_vkEndCommandBuffer end_command_buffer{};
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
    PFN_vkCmdClearColorImage cmd_clear_color_image{};
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer{};
    PFN_vkQueueSubmit queue_submit{};
    PFN_vkResetCommandPool reset_command_pool{};
    // A host-visible buffer one pixel of every private image is read back
    // through, mapped for the life of the device.
    VkBuffer readback{};
    VkDeviceMemory readback_memory{};
    void* readback_mapped{};
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memories;
};
VulkanTestDevice g_vulkan;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_current_swapchain_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_current_swapchain_b_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_synthetic_swapchain_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_synthetic_swapchain_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_application_swapchain_images;
std::array<ComPtr<ID3D12Resource>, 3> g_application_depth_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_application_swapchain_right_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_right_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_right_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_right_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_right_b_images;

[[nodiscard]] bool is_application_swapchain(XrSwapchain swapchain) {
    return swapchain == g_application_swapchain ||
           swapchain == g_application_swapchain_right;
}

[[nodiscard]] bool is_current_swapchain(XrSwapchain swapchain) {
    return swapchain == g_current_swapchain ||
           swapchain == g_current_swapchain_right ||
           swapchain == g_current_swapchain_b ||
           swapchain == g_current_swapchain_right_b;
}

[[nodiscard]] bool is_synthetic_swapchain(XrSwapchain swapchain) {
    return swapchain == g_synthetic_swapchain ||
           swapchain == g_synthetic_swapchain_b ||
           swapchain == g_synthetic_swapchain_right ||
           swapchain == g_synthetic_swapchain_right_b;
}

[[nodiscard]] XrTime fake_camera_time(XrTime display_time) noexcept {
    if (g_promise_mode) {
        // A still head: the scenario is about time, not motion.
        return 0;
    }
    if (g_steamvr_presenter_mode || g_flight_simulator_mode) {
        return (display_time / kFakeDisplayPeriod) * 100;
    }
    return display_time % kGenerationCooldownDuration;
}

[[nodiscard]] XrView fake_view_for_time(XrTime display_time, std::uint32_t index) {
    // The fake runtime advances in compact synthetic ticks, then jumps one
    // second to exercise cooldown expiry. Keep its camera values bounded
    // across that artificial timeline jump.
    const XrTime camera_time = fake_camera_time(display_time);
    const float sample = static_cast<float>(camera_time) / 1000.0F;
    const float half_yaw = sample * 0.35F;

    XrView view{XR_TYPE_VIEW};
    view.pose.orientation.y = std::sin(half_yaw);
    view.pose.orientation.w = std::cos(half_yaw);
    view.pose.position.x = sample + static_cast<float>(index) * 0.01F;
    view.pose.position.y =
        1.6F + sample * 0.02F + static_cast<float>(index) * 0.002F;
    view.pose.position.z = -sample * 0.03F;
    view.fov.angleLeft = -0.8F - sample * 0.01F - static_cast<float>(index) * 0.001F;
    view.fov.angleRight = 0.8F + sample * 0.012F + static_cast<float>(index) * 0.001F;
    view.fov.angleUp = 0.7F + sample * 0.008F;
    view.fov.angleDown = -0.7F - sample * 0.006F;
    if (g_inverted_vertical_fov) std::swap(view.fov.angleUp, view.fov.angleDown);
    return view;
}

[[nodiscard]] XrView fake_submitted_view_for_time(
    XrTime display_time,
    std::uint32_t index) {
    XrView view = fake_view_for_time(display_time, index);
    const XrTime camera_time = fake_camera_time(display_time);
    const float sample = static_cast<float>(camera_time) / 1000.0F;
    const float submitted_half_yaw = sample * 0.35F + 0.075F;
    view.pose.orientation.y = std::sin(submitted_half_yaw);
    view.pose.orientation.w = std::cos(submitted_half_yaw);
    view.pose.position.x += 0.125F;
    view.pose.position.z += 0.05F;
    view.fov.angleLeft -= 0.005F;
    view.fov.angleRight += 0.007F;
    return view;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_instance(XrInstance) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_create_session(
    XrInstance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    if (g_d3d11_bridge_mode || g_vulkan_bridge_mode) {
        // The bridge hands the runtime a D3D12 binding on the layer's own
        // device. A runtime creates its images on that device; so does the
        // fake, here, since it cannot know the device before this call.
        const XrGraphicsBindingD3D12KHR* binding = nullptr;
        for (auto* next = create_info
                 ? static_cast<const XrBaseInStructure*>(create_info->next)
                 : nullptr;
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
            }
        }
        if (binding == nullptr || binding->device == nullptr ||
            binding->queue == nullptr) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        g_device = binding->device;
        g_queue = binding->queue;
        if (!create_fake_d3d12_images()) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        g_bridge_session_bound.store(true, std::memory_order_release);
    }
    *session = g_session;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_enumerate_instance_extension_properties(
    const char*,
    std::uint32_t capacity,
    std::uint32_t* count,
    XrExtensionProperties* properties) {
    // D3D12 only in d3d11-bridge mode. With the bridge on by default, every
    // other D3D11 mode then runs the direct D3D11 path - the one a runtime
    // without D3D12 gets, and the one d3d11_bridge=0 selects - so both stay
    // covered.
    constexpr const char* kNames[] = {"XR_KHR_D3D11_enable", "XR_KHR_D3D12_enable"};
    const std::uint32_t listed = (g_d3d11_bridge_mode || g_vulkan_bridge_mode) ? 2U : 1U;
    *count = listed;
    if (capacity == 0 || properties == nullptr) {
        return XR_SUCCESS;
    }
    if (capacity < listed) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (std::uint32_t index = 0; index < listed; ++index) {
        std::strncpy(properties[index].extensionName, kNames[index], XR_MAX_EXTENSION_NAME_SIZE - 1);
        properties[index].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
        properties[index].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_get_d3d12_graphics_requirements(
    XrInstance,
    XrSystemId,
    XrGraphicsRequirementsD3D12KHR* requirements) {
    if (requirements == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // The adapter the application's D3D11 device lives on.
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (g_d3d11_device &&
        SUCCEEDED(g_d3d11_device.As(&dxgi_device)) &&
        SUCCEEDED(dxgi_device->GetAdapter(adapter.GetAddressOf())) &&
        SUCCEEDED(adapter->GetDesc(&description))) {
        requirements->adapterLuid = description.AdapterLuid;
    }
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_session(XrSession) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_begin_session(XrSession, const XrSessionBeginInfo*) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_end_session(XrSession) {
    return XR_SUCCESS;
}

// Whether the presenter's first call to the runtime found an application
// wait the runtime still held un-begun: the case the presenter has to adopt
// by beginning that frame. When the application has begun it first there is
// nothing to adopt, and whether it has depends on the threads' timing.
std::atomic<bool> g_presenter_first_call_seen{false};
std::atomic<bool> g_presenter_started_over_application_wait{false};
// With g_frame_loop_mutex held.
void note_presenter_first_call() noexcept {
    const auto is_application_thread = [](DWORD thread) {
        return thread == g_test_application_thread_id ||
            thread == g_application_wait_thread_id.load(std::memory_order_acquire);
    };
    if (is_application_thread(GetCurrentThreadId()) ||
        g_presenter_first_call_seen.exchange(true)) {
        return;
    }
    g_presenter_started_over_application_wait.store(
        !g_waited_by_thread.empty() &&
            is_application_thread(g_waited_by_thread.front()),
        std::memory_order_relaxed);
}

XRAPI_ATTR XrResult XRAPI_CALL fake_wait_frame(
    XrSession,
    const XrFrameWaitInfo*,
    XrFrameState* frame_state) {
    if (frame_state == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const std::uint32_t wait_call =
        g_wait_frame_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    record_frame_call_thread();
    // A SteamVR configuration that paces the application: the wait blocks for
    // most of a display period, so the layer's pacing measurement finds
    // nothing to correct and this mode stays on the inline path. The internal
    // second cycle is deliberately not slowed - that is the other quirk, and
    // the steamvr-presenter mode covers it.
    if (g_steamvr_runtime_mode && !g_steamvr_presenter_mode &&
        !g_application_in_end_frame.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    // The throttled inline cycle, as in steamvr-presenter: on any other
    // session this promotes a presenter thread.
    if (g_single_threaded_mode &&
        g_application_in_end_frame.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    if (g_steamvr_presenter_mode) {
        if (GetCurrentThreadId() == g_test_application_thread_id &&
            g_application_in_end_frame.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        } else if (GetCurrentThreadId() != g_test_application_thread_id) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (g_flight_simulator_mode &&
        GetCurrentThreadId() != g_test_application_thread_id) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (g_wait_begin_handoff_mode.load(std::memory_order_acquire) &&
        g_wait_begin_handoff_calls.fetch_add(1, std::memory_order_acq_rel) == 1) {
        g_second_handoff_wait_entered.store(true, std::memory_order_release);
        const auto escape_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!g_allow_second_handoff_wait_return.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < escape_deadline) {
            std::this_thread::yield();
        }
    }
    if (g_fail_next_wait_frame.exchange(false, std::memory_order_acq_rel)) {
        if (!g_log_path.empty()) {
            std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
            stream << "[XRFG-FAKE] downstream wait frame failed\n";
        }
        return XR_ERROR_RUNTIME_FAILURE;
    }
    // XRFG_TEST_THROTTLE_AFTER_WAITS=N (promise-shown-time): after N waits
    // the runtime halves the rate, as SteamVR does when it judges the caller
    // late - a doubled period, and display times twice as far apart.
    static const long throttle_after_waits = [] {
        const char* setting = std::getenv("XRFG_TEST_THROTTLE_AFTER_WAITS");
        return setting ? std::atol(setting) : 0L;
    }();
    static std::atomic<long> waits_seen{0};
    const bool throttled = g_promise_mode && throttle_after_waits > 0 &&
        waits_seen.fetch_add(1, std::memory_order_relaxed) >= throttle_after_waits;
    frame_state->predictedDisplayTime = g_next_display_time;
    frame_state->predictedDisplayPeriod =
        throttled ? kFakeDisplayPeriod * 2 : kFakeDisplayPeriod;
    frame_state->shouldRender =
        g_next_wait_should_not_render.exchange(false, std::memory_order_acq_rel)
            ? XR_FALSE
            : XR_TRUE;
    g_next_display_time += g_flight_simulator_mode
        ? kFakeDisplayPeriod * 3
        : g_promise_mode ? (throttled ? kFakeDisplayPeriod * 2 : kFakeDisplayPeriod)
        : 100;
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        note_presenter_first_call();
        if (!g_waited_display_times.empty()) {
            g_waits_while_unbegun.fetch_add(1, std::memory_order_relaxed);
            if (!g_log_path.empty()) {
                std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
                stream << "[XRFG-FAKE] wait frame while a waited frame is "
                          "not begun\n";
            }
            return XR_ERROR_RUNTIME_FAILURE;
        }
        g_waited_display_times.push_back(frame_state->predictedDisplayTime);
        g_waited_by_thread.push_back(GetCurrentThreadId());
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream wait frame time="
               << frame_state->predictedDisplayTime << '\n';
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_properties(
    XrInstance,
    XrInstanceProperties* properties) {
    if (properties == nullptr ||
        properties->type != XR_TYPE_INSTANCE_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    strcpy_s(
        properties->runtimeName,
        g_runtime_name_override != nullptr
            ? g_runtime_name_override
            : g_steamvr_runtime_mode
                ? "SteamVR/OpenXR : XRFG fake lighthouse"
                : "XRFG fake runtime");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_begin_frame(XrSession, const XrFrameBeginInfo*) {
    g_begin_frame_calls.fetch_add(1, std::memory_order_relaxed);
    record_frame_call_thread();
    if (g_throw_from_begin_frame) {
        throw std::runtime_error("intentional fake-runtime exception");
    }
    XrTime begun_time = 0;
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        note_presenter_first_call();
        if (g_begun_display_time || g_waited_display_times.empty()) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        begun_time = g_waited_display_times.front();
        g_waited_display_times.pop_front();
        g_begun_display_time = begun_time;
        const DWORD waited_by = g_waited_by_thread.front();
        g_waited_by_thread.pop_front();
        const DWORD beginner = GetCurrentThreadId();
        const auto is_application_thread = [](DWORD thread) {
            return thread == g_test_application_thread_id ||
                thread == g_application_wait_thread_id.load(
                              std::memory_order_acquire);
        };
        if (is_application_thread(waited_by) &&
            !is_application_thread(beginner)) {
            g_presenter_begun_application_waits.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream begin frame time=" << begun_time << '\n';
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_end_frame(
    XrSession,
    const XrFrameEndInfo* end_info) {
    g_end_frame_calls.fetch_add(1, std::memory_order_relaxed);
    record_frame_call_thread();
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        if (!g_begun_display_time || end_info == nullptr ||
            // dcs-d3d11: after the inline cycle adopts the application's
            // held wait, the application labels its frame with the wait it
            // was given while the runtime begun the replacement; a runtime
            // accepts any displayTime, and so does the fake here.
            (!g_flight_simulator_mode &&
             !g_uevr_pipelined_display_time_mode && !g_dcs_d3d11_mode &&
             !g_own_display_time_mode &&
             end_info->displayTime != *g_begun_display_time)) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        g_begun_display_time.reset();
    }
    // The application's own frame, inline: the runtime considers its display
    // time past. The frame is consumed above as a runtime would, and the
    // application sees the error.
    if (g_own_display_time_mode &&
        GetCurrentThreadId() == g_test_application_thread_id &&
        g_reject_end_time_invalid.exchange(false, std::memory_order_acq_rel)) {
        g_time_rejections.fetch_add(1, std::memory_order_relaxed);
        return XR_ERROR_TIME_INVALID;
    }
    EndFrameRecord record{};
    record.display_time = end_info->displayTime;
    record.layer_count = end_info->layerCount;
    bool saw_projection = false;
    std::uint32_t recorded_views = 0;
    if (end_info != nullptr && end_info->layers != nullptr) {
        for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount; ++layer_index) {
            const auto* layer = end_info->layers[layer_index];
            if (layer == g_expected_passthrough_layer) {
                record.passthrough_layer_preserved = true;
                record.passthrough_layer_index = layer_index;
            }
            if (layer != nullptr &&
                layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                ++record.quad_layer_count;
                record.quad_layer_index = layer_index;
            }
            if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                continue;
            }
            const auto* projection =
                reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            record.space = projection->space;
            if (projection->viewCount == 0 || projection->views == nullptr) {
                record.target = SubmittedTarget::mixed;
                continue;
            }
            for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
                saw_projection = true;
                for (const auto* chained = static_cast<const XrBaseInStructure*>(
                         projection->views[view_index].next);
                     chained != nullptr; chained = chained->next) {
                    if (chained->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR &&
                        reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(chained)
                                ->subImage.swapchain == g_application_depth_swapchain) {
                        g_private_depth_submissions.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                const XrSwapchain handle = projection->views[view_index].subImage.swapchain;
                const SubmittedTarget view_target =
                    is_application_swapchain(handle)
                        ? SubmittedTarget::original
                        : is_current_swapchain(handle)
                              ? SubmittedTarget::current
                              : is_synthetic_swapchain(handle)
                                    ? SubmittedTarget::synthetic
                                    : SubmittedTarget::mixed;
                if (record.target == SubmittedTarget::none) {
                    record.target = view_target;
                } else if (record.target != view_target) {
                    record.target = SubmittedTarget::mixed;
                }
                if (recorded_views < record.poses.size()) {
                    record.poses[recorded_views] = projection->views[view_index].pose;
                    record.fovs[recorded_views] = projection->views[view_index].fov;
                    record.image_rects[recorded_views] =
                        projection->views[view_index].subImage.imageRect;
                    ++recorded_views;
                }
            }
        }
    }
    if (!saw_projection && end_info->layerCount != 0) {
        record.target = SubmittedTarget::mixed;
    }
    if (g_flight_simulator_mode &&
        g_delay_composition_validation.exchange(
            false,
            std::memory_order_acq_rel)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_destroy_pending_swapchain && saw_projection &&
        g_swapchain_destroyed.load(std::memory_order_acquire)) {
        ++g_submission_after_destroy;
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_refuse_layer_mode && saw_projection &&
        GetCurrentThreadId() != g_test_application_thread_id &&
        g_presenter_projection_submissions.fetch_add(
            1, std::memory_order_acq_rel) == 2) {
        g_layer_refusals.fetch_add(1, std::memory_order_relaxed);
        return XR_ERROR_LAYER_INVALID;
    }
    if (g_destroy_pending_space && saw_projection &&
        record.space != g_valid_composition_space.load(std::memory_order_acquire)) {
        ++g_submission_after_destroy;
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_flight_simulator_mode && saw_projection &&
        record.space !=
            g_valid_composition_space.load(std::memory_order_acquire)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    {
        std::scoped_lock lock(g_end_records_mutex);
        g_end_records.push_back(record);
    }
    if (end_info->displayTime == g_block_atomic_end_time.load(std::memory_order_acquire) &&
        g_block_atomic_end.exchange(false, std::memory_order_acq_rel)) {
        g_atomic_end_entered.store(true, std::memory_order_release);
        while (!g_allow_atomic_end_return.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    XrTime delayed_time = end_info->displayTime;
    if (g_delay_end_time.compare_exchange_strong(
            delayed_time, 0, std::memory_order_acq_rel)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!g_log_path.empty()) {
        const char* target =
            record.target == SubmittedTarget::original
                ? "original"
                : record.target == SubmittedTarget::current
                      ? "current"
                      : record.target == SubmittedTarget::synthetic
                            ? "synthetic"
                            : record.target == SubmittedTarget::none ? "none" : "mixed";
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream end frame target=" << target
               << " time=" << end_info->displayTime
               << " layers=" << end_info->layerCount
               << " pose_x=" << record.poses[0].position.x << ','
               << record.poses[1].position.x
               << '\n';
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_locate_views(
    XrSession,
    const XrViewLocateInfo* locate_info,
    XrViewState* view_state,
    std::uint32_t view_capacity_input,
    std::uint32_t* view_count_output,
    XrView* views) {
    if (locate_info == nullptr || view_state == nullptr || view_count_output == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    g_locate_views_calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(g_locate_records_mutex);
        g_locate_records.push_back({
            locate_info->displayTime,
            locate_info->space,
            locate_info->viewConfigurationType,
        });
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream locate views time="
               << locate_info->displayTime << " space=" << locate_info->space
               << " config=" << locate_info->viewConfigurationType << '\n';
    }
    *view_count_output = 2;
    view_state->viewStateFlags =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (view_capacity_input >= 2 && views != nullptr) {
        for (std::uint32_t index = 0; index < 2; ++index) {
            const XrView sample = fake_view_for_time(locate_info->displayTime, index);
            views[index].pose = sample.pose;
            views[index].fov = sample.fov;
        }
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_create_swapchain(
    XrSession,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    // The depth swapchain sits outside the call numbering the private slots
    // are routed by.
    if (g_d3d11_bridge_mode && create_info != nullptr &&
        (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        *swapchain = g_application_depth_swapchain;
        return XR_SUCCESS;
    }
    const std::uint32_t call =
        g_create_swapchain_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_split_eye_mode) {
        const bool private_info_valid =
            create_info != nullptr &&
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
            create_info->width == 4 && create_info->height == 4 &&
            create_info->arraySize == 1;
        // Per application swapchain the layer creates two current slots and
        // then two synthetic slots, so each eye claims four private calls.
        const auto record_current = [&](bool first) {
            g_current_create_info_valid.store(
                (first ||
                 g_current_create_info_valid.load(std::memory_order_acquire)) &&
                    private_info_valid,
                std::memory_order_release);
        };
        const auto record_synthetic = [&](bool first) {
            g_synthetic_create_info_valid.store(
                (first ||
                 g_synthetic_create_info_valid.load(std::memory_order_acquire)) &&
                    private_info_valid,
                std::memory_order_release);
        };
        // Only the layer asks for a transfer destination, so that bit says
        // which kind of swapchain this is without depending on when it is
        // asked for. The dimension checks stay an assertion about the private
        // create info rather than a condition for routing it.
        const bool layer_owned =
            create_info != nullptr &&
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0;
        if (!layer_owned) {
            switch (g_split_eye_application_creates.fetch_add(
                1, std::memory_order_relaxed)) {
                case 0:
                    *swapchain = g_application_swapchain;
                    break;
                case 1:
                    *swapchain = g_application_swapchain_right;
                    break;
                default:
                    return XR_ERROR_LIMIT_REACHED;
            }
            return XR_SUCCESS;
        }
        // Per application swapchain the layer creates every current slot and
        // then every synthetic slot, in the order the projection views are
        // mapped.
        //
        // The routing cannot discover the ring sizes: the eye cannot be
        // inferred from how many application swapchains exist, because the
        // layer defers creating its private swapchains until it arms, and two
        // current slots then one synthetic is indistinguishable from two
        // current then two synthetic until the run is over. So it is told
        // the depth, from the ini the layer reads (g_deep_pipeline).
        // Per eye: every current slot, then every synthetic slot - two
        // current, then one synthetic shallow or two deep.
        struct Route {
            XrSwapchain handle;
            bool synthetic;
        };
        // Single-swapchain rings create one of each per eye, current first.
        const std::array<Route, 4> left = g_single_rings
            ? std::array<Route, 4>{{
                  {g_current_swapchain, false},
                  {g_synthetic_swapchain, true},
                  {XR_NULL_HANDLE, false},
                  {XR_NULL_HANDLE, false},
              }}
            : std::array<Route, 4>{{
                  {g_current_swapchain, false},
                  {g_current_swapchain_b, false},
                  {g_synthetic_swapchain, true},
                  {g_synthetic_swapchain_b, true},
              }};
        const std::array<Route, 4> right = g_single_rings
            ? std::array<Route, 4>{{
                  {g_current_swapchain_right, false},
                  {g_synthetic_swapchain_right, true},
                  {XR_NULL_HANDLE, false},
                  {XR_NULL_HANDLE, false},
              }}
            : std::array<Route, 4>{{
                  {g_current_swapchain_right, false},
                  {g_current_swapchain_right_b, false},
                  {g_synthetic_swapchain_right, true},
                  {g_synthetic_swapchain_right_b, true},
              }};
        const std::uint32_t per_eye =
            g_single_rings ? 2U : g_deep_pipeline ? 4U : 3U;
        const std::uint32_t index = g_split_eye_private_creates.fetch_add(
            1, std::memory_order_relaxed);
        if (index >= per_eye * 2U) {
            return XR_ERROR_LIMIT_REACHED;
        }
        const Route& route =
            index < per_eye ? left[index] : right[index - per_eye];
        if (route.synthetic) {
            record_synthetic(index == (g_single_rings ? 1U : 2U));
            g_synthetic_private_creates.fetch_add(1, std::memory_order_relaxed);
        } else {
            record_current(index == 0U);
        }
        *swapchain = route.handle;
        return XR_SUCCESS;
    }
    if (g_swapchain_budget_mode && call > 0) {
        const std::uint32_t live =
            g_budget_live_privates.load(std::memory_order_acquire);
        if (live >= 3) {
            g_budget_refusals.fetch_add(1, std::memory_order_relaxed);
            return XR_ERROR_LIMIT_REACHED;
        }
        g_budget_live_privates.store(live + 1, std::memory_order_release);
        if (live == 0) {
            *swapchain = g_current_swapchain;
        } else if (live == 1) {
            *swapchain = g_current_swapchain_b;
        } else {
            g_synthetic_private_creates.fetch_add(1, std::memory_order_relaxed);
            *swapchain = g_synthetic_swapchain;
        }
        return XR_SUCCESS;
    }
    // Single-swapchain rings: the second private swapchain is the synthetic,
    // and there is no third.
    const std::uint32_t role = g_single_rings && call >= 2
        ? (call == 2 ? 3U : 99U)
        : call;
    if (role == 0) {
        *swapchain = g_application_swapchain;
    } else if (role == 1) {
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_current_create_info_valid.store(valid, std::memory_order_release);
        *swapchain = g_current_swapchain;
    } else if (role == 2) {
        // Second current slot: same create info as the first.
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_current_create_info_valid.store(
            g_current_create_info_valid.load(std::memory_order_acquire) && valid,
            std::memory_order_release);
        *swapchain = g_current_swapchain_b;
    } else if (role == 3) {
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_synthetic_create_info_valid.store(valid, std::memory_order_release);
        g_synthetic_private_creates.fetch_add(1, std::memory_order_relaxed);
        *swapchain = g_synthetic_swapchain;
    } else if (role == 4) {
        // Second synthetic slot: same create info as the first.
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_synthetic_create_info_valid.store(
            g_synthetic_create_info_valid.load(std::memory_order_acquire) && valid,
            std::memory_order_release);
        g_synthetic_private_creates.fetch_add(1, std::memory_order_relaxed);
        *swapchain = g_synthetic_swapchain_b;
    } else {
        return XR_ERROR_LIMIT_REACHED;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_space(XrSpace space) {
    if (space != g_valid_composition_space.load(std::memory_order_acquire)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    g_valid_composition_space.store(XR_NULL_HANDLE, std::memory_order_release);
    return XR_SUCCESS;
}

// Delivers one session state transition and then reports the queue empty, so
// the call chain exercises both branches of the layer's pass-through: the
// event it records and the XR_EVENT_UNAVAILABLE it must forward untouched.
std::atomic<std::uint32_t> g_poll_event_calls{0};
constexpr XrSessionState kFakeSessionState = XR_SESSION_STATE_VISIBLE;
constexpr XrTime kFakeSessionStateTime = 4242;

XRAPI_ATTR XrResult XRAPI_CALL fake_poll_event(
    XrInstance,
    XrEventDataBuffer* event_data) {
    if (event_data == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (g_poll_event_calls.fetch_add(1, std::memory_order_relaxed) != 0) {
        return XR_EVENT_UNAVAILABLE;
    }
    auto* state_changed =
        reinterpret_cast<XrEventDataSessionStateChanged*>(event_data);
    *state_changed = XrEventDataSessionStateChanged{
        XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,
        nullptr,
        g_session,
        kFakeSessionState,
        kFakeSessionStateTime};
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_swapchain(XrSwapchain swapchain) {
    g_swapchain_destroyed.store(true, std::memory_order_release);
    g_destroy_swapchain_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_swapchain_budget_mode &&
        (is_current_swapchain(swapchain) || is_synthetic_swapchain(swapchain)) &&
        g_budget_live_privates.load(std::memory_order_acquire) > 0) {
        g_budget_live_privates.fetch_sub(1, std::memory_order_acq_rel);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    if (!is_application_swapchain(swapchain) &&
        swapchain != g_application_depth_swapchain &&
        !is_current_swapchain(swapchain) &&
        !is_synthetic_swapchain(swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    constexpr std::uint32_t kImageCount = 3;
    *image_count_output = kImageCount;
    if (image_capacity_input == 0 || images == nullptr) {
        return XR_SUCCESS;
    }
    if (image_capacity_input < kImageCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    if (g_vulkan_mode) {
        auto* vulkan_images =
            reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        const auto* selected_images = &g_vulkan_application_swapchain_images;
        if (swapchain == g_current_swapchain) {
            selected_images = &g_vulkan_current_swapchain_images;
        } else if (swapchain == g_current_swapchain_b) {
            selected_images = &g_vulkan_current_swapchain_b_images;
        } else if (swapchain == g_synthetic_swapchain) {
            selected_images = &g_vulkan_synthetic_swapchain_images;
        } else if (swapchain == g_synthetic_swapchain_b) {
            selected_images = &g_vulkan_synthetic_swapchain_b_images;
        }
        for (std::uint32_t index = 0; index < kImageCount; ++index) {
            if (vulkan_images[index].type !=
                XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            vulkan_images[index].image = (*selected_images)[index];
        }
        return XR_SUCCESS;
    }

    if (g_d3d11_interop_mode && !g_d3d11_bridge_mode) {
        auto* d3d11_images =
            reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        const auto* selected_images = &g_d3d11_application_swapchain_images;
        if (swapchain == g_current_swapchain) {
            selected_images = &g_d3d11_current_swapchain_images;
        } else if (swapchain == g_current_swapchain_b) {
            selected_images = &g_d3d11_current_swapchain_b_images;
        } else if (swapchain == g_synthetic_swapchain) {
            selected_images = &g_d3d11_synthetic_swapchain_images;
        } else if (swapchain == g_synthetic_swapchain_b) {
            selected_images = &g_d3d11_synthetic_swapchain_b_images;
        }
        for (std::uint32_t index = 0; index < kImageCount; ++index) {
            if (d3d11_images[index].type !=
                XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d11_images[index].texture = (*selected_images)[index].Get();
        }
        return XR_SUCCESS;
    }

    auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (std::uint32_t index = 0; index < kImageCount; ++index) {
        if (d3d12_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        const auto* selected_images = &g_application_swapchain_images;
        if (swapchain == g_application_depth_swapchain) {
            selected_images = &g_application_depth_images;
        }
        if (swapchain == g_current_swapchain) {
            selected_images = &g_current_swapchain_images;
        } else if (swapchain == g_current_swapchain_b) {
            selected_images = &g_current_swapchain_b_images;
        } else if (swapchain == g_synthetic_swapchain) {
            selected_images = &g_synthetic_swapchain_images;
        } else if (swapchain == g_synthetic_swapchain_b) {
            selected_images = &g_synthetic_swapchain_b_images;
        } else if (swapchain == g_application_swapchain_right) {
            selected_images = &g_application_swapchain_right_images;
        } else if (swapchain == g_current_swapchain_right) {
            selected_images = &g_current_swapchain_right_images;
        } else if (swapchain == g_current_swapchain_right_b) {
            selected_images = &g_current_swapchain_right_b_images;
        } else if (swapchain == g_synthetic_swapchain_right) {
            selected_images = &g_synthetic_swapchain_right_images;
        } else if (swapchain == g_synthetic_swapchain_right_b) {
            selected_images = &g_synthetic_swapchain_right_b_images;
        }
        d3d12_images[index].texture = (*selected_images)[index].Get();
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo*,
    std::uint32_t* index) {
    if (is_current_swapchain(swapchain)) {
        const std::uint32_t call =
            g_current_acquire_calls.fetch_add(1, std::memory_order_relaxed);
        *index = call % static_cast<std::uint32_t>(g_current_swapchain_images.size());
        return XR_SUCCESS;
    }
    if (is_synthetic_swapchain(swapchain)) {
        const std::uint32_t call =
            g_synthetic_acquire_calls.fetch_add(1, std::memory_order_relaxed);
        *index = call % static_cast<std::uint32_t>(g_synthetic_swapchain_images.size());
        return XR_SUCCESS;
    }
    if (g_concurrent_acquire_mode.load(std::memory_order_acquire)) {
        const int call = g_concurrent_acquire_count.fetch_add(1, std::memory_order_acq_rel);
        *index = static_cast<std::uint32_t>(call);
        if (call == 0) {
            g_first_concurrent_acquire_entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        return XR_SUCCESS;
    }
    if (g_acquire_ahead_active.load(std::memory_order_acquire)) {
        // A real ring: the application holds two images at once.
        *index = g_acquire_ahead_calls.fetch_add(1, std::memory_order_relaxed) % 3U;
        return XR_SUCCESS;
    }
    *index = 2;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo*) {
    if (is_current_swapchain(swapchain)) {
        g_current_wait_calls.fetch_add(1, std::memory_order_relaxed);
    } else if (is_synthetic_swapchain(swapchain)) {
        g_synthetic_wait_calls.fetch_add(1, std::memory_order_relaxed);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo*) {
    if (is_current_swapchain(swapchain) || is_synthetic_swapchain(swapchain)) {
        const bool synthetic = is_synthetic_swapchain(swapchain);
        (synthetic ? g_synthetic_release_calls : g_current_release_calls)
            .fetch_add(1, std::memory_order_relaxed);
        if (!g_log_path.empty()) {
            std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
            stream << "[XRFG-FAKE] private release role="
                   << (synthetic ? "synthetic" : "current") << '\n';
        }
        if (synthetic &&
            g_fail_next_synthetic_release.exchange(false, std::memory_order_acq_rel)) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        return XR_SUCCESS;
    }
    g_application_release_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream release entered\n";
    }
    if (g_fail_next_release.exchange(false, std::memory_order_acq_rel)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_proc_addr(
    XrInstance,
    const char* name,
    PFN_xrVoidFunction* function);

// The test executable also stands in for the loader: the layer finds the
// loader by the module that exports xrGetInstanceProcAddr and
// xrEnumerateApiLayerProperties without negotiating as a layer or a
// runtime, and takes the global functions from it when a layer above asks
// for them with XR_NULL_HANDLE before any instance exists.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL fake_loader_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    return fake_get_instance_proc_addr(instance, name, function);
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL fake_loader_enumerate_api_layer_properties(
    std::uint32_t,
    std::uint32_t* count,
    XrApiLayerProperties*) {
    *count = 0;
    return XR_SUCCESS;
}

#pragma comment(linker, "/export:xrGetInstanceProcAddr=fake_loader_get_instance_proc_addr")
#pragma comment(linker, "/export:xrEnumerateApiLayerProperties=fake_loader_enumerate_api_layer_properties")

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_proc_addr(
    XrInstance,
    const char* name,
    PFN_xrVoidFunction* function) {
    *function = nullptr;
#define XRFG_FAKE_FUNCTION(openxr_name, implementation)                         \
    if (std::strcmp(name, openxr_name) == 0) {                                  \
        *function = reinterpret_cast<PFN_xrVoidFunction>(implementation);       \
        return XR_SUCCESS;                                                       \
    }
    XRFG_FAKE_FUNCTION("xrDestroyInstance", fake_destroy_instance)
    XRFG_FAKE_FUNCTION("xrGetInstanceProperties", fake_get_instance_properties)
    XRFG_FAKE_FUNCTION("xrCreateSession", fake_create_session)
    XRFG_FAKE_FUNCTION("xrEnumerateInstanceExtensionProperties", fake_enumerate_instance_extension_properties)
    XRFG_FAKE_FUNCTION("xrGetD3D12GraphicsRequirementsKHR", fake_get_d3d12_graphics_requirements)
    XRFG_FAKE_FUNCTION("xrDestroySession", fake_destroy_session)
    XRFG_FAKE_FUNCTION("xrBeginSession", fake_begin_session)
    XRFG_FAKE_FUNCTION("xrEndSession", fake_end_session)
    XRFG_FAKE_FUNCTION("xrWaitFrame", fake_wait_frame)
    XRFG_FAKE_FUNCTION("xrBeginFrame", fake_begin_frame)
    XRFG_FAKE_FUNCTION("xrEndFrame", fake_end_frame)
    XRFG_FAKE_FUNCTION("xrLocateViews", fake_locate_views)
    XRFG_FAKE_FUNCTION("xrCreateSwapchain", fake_create_swapchain)
    XRFG_FAKE_FUNCTION("xrDestroySwapchain", fake_destroy_swapchain)
    XRFG_FAKE_FUNCTION("xrDestroySpace", fake_destroy_space)
    XRFG_FAKE_FUNCTION("xrPollEvent", fake_poll_event)
    XRFG_FAKE_FUNCTION("xrEnumerateSwapchainImages", fake_enumerate_swapchain_images)
    XRFG_FAKE_FUNCTION("xrAcquireSwapchainImage", fake_acquire_swapchain_image)
    XRFG_FAKE_FUNCTION("xrWaitSwapchainImage", fake_wait_swapchain_image)
    XRFG_FAKE_FUNCTION("xrReleaseSwapchainImage", fake_release_swapchain_image)
#undef XRFG_FAKE_FUNCTION
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_create_api_layer_instance(
    const XrInstanceCreateInfo*,
    const XrApiLayerCreateInfo*,
    XrInstance* instance) {
    *instance = g_instance;
    return XR_SUCCESS;
}

template <typename Function>
[[nodiscard]] Function get_layer_function(
    PFN_xrGetInstanceProcAddr get_instance_proc_addr,
    const char* name) {
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = get_instance_proc_addr(g_instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Function>(function);
}

[[nodiscard]] std::size_t count_occurrences(
    const std::string& text,
    const std::string& expected) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(expected, position)) != std::string::npos) {
        ++count;
        position += expected.size();
    }
    return count;
}

[[nodiscard]] bool initialize_d3d12() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp_adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf()))) ||
        FAILED(D3D12CreateDevice(
            warp_adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(g_device.GetAddressOf())))) {
        std::cerr << "failed to create the D3D12 WARP device\n";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(
            &queue_description,
            IID_PPV_ARGS(g_queue.GetAddressOf())))) {
        std::cerr << "failed to create the D3D12 command queue\n";
        return false;
    }

    return create_fake_d3d12_images();
}

// The runtime's images, on whichever device the runtime has: the WARP device
// the fake makes for itself, or, in d3d11-bridge mode, the device the layer
// bound the session with.
[[nodiscard]] bool create_fake_d3d12_images() {
    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = g_double_wide_mode ? 8 : 4;
    texture_description.Height = 4;
    texture_description.DepthOrArraySize =
        (g_split_eye_mode || g_double_wide_mode) ? 1 : 2;
    // d3d11-bridge: mipmapped, as Cyberpunk 2077 asks for, so the bridge's
    // mip_copy path runs end to end - D3D11 will not open a shared texture
    // with more than one mip.
    texture_description.MipLevels = g_d3d11_bridge_mode ? 3 : 1;
    texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (auto* images : {
             &g_application_swapchain_images,
             &g_current_swapchain_images,
             &g_current_swapchain_b_images,
             &g_synthetic_swapchain_images,
             &g_synthetic_swapchain_b_images,
             &g_application_swapchain_right_images,
             &g_current_swapchain_right_images,
             &g_current_swapchain_right_b_images,
             &g_synthetic_swapchain_right_images,
             &g_synthetic_swapchain_right_b_images}) {
        // Only the game's own images are mipmapped: the layer asks for one
        // mip on a bridged session's private swapchains, and a runtime
        // creates what it is asked for.
        texture_description.MipLevels =
            g_d3d11_bridge_mode &&
                    (images == &g_application_swapchain_images ||
                     images == &g_application_swapchain_right_images)
                ? 3
                : 1;
        for (auto& image : *images) {
            image.Reset();
            if (FAILED(g_device->CreateCommittedResource(
                    &heap_properties,
                    D3D12_HEAP_FLAG_NONE,
                    &texture_description,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    nullptr,
                    IID_PPV_ARGS(image.GetAddressOf())))) {
                std::cerr << "failed to create a fake runtime swapchain image\n";
                return false;
            }
        }
    }
    if (g_d3d11_bridge_mode) {
        // The depth swapchain, in the packed format (Ready or Not's).
        D3D12_RESOURCE_DESC depth_description = texture_description;
        depth_description.MipLevels = 1;
        depth_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        for (auto& image : g_application_depth_images) {
            image.Reset();
            if (FAILED(g_device->CreateCommittedResource(
                    &heap_properties,
                    D3D12_HEAP_FLAG_NONE,
                    &depth_description,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    nullptr,
                    IID_PPV_ARGS(image.GetAddressOf())))) {
                std::cerr << "failed to create a fake runtime depth image\n";
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool initialize_d3d11() {
    D3D_FEATURE_LEVEL feature_level{};
    if (FAILED(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                (g_single_threaded_mode ? D3D11_CREATE_DEVICE_SINGLETHREADED : 0U) |
                // XRFG_D3D11_DEBUG=1: the debug layer, whose messages the
                // depth-swapchain check prints when the layer refuses one.
                (std::getenv("XRFG_D3D11_DEBUG") != nullptr ? D3D11_CREATE_DEVICE_DEBUG : 0U),
            nullptr,
            0,
            D3D11_SDK_VERSION,
            g_d3d11_device.GetAddressOf(),
            &feature_level,
            g_d3d11_context.GetAddressOf())) ||
        feature_level < D3D_FEATURE_LEVEL_11_0) {
        std::cerr << "failed to create the D3D11 hardware device\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = g_double_wide_mode ? 8 : 4;
    description.Height = 4;
    description.MipLevels = 3;
    description.ArraySize = g_double_wide_mode ? 1 : 2;
    description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    for (auto& image : g_d3d11_application_swapchain_images) {
        if (FAILED(g_d3d11_device->CreateTexture2D(
                &description,
                nullptr,
                image.GetAddressOf()))) {
            std::cerr << "failed to create a fake D3D11 application image\n";
            return false;
        }
    }

    // Real runtimes may expose typed one-mip private images even when the
    // application swapchain uses a compatible typeless multi-mip resource.
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BindFlags = D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = 0;
    for (auto* images : {
             &g_d3d11_current_swapchain_images,
             &g_d3d11_current_swapchain_b_images,
             &g_d3d11_synthetic_swapchain_images,
             &g_d3d11_synthetic_swapchain_b_images}) {
        for (auto& image : *images) {
            if (FAILED(g_d3d11_device->CreateTexture2D(
                    &description,
                    nullptr,
                    image.GetAddressOf()))) {
                std::cerr << "failed to create a fake D3D11 private image\n";
                return false;
            }
        }
    }
    return true;
}

// The device an application on OpenComposite would create from SteamVR's
// extension list plus the semaphore extension the layer appends, and the
// images a Vulkan runtime hands out: in COLOR_ATTACHMENT_OPTIMAL, the layout
// OpenXR requires of a released colour image and the one the layer's copies
// assume.
// A machine with no Vulkan implementation at all - no loader, no driver, no
// device with a graphics queue - cannot run the vulkan scenario, and that is a
// property of the machine, not of the layer. CTest matches this marker
// (SKIP_REGULAR_EXPRESSION on the vulkan tests) and reports the scenario as
// skipped instead of failed, which is what a GPU-less build runner hits.
// Every other way Vulkan setup can fail stays a failure, in particular
// instance creation refusing the queue layer the harness puts in the chain.
[[nodiscard]] bool skip_without_vulkan(const char* reason) {
    std::cerr << "SKIP: no Vulkan implementation on this machine: " << reason << '\n';
    return false;
}

[[nodiscard]] bool initialize_vulkan() {
    g_vulkan.module = LoadLibraryW(L"vulkan-1.dll");
    if (g_vulkan.module == nullptr) {
        return skip_without_vulkan("vulkan-1.dll is not available");
    }
    g_vulkan.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkan.module, "vkGetInstanceProcAddr"));
    if (g_vulkan.get_instance_proc_addr == nullptr) {
        return false;
    }
    const auto instance_function = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
            g_vulkan.get_instance_proc_addr(g_vulkan.instance, name));
        return function != nullptr;
    };
    PFN_vkCreateInstance create_instance = nullptr;
    if (!instance_function(create_instance, "vkCreateInstance")) {
        return false;
    }
    VkApplicationInfo application_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application_info.pApplicationName = "xrfg_layer_call_chain";
    application_info.apiVersion = VK_API_VERSION_1_2;
    const std::array<const char*, 3> instance_extensions{
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
    };
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledExtensionCount =
        static_cast<std::uint32_t>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    const VkResult instance_result =
        create_instance(&instance_info, nullptr, &g_vulkan.instance);
    if (instance_result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        return skip_without_vulkan("the loader found no Vulkan driver");
    }
    if (instance_result != VK_SUCCESS) {
        std::cerr << "failed to create the Vulkan instance (" << instance_result << ")\n";
        return false;
    }
    // The test harness puts the bridge's queue-serialising layer in the
    // chain (VK_LAYER_PATH + VK_INSTANCE_LAYERS); instance creation must
    // have loaded it, or the run says nothing about it.
    if (std::getenv("OFXR_TEST_EXPECT_QUEUE_LAYER") != nullptr &&
        GetModuleHandleW(L"OFXR_vulkan_queue_layer.dll") == nullptr) {
        std::cerr << "the OFXR Vulkan queue layer was not loaded\n";
        return false;
    }
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    if (!instance_function(enumerate_physical_devices, "vkEnumeratePhysicalDevices") ||
        !instance_function(get_physical_device_properties, "vkGetPhysicalDeviceProperties") ||
        !instance_function(get_queue_family_properties, "vkGetPhysicalDeviceQueueFamilyProperties") ||
        !instance_function(get_memory_properties, "vkGetPhysicalDeviceMemoryProperties") ||
        !instance_function(create_device, "vkCreateDevice") ||
        !instance_function(get_device_proc_addr, "vkGetDeviceProcAddr")) {
        return false;
    }
    std::uint32_t physical_device_count = 0;
    if (enumerate_physical_devices(g_vulkan.instance, &physical_device_count, nullptr) != VK_SUCCESS ||
        physical_device_count == 0) {
        return skip_without_vulkan("no Vulkan physical device");
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (enumerate_physical_devices(
            g_vulkan.instance, &physical_device_count, physical_devices.data()) < VK_SUCCESS) {
        return false;
    }
    // The first discrete GPU with a graphics queue, as the runtime would pick.
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass) {
        for (VkPhysicalDevice candidate : physical_devices) {
            VkPhysicalDeviceProperties properties{};
            get_physical_device_properties(candidate, &properties);
            if (pass == 0 &&
                properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                continue;
            }
            std::uint32_t family_count = 0;
            get_queue_family_properties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            get_queue_family_properties(candidate, &family_count, families.data());
            for (std::uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    g_vulkan.physical_device = candidate;
                    g_vulkan.queue_family = family;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
    }
    if (!found) {
        return skip_without_vulkan("no Vulkan device with a graphics queue");
    }
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = g_vulkan.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const std::array<const char*, 7> device_extensions{
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_win32",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
    };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline_features.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &timeline_features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount =
        static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    if (create_device(g_vulkan.physical_device, &device_info, nullptr, &g_vulkan.device) != VK_SUCCESS) {
        std::cerr << "failed to create the Vulkan device\n";
        return false;
    }
    const auto device_function = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
            get_device_proc_addr(g_vulkan.device, name));
        return function != nullptr;
    };
    PFN_vkGetDeviceQueue get_device_queue = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    auto& allocate_command_buffers = g_vulkan.allocate_command_buffers;
    auto& begin_command_buffer = g_vulkan.begin_command_buffer;
    auto& end_command_buffer = g_vulkan.end_command_buffer;
    auto& cmd_pipeline_barrier = g_vulkan.cmd_pipeline_barrier;
    auto& queue_submit = g_vulkan.queue_submit;
    if (!device_function(g_vulkan.queue_wait_idle, "vkQueueWaitIdle") ||
        !device_function(get_device_queue, "vkGetDeviceQueue") ||
        !device_function(create_image, "vkCreateImage") ||
        !device_function(get_image_memory_requirements, "vkGetImageMemoryRequirements") ||
        !device_function(allocate_memory, "vkAllocateMemory") ||
        !device_function(bind_image_memory, "vkBindImageMemory") ||
        !device_function(create_command_pool, "vkCreateCommandPool") ||
        !device_function(allocate_command_buffers, "vkAllocateCommandBuffers") ||
        !device_function(begin_command_buffer, "vkBeginCommandBuffer") ||
        !device_function(end_command_buffer, "vkEndCommandBuffer") ||
        !device_function(cmd_pipeline_barrier, "vkCmdPipelineBarrier") ||
        !device_function(queue_submit, "vkQueueSubmit") ||
        !device_function(g_vulkan.cmd_clear_color_image, "vkCmdClearColorImage") ||
        !device_function(g_vulkan.cmd_copy_image_to_buffer, "vkCmdCopyImageToBuffer") ||
        !device_function(g_vulkan.reset_command_pool, "vkResetCommandPool") ||
        !device_function(create_buffer, "vkCreateBuffer") ||
        !device_function(get_buffer_memory_requirements, "vkGetBufferMemoryRequirements") ||
        !device_function(bind_buffer_memory, "vkBindBufferMemory") ||
        !device_function(map_memory, "vkMapMemory")) {
        return false;
    }
    get_device_queue(g_vulkan.device, g_vulkan.queue_family, 0, &g_vulkan.queue);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    get_memory_properties(g_vulkan.physical_device, &memory_properties);

    const auto make_image = [&](std::uint32_t mip_levels, VkImage* output) {
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {4, 4, 1};
        image_info.mipLevels = mip_levels;
        image_info.arrayLayers = 2;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        if (create_image(g_vulkan.device, &image_info, nullptr, &image) != VK_SUCCESS) {
            return false;
        }
        g_vulkan.images.push_back(image);
        VkMemoryRequirements requirements{};
        get_image_memory_requirements(g_vulkan.device, image, &requirements);
        std::optional<std::uint32_t> type;
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
            if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
                (memory_properties.memoryTypes[index].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                type = index;
                break;
            }
        }
        if (!type) {
            return false;
        }
        VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = *type;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (allocate_memory(g_vulkan.device, &allocate_info, nullptr, &memory) != VK_SUCCESS) {
            return false;
        }
        g_vulkan.memories.push_back(memory);
        if (bind_image_memory(g_vulkan.device, image, memory, 0) != VK_SUCCESS) {
            return false;
        }
        *output = image;
        return true;
    };
    for (VkImage& image : g_vulkan_application_swapchain_images) {
        if (!make_image(3, &image)) {
            std::cerr << "failed to create a fake Vulkan application image\n";
            return false;
        }
    }
    // One-mip private images, as a real runtime may hand out.
    for (auto* images : {
             &g_vulkan_current_swapchain_images,
             &g_vulkan_current_swapchain_b_images,
             &g_vulkan_synthetic_swapchain_images,
             &g_vulkan_synthetic_swapchain_b_images}) {
        for (VkImage& image : *images) {
            if (!make_image(1, &image)) {
                std::cerr << "failed to create a fake Vulkan private image\n";
                return false;
            }
        }
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = g_vulkan.queue_family;
    if (create_command_pool(g_vulkan.device, &pool_info, nullptr, &g_vulkan.command_pool) != VK_SUCCESS) {
        return false;
    }
    {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = 4 * 16;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (create_buffer(g_vulkan.device, &buffer_info, nullptr, &g_vulkan.readback) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        get_buffer_memory_requirements(g_vulkan.device, g_vulkan.readback, &requirements);
        std::optional<std::uint32_t> type;
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
            const VkMemoryPropertyFlags wanted =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
                (memory_properties.memoryTypes[index].propertyFlags & wanted) == wanted) {
                type = index;
                break;
            }
        }
        VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = type.value_or(0);
        if (!type ||
            allocate_memory(g_vulkan.device, &allocate_info, nullptr, &g_vulkan.readback_memory) != VK_SUCCESS ||
            bind_buffer_memory(g_vulkan.device, g_vulkan.readback, g_vulkan.readback_memory, 0) != VK_SUCCESS ||
            map_memory(g_vulkan.device, g_vulkan.readback_memory, 0, VK_WHOLE_SIZE, 0, &g_vulkan.readback_mapped) != VK_SUCCESS) {
            std::cerr << "failed to create the Vulkan readback buffer\n";
            return false;
        }
    }
    VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffer_info.commandPool = g_vulkan.command_pool;
    buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer_info.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (allocate_command_buffers(g_vulkan.device, &buffer_info, &buffer) != VK_SUCCESS ||
        begin_command_buffer(buffer, &begin_info) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkImageMemoryBarrier> barriers;
    for (VkImage image : g_vulkan.images) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
            VK_REMAINING_ARRAY_LAYERS};
        barriers.push_back(barrier);
    }
    cmd_pipeline_barrier(
        buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, static_cast<std::uint32_t>(barriers.size()),
        barriers.data());
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer;
    if (end_command_buffer(buffer) != VK_SUCCESS ||
        queue_submit(g_vulkan.queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS ||
        g_vulkan.queue_wait_idle(g_vulkan.queue) != VK_SUCCESS) {
        std::cerr << "failed to put the fake Vulkan images in COLOR_ATTACHMENT_OPTIMAL\n";
        return false;
    }
    return true;
}

// Records and runs one command buffer on the application's queue, then
// waits for it. Test-side only; the layer never does this on a frame.
template <typename Record>
[[nodiscard]] bool run_vulkan_commands(Record&& record) {
    if (g_vulkan.queue_wait_idle(g_vulkan.queue) != VK_SUCCESS ||
        g_vulkan.reset_command_pool(g_vulkan.device, g_vulkan.command_pool, 0) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffer_info.commandPool = g_vulkan.command_pool;
    buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer_info.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_vulkan.allocate_command_buffers(g_vulkan.device, &buffer_info, &buffer) != VK_SUCCESS ||
        g_vulkan.begin_command_buffer(buffer, &begin_info) != VK_SUCCESS) {
        return false;
    }
    record(buffer);
    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer;
    return g_vulkan.end_command_buffer(buffer) == VK_SUCCESS &&
        g_vulkan.queue_submit(g_vulkan.queue, 1, &submit_info, VK_NULL_HANDLE) == VK_SUCCESS &&
        g_vulkan.queue_wait_idle(g_vulkan.queue) == VK_SUCCESS;
}

[[nodiscard]] VkImageMemoryBarrier vulkan_test_barrier(
    VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    return barrier;
}

// The application "renders" a frame: every pixel of the image one colour,
// and the image back in the layout the runtime expects at release.
[[nodiscard]] bool paint_vulkan_image(VkImage image, std::uint8_t red) {
    return run_vulkan_commands([&](VkCommandBuffer buffer) {
        const VkImageMemoryBarrier to_transfer = vulkan_test_barrier(
            image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        g_vulkan.cmd_pipeline_barrier(
            buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &to_transfer);
        VkClearColorValue color{};
        color.float32[0] = static_cast<float>(red) / 255.0F;
        color.float32[1] = 0.25F;
        color.float32[2] = 0.5F;
        color.float32[3] = 1.0F;
        const VkImageSubresourceRange range{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        g_vulkan.cmd_clear_color_image(
            buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        const VkImageMemoryBarrier back = vulkan_test_barrier(
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        g_vulkan.cmd_pipeline_barrier(
            buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &back);
    });
}

// The red channel of pixel (0,0), layer 0, of a private image, read the
// way the runtime would read it: from COLOR_ATTACHMENT_OPTIMAL and back.
[[nodiscard]] std::optional<std::uint8_t> read_vulkan_pixel(VkImage image) {
    const bool ran = run_vulkan_commands([&](VkCommandBuffer buffer) {
        const VkImageMemoryBarrier to_transfer = vulkan_test_barrier(
            image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        g_vulkan.cmd_pipeline_barrier(
            buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &to_transfer);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {1, 1, 1};
        g_vulkan.cmd_copy_image_to_buffer(
            buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vulkan.readback, 1, &region);
        const VkImageMemoryBarrier back = vulkan_test_barrier(
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        g_vulkan.cmd_pipeline_barrier(
            buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &back);
    });
    if (!ran) {
        return std::nullopt;
    }
    return static_cast<const std::uint8_t*>(g_vulkan.readback_mapped)[0];
}

// Whether any current private image holds a frame painted with this red
// value: the pair's "current" output is a copy of the frame just submitted,
// and it reached the runtime through both interop copies and the D3D12
// history in between.
[[nodiscard]] bool vulkan_current_images_contain(std::uint8_t red) {
    for (const auto* images : {&g_vulkan_current_swapchain_images,
                               &g_vulkan_current_swapchain_b_images}) {
        for (VkImage image : *images) {
            const auto pixel = read_vulkan_pixel(image);
            if (pixel && *pixel == red) {
                return true;
            }
        }
    }
    return false;
}

// d3d11-bridge: paint mip 0, slice 0 of a D3D11 texture the layer handed the
// application, on the application's own context.
[[nodiscard]] bool paint_d3d11_image(ID3D11Texture2D* texture, std::uint8_t red) {
    if (texture == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(description.Width) * description.Height * 4, 0);
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
        pixels[index] = red;
        pixels[index + 3] = 255;
    }
    g_d3d11_context->UpdateSubresource(
        texture, 0, nullptr, pixels.data(), description.Width * 4, 0);
    return true;
}

// d3d11-bridge: the first byte of mip 0, slice 0 of one of the runtime's
// D3D12 images, read back on the queue the layer bound the session with.
// The runtime holds its images in RENDER_TARGET between frames.
[[nodiscard]] bool d3d12_image_first_red(ID3D12Resource* image, std::uint8_t* red) {
    if (image == nullptr || !g_device || !g_queue) {
        return false;
    }
    const D3D12_RESOURCE_DESC description = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(g_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(readback.GetAddressOf()))) ||
        FAILED(g_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(g_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(list.GetAddressOf()))) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())))) {
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = image;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
    if (FAILED(list->Close())) {
        return false;
    }
    ID3D12CommandList* const lists[] = {list.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    if (FAILED(g_queue->Signal(fence.Get(), 1))) {
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }
    bool completed = false;
    if (SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
        completed = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    }
    CloseHandle(event);
    if (!completed) {
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, &mapped))) {
        return false;
    }
    *red = static_cast<const std::uint8_t*>(mapped)[0];
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return true;
}

[[nodiscard]] bool wait_for_queue_idle() {
    if (g_vulkan_mode) {
        return g_vulkan.queue_wait_idle(g_vulkan.queue) == VK_SUCCESS;
    }
    if (g_d3d11_interop_mode) {
        ComPtr<ID3D11Device5> device5;
        ComPtr<ID3D11DeviceContext4> context4;
        ComPtr<ID3D11Fence> fence;
        if (FAILED(g_d3d11_device.As(&device5)) ||
            FAILED(g_d3d11_context.As(&context4)) ||
            FAILED(device5->CreateFence(
                0,
                D3D11_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence.GetAddressOf()))) ||
            FAILED(context4->Signal(fence.Get(), 1))) {
            return false;
        }
        context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return false;
        }
        const HRESULT event_result = fence->SetEventOnCompletion(1, event);
        const DWORD wait_result = SUCCEEDED(event_result)
            ? WaitForSingleObject(event, 5'000)
            : WAIT_FAILED;
        CloseHandle(event);
        return wait_result == WAIT_OBJECT_0 &&
               fence->GetCompletedValue() >= 1;
    }
    ComPtr<ID3D12Fence> fence;
    if (FAILED(g_device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()))) ||
        FAILED(g_queue->Signal(fence.Get(), 1))) {
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }
    const HRESULT event_result = fence->SetEventOnCompletion(1, event);
    const DWORD wait_result =
        SUCCEEDED(event_result) ? WaitForSingleObject(event, 5'000) : WAIT_FAILED;
    CloseHandle(event);
    return wait_result == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr <<
            "usage: xrfg_layer_call_chain <layer-dll> <log-path> "
            "[split-eye|cropped-split-eye|double-wide|d3d11-interop|"
            "d3d11-double-wide|steamvr-inline|steamvr-presenter|"
            "flight-simulator|uevr-pipelined-time|inverted-fov|"
            "d3d11-inverted-fov|d3d11-single-threaded|vulkan|swapchain-budget|"
            "dcs|dcs-d3d11|d3d11-bridge|d3d11-bridge-acquire-ahead|"
            "steamvr-own-time|promise-shown-time]\n";
        return EXIT_FAILURE;
    }
    g_dcs_d3d11_mode = argc == 4 && std::strcmp(argv[3], "dcs-d3d11") == 0;
    g_acquire_ahead_mode =
        argc == 4 && std::strcmp(argv[3], "d3d11-bridge-acquire-ahead") == 0;
    g_d3d11_bridge_mode = g_acquire_ahead_mode ||
        (argc == 4 && std::strcmp(argv[3], "d3d11-bridge") == 0);
    g_dcs_mode = g_dcs_d3d11_mode ||
        (argc == 4 && std::strcmp(argv[3], "dcs") == 0);
    g_cropped_subimage_mode =
        argc == 4 && std::strcmp(argv[3], "cropped-split-eye") == 0;
    const bool d3d11_double_wide_mode =
        argc == 4 && std::strcmp(argv[3], "d3d11-double-wide") == 0;
    g_destroy_pending_space =
        argc == 4 && std::strcmp(argv[3], "steamvr-destroy-space") == 0;
    g_refuse_layer_mode =
        argc == 4 && std::strcmp(argv[3], "steamvr-layer-invalid") == 0;
    g_own_display_time_mode =
        argc == 4 && std::strcmp(argv[3], "steamvr-own-time") == 0;
    g_promise_mode =
        argc == 4 && std::strcmp(argv[3], "promise-shown-time") == 0;
    g_steamvr_presenter_mode = g_destroy_pending_space || g_dcs_mode ||
        g_refuse_layer_mode || g_own_display_time_mode || g_promise_mode ||
        (argc == 4 && std::strcmp(argv[3], "steamvr-presenter") == 0);
    g_single_threaded_mode =
        argc == 4 && std::strcmp(argv[3], "d3d11-single-threaded") == 0;
    g_vulkan_mode = argc == 4 && std::strcmp(argv[3], "vulkan") == 0;
    g_vulkan_bridge_mode = argc == 4 && std::strcmp(argv[3], "vulkan-bridge") == 0;
    g_vulkan_application = g_vulkan_mode || g_vulkan_bridge_mode;
    g_swapchain_budget_mode =
        argc == 4 && std::strcmp(argv[3], "swapchain-budget") == 0;
    g_steamvr_runtime_mode = g_steamvr_presenter_mode ||
        g_single_threaded_mode ||
        (argc == 4 && std::strcmp(argv[3], "steamvr-inline") == 0);
    g_destroy_pending_swapchain =
        argc == 4 && std::strcmp(argv[3], "flight-destroy-swapchain") == 0;
    g_flight_simulator_mode = g_destroy_pending_swapchain ||
        (argc == 4 && std::strcmp(argv[3], "flight-simulator") == 0);
    g_test_application_thread_id = GetCurrentThreadId();
    g_inverted_vertical_fov = argc == 4 &&
        (std::strcmp(argv[3], "inverted-fov") == 0 ||
         std::strcmp(argv[3], "d3d11-inverted-fov") == 0);
    g_d3d11_interop_mode = argc == 4 &&
        (std::strcmp(argv[3], "d3d11-interop") == 0 ||
         std::strcmp(argv[3], "d3d11-inverted-fov") == 0 ||
         d3d11_double_wide_mode || g_single_threaded_mode ||
         g_dcs_d3d11_mode || g_d3d11_bridge_mode);
    g_uevr_pipelined_display_time_mode =
        argc == 4 && std::strcmp(argv[3], "uevr-pipelined-time") == 0;
    g_double_wide_mode = argc == 4 &&
        (std::strcmp(argv[3], "double-wide") == 0 ||
         d3d11_double_wide_mode || g_uevr_pipelined_display_time_mode);
    g_split_eye_mode = argc == 4 &&
        (std::strcmp(argv[3], "split-eye") == 0 || g_cropped_subimage_mode);
    {
        std::filesystem::path ini = std::filesystem::path(argv[1]).parent_path() /
            L"ofxr_bridge.ini";
        // Same default as the layer: on unless the ini says 0.
        g_deep_pipeline = GetPrivateProfileIntW(
            L"ofxr", L"deep_pipeline", 1, ini.wstring().c_str()) != 0;
        g_single_rings = GetPrivateProfileIntW(
                             L"ofxr", L"single_swapchain_rings", 1,
                             ini.wstring().c_str()) != 0 &&
            // The bridge makes a D3D12 session, which takes the single
            // rings; the legacy interop and Vulkan mirror the rings.
            !(g_d3d11_interop_mode && !g_d3d11_bridge_mode) && !g_vulkan_mode;
        if (GetPrivateProfileIntW(
                L"ofxr", L"triple_frame_gen", 0, ini.wstring().c_str()) != 0) {
            // What the flag stands for in this file is the synthetic ring's
            // two slots, and 3X takes two as well: one per synthetic.
            g_frames_per_application_frame = 3;
            g_deep_pipeline = true;
        }
    }
    if (argc == 4 && !g_split_eye_mode && !g_double_wide_mode &&
        !g_d3d11_interop_mode && !g_steamvr_runtime_mode &&
        !g_flight_simulator_mode && !g_uevr_pipelined_display_time_mode &&
        !g_inverted_vertical_fov && !g_vulkan_mode && !g_vulkan_bridge_mode &&
        !g_swapchain_budget_mode) {
        std::cerr << "unknown test mode\n";
        return EXIT_FAILURE;
    }

    const HANDLE execution_mutex =
        CreateMutexA(nullptr, FALSE, "Local\\XRFGLayerCallChainFakeRuntimeTest");
    const DWORD mutex_wait = execution_mutex == nullptr
                                 ? WAIT_FAILED
                                 : WaitForSingleObject(execution_mutex, 30'000);
    if (execution_mutex == nullptr ||
        (mutex_wait != WAIT_OBJECT_0 && mutex_wait != WAIT_ABANDONED_0)) {
        if (execution_mutex != nullptr) {
            CloseHandle(execution_mutex);
        }
        std::cerr << "failed to serialize the fake-runtime test\n";
        return EXIT_FAILURE;
    }
    struct ExecutionMutexGuard {
        HANDLE handle{};
        ~ExecutionMutexGuard() {
            if (handle != nullptr) {
                ReleaseMutex(handle);
                CloseHandle(handle);
            }
        }
    } execution_mutex_guard{execution_mutex};
    {
        std::ofstream reset_log(argv[2], std::ios::out | std::ios::trunc);
        if (!reset_log) {
            std::cerr << "failed to truncate the fake-runtime log\n";
            return EXIT_FAILURE;
        }
    }
    g_log_path = argv[2];
    if (g_vulkan_application ? !initialize_vulkan()
        : g_d3d11_interop_mode ? !initialize_d3d11()
                               : !initialize_d3d12()) {
        return EXIT_FAILURE;
    }

    const HMODULE module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    if (negotiate == nullptr) {
        return EXIT_FAILURE;
    }

    XrNegotiateLoaderInfo loader_info{};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(loader_info);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_CURRENT_API_VERSION;

    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    if (XR_FAILED(negotiate(&loader_info, kLayerName, &request))) {
        return EXIT_FAILURE;
    }

    XrApiLayerNextInfo next_info{};
    next_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    next_info.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    next_info.structSize = sizeof(next_info);
    strcpy_s(next_info.layerName, kLayerName);
    next_info.nextGetInstanceProcAddr = fake_get_instance_proc_addr;
    next_info.nextCreateApiLayerInstance = fake_create_api_layer_instance;

    XrApiLayerCreateInfo layer_info{};
    layer_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    layer_info.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    layer_info.structSize = sizeof(layer_info);
    layer_info.nextInfo = &next_info;

    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(instance_info.applicationInfo.applicationName, "XRFG fake runtime test");
    strcpy_s(instance_info.applicationInfo.engineName, "XRFG tests");
    instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    // A D3D11 application enables the D3D11 extension, and the D3D11 bridge
    // keys on that to add the D3D12 one for the runtime.
    const char* const d3d11_extensions[] = {"XR_KHR_D3D11_enable"};
    if (g_d3d11_interop_mode) {
        instance_info.enabledExtensionNames = d3d11_extensions;
        instance_info.enabledExtensionCount = 1;
    }
    // A Vulkan application enables the Vulkan extension, which the Vulkan
    // bridge keys on the same way. Through OpenComposite it enables the
    // D3D11 one as well (No Man's Sky), which must not keep the Vulkan
    // bridge off; the bridge mode reproduces that.
    const char* const vulkan_extensions[] = {"XR_KHR_vulkan_enable2", "XR_KHR_D3D11_enable"};
    if (g_vulkan_application) {
        instance_info.enabledExtensionNames = vulkan_extensions;
        instance_info.enabledExtensionCount = g_vulkan_bridge_mode ? 2U : 1U;
    }

    // A layer above this one (Cheeky Foveated DLSS) probes the extension
    // list through this layer's xrGetInstanceProcAddr with XR_NULL_HANDLE
    // before creating the instance, and enables eye tracking only if the
    // answer lists what it needs. The answer has to be the runtime's list.
    {
        PFN_xrVoidFunction probe = nullptr;
        if (XR_FAILED(request.getInstanceProcAddr(
                XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &probe)) ||
            probe == nullptr) {
            std::cerr << "null-instance xrEnumerateInstanceExtensionProperties not resolved\n";
            return EXIT_FAILURE;
        }
        std::uint32_t count = 0;
        const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(probe);
        if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)) ||
            count != ((g_d3d11_bridge_mode || g_vulkan_bridge_mode) ? 2U : 1U)) {
            std::cerr << "null-instance extension enumeration did not reach the runtime\n";
            return EXIT_FAILURE;
        }
        PFN_xrVoidFunction unknown = nullptr;
        if (request.getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateSession", &unknown) !=
                XR_ERROR_FUNCTION_UNSUPPORTED || unknown != nullptr) {
            std::cerr << "null-instance query for an instance function must stay unsupported\n";
            return EXIT_FAILURE;
        }
    }

    XrInstance instance = XR_NULL_HANDLE;
    if (XR_FAILED(request.createApiLayerInstance(&instance_info, &layer_info, &instance)) ||
        instance != g_instance) {
        return EXIT_FAILURE;
    }

    const auto create_session = get_layer_function<PFN_xrCreateSession>(request.getInstanceProcAddr, "xrCreateSession");
    const auto destroy_session = get_layer_function<PFN_xrDestroySession>(request.getInstanceProcAddr, "xrDestroySession");
    const auto begin_session = get_layer_function<PFN_xrBeginSession>(request.getInstanceProcAddr, "xrBeginSession");
    const auto end_session = get_layer_function<PFN_xrEndSession>(request.getInstanceProcAddr, "xrEndSession");
    const auto wait_frame = get_layer_function<PFN_xrWaitFrame>(request.getInstanceProcAddr, "xrWaitFrame");
    const auto begin_frame = get_layer_function<PFN_xrBeginFrame>(request.getInstanceProcAddr, "xrBeginFrame");
    const auto end_frame = get_layer_function<PFN_xrEndFrame>(request.getInstanceProcAddr, "xrEndFrame");
    const auto locate_views = get_layer_function<PFN_xrLocateViews>(request.getInstanceProcAddr, "xrLocateViews");
    const auto create_swapchain = get_layer_function<PFN_xrCreateSwapchain>(request.getInstanceProcAddr, "xrCreateSwapchain");
    const auto destroy_swapchain = get_layer_function<PFN_xrDestroySwapchain>(request.getInstanceProcAddr, "xrDestroySwapchain");
    const auto destroy_space = get_layer_function<PFN_xrDestroySpace>(request.getInstanceProcAddr, "xrDestroySpace");
    const auto poll_event = get_layer_function<PFN_xrPollEvent>(request.getInstanceProcAddr, "xrPollEvent");
    const auto enumerate_images = get_layer_function<PFN_xrEnumerateSwapchainImages>(request.getInstanceProcAddr, "xrEnumerateSwapchainImages");
    const auto acquire_image = get_layer_function<PFN_xrAcquireSwapchainImage>(request.getInstanceProcAddr, "xrAcquireSwapchainImage");
    const auto wait_image = get_layer_function<PFN_xrWaitSwapchainImage>(request.getInstanceProcAddr, "xrWaitSwapchainImage");
    const auto release_image = get_layer_function<PFN_xrReleaseSwapchainImage>(request.getInstanceProcAddr, "xrReleaseSwapchainImage");
    const auto destroy_instance = get_layer_function<PFN_xrDestroyInstance>(request.getInstanceProcAddr, "xrDestroyInstance");

    if (!create_session || !destroy_session || !begin_session || !end_session || !wait_frame ||
        !begin_frame || !end_frame || !locate_views || !create_swapchain || !destroy_swapchain ||
        !enumerate_images || !acquire_image || !wait_image || !release_image || !destroy_instance || !poll_event) {
        return EXIT_FAILURE;
    }

    // The layer forwards events untouched. Both branches: the session state
    // change it records, and the empty-queue answer it must pass through
    // without inventing one.
    XrEventDataBuffer polled{XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult first_poll = poll_event(instance, &polled);
    const auto* polled_state =
        reinterpret_cast<const XrEventDataSessionStateChanged*>(&polled);
    XrEventDataBuffer drained{XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult second_poll = poll_event(instance, &drained);
    if (first_poll != XR_SUCCESS ||
        polled.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED ||
        polled_state->session != g_session ||
        polled_state->state != kFakeSessionState ||
        polled_state->time != kFakeSessionStateTime ||
        second_poll != XR_EVENT_UNAVAILABLE ||
        g_poll_event_calls.load(std::memory_order_relaxed) != 2) {
        return EXIT_FAILURE;
    }

    XrGraphicsBindingD3D12KHR graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    graphics_binding.device = g_device.Get();
    graphics_binding.queue = g_queue.Get();
    XrGraphicsBindingD3D11KHR d3d11_graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11_graphics_binding.device = g_d3d11_device.Get();
    XrGraphicsBindingVulkanKHR vulkan_graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vulkan_graphics_binding.instance = g_vulkan.instance;
    vulkan_graphics_binding.physicalDevice = g_vulkan.physical_device;
    vulkan_graphics_binding.device = g_vulkan.device;
    vulkan_graphics_binding.queueFamilyIndex = g_vulkan.queue_family;
    vulkan_graphics_binding.queueIndex = 0;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = g_vulkan_application
        ? static_cast<const void*>(&vulkan_graphics_binding)
        : g_d3d11_interop_mode
            ? static_cast<const void*>(&d3d11_graphics_binding)
            : static_cast<const void*>(&graphics_binding);
    session_info.systemId = 1;
    XrSession session = XR_NULL_HANDLE;
    XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    session_begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (XR_FAILED(create_session(instance, &session_info, &session)) ||
        XR_FAILED(begin_session(session, &session_begin_info))) {
        return EXIT_FAILURE;
    }

    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // A Vulkan application through OpenComposite copies its own texture into
    // the swapchain image and asks for nothing but TRANSFER_DST.
    swapchain_info.usageFlags = g_vulkan_application
        ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
        : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    // A Vulkan session's swapchain format is a VkFormat.
    swapchain_info.format = g_vulkan_application
        ? static_cast<std::int64_t>(VK_FORMAT_R8G8B8A8_UNORM)
        : static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    swapchain_info.sampleCount = 1;
    swapchain_info.width = g_double_wide_mode ? 8 : 4;
    swapchain_info.height = 4;
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize =
        (g_split_eye_mode || g_double_wide_mode) ? 1 : 2;
    swapchain_info.mipCount = (g_d3d11_interop_mode || g_vulkan_application) ? 3 : 1;

    if (g_split_eye_mode) {
        XrSwapchain left_swapchain = XR_NULL_HANDLE;
        XrSwapchain right_swapchain = XR_NULL_HANDLE;
        std::array<XrSwapchainImageD3D12KHR, 3> enumerated_images{};
        for (auto& image : enumerated_images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        }
        auto enumerate_application_images = [&](XrSwapchain handle) {
            std::uint32_t count = 0;
            return XR_SUCCEEDED(enumerate_images(
                       handle,
                       static_cast<std::uint32_t>(enumerated_images.size()),
                       &count,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(
                           enumerated_images.data()))) &&
                   count == enumerated_images.size();
        };
        if (XR_FAILED(create_swapchain(
                session,
                &swapchain_info,
                &left_swapchain)) ||
            left_swapchain != g_application_swapchain ||
            !enumerate_application_images(left_swapchain) ||
            XR_FAILED(create_swapchain(
                session,
                &swapchain_info,
                &right_swapchain)) ||
            right_swapchain != g_application_swapchain_right ||
            !enumerate_application_images(right_swapchain)) {
            return EXIT_FAILURE;
        }

        XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo image_wait_info{
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        image_wait_info.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo release_info{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        auto capture_application_image = [&](XrSwapchain handle) {
            std::uint32_t index = 0;
            return XR_SUCCEEDED(acquire_image(handle, &acquire_info, &index)) &&
                   index == 2 &&
                   XR_SUCCEEDED(wait_image(handle, &image_wait_info)) &&
                   XR_SUCCEEDED(release_image(handle, &release_info));
        };

        std::array<XrCompositionLayerProjectionView, 2> projection_views{};
        for (std::uint32_t index = 0; index < projection_views.size(); ++index) {
            auto& view = projection_views[index];
            view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            view.pose.orientation.w = 1.0F;
            view.subImage.swapchain =
                index == 0 ? left_swapchain : right_swapchain;
            view.subImage.imageRect.offset =
                g_cropped_subimage_mode ? XrOffset2Di{0, 1}
                                        : XrOffset2Di{0, 0};
            view.subImage.imageRect.extent =
                g_cropped_subimage_mode ? XrExtent2Di{4, 2}
                                        : XrExtent2Di{4, 4};
            view.subImage.imageArrayIndex = 0;
        }
        std::array<XrCompositionLayerProjection, 2> eye_projections{{
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION},
        }};
        for (std::size_t index = 0; index < eye_projections.size(); ++index) {
            eye_projections[index].space = g_space;
            eye_projections[index].viewCount = 1;
            eye_projections[index].views = &projection_views[index];
        }
        // XRFG_TEST_SPLIT_EYE_ONE_LAYER: one projection layer holding both
        // views, each on its own swapchain, as games with a swapchain per eye
        // submit them (layer_projection_eyes.cmake).
        const bool one_layer = std::getenv("XRFG_TEST_SPLIT_EYE_ONE_LAYER") != nullptr;
        if (one_layer) {
            eye_projections[0].viewCount = 2;
            eye_projections[0].views = projection_views.data();
        }

        XrCompositionLayerQuad passthrough_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        passthrough_quad.space = g_space;
        passthrough_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        passthrough_quad.pose.orientation.w = 1.0F;
        passthrough_quad.size = {1.0F, 1.0F};
        passthrough_quad.subImage.swapchain = left_swapchain;
        passthrough_quad.subImage.imageRect.extent = {4, 4};
        const auto* passthrough_base =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &passthrough_quad);
        g_expected_passthrough_layer = passthrough_base;
        const XrCompositionLayerBaseHeader* all_layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &eye_projections[0]),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &eye_projections[1]),
            passthrough_base,
        };
        const XrCompositionLayerBaseHeader* one_layer_layers[] = {
            all_layers[0],
            passthrough_base,
        };
        const XrCompositionLayerBaseHeader* const* layers =
            one_layer ? one_layer_layers : all_layers;
        const std::uint32_t layer_count = one_layer
            ? static_cast<std::uint32_t>(std::size(one_layer_layers))
            : static_cast<std::uint32_t>(std::size(all_layers));

        auto submit_frame = [&](XrTime display_time) {
            XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
            locate_info.viewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate_info.displayTime = display_time;
            locate_info.space = g_space;
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            std::uint32_t view_count = 0;
            if (XR_FAILED(locate_views(
                    session,
                    &locate_info,
                    &view_state,
                    static_cast<std::uint32_t>(views.size()),
                    &view_count,
                    views.data())) ||
                view_count != views.size()) {
                return false;
            }
            for (std::uint32_t index = 0; index < projection_views.size(); ++index) {
                const XrView submitted =
                    fake_submitted_view_for_time(display_time, index);
                projection_views[index].pose = submitted.pose;
                projection_views[index].fov = submitted.fov;
            }
            XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
            frame_end_info.displayTime = display_time;
            frame_end_info.environmentBlendMode =
                XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frame_end_info.layerCount = layer_count;
            frame_end_info.layers = layers;
            return XR_SUCCEEDED(end_frame(session, &frame_end_info));
        };

        XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        XrFrameState frame_a{XR_TYPE_FRAME_STATE};
        XrFrameState frame_b{XR_TYPE_FRAME_STATE};
        XrFrameState frame_c{XR_TYPE_FRAME_STATE};
        const bool frame_sequence_succeeded =
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame_a)) &&
            frame_a.predictedDisplayTime == 100 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_application_image(left_swapchain) &&
            capture_application_image(right_swapchain) &&
            submit_frame(frame_a.predictedDisplayTime) &&
            wait_for_queue_idle() &&
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame_b)) &&
            frame_b.predictedDisplayTime == 200 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_application_image(left_swapchain) &&
            capture_application_image(right_swapchain) &&
            submit_frame(frame_b.predictedDisplayTime) &&
            wait_for_queue_idle() &&
            // The first frame arms generation and passes through, so reaching
            // a generated pair takes one application frame longer than it did
            // when the private swapchains were taken at enumeration.
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame_c)) &&
            frame_c.predictedDisplayTime == 300 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_application_image(left_swapchain) &&
            capture_application_image(right_swapchain) &&
            submit_frame(frame_c.predictedDisplayTime) &&
            wait_for_queue_idle();

        g_expected_passthrough_layer = nullptr;
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(left_swapchain)) &&
            XR_SUCCEEDED(destroy_swapchain(right_swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        const auto nearly_equal = [](float actual, float expected) {
            return std::fabs(actual - expected) <= 1.0e-5F;
        };
        const auto camera_matches = [&](const EndFrameRecord& record,
                                        XrTime metadata_time) {
            for (std::uint32_t index = 0; index < record.poses.size(); ++index) {
                const XrView expected =
                    fake_submitted_view_for_time(metadata_time, index);
                const XrPosef& pose = record.poses[index];
                const XrFovf& fov = record.fovs[index];
                if (!nearly_equal(pose.orientation.y, expected.pose.orientation.y) ||
                    !nearly_equal(pose.orientation.w, expected.pose.orientation.w) ||
                    !nearly_equal(pose.position.x, expected.pose.position.x) ||
                    !nearly_equal(pose.position.y, expected.pose.position.y) ||
                    !nearly_equal(pose.position.z, expected.pose.position.z) ||
                    !nearly_equal(fov.angleLeft, expected.fov.angleLeft) ||
                    !nearly_equal(fov.angleRight, expected.fov.angleRight) ||
                    !nearly_equal(fov.angleUp, expected.fov.angleUp) ||
                    !nearly_equal(fov.angleDown, expected.fov.angleDown)) {
                    return false;
                }
            }
            return true;
        };
        const auto record_matches = [&](std::size_t index,
                                         XrTime time,
                                         SubmittedTarget target,
                                         XrTime metadata_time,
                                         std::uint32_t layer_count,
                                         bool passthrough_layer_preserved,
                                         std::uint32_t passthrough_layer_index) {
            const XrRect2Di expected_rect = g_cropped_subimage_mode
                ? XrRect2Di{{0, 1}, {4, 2}}
                : XrRect2Di{{0, 0}, {4, 4}};
            const bool rects_match = index < end_records.size() &&
                std::all_of(
                    end_records[index].image_rects.begin(),
                    end_records[index].image_rects.end(),
                    [&](const XrRect2Di& rect) {
                        return rect.offset.x == expected_rect.offset.x &&
                               rect.offset.y == expected_rect.offset.y &&
                               rect.extent.width == expected_rect.extent.width &&
                               rect.extent.height == expected_rect.extent.height;
                    });
            return index < end_records.size() && rects_match &&
                   end_records[index].display_time == time &&
                   end_records[index].target == target &&
                   end_records[index].layer_count == layer_count &&
                   end_records[index].space == g_space &&
                   end_records[index].passthrough_layer_preserved ==
                       passthrough_layer_preserved &&
                   end_records[index].passthrough_layer_index ==
                       passthrough_layer_index &&
                   camera_matches(end_records[index], metadata_time);
        };
        const bool valid =
            frame_sequence_succeeded && teardown_succeeded &&
            end_records.size() == 4 &&
            // The first frame arms and passes through, the second primes, and
            // the pair is the third.
            record_matches(0, 100, SubmittedTarget::original, 100, layer_count, true, layer_count - 1) &&
            record_matches(1, 200, SubmittedTarget::current, 200, layer_count, true, layer_count - 1) &&
            record_matches(2, 300, SubmittedTarget::synthetic, 300, layer_count, true, layer_count - 1) &&
            record_matches(3, 400, SubmittedTarget::current, 300, layer_count, true, layer_count - 1) &&
            g_wait_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_begin_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_end_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_locate_views_calls.load(std::memory_order_relaxed) == 3 &&
            // Two application swapchains, each backed by two current slots
            // and however many synthetic slots this depth needs - or one of
            // each with single-swapchain rings.
            g_synthetic_private_creates.load(std::memory_order_relaxed) ==
                (g_single_rings ? 2U : g_deep_pipeline ? 4U : 2U) &&
            g_create_swapchain_calls.load(std::memory_order_relaxed) ==
                (g_single_rings ? 4U : 6U) +
                    g_synthetic_private_creates.load(std::memory_order_relaxed) &&
            g_destroy_swapchain_calls.load(std::memory_order_relaxed) ==
                g_create_swapchain_calls.load(std::memory_order_relaxed) &&
            g_application_release_calls.load(std::memory_order_relaxed) == 6 &&
            g_current_acquire_calls.load(std::memory_order_relaxed) == 4 &&
            g_current_wait_calls.load(std::memory_order_relaxed) == 4 &&
            g_current_release_calls.load(std::memory_order_relaxed) == 4 &&
            g_synthetic_acquire_calls.load(std::memory_order_relaxed) == 2 &&
            g_synthetic_wait_calls.load(std::memory_order_relaxed) == 2 &&
            g_synthetic_release_calls.load(std::memory_order_relaxed) == 2 &&
            g_current_create_info_valid.load(std::memory_order_acquire) &&
            g_synthetic_create_info_valid.load(std::memory_order_acquire) &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            return EXIT_FAILURE;
        }
        std::cout << (g_cropped_subimage_mode
                          ? "OpenXR cropped split-eye fake-runtime call-chain test passed\n"
                          : "OpenXR split-eye fake-runtime call-chain test passed\n");
        return EXIT_SUCCESS;
    }

    XrSwapchain swapchain = XR_NULL_HANDLE;
    if (XR_FAILED(create_swapchain(session, &swapchain_info, &swapchain))) {
        return EXIT_FAILURE;
    }
    // d3d11-bridge: a depth swapchain in the packed format, which the
    // bridge cannot share. The creation must succeed, the application must
    // get D3D11 textures it can bind as depth, and the depth information
    // it chains into its views must never reach the runtime.
    XrSwapchain depth_swapchain = XR_NULL_HANDLE;
    std::array<XrSwapchainImageD3D11KHR, 3> d3d11_depth_images{};
    if (g_d3d11_bridge_mode && !g_acquire_ahead_mode) {
        XrSwapchainCreateInfo depth_info = swapchain_info;
        depth_info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depth_info.format = static_cast<std::int64_t>(DXGI_FORMAT_D24_UNORM_S8_UINT);
        for (auto& image : d3d11_depth_images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        }
        std::uint32_t depth_count = 0;
        if (XR_FAILED(create_swapchain(session, &depth_info, &depth_swapchain)) ||
            depth_swapchain != g_application_depth_swapchain ||
            XR_FAILED(enumerate_images(
                depth_swapchain,
                static_cast<std::uint32_t>(d3d11_depth_images.size()),
                &depth_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d11_depth_images.data()))) ||
            depth_count != 3) {
            std::cerr << "d3d11-bridge: the packed depth swapchain was refused\n";
            ComPtr<ID3D11InfoQueue> info_queue;
            if (SUCCEEDED(g_d3d11_device.As(&info_queue))) {
                const UINT64 count = info_queue->GetNumStoredMessages();
                for (UINT64 message_index = 0; message_index < count; ++message_index) {
                    SIZE_T length = 0;
                    if (FAILED(info_queue->GetMessage(message_index, nullptr, &length)) || length == 0) {
                        continue;
                    }
                    std::vector<char> bytes(length);
                    auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                    if (SUCCEEDED(info_queue->GetMessage(message_index, message, &length))) {
                        std::cerr << "  D3D11: " << message->pDescription << '\n';
                    }
                }
            }
            return EXIT_FAILURE;
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC view_description{};
        view_description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        view_description.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        view_description.Texture2DArray.ArraySize = 2;
        ComPtr<ID3D11DepthStencilView> depth_view;
        if (d3d11_depth_images[0].texture == nullptr ||
            FAILED(g_d3d11_device->CreateDepthStencilView(
                d3d11_depth_images[0].texture, &view_description, depth_view.GetAddressOf()))) {
            std::cerr << "d3d11-bridge: the application cannot bind the depth texture\n";
            return EXIT_FAILURE;
        }
    }

    std::uint32_t image_count = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo image_wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    image_wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    std::uint32_t acquired_index = 0;
    std::array<XrSwapchainImageD3D12KHR, 3> swapchain_images{};
    std::array<XrSwapchainImageD3D11KHR, 3> d3d11_swapchain_images{};
    std::array<XrSwapchainImageVulkanKHR, 3> vulkan_swapchain_images{};
    for (auto& image : swapchain_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    }
    for (auto& image : d3d11_swapchain_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    for (auto& image : vulkan_swapchain_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    }
    std::array<XrSwapchainImageD3D12KHR, 2> partial_images{};
    std::array<XrSwapchainImageD3D11KHR, 2> d3d11_partial_images{};
    std::array<XrSwapchainImageVulkanKHR, 2> vulkan_partial_images{};
    for (auto& image : partial_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    }
    for (auto& image : d3d11_partial_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    for (auto& image : vulkan_partial_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    }
    XrSwapchainImageBaseHeader* const full_image_headers =
        g_vulkan_application
            ? reinterpret_cast<XrSwapchainImageBaseHeader*>(
                  vulkan_swapchain_images.data())
            : g_d3d11_interop_mode
                ? reinterpret_cast<XrSwapchainImageBaseHeader*>(
                      d3d11_swapchain_images.data())
                : reinterpret_cast<XrSwapchainImageBaseHeader*>(
                      swapchain_images.data());
    XrSwapchainImageBaseHeader* const partial_image_headers =
        g_vulkan_application
            ? reinterpret_cast<XrSwapchainImageBaseHeader*>(
                  vulkan_partial_images.data())
            : g_d3d11_interop_mode
                ? reinterpret_cast<XrSwapchainImageBaseHeader*>(
                      d3d11_partial_images.data())
                : reinterpret_cast<XrSwapchainImageBaseHeader*>(
                      partial_images.data());
    if (XR_FAILED(enumerate_images(swapchain, 0, &image_count, nullptr)) || image_count != 3 ||
        enumerate_images(
            swapchain,
            static_cast<std::uint32_t>(partial_images.size()),
            &image_count,
            partial_image_headers) !=
            XR_ERROR_SIZE_INSUFFICIENT ||
        XR_FAILED(enumerate_images(
            swapchain,
            static_cast<std::uint32_t>(swapchain_images.size()),
            &image_count,
            full_image_headers)) ||
        image_count != swapchain_images.size() ||
        XR_FAILED(enumerate_images(
            swapchain,
            static_cast<std::uint32_t>(swapchain_images.size()),
            &image_count,
            full_image_headers)) ||
        XR_FAILED(acquire_image(swapchain, &acquire_info, &acquired_index)) || acquired_index != 2 ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        XR_FAILED(release_image(swapchain, &release_info))) {
        return EXIT_FAILURE;
    }

    g_fail_next_release.store(true, std::memory_order_release);
    if (XR_FAILED(acquire_image(swapchain, &acquire_info, &acquired_index)) || acquired_index != 2 ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        release_image(swapchain, &release_info) != XR_ERROR_RUNTIME_FAILURE ||
        !wait_for_queue_idle() ||
        XR_FAILED(release_image(swapchain, &release_info))) {
        return EXIT_FAILURE;
    }

    g_concurrent_acquire_count.store(0, std::memory_order_release);
    g_first_concurrent_acquire_entered.store(false, std::memory_order_release);
    g_concurrent_acquire_mode.store(true, std::memory_order_release);
    XrResult first_acquire_result = XR_ERROR_RUNTIME_FAILURE;
    XrResult second_acquire_result = XR_ERROR_RUNTIME_FAILURE;
    std::uint32_t first_concurrent_index = 0;
    std::uint32_t second_concurrent_index = 0;
    std::thread first_acquire([&] {
        first_acquire_result = acquire_image(
            swapchain,
            &acquire_info,
            &first_concurrent_index);
    });

    const auto acquire_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!g_first_concurrent_acquire_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < acquire_deadline) {
        std::this_thread::yield();
    }
    if (!g_first_concurrent_acquire_entered.load(std::memory_order_acquire)) {
        first_acquire.join();
        return EXIT_FAILURE;
    }

    std::thread second_acquire([&] {
        second_acquire_result = acquire_image(
            swapchain,
            &acquire_info,
            &second_concurrent_index);
    });
    first_acquire.join();
    second_acquire.join();
    g_concurrent_acquire_mode.store(false, std::memory_order_release);

    if (XR_FAILED(first_acquire_result) || XR_FAILED(second_acquire_result) ||
        first_concurrent_index != 0 || second_concurrent_index != 1 ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        XR_FAILED(release_image(swapchain, &release_info)) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        XR_FAILED(release_image(swapchain, &release_info)) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    XrFrameState frame_a{XR_TYPE_FRAME_STATE};
    XrFrameState frame_b{XR_TYPE_FRAME_STATE};
    XrFrameState frame_c{XR_TYPE_FRAME_STATE};
    XrFrameState frame_d{XR_TYPE_FRAME_STATE};
    XrFrameState frame_e{XR_TYPE_FRAME_STATE};
    XrFrameState frame_f{XR_TYPE_FRAME_STATE};
    XrFrameState frame_g{XR_TYPE_FRAME_STATE};
    XrFrameState frame_h{XR_TYPE_FRAME_STATE};
    XrFrameState frame_i{XR_TYPE_FRAME_STATE};
    XrFrameState frame_j{XR_TYPE_FRAME_STATE};
    XrFrameState handoff_frame_a{XR_TYPE_FRAME_STATE};
    XrFrameState handoff_frame_b{XR_TYPE_FRAME_STATE};
    if (!g_steamvr_presenter_mode && !g_flight_simulator_mode) {
        g_throw_from_begin_frame = true;
        const XrResult contained_exception_result =
            begin_frame(session, &frame_begin_info);
        g_throw_from_begin_frame = false;
        if (contained_exception_result != XR_ERROR_RUNTIME_FAILURE) {
            return EXIT_FAILURE;
        }
    }

    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    for (std::uint32_t index = 0; index < projection_views.size(); ++index) {
        auto& projection_view = projection_views[index];
        projection_view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projection_view.pose.orientation.w = 1.0F;
        projection_view.subImage.swapchain = swapchain;
        projection_view.subImage.imageRect.offset =
            g_double_wide_mode
                ? XrOffset2Di{static_cast<std::int32_t>(index * 4U), 0}
                : XrOffset2Di{0, 0};
        projection_view.subImage.imageRect.extent = {4, 4};
        projection_view.subImage.imageArrayIndex =
            g_double_wide_mode ? 0 : index;
    }
    std::array<XrCompositionLayerDepthInfoKHR, 2> depth_infos{};
    if (depth_swapchain != XR_NULL_HANDLE) {
        for (std::uint32_t index = 0; index < depth_infos.size(); ++index) {
            auto& depth = depth_infos[index];
            depth.type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
            depth.subImage = projection_views[index].subImage;
            depth.subImage.swapchain = depth_swapchain;
            depth.minDepth = 0.0F;
            depth.maxDepth = 1.0F;
            depth.nearZ = 0.1F;
            depth.farZ = 100.0F;
            projection_views[index].next = &depth;
        }
    }
    XrSpace application_space = g_space;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = application_space;
    projection.viewCount = static_cast<std::uint32_t>(projection_views.size());
    projection.views = projection_views.data();
    XrCompositionLayerQuad flight_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    flight_quad.space = application_space;
    flight_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    flight_quad.subImage.swapchain = swapchain;
    flight_quad.subImage.imageRect.extent = {4, 4};
    flight_quad.pose.orientation.w = 1.0F;
    flight_quad.size = {1.0F, 1.0F};
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&flight_quad),
    };

    auto submit_frame = [&](XrTime display_time) {
        XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
        locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate_info.displayTime = display_time;
        locate_info.space = application_space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        std::uint32_t view_count = 0;
        if (XR_FAILED(locate_views(
                session,
                &locate_info,
                &view_state,
                static_cast<std::uint32_t>(views.size()),
                &view_count,
                views.data())) ||
            view_count != views.size()) {
            return false;
        }
        for (std::size_t index = 0; index < projection_views.size(); ++index) {
            const XrView submitted = fake_submitted_view_for_time(
                display_time,
                static_cast<std::uint32_t>(index));
            projection_views[index].pose = submitted.pose;
            projection_views[index].fov = submitted.fov;
        }

        XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
        frame_end_info.displayTime = display_time;
        frame_end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frame_end_info.layerCount = g_flight_simulator_mode ? 2 : 1;
        frame_end_info.layers = layers;
        // SteamVR throttles the *inline* second cycle, not the application's
        // own wait. Mark the window so the fake runtime can tell them apart by
        // what is happening rather than by counting calls: the layer runs its
        // internal wait from this thread while the application is inside
        // xrEndFrame, and a parity rule breaks the moment the schedule shifts
        // by a frame.
        g_application_in_end_frame.store(true, std::memory_order_release);
        const bool ended = XR_SUCCEEDED(end_frame(session, &frame_end_info));
        g_application_in_end_frame.store(false, std::memory_order_release);
        return ended;
    };

    auto capture_fresh_application_image = [&] {
        return wait_for_queue_idle() &&
               XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &acquired_index)) &&
               XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
               XR_SUCCEEDED(release_image(swapchain, &release_info));
    };

    if (g_uevr_pipelined_display_time_mode) {
        XrFrameState uevr_frame_a{XR_TYPE_FRAME_STATE};
        XrFrameState uevr_frame_b{XR_TYPE_FRAME_STATE};
        XrFrameState uevr_frame_c{XR_TYPE_FRAME_STATE};
        const bool frame_sequence_succeeded =
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &uevr_frame_a)) &&
            uevr_frame_a.predictedDisplayTime == 100 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(uevr_frame_a.predictedDisplayTime - 10) &&
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &uevr_frame_b)) &&
            uevr_frame_b.predictedDisplayTime == 200 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(uevr_frame_b.predictedDisplayTime - 10) &&
            wait_for_queue_idle() &&
            // The first frame arms generation and passes through, so the pair
            // is the third application frame rather than the second.
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &uevr_frame_c)) &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(uevr_frame_c.predictedDisplayTime - 10) &&
            wait_for_queue_idle();

        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        const bool valid =
            frame_sequence_succeeded && teardown_succeeded &&
            end_records.size() == 4 &&
            // The first frame arms and passes through, the second primes, and
            // the third is the pair. The internal cycle still takes the
            // runtime's own next predicted time rather than the application's.
            end_records[0].display_time == 90 &&
            end_records[0].target == SubmittedTarget::original &&
            end_records[1].display_time == 190 &&
            end_records[1].target == SubmittedTarget::current &&
            end_records[2].display_time == 290 &&
            end_records[2].target == SubmittedTarget::synthetic &&
            end_records[3].display_time == 400 &&
            end_records[3].target == SubmittedTarget::current &&
            g_wait_frame_calls.load(std::memory_order_relaxed) == 4 &&
            // Includes the harness's earlier exception-containment probe.
            g_begin_frame_calls.load(std::memory_order_relaxed) == 5 &&
            g_end_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            return EXIT_FAILURE;
        }
        std::cout <<
            "OpenXR UEVR pipelined display-time call-chain test passed\n";
        return EXIT_SUCCESS;
    }

    if (g_flight_simulator_mode) {
        std::array<XrFrameState, 5> application_frames{{
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
        }};
        auto wait_next_while_beginning_current =
            [&](XrFrameState& next_frame) {
                std::atomic<bool> wait_started{false};
                XrResult wait_result = XR_ERROR_RUNTIME_FAILURE;
                std::thread wait_thread([&] {
                    wait_started.store(true, std::memory_order_release);
                    wait_result = wait_frame(
                        session,
                        &frame_wait_info,
                        &next_frame);
                });
                while (!wait_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const XrResult begin_result =
                    begin_frame(session, &frame_begin_info);
                wait_thread.join();
                return XR_SUCCEEDED(begin_result) && XR_SUCCEEDED(wait_result);
            };
        auto submit_flight_frame = [&](const XrFrameState& frame) {
            return capture_fresh_application_image() &&
                submit_frame(
                    frame.predictedDisplayTime + kFakeDisplayPeriod) &&
                wait_for_queue_idle();
        };

        bool sequence_succeeded =
            XR_SUCCEEDED(wait_frame(
                session,
                &frame_wait_info,
                &application_frames[0])) &&
            application_frames[0].predictedDisplayPeriod ==
                kFakeDisplayPeriod &&
            wait_next_while_beginning_current(application_frames[1]) &&
            application_frames[1].predictedDisplayPeriod ==
                kFakeDisplayPeriod &&
            submit_flight_frame(application_frames[0]) &&
            wait_next_while_beginning_current(application_frames[2]) &&
            application_frames[2].predictedDisplayPeriod ==
                kFakeDisplayPeriod * g_frames_per_application_frame;
        const std::uint32_t downstream_waits_before_transition =
            g_wait_frame_calls.load(std::memory_order_acquire);
        sequence_succeeded = sequence_succeeded &&
            downstream_waits_before_transition == 2;
        g_delay_composition_validation.store(true, std::memory_order_release);
        sequence_succeeded = sequence_succeeded &&
            submit_flight_frame(application_frames[1]);
        application_space = fake_handle<XrSpace>(0x405);
        projection.space = application_space;
        flight_quad.space = application_space;
        g_valid_composition_space.store(
            application_space,
            std::memory_order_release);
        sequence_succeeded = sequence_succeeded &&
            wait_next_while_beginning_current(application_frames[3]) &&
            application_frames[3].predictedDisplayPeriod ==
                kFakeDisplayPeriod * g_frames_per_application_frame &&
            submit_flight_frame(application_frames[2]) &&
            // One more application frame than the pairing needs on its own:
            // the frame that arms generation passes through, so reaching the
            // same generation depth takes one frame longer.
            wait_next_while_beginning_current(application_frames[4]) &&
            submit_flight_frame(application_frames[3]) &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            submit_flight_frame(application_frames[4]);

        const bool destroyed_before_end_session = g_destroy_pending_swapchain &&
            XR_SUCCEEDED(destroy_swapchain(swapchain));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            (destroyed_before_end_session || XR_SUCCEEDED(destroy_swapchain(swapchain))) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        constexpr std::array expected_generated_order{
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
        };
        std::size_t matched_targets = 0;
        std::size_t empty_frames = 0;
        bool composition_preserved = true;
        for (const EndFrameRecord& record : end_records) {
            if (matched_targets < expected_generated_order.size() &&
                record.target == expected_generated_order[matched_targets]) {
                ++matched_targets;
            }
            if (record.layer_count == 0) {
                ++empty_frames;
            } else {
                composition_preserved = composition_preserved &&
                    record.layer_count == 2 &&
                    record.quad_layer_count == 1 &&
                    record.quad_layer_index == 1;
            }
        }
        // One empty frame is allowed mid-sequence, on top of the optional one
        // at teardown. The deeper pipeline holds the first synthetic for a
        // display period while it establishes its depth, and in pipelined mode
        // a held slot goes out empty rather than repeating a frame whose
        // application handles may already be gone - the runtime keeps showing
        // the last image. It happens once: a queue that is a slot deeper stays
        // a slot deeper. More than one means the queue ran dry, which is the
        // defect this check exists for.
        const std::size_t trailing_empty =
            (!end_records.empty() && end_records.back().layer_count == 0)
                ? 1U
                : 0U;
        const bool only_optional_teardown_empty =
            empty_frames <= 1U + trailing_empty;
        const std::uint32_t downstream_waits =
            g_wait_frame_calls.load(std::memory_order_relaxed);
        const bool valid = sequence_succeeded && teardown_succeeded &&
            g_submission_after_destroy.load() == 0 &&
            matched_targets == expected_generated_order.size() &&
            composition_preserved && only_optional_teardown_empty &&
            downstream_waits > downstream_waits_before_transition &&
            downstream_waits ==
                g_begin_frame_calls.load(std::memory_order_relaxed) &&
            downstream_waits ==
                g_end_frame_calls.load(std::memory_order_relaxed) &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << "pipelined presenter validation failed: sequence="
                      << sequence_succeeded << " teardown="
                      << teardown_succeeded << " matched=" << matched_targets
                      << " after-destroy=" << g_submission_after_destroy.load()
                      << " waits-before=" << downstream_waits_before_transition
                      << " empty=" << empty_frames << " composition="
                      << composition_preserved
                      << " waits=" << downstream_waits << " begins="
                      << g_begin_frame_calls.load() << " ends="
                      << g_end_frame_calls.load() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR pipelined continuous-presenter test passed\n";
        return EXIT_SUCCESS;
    }

    if (g_single_threaded_mode || g_vulkan_mode || g_swapchain_budget_mode ||
        g_d3d11_bridge_mode || g_vulkan_bridge_mode) {
        // Every frame from this one thread, as an application with a
        // single-threaded device must and a Vulkan application always does:
        // the runtime submits on the queue the application handed it. In the
        // single-threaded mode the inline second cycle is throttled, which
        // promotes a presenter on any other session; either way the layer has
        // to keep generating inline and make every runtime frame call from
        // this thread. In vulkan mode the generation behind those counts ran
        // through the Vulkan interop on a real device.
        bool frame_sequence_succeeded = true;
        // In vulkan mode every frame is painted a colour of its own before
        // it is released, and once a pair has gone out its current copy has
        // to be found in a private image: the pixels crossed into D3D12 and
        // back, through every wait between the two APIs.
        int pixel_failures = 0;
        XrFrameState application_frame{XR_TYPE_FRAME_STATE};
        // d3d11-bridge-acquire-ahead: one image is acquired before the
        // frames start and stays acquired across a session restart, then
        // every frame waits on the image acquired the frame before, renders
        // it, acquires the next and releases the rendered one. The layer's
        // mirror of the runtime's queue has to survive all of that, because
        // on the bridge it decides which texture reaches the runtime.
        std::uint32_t ahead_index = 0;
        std::uint32_t depth_index = 0;
        if (g_acquire_ahead_mode) {
            g_acquire_ahead_active.store(true, std::memory_order_release);
            if (XR_FAILED(acquire_image(swapchain, &acquire_info, &ahead_index)) ||
                XR_FAILED(end_session(session)) ||
                XR_FAILED(begin_session(session, &session_begin_info))) {
                std::cerr << "acquire-ahead: session restart with an image acquired failed\n";
                return EXIT_FAILURE;
            }
        }
        auto render_application_image = [&](std::uint8_t red) {
            if (g_acquire_ahead_mode) {
                acquired_index = ahead_index;
                return XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
                       paint_d3d11_image(
                           d3d11_swapchain_images[acquired_index].texture, red) &&
                       XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &ahead_index)) &&
                       ahead_index != acquired_index;
            }
            return XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &acquired_index)) &&
                   XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
                   (!g_vulkan_mode ||
                    paint_vulkan_image(
                        g_vulkan_application_swapchain_images[acquired_index], red)) &&
                   // vulkan-bridge: the enumerated images are the layer's
                   // imports; paint what the runtime's D3D12 image must
                   // then receive.
                   (!g_vulkan_bridge_mode ||
                    paint_vulkan_image(
                        vulkan_swapchain_images[acquired_index].image, red)) &&
                   (!g_d3d11_bridge_mode ||
                    paint_d3d11_image(
                        d3d11_swapchain_images[acquired_index].texture, red));
        };
        for (int index = 0; index < 10; ++index) {
            const auto red = static_cast<std::uint8_t>(40 + index * 20);
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(wait_frame(
                    session, &frame_wait_info, &application_frame)) &&
                application_frame.predictedDisplayPeriod == kFakeDisplayPeriod &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                wait_for_queue_idle() &&
                render_application_image(red) &&
                (depth_swapchain == XR_NULL_HANDLE ||
                 (XR_SUCCEEDED(acquire_image(depth_swapchain, &acquire_info, &depth_index)) &&
                  XR_SUCCEEDED(wait_image(depth_swapchain, &image_wait_info)) &&
                  XR_SUCCEEDED(release_image(depth_swapchain, &release_info)))) &&
                XR_SUCCEEDED(release_image(swapchain, &release_info)) &&
                submit_frame(application_frame.predictedDisplayTime) &&
                wait_for_queue_idle();
            // The first frame arms and passes through, the second primes.
            if (g_vulkan_mode && frame_sequence_succeeded && index >= 2 &&
                !vulkan_current_images_contain(red)) {
                ++pixel_failures;
            }
            // d3d11-bridge: what the application painted into the D3D11
            // texture is in the runtime's D3D12 image after the release.
            if ((g_d3d11_bridge_mode || g_vulkan_bridge_mode) && frame_sequence_succeeded) {
                std::uint8_t found = 0;
                if (!d3d12_image_first_red(
                        g_application_swapchain_images[acquired_index].Get(), &found) ||
                    found != red) {
                    ++pixel_failures;
                }
            }
        }
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            (depth_swapchain == XR_NULL_HANDLE ||
             XR_SUCCEEDED(destroy_swapchain(depth_swapchain))) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);
        const std::uint32_t private_depth =
            g_private_depth_submissions.load(std::memory_order_relaxed);
        const std::uint32_t off_thread =
            g_off_thread_frame_calls.load(std::memory_order_relaxed);
        const std::uint32_t synthetic_acquires =
            g_synthetic_acquire_calls.load(std::memory_order_relaxed);
        // swapchain-budget: exactly one refusal, the deeper attempt's fourth
        // private swapchain, and every swapchain the layer got is given back.
        const std::uint32_t refusals = g_budget_refusals.load(std::memory_order_relaxed);
        const bool budget_valid = !g_swapchain_budget_mode ||
            (refusals == 1 &&
             g_destroy_swapchain_calls.load(std::memory_order_relaxed) ==
                 g_create_swapchain_calls.load(std::memory_order_relaxed) - refusals);
        // The fake runtime never throttles this mode, so vulkan stays
        // inline here; only the single-threaded device forbids a presenter.
        const bool valid = frame_sequence_succeeded && teardown_succeeded &&
            (off_thread == 0 || g_vulkan_mode) && pixel_failures == 0 &&
            budget_valid &&
            (!g_d3d11_bridge_mode ||
             g_bridge_session_bound.load(std::memory_order_acquire)) &&
            // The private depth swapchain's information never reaches the
            // runtime, on any of the layer's submissions.
            private_depth == 0 &&
            // Generation still runs, inline: all but the arming frame and
            // the first frame, which primes, submit a synthetic.
            synthetic_acquires >= 7 &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << (g_vulkan_bridge_mode ? "Vulkan bridge" : g_vulkan_mode ? "Vulkan"
                          : g_acquire_ahead_mode ? "D3D11 bridge, acquire ahead"
                          : g_d3d11_bridge_mode ? "D3D11 bridge"
                                                : "single-threaded D3D11")
                      << " validation failed: sequence="
                      << frame_sequence_succeeded << " teardown="
                      << teardown_succeeded << " off-thread=" << off_thread
                      << " synthetic=" << synthetic_acquires
                      << " stale-pixels=" << pixel_failures
                      << " private-depth=" << private_depth
                      << " budget-refusals=" << refusals
                      << " creates=" << g_create_swapchain_calls.load()
                      << " destroys=" << g_destroy_swapchain_calls.load() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << (g_vulkan_mode
                          ? "OpenXR Vulkan interop test passed\n"
                          : g_swapchain_budget_mode
                              ? "OpenXR swapchain-budget fallback test passed\n"
                              : g_d3d11_bridge_mode
                                  ? "OpenXR D3D11 bridge test passed\n"
                                  : "OpenXR single-threaded D3D11 test passed\n");
        return EXIT_SUCCESS;
    }

    if (g_dcs_mode) {
        // DCS's shape from the first frame, on the throttling fake SteamVR,
        // so the presenter arrives by the SteamVR route in the middle of it -
        // as it did in the session that hung. Two kinds of frame:
        //
        //   overlapping: the wait thread issues the next wait before the
        //     render thread begins this frame; the layer holds it until that
        //     begin, then forwards it, and the runtime holds the returned
        //     frame un-begun while this frame is ended. Nothing the layer
        //     does inside that xrEndFrame may wait on the runtime again.
        //   plain: the next wait comes after this frame has ended.
        //
        // DCS alternates between them with load, which is why it never
        // reaches the pipelined route's two consecutive overlaps. The pattern
        // here is plain, plain, overlapping: the two plain frames give the
        // throttled inline cycle its pair, and whichever pair requests the
        // promotion, the frame that starts the presenter is an overlapping
        // one with the runtime holding the application's wait. The presenter
        // has to begin that frame instead of waiting for its own.
        bool sequence_succeeded = true;
        bool presenter_seen = false;
        XrFrameState pending{XR_TYPE_FRAME_STATE};
        sequence_succeeded =
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &pending));
        // Ends the pending frame, having issued the next wait either before
        // (overlapping) or after (plain) the begin. Leaves the next frame
        // pending.
        auto run_frame = [&](bool overlapping) {
            XrFrameState next{XR_TYPE_FRAME_STATE};
            XrResult next_wait_result = XR_ERROR_RUNTIME_FAILURE;
            std::atomic<bool> wait_started{false};
            auto wait_next = [&] {
                g_application_wait_thread_id.store(
                    GetCurrentThreadId(), std::memory_order_release);
                wait_started.store(true, std::memory_order_release);
                next_wait_result = wait_frame(session, &frame_wait_info, &next);
            };
            bool ok = true;
            if (overlapping) {
                std::thread wait_thread(wait_next);
                while (!wait_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ok = XR_SUCCEEDED(begin_frame(session, &frame_begin_info));
                wait_thread.join();
                ok = ok && XR_SUCCEEDED(next_wait_result) &&
                    capture_fresh_application_image() &&
                    submit_frame(pending.predictedDisplayTime) &&
                    wait_for_queue_idle();
            } else {
                ok = XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                    capture_fresh_application_image() &&
                    submit_frame(pending.predictedDisplayTime) &&
                    wait_for_queue_idle();
                std::thread wait_thread(wait_next);
                wait_thread.join();
                ok = ok && XR_SUCCEEDED(next_wait_result);
            }
            presenter_seen = presenter_seen ||
                pending.predictedDisplayPeriod == kFakeDisplayPeriod * g_frames_per_application_frame;
            ok = ok && next.predictedDisplayTime > pending.predictedDisplayTime;
            pending = next;
            return ok;
        };
        constexpr int kPromotionCycles = 8;
        for (int cycle = 0; sequence_succeeded && cycle < kPromotionCycles;
             ++cycle) {
            sequence_succeeded = run_frame(false) && run_frame(false) &&
                run_frame(true);
        }
        const bool promoted_during_pattern = presenter_seen;
        // Under the presenter, every frame overlapping: the label the
        // application ends with is the older of two pending waits, and the
        // lookup has to accept it for the frame to pair.
        std::size_t records_before_overlapping_frames = 0;
        {
            std::scoped_lock lock(g_end_records_mutex);
            records_before_overlapping_frames = g_end_records.size();
        }
        constexpr int kOverlappingFrames = 8;
        for (int index = 0; sequence_succeeded && index < kOverlappingFrames;
             ++index) {
            sequence_succeeded = run_frame(true);
        }
        sequence_succeeded = sequence_succeeded &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(pending.predictedDisplayTime) &&
            wait_for_queue_idle();

        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::size_t synthetic_after = 0;
        std::size_t current_after = 0;
        {
            std::scoped_lock lock(g_end_records_mutex);
            for (std::size_t index = records_before_overlapping_frames;
                 index < g_end_records.size(); ++index) {
                if (g_end_records[index].target == SubmittedTarget::synthetic) {
                    ++synthetic_after;
                } else if (g_end_records[index].target ==
                           SubmittedTarget::current) {
                    ++current_after;
                }
            }
        }
        std::size_t synthetic_total = 0;
        {
            std::scoped_lock lock(g_end_records_mutex);
            for (const EndFrameRecord& record : g_end_records) {
                if (record.target == SubmittedTarget::synthetic) {
                    ++synthetic_total;
                }
            }
        }
        const std::uint32_t adopted =
            g_presenter_begun_application_waits.load(std::memory_order_relaxed);
        const std::uint32_t violations =
            g_waits_while_unbegun.load(std::memory_order_relaxed);
        const std::uint32_t off_thread =
            g_off_thread_frame_calls.load(std::memory_order_relaxed);
        bool valid = false;
        if (g_dcs_d3d11_mode) {
            // The D3D11 shape never takes the presenter: no virtual period,
            // no frame begun by a presenter, nothing to the runtime off the
            // application's two threads. Every frame after the first still
            // generates inline, the overlapping ones by the cycle adopting
            // the held wait: 32 frames in the run, so well over 20 synthetics
            // where passing the overlapping frames through left about 8.
            valid = sequence_succeeded && teardown_succeeded &&
                !promoted_during_pattern && adopted == 0 && off_thread == 0 &&
                violations == 0 && synthetic_total >= 20 &&
                g_waited_display_times.empty() && !g_begun_display_time;
        } else {
            // Every overlapping frame under the presenter but the first has
            // a previous frame to pair with; a run that primes instead
            // submits the current copy alone. A presenter that starts while
            // the runtime holds the application's wait must begin it; one
            // that starts after the application began it has nothing to
            // adopt (about one run in eight), and either way it must never
            // wait over a held frame.
            const bool over_wait = g_presenter_started_over_application_wait.load();
            valid = sequence_succeeded && teardown_succeeded &&
                promoted_during_pattern && (adopted >= 1 || !over_wait) &&
                violations == 0 &&
                synthetic_after + 2 >=
                    static_cast<std::size_t>(kOverlappingFrames) &&
                g_submission_after_destroy.load() == 0 &&
                g_waited_display_times.empty() && !g_begun_display_time;
        }
        if (!valid) {
            std::cerr << "DCS pipelined-wait validation failed: sequence="
                      << sequence_succeeded << " teardown="
                      << teardown_succeeded << " promoted="
                      << promoted_during_pattern << " adopted=" << adopted
                      << " over-wait="
                      << g_presenter_started_over_application_wait.load()
                      << " off-thread=" << off_thread
                      << " violations=" << violations << " synthetic="
                      << synthetic_after << " synthetic-total="
                      << synthetic_total << " current=" << current_after
                      << '\n';
            return EXIT_FAILURE;
        }
        std::cout << (g_dcs_d3d11_mode
                          ? "OpenXR DCS D3D11 inline-only test passed\n"
                          : "OpenXR DCS pipelined-wait pairing test passed\n");
        return EXIT_SUCCESS;
    }

    if (g_own_display_time_mode) {
        // Sequential wait/begin/end, every frame labelled with a time of the
        // application's own. The fake's predicted times are multiples of 100
        // and the virtual ones the presenter serves keep that, so an offset
        // of 37 never names a wait. The third frame is rejected by the
        // runtime while the session is still inline, as the slip was seen;
        // from then on the queue holds a wait no frame will ever match.
        //
        // Every frame after the promotion has a previous frame to pair with.
        // A layer that keeps the orphan primes all of them, which is the
        // reported session: a real frame and a repeat, for as long as the
        // game ran.
        const auto own_time = [](XrTime predicted) { return predicted + 37; };
        constexpr int kInlineFrames = 4;
        constexpr int kPresenterFrames = 12;
        constexpr int kRejectedFrame = 2;
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        XrTime last_predicted = 0;
        bool sequence_succeeded = true;
        bool promoted = false;
        bool rejected_while_inline = false;
        std::size_t records_at_promotion = 0;
        for (int index = 0;
             sequence_succeeded && index < kInlineFrames + kPresenterFrames;
             ++index) {
            sequence_succeeded =
                XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame)) &&
                frame.predictedDisplayTime > last_predicted &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                capture_fresh_application_image();
            if (!sequence_succeeded) {
                break;
            }
            last_predicted = frame.predictedDisplayTime;
            if (!promoted &&
                frame.predictedDisplayPeriod ==
                    kFakeDisplayPeriod * g_frames_per_application_frame) {
                promoted = true;
                std::scoped_lock lock(g_end_records_mutex);
                records_at_promotion = g_end_records.size();
            }
            if (index == kRejectedFrame) {
                rejected_while_inline = !promoted;
                g_reject_end_time_invalid.store(true, std::memory_order_release);
                // The rejection reaches the application as its own error.
                sequence_succeeded =
                    !submit_frame(own_time(frame.predictedDisplayTime)) &&
                    g_time_rejections.load(std::memory_order_relaxed) == 1;
            } else {
                sequence_succeeded =
                    submit_frame(own_time(frame.predictedDisplayTime));
            }
        }

        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::size_t synthetic_after = 0;
        std::size_t current_after = 0;
        {
            std::scoped_lock lock(g_end_records_mutex);
            for (std::size_t index = records_at_promotion;
                 index < g_end_records.size(); ++index) {
                if (g_end_records[index].target == SubmittedTarget::synthetic) {
                    ++synthetic_after;
                } else if (g_end_records[index].target ==
                           SubmittedTarget::current) {
                    ++current_after;
                }
            }
        }
        // The frame after the rejection sheds the orphan and primes, and the
        // first frame under the presenter may prime too; everything else
        // pairs.
        const bool valid = sequence_succeeded && teardown_succeeded &&
            promoted && rejected_while_inline &&
            g_time_rejections.load(std::memory_order_relaxed) == 1 &&
            synthetic_after + 4 >= static_cast<std::size_t>(kPresenterFrames) &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << "SteamVR own-display-time validation failed: sequence="
                      << sequence_succeeded << " teardown="
                      << teardown_succeeded << " promoted=" << promoted
                      << " rejected-inline=" << rejected_while_inline
                      << " rejections=" << g_time_rejections.load()
                      << " synthetic=" << synthetic_after
                      << " current=" << current_after << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR SteamVR own-display-time pairing test passed\n";
        return EXIT_SUCCESS;
    }

    if (g_promise_mode) {
        // Inline frames until the presenter is promoted, then enough paired
        // frames for the layer to measure where real frames go down, move the
        // promise, and show it there. The timing is checked from the flight
        // log by layer_promise_shown_time.cmake.
        bool frame_sequence_succeeded = true;
        XrTime last_predicted = 0;
        // XRFG_TEST_REPEAT_STAMP_EVERY=N: every Nth frame is stamped with the
        // display time of the frame before it, as Unreal Engine 4's OpenXR
        // plugin does now and then (layer_repeat_stamp.cmake).
        const char* repeat_setting = std::getenv("XRFG_TEST_REPEAT_STAMP_EVERY");
        const int repeat_every = repeat_setting ? std::atoi(repeat_setting) : 0;
        XrTime previous_stamp = 0;
        int repeated_stamps = 0;
        // XRFG_TEST_PROMISE_FRAMES: how many frames to run (320).
        const char* frames_setting = std::getenv("XRFG_TEST_PROMISE_FRAMES");
        const int frame_count = frames_setting ? std::atoi(frames_setting) : 320;
        for (int index = 0; index < frame_count && frame_sequence_succeeded; ++index) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            frame_sequence_succeeded =
                XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame)) &&
                frame.predictedDisplayTime > last_predicted &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info));
            last_predicted = frame.predictedDisplayTime;
            if (index >= 4) {
                std::this_thread::sleep_for(std::chrono::microseconds(
                    kFakeDisplayPeriod * 3 / 2 / 1000));
            }
            // Alternately a little after and a little before the stamp it
            // repeats, as Hubris's are.
            const XrTime stamp = repeat_every > 0 && index > 16 &&
                    index % repeat_every == 0 && previous_stamp != 0
                ? previous_stamp + ((index / repeat_every) % 2 != 0 ? 1500 : -2500)
                : frame.predictedDisplayTime;
            repeated_stamps += stamp != frame.predictedDisplayTime ? 1 : 0;
            previous_stamp = stamp;
            frame_sequence_succeeded = frame_sequence_succeeded &&
                capture_fresh_application_image() &&
                submit_frame(stamp);
        }
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);
        if (!frame_sequence_succeeded || !teardown_succeeded) {
            std::cerr << "promise-shown-time frame loop failed: frames="
                      << frame_sequence_succeeded << " teardown="
                      << teardown_succeeded << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR promise-shown-time frame loop completed; repeated stamps "
                  << repeated_stamps << "\n";
        return EXIT_SUCCESS;
    }

    if (g_steamvr_presenter_mode) {
        // The promotion is requested when the throttled streak completes and
        // takes effect at the next xrEndFrame, so one more application frame
        // runs inline at the real rate before the virtual half-rate loop starts.
        std::array<XrFrameState, 8> application_frames{{
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
        }};
        bool frame_sequence_succeeded = true;
        for (std::size_t index = 0; index < 4; ++index) {
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(wait_frame(
                    session,
                    &frame_wait_info,
                    &application_frames[index])) &&
                application_frames[index].predictedDisplayPeriod ==
                    kFakeDisplayPeriod &&
                (index == 0 ||
                 application_frames[index].predictedDisplayTime >
                     application_frames[index - 1].predictedDisplayTime) &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                capture_fresh_application_image() &&
                submit_frame(application_frames[index].predictedDisplayTime);
        }

        // Two ordinary paired frames. The presenter is promoted earlier now
        // that the layer measures the runtime's pacing directly instead of
        // waiting for three throttled inline cycles, so these stand in for the
        // warm-up frames the old criterion needed to reach the same depth of
        // generated submissions.
        XrFrameState filler_frame{XR_TYPE_FRAME_STATE};
        for (int filler = 0; filler < 2; ++filler) {
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(wait_frame(
                    session, &frame_wait_info, &filler_frame)) &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                capture_fresh_application_image() &&
                submit_frame(filler_frame.predictedDisplayTime);
        }

        // frameWaitInfo and frameBeginInfo are optional in the registry, and the
        // presenter answers both calls itself rather than forwarding them, so
        // this frame passes null to keep that path accepting what a runtime
        // would. Luke Ross's mods call xrWaitFrame(session, NULL, &state).
        frame_sequence_succeeded = frame_sequence_succeeded &&
            XR_SUCCEEDED(wait_frame(
                session,
                nullptr,
                &application_frames[4])) &&
            application_frames[4].predictedDisplayPeriod ==
                kFakeDisplayPeriod * g_frames_per_application_frame &&
            application_frames[4].predictedDisplayTime >
                application_frames[3].predictedDisplayTime &&
            XR_SUCCEEDED(begin_frame(session, nullptr)) &&
            capture_fresh_application_image() &&
            submit_frame(application_frames[4].predictedDisplayTime);

        if (g_destroy_pending_space) {
            frame_sequence_succeeded = frame_sequence_succeeded && destroy_space &&
                XR_SUCCEEDED(destroy_space(application_space));
            // The presenter must not autonomously repeat a frame naming the
            // destroyed space while the application prepares its next frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }

        XrFrameEndInfo empty_end{XR_TYPE_FRAME_END_INFO};
        if (frame_sequence_succeeded) {
            frame_sequence_succeeded =
                XR_SUCCEEDED(wait_frame(
                    session,
                    &frame_wait_info,
                    &application_frames[5])) &&
                application_frames[5].predictedDisplayPeriod ==
                    kFakeDisplayPeriod * g_frames_per_application_frame &&
                application_frames[5].predictedDisplayTime >
                    application_frames[4].predictedDisplayTime &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info));
            empty_end.displayTime = application_frames[5].predictedDisplayTime;
            empty_end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(end_frame(session, &empty_end));
        }

        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        constexpr std::array<SubmittedTarget, 9> expected_generated_order{
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
        };
        std::size_t matched_targets = 0;
        for (const EndFrameRecord& record : end_records) {
            if (matched_targets < expected_generated_order.size() &&
                record.target == expected_generated_order[matched_targets]) {
                ++matched_targets;
            }
        }
        const std::uint32_t waits =
            g_wait_frame_calls.load(std::memory_order_relaxed);
        const bool valid = frame_sequence_succeeded && teardown_succeeded &&
            g_submission_after_destroy.load() == 0 &&
            matched_targets == expected_generated_order.size() &&
            waits == g_begin_frame_calls.load(std::memory_order_relaxed) &&
            waits == g_end_frame_calls.load(std::memory_order_relaxed) &&
            waits > application_frames.size() &&
            g_current_acquire_calls.load(std::memory_order_relaxed) >= 5 &&
            g_synthetic_acquire_calls.load(std::memory_order_relaxed) >= 4 &&
            (!g_refuse_layer_mode ||
             g_layer_refusals.load(std::memory_order_relaxed) == 1) &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << "SteamVR presenter validation failed: sequence="
                      << frame_sequence_succeeded << " teardown="
                      << teardown_succeeded << " matched=" << matched_targets
                      << " after-destroy=" << g_submission_after_destroy.load()
                      << " refusals=" << g_layer_refusals.load()
                      << " waits=" << waits << " begins="
                      << g_begin_frame_calls.load() << " ends="
                      << g_end_frame_calls.load() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR SteamVR continuous-presenter test passed\n";
        return EXIT_SUCCESS;
    }

    // The frame that first names a swapchain as a projection view is the frame
    // that arms generation, and it passes through unchanged: taking the
    // resources between the application's xrEndFrame and the submission that
    // follows would put their creation cost on that frame's deadline. So the
    // first generated submission lands one display period later than the naive
    // schedule, every display time after it moves with it, and the session
    // ends with one generated pair fewer and one more application frame passed
    // through in its place. This holds for every graphics API: the D3D11
    // interop, which also does not exist until that first projection use, is
    // no longer a special case.
    const XrTime arm_shift = -100;
    const std::uint32_t arm_skip = 1U;

    // A is primed while B is already waited, proving that the first generated
    // submission still performs exactly one downstream end.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_a)) ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_b)) ||
        frame_a.predictedDisplayTime != 100 || frame_b.predictedDisplayTime != 200 ||
        !submit_frame(frame_a.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_b.predictedDisplayTime) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    // A failed synthetic release must preserve the original application
    // projection and recover its pending ownership on the following pair.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_c)) ||
        frame_c.predictedDisplayTime != 400 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }
    g_fail_next_synthetic_release.store(true, std::memory_order_release);
    if (!submit_frame(frame_c.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_d)) ||
        frame_d.predictedDisplayTime != 500 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }

    // The internal current cycle is serialized against a concurrent
    // application wait. shouldRender=false still closes that cycle legally.
    g_next_wait_should_not_render.store(true, std::memory_order_release);
    g_block_atomic_end_time.store(600 + arm_shift, std::memory_order_release);
    g_block_atomic_end.store(true, std::memory_order_release);
    bool frame_d_submit_succeeded = false;
    std::thread frame_d_submit_thread([&] {
        frame_d_submit_succeeded = submit_frame(frame_d.predictedDisplayTime);
    });
    const auto atomic_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!g_atomic_end_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < atomic_deadline) {
        std::this_thread::yield();
    }
    if (!g_atomic_end_entered.load(std::memory_order_acquire)) {
        g_allow_atomic_end_return.store(true, std::memory_order_release);
        frame_d_submit_thread.join();
        return EXIT_FAILURE;
    }

    XrResult frame_e_wait_result = XR_ERROR_RUNTIME_FAILURE;
    const std::uint32_t waits_before_concurrent_application =
        g_wait_frame_calls.load(std::memory_order_acquire);
    std::thread frame_e_wait_thread([&] {
        frame_e_wait_result = wait_frame(session, &frame_wait_info, &frame_e);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool internal_sequence_was_atomic =
        g_wait_frame_calls.load(std::memory_order_acquire) ==
        waits_before_concurrent_application;
    g_allow_atomic_end_return.store(true, std::memory_order_release);
    frame_d_submit_thread.join();
    frame_e_wait_thread.join();
    if (!internal_sequence_was_atomic || !frame_d_submit_succeeded ||
        XR_FAILED(frame_e_wait_result) || frame_e.predictedDisplayTime != 700 + arm_shift ||
        !wait_for_queue_idle() ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }

    // A failed internal wait occurs only after the synthetic application end.
    // It clears continuity, so the following frame must prime again.
    g_fail_next_wait_frame.store(true, std::memory_order_release);
    if (!submit_frame(frame_e.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_f)) ||
        frame_f.predictedDisplayTime != 800 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_f.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_g)) ||
        frame_g.predictedDisplayTime != 900 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }

    // FidelityFX must tolerate an occasional slow synthetic downstream end.
    // The NVIDIA-only recoverable cooldown must not suppress its matching
    // internal current-B cycle or clear otherwise valid continuity.
    const std::uint32_t waits_before_slow_pair =
        g_wait_frame_calls.load(std::memory_order_acquire);
    const std::uint32_t begins_before_slow_pair =
        g_begin_frame_calls.load(std::memory_order_acquire);
    const std::uint32_t ends_before_slow_pair =
        g_end_frame_calls.load(std::memory_order_acquire);
    g_delay_end_time.store(frame_g.predictedDisplayTime, std::memory_order_release);
    if (!submit_frame(frame_g.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        g_wait_frame_calls.load(std::memory_order_acquire) != waits_before_slow_pair + 1 ||
        g_begin_frame_calls.load(std::memory_order_acquire) != begins_before_slow_pair + 1 ||
        g_end_frame_calls.load(std::memory_order_acquire) != ends_before_slow_pair + 2) {
        return EXIT_FAILURE;
    }

    // The following FidelityFX frame remains a pair instead of becoming a
    // three-second original-frame pass-through window.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_h)) ||
        frame_h.predictedDisplayTime != 1100 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_h.predictedDisplayTime) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    // Continue two more ordinary pairs to prove the slow end did not leave a
    // delayed cooldown or force a fresh-prime discontinuity.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_i)) ||
        frame_i.predictedDisplayTime != 1300 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_i.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_j)) ||
        frame_j.predictedDisplayTime != 1500 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_j.predictedDisplayTime) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    // AER+AFW can pipeline a second application wait from another thread just
    // before the first wait's thread calls begin. The second wait must remain
    // outside the runtime until that matching begin has been forwarded.
    g_wait_begin_handoff_calls.store(0, std::memory_order_release);
    g_second_handoff_wait_entered.store(false, std::memory_order_release);
    g_allow_second_handoff_wait_return.store(false, std::memory_order_release);
    g_wait_begin_handoff_mode.store(true, std::memory_order_release);
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &handoff_frame_a)) ||
        handoff_frame_a.predictedDisplayTime != 1700 + arm_shift) {
        g_wait_begin_handoff_mode.store(false, std::memory_order_release);
        return EXIT_FAILURE;
    }

    XrResult second_handoff_wait_result = XR_ERROR_RUNTIME_FAILURE;
    std::thread second_handoff_wait([&] {
        second_handoff_wait_result =
            wait_frame(session, &frame_wait_info, &handoff_frame_b);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool second_wait_overtook_begin =
        g_second_handoff_wait_entered.load(std::memory_order_acquire);
    const auto begin_start = std::chrono::steady_clock::now();
    const XrResult first_handoff_begin_result =
        begin_frame(session, &frame_begin_info);
    const auto begin_duration = std::chrono::steady_clock::now() - begin_start;
    g_allow_second_handoff_wait_return.store(true, std::memory_order_release);
    second_handoff_wait.join();
    g_wait_begin_handoff_mode.store(false, std::memory_order_release);

    XrFrameEndInfo handoff_end_info{XR_TYPE_FRAME_END_INFO};
    handoff_end_info.displayTime = handoff_frame_a.predictedDisplayTime;
    handoff_end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const bool handoff_valid =
        !second_wait_overtook_begin &&
        begin_duration < std::chrono::milliseconds(100) &&
        XR_SUCCEEDED(first_handoff_begin_result) &&
        XR_SUCCEEDED(second_handoff_wait_result) &&
        handoff_frame_b.predictedDisplayTime == 1800 + arm_shift &&
        XR_SUCCEEDED(end_frame(session, &handoff_end_info)) &&
        XR_SUCCEEDED(begin_frame(session, &frame_begin_info));
    handoff_end_info.displayTime = handoff_frame_b.predictedDisplayTime;
    if (!handoff_valid || XR_FAILED(end_frame(session, &handoff_end_info)) ||
        XR_FAILED(end_session(session)) ||
        XR_FAILED(destroy_swapchain(swapchain)) ||
        XR_FAILED(destroy_session(session)) ||
        XR_FAILED(destroy_instance(instance))) {
        return EXIT_FAILURE;
    }

    FreeLibrary(module);
    std::ifstream log_stream(argv[2]);
    std::ostringstream log_text;
    log_text << log_stream.rdbuf();
    const std::string log = log_text.str();
    // Which display time carries the first generated pair is not a property of
    // the layer worth pinning. The private swapchains are taken when a
    // projection layer first names an application swapchain, so the frame that
    // arms is itself a prime and the first pair is the one after it, and the
    // first synthetic in the log is not necessarily one whose internal cycle
    // completes - a failed synthetic release or a failed internal wait leaves
    // one standing alone. Walk the synthetics until one is followed by its own
    // internal current cycle with nothing in between; what the assertions below
    // check is that the cycle follows the synthetic and carries the next
    // display time. Every locate in the session is pinned separately by
    // expected_locate_times, which is what proves the internal cycle locates no
    // views of its own.
    const std::string synthetic_prefix =
        "[XRFG-FAKE] downstream end frame target=synthetic time=";
    const std::string locate_marker = "[XRFG-FAKE] downstream locate views";
    std::size_t first_synthetic_end = std::string::npos;
    std::size_t first_internal_wait = std::string::npos;
    std::size_t first_internal_begin = std::string::npos;
    std::size_t first_current_end = std::string::npos;
    for (std::size_t candidate = log.find(synthetic_prefix);
         candidate != std::string::npos;
         candidate =
             log.find(synthetic_prefix, candidate + synthetic_prefix.size())) {
        const std::size_t value_begin = candidate + synthetic_prefix.size();
        std::size_t value_end = value_begin;
        long long synthetic_time = 0;
        while (value_end < log.size() && log[value_end] >= '0' &&
               log[value_end] <= '9') {
            synthetic_time = synthetic_time * 10 + (log[value_end] - '0');
            ++value_end;
        }
        if (value_end == value_begin) {
            continue;
        }
        const std::string internal_time = std::to_string(synthetic_time + 100);
        const std::size_t current_end = log.find(
            "[XRFG-FAKE] downstream end frame target=current time=" +
                internal_time,
            candidate);
        if (current_end == std::string::npos) {
            continue;
        }
        // An application frame always locates views before it submits, so a
        // locate between the two ends means this current came from the
        // application rather than from the synthetic's internal cycle.
        if (log.find(locate_marker, candidate) < current_end) {
            continue;
        }
        first_synthetic_end = candidate;
        first_current_end = current_end;
        first_internal_wait = log.find(
            "[XRFG-FAKE] downstream wait frame time=" + internal_time,
            candidate);
        first_internal_begin = log.find(
            "[XRFG-FAKE] downstream begin frame time=" + internal_time,
            candidate);
        break;
    }

    std::vector<EndFrameRecord> end_records;
    {
        std::scoped_lock lock(g_end_records_mutex);
        end_records = g_end_records;
    }
    std::vector<LocateViewsRecord> locate_records;
    {
        std::scoped_lock lock(g_locate_records_mutex);
        locate_records = g_locate_records;
    }
    const auto nearly_equal = [](float actual, float expected) {
        return std::fabs(actual - expected) <= 1.0e-5F;
    };
    const auto pose_matches = [&](const XrPosef& actual, const XrPosef& expected) {
        return nearly_equal(actual.orientation.x, expected.orientation.x) &&
               nearly_equal(actual.orientation.y, expected.orientation.y) &&
               nearly_equal(actual.orientation.z, expected.orientation.z) &&
               nearly_equal(actual.orientation.w, expected.orientation.w) &&
               nearly_equal(actual.position.x, expected.position.x) &&
               nearly_equal(actual.position.y, expected.position.y) &&
               nearly_equal(actual.position.z, expected.position.z);
    };
    const auto fov_matches = [&](const XrFovf& actual, const XrFovf& expected) {
        return nearly_equal(actual.angleLeft, expected.angleLeft) &&
               nearly_equal(actual.angleRight, expected.angleRight) &&
               nearly_equal(actual.angleUp, expected.angleUp) &&
               nearly_equal(actual.angleDown, expected.angleDown);
    };
    const XrView located_reference = fake_view_for_time(100, 0);
    const XrView submitted_reference = fake_submitted_view_for_time(100, 0);
    const bool submitted_camera_differs_from_locate =
        !pose_matches(submitted_reference.pose, located_reference.pose) &&
        !fov_matches(submitted_reference.fov, located_reference.fov);
    const auto record_matches = [&](std::size_t index,
                                    XrTime time,
                                    SubmittedTarget target,
                                    std::uint32_t layer_count,
                                    XrTime metadata_time) {
        if (index >= end_records.size() ||
            end_records[index].display_time != time ||
            end_records[index].target != target ||
            end_records[index].layer_count != layer_count) {
            return false;
        }
        if (layer_count == 0) {
            return end_records[index].space == XR_NULL_HANDLE;
        }
        if (end_records[index].space != g_space) {
            return false;
        }
        if (g_double_wide_mode &&
            (end_records[index].image_rects[0].offset.x != 0 ||
             end_records[index].image_rects[0].extent.width != 4 ||
             end_records[index].image_rects[1].offset.x != 4 ||
             end_records[index].image_rects[1].extent.width != 4)) {
            return false;
        }
        for (std::uint32_t view_index = 0; view_index < 2; ++view_index) {
            const XrView expected = fake_submitted_view_for_time(
                metadata_time,
                view_index);
            if (!pose_matches(end_records[index].poses[view_index], expected.pose) ||
                !fov_matches(end_records[index].fovs[view_index], expected.fov)) {
                return false;
            }
        }
        return true;
    };

    struct ExpectedEnd {
        XrTime display_time;
        SubmittedTarget target;
        std::uint32_t layer_count;
        XrTime metadata_time;
    };

    // The first frame arms and passes through, the second one primes, and the
    // pair after that is the first generated one. Every submission from the
    // failed-synthetic-release frame onwards therefore carries arm_shift.
    std::vector<ExpectedEnd> expected_ends{
        {100, SubmittedTarget::original, 1, 100},
        {200, SubmittedTarget::current, 1, 200},
    };
    for (const ExpectedEnd& record : std::initializer_list<ExpectedEnd>{
             {400, SubmittedTarget::original, 1, 400},
             {500, SubmittedTarget::synthetic, 1, 500},
             {600, SubmittedTarget::none, 0, 0},
             {700, SubmittedTarget::synthetic, 1, 700},
             {800, SubmittedTarget::current, 1, 800},
             {900, SubmittedTarget::synthetic, 1, 900},
             {1000, SubmittedTarget::current, 1, 900},
             {1100, SubmittedTarget::synthetic, 1, 1100},
             {1200, SubmittedTarget::current, 1, 1100},
             {1300, SubmittedTarget::synthetic, 1, 1300},
             {1400, SubmittedTarget::current, 1, 1300},
             {1500, SubmittedTarget::synthetic, 1, 1500},
             {1600, SubmittedTarget::current, 1, 1500},
             {1700, SubmittedTarget::none, 0, 0},
             {1800, SubmittedTarget::none, 0, 0},
         }) {
        expected_ends.push_back(ExpectedEnd{
            record.display_time + arm_shift,
            record.target,
            record.layer_count,
            record.metadata_time == 0 ? 0 : record.metadata_time + arm_shift,
        });
    }

    bool frame_outputs_valid = end_records.size() == expected_ends.size();
    for (std::size_t index = 0;
         frame_outputs_valid && index < expected_ends.size();
         ++index) {
        frame_outputs_valid = record_matches(index,
                                             expected_ends[index].display_time,
                                             expected_ends[index].target,
                                             expected_ends[index].layer_count,
                                             expected_ends[index].metadata_time);
    }

    const std::array<XrTime, 10> expected_locate_times{
        100,
        200,
        400 + arm_shift,
        500 + arm_shift,
        700 + arm_shift,
        800 + arm_shift,
        900 + arm_shift,
        1100 + arm_shift,
        1300 + arm_shift,
        1500 + arm_shift,
    };
    bool locate_sequence_valid = locate_records.size() == expected_locate_times.size();
    for (std::size_t index = 0;
         locate_sequence_valid && index < expected_locate_times.size();
         ++index) {
        locate_sequence_valid =
            locate_records[index].display_time == expected_locate_times[index] &&
            locate_records[index].space == g_space &&
            locate_records[index].view_configuration_type ==
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }

    const bool valid =
        frame_outputs_valid &&
        locate_sequence_valid &&
        submitted_camera_differs_from_locate &&
        log.find("[XRFG-V") == std::string::npos &&
        first_synthetic_end != std::string::npos &&
        first_internal_wait != std::string::npos &&
        first_internal_begin != std::string::npos &&
        first_current_end != std::string::npos &&
        first_synthetic_end < first_internal_wait &&
        first_internal_wait < first_internal_begin &&
        first_internal_begin < first_current_end &&
        count_occurrences(log, "[XRFG-FAKE] downstream release entered") == 14 &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=current") == 7 - arm_skip &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=synthetic") == 7 - arm_skip &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=original") == 1 + arm_skip &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=none") == 3 &&
        count_occurrences(log, "[XRFG-FAKE] downstream locate views") == 10 &&
        g_wait_frame_calls.load(std::memory_order_relaxed) == 19 - arm_skip &&
        g_begin_frame_calls.load(std::memory_order_relaxed) == 19 - arm_skip &&
        g_end_frame_calls.load(std::memory_order_relaxed) == 18 - arm_skip &&
        g_locate_views_calls.load(std::memory_order_relaxed) == 10 &&
        // One application swapchain, two current slots, and however many
        // synthetic slots this depth needs. Every private swapchain created is
        // destroyed, which is the part that must hold at either depth.
        g_synthetic_private_creates.load(std::memory_order_relaxed) ==
            (g_single_rings ? 1U : g_deep_pipeline ? 2U : 1U) &&
        g_create_swapchain_calls.load(std::memory_order_relaxed) ==
            (g_single_rings ? 2U : 3U) +
                g_synthetic_private_creates.load(std::memory_order_relaxed) &&
        g_destroy_swapchain_calls.load(std::memory_order_relaxed) ==
            g_create_swapchain_calls.load(std::memory_order_relaxed) &&
        g_application_release_calls.load(std::memory_order_relaxed) == 14 &&
        // The pair whose synthetic release the runtime refuses: the ring
        // path has already taken its current image at prepare, while the
        // single-swapchain hand-over never takes one for a frame that passes
        // through. Its synthetic was taken on both paths, and both release
        // it once more - the ring path when the swapchain is destroyed, the
        // hand-over as the retry before the next acquire.
        g_current_acquire_calls.load(std::memory_order_relaxed) ==
            (g_single_rings ? 9 : 10) - arm_skip &&
        g_current_wait_calls.load(std::memory_order_relaxed) ==
            (g_single_rings ? 9 : 10) - arm_skip &&
        g_current_release_calls.load(std::memory_order_relaxed) ==
            (g_single_rings ? 9 : 10) - arm_skip &&
        g_synthetic_acquire_calls.load(std::memory_order_relaxed) == 8 - arm_skip &&
        g_synthetic_wait_calls.load(std::memory_order_relaxed) == 8 - arm_skip &&
        g_synthetic_release_calls.load(std::memory_order_relaxed) == 9 - arm_skip &&
        g_waited_display_times.empty() &&
        !g_begun_display_time &&
        g_current_create_info_valid.load(std::memory_order_acquire) &&
        g_synthetic_create_info_valid.load(std::memory_order_acquire);
    if (!valid) {
        std::cerr << "call-chain validation failed: outputs="
                  << frame_outputs_valid << " locate=" << locate_sequence_valid
                  << " releases=" << g_application_release_calls.load()
                  << " current=" << g_current_acquire_calls.load() << '/'
                  << g_current_wait_calls.load() << '/'
                  << g_current_release_calls.load()
                  << " synthetic=" << g_synthetic_acquire_calls.load() << '/'
                  << g_synthetic_wait_calls.load() << '/'
                  << g_synthetic_release_calls.load()
                  << " waits=" << g_wait_frame_calls.load()
                  << " begins=" << g_begin_frame_calls.load()
                  << " ends=" << g_end_frame_calls.load()
                  << " locates=" << g_locate_views_calls.load() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << (g_double_wide_mode
                      ? "OpenXR double-wide fake-runtime call-chain test passed\n"
                      : "OpenXR layer fake-runtime call-chain test passed\n");
    return EXIT_SUCCESS;
}
