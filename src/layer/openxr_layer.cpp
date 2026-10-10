#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/ngx_guide_capture.hpp"
#include "xrfg/d3d11_bridge.hpp"
#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/vulkan_d3d12_interop.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/provider_api.hpp"
#include <atomic>
#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/steamvr_delivery.hpp"

#include <windows.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace xrfg {
void set_optiscaler_embedded_delegation(bool active) noexcept;
}

namespace {

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept;

namespace optiscaler_bootstrap {

using ProviderIdentity = int (*)(OFXR_OptiScalerProviderIdentityV2*);
using SetEmbeddedLayerActive = int (*)(int);
using NegotiateLayer = XrResult(XRAPI_PTR *)(
    const XrNegotiateLoaderInfo*,
    const char*,
    XrNegotiateApiLayerRequest*);

struct Exports {
    HMODULE module{};
    SetEmbeddedLayerActive set_active{};
    NegotiateLayer negotiate{};
};

[[nodiscard]] bool enumerate_modules(
    HMODULE* modules, DWORD capacity, DWORD* bytes) noexcept {
    __try {
        return EnumProcessModules(
                   GetCurrentProcess(), modules, capacity, bytes) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] FARPROC find_export(HMODULE module, const char* name) noexcept {
    __try {
        return GetProcAddress(module, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

[[nodiscard]] bool retain_export_module(
    FARPROC address, HMODULE expected, HMODULE* retained) noexcept {
    __try {
        HMODULE module = nullptr;
        if (address == nullptr ||
            !GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(address), &module)) {
            return false;
        }
        if (module != expected) {
            FreeLibrary(module);
            return false;
        }
        *retained = module;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool query_identity(
    ProviderIdentity provider,
    OFXR_OptiScalerProviderIdentityV2* identity) {
    __try {
        return provider(identity) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool set_active(
    SetEmbeddedLayerActive function, int active, int* result) {
    __try {
        *result = function(active);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool delegate_negotiation(
    NegotiateLayer function,
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request,
    XrResult* result) {
    __try {
        *result = function(loader_info, layer_name, layer_request);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] std::optional<Exports> find_optiscaler() noexcept {
    std::array<HMODULE, 1024> modules{};
    DWORD bytes = 0;
    if (!enumerate_modules(
            modules.data(), static_cast<DWORD>(sizeof(modules)), &bytes)) {
        return std::nullopt;
    }
    const std::size_t count =
        std::min<std::size_t>(bytes / sizeof(HMODULE), modules.size());
    for (std::size_t index = 0; index < count; ++index) {
        const HMODULE candidate = modules[index];
        if (candidate == nullptr) continue;

        const auto provider = reinterpret_cast<ProviderIdentity>(
            find_export(candidate, "OFXR_OptiScalerProviderV2"));
        HMODULE retained = nullptr;
        if (provider == nullptr ||
            !retain_export_module(
                reinterpret_cast<FARPROC>(provider), candidate, &retained)) {
            continue;
        }

        const auto activation = reinterpret_cast<SetEmbeddedLayerActive>(
            find_export(retained, "OFXR_SetEmbeddedLayerActiveV1"));
        const auto negotiation = reinterpret_cast<NegotiateLayer>(
            find_export(retained, "xrNegotiateLoaderApiLayerInterface"));
        OFXR_OptiScalerProviderIdentityV2 identity{};
        constexpr std::uint64_t required_capabilities =
            OFXR_OPTISCALER_CAP_DLSS_GUIDES_V2;
        bool compatible = false;
        try {
            compatible = activation != nullptr && negotiation != nullptr &&
                query_identity(provider, &identity) &&
                identity.struct_size >= sizeof(identity) &&
                identity.api_version == OFXR_PROVIDER_API_VERSION_V2 &&
                identity.magic == OFXR_OPTISCALER_PROVIDER_MAGIC_V2 &&
                (identity.capabilities & required_capabilities) ==
                    required_capabilities;
        } catch (...) {
            compatible = false;
        }
        if (compatible) {
            return Exports{retained, activation, negotiation};
        }
        FreeLibrary(retained);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<XrResult> negotiate(
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request) noexcept {
    auto exports = find_optiscaler();
    if (!exports) return std::nullopt;

    bool activated = false;
    try {
        int activation_result = 0;
        if (!set_active(exports->set_active, 1, &activation_result) ||
            activation_result == 0) {
            FreeLibrary(exports->module);
            return std::nullopt;
        }
        activated = true;

        XrResult result = XR_ERROR_INITIALIZATION_FAILED;
        if (!delegate_negotiation(
                exports->negotiate, loader_info, layer_name, layer_request,
                &result) || XR_FAILED(result)) {
            int ignored = 0;
            (void)set_active(exports->set_active, 0, &ignored);
            FreeLibrary(exports->module);
            return std::nullopt;
        }

        ::xrfg::set_optiscaler_embedded_delegation(true);
        return result;
    } catch (...) {
        if (activated) {
            int ignored = 0;
            (void)set_active(exports->set_active, 0, &ignored);
        }
        FreeLibrary(exports->module);
        return std::nullopt;
    }
}

}  // namespace optiscaler_bootstrap

constexpr char kLayerName[] = "XR_APILAYER_XRFrameBridge_diagnostic";
constexpr XrVersion kLayerApiVersion = XR_MAKE_VERSION(1, 0, 0);
constexpr XrDuration kGenerationCooldownDuration = 1'000'000'000;
// Consecutive submissions one scanout apart before any phase correction is
// allowed to run. Phase means nothing until the rate is right: a grid that is
// skipping slots has no stable phase to correct towards, and correcting one
// anyway drives a feedback loop - see the comments at both correction sites.
// Eight is a quarter of a second at 90 Hz.
constexpr std::uint32_t kPhaseCorrectionGridStreak = 8;
constexpr auto kStructuralQuarantineDuration = std::chrono::seconds(1);
// Application frames after which a session counts as established even though
// no XR_SESSION_STATE_VISIBLE was ever seen, so the delivery connection may
// open. The state transition is the real signal; this only covers a runtime
// that gives the layer no xrPollEvent to read it from, where waiting for a
// state that can never arrive would silently leave the process with no
// delivery interface and no measured pacing for its whole life. A hundred
// frames is about a second at 90 Hz, and the probe sessions this gate exists
// to exclude submit one.
constexpr std::uint32_t kEstablishedApplicationFrames = 100;

template <typename Handle>
[[nodiscard]] std::uint64_t handle_value(Handle handle) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept {
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&current_layer_directory),
                &module)) {
            return {};
        }
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return {};
        }
        return std::filesystem::path(path.data()).parent_path();
    } catch (...) {
        return {};
    }
}

[[nodiscard]] std::uint64_t optical_flow_configuration_code(
    xrfg::D3D12OpticalFlowBackend backend,
    const xrfg::D3D12NvidiaOpticalFlowOptions& options) noexcept {
    if (backend != xrfg::D3D12OpticalFlowBackend::nvidia) {
        return 0;
    }
    std::uint64_t code = 2;
    if (options.preset == xrfg::D3D12NvidiaPerformancePreset::slow) {
        code = 1;
    } else if (options.preset == xrfg::D3D12NvidiaPerformancePreset::fast) {
        code = 3;
    }
    std::uint64_t scale_code = 0;
    if (options.input_scale ==
        xrfg::D3D12NvidiaInputScale::three_quarter) {
        scale_code = 0x10000U;
    } else if (options.input_scale == xrfg::D3D12NvidiaInputScale::half) {
        scale_code = 0x20000U;
    } else if (options.input_scale == xrfg::D3D12NvidiaInputScale::quarter) {
        scale_code = 0x30000U;
    }
    return code | (options.bidirectional ? 0x100U : 0U) | scale_code;
}

// The runtime's Vulkan entry points, by the names the layer gives them. The
// enable2 variants share the signatures of the originals.
using PFN_GetVulkanExtensions = PFN_xrGetVulkanInstanceExtensionsKHR;
using PFN_CreateVulkanInstance = PFN_xrCreateVulkanInstanceKHR;
using PFN_CreateVulkanDevice = PFN_xrCreateVulkanDeviceKHR;
using PFN_GetVulkanGraphicsDevice = PFN_xrGetVulkanGraphicsDeviceKHR;
using PFN_GetVulkanGraphicsDevice2 = PFN_xrGetVulkanGraphicsDevice2KHR;
using PFN_GetVulkanGraphicsRequirements = PFN_xrGetVulkanGraphicsRequirementsKHR;

struct VulkanNegotiationDispatch {
    PFN_GetVulkanExtensions get_instance_extensions{};
    PFN_GetVulkanExtensions get_device_extensions{};
    PFN_CreateVulkanInstance create_instance{};
    PFN_CreateVulkanDevice create_device{};
    PFN_GetVulkanGraphicsDevice get_graphics_device{};
    PFN_GetVulkanGraphicsDevice2 get_graphics_device2{};
    PFN_GetVulkanGraphicsRequirements get_graphics_requirements{};
    PFN_GetVulkanGraphicsRequirements get_graphics_requirements2{};
};

// The Vulkan extensions an interop with D3D12 would need, as bits; the
// numbering is the one BridgeFlightOperation::vulkan_negotiation documents.
[[nodiscard]] std::uint64_t vulkan_interop_extension_bit(
    std::string_view name) noexcept {
    constexpr std::array<std::string_view, 11> kNames{
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore_win32",
        "VK_KHR_external_memory",
        "VK_KHR_external_semaphore",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_win32_keyed_mutex",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_get_physical_device_properties2",
    };
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (name == kNames[index]) {
            return 1ULL << index;
        }
    }
    return 0;
}

struct VulkanExtensionSummary {
    std::uint64_t count{};
    std::uint64_t bits{};
};

// The runtime's answer to xrGetVulkan*ExtensionsKHR: one space-separated
// string.
[[nodiscard]] VulkanExtensionSummary summarize_vulkan_extension_string(
    std::string_view list) noexcept {
    VulkanExtensionSummary summary{};
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t end = list.find(' ', start);
        const std::string_view name = list.substr(
            start,
            end == std::string_view::npos ? list.size() - start : end - start);
        if (!name.empty()) {
            ++summary.count;
            summary.bits |= vulkan_interop_extension_bit(name);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return summary;
}

[[nodiscard]] VulkanExtensionSummary summarize_vulkan_extension_array(
    const char* const* names,
    std::uint32_t count) noexcept {
    VulkanExtensionSummary summary{};
    if (names == nullptr) {
        return summary;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        if (names[index] != nullptr) {
            ++summary.count;
            summary.bits |= vulkan_interop_extension_bit(names[index]);
        }
    }
    return summary;
}

[[nodiscard]] std::uint64_t pack_negotiation_results(
    XrResult xr_result,
    VkResult vk_result) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(xr_result))
            << 32) |
        static_cast<std::uint32_t>(vk_result);
}

void log_vulkan_negotiation(
    std::int64_t selector,
    std::uint64_t a,
    std::uint64_t b = 0,
    std::uint64_t c = 0) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::vulkan_negotiation, selector, a, b, c);
}

// Whether the application's Vulkan device can take part in a D3D12 interop,
// asked of the device itself: with XR_KHR_vulkan_enable the application
// creates the device, so the list it was handed says what it was asked to
// enable, not what it did. A device returns no entry point for a command of
// an extension it did not enable.
//   11  a= commands the device returned: bit 0 vkGetMemoryWin32HandleKHR,
//       1 vkGetSemaphoreWin32HandleKHR, 2 vkImportSemaphoreWin32HandleKHR,
//       3 vkGetSemaphoreCounterValueKHR, 4 vkGetSemaphoreCounterValue
//   12  a= the same interop extension bits, for what the physical device
//       supports at all; b= how many device extensions it lists
void probe_vulkan_interop_support(
    const XrGraphicsBindingVulkanKHR& binding) noexcept {
    try {
        const HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
        if (vulkan == nullptr || binding.device == VK_NULL_HANDLE) {
            log_vulkan_negotiation(11, 0, 0, 1);
            return;
        }
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
        const auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
        if (get_device_proc != nullptr) {
            constexpr std::array<const char*, 5> kCommands{
                "vkGetMemoryWin32HandleKHR",
                "vkGetSemaphoreWin32HandleKHR",
                "vkImportSemaphoreWin32HandleKHR",
                "vkGetSemaphoreCounterValueKHR",
                "vkGetSemaphoreCounterValue",
            };
            std::uint64_t live = 0;
            for (std::size_t index = 0; index < kCommands.size(); ++index) {
                if (get_device_proc(binding.device, kCommands[index]) != nullptr) {
                    live |= 1ULL << index;
                }
            }
            log_vulkan_negotiation(11, live);
        }
        if (get_instance_proc != nullptr && binding.instance != VK_NULL_HANDLE &&
            binding.physicalDevice != VK_NULL_HANDLE) {
            const auto enumerate =
                reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    get_instance_proc(
                        binding.instance, "vkEnumerateDeviceExtensionProperties"));
            std::uint32_t extension_count = 0;
            if (enumerate != nullptr &&
                enumerate(binding.physicalDevice, nullptr, &extension_count,
                          nullptr) == VK_SUCCESS &&
                extension_count > 0) {
                std::vector<VkExtensionProperties> extensions(extension_count);
                if (enumerate(binding.physicalDevice, nullptr, &extension_count,
                              extensions.data()) >= VK_SUCCESS) {
                    std::uint64_t supported = 0;
                    for (std::uint32_t index = 0; index < extension_count; ++index) {
                        supported |= vulkan_interop_extension_bit(
                            extensions[index].extensionName);
                    }
                    log_vulkan_negotiation(12, supported, extension_count);
                }
            }
        }
    } catch (...) {
    }
}

struct Dispatch {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    // The D3D11 bridge (SessionState::d3d11_bridge): set when the ini asks
    // for it, the application enabled XR_KHR_D3D11_enable, and the layer
    // could add XR_KHR_D3D12_enable to the instance. The runtime's D3D12
    // requirements call has to be made before a D3D12 session is created.
    bool d3d11_bridge{};
    PFN_xrGetD3D12GraphicsRequirementsKHR get_d3d12_graphics_requirements{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images{};
    PFN_xrAcquireSwapchainImage acquire_swapchain_image{};
    PFN_xrWaitSwapchainImage wait_swapchain_image{};
    PFN_xrReleaseSwapchainImage release_swapchain_image{};
    // Best-effort: a runtime that does not expose it simply never has a space
    // destroyed underneath a queued submission through this layer.
    PFN_xrDestroySpace destroy_space{};
    // Best-effort, like destroy_space above. The layer does not act on events
    // and never consumes one; it records session state transitions so a
    // capture can say whether the runtime stopped asking for frames, and why.
    PFN_xrPollEvent poll_event{};
    // Best-effort, like the two above: the recorder's reprojection_angle
    // locates a VIEW space of the layer's own. Without them it is not written.
    PFN_xrCreateReferenceSpace create_reference_space{};
    PFN_xrLocateSpace locate_space{};
    // Best-effort, like the two above: forwarded unchanged unless a Vulkan
    // session was bridged to D3D12, where the runtime's DXGI list is shown
    // to the application as Vulkan formats.
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats{};
    // The Vulkan bridge (SessionState::vulkan_bridge): set when the ini asks
    // for it, the application enabled a Vulkan extension, and the layer
    // could add XR_KHR_D3D12_enable to the instance.
    bool vulkan_bridge{};
    // Vulkan negotiation, forwarded unchanged and only recorded: see
    // BridgeFlightOperation::vulkan_negotiation.
    VulkanNegotiationDispatch vulkan{};
    bool steamvr_runtime{};
    // Virtual Desktop's own runtime (VDXR). Its presenter is paced on the
    // steady_clock grid rather than the floor; see pace_presenter_submission.
    bool virtual_desktop_runtime{};
    // Virtual Desktop's runtime or Pimax Play's ("Pimax OpenXR"): the two
    // where a session's mode is fixed by the application's frame loop and by
    // nothing measured. A title that waits for its next frame inside the
    // current one takes the presenter, as on every runtime. On Virtual
    // Desktop a title whose frame loop runs on one thread takes it too, from
    // its first generating frame (frame_loop_takes_presenter). Every other
    // title stays inline for the whole session.
    //
    // They went by the bunched-pair detector before, and on these runtimes
    // its evidence was an accident of the session: the same title produced
    // the thirty pairs at a stall 46 seconds in on one run, in the first half
    // second after a resume on another, and never on a third. And the
    // presenter it switched to was not the better mode there. No Man's Sky at
    // 144 Hz on the presenter ran the GPU out of room in a heavy scene -
    // synthetics held three periods for their pixels, fifteen continuity
    // resets a second, a quarter of the application's frames without a
    // synthetic - where inline, which holds the application's thread and so
    // its rate, gave a steady 55 and a synthetic for every one of them.
    bool inline_unless_pipelined{};
    XrVersion runtime_version{};
    std::string runtime_name;
    // The status panel's flip gesture: an action set of the layer's own with
    // one pose action on both grips, added to the application's input (see
    // layer_suggest_interaction_profile_bindings). Made at xrCreateInstance
    // when `[overlay] panel` is gesture. Without it none of the three input
    // calls is intercepted.
    struct PanelGesture {
        PFN_xrSuggestInteractionProfileBindings suggest_bindings{};
        PFN_xrAttachSessionActionSets attach_action_sets{};
        PFN_xrSyncActions sync_actions{};
        PFN_xrCreateActionSpace create_action_space{};
        XrActionSet action_set{XR_NULL_HANDLE};
        XrAction grip_action{XR_NULL_HANDLE};
        std::array<XrPath, 2> hands{XR_NULL_PATH, XR_NULL_PATH};
        std::array<XrPath, 2> grips{XR_NULL_PATH, XR_NULL_PATH};
    } panel_gesture;
};

[[nodiscard]] std::uint64_t runtime_name_hash(
    std::string_view name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : name) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct GeneratedFrameEndInfo;

struct PresenterSubmission {
    std::uint64_t sequence{};
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame;
    const XrFrameEndInfo* borrowed_frame{};
    XrResult result{XR_SUCCESS};
    bool completed{};
    // When the application handed it over. The deeper pipeline holds a
    // synthetic until it has been queued for a whole display period.
    std::chrono::steady_clock::time_point queued_at{};
    // app_release_counter at the application's xrEndFrame for this frame;
    // see join_runtime_queue_to_application.
    std::uint64_t app_release_value{};
};

struct ProjectionLayerSnapshot {
    std::uint32_t layer_index{};
    XrCompositionLayerFlags layer_flags{};
    XrSpace space{XR_NULL_HANDLE};
    std::vector<XrCompositionLayerProjectionView> views;
};

struct ProjectionSnapshot {
    XrTime display_time{};
    XrEnvironmentBlendMode environment_blend_mode{};
    std::vector<ProjectionLayerSnapshot> layers;
};

struct ProjectionViewReference {
    std::size_t projection_index{};
    std::size_t view_index{};
};

struct ProjectionResourceMapping {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    // Canonical logical views for this resource. Ordinarily there is one per
    // array slice; UEVR Native Stereo may instead submit two non-overlapping
    // eye viewports in one physical slice.
    std::vector<ProjectionViewReference> views;
};

enum class ProjectionMappingReason : std::int64_t {
    ready = 0,
    no_projection_views = 1,
    unknown_swapchain = 2,
    unsupported_array_size = 3,
    zero_resource_extent = 4,
    array_slice_out_of_range = 5,
    negative_subimage_offset = 6,
    nonpositive_subimage_extent = 7,
    subimage_out_of_bounds = 8,
    missing_array_slice = 9,
    exception = 10,
    unsupported_view_layout = 11,
};

struct ProjectionMappingResult {
    std::vector<ProjectionResourceMapping> mappings;
    ProjectionMappingReason reason{ProjectionMappingReason::exception};
    std::uint64_t detail{};

    [[nodiscard]] bool ready() const noexcept {
        return reason == ProjectionMappingReason::ready && !mappings.empty();
    }
};

enum class SessionGraphicsBinding : std::int64_t {
    none = 0,
    d3d11 = 1,
    d3d12 = 2,
    vulkan = 3,
    opengl = 4,
};

struct PendingApplicationFrame {
    XrTime display_time{};
    XrDuration display_period{};
};

enum class GenerationQuarantineReason : std::int64_t {
    swapchain_created = 1,
    swapchain_destroyed = 2,
    d3d11_images_changed = 3,
    d3d12_images_changed = 4,
    projection_changed = 6,
    projection_mapping_failed = 7,
    generation_prepare_failed = 8,
    generated_end_info_failed = 9,
    presenter_composition_failed = 10,
    downstream_end_failed = 11,
    vulkan_images_changed = 12,
};

// Windows 10 1803 and later. Declared here so the layer still builds against
// an SDK that predates it; the create call degrades to a coarse timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Diagnostic only, and only with logging on. Measures when the application's
// own queue gets past the join a hand-over queues on it.
//
// On a native D3D12 session the runtime orders its use of a private image
// against the application's queue, so an image is only ready, as the
// compositor sees it, once that queue has executed the join - which sits
// behind whatever the game had already queued. The synthesis fence says when
// the pixels exist; nothing said when the runtime could see them. The
// presenter signals `fence` on the application's queue straight after the
// join and records it (913); this thread timestamps each value as it
// completes (914). Values complete in order on one queue, so it waits for
// them one at a time.
struct JoinProbe {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t signaled{};
    bool stop{};

    JoinProbe() = default;
    JoinProbe(const JoinProbe&) = delete;
    JoinProbe& operator=(const JoinProbe&) = delete;

    ~JoinProbe() {
        {
            std::scoped_lock lock(mutex);
            stop = true;
        }
        condition.notify_all();
        if (event != nullptr) {
            SetEvent(event);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (event != nullptr) {
            CloseHandle(event);
        }
    }

    void run() noexcept {
        try {
            std::uint64_t waited = 0;
            for (;;) {
                std::uint64_t target = 0;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(
                        lock, [&] { return stop || signaled > waited; });
                    if (stop) {
                        return;
                    }
                    target = waited + 1;
                }
                std::uint64_t completed = fence->GetCompletedValue();
                if (completed == std::numeric_limits<std::uint64_t>::max()) {
                    return; // Device removed: nothing further will complete.
                }
                if (completed < target) {
                    if (FAILED(fence->SetEventOnCompletion(target, event))) {
                        return;
                    }
                    WaitForSingleObject(event, INFINITE);
                    {
                        std::scoped_lock lock(mutex);
                        if (stop) {
                            return;
                        }
                    }
                    completed = fence->GetCompletedValue();
                    if (completed == std::numeric_limits<std::uint64_t>::max()) {
                        return;
                    }
                    if (completed < target) {
                        continue; // A stale wake; re-arm for the same value.
                    }
                }
                // Several may have completed together; stamp each.
                for (std::uint64_t value = target; value <= completed; ++value) {
                    {
                        std::scoped_lock lock(mutex);
                        if (value > signaled) {
                            break;
                        }
                    }
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_vsync_lock,
                        914,
                        value,
                        0,
                        0);
                    waited = value;
                }
            }
        } catch (...) {
        }
    }
};

struct SessionState {
    explicit SessionState(std::shared_ptr<Dispatch> next_dispatch)
        : dispatch(std::move(next_dispatch)), manual_control(current_layer_directory()) {}

    ~SessionState() {
        xrfg::embedded::detach(control_id);
        if (presenter_pace_timer != nullptr) {
            CloseHandle(presenter_pace_timer);
        }
        if (application_release_timer != nullptr) {
            CloseHandle(application_release_timer);
        }
    }

    SessionState(const SessionState&) = delete;
    SessionState& operator=(const SessionState&) = delete;

    std::shared_ptr<Dispatch> dispatch;
    xrfg::implicit_layer::ManualArmControl manual_control;
    std::uint64_t control_id{xrfg::embedded::attach()};
    std::uint64_t control_revision{}; // frame_call_mutex
    bool control_reconfigure_required{};
    // The tray's pause as last applied by apply_embedded_control, which
    // folds it into menu_enabled.
    bool pause_applied{}; // frame_call_mutex
    // Whether the flight recorder was writing when apply_embedded_control
    // last ran. A change rebuilds each synthesizer with or without its GPU
    // timing, so a log switched on in a running session has the timings.
    bool recorder_applied{}; // frame_call_mutex
    // The pause took a presenter this session had earned. The resume asks
    // for it back instead of waiting for the runtime to show the evidence
    // again: on Virtual Desktop that evidence was a transient, and a resumed
    // No Man's Sky stayed inline at 112 frames a second where it had held
    // 144 before the pause.
    bool presenter_restore_after_pause{}; // frame_call_mutex
    std::atomic<bool> menu_enabled{true};
    std::atomic<bool> generation_steady_state_established{false};
    bool manual_stop_applied{}; // frame_call_mutex; terminal for this XrSession.
    std::unique_ptr<xrfg::OpenXrFpsOverlay> fps_overlay;
    // The status panel. Grip spaces of the layer's own action, made when the
    // application attached its action sets with the layer's beside them and
    // destroyed after the overlay; whether that attach took ours, which every
    // xrSyncActions then follows.
    std::array<XrSpace, 2> panel_grip_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::atomic<bool> panel_actions_attached{false};
    // What the panel says that nothing else keeps. Why the last frame passed
    // through (a GenerationPrepareReason) and when; whether the depth or 3X
    // fell back for the runtime's swapchain limit; where the DLSS guide
    // counters stood at the panel's last refresh; and NGX's frame count,
    // asked once. The application's xrEndFrame thread only.
    std::int64_t panel_bypass_reason{};
    std::chrono::steady_clock::time_point panel_bypass_at{};
    bool shallow_fallback{};
    xrfg::DlssMotionVectorStatistics panel_vectors{};
    std::chrono::steady_clock::time_point panel_vectors_at{};
    std::optional<std::uint32_t> panel_native_frames;
    // Each synthesizer's GPU time a pair, smoothed, and when it was last
    // measured: there is one per eye in a game with a swapchain per eye, and
    // a pair costs them all. Only while the flight recorder runs, which is
    // when synthesis is timed. gpu_mutex.
    struct PanelGpuTime {
        float microseconds{};
        std::chrono::steady_clock::time_point at{};
    };
    std::unordered_map<const void*, PanelGpuTime> panel_gpu;
    // Owned here rather than by the overlay because the presenter reads the
    // vsync anchor from the same connection. Null off SteamVR.
    std::unique_ptr<xrfg::SteamVrDelivery> steamvr_delivery;
    // The D3D11 bridge. The application bound D3D11; the runtime was handed
    // a D3D12 session on the layer's own device, so from here on the session
    // is a D3D12 one (graphics_binding, d3d12_device, d3d12_queue) and
    // d3d11_device stays null: no interop, no runtime-entry gate, no D3D11
    // work by the runtime on any thread. The application's device lives
    // here, for the per-swapchain shared textures it renders into
    // (D3D11BridgeSwapchain). Why: the NVIDIA D3D11 driver faults when the
    // runtime works the game's device from the presenter thread, and the
    // inline alternative is throttled by SteamVR; a runtime that only ever
    // sees D3D12 has neither problem. Capability bit 128.
    struct D3D11Bridge {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    };
    std::unique_ptr<D3D11Bridge> d3d11_bridge;
    // The Vulkan bridge: the application bound Vulkan; the runtime was
    // handed a D3D12 session on the layer's own device, so from here on the
    // session is a D3D12 one (graphics_binding, d3d12_device, d3d12_queue)
    // and every path downstream is the native D3D12 one. The application
    // renders into Vulkan imports of the layer's shared textures
    // (xrfg::VulkanBridgeSwapchain), on its own queue, which only its own
    // thread drives. Why: the Vulkan interop mirrors every image three
    // times over, 24 copies per swapchain measured in No Man's Sky.
    struct VulkanBridge {
        xrfg::VulkanSessionBinding binding;
    };
    std::unique_ptr<VulkanBridge> vulkan_bridge;
    // Set once a bridged swapchain took D3D11BridgePath::depth_private, so
    // xrEndFrame knows to look for depth information naming one. Never
    // cleared: a strip on a frame that names none is a cheap walk.
    std::atomic<bool> has_private_depth_swapchain{false};
    // Application frames submitted on this session, counted only until the
    // delivery connection is released. Guarded by frame_call_mutex.
    std::uint32_t application_frames_submitted{};
    // The offset from a real vsync that the schedule is held at. Learned from
    // wherever the existing servo had settled when the first anchor arrived,
    // never chosen: the lock's job is to stop the grid drifting away from the
    // scanout, not to decide where on the scanout it belongs. Guarded by
    // presenter_mutex.
    std::chrono::nanoseconds presenter_vsync_offset{};
    bool presenter_vsync_offset_valid{};
    // How far the last submission actually landed from the phase being held,
    // signed and taken the short way round. Written and read only by the
    // presenter thread, between measuring it and applying it a few lines later.
    std::chrono::nanoseconds presenter_landed_error{};
    // Real frames left before the acquisition step may take a whole scanout
    // in one step again, and whether the step just taken is still to be
    // held: the pace caps a hold at one period, which would drain a jump at
    // about a millisecond a cycle - the walk under another name, measured as
    // exactly that - so the cycle after a jump may hold two. Cooldown is
    // presenter thread only; the flag is guarded by presenter_mutex.
    std::uint32_t presenter_scanout_jump_cooldown{};
    bool presenter_scanout_jump_pending{};
    // How much of the compositor's frame to leave in hand when a submission
    // lands. Fixed for the session and presenter thread only.
    //
    // This is clearance, not a deadline. The two halves of a pair go out one
    // scanout apart into compositor frames one scanout apart, so the margin is
    // the whole of the room they have: a pair that compresses by more than
    // this lands both halves inside one compositor frame, and the compositor
    // keeps only the later one. That is a cliff, not a slope - jitter under
    // the margin costs nothing at all and jitter over it costs a whole frame -
    // which is why the failure arrives suddenly rather than degrading.
    //
    // 3 ms because the runtime's own xrEndFrame is what injects that jitter,
    // and it was measured at p99 2.7 ms while delivery held 90 and p99 6.5 ms
    // while it fell to 40. The upper bound is the compositor's running start,
    // past which a submission is taken for the frame already being assembled
    // and about half are never scanned out; that bound was measured between
    // 4.0 and 5.0 ms across three titles on one machine, so 3 ms buys
    // clearance without approaching it.
    std::chrono::nanoseconds presenter_submit_margin{3'000'000};
    std::uint32_t presenter_vsync_tick{};
    // Serializes each generated synthetic/real frame pair atomically with
    // respect to application frame calls. A successful application wait owns
    // the next admission until its matching begin has been attempted, so a
    // pipelined second wait cannot overtake that begin and deadlock the runtime.
    std::mutex frame_call_mutex;
    std::condition_variable frame_call_condition;
    bool application_wait_pending_begin{};
    // Some applications run the next wait on a dedicated thread before the
    // current render thread ends its frame. Two consecutive overlaps select a
    // virtual application loop; the runtime-facing presenter then owns all
    // later physical frame cycles.
    bool application_frame_in_progress{};
    bool application_frame_has_overlapping_wait{};
    // A forwarded application xrWaitFrame has returned and the matching
    // xrBeginFrame has not reached the runtime. The runtime holds that frame
    // open and blocks every further xrWaitFrame until it is begun - SteamVR
    // did exactly that - so while this is set the layer must not issue a wait
    // of its own: not the inline second cycle, and not a presenter's first.
    // Only an inline session forwards waits; under the presenter every
    // application wait is virtual and this stays clear.
    bool runtime_frame_waited_unbegun{};
    // The inline second cycle took the application's held runtime frame for
    // itself (see submit_current_cycle's adopted frame), so the application's
    // next xrBeginFrame has to take a fresh xrWaitFrame from the runtime
    // first. Sessions that never take a presenter pipeline this way instead
    // of passing the frame through: DCS World issues its next wait before
    // the render thread begins, on every frame, and passing those through
    // meant generating nothing.
    bool application_begin_needs_wait{};
    std::uint32_t pipelined_wait_streak{};
    bool pipelined_presenter_mode{};
    bool pipelined_presenter_start_requested{};
    bool steamvr_presenter_start_requested{};
    // The application's D3D11 device was created
    // D3D11_CREATE_DEVICE_SINGLETHREADED - Unity's default. D3D11 then does
    // no locking of its own, and the runtime uses that device inside the
    // frame calls: SteamVR flushes the application's immediate context in
    // them. Made from a presenter thread, those calls race the application's
    // render thread on the device and deadlock inside the driver (seen on The
    // Forest through OpenComposite: the presenter in a SteamVR frame call
    // flushing the context while Unity's render thread sat in CreateBuffer,
    // both waiting on the same driver lock). The multithread protection the
    // interop turns on covers the immediate context, not the device, so it
    // cannot help. Such a session never promotes a presenter: every runtime
    // call stays on the application's own thread, and frames the application
    // pipelines pass through ungenerated. Fixed for the session. A Vulkan
    // session is the same case by construction; see presenter_forbidden.
    bool single_threaded_d3d11{};
    // The application's frame loop runs on two threads: xrWaitFrame has been
    // seen on a thread other than the one that calls xrEndFrame. Latched at
    // the first such wait, under frame_call_mutex. DCS World's shape. On a
    // D3D11 session it forbids the presenter; see presenter_forbidden.
    bool split_frame_loop{};
    DWORD application_end_thread_id{};
    XrFrameState last_inline_frame_state{XR_TYPE_FRAME_STATE};
    bool last_inline_frame_state_valid{};
    std::mutex mutex;
    std::mutex gpu_mutex;
    std::deque<PendingApplicationFrame> pending_frames;
    std::optional<ProjectionSnapshot> previous_projection;
    XrTime generation_resume_display_time{};
    std::chrono::steady_clock::time_point generation_resume_wall_time{};
    XrDuration minimum_runtime_display_period{};
    // How long the runtime's own xrWaitFrame blocked, and how many consecutive
    // waits came back too quickly to have been pacing anything. This is what
    // the presenter's existence actually turns on; see
    // runtime_wait_lacks_pacing.
    std::chrono::steady_clock::duration last_application_wait_elapsed{};
    std::uint32_t unpaced_wait_streak{};
    // Consecutive pairs whose two frames left the layer close enough together
    // to land in one scanout window - see inline_pair_lands_in_one_scanout.
    std::uint32_t bunched_pair_streak{};
    std::uint32_t steamvr_throttled_wait_streak{};
    // Consecutive application frames the layer could not generate from. A
    // single one says nothing - the frame passes through and the next one
    // usually pairs - so the presenter is only demoted once they run together.
    std::uint32_t generation_failure_streak{};
    xrfg::D3D12OpticalFlowBackend optical_flow_backend{
        xrfg::D3D12OpticalFlowBackend::fidelity_fx};
    // NVIDIA optical flow failed to initialise on this session - no
    // nvofapi64.dll on an AMD or Intel machine, or an NVIDIA GPU without the
    // optical-flow hardware - and FidelityFX took over. Latched: later
    // swapchains and later menu changes to NVIDIA go straight to FidelityFX
    // instead of loading the DLL again. The tray is not told; the flight
    // recorder is (synthesis_initialize, phase I).
    bool nvidia_backend_unavailable{};
    // Fixed for the session. A depth that engaged on load would re-phase the
    // whole pipeline whenever the scene got heavy, costing a repeated frame
    // each time, and where it settled would depend on what the player did.
    // The one change allowed is downward, while arming, before anything has
    // been generated: see fall_back_to_shallow_pipeline.
    bool deep_pipeline{};
    // How many frames the runtime is handed for each application frame: two,
    // one synthetic and the real one, or three with "3X Frame Gen", where a
    // second synthetic fills the extra display period of an application
    // running at a third of the display rate. The application's virtual
    // period, the once-per-frame hold, the synthetic ring and the
    // interpolation instants all follow it. Three always runs the shallow
    // pipeline. Not through the D3D11 interop, whose publish carries one
    // synthetic; the Vulkan interop's carries both.
    //
    // It follows the tray's switch while the session runs
    // (apply_live_frame_multiplier), which is the one thing allowed to move
    // the depth mid-session: the user asked for it, and it is done on an
    // empty presenter queue, so the pipeline re-phases once, on a frame
    // that primes. Atomic because the application's wait may run on another
    // thread than the xrEndFrame that changes it.
    std::atomic<std::uint32_t> frames_per_application_frame{2};
    // The depth the ini asked for, which is what a session goes back to
    // when 3X is switched off.
    bool deep_pipeline_configured{};
    // The synthetic ring has two slots - the session started in the deeper
    // pipeline or in 3X - which both of those need and which is what lets
    // the session move between them without making a swapchain. Cleared
    // with the ring by fall_back_to_shallow_pipeline.
    bool two_slot_synthetic_ring{};
    // `[ofxr] dlss_flow_hybrid` and `[ofxr] extrapolate`, read at
    // xrCreateSession, the hybrid again at each control change; see
    // nvidia_options_for.
    bool dlss_flow_hybrid{};
    // Whether the process has published DLSS vectors. The hybrid waits for
    // them: until a game shows it has DLSS, the chosen engine runs as in any
    // game without, and the first publication takes the hybrid up through
    // apply_embedded_control as a settings change would.
    bool dlss_vectors_published{};
    // Extrapolation shows the real frame first and the synthetics after it,
    // each predicted from it, instead of interpolating before it. 2 also runs
    // FidelityFX's flow beside the game's vectors.
    int extrapolate{};
    // Extrapolation with Meta's mesh warps rather than the gather.
    bool extrapolate_mesh{true};
    // `[ofxr] synthetic_pose`: each synthetic is generated in and submitted
    // with the head's pose at the instant it is shown, rather than the newer
    // real frame's (synthetic_camera_snapshot). The application's xrEndFrame
    // thread reads it; it is set at xrCreateSession, at control changes and
    // by follow_synthetic_pose, which run on that thread too. Atomic for the
    // presenter's reprojection_angle record, which only reports it.
    std::atomic<bool> synthetic_pose_interpolated{true};
    std::chrono::steady_clock::time_point synthetic_pose_poll_at{};
    // The recorder's reprojection_angle: a VIEW space of the layer's own,
    // made the first time a frame is handed over while the recorder runs, by
    // the presenter or the application's thread, whichever hands frames over
    // then; destroyed with the session.
    std::mutex reprojection_space_mutex;
    XrSpace reprojection_view_space{XR_NULL_HANDLE}; // reprojection_space_mutex
    bool reprojection_space_attempted{}; // reprojection_space_mutex
    // One private swapchain per output with staging textures, where the
    // synthesizer writes D3D12 images directly; see kStagingSlotCount.
    // Read at xrCreateSession from `[ofxr] single_swapchain_rings`.
    bool single_swapchain_rings{};
    // Native D3D12 only: the history capture of a released application image
    // is queued at the application's xrEndFrame, not at the release. See
    // SwapchainState::pending_end_frame_capture. Read at xrCreateSession
    // from `[ofxr] capture_at_end_frame`.
    bool capture_at_end_frame{};
    // The switch can be followed live: a binding 3X covers, and a ring it
    // fits in.
    bool triple_switchable{};
    // The application's xrEndFrame thread only.
    std::chrono::steady_clock::time_point triple_poll_at{};
    // The application's xrEndFrame thread only: the run of pairs that waited
    // at admission observe_admission_wait counts, how many runs long it must
    // be, and the presenter frame it adds to the next once-per-pair hold.
    // Tests add XRFG_TEST_ADMISSION_WAIT_MS to every wait it measures.
    std::uint32_t admission_late_run{};
    std::uint32_t rephase_backoff{1};
    bool rephase_check_due{};
    std::uint32_t pair_hold_extra_frames{};
    std::chrono::nanoseconds test_admission_wait{};
    // The flight log's vram_usage records. The adapter is found from the
    // first device the session has; what it reports is the process's use of
    // that adapter, whichever device made the allocation.
    std::mutex video_memory_mutex;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> video_memory_adapter; // video_memory_mutex
    bool video_memory_unavailable{}; // video_memory_mutex
    // The next periodic record. The application's xrEndFrame thread only.
    std::chrono::steady_clock::time_point video_memory_poll_at{};
    std::chrono::steady_clock::time_point video_memory_first_poll{};
    // projection_view_rect: the last rectangle recorded per swapchain, so
    // the record is written on change only. The application's xrEndFrame
    // thread only.
    std::unordered_map<XrSwapchain, std::array<std::uint64_t, 3>> logged_view_rects;
    // Held while this session cannot follow the switch, so the tray can say
    // a restart is needed.
    xrfg::implicit_layer::FixedFrameMultiplierMarker fixed_frame_multiplier;
    // `[ofxr] vulkan_bridge`: off, a Vulkan session passes through. See
    // implicit_layer::read_vulkan_support for why it is a choice.
    bool vulkan_support{};
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options{};
    bool dlss_motion_vectors{};
    // For a game with a swapchain per eye, how many DLSS evaluations had been
    // published when each eye's image was last released (left, right): an
    // eye's evaluation comes before its release (resolve_dlss_motion_vectors).
    std::array<std::atomic<std::uint64_t>, 2> eye_release_publication{};
    SessionGraphicsBinding graphics_binding{SessionGraphicsBinding::none};
    std::uint64_t graphics_binding_capabilities{};
    // Set once the runtime has refused a private swapchain. A runtime caps how
    // many swapchains one session may hold at all -- SteamVR hands out 16 --
    // and the application is usually still creating its own when the layer
    // reaches that ceiling, so the private swapchains already taken are what
    // makes the application's next creation fail. Hand the whole budget back
    // and pass frames through for the rest of the session: an application that
    // cannot finish creating its swapchains has no way to recover, while one
    // that merely loses generation carries on.
    std::atomic<bool> generation_budget_exhausted{false};
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    // The application's Vulkan session, for the interop; the queue is the
    // application's own, which is why a Vulkan session never takes a
    // presenter (presenter_forbidden).
    xrfg::VulkanSessionBinding vulkan_binding;
    // Serialises this layer's entries into the runtime for as long as the
    // application's binding is D3D11. See RuntimeEntry below for why the gate
    // is here rather than on the device. Recursive because the frame path
    // already nests these calls inside one another on one thread.
    std::recursive_mutex runtime_entry_mutex;
    std::uint64_t runtime_entry_count{};       // runtime_entry_mutex
    std::uint64_t runtime_entry_contended{};   // runtime_entry_mutex
    std::uint64_t runtime_entry_wait_ns{};     // runtime_entry_mutex
    std::uint64_t runtime_entry_wait_max_ns{}; // runtime_entry_mutex
    std::uint64_t runtime_entry_hold_max_ns{}; // runtime_entry_mutex
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_queue;
    // Synthesis runs here rather than on the application's queue. The work
    // itself is short - the pack and composite command lists measure 249 us
    // per swapchain against MSFS 2024, and the 1610 us of optical flow runs
    // on the OFA engine rather than on any D3D12 queue - but the queue waits
    // on the OFA fence between them, and submitting that on the application's
    // queue blocks everything the application has already queued behind it,
    // which is its next frame's rendering. On a queue of its own the wait
    // holds only this. Null when the application binds D3D11, where
    // d3d12_queue is already the layer's own bridge queue.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_synthesis_queue;
    // The queue the runtime is handed at session creation, in place of the
    // application's, on a native D3D12 session. Null where that could not be
    // arranged, and then d3d12_queue is what the runtime has.
    //
    // A D3D12 runtime judges a frame complete by the work queued on the
    // binding queue up to the xrEndFrame that names it: it passes that queue
    // to the compositor with each texture, and the compositor waits for the
    // queue to reach the point of submission. Handed the application's queue,
    // that point sat behind whatever the game had queued since it was last
    // released - its whole next frame, in the deeper pipeline - and one half
    // of every pair became visible to the compositor only when that frame
    // had rendered. Measured on Hogwarts Legacy at 8344x3268: the real
    // frame's mark cleared 7.6 ms after its hand-over (p50) and past the
    // compositor's render start on 88% of the frames the compositor never
    // showed, against 4% of the ones it did.
    //
    // With a queue of the layer's own, the only work ahead of a submission
    // is what the layer put there: a wait for the synthesis that wrote a
    // private image, and a wait for the application's release of any image
    // of its own that the frame names. The game's own rendering is never in
    // front of it, and the join before a private image's release no longer
    // holds the game's next frame either.
    //
    // Two joins keep the runtime's ordering intact across the two queues,
    // both GPU-side. At each of the application's releases, its queue
    // signals app_release_fence; before each downstream xrEndFrame the
    // binding queue waits for the value that stood at the application's own
    // xrEndFrame, which every image the frame names was released before.
    // It does not wait at the release itself: that would put the game's
    // next frame back in front of the next hand-over. After each of the
    // application's xrWaitSwapchainImage calls, the binding queue signals
    // reacquire_fence and the application's queue waits for it, so whatever
    // the runtime queued there to keep the image safe to write still holds
    // the application's writes.
    //
    // SteamVR only. Virtual Desktop measured a slight loss with it and has
    // the application's queue back. Pimax's runtime measured no difference
    // either way - MSFS 2024 at 72 Hz, 1.44 two-period waits a second on the
    // application's queue against 1.43 on the layer's - so it keeps the
    // application's queue as well.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> binding_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> app_release_fence;
    Microsoft::WRL::ComPtr<ID3D12Fence> reacquire_fence;
    std::mutex binding_join_mutex;
    std::uint64_t app_release_counter{};   // binding_join_mutex
    std::uint64_t reacquire_counter{};     // binding_join_mutex
    // app_release_counter as it stood when the application entered its
    // current xrEndFrame. frame_call_mutex.
    std::uint64_t app_end_frame_release_value{};
    // Some SteamVR configurations throttle the inline second wait/begin/end
    // cycle to the application's half-rate interval. After that behavior is
    // measured, the dedicated presenter becomes the sole owner of downstream
    // calls while the application observes a virtual half-rate loop.
    std::mutex presenter_mutex;
    std::condition_variable presenter_condition;
    std::mutex presenter_content_mutex;
    std::deque<std::shared_ptr<PresenterSubmission>> presenter_submissions;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_last_frame;
    std::thread presenter_thread;
    // When the presenter last handed a frame to the runtime, and the period it
    // was told to expect. The presenter paces itself against these because
    // xrWaitFrame cannot be relied on to do it: see pace_presenter_submission.
    // Guarded by presenter_mutex.
    // When the next submission is due. Advanced by exactly one period per
    // submission rather than measured from the previous one, so the loop's own
    // cost lands as a constant offset instead of accumulating into the
    // cadence. Chaining each wait off the last submission added about 4.6 ms
    // of per-cycle overhead to a 10 ms pace and held the runtime to 64/s.
    std::chrono::steady_clock::time_point presenter_next_submit{};
    // The best phase this presenter has managed lately: predictedDisplayTime
    // minus the moment the frame aimed at it was actually submitted, reduced
    // modulo the display period. The two clocks have different epochs, so the
    // value is meaningless on its own and exact as a comparison - which is
    // all the phase needs.
    std::int64_t presenter_lead_reference{};
    bool presenter_lead_valid{};
    // When the previous submission actually went out, so the phase controller
    // can tell whether a lead was achieved on the grid or by overshooting it.
    std::chrono::steady_clock::time_point presenter_last_submitted_at{};
    // Consecutive submissions that landed one scanout apart. Phase only means
    // anything once the rate is right, so the correction waits for a run of
    // them - see the comment at the controller.
    std::uint32_t presenter_on_grid_streak{};
    // What each half of the pair costs downstream, smoothed. The synthetic's
    // xrEndFrame costs more than the real frame's exactly when the runtime is
    // blocking on pixels that are not finished, so the difference between them
    // is that block. The real frame is the baseline rather than a constant
    // because it absorbs whatever the runtime charges per submission
    // regardless - 0.65 to 0.81 ms across every capture here, and different
    // elsewhere. Diagnostic: nothing schedules on these.
    std::chrono::nanoseconds presenter_synthetic_call_mean{};
    std::chrono::nanoseconds presenter_real_call_mean{};
    // Reported every kCallReportFrames submissions as presenter_transition
    // 301, with the pair gaps below.
    std::uint32_t presenter_call_report_tick{};
    // The interval between the two hand-overs of a pair, measured where it
    // matters - between the calls, so it already carries whatever the runtime
    // spent inside the synthetic's. One display period when the spacing is
    // right. Diagnostic: nothing schedules on this either. An earlier build
    // held a floor under it, on figures that did not survive being checked
    // against what the compositor actually scanned out.
    std::chrono::steady_clock::time_point presenter_synthetic_returned_at{};
    std::chrono::nanoseconds presenter_pair_gap_mean{};
    // The interval on the *other* side of the synthetic: from the real frame's
    // hand-over to the synthetic's. This is the one that decides whether the
    // synthetic gets a scanout at all - measured over 1146 of them, the ones
    // the compositor presented arrived a median 11.032 ms after the previous
    // frame and the ones it dropped 9.568 ms, with the dropped set's p90 at
    // 10.267, below one period almost without exception. 99.9% of the dropped
    // ones followed a real frame that had been presented: two frames inside one
    // scanout, and the second one loses.
    std::chrono::steady_clock::time_point presenter_real_returned_at{};
    std::chrono::nanoseconds presenter_pair_lead_gap_mean{};
    bool presenter_schedule_valid{};
    // The display time the runtime reported for the previous internal
    // xrWaitFrame, and how many slots the pace has been asked to give back.
    //
    // The schedule above is a steady_clock grid. The compositor scans out on
    // the display's clock, and nothing related the two: the phase between
    // them was whatever it happened to be when the presenter thread started,
    // and no path corrected it. A grid sitting just after the compositor's
    // deadline makes every submission miss the scanout it was built for and
    // land in the next, so each scanout sees either nothing new or two
    // frames - continuously, not occasionally, and latched for the life of
    // the session, which is why leaving and re-entering changed everything.
    //
    // predictedDisplayTime is the runtime naming the scanout each frame is
    // for, so consecutive waits advancing by exactly one period is the lock
    // condition, and the delta is the error. Guarded by presenter_mutex.
    XrTime presenter_last_predicted_display{};
    bool presenter_last_predicted_valid{};
    // When the presenter's previous xrEndFrame returned.
    //
    // On a runtime other than SteamVR the steady_clock grid is not held at
    // all: such a runtime paces the presenter with its own xrWaitFrame, and
    // its return is the deadline signal. Holding a grid on top of it put a
    // second clock that knew nothing of the runtime's deadline between that
    // return and xrEndFrame. Measured on Pimax OpenXR, MSFS 2024, 72 Hz: the
    // wait blocked a median 3.7 ms and the grid then held 9.5 ms, the
    // runtime's next wait blocked two periods, the grid counted itself behind
    // and submitted back to back - 8.5% of scanouts missed and 6.4% doubled,
    // where the same title on SteamVR had 0.6% and none. Without the hold,
    // 0.8% and 0.1%.
    //
    // What is kept is a floor of half a period between hand-overs, measured
    // from this timestamp. A floor from the previous call cannot drift the way
    // a grid could, and in the paced steady state, where hand-overs are a
    // period apart, it never fires.
    //
    // Virtual Desktop keeps the grid instead. It returns the wait instantly
    // while the application is behind, so once the application's frame takes
    // longer than a period after its release nothing paces the presenter: the
    // application is released from the presenter's progress, the presenter
    // waits on the application, and the loop clocks itself at frame time plus
    // the floor. Measured at 90 and 100 Hz on two machines: the real frame went
    // out 6.5 ms after its synthetic on 81-90% of pairs, inside the
    // synthetic's scanout, and the application fell from 45 to 42 a second,
    // against an even 11 ms cadence and a steady 45 under the grid. The floor
    // alone did not help at 144 Hz either - 12% of submissions bunched, 1-2%
    // under the grid. Pimax's two-clock stacking needs a wait that blocks
    // every frame, which Virtual Desktop's does not. Presenter thread only.
    std::chrono::steady_clock::time_point presenter_last_end_returned_at{};
    // 3X only: how long the application is kept, after its frame is handed
    // over, before it is released to render the next one.
    //
    // Released at the previous real frame's hand-over, an application that
    // needs a third of its 3-period cycle finishes two periods before its
    // frames can start to be shown: the previous frame's two synthetics and
    // its real frame are still in front. Measured at 90 Hz on The Callisto
    // Protocol and MSFS 2024, the application entered xrEndFrame 10-13 ms
    // after its release and its real frame went out 53-57 ms after that,
    // against 31 ms for a pair in the deeper pipeline. Everything it waited
    // was latency: its poses and its input were that much older when shown.
    //
    // The delay is found, not computed, because what bounds it is when the
    // GPU finishes the frame and its synthesis, which the layer cannot see
    // from the application's call. What it can see is whether the next
    // frame's first synthetic is queued and written a set margin before it
    // is handed over.
    //
    // On SteamVR the margin is kTripleReadinessMargin, and the presenter
    // looks that long before the hand-over, inside the pace's own hold
    // (pace_presenter_submission). Synthesis is a few milliseconds of GPU
    // time; asking for it a whole display period early - at the previous
    // real frame's hand-over, which was the first version of this test -
    // left most of that period unused: The Callisto Protocol settled at 4 ms
    // of delay with the application still finishing 8 ms before it could be
    // queued. A runtime that paces the presenter with its own wait gives no
    // such hold to look from, and there the test stays at the previous real
    // frame's hand-over.
    //
    // It is judged over thirty frames, a second of them, because a frame
    // that runs long is late whatever the delay, and stepping back for each
    // one held the delay far below what the title could take. A window with
    // none late raises the delay by two milliseconds, up to the ceiling. One
    // with two or more puts the ceiling three milliseconds under where that
    // happened and holds it there for ten windows before probing above it
    // again at the same two milliseconds. Late frames with no delay applied
    // move nothing: a start-up burst once put the ceiling at zero and cost
    // the first forty seconds of a session. Guarded by presenter_mutex.
    std::chrono::nanoseconds triple_release_delay{0};
    // Negative until the first window sets it to two periods: the
    // application needs the third to render in.
    std::chrono::nanoseconds triple_release_ceiling{-1};
    std::uint32_t triple_release_ceiling_hold{};
    std::uint32_t triple_release_window_frames{};
    std::uint32_t triple_release_window_late{};
    // First synthetics that went to the runtime before their output was
    // written, this window. What the margin exists to keep at zero.
    std::uint32_t triple_release_window_unwritten{};
    // A real frame has gone out and the next hand-over is a frame's first
    // synthetic: the pace looks at it before that hand-over.
    bool triple_release_probe_due{};
    // The application's thread only.
    HANDLE application_release_timer{};
    // A condition variable waits on the system tick, which is 15.6 ms by
    // default on Windows. Every pace wait rounded up to that, so an 11.11 ms
    // schedule produced 15.5 ms submissions and exactly 64/s no matter what
    // the schedule asked for. A high-resolution timer sleeps to well under a
    // millisecond without changing the process-wide timer period, which a
    // layer has no business doing to its host.
    HANDLE presenter_pace_timer{};
    // The smallest display period the runtime has reported, which is the one
    // the hardware actually scans at. Not the latest reported value: SteamVR
    // returns a multiple of the true period when it considers the caller
    // behind - 11.1, then 55.6, then 22.2 ms within a few frames - so pacing
    // against the latest value lets a slow frame widen the pace, which makes
    // the next frame later still. That spiral throttled the presenter to
    // 3.7 Hz and froze the session.
    XrDuration presenter_display_period{};
    XrFrameState presenter_frame_state{XR_TYPE_FRAME_STATE};
    XrTime last_virtual_display_time{};
    // Whole display periods the promise to the application is moved later
    // by, so it names the time its real frames are actually shown at (see
    // observe_promise_lateness). Written by the presenter, read at waits.
    bool promise_shown_time{true};
    std::atomic<std::int32_t> promise_correction_periods{};
    // The presenter's window of measurements, in periods late against the
    // uncorrected promise; its thread alone touches these.
    std::array<std::int8_t, 64> promise_samples{};
    std::uint32_t promise_sample_count{};
    std::uint32_t promise_settle{};
    // The correction the last window named, waiting for the next to agree;
    // -1 for none.
    std::int32_t promise_candidate{-1};
    bool promise_runtime_slowed{};
    // Whether the runtime's last frame was at a multiple of the display
    // period, written by the presenter for the application's thread.
    std::atomic<bool> runtime_slowed{};
    XrResult presenter_failure{XR_SUCCESS};
    std::uint64_t next_presenter_sequence{1};
    std::size_t outstanding_presenter_submissions{};
    bool presenter_frame_state_valid{};
    // The frame the presenter's first cycle adopts instead of waiting: an
    // application wait the runtime still holds un-begun when the presenter
    // starts (DCS World issues its next wait from another thread before the
    // render thread ends the frame). The runtime would block the presenter's
    // own wait until that frame is begun, and the application's begin, which
    // would have done it, has just become virtual. So the presenter begins
    // it. Set at start, consumed by the first cycle.
    XrFrameState presenter_adopted_frame_state{XR_TYPE_FRAME_STATE};
    bool presenter_adopted_frame_state_valid{};
    // The presenter's frame counter, and the count the application was last
    // released at. The application is handed a doubled period, so it has to be
    // released once per pair, and the validity flag above cannot do that: it is
    // a latch set on the presenter's first wait and cleared only on start and
    // stop, so waiting on it returned in microseconds on every frame after the
    // first. An application slow enough to be the limit never noticed, because
    // its own rendering paced it. One fast enough to keep up ran at a frame per
    // display period against a layer that can consume one per pair, and the
    // surplus frame lost the history ring's capture slot and was rendered and
    // thrown away -- one application frame in six on UEVR, at a steady beat,
    // never two in a row.
    std::uint64_t presenter_frame_serial{};
    std::uint64_t application_served_serial{};
    bool presenter_stop_requested{};
    bool presenter_active{};
    XrSession handle{XR_NULL_HANDLE};
    // Created by the presenter on first use; see JoinProbe. Presenter thread
    // only, apart from its own waiter.
    std::unique_ptr<JoinProbe> join_probe;
    std::uint64_t join_probe_next{};
    // Both the application's thread (at a release) and the presenter (at a
    // submission) signal the probe, and the values must reach the queue in
    // the order they are numbered.
    std::mutex join_probe_mutex;
};

// The queue the runtime orders swapchain images against: the layer's own
// binding queue where one was arranged, otherwise whatever the runtime was
// given (the application's queue, or the D3D11 session's bridge queue).
[[nodiscard]] ID3D12CommandQueue* runtime_queue(const SessionState& state) noexcept {
    return state.binding_queue ? state.binding_queue.Get()
                               : state.d3d12_queue.Get();
}

// The application has released one of its images: mark where its queue
// stands, for the join before a downstream xrEndFrame that names it. See
// SessionState::binding_queue. Nothing waits here.
void mark_application_release(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->app_release_fence ||
        !state->d3d12_queue) {
        return;
    }
    try {
        std::scoped_lock lock(state->binding_join_mutex);
        const std::uint64_t value = state->app_release_counter + 1;
        if (SUCCEEDED(state->d3d12_queue->Signal(
                state->app_release_fence.Get(), value))) {
            state->app_release_counter = value;
        }
    } catch (...) {
    }
}

// The runtime has made one of the application's images safe to write, on the
// binding queue. Carry that to the queue the application will write it on.
void carry_reacquire_to_application(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->reacquire_fence ||
        !state->d3d12_queue) {
        return;
    }
    try {
        std::scoped_lock lock(state->binding_join_mutex);
        const std::uint64_t value = state->reacquire_counter + 1;
        if (SUCCEEDED(state->binding_queue->Signal(
                state->reacquire_fence.Get(), value))) {
            state->reacquire_counter = value;
            static_cast<void>(state->d3d12_queue->Wait(
                state->reacquire_fence.Get(), value));
        }
    } catch (...) {
    }
}

// Before a downstream xrEndFrame: the binding queue waits until the
// application's queue has run past the release of every image of the
// application's that the frame names. `value` is app_release_counter as it
// stood at the application's own xrEndFrame for that frame. A repeat waits
// the same value again, which costs nothing.
void join_runtime_queue_to_application(
    SessionState* state,
    std::uint64_t value) noexcept {
    if (state == nullptr || !state->binding_queue || !state->app_release_fence ||
        value == 0) {
        return;
    }
    static_cast<void>(
        state->binding_queue->Wait(state->app_release_fence.Get(), value));
}

// Lets the binding queue finish what it holds before the session it serves
// goes away. Bounded: a wait it holds for the application's queue could
// otherwise keep this here for as long as the application's queue is stuck.
void drain_binding_queue(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->reacquire_fence) {
        return;
    }
    try {
        std::uint64_t value = 0;
        {
            std::scoped_lock lock(state->binding_join_mutex);
            value = state->reacquire_counter + 1;
            if (FAILED(state->binding_queue->Signal(
                    state->reacquire_fence.Get(), value))) {
                return;
            }
            state->reacquire_counter = value;
        }
        if (state->reacquire_fence->GetCompletedValue() >= value) {
            return;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return;
        }
        if (SUCCEEDED(state->reacquire_fence->SetEventOnCompletion(value, event))) {
            WaitForSingleObject(event, 2000);
        }
        CloseHandle(event);
    } catch (...) {
    }
}

// Marks a point in the application's queue, so the probe's waiter can stamp
// when the queue gets there: `record` with the caller's b and c, then 914
// when it completes. 913 is a release's join; 916 is the moment just before
// the presenter's xrEndFrame, which is where a D3D12 runtime takes the queue
// it judges the frame's completion by. Logging on only. See JoinProbe.
void signal_join_probe(
    SessionState* state,
    std::int64_t record,
    std::uint64_t b,
    std::uint64_t c) noexcept {
    if (state == nullptr || runtime_queue(*state) == nullptr ||
        !xrfg::bridge_flight_logger().enabled()) {
        return;
    }
    try {
        std::scoped_lock signal_lock(state->join_probe_mutex);
        if (!state->join_probe && state->d3d12_device) {
            auto probe = std::make_unique<JoinProbe>();
            probe->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (probe->event != nullptr &&
                SUCCEEDED(state->d3d12_device->CreateFence(
                    0,
                    D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(probe->fence.ReleaseAndGetAddressOf())))) {
                probe->thread =
                    std::thread([raw = probe.get()] { raw->run(); });
                state->join_probe = std::move(probe);
            }
        }
        if (state->join_probe &&
            SUCCEEDED(runtime_queue(*state)->Signal(
                state->join_probe->fence.Get(),
                state->join_probe_next + 1))) {
            const std::uint64_t value = ++state->join_probe_next;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::presenter_vsync_lock,
                record,
                value,
                b,
                c);
            {
                std::scoped_lock lock(state->join_probe->mutex);
                state->join_probe->signaled = value;
            }
            state->join_probe->condition.notify_all();
        }
    } catch (...) {
    }
}

// Sessions that never take a presenter thread, so that every runtime call
// stays on the thread the application makes its own calls on: a D3D11 device
// created single-threaded has no locking for a second thread to rely on
// (single_threaded_d3d11). Such a session runs generation inline and passes
// pipelined frames through.
//
// A Vulkan session is not excluded, although by the API's rules it could
// be: the runtime submits on the queue the application handed it, and a
// Vulkan queue may be driven by one thread at a time, so the presenter's
// frame calls race the application's render thread on that queue. It is
// allowed anyway because the inline path cannot survive SteamVR's throttle:
// No Man's Sky through OpenComposite at 72 Hz, throttled to half rate, had
// SteamVR stop advancing the predicted time after the first inline pair and
// return every wait at once, until the compositor showed one frame pinned
// in space. The presenter is what handles that shape on every other API.
//
// The second D3D11 case is a driver workaround, not a rule of the API: a
// D3D11 session whose frame loop is split across two threads
// (split_frame_loop) - xrWaitFrame from one, xrBeginFrame/xrEndFrame from
// another. DCS World. With a presenter, the NVIDIA D3D11 user-mode driver
// (nvwgf2umx, seen on 616.92 and 617.14, September 2026) faults on its own
// worker thread in SetDependencyInfo: at mission start on this rig, four
// runs out of four, and mid-flight for a reporter on a 4060 Ti, on SteamVR
// and on VDXR alike. The runtime-entry gate (RuntimeEntry) was held when it
// faulted, and so was the context's own ID3D11Multithread section in an
// interim build that held it across the runtime's begin and end - measured
// holding, in a debugger, at the fault - so serialising the calls, against
// the layer and against the game, does not reach whatever the driver keeps
// per thread. That section was removed again. Inline, the same sessions
// ran. The cost is on
// SteamVR only, where the inline second cycle can be throttled to half
// rate, and for this shape only: every other D3D11 title keeps its
// presenter.
//
// To retire it: on a driver newer than 617.14, drop the split_frame_loop
// clause below, run DCS World through several mission starts on SteamVR and
// on VDXR with the flight recorder on, and read DCS's own
// Saved Games\DCS\Logs\dcs.log for a "C0000005 ACCESS_VIOLATION ...
// nvwgf2umx ... SetDependencyInfo" block. One clean start proves nothing;
// one mission ran 27 s before faulting. The dcs-d3d11 call-chain mode
// asserts the refusal and retires with it.
[[nodiscard]] bool presenter_forbidden(const SessionState& state) noexcept {
    return state.single_threaded_d3d11 ||
        (state.graphics_binding == SessionGraphicsBinding::d3d11 &&
         state.split_frame_loop);
}

// Inside xrEndFrame, and inside each swapchain image call, the runtime drives
// the application's single D3D11 immediate context. ID3D11Multithread makes
// each of the runtime's own D3D11 calls atomic and no more, so the sequence
// one of those OpenXR calls issues interleaves with the sequence another
// issues on the other thread, and the driver's dependency tracking walks a
// chain that has moved. That is what kills nvwgf2umx when a presenter thread
// runs beside the application thread on a D3D11 session. Necessary, and not
// sufficient for every title: DCS World faulted the driver with the gate
// held, so its shape never takes the presenter (presenter_forbidden).
//
// Holding the layer's own device section could not cover it and recorded no
// contention at all: that guards the layer's D3D11 work, while the work that
// collides is the runtime's, inside the runtime's own calls. The gate has to
// wrap our entry into the runtime, because that is the only place from which
// the runtime's sequence is reachable.
//
// One OpenXR call's worth of D3D11 work is the unit that has to be atomic, so
// this wraps single calls and never a longer span. xrWaitFrame stays outside
// it deliberately: it blocks for most of a display period and issues no D3D11
// work, and holding it here would hand the application thread exactly the
// stall the presenter exists to remove.
thread_local int runtime_entry_depth = 0;

struct RuntimeEntry {
    explicit RuntimeEntry(SessionState* session) noexcept
        : session_(session != nullptr &&
                           (session->d3d11_device != nullptr ||
                            session->graphics_binding ==
                                SessionGraphicsBinding::vulkan)
                       ? session
                       : nullptr) {
        if (session_ == nullptr) {
            return;
        }
        outermost_ = runtime_entry_depth == 0;
        const auto before = std::chrono::steady_clock::now();
        session_->runtime_entry_mutex.lock();
        entered_ = std::chrono::steady_clock::now();
        ++runtime_entry_depth;
        if (!outermost_) {
            return;
        }
        const auto waited = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                entered_ - before)
                .count());
        ++session_->runtime_entry_count;
        session_->runtime_entry_wait_ns += waited;
        if (waited > session_->runtime_entry_wait_max_ns) {
            session_->runtime_entry_wait_max_ns = waited;
        }
        // A lock handed over uncontended still costs a few hundred
        // nanoseconds, so only a wait long enough to be a real hand-off
        // counts as one thread having found the other inside the runtime.
        if (waited >= 20'000) {
            ++session_->runtime_entry_contended;
        }
    }

    ~RuntimeEntry() {
        if (session_ == nullptr) {
            return;
        }
        --runtime_entry_depth;
        if (outermost_) {
            const auto held = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - entered_)
                    .count());
            if (held > session_->runtime_entry_hold_max_ns) {
                session_->runtime_entry_hold_max_ns = held;
            }
            // Reported in windows rather than per entry: one record per frame
            // would bury the log, and the question this answers - whether the
            // two threads ever meet here - is a rate, not an event.
            // Eight application frames' worth, so a short capture still
            // reports and a long one costs a handful of records a second.
            if (session_->runtime_entry_count >= 64) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::runtime_entry_section,
                    static_cast<std::int64_t>(session_->runtime_entry_contended),
                    session_->runtime_entry_wait_ns /
                        session_->runtime_entry_count,
                    session_->runtime_entry_wait_max_ns,
                    session_->runtime_entry_hold_max_ns);
                session_->runtime_entry_count = 0;
                session_->runtime_entry_contended = 0;
                session_->runtime_entry_wait_ns = 0;
                session_->runtime_entry_wait_max_ns = 0;
                session_->runtime_entry_hold_max_ns = 0;
            }
        }
        session_->runtime_entry_mutex.unlock();
    }

    RuntimeEntry(const RuntimeEntry&) = delete;
    RuntimeEntry& operator=(const RuntimeEntry&) = delete;

private:
    SessionState* session_{};
    bool outermost_{};
    std::chrono::steady_clock::time_point entered_{};
};

// The gate covers exactly one call into the runtime, so a call is what it
// takes. Several frame submissions reach the runtime through the overlay
// rather than through the dispatch pointer, and those are the ones a live
// session actually uses, so the whole submitting expression is handed over
// rather than the dispatch call inside it.
template <typename Call>
[[nodiscard]] auto with_runtime_entry(SessionState* session, Call&& call)
    -> decltype(call()) {
    const RuntimeEntry gate(session);
    return call();
}

// The frame paths hold the session by shared_ptr and the swapchain paths by
// raw pointer; the gate does not care which.
template <typename Call>
[[nodiscard]] auto with_runtime_entry(
    const std::shared_ptr<SessionState>& session, Call&& call)
    -> decltype(call()) {
    return with_runtime_entry(session.get(), std::forward<Call>(call));
}

// xrWaitFrame, which the gate covers on D3D11 (the runtime's D3D11 work in a
// wait is what a V265 capture found interleaving with a gated swapchain call)
// and must not cover on Vulkan: a wait submits nothing on a Vulkan queue, and
// SteamVR blocks the presenter's wait a whole throttled period - 27.8 ms at
// 72 Hz - so a gate held across it starves every swapchain call the
// application makes in the meantime. No Man's Sky spent 26-382 ms in each
// and hung inside OpenComposite's own queue submission.
template <typename Call>
[[nodiscard]] auto with_runtime_entry_for_wait(
    const std::shared_ptr<SessionState>& session, Call&& call)
    -> decltype(call()) {
    return with_runtime_entry(
        session && session->graphics_binding == SessionGraphicsBinding::vulkan
            ? nullptr
            : session.get(),
        std::forward<Call>(call));
}

// The recorder's reprojection_angle for a frame about to be handed to the
// runtime (BridgeFlightOperation::reprojection_angle); kind is
// presenter_submission's. Made before the hand-over rather than after it, so
// the space the frame names cannot be destroyed under the locate: the layer
// holds an application's xrDestroySpace until every frame naming that space
// has gone down. Two xrLocateSpace calls a frame, and only while the
// recorder runs.
void log_reprojection_angle(
    SessionState& state,
    const XrFrameEndInfo& submitted,
    std::uint32_t kind) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled() || submitted.layers == nullptr ||
            state.dispatch->create_reference_space == nullptr ||
            state.dispatch->locate_space == nullptr) {
            return;
        }
        const XrCompositionLayerProjection* projection = nullptr;
        for (std::uint32_t index = 0; index < submitted.layerCount && projection == nullptr; ++index) {
            const XrCompositionLayerBaseHeader* layer = submitted.layers[index];
            if (layer != nullptr && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            }
        }
        if (projection == nullptr || projection->viewCount == 0 || projection->views == nullptr) {
            return;
        }
        XrSpace view_space = XR_NULL_HANDLE;
        {
            std::scoped_lock lock(state.reprojection_space_mutex);
            if (!state.reprojection_space_attempted) {
                state.reprojection_space_attempted = true;
                XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                info.poseInReferenceSpace.orientation.w = 1.0F;
                XrSpace created = XR_NULL_HANDLE;
                if (XR_SUCCEEDED(with_runtime_entry(&state, [&] {
                        return state.dispatch->create_reference_space(state.handle, &info, &created);
                    }))) {
                    state.reprojection_view_space = created;
                }
            }
            view_space = state.reprojection_view_space;
        }
        if (view_space == XR_NULL_HANDLE) {
            return;
        }
        // The views' mean orientation: a headset that cants its eyes cants
        // them symmetrically about the head, which this cancels, so a real
        // frame shown when it was predicted for reads near zero.
        const XrQuaternionf& first = projection->views[0].pose.orientation;
        xrfg::Quaternion mean{0.0F, 0.0F, 0.0F, 0.0F};
        for (std::uint32_t index = 0; index < projection->viewCount; ++index) {
            const XrQuaternionf& view = projection->views[index].pose.orientation;
            const float sign = view.x * first.x + view.y * first.y + view.z * first.z +
                    view.w * first.w < 0.0F ? -1.0F : 1.0F;
            mean.x += view.x * sign;
            mean.y += view.y * sign;
            mean.z += view.z * sign;
            mean.w += view.w * sign;
        }
        const auto locate = [&](XrTime time, xrfg::Quaternion* orientation) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            const XrResult result = with_runtime_entry(&state, [&] {
                return state.dispatch->locate_space(view_space, projection->space, time, &location);
            });
            if (XR_FAILED(result) ||
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
                return false;
            }
            *orientation = {location.pose.orientation.x, location.pose.orientation.y,
                            location.pose.orientation.z, location.pose.orientation.w};
            return true;
        };
        xrfg::Quaternion head{};
        if (!locate(submitted.displayTime, &head)) {
            return;
        }
        XrDuration period = 0;
        {
            std::scoped_lock lock(state.mutex);
            period = state.minimum_runtime_display_period;
        }
        xrfg::Quaternion earlier{};
        const bool turn_known = period > 0 && locate(submitted.displayTime - period, &earlier);
        constexpr float kMillidegreesPerRadian = 180000.0F / 3.14159265F;
        const auto millidegrees = [&](const xrfg::Quaternion& from, const xrfg::Quaternion& to) {
            return static_cast<std::uint64_t>(
                xrfg::rotation_angle(from, to) * kMillidegreesPerRadian + 0.5F);
        };
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::reprojection_angle,
            kind,
            millidegrees(xrfg::normalize(mean), head),
            turn_known ? millidegrees(earlier, head) : 0,
            state.synthetic_pose_interpolated.load(std::memory_order_relaxed) ? 1U : 0U);
    } catch (...) {
    }
}

enum class PrivateOwnershipPhase {
    idle,
    acquired,
    waited,
    release_pending,
};

// Copies a frame's staging texture into the private swapchain image the
// runtime has just handed out, on the runtime's binding queue, immediately
// before the release that marks the image complete. Command lists come from
// a small ring recycled on a fence. A list still in flight eight hand-overs
// later means the GPU is that far behind; the copy is then skipped rather
// than waited for, and the frame shows the image's previous content, the
// last frame of that output.
struct HandoverCopier {
    static constexpr std::size_t kSlots = 8;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::array<Slot, kSlots> slots{};
    std::size_t next_slot{};
    std::uint64_t next_value{1};
    std::mutex mutex;

    [[nodiscard]] HRESULT initialize(ID3D12Device* input_device) noexcept {
        try {
            if (input_device == nullptr) {
                return E_INVALIDARG;
            }
            device = input_device;
            HRESULT result = device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            for (Slot& slot : slots) {
                result = device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(slot.allocator.ReleaseAndGetAddressOf()));
                if (FAILED(result)) {
                    return result;
                }
                result = device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(),
                    nullptr, IID_PPV_ARGS(slot.list.ReleaseAndGetAddressOf()));
                if (FAILED(result)) {
                    return result;
                }
                result = slot.list->Close();
                if (FAILED(result)) {
                    return result;
                }
            }
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    // Both resources rest in COMMON: the staging because the synthesizer
    // leaves its destinations there, the runtime's image because that is
    // the state the layer's private images are kept in.
    [[nodiscard]] HRESULT copy(
        ID3D12CommandQueue* queue,
        ID3D12Resource* source,
        ID3D12Resource* destination) noexcept {
        try {
            std::scoped_lock lock(mutex);
            if (queue == nullptr || source == nullptr || destination == nullptr ||
                !fence) {
                return E_INVALIDARG;
            }
            Slot& slot = slots[next_slot];
            if (slot.fence_value != 0 &&
                fence->GetCompletedValue() < slot.fence_value) {
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            next_slot = (next_slot + 1) % kSlots;
            HRESULT result = slot.allocator->Reset();
            if (FAILED(result)) {
                return result;
            }
            result = slot.list->Reset(slot.allocator.Get(), nullptr);
            if (FAILED(result)) {
                return result;
            }
            std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = source;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = destination;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            slot.list->ResourceBarrier(
                static_cast<UINT>(barriers.size()), barriers.data());
            slot.list->CopyResource(destination, source);
            std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
            std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
            slot.list->ResourceBarrier(
                static_cast<UINT>(barriers.size()), barriers.data());
            result = slot.list->Close();
            if (FAILED(result)) {
                return result;
            }
            ID3D12CommandList* lists[] = {slot.list.Get()};
            queue->ExecuteCommandLists(1, lists);
            result = queue->Signal(fence.Get(), next_value);
            if (FAILED(result)) {
                return result;
            }
            slot.fence_value = next_value++;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    // The destroy path only: the staging and the images are about to go.
    // False when the copies were not seen to finish within two seconds.
    [[nodiscard]] bool wait_idle() noexcept {
        try {
            std::scoped_lock lock(mutex);
            if (!fence || next_value <= 1) {
                return true;
            }
            const std::uint64_t target = next_value - 1;
            if (fence->GetCompletedValue() >= target) {
                return true;
            }
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (event == nullptr) {
                return false;
            }
            bool idle = false;
            if (SUCCEEDED(fence->SetEventOnCompletion(target, event))) {
                idle = WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
            }
            CloseHandle(event);
            return idle;
        } catch (...) {
            return false;
        }
    }
};

struct PrivateSwapchainState {
    XrSwapchain handle{XR_NULL_HANDLE};
    PrivateOwnershipPhase phase{PrivateOwnershipPhase::idle};
    std::uint32_t acquired_index{};
    // vram_usage private_first_use has been written for this swapchain.
    bool first_use_logged{};
};

// Each output alternates between private swapchains so the application can
// release frame N+1's output while the presenter still holds frame N's
// submission. A composition layer names a swapchain rather than an image
// index, and the runtime binds whichever image was released last when
// xrEndFrame runs, so one shared swapchain would repoint a submission the
// presenter has not finished with at the newer image.
//
// What makes a slot safe to reuse is retirement, not dequeue.
// outstanding_presenter_submissions is decremented only after the downstream
// xrEndFrame has returned, so a submission still counted may be sitting in the
// queue or inside the runtime; one that is no longer counted has had its image
// bound. The application is admitted at a fixed count of un-retired
// submissions, and a ring needs one slot for every submission of that output
// which can still be un-retired at that moment.
//
// The current output always needs two. The synthetic needs two with the
// deeper pipeline: at the shallow admission bound the previous pair's
// synthetic has always retired by the time the application is admitted, and at
// the deeper bound it has not. It needs two with 3X as well, one for each of
// the frame's two synthetics; both have retired by the next admission, so
// the same two serve every frame.
constexpr std::size_t kCurrentSlotCount = 2;
constexpr std::size_t kSyntheticSlotCountShallow = 1;
constexpr std::size_t kSyntheticSlotCountDeep = 2;
constexpr std::size_t kPrivateRingSlotMax = 2;
// Single-swapchain rings (SessionState::single_swapchain_rings, the default
// where the synthesizer writes D3D12 images directly): one private swapchain
// per output, and the synthesizer writes layer-owned staging textures
// instead - two per output, the count of that output's frames the rings
// above were sized to keep un-retired at once. Each hand-over acquires the
// one swapchain, waits it, copies the frame's staging into the image and
// releases it immediately before the xrEndFrame that names it, so the image
// the runtime binds is always the frame being handed over and no image is
// ever left waited across a frame boundary. Half the runtime images per
// output, and half again on a runtime that keeps a copy of every image it
// is handed, as Pimax's does. The interops (Vulkan, the legacy D3D11 path)
// mirror private images by index and keep the rings.
constexpr std::size_t kStagingSlotCount = 2;

struct FrameGenerationSwapchainState {
    std::array<PrivateSwapchainState, kCurrentSlotCount> current{};
    // Destination images are addressed by a flat index across every slot, so
    // slot s image i is s * images_per_slot + i. Both rings are addressed this
    // way, by the synthesizer and by the D3D11 interop.
    std::uint32_t current_images_per_slot{};
    std::size_t current_slot{};
    std::array<PrivateSwapchainState, kPrivateRingSlotMax> synthetic{};
    std::uint32_t synthetic_images_per_slot{};
    std::size_t synthetic_slot_count{kSyntheticSlotCountShallow};
    std::size_t synthetic_slot{};
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    std::shared_ptr<xrfg::SwapchainInterop> interop;
    // Single-swapchain rings: current[0] and synthetic[0] are the swapchains,
    // the staging textures are what the synthesizer writes, indexed by
    // current_slot / synthetic_slot, and the runtime images are what the
    // hand-over copies into, by acquired index.
    bool single_swapchain_rings{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kStagingSlotCount> current_staging{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kStagingSlotCount> synthetic_staging{};
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> current_runtime_images;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> synthetic_runtime_images;
    std::shared_ptr<HandoverCopier> copier;
};

// The caller holds the session's gpu_mutex, which also guards the status
// panel's GPU times this keeps.
void log_completed_nvidia_gpu_timings(
    SessionState& session,
    const std::shared_ptr<xrfg::D3D12FrameSynthesizer>& synthesizer) noexcept {
    if (!synthesizer || !xrfg::bridge_flight_logger().enabled()) {
        return;
    }
    for (;;) {
        xrfg::D3D12NvidiaGpuTiming timing{};
        const HRESULT result =
            synthesizer->consume_nvidia_gpu_timing(&timing);
        if (result == S_FALSE) {
            return;
        }
        if (FAILED(result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::nvidia_gpu_total,
                result);
            return;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::nvidia_gpu_stages,
            timing.eye_count,
            timing.pack_microseconds,
            timing.eye0_microseconds,
            timing.eye1_microseconds);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::nvidia_gpu_total,
            0,
            timing.composition_microseconds,
            timing.total_microseconds,
            timing.current_serial);
        // The status panel's GPU time, smoothed over about a second of pairs.
        try {
            auto& smoothed = session.panel_gpu[synthesizer.get()];
            const auto now = std::chrono::steady_clock::now();
            const float measured = static_cast<float>(timing.total_microseconds);
            smoothed.microseconds = now - smoothed.at > std::chrono::seconds(2)
                ? measured
                : smoothed.microseconds + (measured - smoothed.microseconds) * 0.05F;
            smoothed.at = now;
        } catch (...) {
        }
        // When the GPU actually began and ended this pair's synthesis, on
        // the log's own timeline, so it can be compared directly with the
        // internal_end_frame that handed the synthetic to the runtime.
        if (timing.gpu_begin_qpc != 0 && timing.gpu_end_qpc != 0) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::synthesis_gpu_span,
                static_cast<std::int64_t>(timing.total_microseconds),
                static_cast<std::uint64_t>(
                    xrfg::bridge_flight_logger().microseconds_for_counter(
                        static_cast<std::int64_t>(timing.gpu_begin_qpc))),
                static_cast<std::uint64_t>(
                    xrfg::bridge_flight_logger().microseconds_for_counter(
                        static_cast<std::int64_t>(timing.gpu_end_qpc))),
                timing.current_serial);
        }
    }
}

struct SwapchainState;

// The steps a vram_usage record is taken after. A report of what the layer
// costs in video memory is split by them: the runtime's own swapchain for the
// application, then for each swapchain the layer arms, its private rings (also
// the runtime's, at the layer's request), the interop's shared textures, the
// history and the synthesizer. periodic is every five seconds of frames.
enum class VideoMemoryStage : std::int64_t {
    session = 1,
    application_swapchain = 2,
    current_ring = 3,
    synthetic_ring = 4,
    interop = 5,
    history = 6,
    synthesizer = 7,
    swapchain_destroyed = 8,
    periodic = 9,
    // The first acquire of a private swapchain: a runtime that allocates
    // on first use rather than at creation shows up here, c= the swapchain.
    private_first_use = 10,
    // The staging textures of single-swapchain rings.
    staging = 11,
};

void log_video_memory(
    SessionState& session,
    VideoMemoryStage stage,
    std::uint64_t subject) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        {
            std::scoped_lock lock(session.video_memory_mutex);
            if (!session.video_memory_adapter && !session.video_memory_unavailable) {
                LUID luid{};
                bool found = false;
                if (session.d3d12_device) {
                    luid = session.d3d12_device->GetAdapterLuid();
                    found = true;
                } else if (session.d3d11_device) {
                    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
                    Microsoft::WRL::ComPtr<IDXGIAdapter> device_adapter;
                    DXGI_ADAPTER_DESC description{};
                    if (SUCCEEDED(session.d3d11_device.As(&dxgi_device)) &&
                        SUCCEEDED(dxgi_device->GetAdapter(&device_adapter)) &&
                        SUCCEEDED(device_adapter->GetDesc(&description))) {
                        luid = description.AdapterLuid;
                        found = true;
                    }
                }
                // No device yet: a later record finds one.
                if (!found) {
                    return;
                }
                Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                    FAILED(factory->EnumAdapterByLuid(
                        luid, IID_PPV_ARGS(&session.video_memory_adapter)))) {
                    session.video_memory_adapter.Reset();
                    session.video_memory_unavailable = true;
                    return;
                }
            }
            adapter = session.video_memory_adapter;
        }
        if (!adapter) {
            return;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (FAILED(adapter->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
            return;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::vram_usage,
            static_cast<std::int64_t>(stage),
            local.CurrentUsage,
            local.Budget,
            subject);
    } catch (...) {
    }
}

// swapchain_image for a D3D12 image: what the runtime actually allocated
// against what was asked for. result= DXGI format, a= swapchain, b= width
// <<32 | height, c= mip levels<<48 | array size<<32 | resource flags.
void log_d3d12_image_description(XrSwapchain swapchain, ID3D12Resource* image) noexcept {
    try {
        if (image == nullptr ||
            !xrfg::bridge_flight_logger().keeps_session_records()) {
            return;
        }
        const D3D12_RESOURCE_DESC description = image->GetDesc();
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::swapchain_image,
            static_cast<std::int64_t>(description.Format),
            handle_value(swapchain),
            (static_cast<std::uint64_t>(description.Width) << 32) |
                description.Height,
            (static_cast<std::uint64_t>(description.MipLevels) << 48) |
                (static_cast<std::uint64_t>(description.DepthOrArraySize) << 32) |
                static_cast<std::uint32_t>(description.Flags));
    } catch (...) {
    }
}

enum class SwapchainEligibilityReason : std::int64_t {
    ready = 0,
    no_d3d12_binding = 1,
    incomplete_enumeration = 2,
    invalid_d3d12_image = 3,
    ambiguous_attachment_usage = 4,
    protected_content = 5,
    history_initialize_failed = 6,
    depth_only = 7,
    static_image = 8,
    unsupported_face_count = 9,
    missing_dispatch_or_history = 10,
    current_private_swapchain_failed = 11,
    synthetic_private_swapchain_failed = 12,
    synthesis_initialize_failed = 13,
    exception = 14,
    d3d11_interop_initialize_failed = 15,
    invalid_d3d11_image = 16,
    awaiting_projection_use = 17,
    budget_exhausted = 18,
    vulkan_interop_initialize_failed = 19,
    unsupported_vulkan_format = 20,
    invalid_vulkan_image = 21,
    vulkan_support_off = 22,
    // The runtime refused a private swapchain while the session was arming
    // with the deeper pipeline; it fell back to the shallow depth and armed
    // again. `detail` is the runtime's refusal, `auxiliary` the session.
    deep_pipeline_fallback = 23,
};

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail = 0,
    std::uint64_t auxiliary = 0) noexcept;

struct SwapchainState {
    SwapchainState(
        std::shared_ptr<SessionState> owner,
        const XrSwapchainCreateInfo& input_create_info)
        : session(std::move(owner)),
          create_info(input_create_info) {
        create_info.next = nullptr;
    }

    std::shared_ptr<SessionState> session;
    XrSwapchain handle{XR_NULL_HANDLE};
    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // OpenXR permits acquire/wait/release calls from different threads. Keep the
    // downstream call and the matching ownership bookkeeping in one total order.
    std::mutex call_mutex;
    std::mutex mutex;
    std::deque<std::uint32_t> acquired_indices;
    // How many of acquired_indices, from the front, the application has
    // waited on. A release always takes the front, and only a waited one.
    // This mirrors the runtime's own queue exactly as long as every
    // acquire, wait and release passes through here, which they do; so it
    // is never cleared behind the application's back, because on the D3D11
    // bridge it also decides which shared texture is copied into the
    // runtime's image at release, and losing it shows the runtime images
    // nothing was ever written into (Ready or Not's VR mod acquires its
    // first image before xrBeginSession and keeps one acquired ahead).
    std::size_t waited_count{};
    bool ownership_tracking_valid{true};
    std::optional<std::uint32_t> last_released_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> d3d12_history;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> enumerated_d3d11_images;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> enumerated_d3d12_images;
    // On a bridged D3D11 session (SessionState::d3d11_bridge): the shared
    // textures the application renders into and the copies into the
    // runtime's images. enumerated_d3d12_images are then the shared
    // textures, which is what the D3D12 history captures from.
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    // On a bridged Vulkan session (SessionState::vulkan_bridge), the same
    // role: the shared textures the application renders into through its
    // Vulkan imports, and the copies into the runtime's images.
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    // Either bridge took the private depth path: the runtime's images are
    // never written, and depth information naming this swapchain is
    // stripped from every submission.
    bool private_depth{};
    // Not owned: a VkImage is a handle the application's device owns, and
    // the layer holds nothing that keeps it alive.
    std::vector<VkImage> enumerated_vulkan_images;
    std::optional<xrfg::D3D12HistoryCaptureTicket> last_released_capture;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> last_released_motion_vectors;
    // Which eye of a two-view projection this swapchain was last submitted
    // as, when it holds that eye alone (a game with a swapchain per eye): 0
    // left, 1 right, -1 otherwise. It picks the eye's own DLSS evaluation
    // (resolve_dlss_motion_vectors). Written at the application's xrEndFrame.
    std::atomic<int> projection_eye{-1};
    // Whether a projection layer has named this swapchain as a view. Only
    // such a swapchain is armed outside the projection path
    // (apply_embedded_control). Written at the application's xrEndFrame.
    std::atomic<bool> projection_used{false};
    // How many DLSS evaluations had been published at this image's last
    // release. Guarded by mutex.
    std::uint64_t release_publication{};
    // SessionState::capture_at_end_frame: the image the application released
    // since its last xrEndFrame, whose history capture is still to be queued.
    // Set at the release in place of the capture, consumed at the top of the
    // application's xrEndFrame (capture_pending_end_frame_images), where
    // every command list of the frame has reached the application's queue.
    // A second release before then replaces it: the earlier image was never
    // going to be shown. Guarded by mutex.
    std::optional<std::uint32_t> pending_end_frame_capture;
    std::shared_ptr<FrameGenerationSwapchainState> frame_generation;
    // Generation costs three runtime swapchains, and a swapchain that never
    // reaches a projection layer never spends them: an application's UI quads
    // and its stereo views are indistinguishable at enumeration time, so the
    // decision waits until xrEndFrame names this swapchain as a projection
    // view. Guarded by call_mutex.
    bool generation_eligible_pending{};
    std::uint32_t enumerated_image_count{};
    // An attempt that failed for its own reasons -- synthesis, interop -- is
    // not retried every frame. Cleared whenever the history is rebuilt, which
    // is the point at which the images themselves changed.
    bool generation_declined{};
};

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail,
    std::uint64_t auxiliary) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_eligibility,
        static_cast<std::int64_t>(reason),
        state ? handle_value(state->handle) : 0,
        detail,
        auxiliary);
}

std::mutex g_state_mutex;
std::unordered_map<XrInstance, std::shared_ptr<Dispatch>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> g_sessions;
std::unordered_map<XrSwapchain, std::shared_ptr<SwapchainState>> g_swapchains;

template <typename Function>
[[nodiscard]] XrResult guard_c_api_boundary(Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] std::shared_ptr<Dispatch> find_dispatch(XrInstance instance) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_instances.find(instance);
    return iterator == g_instances.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::shared_ptr<SessionState> find_session(XrSession session) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_sessions.find(session);
    return iterator == g_sessions.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::shared_ptr<SwapchainState> find_swapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_swapchains.find(swapchain);
    return iterator == g_swapchains.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::vector<std::shared_ptr<SwapchainState>> find_swapchains(
    const std::shared_ptr<SessionState>& session) {
    std::vector<std::shared_ptr<SwapchainState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_swapchains) {
        (void)handle;
        if (state->session == session) {
            matches.push_back(state);
        }
    }
    return matches;
}

[[nodiscard]] std::vector<std::shared_ptr<SwapchainState>> find_swapchains(
    const std::shared_ptr<Dispatch>& dispatch) {
    std::vector<std::shared_ptr<SwapchainState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_swapchains) {
        (void)handle;
        if (state->session->dispatch == dispatch) {
            matches.push_back(state);
        }
    }
    return matches;
}

// Submit the held-back current copy without waiting for anything.
//
// The copy is the real frame's own content, deferred to keep it off the
// synthetic's critical path, and it is not signalled until something submits
// it. It has to go out at frame start, with the whole frame ahead of it:
// submitted about fifteen milliseconds later instead, the real frame's
// hand-over landed on pixels still in flight and its xrEndFrame went from the
// 0.69-0.75 ms of a copy that is already done to 2.5 ms of blocking.
//
// That block is subtracted from the interval before the synthetic, and that
// interval is what decides whether the synthetic gets a scanout at all. Per
// submission, over 1146 synthetics: the ones the compositor presented arrived a
// median 11.032 ms after the previous frame, the ones it dropped 9.568 ms, and
// 99.9% of the dropped ones followed a real frame that had been presented - two
// frames inside one scanout, second one loses. Restoring the early flush took
// realCall 2.510 -> 1.324 ms, the interval 9.648 -> 10.432 ms, and the share of
// synthetics reaching the headset from 15.0% to 62.6%.
//
// This submits and returns; it never blocks.
[[nodiscard]] HRESULT flush_session_pending_copies(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        HRESULT aggregate = S_OK;
        for (const auto& swapchain : find_swapchains(session)) {
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            const HRESULT result =
                // No consumer queue: this only submits the copy so it has
                // the whole frame to complete in. The join that names a
                // value happens later, at the presenter.
                generation->synthesizer->flush_current_copy(nullptr, 0);
            if (FAILED(result)) {
                aggregate = result;
            }
        }
        return aggregate;
    } catch (...) {
        return E_FAIL;
    }
}

[[nodiscard]] std::vector<std::shared_ptr<SessionState>> find_sessions(
    const std::shared_ptr<Dispatch>& dispatch) {
    std::vector<std::shared_ptr<SessionState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_sessions) {
        (void)handle;
        if (state->dispatch == dispatch) {
            matches.push_back(state);
        }
    }
    return matches;
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame = nullptr,
    bool preserve_virtual_timeline = false,
    std::optional<XrFrameState> adopted_frame_state = std::nullopt) noexcept;
void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept;
// Prevent new enqueues, finish queued/in-flight submissions, then retire any
// autonomous repeat before invalidating an application-owned handle.
struct PresenterResourceLifetimeGuard {
    explicit PresenterResourceLifetimeGuard(const std::shared_ptr<SessionState>& state);
    std::unique_lock<std::mutex> frame_lock;
    std::unique_lock<std::mutex> content_lock;
};
void schedule_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail = 0) noexcept;
void clear_generation_continuity(
    const std::shared_ptr<SessionState>& state) noexcept;
void enter_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail = 0) noexcept;
[[nodiscard]] XrDuration virtual_display_period(
    XrDuration period,
    std::uint32_t frames) noexcept;
[[nodiscard]] XrTime add_display_duration(
    XrTime time,
    XrDuration duration) noexcept;

// How far past the runtime's prediction the application's frame is anchored:
// a virtual period when it is the last of its group, as interpolation shows
// it, and that less the group's other display periods when extrapolation
// shows it first.
[[nodiscard]] XrDuration extrapolation_shown_sooner(
    const SessionState& state,
    XrDuration virtual_period,
    XrDuration period) noexcept {
    if (!state.nvidia_options.extrapolate || period <= 0 || virtual_period <= period) {
        return virtual_period;
    }
    const XrDuration sooner =
        period * (static_cast<XrDuration>(state.frames_per_application_frame.load()) - 1);
    return virtual_period > sooner ? virtual_period - sooner : virtual_period;
}

// How far past the runtime's prediction the application's frame is promised:
// the anchor above, and then as many display periods as its real frames have
// been going down after that while generating.
[[nodiscard]] XrDuration promised_display_offset(
    const SessionState& state,
    XrDuration virtual_period,
    XrDuration period) noexcept {
    const XrDuration anchor = extrapolation_shown_sooner(state, virtual_period, period);
    if (!state.promise_shown_time || period <= 0 || virtual_period <= period) {
        return anchor;
    }
    return anchor +
        period * static_cast<XrDuration>(
            state.promise_correction_periods.load(std::memory_order_relaxed));
}

// The anchor assumes the application hands its frame over within a display
// period of its wait returning, so that the pair's first submission goes down
// the period after. A game rendering at half the display rate takes most of
// its two: measured in Galactic Racer at 120 Hz, the pair's first submission
// went down a period later than that, so each real frame was shown a period
// after the time it was promised extrapolating, and two after interpolating
// with the deeper pipeline, which holds the synthetic a period more. The game
// rendered every frame for a head pose that much early, and the runtime's
// reprojection made up the difference. The order and the depth decide when
// frames go down; this only makes the promise say so.
//
// Each real frame of a pair is measured, in whole periods, against the
// promise it was given less the correction already in it, and the correction
// is the one that leaves a window of them least late or early on average -
// its median, in whole periods. It follows once two windows in a row name the
// same one and it saves at least a quarter of a period a frame, so one late
// frame, or a scene that alternates, changes nothing. Frames promised before
// a change are not counted against it.
//
// It used to follow only when nine in ten of a window agreed. Kayak VR's
// frames go down in two groups a frame apart - about 72% at one lateness and
// 27% a whole frame later, as Unreal Engine 4 ends a frame now before and now
// after its next wait - so no window ever agreed, and with FidelityFX flow 71%
// of its frames went down a period after the time they were promised.
void observe_promise_lateness(
    SessionState& state,
    XrDuration late,
    XrDuration period,
    XrDuration runtime_period) noexcept {
    if (!state.promise_shown_time || period <= 0) {
        return;
    }
    // Not while the runtime runs at a multiple of the display period, as
    // SteamVR does for a while after it judges the caller late: frames then
    // go down two periods apart, and in Galactic Racer the measurement moved
    // the promise between one period and three every 2.4 s for as long as
    // that lasted. The correction found at the true rate is kept, and the
    // window starts again once the rate is back.
    const bool slowed = runtime_period > period + period / 2;
    if (slowed != state.promise_runtime_slowed) {
        state.promise_runtime_slowed = slowed;
        state.promise_sample_count = 0;
        state.promise_settle = 8;
        state.promise_candidate = -1;
    }
    if (slowed) {
        return;
    }
    if (state.promise_settle > 0) {
        --state.promise_settle;
        return;
    }
    constexpr std::int32_t kMaximumCorrection = 3;
    const std::int32_t current =
        state.promise_correction_periods.load(std::memory_order_relaxed);
    const double periods =
        static_cast<double>(late) / static_cast<double>(period) + current;
    const auto rounded = static_cast<std::int32_t>(
        std::clamp(periods < 0.0 ? periods - 0.5 : periods + 0.5, -8.0, 8.0));
    state.promise_samples[state.promise_sample_count++] =
        static_cast<std::int8_t>(rounded);
    if (state.promise_sample_count < state.promise_samples.size()) {
        return;
    }
    state.promise_sample_count = 0;
    std::array<std::uint32_t, 17> counts{};
    for (const std::int8_t sample : state.promise_samples) {
        ++counts[static_cast<std::size_t>(sample + 8)];
    }
    // The summed distance, in periods, of the window's frames from a
    // correction.
    const auto cost = [&](std::int32_t correction) {
        std::uint64_t total = 0;
        for (std::int32_t value = -8; value <= 8; ++value) {
            total += static_cast<std::uint64_t>(counts[static_cast<std::size_t>(value + 8)]) *
                static_cast<std::uint64_t>(std::abs(value - correction));
        }
        return total;
    };
    std::int32_t target = current;
    std::uint64_t target_cost = cost(current);
    for (std::int32_t correction = 0; correction <= kMaximumCorrection; ++correction) {
        const std::uint64_t candidate = cost(correction);
        if (candidate < target_cost) {
            target = correction;
            target_cost = candidate;
        }
    }
    // At least a quarter of a period a frame over the window (about a second
    // of frames at 60 a second), and the same answer from the window before.
    const std::uint64_t saving = cost(current) - target_cost;
    if (target == current || saving * 4 < state.promise_samples.size()) {
        state.promise_candidate = -1;
        return;
    }
    if (state.promise_candidate != target) {
        state.promise_candidate = target;
        return;
    }
    state.promise_candidate = -1;
    const auto agreeing = counts[static_cast<std::size_t>(target + 8)];
    state.promise_correction_periods.store(target, std::memory_order_relaxed);
    // The frames already promised under the old correction.
    state.promise_settle = 8;
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::promise_correction,
        0,
        static_cast<std::uint64_t>(target),
        static_cast<std::uint64_t>(current),
        agreeing);
}

void drain_swapchain_gpu(const std::shared_ptr<SwapchainState>& state) noexcept {
    const auto flight_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::gpu_drain,
        state && state->session ? handle_value(state->session->handle) : 0);
    try {
        std::scoped_lock gpu_lock(state->session->gpu_mutex);
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        {
            std::scoped_lock lock(state->mutex);
            history = state->d3d12_history;
            generation = state->frame_generation;
        }
        HRESULT result = S_OK;
        if (generation && generation->synthesizer) {
            result = generation->synthesizer->wait_for_idle();
        }
        if (history) {
            const HRESULT history_result = history->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = history_result;
            }
        }
        if (generation && generation->interop) {
            const HRESULT interop_result =
                generation->interop->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = interop_result;
            }
        }
        std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
        {
            std::scoped_lock lock(state->mutex);
            vulkan_bridge = state->vulkan_bridge;
        }
        if (vulkan_bridge) {
            const HRESULT vulkan_result = vulkan_bridge->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = vulkan_result;
            }
        }
        std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
        {
            std::scoped_lock lock(state->mutex);
            bridge = state->bridge;
        }
        if (bridge) {
            const HRESULT bridge_result = bridge->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = bridge_result;
            }
        }
        xrfg::bridge_flight_logger().end(
            flight_token,
            xrfg::BridgeFlightOperation::gpu_drain,
            result);
    } catch (...) {
        xrfg::bridge_flight_logger().end(
            flight_token,
            xrfg::BridgeFlightOperation::gpu_drain,
            E_FAIL);
    }
}

[[nodiscard]] bool release_private_image(
    SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept {
    try {
        if (image.handle == XR_NULL_HANDLE || image.phase == PrivateOwnershipPhase::idle) {
            return true;
        }
        if (!dispatch || dispatch->wait_swapchain_image == nullptr ||
            dispatch->release_swapchain_image == nullptr) {
            return false;
        }
        if (image.phase == PrivateOwnershipPhase::acquired) {
            XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait_info.timeout = XR_INFINITE_DURATION;
            const auto wait_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::private_swapchain_wait,
                handle_value(image.handle),
                image.acquired_index,
                static_cast<std::uint64_t>(image.phase));
            const XrResult wait_result = with_runtime_entry(session, [&] {
                return dispatch->wait_swapchain_image(image.handle, &wait_info);
            });
            xrfg::bridge_flight_logger().end(
                wait_token,
                xrfg::BridgeFlightOperation::private_swapchain_wait,
                wait_result,
                handle_value(image.handle),
                image.acquired_index,
                static_cast<std::uint64_t>(image.phase));
            if (wait_result != XR_SUCCESS && wait_result != XR_SESSION_LOSS_PENDING) {
                return false;
            }
            image.phase = PrivateOwnershipPhase::waited;
        }
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto release_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_release,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        const XrResult release_result = with_runtime_entry(session, [&] {
            return dispatch->release_swapchain_image(image.handle, &release_info);
        });
        xrfg::bridge_flight_logger().end(
            release_token,
            xrfg::BridgeFlightOperation::private_swapchain_release,
            release_result,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (XR_FAILED(release_result)) {
            image.phase = PrivateOwnershipPhase::release_pending;
            return false;
        }
        image.phase = PrivateOwnershipPhase::idle;
        return true;
    } catch (...) {
        return false;
    }
}

void destroy_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        {
            std::scoped_lock lock(state->mutex);
            generation = std::move(state->frame_generation);
        }
        if (!generation || !state->session || !state->session->dispatch) {
            return;
        }

        const auto& dispatch = state->session->dispatch;
        // A hand-over copy still running would read staging and write an
        // image that are both about to be destroyed. Copies not seen to
        // finish could still run from command lists freed with the copier,
        // so then everything is kept, as the synthesizer and history keep
        // their resources when their work cannot be proven complete. The
        // runtime frees its private images with the session.
        if (generation->copier && !generation->copier->wait_idle()) {
            static_cast<void>(
                new std::shared_ptr<FrameGenerationSwapchainState>(std::move(generation)));
            return;
        }
        for (PrivateSwapchainState& image : generation->synthetic) {
            static_cast<void>(
                release_private_image(state->session.get(), dispatch, image));
        }
        for (PrivateSwapchainState& image : generation->current) {
            static_cast<void>(
                release_private_image(state->session.get(), dispatch, image));
        }
        if (dispatch->destroy_swapchain == nullptr) {
            return;
        }
        // A ring shorter than its maximum leaves the unused slots null, and
        // the loop below already skips those.
        std::array<PrivateSwapchainState*,
                   kPrivateRingSlotMax + kCurrentSlotCount> owned{};
        for (std::size_t slot = 0; slot < kPrivateRingSlotMax; ++slot) {
            owned[slot] = &generation->synthetic[slot];
        }
        for (std::size_t slot = 0; slot < kCurrentSlotCount; ++slot) {
            owned[kPrivateRingSlotMax + slot] = &generation->current[slot];
        }
        for (PrivateSwapchainState* image : owned) {
            if (image->handle == XR_NULL_HANDLE) {
                continue;
            }
            static_cast<void>(dispatch->destroy_swapchain(image->handle));
            image->handle = XR_NULL_HANDLE;
        }
    } catch (...) {
    }
}

[[nodiscard]] std::optional<D3D12_RESOURCE_STATES> required_release_state(
    const SwapchainState& state) noexcept {
    const bool is_color =
        (state.create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    const bool is_depth =
        (state.create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    if (is_color == is_depth) {
        return std::nullopt;
    }
    return is_color ? D3D12_RESOURCE_STATE_RENDER_TARGET
                    : D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

struct CreatedPrivateSwapchain {
    PrivateSwapchainState state;
    std::vector<ID3D12Resource*> d3d12_resources;
    std::vector<ID3D11Texture2D*> d3d11_resources;
    std::vector<VkImage> vulkan_resources;
};

[[nodiscard]] bool create_private_swapchain(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainCreateInfo& create_info,
    CreatedPrivateSwapchain* output,
    XrResult* refusal = nullptr) {
    if (refusal != nullptr) {
        *refusal = XR_SUCCESS;
    }
    if (!state || !state->session || !state->session->dispatch || output == nullptr) {
        return false;
    }
    const auto& dispatch = state->session->dispatch;
    XrSwapchain handle = XR_NULL_HANDLE;
    XrResult result = dispatch->create_swapchain(
        state->session->handle,
        &create_info,
        &handle);
    if (XR_FAILED(result) || handle == XR_NULL_HANDLE) {
        // Which error the runtime gave separates a budget it will not exceed
        // from a create info it rejects outright, and only the first is worth
        // handing the whole session's private swapchains back for.
        if (refusal != nullptr) {
            *refusal = XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE;
        }
        return false;
    }

    std::uint32_t image_count = 0;
    result = dispatch->enumerate_swapchain_images(handle, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        dispatch->destroy_swapchain(handle);
        return false;
    }

    if (state->session->graphics_binding == SessionGraphicsBinding::vulkan) {
        std::vector<XrSwapchainImageVulkanKHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
            image.next = nullptr;
            image.image = VK_NULL_HANDLE;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->vulkan_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR ||
                images[index].image == VK_NULL_HANDLE) {
                dispatch->destroy_swapchain(handle);
                output->vulkan_resources.clear();
                return false;
            }
            output->vulkan_resources[index] = images[index].image;
        }
    } else if (state->session->graphics_binding == SessionGraphicsBinding::d3d11) {
        std::vector<XrSwapchainImageD3D11KHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            image.next = nullptr;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->d3d11_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR ||
                images[index].texture == nullptr) {
                dispatch->destroy_swapchain(handle);
                output->d3d11_resources.clear();
                return false;
            }
            output->d3d11_resources[index] = images[index].texture;
        }
    } else {
        std::vector<XrSwapchainImageD3D12KHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            image.next = nullptr;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->d3d12_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ||
                images[index].texture == nullptr) {
                dispatch->destroy_swapchain(handle);
                output->d3d12_resources.clear();
                return false;
            }
            output->d3d12_resources[index] = images[index].texture;
        }
        log_d3d12_image_description(handle, output->d3d12_resources.front());
    }
    output->state.handle = handle;
    return true;
}

struct CreatedPrivateRing {
    std::array<CreatedPrivateSwapchain, kPrivateRingSlotMax> slots{};
    // Every slot's destination images end to end, in slot order, which is the
    // flat addressing both the synthesizer and the interops expect.
    std::vector<ID3D12Resource*> d3d12_resources;
    std::vector<ID3D11Texture2D*> d3d11_resources;
    std::vector<VkImage> vulkan_resources;
    std::uint32_t images_per_slot{};
    std::size_t slot_count{};
};

void destroy_private_ring(
    const std::shared_ptr<Dispatch>& dispatch,
    CreatedPrivateRing* ring) noexcept {
    if (ring == nullptr || !dispatch || dispatch->destroy_swapchain == nullptr) {
        return;
    }
    for (CreatedPrivateSwapchain& created : ring->slots) {
        if (created.state.handle == XR_NULL_HANDLE) {
            continue;
        }
        static_cast<void>(dispatch->destroy_swapchain(created.state.handle));
        created.state.handle = XR_NULL_HANDLE;
    }
}

[[nodiscard]] bool create_private_ring(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainCreateInfo& create_info,
    std::size_t slot_count,
    CreatedPrivateRing* output,
    XrResult* refusal = nullptr) {
    if (refusal != nullptr) {
        *refusal = XR_SUCCESS;
    }
    if (output == nullptr || slot_count == 0 ||
        slot_count > kPrivateRingSlotMax) {
        return false;
    }
    output->slot_count = slot_count;
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        if (!create_private_swapchain(
                state, create_info, &output->slots[slot], refusal)) {
            return false;
        }
        const CreatedPrivateSwapchain& created = output->slots[slot];
        const std::size_t count = !created.d3d12_resources.empty()
            ? created.d3d12_resources.size()
            : !created.d3d11_resources.empty()
                ? created.d3d11_resources.size()
                : created.vulkan_resources.size();
        // A flat destination index assumes one stride for every slot, so a
        // runtime that hands out different image counts is not usable here.
        if (count == 0 ||
            (slot != 0 && count != output->images_per_slot)) {
            return false;
        }
        output->images_per_slot = static_cast<std::uint32_t>(count);
        output->d3d12_resources.insert(
            output->d3d12_resources.end(),
            created.d3d12_resources.begin(),
            created.d3d12_resources.end());
        output->d3d11_resources.insert(
            output->d3d11_resources.end(),
            created.d3d11_resources.begin(),
            created.d3d11_resources.end());
        output->vulkan_resources.insert(
            output->vulkan_resources.end(),
            created.vulkan_resources.begin(),
            created.vulkan_resources.end());
    }
    return true;
}

// Publishes a created ring into the shared generation state.
void adopt_current_ring(
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    CreatedPrivateRing& ring) noexcept {
    for (std::size_t slot = 0; slot < ring.slot_count; ++slot) {
        generation->current[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->current_images_per_slot = ring.images_per_slot;
    generation->current_slot = 0;
}

void adopt_synthetic_ring(
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    CreatedPrivateRing& ring) noexcept {
    for (std::size_t slot = 0; slot < ring.slot_count; ++slot) {
        generation->synthetic[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->synthetic_images_per_slot = ring.images_per_slot;
    generation->synthetic_slot_count = ring.slot_count;
    generation->synthetic_slot = 0;
}

// How many synthetic slots this session needs. See the comment above
// kCurrentSlotCount: the deeper pipeline admits the application while the
// previous pair's synthetic is still un-retired, so it cannot share one.
// The synthesizer's backend for this session as it stands, and the
// initialisation with the fallback: NVIDIA first when configured, FidelityFX
// when NVIDIA cannot initialise here. `initialize` runs one attempt for the
// backend it is given and returns its HRESULT; the record either side of it
// is the synthesis_initialize B/E pair the creation paths always wrote, and
// the phase-I record between two attempts carries the NVIDIA failure. The
// fallback is silent towards the tray, which keeps showing what the user
// picked; a log always says which backend ran.
struct SynthesisInitializeRecord {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t current_count{};
    std::uint64_t synthetic_count{};
    std::uint32_t array_size{};
};

template <typename Initialize>
[[nodiscard]] HRESULT initialize_synthesis_with_fallback(
    SessionState& session,
    const SynthesisInitializeRecord& record,
    Initialize&& initialize) {
    xrfg::D3D12OpticalFlowBackend backend = session.optical_flow_backend;
    if (backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        session.nvidia_backend_unavailable) {
        backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    }
    const auto attempt = [&](xrfg::D3D12OpticalFlowBackend candidate) {
        const auto token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::synthesis_initialize,
            handle_value(session.handle),
            (static_cast<std::uint64_t>(record.width) << 32) | record.height,
            optical_flow_configuration_code(candidate, session.nvidia_options));
        const HRESULT result = initialize(candidate);
        xrfg::bridge_flight_logger().end(
            token,
            xrfg::BridgeFlightOperation::synthesis_initialize,
            result,
            record.current_count,
            record.synthetic_count,
            record.array_size);
        return result;
    };
    HRESULT result = attempt(backend);
    if (FAILED(result) && backend == xrfg::D3D12OpticalFlowBackend::nvidia) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::synthesis_initialize,
            result,
            handle_value(session.handle),
            1,
            optical_flow_configuration_code(backend, session.nvidia_options));
        session.nvidia_backend_unavailable = true;
        session.optical_flow_backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        result = attempt(xrfg::D3D12OpticalFlowBackend::fidelity_fx);
    }
    return result;
}

[[nodiscard]] std::size_t synthetic_slot_count_for(
    const SessionState& session) noexcept {
    return session.two_slot_synthetic_ring ? kSyntheticSlotCountDeep
                                           : kSyntheticSlotCountShallow;
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d12_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedPrivateRing current;
    CreatedPrivateRing synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& dispatch = state->session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (!is_color || is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 ||
            state->session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            state->session->d3d12_device == nullptr ||
            state->session->d3d12_queue == nullptr ||
            !state->d3d12_history) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            return nullptr;
        }

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |=
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        // A bridged D3D11 game's mipmapped swapchain (Cyberpunk 2077 asks for
        // four): the synthesizer writes mip 0 and takes single-mip outputs
        // only, and the bridge carries mip 0 only, so the private swapchains
        // have one. The direct D3D11 path never met this, because synthesis
        // wrote to the interop's own single-mip textures; a native D3D12
        // game keeps what it asked for.
        if (state->session->d3d11_bridge || state->session->vulkan_bridge) {
            private_info.mipCount = 1;
        }
        XrResult refusal = XR_SUCCESS;
        const bool single_rings = state->session->single_swapchain_rings;
        if (!create_private_ring(
                state, private_info, single_rings ? 1 : kCurrentSlotCount,
                &current, &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private_ring(dispatch, &current);
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring,
            handle_value(state->handle));
        if (!create_private_ring(
                state,
                private_info,
                single_rings ? 1 : synthetic_slot_count_for(*state->session),
                &synthetic,
                &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private_ring(dispatch, &synthetic);
            destroy_private_ring(dispatch, &current);
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring,
            handle_value(state->handle));
        // Single-swapchain rings: the synthesizer's destinations are staging
        // textures described like the runtime's images, so a hand-over copy
        // is a whole-resource copy.
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kStagingSlotCount> current_staging{};
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kStagingSlotCount> synthetic_staging{};
        std::vector<ID3D12Resource*> current_destinations_raw;
        std::vector<ID3D12Resource*> synthetic_destinations_raw;
        std::shared_ptr<HandoverCopier> copier;
        if (single_rings) {
            D3D12_RESOURCE_DESC staging_description =
                current.d3d12_resources.front()->GetDesc();
            staging_description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap_properties{};
            heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap_properties.CreationNodeMask = 1;
            heap_properties.VisibleNodeMask = 1;
            HRESULT staging_result = S_OK;
            for (auto* staging : {&current_staging, &synthetic_staging}) {
                for (auto& texture : *staging) {
                    if (FAILED(staging_result)) {
                        break;
                    }
                    staging_result = state->session->d3d12_device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        &staging_description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()));
                }
            }
            copier = std::make_shared<HandoverCopier>();
            if (SUCCEEDED(staging_result)) {
                staging_result = copier->initialize(state->session->d3d12_device.Get());
            }
            if (FAILED(staging_result)) {
                if (failure_reason != nullptr) {
                    *failure_reason =
                        SwapchainEligibilityReason::synthesis_initialize_failed;
                }
                if (failure_detail != nullptr) {
                    *failure_detail = static_cast<std::uint64_t>(staging_result);
                }
                destroy_private_ring(dispatch, &synthetic);
                destroy_private_ring(dispatch, &current);
                return nullptr;
            }
            for (const auto& texture : current_staging) {
                current_destinations_raw.push_back(texture.Get());
            }
            for (const auto& texture : synthetic_staging) {
                synthetic_destinations_raw.push_back(texture.Get());
            }
            log_video_memory(*state->session, VideoMemoryStage::staging,
                handle_value(state->handle));
        } else {
            current_destinations_raw = current.d3d12_resources;
            synthetic_destinations_raw = synthetic.d3d12_resources;
        }
        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const SynthesisInitializeRecord initialize_record{
            state->create_info.width,
            state->create_info.height,
            current_destinations_raw.size(),
            synthetic_destinations_raw.size(),
            state->create_info.arraySize};
        const HRESULT gpu_result = initialize_synthesis_with_fallback(
            *state->session, initialize_record,
            [&](xrfg::D3D12OpticalFlowBackend backend) {
        return synthesizer->initialize(
            state->session->d3d12_device.Get(),
            state->session->d3d12_synthesis_queue
                ? state->session->d3d12_synthesis_queue.Get()
                : state->session->d3d12_queue.Get(),
            state->d3d12_history,
            std::span<ID3D12Resource* const>(
                current_destinations_raw.data(),
                current_destinations_raw.size()),
            std::span<ID3D12Resource* const>(
                synthetic_destinations_raw.data(),
                synthetic_destinations_raw.size()),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            // These are the layer's own private swapchains, so the layer
            // picks the state they rest in. On a private synthesis queue
            // that has to be COMMON: D3D12 requires a non-simultaneous-
            // access texture to be in COMMON at the point queue ownership
            // transfers, and these transfer twice a pair - written by the
            // synthesis queue, read by the runtime against the queue the
            // application supplied. synchronize_consumer_queue orders the
            // two; it does not transfer ownership, and a fence alone leaves
            // what the reader sees undefined. Observed as synthetic frames
            // that were generated, paced and complete on time, and still
            // did not reach the headset: The Callisto Protocol under UEVR
            // held 45 with 1257 pairs, 99.6% of submissions on grid and the
            // synthetic marker flashing a few times a second rather than
            // forty-five.
            //
            // Resting in COMMON costs nothing. Every transition in the
            // synthesis lists already starts and ends at this state, and a
            // resource in COMMON is implicitly promoted on first use, so a
            // runtime that wants it as a render target still gets one.
            state->session->d3d12_synthesis_queue
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            state->session->nvidia_options,
            xrfg::bridge_flight_logger().enabled());
            });
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            destroy_private_ring(dispatch, &synthetic);
            destroy_private_ring(dispatch, &current);
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthesizer,
            handle_value(state->handle));
        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        if (single_rings) {
            for (ID3D12Resource* image : current.d3d12_resources) {
                generation->current_runtime_images.emplace_back(image);
            }
            for (ID3D12Resource* image : synthetic.d3d12_resources) {
                generation->synthetic_runtime_images.emplace_back(image);
            }
        }
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        if (single_rings) {
            generation->single_swapchain_rings = true;
            generation->current_staging = std::move(current_staging);
            generation->synthetic_staging = std::move(synthetic_staging);
            generation->copier = std::move(copier);
            // Destination indices are staging slots.
            generation->current_images_per_slot = 1;
            generation->synthetic_images_per_slot = 1;
            generation->synthetic_slot_count = kStagingSlotCount;
        }
        // 703: the rings' shape. a 1 for single-swapchain rings, b the
        // runtime images per output, c the swapchain.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            703,
            single_rings ? 1u : 0u,
            generation->current_runtime_images.size(),
            handle_value(state->handle));
        generation->synthesizer = std::move(synthesizer);
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            destroy_private_ring(state->session->dispatch, &synthetic);
            destroy_private_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d11_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    std::span<ID3D11Texture2D* const> application_images,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedPrivateRing current;
    CreatedPrivateRing synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& session = state->session;
        const auto& dispatch = session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (!is_color || is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 || application_images.empty() ||
            session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            session->d3d11_device == nullptr ||
            session->d3d11_context == nullptr ||
            session->d3d12_device == nullptr ||
            session->d3d12_queue == nullptr) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            return nullptr;
        }

        const auto destroy_private = [&]() noexcept {
            destroy_private_ring(dispatch, &synthetic);
            destroy_private_ring(dispatch, &current);
        };

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrResult refusal = XR_SUCCESS;
        if (!create_private_ring(
                state, private_info, kCurrentSlotCount, &current, &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring,
            handle_value(state->handle));
        if (!create_private_ring(
                state,
                private_info,
                synthetic_slot_count_for(*session),
                &synthetic,
                &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring,
            handle_value(state->handle));
        auto interop =
            std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
        xrfg::D3D11InteropInitializationStage interop_failure_stage =
            xrfg::D3D11InteropInitializationStage::complete;
        HRESULT gpu_result = interop->initialize(
            session->d3d11_device.Get(),
            session->d3d11_context.Get(),
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            application_images,
            std::span<ID3D11Texture2D* const>(
                current.d3d11_resources.data(),
                current.d3d11_resources.size()),
            std::span<ID3D11Texture2D* const>(
                synthetic.d3d11_resources.data(),
                synthetic.d3d11_resources.size()),
            &interop_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::d3d11_interop_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(interop_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::interop,
            handle_value(state->handle));
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        xrfg::D3D12HistoryInitializationStage history_failure_stage =
            xrfg::D3D12HistoryInitializationStage::complete;
        gpu_result = history->initialize(
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            interop->source_images(),
            D3D12_RESOURCE_STATE_COMMON,
            &history_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::history_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(history_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::history,
            handle_value(state->handle));
        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const SynthesisInitializeRecord initialize_record{
            state->create_info.width,
            state->create_info.height,
            interop->current_destination_images().size(),
            interop->synthetic_destination_images().size(),
            state->create_info.arraySize};
        gpu_result = initialize_synthesis_with_fallback(
            *session, initialize_record,
            [&](xrfg::D3D12OpticalFlowBackend backend) {
                return synthesizer->initialize(
                    session->d3d12_device.Get(),
                    session->d3d12_queue.Get(),
                    history,
                    interop->current_destination_images(),
                    interop->synthetic_destination_images(),
                    static_cast<DXGI_FORMAT>(state->create_info.format),
                    D3D12_RESOURCE_STATE_COMMON,
                    backend,
                    session->nvidia_options,
                    xrfg::bridge_flight_logger().enabled());
            });
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthesizer,
            handle_value(state->handle));
        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        generation->synthesizer = std::move(synthesizer);
        generation->interop = std::move(interop);
        {
            std::scoped_lock lock(state->mutex);
            state->d3d12_history = std::move(history);
        }
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            destroy_private_ring(state->session->dispatch, &synthetic);
            destroy_private_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

// A refused private swapchain is the only failure that says anything about the
// rest of the session: whatever ceiling the runtime reached, the application's
// own creations are competing for it. Synthesis, interop and an unusable image
// are local to the one swapchain that hit them.
[[nodiscard]] bool budget_refusal(SwapchainEligibilityReason reason) noexcept {
    return reason ==
               SwapchainEligibilityReason::current_private_swapchain_failed ||
           reason ==
               SwapchainEligibilityReason::synthetic_private_swapchain_failed;
}

// Hands every private swapchain in the session back to the runtime.
//
// The caller must own the frame call mutex and, when a presenter is running,
// have drained it and taken the content lock first: a queued submission names
// these swapchains, and destroying one the runtime still has queued latches a
// presenter failure for the rest of the session.
void release_session_generation_budget(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        if (!session) {
            return;
        }
        {
            // The retained repeat names private swapchains of its own.
            std::scoped_lock lock(session->presenter_mutex);
            session->presenter_last_frame.reset();
        }
        for (const auto& swapchain : find_swapchains(session)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            swapchain->generation_eligible_pending = false;
            swapchain->generation_declined = true;
            drain_swapchain_gpu(swapchain);
            destroy_frame_generation_swapchains(swapchain);
        }
    } catch (...) {
    }
}

// Creates the generation resources a swapchain deferred at enumeration time.
// Returns false only when the runtime refused a private swapchain, which is
// the caller's signal to hand the session's whole budget back.
[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_vulkan_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    std::span<const VkImage> application_images,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedPrivateRing current;
    CreatedPrivateRing synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& session = state->session;
        const auto& dispatch = session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        // In a Vulkan session the swapchain format is a VkFormat; the
        // synthesizer works in the D3D12 format the shared textures take.
        const auto vulkan_format =
            static_cast<VkFormat>(state->create_info.format);
        const DXGI_FORMAT dxgi_format =
            xrfg::dxgi_format_for_vulkan(vulkan_format);
        // A Vulkan application need not render into the swapchain image:
        // OpenComposite copies the game's own texture in, so No Man's Sky's
        // swapchains carry only TRANSFER_DST. Any non-depth swapchain is a
        // colour one here; the layer's private rings add the attachment bit
        // for themselves.
        static_cast<void>(is_color);
        if (is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 || application_images.empty() ||
            session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            session->vulkan_binding.device == VK_NULL_HANDLE ||
            session->d3d12_device == nullptr ||
            session->d3d12_queue == nullptr ||
            dxgi_format == DXGI_FORMAT_UNKNOWN) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : dxgi_format == DXGI_FORMAT_UNKNOWN
                                ? SwapchainEligibilityReason::unsupported_vulkan_format
                                : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            if (failure_detail != nullptr && dxgi_format == DXGI_FORMAT_UNKNOWN) {
                *failure_detail = static_cast<std::uint64_t>(state->create_info.format);
            }
            return nullptr;
        }

        const auto destroy_private = [&]() noexcept {
            destroy_private_ring(dispatch, &synthetic);
            destroy_private_ring(dispatch, &current);
        };

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrResult refusal = XR_SUCCESS;
        if (!create_private_ring(
                state, private_info, kCurrentSlotCount, &current, &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring,
            handle_value(state->handle));
        if (!create_private_ring(
                state,
                private_info,
                synthetic_slot_count_for(*session),
                &synthetic,
                &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }

        // Vulkan cannot describe an image after the fact; the create info
        // the application's swapchain was made with is the description, and
        // the private swapchains were made from the same one.
        xrfg::VulkanImageDescription description{};
        description.format = vulkan_format;
        description.width = state->create_info.width;
        description.height = state->create_info.height;
        description.array_size = state->create_info.arraySize;
        description.mip_levels = state->create_info.mipCount;
        description.sample_count = state->create_info.sampleCount;
        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring,
            handle_value(state->handle));
        auto interop = std::make_shared<xrfg::VulkanD3D12SwapchainInterop>();
        xrfg::VulkanInteropInitializationStage interop_failure_stage =
            xrfg::VulkanInteropInitializationStage::complete;
        HRESULT gpu_result = interop->initialize(
            session->vulkan_binding,
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            description,
            application_images,
            std::span<const VkImage>(
                current.vulkan_resources.data(),
                current.vulkan_resources.size()),
            std::span<const VkImage>(
                synthetic.vulkan_resources.data(),
                synthetic.vulkan_resources.size()),
            &interop_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::vulkan_interop_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(interop_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::interop,
            handle_value(state->handle));
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        xrfg::D3D12HistoryInitializationStage history_failure_stage =
            xrfg::D3D12HistoryInitializationStage::complete;
        gpu_result = history->initialize(
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            interop->source_images(),
            D3D12_RESOURCE_STATE_COMMON,
            &history_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::history_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(history_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::history,
            handle_value(state->handle));
        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const SynthesisInitializeRecord initialize_record{
            state->create_info.width,
            state->create_info.height,
            interop->current_destination_images().size(),
            interop->synthetic_destination_images().size(),
            state->create_info.arraySize};
        gpu_result = initialize_synthesis_with_fallback(
            *session, initialize_record,
            [&](xrfg::D3D12OpticalFlowBackend backend) {
                return synthesizer->initialize(
                    session->d3d12_device.Get(),
                    session->d3d12_queue.Get(),
                    history,
                    interop->current_destination_images(),
                    interop->synthetic_destination_images(),
                    dxgi_format,
                    D3D12_RESOURCE_STATE_COMMON,
                    backend,
                    session->nvidia_options,
                    xrfg::bridge_flight_logger().enabled());
            });
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthesizer,
            handle_value(state->handle));
        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        generation->synthesizer = std::move(synthesizer);
        generation->interop = std::move(interop);
        {
            std::scoped_lock lock(state->mutex);
            state->d3d12_history = std::move(history);
        }
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch) {
            destroy_private_ring(state->session->dispatch, &synthetic);
            destroy_private_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

[[nodiscard]] bool ensure_frame_generation(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        if (!state || !state->session) {
            return true;
        }
        std::scoped_lock call_lock(state->call_mutex);
        if (!state->generation_eligible_pending || state->generation_declined) {
            return true;
        }
        {
            std::scoped_lock lock(state->mutex);
            if (state->frame_generation) {
                state->generation_eligible_pending = false;
                return true;
            }
        }
        const std::uint64_t auxiliary =
            (static_cast<std::uint64_t>(state->enumerated_image_count) << 32) |
            state->create_info.arraySize;
        SwapchainEligibilityReason reason =
            SwapchainEligibilityReason::exception;
        std::uint64_t detail = 0;
        // Enumeration ran before the application had submitted anything, so
        // nothing else was touching the queue. This runs on the frame path,
        // where a release on another thread may be capturing into history and
        // synthesizer initialisation submits work of its own.
        std::shared_ptr<FrameGenerationSwapchainState> candidate;
        {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            if (state->session->graphics_binding ==
                SessionGraphicsBinding::d3d11) {
                // Ask the runtime for the images again rather than holding a
                // reference to each of them from enumeration until whenever
                // the application first composites with this swapchain. Those
                // are the application's textures: keeping them alive here
                // outlives what the layer is entitled to hold, and an
                // application that exits without destroying its swapchains
                // then releases them during teardown, which hung the D3D11
                // call chain tests at process exit. xrEnumerateSwapchainImages
                // may be called as often as we like.
                std::vector<XrSwapchainImageD3D11KHR> enumerated(
                    state->enumerated_image_count,
                    XrSwapchainImageD3D11KHR{
                        XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, nullptr});
                std::uint32_t count = 0;
                const XrResult enumerate_result =
                    state->session->dispatch->enumerate_swapchain_images(
                        state->handle,
                        state->enumerated_image_count,
                        &count,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(
                            enumerated.data()));
                std::vector<ID3D11Texture2D*> images;
                if (XR_SUCCEEDED(enumerate_result) &&
                    count == state->enumerated_image_count) {
                    images.reserve(count);
                    for (const XrSwapchainImageD3D11KHR& image : enumerated) {
                        images.push_back(image.texture);
                    }
                }
                if (images.empty()) {
                    reason = SwapchainEligibilityReason::invalid_d3d11_image;
                    detail = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(enumerate_result));
                } else {
                    candidate = create_d3d11_frame_generation_swapchains(
                        state,
                        std::span<ID3D11Texture2D* const>(
                            images.data(), images.size()),
                        &reason,
                        &detail);
                }
            } else if (state->session->graphics_binding ==
                       SessionGraphicsBinding::vulkan) {
                // Re-enumerated for the same reason as D3D11's above: the
                // images are the application's.
                std::vector<XrSwapchainImageVulkanKHR> enumerated(
                    state->enumerated_image_count,
                    XrSwapchainImageVulkanKHR{
                        XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr,
                        VK_NULL_HANDLE});
                std::uint32_t count = 0;
                const XrResult enumerate_result =
                    state->session->dispatch->enumerate_swapchain_images(
                        state->handle,
                        state->enumerated_image_count,
                        &count,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(
                            enumerated.data()));
                std::vector<VkImage> images;
                if (XR_SUCCEEDED(enumerate_result) &&
                    count == state->enumerated_image_count) {
                    images.reserve(count);
                    for (const XrSwapchainImageVulkanKHR& image : enumerated) {
                        if (image.image != VK_NULL_HANDLE) {
                            images.push_back(image.image);
                        }
                    }
                }
                if (images.size() != state->enumerated_image_count) {
                    images.clear();
                }
                if (images.empty()) {
                    reason = SwapchainEligibilityReason::invalid_vulkan_image;
                    detail = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(enumerate_result));
                } else {
                    candidate = create_vulkan_frame_generation_swapchains(
                        state,
                        std::span<const VkImage>(images.data(), images.size()),
                        &reason,
                        &detail);
                }
            } else {
                candidate = create_d3d12_frame_generation_swapchains(
                    state, &reason, &detail);
            }
        }
        if (!candidate) {
            state->generation_declined = true;
            log_swapchain_eligibility(state, reason, detail, auxiliary);
            return !budget_refusal(reason);
        }
        {
            std::scoped_lock lock(state->mutex);
            state->frame_generation = std::move(candidate);
        }
        state->generation_eligible_pending = false;
        log_swapchain_eligibility(
            state,
            SwapchainEligibilityReason::ready,
            state->enumerated_image_count,
            auxiliary);
        return true;
    } catch (...) {
        return true;
    }
}

// The deeper pipeline costs four private swapchains per application
// swapchain against the shallow one's three, and a runtime caps the session:
// SteamVR at 16. Ghostwire Tokyo under UEVR with depth submission creates
// eight of its own, so the second eye's synthetic ring was the sixteenth and
// SteamVR refused it, which latched the budget and passed the whole session
// through. Nothing has been generated when arming fails, so the depth is
// still free to change: every private ring made so far is released, the
// session drops to the shallow depth, and arming runs once more. Only
// without a presenter, which would hold submissions naming those rings.
// 3X costs the same four and falls back the same way, to one synthetic.
[[nodiscard]] bool fall_back_to_shallow_pipeline(
    const std::shared_ptr<SessionState>& session,
    const std::shared_ptr<SwapchainState>& refused,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    try {
        if (!session ||
            (!session->deep_pipeline &&
             session->frames_per_application_frame <= 2) ||
            continuous_presenter_active(session)) {
            return false;
        }
        log_swapchain_eligibility(
            refused,
            SwapchainEligibilityReason::deep_pipeline_fallback,
            0,
            handle_value(session->handle));
        for (const auto& swapchain : find_swapchains(session)) {
            drain_swapchain_gpu(swapchain);
            destroy_frame_generation_swapchains(swapchain);
            std::scoped_lock call_lock(swapchain->call_mutex);
            std::scoped_lock lock(swapchain->mutex);
            // The next capture is a new ring's first; a serial from the old
            // one would only refuse the pair.
            swapchain->last_released_capture.reset();
            swapchain->last_released_motion_vectors.reset();
        }
        session->deep_pipeline = false;
        session->frames_per_application_frame = 2;
        session->two_slot_synthetic_ring = false;
        session->shallow_fallback = true;
        if (session->triple_switchable) {
            session->fixed_frame_multiplier.hold();
        }
        session->triple_switchable = false;
        for (const ProjectionResourceMapping& mapping : mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) {
                continue;
            }
            std::scoped_lock call_lock(swapchain->call_mutex);
            swapchain->generation_declined = false;
            swapchain->generation_eligible_pending = true;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// Spends the generation budget on the swapchains this frame actually submits
// as projection views. Returns false once the runtime has refused one, meaning
// the caller must release the session's budget and stop generating.
[[nodiscard]] bool ensure_projection_frame_generation(
    const std::shared_ptr<SessionState>& session,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    if (!session ||
        session->generation_budget_exhausted.load(std::memory_order_acquire)) {
        return true;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool refused = false;
        for (const ProjectionResourceMapping& mapping : mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain || ensure_frame_generation(swapchain)) {
                continue;
            }
            if (attempt == 0 &&
                fall_back_to_shallow_pipeline(session, swapchain, mappings)) {
                refused = true;
                break;
            }
            session->generation_budget_exhausted.store(
                true, std::memory_order_release);
            log_swapchain_eligibility(
                swapchain,
                SwapchainEligibilityReason::budget_exhausted,
                0,
                handle_value(session->handle));
            return false;
        }
        if (!refused) {
            return true;
        }
    }
    return true;
}

// Whether any swapchain this frame submits as a projection view still owes the
// work ensure_projection_frame_generation would do. Cheap: two locks per
// mapping and no allocation, and it answers true at most once per swapchain
// for the life of a session.
[[nodiscard]] bool projection_frame_generation_pending(
    const std::shared_ptr<SessionState>& session,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    try {
        if (!session || session->generation_budget_exhausted.load(
                            std::memory_order_acquire)) {
            return false;
        }
        for (const ProjectionResourceMapping& mapping : mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) {
                continue;
            }
            std::scoped_lock call_lock(swapchain->call_mutex);
            if (swapchain->generation_eligible_pending &&
                !swapchain->generation_declined) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

template <typename Function>
[[nodiscard]] bool load_function(
    PFN_xrGetInstanceProcAddr get_instance_proc_addr,
    XrInstance instance,
    const char* name,
    Function& output) {
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = get_instance_proc_addr(instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        output = nullptr;
        return false;
    }
    output = reinterpret_cast<Function>(function);
    return true;
}

// view_configuration, once per view at session creation.
void log_view_configuration(
    const Dispatch& dispatch, XrInstance instance, XrSystemId system_id) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().keeps_session_records() ||
            system_id == XR_NULL_SYSTEM_ID) {
            return;
        }
        PFN_xrEnumerateViewConfigurationViews enumerate = nullptr;
        if (!load_function(
                dispatch.get_instance_proc_addr, instance,
                "xrEnumerateViewConfigurationViews", enumerate)) {
            return;
        }
        std::array<XrViewConfigurationView, 4> views{};
        for (auto& view : views) {
            view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        }
        std::uint32_t count = 0;
        if (XR_FAILED(enumerate(
                instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                static_cast<std::uint32_t>(views.size()), &count, views.data()))) {
            return;
        }
        for (std::uint32_t index = 0; index < std::min<std::uint32_t>(count, 4); ++index) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::view_configuration,
                index,
                (static_cast<std::uint64_t>(views[index].recommendedImageRectWidth) << 32) |
                    views[index].recommendedImageRectHeight,
                (static_cast<std::uint64_t>(views[index].maxImageRectWidth) << 32) |
                    views[index].maxImageRectHeight,
                views[index].recommendedSwapchainSampleCount);
        }
    } catch (...) {
    }
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL layer_begin_session(
    XrSession session,
    const XrSessionBeginInfo* begin_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_end_session(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL layer_wait_frame(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state);
XRAPI_ATTR XrResult XRAPI_CALL layer_begin_frame(
    XrSession session,
    const XrFrameBeginInfo* begin_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_end_frame(
    XrSession session,
    const XrFrameEndInfo* end_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_swapchain(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain swapchain);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space);
XRAPI_ATTR XrResult XRAPI_CALL layer_suggest_interaction_profile_bindings(
    XrInstance instance,
    const XrInteractionProfileSuggestedBinding* suggested_bindings);
XRAPI_ATTR XrResult XRAPI_CALL layer_attach_session_action_sets(
    XrSession session,
    const XrSessionActionSetsAttachInfo* attach_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_sync_actions(
    XrSession session,
    const XrActionsSyncInfo* sync_info);
// The runtime's list with the sharing extensions a D3D12 interop needs added
// where it lacks them. SteamVR asks for external memory and timeline
// semaphores but not for exporting a semaphore to D3D12, so a list from it
// gains VK_KHR_external_semaphore_win32 (device) and
// VK_KHR_external_semaphore_capabilities (instance). Every Windows driver
// that implements external memory implements these too.
[[nodiscard]] std::string augment_vulkan_extension_list(
    std::string_view list,
    bool device_list,
    std::uint64_t* appended_bits) {
    std::string augmented(list);
    const VulkanExtensionSummary present =
        summarize_vulkan_extension_string(list);
    const auto append = [&](std::string_view name) {
        const std::uint64_t bit = vulkan_interop_extension_bit(name);
        if ((present.bits & bit) != 0) {
            return;
        }
        if (!augmented.empty()) {
            augmented.push_back(' ');
        }
        augmented.append(name);
        *appended_bits |= bit;
    };
    if (device_list) {
        append("VK_KHR_external_semaphore_win32");
    } else {
        append("VK_KHR_external_semaphore_capabilities");
    }
    return augmented;
}

XrResult get_vulkan_extensions_recorded(
    PFN_GetVulkanExtensions next,
    std::int64_t selector,
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    if (next == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (count == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Read the runtime's whole list whatever the application asked for, so
    // the size query and the fill call both answer for the augmented list.
    std::uint32_t runtime_count = 0;
    XrResult result = next(instance, system_id, 0, &runtime_count, nullptr);
    if (XR_FAILED(result) || runtime_count == 0) {
        return next(instance, system_id, capacity, count, buffer);
    }
    std::string runtime_list(runtime_count, '\0');
    result = next(
        instance, system_id, runtime_count, &runtime_count, runtime_list.data());
    if (XR_FAILED(result)) {
        return next(instance, system_id, capacity, count, buffer);
    }
    runtime_list.resize(runtime_count > 0 ? runtime_count - 1 : 0);
    std::uint64_t appended_bits = 0;
    const std::string augmented = augment_vulkan_extension_list(
        runtime_list, selector == 3, &appended_bits);
    *count = static_cast<std::uint32_t>(augmented.size() + 1);
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (buffer == nullptr || capacity < *count) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, augmented.c_str(), augmented.size() + 1);
    // Only the call that fills the buffer is recorded; the size query before
    // it would record the same list twice.
    const VulkanExtensionSummary summary =
        summarize_vulkan_extension_string(runtime_list);
    log_vulkan_negotiation(
        selector, summary.count, summary.bits, runtime_list.size());
    log_vulkan_negotiation(10, static_cast<std::uint64_t>(selector), appended_bits);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_instance_extensions(
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_extensions_recorded(
            dispatch ? dispatch->vulkan.get_instance_extensions : nullptr,
            2, instance, system_id, capacity, count, buffer);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_device_extensions(
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_extensions_recorded(
            dispatch ? dispatch->vulkan.get_device_extensions : nullptr,
            3, instance, system_id, capacity, count, buffer);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_vulkan_instance(
    XrInstance instance,
    const XrVulkanInstanceCreateInfoKHR* create_info,
    VkInstance* vulkan_instance,
    VkResult* vulkan_result) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.create_instance == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->vulkan.create_instance(
            instance, create_info, vulkan_instance, vulkan_result);
        VulkanExtensionSummary summary{};
        if (create_info != nullptr && create_info->vulkanCreateInfo != nullptr) {
            summary = summarize_vulkan_extension_array(
                create_info->vulkanCreateInfo->ppEnabledExtensionNames,
                create_info->vulkanCreateInfo->enabledExtensionCount);
        }
        log_vulkan_negotiation(
            4, summary.count, summary.bits,
            pack_negotiation_results(
                result, vulkan_result != nullptr ? *vulkan_result : VK_SUCCESS));
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_vulkan_device(
    XrInstance instance,
    const XrVulkanDeviceCreateInfoKHR* create_info,
    VkDevice* vulkan_device,
    VkResult* vulkan_result) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.create_device == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->vulkan.create_device(
            instance, create_info, vulkan_device, vulkan_result);
        const VkDeviceCreateInfo* device_info =
            create_info != nullptr ? create_info->vulkanCreateInfo : nullptr;
        VulkanExtensionSummary summary{};
        if (device_info != nullptr) {
            summary = summarize_vulkan_extension_array(
                device_info->ppEnabledExtensionNames,
                device_info->enabledExtensionCount);
        }
        log_vulkan_negotiation(
            5, summary.count, summary.bits,
            pack_negotiation_results(
                result, vulkan_result != nullptr ? *vulkan_result : VK_SUCCESS));
        if (device_info != nullptr && device_info->pQueueCreateInfos != nullptr) {
            for (std::uint32_t index = 0;
                 index < device_info->queueCreateInfoCount;
                 ++index) {
                const auto& queue = device_info->pQueueCreateInfos[index];
                log_vulkan_negotiation(
                    6, queue.queueFamilyIndex, queue.queueCount);
            }
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_device(
    XrInstance instance,
    XrSystemId system_id,
    VkInstance vulkan_instance,
    VkPhysicalDevice* physical_device) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.get_graphics_device == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        log_vulkan_negotiation(7, 1);
        return dispatch->vulkan.get_graphics_device(
            instance, system_id, vulkan_instance, physical_device);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_device2(
    XrInstance instance,
    const XrVulkanGraphicsDeviceGetInfoKHR* get_info,
    VkPhysicalDevice* physical_device) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.get_graphics_device2 == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        log_vulkan_negotiation(7, 2);
        return dispatch->vulkan.get_graphics_device2(
            instance, get_info, physical_device);
    });
}

XrResult get_vulkan_requirements_recorded(
    PFN_GetVulkanGraphicsRequirements next,
    std::uint64_t which,
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    if (next == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = next(instance, system_id, requirements);
    if (XR_SUCCEEDED(result) && requirements != nullptr) {
        log_vulkan_negotiation(
            8, which, requirements->minApiVersionSupported,
            requirements->maxApiVersionSupported);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_requirements(
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_requirements_recorded(
            dispatch ? dispatch->vulkan.get_graphics_requirements : nullptr,
            1, instance, system_id, requirements);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_requirements2(
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_requirements_recorded(
            dispatch ? dispatch->vulkan.get_graphics_requirements2 : nullptr,
            2, instance, system_id, requirements);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_poll_event(
    XrInstance instance,
    XrEventDataBuffer* event_data);
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_formats(
    XrSession session,
    std::uint32_t format_capacity_input,
    std::uint32_t* format_count_output,
    std::int64_t* formats);
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images);
XRAPI_ATTR XrResult XRAPI_CALL layer_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index);
XRAPI_ATTR XrResult XRAPI_CALL layer_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info);

template <typename Function>
XrResult expose_intercept(
    const std::shared_ptr<Dispatch>& dispatch,
    Function next_function,
    Function layer_function,
    PFN_xrVoidFunction* output) {
    if (!dispatch || next_function == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    *output = reinterpret_cast<PFN_xrVoidFunction>(layer_function);
    return XR_SUCCESS;
}

// The loader is the module that exports the two global entry points
// without negotiating as a layer or a runtime. Looked up by name first,
// then by exports, for the applications that carry it under another name.
[[nodiscard]] PFN_xrGetInstanceProcAddr find_loader_get_instance_proc_addr() noexcept {
    const auto loader_entry = [](HMODULE module) noexcept -> PFN_xrGetInstanceProcAddr {
        using optiscaler_bootstrap::find_export;
        if (module == nullptr ||
            find_export(module, "xrNegotiateLoaderApiLayerInterface") != nullptr ||
            find_export(module, "xrNegotiateLoaderRuntimeInterface") != nullptr ||
            find_export(module, "xrEnumerateApiLayerProperties") == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<PFN_xrGetInstanceProcAddr>(
            find_export(module, "xrGetInstanceProcAddr"));
    };
    if (const auto entry = loader_entry(GetModuleHandleW(L"openxr_loader.dll"))) {
        return entry;
    }
    std::vector<HMODULE> modules(256);
    DWORD bytes = 0;
    using optiscaler_bootstrap::enumerate_modules;
    if (!enumerate_modules(
            modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
        return nullptr;
    }
    if (bytes > modules.size() * sizeof(HMODULE)) {
        modules.resize(bytes / sizeof(HMODULE));
        if (!enumerate_modules(
                modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
            return nullptr;
        }
    }
    const std::size_t count = std::min(modules.size(), static_cast<std::size_t>(bytes / sizeof(HMODULE)));
    for (std::size_t index = 0; index < count; ++index) {
        if (const auto entry = loader_entry(modules[index])) {
            return entry;
        }
    }
    return nullptr;
}

// A layer above this one may ask for the global functions before any
// instance exists. Cheeky Foveated DLSS's layer probes
// xrEnumerateInstanceExtensionProperties through the next
// xrGetInstanceProcAddr with XR_NULL_HANDLE, the way the loader probes a
// runtime, to decide whether to enable XR_EXT_eye_gaze_interaction; a
// runtime answers, and answering "unsupported" here made it conclude the
// headset had no eye tracking whenever this layer sat beneath it. There is
// no next entry point to forward to yet - the loader hands one over only
// with xrCreateApiLayerInstance - so the global functions come from the
// loader itself, which is what the application reaches with the same call.
XrResult resolve_global_function(const char* name, PFN_xrVoidFunction* function) noexcept {
    if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") != 0 &&
        std::strcmp(name, "xrEnumerateApiLayerProperties") != 0) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const PFN_xrGetInstanceProcAddr loader = find_loader_get_instance_proc_addr();
    if (loader == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = loader(XR_NULL_HANDLE, name, function);
    if (XR_FAILED(result) || *function == nullptr) {
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

XrResult layer_get_instance_proc_addr_impl(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    if (name == nullptr || function == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;

    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(layer_get_instance_proc_addr);
        return XR_SUCCESS;
    }

    if (instance == XR_NULL_HANDLE) {
        return resolve_global_function(name, function);
    }
    const auto dispatch = find_dispatch(instance);
    if (!dispatch) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (std::strcmp(name, "xrDestroyInstance") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_instance, layer_destroy_instance, function);
    }
    if (std::strcmp(name, "xrCreateSession") == 0) {
        return expose_intercept(dispatch, dispatch->create_session, layer_create_session, function);
    }
    if (std::strcmp(name, "xrDestroySession") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_session, layer_destroy_session, function);
    }
    if (std::strcmp(name, "xrBeginSession") == 0) {
        return expose_intercept(dispatch, dispatch->begin_session, layer_begin_session, function);
    }
    if (std::strcmp(name, "xrEndSession") == 0) {
        return expose_intercept(dispatch, dispatch->end_session, layer_end_session, function);
    }
    if (std::strcmp(name, "xrWaitFrame") == 0) {
        return expose_intercept(dispatch, dispatch->wait_frame, layer_wait_frame, function);
    }
    if (std::strcmp(name, "xrBeginFrame") == 0) {
        return expose_intercept(dispatch, dispatch->begin_frame, layer_begin_frame, function);
    }
    if (std::strcmp(name, "xrEndFrame") == 0) {
        return expose_intercept(dispatch, dispatch->end_frame, layer_end_frame, function);
    }
    if (std::strcmp(name, "xrCreateSwapchain") == 0) {
        return expose_intercept(dispatch, dispatch->create_swapchain, layer_create_swapchain, function);
    }
    if (std::strcmp(name, "xrDestroySwapchain") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_swapchain, layer_destroy_swapchain, function);
    }
    if (std::strcmp(name, "xrDestroySpace") == 0) {
        return expose_intercept(
            dispatch, dispatch->destroy_space, layer_destroy_space, function);
    }
    if (std::strcmp(name, "xrPollEvent") == 0) {
        return expose_intercept(
            dispatch, dispatch->poll_event, layer_poll_event, function);
    }
    if (std::strcmp(name, "xrEnumerateSwapchainFormats") == 0) {
        return expose_intercept(
            dispatch, dispatch->enumerate_swapchain_formats,
            layer_enumerate_swapchain_formats, function);
    }
    if (std::strcmp(name, "xrEnumerateSwapchainImages") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->enumerate_swapchain_images,
            layer_enumerate_swapchain_images,
            function);
    }
    if (std::strcmp(name, "xrAcquireSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->acquire_swapchain_image,
            layer_acquire_swapchain_image,
            function);
    }
    if (std::strcmp(name, "xrWaitSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->wait_swapchain_image,
            layer_wait_swapchain_image,
            function);
    }
    if (std::strcmp(name, "xrReleaseSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->release_swapchain_image,
            layer_release_swapchain_image,
            function);
    }
    // The status panel's flip gesture adds to the application's input only
    // where its action set was made; everywhere else these three go straight
    // to the runtime, untouched.
    if (dispatch->panel_gesture.action_set != XR_NULL_HANDLE) {
        const auto& gesture = dispatch->panel_gesture;
        if (std::strcmp(name, "xrSuggestInteractionProfileBindings") == 0) {
            return expose_intercept(dispatch, gesture.suggest_bindings,
                layer_suggest_interaction_profile_bindings, function);
        }
        if (std::strcmp(name, "xrAttachSessionActionSets") == 0) {
            return expose_intercept(dispatch, gesture.attach_action_sets,
                layer_attach_session_action_sets, function);
        }
        if (std::strcmp(name, "xrSyncActions") == 0) {
            return expose_intercept(dispatch, gesture.sync_actions, layer_sync_actions, function);
        }
    }

    // Vulkan negotiation: recorded, then forwarded unchanged. Only offered
    // where the runtime has the function, so the application sees exactly
    // the extensions it would without the layer.
    const struct {
        const char* name;
        PFN_xrVoidFunction next;
        PFN_xrVoidFunction layer;
    } vulkan_intercepts[] = {
        {"xrGetVulkanInstanceExtensionsKHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.get_instance_extensions),
         reinterpret_cast<PFN_xrVoidFunction>(layer_get_vulkan_instance_extensions)},
        {"xrGetVulkanDeviceExtensionsKHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.get_device_extensions),
         reinterpret_cast<PFN_xrVoidFunction>(layer_get_vulkan_device_extensions)},
        {"xrCreateVulkanInstanceKHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.create_instance),
         reinterpret_cast<PFN_xrVoidFunction>(layer_create_vulkan_instance)},
        {"xrCreateVulkanDeviceKHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.create_device),
         reinterpret_cast<PFN_xrVoidFunction>(layer_create_vulkan_device)},
        {"xrGetVulkanGraphicsDeviceKHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.get_graphics_device),
         reinterpret_cast<PFN_xrVoidFunction>(layer_get_vulkan_graphics_device)},
        {"xrGetVulkanGraphicsDevice2KHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.get_graphics_device2),
         reinterpret_cast<PFN_xrVoidFunction>(layer_get_vulkan_graphics_device2)},
        {"xrGetVulkanGraphicsRequirementsKHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.get_graphics_requirements),
         reinterpret_cast<PFN_xrVoidFunction>(layer_get_vulkan_graphics_requirements)},
        {"xrGetVulkanGraphicsRequirements2KHR",
         reinterpret_cast<PFN_xrVoidFunction>(dispatch->vulkan.get_graphics_requirements2),
         reinterpret_cast<PFN_xrVoidFunction>(layer_get_vulkan_graphics_requirements2)},
    };
    for (const auto& intercept : vulkan_intercepts) {
        if (std::strcmp(name, intercept.name) == 0 && intercept.next != nullptr) {
            *function = intercept.layer;
            return XR_SUCCESS;
        }
    }

    return dispatch->get_instance_proc_addr(instance, name, function);
}

// `[overlay] panel`, as the instance is made: whether the layer adds its
// grip action to the application's input is decided then, because the
// application suggests its bindings and attaches its action sets once, before
// its first frame. The overlay follows the setting live after that.
[[nodiscard]] xrfg::StatusPanelMode read_status_panel_mode() noexcept {
    try {
        std::array<wchar_t, 32> value{};
        const auto ini = current_layer_directory() / L"ofxr_bridge.ini";
        GetPrivateProfileStringW(L"overlay", L"panel", L"gesture", value.data(),
            static_cast<DWORD>(value.size()), ini.c_str());
        std::string text;
        for (const wchar_t c : value) {
            if (c == L'\0') break;
            text += c < 128 ? static_cast<char>(c) : '?';
        }
        return xrfg::parse_status_panel_mode(text);
    } catch (...) {
        return xrfg::StatusPanelMode::off;
    }
}

// The status panel's flip gesture needs the controllers' poses, and an API
// layer has no input of its own: an action set reaches the runtime only
// through the application's suggest, attach and sync calls, and only before
// the application has attached. So, as xrFPS does, the layer makes an action
// set of its own now, one pose action on both grips, and adds it to each of
// those calls (layer_suggest_interaction_profile_bindings and the two after
// it). A runtime that will not have it costs the gesture and nothing else.
void create_panel_gesture(
    Dispatch& dispatch,
    PFN_xrGetInstanceProcAddr next,
    XrInstance instance) noexcept {
    try {
        auto& gesture = dispatch.panel_gesture;
        PFN_xrStringToPath string_to_path = nullptr;
        PFN_xrCreateActionSet create_action_set = nullptr;
        PFN_xrDestroyActionSet destroy_action_set = nullptr;
        PFN_xrCreateAction create_action = nullptr;
        if (!load_function(next, instance, "xrStringToPath", string_to_path) ||
            !load_function(next, instance, "xrCreateActionSet", create_action_set) ||
            !load_function(next, instance, "xrDestroyActionSet", destroy_action_set) ||
            !load_function(next, instance, "xrCreateAction", create_action) ||
            !load_function(next, instance, "xrSuggestInteractionProfileBindings",
                           gesture.suggest_bindings) ||
            !load_function(next, instance, "xrAttachSessionActionSets",
                           gesture.attach_action_sets) ||
            !load_function(next, instance, "xrSyncActions", gesture.sync_actions) ||
            !load_function(next, instance, "xrCreateActionSpace", gesture.create_action_space)) {
            return;
        }
        constexpr std::array<const char*, 2> kHands{"/user/hand/left", "/user/hand/right"};
        constexpr std::array<const char*, 2> kGrips{
            "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"};
        for (std::size_t hand = 0; hand < kHands.size(); ++hand) {
            if (XR_FAILED(string_to_path(instance, kHands[hand], &gesture.hands[hand])) ||
                XR_FAILED(string_to_path(instance, kGrips[hand], &gesture.grips[hand]))) {
                return;
            }
        }
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(set_info.actionSetName, "ofxr_status_panel");
        strcpy_s(set_info.localizedActionSetName, "OFXR Bridge status panel");
        // The lowest priority: where the application binds the same grip,
        // its own action sets are never the ones that give way.
        set_info.priority = 0;
        XrActionSet action_set = XR_NULL_HANDLE;
        XrResult result = create_action_set(instance, &set_info, &action_set);
        if (XR_SUCCEEDED(result)) {
            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(action_info.actionName, "grip_pose");
            strcpy_s(action_info.localizedActionName,
                     "Controller pose, for the OFXR Bridge status panel");
            action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
            action_info.countSubactionPaths = static_cast<std::uint32_t>(gesture.hands.size());
            action_info.subactionPaths = gesture.hands.data();
            XrAction action = XR_NULL_HANDLE;
            result = create_action(action_set, &action_info, &action);
            if (XR_SUCCEEDED(result)) {
                gesture.action_set = action_set;
                gesture.grip_action = action;
            } else {
                destroy_action_set(action_set);
            }
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::status_panel, 1,
            gesture.action_set != XR_NULL_HANDLE ? 1 : 0,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(result)));
    } catch (...) {
    }
}

XrResult layer_create_api_layer_instance_impl(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    if (create_info == nullptr || layer_info == nullptr || instance == nullptr ||
        layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer_info->structVersion < XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer_info->structSize < sizeof(XrApiLayerCreateInfo) ||
        layer_info->nextInfo == nullptr ||
        layer_info->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        layer_info->nextInfo->structVersion < XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        layer_info->nextInfo->structSize < sizeof(XrApiLayerNextInfo) ||
        std::strcmp(layer_info->nextInfo->layerName, kLayerName) != 0 ||
        layer_info->nextInfo->nextGetInstanceProcAddr == nullptr ||
        layer_info->nextInfo->nextCreateApiLayerInstance == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrApiLayerCreateInfo next_layer_info = *layer_info;
    next_layer_info.nextInfo = layer_info->nextInfo->next;

    const PFN_xrGetInstanceProcAddr next_get_instance_proc_addr =
        layer_info->nextInfo->nextGetInstanceProcAddr;
    const PFN_xrCreateApiLayerInstance next_create_api_layer_instance =
        layer_info->nextInfo->nextCreateApiLayerInstance;

    auto dispatch = std::make_shared<Dispatch>();
    dispatch->get_instance_proc_addr = next_get_instance_proc_addr;

    // The D3D11 bridge needs XR_KHR_D3D12_enable on the instance, which the
    // application did not ask for. Added here, only when the ini asks for
    // the bridge, the application enabled D3D11 and the runtime lists D3D12;
    // anything short of that leaves the instance as the application made it.
    XrInstanceCreateInfo bridged_create_info{};
    std::vector<const char*> bridged_extensions;
    const XrInstanceCreateInfo* forwarded_create_info = create_info;
    bool bridge_extension_added = false;
    bool bridge_requested_by_ini = false;
    bool vulkan_bridge_requested_by_ini = false;
    bool asked_d3d11 = false;
    bool asked_d3d12 = false;
    bool asked_vulkan = false;
    try {
        for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index) {
            const char* const extension = create_info->enabledExtensionNames[index];
            if (extension == nullptr) continue;
            if (std::string_view(extension) == "XR_KHR_D3D11_enable") asked_d3d11 = true;
            if (std::string_view(extension) == "XR_KHR_D3D12_enable") asked_d3d12 = true;
            if (std::string_view(extension) == "XR_KHR_vulkan_enable" ||
                std::string_view(extension) == "XR_KHR_vulkan_enable2") asked_vulkan = true;
        }
        // A game that enabled XR_KHR_D3D12_enable itself - Assetto Corsa
        // and SkyrimVR enable every graphics extension the runtime lists -
        // needs nothing added; the bridge then decides at xrCreateSession
        // on the binding it sees.
        bridge_requested_by_ini =
            xrfg::implicit_layer::read_d3d11_bridge(current_layer_directory());
        // The Vulkan bridge wants the same extension, for the same reason.
        vulkan_bridge_requested_by_ini =
            xrfg::implicit_layer::read_vulkan_support(current_layer_directory()) &&
            xrfg::implicit_layer::read_vulkan_session_bridge(current_layer_directory());
        if (!asked_d3d12 &&
            ((asked_d3d11 && bridge_requested_by_ini) ||
             (asked_vulkan && vulkan_bridge_requested_by_ini))) {
            PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
            bool runtime_has_d3d12 = false;
            if (XR_SUCCEEDED(next_get_instance_proc_addr(
                    XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&enumerate_extensions))) &&
                enumerate_extensions != nullptr) {
                std::uint32_t count = 0;
                if (XR_SUCCEEDED(enumerate_extensions(nullptr, 0, &count, nullptr)) && count > 0) {
                    std::vector<XrExtensionProperties> properties(
                        count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                    if (XR_SUCCEEDED(enumerate_extensions(
                            nullptr, count, &count, properties.data()))) {
                        for (std::uint32_t index = 0; index < count; ++index) {
                            if (std::string_view(properties[index].extensionName) ==
                                "XR_KHR_D3D12_enable") {
                                runtime_has_d3d12 = true;
                            }
                        }
                    }
                }
            }
            if (runtime_has_d3d12) {
                bridged_extensions.assign(
                    create_info->enabledExtensionNames,
                    create_info->enabledExtensionNames + create_info->enabledExtensionCount);
                bridged_extensions.push_back("XR_KHR_D3D12_enable");
                bridged_create_info = *create_info;
                bridged_create_info.enabledExtensionNames = bridged_extensions.data();
                bridged_create_info.enabledExtensionCount =
                    static_cast<std::uint32_t>(bridged_extensions.size());
                forwarded_create_info = &bridged_create_info;
                bridge_extension_added = true;
            }
        }
    } catch (...) {
        forwarded_create_info = create_info;
        bridge_extension_added = false;
    }

    XrInstance created_instance = XR_NULL_HANDLE;
    const XrResult result = next_create_api_layer_instance(
        forwarded_create_info,
        &next_layer_info,
        &created_instance);
    if (XR_FAILED(result)) {
        return result;
    }
    // Before the renderer exists - an engine creates its OpenXR instance to
    // choose the adapter - so before the game's DLSS initialises NGX. Either
    // DLSS method qualifies: both already hook the game's DLSS, and the menu
    // switches between them live.
    try {
        const auto desired = xrfg::embedded::snapshot().desired;
        if (asked_d3d12 && (desired.frame_generation == 1 || desired.motion_vectors == 1) &&
            GetEnvironmentVariableW(L"XRFG_TEST_NATIVE_DLSSG_NO_DISCOVERY", nullptr, 0) == 0) {
            xrfg::prepare_ngx_feature_discovery(current_layer_directory().c_str());
        }
    } catch (...) {
    }

    const bool loaded =
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroyInstance", dispatch->destroy_instance) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrCreateSession", dispatch->create_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySession", dispatch->destroy_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrBeginSession", dispatch->begin_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrEndSession", dispatch->end_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrWaitFrame", dispatch->wait_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrBeginFrame", dispatch->begin_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrEndFrame", dispatch->end_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrCreateSwapchain", dispatch->create_swapchain) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySwapchain", dispatch->destroy_swapchain) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, it just never has a space settled before destroy.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrDestroySpace",
             dispatch->destroy_space)),
         true) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, the recorder just never sees a state transition.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrPollEvent",
             dispatch->poll_event)),
         true) &&
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrEnumerateSwapchainFormats",
             dispatch->enumerate_swapchain_formats)),
         true) &&
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrCreateReferenceSpace",
             dispatch->create_reference_space)),
         true) &&
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrLocateSpace",
             dispatch->locate_space)),
         true) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrEnumerateSwapchainImages",
            dispatch->enumerate_swapchain_images) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrAcquireSwapchainImage",
            dispatch->acquire_swapchain_image) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrWaitSwapchainImage",
            dispatch->wait_swapchain_image) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrReleaseSwapchainImage",
            dispatch->release_swapchain_image);
    if (!loaded) {
        if (dispatch->destroy_instance != nullptr) {
            dispatch->destroy_instance(created_instance);
        }
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    PFN_xrGetInstanceProperties get_instance_properties = nullptr;
    if (load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrGetInstanceProperties",
            get_instance_properties)) {
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(get_instance_properties(created_instance, &properties))) {
            dispatch->runtime_version = properties.runtimeVersion;
            dispatch->runtime_name = properties.runtimeName;
            dispatch->steamvr_runtime =
                std::string_view(dispatch->runtime_name).find("SteamVR") !=
                std::string_view::npos;
            dispatch->virtual_desktop_runtime =
                std::string_view(dispatch->runtime_name).find(
                    "VirtualDesktopXR") != std::string_view::npos;
            dispatch->inline_unless_pipelined =
                dispatch->virtual_desktop_runtime ||
                std::string_view(dispatch->runtime_name).find(
                    "Pimax OpenXR") != std::string_view::npos;
        }
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::runtime_identity,
        dispatch->steamvr_runtime ? 1 : 0,
        dispatch->runtime_version,
        dispatch->runtime_name.size(),
        runtime_name_hash(dispatch->runtime_name));

    // Which graphics API the application asked the runtime for, and the
    // runtime's Vulkan entry points, recorded before the layer supports
    // Vulkan. A function the application did not enable fails to load and is
    // simply not intercepted.
    {
        std::uint64_t graphics_extensions = 0;
        for (std::uint32_t index = 0;
             index < create_info->enabledExtensionCount;
             ++index) {
            const char* const extension =
                create_info->enabledExtensionNames[index];
            if (extension == nullptr) {
                continue;
            }
            const std::string_view name(extension);
            if (name == "XR_KHR_vulkan_enable") graphics_extensions |= 1ULL;
            if (name == "XR_KHR_vulkan_enable2") graphics_extensions |= 2ULL;
            if (name == "XR_KHR_D3D11_enable") graphics_extensions |= 4ULL;
            if (name == "XR_KHR_D3D12_enable") graphics_extensions |= 8ULL;
            if (name == "XR_KHR_opengl_enable") graphics_extensions |= 16ULL;
        }
        log_vulkan_negotiation(1, graphics_extensions);
        auto& vulkan = dispatch->vulkan;
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrGetVulkanInstanceExtensionsKHR", vulkan.get_instance_extensions));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrGetVulkanDeviceExtensionsKHR", vulkan.get_device_extensions));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrCreateVulkanInstanceKHR", vulkan.create_instance));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrCreateVulkanDeviceKHR", vulkan.create_device));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrGetVulkanGraphicsDeviceKHR", vulkan.get_graphics_device));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrGetVulkanGraphicsDevice2KHR", vulkan.get_graphics_device2));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrGetVulkanGraphicsRequirementsKHR", vulkan.get_graphics_requirements));
        static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
            "xrGetVulkanGraphicsRequirements2KHR", vulkan.get_graphics_requirements2));
        // The bridge decision, on every instance: a the graphics extensions
        // the application enabled (4 D3D11, 8 D3D12), b whether the ini asks
        // for the bridge, c whether the extension was added. result 0 with
        // c=1 is the bridge armed; -1 is an added extension whose
        // requirements call the runtime then failed to hand out.
        const bool d3d12_on_instance = bridge_extension_added || asked_d3d12;
        if (bridge_requested_by_ini && asked_d3d11 && d3d12_on_instance) {
            dispatch->d3d11_bridge = load_function(
                next_get_instance_proc_addr, created_instance,
                "xrGetD3D12GraphicsRequirementsKHR",
                dispatch->get_d3d12_graphics_requirements);
        }
        // Not exclusive with the D3D11 bridge: OpenComposite enables every
        // graphics extension the runtime lists and opens a D3D11 session
        // before its Vulkan one (No Man's Sky). Each bridge engages on the
        // binding a session actually makes.
        if (vulkan_bridge_requested_by_ini && asked_vulkan && d3d12_on_instance) {
            dispatch->vulkan_bridge =
                dispatch->get_d3d12_graphics_requirements != nullptr ||
                load_function(
                    next_get_instance_proc_addr, created_instance,
                    "xrGetD3D12GraphicsRequirementsKHR",
                    dispatch->get_d3d12_graphics_requirements);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                dispatch->vulkan_bridge ? 0 : -1,
                graphics_extensions,
                1,
                (bridge_extension_added ? 1ULL : 0ULL) |
                    (dispatch->vulkan_bridge ? 2ULL : 0ULL));
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            bridge_requested_by_ini && asked_d3d11 && d3d12_on_instance &&
                    !dispatch->d3d11_bridge
                ? -1
                : 0,
            graphics_extensions,
            bridge_requested_by_ini ? 1 : 0,
            (bridge_extension_added ? 1ULL : 0ULL) |
                (dispatch->d3d11_bridge ? 2ULL : 0ULL));
    }
    if (read_status_panel_mode() == xrfg::StatusPanelMode::gesture) {
        create_panel_gesture(*dispatch, next_get_instance_proc_addr, created_instance);
    }

    try {
        std::scoped_lock lock(g_state_mutex);
        g_instances[created_instance] = dispatch;
    } catch (const std::bad_alloc&) {
        dispatch->destroy_instance(created_instance);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        dispatch->destroy_instance(created_instance);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *instance = created_instance;
    return result;
}

XrResult layer_destroy_instance_impl(XrInstance instance) {
    const auto dispatch = find_dispatch(instance);
    if (!dispatch || dispatch->destroy_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    for (const auto& session_state : find_sessions(dispatch)) {
        stop_continuous_presenter(session_state);
        session_state->fps_overlay.reset();
    }
    for (const auto& swapchain_state : find_swapchains(dispatch)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    bool last_instance = false;
    const XrResult result = dispatch->destroy_instance(instance);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_sessions.begin(); iterator != g_sessions.end();) {
            if (iterator->second->dispatch == dispatch) {
                iterator = g_sessions.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session->dispatch == dispatch) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_instances.erase(instance);
        last_instance = g_instances.empty();
    }
    if (last_instance) {
        // The connection must not outlive the instance. See
        // SteamVrDelivery::release_process_connection() for why that is not in
        // conflict with it having to outlive every session.
        xrfg::SteamVrDelivery::release_process_connection();
    }
    return result;
}

// Native DLSS FG makes 3X only where NGX generates two frames per pair. On
// other adapters the session stays at a pair instead of showing repeats.
[[nodiscard]] bool native_triple_limited(const SessionState& state) noexcept {
    return state.nvidia_options.frame_generation == xrfg::D3D12FrameGeneration::native_dlss &&
        xrfg::native_dlssg_max_generated_frames(state.d3d12_device.Get()) < 2U;
}

// The tray's synthesis modes for OFXR's own algorithm: extrapolation, from
// the game's DLSS vectors and depth where it has them and from FidelityFX's
// flow where it does not; else the hybrid of the game's vectors and
// FidelityFX's flow, once the game has published DLSS vectors. Both take the
// FidelityFX backend.
void synthesis_modes(const SessionState& state, bool dlss_motion_vectors,
                     xrfg::D3D12NvidiaOpticalFlowOptions& options,
                     xrfg::D3D12OpticalFlowBackend& backend) noexcept {
    const bool ofxr = options.frame_generation == xrfg::D3D12FrameGeneration::ofxr;
    options.extrapolate = state.extrapolate != 0 && ofxr;
    options.extrapolate_hybrid = state.extrapolate == 2 && ofxr;
    options.extrapolate_mesh = state.extrapolate != 0 && state.extrapolate_mesh && ofxr;
    options.hybrid = state.dlss_flow_hybrid && state.dlss_vectors_published && ofxr &&
        dlss_motion_vectors && !options.extrapolate;
    // Where the flow only patches what the game's vectors miss, it runs at a
    // quarter per axis rather than half: on the recorded frames the hybrid
    // erred 7.19 against 7.18 (SSIM 0.815 both) for 11% less time, and the
    // combined extrapolation 11.56 against 11.53 for 16% less. Alone the flow
    // needs its half: FidelityFX interpolation lost 0.27 at a quarter and its
    // extrapolation 0.71.
    if ((options.hybrid || (options.extrapolate_hybrid && dlss_motion_vectors)) &&
        options.input_scale == xrfg::D3D12OpticalFlowInputScale::half) {
        options.input_scale = xrfg::D3D12OpticalFlowInputScale::quarter;
    }
    if (options.hybrid || options.extrapolate) {
        backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    }
}

XrResult layer_create_session_impl(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    const auto dispatch = find_dispatch(instance);
    if (!dispatch || dispatch->create_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (session == nullptr) {
        return dispatch->create_session(instance, create_info, session);
    }

    auto state = std::make_shared<SessionState>(dispatch);
    const auto initial_control = xrfg::embedded::snapshot();
    state->optical_flow_backend = static_cast<xrfg::D3D12OpticalFlowBackend>(initial_control.desired.backend);
    state->nvidia_options = {
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(initial_control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(initial_control.desired.scale),
        initial_control.desired.backward,
        static_cast<xrfg::D3D12FrameGeneration>(initial_control.desired.frame_generation),
        static_cast<std::uint32_t>(initial_control.desired.native_scale)};
    // One display period of extra depth, bought with one display period of
    // latency: every synthetic is held until it is a period old, so synthesis
    // gets a period to finish instead of the gap the game leaves. On by
    // default; the tray's "Prefer FPS over latency" turns it off for anyone
    // who would rather not pay the latency. Fixed for the session - it sets
    // the private swapchain rings and the admission bounds.
    state->deep_pipeline =
        xrfg::implicit_layer::read_deep_pipeline(current_layer_directory());
    state->dlss_flow_hybrid =
        xrfg::implicit_layer::read_dlss_flow_hybrid(current_layer_directory());
    state->promise_shown_time =
        xrfg::implicit_layer::read_promise_shown_time(current_layer_directory());
    state->extrapolate =
        xrfg::implicit_layer::read_extrapolate(current_layer_directory());
    state->extrapolate_mesh =
        xrfg::implicit_layer::read_extrapolate_mesh(current_layer_directory());
    state->synthetic_pose_interpolated =
        xrfg::implicit_layer::read_synthetic_pose_interpolated(current_layer_directory());
    {
        wchar_t value[8]{};
        const DWORD n =
            GetEnvironmentVariableW(L"XRFG_TEST_ADMISSION_WAIT_MS", value, 8);
        if (n != 0 && n < 8) {
            state->test_admission_wait =
                std::chrono::milliseconds(std::wcstol(value, nullptr, 10));
        }
    }
    if (state->extrapolate != 0) {
        // Extrapolation is there for latency, and its synthetics already
        // trail the real frame by a period: the deeper pipeline's held
        // period would only add latency back.
        state->deep_pipeline = false;
    }
    const bool triple_requested =
        xrfg::implicit_layer::read_triple_frame_gen(current_layer_directory());
    state->single_swapchain_rings =
        xrfg::implicit_layer::read_single_swapchain_rings(current_layer_directory());
    state->capture_at_end_frame =
        xrfg::implicit_layer::read_capture_at_end_frame(current_layer_directory());
    state->vulkan_support =
        xrfg::implicit_layer::read_vulkan_support(current_layer_directory());
    state->pause_applied = state->manual_control.pause_requested();
    state->recorder_applied = xrfg::bridge_flight_logger().enabled();
    state->menu_enabled = initial_control.desired.enabled && !state->pause_applied;
    state->dlss_motion_vectors = initial_control.desired.motion_vectors == 1 || initial_control.desired.frame_generation == 1;
    state->dlss_vectors_published = xrfg::dlss_motion_vector_publications() != 0;
    synthesis_modes(*state, state->dlss_motion_vectors, state->nvidia_options,
                    state->optical_flow_backend);
    state->control_revision = initial_control.revision;
    xrfg::embedded::applied(state->control_id, state->control_revision, state->menu_enabled, 0);
    XrStructureType binding_structure_type = XR_TYPE_UNKNOWN;
    if (create_info != nullptr) {
        auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
        while (next != nullptr) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                const auto* binding =
                    reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
                state->graphics_binding = SessionGraphicsBinding::d3d11;
                binding_structure_type = next->type;
                state->d3d11_device = binding->device;
                if (state->d3d11_device) {
                    state->d3d11_device->GetImmediateContext(
                        state->d3d11_context.ReleaseAndGetAddressOf());
                    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
                    if (SUCCEEDED(state->d3d11_device.As(&device5))) {
                        state->graphics_binding_capabilities |= 1ULL;
                    }
                    if ((state->d3d11_device->GetCreationFlags() &
                         D3D11_CREATE_DEVICE_SINGLETHREADED) != 0) {
                        state->single_threaded_d3d11 = true;
                        state->graphics_binding_capabilities |= 32ULL;
                    }
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
                    if (state->d3d11_context &&
                        SUCCEEDED(state->d3d11_context.As(&context4))) {
                        state->graphics_binding_capabilities |= 2ULL;
                    }
                }
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
                state->graphics_binding = SessionGraphicsBinding::d3d12;
                binding_structure_type = next->type;
                state->d3d12_device = binding->device;
                state->d3d12_queue = binding->queue;
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                state->graphics_binding = SessionGraphicsBinding::vulkan;
                binding_structure_type = next->type;
                const auto* binding =
                    reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
                state->vulkan_binding = xrfg::VulkanSessionBinding{
                    binding->instance,
                    binding->physicalDevice,
                    binding->device,
                    binding->queueFamilyIndex,
                    binding->queueIndex};
                log_vulkan_negotiation(
                    9,
                    reinterpret_cast<std::uintptr_t>(binding->device),
                    binding->queueFamilyIndex,
                    binding->queueIndex);
                probe_vulkan_interop_support(*binding);
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                state->graphics_binding = SessionGraphicsBinding::opengl;
                binding_structure_type = next->type;
                break;
            }
            next = next->next;
        }
    }

    // Hand the runtime a queue of the layer's own on a native D3D12 session:
    // see SessionState::binding_queue. SteamVR only: it is the runtime whose
    // compositor was measured judging readiness from the binding queue;
    // nothing measured on another runtime shows a gain from it, so they keep
    // the application's queue. The binding has
    // to be the first structure chained on the create info for the
    // substitution to be made without copying structures the layer does not
    // know; every application seen chains it first. Anything that fails here
    // leaves the runtime with the application's queue, which every path
    // downstream also handles.
    XrSessionCreateInfo substituted_info{};
    XrGraphicsBindingD3D12KHR substituted_binding{};
    const XrSessionCreateInfo* forwarded_info = create_info;
    // The D3D11 bridge: see SessionState::d3d11_bridge. From here the
    // session is a D3D12 one for every path below, and the runtime gets a
    // D3D12 binding on the layer's device. Anything that fails leaves the
    // D3D11 binding as the application made it.
    XrGraphicsBindingD3D12KHR bridge_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    const void* d3d11_binding_next = nullptr;
    if (create_info != nullptr) {
        for (auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                d3d11_binding_next = next->next;
            }
        }
    }
    if (dispatch->d3d11_bridge &&
        dispatch->get_d3d12_graphics_requirements != nullptr &&
        state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        create_info != nullptr && state->d3d11_device && state->d3d11_context) {
        XrGraphicsRequirementsD3D12KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult requirements_result =
            dispatch->get_d3d12_graphics_requirements(
                instance, create_info->systemId, &requirements);
        Microsoft::WRL::ComPtr<ID3D12Device> bridge_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> bridge_queue;
        HRESULT device_result = E_FAIL;
        if (XR_SUCCEEDED(requirements_result)) {
            device_result = xrfg::create_d3d12_device_for_d3d11(
                state->d3d11_device.Get(),
                bridge_device.ReleaseAndGetAddressOf(),
                bridge_queue.ReleaseAndGetAddressOf());
        }
        if (XR_SUCCEEDED(requirements_result) && SUCCEEDED(device_result) &&
            bridge_device && bridge_queue) {
            auto bridge = std::make_unique<SessionState::D3D11Bridge>();
            bridge->device = std::move(state->d3d11_device);
            bridge->context = std::move(state->d3d11_context);
            state->d3d11_device.Reset();
            state->d3d11_context.Reset();
            state->d3d11_bridge = std::move(bridge);
            state->d3d12_device = std::move(bridge_device);
            state->d3d12_queue = std::move(bridge_queue);
            state->graphics_binding = SessionGraphicsBinding::d3d12;
            state->single_threaded_d3d11 = false;
            state->graphics_binding_capabilities |= 128ULL;
            bridge_binding.device = state->d3d12_device.Get();
            bridge_binding.queue = state->d3d12_queue.Get();
            // Whatever the application chained after its binding stays
            // chained, after ours.
            bridge_binding.next = d3d11_binding_next;
            substituted_info = *create_info;
            substituted_info.next = &bridge_binding;
            forwarded_info = &substituted_info;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                1,
                0,
                0,
                static_cast<std::uint64_t>(requirements.adapterLuid.LowPart));
        } else {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                XR_FAILED(requirements_result)
                    ? static_cast<std::int64_t>(requirements_result)
                    : static_cast<std::int64_t>(device_result),
                0,
                XR_FAILED(requirements_result) ? 1 : 2,
                0);
        }
    }
    // The Vulkan bridge: see SessionState::vulkan_bridge. The same
    // substitution as the D3D11 bridge's, on a D3D12 device of the layer's
    // for the adapter the Vulkan physical device reports. Anything that
    // fails leaves the Vulkan binding as the application made it, on the
    // interop.
    const void* vulkan_binding_next = nullptr;
    if (create_info != nullptr) {
        for (auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                vulkan_binding_next = next->next;
            }
        }
    }
    if (dispatch->vulkan_bridge && state->vulkan_support &&
        dispatch->get_d3d12_graphics_requirements != nullptr &&
        state->graphics_binding == SessionGraphicsBinding::vulkan &&
        create_info != nullptr &&
        state->vulkan_binding.device != VK_NULL_HANDLE) {
        XrGraphicsRequirementsD3D12KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult requirements_result =
            dispatch->get_d3d12_graphics_requirements(
                instance, create_info->systemId, &requirements);
        Microsoft::WRL::ComPtr<ID3D12Device> bridge_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> bridge_queue;
        HRESULT device_result = E_FAIL;
        if (XR_SUCCEEDED(requirements_result)) {
            device_result = xrfg::create_d3d12_device_for_vulkan(
                state->vulkan_binding,
                bridge_device.ReleaseAndGetAddressOf(),
                bridge_queue.ReleaseAndGetAddressOf());
        }
        if (XR_SUCCEEDED(requirements_result) && SUCCEEDED(device_result) &&
            bridge_device && bridge_queue) {
            auto bridge = std::make_unique<SessionState::VulkanBridge>();
            bridge->binding = state->vulkan_binding;
            state->vulkan_bridge = std::move(bridge);
            state->d3d12_device = std::move(bridge_device);
            state->d3d12_queue = std::move(bridge_queue);
            state->graphics_binding = SessionGraphicsBinding::d3d12;
            state->graphics_binding_capabilities |= 256ULL;
            bridge_binding.device = state->d3d12_device.Get();
            bridge_binding.queue = state->d3d12_queue.Get();
            bridge_binding.next = vulkan_binding_next;
            substituted_info = *create_info;
            substituted_info.next = &bridge_binding;
            forwarded_info = &substituted_info;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                1,
                0,
                0,
                static_cast<std::uint64_t>(requirements.adapterLuid.LowPart));
        } else {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                XR_FAILED(requirements_result)
                    ? static_cast<std::int64_t>(requirements_result)
                    : static_cast<std::int64_t>(device_result),
                0,
                XR_FAILED(requirements_result) ? 1 : 2,
                0);
        }
    }
    // Decided here, once the binding is final: a D3D11 session the bridge
    // took over is a D3D12 one by now. One left on the D3D11 interop stays
    // at a pair, because that interop publishes one synthetic.
    const bool triple_binding =
        state->graphics_binding == SessionGraphicsBinding::d3d12 ||
        state->graphics_binding == SessionGraphicsBinding::vulkan;
    state->deep_pipeline_configured = state->deep_pipeline;
    // A 3X request the native feature cannot meet keeps the 3X ring, so the
    // OFXR algorithm can still be switched to 3X live.
    const bool triple_limited =
        triple_requested && triple_binding && native_triple_limited(*state);
    if (triple_requested && triple_binding && !triple_limited) {
        state->frames_per_application_frame = 3;
        state->deep_pipeline = false;
    }
    // Extrapolation's synthetic is shown after the real frame, so it can
    // still be queued when the next pair composes: it needs the second slot.
    state->two_slot_synthetic_ring =
        state->deep_pipeline || state->frames_per_application_frame > 2 || triple_limited ||
        state->extrapolate != 0;
    state->triple_switchable =
        triple_binding && state->two_slot_synthetic_ring;
    if (triple_binding && !state->triple_switchable) {
        state->fixed_frame_multiplier.hold();
    }
    // 700: the session's shape. a frames per application frame, b the deeper
    // pipeline, c whether 3X was asked for - or 2 when the record marks a
    // live change rather than the session's creation.
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        700,
        state->frames_per_application_frame,
        state->deep_pipeline ? 1u : 0u,
        triple_requested ? 1u : 0u);
    if (dispatch->steamvr_runtime &&
        state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        create_info != nullptr && state->d3d12_device && state->d3d12_queue) {
        const auto* first = state->d3d11_bridge || state->vulkan_bridge
            ? reinterpret_cast<const XrBaseInStructure*>(&bridge_binding)
            : static_cast<const XrBaseInStructure*>(create_info->next);
        if (first != nullptr && first->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            D3D12_COMMAND_QUEUE_DESC binding_queue_description{};
            binding_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            binding_queue_description.NodeMask =
                state->d3d12_queue->GetDesc().NodeMask;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> binding_queue;
            Microsoft::WRL::ComPtr<ID3D12Fence> release_fence;
            Microsoft::WRL::ComPtr<ID3D12Fence> reacquire_fence;
            if (SUCCEEDED(state->d3d12_device->CreateCommandQueue(
                    &binding_queue_description,
                    IID_PPV_ARGS(binding_queue.GetAddressOf()))) &&
                SUCCEEDED(state->d3d12_device->CreateFence(
                    0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(release_fence.GetAddressOf()))) &&
                SUCCEEDED(state->d3d12_device->CreateFence(
                    0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(reacquire_fence.GetAddressOf())))) {
                substituted_binding =
                    *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(first);
                substituted_binding.queue = binding_queue.Get();
                substituted_info = *create_info;
                substituted_info.next = &substituted_binding;
                forwarded_info = &substituted_info;
                state->binding_queue = std::move(binding_queue);
                state->app_release_fence = std::move(release_fence);
                state->reacquire_fence = std::move(reacquire_fence);
                state->graphics_binding_capabilities |= 16ULL;
            }
        }
    }

    XrSession created_session = XR_NULL_HANDLE;
    const XrResult result = dispatch->create_session(instance, forwarded_info, &created_session);
    if (XR_FAILED(result)) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            static_cast<std::int64_t>(result),
            0,
            3,
            state->d3d11_bridge ? 1 : 0);
        return result;
    }
    state->handle = created_session;
    log_video_memory(*state, VideoMemoryStage::session, 0);
    log_view_configuration(*dispatch, instance, create_info ? create_info->systemId : XR_NULL_SYSTEM_ID);
    // Optional instrumentation cannot fail an otherwise valid session.
    try {
        state->steamvr_delivery =
            std::make_unique<xrfg::SteamVrDelivery>(dispatch->steamvr_runtime);
        state->fps_overlay = std::make_unique<xrfg::OpenXrFpsOverlay>(
            instance, created_session, create_info ? create_info->systemId : 0,
            dispatch->get_instance_proc_addr, dispatch->end_frame,
            // The overlay draws into swapchain images of its own, so it
            // draws on the queue the runtime orders those images against.
            state->d3d12_device.Get(), runtime_queue(*state),
            state->d3d11_device.Get(), current_layer_directory() / L"ofxr_bridge.ini",
            state->steamvr_delivery.get());
    } catch (...) {}
    if (state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        (state->graphics_binding_capabilities & 3ULL) == 3ULL) {
        const HRESULT bridge_device_result =
            xrfg::create_d3d12_device_for_d3d11(
                state->d3d11_device.Get(),
                state->d3d12_device.ReleaseAndGetAddressOf(),
                state->d3d12_queue.ReleaseAndGetAddressOf());
        if (SUCCEEDED(bridge_device_result)) {
            state->graphics_binding_capabilities |= 4ULL;
        } else {
            state->d3d12_device.Reset();
            state->d3d12_queue.Reset();
        }
    }
    // A Vulkan session gets the same arrangement as a D3D11 one: a D3D12
    // device of the layer's own on the application's adapter, found through
    // the LUID its physical device reports, with a high-priority queue that
    // all of synthesis runs on. Capability bit 64.
    if (state->graphics_binding == SessionGraphicsBinding::vulkan &&
        state->vulkan_support &&
        state->vulkan_binding.device != VK_NULL_HANDLE) {
        const HRESULT bridge_device_result =
            xrfg::create_d3d12_device_for_vulkan(
                state->vulkan_binding,
                state->d3d12_device.ReleaseAndGetAddressOf(),
                state->d3d12_queue.ReleaseAndGetAddressOf());
        if (SUCCEEDED(bridge_device_result)) {
            state->graphics_binding_capabilities |= 64ULL;
        } else {
            state->d3d12_device.Reset();
            state->d3d12_queue.Reset();
        }
    }
    // A queue of the layer's own for synthesis, so the GPU-side wait on the
    // optical flow fence does not sit in the middle of the application's
    // queue. Only for a native D3D12 application: a D3D11 one already runs
    // synthesis on the bridge queue created just above.
    if (state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        state->d3d12_device && state->d3d12_queue) {
        D3D12_COMMAND_QUEUE_DESC synthesis_queue_description{};
        synthesis_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // Synthesis has a hard deadline the application's rendering does
        // not: its frame has a fixed slot on the presentation grid, and if
        // the output is not written by then the frame is lost. The game's own
        // frame can absorb a delay.
        synthesis_queue_description.Priority =
            D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        synthesis_queue_description.NodeMask =
            state->d3d12_queue->GetDesc().NodeMask;
        if (FAILED(state->d3d12_device->CreateCommandQueue(
                &synthesis_queue_description,
                IID_PPV_ARGS(
                    state->d3d12_synthesis_queue.ReleaseAndGetAddressOf())))) {
            // Fail open: synthesis stays on the application's queue, which
            // is what every build before this one did.
            state->d3d12_synthesis_queue.Reset();
        } else {
            state->graphics_binding_capabilities |= 8ULL;
        }
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::session_binding,
        static_cast<std::int64_t>(state->graphics_binding),
        static_cast<std::uint64_t>(binding_structure_type) |
            (state->graphics_binding_capabilities << 32),
        state->d3d11_device
            ? handle_value(state->d3d11_device.Get())
            : handle_value(state->d3d12_device.Get()),
        state->d3d11_context
            ? handle_value(state->d3d11_context.Get())
            : handle_value(state->d3d12_queue.Get()));

    try {
        {
            std::scoped_lock lock(g_state_mutex);
            g_sessions[created_session] = state;
        }
    } catch (const std::bad_alloc&) {
        dispatch->destroy_session(created_session);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        dispatch->destroy_session(created_session);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    // OFXR's own synthesis can use the same captured DLSS guides as native
    // generation; without a provider publishing them, nothing else does.
    xrfg::configure_ngx_guide_capture(state->d3d12_queue.Get(),
        state->graphics_binding == SessionGraphicsBinding::d3d12 && state->menu_enabled &&
        state->dlss_motion_vectors);
    *session = created_session;
    return result;
}

XrResult layer_destroy_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->destroy_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    xrfg::stop_ngx_guide_capture(state->d3d12_queue.Get());
    stop_continuous_presenter(state);
    state->fps_overlay.reset();
    // The status panel's grip spaces, which the overlay read until now.
    for (XrSpace& grip : state->panel_grip_spaces) {
        if (grip != XR_NULL_HANDLE && state->dispatch->destroy_space != nullptr) {
            state->dispatch->destroy_space(grip);
        }
        grip = XR_NULL_HANDLE;
    }
    {
        // The presenter, which also reads it, has stopped.
        std::scoped_lock lock(state->reprojection_space_mutex);
        if (state->reprojection_view_space != XR_NULL_HANDLE &&
            state->dispatch->destroy_space != nullptr) {
            state->dispatch->destroy_space(state->reprojection_view_space);
        }
        state->reprojection_view_space = XR_NULL_HANDLE;
    }
    for (const auto& swapchain_state : find_swapchains(state)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }
    drain_binding_queue(state.get());

    const XrResult result = state->dispatch->destroy_session(session);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session == state) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_sessions.erase(session);
    }
    return result;
}

void reset_frame_bookkeeping(const std::shared_ptr<SessionState>& state) {
    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    {
        std::scoped_lock frame_call_lock(state->frame_call_mutex);
        state->application_wait_pending_begin = false;
        state->application_frame_in_progress = false;
        state->application_frame_has_overlapping_wait = false;
        state->pipelined_wait_streak = 0;
        state->pipelined_presenter_mode = false;
        state->pipelined_presenter_start_requested = false;
        state->steamvr_presenter_start_requested = false;
        state->runtime_frame_waited_unbegun = false;
        state->application_begin_needs_wait = false;
        state->split_frame_loop = false;
        state->application_end_thread_id = 0;
        state->last_inline_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        state->last_inline_frame_state_valid = false;
        state->generation_steady_state_established = false;
    }
    state->frame_call_condition.notify_all();
    {
        std::scoped_lock presenter_lock(state->presenter_mutex);
        state->presenter_adopted_frame_state_valid = false;
        state->last_virtual_display_time = 0;
    }
    {
        std::scoped_lock lock(state->mutex);
        state->pending_frames.clear();
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        state->generation_resume_wall_time = {};
        state->minimum_runtime_display_period = 0;
        state->steamvr_throttled_wait_streak = 0;
        state->unpaced_wait_streak = 0;
    }
}

void reset_swapchain_bookkeeping(const std::shared_ptr<SessionState>& session) noexcept {
    try {
        for (const auto& state : find_swapchains(session)) {
            std::scoped_lock call_lock(state->call_mutex);
            std::scoped_lock gpu_lock(session->gpu_mutex);
            std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(state->mutex);
                // Neither xrBeginSession nor xrEndSession releases an image
                // at the runtime, so what the application has acquired and
                // waited stays acquired and waited across them; only the
                // history side starts over.
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                history = state->d3d12_history;
                generation = state->frame_generation;
            }
            if (generation && generation->synthesizer) {
                static_cast<void>(generation->synthesizer->wait_for_idle());
            }
            if (history) {
                static_cast<void>(history->invalidate());
            }
            if (generation && generation->interop) {
                static_cast<void>(generation->interop->wait_for_idle());
            }
        }
    } catch (...) {
    }
}

XrResult layer_begin_session_impl(
    XrSession session,
    const XrSessionBeginInfo* begin_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    const XrResult result = state->dispatch->begin_session(session, begin_info);
    if (XR_SUCCEEDED(result)) {
        reset_frame_bookkeeping(state);
        reset_swapchain_bookkeeping(state);
    }
    return result;
}

XrResult layer_end_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    stop_continuous_presenter(state);
    const XrResult result = state->dispatch->end_session(session);
    if (XR_SUCCEEDED(result)) {
        reset_frame_bookkeeping(state);
        reset_swapchain_bookkeeping(state);
    }
    return result;
}

// frameWaitInfo and frameBeginInfo are optional="true" in the OpenXR registry,
// so a null pointer is valid input that a runtime must accept, and only a
// non-null one carries a type worth checking. This matters wherever the layer
// answers a frame call itself instead of forwarding it: the pass-through path
// hands the pointer to the runtime, which accepts null, while the virtual wait
// and begin used in presenter and pipelined modes have to accept it too.
// Rejecting null there fails applications that pass it and only on the runtimes
// that promote a presenter -- Luke Ross's mods call xrWaitFrame(session, NULL,
// &state) and drop back to 2D on the XR_ERROR_VALIDATION_FAILURE.
template <typename Info>
[[nodiscard]] bool valid_optional_frame_info(
    const Info* info,
    XrStructureType expected) noexcept {
    return info == nullptr || info->type == expected;
}

XrResult layer_wait_frame_impl(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state) {
    const auto state = find_session(session);
    if (!state || state->dispatch->wait_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::unique_lock frame_call_lock(state->frame_call_mutex);
    state->frame_call_condition.wait(frame_call_lock, [&] {
        return !state->application_wait_pending_begin;
    });
    if (!state->split_frame_loop && state->application_end_thread_id != 0 &&
        state->application_end_thread_id != GetCurrentThreadId()) {
        state->split_frame_loop = true;
        // 600: the frame loop is split across threads. a the wait thread, b
        // the end thread, c the graphics binding - on D3D11 this is what
        // keeps the session inline (presenter_forbidden).
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            600,
            GetCurrentThreadId(),
            state->application_end_thread_id,
            static_cast<std::uint64_t>(state->graphics_binding));
    }
    XrResult result = XR_SUCCESS;
    const bool use_continuous_presenter = continuous_presenter_active(state);
    bool use_provisional_pipelined_wait = false;
    bool forwarded_application_wait = false;
    if (!use_continuous_presenter && state->application_frame_in_progress) {
        if (!state->application_frame_has_overlapping_wait) {
            state->application_frame_has_overlapping_wait = true;
            ++state->pipelined_wait_streak;
        }
        if (state->pipelined_wait_streak >= 2 &&
            state->last_inline_frame_state_valid &&
            !presenter_forbidden(*state)) {
            // The current runtime frame is still owned by the render thread,
            // so the transition cannot start its presenter until xrEndFrame
            // closes that boundary. Return the first virtual wait now and
            // preserve its timeline when the presenter starts at that end.
            state->pipelined_presenter_mode = true;
            state->pipelined_presenter_start_requested = true;
            use_provisional_pipelined_wait = true;
        }
    }
    if (use_continuous_presenter) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::unique_lock presenter_lock(state->presenter_mutex);
        // Hold the application to the period it was handed. The layer submits
        // one pair per application frame and a pair costs two presenter
        // frames, so releasing on every presenter frame lets an application
        // that can render faster than half the display rate produce frames the
        // pairing has no room for.
        //
        // The hold itself is in xrEndFrame, not here. Two reasons, and the
        // first is fatal on its own: this function holds frame_call_mutex
        // across the wait, and xrEndFrame needs that mutex to enqueue
        // anything. Gate here and an application whose wait and end run on
        // different threads deadlocks on its first frame - the wait holds the
        // mutex until the presenter advances, the presenter is parked waiting
        // for composition, and the only call that could supply it is blocked
        // on the mutex. MSFS 2024 froze on entering VR exactly there.
        //
        // The second is that this is the wrong end of the frame. Held here the
        // application has not started rendering, so it wakes late and hands
        // its frame over at the end of the period with nothing left for
        // synthesis: on the Luke Ross mods that took the gap between queueing
        // synthesis and handing the synthetic over from 8.19 ms to 0.04 ms
        // against pixels needing 11.51 ms, so every synthetic reached the
        // compositor unrendered and was reprojected - indistinguishable from
        // the layer being off, with a flawless cadence in the log.
        state->presenter_condition.wait(presenter_lock, [&] {
            return state->presenter_frame_state_valid ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        if (!state->presenter_frame_state_valid ||
            state->presenter_stop_requested) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        const XrDuration virtual_period = (state->manual_control.stop_requested() || !state->menu_enabled)
            ? state->presenter_frame_state.predictedDisplayPeriod
            : virtual_display_period(
                state->presenter_frame_state.predictedDisplayPeriod,
                state->frames_per_application_frame);
        // The application's timeline is anchored to the runtime's own
        // prediction, one virtual period ahead of the frame the presenter is
        // about to submit. Extrapolating, the real frame is the first of its
        // group rather than the last, so it is shown the group's other
        // periods sooner.
        const XrTime anchor = add_display_duration(
            state->presenter_frame_state.predictedDisplayTime,
            promised_display_offset(*state, virtual_period,
                state->presenter_frame_state.predictedDisplayPeriod));
        // A second wait inside one presenter frame has to come back later than
        // the first, so the guard below steps off the last time served. That
        // step invents time the runtime never advanced, and the ceiling is
        // what stops it becoming a clock of its own: every period handed out
        // beyond the anchor is a period the application's prediction runs
        // ahead of the runtime's, and nothing ever gives it back.
        //
        // It is not a corner case. Whenever the layer fails open - no
        // projection layers in the submission, which is what a menu or a
        // loading screen looks like - the application is paced one frame per
        // presenter frame instead of one per pair, while still being handed a
        // doubled period on every one of them. Unbounded, that drifts a full
        // second per second: a captured session reached 637 s of lead,
        // predicting poses ten minutes into the future, and never generated
        // again once it got there, because the ratchet only turns one way.
        // With the ceiling the clock simply ticks at the rate the application
        // is actually being paced at, and re-anchors as soon as the pairing
        // comes back.
        const XrTime ceiling = add_display_duration(anchor, virtual_period);
        XrTime virtual_time = anchor;
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = add_display_duration(
                state->last_virtual_display_time,
                virtual_period);
        }
        if (virtual_time > ceiling) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::virtual_clock_clamp,
                0,
                static_cast<std::uint64_t>(virtual_time - ceiling),
                static_cast<std::uint64_t>(ceiling),
                static_cast<std::uint64_t>(
                    state->presenter_frame_state.predictedDisplayTime));
            virtual_time = ceiling;
        }
        // The ceiling can land at or below the time already served, because
        // the anchor tracks a runtime prediction that does not advance by a
        // whole period every time it is read. Never hand the application a
        // display time that does not move: it is not something a caller has
        // to tolerate, and the runtime safeguards added in v0.2.1 read a
        // non-advancing time as a projection resource layout change and stop
        // generation for a second, which the fail-open pacing that follows
        // then reproduces on the next frame.
        //
        // A nanosecond is enough. The drift the ceiling exists to prevent
        // accumulates in whole periods - it reached 637 s of lead in a
        // captured session - so a floor measured in nanoseconds keeps the
        // sequence strictly increasing without giving the ratchet anything
        // to turn on.
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = state->last_virtual_display_time + 1;
        }
        state->last_virtual_display_time = virtual_time;
        frame_state->predictedDisplayTime = virtual_time;
        frame_state->predictedDisplayPeriod = virtual_period;
        frame_state->shouldRender = state->presenter_frame_state.shouldRender;
    } else if (use_provisional_pipelined_wait) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        const XrDuration virtual_period = (state->manual_control.stop_requested() || !state->menu_enabled)
            ? state->last_inline_frame_state.predictedDisplayPeriod
            : virtual_display_period(
                state->last_inline_frame_state.predictedDisplayPeriod,
                state->frames_per_application_frame);
        const XrTime anchor = add_display_duration(
            state->last_inline_frame_state.predictedDisplayTime,
            promised_display_offset(*state, virtual_period,
                state->last_inline_frame_state.predictedDisplayPeriod));
        // Same ceiling as the presenter path above, for the same reason: this
        // branch repeats for as long as the promotion takes, and each repeat
        // would otherwise push the application's timeline a period further
        // from the one the runtime is predicting on.
        const XrTime ceiling = add_display_duration(anchor, virtual_period);
        XrTime virtual_time = anchor;
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = add_display_duration(
                    state->last_virtual_display_time,
                    virtual_period);
            }
            if (virtual_time > ceiling) {
                virtual_time = ceiling;
            }
            // Same floor as the presenter path, for the same reason: a
            // display time that does not advance is not something a caller
            // has to tolerate.
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = state->last_virtual_display_time + 1;
            }
            state->last_virtual_display_time = virtual_time;
        }
        frame_state->predictedDisplayTime = virtual_time;
        frame_state->predictedDisplayPeriod = virtual_period;
        frame_state->shouldRender =
            state->last_inline_frame_state.shouldRender;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            100,
            state->pipelined_wait_streak,
            static_cast<std::uint64_t>(virtual_time),
            static_cast<std::uint64_t>(virtual_period));
    } else {
        const auto application_wait_started = std::chrono::steady_clock::now();
        // Gated for the same reason begin, end and the swapchain calls are:
        // on a D3D11 session this is the runtime working on the application's
        // single immediate context, and a second thread inside it at the same
        // time is what kills the driver. See RuntimeEntry.
        result = with_runtime_entry_for_wait(state, [&] {
            return state->dispatch->wait_frame(session, wait_info, frame_state);
        });
        const auto application_wait_elapsed =
            std::chrono::steady_clock::now() - application_wait_started;
        if (XR_SUCCEEDED(result) && frame_state != nullptr) {
            state->last_inline_frame_state = *frame_state;
            state->last_inline_frame_state.next = nullptr;
            state->last_inline_frame_state_valid = true;
            state->last_application_wait_elapsed = application_wait_elapsed;
            forwarded_application_wait = true;
            state->runtime_frame_waited_unbegun = true;
        }
    }
    if (XR_SUCCEEDED(result) && frame_state != nullptr) {
        // The real frame's held-back copy goes out here, at frame start, so
        // it has the whole frame to complete in. Submitted later instead, its
        // hand-over lands on pixels still in flight and the interval before
        // the synthetic collapses inside one scanout. Submits and returns.
        static_cast<void>(flush_session_pending_copies(state));
        state->application_wait_pending_begin = true;
        std::scoped_lock lock(state->mutex);
        // Every runtime, not only SteamVR: the synthetic interpolation fraction
        // needs a display period wherever generation runs, and the promotion
        // that also reads this guards on the runtime itself. A runtime that
        // never reports one keeps the fixed midpoint.
        if (!use_continuous_presenter &&
            frame_state->predictedDisplayPeriod > 0 &&
            (state->minimum_runtime_display_period == 0 ||
             frame_state->predictedDisplayPeriod <
                 state->minimum_runtime_display_period)) {
            state->minimum_runtime_display_period =
                frame_state->predictedDisplayPeriod;
        }
        // The runtime's period can be a throttled one - SteamVR reports two
        // refreshes for an application it is throttling, and a session
        // throttled from its first frame never sees anything shorter. The
        // compositor's own refresh rate bounds it wherever it is known.
        if (!use_continuous_presenter && state->steamvr_delivery) {
            if (const auto scanout = state->steamvr_delivery->display_period();
                scanout && scanout->count() > 0 &&
                (state->minimum_runtime_display_period == 0 ||
                 scanout->count() < state->minimum_runtime_display_period)) {
                state->minimum_runtime_display_period = scanout->count();
            }
        }
        if (!use_continuous_presenter && state->fps_overlay) {
            state->fps_overlay->set_display_period(
                state->minimum_runtime_display_period);
        }
        // A runtime that is pacing this application blocks the wait for most
        // of a display period. SteamVR returns in about 1.55 ms against
        // 11.11 ms, which is not pacing anything. Count the waits that came
        // back too quickly, on the same half-period line the throttle detector
        // uses in the other direction.
        if (forwarded_application_wait &&
            state->minimum_runtime_display_period > 0) {
            const auto elapsed_nanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    state->last_application_wait_elapsed).count();
            state->unpaced_wait_streak =
                elapsed_nanoseconds * 2 < state->minimum_runtime_display_period
                    ? state->unpaced_wait_streak + 1
                    : 0;
        }
        state->pending_frames.push_back({
            frame_state->predictedDisplayTime,
            frame_state->predictedDisplayPeriod,
        });
        constexpr std::size_t kMaximumPendingFrameRecords = 32;
        while (state->pending_frames.size() > kMaximumPendingFrameRecords) {
            state->pending_frames.pop_front();
        }
    }
    return result;
}

XrResult layer_begin_frame_impl(
    XrSession session,
    const XrFrameBeginInfo* begin_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        if (continuous_presenter_active(state)) {
            result = valid_optional_frame_info(
                    begin_info, XR_TYPE_FRAME_BEGIN_INFO)
                ? XR_SUCCESS
                : XR_ERROR_VALIDATION_FAILURE;
        } else if (state->pipelined_presenter_mode) {
            result = XR_ERROR_RUNTIME_FAILURE;
        } else {
            XrResult replacement_wait = XR_SUCCESS;
            if (state->application_begin_needs_wait) {
                // The inline cycle begun the frame this application's wait
                // returned; take it another before its begin, on its own
                // thread. See application_begin_needs_wait.
                state->application_begin_needs_wait = false;
                XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState replacement{XR_TYPE_FRAME_STATE};
                const auto wait_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::internal_wait_frame,
                    handle_value(session));
                replacement_wait = with_runtime_entry_for_wait(state, [&] {
                    return state->dispatch->wait_frame(
                        session, &wait_info, &replacement);
                });
                xrfg::bridge_flight_logger().end(
                    wait_token,
                    xrfg::BridgeFlightOperation::internal_wait_frame,
                    replacement_wait,
                    static_cast<std::uint64_t>(replacement.predictedDisplayTime),
                    static_cast<std::uint64_t>(replacement.predictedDisplayPeriod),
                    replacement.shouldRender);
                if (XR_SUCCEEDED(replacement_wait)) {
                    state->last_inline_frame_state = replacement;
                    state->last_inline_frame_state.next = nullptr;
                    state->last_inline_frame_state_valid = true;
                }
            }
            result = XR_FAILED(replacement_wait)
                ? replacement_wait
                : with_runtime_entry(state, [&] {
                      return state->dispatch->begin_frame(session, begin_info);
                  });
            if (XR_SUCCEEDED(result)) {
                state->runtime_frame_waited_unbegun = false;
            }
        }
    } catch (...) {
        if (state->application_wait_pending_begin) {
            state->application_wait_pending_begin = false;
            frame_call_lock.unlock();
            state->frame_call_condition.notify_all();
        }
        throw;
    }
    if (XR_SUCCEEDED(result)) {
        state->application_frame_in_progress = true;
        state->application_frame_has_overlapping_wait = false;
    }
    if (state->application_wait_pending_begin) {
        state->application_wait_pending_begin = false;
        frame_call_lock.unlock();
        state->frame_call_condition.notify_all();
    }
    return result;
}

XrResult layer_create_swapchain_impl(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    const auto state = find_session(session);
    if (!state || state->dispatch->create_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (create_info == nullptr || swapchain == nullptr) {
        return state->dispatch->create_swapchain(session, create_info, swapchain);
    }

    PresenterResourceLifetimeGuard presenter_guard(state);
    // A new swapchain no longer holds generation off for the structural
    // quarantine's second, for the reason destroying one stopped doing so
    // (layer_destroy_swapchain_impl): generation resources are only taken
    // when a projection layer first names a swapchain, so a new one changes
    // nothing the current pair uses, and a projection that moves onto it is
    // still handled as a projection change. Riven made a 1024x1024 swapchain
    // two seconds into generation and lost the second's 120 generated
    // frames to the deadline alone.
    // Bridged and multisampled: the runtime gets a single-sample swapchain
    // and the application a multisampled texture of its own, resolved at
    // release (D3D11BridgePath::resolve). Every path below sees the
    // single-sample create info, which is what the runtime's images are.
    XrSwapchainCreateInfo bridged_create_info{};
    const XrSwapchainCreateInfo* runtime_create_info = create_info;
    if (state->d3d11_bridge && create_info->sampleCount > 1) {
        bridged_create_info = *create_info;
        bridged_create_info.sampleCount = 1;
        runtime_create_info = &bridged_create_info;
    }
    // Bridged Vulkan: the application's format is a VkFormat and the
    // runtime's session is D3D12, so the create info the runtime sees
    // carries the DXGI equivalent, and so does everything below, which is
    // what the runtime's images are. A format with no equivalent is refused
    // the way a runtime refuses one it does not list; so is multisampled
    // colour, which the bridge does not resolve.
    if (state->vulkan_bridge) {
        const auto vulkan_format = static_cast<VkFormat>(create_info->format);
        const bool depth =
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const DXGI_FORMAT dxgi_format = depth
            ? xrfg::dxgi_depth_format_for_vulkan(vulkan_format)
            : xrfg::dxgi_format_for_vulkan(vulkan_format);
        if (dxgi_format == DXGI_FORMAT_UNKNOWN ||
            (!depth && create_info->sampleCount > 1)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                4,
                static_cast<std::uint64_t>(create_info->format),
                create_info->usageFlags,
                create_info->sampleCount);
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        bridged_create_info = *create_info;
        bridged_create_info.format = static_cast<std::int64_t>(dxgi_format);
        // A Vulkan application need not render into the swapchain image:
        // OpenComposite copies the game's own texture in, so No Man's Sky's
        // swapchains carry only TRANSFER_DST. The runtime's image and the
        // shared texture are colour attachments whatever was asked, which
        // is what makes the swapchain a colour one to every path below.
        if (!depth) {
            bridged_create_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        }
        // The runtime's depth images are never written (depth_private), so
        // they are the cheapest shape that creates.
        if (depth) {
            bridged_create_info.sampleCount = 1;
        }
        runtime_create_info = &bridged_create_info;
    }
    auto swapchain_state = std::make_shared<SwapchainState>(state, *runtime_create_info);
    XrSwapchain created_swapchain = XR_NULL_HANDLE;
    const XrResult result = state->dispatch->create_swapchain(
        session,
        runtime_create_info,
        &created_swapchain);
    if (XR_FAILED(result)) {
        // The application losing a swapchain of its own is the shape a layer
        // that overspends the runtime's budget takes from the outside, and
        // without this record the log ends at the last creation that worked.
        // A negative result distinguishes it from the create-info records
        // below, which carry a field index there.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::swapchain_create,
            result,
            0,
            (static_cast<std::uint64_t>(create_info->width) << 32) |
                create_info->height,
            static_cast<std::uint64_t>(create_info->usageFlags));
        return result;
    }
    swapchain_state->handle = created_swapchain;

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        0,
        handle_value(created_swapchain),
        static_cast<std::uint64_t>(create_info->createFlags),
        static_cast<std::uint64_t>(create_info->usageFlags));
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        1,
        handle_value(created_swapchain),
        (static_cast<std::uint64_t>(create_info->width) << 32) |
            create_info->height,
        (static_cast<std::uint64_t>(create_info->arraySize) << 32) |
            create_info->sampleCount);
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        2,
        handle_value(created_swapchain),
        static_cast<std::uint64_t>(create_info->format),
        (static_cast<std::uint64_t>(create_info->faceCount) << 32) |
            create_info->mipCount);
    log_video_memory(*state, VideoMemoryStage::application_swapchain,
        handle_value(created_swapchain));

    try {
        {
            std::scoped_lock lock(g_state_mutex);
            g_swapchains[created_swapchain] = swapchain_state;
        }
    } catch (const std::bad_alloc&) {
        state->dispatch->destroy_swapchain(created_swapchain);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        state->dispatch->destroy_swapchain(created_swapchain);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (state->d3d11_bridge) {
        // Bridge it now, while the runtime's images can be enumerated
        // without the application seeing D3D12 ones: the application's
        // enumeration is answered from the bridge. A shape the bridge cannot
        // share fails the creation, which the application sees as a
        // swapchain it could not create.
        std::uint32_t runtime_count = 0;
        std::vector<XrSwapchainImageD3D12KHR> runtime_images;
        XrResult bridge_result = state->dispatch->enumerate_swapchain_images(
            created_swapchain, 0, &runtime_count, nullptr);
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            runtime_images.assign(
                runtime_count,
                XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            bridge_result = state->dispatch->enumerate_swapchain_images(
                created_swapchain, runtime_count, &runtime_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data()));
        }
        HRESULT shared_result = E_FAIL;
        std::uint32_t failure_stage = 0;
        auto bridge = std::make_shared<xrfg::D3D11BridgeSwapchain>();
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            std::vector<ID3D12Resource*> resources;
            for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
                resources.push_back(image.texture);
            }
            xrfg::D3D11BridgeSwapchainDescription requested{};
            requested.requested_format =
                static_cast<DXGI_FORMAT>(create_info->format);
            requested.requested_sample_count = create_info->sampleCount;
            requested.unordered_access =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;
            requested.depth_stencil =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            shared_result = bridge->initialize(
                state->d3d11_bridge->device.Get(),
                state->d3d11_bridge->context.Get(),
                state->d3d12_device.Get(),
                state->d3d12_queue.Get(),
                resources,
                requested,
                &failure_stage);
        }
        if (XR_FAILED(bridge_result) || FAILED(shared_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                XR_FAILED(bridge_result)
                    ? static_cast<std::int64_t>(bridge_result)
                    : static_cast<std::int64_t>(shared_result),
                handle_value(created_swapchain),
                failure_stage,
                static_cast<std::uint64_t>(create_info->format));
            {
                std::scoped_lock lock(g_state_mutex);
                g_swapchains.erase(created_swapchain);
            }
            state->dispatch->destroy_swapchain(created_swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        // 2: bridged. b the image count and, in the high half, the path
        // (D3D11BridgePath); c the requested format and, in the high half,
        // the format the shared textures took from the runtime's images.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            2,
            handle_value(created_swapchain),
            (static_cast<std::uint64_t>(bridge->path()) << 32) | runtime_count,
            (static_cast<std::uint64_t>(bridge->shared_format()) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(create_info->format)));
        if (bridge->path() == xrfg::D3D11BridgePath::depth_private) {
            state->has_private_depth_swapchain.store(true, std::memory_order_release);
            swapchain_state->private_depth = true;
        }
        std::scoped_lock lock(swapchain_state->mutex);
        swapchain_state->bridge = std::move(bridge);
    } else if (state->vulkan_bridge) {
        // Bridge it now, as for D3D11: the application's enumeration is
        // answered from the bridge's imported images.
        std::uint32_t runtime_count = 0;
        std::vector<XrSwapchainImageD3D12KHR> runtime_images;
        XrResult bridge_result = state->dispatch->enumerate_swapchain_images(
            created_swapchain, 0, &runtime_count, nullptr);
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            runtime_images.assign(
                runtime_count,
                XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            bridge_result = state->dispatch->enumerate_swapchain_images(
                created_swapchain, runtime_count, &runtime_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data()));
        }
        HRESULT shared_result = E_FAIL;
        std::uint32_t failure_stage = 0;
        auto bridge = std::make_shared<xrfg::VulkanBridgeSwapchain>();
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            std::vector<ID3D12Resource*> resources;
            for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
                resources.push_back(image.texture);
            }
            xrfg::VulkanBridgeSwapchainDescription requested{};
            requested.requested_format = static_cast<VkFormat>(create_info->format);
            requested.width = create_info->width;
            requested.height = create_info->height;
            requested.array_size = create_info->arraySize;
            requested.mip_levels = create_info->mipCount;
            requested.sample_count = create_info->sampleCount;
            requested.unordered_access =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;
            requested.depth_stencil =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            shared_result = bridge->initialize(
                state->vulkan_bridge->binding,
                state->d3d12_device.Get(),
                state->d3d12_queue.Get(),
                resources,
                requested,
                &failure_stage);
        }
        if (XR_FAILED(bridge_result) || FAILED(shared_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                XR_FAILED(bridge_result)
                    ? static_cast<std::int64_t>(bridge_result)
                    : static_cast<std::int64_t>(shared_result),
                handle_value(created_swapchain),
                failure_stage,
                static_cast<std::uint64_t>(create_info->format));
            {
                std::scoped_lock lock(g_state_mutex);
                g_swapchains.erase(created_swapchain);
            }
            state->dispatch->destroy_swapchain(created_swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::vulkan_bridge,
            2,
            handle_value(created_swapchain),
            (static_cast<std::uint64_t>(bridge->path()) << 32) | runtime_count,
            (static_cast<std::uint64_t>(bridge->shared_format()) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(create_info->format)));
        if (bridge->path() == xrfg::VulkanBridgePath::depth_private) {
            state->has_private_depth_swapchain.store(true, std::memory_order_release);
            swapchain_state->private_depth = true;
        }
        std::scoped_lock lock(swapchain_state->mutex);
        swapchain_state->vulkan_bridge = std::move(bridge);
    }
    *swapchain = created_swapchain;
    return result;
}

XrResult layer_destroy_swapchain_impl(XrSwapchain swapchain) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->destroy_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    PresenterResourceLifetimeGuard presenter_guard(state->session);
    const bool active_color_reconfiguration =
        state->session->generation_steady_state_established &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    if (active_color_reconfiguration) {
        // Drop the pair and the retained repeat - the repeat's composition
        // names this swapchain - but do not hold generation off for the
        // structural quarantine's full second.
        //
        // The wall-clock deadline was covering a race that lazy arming has
        // since removed. The lines below drain this swapchain's GPU work and
        // destroy its private swapchains synchronously, under the call lock
        // and with the presenter held by the guard above, so nothing queued
        // names anything being torn down by the time this returns. What the
        // deadline additionally prevented was the layer arming the
        // replacement swapchains the moment they were enumerated, mid
        // reconfiguration; generation resources are now taken when a
        // projection layer first names a swapchain, so the application
        // decides when the replacement is ready and the layer cannot run
        // ahead of it.
        //
        // It is not free to keep. MSFS 2024 recreates its swapchains on world
        // and settings transitions - four times in 96 s in one capture - and
        // each one cost almost exactly a second of generation: the
        // replacements were enumerated within 40 ms and armed 1.07 s later,
        // the delay being the deadline and nothing else. Every other
        // quarantine reason is unchanged.
        clear_generation_continuity(state->session);
        {
            std::scoped_lock presenter_lock(state->session->presenter_mutex);
            state->session->presenter_last_frame.reset();
        }
        state->session->generation_steady_state_established = false;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::continuity_reset,
            static_cast<std::int64_t>(
                GenerationQuarantineReason::swapchain_destroyed),
            handle_value(state->session->handle),
            handle_value(swapchain),
            0);
    }
    std::scoped_lock call_lock(state->call_mutex);
    drain_swapchain_gpu(state);
    destroy_frame_generation_swapchains(state);
    {
        // The runtime's images go with the swapchain; the shared textures
        // the application rendered into go with the bridge, after the drain.
        std::scoped_lock lock(state->mutex);
        state->bridge.reset();
        state->vulkan_bridge.reset();
    }
    const XrResult result = state->session->dispatch->destroy_swapchain(swapchain);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        g_swapchains.erase(swapchain);
    }
    log_video_memory(*state->session, VideoMemoryStage::swapchain_destroyed,
        handle_value(swapchain));
    return result;
}

XrResult layer_enumerate_swapchain_images_impl(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->enumerate_swapchain_images == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    // Resource/context creation must observe one applied menu configuration,
    // and no presenter-owned composition may retain the resources while an
    // application replaces them during a resize/reconfigure transaction.
    PresenterResourceLifetimeGuard presenter_guard(state->session);
    std::scoped_lock call_lock(state->call_mutex);
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    {
        std::scoped_lock lock(state->mutex);
        bridge = state->bridge;
        vulkan_bridge = state->vulkan_bridge;
    }
    std::vector<ID3D12Resource*> bridged_resources;
    const bool bridged = bridge != nullptr || vulkan_bridge != nullptr;
    XrResult result = XR_SUCCESS;
    if (vulkan_bridge) {
        // The application gets the bridge's imported Vulkan images, never
        // the runtime's D3D12 ones; the D3D12 bookkeeping below sees the
        // shared textures, which is what history captures from.
        const auto vulkan_views = vulkan_bridge->vulkan_images();
        const auto count = static_cast<std::uint32_t>(vulkan_views.size());
        if (image_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = count;
        if (image_capacity_input == 0 || images == nullptr) {
            return XR_SUCCESS;
        }
        if (image_capacity_input < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        auto* vulkan_images = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (vulkan_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            vulkan_images[index].image = vulkan_views[index];
        }
        const auto shared = vulkan_bridge->shared_images();
        bridged_resources.assign(shared.begin(), shared.end());
    } else if (bridge) {
        // The application gets the bridge's D3D11 textures, never the
        // runtime's D3D12 images; the D3D12 bookkeeping below sees the
        // shared textures, which is what history captures from.
        const auto d3d11_views = bridge->d3d11_images();
        const auto count = static_cast<std::uint32_t>(d3d11_views.size());
        if (image_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = count;
        if (image_capacity_input == 0 || images == nullptr) {
            return XR_SUCCESS;
        }
        if (image_capacity_input < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        auto* d3d11_images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (d3d11_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d11_images[index].texture = d3d11_views[index];
        }
        const auto shared = bridge->shared_images();
        bridged_resources.assign(shared.begin(), shared.end());
    } else {
        result = state->session->dispatch->enumerate_swapchain_images(
            swapchain,
            image_capacity_input,
            image_count_output,
            images);
        if (XR_FAILED(result) || image_count_output == nullptr || images == nullptr || image_capacity_input == 0) {
            return result;
        }
    }

    try {
        if (state->session->graphics_binding == SessionGraphicsBinding::d3d11) {
            if (*image_count_output == 0 ||
                image_capacity_input < *image_count_output) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::incomplete_enumeration,
                    image_capacity_input,
                    *image_count_output);
                return result;
            }
            const std::uint32_t count = *image_count_output;
            std::vector<ID3D11Texture2D*> resources(count);
            auto* d3d11_images =
                reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            for (std::uint32_t index = 0; index < count; ++index) {
                if (d3d11_images[index].type !=
                        XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR ||
                    d3d11_images[index].texture == nullptr) {
                    log_swapchain_eligibility(
                        state,
                        SwapchainEligibilityReason::invalid_d3d11_image,
                        index,
                        static_cast<std::uint64_t>(d3d11_images[index].type));
                    return result;
                }
                resources[index] = d3d11_images[index].texture;
                D3D11_TEXTURE2D_DESC description{};
                d3d11_images[index].texture->GetDesc(&description);
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::swapchain_image,
                    static_cast<std::int64_t>(description.Format),
                    handle_value(state->handle),
                    handle_value(d3d11_images[index].texture),
                    (static_cast<std::uint64_t>(description.BindFlags) << 32) |
                        description.MiscFlags);
            }

            bool has_generation = false;
            bool resources_changed = false;
            {
                std::scoped_lock lock(state->mutex);
                resources_changed = !state->enumerated_d3d11_images.empty() &&
                    (state->enumerated_d3d11_images.size() != resources.size() ||
                     !std::equal(
                         state->enumerated_d3d11_images.begin(),
                         state->enumerated_d3d11_images.end(),
                         resources.begin(),
                         [](const auto& stored, ID3D11Texture2D* current) {
                             return stored.Get() == current;
                         }));
            }
            if (resources_changed) {
                schedule_generation_quarantine(
                    state->session,
                    GenerationQuarantineReason::d3d11_images_changed,
                    handle_value(state->handle));
                drain_swapchain_gpu(state);
                destroy_frame_generation_swapchains(state);
                std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
                {
                    std::scoped_lock lock(state->mutex);
                    retired_history = std::move(state->d3d12_history);
                    state->last_released_capture.reset();
                    state->last_released_motion_vectors.reset();
                }
                retired_history.reset();
            }
            {
                std::scoped_lock lock(state->mutex);
                state->enumerated_d3d11_images.clear();
                for (auto* resource : resources) state->enumerated_d3d11_images.emplace_back(resource);
                has_generation = static_cast<bool>(state->frame_generation);
            }
            SwapchainEligibilityReason eligibility_reason =
                SwapchainEligibilityReason::ready;
            std::uint64_t eligibility_detail = count;
            // Same deferral as the D3D12 path below, and for the same reason:
            // an interop swapchain costs the session three runtime swapchains
            // and an application's UI surfaces are indistinguishable from its
            // stereo views here. The D3D12 path can rebuild its images from
            // the history ring when it arms; this one has to keep them.
            const bool static_image =
                (state->create_info.createFlags &
                 XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
            if (!has_generation && !state->generation_declined &&
                !state->session->generation_budget_exhausted.load(
                    std::memory_order_acquire)) {
                if (static_image) {
                    eligibility_reason =
                        SwapchainEligibilityReason::static_image;
                    eligibility_detail = state->create_info.createFlags;
                } else if (state->create_info.faceCount != 1) {
                    eligibility_reason =
                        SwapchainEligibilityReason::unsupported_face_count;
                    eligibility_detail = state->create_info.faceCount;
                } else {
                    state->enumerated_image_count = count;
                    state->generation_eligible_pending = true;
                    eligibility_reason =
                        SwapchainEligibilityReason::awaiting_projection_use;
                }
            }
            log_swapchain_eligibility(
                state,
                has_generation ? SwapchainEligibilityReason::ready
                               : eligibility_reason,
                eligibility_detail,
                (static_cast<std::uint64_t>(count) << 32) |
                    state->create_info.arraySize);
            return result;
        }
        if (state->session->graphics_binding == SessionGraphicsBinding::vulkan) {
            if (*image_count_output == 0 ||
                image_capacity_input < *image_count_output) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::incomplete_enumeration,
                    image_capacity_input,
                    *image_count_output);
                return result;
            }
            const std::uint32_t count = *image_count_output;
            std::vector<VkImage> resources(count);
            auto* vulkan_images =
                reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
            for (std::uint32_t index = 0; index < count; ++index) {
                if (vulkan_images[index].type !=
                        XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR ||
                    vulkan_images[index].image == VK_NULL_HANDLE) {
                    log_swapchain_eligibility(
                        state,
                        SwapchainEligibilityReason::invalid_vulkan_image,
                        index,
                        static_cast<std::uint64_t>(vulkan_images[index].type));
                    return result;
                }
                resources[index] = vulkan_images[index].image;
                // A Vulkan image cannot be asked for its description; the
                // create info stands in for it.
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::swapchain_image,
                    state->create_info.format,
                    handle_value(state->handle),
                    static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(vulkan_images[index].image)),
                    state->create_info.usageFlags);
            }

            bool has_generation = false;
            bool resources_changed = false;
            {
                std::scoped_lock lock(state->mutex);
                resources_changed = !state->enumerated_vulkan_images.empty() &&
                    (state->enumerated_vulkan_images.size() != resources.size() ||
                     !std::equal(
                         state->enumerated_vulkan_images.begin(),
                         state->enumerated_vulkan_images.end(),
                         resources.begin()));
            }
            if (resources_changed) {
                schedule_generation_quarantine(
                    state->session,
                    GenerationQuarantineReason::vulkan_images_changed,
                    handle_value(state->handle));
                drain_swapchain_gpu(state);
                destroy_frame_generation_swapchains(state);
                std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
                {
                    std::scoped_lock lock(state->mutex);
                    retired_history = std::move(state->d3d12_history);
                    state->last_released_capture.reset();
                    state->last_released_motion_vectors.reset();
                }
                retired_history.reset();
                state->generation_declined = false;
            }
            {
                std::scoped_lock lock(state->mutex);
                state->enumerated_vulkan_images = resources;
                has_generation = static_cast<bool>(state->frame_generation);
            }
            SwapchainEligibilityReason eligibility_reason =
                SwapchainEligibilityReason::ready;
            std::uint64_t eligibility_detail = count;
            // Deferred to first projection use, as the D3D11 path is.
            const bool static_image =
                (state->create_info.createFlags &
                 XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
            if (!has_generation && !state->generation_declined &&
                !state->session->generation_budget_exhausted.load(
                    std::memory_order_acquire)) {
                if (!state->session->vulkan_support) {
                    eligibility_reason =
                        SwapchainEligibilityReason::vulkan_support_off;
                } else if (static_image) {
                    eligibility_reason =
                        SwapchainEligibilityReason::static_image;
                    eligibility_detail = state->create_info.createFlags;
                } else if (state->create_info.faceCount != 1) {
                    eligibility_reason =
                        SwapchainEligibilityReason::unsupported_face_count;
                    eligibility_detail = state->create_info.faceCount;
                } else if (state->session->d3d12_device.Get() == nullptr ||
                           state->session->d3d12_queue.Get() == nullptr) {
                    eligibility_reason =
                        SwapchainEligibilityReason::no_d3d12_binding;
                } else if (xrfg::dxgi_format_for_vulkan(static_cast<VkFormat>(
                               state->create_info.format)) ==
                           DXGI_FORMAT_UNKNOWN) {
                    eligibility_reason =
                        SwapchainEligibilityReason::unsupported_vulkan_format;
                    eligibility_detail =
                        static_cast<std::uint64_t>(state->create_info.format);
                } else {
                    state->enumerated_image_count = count;
                    state->generation_eligible_pending = true;
                    eligibility_reason =
                        SwapchainEligibilityReason::awaiting_projection_use;
                }
            }
            log_swapchain_eligibility(
                state,
                has_generation ? SwapchainEligibilityReason::ready
                               : eligibility_reason,
                eligibility_detail,
                (static_cast<std::uint64_t>(count) << 32) |
                    state->create_info.arraySize);
            return result;
        }
        if (state->session->d3d12_device.Get() == nullptr ||
            state->session->d3d12_queue.Get() == nullptr) {
            log_swapchain_eligibility(
                state, SwapchainEligibilityReason::no_d3d12_binding);
            return result;
        }
        if (*image_count_output == 0 || image_capacity_input < *image_count_output) {
            log_swapchain_eligibility(
                state,
                SwapchainEligibilityReason::incomplete_enumeration,
                image_capacity_input,
                *image_count_output);
            return result;
        }
        const std::uint32_t count = *image_count_output;

        std::vector<ID3D12Resource*> resources(count);
        auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        if (bridged) {
            resources = bridged_resources;
        }
        for (std::uint32_t index = 0; index < count && !bridged; ++index) {
            if (d3d12_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ||
                d3d12_images[index].texture == nullptr) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::invalid_d3d12_image,
                    index,
                    static_cast<std::uint64_t>(d3d12_images[index].type));
                return result;
            }
            resources[index] = d3d12_images[index].texture;
        }
        if (!bridged && !resources.empty()) {
            log_d3d12_image_description(state->handle, resources.front());
        }
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        bool resources_changed = false;
        {
            std::scoped_lock lock(state->mutex);
            history = state->d3d12_history;
            resources_changed = !state->enumerated_d3d12_images.empty() &&
                (state->enumerated_d3d12_images.size() != resources.size() ||
                 !std::equal(
                     state->enumerated_d3d12_images.begin(),
                     state->enumerated_d3d12_images.end(),
                     resources.begin(),
                     [](const auto& stored, ID3D12Resource* current) {
                         return stored.Get() == current;
                     }));
            state->enumerated_d3d12_images.clear();
            for (auto* resource : resources) state->enumerated_d3d12_images.emplace_back(resource);
        }
        if (resources_changed) {
            schedule_generation_quarantine(
                state->session,
                GenerationQuarantineReason::d3d12_images_changed,
                handle_value(state->handle));
        }
        const bool reused_history =
            !resources_changed && history && history->initialized();
        if (!reused_history) {
            history.reset();
        }
        const auto release_state = required_release_state(*state);
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        // Nothing synthesizes from depth, so a depth swapchain gets no
        // history: one would hold three full-size copies of the depth images
        // and copy one in at every release. At 8192x6412 per eye that was
        // 0.75 GB per eye, measured.
        const bool depth_only = release_state &&
            *release_state == D3D12_RESOURCE_STATE_DEPTH_WRITE;
        HRESULT history_result = S_OK;
        bool history_attempted = false;
        if (!history && release_state && !depth_only && !protected_content) {
            history_attempted = true;
            auto candidate = std::make_shared<xrfg::D3D12SwapchainHistory>();
            history_result = candidate->initialize(
                state->session->d3d12_device.Get(),
                state->session->d3d12_queue.Get(),
                std::span<ID3D12Resource* const>(resources.data(), resources.size()),
                *release_state);
            if (SUCCEEDED(history_result)) {
                history = std::move(candidate);
                log_video_memory(*state->session, VideoMemoryStage::history,
                    handle_value(state->handle));
            }
        }
        if (!reused_history) {
            drain_swapchain_gpu(state);
            destroy_frame_generation_swapchains(state);
            std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
            {
                std::scoped_lock lock(state->mutex);
                retired_history = std::move(state->d3d12_history);
                state->d3d12_history = history;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
            }
            retired_history.reset();
            // The images themselves changed, so an earlier refusal says
            // nothing about this set.
            state->generation_declined = false;
        }
        state->enumerated_image_count = count;

        bool has_generation = false;
        {
            std::scoped_lock lock(state->mutex);
            has_generation = static_cast<bool>(state->frame_generation);
        }
        SwapchainEligibilityReason eligibility_reason =
            SwapchainEligibilityReason::ready;
        std::uint64_t eligibility_detail = count;
        if (!release_state) {
            eligibility_reason =
                SwapchainEligibilityReason::ambiguous_attachment_usage;
            eligibility_detail = state->create_info.usageFlags;
        } else if (protected_content) {
            eligibility_reason = SwapchainEligibilityReason::protected_content;
            eligibility_detail = state->create_info.createFlags;
        } else if (depth_only) {
            eligibility_reason = SwapchainEligibilityReason::depth_only;
            eligibility_detail = state->create_info.usageFlags;
        } else if (!history) {
            eligibility_reason = SwapchainEligibilityReason::history_initialize_failed;
            eligibility_detail = history_attempted
                ? static_cast<std::uint64_t>(history_result)
                : 0;
        }
        // Generation costs three runtime swapchains here and a runtime caps how
        // many one session may hold at all. Nothing about a colour swapchain
        // says whether the application will submit it as a projection view or
        // as a UI quad it composites once, so spending the budget now spends it
        // on both -- and the application, still creating its own swapchains,
        // is the one that finds the ceiling. Record the swapchain as a
        // candidate and let the first xrEndFrame that names it decide.
        //
        // What the create info alone settles is still settled here. A static
        // image and a face count above one can never carry generation, so
        // deferring them would put a candidacy in the log that is never
        // resolved in place of the true reason, and leave an arming attempt to
        // discover at the first frame what was knowable at creation.
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (history && release_state == D3D12_RESOURCE_STATE_RENDER_TARGET &&
            !has_generation && !state->generation_declined &&
            !state->session->generation_budget_exhausted.load(
                std::memory_order_acquire)) {
            if (static_image) {
                eligibility_reason = SwapchainEligibilityReason::static_image;
                eligibility_detail = state->create_info.createFlags;
            } else if (state->create_info.faceCount != 1) {
                eligibility_reason =
                    SwapchainEligibilityReason::unsupported_face_count;
                eligibility_detail = state->create_info.faceCount;
            } else {
                state->generation_eligible_pending = true;
                eligibility_reason =
                    SwapchainEligibilityReason::awaiting_projection_use;
            }
        }

        log_swapchain_eligibility(
            state,
            has_generation ? SwapchainEligibilityReason::ready
                           : eligibility_reason,
            eligibility_detail,
            (static_cast<std::uint64_t>(count) << 32) |
                state->create_info.arraySize);

    } catch (...) {
        log_swapchain_eligibility(
            state, SwapchainEligibilityReason::exception);
    }
    return result;
}

XrResult layer_acquire_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->acquire_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = with_runtime_entry(state->session.get(), [&] {
        return state->session->dispatch->acquire_swapchain_image(
            swapchain, acquire_info, index);
    });
    if (XR_SUCCEEDED(result) && index != nullptr) {
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid) {
            try {
                state->acquired_indices.push_back(*index);
            } catch (...) {
                state->acquired_indices.clear();
                state->waited_count = 0;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->ownership_tracking_valid = false;
            }
        }
    }
    return result;
}

XrResult layer_wait_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->wait_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = with_runtime_entry(state->session.get(), [&] {
        return state->session->dispatch->wait_swapchain_image(
            swapchain, wait_info);
    });
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) {
        // The runtime made the image safe to write on the binding queue;
        // the application writes it on its own.
        carry_reacquire_to_application(state->session.get());
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid &&
            state->waited_count < state->acquired_indices.size()) {
            const std::uint32_t waited_index =
                state->acquired_indices[state->waited_count];
            if (state->bridge) {
                // Bridged: the application is about to render into the
                // shared texture, and the layer's queue may still be
                // reading it.
                static_cast<void>(state->bridge->before_write(waited_index));
            } else if (state->vulkan_bridge) {
                static_cast<void>(state->vulkan_bridge->before_write(waited_index));
            }
            ++state->waited_count;
        }
    }
    return result;
}

XrResult layer_release_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->release_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    std::optional<std::uint32_t> candidate_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
    std::shared_ptr<xrfg::SwapchainInterop> interop;
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    {
        std::scoped_lock lock(state->mutex);
        bridge = state->bridge;
        vulkan_bridge = state->vulkan_bridge;
        if (state->ownership_tracking_valid && state->waited_count > 0 &&
            !state->acquired_indices.empty()) {
            candidate_index = state->acquired_indices.front();
            history = state->d3d12_history;
            if (state->frame_generation) {
                interop = state->frame_generation->interop;
            }
        }
    }
    // Bridged: the application's rendering goes to the runtime's image now,
    // whatever generation does with it afterwards. A failed copy is logged
    // and the release still goes down, so the runtime shows its last content
    // rather than the session failing.
    if (vulkan_bridge && candidate_index) {
        // Bridged Vulkan: the same hand-over as D3D11's below, and the same
        // policy on failure.
        const HRESULT copy_result = vulkan_bridge->release(*candidate_index);
        if (FAILED(copy_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                3,
                handle_value(state->handle),
                *candidate_index,
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(copy_result)));
        }
    }
    if (bridge && candidate_index) {
        const HRESULT copy_result = bridge->release(*candidate_index);
        if (FAILED(copy_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                3,
                handle_value(state->handle),
                *candidate_index,
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(copy_result)));
        }
    }

    std::unique_lock<std::mutex> gpu_lock;
    if (state->session->manual_control.stop_requested() || !state->session->menu_enabled) {
        // Preserve the application's acquire/wait/release bookkeeping, but
        // stop recording new bridge history after the manual arm is revoked.
        history.reset();
        interop.reset();
    }
    // Native D3D12 with capture_at_end_frame: the capture waits for the
    // application's xrEndFrame (capture_pending_end_frame_images). A release
    // says the image's rendering has been submitted, and an application whose
    // submission trails its release calls makes that false for the image it
    // releases first; a capture queued here then copies the previous frame,
    // for that eye alone. The bridges and the D3D11 interop keep their
    // release-time capture: their copy into the runtime's image is made here
    // too, so the two would otherwise disagree.
    const bool defer_capture = candidate_index && history &&
        state->session->capture_at_end_frame && !interop && !bridge &&
        !vulkan_bridge;
    if (candidate_index && history && !defer_capture) {
        gpu_lock = std::unique_lock<std::mutex>(state->session->gpu_mutex);
    }

    std::optional<xrfg::D3D12HistoryCaptureTicket> pending_capture;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> pending_motion_vectors;
    if (candidate_index && history && !defer_capture) {
        if (state->session->dlss_motion_vectors) {
            std::scoped_lock lock(state->mutex);
            if (*candidate_index < state->enumerated_d3d12_images.size()) {
                const int eye = state->projection_eye.load(std::memory_order_relaxed);
                const std::uint64_t now = xrfg::dlss_motion_vector_publications();
                pending_motion_vectors = xrfg::resolve_dlss_motion_vectors(
                    state->enumerated_d3d12_images[*candidate_index].Get(),
                    state->session->d3d12_queue.Get(),
                    eye,
                    now,
                    eye == 0 || eye == 1
                        ? state->session->eye_release_publication[1 - eye].load(
                              std::memory_order_relaxed)
                        : 0);
            }
        }
        xrfg::D3D12HistoryCaptureTicket ticket{};
        HRESULT capture_result = S_OK;
        if (interop) {
            const auto interop_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::d3d11_capture,
                handle_value(state->handle),
                *candidate_index,
                0);
            capture_result = interop->prepare_capture(*candidate_index);
            xrfg::bridge_flight_logger().end(
                interop_token,
                xrfg::BridgeFlightOperation::d3d11_capture,
                capture_result,
                handle_value(state->handle),
                *candidate_index,
                0);
        }
        if (SUCCEEDED(capture_result)) {
            capture_result = history->capture(*candidate_index, &ticket);
        }
        if (SUCCEEDED(capture_result) && interop) {
            const HRESULT finish_result = interop->finish_capture();
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_capture,
                finish_result,
                handle_value(state->handle),
                *candidate_index,
                1);
            capture_result = finish_result;
        }
        if (SUCCEEDED(capture_result)) {
            pending_capture = ticket;
        } else if (ticket.serial != 0) {
            history->discard(ticket);
        }
    }
    if (bridge && candidate_index) {
        // Every read of the shared texture this frame - the copy and the
        // capture - is queued; the next acquire of this image waits for it.
        static_cast<void>(bridge->mark_read(*candidate_index));
    }
    if (vulkan_bridge && candidate_index) {
        static_cast<void>(vulkan_bridge->mark_read(*candidate_index));
    }

    // Where the application's queue stands with this image rendered, for the
    // binding queue to wait on before a frame that names it goes down.
    mark_application_release(state->session.get());
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        result = with_runtime_entry(state->session.get(), [&] {
            return state->session->dispatch->release_swapchain_image(
                swapchain, release_info);
        });
    } catch (...) {
        if (pending_capture && history) {
            history->discard(*pending_capture);
        }
        throw;
    }

    if (XR_SUCCEEDED(result)) {
        bool commit_capture = false;
        {
            std::scoped_lock lock(state->mutex);
            if (state->ownership_tracking_valid && state->waited_count > 0 &&
                !state->acquired_indices.empty()) {
                const std::uint32_t released_index = state->acquired_indices.front();
                state->acquired_indices.pop_front();
                --state->waited_count;
                state->last_released_index = released_index;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                commit_capture = pending_capture &&
                                 pending_capture->source_index == released_index;
                // Deferred: the capture of this image is queued at the
                // application's xrEndFrame. A release before then replaces
                // it, the way the runtime shows only the last released image.
                if (defer_capture) {
                    state->pending_end_frame_capture = released_index;
                }
                if (state->session->dlss_motion_vectors) {
                    state->release_publication = xrfg::dlss_motion_vector_publications();
                    const int eye = state->projection_eye.load(std::memory_order_relaxed);
                    if (eye == 0 || eye == 1) {
                        state->session->eye_release_publication[static_cast<std::size_t>(eye)].store(
                            state->release_publication, std::memory_order_relaxed);
                    }
                }
            } else if (state->ownership_tracking_valid) {
                // A release the runtime accepted with nothing waited here:
                // the mirror has diverged and nothing can say which image
                // went out, so it stops guessing.
                state->acquired_indices.clear();
                state->waited_count = 0;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                state->pending_end_frame_capture.reset();
                state->ownership_tracking_valid = false;
            }
        }

        if (pending_capture && history) {
            const bool committed =
                commit_capture && SUCCEEDED(history->commit(*pending_capture));
            if (committed) {
                std::scoped_lock lock(state->mutex);
                state->last_released_capture = pending_capture;
                state->last_released_motion_vectors = pending_motion_vectors;
            } else {
                history->discard(*pending_capture);
                std::scoped_lock lock(state->mutex);
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
            }
        }
    } else if (pending_capture && history) {
        history->discard(*pending_capture);
    }
    return result;
}

// SessionState::capture_at_end_frame: queues the history capture of every
// application image released since the last xrEndFrame, now that every
// command list of the frame has reached the application's queue. Called at
// the top of the application's xrEndFrame, under frame_call_mutex, before
// anything reads last_released_capture. The release left that reset, so a
// capture that fails here leaves the frame to pass through as
// missing_capture, exactly as a failed release-time capture does.
//
// The capture goes onto the application's queue behind everything the frame
// submitted, which is the whole point: at the release some of it might not
// have been submitted yet. Only images the mirror still vouches for are
// captured; one it has lost track of since the release is dropped.
void capture_pending_end_frame_images(
    const std::shared_ptr<SessionState>& session) noexcept {
    if (!session || !session->capture_at_end_frame) {
        return;
    }
    try {
        const bool armed = !session->manual_control.stop_requested() &&
            session->menu_enabled;
        for (const auto& state : find_swapchains(session)) {
            std::scoped_lock call_lock(state->call_mutex);
            std::optional<std::uint32_t> index;
            std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
            {
                std::scoped_lock lock(state->mutex);
                index = state->pending_end_frame_capture;
                state->pending_end_frame_capture.reset();
                if (index && state->ownership_tracking_valid &&
                    state->last_released_index == index) {
                    history = state->d3d12_history;
                }
            }
            if (!index) {
                continue;
            }
            if (!history || !armed) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::deferred_capture,
                    E_ABORT,
                    handle_value(state->handle),
                    *index,
                    0);
                continue;
            }
            std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_vectors;
            xrfg::D3D12HistoryCaptureTicket ticket{};
            HRESULT capture_result = S_OK;
            {
                std::scoped_lock gpu_lock(session->gpu_mutex);
                if (session->dlss_motion_vectors) {
                    std::scoped_lock lock(state->mutex);
                    if (*index < state->enumerated_d3d12_images.size()) {
                        const int eye = state->projection_eye.load(std::memory_order_relaxed);
                        motion_vectors = xrfg::resolve_dlss_motion_vectors(
                            state->enumerated_d3d12_images[*index].Get(),
                            session->d3d12_queue.Get(),
                            eye,
                            state->release_publication,
                            eye == 0 || eye == 1
                                ? session->eye_release_publication[1 - eye].load(
                                      std::memory_order_relaxed)
                                : 0);
                    }
                }
                capture_result = history->capture(*index, &ticket);
                if (SUCCEEDED(capture_result)) {
                    // The image went out at its release: nothing is left
                    // to wait for before the capture becomes history.
                    capture_result = history->commit(ticket);
                    if (FAILED(capture_result)) {
                        history->discard(ticket);
                    }
                } else if (ticket.serial != 0) {
                    history->discard(ticket);
                }
            }
            {
                std::scoped_lock lock(state->mutex);
                if (SUCCEEDED(capture_result)) {
                    state->last_released_capture = ticket;
                    state->last_released_motion_vectors = motion_vectors;
                } else {
                    state->last_released_capture.reset();
                    state->last_released_motion_vectors.reset();
                }
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::deferred_capture,
                capture_result,
                handle_value(state->handle),
                *index,
                SUCCEEDED(capture_result) ? ticket.serial : 0);
        }
    } catch (...) {
    }
}

struct ProjectionLayerCopy {
    std::uint32_t layer_index{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> views;
};

using OwnedCompositionLayer = std::variant<
    XrCompositionLayerQuad,
    XrCompositionLayerCubeKHR,
    XrCompositionLayerCylinderKHR,
    XrCompositionLayerEquirectKHR,
    XrCompositionLayerEquirect2KHR,
    XrCompositionLayerPassthroughFB,
    XrCompositionLayerPassthroughHTC,
    XrCompositionLayerPassthroughANDROID>;

// A private image left acquired so that it is handed to the runtime by
// whoever hands its frame over, together with the join that has to precede
// that release on the application's queue. See prepare_frame_generation.
struct PendingPrivateRelease {
    // Keeps the slot the pointer below addresses alive.
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    PrivateSwapchainState* image{};
    xrfg::D3D12FrameSynthesisTicket ticket{};
    // Single-swapchain rings: the staging the frame was written to, copied
    // into the image acquired at the hand-over. Null on the ring path, where
    // the image was written directly and is only released here.
    ID3D12Resource* staging{};
    const std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* runtime_images{};
};

[[nodiscard]] bool acquire_and_wait_private_image(
    SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept;

struct GeneratedFrameEndInfo {
    bool synthetic{};
    // The first of an application frame's submissions, when that is a
    // synthetic: the one the 3X release delay is judged on.
    bool leads_frame{};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<ProjectionLayerCopy> projections;
    std::vector<OwnedCompositionLayer> composition_layers;
    std::vector<const XrCompositionLayerBaseHeader*> layer_pointers;
    // Synthesizers whose current copy is recorded but not yet submitted.
    // Only the synthetic half of a pair carries these: the copy must reach
    // the queue after this frame has been handed over and before the
    // current frame follows a display period later.
    struct PendingCurrentCopy {
        std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
        // The value this pair's copy signals. Carried rather than looked up
        // at flush time: see D3D12FrameSynthesizer::flush_current_copy.
        std::uint64_t fence_value{};
    };
    std::vector<PendingCurrentCopy> pending_current_copies;
    // Private images this frame names that have not been released to the
    // runtime yet. Run and cleared by the presenter immediately before this
    // frame goes downstream, never again for a repeat of it.
    std::vector<PendingPrivateRelease> pending_releases;
    // Only the synthetic half of a pair in the deeper pipeline: one entry per
    // generated swapchain, each complete once that swapchain's output exists.
    // The presenter polls these and never waits on them.
    struct ReadyFence {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
    };
    std::vector<ReadyFence> synthetic_ready;
    // As on PresenterSubmission, kept here as well so a repeat of this frame
    // can make the same join.
    std::uint64_t app_release_value{};
};

// Whether every output a synthetic names has been written. A fence that
// reports the device lost reads as ready: holding cannot make it signal, and
// the frame should go down and fail where the runtime can see it.
[[nodiscard]] bool synthetic_output_ready(
    const GeneratedFrameEndInfo& frame) noexcept {
    for (const auto& ready : frame.synthetic_ready) {
        if (ready.fence == nullptr || ready.value == 0) {
            continue;
        }
        const std::uint64_t completed = ready.fence->GetCompletedValue();
        if (completed != std::numeric_limits<std::uint64_t>::max() &&
            completed < ready.value) {
            return false;
        }
    }
    return true;
}

// Joins the application's queue to synthesis and releases each image to the
// runtime, in that order: the runtime orders its use of a private image
// against the application's queue, so the join has to be on that queue ahead
// of the release. Clears the list so a frame never releases twice.
// Returns false when the runtime refused a release: the frame about to name
// that image cannot be shown as generated.
bool run_private_releases(
    SessionState* session,
    std::vector<PendingPrivateRelease>& releases) noexcept {
    bool all_released = true;
    if (session != nullptr) {
        for (PendingPrivateRelease& pending : releases) {
            if (!pending.generation || pending.image == nullptr) {
                continue;
            }
            // Single-swapchain rings: the frame's image is taken now, and
            // handed back with the frame's pixels in it. A failed acquire or
            // copy leaves the image's previous content for the runtime to
            // show - the last frame of this output - and records which.
            // 704: a the step (1 acquire, 2 copy), b the HRESULT or 0, c the
            // swapchain.
            if (pending.staging != nullptr) {
                if (!acquire_and_wait_private_image(
                        session, session->dispatch, *pending.image)) {
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_transition,
                        704, 1, 0, handle_value(pending.image->handle));
                    all_released = false;
                    continue;
                }
            }
            if (session->d3d12_synthesis_queue &&
                pending.generation->synthesizer) {
                static_cast<void>(
                    pending.generation->synthesizer->synchronize_consumer_queue(
                        runtime_queue(*session),
                        pending.ticket));
            }
            if (pending.staging != nullptr) {
                ID3D12Resource* destination =
                    pending.runtime_images != nullptr &&
                            pending.image->acquired_index < pending.runtime_images->size()
                        ? (*pending.runtime_images)[pending.image->acquired_index].Get()
                        : nullptr;
                const HRESULT copy_result = pending.generation->copier
                    ? pending.generation->copier->copy(
                          runtime_queue(*session), pending.staging, destination)
                    : E_POINTER;
                if (FAILED(copy_result)) {
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_transition,
                        704, 2,
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(copy_result)),
                        handle_value(pending.image->handle));
                }
            }
            if (!release_private_image(
                    session, session->dispatch, *pending.image)) {
                all_released = false;
            }
        }
    }
    releases.clear();
    return all_released;
}

// For a frame that will not be handed over: a ring image left acquired is
// released; a staging entry has nothing acquired and is dropped.
void discard_private_releases(
    SessionState* session,
    std::vector<PendingPrivateRelease>& releases) noexcept {
    if (session != nullptr) {
        for (PendingPrivateRelease& pending : releases) {
            if (!pending.generation || pending.image == nullptr ||
                pending.staging != nullptr) {
                continue;
            }
            if (session->d3d12_synthesis_queue &&
                pending.generation->synthesizer) {
                static_cast<void>(
                    pending.generation->synthesizer->synchronize_consumer_queue(
                        runtime_queue(*session),
                        pending.ticket));
            }
            static_cast<void>(release_private_image(
                session, session->dispatch, *pending.image));
        }
    }
    releases.clear();
}

// Whatever was deferred for a frame and did not end up in a frame the
// presenter took is released when the frame call returns, on the application
// thread - which is exactly what every frame did before the handoff moved.
// Nothing reaches here on the path that works; this is for the ones that do
// not, so a failed build or a refused enqueue cannot leave an image acquired.
struct PrivateReleaseBatch {
    SessionState* session{};
    std::vector<PendingPrivateRelease> releases;
    PrivateReleaseBatch() = default;
    PrivateReleaseBatch(const PrivateReleaseBatch&) = delete;
    PrivateReleaseBatch& operator=(const PrivateReleaseBatch&) = delete;
    ~PrivateReleaseBatch() { discard_private_releases(session, releases); }
    [[nodiscard]] std::vector<PendingPrivateRelease> take() noexcept {
        std::vector<PendingPrivateRelease> taken;
        taken.swap(releases);
        return taken;
    }
};

[[nodiscard]] std::optional<OwnedCompositionLayer>
copy_composition_layer_for_presenter(
    const XrCompositionLayerBaseHeader* source) {
    if (source == nullptr) {
        return std::nullopt;
    }

#define XRFG_COPY_COMPOSITION_LAYER(structure_type, structure_name)            \
    case structure_type: {                                                    \
        structure_name copy =                                                 \
            *reinterpret_cast<const structure_name*>(source);                 \
        copy.next = nullptr;                                                  \
        return OwnedCompositionLayer{copy};                                   \
    }
    switch (source->type) {
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_QUAD,
            XrCompositionLayerQuad)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_CUBE_KHR,
            XrCompositionLayerCubeKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,
            XrCompositionLayerCylinderKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR,
            XrCompositionLayerEquirectKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,
            XrCompositionLayerEquirect2KHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
            XrCompositionLayerPassthroughFB)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC,
            XrCompositionLayerPassthroughHTC)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_ANDROID,
            XrCompositionLayerPassthroughANDROID)
        default:
            return std::nullopt;
    }
#undef XRFG_COPY_COMPOSITION_LAYER
}

[[nodiscard]] const XrCompositionLayerBaseHeader* composition_layer_header(
    OwnedCompositionLayer& layer) noexcept {
    return std::visit(
        [](auto& value) {
            return reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &value);
        },
        layer);
}

[[nodiscard]] std::shared_ptr<GeneratedFrameEndInfo>
make_presenter_owned_frame(GeneratedFrameEndInfo&& source) {
    if (source.info.next != nullptr || source.projections.empty() ||
        source.info.layerCount != source.layer_pointers.size()) {
        return nullptr;
    }

    auto output = std::make_shared<GeneratedFrameEndInfo>(std::move(source));
    std::vector<bool> projection_indices(output->layer_pointers.size(), false);
    for (ProjectionLayerCopy& projection : output->projections) {
        if (projection.layer_index >= output->layer_pointers.size() ||
            projection.views.empty() ||
            projection_indices[projection.layer_index]) {
            return nullptr;
        }
        projection_indices[projection.layer_index] = true;
        projection.layer.next = nullptr;
        for (XrCompositionLayerProjectionView& view : projection.views) {
            view.next = nullptr;
        }
        projection.layer.views = projection.views.data();
        output->layer_pointers[projection.layer_index] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &projection.layer);
    }

    output->composition_layers.reserve(
        output->layer_pointers.size() - output->projections.size());
    for (std::size_t index = 0; index < output->layer_pointers.size(); ++index) {
        if (projection_indices[index]) {
            continue;
        }
        auto layer = copy_composition_layer_for_presenter(
            output->layer_pointers[index]);
        if (!layer) {
            return nullptr;
        }
        output->composition_layers.push_back(std::move(*layer));
        output->layer_pointers[index] = composition_layer_header(
            output->composition_layers.back());
    }

    output->info.next = nullptr;
    output->info.layers = output->layer_pointers.data();
    return output;
}

// The period the application is handed: the display's, times the frames the
// layer submits for each of its own.
[[nodiscard]] XrDuration virtual_display_period(
    XrDuration period,
    std::uint32_t frames) noexcept {
    if (period <= 0 || frames == 0) {
        return period;
    }
    constexpr XrDuration maximum = std::numeric_limits<XrDuration>::max();
    return period > maximum / frames ? maximum : period * frames;
}

[[nodiscard]] XrTime add_display_duration(
    XrTime time,
    XrDuration duration) noexcept {
    if (duration <= 0) {
        return time;
    }
    constexpr XrTime maximum = std::numeric_limits<XrTime>::max();
    return time > maximum - duration ? maximum : time + duration;
}

// The runtime refused one frame for what it contains, not because the session
// is gone. An application gets this back for that frame alone and carries on;
// Alien: Rogue Incursion submits a loading-screen layer naming a swapchain it
// has not released an image of yet, and Virtual Desktop's runtime says
// XR_ERROR_LAYER_INVALID. Latching it in the presenter failed every later
// frame call of the session and left the headset black, so the presenter
// drops the frame and keeps going instead.
[[nodiscard]] constexpr bool frame_content_refused(XrResult result) noexcept {
    return result == XR_ERROR_LAYER_INVALID ||
           result == XR_ERROR_LAYER_LIMIT_EXCEEDED ||
           result == XR_ERROR_SWAPCHAIN_RECT_INVALID ||
           result == XR_ERROR_POSE_INVALID;
}

void fail_pending_presenter_submissions_locked(
    SessionState& state,
    XrResult failure) noexcept {
    if (XR_FAILED(failure) && XR_SUCCEEDED(state.presenter_failure)) {
        state.presenter_failure = failure;
    }
    for (const auto& request : state.presenter_submissions) {
        request->result = failure;
        request->completed = true;
    }
    state.presenter_submissions.clear();
    state.outstanding_presenter_submissions = 0;
}

// Holds the presenter to one submission per display period.
//
// The layer submits two frames for every application frame and relies on
// xrWaitFrame to space them, one per scanout. That holds only while the
// runtime actually throttles the wait. Measured against MSFS 2024: VDXR
// blocks it for a metronomic 9.95-9.97 ms and the pair goes out 11.11 ms
// apart, exactly as intended; SteamVR with the Pimax driver returns it in
// 1.55 ms, so the presenter free-runs and submits the pair 1.12 ms apart -
// both inside one 11.11 ms window - followed by a 21.1 ms gap.
//
// A compositor holding one submitted frame at a time then never scans out the
// first of each pair: the second replaces it. Half the generated frames are
// discarded before they are ever displayed, which is why the layer's own
// overlay reported a steady 90 while the compositor reported 10% reprojection
// and about 80 FPS, and why neither pipeline depth nor synthesis readiness
// moved the result - the frames were being dropped for their timing, never
// for their contents.
//
// Pacing here rather than trusting the wait costs nothing where the wait
// already paces: the elapsed check passes immediately and the runtime's own
// blocking still sets the cadence. The wait is interruptible, and it happens
// inside the begun frame, so the runtime measures a frame that contains the
// work actually being done rather than an empty window. It must not run
// under presenter_content_mutex - waiting for presenter progress while
// holding that lock has deadlocked this layer twice.
// Whether the schedule's phase is being set from the compositor's own frame
// clock. When it is, this is the only thing allowed to move the schedule: the
// drift servo, the slot corrector and the vsync phase lock all stand down,
// because two controllers on one variable is the failure this layer keeps
// rediscovering.
// Real frames between two whole-scanout jumps of the acquisition step while
// the reading has not come back: one jump is meant to be the whole
// correction, and if the reading is still a scanout out afterwards the step
// walks it off as it always did, rather than skipping a slot on every real
// frame. A reading back near the margin ends the cooldown at once, because
// it says the jump took. Measured on a machine where SteamVR holds the
// presenter's wait two to three times a second, the full eight frames left
// most disturbances inside it: 33 jumped and 52 walked in a minute.
constexpr std::uint32_t kScanoutJumpCooldownFrames = 8;

[[nodiscard]] bool measured_pace_active(
    const std::shared_ptr<SessionState>& state) noexcept {
    return state->steamvr_delivery && state->dispatch &&
        state->dispatch->steamvr_runtime;
}

// Holds presenter_next_submit at a fixed offset from a real vsync, for runtimes
// where nothing else knows where the scanout is.
//
// Returns the correction applied, in nanoseconds, and writes the error it saw
// before correcting. Zero means it did nothing: no connection, no anchor - the
// runtime is explicitly allowed to have no vsync times - the grid was already
// where it should be, or the measured pace owns the schedule.
//
// Off the frame path: a quarter second apart, on the presenter thread, and
// never while the presenter mutex is held.
//
// Caller holds presenter_mutex.
[[nodiscard]] std::int64_t apply_vsync_phase_lock(
    const std::shared_ptr<SessionState>& state,
    std::chrono::nanoseconds period,
    std::int64_t* error_out,
    std::int64_t* cost_out) noexcept {
    if (error_out != nullptr) {
        *error_out = 0;
    }
    if (cost_out != nullptr) {
        *cost_out = 0;
    }
    if (period <= std::chrono::nanoseconds::zero()) {
        return 0;
    }
    const auto anchor = state->steamvr_delivery->vsync_anchor();
    if (!anchor) {
        return 0;
    }
    if (cost_out != nullptr) {
        *cost_out = anchor->cost.count();
    }
    // Where the schedule sits within one scanout interval, measured from the
    // anchor. Modulo, so it does not matter how many periods separate them.
    const auto since = state->presenter_next_submit - anchor->at;
    auto phase = since % period;
    if (phase < std::chrono::nanoseconds::zero()) {
        phase += period;
    }
    if (!state->presenter_vsync_offset_valid) {
        // The schedule is normally given its phase where it is seeded, before
        // anything has had a chance to move it. This covers the case where no
        // vsync anchor was available then: take the first phase seen rather
        // than averaging a window, because the window is time the grid spends
        // with nothing holding it.
        state->presenter_vsync_offset = phase;
        state->presenter_vsync_offset_valid = true;
        return 0;
    }
    // Correcting needs a grid that is already keeping rate - one skipping slots
    // has no stable phase to hold - but the sampling above does not, which is
    // why the gate is here and not at the call site.
    if (state->presenter_on_grid_streak < kPhaseCorrectionGridStreak) {
        return 0;
    }
    // Signed distance to the held offset, taken the short way round so a grid
    // just past the offset is pulled back rather than dragged a whole period
    // forward. Getting this wrong is what section 13 of the low-headroom notes
    // cost, in the phase reference that had the same shape.
    auto error = phase - state->presenter_vsync_offset;
    if (error > period / 2) {
        error -= period;
    } else if (error < -(period / 2)) {
        error += period;
    }
    if (error_out != nullptr) {
        *error_out = error.count();
    }
    // Observe, but do not act, where the measured pace owns the schedule.
    //
    // This lock predates both the measured pace and the acquisition step, and
    // it holds the schedule at `presenter_vsync_offset` - the phase the grid
    // happened to rest at when it was seeded. That was the whole design when it
    // was the only phase mechanism. It stopped being one the moment a
    // controller arrived whose job is to move the grid *off* its seed, towards
    // the compositor's own deadline: the two then pull to targets that have no
    // reason to agree, and the seed is not chosen, so the disagreement is
    // whatever the session happened to start at.
    //
    // Measured on Hogwarts through UEVR: 774 evaluations across 45 seconds of
    // generation, *every one* of them pinned at this function's clamp with a
    // standing error of +3.5 ms that never closed, against an offset fixed at
    // 4.215 ms while the acquisition was pulling towards roughly 8.4. The
    // acquisition corrected on 97-100% of real frames and never went quiet,
    // GetFrameTimeRemaining sat at 3.3 ms against its 2.5 ms target for the
    // whole run, and delivery held 78-87 where single seconds reached 90.
    //
    // So the offset keeps being sampled and recorded - it is the reference the
    // `905` record is read against, and a capture should still show what this
    // would have done - but the schedule is left to the one controller aimed at
    // a target the compositor actually stated.
    if (measured_pace_active(state)) {
        return 0;
    }
    // A fraction of the error, bounded. Drift is a slow accumulation, so the
    // correction that cancels it can be slow too, and a small step cannot move
    // the grid far enough in one go to matter if the anchor was wrong.
    auto correction = error / 8;
    const auto limit = period / 32;
    if (correction > limit) {
        correction = limit;
    } else if (correction < -limit) {
        correction = -limit;
    }
    if (correction == std::chrono::nanoseconds::zero()) {
        return 0;
    }
    state->presenter_next_submit -= correction;
    return correction.count();
}

// How long before its hand-over a frame's first synthetic has to be written
// for the 3X release delay to count it on time. See triple_release_delay.
constexpr auto kTripleReadinessMargin = std::chrono::nanoseconds(4'000'000);

struct TripleReleaseReport {
    std::int64_t delay_us{-1};
    std::int64_t ceiling_us{};
    std::uint32_t late{};
    std::uint32_t unwritten{};
};

// Whether the next submission is a frame's first synthetic, queued and
// written. presenter_mutex held.
[[nodiscard]] bool triple_leader_ready(const SessionState& state) noexcept {
    if (state.presenter_submissions.empty()) {
        return false;
    }
    const auto& front = state.presenter_submissions.front();
    return front->owned_frame && front->owned_frame->leads_frame &&
        synthetic_output_ready(*front->owned_frame);
}

// One observation for the 3X release delay, and the step when a window of
// them closes. presenter_mutex held; the report is for the caller to record
// once it has let go.
void observe_triple_release(
    SessionState& state,
    bool leader_ready,
    TripleReleaseReport* report) noexcept {
    const auto period = std::chrono::nanoseconds(
        static_cast<std::int64_t>(state.presenter_display_period));
    if (period <= std::chrono::nanoseconds::zero()) {
        return;
    }
    if (!leader_ready) {
        ++state.triple_release_window_late;
    }
    constexpr std::uint32_t kWindowFrames = 30;
    constexpr std::uint32_t kHoldWindows = 10;
    constexpr auto kMillisecond = std::chrono::nanoseconds(1'000'000);
    if (++state.triple_release_window_frames < kWindowFrames) {
        return;
    }
    const std::uint32_t late = state.triple_release_window_late;
    const std::uint32_t unwritten = state.triple_release_window_unwritten;
    state.triple_release_window_frames = 0;
    state.triple_release_window_late = 0;
    state.triple_release_window_unwritten = 0;
    auto& delay = state.triple_release_delay;
    auto& ceiling = state.triple_release_ceiling;
    if (ceiling < std::chrono::nanoseconds::zero()) {
        ceiling = period * 2;
    }
    if (late >= 2) {
        // Only a delay can be blamed. Frames that are late with none
        // applied are the title's own - a session's first second always
        // has some - and say nothing about where the ceiling belongs.
        if (delay > std::chrono::nanoseconds::zero()) {
            ceiling = std::max(
                std::chrono::nanoseconds::zero(), delay - 3 * kMillisecond);
            delay = ceiling;
            state.triple_release_ceiling_hold = kHoldWindows;
        }
    } else if (late == 0) {
        if (delay < ceiling) {
            delay = std::min(ceiling, delay + 2 * kMillisecond);
        } else if (state.triple_release_ceiling_hold > 0) {
            --state.triple_release_ceiling_hold;
        } else {
            ceiling = std::min(period * 2, ceiling + 2 * kMillisecond);
            delay = ceiling;
        }
    }
    if (report != nullptr) {
        report->delay_us = delay.count() / 1000;
        report->ceiling_us = ceiling.count() / 1000;
        report->late = late;
        report->unwritten = unwritten;
    }
}

// 801: the 3X release delay, once a window. a the delay in microseconds; b
// how many of the window's thirty first synthetics were late by the test in
// force, with the count handed over unwritten in the bits above 16; c the
// ceiling in microseconds.
void log_triple_release(const TripleReleaseReport& report) noexcept {
    if (report.delay_us < 0) {
        return;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        801,
        static_cast<std::uint64_t>(report.delay_us),
        report.late | (static_cast<std::uint64_t>(report.unwritten) << 16),
        static_cast<std::uint64_t>(report.ceiling_us));
}

void pace_presenter_submission(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        const auto entered = std::chrono::steady_clock::now();
        std::int64_t behind_us = 0;
        std::chrono::nanoseconds remaining{0};
        // -1 pulled earlier off the ceiling, +1 pushed later off the floor.
        int pace_band_correction = 0;
        // A runtime other than SteamVR paces the presenter with its own
        // xrWaitFrame and the grid is not held; what remains is the floor
        // between hand-overs. Virtual Desktop excepted: it holds the grid
        // below. See presenter_last_end_returned_at. Recorded as result=5,
        // after the lock: a the hold in microseconds, b 1 where the floor
        // held, so a capture tells a paced cycle from a spaced one.
        if (!state->dispatch->steamvr_runtime &&
            !state->dispatch->virtual_desktop_runtime) {
            std::chrono::nanoseconds floor_remaining{0};
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (state->presenter_display_period > 0 &&
                    state->presenter_last_end_returned_at !=
                        std::chrono::steady_clock::time_point{}) {
                    const auto period = std::chrono::nanoseconds(
                        static_cast<std::int64_t>(
                            state->presenter_display_period));
                    floor_remaining = std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        state->presenter_last_end_returned_at + period / 2 -
                        entered);
                }
            }
            if (floor_remaining > std::chrono::nanoseconds::zero()) {
                if (state->presenter_pace_timer == nullptr) {
                    state->presenter_pace_timer = CreateWaitableTimerExW(
                        nullptr,
                        nullptr,
                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                        TIMER_ALL_ACCESS);
                    if (state->presenter_pace_timer == nullptr) {
                        state->presenter_pace_timer = CreateWaitableTimerExW(
                            nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                    }
                }
                if (state->presenter_pace_timer != nullptr) {
                    LARGE_INTEGER due{};
                    due.QuadPart = -(floor_remaining.count() / 100);
                    if (SetWaitableTimer(
                            state->presenter_pace_timer,
                            &due, 0, nullptr, nullptr, FALSE)) {
                        static_cast<void>(WaitForSingleObject(
                            state->presenter_pace_timer, INFINITE));
                    }
                }
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::presenter_pace,
                5,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - entered)
                        .count()),
                floor_remaining > std::chrono::nanoseconds::zero() ? 1u : 0u,
                0);
            return;
        }
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (!state->presenter_schedule_valid ||
                state->presenter_display_period <= 0 ||
                state->presenter_stop_requested) {
                return;
            }
            const auto period = std::chrono::nanoseconds(
                static_cast<std::int64_t>(state->presenter_display_period));
            auto ready_at = state->presenter_next_submit;
            behind_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    entered - ready_at)
                    .count();
            if (ready_at > entered) {
                // Never hold longer than one period, whatever the schedule
                // says. Pacing exists to stop submissions bunching up; it must
                // never be able to hold the presenter back instead. The one
                // exception is the cycle after the acquisition step took a
                // whole scanout in one step: that hold is the step, and it is
                // allowed two.
                const auto cap = state->presenter_scanout_jump_pending
                    ? period * 2
                    : period;
                state->presenter_scanout_jump_pending = false;
                ready_at = std::min(ready_at, entered + cap);
                remaining = ready_at - entered;
            }
            {
                // The one restoring force on the grid's phase. Every other
                // path here moves the deadline later - the catch-up adds whole
                // periods, the step-over adds one - and the phase reference
                // below cannot pull it back, because it tracks whatever phase
                // the presenter is already holding and so has no opinion about
                // a grid that is self-consistent and parked at the worst place
                // in the period. Left alone the schedule ratchets to the end
                // of the period and stays there.
                //
                // Measured in Callisto Protocol on SteamVR: V155 held 10.30 ms
                // of an 11.11 ms period, dead flat from the seventh second to
                // the end of the run, with 45.0 in, 90.0 out, an 11.111 ms
                // grid and no skips or bunching at all. Every submission
                // landed as late in its period as it could, the compositor
                // kept the real frame and dropped the synthetic, and the
                // headset showed 45. The builds that worked held 1.7 to 9.6 ms
                // and moved about.
                //
                // `entered` is the moment the runtime's own xrWaitFrame
                // released this presenter, so the hold is measured against the
                // runtime's timeline rather than the layer's own, and the
                // threshold means something absolute. Three quarters leaves
                // the working range untouched and only acts on a schedule that
                // has walked to the end.
                //
                // There is a floor as well as a ceiling, and it is not
                // symmetry for its own sake. This hold *is* the window the
                // runtime measures its client across: the pace runs inside the
                // begun frame precisely so that begin-to-end spans the real
                // work rather than an empty gap (47d8590). Let the hold reach
                // zero and begin and end go back to back again, the runtime is
                // handed a frame that costs a millisecond, and it schedules
                // against that - asking for the frame late, assuming it will
                // be ready, and reprojecting when it is not.
                //
                // That regressed here. The phase correction walks the grid
                // earlier and nothing bounded it from below, so in The
                // Witcher 3 the measured window fell from 8.0 ms at 35 s to
                // 0.06 ms at 50 s, with 97.3% of frames reporting under a
                // millisecond, and recovered only when the catch-up and
                // step-over had pushed the schedule back later. Twenty-five
                // seconds of the runtime scheduling against an empty client,
                // with the layer's own view a flawless 45 in, 90 out, 11.111
                // ms grid, no skips and no bunching throughout.
                //
                // So drive the hold into a band rather than off one edge. A
                // quarter to three quarters of a period brackets the 5.7 to
                // 8.5 ms the healthy stretches of that same run held, and
                // since every other path moves the deadline later the
                // schedule settles near the top of the band, which is also
                // where the measured window is longest.
                //
                // The floor applies when the presenter is already past its
                // deadline too - `remaining` is zero there, and that is the
                // case that empties the window completely.
                //
                // Both ends wait for a run of on-grid submissions, for the
                // same reason the phase reference does: a grid that is
                // skipping slots has no stable phase to correct towards, and
                // correcting one anyway closes a loop. Measured in Atomic
                // Heart on V157, where the application hitched to 16-33/s for
                // four seconds: the starvation skips pushed the grid later
                // through the catch-up, that lifted the hold over the ceiling,
                // the pull then fired on 44-83% of frames and held the grid at
                // 10.55-10.78 ms against 11.111, and running fast tripped the
                // step-over into the next skip. It sustained itself for nine
                // seconds after the application was back at 43-45/s, throwing
                // away five to seven slots a second where two would have done,
                // and stopped only when the hold drifted back under the
                // ceiling on its own.
                //
                // The gate costs nothing in the cases the band exists for.
                // Both were clean grids: Callisto parked at the ceiling for
                // 47 s with no skips at all, and The Witcher 3 walked to the
                // floor with 0.0-0.4 skips a second.
                const auto band_ceiling = period * 3 / 4;
                if (state->presenter_vsync_offset_valid) {
                    // The measured pace owns the schedule wherever there is a
                    // compositor to ask. A valid offset means exactly that: it
                    // is only ever set where an OpenVR anchor was available.
                    //
                    // This band predates every measured mechanism here and
                    // approximates their job by inference - it has no reference
                    // for where the scanout is, so it watches the hold and
                    // shoves the grid a sixteenth of a period when the hold
                    // leaves a band. Run alongside one, the two write one
                    // variable and this one wins: a capture recorded 1334 of
                    // these corrections, 0.694 ms each, about 926 ms of
                    // commanded displacement, against some 196 ms from the
                    // controller it was competing with over the same session.
                    // Roughly five to one.
                    //
                    // Measured, the grid oscillated between 7.1 and 8.6 ms of
                    // the scanout interval on a two to three second rhythm, and
                    // delivery followed it exactly: 54-100% of submitted frames
                    // scanned out at the low phase, 0-12% at the high one. No
                    // phase could be made to hold, because this kept moving it.
                    //
                    // So it yields where the schedule is placed from a reading,
                    // and keeps its old behaviour where it is not - no anchor,
                    // or a runtime that reports no vsync times.
                } else if (state->presenter_on_grid_streak <
                    kPhaseCorrectionGridStreak) {
                    // Rate is wrong; leave the schedule alone.
                } else if (remaining > band_ceiling) {
                    state->presenter_next_submit -= period / 16;
                    pace_band_correction = -1;
                } else if (remaining < period / 4) {
                    state->presenter_next_submit += period / 16;
                    pace_band_correction = 1;
                }
            }
        }
        // Pace against the compositor's own frame clock where it can be asked,
        // rather than against a steady_clock grid.
        //
        // GetFrameTimeRemaining reports how long is left in the frame the
        // compositor is currently assembling, and it falls one for one with
        // wall time - so waiting `remaining - target` puts the submission at a
        // chosen point in that frame exactly, with no phase to choose, inherit
        // or hold, and nothing running on this machine's clock to drift against.
        //
        // The target is a constant and not a per-machine one, because it is
        // expressed in the compositor's clock. Measured on 3961 paired samples:
        // submitting with under 3 ms left put the frame two scanouts ahead of
        // its view, and 2422 of those 2425 frames were scanned out. Submitting
        // with more left put it one scanout ahead, and only half survived - the
        // frame is taken for the frame already being assembled instead of the
        // next one, so it has a single interval of lead instead of two.
        //
        // 1.5 ms sits in the middle of that band with room on both sides for
        // the pair's own spread and for the 0.14 ms the reading moves frame to
        // frame.
        //
        // Nothing to converge on: the reading is exact, so this is arithmetic
        // rather than a controller. If the moment has already passed, submit
        // now and let the next frame land properly.
        //
        // SteamVR only. Every other runtime keeps the grid untouched.
        if (measured_pace_active(state)) {
            // How much of the compositor's frame to leave in hand when the
            // submission lands.
            //
            // Margin against the application's jitter, not against the
            // deadline. The application holds half rate exactly - 22.20 ms in
            // every window measured - but the *spread* of its arrivals tracks
            // the losses: 0.76 to 0.85 ms while 96-100% of frames were scanned
            // out, 1.30 to 2.10 ms in the stretches that dipped to 87-94%. Its
            // GPU cost does not track them at all, 10.77 ms in a dip against
            // 10.75 in the best window.
            //
            // A late arrival delays the submission, which spends margin. At
            // 1.5 ms of target - and about 1.07 ms actually reached, the rest
            // going to work between the sleep and the submission - a 2 ms
            // wobble puts the frame past the compositor's deadline, where it is
            // shown on a vsync other than the one it was predicted for. Which
            // is the whole of what a dip looks like in the records.
            //
            // Fixed for the session, and the value and the reasoning are on
            // presenter_submit_margin itself.
            const auto submit_target_remaining =
                state->presenter_submit_margin;
            if (const auto left =
                    state->steamvr_delivery->frame_time_remaining()) {
                // Read here, but do not move the schedule from here. The
                // acquisition step beside the `905` record owns the phase.
                //
                // This reading is taken *before* the hold and the submission
                // happens a hold later - about 9.9 ms of an 11.11 ms scanout -
                // while `submit_target_remaining` describes where the
                // compositor should be *at the submission*. The two are very
                // nearly a whole scanout apart, so the mismatch does not show
                // up as an obvious ten milliseconds: it wraps, and comes back
                // as a plausible half-millisecond error of the wrong sign.
                //
                // Measured on Hogwarts through UEVR, per submission:
                //
                //   left at this reading   13.08 ms   (past the 11.11 scanout)
                //   left at the submission  2.89 ms
                //   hold                    9.88 ms
                //
                // 13.08 - 2.5 wraps to -0.53 ms, so the step ran against its
                // negative clamp on 68.6% of 6753 submissions, mean -42.2 us,
                // pulling the grid earlier by about 4 ms a second without pause.
                // The acquisition spent its entire budget cancelling exactly
                // that: in the healthy window the two summed to -20.0 and +20.0
                // ms per five seconds and netted 0.00. Delivery then rested
                // wherever the two balanced rather than at the target, held
                // 89-90 while the balance held, and fell to 74 as the
                // application's arrival spread widened and moved it.
                //
                // So the reading and the record stay - the record is how a
                // capture shows what this would have commanded - and the
                // schedule is left to the one controller that reads the
                // compositor at the moment the frame actually goes out.
                //
                // The step is recorded under result=4 rather than 3: same
                // fields, and the different code says it was computed and not
                // applied.
                const auto scanout = std::chrono::nanoseconds(
                    static_cast<std::int64_t>(state->presenter_display_period));
                auto error = *left - submit_target_remaining;
                if (scanout > std::chrono::nanoseconds::zero()) {
                    while (error > scanout / 2) {
                        error -= scanout;
                    }
                    while (error < -(scanout / 2)) {
                        error += scanout;
                    }
                }
                constexpr auto kMeasuredStepCeiling =
                    std::chrono::nanoseconds(50'000);
                const auto step = std::clamp(
                    error / 8, -kMeasuredStepCeiling, kMeasuredStepCeiling);
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_pace,
                    4,
                    static_cast<std::uint64_t>(left->count()),
                    static_cast<std::uint64_t>(step.count() + 1'000'000),
                    static_cast<std::uint64_t>(remaining.count()));
            }
        }
        // Slept without the lock: the application thread enqueues against this
        // mutex, and a paced presenter holding it would stall the very frame
        // it is waiting for.
        const auto hold = [&](std::chrono::nanoseconds duration) {
            if (duration <= std::chrono::nanoseconds::zero()) {
                return;
            }
            if (state->presenter_pace_timer == nullptr) {
                state->presenter_pace_timer = CreateWaitableTimerExW(
                    nullptr,
                    nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_ALL_ACCESS);
                if (state->presenter_pace_timer == nullptr) {
                    // Pre-1803, or the flag was refused. A coarse timer still
                    // beats the condition variable's tick rounding.
                    state->presenter_pace_timer = CreateWaitableTimerExW(
                        nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                }
            }
            if (state->presenter_pace_timer != nullptr) {
                LARGE_INTEGER due{};
                // Negative is relative, in 100 ns units.
                due.QuadPart = -(duration.count() / 100);
                if (SetWaitableTimer(
                        state->presenter_pace_timer,
                        &due,
                        0,
                        nullptr,
                        nullptr,
                        FALSE)) {
                    static_cast<void>(WaitForSingleObject(
                        state->presenter_pace_timer, INFINITE));
                }
            }
        };
        // 3X: the look at the next frame's first synthetic, the margin
        // before its hand-over. The hold is split in two for it and ends
        // where it always did, on a deadline taken before either part.
        bool probe_due = false;
        if (state->frames_per_application_frame > 2) {
            std::scoped_lock lock(state->presenter_mutex);
            probe_due = state->triple_release_probe_due;
            state->triple_release_probe_due = false;
        }
        if (probe_due) {
            const auto deadline = entered + remaining;
            hold(remaining - kTripleReadinessMargin);
            TripleReleaseReport report{};
            {
                std::scoped_lock lock(state->presenter_mutex);
                observe_triple_release(
                    *state, triple_leader_ready(*state), &report);
            }
            log_triple_release(report);
            hold(std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - std::chrono::steady_clock::now()));
        } else {
            hold(remaining);
        }
        // result=1 marks a frame where the hold had reached the ceiling and
        // the grid was pulled back off it, result=2 one pushed up off the
        // floor, so a capture shows which edge the schedule is being held away
        // from and whether the band is working or fighting.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pace,
            pace_band_correction < 0 ? 1 : (pace_band_correction > 0 ? 2 : 0),
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - entered)
                    .count()),
            static_cast<std::uint64_t>(behind_us < 0 ? -behind_us : behind_us),
            behind_us > 0 ? 1u : 0u);
    } catch (...) {
    }
}

void continuous_presenter_main(
    const std::shared_ptr<SessionState>& state) noexcept {
    for (;;) {
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (state->presenter_stop_requested) {
                break;
            }
        }


        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        XrResult wait_result = XR_SUCCESS;
        std::optional<XrFrameState> adopted_frame_state;
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (state->presenter_adopted_frame_state_valid) {
                adopted_frame_state = state->presenter_adopted_frame_state;
                state->presenter_adopted_frame_state_valid = false;
            }
        }
        if (adopted_frame_state) {
            // The application's wait the runtime still holds un-begun; see
            // presenter_adopted_frame_state. Asking the runtime for another
            // frame here blocked until this one was begun, which nothing
            // was going to do.
            frame_state = *adopted_frame_state;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::presenter_transition,
                500,
                static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
                frame_state.shouldRender);
        } else {
            const auto wait_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_wait_frame,
                handle_value(state->handle));
            wait_result = with_runtime_entry_for_wait(state, [&] {
                return state->dispatch->wait_frame(
                    state->handle, &wait_info, &frame_state);
            });
            xrfg::bridge_flight_logger().end(
                wait_token,
                xrfg::BridgeFlightOperation::internal_wait_frame,
                wait_result,
                static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
                frame_state.shouldRender);
        }
        if (XR_FAILED(wait_result)) {
            std::scoped_lock lock(state->presenter_mutex);
            fail_pending_presenter_submissions_locked(*state, wait_result);
            state->presenter_condition.notify_all();
            break;
        }

        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_frame_state = frame_state;
            state->presenter_frame_state.next = nullptr;
            state->presenter_frame_state_valid = true;
            // One tick per presenter frame. The application's wait counts these
            // so that it is released once per pair rather than once per frame.
            ++state->presenter_frame_serial;
            // Lock the grid to the display the runtime is actually scanning
            // out on. One frame per period means consecutive waits advance
            // predictedDisplayTime by exactly one period; a repeat says two
            // submissions were aimed at one scanout and the grid is ahead, a
            // skip says a scanout went unfilled and it is behind. Correct by
            // a bounded fraction so a single odd prediction cannot jerk the
            // cadence, and only once the grid exists to be corrected.
            const XrDuration locked_period =
                state->presenter_display_period;
            if (state->presenter_last_predicted_valid &&
                state->presenter_schedule_valid && locked_period > 0) {
                const XrTime previous =
                    state->presenter_last_predicted_display;
                const XrTime current = frame_state.predictedDisplayTime;
                const XrDuration advance = current > previous
                    ? static_cast<XrDuration>(current - previous)
                    : 0;
                // A predicted time that did not move, or moved absurdly,
                // is the runtime not describing a new scanout - a session
                // transition, a frame it does not want rendered, a stall.
                // It is not evidence about this grid's phase, and treating
                // a repeat as proof the grid was early is what let the
                // deadline run away into the future.
                const bool usable_signal =
                    advance > 0 && advance < locked_period * 8;
                const std::int64_t slots = usable_signal
                    ? (advance + locked_period / 2) / locked_period
                    : 1;
                if (slots != 1 && !measured_pace_active(state)) {
                    const auto period_ns =
                        std::chrono::nanoseconds(locked_period);
                    // A repeated slot means the grid is early and must be
                    // pushed later; a skipped one means it is late. Step by a
                    // sixteenth of a period, which still walks out a whole slot
                    // in about a fifth of a second and cannot bunch a pair
                    // inside one scanout window on its own.
                    const auto step = period_ns / 16;
                    if (slots < 1) {
                        state->presenter_next_submit += step;
                        // Never let a correction put the deadline further
                        // out than one period. Every other path moves it
                        // later too, so without a ceiling the schedule can
                        // only walk forwards, and a presenter that keeps
                        // sleeping longer stops draining the queue the
                        // application is admitted against.
                        const auto ceiling =
                            std::chrono::steady_clock::now() + period_ns;
                        if (state->presenter_next_submit > ceiling) {
                            state->presenter_next_submit = ceiling;
                        }
                    } else {
                        state->presenter_next_submit -= step;
                    }
                }
            }
            state->presenter_last_predicted_display =
                frame_state.predictedDisplayTime;
            state->presenter_last_predicted_valid =
                frame_state.predictedDisplayTime > 0;
            // Keep the smallest plausible period seen, so a runtime that
            // inflates the value under load cannot inflate the pace with it.
            constexpr XrDuration kShortestCredibleDisplayPeriod = 2'000'000;
            constexpr XrDuration kLongestCredibleDisplayPeriod = 50'000'000;
            if (frame_state.predictedDisplayPeriod >=
                    kShortestCredibleDisplayPeriod &&
                frame_state.predictedDisplayPeriod <=
                    kLongestCredibleDisplayPeriod &&
                (state->presenter_display_period == 0 ||
                 frame_state.predictedDisplayPeriod <
                     state->presenter_display_period)) {
                state->presenter_display_period =
                    frame_state.predictedDisplayPeriod;
            }
            // And never longer than the headset's own scanout: the runtime
            // reports a throttled period as if it were the display's. The
            // grid paced to 27.8 ms on a 72 Hz headset that way, and the
            // presenter, running at 36, kept the throttle going.
            if (state->steamvr_delivery) {
                if (const auto scanout = state->steamvr_delivery->display_period();
                    scanout && scanout->count() >= kShortestCredibleDisplayPeriod &&
                    scanout->count() <= kLongestCredibleDisplayPeriod &&
                    (state->presenter_display_period == 0 ||
                     scanout->count() < state->presenter_display_period)) {
                    state->presenter_display_period = scanout->count();
                }
            }
            if (state->fps_overlay) {
                state->fps_overlay->set_display_period(
                    state->presenter_display_period);
            }
        }
        state->presenter_condition.notify_all();

        // A successful runtime wait supplies the virtual application timing,
        // but do not begin a frame until there is valid composition to submit.
        // This prevents an unsupported first application layer list from being
        // alternated with empty presenter frames while still allowing the
        // virtualized application to advance and enqueue that list.
        {
            std::unique_lock lock(state->presenter_mutex);
            state->presenter_condition.wait(lock, [&] {
                return state->presenter_stop_requested ||
                       XR_FAILED(state->presenter_failure) ||
                       state->presenter_last_frame != nullptr ||
                       !state->presenter_submissions.empty();
            });
        }

        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        const auto begin_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_begin_frame,
            handle_value(state->handle));
        const XrResult begin_result = with_runtime_entry(state, [&] {
            return state->dispatch->begin_frame(state->handle, &begin_info);
        });
        xrfg::bridge_flight_logger().end(
            begin_token,
            xrfg::BridgeFlightOperation::internal_begin_frame,
            begin_result,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
        if (XR_FAILED(begin_result)) {
            std::scoped_lock lock(state->presenter_mutex);
            fail_pending_presenter_submissions_locked(*state, begin_result);
            state->presenter_condition.notify_all();
            break;
        }

        // The pace runs here, inside the begun frame, rather than before the
        // cycle. A runtime measures an application frame between xrBeginFrame
        // and xrEndFrame; calling them back to back, as this loop did, gives
        // it a frame containing nothing - measured as 0.73 ms of CPU and
        // under a millisecond of GPU, while the real work costs 12 ms of
        // application rendering and 3.59 ms of synthesis outside the window.
        //
        // A scheduler told its client costs a millisecond has every reason to
        // ask for the frame late and to assume it will be ready. Holding the
        // frame open across the pace is what every ordinary application does
        // - begin, render, end - and lets the queue timestamps span the work
        // actually being done.
        pace_presenter_submission(state);

        std::shared_ptr<PresenterSubmission> request;
        std::shared_ptr<GeneratedFrameEndInfo> repeated_frame;
        XrResult end_result = XR_ERROR_RUNTIME_FAILURE;
        std::uint32_t submitted_layer_count = 0;
        // Which half of the pair the presenter submitted, kept at this scope so
        // the flight record below can carry it.
        bool fresh_synthetic = false;
        // The application's display time for the newest real frame this
        // submission is made from, for presenter_content.
        XrTime submitted_content_time = 0;
        // Same, for the vsync lock. Recorded after presenter_mutex is released:
        // the presenter thread takes it every frame and logging under it has
        // deadlocked the layer twice.
        std::int64_t vsync_lock_correction = 0;
        std::int64_t vsync_lock_error_ns = 0;
        std::int64_t vsync_lock_offset_ns = 0;
        std::int64_t vsync_lock_cost_us = 0;
        bool pending_vsync_lock = false;
        // When the downstream xrEndFrame was entered, so the call means below
        // can record how long the runtime held this submission.
        auto downstream_end_started = std::chrono::steady_clock::now();
        {
            // Do not let the application release a newer private output while
            // the runtime is resolving the previous handle's last image.
            std::scoped_lock content_lock(state->presenter_content_mutex);
            // The deeper pipeline's depth, enforced where it lives: on the
            // presentation side. A synthetic is not handed over until its pair
            // has been queued for a whole display period, so it always goes
            // out one grid point later than the earliest one it could have
            // taken, and synthesis always has at least a period to finish.
            // That is the pipeline's whole contract - one period of latency
            // for one period of synthesis - and it holds whether the
            // application arrives early or late in the period.
            //
            // Measured before this existed, depth was only ever a side effect
            // of when the application happened to be released, and on a title
            // the layer was not holding back there was none: on MSFS 2024 a
            // pair's synthesis finished about 18.5 ms after the pair was ready
            // while its synthetic was handed over at 10.6 ms, and 81.6% of
            // them went downstream before their pixels existed.
            //
            // Only a synthetic is held. The real frame behind it follows a
            // grid point later as it always has, and a prime or a borrowed
            // frame has nothing to wait for. Holding shows a repeat for one
            // slot - once, when the phase is first established, because a
            // queue that is a slot deeper stays a slot deeper: the next pair
            // arrives a pair later and finds the previous real frame still in
            // front of it.
            //
            // A period of age is counted from the application's xrEndFrame,
            // which is CPU time. Synthesis cannot start until the GPU has
            // finished the frame that call submitted, and on a title at the
            // edge of the GPU the CPU runs up to a frame ahead of it - so a
            // synthetic can be a period old and still not exist. Handed over
            // then, the compositor repeats the previous frame, and the real
            // frame a slot later is ready, so the synthetic is never shown at
            // all. The same hold therefore also waits for its output to have
            // been written. That costs nothing the compositor was not about to
            // cost anyway - the slot shows a repeat either way - and it keeps
            // the synthetic for the next slot instead of losing it. It deepens
            // the queue by a slot, once, for the same reason the age hold
            // does; the admission bound stops it going further.
            //
            // Bounded, because a fence that never signals must not hold the
            // presenter: past kReadinessHoldPeriods of age the synthetic goes
            // down regardless, as it did before the check existed.
            constexpr std::int64_t kReadinessHoldPeriods = 3;
            const auto pop_now = std::chrono::steady_clock::now();
            std::int64_t held_age_ns = -1;
            std::uint64_t held_sequence = 0;
            std::int64_t held_reason = 0;
            // 3X release delay, recorded after the lock.
            TripleReleaseReport release_report{};
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (!state->presenter_stop_requested &&
                    !state->presenter_submissions.empty()) {
                    const auto& front = state->presenter_submissions.front();
                    const auto age = pop_now - front->queued_at;
                    const auto period = std::chrono::nanoseconds(
                        static_cast<std::int64_t>(
                            state->presenter_display_period));
                    const bool holdable_synthetic = state->deep_pipeline &&
                        period > std::chrono::nanoseconds::zero() &&
                        front->owned_frame && front->owned_frame->synthetic;
                    const bool too_young = holdable_synthetic && age < period;
                    const bool not_written = holdable_synthetic &&
                        !too_young &&
                        age < period * kReadinessHoldPeriods &&
                        !synthetic_output_ready(*front->owned_frame);
                    // A hold shows the previous frame for a slot, so it needs a
                    // previous frame to show. A pipelined application gets
                    // none - its frames may name handles it destroys as soon
                    // as its xrEndFrame returns, so the presenter never keeps
                    // one - and neither does a presenter that has not
                    // submitted yet. Held there, the slot went down with no
                    // layers, and SteamVR shows black for that: measured on
                    // MSFS 2024, 31 of 32 empty submissions in one run were
                    // holds, and each was a black flash in the headset.
                    //
                    // So with nothing to repeat, hand the synthetic over now.
                    // The runtime judges whether it is finished by the
                    // binding queue, which waits for its synthesis, so an
                    // unwritten one is treated as not ready and the
                    // compositor keeps showing the previous image, reprojected,
                    // instead of black. What this gives up is the slot of
                    // depth the hold would have bought, for this frame only.
                    const bool can_hold =
                        state->presenter_last_frame != nullptr;
                    if ((too_young || not_written) && can_hold) {
                        held_age_ns = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(age).count();
                        held_sequence = front->sequence;
                        held_reason = too_young ? 400 : 401;
                        repeated_frame = state->presenter_last_frame;
                    } else {
                        if (too_young || not_written) {
                            // Recorded below as 402: would have held.
                            held_age_ns = std::chrono::duration_cast<
                                std::chrono::nanoseconds>(age).count();
                            held_sequence = front->sequence;
                            held_reason = 402;
                        }
                        request = front;
                        state->presenter_submissions.pop_front();
                        // 3X release delay; see triple_release_delay.
                        if (state->frames_per_application_frame > 2 &&
                            request->owned_frame) {
                            if (request->owned_frame->leads_frame) {
                                if (!synthetic_output_ready(
                                        *request->owned_frame)) {
                                    ++state->triple_release_window_unwritten;
                                }
                            } else if (!request->owned_frame->synthetic) {
                                // A real frame is going out, so the next
                                // hand-over is a first synthetic. SteamVR's
                                // pace looks at it the margin before that;
                                // elsewhere this is the only place to look.
                                if (state->dispatch->steamvr_runtime) {
                                    state->triple_release_probe_due = true;
                                } else {
                                    observe_triple_release(
                                        *state,
                                        triple_leader_ready(*state),
                                        &release_report);
                                }
                            }
                        }
                    }
                } else if (!state->presenter_stop_requested) {
                    repeated_frame = state->presenter_last_frame;
                }
            }
            // Recorded outside presenter_mutex, which the application waits
            // on. result=400: a synthetic held for the pipeline's depth;
            // 401: held because its output had not been written yet; 402: one
            // of those, handed over anyway because there was no frame to
            // repeat. a is its age in microseconds, b the display period in
            // microseconds, c its sequence.
            log_triple_release(release_report);
            if (held_age_ns >= 0) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_transition,
                    held_reason,
                    static_cast<std::uint64_t>(held_age_ns / 1000),
                    static_cast<std::uint64_t>(
                        state->presenter_display_period / 1000),
                    held_sequence);
            }
            // A fresh frame whose images were left acquired: join the
            // application's queue to synthesis and hand them to the runtime
            // now, immediately before the frame that names them goes down.
            // Outside presenter_mutex, because this calls into the runtime.
            // Never for a repeat - its images were released when it first
            // went down, and the slot may have been reacquired since.
            if (request && request->owned_frame) {
                const bool joined =
                    !request->owned_frame->pending_releases.empty() &&
                    state->d3d12_synthesis_queue != nullptr;
                run_private_releases(
                    state.get(), request->owned_frame->pending_releases);
                if (joined) {
                    signal_join_probe(
                        state.get(),
                        913,
                        request->sequence,
                        request->owned_frame->synthetic ? 2u : 1u);
                }
            }

            const XrFrameEndInfo* source = request
                ? request->owned_frame
                    ? &request->owned_frame->info
                    : request->borrowed_frame
                : repeated_frame
                    ? &repeated_frame->info
                    : nullptr;
            XrFrameEndInfo submitted{XR_TYPE_FRAME_END_INFO};
            if (source != nullptr) {
                submitted_content_time = source->displayTime;
                submitted = *source;
                submitted.next = nullptr;
                submitted.displayTime = frame_state.predictedDisplayTime;
            } else {
                submitted.displayTime = frame_state.predictedDisplayTime;
                submitted.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            }
            if (frame_state.shouldRender == XR_FALSE) {
                submitted.layerCount = 0;
                submitted.layers = nullptr;
            }
            submitted_layer_count = submitted.layerCount;
            const auto end_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_end_frame,
                handle_value(state->handle),
                static_cast<std::uint64_t>(submitted.displayTime),
                submitted.layerCount);
            fresh_synthetic = request && request->owned_frame &&
                request->owned_frame->synthetic;
            if (state->fps_overlay && state->manual_control.stop_requested())
                state->fps_overlay->suspend();
            downstream_end_started = std::chrono::steady_clock::now();
            // Where this submission actually lands in the scanout interval,
            // once per frame rather than only when the lock happens to
            // evaluate. Every explanation of why a working phase stops working
            // after ten to thirty seconds has assumed the grid walks away from
            // where it was put; nothing has measured it. If the phase recorded
            // here is constant while delivery decays, the schedule is exactly
            // where it was placed and it is the compositor's acceptance that
            // changed - which rules out the whole class of drift explanations.
            if (state->steamvr_delivery &&
                state->presenter_display_period > 0) {
                if (const auto anchor =
                        state->steamvr_delivery->vsync_anchor()) {
                    const auto scanout = std::chrono::nanoseconds(
                        static_cast<std::int64_t>(
                            state->presenter_display_period));
                    auto landed =
                        (downstream_end_started - anchor->at) % scanout;
                    if (landed < std::chrono::nanoseconds::zero()) {
                        landed += scanout;
                    }
                    // What the compositor says is left in the frame it is
                    // assembling, read at the moment this submission goes out.
                    // Packed above the half-of-the-pair flag, biased by a
                    // millisecond because it can be slightly negative.
                    std::uint64_t remaining = 0;
                    if (const auto left =
                            state->steamvr_delivery->frame_time_remaining()) {
                        const auto microseconds =
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                *left).count() + 1000;
                        if (microseconds > 0 && microseconds < 0xFFFFFF) {
                            remaining = static_cast<std::uint64_t>(microseconds);
                        }
                    }
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_vsync_lock,
                        905,
                        static_cast<std::uint64_t>(landed.count()),
                        static_cast<std::uint64_t>(
                            state->presenter_vsync_offset.count()),
                        (fresh_synthetic ? 2u : 1u) | (remaining << 8));
                    // Only the real frame is measured against the offset. The
                    // two halves of a pair are not interchangeable - the
                    // synthetic's call costs the runtime more, and it is the
                    // real frame's scanout that delivery is judged on - so one
                    // reference phase is held, by the half that matters.
                    if (state->presenter_vsync_offset_valid &&
                        !fresh_synthetic) {
                        auto error = landed - state->presenter_vsync_offset;
                        if (error > scanout / 2) {
                            error -= scanout;
                        } else if (error < -(scanout / 2)) {
                            error += scanout;
                        }
                        state->presenter_landed_error = error;
                    }
                    // Acquire the phase the pace is aiming at, rather than
                    // creeping towards it forever.
                    //
                    // The pace computes the same error every frame from the
                    // same reading, and is then clamped to 50 microseconds a
                    // frame - a bound sized for cancelling the drift between
                    // the nominal period and the real one, which is tens of
                    // microseconds and never ends. Acquiring the phase is a
                    // different job: up to half a scanout, once. At 50 us a
                    // frame that needs 110 frames of uninterrupted correction
                    // and never gets them, because the same budget is
                    // absorbing the drift at the same time. Measured, the step
                    // sat on its clamp for 100% of frames in every second of a
                    // capture while the phase never moved, and the grid kept
                    // whatever phase it happened to be seeded with for the
                    // whole session. Across eighteen captured sessions that
                    // seed was spread evenly across the scanout and delivery
                    // ran from 58 to 86 frames a second with nothing else
                    // different - same build, same title, same machine.
                    //
                    // Measured here rather than in the pace for two reasons.
                    // The reading is taken as the submission goes out instead
                    // of before the hold, so it is the phase that actually
                    // happened; and the half of the pair is known here, which
                    // it is not when the pace runs. Only the real frame drives
                    // this, for the same reason the offset above measures only
                    // the real frame: correcting both halves towards one
                    // target would leave them fighting over it every frame.
                    //
                    // A quarter of the error per pair, so a worst-case half
                    // scanout is consumed in about a quarter of a second, and
                    // anything under the dead band is left to the pace - that
                    // is the drift job, and it is already sized for it.
                    // Every SteamVR session, both graphics APIs.
                    //
                    // This ran D3D11-only for one build, after a D3D12 UEVR
                    // title lost delivery from 90 to 46 over a minute with
                    // mispresents going from 0.1% to 60% (the pair bias of the
                    // day, since retired, wound to its ceiling alongside). The
                    // correction was blamed, because it was firing on 60-76%
                    // of real frames with a standing error of +0.6 to +0.8 ms
                    // instead of acquiring once and falling silent.
                    //
                    // That standing error was the wrapped remnant of a whole
                    // scanout - see the reading's two clusters below. The same
                    // capture reads +11.96 ms of true error where the
                    // controller saw +0.85. It was never converging, because
                    // it was aiming at a target the arithmetic had hidden
                    // from it. With the fold removed it converges and goes
                    // quiet.
                    if (measured_pace_active(state) && !fresh_synthetic &&
                        remaining != 0) {
                        constexpr auto kAcquireDeadBand =
                            std::chrono::nanoseconds(250'000);
                        const auto left_at_submit =
                            std::chrono::microseconds(
                                static_cast<std::int64_t>(remaining) - 1000);
                        auto error = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(left_at_submit) -
                            state->presenter_submit_margin;
                        // Not wrapped into one scanout. The target is absolute,
                        // so folding the reading hides the failure it exists to
                        // catch.
                        //
                        // The reading is bimodal, and the two modes are about
                        // one scanout apart. Measured on a real frame across a
                        // capture where delivery halved mid-session: 972
                        // submissions at 2.31 ms remaining delivered 87.9 a
                        // second, and 2195 at 13.11 ms delivered 60.3. The
                        // difference is whether the frame has one interval of
                        // lead or two - the same cliff the target constant was
                        // chosen from. But 11.15 ms of error, folded into plus
                        // or minus half a scanout, reads as +0.03 ms. The two
                        // states are arithmetically identical to a wrapped
                        // controller, so it reports itself converged while the
                        // headset receives half the frames, and nothing else
                        // in the layer can see the difference either.
                        //
                        // The same shape is in a D3D12 title's capture -
                        // clusters at 2-4 ms and 13-15 ms, a true error of
                        // +11.96 ms reading as +0.85 - so this is in the
                        // arithmetic, not in one application's behaviour.
                        //
                        // Descent cannot overshoot: the step is a quarter of
                        // the error, so the reading approaches the target from
                        // above and never crosses zero into the rollover the
                        // API warns about. The bound is a safety rail for an
                        // absurd reading, not part of the control law.
                        //
                        // A whole scanout of error is taken in one step. It
                        // arrives all at once: after a single disturbed frame
                        // - the runtime holding this thread's wait a couple of
                        // milliseconds, or one hand-over past the vsync - the
                        // reading sits a scanout higher at the same phase
                        // (16.6 ms for 3.0 at 72 Hz) and every frame handed
                        // over there is mispresented until the scanout is
                        // taken back. Holding the phase instead was tried and
                        // measured: 21 to 25 of every 24 frames wrong. Walked
                        // off a quarter at a time, the frames in between are
                        // wrong too, about twenty per disturbance; at 3X on a
                        // 72 Hz display, where 24 real frames a second step
                        // this, that is most of a second, several times a
                        // minute. Taken in one step it costs one repeated
                        // frame. The step is the whole error, so the reading
                        // comes out on the margin, and the usual quarter-steps
                        // take anything left. One jump per cooldown: if the
                        // reading is still out afterwards the walk is the
                        // fallback, not another skipped slot.
                        // 907: a 1 jumped, 2 far reading inside the cooldown,
                        // walked instead; b error, c the reading.
                        const auto acquire_step_ceiling = scanout / 2;
                        const bool far_reading = error > scanout / 2;
                        if (!far_reading) {
                            state->presenter_scanout_jump_cooldown = 0;
                        }
                        if (far_reading && state->presenter_scanout_jump_cooldown == 0) {
                            const auto jump = std::min(error, scanout * 2);
                            state->presenter_scanout_jump_cooldown =
                                kScanoutJumpCooldownFrames;
                            std::scoped_lock lock(state->presenter_mutex);
                            state->presenter_next_submit += jump;
                            state->presenter_scanout_jump_pending = true;
                            xrfg::bridge_flight_logger().event(
                                xrfg::BridgeFlightOperation::
                                    presenter_vsync_lock,
                                907,
                                1,
                                static_cast<std::uint64_t>(error.count()),
                                remaining);
                        } else if (error > kAcquireDeadBand ||
                                   error < -kAcquireDeadBand) {
                            if (far_reading) {
                                xrfg::bridge_flight_logger().event(
                                    xrfg::BridgeFlightOperation::
                                        presenter_vsync_lock,
                                    907,
                                    2,
                                    static_cast<std::uint64_t>(error.count()),
                                    remaining);
                            }
                            if (state->presenter_scanout_jump_cooldown != 0) {
                                --state->presenter_scanout_jump_cooldown;
                            }
                            const auto step = std::clamp(
                                error / 4,
                                -acquire_step_ceiling,
                                acquire_step_ceiling);
                            std::scoped_lock lock(state->presenter_mutex);
                            state->presenter_next_submit += step;
                            xrfg::bridge_flight_logger().event(
                                xrfg::BridgeFlightOperation::
                                    presenter_vsync_lock,
                                906,
                                static_cast<std::uint64_t>(
                                    error.count() + scanout.count()),
                                static_cast<std::uint64_t>(
                                    step.count() + scanout.count()),
                                remaining);
                        } else if (state->presenter_scanout_jump_cooldown != 0) {
                            --state->presenter_scanout_jump_cooldown;
                        }
                    }
                }
            }
            // Where the application's queue stands as this frame is handed
            // over. A D3D12 runtime is given that queue at session creation
            // and judges the frame complete by the work queued on it up to
            // this submission, so this mark clearing is the earliest the
            // compositor can use the frame. 916: b the submission sequence
            // (0 for a repeat), c 2 synthetic, 1 real, 0 repeat.
            // The binding queue must have run past the application's release
            // of every image of its own that this frame names.
            join_runtime_queue_to_application(
                state.get(),
                request ? request->app_release_value
                        : repeated_frame ? repeated_frame->app_release_value
                                         : 0);
            if (state->graphics_binding == SessionGraphicsBinding::d3d12) {
                signal_join_probe(
                    state.get(),
                    916,
                    request ? request->sequence : 0,
                    fresh_synthetic ? 2u : (request ? 1u : 0u));
            }
            log_reprojection_angle(
                *state, submitted, request ? (fresh_synthetic ? 2U : 1U) : 0U);
            end_result = with_runtime_entry(state, [&] {
                return state->fps_overlay
                    // No request is the presenter repeating what it already
                    // holds, to keep the cadence when the application produced
                    // nothing. It is a submission and not a frame, so the
                    // overlay must not count it.
                    ? state->fps_overlay->end_frame(
                          &submitted, fresh_synthetic, request != nullptr)
                    : state->dispatch->end_frame(state->handle, &submitted);
            });
            state->presenter_last_end_returned_at =
                std::chrono::steady_clock::now();
            // The synthetic has reached the runtime, so its current copy can
            // go to the queue now rather than ahead of it. It has a display
            // period before the current frame that reads it is submitted.
            if (request && request->owned_frame) {
                for (const auto& pending :
                     request->owned_frame->pending_current_copies) {
                    if (pending.synthesizer) {
                        static_cast<void>(
                            pending.synthesizer->flush_current_copy(
                                state->d3d12_synthesis_queue
                                    ? runtime_queue(*state)
                                    : nullptr,
                                pending.fence_value));
                    }
                }
            }
            // Where in the period this submission landed, measured against the
            // scanout the runtime aimed it at. The epochs differ, so only
            // comparisons between these values mean anything - which is what
            // the phase correction below uses them for.
            const std::int64_t submitted_lead =
                static_cast<std::int64_t>(frame_state.predictedDisplayTime) -
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            {
                // Advance the schedule by exactly one period so this loop's
                // own cost does not compound into the cadence, and resync
                // rather than chase if a stall has put the schedule in the
                // past - catching up would submit a burst, which is the very
                // thing the pace exists to prevent.
                std::scoped_lock lock(state->presenter_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto period = std::chrono::nanoseconds(
                    static_cast<std::int64_t>(state->presenter_display_period));
                if (!state->presenter_schedule_valid ||
                    state->presenter_display_period <= 0) {
                    state->presenter_next_submit = now + period;
                    state->presenter_schedule_valid =
                        state->presenter_display_period > 0;
                    // Give the grid its phase here, at the instant the schedule
                    // is seeded, rather than sampling for it over the following
                    // seconds.
                    //
                    // Nothing is chosen: this is still whatever phase the
                    // schedule rests at, only taken before anything has moved
                    // it. What the sampling window it replaces cost was the
                    // three seconds it took - the correction that holds the
                    // phase is gated on the offset being valid, while the lead
                    // band that moves the schedule is not, so for that whole
                    // window the grid was pushed around with nothing holding
                    // it. Measured against the compositor with a fixed
                    // producer, it drifted within the first second into a state
                    // where every synthetic frame was discarded - 45 delivered
                    // frames a second out of 90 submitted - and stayed there
                    // for the rest of the session, because the window then
                    // adopted the median of its own drift and held the grid
                    // exactly where it had ended up. Taking the phase at the
                    // seed instead raised the same measurement to 77.
                    //
                    // Every later step advances the schedule by whole periods,
                    // so the phase set here is the phase for the session.
                    //
                    // Before placing it deliberately, note what constrains the
                    // choice. A fixed quarter period collided with the pair
                    // bias of the day (retired V377), whose ceiling was also a
                    // quarter period: at full bias the real frame sat exactly
                    // on a vsync boundary, and
                    // which compositor frame it belongs to then follows the
                    // prediction depth - synthetic 28.8% presented against real
                    // 86.9% at depth 0, and 92.3% against 36.8% at depth 1,
                    // swapping between them. Half a period avoided that and
                    // took one title to 86-90 delivered frames a second while
                    // costing another a third of its delivery. So a phase
                    // cannot be picked as a constant; it has to be computed
                    // against the pair's own spacing and the compositor's
                    // deadline.
                    if (state->steamvr_delivery &&
                        period > std::chrono::nanoseconds::zero()) {
                        if (const auto anchor =
                                state->steamvr_delivery->vsync_anchor()) {
                            auto seed_phase =
                                (state->presenter_next_submit - anchor->at) %
                                period;
                            if (seed_phase < std::chrono::nanoseconds::zero()) {
                                seed_phase += period;
                            }
                            state->presenter_vsync_offset = seed_phase;
                            state->presenter_vsync_offset_valid = true;

                        }
                    }
                } else {
                    // The two frames of a pair do not cost the runtime the
                    // same. Measured in Atomic Heart across 1479 pairs, with
                    // the submission record carrying which half of the pair
                    // it was: the synthetic's xrEndFrame took 2.12 ms against
                    // the current's 0.68, so an even deadline spacing produced
                    // an uneven arrival spacing (9.67 / 12.55 ms), and two
                    // arrivals 9.67 ms apart can share one scanout. (V163
                    // asserted the opposite from a capture anchored on the
                    // enqueue, which labels the previous pair's current as
                    // this pair's synthetic.)
                    //
                    // V172-V185 and V196-V376 answered that with an adaptive
                    // pair bias: the schedule advanced by period - bias after
                    // the synthetic and period + bias after the current, the
                    // bias climbing whenever the synthetic's call cost more
                    // than the real frame's. Both halves sum to two periods,
                    // so any bias puts one gap under a period, and the
                    // controller judged itself on the call cost - a reading
                    // upstream of xrEndFrame - never on what the compositor
                    // scanned out. On Cyberpunk 2077 (R.E.A.L. VR, SteamVR
                    // 90 Hz) the synthetic's call sat at 1.7 ms for 80 s
                    // without ever shrinking, the bias held its 2.78 ms
                    // ceiling, the pair went out 13.3 / 9.4 ms and the headset
                    // received 77 frames a second against 90.0 with the bias
                    // at zero; with the climb off the block was still there
                    // and cost nothing. Skyrim VR through OpenComposite, the
                    // one session where zeroing the bias had once made the
                    // real frame block the runtime's one-second timeout, ran
                    // at 90.0 on the same build with a worst call of 11.7 ms:
                    // that case belonged to the shallow pipeline. So the pair
                    // advances evenly, and the pace owns where each half lands.
                    //
                    // What the pair cost and how far apart it landed. Both are
                    // recorded per submission and smoothed; nothing schedules
                    // on them.
                    if (request) {
                        const auto call = now - downstream_end_started;
                        auto& mean = fresh_synthetic
                            ? state->presenter_synthetic_call_mean
                            : state->presenter_real_call_mean;
                        mean = mean.count() == 0
                            ? std::chrono::duration_cast<
                                  std::chrono::nanoseconds>(call)
                            : mean + (std::chrono::duration_cast<
                                          std::chrono::nanoseconds>(call) -
                                      mean) / 16;
                        // The arrival gap, measured where it matters: between
                        // the two hand-overs, so it already carries whatever
                        // the runtime spent inside the synthetic's call.
                        if (fresh_synthetic) {
                            state->presenter_synthetic_returned_at = now;
                            if (state->presenter_real_returned_at !=
                                std::chrono::steady_clock::time_point{}) {
                                const auto lead = std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(
                                    now - state->presenter_real_returned_at);
                                if (lead > std::chrono::nanoseconds::zero() &&
                                    lead < period * 4) {
                                    auto& lead_mean =
                                        state->presenter_pair_lead_gap_mean;
                                    lead_mean = lead_mean.count() == 0
                                        ? lead
                                        : lead_mean + (lead - lead_mean) / 16;
                                }
                                state->presenter_real_returned_at = {};
                            }
                        } else if (state->presenter_synthetic_returned_at !=
                                   std::chrono::steady_clock::time_point{}) {
                            const auto gap = std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                now - state->presenter_synthetic_returned_at);
                            if (gap > std::chrono::nanoseconds::zero() &&
                                gap < period * 4) {
                                state->presenter_pair_gap_mean =
                                    state->presenter_pair_gap_mean.count() == 0
                                        ? gap
                                        : state->presenter_pair_gap_mean +
                                              (gap -
                                               state->presenter_pair_gap_mean) /
                                                  16;
                            }
                            state->presenter_synthetic_returned_at = {};
                            state->presenter_real_returned_at = now;
                        } else {
                            state->presenter_real_returned_at = now;
                        }
                    }
                    // A long synthetic xrEndFrame is the runtime holding the
                    // call while the pixels finish; read it as load, not as a
                    // fault to correct. V172-V185 corrected it by spacing the
                    // pair unevenly and cost about a third of the frames that
                    // reached the headset while every instrument here read
                    // healthy; V196-V376 did it adaptively and cost the same
                    // third on Cyberpunk 2077. Every instrument here sits
                    // upstream of xrEndFrame and none can see what the
                    // compositor scanned out. Do not close a loop on these
                    // three again without measuring delivery alongside;
                    // steamvr_delivery records it from the compositor on
                    // SteamVR, and xrfg_steamvr_delivery_probe reads it live.
                    constexpr std::uint32_t kCallReportFrames = 15;
                    if (++state->presenter_call_report_tick >=
                        kCallReportFrames) {
                        state->presenter_call_report_tick = 0;
                        xrfg::bridge_flight_logger().event(
                            xrfg::BridgeFlightOperation::presenter_transition,
                            301,
                            static_cast<std::uint64_t>(
                                state->presenter_synthetic_call_mean.count()),
                            static_cast<std::uint64_t>(
                                state->presenter_real_call_mean.count()),
                            static_cast<std::uint64_t>(
                                state->presenter_pair_lead_gap_mean.count()));
                    }
                    // A pair and a repeat advance alike: one period.
                    state->presenter_next_submit += period;
                    // Cancel the drift between this schedule's clock and the
                    // display's.
                    //
                    // The schedule advances by a nominal period on
                    // steady_clock; the scanout runs on the headset's own
                    // oscillator. Measured against the compositor, the two
                    // differ by about twenty parts per million - the submission
                    // walked 0.22 ms up the scanout interval in fifteen seconds
                    // - and delivery is only whole while it sits inside a
                    // window roughly 0.2 ms wide. So a phase that works stops
                    // working in ten to fifteen seconds, which is what made
                    // every attempt to choose one contradict the last.
                    //
                    // Proportional and small on purpose: twenty ppm is 0.22
                    // microseconds a frame, so a thirty-second of the error
                    // holds it with about seven microseconds left over, a
                    // thirtieth of the window. Nothing here needs to move fast,
                    // and a correction that can move fast is one that can walk
                    // the grid somewhere worse.
                    if (state->presenter_vsync_offset_valid &&
                        !measured_pace_active(state)) {
                        constexpr std::int64_t kDriftGain = 32;
                        constexpr auto kDriftStepCeiling =
                            std::chrono::nanoseconds(10'000);
                        auto step = state->presenter_landed_error / kDriftGain;
                        if (step > kDriftStepCeiling) {
                            step = kDriftStepCeiling;
                        } else if (step < -kDriftStepCeiling) {
                            step = -kDriftStepCeiling;
                        }
                        state->presenter_next_submit -= step;
                    }
                    // Hold the grid against the display's own clock.
                    //
                    // Everything else in this function infers where the scanout
                    // is from how the runtime behaved. It has to, because the
                    // schedule is a steady_clock grid advancing by a nominal
                    // period against a display whose true period is not exactly
                    // that - so it drifts, continuously, and every correction
                    // here is chasing that drift after the fact.
                    //
                    // GetTimeSinceLastVsync says where the scanout actually is.
                    // The offset to hold is not chosen: the first anchor records
                    // wherever the servo had already settled, and from then on
                    // this only removes the drift away from it. That cannot put
                    // the grid anywhere the existing machinery would not have,
                    // which is the point - a wrong offset is the failure the
                    // comment on presenter_next_submit describes, where every
                    // submission misses and is latched there for the session.
                    //
                    // Bounded and gradual for the same reason, and it runs only
                    // once the rate is right: a grid that is skipping slots has
                    // no stable phase to hold.
                    //
                    // Sampled after the synthetic only, never after the real
                    // frame. The deadlines are evenly spaced but the two halves
                    // do not arrive evenly - the runtime's own call costs differ
                    // between them, and that difference falls between the two
                    // submissions. Measured on Hogwarts Legacy through UEVR:
                    // 9.5 ms from synthetic to real and 13.1 ms back, steady.
                    //
                    // Sampling both halves therefore mixes two populations
                    // about 1.8 ms apart, and taking every fourth submission
                    // aliases against a two-cycle alternation, so which
                    // population is read depends on where the count happens to
                    // land. The lock reads that as drift and corrects against
                    // it: mean error 2.77 ms with swings across the full
                    // +/- half period, against 0.018 ms on a title whose halves
                    // arrive nearly together. It was not holding a phase, it
                    // was chasing an alternation.
                    //
                    // One half is enough. The grid advances by a whole period
                    // between consecutive synthetics, so their phase is the
                    // schedule's phase, with nothing to alias against.
                    // The on-grid gate is applied to the correction inside,
                    // not to sampling. Observing the phase moves nothing, and
                    // gating it means the offset can never be learned on a
                    // title whose grid is rarely on-grid for long: measured on
                    // Hogwarts through UEVR, 54 samples in 32 seconds against
                    // the 64 needed, so the lock never settled at all.
                    if (state->steamvr_delivery && fresh_synthetic) {
                        constexpr std::uint32_t kVsyncLockFrames = 2;
                        if (++state->presenter_vsync_tick >= kVsyncLockFrames) {
                            state->presenter_vsync_tick = 0;
                            vsync_lock_correction = apply_vsync_phase_lock(
                                state,
                                period,
                                &vsync_lock_error_ns,
                                &vsync_lock_cost_us);
                        }
                    }
                    if (vsync_lock_correction != 0 ||
                        vsync_lock_error_ns != 0 || vsync_lock_cost_us != 0) {
                        pending_vsync_lock = true;
                        vsync_lock_offset_ns =
                            state->presenter_vsync_offset.count();
                    }
                    // Pull the grid back towards the best phase this
                    // presenter has managed. Everything else here corrects
                    // the grid's *rate* - a repeated scanout or a skipped
                    // one - and so only runs when the rate is wrong. Once one
                    // submission lands per scanout the rate is right at every
                    // phase, including phases that put the submission on top
                    // of the runtime's deadline, so those corrections switch
                    // off and whatever offset the last disturbance left is
                    // frozen. Captured: a hard scene walked the offset 7.7 ms
                    // later in two steps and it never came back, while the
                    // layer's own view stayed a flawless 45 in, 90 out, 100%
                    // on grid - the grid was self-consistent and simply in
                    // the wrong place.
                    //
                    // predictedDisplayTime is the runtime's own scanout time,
                    // so the interval from it back to the submission says
                    // where in the period this presenter sits. Only *where in
                    // the period* though - the lead itself is not comparable
                    // across a skipped slot. The runtime advances
                    // predictedDisplayTime by exactly one period per wait
                    // whether or not the presenter filled the slot, so missing
                    // one costs a whole period of lead while changing nothing
                    // about the phase. Measured on MSFS 2024 at a flat 45 in,
                    // 90 out with nothing missing: the controller read every
                    // skip as an 11 ms deficit, pulled the grid earlier by
                    // period/16 each frame chasing a period it cannot recover,
                    // and the walk tripped the step-over into the next skip.
                    // A 5 Hz limit cycle - submissions 10.645 ms apart against
                    // an 11.111 ms period across 3117 samples, ~4 slots a
                    // second discarded, and the pace sleep sawtoothing from
                    // 11 ms down to 2 and back, which is what the application
                    // sees as ratcheting CPU frame time.
                    //
                    // So reduce both sides modulo the period before comparing.
                    // A skipped slot then reads as no phase error at all,
                    // which is the truth.
                    //
                    // Reducing is not enough on its own, and the first attempt
                    // at it (V154) made a worse failure than the one it fixed.
                    // It took the difference the short way round the period,
                    // into (-period/2, period/2], so a loss of more than half
                    // a period came back with the wrong sign. Measured in
                    // Callisto Protocol: SteamVR's own xrEndFrame ran 3.8 to
                    // 8.0 ms on alternating frames for 80 ms, the presenter
                    // overran and recovered onto a grid point through one
                    // 19.4 ms gap, and the margin to the runtime's scanout
                    // dropped 8.3 ms in one step. The controller read that as
                    // being 2.8 ms *early*, corrected nothing, and the grid
                    // stayed 8.3 ms closer to the deadline for the remaining
                    // 26 seconds - a flawless 45 in, 90 out, 99.92% on grid,
                    // all of it landing too late to be shown.
                    //
                    // The two readings describe the same timeline and the lead
                    // cannot separate them. So do not try: take the deficit
                    // into [0, period) and always read it as being behind,
                    // because pulling earlier is the direction that gains
                    // margin and the step-over below is what stops it bunching.
                    // Two dead bands keep that from firing on noise - nothing
                    // under a millisecond, and nothing within an eighth of a
                    // period of a whole one, which is the reference sitting
                    // just under a steady phase after a decay step.
                    const std::int64_t period_ns = period.count();
                    const std::int64_t submitted_phase =
                        ((submitted_lead % period_ns) + period_ns) % period_ns;
                    // Distance from b forward to a, in [0, period).
                    const auto phase_deficit =
                        [period_ns](std::int64_t difference) -> std::int64_t {
                        return ((difference % period_ns) + period_ns) %
                            period_ns;
                    };
                    const std::int64_t kAheadBand = period_ns / 8;

                    // Whether the *rate* was right for this submission. Phase
                    // is only meaningful once it is - a grid that is skipping
                    // slots has no stable phase to correct towards - so the
                    // correction waits for a run of these.
                    const auto since_previous =
                        state->presenter_last_submitted_at ==
                            std::chrono::steady_clock::time_point{}
                        ? period
                        : (now - state->presenter_last_submitted_at);
                    const bool landed_on_grid =
                        since_previous > period - period / 10 &&
                        since_previous < period + period / 10;
                    state->presenter_on_grid_streak =
                        landed_on_grid ? state->presenter_on_grid_streak + 1 : 0;
                    state->presenter_last_submitted_at = now;

                    // The reference follows the best phase achieved, and bleeds
                    // down about a period every four seconds so a runtime that
                    // genuinely changes its timing is tracked rather than
                    // chased forever against a stale best.
                    //
                    // Only a submission that landed on the grid may raise the
                    // reference, and only by a small step. Without the grid
                    // test the controller feeds itself: pulling the deadline
                    // earlier lengthens the lead, the longer lead becomes the
                    // new best, and the next frame is pulled earlier again,
                    // creeping forward until the step-over shoves the deadline
                    // a whole period and starts over. Measured on a 90 Hz
                    // Pimax with the application flat at 45/s: the phase
                    // walked 6 ms earlier across a minute, the mean gap stayed
                    // a perfect 11.11 ms, and only 78% of submissions landed in
                    // their slot. Without the step limit the Callisto case
                    // above would latch its own 8.3 ms loss as the new best,
                    // because that loss is also readable as a small gain.
                    //
                    // This runs before the correction so that a submission
                    // which really is ahead sets the mark rather than being
                    // corrected towards a mark it has already passed.
                    const std::int64_t ahead_of_reference = phase_deficit(
                        submitted_phase - state->presenter_lead_reference);
                    if (!state->presenter_lead_valid ||
                        (landed_on_grid && ahead_of_reference > 0 &&
                         ahead_of_reference <= kAheadBand)) {
                        state->presenter_lead_reference = submitted_phase;
                        state->presenter_lead_valid = true;
                    } else {
                        constexpr std::int64_t kLeadReferenceDecay = 31'000;
                        state->presenter_lead_reference = phase_deficit(
                            state->presenter_lead_reference -
                            kLeadReferenceDecay);
                    }

                    // Eight consecutive on-grid submissions is a quarter of a
                    // second at 90 Hz. It is what separates the two captures
                    // above: Callisto ran clean for 26 s carrying its 8.3 ms
                    // loss, so the correction gets to run, while the MSFS
                    // capture was a deliberately hard scene skipping slots
                    // about twelve times a second, where it stays switched off
                    // and cannot ratchet.
                    // Stand down once the vsync lock has a settled offset.
                    // Both correct the grid's phase on the same gate, this one
                    // by up to period/16 against the lock's period/32, and they
                    // pull to different targets - this one to the best phase
                    // inferred from the runtime's own behaviour, the lock to a
                    // measured scanout. Two controllers on one variable is not
                    // a tuning problem: measured, the lock sat saturated at its
                    // clamp for sixty-eight seconds while this dragged the grid
                    // back every fourth sample, a perfect sawtooth that never
                    // converged. The lock knows the phase; this infers it.
                    if (state->presenter_on_grid_streak >=
                            kPhaseCorrectionGridStreak &&
                        !state->presenter_vsync_offset_valid) {
                        const std::int64_t deficit = phase_deficit(
                            state->presenter_lead_reference - submitted_phase);
                        constexpr std::int64_t kLeadTolerance = 1'000'000;
                        if (deficit > kLeadTolerance &&
                            deficit < period_ns - kAheadBand) {
                            const auto correction =
                                std::min<std::chrono::nanoseconds>(
                                    period / 16,
                                    std::chrono::nanoseconds(deficit));
                            state->presenter_next_submit -= correction;
                        }
                    }
                    // fresh full period for being late. Resetting to now+period
                    // instead made every cycle cost a period plus whatever the
                    // loop took, which is how a 11.11 ms pace produced 15.5 ms
                    // submissions and held the runtime to 64/s.
                    if (state->presenter_next_submit <= now) {
                        const auto behind = now - state->presenter_next_submit;
                        state->presenter_next_submit +=
                            (behind / period + 1) * period;
                    }
                    // Then step off any deadline that falls too soon after the
                    // frame just handed over. The schedule is an absolute grid
                    // and knows nothing about how long this submission took;
                    // when the runtime's own xrEndFrame ran long - 3.7 ms
                    // typical against 17 ms worst on SteamVR - the next grid
                    // point can be a couple of milliseconds away, so the pair
                    // lands inside one scanout window and the compositor keeps
                    // only the later one. That is the bunching the pace exists
                    // to prevent, reappearing after an overrun instead of at
                    // free-run: measured as a repeating on-grid, long, short
                    // cadence with 28.8% of gaps under 9 ms against an 11.11 ms
                    // period.
                    //
                    // The threshold has to clear the bunching without eating
                    // slots the presenter could still have filled. Stepping
                    // over costs a whole scanout - the slot shows a repeat -
                    // so the band is not free, and at half a period it fires
                    // on roughly half the deadlines left behind an overrun,
                    // which is exactly the case where content is already
                    // scarce. A quarter still clears the 3.7 ms typical
                    // xrEndFrame that produces the bunching, and keeps the
                    // deadlines between a quarter and a half of a period out
                    // - about 2.8 ms of every 11.11 - that half a period
                    // discarded.
                    //
                    // Steady state is unaffected either way: the deadline
                    // there already falls about 7 ms after the handover, so
                    // neither threshold fires. Chaining every deadline off
                    // the handover instead would add the loop's own cost to
                    // each cycle, which is what held an earlier build to
                    // 64/s.
                    //
                    // If this is too narrow it will show as submission gaps
                    // bunching under about 9 ms against the 11.11 ms period,
                    // which is the compositor discarding one of a pair.
                    const auto earliest = now + period / 4;
                    while (state->presenter_next_submit < earliest) {
                        state->presenter_next_submit += period;
                    }
                }
            }
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::internal_end_frame,
                end_result,
                static_cast<std::uint64_t>(submitted.displayTime),
                submitted.layerCount,
                frame_state.shouldRender);
        }
        // c carries which half of the pair this was: 2 synthetic, 1 current,
        // 0 a repeat. Without it the two are indistinguishable in a capture -
        // anchoring on the enqueue does not work, because the submission that
        // follows it is the previous pair's current, not this pair's
        // synthetic. That mislabelling is what made the V162 margin
        // comparison name the wrong frame.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_submission,
            end_result,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            request ? (fresh_synthetic ? 2u : 1u) : 0u);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_content,
            request ? (fresh_synthetic ? 2 : 1) : 0,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(submitted_content_time),
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
        if (XR_SUCCEEDED(end_result) && state->presenter_display_period > 0) {
            state->runtime_slowed.store(
                frame_state.predictedDisplayPeriod >
                    state->presenter_display_period +
                        state->presenter_display_period / 2,
                std::memory_order_relaxed);
        }
        // A generated pair's real frame: how late it went down against the
        // time the application was promised.
        if (request && request->owned_frame && !fresh_synthetic &&
            submitted_content_time != 0 && XR_SUCCEEDED(end_result) &&
            frame_state.shouldRender != XR_FALSE) {
            observe_promise_lateness(
                *state,
                frame_state.predictedDisplayTime - submitted_content_time,
                static_cast<XrDuration>(state->presenter_display_period),
                frame_state.predictedDisplayPeriod);
        }
        if (pending_vsync_lock) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::presenter_vsync_lock,
                vsync_lock_error_ns,
                static_cast<std::uint64_t>(
                    vsync_lock_correction < 0 ? -vsync_lock_correction
                                              : vsync_lock_correction),
                static_cast<std::uint64_t>(vsync_lock_offset_ns),
                static_cast<std::uint64_t>(vsync_lock_cost_us));
        }
        // Whether the compositor put the frame before this one on the vsync it
        // was predicted for. Submission counts cannot see this: a frame can be
        // accepted, correctly spaced and aimed at a distinct slot and still be
        // shown somewhere else, which is the loss every layer-side instrument
        // reads as healthy. Recorded per submission so a dip can be attributed
        // to individual frames rather than averaged across a second.
        if (state->steamvr_delivery) {
            if (const auto presented =
                    state->steamvr_delivery->last_presentation()) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_frame_presented,
                    presented->mispresented,
                    request ? request->sequence : 0,
                    // The compositor's own account, packed above the frame
                    // index: this frame's flags in bits 32-47 and the OR across
                    // the window in 48-63. The index is a 32-bit counter, so
                    // the room is there.
                    static_cast<std::uint64_t>(presented->frame_index) |
                        (static_cast<std::uint64_t>(
                             presented->reprojection_flags & 0xFFFFU)
                         << 32) |
                        (static_cast<std::uint64_t>(
                             presented->reprojection_flags_window & 0xFFFFU)
                         << 48),
                    // Skipped in bits 8-15 and presents in 0-7 as before, with
                    // what the compositor attributes to this frame's rendering
                    // above them: application GPU microseconds in 16-39 and its
                    // own in 40-63.
                    (static_cast<std::uint64_t>(
                         presented->compositor_render_gpu_us & 0xFFFFFFU)
                     << 40) |
                        (static_cast<std::uint64_t>(
                             presented->total_render_gpu_us & 0xFFFFFFU)
                         << 16) |
                        (static_cast<std::uint64_t>(presented->skipped) << 8) |
                        presented->presents);
                // Margin in whole scanouts, which is the unit the compositor
                // decides in. Diagnostic only: nothing reads these yet, and the
                // question they exist to answer is whether the count holds
                // steady while delivery is whole and changes at the edges.
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_vsync_lock,
                    908,
                    presented->ready_vsyncs,
                    presented->vsyncs_to_first_view,
                    presented->presents);
                // The compositor's own timeline for the same frame: where its
                // running start and its render fell, and where our submission
                // and our wait arrived, all against the vsync it measures
                // from. The submit margin stands in for this window; recorded
                // so it can be read instead of assumed.
                // Diagnostic only.
                //
                // Each millisecond offset is packed into 16 bits, in 10 us
                // units biased by 32768 - plus or minus 327 ms, far more than
                // a frame. 910 and 911 carry the frame index in a so the two
                // halves join; 910's b is the frame's vsync reference in
                // microseconds of the compositor's clock, and 912 puts the
                // most recent vsync on this log's clock beside the
                // compositor's frame counter, so the two clocks can be tied
                // together afterwards.
                if (xrfg::bridge_flight_logger().enabled()) {
                    const auto pack = [](float ms) -> std::uint64_t {
                        const double units =
                            static_cast<double>(ms) * 100.0 + 32768.0;
                        return static_cast<std::uint64_t>(
                            std::clamp(units, 0.0, 65535.0));
                    };
                    const auto pack4 = [&](float first, float second,
                                           float third, float fourth) {
                        return pack(first) | (pack(second) << 16) |
                            (pack(third) << 32) | (pack(fourth) << 48);
                    };
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_vsync_lock,
                        910,
                        presented->frame_index,
                        static_cast<std::uint64_t>(
                            std::max(0.0, presented->system_time_seconds) *
                            1e6),
                        pack4(
                            presented->wait_get_poses_called_ms,
                            presented->new_poses_ready_ms,
                            presented->new_frame_ready_ms,
                            presented->compositor_render_start_ms));
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_vsync_lock,
                        911,
                        presented->frame_index,
                        0,
                        pack4(
                            presented->compositor_update_start_ms,
                            presented->compositor_update_end_ms,
                            presented->client_frame_interval_ms,
                            presented->compositor_idle_cpu_ms));
                    if (const auto anchor =
                            state->steamvr_delivery->vsync_anchor()) {
                        LARGE_INTEGER counter{};
                        QueryPerformanceCounter(&counter);
                        const auto steady_now = std::chrono::steady_clock::now();
                        const std::int64_t vsync_log_us =
                            xrfg::bridge_flight_logger().microseconds_for_counter(
                                counter.QuadPart) -
                            std::chrono::duration_cast<
                                std::chrono::microseconds>(
                                steady_now - anchor->at)
                                .count();
                        xrfg::bridge_flight_logger().event(
                            xrfg::BridgeFlightOperation::presenter_vsync_lock,
                            912,
                            static_cast<std::uint64_t>(
                                std::max<std::int64_t>(0, vsync_log_us)),
                            anchor->frame_counter,
                            0);
                    }
                }
                // The margin used to walk itself down from here, half a
                // millisecond at a time, whenever more than 30% of a 450-frame
                // window was presented other than once. It is gone, and the
                // premise is why: it read every loss as "past the running
                // start, give margin back", and the loss that actually occurs
                // here is pairs colliding into one compositor frame because
                // the runtime's own xrEndFrame jittered by more than the
                // margin. Narrowing the margin makes that strictly worse, so
                // the controller steered into the fault while reporting that
                // it was steering out of it, and it only ever moved one way so
                // nothing walked it back.
                //
                // Do not reinstate it without an instrument that separates the
                // two causes. Delivery alone cannot: both look like frames
                // presented more than once.
            }
        }
        // Every compositor frame that settled since the last submission,
        // including the ones it presented zero times - the direct record of a
        // frame the compositor had and never showed, which the records above
        // cannot carry because they describe the newest presented frame only.
        // 915: a frame index; b the frame's vsync reference in microseconds of
        // the compositor's clock; c the same 16-bit packing as 910 for
        // WaitGetPosesCalled | NewFrameReady<<16 | CompositorRenderStart<<32,
        // then presents (8 bits) <<48 and mispresented (8 bits) <<56.
        // Diagnostic only, logging on only.
        if (state->steamvr_delivery && xrfg::bridge_flight_logger().enabled()) {
            const auto pack = [](float ms) -> std::uint64_t {
                const double units = static_cast<double>(ms) * 100.0 + 32768.0;
                return static_cast<std::uint64_t>(
                    std::clamp(units, 0.0, 65535.0));
            };
            for (const auto& frame : state->steamvr_delivery->settled_frames()) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_vsync_lock,
                    915,
                    frame.frame_index,
                    static_cast<std::uint64_t>(
                        std::max(0.0, frame.system_time_seconds) * 1e6),
                    pack(frame.wait_get_poses_called_ms) |
                        (pack(frame.new_frame_ready_ms) << 16) |
                        (pack(frame.compositor_render_start_ms) << 32) |
                        (static_cast<std::uint64_t>(
                             std::min<std::uint32_t>(frame.presents, 255))
                         << 48) |
                        (static_cast<std::uint64_t>(
                             std::min<std::uint32_t>(frame.mispresented, 255))
                         << 56));
            }
        }
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (request) {
                request->result = end_result;
                request->completed = true;
                if (state->outstanding_presenter_submissions != 0) {
                    --state->outstanding_presenter_submissions;
                }
                if (XR_SUCCEEDED(end_result) && request->owned_frame &&
                    !state->pipelined_presenter_mode) {
                    state->presenter_last_frame = request->owned_frame;
                } else if (state->pipelined_presenter_mode) {
                    // A persistent pipelined application may destroy or replace
                    // XrSpace and non-projection swapchain handles as soon as
                    // its xrEndFrame returns. Never repeat retained application
                    // handles beyond that call boundary; let the runtime hold
                    // the last submitted image until the next owned pair.
                    state->presenter_last_frame.reset();
                }
            }
            if (frame_content_refused(end_result)) {
                // Whatever was retained may name the handle that was refused.
                state->presenter_last_frame.reset();
            } else if (XR_FAILED(end_result)) {
                fail_pending_presenter_submissions_locked(*state, end_result);
            }
        }
        state->presenter_condition.notify_all();
        if (XR_FAILED(end_result) && !frame_content_refused(end_result)) {
            break;
        }
    }

    {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_submissions.empty()) {
            const XrResult failure = XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
            fail_pending_presenter_submissions_locked(*state, failure);
        }
    }
    state->presenter_condition.notify_all();
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame,
    bool preserve_virtual_timeline,
    std::optional<XrFrameState> adopted_frame_state) noexcept {
    try {
        std::scoped_lock lock(state->presenter_mutex);
        if (state->presenter_active) {
            return true;
        }
        state->presenter_adopted_frame_state_valid =
            adopted_frame_state.has_value();
        if (adopted_frame_state) {
            state->presenter_adopted_frame_state = *adopted_frame_state;
            state->presenter_adopted_frame_state.next = nullptr;
        }
        state->presenter_submissions.clear();
        state->presenter_last_frame.reset();
        state->presenter_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        if (!preserve_virtual_timeline) {
            state->last_virtual_display_time = 0;
        }
        state->presenter_failure = XR_SUCCESS;
        state->next_presenter_sequence = 1;
        state->outstanding_presenter_submissions = 0;
        if (state->pipelined_presenter_mode && seed_frame) {
            auto request = std::make_shared<PresenterSubmission>();
            request->sequence = state->next_presenter_sequence++;
            request->owned_frame = std::move(seed_frame);
            state->presenter_submissions.push_back(std::move(request));
            state->outstanding_presenter_submissions = 1;
        } else {
            state->presenter_last_frame = std::move(seed_frame);
        }
        state->presenter_frame_state_valid = false;
        // A serial from a previous presenter would either release the first
        // wait of this one straight away or never.
        state->presenter_frame_serial = 0;
        state->application_served_serial = 0;
        // A schedule left over from a previous presenter would stall the first
        // submission of this one.
        // A display time from a previous presenter says nothing about this
        // grid, and its schedule is about to be rebuilt.
        state->presenter_last_predicted_valid = false;
        state->presenter_schedule_valid = false;
        state->presenter_display_period = 0;
        state->presenter_stop_requested = false;
        state->presenter_active = true;
        state->presenter_thread = std::thread(continuous_presenter_main, state);
        return true;
    } catch (...) {
        state->presenter_active = false;
        return false;
    }
}

void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept {
    if (!state) {
        return;
    }
    try {
        {
            std::unique_lock lock(state->presenter_mutex);
            if (!state->presenter_active) {
                return;
            }
            // Let a submission the application already handed over reach the
            // runtime instead of dropping it on the floor. The application no
            // longer waits for this queue to empty, so the last frame of a
            // session is normally still sitting in it. Bounded, because
            // teardown must not depend on the presenter being healthy.
            static_cast<void>(state->presenter_condition.wait_for(
                lock, std::chrono::milliseconds(100), [&] {
                    return state->outstanding_presenter_submissions == 0 ||
                           XR_FAILED(state->presenter_failure);
                }));
            state->presenter_stop_requested = true;
        }
        state->presenter_condition.notify_all();
        if (state->presenter_thread.joinable()) {
            state->presenter_thread.join();
        }
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_active = false;
            state->presenter_frame_state_valid = false;
            state->presenter_last_frame.reset();
        }
        state->presenter_condition.notify_all();
    } catch (...) {
    }
}

[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept {
    std::scoped_lock lock(state->presenter_mutex);
    return state->presenter_active && !state->presenter_stop_requested;
}

// Where the once-per-pair hold keeps the application: after the pair is handed
// over, or at admission, before its synthesis is queued. It decides what
// synthesis waits behind on the GPU, and the two APIs want opposite answers.
//
// Held after the hand-over, the application queues synthesis the moment its
// images are captured, and synthesis follows the game's frame directly on the
// GPU. Native D3D12 wants exactly that: nothing of the game's waits on it.
// Hogwarts Legacy at 8344x3268 held 87+ for 67% and 69% of the time over two
// runs held there, and 44-47% over three held at admission.
//
// A D3D11 session cannot have it. The synthetic reaches the runtime through a
// copy on the game's own immediate context that first waits for synthesis to
// finish, so every D3D11 command the game issues after it waits on the GPU
// behind the whole of synthesis - and queued at capture, that is the game's
// next frame, pushed late into the synthetic's compositor slot. Held at
// admission, synthesis is queued about a pair after the capture, when that
// frame has long finished. Cyberpunk 2077 at 3088x2592: held after the
// hand-over, the compositor skipped 152 frames a minute, almost all on
// synthetic slots; at admission, 3-16.
//
// Admission has to use this hold rather than leave the capacity bound to meter
// the application: the bound counts retirements, so it releases straight after
// a real frame and the queue settles a whole pair deep, where this hold
// follows the phase the presenter's depth rule sets and gives the one period
// the pipeline is meant to add - on Cyberpunk, synthetics out 21 ms after
// their pair rather than 32.
[[nodiscard]] bool presenter_hold_at_admission(
    const SessionState& state) noexcept {
    // Vulkan has the D3D11 coupling too: its publish copy sits on the
    // application's queue and waits there for synthesis.
    return state.deep_pipeline &&
        (state.graphics_binding == SessionGraphicsBinding::d3d11 ||
         state.graphics_binding == SessionGraphicsBinding::vulkan);
}

// A pair's frames reach the presenter at the same point whichever of two
// waits in xrEndFrame takes the application's slack: the capacity wait at
// admission, before synthesis, or the once-per-pair hold after the
// hand-over. Both arrangements are stable, because the hold releases the
// application at a presenter frame and the application's own work takes the
// same time from there either way. But where admission takes the slack, the
// frame was rendered a display period before it had to be and waits out that
// period finished. Galactic Racer at 120 Hz came back that way from a few
// seconds of SteamVR halving the rate and stayed for the rest of the race:
// 6.5 ms at admission and none in the hold, against 0.2 and 6.5 before, and
// each real frame shown 70 ms after the game's wait instead of 62 at the same
// 119.7 frames a second. Over a whole race, 57% of its pairs were like that.
//
// So when eight pairs in a row - an eighth of a second - wait more than half
// a display period at admission, the next hold runs one presenter frame
// longer, once. That starts the application's next frame a period later, so
// the wait moves to after its hand-over. Single late pairs are common and
// runs of up to 14 ended by themselves in Galactic Racer, while the slips
// that stay ran 20 pairs and more. A move made on a run that would have
// ended costs a run of the same length to move back, so the length was
// chosen on three races' runs: eight left the fewest pairs slow (12 about a
// quarter more, 6 a little fewer for half again as many needless moves).
// Judged over fixed windows of 32 instead, a slip cost 32 to 64 pairs: 1.7-
// 6.8% of a race's pairs, from 57% with no move at all. Not while the runtime runs at a multiple of the
// period, which moves both waits. The promise is left to its own
// measurement; moving it with the phase only had the measurement move it
// back. A move that leaves the waits where they were is not repeated until a
// run twice as long has said so again.
void observe_admission_wait(
    SessionState& state, std::chrono::nanoseconds waited) noexcept {
    constexpr std::uint32_t kLateRun = 8;
    constexpr std::uint32_t kLongestBackoff = 64;
    const auto period = std::chrono::nanoseconds(
        static_cast<std::int64_t>(state.presenter_display_period));
    if (state.frames_per_application_frame != 2U ||
        period <= std::chrono::nanoseconds::zero() ||
        state.runtime_slowed.load(std::memory_order_relaxed)) {
        state.admission_late_run = 0;
        return;
    }
    if ((waited + state.test_admission_wait) * 2 <= period) {
        // A move that took, or the phase as it should be.
        if (state.rephase_check_due) {
            state.rephase_check_due = false;
            state.rephase_backoff = 1;
        }
        state.admission_late_run = 0;
        return;
    }
    if (++state.admission_late_run < kLateRun * state.rephase_backoff) {
        return;
    }
    const std::uint32_t run = state.admission_late_run;
    state.admission_late_run = 0;
    if (state.rephase_check_due) {
        // The last move left the waits where they were.
        state.rephase_check_due = false;
        state.rephase_backoff =
            std::min(state.rephase_backoff * 2, kLongestBackoff);
        return;
    }
    state.pair_hold_extra_frames = 1;
    state.rephase_check_due = true;
    // 720: a the late pairs in a row, b the run it took, c how many runs
    // long that was.
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        720,
        run,
        kLateRun * state.rephase_backoff,
        state.rephase_backoff);
}

// Holds the application until the presenter has run `presenter_frames` frames
// since it was last released: a whole pair for a pair, one frame for a prime.
// Deliberately returns nothing: it is called after the frame has already been
// handed over, so a presenter that stops or fails while this waits must not
// turn a submitted frame into an error - it just stops waiting.
//
// This is the gate that paces the application on the presenter path. The
// capacity bounds in xrEndFrame sit beside it and are normally slack, so
// anything meant to move where the application runs has to move this.
//
// It is called after the pair is handed over, except in the deeper pipeline on
// a D3D11 session, where it is called at admission, before the pair's
// synthesis is queued; see presenter_hold_at_admission.
//
// A prime is held too, for one frame, and the reason is what the hold does
// to the surplus below rather than the frame it waits for. A continuity
// reset in a scene the application renders in a few milliseconds - a hangar,
// a menu, a loading screen - used to let three frames reach the GPU in
// 18 ms: the prime was not held at all, the first pair frame passed on the
// surplus the reset's own stall had left, and only the second pair frame
// was paced. With a game frame and two synthesis pairs queued inside those
// 18 ms the GPU ran about 30 ms behind, the fourth frame's history capture
// found its slot still read by the first pair's synthesis (ERROR_BUSY),
// continuity reset again, and the next frame primed again: a cycle of
// exactly 100 ms, 40 real and 20 synthetic images a second, that only a
// heavier scene could break. IL-2 at 90 Hz on Virtual Desktop sat in it for
// a whole hangar; V416's own log has the same cycle for 0.8 s of a flight
// load. Held for one frame the prime absorbs the surplus, so the first
// pair frame waits its two periods and nothing bursts. A prime arriving
// on cadence passes at once: the presenter has a frame behind it already.
void wait_for_presenter_pair(
    const std::shared_ptr<SessionState>& state,
    std::uint64_t presenter_frames) noexcept {
    try {
        // The pair rate does not follow this number: the application enqueues
        // one pair per release and the presenter spends two frames on it -
        // three with 3X - so all it sets is the phase at which the
        // application renders. The depth is not set here either - the
        // presenter enforces it where it pops a submission.
        const std::uint64_t kPresenterFramesPerPair = presenter_frames;
        const auto entered = std::chrono::steady_clock::now();
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            // A presenter with nothing queued and nothing to repeat stops
            // counting until the application gives it something - which,
            // when this is called at admission, is what it is waiting to do.
            // A pipelined application leaves it in that state after every
            // submission, and so does a swapchain teardown.
            const bool presenter_starved =
                state->presenter_submissions.empty() &&
                state->presenter_last_frame == nullptr;
            return state->presenter_frame_serial >=
                       state->application_served_serial +
                           kPresenterFramesPerPair ||
                   presenter_starved ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure) ||
            state->presenter_stop_requested) {
            return;
        }
        // Diagnostic only, and the number this hold is judged by. Assigning
        // the presenter's serial rather than advancing by the pair means an
        // application that arrives late catches up in one step: the surplus
        // below is what that step discards, and while it is non-zero this
        // hold is not throttling anything - the application is gated only by
        // whatever it blocks on next, which is the frame-start synthesis
        // fence, and that one meters nothing.
        const std::uint64_t served = state->application_served_serial;
        const std::uint64_t reached = state->presenter_frame_serial;
        const std::uint64_t surplus =
            reached > served + kPresenterFramesPerPair
            ? reached - served - kPresenterFramesPerPair
            : 0;
        state->application_served_serial = reached;
        // 3X only: see triple_release_delay. A pair is released as soon as
        // the presenter has run it. A delay that steered the application's
        // completion away from the presenter's cycle boundary used to sit
        // here on every runtime but SteamVR; it only ever stepped later, and
        // whatever it reached came out of the application's two periods. At
        // 90 Hz, 10.7 ms of it on a 12.2 ms frame put half the frames past
        // their slot and the application at 39 a second, against 45 with
        // 5.9 ms on the same frame.
        const std::chrono::nanoseconds release_offset =
            presenter_frames > 2
                ? state->triple_release_delay
                : std::chrono::nanoseconds::zero();
        // Never record under presenter_mutex: the presenter thread takes it
        // every frame, and this path has deadlocked the layer twice.
        lock.unlock();
        if (release_offset > std::chrono::nanoseconds::zero()) {
            // The same high-resolution timer the presenter's pace uses, for
            // the same reason: a condition variable would round the delay up
            // to the 15.6 ms system tick.
            if (state->application_release_timer == nullptr) {
                state->application_release_timer = CreateWaitableTimerExW(
                    nullptr,
                    nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_ALL_ACCESS);
                if (state->application_release_timer == nullptr) {
                    state->application_release_timer = CreateWaitableTimerExW(
                        nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                }
            }
            if (state->application_release_timer != nullptr) {
                LARGE_INTEGER due{};
                due.QuadPart = -(release_offset.count() / 100);
                if (SetWaitableTimer(
                        state->application_release_timer,
                        &due, 0, nullptr, nullptr, FALSE)) {
                    static_cast<void>(WaitForSingleObject(
                        state->application_release_timer, INFINITE));
                }
            }
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pair_release,
            static_cast<std::int64_t>(surplus),
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - entered)
                    .count()),
            reached,
            served);
    } catch (...) {
    }
}

[[nodiscard]] XrResult wait_for_presenter_capacity(
    const std::shared_ptr<SessionState>& state,
    std::size_t maximum_outstanding) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->outstanding_presenter_submissions <=
                       maximum_outstanding ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        return state->presenter_stop_requested
            ? XR_ERROR_SESSION_NOT_RUNNING
            : XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] XrResult wait_for_presenter_idle(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->outstanding_presenter_submissions == 0 ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        return state->presenter_stop_requested
            ? XR_ERROR_SESSION_NOT_RUNNING
            : XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

PresenterResourceLifetimeGuard::PresenterResourceLifetimeGuard(
    const std::shared_ptr<SessionState>& state)
    : frame_lock(state->frame_call_mutex) {
    if (continuous_presenter_active(state)) {
        // The content lock must be acquired AFTER the drain: the presenter
        // needs it to complete the very submissions we are waiting for.
        static_cast<void>(wait_for_presenter_idle(state));
        content_lock = std::unique_lock<std::mutex>(state->presenter_content_mutex);
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
    }
}

[[nodiscard]] std::shared_ptr<PresenterSubmission>
enqueue_presenter_submission(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame,
    const XrFrameEndInfo* borrowed_frame) {
    auto request = std::make_shared<PresenterSubmission>();
    {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            request->result = XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
            request->completed = true;
            return request;
        }
        request->sequence = state->next_presenter_sequence++;
        request->owned_frame = std::move(owned_frame);
        request->borrowed_frame = borrowed_frame;
        request->queued_at = std::chrono::steady_clock::now();
        request->app_release_value = state->app_end_frame_release_value;
        if (request->owned_frame) {
            request->owned_frame->app_release_value =
                request->app_release_value;
        }
        state->presenter_submissions.push_back(request);
        ++state->outstanding_presenter_submissions;
    }
    state->presenter_condition.notify_all();
    return request;
}

[[nodiscard]] XrResult wait_for_presenter_submission(
    const std::shared_ptr<SessionState>& state,
    const std::shared_ptr<PresenterSubmission>& request) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return request->completed || XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        return request->completed
            ? request->result
            : XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

// One application frame's submissions, in the order they are shown: the
// synthetic, the second synthetic where the session makes one (null
// otherwise), then the real frame.
[[nodiscard]] XrResult enqueue_presenter_pair(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> synthetic,
    std::shared_ptr<GeneratedFrameEndInfo> extra_synthetic,
    std::shared_ptr<GeneratedFrameEndInfo> current) noexcept {
    try {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            return XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
        }
        const auto queued_at = std::chrono::steady_clock::now();
        const std::uint64_t app_release_value =
            state->app_end_frame_release_value;
        auto first = std::make_shared<PresenterSubmission>();
        first->sequence = state->next_presenter_sequence++;
        first->owned_frame = std::move(synthetic);
        first->queued_at = queued_at;
        first->app_release_value = app_release_value;
        if (first->owned_frame) {
            first->owned_frame->app_release_value = app_release_value;
        }
        state->presenter_submissions.push_back(std::move(first));
        ++state->outstanding_presenter_submissions;
        if (extra_synthetic) {
            auto middle = std::make_shared<PresenterSubmission>();
            middle->sequence = state->next_presenter_sequence++;
            middle->owned_frame = std::move(extra_synthetic);
            middle->queued_at = queued_at;
            middle->app_release_value = app_release_value;
            middle->owned_frame->app_release_value = app_release_value;
            state->presenter_submissions.push_back(std::move(middle));
            ++state->outstanding_presenter_submissions;
        }
        auto second = std::make_shared<PresenterSubmission>();
        second->sequence = state->next_presenter_sequence++;
        second->owned_frame = std::move(current);
        second->queued_at = queued_at;
        second->app_release_value = app_release_value;
        if (second->owned_frame) {
            second->owned_frame->app_release_value = app_release_value;
        }
        state->presenter_submissions.push_back(std::move(second));
        ++state->outstanding_presenter_submissions;
        state->presenter_condition.notify_all();
        return XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
}

[[nodiscard]] bool capture_projection_snapshot(
    const XrFrameEndInfo* source,
    ProjectionSnapshot* output) {
    if (source == nullptr || output == nullptr || source->layerCount == 0 ||
        source->layers == nullptr) {
        return false;
    }

    ProjectionSnapshot snapshot{};
    snapshot.display_time = source->displayTime;
    snapshot.environment_blend_mode = source->environmentBlendMode;
    for (std::uint32_t layer_index = 0; layer_index < source->layerCount; ++layer_index) {
        const XrCompositionLayerBaseHeader* layer = source->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount == 0 || projection->views == nullptr ||
            projection->space == XR_NULL_HANDLE) {
            return false;
        }

        ProjectionLayerSnapshot stored{};
        stored.layer_index = layer_index;
        stored.layer_flags = projection->layerFlags;
        stored.space = projection->space;
        stored.views.assign(
            projection->views,
            projection->views + projection->viewCount);
        for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
            const XrSwapchain application_swapchain =
                projection->views[view_index].subImage.swapchain;
            if (application_swapchain == XR_NULL_HANDLE) {
                return false;
            }
            stored.views[view_index].next = nullptr;
        }
        snapshot.layers.push_back(std::move(stored));
    }
    if (snapshot.layers.empty()) {
        return false;
    }
    *output = std::move(snapshot);
    return true;
}

[[nodiscard]] bool matching_sub_image(
    const XrSwapchainSubImage& left,
    const XrSwapchainSubImage& right) noexcept {
    return left.swapchain == right.swapchain &&
           left.imageArrayIndex == right.imageArrayIndex &&
           left.imageRect.offset.x == right.imageRect.offset.x &&
           left.imageRect.offset.y == right.imageRect.offset.y &&
           left.imageRect.extent.width == right.imageRect.extent.width &&
           left.imageRect.extent.height == right.imageRect.extent.height;
}

[[nodiscard]] bool overlapping_sub_images(
    const XrSwapchainSubImage& left,
    const XrSwapchainSubImage& right) noexcept {
    if (left.imageArrayIndex != right.imageArrayIndex) {
        return false;
    }
    const std::int64_t left_right =
        static_cast<std::int64_t>(left.imageRect.offset.x) +
        left.imageRect.extent.width;
    const std::int64_t left_bottom =
        static_cast<std::int64_t>(left.imageRect.offset.y) +
        left.imageRect.extent.height;
    const std::int64_t right_right =
        static_cast<std::int64_t>(right.imageRect.offset.x) +
        right.imageRect.extent.width;
    const std::int64_t right_bottom =
        static_cast<std::int64_t>(right.imageRect.offset.y) +
        right.imageRect.extent.height;
    return left.imageRect.offset.x < right_right &&
           right.imageRect.offset.x < left_right &&
           left.imageRect.offset.y < right_bottom &&
           right.imageRect.offset.y < left_bottom;
}

// Records, for each swapchain that holds one eye of a two-view projection
// alone, which eye it is (SwapchainState::projection_eye).
void note_projection_eyes(
    const ProjectionSnapshot& snapshot,
    const ProjectionMappingResult& mappings) noexcept {
    try {
        if (mappings.reason != ProjectionMappingReason::ready) {
            return;
        }
        for (const ProjectionResourceMapping& mapping : mappings.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) {
                continue;
            }
            swapchain->projection_used.store(true, std::memory_order_relaxed);
            int eye = -1;
            if (mapping.views.size() == 1 && swapchain->create_info.arraySize == 1) {
                const ProjectionViewReference& view = mapping.views.front();
                if (view.projection_index < snapshot.layers.size() &&
                    snapshot.layers[view.projection_index].views.size() == 2 &&
                    view.view_index < 2) {
                    eye = static_cast<int>(view.view_index);
                }
            }
            const int previous =
                swapchain->projection_eye.exchange(eye, std::memory_order_relaxed);
            if (previous != eye) {
                // 750: a swapchain's eye, a the swapchain, b the eye + 1 (0
                // when it holds both or none), c the eye before + 1.
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_transition,
                    750,
                    handle_value(mapping.application_swapchain),
                    static_cast<std::uint64_t>(eye + 1),
                    static_cast<std::uint64_t>(previous + 1));
            }
        }
    } catch (...) {
    }
}

[[nodiscard]] ProjectionMappingResult
build_projection_resource_mappings(const ProjectionSnapshot& snapshot) noexcept {
    ProjectionMappingResult output{};
    try {
        if (snapshot.layers.empty() || snapshot.layers.front().views.empty()) {
            output.reason = ProjectionMappingReason::no_projection_views;
            return output;
        }
        for (std::size_t projection_index = 0;
             projection_index < snapshot.layers.size();
             ++projection_index) {
            const ProjectionLayerSnapshot& layer =
                snapshot.layers[projection_index];
            for (std::size_t view_index = 0;
                 view_index < layer.views.size();
                 ++view_index) {
                const XrSwapchainSubImage& sub_image =
                    layer.views[view_index].subImage;
                const auto swapchain = find_swapchain(sub_image.swapchain);
                if (!swapchain) {
                    output.reason = ProjectionMappingReason::unknown_swapchain;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                const XrSwapchainCreateInfo& create_info =
                    swapchain->create_info;
                if (create_info.arraySize == 0 || create_info.arraySize > 2) {
                    output.reason = ProjectionMappingReason::unsupported_array_size;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                if (create_info.width == 0 || create_info.height == 0) {
                    output.reason = ProjectionMappingReason::zero_resource_extent;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                if (sub_image.imageArrayIndex >= create_info.arraySize) {
                    output.reason = ProjectionMappingReason::array_slice_out_of_range;
                    output.detail =
                        (handle_value(sub_image.swapchain) << 8) |
                        sub_image.imageArrayIndex;
                    return output;
                }
                if (sub_image.imageRect.offset.x < 0 ||
                    sub_image.imageRect.offset.y < 0) {
                    output.reason = ProjectionMappingReason::negative_subimage_offset;
                    output.detail =
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 sub_image.imageRect.offset.x)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.offset.y);
                    return output;
                }
                if (sub_image.imageRect.extent.width <= 0 ||
                    sub_image.imageRect.extent.height <= 0) {
                    output.reason =
                        ProjectionMappingReason::nonpositive_subimage_extent;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                const std::uint64_t subimage_right =
                    static_cast<std::uint64_t>(sub_image.imageRect.offset.x) +
                    static_cast<std::uint32_t>(
                        sub_image.imageRect.extent.width);
                const std::uint64_t subimage_bottom =
                    static_cast<std::uint64_t>(sub_image.imageRect.offset.y) +
                    static_cast<std::uint32_t>(
                        sub_image.imageRect.extent.height);
                if (subimage_right > create_info.width ||
                    subimage_bottom > create_info.height) {
                    output.reason = ProjectionMappingReason::subimage_out_of_bounds;
                    output.detail =
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 sub_image.imageRect.extent.width)) << 32) |
                        static_cast<std::uint32_t>(
                            sub_image.imageRect.extent.height);
                    return output;
                }

                auto mapping = std::find_if(
                    output.mappings.begin(),
                    output.mappings.end(),
                    [&](const ProjectionResourceMapping& candidate) {
                        return candidate.application_swapchain ==
                               sub_image.swapchain;
                    });
                if (mapping == output.mappings.end()) {
                    ProjectionResourceMapping created{};
                    created.application_swapchain = sub_image.swapchain;
                    output.mappings.push_back(std::move(created));
                    mapping = std::prev(output.mappings.end());
                }
                const auto existing_view = std::find_if(
                    mapping->views.begin(),
                    mapping->views.end(),
                    [&](const ProjectionViewReference& reference) {
                        return matching_sub_image(
                            snapshot.layers[reference.projection_index]
                                .views[reference.view_index]
                                .subImage,
                            sub_image);
                    });
                if (existing_view == mapping->views.end()) {
                    mapping->views.push_back({projection_index, view_index});
                }
            }
        }
        for (const ProjectionResourceMapping& mapping : output.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain || mapping.views.empty() || mapping.views.size() > 2) {
                output.reason = ProjectionMappingReason::unsupported_view_layout;
                output.detail = handle_value(mapping.application_swapchain);
                return output;
            }
            std::array<bool, 2> covered_slices{};
            for (std::size_t view_index = 0;
                 view_index < mapping.views.size();
                 ++view_index) {
                const ProjectionViewReference& reference = mapping.views[view_index];
                const XrSwapchainSubImage& sub_image =
                    snapshot.layers[reference.projection_index]
                        .views[reference.view_index]
                        .subImage;
                covered_slices[sub_image.imageArrayIndex] = true;
                for (std::size_t preceding = 0;
                     preceding < view_index;
                     ++preceding) {
                    const ProjectionViewReference& preceding_reference =
                        mapping.views[preceding];
                    const XrSwapchainSubImage& preceding_sub_image =
                        snapshot.layers[preceding_reference.projection_index]
                            .views[preceding_reference.view_index]
                            .subImage;
                    if (overlapping_sub_images(sub_image, preceding_sub_image)) {
                        output.reason =
                            ProjectionMappingReason::unsupported_view_layout;
                        output.detail = handle_value(mapping.application_swapchain);
                        return output;
                    }
                }
            }
            for (std::uint32_t slice = 0;
                 slice < swapchain->create_info.arraySize;
                 ++slice) {
                if (!covered_slices[slice]) {
                    output.reason = ProjectionMappingReason::missing_array_slice;
                    output.detail = handle_value(mapping.application_swapchain);
                    return output;
                }
            }
        }
        output.reason = ProjectionMappingReason::ready;
        return output;
    } catch (...) {
        output.mappings.clear();
        output.reason = ProjectionMappingReason::exception;
        return output;
    }
}

[[nodiscard]] bool matching_projection_camera(
    const XrPosef& left_pose,
    const XrFovf& left_fov,
    const XrPosef& right_pose,
    const XrFovf& right_fov) noexcept {
    return left_pose.orientation.x == right_pose.orientation.x &&
           left_pose.orientation.y == right_pose.orientation.y &&
           left_pose.orientation.z == right_pose.orientation.z &&
           left_pose.orientation.w == right_pose.orientation.w &&
           left_pose.position.x == right_pose.position.x &&
           left_pose.position.y == right_pose.position.y &&
           left_pose.position.z == right_pose.position.z &&
           left_fov.angleLeft == right_fov.angleLeft &&
           left_fov.angleRight == right_fov.angleRight &&
           left_fov.angleUp == right_fov.angleUp &&
           left_fov.angleDown == right_fov.angleDown;
}

[[nodiscard]] std::optional<std::vector<xrfg::D3D12ReprojectionView>>
build_reprojection_views(
    const ProjectionSnapshot& snapshot,
    const ProjectionResourceMapping& mapping) {
    if (snapshot.layers.empty() || mapping.views.empty()) {
        return std::nullopt;
    }
    // The application-submitted projection pose/FOV is the authoritative
    // render-camera contract for these pixels. An application may legitimately
    // transform or replace its xrLocateViews result before submission, so the
    // raw locate sample is neither intercepted nor an eligibility condition.
    for (const ProjectionLayerSnapshot& layer : snapshot.layers) {
        for (const XrCompositionLayerProjectionView& view : layer.views) {
            if (view.subImage.swapchain != mapping.application_swapchain) {
                continue;
            }
            const auto reference = std::find_if(
                mapping.views.begin(),
                mapping.views.end(),
                [&](const ProjectionViewReference& candidate) {
                    if (candidate.projection_index >= snapshot.layers.size() ||
                        candidate.view_index >=
                            snapshot.layers[candidate.projection_index].views.size()) {
                        return false;
                    }
                    return matching_sub_image(
                        view.subImage,
                        snapshot.layers[candidate.projection_index]
                            .views[candidate.view_index]
                            .subImage);
                });
            if (reference == mapping.views.end()) {
                return std::nullopt;
            }
            if (reference->projection_index >= snapshot.layers.size() ||
                reference->view_index >=
                    snapshot.layers[reference->projection_index].views.size()) {
                return std::nullopt;
            }
            const XrCompositionLayerProjectionView& canonical =
                snapshot.layers[reference->projection_index]
                    .views[reference->view_index];
            if (!matching_projection_camera(
                    view.pose,
                    view.fov,
                    canonical.pose,
                    canonical.fov)) {
                return std::nullopt;
            }
        }
    }

    std::vector<xrfg::D3D12ReprojectionView> output(
        mapping.views.size());
    for (std::size_t view_index = 0;
         view_index < mapping.views.size();
         ++view_index) {
        const ProjectionViewReference& reference =
            mapping.views[view_index];
        if (reference.projection_index >= snapshot.layers.size() ||
            reference.view_index >=
                snapshot.layers[reference.projection_index].views.size()) {
            return std::nullopt;
        }
        const XrCompositionLayerProjectionView& source =
            snapshot.layers[reference.projection_index]
                .views[reference.view_index];
        xrfg::D3D12ReprojectionView& destination = output[view_index];
        destination.pose.orientation = {
            source.pose.orientation.x,
            source.pose.orientation.y,
            source.pose.orientation.z,
            source.pose.orientation.w,
        };
        destination.pose.position = {
            source.pose.position.x,
            source.pose.position.y,
            source.pose.position.z,
        };
            destination.fov = {
            source.fov.angleLeft,
            source.fov.angleRight,
            source.fov.angleUp,
            source.fov.angleDown,
        };
        destination.image_rect = {
            static_cast<std::uint32_t>(source.subImage.imageRect.offset.x),
            static_cast<std::uint32_t>(source.subImage.imageRect.offset.y),
            static_cast<std::uint32_t>(source.subImage.imageRect.extent.width),
            static_cast<std::uint32_t>(source.subImage.imageRect.extent.height),
        };
        destination.array_slice = source.subImage.imageArrayIndex;
    }
    return output;
}

// Whether two consecutive application frames can be paired.
//
// require_advancing_time is off in the deeper pipeline, and the reason is the
// same one that keeps the time out of the layout test below: the display time
// an application submits is its own label, not the layer's timeline and not a
// statement about content. MSFS 2024 runs its own frame timing and submits
// times that wander by whole display periods around the ones it is served -
// with the served sequence strictly increasing, 1373 of 1567 submitted times
// were values the layer never handed it, and 202 went backwards. Each of those
// frames was refused a pair and generated as a prime, with no synthetic at
// all: 13.9% of frames, up from 2.7% on the shallow pipeline, because a
// deeper pipeline puts more of that application's frames in flight.
//
// What the time was standing in for is checked properly elsewhere. That the
// current frame is the one after the previous is proven by the capture
// serials in D3D12FrameSynthesizer::submit_pair, which refuses anything that
// is neither the same capture nor the next one. And a degenerate interval
// cannot produce a bad midpoint, because the interpolation fraction falls
// back to one half outside 0.05-0.95.
[[nodiscard]] bool projection_snapshots_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current,
    bool require_advancing_time) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        (require_advancing_time &&
         current.display_time <= previous.display_time) ||
        previous.layers.size() != current.layers.size()) {
        return false;
    }
    for (std::size_t layer_index = 0; layer_index < current.layers.size(); ++layer_index) {
        const ProjectionLayerSnapshot& left = previous.layers[layer_index];
        const ProjectionLayerSnapshot& right = current.layers[layer_index];
        if (left.layer_index != right.layer_index || left.space != right.space ||
            left.layer_flags != right.layer_flags || left.views.size() != right.views.size()) {
            return false;
        }
        for (std::size_t view_index = 0; view_index < right.views.size(); ++view_index) {
            if (!matching_sub_image(
                    left.views[view_index].subImage,
                    right.views[view_index].subImage)) {
                return false;
            }
        }
    }
    return true;
}

// A space/flag transition only invalidates the interpolation pair and can be
// re-primed on the current application frame. A layout transition changes the
// resources or regions consumed by generation and must cross the quarantine.
//
// A display time that does not advance is deliberately not tested here. It is
// not a layout change - it names no different swapchain, sub-image, view or
// blend mode - and it is not the layer's own timeline either. MSFS 2024
// derives the time it submits from the predicted time it was given, and it
// derives it non-monotonically: with the virtual clock made strictly
// increasing and verified so over 1281 consecutive waits, 287 of the 1280
// frames that application submitted still went backwards, by whole multiples
// of the display period. A pipelined application that reorders two frames in
// flight will do this, and no clock the layer hands it prevents it.
//
// Quarantining for it is also self-sustaining: the outage puts the
// application on the fail-open path, which produces the next non-advancing
// time, which starts the next outage. Captured MSFS 2024 sessions spent 3768
// of 3895 and 1176 of 1280 frames in structural quarantine that way,
// re-entering it faster than the one-second duration expires. Clearing
// continuity and priming again on this frame handles it for the cost of one
// prime.
[[nodiscard]] bool projection_resource_layout_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        previous.layers.size() != current.layers.size()) {
        return false;
    }
    for (std::size_t layer_index = 0; layer_index < current.layers.size();
         ++layer_index) {
        const ProjectionLayerSnapshot& left = previous.layers[layer_index];
        const ProjectionLayerSnapshot& right = current.layers[layer_index];
        if (left.layer_index != right.layer_index ||
            left.views.size() != right.views.size()) {
            return false;
        }
        for (std::size_t view_index = 0; view_index < right.views.size();
             ++view_index) {
            if (!matching_sub_image(
                    left.views[view_index].subImage,
                    right.views[view_index].subImage)) {
                return false;
            }
        }
    }
    return true;
}

struct ProjectionResourceDestination {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain destination_swapchain{XR_NULL_HANDLE};
    // The synthetic written there was generated in its own camera
    // (D3D12FrameSynthesisTicket::synthetics_in_target_camera), so its views
    // take their poses from that camera.
    bool synthetic_camera{};
};

// A synthetic's own camera. A synthetic is shown between the two real frames
// it is made from - after the newer one when extrapolating - and the runtime
// reprojects every submitted image from the pose it was submitted with to the
// head's pose when it is shown. Submitted with the newer frame's pose, a
// synthetic shown a display period before that frame is turned back by the
// head's motion over the period, and the edge it turns away from has no
// pixels: under a head turn, black strips that flicker at the game's frame
// rate (at 3X, 144 Hz and 120 degrees a second, 1.7 and 0.8 degrees of view
// for the two synthetics). Here each view's pose is instead the one `fraction`
// of the way from the previous frame's to the current one's - the share of
// the span the synthetic's content is placed at, so the head's pose when it is
// shown - with the current frame's field of view. The synthesizer generates
// the synthetic in that camera and it is submitted with it.
//
// A turn or a move between the two frames that no head makes within a frame
// - a recentre, a teleport - keeps the current frame's pose for every view:
// what lies between two such poses is not a camera anyone looked from.
constexpr float kMaxSyntheticCameraTurnRadians = 0.7853982F;  // 45 degrees
constexpr float kMaxSyntheticCameraMoveMeters = 0.5F;

[[nodiscard]] xrfg::Pose to_pose(const XrPosef& pose) noexcept {
    return {
        {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
        {pose.position.x, pose.position.y, pose.position.z},
    };
}

[[nodiscard]] std::optional<ProjectionSnapshot> synthetic_camera_snapshot(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current,
    float fraction) noexcept {
    try {
        if (previous.layers.size() != current.layers.size()) {
            return std::nullopt;
        }
        ProjectionSnapshot camera = current;
        for (std::size_t layer_index = 0; layer_index < camera.layers.size(); ++layer_index) {
            auto& views = camera.layers[layer_index].views;
            const auto& previous_views = previous.layers[layer_index].views;
            if (previous_views.size() != views.size()) {
                return std::nullopt;
            }
            for (std::size_t view_index = 0; view_index < views.size(); ++view_index) {
                const xrfg::Pose from = to_pose(previous_views[view_index].pose);
                const xrfg::Pose to = to_pose(views[view_index].pose);
                const float move_x = to.position.x - from.position.x;
                const float move_y = to.position.y - from.position.y;
                const float move_z = to.position.z - from.position.z;
                const float turn = xrfg::rotation_angle(from.orientation, to.orientation);
                const float move = std::sqrt(move_x * move_x + move_y * move_y + move_z * move_z);
                if (!(turn <= kMaxSyntheticCameraTurnRadians) ||
                    !(move <= kMaxSyntheticCameraMoveMeters)) {
                    return std::nullopt;
                }
                const xrfg::Pose shown = xrfg::pose_at_fraction(from, to, fraction);
                views[view_index].pose = {
                    {shown.orientation.x, shown.orientation.y, shown.orientation.z,
                     shown.orientation.w},
                    {shown.position.x, shown.position.y, shown.position.z},
                };
            }
        }
        return camera;
    } catch (...) {
        return std::nullopt;
    }
}

// camera: a synthetic's own camera (synthetic_camera_snapshot), whose poses
// its views take where the destination says it was generated in it.
[[nodiscard]] bool build_generated_frame_end_info(
    const XrFrameEndInfo* source,
    const ProjectionSnapshot& current_snapshot,
    bool synthetic,
    std::span<const ProjectionResourceDestination> destinations,
    GeneratedFrameEndInfo* output,
    const ProjectionSnapshot* camera = nullptr) {
    if (source == nullptr || output == nullptr || destinations.empty() ||
        source->layerCount == 0 || source->layers == nullptr ||
        current_snapshot.layers.empty()) {
        return false;
    }

    output->info = *source;
    output->layer_pointers.assign(
        source->layers,
        source->layers + source->layerCount);
    output->projections.reserve(current_snapshot.layers.size());

    for (std::size_t projection_index = 0;
         projection_index < current_snapshot.layers.size();
         ++projection_index) {
        const ProjectionLayerSnapshot& metadata = current_snapshot.layers[projection_index];
        if (metadata.layer_index >= source->layerCount) {
            return false;
        }
        const XrCompositionLayerBaseHeader* layer = source->layers[metadata.layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            return false;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount != metadata.views.size() || projection->views == nullptr) {
            return false;
        }
        ProjectionLayerCopy copy{};
        copy.layer_index = metadata.layer_index;
        copy.layer = *projection;
        copy.views.assign(
            projection->views,
            projection->views + projection->viewCount);
        for (std::size_t view_index = 0; view_index < copy.views.size(); ++view_index) {
            XrCompositionLayerProjectionView& view = copy.views[view_index];
            const auto destination = std::find_if(
                destinations.begin(),
                destinations.end(),
                [&](const ProjectionResourceDestination& candidate) {
                    return candidate.application_swapchain ==
                           view.subImage.swapchain;
                });
            if (destination == destinations.end() ||
                destination->destination_swapchain == XR_NULL_HANDLE) {
                return false;
            }
            view.subImage.swapchain = destination->destination_swapchain;
            if (synthetic) {
                // The synthetic image is generated in B's field of view, and
                // from B's pose or from a camera of its own: its projection
                // metadata must say which. Nothing chained to B's view
                // describes it.
                view.next = nullptr;
                if (camera != nullptr && destination->synthetic_camera &&
                    projection_index < camera->layers.size() &&
                    view_index < camera->layers[projection_index].views.size()) {
                    view.pose = camera->layers[projection_index].views[view_index].pose;
                }
            }
        }
        output->projections.push_back(std::move(copy));
        ProjectionLayerCopy& stored = output->projections.back();
        if (synthetic) {
            stored.layer.next = nullptr;
        }
        stored.layer.views = stored.views.data();
        const auto* stored_header =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stored.layer);
        output->layer_pointers[metadata.layer_index] = stored_header;
    }
    output->info.layerCount =
        static_cast<std::uint32_t>(output->layer_pointers.size());
    output->info.layers = output->layer_pointers.data();
    return true;
}

[[nodiscard]] bool acquire_and_wait_private_image(
    SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept {
    try {
        if (!dispatch || image.handle == XR_NULL_HANDLE ||
            dispatch->acquire_swapchain_image == nullptr ||
            dispatch->wait_swapchain_image == nullptr ||
            dispatch->release_swapchain_image == nullptr) {
            return false;
        }
        if (image.phase != PrivateOwnershipPhase::idle &&
            !release_private_image(session, dispatch, image)) {
            return false;
        }

        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        std::uint32_t acquired_index = 0;
        const auto acquire_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            handle_value(image.handle),
            static_cast<std::uint64_t>(image.phase));
        const XrResult acquire_result = with_runtime_entry(session, [&] {
            return dispatch->acquire_swapchain_image(
                image.handle, &acquire_info, &acquired_index);
        });
        xrfg::bridge_flight_logger().end(
            acquire_token,
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            acquire_result,
            handle_value(image.handle),
            acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (XR_FAILED(acquire_result)) {
            return false;
        }
        image.acquired_index = acquired_index;
        image.phase = PrivateOwnershipPhase::acquired;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        const auto wait_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_wait,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        const XrResult wait_result = with_runtime_entry(session, [&] {
            return dispatch->wait_swapchain_image(image.handle, &wait_info);
        });
        xrfg::bridge_flight_logger().end(
            wait_token,
            xrfg::BridgeFlightOperation::private_swapchain_wait,
            wait_result,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (wait_result != XR_SUCCESS && wait_result != XR_SESSION_LOSS_PENDING) {
            return false;
        }
        image.phase = PrivateOwnershipPhase::waited;
        if (!image.first_use_logged && session != nullptr) {
            image.first_use_logged = true;
            log_video_memory(*session, VideoMemoryStage::private_first_use,
                handle_value(image.handle));
        }
        return true;
    } catch (...) {
        return false;
    }
}

enum class PreparedGenerationKind {
    none,
    prime,
    pair,
};

enum class GenerationPrepareReason : std::int64_t {
    ready = 0,
    empty_mappings = 1,
    reprojection_views_failed = 2,
    unknown_swapchain = 3,
    missing_generation = 4,
    missing_capture = 5,
    invalid_private_swapchain = 6,
    retire_previous_failed = 7,
    current_private_acquire_failed = 8,
    synthetic_private_acquire_failed = 9,
    synthesis_failed = 10,
    private_release_failed = 11,
    mixed_resource_transaction = 12,
    generated_end_info_failed = 13,
    exception = 14,
    cooldown_active = 15,
    presenter_unsafe_composition = 16,
    manual_disarmed = 17,
    structural_quarantine_active = 18,
    synthesis_busy = 19,
    // Retired in V328: a session that never takes a presenter now generates
    // a pipelined frame by adopting the held wait (submit_current_cycle).
    // Kept so the flight-log value stays unique.
    inline_only_pipelined = 20,
    // The application's next wait was forwarded and the runtime still holds
    // it un-begun, so the inline second cycle's own wait would block until
    // a begin that cannot come before this call returns. Passed through;
    // see SessionState::runtime_frame_waited_unbegun.
    inline_wait_outstanding = 21,
};

[[nodiscard]] constexpr GenerationPrepareReason classify_synthesis_failure(
    HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_BUSY) ||
            result == DXGI_ERROR_WAS_STILL_DRAWING
        ? GenerationPrepareReason::synthesis_busy
        : GenerationPrepareReason::synthesis_failed;
}

static_assert(classify_synthesis_failure(DXGI_ERROR_WAS_STILL_DRAWING) ==
    GenerationPrepareReason::synthesis_busy);
struct PreparedGeneration {
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain current_handle{XR_NULL_HANDLE};
    XrSwapchain synthetic_handle{XR_NULL_HANDLE};
    // The pair's second synthetic, where one was made: null otherwise.
    XrSwapchain extra_synthetic_handle{XR_NULL_HANDLE};
    // The synthesizer holding this pair's deferred current copy, so the
    // presenter can submit it once the synthetic frame has gone, and the
    // value that copy signals - which the flush needs by hand, because the
    // synthesiser's own latest value may belong to a later pair.
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    std::uint64_t copy_fence_value{};
    bool anchor_is_current{};
    // The pair's synthetics were generated in the target cameras they were
    // given; see D3D12FrameSynthesisTicket::synthetics_in_target_camera.
    bool synthetics_in_target_camera{};
    // Set only when the release to the runtime is left to whoever hands the
    // frame over; the images stay acquired until then.
    std::shared_ptr<FrameGenerationSwapchainState> deferred_generation;
    PrivateSwapchainState* deferred_synthetic{};
    PrivateSwapchainState* deferred_extra_synthetic{};
    PrivateSwapchainState* deferred_current{};
    xrfg::D3D12FrameSynthesisTicket deferred_ticket{};
    // Single-swapchain rings: the staging each output was written to.
    ID3D12Resource* deferred_synthetic_staging{};
    ID3D12Resource* deferred_extra_staging{};
    ID3D12Resource* deferred_current_staging{};
    // Where the pair's output becomes readable by the runtime, for the
    // presenter's readiness hold in the deeper pipeline and for the release
    // delay in 3X. Not set otherwise.
    Microsoft::WRL::ComPtr<ID3D12Fence> ready_fence;
    std::uint64_t ready_value{};
};

struct PreparedProjectionResource {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    PreparedGeneration generation;
};

struct PreparedProjectionFrame {
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain failed_swapchain{XR_NULL_HANDLE};
    std::vector<PreparedProjectionResource> resources;
    bool anchor_is_current{};
    // Every resource's synthetics were asked for in their own cameras.
    bool synthetic_cameras{};
};

[[nodiscard]] PreparedGeneration prepare_frame_generation(
    XrSwapchain application_swapchain,
    bool request_pair,
    std::span<const xrfg::D3D12ReprojectionView> current_source_views,
    // The cameras the synthetic and the second synthetic are generated in
    // (synthetic_camera_snapshot). Empty: the current frame's.
    std::span<const xrfg::D3D12ReprojectionView> synthetic_target_views,
    std::span<const xrfg::D3D12ReprojectionView> extra_target_views,
    float interpolation_fraction,
    // Where a second synthetic belongs, for a session that makes one.
    std::optional<float> extra_interpolation_fraction,
    bool release_at_handover_requested) noexcept {
    PreparedGeneration output{};
    try {
        const auto state = find_swapchain(application_swapchain);
        if (!state) {
            output.reason = GenerationPrepareReason::unknown_swapchain;
            return output;
        }

        std::scoped_lock call_lock(state->call_mutex);
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        std::optional<xrfg::D3D12HistoryCaptureTicket> capture;
        std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_vectors;
        {
            std::scoped_lock lock(state->mutex);
            generation = state->frame_generation;
            capture = state->last_released_capture;
            motion_vectors = state->last_released_motion_vectors;
        }
        if (!generation || !generation->synthesizer) {
            output.reason = GenerationPrepareReason::missing_generation;
            return output;
        }
        if (!capture) {
            output.reason = GenerationPrepareReason::missing_capture;
            return output;
        }
        // Single-swapchain rings: the slots index the staging textures and
        // every output goes through its one swapchain, which nothing here
        // acquires - the hand-over does, with the staging in hand.
        const bool single_rings = generation->single_swapchain_rings;
        const std::size_t current_slot = generation->current_slot;
        PrivateSwapchainState& current_image =
            generation->current[single_rings ? 0 : current_slot];
        const std::size_t synthetic_slot = generation->synthetic_slot;
        PrivateSwapchainState& synthetic_image =
            generation->synthetic[single_rings ? 0 : synthetic_slot];
        if (current_image.handle == XR_NULL_HANDLE ||
            synthetic_image.handle == XR_NULL_HANDLE ||
            generation->current_images_per_slot == 0 ||
            generation->synthetic_images_per_slot == 0) {
            output.reason = GenerationPrepareReason::invalid_private_swapchain;
            return output;
        }
        // The second synthetic takes the ring's other slot. Without one to
        // take, the pair is an ordinary pair.
        const std::size_t extra_slot =
            (synthetic_slot + 1) % generation->synthetic_slot_count;
        PrivateSwapchainState& extra_image =
            generation->synthetic[single_rings ? 0 : extra_slot];
        const bool request_extra = request_pair &&
            extra_interpolation_fraction.has_value() &&
            extra_slot != synthetic_slot &&
            extra_image.handle != XR_NULL_HANDLE;

        if (!request_pair) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            const HRESULT retire_result = generation->synthesizer->retire_previous();
            if (FAILED(retire_result)) {
                output.reason = GenerationPrepareReason::retire_previous_failed;
                return output;
            }
        }

        if (!single_rings &&
            !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image)) {
            output.reason = GenerationPrepareReason::current_private_acquire_failed;
            if (request_pair) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(generation->synthesizer->retire_previous());
            }
            return output;
        }

        if (request_pair && !single_rings &&
            !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                synthetic_image)) {
            output.reason =
                GenerationPrepareReason::synthetic_private_acquire_failed;
            static_cast<void>(release_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image));
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
            return output;
        }

        if (request_extra && !single_rings &&
            !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                extra_image)) {
            output.reason =
                GenerationPrepareReason::synthetic_private_acquire_failed;
            static_cast<void>(release_private_image(
                state->session.get(),
                state->session->dispatch,
                synthetic_image));
            static_cast<void>(release_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image));
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
            return output;
        }

        // Both private images are acquired. The runtime made them safe to
        // write on the queue the application supplied, which is not the one
        // about to write them, so carry that guarantee across before any
        // synthesis is queued.
        if (state->session->d3d12_synthesis_queue && !single_rings) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(
                generation->synthesizer->synchronize_producer_queue(
                    runtime_queue(*state->session)));
        }

        const std::uint32_t current_destination_index = single_rings
            ? static_cast<std::uint32_t>(current_slot)
            : static_cast<std::uint32_t>(current_slot) *
                    generation->current_images_per_slot +
                current_image.acquired_index;
        const std::uint32_t synthetic_destination_index = single_rings
            ? static_cast<std::uint32_t>(synthetic_slot)
            : static_cast<std::uint32_t>(synthetic_slot) *
                    generation->synthetic_images_per_slot +
                synthetic_image.acquired_index;
        std::optional<xrfg::D3D12ExtraSynthetic> extra_synthetic;
        if (request_extra) {
            extra_synthetic = xrfg::D3D12ExtraSynthetic{
                single_rings
                    ? static_cast<std::uint32_t>(extra_slot)
                    : static_cast<std::uint32_t>(extra_slot) *
                            generation->synthetic_images_per_slot +
                        extra_image.acquired_index,
                *extra_interpolation_fraction,
                extra_target_views};
        }

        // Deferring the current copy keeps a full-resolution copy the
        // synthetic never reads off its critical path, but it only works where
        // the copy's destination is read after flush_current_copy. The D3D11
        // interop's publish is read before it: it moves both results back
        // across to the application's D3D11 images while the copy is still an
        // unsubmitted command list, so it publishes the previous pair's B as
        // this pair's current frame. The headset then runs forward to the
        // midpoint and back a whole pair, every pair, which reads as doubling
        // that scales with motion and disappears wherever the scene is still.
        //
        // The deeper pipeline does not defer either, for the opposite reason.
        // Deferring existed to keep the copy off the critical path of a
        // synthetic that had no slack; the depth gives it a whole display
        // period of slack, so there is nothing left to protect. What deferring
        // costs instead is an asymmetry between the two halves of a pair: the
        // synthetic's pixels exist milliseconds before its hand-over, while
        // the real frame's do not exist until the synthetic is handed over and
        // then have to cross a queue boundary to reach the runtime. Submitted
        // inline both halves are written and signalled together, and the
        // consumer-queue join for the current image is not needed at all.
        // Extrapolation hands the real frame over first, so its copy must
        // already be on the queue.
        const bool defer_current_copy = generation->interop == nullptr &&
            !state->session->deep_pipeline &&
            !state->session->nvidia_options.extrapolate &&
            state->session->nvidia_options.frame_generation == xrfg::D3D12FrameGeneration::ofxr;
        xrfg::D3D12FrameSynthesisTicket ticket{};
        const auto debug_marker = request_pair && xrfg::bridge_flight_logger().enabled() &&
                state->session->fps_overlay
            ? state->session->fps_overlay->marker_placement()
            : std::nullopt;
        HRESULT submit_result = E_UNEXPECTED;
        const xrfg::BridgeFlightOperation synthesis_operation = request_pair
            ? xrfg::BridgeFlightOperation::synthesis_pair
            : xrfg::BridgeFlightOperation::synthesis_prime;
        const auto synthesis_token = xrfg::bridge_flight_logger().begin(
            synthesis_operation,
            handle_value(application_swapchain),
            capture->serial,
            (static_cast<std::uint64_t>(
                 synthetic_destination_index) << 32) |
                current_destination_index);
        {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            log_completed_nvidia_gpu_timings(*state->session, generation->synthesizer);
            if (generation->interop) {
                submit_result =
                    generation->interop->prepare_synthesis();
            }
            if (!generation->interop || SUCCEEDED(submit_result)) {
                submit_result = request_pair
                                    ? generation->synthesizer->submit_pair(
                                          *capture,
                                          current_source_views,
                                          synthetic_target_views.empty()
                                              ? current_source_views
                                              : synthetic_target_views,
                                          synthetic_destination_index,
                                          current_destination_index,
                                          &ticket,
                                          debug_marker,
                                          motion_vectors,
                                          defer_current_copy,
                                          interpolation_fraction,
                                          extra_synthetic)
                                    : generation->synthesizer->submit_prime(
                                          *capture,
                                          current_source_views,
                                          current_destination_index,
                                          &ticket,
                                          motion_vectors);
            }
            if (SUCCEEDED(submit_result) && generation->interop) {
                const auto publish_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? synthetic_destination_index
                                 : std::numeric_limits<std::uint32_t>::max());
                const HRESULT publish_result =
                    generation->interop->publish(
                        current_destination_index,
                        request_pair
                            ? std::optional<std::uint32_t>(
                                  synthetic_destination_index)
                            : std::nullopt,
                        extra_synthetic
                            ? std::optional<std::uint32_t>(
                                  extra_synthetic->destination_index)
                            : std::nullopt);
                xrfg::bridge_flight_logger().end(
                    publish_token,
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    publish_result,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? synthetic_destination_index
                                 : std::numeric_limits<std::uint32_t>::max());
                if (FAILED(publish_result)) {
                    static_cast<void>(generation->synthesizer->retire_previous());
                    submit_result = publish_result;
                }
            }
        }
        xrfg::bridge_flight_logger().end(
            synthesis_token,
            synthesis_operation,
            submit_result,
            ticket.previous_serial,
            ticket.current_serial,
            ticket.fence_value);

        // The runtime orders its use of these swapchain images against the
        // binding queue (runtime_queue): a release marks "complete as of
        // here" on it. So when synthesis ran on a queue of its own, the
        // binding queue has to wait for it immediately before the release,
        // or the mark says complete before the pixels exist.
        //
        // Where that join and release go depends on whose queue the runtime
        // has. Everything on it is first in, first out, and it is also where
        // the runtime marks each submission at xrEndFrame - the point the
        // compositor waits for before it can use the frame.
        //
        // With the layer's own binding queue they wait for the hand-over, on
        // the presenter. Joined here instead, the wait for this pair's
        // synthesis would sit on that queue ahead of the previous pair's
        // synthetic hand-over, which follows this call, and the previous
        // synthetic would become visible only once this pair's synthesis -
        // queued behind the game's frame - had finished. Measured on Hogwarts
        // Legacy (V307): 124 synthetics lost that way, their marks clearing
        // 8.5 ms after submission. At the hand-over the only thing ahead of
        // the frame is its own join, and the readiness hold has already seen
        // that synthesis finish.
        //
        // With the application's queue (no binding queue, or D3D11, whose
        // immediate context must not be driven from the presenter thread)
        // they stay here, where the mark lands before anything of the game's
        // next frame.
        // Single-swapchain rings always hand over: nothing was acquired here
        // and the copy into the runtime's image is the hand-over's.
        const bool release_at_handover = single_rings ||
            (release_at_handover_requested &&
             generation->interop == nullptr &&
             SUCCEEDED(submit_result));
        bool synthetic_released = true;
        bool current_released = true;
        if (!release_at_handover) {
            if (state->session->d3d12_synthesis_queue &&
                SUCCEEDED(submit_result)) {
                static_cast<void>(
                    generation->synthesizer->synchronize_consumer_queue(
                        runtime_queue(*state->session),
                        ticket));
            }
            if (request_pair) {
                synthetic_released = release_private_image(
                    state->session.get(),
                    state->session->dispatch,
                    synthetic_image);
            }
            if (request_extra) {
                synthetic_released = release_private_image(
                    state->session.get(),
                    state->session->dispatch,
                    extra_image) && synthetic_released;
            }
            current_released = release_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image);
            if (state->session->d3d12_synthesis_queue &&
                SUCCEEDED(submit_result)) {
                // One mark for the pair: both images were released at the
                // same point in the queue. b is the capture serial, c is 3
                // for a pair and 1 for a prime.
                signal_join_probe(
                    state->session.get(),
                    913,
                    ticket.current_serial,
                    request_pair ? 3u : 1u);
            }
        }

        if (FAILED(submit_result)) {
            output.reason = classify_synthesis_failure(submit_result);
            if (request_pair) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(generation->synthesizer->retire_previous());
            }
            return output;
        }

        output.anchor_is_current = true;
        output.synthetics_in_target_camera =
            request_pair && ticket.synthetics_in_target_camera;
        output.current_handle = current_image.handle;
        output.synthetic_handle = synthetic_image.handle;
        if (request_extra) {
            output.extra_synthetic_handle = extra_image.handle;
        }
        if (current_released && synthetic_released) {
            // Hand the next frame the other slot, so it can release its output
            // while this one is still un-retired behind the presenter.
            generation->current_slot =
                (current_slot + 1) % kCurrentSlotCount;
            // Only a pair touches the synthetic, so only a pair advances it.
            // With one slot this is a no-op and the synthetic stays shared,
            // which is correct at the shallow admission bound.
            if (request_pair) {
                generation->synthetic_slot =
                    (synthetic_slot + (request_extra ? 2 : 1)) %
                    generation->synthetic_slot_count;
            }
            // Only a deferred copy leaves anything to do after the
            // synthetic reaches the runtime. A prime always submits inline,
            // and so does a pair in the deeper pipeline.
            if (request_pair && defer_current_copy) {
                output.synthesizer = generation->synthesizer;
                output.copy_fence_value = ticket.fence_value;
            }
            // The value that says this pair's output exists. On D3D11 that is
            // the publish copy on the application's context, not synthesis:
            // the runtime reads the D3D11 images the copy writes. Both
            // accessors hand back a fence the presenter can poll without
            // either object's lock.
            // And 3X, where the release delay is steered on it.
            if (request_pair &&
                (state->session->deep_pipeline ||
                 state->session->frames_per_application_frame > 2)) {
                if (generation->interop) {
                    std::uint64_t published = 0;
                    if (SUCCEEDED(generation->interop->publication_fence(
                            output.ready_fence.ReleaseAndGetAddressOf(),
                            &published))) {
                        output.ready_value = published;
                    }
                } else if (SUCCEEDED(generation->synthesizer->completion_fence(
                               output.ready_fence.ReleaseAndGetAddressOf()))) {
                    output.ready_value = ticket.synthetic_fence_value != 0
                        ? ticket.synthetic_fence_value
                        : ticket.fence_value;
                }
            }
            if (release_at_handover) {
                // Slot addresses stay valid for as long as the generation
                // state lives, which the shared pointer guarantees. Whether
                // the slot is free to reuse is still decided by retirement:
                // a submission retires only after its downstream xrEndFrame,
                // and that comes after this release.
                output.deferred_generation = generation;
                output.deferred_current = &current_image;
                output.deferred_synthetic =
                    request_pair ? &synthetic_image : nullptr;
                output.deferred_extra_synthetic =
                    request_extra ? &extra_image : nullptr;
                output.deferred_ticket = ticket;
                if (single_rings) {
                    output.deferred_current_staging =
                        generation->current_staging[current_slot].Get();
                    output.deferred_synthetic_staging = request_pair
                        ? generation->synthetic_staging[synthetic_slot].Get()
                        : nullptr;
                    output.deferred_extra_staging = request_extra
                        ? generation->synthetic_staging[extra_slot].Get()
                        : nullptr;
                }
            }
            output.kind = request_pair ? PreparedGenerationKind::pair
                                       : PreparedGenerationKind::prime;
            output.reason = GenerationPrepareReason::ready;
        } else {
            output.reason = GenerationPrepareReason::private_release_failed;
        }

        return output;
    } catch (...) {
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}


// Where a synthetic belongs, as a share of the span from the previous capture
// to the current one. Real frames go down `frames` display periods apart with
// a synthetic in each period between. Interpolating, synthetic `index`,
// counted from the first one shown, is (index + 1) / frames of the way - the
// midpoint for a pair. Extrapolating, the real frame goes first, and a
// synthetic `periods` display periods after it shows the scene 1 + periods /
// frames spans on.
//
// These used to be measured from the frames' display times, as 1 - period /
// interval and 1 + period * periods / interval, so that a game below half the
// display rate had its synthetic at the instant its content was from. But the
// frames do not go down at their stamps: a pair takes its slots whatever the
// stamps say, and a frame that misses its slot repeats the frame before
// rather than widening the pair. Against where each synthetic was actually
// shown between its real frames, in Galactic Racer races on the Steam Frame,
// the cadence erred 0.0080 on average where the stamps erred 0.0159 with the
// deeper pipeline, 0.0046 against 0.0089 without it, 0.0211 against 0.0311 at
// 3X and 0.0126 against 0.0205 extrapolating; in Hubris, whose engine stamps
// one frame in ten with the time of the frame before, 0.0010 against 0.0124.
// For every gap between the stamps, the median of where the synthetic
// belonged was the cadence's share. Where a frame does miss its slot, the
// cadence is also the smoother choice: the stall is the repeat, and the
// cadence spreads what is left of the motion evenly over the frames after it.
[[nodiscard]] float synthetic_interpolation_fraction(
    std::uint32_t frames,
    std::uint32_t index) noexcept {
    return static_cast<float>(index + 1) / static_cast<float>(frames);
}
// Extrapolating goes on from the current frame along its motion, which spans
// the game's own step from the previous frame - the frame time it gives DLSS.
// The synthetic should show the scene `periods` display periods of game time
// on: 1 + periods / (the game's step in display periods), which is the
// cadence's share when the step is the frame period. The game's step swings
// about the cadence and back the next frame, so the fixed share overshoots
// after a long step and falls short after a short one. In a Galactic Racer
// race scored against the game's own frame times (the next frame's step
// spread over the display until it is shown), this erred 0.0179 on average
// where the fixed share erred 0.0266, the stamps 0.0303 and the hand-over
// interval 0.0394. Without the game's frame time, or with a step outside
// half to twice the frame period, the fixed share.
[[nodiscard]] float synthetic_extrapolation_fraction(
    std::uint32_t frames,
    std::uint32_t periods,
    float game_step_periods = 0.0F) noexcept {
    const float frames_f = static_cast<float>(frames);
    if (game_step_periods >= frames_f * 0.5F && game_step_periods <= frames_f * 2.0F) {
        return 1.0F + static_cast<float>(periods) / game_step_periods;
    }
    return 1.0F + static_cast<float>(periods) / frames_f;
}

// The game's step to the current frame in display periods, from the frame
// time it gave DLSS with this frame's vectors; 0 when it gave none.
[[nodiscard]] float game_step_display_periods(
    SessionState& state,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    try {
        if (mappings.empty()) {
            return 0.0F;
        }
        const auto swapchain = find_swapchain(mappings.front().application_swapchain);
        if (!swapchain) {
            return 0.0F;
        }
        std::shared_ptr<const xrfg::DlssMotionVectorSet> vectors;
        {
            std::scoped_lock lock(swapchain->mutex);
            vectors = swapchain->last_released_motion_vectors;
        }
        if (!vectors || vectors->eye_count == 0 || !vectors->eyes[0] ||
            !vectors->eyes[0]->frame_time_known) {
            return 0.0F;
        }
        XrDuration period = 0;
        {
            std::scoped_lock lock(state.mutex);
            period = state.minimum_runtime_display_period;
        }
        // While SteamVR has halved the rate a display period is two.
        if (state.runtime_slowed.load(std::memory_order_relaxed)) {
            period *= 2;
        }
        if (period <= 0) {
            return 0.0F;
        }
        return vectors->eyes[0]->frame_time_delta_ms * 1.0e6F / static_cast<float>(period);
    } catch (...) {
        return 0.0F;
    }
}

[[nodiscard]] PreparedProjectionFrame prepare_projection_frame(
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    bool request_pair,
    float interpolation_fraction,
    std::optional<float> extra_interpolation_fraction,
    // The synthetics' own cameras (synthetic_camera_snapshot), or null for
    // the current frame's.
    const ProjectionSnapshot* synthetic_camera,
    const ProjectionSnapshot* extra_synthetic_camera,
    bool release_at_handover) noexcept {
    PreparedProjectionFrame output{};
    try {
        if (mappings.empty()) {
            output.reason = GenerationPrepareReason::empty_mappings;
            return output;
        }
        const PreparedGenerationKind expected_kind = request_pair
            ? PreparedGenerationKind::pair
            : PreparedGenerationKind::prime;
        bool all_expected_kind = true;
        bool all_anchor_current = true;
        // The same views from the synthetics' own cameras, for every resource
        // or none: the frame's views are submitted with one camera's poses,
        // so a resource whose views could not be built would leave the frame
        // half in one camera and half in the other.
        using CameraViews = std::vector<xrfg::D3D12ReprojectionView>;
        std::vector<CameraViews> synthetic_views;
        std::vector<CameraViews> extra_views;
        output.synthetic_cameras = request_pair && synthetic_camera != nullptr;
        for (const ProjectionResourceMapping& mapping : mappings) {
            if (!output.synthetic_cameras) {
                break;
            }
            auto views = build_reprojection_views(*synthetic_camera, mapping);
            auto extra = extra_synthetic_camera
                ? build_reprojection_views(*extra_synthetic_camera, mapping)
                : std::optional<CameraViews>(CameraViews{});
            output.synthetic_cameras = views.has_value() && extra.has_value();
            if (output.synthetic_cameras) {
                synthetic_views.push_back(std::move(*views));
                extra_views.push_back(std::move(*extra));
            }
        }
        output.resources.reserve(mappings.size());
        for (std::size_t mapping_index = 0; mapping_index < mappings.size(); ++mapping_index) {
            const ProjectionResourceMapping& mapping = mappings[mapping_index];
            const auto reprojection_views =
                build_reprojection_views(snapshot, mapping);
            if (!reprojection_views) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                return output;
            }
            const auto camera_span = [&](const std::vector<CameraViews>& views) {
                return output.synthetic_cameras
                    ? std::span<const xrfg::D3D12ReprojectionView>(views[mapping_index])
                    : std::span<const xrfg::D3D12ReprojectionView>();
            };
            PreparedGeneration generation = prepare_frame_generation(
                mapping.application_swapchain,
                request_pair,
                std::span<const xrfg::D3D12ReprojectionView>(
                    reprojection_views->data(),
                    reprojection_views->size()),
                camera_span(synthetic_views),
                camera_span(extra_views),
                interpolation_fraction,
                extra_interpolation_fraction,
                release_at_handover);
            all_expected_kind =
                all_expected_kind && generation.kind == expected_kind;
            all_anchor_current =
                all_anchor_current && generation.anchor_is_current;
            if (generation.reason != GenerationPrepareReason::ready &&
                output.failed_swapchain == XR_NULL_HANDLE) {
                output.reason = generation.reason;
                output.failed_swapchain = mapping.application_swapchain;
            }
            output.resources.push_back({
                mapping.application_swapchain,
                generation,
            });
        }
        output.kind = all_expected_kind
            ? expected_kind
            : PreparedGenerationKind::none;
        output.anchor_is_current = all_anchor_current;
        if (all_expected_kind) {
            output.reason = GenerationPrepareReason::ready;
        } else if (output.failed_swapchain == XR_NULL_HANDLE) {
            output.reason = GenerationPrepareReason::mixed_resource_transaction;
        }
        return output;
    } catch (...) {
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

[[nodiscard]] std::optional<XrDuration> latest_pending_application_period(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order) noexcept {
    try {
        std::scoped_lock lock(state->mutex);
        if (consume_in_submission_order) {
            return state->pending_frames.empty()
                ? std::nullopt
                : std::optional<XrDuration>{
                      state->pending_frames.front().display_period};
        }
        // xrEndFrame::displayTime is an application-selected presentation
        // time, not an identity token for the preceding xrWaitFrame. UEVR's
        // Native Stereo Fix deliberately submits an older pipelined render
        // state while keeping a strictly sequential wait/begin/end call chain.
        // When exactly one wait is outstanding, call order identifies the
        // frame unambiguously even if those two times differ.
        if (state->pending_frames.size() == 1) {
            return state->pending_frames.front().display_period;
        }
        // With more than one outstanding, the label has to name one of them,
        // and any of them will do. DCS World waits from its own thread and
        // keeps two waits in flight, so the frame it ends is always the older
        // of the two - inline that shows up as an overlapping wait and is
        // consumed in submission order, but under the presenter the wait is
        // answered virtually and nothing marks the overlap, so this lookup is
        // what decides whether the frame pairs. Requiring the newest pending
        // frame here left every DCS frame primed and none generated.
        // consume_application_frame drops everything up to the match, so the
        // two stay in step.
        //
        // A label that names none of them primes this frame, and the consume
        // then empties the queue rather than keeping it. MSFS 2024 labels
        // every frame with a time of its own, so for it only the
        // one-outstanding case above ever pairs: a single wait left pending
        // by a frame the runtime rejected kept the queue at two or more for
        // the rest of a reported session, and every frame of it primed.
        const auto frame = std::find_if(
            state->pending_frames.begin(),
            state->pending_frames.end(),
            [display_time](const PendingApplicationFrame& candidate) {
                return candidate.display_time == display_time;
            });
        if (frame == state->pending_frames.end()) {
            return std::nullopt;
        }
        return frame->display_period;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] XrTime generation_resume_time(XrTime display_time) noexcept {
    constexpr XrTime maximum_time = std::numeric_limits<XrTime>::max();
    if (display_time > maximum_time - kGenerationCooldownDuration) {
        return maximum_time;
    }
    return display_time + kGenerationCooldownDuration;
}

void schedule_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail) noexcept {
    if (!state) {
        return;
    }
    try {
        const auto deadline =
            std::chrono::steady_clock::now() + kStructuralQuarantineDuration;
        {
            std::scoped_lock lock(state->mutex);
            if (deadline > state->generation_resume_wall_time) {
                state->generation_resume_wall_time = deadline;
            }
            state->previous_projection.reset();
        }
        state->generation_steady_state_established = false;
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_last_frame.reset();
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::continuity_reset,
            static_cast<std::int64_t>(reason),
            handle_value(state->handle),
            detail,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kStructuralQuarantineDuration)
                    .count()));
    } catch (...) {
    }
}

void clear_generation_continuity(
    const std::shared_ptr<SessionState>& state) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::continuity_reset,
        0,
        state ? handle_value(state->handle) : 0);
    try {
        {
            std::scoped_lock lock(state->mutex);
            state->previous_projection.reset();
        }
        for (const auto& swapchain : find_swapchains(state)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            std::scoped_lock gpu_lock(state->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
        }
    } catch (...) {
    }
}

void enter_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail) noexcept {
    schedule_generation_quarantine(state, reason, detail);
    clear_generation_continuity(state);
}

[[nodiscard]] bool should_quarantine_generation_failure(
    GenerationPrepareReason reason) noexcept {
    switch (reason) {
        case GenerationPrepareReason::reprojection_views_failed:
        case GenerationPrepareReason::unknown_swapchain:
        case GenerationPrepareReason::invalid_private_swapchain:
        case GenerationPrepareReason::retire_previous_failed:
        case GenerationPrepareReason::synthesis_failed:
        case GenerationPrepareReason::mixed_resource_transaction:
        case GenerationPrepareReason::generated_end_info_failed:
        case GenerationPrepareReason::exception:
        case GenerationPrepareReason::presenter_unsafe_composition:
            return true;
        default:
            return false;
    }
}

struct InternalCycleResult {
    XrResult result{XR_SUCCESS};
    std::chrono::steady_clock::duration wait_elapsed{};
    XrDuration predicted_display_period{};
    bool completed{};
};

// adopted_frame_state: the application's forwarded wait the runtime still
// holds un-begun. Asking the runtime for another frame would block until that
// one is begun, which the application cannot do before this call returns, so
// the cycle begins it instead and the application's next xrBeginFrame takes
// a fresh wait (application_begin_needs_wait). Same pacing - one runtime
// wait per application frame - moved from here to the application's begin.
[[nodiscard]] InternalCycleResult submit_current_cycle(
    const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo& current_end_info,
    std::optional<XrFrameState> adopted_frame_state = std::nullopt,
    // The frame is a synthetic: 3X runs one cycle for its second synthetic
    // before the one for the real frame.
    bool synthetic = false,
    // The frame is a synthetic for the recorder's reprojection_angle, where
    // it differs from `synthetic`: extrapolating, this cycle carries the
    // pair's synthetic, which the overlay has always counted as the real one.
    std::optional<bool> recorded_synthetic = std::nullopt) {
    InternalCycleResult output{};
    if (state->dispatch->wait_frame == nullptr ||
        state->dispatch->begin_frame == nullptr ||
        state->dispatch->end_frame == nullptr) {
        return output;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    if (adopted_frame_state) {
        frame_state = *adopted_frame_state;
        frame_state.next = nullptr;
        output.predicted_display_period = frame_state.predictedDisplayPeriod;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            501,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
            frame_state.shouldRender);
    } else {
        const auto wait_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_wait_frame,
            handle_value(state->handle));
        const auto wait_started = std::chrono::steady_clock::now();
        const XrResult wait_result = with_runtime_entry_for_wait(state, [&] {
            return state->dispatch->wait_frame(
                state->handle, &wait_info, &frame_state);
        });
        output.wait_elapsed = std::chrono::steady_clock::now() - wait_started;
        output.predicted_display_period = frame_state.predictedDisplayPeriod;
        xrfg::bridge_flight_logger().end(
            wait_token,
            xrfg::BridgeFlightOperation::internal_wait_frame,
            wait_result,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
            frame_state.shouldRender);
        if (XR_FAILED(wait_result)) {
            return output;
        }
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto begin_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_begin_frame,
        handle_value(state->handle));
    const XrResult begin_result = with_runtime_entry(state, [&] {
        return state->dispatch->begin_frame(state->handle, &begin_info);
    });
    xrfg::bridge_flight_logger().end(
        begin_token,
        xrfg::BridgeFlightOperation::internal_begin_frame,
        begin_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
    if (XR_FAILED(begin_result)) {
        output.result = begin_result;
        return output;
    }

    XrFrameEndInfo submitted = current_end_info;
    submitted.next = nullptr;
    submitted.displayTime = frame_state.predictedDisplayTime;
    if (frame_state.shouldRender == XR_FALSE) {
        submitted.layerCount = 0;
        submitted.layers = nullptr;
    }

    const auto end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_end_frame,
        handle_value(state->handle),
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount);
    join_runtime_queue_to_application(
        state.get(), state->app_end_frame_release_value);
    log_reprojection_angle(*state, submitted, recorded_synthetic.value_or(synthetic) ? 2U : 1U);
    const XrResult end_result = with_runtime_entry(state, [&] {
        return state->fps_overlay
            ? state->fps_overlay->end_frame(&submitted, synthetic)
            : state->dispatch->end_frame(state->handle, &submitted);
    });
    xrfg::bridge_flight_logger().end(
        end_token,
        xrfg::BridgeFlightOperation::internal_end_frame,
        end_result,
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount,
        frame_state.shouldRender);
    if (XR_FAILED(end_result)) {
        output.result = end_result;
        return output;
    }

    output.completed = true;
    return output;
}


// The presenter exists because some runtimes do not pace xrWaitFrame. The
// layer submits two frames per application frame and they must land one
// display period apart; a runtime that returns the wait immediately leaves
// both in the same scanout window and the compositor discards one, so the
// headset shows half rate while the GPU does all of the synthesis work.
//
// Inferring that from the application's threading - whether its second wait
// lands inside the previous frame, twice in a row - reads a property of the
// application under load rather than of the runtime. It moves with scene
// cost, and because the promotion latches the first time the race resolves,
// a session can run un-paced indefinitely and then engage the moment the
// view happens to get cheap. Measuring the runtime does not have that
// problem: how long its wait blocks is the same whether the headset is
// pointed at the floor or at a city.
//
// Scoped to SteamVR, like the throttle detector it sits beside. The two are
// the same runtime quirk seen from opposite ends - one throttles the inline
// second cycle, the other does not pace the application at all - and VDXR,
// which blocks a full period and gets this right for free, is deliberately
// left alone rather than being told apart by a measurement that a merely
// late application can also produce.
[[nodiscard]] bool runtime_wait_lacks_pacing(
    const std::shared_ptr<SessionState>& state) noexcept {
    // Enough waits to rule out an application that was simply late for one.
    constexpr std::uint32_t kUnpacedWaitPromotionStreak = 3;
    if (!state->dispatch->steamvr_runtime) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    return state->minimum_runtime_display_period > 0 &&
           state->unpaced_wait_streak >= kUnpacedWaitPromotionStreak;
}
// The inline path hands the synthetic to the runtime and then, immediately
// after, the real frame. Nothing in the layer decides how far apart those two
// land: the gap is whatever the runtime imposes when it blocks the second
// wait. When that gap is much shorter than a display period both frames
// arrive inside one scanout window, the compositor keeps the later one - the
// real frame - and drops the synthetic. The next window has nothing new and
// repeats. Half the layer's work is discarded, the overlay still counts it,
// and the application is separately halved because each of its frames now
// costs two runtime cycles. A game that held the full rate without the layer
// drops to half with it.
//
// The presenter fixes this by construction: it sleeps to its own
// scanout-locked grid between hand-overs, so the two frames are a period
// apart whatever the runtime does with a wait.
//
// Measured here rather than inferred. The two existing detectors ask whether
// the situation looks like one that needs a presenter - how many threads the
// application uses, how long a wait took - and on Atomic Heart both answer
// no while the output is plainly wrong. Captured on that title across two
// runtimes and two refresh rates, the gap sits at 0.42, 0.45 and 0.54 of a
// period against about 1.0 when the spacing is right, so three quarters
// separates them with margin either way.
//
// A long streak because promoting is a large switch - it hands every
// downstream frame call to another thread, and an over-eager version of the
// SteamVR detector once hung the GPU four milliseconds after firing. Thirty
// pairs is a quarter of a second at 120 Hz and cannot be reached by a hitch,
// while the real thing holds for thousands of consecutive frames.
[[nodiscard]] bool inline_pair_lands_in_one_scanout(
    const std::shared_ptr<SessionState>& state,
    std::chrono::steady_clock::duration gap) noexcept {
    constexpr std::uint32_t kBunchedPairPromotionStreak = 30;
    constexpr std::int64_t kBunchedNumerator = 3;
    constexpr std::int64_t kBunchedDenominator = 4;
    const auto gap_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(gap).count();
    std::scoped_lock lock(state->mutex);
    const XrDuration period = state->minimum_runtime_display_period;
    if (period <= 0 || gap_nanoseconds < 0) {
        state->bunched_pair_streak = 0;
        return false;
    }
    const bool bunched =
        gap_nanoseconds * kBunchedDenominator < period * kBunchedNumerator;
    state->bunched_pair_streak =
        bunched ? state->bunched_pair_streak + 1 : 0;
    if (!bunched ||
        state->bunched_pair_streak < kBunchedPairPromotionStreak) {
        return false;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        static_cast<std::int64_t>(state->bunched_pair_streak),
        static_cast<std::uint64_t>(gap_nanoseconds),
        static_cast<std::uint64_t>(period),
        200);
    return true;
}

// Virtual Desktop, a frame loop on one thread: the presenter, from the first
// generating frame, for the whole session.
//
// VDXR paces the first xrWaitFrame of a period and not the second, so inline
// the real frame follows the synthetic by a fraction of a period - IL-2 Great
// Battles at 90 Hz: 3.1 ms, 0.28 of the period, on 813 of 816 pairs - and
// the headset runs at half rate with more latency than the presenter gave the
// same title. Pacing the pair inline instead (V432) spaced it exactly and was
// worse: the hold sits on the application's thread ahead of its next render,
// so a pair costs render plus a period, and a title that renders in about a
// period - IL-2 over a city, CPU-bound, 10.3 ms - fell from 45 to 40 a second
// and the headset to 70. The presenter hands the real frame over on its own
// thread while the application renders the next one, which is the only way
// the two overlap. It is what V414-V416 did for this title, through the
// bunched-pair detector, and what the reporter called "super smooth".
//
// A split loop - the wait on one thread, the end on another, No Man's Sky -
// stays inline: on the presenter at 144 Hz it ran the GPU out of room, with
// synthetics held three periods and fifteen continuity resets a second,
// where inline held a steady 55 with a synthetic for each. Which mode wins
// there depends on GPU headroom, which is not known at start-up; the thread
// shape is, and it is the same every run.
//
// Decided from the runtime's name and from which thread calls which
// function, both fixed by the application's code and seen by its second
// frame (split_frame_loop latches on the first wait that follows an end from
// another thread, before anything can generate). Nothing measured, nothing
// that moves with the scene: the mode is settled at start-up and holds for
// the session, as on Pimax OpenXR. Asked on every inline generating frame,
// so a presenter the pause stopped, or a run of failures demoted, comes back
// the same way. 601: the request, once per request; a the end thread, c the
// graphics binding.
[[nodiscard]] bool frame_loop_takes_presenter(
    const std::shared_ptr<SessionState>& state) noexcept {
    if (!state->dispatch->virtual_desktop_runtime || state->split_frame_loop ||
        state->steamvr_presenter_start_requested) {
        return false;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        601,
        state->application_end_thread_id,
        0,
        static_cast<std::uint64_t>(state->graphics_binding));
    return true;
}

[[nodiscard]] bool steamvr_wait_requires_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    const InternalCycleResult& cycle) noexcept {
    if (!state->dispatch->steamvr_runtime || !cycle.completed) {
        return false;
    }
    const auto elapsed_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycle.wait_elapsed).count();
    std::uint32_t streak = 0;
    XrDuration baseline = 0;
    bool throttled = false;
    {
        std::scoped_lock lock(state->mutex);
        baseline = state->minimum_runtime_display_period;
        if (baseline > 0) {
            // Only a wait that actually blocked is evidence that the runtime is
            // throttling the inline cycle. An inflated predictedDisplayPeriod
            // says something different: SteamVR widens it when it considers the
            // *caller* late, so counting it closed a feedback loop: a warm-up
            // hitch widened the period, the widened period promoted the
            // presenter, and the promotion hitched harder. UEVR reached the
            // promotion three frames after its first synthesis on two 20 us
            // waits, with SteamVR reporting 33.3 ms and then 22.2 ms against an
            // 11.1 ms baseline, and the GPU hung 4 ms later.
            const XrDuration half_period = baseline / 2;
            throttled = elapsed_nanoseconds >= half_period;
        }
        state->steamvr_throttled_wait_streak = throttled
            ? state->steamvr_throttled_wait_streak + 1
            : 0;
        streak = state->steamvr_throttled_wait_streak;
    }
    if (throttled) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            streak,
            elapsed_nanoseconds > 0
                ? static_cast<std::uint64_t>(elapsed_nanoseconds)
                : 0,
            static_cast<std::uint64_t>(cycle.predicted_display_period),
            static_cast<std::uint64_t>(baseline));
    }
    return streak >= 3;
}

void consume_application_frame(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order) {
    std::scoped_lock lock(state->mutex);
    if (consume_in_submission_order) {
        if (!state->pending_frames.empty()) {
            state->pending_frames.pop_front();
        }
        return;
    }
    if (state->pending_frames.size() == 1) {
        state->pending_frames.pop_front();
        return;
    }
    const auto frame = std::find_if(
        state->pending_frames.begin(),
        state->pending_frames.end(),
        [display_time](const PendingApplicationFrame& candidate) {
            return candidate.display_time == display_time;
        });
    if (frame == state->pending_frames.end()) {
        // Nothing pending names this frame, so nothing pending can be told
        // apart from a wait whose frame is never coming. Left in place, such
        // a wait keeps an application that labels frames with its own time
        // (MSFS 2024) from ever being matched by elimination again, and it
        // primed every frame of a reported session. Start over: the next wait
        // is the only one outstanding, and the next frame pairs.
        state->pending_frames.clear();
        return;
    }
    state->pending_frames.erase(state->pending_frames.begin(), std::next(frame));
}

// Called with frame ownership and presenter content excluded. No GUI thread
// touches GPU objects; no new captures may enter during this transaction.
//
// The tray's "Pause frame generation" goes through the same transaction as
// the menu's enable: generation is on when both allow it. A pause or resume
// moves the application between its virtual period and the native one, so
// the presenter's queue is drained first and no submission of the old shape
// is left behind the change.
void apply_embedded_control(
    const std::shared_ptr<SessionState>& state,
    bool use_continuous_presenter) {
    const auto control = xrfg::embedded::snapshot();
    const bool paused = state->manual_control.pause_requested();
    const bool recording = xrfg::bridge_flight_logger().enabled();
    // The game's first DLSS vectors take up the hybrid; see
    // dlss_vectors_published.
    const bool vectors_arrived = state->dlss_flow_hybrid && state->dlss_motion_vectors &&
        state->extrapolate == 0 &&
        state->nvidia_options.frame_generation == xrfg::D3D12FrameGeneration::ofxr &&
        !state->dlss_vectors_published && xrfg::dlss_motion_vector_publications() != 0;
    if (state->control_revision == control.revision &&
        paused == state->pause_applied &&
        recording == state->recorder_applied && !vectors_arrived) return;
    // Any change, not only a pause or the recorder: a settings change rebuilds
    // the synthesizer, and frames the presenter still holds were made by the
    // old one. Their deferred real-frame copies must be flushed by it; flushed
    // against its successor's fence, whose values start again, they had the
    // application's queue wait for a value it would never reach, and SteamVR's
    // xrEndFrame waited on that queue for ever.
    if (use_continuous_presenter && XR_FAILED(wait_for_presenter_idle(state))) return;
    state->menu_enabled = false;
    xrfg::stop_ngx_guide_capture(state->d3d12_queue.Get());
    {
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
    }
    auto swapchains = find_swapchains(state);
    std::vector<std::unique_lock<std::mutex>> capture_locks;
    for (const auto& chain : swapchains) capture_locks.emplace_back(chain->call_mutex);
    std::scoped_lock gpu_lock(state->gpu_mutex);
    HRESULT result = S_OK;
    auto backend = static_cast<xrfg::D3D12OpticalFlowBackend>(control.desired.backend);
    if (backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state->nvidia_backend_unavailable) {
        backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    }
    xrfg::D3D12NvidiaOpticalFlowOptions options{
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(control.desired.scale), control.desired.backward,
        static_cast<xrfg::D3D12FrameGeneration>(control.desired.frame_generation),
        static_cast<std::uint32_t>(control.desired.native_scale)};
    const bool dlss_motion_vectors =
        control.desired.motion_vectors == 1 || control.desired.frame_generation == 1;
    // A control change also takes up the ini's choice of extrapolation warp,
    // of the hybrid and of the synthetics' pose, so they can be compared with
    // the other methods within one session.
    state->extrapolate_mesh =
        xrfg::implicit_layer::read_extrapolate_mesh(current_layer_directory());
    state->dlss_flow_hybrid =
        xrfg::implicit_layer::read_dlss_flow_hybrid(current_layer_directory());
    state->synthetic_pose_interpolated =
        xrfg::implicit_layer::read_synthetic_pose_interpolated(current_layer_directory());
    if (!state->dlss_vectors_published)
        state->dlss_vectors_published = xrfg::dlss_motion_vector_publications() != 0;
    synthesis_modes(*state, dlss_motion_vectors, options, backend);
    const bool changed = state->control_reconfigure_required || backend != state->optical_flow_backend ||
        options.preset != state->nvidia_options.preset ||
        options.input_scale != state->nvidia_options.input_scale ||
        options.bidirectional != state->nvidia_options.bidirectional ||
        options.frame_generation != state->nvidia_options.frame_generation ||
        options.native_scale != state->nvidia_options.native_scale ||
        options.hybrid != state->nvidia_options.hybrid ||
        options.extrapolate != state->nvidia_options.extrapolate ||
        options.extrapolate_mesh != state->nvidia_options.extrapolate_mesh;
    // Enumeration is excluded by frame_call_mutex, so newly created contexts
    // also see this tuple. A partial failure remains bypass, never mixed flow.
    state->optical_flow_backend = backend;
    state->nvidia_options = options;
    state->dlss_motion_vectors = dlss_motion_vectors;
    for (const auto& chain : swapchains) {
        std::unique_lock lock(chain->mutex);
        // The same gates the enumeration path applies, and for the same reason.
        //
        // This loop used to build generation for every colour swapchain in the
        // session. That is what 023bed4 removed from xrEnumerateSwapchainImages:
        // two private swapchains for everything judged eligible spent the
        // runtime's whole swapchain budget on UI quads, mirrors and per-pass
        // targets that never carry a projection view, and The Callisto Protocol
        // under UEVR hit the ceiling hard enough that xrCreateSession returned
        // XR_ERROR_LIMIT_REACHED four times and the game did not reach VR at
        // all. The fix went into the enumeration path; this one was missed, so
        // the same allocation still happened on any control revision - arming,
        // disarming, or changing a setting while a session is live. It only
        // stayed hidden because arming before launch leaves nothing here to
        // iterate.
        //
        // generation_eligible_pending is the projection-use gate: set at
        // enumeration, cleared once a projection layer has actually used the
        // swapchain. generation_declined means creation already failed for this
        // one, and retrying it on every settings change is how a transient
        // refusal becomes a permanent budget leak. The budget flag latches when
        // a runtime refuses a private swapchain, which says something about the
        // whole session rather than this swapchain.
        //
        // And only a swapchain a projection has named: pending is set at
        // enumeration for every candidate, so the session's first control
        // revision armed them all. Under UEVR that took three of SteamVR's
        // sixteen for each 2560x1440 spectator image before the eyes were
        // named, and the eyes were then refused (Lies of P and Subnautica 2 in
        // Alternating/AFR, whose eyes are images of their own; which way it
        // went depended on whether the revision came before the first
        // projection). The projection path arms the rest when they are named.
        const bool eligible = chain->generation_eligible_pending &&
            chain->projection_used.load(std::memory_order_relaxed) &&
            !chain->generation_declined &&
            !state->generation_budget_exhausted.load(std::memory_order_acquire);
        if (!chain->frame_generation && control.desired.enabled && !paused && eligible) {
            std::vector<ID3D11Texture2D*> sources;
            for (const auto& image : chain->enumerated_d3d11_images) sources.push_back(image.Get());
            const std::vector<VkImage> vulkan_sources = chain->enumerated_vulkan_images;
            lock.unlock();
            SwapchainEligibilityReason reason{};
            std::uint64_t detail{};
            std::shared_ptr<FrameGenerationSwapchainState> candidate;
            if (state->graphics_binding == SessionGraphicsBinding::d3d11) {
                candidate = create_d3d11_frame_generation_swapchains(chain, sources, &reason, &detail);
            } else if (state->graphics_binding == SessionGraphicsBinding::vulkan) {
                candidate = create_vulkan_frame_generation_swapchains(
                    chain, std::span<const VkImage>(vulkan_sources), &reason, &detail);
            } else {
                candidate = create_d3d12_frame_generation_swapchains(chain, &reason, &detail);
            }
            lock.lock();
            const std::uint64_t auxiliary =
                (static_cast<std::uint64_t>(chain->enumerated_image_count) << 32) |
                chain->create_info.arraySize;
            if (candidate) {
                chain->frame_generation = std::move(candidate);
                chain->generation_eligible_pending = false;
                lock.unlock();
                // Logged, unlike before. Its silence is why a build armed this
                // way showed no eligibility record at all and the two creation
                // paths could not be told apart in a capture.
                log_swapchain_eligibility(
                    chain,
                    SwapchainEligibilityReason::ready,
                    chain->enumerated_image_count,
                    auxiliary);
                lock.lock();
            } else {
                chain->generation_declined = true;
                if (reason == SwapchainEligibilityReason::synthesis_initialize_failed)
                    result = static_cast<HRESULT>(detail);
                lock.unlock();
                log_swapchain_eligibility(chain, reason, detail, auxiliary);
                lock.lock();
            }
        }
        if (chain->frame_generation && chain->frame_generation->synthesizer) {
            auto& synthesis = chain->frame_generation->synthesizer;
            result = synthesis->wait_for_idle();
            if (SUCCEEDED(result) && changed) {
                result = synthesis->reconfigure(backend, options);
                if (FAILED(result) &&
                    backend == xrfg::D3D12OpticalFlowBackend::nvidia) {
                    // The menu switched to NVIDIA on a machine without it.
                    // Same fallback as at creation, and the tray is told the
                    // change applied: generation carries on, on FidelityFX.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::synthesis_initialize,
                        result,
                        handle_value(state->handle),
                        2,
                        optical_flow_configuration_code(backend, options));
                    state->nvidia_backend_unavailable = true;
                    backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
                    result = synthesis->reconfigure(backend, options);
                }
            }
            if (SUCCEEDED(result) &&
                synthesis->gpu_timing_enabled() != recording) {
                // The recorder was switched while the session runs. The
                // rebuild costs one hitch and holds two sets of contexts for
                // a moment; where there is no room for that it fails, and
                // the session carries on with the synthesizer it had - a log
                // without GPU timings, never a session without generation.
                const HRESULT timing_result =
                    synthesis->reconfigure_gpu_timing(recording);
                if (FAILED(timing_result)) {
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::synthesis_initialize,
                        timing_result,
                        handle_value(state->handle),
                        3,
                        optical_flow_configuration_code(backend, options));
                }
            }
        }
        if (SUCCEEDED(result) && chain->d3d12_history) result = chain->d3d12_history->wait_for_idle();
        if (SUCCEEDED(result) && chain->frame_generation && chain->frame_generation->interop)
            result = chain->frame_generation->interop->wait_for_idle();
        chain->last_released_capture.reset();
        chain->last_released_motion_vectors.reset();
        if (FAILED(result)) break;
    }
    // OptiScaler sees the desired menu value during its earlier DLSS evaluate.
    // A disable stops new publications there. Keep the private snapshot slots
    // alive: the DLSS command list that recorded their CopyResource operations
    // is owned and submitted by the game, so an OFXR fence cannot prove that an
    // open/not-yet-submitted producer list has finished referencing them.
    // Re-enable resets temporal ownership while reusing the retained slots.
    {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        if (SUCCEEDED(result)) {
            state->optical_flow_backend = backend;
            state->nvidia_options = options;
        }
    }
    state->control_revision = control.revision;
    state->control_reconfigure_required = FAILED(result);
    state->pause_applied = paused;
    state->recorder_applied = recording;
    state->menu_enabled = SUCCEEDED(result) && control.desired.enabled && !paused;
    // OFXR's own synthesis can use the same captured DLSS guides as native
    // generation; without a provider publishing them, nothing else does.
    xrfg::configure_ngx_guide_capture(state->d3d12_queue.Get(),
        state->graphics_binding == SessionGraphicsBinding::d3d12 && state->menu_enabled &&
        state->dlss_motion_vectors);
    if (state->menu_enabled && state->presenter_restore_after_pause) {
        state->presenter_restore_after_pause = false;
        if (!presenter_forbidden(*state)) {
            // Started by the next xrEndFrame that generates, through the path
            // every promotion takes.
            state->steamvr_presenter_start_requested = true;
        }
    }
    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    xrfg::embedded::applied(state->control_id, control.revision, state->menu_enabled, result);
    // c: 1 generating, 0 off from the menu or a failed reconfiguration,
    // 2 paused from the tray.
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::embedded_configuration,
        result, control.revision, optical_flow_configuration_code(backend, options),
        state->menu_enabled ? 1 : paused ? 2 : 0);
}

// A bridged depth swapchain on D3D11BridgePath::depth_private has runtime
// images nothing ever writes, so depth information naming it must not reach
// the runtime. The application's end-frame data is read-only, so the
// projection layers whose views carry such a node are copied with the node
// unlinked; every other layer pointer is forwarded as it came. The storage
// lives for the xrEndFrame call, which is as long as anything downstream
// reads the application's data: an owned frame copies what it keeps, a
// borrowed one is waited for before this call returns.
struct PrivateDepthStrip {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::deque<ProjectionLayerCopy> projections;
};

[[nodiscard]] bool names_private_depth_swapchain(const void* next) noexcept {
    const auto* header = static_cast<const XrBaseInStructure*>(next);
    if (header == nullptr ||
        header->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
        return false;
    }
    const auto* depth = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(header);
    const auto swapchain_state = find_swapchain(depth->subImage.swapchain);
    if (!swapchain_state) {
        return false;
    }
    std::scoped_lock lock(swapchain_state->mutex);
    return swapchain_state->private_depth;
}

// Returns the end info to submit: the application's own unless a projection
// view's chain starts with depth information naming a private depth
// swapchain, in which case a copy without those nodes. Only a leading run
// of such nodes is unlinked: the chain's other structures are of types this
// layer does not know the size of, so a node behind one of them stays.
[[nodiscard]] const XrFrameEndInfo* strip_private_depth(
    const SessionState& state,
    const XrFrameEndInfo* end_info,
    PrivateDepthStrip& storage) {
    if (end_info == nullptr || end_info->layers == nullptr ||
        !state.has_private_depth_swapchain.load(std::memory_order_acquire)) {
        return end_info;
    }
    bool any = false;
    for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount && !any; ++layer_index) {
        const auto* layer = end_info->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        for (std::uint32_t view_index = 0;
             projection->views != nullptr && view_index < projection->viewCount; ++view_index) {
            if (names_private_depth_swapchain(projection->views[view_index].next)) {
                any = true;
                break;
            }
        }
    }
    if (!any) {
        return end_info;
    }
    storage.info = *end_info;
    storage.layers.assign(end_info->layers, end_info->layers + end_info->layerCount);
    for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount; ++layer_index) {
        const auto* layer = end_info->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->views == nullptr) {
            continue;
        }
        bool affected = false;
        for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
            affected = affected ||
                names_private_depth_swapchain(projection->views[view_index].next);
        }
        if (!affected) {
            continue;
        }
        ProjectionLayerCopy& copy = storage.projections.emplace_back();
        copy.layer_index = layer_index;
        copy.layer = *projection;
        copy.views.assign(projection->views, projection->views + projection->viewCount);
        for (XrCompositionLayerProjectionView& view : copy.views) {
            while (names_private_depth_swapchain(view.next)) {
                view.next = static_cast<const XrBaseInStructure*>(view.next)->next;
            }
        }
        copy.layer.views = copy.views.data();
        storage.layers[layer_index] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&copy.layer);
    }
    storage.info.layers = storage.layers.data();
    return &storage.info;
}

// Follows `[ofxr] synthetic_pose` while the session runs, so the two can be
// compared under the same head turn by editing the ini. Nothing has to be
// drained: each pair takes its synthetics' cameras and the poses it submits
// them with from one snapshot at its own xrEndFrame, so a pair already queued
// keeps the setting it was made with. Read at most twice a second.
void follow_synthetic_pose(SessionState& state) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now < state.synthetic_pose_poll_at) {
        return;
    }
    state.synthetic_pose_poll_at = now + std::chrono::milliseconds(500);
    state.synthetic_pose_interpolated =
        xrfg::implicit_layer::read_synthetic_pose_interpolated(current_layer_directory());
}

// Follows the tray's "3X Frame Gen" switch while the session runs. Called at
// the top of the application's xrEndFrame, under frame_call_mutex, before
// this frame is prepared.
//
// The two shapes share everything the session owns - two current slots and
// two synthetic ones - so nothing is created or destroyed. What changes is
// how many frames the presenter is given per application frame and the
// depth, and both are read by the presenter and by the application's wait.
// So the queue is drained first: every submission of the old shape has gone
// to the runtime, every private image has been released, and the frame that
// follows primes rather than pairing across the change. The application
// sees its period step between two and three display periods at its next
// wait, and one frame without synthetics.
void apply_live_frame_multiplier(
    const std::shared_ptr<SessionState>& state,
    bool use_continuous_presenter) noexcept {
    try {
        if (!state->triple_switchable) {
            return;
        }
        // The ini is rewritten by the tray; a read every quarter second is
        // cheap and nothing here is urgent.
        const auto now = std::chrono::steady_clock::now();
        if (now < state->triple_poll_at) {
            return;
        }
        state->triple_poll_at = now + std::chrono::milliseconds(250);
        const std::uint32_t target =
            xrfg::implicit_layer::read_triple_frame_gen(current_layer_directory()) &&
                    !native_triple_limited(*state)
                ? 3U
                : 2U;
        if (target == state->frames_per_application_frame) {
            return;
        }
        if (use_continuous_presenter &&
            XR_FAILED(wait_for_presenter_idle(state))) {
            return;
        }
        bool deep = false;
        {
            std::scoped_lock content_lock(state->presenter_content_mutex);
            std::scoped_lock lock(state->presenter_mutex);
            state->frames_per_application_frame = target;
            state->deep_pipeline =
                target == 2U && state->deep_pipeline_configured;
            deep = state->deep_pipeline;
            // The release delay belongs to the shape it was found in.
            state->triple_release_delay = std::chrono::nanoseconds::zero();
            state->triple_release_ceiling = std::chrono::nanoseconds(-1);
            state->triple_release_ceiling_hold = 0;
            state->triple_release_window_frames = 0;
            state->triple_release_window_late = 0;
            state->triple_release_window_unwritten = 0;
            state->triple_release_probe_due = false;
            // The once-per-frame hold counts from here in the new shape.
            state->application_served_serial = state->presenter_frame_serial;
        }
        clear_generation_continuity(state);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            700,
            target,
            deep ? 1u : 0u,
            2);
    } catch (...) {
    }
}

// projection_view_rect, for every view whose rectangle or array index
// differs from the last record for its swapchain.
void log_projection_view_rects(
    SessionState& state, const ProjectionSnapshot& snapshot) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().keeps_session_records()) {
            return;
        }
        for (const ProjectionLayerSnapshot& layer : snapshot.layers) {
            for (std::size_t view_index = 0; view_index < layer.views.size(); ++view_index) {
                const XrSwapchainSubImage& sub_image = layer.views[view_index].subImage;
                const std::array<std::uint64_t, 3> rect{
                    static_cast<std::uint64_t>(sub_image.imageArrayIndex),
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sub_image.imageRect.offset.x)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.offset.y),
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sub_image.imageRect.extent.width)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.extent.height)};
                auto& last = state.logged_view_rects[sub_image.swapchain];
                if (last == rect) {
                    continue;
                }
                last = rect;
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::projection_view_rect,
                    static_cast<std::int64_t>((rect[0] << 8) | (view_index & 0xFFU)),
                    handle_value(sub_image.swapchain),
                    rect[1],
                    rect[2]);
            }
        }
    } catch (...) {
    }
}

// A vram_usage record every five seconds of the application's frames, so a
// report of memory growing during play has a timeline to read against.
void log_video_memory_periodically(SessionState& state) noexcept {
    if (!xrfg::bridge_flight_logger().enabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < state.video_memory_poll_at) {
        return;
    }
    // Every second for the first minute: what a runtime or the game does
    // right after arming lands inside the first few seconds.
    if (state.video_memory_first_poll == std::chrono::steady_clock::time_point{}) {
        state.video_memory_first_poll = now;
    }
    state.video_memory_poll_at = now +
        (now - state.video_memory_first_poll < std::chrono::seconds(60)
             ? std::chrono::seconds(1)
             : std::chrono::seconds(5));
    log_video_memory(state, VideoMemoryStage::periodic, 0);
}

// The panel reads DLSS guide statuses by value; both lists must stay in step.
static_assert(static_cast<int>(xrfg::PanelVectorStatus::used) ==
              static_cast<int>(xrfg::DlssMotionVectorStatus::used));
static_assert(static_cast<int>(xrfg::PanelVectorStatus::multi_frame_unsupported) ==
              static_cast<int>(xrfg::DlssMotionVectorStatus::multi_frame_unsupported));

[[nodiscard]] int panel_scale_percent(int scale) noexcept {
    switch (scale) {
    case static_cast<int>(xrfg::D3D12OpticalFlowInputScale::full): return 100;
    case static_cast<int>(xrfg::D3D12OpticalFlowInputScale::three_quarter): return 75;
    case static_cast<int>(xrfg::D3D12OpticalFlowInputScale::quarter): return 25;
    default: return 50;
    }
}

// The status panel's account of the session: what the tray asks for now,
// what the session was started with, what it runs, and what is holding it
// back. Built at the application's xrEndFrame, a few times a second while
// the panel is up and never otherwise. The rates are the overlay's own, and
// the GPU time is added under gpu_mutex (panel_gpu_milliseconds).
[[nodiscard]] xrfg::StatusPanelInput status_panel_input(
    const std::shared_ptr<SessionState>& state,
    bool use_continuous_presenter,
    std::chrono::steady_clock::time_point now) {
    namespace layer = xrfg::implicit_layer;
    xrfg::StatusPanelInput input;
    const auto directory = current_layer_directory();

    // The tray's choice, as its ini now says.
    auto& asked = input.asked;
    asked.native = layer::read_frame_generation(directory) == layer::ConfiguredFrameGeneration::native_dlss;
    asked.native_scale = layer::read_native_dlssg_scale(directory);
    asked.flow = layer::read_flow_backend(directory) == layer::ConfiguredFlowBackend::nvidia
        ? xrfg::PanelFlow::nvidia : xrfg::PanelFlow::fidelity_fx;
    const auto configured = layer::read_nvidia_options(directory);
    asked.preset = static_cast<xrfg::PanelPreset>(configured.preset);
    asked.flow_scale = panel_scale_percent(static_cast<int>(configured.input_scale));
    asked.both_ways = configured.bidirectional;
    {
        std::array<wchar_t, 16> motion{};
        GetPrivateProfileStringW(L"ofxr", L"motion_vectors", L"off", motion.data(),
            static_cast<DWORD>(motion.size()), (directory / L"ofxr_bridge.ini").c_str());
        asked.game_vectors = asked.native || _wcsicmp(motion.data(), L"dlss") == 0;
    }
    asked.hybrid = layer::read_dlss_flow_hybrid(directory);
    asked.extrapolate = layer::read_extrapolate(directory);
    asked.mesh = layer::read_extrapolate_mesh(directory);
    asked.frames = layer::read_triple_frame_gen(directory) ? 3U : 2U;
    asked.deep = layer::read_deep_pipeline(directory);

    // What the session set out to run: the method the process read when it
    // started, the modes read at xrCreateSession, and 3X as the tray has it,
    // since a session follows that live.
    const auto desired = xrfg::embedded::snapshot().desired;
    auto& session = input.session;
    session.native = desired.frame_generation == 1;
    session.native_scale = desired.native_scale;
    session.flow = desired.backend == 1 ? xrfg::PanelFlow::nvidia : xrfg::PanelFlow::fidelity_fx;
    session.preset = static_cast<xrfg::PanelPreset>(desired.preset);
    session.flow_scale = panel_scale_percent(desired.scale);
    session.both_ways = desired.backward;
    session.game_vectors = desired.motion_vectors == 1 || session.native;
    session.hybrid = state->dlss_flow_hybrid;
    session.extrapolate = state->extrapolate;
    session.mesh = state->extrapolate_mesh;
    session.frames = asked.frames;
    session.deep = state->deep_pipeline_configured;

    // And what runs, after every fallback.
    const auto& options = state->nvidia_options;
    auto& running = input.running;
    running.native = options.frame_generation == xrfg::D3D12FrameGeneration::native_dlss;
    running.native_scale = static_cast<int>(options.native_scale);
    running.flow = state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia
        ? xrfg::PanelFlow::nvidia : xrfg::PanelFlow::fidelity_fx;
    running.preset = static_cast<xrfg::PanelPreset>(options.preset);
    running.flow_scale = panel_scale_percent(static_cast<int>(options.input_scale));
    running.both_ways = options.bidirectional;
    running.game_vectors = state->dlss_motion_vectors;
    running.hybrid = options.hybrid;
    running.extrapolate = options.extrapolate ? (options.extrapolate_hybrid ? 2 : 1) : 0;
    running.mesh = options.extrapolate_mesh;
    running.frames = state->frames_per_application_frame.load();
    running.deep = state->deep_pipeline;

    input.paused = state->pause_applied;
    input.enabled = desired.enabled && !state->control_reconfigure_required;
    input.settings_failed = state->control_reconfigure_required;
    input.budget_exhausted = state->generation_budget_exhausted.load(std::memory_order_acquire);
    for (const auto& chain : find_swapchains(state)) {
        std::scoped_lock lock(chain->mutex);
        if (chain->generation_declined && chain->projection_used.load(std::memory_order_relaxed)) {
            ++input.declined_images;
        }
    }
    if (now - state->panel_bypass_at < std::chrono::seconds(1)) {
        switch (static_cast<GenerationPrepareReason>(state->panel_bypass_reason)) {
        case GenerationPrepareReason::cooldown_active:
            input.bypass = xrfg::PanelBypass::cooldown;
            break;
        case GenerationPrepareReason::structural_quarantine_active:
            input.bypass = xrfg::PanelBypass::quarantine;
            break;
        case GenerationPrepareReason::empty_mappings:
            input.bypass = xrfg::PanelBypass::no_projection;
            break;
        case GenerationPrepareReason::inline_wait_outstanding:
            input.bypass = xrfg::PanelBypass::waiting_ahead;
            break;
        default:
            input.bypass = xrfg::PanelBypass::other;
            break;
        }
    }
    input.nvidia_unavailable = state->nvidia_backend_unavailable;
    input.vectors_published = state->dlss_vectors_published;
    if (session.native) {
        // Asked of NGX once: the first question may initialise it.
        if (!state->panel_native_frames) {
            state->panel_native_frames =
                xrfg::native_dlssg_max_generated_frames(state->d3d12_device.Get());
        }
        input.native_available = *state->panel_native_frames > 0;
        input.native_single_frame = *state->panel_native_frames == 1;
    }
    input.shallow_fallback = state->shallow_fallback;
    input.triple_fixed = !state->triple_switchable;

    // The guide counters since the last refresh. After a while hidden the
    // window would span all of it, so it starts again, and the panel reads
    // the last pair's report instead.
    const auto statistics = xrfg::dlss_motion_vector_statistics();
    auto& vectors = input.vectors;
    vectors.status = static_cast<xrfg::PanelVectorStatus>(statistics.status);
    vectors.published = statistics.published;
    if (now - state->panel_vectors_at < std::chrono::seconds(1)) {
        const auto since = [](std::uint64_t value, std::uint64_t before) {
            return value > before ? value - before : 0;
        };
        vectors.used = since(statistics.used, state->panel_vectors.used);
        vectors.temporal_rejections =
            since(statistics.temporal_rejections, state->panel_vectors.temporal_rejections);
        vectors.invalid_rejections =
            since(statistics.invalid_rejections, state->panel_vectors.invalid_rejections);
    }
    state->panel_vectors = statistics;
    state->panel_vectors_at = now;

    input.gpu_timing = state->recorder_applied;
    input.presenter = use_continuous_presenter;
    input.promise_periods = state->promise_shown_time
        ? static_cast<std::uint32_t>(std::max(
              state->promise_correction_periods.load(std::memory_order_relaxed), 0))
        : 0U;
    switch (state->graphics_binding) {
    case SessionGraphicsBinding::d3d12:
        input.graphics = state->d3d11_bridge ? xrfg::PanelGraphics::d3d11_bridge
            : state->vulkan_bridge          ? xrfg::PanelGraphics::vulkan_bridge
                                            : xrfg::PanelGraphics::d3d12;
        break;
    case SessionGraphicsBinding::d3d11:
        input.graphics = xrfg::PanelGraphics::d3d11_interop;
        break;
    case SessionGraphicsBinding::vulkan:
        input.graphics = xrfg::PanelGraphics::vulkan_interop;
        break;
    default:
        input.graphics = xrfg::PanelGraphics::other;
        break;
    }
    input.runtime = state->dispatch->runtime_name;
    return input;
}

// The status panel's GPU time a pair: every synthesizer measured in the last
// two seconds, which in a game with a swapchain per eye is both eyes'. The
// caller holds gpu_mutex.
[[nodiscard]] float panel_gpu_milliseconds(
    SessionState& state, std::chrono::steady_clock::time_point now) noexcept {
    float total = 0.0F;
    bool measured = false;
    for (auto entry = state.panel_gpu.begin(); entry != state.panel_gpu.end();) {
        if (now - entry->second.at > std::chrono::seconds(2)) {
            entry = state.panel_gpu.erase(entry);
            continue;
        }
        total += entry->second.microseconds;
        measured = true;
        ++entry;
    }
    return measured ? total / 1000.0F : -1.0F;
}

XrResult layer_end_frame_impl(
    XrSession session,
    const XrFrameEndInfo* end_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    PrivateDepthStrip private_depth_strip;
    if (end_info && end_info->layers) {
        for (uint32_t l=0;l<end_info->layerCount;++l) {
            const auto* header=end_info->layers[l];
            if (!header || header->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(header);
            for (uint32_t v=0;projection->views && v<projection->viewCount;++v) {
                auto* next=static_cast<const XrBaseInStructure*>(projection->views[v].next);
                for (;next;next=next->next) {
                    if (next->type!=XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) continue;
                    const auto* depth=reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(next);
                    xrfg::update_ngx_guide_depth(depth->nearZ,depth->farZ,depth->minDepth,depth->maxDepth);
                }
            }
        }
    }
    end_info = strip_private_depth(*state, end_info, private_depth_strip);

    // Releasable, because the once-per-pair hold at the end of this function
    // must not keep the application's other thread out of xrWaitFrame while it
    // waits. Everything this mutex protects is finished by then.
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    state->application_end_thread_id = GetCurrentThreadId();
    const auto application_end_now = std::chrono::steady_clock::now();
    if (state->capture_at_end_frame) {
        // The history captures this frame's releases left for here; before
        // anything below reads last_released_capture.
        capture_pending_end_frame_images(state);
        // Then mark the queue once more, behind those captures, so the join
        // below covers them and whatever the application submitted after
        // its releases - the case capture_at_end_frame exists for. The mark
        // has to follow the capture, as it does at a release: the capture
        // moves the application's image through a copy state, and a runtime
        // on the binding queue that began reading the image before the
        // capture had run would be reading it under that transition.
        mark_application_release(state.get());
    }
    // Every image of the application's that this frame names was released
    // before this call, so the counter as it stands now covers them all.
    {
        std::scoped_lock join_lock(state->binding_join_mutex);
        state->app_end_frame_release_value = state->app_release_counter;
    }
    const bool frame_had_overlapping_wait =
        state->application_frame_has_overlapping_wait;
    const bool pipelined_presenter_mode = state->pipelined_presenter_mode;
    // Both promotions hand the presenter its first frame at the top of an
    // xrEndFrame, before the inline second cycle runs, so the handoff never
    // overlaps a frame this thread has already submitted.
    const bool pipelined_presenter_start_requested =
        state->pipelined_presenter_start_requested ||
        state->steamvr_presenter_start_requested;
    const bool consume_in_submission_order =
        pipelined_presenter_mode || frame_had_overlapping_wait;
    const bool runtime_wait_outstanding = state->runtime_frame_waited_unbegun;
    state->application_frame_in_progress = false;
    state->application_frame_has_overlapping_wait = false;
    if (state->steamvr_delivery &&
        state->application_frames_submitted < kEstablishedApplicationFrames) {
        if (++state->application_frames_submitted >=
            kEstablishedApplicationFrames) {
            state->steamvr_delivery->mark_established();
        }
    }
    if (!frame_had_overlapping_wait && !pipelined_presenter_mode) {
        state->pipelined_wait_streak = 0;
    }
    const bool use_continuous_presenter = continuous_presenter_active(state);
    if (use_continuous_presenter &&
        (end_info == nullptr || end_info->type != XR_TYPE_FRAME_END_INFO)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::unique_lock<std::mutex> presenter_content_lock;
    // Drains the presenter and takes the content lock. Used by the paths that
    // hand the runtime the application's own composition unchanged: they have
    // no private output of their own, so the retained repeat has to be settled
    // before they submit.
    const auto enter_presenter_exclusive = [&]() -> XrResult {
        if (!use_continuous_presenter || presenter_content_lock.owns_lock()) {
            return XR_SUCCESS;
        }
        const XrResult idle_result = wait_for_presenter_idle(state);
        if (XR_FAILED(idle_result)) {
            return idle_result;
        }
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
        return XR_SUCCESS;
    };

    apply_live_frame_multiplier(state, use_continuous_presenter);
    follow_synthetic_pose(*state);
    log_video_memory_periodically(*state);
    apply_embedded_control(state, use_continuous_presenter);
    const bool manually_disarmed = state->manual_control.stop_requested();
    if (manually_disarmed && !state->manual_stop_applied) {
        // The existing queue has been drained before taking the content lock.
        // Do not unload hooks/resources, cancel submitted GPU work, or hand
        // overlapping frame calls to a different owner mid-cycle.
        clear_generation_continuity(state);
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            state->presenter_last_frame.reset();
        }
        state->manual_stop_applied = true;
        if (state->fps_overlay) state->fps_overlay->suspend();
    }
    if (state->fps_overlay && !manually_disarmed) {
        // The status panel's input only when it is up and due a repaint.
        std::optional<xrfg::StatusPanelInput> status;
        if (state->fps_overlay->status_wanted()) {
            status = status_panel_input(state, use_continuous_presenter, application_end_now);
        }
        std::scoped_lock gpu_lock(state->gpu_mutex);
        if (status) {
            status->gpu_ms = panel_gpu_milliseconds(*state, application_end_now);
        }
        state->fps_overlay->set_paused(state->pause_applied);
        state->fps_overlay->application_frame(end_info, status ? &*status : nullptr);
    }

    const auto submit_borrowed_to_presenter = [&]() -> XrResult {
        auto request = enqueue_presenter_submission(state, nullptr, end_info);
        if (presenter_content_lock.owns_lock()) {
            presenter_content_lock.unlock();
        }
        return wait_for_presenter_submission(state, request);
    };
    const auto start_requested_pipelined_presenter =
        [&](std::shared_ptr<GeneratedFrameEndInfo> seed_frame) -> XrResult {
            if (!pipelined_presenter_start_requested) {
                return XR_SUCCESS;
            }
            // frame_call_mutex is held here, so the wait flag and the frame
            // it names are stable; the presenter takes the frame over.
            std::optional<XrFrameState> adopted_frame_state;
            if (runtime_wait_outstanding &&
                state->last_inline_frame_state_valid) {
                adopted_frame_state = state->last_inline_frame_state;
            }
            if (!start_continuous_presenter(
                    state,
                    std::move(seed_frame),
                    true,
                    adopted_frame_state)) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            if (adopted_frame_state) {
                state->runtime_frame_waited_unbegun = false;
            }
            state->pipelined_presenter_start_requested = false;
            state->steamvr_presenter_start_requested = false;
            return wait_for_presenter_idle(state);
        };
    const auto bypass_generation =
        [&](GenerationPrepareReason reason) -> XrResult {
            // For the status panel's account of why nothing is generated.
            state->panel_bypass_reason = static_cast<std::int64_t>(reason);
            state->panel_bypass_at = application_end_now;
            const XrResult exclusive_result = enter_presenter_exclusive();
            if (XR_FAILED(exclusive_result)) {
                return exclusive_result;
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::generation_prepare,
                static_cast<std::int64_t>(reason),
                0,
                0,
                0);
            const auto end_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                handle_value(session),
                end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
                end_info ? end_info->layerCount : 0);
            if (!use_continuous_presenter) {
                join_runtime_queue_to_application(
                    state.get(), state->app_end_frame_release_value);
            }
            const XrResult end_result = use_continuous_presenter
                ? submit_borrowed_to_presenter()
                : with_runtime_entry(state, [&] {
                      return state->fps_overlay
                          ? state->fps_overlay->end_frame(end_info, false)
                          : state->dispatch->end_frame(session, end_info);
                  });
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                end_result,
                0,
                0,
                0);
            // On failure as well: the frame is over, and a wait left pending
            // by it would outlive every frame that could have matched it.
            if (end_info != nullptr) {
                consume_application_frame(
                    state,
                    end_info->displayTime,
                    consume_in_submission_order);
            }
            if (XR_SUCCEEDED(end_result) && !use_continuous_presenter) {
                const XrResult start_result =
                    start_requested_pipelined_presenter(nullptr);
                if (XR_FAILED(start_result)) {
                    return start_result;
                }
            }
            if (use_continuous_presenter && !pipelined_presenter_mode) {
                stop_continuous_presenter(state);
                if (state->pause_applied) {
                    state->presenter_restore_after_pause = true;
                }
                std::scoped_lock lock(state->mutex);
                state->steamvr_throttled_wait_streak = 0;
                // Both promotion routes start over, or the demotion would be
                // undone by the next frame that measures the same runtime.
                state->unpaced_wait_streak = 0;
            }
            return end_result;
        };

    bool generation_cooling_down = false;
    bool structural_quarantine_active = false;
    {
        std::scoped_lock lock(state->mutex);
        if (state->generation_resume_wall_time !=
            std::chrono::steady_clock::time_point{}) {
            if (application_end_now >= state->generation_resume_wall_time) {
                state->generation_resume_wall_time = {};
            } else {
                generation_cooling_down = true;
                structural_quarantine_active = true;
            }
        }
        if (state->generation_resume_display_time != 0) {
            if (end_info != nullptr &&
                end_info->displayTime >= state->generation_resume_display_time) {
                state->generation_resume_display_time = 0;
            } else {
                generation_cooling_down = true;
            }
        }
    }
    if (generation_cooling_down || manually_disarmed || !state->menu_enabled) {
        return bypass_generation(
            manually_disarmed
                ? GenerationPrepareReason::manual_disarmed
                : structural_quarantine_active
                    ? GenerationPrepareReason::structural_quarantine_active
                    : GenerationPrepareReason::cooldown_active);
    }

    ProjectionSnapshot current_snapshot{};
    const bool has_projection =
        capture_projection_snapshot(end_info, &current_snapshot);
    ProjectionMappingResult resource_mappings{};
    if (has_projection) {
        resource_mappings = build_projection_resource_mappings(current_snapshot);
        log_projection_view_rects(*state, current_snapshot);
        note_projection_eyes(current_snapshot, resource_mappings);
    } else {
        resource_mappings.reason = ProjectionMappingReason::no_projection_views;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::projection_mapping,
        static_cast<std::int64_t>(resource_mappings.reason),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        (static_cast<std::uint64_t>(current_snapshot.layers.size()) << 32) |
            resource_mappings.mappings.size(),
        resource_mappings.detail);
    // The first frame that names a swapchain as a projection view is where the
    // deferred generation resources are taken - but taking them here, between
    // the application's xrEndFrame and the submission that follows it, puts
    // the cost of creating them on the frame's own deadline. Synthesizer
    // initialization measured 80 ms and 46 ms in one captured session, which
    // is seven display periods: the submission left 92 ms after the
    // application entered xrEndFrame, the runtime rejected its display time
    // with XR_ERROR_TIME_INVALID, and the failure quarantined generation for a
    // second. Generation then engaged several seconds into the session instead
    // of immediately.
    //
    // So the frame that triggers arming passes through, and the arming happens
    // after its submission has gone out. It costs one frame at world load
    // rather than a rejected submission and a second of outage, and the next
    // frame generates.
    const std::span<const ProjectionResourceMapping> projection_mappings(
        resource_mappings.mappings.data(),
        resource_mappings.mappings.size());
    const bool generation_arming_pending =
        resource_mappings.ready() &&
        projection_frame_generation_pending(state, projection_mappings);
    if (!resource_mappings.ready() || generation_arming_pending) {
        clear_generation_continuity(state);
        if (!resource_mappings.ready() &&
            resource_mappings.reason !=
                ProjectionMappingReason::no_projection_views) {
            schedule_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_mapping_failed,
                (static_cast<std::uint64_t>(resource_mappings.reason) << 56) |
                    (resource_mappings.detail & 0x00FFFFFFFFFFFFFFULL));
        }
        const XrResult passthrough_result =
            bypass_generation(GenerationPrepareReason::empty_mappings);
        if (!generation_arming_pending) {
            return passthrough_result;
        }
        // The application's frame is already downstream; nothing this costs is
        // on its deadline any more.
        if (!ensure_projection_frame_generation(state, projection_mappings)) {
            // A refusal means the runtime has no swapchains left for the
            // application either. A queued submission names a private
            // swapchain, so the presenter must be idle and its content lock
            // held before one is destroyed.
            const XrResult exclusive_result = enter_presenter_exclusive();
            if (XR_FAILED(exclusive_result)) {
                return exclusive_result;
            }
            release_session_generation_budget(state);
        }
        return passthrough_result;
    }
    // An inline session whose next wait the runtime already holds: the inline
    // second cycle's own wait would block until the held frame is begun,
    // which the render thread can only do after this call returns. DCS World
    // deadlocked there with the presenter's first wait. A session that can
    // take a presenter passes the frame through - a promotion pending at this
    // end still happens, and the presenter begins the held frame. One that
    // never takes a presenter (presenter_forbidden) generates anyway: its
    // cycle adopts the held frame (submit_current_cycle), because for DCS's
    // shape this is every frame, and passing them all through meant no
    // generation at all. After arming, so a session that starts this way
    // still arms on its first frame.
    const bool adopt_outstanding_wait = !use_continuous_presenter &&
        runtime_wait_outstanding && presenter_forbidden(*state) &&
        state->last_inline_frame_state_valid;
    if (!use_continuous_presenter && runtime_wait_outstanding &&
        !adopt_outstanding_wait) {
        clear_generation_continuity(state);
        return bypass_generation(
            GenerationPrepareReason::inline_wait_outstanding);
    }
    const std::optional<XrDuration> application_display_period =
        latest_pending_application_period(
            state,
            current_snapshot.display_time,
            consume_in_submission_order);
    const bool latest_application_frame = application_display_period.has_value();
    std::optional<ProjectionSnapshot> previous_snapshot;
    {
        std::scoped_lock lock(state->mutex);
        previous_snapshot = state->previous_projection;
    }
    const bool snapshots_compatible =
        !previous_snapshot ||
        projection_snapshots_compatible(
            *previous_snapshot,
            current_snapshot,
            !state->deep_pipeline);
    const bool metadata_pairable =
        latest_application_frame && previous_snapshot && snapshots_compatible;
    if (previous_snapshot && !snapshots_compatible) {
        if (!projection_resource_layout_compatible(
                *previous_snapshot,
                current_snapshot)) {
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_changed,
                static_cast<std::uint64_t>(current_snapshot.display_time));
            return bypass_generation(
                GenerationPrepareReason::structural_quarantine_active);
        }
        // Recenter/reference-space and layer-flag changes are safe to prime
        // immediately; do not turn an ordinary pose-space transition into a
        // one-second outage.
        clear_generation_continuity(state);
    }
    // Admit this frame once the presenter has retired the previous pair's
    // synthetic, leaving only its current submission un-retired.
    //
    // Waiting for the queue to empty instead spent half the available budget:
    // the application was released only after the second of two paced
    // submissions and still had to enqueue before the very next slot, so it
    // had one display period to render a frame that two periods were available
    // for. Admitting it a slot earlier is what the alternating current
    // swapchain exists to make safe -- the previous pair's un-retired current
    // frame names the other slot, and its synthetic has been retired, so
    // neither can be repointed by the release below.
    //
    // The deeper pipeline admits one notch later in the count, because the
    // presenter holds each synthetic a display period longer: at the shallow
    // bound the application would be kept waiting for a slot the pipeline is
    // holding on purpose. That needs the synthetic ring for the same reason
    // the current one exists -- the previous pair's synthetic is no longer
    // retired by the time we release into it.
    if (use_continuous_presenter) {
        const auto admission_entered = std::chrono::steady_clock::now();
        const XrResult capacity_result = wait_for_presenter_capacity(
            state, state->deep_pipeline ? 2 : 1);
        if (XR_FAILED(capacity_result)) {
            return capacity_result;
        }
        if (!presenter_hold_at_admission(*state) && metadata_pairable) {
            observe_admission_wait(
                *state, std::chrono::steady_clock::now() - admission_entered);
        }
        if (presenter_hold_at_admission(*state)) {
            // The once-per-pair hold, ahead of synthesis rather than after the
            // hand-over: see presenter_hold_at_admission for why the placement
            // matters. A frame that will pair is held for the pair; one that
            // will not - a prime, after a reset - for a single presenter
            // frame, which is what keeps a reset from bursting (see
            // wait_for_presenter_pair). It returns without an error on a
            // presenter failure; the enqueue below is what refuses the frame.
            wait_for_presenter_pair(
                state,
                metadata_pairable ? state->frames_per_application_frame.load() : 1U);
        }
    }
    const std::uint32_t frames_per_frame = state->frames_per_application_frame;
    const bool native_dlss = state->nvidia_options.frame_generation == xrfg::D3D12FrameGeneration::native_dlss;
    // Extrapolating, the real frame goes first: the synthetic is shown last,
    // frames - 1 periods after it, and a 3X second synthetic one period
    // after it.
    const bool extrapolating = !native_dlss && state->nvidia_options.extrapolate;
    const float game_step = extrapolating
        ? game_step_display_periods(
              *state,
              std::span<const ProjectionResourceMapping>(
                  resource_mappings.mappings.data(), resource_mappings.mappings.size()))
        : 0.0F;
    const float interpolation_fraction = extrapolating
        ? synthetic_extrapolation_fraction(frames_per_frame, frames_per_frame - 1, game_step)
        : synthetic_interpolation_fraction(frames_per_frame, 0);
    if (previous_snapshot && metadata_pairable && !native_dlss) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::synthesis_fraction,
            static_cast<std::int64_t>(interpolation_fraction * 10000.0F + 0.5F),
            static_cast<std::uint64_t>(std::max<XrTime>(
                (current_snapshot.display_time - previous_snapshot->display_time) / 1000,
                0)),
            frames_per_frame,
            static_cast<std::uint64_t>(current_snapshot.display_time));
    }
    // 3X: the second synthetic, a period after the first and a period before
    // the real frame.
    const std::optional<float> extra_interpolation_fraction =
        frames_per_frame > 2
            ? std::optional<float>(extrapolating
                  ? synthetic_extrapolation_fraction(frames_per_frame, 1, game_step)
                  : synthetic_interpolation_fraction(frames_per_frame, 1))
            : std::nullopt;
    // Each synthetic's own camera, at the share of the span its content is
    // placed at (synthetic_camera_snapshot); none with `[ofxr]
    // synthetic_pose=real`, or for a turn no head makes in a frame.
    std::optional<ProjectionSnapshot> synthetic_camera;
    std::optional<ProjectionSnapshot> extra_synthetic_camera;
    if (previous_snapshot && metadata_pairable && state->synthetic_pose_interpolated) {
        synthetic_camera = synthetic_camera_snapshot(
            *previous_snapshot, current_snapshot,
            xrfg::usable_synthesis_fraction(interpolation_fraction, extrapolating));
        if (synthetic_camera && extra_interpolation_fraction) {
            extra_synthetic_camera = synthetic_camera_snapshot(
                *previous_snapshot, current_snapshot,
                xrfg::usable_synthesis_fraction(*extra_interpolation_fraction, extrapolating));
            if (!extra_synthetic_camera) {
                synthetic_camera.reset();
            }
        }
    }
    PreparedProjectionFrame prepared = prepare_projection_frame(
        current_snapshot,
        std::span<const ProjectionResourceMapping>(
            resource_mappings.mappings.data(),
            resource_mappings.mappings.size()),
        metadata_pairable,
        interpolation_fraction,
        extra_interpolation_fraction,
        synthetic_camera ? &*synthetic_camera : nullptr,
        extra_synthetic_camera ? &*extra_synthetic_camera : nullptr,
        // Released at the hand-over where the runtime has the layer's own
        // queue, in either pipeline: see the note on the release in
        // prepare_frame_generation. The shallow pipeline admits the next
        // pair while this one's real frame is still to be handed over, so
        // joined here that pair's synthesis wait would sit in front of it.
        use_continuous_presenter && state->binding_queue != nullptr);
    // Collected before anything below can reset `prepared`, so every image
    // left acquired is accounted for on every path out of this call.
    PrivateReleaseBatch synthetic_releases;
    PrivateReleaseBatch extra_releases;
    PrivateReleaseBatch current_releases;
    synthetic_releases.session = state.get();
    extra_releases.session = state.get();
    current_releases.session = state.get();
    for (const PreparedProjectionResource& resource : prepared.resources) {
        const PreparedGeneration& deferred = resource.generation;
        if (!deferred.deferred_generation) {
            continue;
        }
        if (deferred.deferred_synthetic != nullptr) {
            synthetic_releases.releases.push_back({
                deferred.deferred_generation,
                deferred.deferred_synthetic,
                deferred.deferred_ticket,
                deferred.deferred_synthetic_staging,
                &deferred.deferred_generation->synthetic_runtime_images,
            });
        }
        if (deferred.deferred_extra_synthetic != nullptr) {
            extra_releases.releases.push_back({
                deferred.deferred_generation,
                deferred.deferred_extra_synthetic,
                deferred.deferred_ticket,
                deferred.deferred_extra_staging,
                &deferred.deferred_generation->synthetic_runtime_images,
            });
        }
        if (deferred.deferred_current != nullptr) {
            current_releases.releases.push_back({
                deferred.deferred_generation,
                deferred.deferred_current,
                deferred.deferred_ticket,
                deferred.deferred_current_staging,
                &deferred.deferred_generation->current_runtime_images,
            });
        }
    }
    // Acquire, synthesis and release all ran without the content lock, so the
    // presenter kept submitting throughout. Hold it only across the handoff.
    if (use_continuous_presenter) {
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
    }
    GenerationPrepareReason prepare_reason = prepared.reason;
    XrSwapchain failed_prepare_swapchain = prepared.failed_swapchain;
    if (prepared.kind == PreparedGenerationKind::none &&
        should_quarantine_generation_failure(prepare_reason)) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::generation_prepare,
            static_cast<std::int64_t>(prepare_reason),
            handle_value(failed_prepare_swapchain),
            static_cast<std::uint64_t>(prepared.kind),
            (metadata_pairable ? 1u : 0u) |
                (latest_application_frame ? 2u : 0u));
        schedule_generation_quarantine(
            state,
            GenerationQuarantineReason::generation_prepare_failed,
            (static_cast<std::uint64_t>(prepare_reason) << 56) |
                (handle_value(failed_prepare_swapchain) &
                 0x00FFFFFFFFFFFFFFULL));
        return bypass_generation(
            GenerationPrepareReason::structural_quarantine_active);
    }

    GeneratedFrameEndInfo first_generated{};
    GeneratedFrameEndInfo extra_generated{};
    GeneratedFrameEndInfo current_generated{};
    const XrFrameEndInfo* submitted_end_info = end_info;
    bool pair_ready = false;
    // The frame has a second synthetic: every resource made one.
    bool extra_ready = !prepared.resources.empty();
    std::vector<ProjectionResourceDestination> current_destinations;
    std::vector<ProjectionResourceDestination> synthetic_destinations;
    std::vector<ProjectionResourceDestination> extra_destinations;
    current_destinations.reserve(prepared.resources.size());
    synthetic_destinations.reserve(prepared.resources.size());
    extra_destinations.reserve(prepared.resources.size());
    for (const PreparedProjectionResource& resource : prepared.resources) {
        extra_ready = extra_ready &&
            resource.generation.extra_synthetic_handle != XR_NULL_HANDLE;
        // A synthetic generated in its own camera is submitted with it.
        const bool synthetic_camera_used = prepared.synthetic_cameras &&
            resource.generation.synthetics_in_target_camera;
        extra_destinations.push_back({
            resource.application_swapchain,
            resource.generation.extra_synthetic_handle,
            synthetic_camera_used,
        });
        current_destinations.push_back({
            resource.application_swapchain,
            resource.generation.current_handle,
        });
        synthetic_destinations.push_back({
            resource.application_swapchain,
            resource.generation.synthetic_handle,
            synthetic_camera_used,
        });
    }
    if (prepared.kind == PreparedGenerationKind::prime) {
        if (build_generated_frame_end_info(
                end_info,
                current_snapshot,
                false,
                std::span<const ProjectionResourceDestination>(
                    current_destinations.data(), current_destinations.size()),
                &first_generated)) {
            submitted_end_info = &first_generated.info;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (prepared.kind == PreparedGenerationKind::pair && previous_snapshot) {
        const bool synthetic_built = build_generated_frame_end_info(
            end_info,
            current_snapshot,
            true,
            std::span<const ProjectionResourceDestination>(
                synthetic_destinations.data(), synthetic_destinations.size()),
            &first_generated,
            synthetic_camera ? &*synthetic_camera : nullptr);
        const bool current_built = build_generated_frame_end_info(
            end_info,
            current_snapshot,
            false,
            std::span<const ProjectionResourceDestination>(
                current_destinations.data(), current_destinations.size()),
            &current_generated);
        extra_ready = extra_ready && build_generated_frame_end_info(
            end_info,
            current_snapshot,
            true,
            std::span<const ProjectionResourceDestination>(
                extra_destinations.data(), extra_destinations.size()),
            &extra_generated,
            extra_synthetic_camera ? &*extra_synthetic_camera : nullptr);
        if (synthetic_built && current_built) {
            // The synthetic frame owns the deferred copies: they must reach
            // the queue after it has been handed to the runtime.
            for (const PreparedProjectionResource& resource :
                 prepared.resources) {
                if (resource.generation.synthesizer) {
                    first_generated.pending_current_copies.push_back(
                        {resource.generation.synthesizer,
                         resource.generation.copy_fence_value});
                }
                if (resource.generation.ready_fence) {
                    first_generated.synthetic_ready.push_back(
                        {resource.generation.ready_fence,
                         resource.generation.ready_value});
                }
            }
            if (extrapolating) {
                // The real frame goes down first, at the application's own
                // display time, and the synthetic after it, where the
                // interpolating order puts the real frame. Everything below
                // hands over first_generated first, so the two change
                // places, the deferred copies and readiness going with the
                // first. A 3X second synthetic then goes between them, which
                // is why its fraction is the nearer one.
                std::swap(first_generated, current_generated);
                std::swap(first_generated.pending_current_copies,
                          current_generated.pending_current_copies);
                std::swap(first_generated.synthetic_ready,
                          current_generated.synthetic_ready);
                std::swap(synthetic_releases.releases, current_releases.releases);
            }
            submitted_end_info = &first_generated.info;
            pair_ready = true;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (!prepared.anchor_is_current) {
        clear_generation_continuity(state);
    }

    std::shared_ptr<GeneratedFrameEndInfo> presenter_first_frame;
    first_generated.synthetic = pair_ready && !extrapolating;
    first_generated.leads_frame = pair_ready;
    if (pair_ready && extrapolating) {
        current_generated.synthetic = true;
    }
    extra_ready = extra_ready && pair_ready;
    extra_generated.synthetic = extra_ready;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_extra_frame;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_current_frame;
    if (use_continuous_presenter &&
        (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        presenter_first_frame =
            make_presenter_owned_frame(std::move(first_generated));
        if (pair_ready) {
            presenter_current_frame =
                make_presenter_owned_frame(std::move(current_generated));
        }
        if (extra_ready) {
            presenter_extra_frame =
                make_presenter_owned_frame(std::move(extra_generated));
        }
        if (!presenter_first_frame ||
            (pair_ready && !presenter_current_frame) ||
            (extra_ready && !presenter_extra_frame)) {
            presenter_first_frame.reset();
            presenter_extra_frame.reset();
            presenter_current_frame.reset();
            extra_ready = false;
            submitted_end_info = end_info;
            pair_ready = false;
            prepare_reason =
                GenerationPrepareReason::presenter_unsafe_composition;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::presenter_composition_failed);
            prepared = {};
        } else {
            submitted_end_info = &presenter_first_frame->info;
            // Handed to the frames before they are queued, so the presenter
            // can never see a frame without them. A prime is the current
            // frame alone.
            if (pair_ready) {
                presenter_first_frame->pending_releases =
                    synthetic_releases.take();
                if (presenter_extra_frame) {
                    presenter_extra_frame->pending_releases =
                        extra_releases.take();
                }
                presenter_current_frame->pending_releases =
                    current_releases.take();
            } else {
                presenter_first_frame->pending_releases =
                    current_releases.take();
            }
        }
    } else if (!use_continuous_presenter &&
               (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        // The inline path hands over itself, immediately before each of its
        // downstream calls below.
        if (pair_ready) {
            first_generated.pending_releases = synthetic_releases.take();
            if (extra_ready) {
                extra_generated.pending_releases = extra_releases.take();
            }
            current_generated.pending_releases = current_releases.take();
        } else {
            first_generated.pending_releases = current_releases.take();
        }
    }

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::generation_prepare,
        static_cast<std::int64_t>(prepare_reason),
        handle_value(failed_prepare_swapchain),
        static_cast<std::uint64_t>(prepared.kind),
        (metadata_pairable ? 1u : 0u) |
            (latest_application_frame ? 2u : 0u));

    const auto first_end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        handle_value(session),
        submitted_end_info
            ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
            : 0,
        submitted_end_info ? submitted_end_info->layerCount : 0);
    const auto first_end_started = std::chrono::steady_clock::now();
    XrResult result = XR_SUCCESS;
    if (use_continuous_presenter) {
        if (pair_ready) {
            result = enqueue_presenter_pair(
                state,
                presenter_first_frame,
                presenter_extra_frame,
                presenter_current_frame);
            if (XR_FAILED(result)) {
                // Refused before anything was queued, so nothing else will
                // ever release these.
                discard_private_releases(
                    state.get(), presenter_first_frame->pending_releases);
                if (presenter_extra_frame) {
                    discard_private_releases(
                        state.get(), presenter_extra_frame->pending_releases);
                }
                discard_private_releases(
                    state.get(), presenter_current_frame->pending_releases);
            }
            if (presenter_content_lock.owns_lock()) {
                presenter_content_lock.unlock();
            }
            if (XR_SUCCEEDED(result) && pipelined_presenter_mode) {
                // Let a whole pair stay outstanding. The presenter paces its
                // submissions a display period apart, so any smaller bound
                // puts one of those periods on the application's critical
                // path: waiting for the queue to empty cost both periods and
                // held it to 36/s, waiting for one cost a single period and
                // held it to 34.6/s, with 11.93 ms of every 28.9 ms frame
                // spent blocked here. What should govern the application is
                // its own virtual wait, one pair per two display periods, and
                // that only gets to act once this stops pre-empting it.
                //
                // The pair enqueued here is drained by the time the next
                // one arrives, so this bound does not throttle the steady
                // state -- but it is what releases the application to render
                // its next frame, so it has to move with the admission gate
                // above. Left behind, it waits for two retirements instead of
                // one and hands back the whole period that gate just bought,
                // while the rate stays at two retirements per application
                // frame and the log still reads a clean 45/s.
                // With 3X the whole frame is three submissions.
                result = wait_for_presenter_capacity(
                    state,
                    state->deep_pipeline
                        ? 3
                        : state->frames_per_application_frame.load());
            }
        } else if (presenter_first_frame) {
            auto request = enqueue_presenter_submission(
                state,
                presenter_first_frame,
                nullptr);
            if (!request->owned_frame) {
                // Refused: the presenter never saw this frame.
                discard_private_releases(
                    state.get(), presenter_first_frame->pending_releases);
            }
            if (presenter_content_lock.owns_lock()) {
                presenter_content_lock.unlock();
            }
            if (state->deep_pipeline) {
                // Enqueue and return, exactly as a pair does. Waiting here
                // blocks the application until the presenter has submitted
                // this frame, which behind a deeper queue is most of the
                // queue: the application must never wait on presentation.
                // Nothing needs the wait. The frame is owned rather than
                // borrowed, so the application's end-frame data does not
                // have to outlive the call - that is what the borrowed path
                // below waits for - and a downstream failure latches in the
                // presenter and fails the next frame call, the same way a
                // pair's does. A refusal is the only result worth returning
                // now, and it is final before the request ever leaves here.
                result = request->owned_frame ? XR_SUCCESS : request->result;
            } else {
                result = wait_for_presenter_submission(state, request);
            }
        } else {
            result = submit_borrowed_to_presenter();
        }
    } else {
        join_runtime_queue_to_application(
            state.get(), state->app_end_frame_release_value);
        if (!run_private_releases(state.get(), first_generated.pending_releases)) {
            // The runtime refused a private image at the hand-over, so the
            // frame naming it cannot go down as generated: the application's
            // own frame goes instead, the pair with it is dropped, and the
            // deferred copies are still flushed so the synthesizer's state
            // stays in step. The ring path meets the same refusal at prepare
            // (private_release_failed) and passes through the same way.
            discard_private_releases(state.get(), extra_generated.pending_releases);
            discard_private_releases(state.get(), current_generated.pending_releases);
            for (const auto& pending : first_generated.pending_current_copies) {
                if (pending.synthesizer) {
                    static_cast<void>(pending.synthesizer->flush_current_copy(
                        state->d3d12_synthesis_queue ? runtime_queue(*state) : nullptr,
                        pending.fence_value));
                }
            }
            // Continuity is kept, as the ring path keeps it: the capture
            // this pair was made from is still the next pair's previous
            // frame, and resetting it would invalidate that capture.
            submitted_end_info = end_info;
            pair_ready = false;
            extra_ready = false;
            prepare_reason = GenerationPrepareReason::private_release_failed;
        }
        if (submitted_end_info != nullptr) {
            // Extrapolating, the first frame down is the real one.
            log_reprojection_angle(*state, *submitted_end_info,
                submitted_end_info == &first_generated.info && first_generated.synthetic
                    ? 2U : 1U);
        }
        result = with_runtime_entry(state, [&] {
            return state->fps_overlay
                ? state->fps_overlay->end_frame(submitted_end_info, pair_ready)
                : state->dispatch->end_frame(session, submitted_end_info);
        });
    }
    const auto first_end_elapsed =
        std::chrono::steady_clock::now() - first_end_started;
    xrfg::bridge_flight_logger().end(
        first_end_token,
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        result,
        static_cast<std::uint64_t>(prepared.kind),
        pair_ready ? 1u : 0u,
        latest_application_frame ? 1u : 0u);
    if (XR_FAILED(result)) {
        // A rejected display time says nothing about the generation
        // resources. No swapchain changed shape, no private image is in an
        // uncertain ownership phase - the runtime was handed a time it
        // considers past, which is the application's own: MSFS 2024 submits a
        // display time older than the previous frame's after a hitch, 3 times
        // in 2350 frames in one capture. Clear continuity so the next frame
        // primes, and leave the quarantine for failures that really do mean
        // the resources are no longer safe to use. Quarantining cost a full
        // second of generation for each of those three frames.
        //
        // The frame is over either way, so its wait is consumed as it is on
        // success. Left pending, it stayed in the queue for the rest of the
        // session, and for a title whose labels never match a wait that
        // meant no frame paired again.
        consume_application_frame(
            state,
            current_snapshot.display_time,
            consume_in_submission_order);
        if (result == XR_ERROR_TIME_INVALID) {
            clear_generation_continuity(state);
            return result;
        }
        enter_generation_quarantine(
            state,
            GenerationQuarantineReason::downstream_end_failed,
            static_cast<std::uint32_t>(result));
        return result;
    }
    if (pair_ready) {
        state->generation_steady_state_established = true;
    }
    if (use_continuous_presenter && !pipelined_presenter_mode) {
        // A frame the layer could not generate from is already handled: it was
        // passed through unchanged above. Demoting the presenter for it buys
        // nothing and costs a great deal, because the promotion that follows
        // is automatic - three throttled waits later the presenter is back.
        //
        // Cyberpunk 2077 through the Luke Ross mod fails one frame in fourteen
        // on the D3D11 interop path, and every one of those failures was
        // isolated: 175 failures in 59 s, 380 in 66 s, never two in a row. So
        // the presenter was stopped and restarted three times a second, 524
        // transitions in one session, each teardown submitting a frame with
        // nothing in it - black until the shutdown frame repeated the last
        // composition, and a stale pose under a newer image afterwards.
        // Neither artifact is worth having, and both exist only because the
        // presenter is being torn down mid-stride.
        //
        // Demote on a run of failures instead, which is what "generation is
        // not working here" actually looks like, using the same count of three
        // the promotion uses in the other direction.
        constexpr std::uint32_t kGenerationFailureDemotionStreak = 3;
        bool demote = false;
        {
            std::scoped_lock lock(state->mutex);
            if (presenter_first_frame) {
                state->generation_failure_streak = 0;
            } else if (++state->generation_failure_streak >=
                       kGenerationFailureDemotionStreak) {
                demote = true;
            }
        }
        if (demote) {
            // Not under state->mutex: stopping the presenter joins its thread.
            stop_continuous_presenter(state);
            std::scoped_lock lock(state->mutex);
            state->steamvr_throttled_wait_streak = 0;
            state->unpaced_wait_streak = 0;
            state->generation_failure_streak = 0;
        }
    }

    consume_application_frame(
        state,
        current_snapshot.display_time,
        consume_in_submission_order);
    if (!use_continuous_presenter && pair_ready && application_display_period &&
        !pipelined_presenter_mode &&
        xrfg::should_enter_generation_cooldown(
            state->optical_flow_backend ==
                xrfg::D3D12OpticalFlowBackend::nvidia,
            first_end_elapsed,
            *application_display_period)) {
        {
            std::scoped_lock lock(state->mutex);
            state->generation_resume_display_time =
                generation_resume_time(current_snapshot.display_time);
        }
        clear_generation_continuity(state);
        return result;
    }
    if (prepared.anchor_is_current) {
        std::scoped_lock lock(state->mutex);
        state->previous_projection = current_snapshot;
    } else {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
    }

    if (!use_continuous_presenter &&
        pipelined_presenter_start_requested) {
        std::shared_ptr<GeneratedFrameEndInfo> seed_frame;
        if (pair_ready) {
            seed_frame = make_presenter_owned_frame(
                std::move(current_generated));
        } else if (prepared.kind == PreparedGenerationKind::prime) {
            seed_frame = make_presenter_owned_frame(
                std::move(first_generated));
        }
        const XrResult start_result =
            start_requested_pipelined_presenter(std::move(seed_frame));
        return XR_FAILED(start_result) ? start_result : result;
    }

    const bool presenter_prime = use_continuous_presenter &&
        prepared.kind == PreparedGenerationKind::prime;
    if (!pair_ready && !presenter_prime) {
        return result;
    }

    if (use_continuous_presenter) {
        // The once-per-pair hold. The layer submits one pair per application
        // frame and a pair costs two presenter frames, so without this an
        // application that renders faster than half the display rate produces
        // frames the pairing has no room for - they lose the history ring's
        // capture slot and are rendered and thrown away. A prime is held for
        // one presenter frame, so that a reset cannot burst; see
        // wait_for_presenter_pair.
        //
        // It sits here rather than in the virtual wait for two reasons. The
        // frame is already handed over, so the presenter has composition to
        // work with and its count keeps advancing - gating the wait instead
        // deadlocked on the first frame, before anything had been enqueued.
        // And the application is released at the top of its next period rather
        // than the end of this one, so it renders straight away and synthesis
        // is queued with most of a period still in front of it.
        //
        // The frame lock goes first: an application whose wait runs on another
        // thread must not be shut out of xrWaitFrame while this waits.
        //
        // Unless this frame was already held at admission.
        // Plus a frame, once, to move the application's phase: see
        // observe_admission_wait.
        const std::uint32_t extra_frames =
            std::exchange(state->pair_hold_extra_frames, 0U);
        frame_call_lock.unlock();
        if (!presenter_hold_at_admission(*state)) {
            wait_for_presenter_pair(
                state,
                (pair_ready ? state->frames_per_application_frame.load() : 1U) +
                    extra_frames);
        }
        return result;
    }

    // The synthetic has gone downstream; submit its current copy before the
    // cycle that hands the runtime the frame which reads it.
    for (const auto& pending : first_generated.pending_current_copies) {
        if (pending.synthesizer) {
            static_cast<void>(pending.synthesizer->flush_current_copy(
                state->d3d12_synthesis_queue
                    ? runtime_queue(*state)
                    : nullptr,
                pending.fence_value));
        }
    }
    // 3X: the second synthetic takes a cycle of its own first, and the
    // runtime's held wait with it where there is one.
    bool extra_cycle_completed = true;
    if (extra_ready) {
        run_private_releases(state.get(), extra_generated.pending_releases);
        extra_cycle_completed = submit_current_cycle(
            state,
            extra_generated.info,
            adopt_outstanding_wait
                ? std::optional<XrFrameState>(state->last_inline_frame_state)
                : std::nullopt,
            true).completed;
    }
    const auto current_cycle_started = std::chrono::steady_clock::now();
    run_private_releases(state.get(), current_generated.pending_releases);
    const InternalCycleResult current_cycle = submit_current_cycle(
        state,
        current_generated.info,
        adopt_outstanding_wait && !extra_ready
            ? std::optional<XrFrameState>(state->last_inline_frame_state)
            : std::nullopt,
        false,
        current_generated.synthetic);
    if (adopt_outstanding_wait) {
        // frame_call_mutex is held on the inline path, so these are seen in
        // order by the application's next begin.
        state->runtime_frame_waited_unbegun = false;
        state->application_begin_needs_wait = true;
    }
    // How far apart the runtime actually received the two frames of this
    // pair: from the synthetic's hand-over completing to the real frame's.
    // With a second synthetic, from that one's hand-over.
    const auto inline_pair_gap = std::chrono::steady_clock::now() -
        (extra_ready ? current_cycle_started
                     : first_end_started + first_end_elapsed);
    if (!current_cycle.completed || !extra_cycle_completed) {
        // The synthetic submission has already completed. A transient runtime
        // failure in the optional second cycle is recovered by the established
        // continuity reset; treating it as a structural resize would retain a
        // private image in an uncertain ownership phase for the whole timeout.
        clear_generation_continuity(state);
    } else if (!presenter_forbidden(*state) &&
               (steamvr_wait_requires_continuous_presenter(
                    state,
                    current_cycle) ||
                runtime_wait_lacks_pacing(state) ||
                (!state->dispatch->inline_unless_pipelined &&
                 inline_pair_lands_in_one_scanout(state, inline_pair_gap)) ||
                frame_loop_takes_presenter(state))) {
        // Request the promotion; do not perform it here. submit_current_cycle
        // has already submitted this frame, so seeding a freshly started
        // presenter thread with it handed a second owner to composition layers
        // and private swapchain leases that this frame still holds. The next
        // xrEndFrame starts the presenter through the same path the pipelined
        // promotion uses, before the inline cycle runs and with a frame nothing
        // else has submitted.
        state->steamvr_presenter_start_requested = true;
    }
    return XR_FAILED(current_cycle.result) ? current_cycle.result : result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    return guard_c_api_boundary([&] {
        return layer_get_instance_proc_addr_impl(instance, name, function);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::instance_create);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_create_api_layer_instance_impl(create_info, layer_info, instance);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::instance_create,
        result,
        instance && XR_SUCCEEDED(result) ? handle_value(*instance) : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::instance_destroy,
        handle_value(instance));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_destroy_instance_impl(instance);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::instance_destroy,
        result,
        handle_value(instance));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_create,
        handle_value(instance));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_create_session_impl(instance, create_info, session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_create,
        result,
        handle_value(instance),
        session && XR_SUCCEEDED(result) ? handle_value(*session) : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_destroy,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_destroy_session_impl(session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_destroy,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_begin_session(
    XrSession session,
    const XrSessionBeginInfo* begin_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_begin,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_session_impl(session, begin_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_begin,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_end_session(XrSession session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_end,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_session_impl(session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_end,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_wait_frame(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_wait_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_frame_impl(session, wait_info, frame_state);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_wait_frame,
        result,
        frame_state && XR_SUCCEEDED(result)
            ? static_cast<std::uint64_t>(frame_state->predictedDisplayTime)
            : 0,
        frame_state && XR_SUCCEEDED(result)
            ? static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod)
            : 0,
        frame_state && XR_SUCCEEDED(result) ? frame_state->shouldRender : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_begin_frame(
    XrSession session,
    const XrFrameBeginInfo* begin_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_begin_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_frame_impl(session, begin_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_begin_frame,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_swapchain(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    return guard_c_api_boundary([&] {
        return layer_create_swapchain_impl(session, create_info, swapchain);
    });
}

// A composition layer names a space, and at any pipeline depth above zero a
// submission naming one can still be queued when the application destroys it.
// The runtime then rejects that submission, presenter_failure latches, and
// every later frame call fails for the life of the session - seen as the
// session freezing on recentre, which is when MSFS rebuilds its reference
// space. Settle the presenter first so nothing queued names this handle.
//
// Upstream never needed this because the application waited for the queue to
// empty inside xrEndFrame; it does not any more.
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space) {
    return guard_c_api_boundary([&]() -> XrResult {
        std::shared_ptr<Dispatch> dispatch;
        std::vector<std::shared_ptr<SessionState>> sessions;
        {
            std::scoped_lock lock(g_state_mutex);
            for (const auto& [handle, session] : g_sessions) {
                static_cast<void>(handle);
                if (session) {
                    sessions.push_back(session);
                    dispatch = session->dispatch;
                }
            }
        }
        if (!dispatch || dispatch->destroy_space == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
            return handle_value(a->handle) < handle_value(b->handle);
        });
        std::vector<PresenterResourceLifetimeGuard> guards;
        guards.reserve(sessions.size());
        for (const auto& session : sessions) {
            guards.emplace_back(session);
        }
        return dispatch->destroy_space(space);
    });
}

// The runtime's own entry point, for a session the layer keeps nothing for -
// which should not happen: the call then goes on unchanged rather than
// failing the application.
template <typename Function>
[[nodiscard]] Function panel_gesture_next(Function Dispatch::PanelGesture::*member) {
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, dispatch] : g_instances) {
        static_cast<void>(handle);
        if (dispatch && dispatch->panel_gesture.*member != nullptr) {
            return dispatch->panel_gesture.*member;
        }
    }
    return nullptr;
}

// The status panel's grip bindings, beside the application's own for every
// interaction profile it suggests (see create_panel_gesture). A profile with
// no grip - a gamepad, eye gaze - refuses them, and then the application's
// call is made again exactly as it made it, so its own bindings never depend
// on the layer's being accepted.
XRAPI_ATTR XrResult XRAPI_CALL layer_suggest_interaction_profile_bindings(
    XrInstance instance,
    const XrInteractionProfileSuggestedBinding* suggested_bindings) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->panel_gesture.suggest_bindings == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        const auto& gesture = dispatch->panel_gesture;
        // A call the runtime should refuse is passed on as it came, so the
        // layer's additions never make an invalid one valid.
        if (suggested_bindings == nullptr ||
            suggested_bindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING ||
            suggested_bindings->countSuggestedBindings == 0 ||
            suggested_bindings->suggestedBindings == nullptr) {
            return gesture.suggest_bindings(instance, suggested_bindings);
        }
        std::vector<XrActionSuggestedBinding> bindings(
            suggested_bindings->suggestedBindings,
            suggested_bindings->suggestedBindings + suggested_bindings->countSuggestedBindings);
        bindings.push_back({gesture.grip_action, gesture.grips[0]});
        bindings.push_back({gesture.grip_action, gesture.grips[1]});
        XrInteractionProfileSuggestedBinding merged = *suggested_bindings;
        merged.suggestedBindings = bindings.data();
        merged.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        XrResult result = gesture.suggest_bindings(instance, &merged);
        const bool added = XR_SUCCEEDED(result);
        if (!added) {
            result = gesture.suggest_bindings(instance, suggested_bindings);
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::status_panel, 2, added ? 1 : 0,
            static_cast<std::uint64_t>(suggested_bindings->interactionProfile),
            static_cast<std::uint64_t>(static_cast<std::int64_t>(result)));
        return result;
    });
}

// The layer's action set beside the application's. Taken, it brings the two
// grip spaces the panel's gesture reads; refused, the application's sets are
// attached as it asked, and the session simply has no gesture.
XRAPI_ATTR XrResult XRAPI_CALL layer_attach_session_action_sets(
    XrSession session,
    const XrSessionActionSetsAttachInfo* attach_info) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->panel_gesture.attach_action_sets == nullptr) {
            const auto next = panel_gesture_next(&Dispatch::PanelGesture::attach_action_sets);
            return next ? next(session, attach_info) : XR_ERROR_HANDLE_INVALID;
        }
        const auto& gesture = state->dispatch->panel_gesture;
        if (attach_info == nullptr ||
            attach_info->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO ||
            attach_info->countActionSets == 0 || attach_info->actionSets == nullptr) {
            return gesture.attach_action_sets(session, attach_info);
        }
        std::vector<XrActionSet> action_sets(
            attach_info->actionSets, attach_info->actionSets + attach_info->countActionSets);
        action_sets.push_back(gesture.action_set);
        XrSessionActionSetsAttachInfo merged = *attach_info;
        merged.actionSets = action_sets.data();
        merged.countActionSets = static_cast<std::uint32_t>(action_sets.size());
        XrResult result = gesture.attach_action_sets(session, &merged);
        if (XR_FAILED(result)) {
            result = gesture.attach_action_sets(session, attach_info);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::status_panel, 3, 0, 0,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(result)));
            return result;
        }
        std::array<XrSpace, 2> grips{XR_NULL_HANDLE, XR_NULL_HANDLE};
        std::uint64_t made = 0;
        for (std::size_t hand = 0; hand < grips.size(); ++hand) {
            XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space_info.action = gesture.grip_action;
            space_info.subactionPath = gesture.hands[hand];
            space_info.poseInActionSpace.orientation.w = 1.0F;
            if (XR_SUCCEEDED(gesture.create_action_space(session, &space_info, &grips[hand]))) {
                ++made;
            } else {
                grips[hand] = XR_NULL_HANDLE;
            }
        }
        state->panel_grip_spaces = grips;
        state->panel_actions_attached.store(true, std::memory_order_release);
        if (state->fps_overlay) {
            state->fps_overlay->set_grip_spaces(grips[0], grips[1]);
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::status_panel, 3, 1, made);
        return result;
    });
}

// The layer's action set is synced with the application's, once it was
// attached. Should the runtime refuse the addition, the application's sync is
// made again as it asked.
XRAPI_ATTR XrResult XRAPI_CALL layer_sync_actions(
    XrSession session,
    const XrActionsSyncInfo* sync_info) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->panel_gesture.sync_actions == nullptr) {
            const auto next = panel_gesture_next(&Dispatch::PanelGesture::sync_actions);
            return next ? next(session, sync_info) : XR_ERROR_HANDLE_INVALID;
        }
        const auto& gesture = state->dispatch->panel_gesture;
        if (!state->panel_actions_attached.load(std::memory_order_acquire) ||
            sync_info == nullptr || sync_info->type != XR_TYPE_ACTIONS_SYNC_INFO ||
            (sync_info->countActiveActionSets != 0 && sync_info->activeActionSets == nullptr)) {
            return gesture.sync_actions(session, sync_info);
        }
        std::vector<XrActiveActionSet> active(
            sync_info->activeActionSets,
            sync_info->activeActionSets + sync_info->countActiveActionSets);
        active.push_back({gesture.action_set, XR_NULL_PATH});
        XrActionsSyncInfo merged = *sync_info;
        merged.activeActionSets = active.data();
        merged.countActiveActionSets = static_cast<std::uint32_t>(active.size());
        const XrResult result = gesture.sync_actions(session, &merged);
        return XR_FAILED(result) ? gesture.sync_actions(session, sync_info) : result;
    });
}

// Pure observation. The event is forwarded exactly as the runtime produced it
// and is never consumed, reordered or synthesized: the application sees the
// same queue it would without the layer.
//
// It exists because a session that stops being displayed is invisible from
// everywhere else in this layer. When a runtime drops a session out of the
// visible state it reports shouldRender=false and stops blocking xrWaitFrame,
// the application stops submitting projection layers, and generation fails
// open - which in a capture is indistinguishable from the layer breaking. A
// captured MSFS 2024 session did exactly that 104 s in and never recovered,
// and nothing in the log could say whether the runtime or the layer started
// it. The state transition is the answer, and the layer only sees it here.
XRAPI_ATTR XrResult XRAPI_CALL layer_poll_event(
    XrInstance instance,
    XrEventDataBuffer* event_data) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->poll_event == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->poll_event(instance, event_data);
        // XR_EVENT_UNAVAILABLE is the common answer and carries no buffer, so
        // this costs one comparison on the overwhelming majority of calls.
        if (result == XR_SUCCESS && event_data != nullptr &&
            event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* state_changed =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(
                    event_data);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::session_state,
                static_cast<std::int64_t>(state_changed->state),
                handle_value(state_changed->session),
                static_cast<std::uint64_t>(state_changed->time),
                0);
            // Reaching VISIBLE is the runtime saying this session is being
            // shown, which is the proof the delivery connection waits for. A
            // session that is only ever built to be measured and destroyed
            // stops at READY and never opens one. Named rather than compared:
            // STOPPING and EXITING sort above VISIBLE and mean the opposite.
            if (state_changed->state == XR_SESSION_STATE_VISIBLE ||
                state_changed->state == XR_SESSION_STATE_FOCUSED) {
                if (const auto session_state =
                        find_session(state_changed->session)) {
                    if (session_state->steamvr_delivery) {
                        session_state->steamvr_delivery->mark_established();
                    }
                }
            }
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain swapchain) {
    return guard_c_api_boundary([&] {
        return layer_destroy_swapchain_impl(swapchain);
    });
}

// A bridged Vulkan session's runtime is D3D12 and lists DXGI formats; the
// application asked a Vulkan session and reads VkFormats. Each DXGI format
// with a Vulkan equivalent is shown once, in the runtime's order.
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_formats(
    XrSession session,
    std::uint32_t format_capacity_input,
    std::uint32_t* format_count_output,
    std::int64_t* formats) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->enumerate_swapchain_formats == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!state->vulkan_bridge) {
            return state->dispatch->enumerate_swapchain_formats(
                session, format_capacity_input, format_count_output, formats);
        }
        if (format_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::uint32_t runtime_count = 0;
        XrResult result = state->dispatch->enumerate_swapchain_formats(
            session, 0, &runtime_count, nullptr);
        if (XR_FAILED(result)) {
            return result;
        }
        std::vector<std::int64_t> runtime_formats(runtime_count);
        if (runtime_count > 0) {
            result = state->dispatch->enumerate_swapchain_formats(
                session, runtime_count, &runtime_count, runtime_formats.data());
            if (XR_FAILED(result)) {
                return result;
            }
            runtime_formats.resize(runtime_count);
        }
        std::vector<std::int64_t> translated;
        for (const std::int64_t format : runtime_formats) {
            const VkFormat vulkan_format =
                xrfg::vulkan_format_for_dxgi(static_cast<DXGI_FORMAT>(format));
            if (vulkan_format == VK_FORMAT_UNDEFINED ||
                std::find(translated.begin(), translated.end(),
                          static_cast<std::int64_t>(vulkan_format)) != translated.end()) {
                continue;
            }
            translated.push_back(static_cast<std::int64_t>(vulkan_format));
        }
        *format_count_output = static_cast<std::uint32_t>(translated.size());
        if (format_capacity_input == 0) {
            return XR_SUCCESS;
        }
        if (formats == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (format_capacity_input < translated.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        std::copy(translated.begin(), translated.end(), formats);
        return XR_SUCCESS;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    return guard_c_api_boundary([&] {
        return layer_enumerate_swapchain_images_impl(
            swapchain,
            image_capacity_input,
            image_count_output,
            images);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_acquire_swapchain_image_impl(swapchain, acquire_info, index);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        result,
        handle_value(swapchain),
        index && XR_SUCCEEDED(result) ? *index : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        handle_value(swapchain),
        wait_info ? static_cast<std::uint64_t>(wait_info->timeout) : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_swapchain_image_impl(swapchain, wait_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        result,
        handle_value(swapchain));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_release,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_release_swapchain_image_impl(swapchain, release_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_release,
        result,
        handle_value(swapchain));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_end_frame(
    XrSession session,
    const XrFrameEndInfo* end_info) {
    // Before this frame's first record, so a frame is in the file whole or
    // not at all.
    xrfg::bridge_flight_logger().follow_setting();
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_end_frame,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_frame_impl(session, end_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_end_frame,
        result,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    return result;
}

}  // namespace

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request) {
    if (const auto delegated = optiscaler_bootstrap::negotiate(
            loader_info, layer_name, layer_request)) {
        return *delegated;
    }
    xrfg::initialize_bridge_flight_logger();
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::negotiation,
        loader_info ? loader_info->minInterfaceVersion : 0,
        loader_info ? loader_info->maxInterfaceVersion : 0,
        loader_info ? loader_info->maxApiVersion : 0);
    const XrResult result = guard_c_api_boundary([&] {
        if (loader_info == nullptr || layer_request == nullptr || layer_name == nullptr ||
            std::strcmp(layer_name, kLayerName) != 0 ||
            loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
            loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
            loader_info->structSize != sizeof(XrNegotiateLoaderInfo) ||
            layer_request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
            layer_request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
            layer_request->structSize != sizeof(XrNegotiateApiLayerRequest)) {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        if (loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
            loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
            loader_info->minApiVersion > kLayerApiVersion ||
            loader_info->maxApiVersion < kLayerApiVersion) {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        // A refusal here is the one way to keep the layer out of a process
        // entirely: the loader drops an implicit layer whose negotiation
        // fails and carries on without it, so an excluded process has no
        // hook, no history and no private swapchain from this layer.
        const std::wstring executable =
            xrfg::implicit_layer::current_executable_name();
        if (xrfg::implicit_layer::executable_is_excluded(
                executable,
                xrfg::implicit_layer::read_excluded_processes(
                    current_layer_directory()))) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::process_excluded,
                0,
                executable.size(),
                0,
                0);
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        layer_request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        layer_request->layerApiVersion = kLayerApiVersion;
        layer_request->getInstanceProcAddr = layer_get_instance_proc_addr;
        layer_request->createApiLayerInstance = layer_create_api_layer_instance;
        return XR_SUCCESS;
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::negotiation,
        result,
        layer_request ? layer_request->layerInterfaceVersion : 0,
        layer_request ? layer_request->layerApiVersion : 0);
    return result;
}
