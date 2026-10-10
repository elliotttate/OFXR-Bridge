#include "xrfg/benchmark_labels.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/standalone_launcher.hpp"
#include "benchmark_window.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(linker,                                                     \
    "\"/manifestdependency:type='win32' "                                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "           \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "          \
    "language='*'\"")

namespace {

constexpr wchar_t kWindowClass[] = L"OFXRBridgeTrayWindow";
constexpr wchar_t kApplicationName[] = L"OFXR Bridge";
constexpr wchar_t kCleanupArgument[] = L"--cleanup-manual-arm";
constexpr UINT kTrayMessage = WM_APP + 1;
// Posted by the benchmark's reader thread: new output, and the run's end.
constexpr UINT kBenchmarkProgressMessage = WM_APP + 2;
constexpr UINT kBenchmarkFinishedMessage = WM_APP + 3;
constexpr UINT_PTR kTrayId = 1;
constexpr int kPauseHotkeyId = 1;
constexpr UINT kArmPollMilliseconds = 250;
constexpr std::uint32_t kImplementationVersion = XRFG_IMPLEMENTATION_VERSION;
// One Donate entry each, creator first, as in the About box and the README.
constexpr wchar_t kDonateCreatorUrl[] = L"https://ko-fi.com/tig3rmast3r";
constexpr wchar_t kDonateMaintainerUrl[] = L"https://ko-fi.com/djules";
// The pause symbol beside the menu's "Resume frame generation", orange so a
// glance at the menu says generation is off. The FPS overlay's pause symbol
// is the same colour.
constexpr COLORREF kPausedColor = RGB(230, 115, 0);

enum MenuCommand : UINT {
    toggle_arm = 90,
    toggle_pause = 91,
    change_pause_key = 92,
    backend_fidelity_fx = 110,
    backend_nvidia_slow = 111,
    backend_nvidia_medium = 112,
    backend_nvidia_fast = 113,
    toggle_nvidia_bidirectional = 114,
    nvidia_scale_full = 115,
    nvidia_scale_three_quarter = 116,
    nvidia_scale_half = 117,
    toggle_deep_pipeline = 118,
    toggle_triple_frame_gen = 119,
    toggle_lower_vram = 126,
    generation_ofxr = 127,
    generation_dlss = 128,
    toggle_diagnostics = 120,
    overlay_off = 121,
    overlay_upper_left = 122,
    overlay_upper_right = 123,
    overlay_lower_left = 124,
    overlay_lower_right = 125,
    open_logs = 130,
    dlssg_scale_full = 131,
    dlssg_scale_two_thirds = 132,
    dlssg_scale_half = 133,
    donate_creator = 138,
    donate_maintainer = 139,
    show_about = 140,
    exit_application = 150,
    // The menu's single method list: each sets the algorithm and, for OFXR,
    // the optical-flow engine. The older commands above still do what they
    // did, for anything that sends them.
    method_fidelity_fx = 160,
    method_nvidia_fast = 161,
    method_nvidia_medium = 162,
    method_nvidia_slow = 163,
    method_native_dlss = 164,
    // How OFXR makes frames: interpolation, the hybrid, extrapolation.
    ofxr_mode_interpolate = 165,
    ofxr_mode_hybrid = 166,
    ofxr_mode_extrapolate = 167,
    // 2X and 3X as a pair of choices; each is toggle_triple_frame_gen when
    // it changes anything.
    frames_2x = 168,
    frames_3x = 169,
    show_benchmark = 170,
    // The status panel, in the FPS overlay menu: on a controller turned
    // upside down, always in the view, or off.
    panel_gesture = 171,
    panel_always = 172,
    panel_off = 173,
};

struct AppState {
    HWND window{};
    NOTIFYICONDATAW icon{};
    xrfg::standalone::LauncherSettings settings;
    std::filesystem::path executable_directory;
    std::filesystem::path local_directory;
    std::filesystem::path settings_path;
    std::filesystem::path armed_manifest;
    // The Vulkan layer's manifest, registered beside the OpenXR one while
    // Vulkan support is on; empty otherwise.
    std::filesystem::path armed_vulkan_manifest;
    xrfg::implicit_layer::RegistryScope armed_scope{
        xrfg::implicit_layer::RegistryScope::current_user};
    HANDLE arm_signal{};
    // Set while "Pause frame generation" is on. Made with the arm and closed
    // with it, so every arm starts resumed; null if it could not be made.
    HANDLE pause_signal{};
    bool paused{};
    // The `pause_hotkey` of tray.ini is held system-wide, while armed only.
    bool pause_hotkey_registered{};
    // Alternate namespace used by lifecycle tests, never read from user INI.
    std::wstring registry_subkey{xrfg::implicit_layer::kRegistrySubkey};
    HICON armed_icon{};
    HICON disarmed_icon{};
    UINT taskbar_created_message{};
    bool armed{};
    // "Benchmark this PC": its results label the menu's methods.
    ofxr_tray::BenchmarkController benchmark;
};

[[nodiscard]] std::wstring last_error_message(std::wstring_view action) {
    const DWORD code = GetLastError();
    wchar_t* system_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);
    std::wstring output(action);
    output += L" failed (" + std::to_wstring(code) + L")";
    if (length > 0 && system_message != nullptr) {
        output += L": ";
        output.append(system_message, length);
        LocalFree(system_message);
    }
    return output;
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    return std::filesystem::path(path.data()).parent_path();
}

[[nodiscard]] std::filesystem::path local_app_data() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value))) {
        throw std::runtime_error("SHGetKnownFolderPath failed");
    }
    const std::filesystem::path output(value);
    CoTaskMemFree(value);
    return output;
}

[[nodiscard]] std::filesystem::path runtime_directory(
    const std::filesystem::path& local_directory) {
    return xrfg::standalone::runtime_version_directory(
        local_directory,
        kImplementationVersion);
}

[[nodiscard]] bool write_text_atomic(
    const std::filesystem::path& path,
    std::string_view text,
    std::wstring* error) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream) {
                if (error) *error = L"Unable to write " + temporary.wstring();
                return false;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (error) *error = last_error_message(L"Saving " + path.wstring());
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Unable to save " + path.wstring();
        return false;
    }
}

void load_settings(AppState& state) {
    std::ifstream stream(state.settings_path, std::ios::binary);
    if (!stream) {
        return;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    state.settings = xrfg::standalone::parse_settings(text.str());
}

void save_settings(const AppState& state) {
    std::wstring ignored;
    static_cast<void>(write_text_atomic(
        state.settings_path,
        xrfg::standalone::serialize_settings(state.settings),
        &ignored));
}

void show_error(HWND owner, const std::wstring& error) {
    MessageBoxW(owner, error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
}

void remove_manifest_file(const std::filesystem::path& manifest) {
    std::error_code ignored;
    std::filesystem::remove(manifest, ignored);
}

void log_lifecycle(const std::filesystem::path& local_directory,
                   std::wstring_view action, std::wstring_view error = {}) noexcept {
    try {
        std::filesystem::create_directories(local_directory);
        const auto path = local_directory / L"tray-lifecycle.log";
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        std::wofstream stream(path, !size_error && size > 262144
            ? std::ios::trunc : std::ios::app);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        stream << time.wYear << L'-' << time.wMonth << L'-' << time.wDay << L' '
               << time.wHour << L':' << time.wMinute << L':' << time.wSecond
               << L" pid=" << GetCurrentProcessId() << L" V" << kImplementationVersion
               << L' ' << action << (error.empty() ? L" OK" : L" FAILED: ") << error << L'\n';
    } catch (...) { /* Cleanup must not depend on diagnostic I/O. */ }
}

// The newest ofxr_bridge.ini under any other version's cache folder.
//
// The two diagnostics knobs are carried forward from the file arming is about
// to overwrite, which keeps a hand edit across an arm - but a version bump
// creates an empty folder, so the lookup finds nothing and silently falls back
// to the defaults. A capture setting deliberately raised for an investigation
// was then reset by the next build, and the resulting logs were truncated
// without anything saying so.
[[nodiscard]] std::filesystem::path previous_runtime_configuration(
    const std::filesystem::path& local_directory,
    const std::filesystem::path& destination) {
    std::error_code code;
    const auto root = xrfg::standalone::runtime_version_directory(
        local_directory,
        kImplementationVersion).parent_path();
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_at{};
    for (const auto& entry :
         std::filesystem::directory_iterator(root, code)) {
        if (code || !entry.is_directory(code)) {
            continue;
        }
        const auto candidate = entry.path() / L"ofxr_bridge.ini";
        if (candidate == destination ||
            !std::filesystem::exists(candidate, code)) {
            continue;
        }
        const auto at = std::filesystem::last_write_time(candidate, code);
        if (code) {
            continue;
        }
        if (newest.empty() || at > newest_at) {
            newest = candidate;
            newest_at = at;
        }
    }
    return newest;
}

[[nodiscard]] bool write_runtime_configuration(
    const AppState& state,
    std::wstring* error,
    const std::filesystem::path& arm_manifest = {}) {
    const auto& manifest = arm_manifest.empty() ? state.armed_manifest : arm_manifest;
    // Carry forward the two diagnostics knobs the tray has no UI for. Arming
    // rewrites this file whole, so hardcoding them here is what made a
    // hand-edited max_file_mb survive exactly until the next arm.
    const auto destination =
        runtime_directory(state.local_directory) / L"ofxr_bridge.ini";
    // This version's file if it has one, otherwise the newest any other version
    // left behind, so the knobs survive a version bump as well as an arm.
    std::error_code exists_code;
    const auto source = std::filesystem::exists(destination, exists_code)
        ? destination
        : previous_runtime_configuration(state.local_directory, destination);
    const unsigned max_file_mb = GetPrivateProfileIntW(
        L"diagnostics",
        L"max_file_mb",
        static_cast<INT>(xrfg::standalone::kDefaultMaxFileMb),
        source.empty() ? destination.c_str() : source.c_str());
    const bool flush_each_event = GetPrivateProfileIntW(
        L"diagnostics",
        L"flush_each_event",
        0,
        source.empty() ? destination.c_str() : source.c_str()) != 0;
    // And `[ofxr] synthetic_pose`, likewise without a menu entry.
    std::array<wchar_t, 32> synthetic_pose{};
    GetPrivateProfileStringW(L"ofxr", L"synthetic_pose", L"", synthetic_pose.data(),
        static_cast<DWORD>(synthetic_pose.size()),
        source.empty() ? destination.c_str() : source.c_str());
    std::string configuration = xrfg::standalone::build_runtime_ini(
        state.settings, max_file_mb, flush_each_event,
        _wcsicmp(synthetic_pose.data(), L"real") == 0);
    if (!manifest.empty()) {
        const auto control = xrfg::implicit_layer::arm_signal_name(manifest);
        std::string ascii_control;
        for (const wchar_t c : control) ascii_control.push_back(static_cast<char>(c));
        configuration.insert(std::string("[ofxr]\r\n").size(),
            "control_event=" + ascii_control + "\r\n");
    }
    return write_text_atomic(destination, configuration, error);
}

[[nodiscard]] bool prepare_runtime_layer(
    AppState& state,
    std::filesystem::path* manifest,
    std::filesystem::path* vulkan_manifest,
    std::wstring* error) {
    if (vulkan_manifest) vulkan_manifest->clear();
    try {
        const auto source_dll = state.executable_directory / L"ofxr" /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (!std::filesystem::is_regular_file(source_dll)) {
            if (error) {
                *error = L"The bundled OFXR layer is missing:\r\n" +
                         source_dll.wstring();
            }
            return false;
        }
        const auto directory = runtime_directory(state.local_directory);
        std::filesystem::create_directories(directory);
        const auto runtime_dll = directory /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (!xrfg::standalone::install_runtime_layer_dll(
                source_dll, runtime_dll)) {
            if (error) *error = last_error_message(L"Installing the runtime layer");
            return false;
        }
#ifdef XRFG_NATIVE_DLSSG
        const auto source_dlss = state.executable_directory / L"ofxr" / L"nvngx_dlssg.dll";
        if (!xrfg::standalone::install_runtime_layer_dll(source_dlss, directory / L"nvngx_dlssg.dll")) {
            if (error) *error = L"Installing the bundled NVIDIA DLSS Frame Generation runtime failed.";
            return false;
        }
#endif

        static std::uint64_t last_arm_id = 0;
        last_arm_id = std::max<std::uint64_t>(last_arm_id + 1, GetTickCount64());
        const std::wstring manifest_name =
            std::wstring(xrfg::implicit_layer::kManifestPrefix) +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(last_arm_id) +
            xrfg::implicit_layer::kManifestSuffix;
        const auto generated_manifest = directory / manifest_name;
        *manifest = generated_manifest;
        state.arm_signal = xrfg::implicit_layer::create_arm_signal(generated_manifest, error);
        if (!state.arm_signal) return false;
        // Not worth refusing the arm over: without it the menu entry is grey.
        std::wstring pause_error;
        state.pause_signal = xrfg::implicit_layer::create_pause_signal(
            generated_manifest, &pause_error);
        state.paused = false;
        if (!state.pause_signal)
            log_lifecycle(state.local_directory, L"pause-control", pause_error);
        if (!write_text_atomic(
                generated_manifest,
                xrfg::standalone::build_implicit_layer_manifest(
                    runtime_dll, kImplementationVersion),
                error) ||
            !write_runtime_configuration(state, error, generated_manifest)) {
            remove_manifest_file(generated_manifest);
            return false;
        }
        *manifest = generated_manifest;
        if (state.settings.vulkan_support && vulkan_manifest) {
            // The queue-serialising Vulkan layer goes beside the OpenXR one,
            // in the same version folder, with a manifest of the same shape.
            const auto source_vulkan_dll = state.executable_directory / L"ofxr" /
                xrfg::implicit_layer::kVulkanLayerDll;
            if (!std::filesystem::is_regular_file(source_vulkan_dll)) {
                if (error) {
                    *error = L"The bundled OFXR Vulkan layer is missing:\r\n" +
                             source_vulkan_dll.wstring();
                }
                remove_manifest_file(generated_manifest);
                return false;
            }
            const auto runtime_vulkan_dll =
                directory / xrfg::implicit_layer::kVulkanLayerDll;
            if (!xrfg::standalone::install_runtime_layer_dll(
                    source_vulkan_dll, runtime_vulkan_dll)) {
                if (error) *error = last_error_message(L"Installing the Vulkan layer");
                remove_manifest_file(generated_manifest);
                return false;
            }
            const std::wstring vulkan_manifest_name =
                std::wstring(xrfg::implicit_layer::kVulkanManifestPrefix) +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(last_arm_id) +
                xrfg::implicit_layer::kManifestSuffix;
            const auto generated_vulkan_manifest = directory / vulkan_manifest_name;
            if (!write_text_atomic(
                    generated_vulkan_manifest,
                    xrfg::standalone::build_vulkan_layer_manifest(
                        runtime_vulkan_dll, kImplementationVersion),
                    error)) {
                remove_manifest_file(generated_manifest);
                return false;
            }
            *vulkan_manifest = generated_vulkan_manifest;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Unable to prepare the manual OpenXR layer.";
        return false;
    }
}

[[nodiscard]] bool spawn_cleanup_helper(
    const AppState& state,
    const std::filesystem::path& manifest,
    xrfg::implicit_layer::RegistryScope scope,
    std::wstring* error,
    const std::filesystem::path& vulkan_manifest = {}) {
    std::wstring command = xrfg::standalone::quote_windows_argument(
        (state.executable_directory / L"OFXRBridgeTray.exe").wstring());
    command += L" ";
    command += kCleanupArgument;
    command += L" ";
    command += xrfg::standalone::quote_windows_argument(manifest.wstring());
    command += L" ";
    command += std::to_wstring(GetCurrentProcessId());
    command += L" ";
    command += xrfg::implicit_layer::registry_scope_name(scope);
    if (!vulkan_manifest.empty()) {
        command += L" ";
        command += xrfg::standalone::quote_windows_argument(vulkan_manifest.wstring());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto tray_executable =
        state.executable_directory / L"OFXRBridgeTray.exe";
    if (!CreateProcessW(
            tray_executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            state.executable_directory.c_str(),
            &startup,
            &process)) {
        if (error) *error = last_error_message(L"Starting the cleanup watchdog");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

[[nodiscard]] bool cleanup_all_owned_registrations(
    const AppState& state,
    std::wstring* error) {
    if (error) error->clear();
    bool success = true;
    for (const auto scope : {
             xrfg::implicit_layer::RegistryScope::current_user,
             xrfg::implicit_layer::RegistryScope::local_machine}) {
        std::wstring detail;
        if (!xrfg::implicit_layer::cleanup_owned_registrations(
                state.local_directory, scope, &detail, state.registry_subkey)) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += xrfg::implicit_layer::registry_scope_name(scope);
                *error += L": ";
                *error += detail;
            }
        }
        // The Vulkan layer's registrations, in the loader's own key.
        std::wstring vulkan_detail;
        if (!xrfg::implicit_layer::cleanup_owned_registrations(
                state.local_directory, scope, &vulkan_detail,
                xrfg::implicit_layer::kVulkanRegistrySubkey,
                xrfg::implicit_layer::kVulkanManifestPrefix)) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += xrfg::implicit_layer::registry_scope_name(scope);
                *error += L" (Vulkan): ";
                *error += vulkan_detail;
            }
        }
    }
    return success;
}

[[nodiscard]] std::wstring tray_tooltip(const AppState& state) {
    namespace sl = xrfg::standalone;
    std::wstring tooltip = state.paused
        ? L"OFXR Bridge PAUSED - "
        : state.armed ? L"OFXR Bridge ARMED - " : L"OFXR Bridge - ";
    // What the menu's top says: the method, its resolution, 2X or 3X.
    tooltip += sl::active_method_summary(state.settings);
    const bool ofxr = sl::current_method(state.settings) != sl::Method::native_dlss;
    const sl::OfxrMode mode = sl::ofxr_mode(state.settings);
    if (ofxr) {
        if (state.settings.backend == sl::FlowBackend::nvidia && state.settings.nvidia_bidirectional &&
            !sl::mode_forces_fidelity_fx(mode)) {
            tooltip += L" + bidirectional";
        }
        switch (mode) {
        case sl::OfxrMode::hybrid:
            tooltip += L" - vectors + FidelityFX flow";
            break;
        case sl::OfxrMode::extrapolate:
            tooltip += L" - extrapolating";
            break;
        case sl::OfxrMode::interpolate:
        default:
            break;
        }
    }
    // Extrapolation turns the deeper pipeline off in the layer.
    if (!state.settings.triple_frame_gen && state.settings.deep_pipeline &&
        !(ofxr && mode == sl::OfxrMode::extrapolate)) {
        tooltip += L" - prefer FPS";
    }
    if (state.settings.diagnostics) {
        tooltip += L" - recorder on";
    }
    return tooltip;
}

void refresh_tray_icon(AppState& state) {
    const std::wstring tooltip = tray_tooltip(state);
    wcsncpy_s(state.icon.szTip, tooltip.c_str(), _TRUNCATE);
    state.icon.hIcon = state.armed ? state.armed_icon : state.disarmed_icon;
    state.icon.uFlags = NIF_ICON | NIF_TIP;
    static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &state.icon));
}

void show_balloon(
    AppState& state,
    std::wstring_view title,
    std::wstring_view message,
    DWORD flags = NIIF_INFO) {
    wcsncpy_s(state.icon.szInfoTitle, std::wstring(title).c_str(), _TRUNCATE);
    wcsncpy_s(state.icon.szInfo, std::wstring(message).c_str(), _TRUNCATE);
    state.icon.dwInfoFlags = flags;
    state.icon.uFlags = NIF_INFO;
    static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &state.icon));
}

[[nodiscard]] bool add_tray_icon(AppState& state) {
    state.icon = {};
    state.icon.cbSize = sizeof(state.icon);
    state.icon.hWnd = state.window;
    state.icon.uID = kTrayId;
    state.icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    state.icon.uCallbackMessage = kTrayMessage;
    state.icon.hIcon = state.armed ? state.armed_icon : state.disarmed_icon;
    const std::wstring tooltip = tray_tooltip(state);
    wcsncpy_s(state.icon.szTip, tooltip.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_ADD, &state.icon) != FALSE;
}

// Takes tray.ini's `pause_hotkey` for as long as the bridge is armed. The key
// is the tray's and not the layer's: nothing is added to the game, and the
// same chord works in every title. A key another program already holds is
// left to it; the menu entry still works.
void register_pause_hotkey(AppState& state) {
    if (state.pause_hotkey_registered || state.window == nullptr ||
        state.pause_signal == nullptr) return;
    const auto hotkey = xrfg::standalone::parse_hotkey(state.settings.pause_hotkey);
    if (!hotkey) return;
    state.pause_hotkey_registered = RegisterHotKey(
        state.window, kPauseHotkeyId, hotkey->modifiers | MOD_NOREPEAT,
        hotkey->key) != FALSE;
    if (!state.pause_hotkey_registered)
        log_lifecycle(state.local_directory, L"pause-hotkey",
            last_error_message(L"Registering the pause key"));
}

void unregister_pause_hotkey(AppState& state) {
    if (!state.pause_hotkey_registered) return;
    UnregisterHotKey(state.window, kPauseHotkeyId);
    state.pause_hotkey_registered = false;
}

// Ends the pause with the arm it belonged to. A running session keeps its own
// handle, and by now the arm signal has stopped it for good.
void close_pause_signal(AppState& state) {
    unregister_pause_hotkey(state);
    if (state.pause_signal) {
        CloseHandle(state.pause_signal);
        state.pause_signal = nullptr;
    }
    state.paused = false;
}

[[nodiscard]] bool disarm_bridge(
    AppState& state,
    std::wstring* error) {
    // Signal the active DLL BEFORE registry/file cleanup or window teardown.
    std::wstring signal_error;
    if (state.arm_signal && !SetEvent(state.arm_signal))
        signal_error = last_error_message(L"Stopping the loaded OFXR layer");
    std::wstring detail;
    const bool cleaned = cleanup_all_owned_registrations(state, &detail);
    if (!signal_error.empty()) {
        if (!detail.empty()) detail += L"\r\n";
        detail += signal_error;
    }
    if (!cleaned || !signal_error.empty()) {
        log_lifecycle(state.local_directory, L"disarm-all", detail);
        if (error) *error = detail;
        return false;
    }
    log_lifecycle(state.local_directory, L"disarm-all");
    if (state.arm_signal) {
        CloseHandle(state.arm_signal);
        state.arm_signal = nullptr;
    }
    close_pause_signal(state);
    state.armed = false;
    state.armed_manifest.clear();
    state.armed_vulkan_manifest.clear();
    state.armed_scope = xrfg::implicit_layer::RegistryScope::current_user;
    refresh_tray_icon(state);
    return true;
}

[[nodiscard]] bool arm_bridge(AppState& state, std::wstring* error) {
    // Reconcile disk/registry state, not just this tray process's memory.
    if (!disarm_bridge(state, error)) return false;

    std::filesystem::path manifest;
    std::filesystem::path vulkan_manifest;
    const auto scope = xrfg::implicit_layer::preferred_registry_scope();
    if (!prepare_runtime_layer(state, &manifest, &vulkan_manifest, error) ||
        !xrfg::implicit_layer::register_manifest(
            manifest, scope, error, state.registry_subkey) ||
        (!vulkan_manifest.empty() &&
         !xrfg::implicit_layer::register_manifest(
             vulkan_manifest, scope, error,
             xrfg::implicit_layer::kVulkanRegistrySubkey))) {
        if (!vulkan_manifest.empty()) {
            std::wstring cleanup_error;
            if (!xrfg::implicit_layer::retire_manifest(
                    vulkan_manifest, scope, &cleanup_error,
                    xrfg::implicit_layer::kVulkanRegistrySubkey))
                log_lifecycle(state.local_directory, L"arm-failure-cleanup", cleanup_error);
        }
        if (!manifest.empty()) {
            std::wstring cleanup_error;
            if (!xrfg::implicit_layer::retire_manifest(
                    manifest, scope, &cleanup_error, state.registry_subkey))
                log_lifecycle(state.local_directory, L"arm-failure-cleanup", cleanup_error);
        }
        if (state.arm_signal) {
            SetEvent(state.arm_signal);
            CloseHandle(state.arm_signal);
            state.arm_signal = nullptr;
        }
        close_pause_signal(state);
        return false;
    }
    if (!spawn_cleanup_helper(state, manifest, scope, error, vulkan_manifest)) {
        std::wstring ignored;
        if (!vulkan_manifest.empty()) {
            static_cast<void>(xrfg::implicit_layer::retire_manifest(
                vulkan_manifest, scope, &ignored,
                xrfg::implicit_layer::kVulkanRegistrySubkey));
        }
        static_cast<void>(xrfg::implicit_layer::retire_manifest(
            manifest, scope, &ignored, state.registry_subkey));
        if (state.arm_signal) {
            SetEvent(state.arm_signal);
            CloseHandle(state.arm_signal);
            state.arm_signal = nullptr;
        }
        close_pause_signal(state);
        return false;
    }
    state.armed = true;
    register_pause_hotkey(state);
    state.armed_manifest = manifest;
    state.armed_scope = scope;
    log_lifecycle(state.local_directory,
        scope == xrfg::implicit_layer::RegistryScope::local_machine
            ? L"arm-hklm"
            : L"arm-hkcu");
    refresh_tray_icon(state);
    show_balloon(
        state,
        L"OFXR Bridge armed",
        L"The bridge will remain enabled for OpenXR applications until you disarm it or exit the tray.");
    return true;
}

// `running_message`: what the balloon says for an option a running game
// follows; without one the option is for the next session.
void update_runtime_options(
    AppState& state, const wchar_t* running_message = nullptr) {
    save_settings(state);
    state.benchmark.settings_changed(state.settings);
    if (state.armed) {
        std::wstring error;
        if (!write_runtime_configuration(state, &error)) {
            show_error(state.window, error);
        }
    }
    refresh_tray_icon(state);
    if (state.armed) {
        show_balloon(
            state,
            L"OFXR options saved",
            running_message != nullptr
                ? running_message
                : L"The new optical-flow settings will be used by the next OpenXR session.");
    }
}

// The pause symbol shown beside "Resume frame generation": two orange bars on
// a transparent square the size of a check mark. A 32-bit premultiplied
// bitmap set as the entry's item bitmap is the one way to colour an entry
// that leaves the popup in the system's own menu style; an owner-drawn entry
// puts the whole menu in the flat unthemed one.
[[nodiscard]] HBITMAP create_pause_bitmap() {
    const int size = std::max(GetSystemMetrics(SM_CXMENUCHECK), 8);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) return bitmap;
    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::fill_n(pixels, static_cast<std::size_t>(size) * size, 0U);
    const std::uint32_t ink = 0xff000000U |
        (static_cast<std::uint32_t>(GetRValue(kPausedColor)) << 16) |
        (static_cast<std::uint32_t>(GetGValue(kPausedColor)) << 8) |
        GetBValue(kPausedColor);
    const int bar = std::max(size / 5, 2);
    const int gap = std::max(size / 5, 2);
    const int left = (size - 2 * bar - gap) / 2;
    const int top = size / 5;
    for (int y = top; y < size - top; ++y) {
        for (int x = 0; x < bar; ++x) {
            pixels[y * size + left + x] = ink;
            pixels[y * size + left + bar + gap + x] = ink;
        }
    }
    return bitmap;
}

// What the "Current key binding" entry and the dialog call a setting.
[[nodiscard]] std::wstring pause_key_display(std::string_view setting) {
    const std::string name = xrfg::standalone::hotkey_display_name(setting);
    if (name.empty()) return L"none";
    return std::wstring(name.begin(), name.end());
}

// The dialog's own state: the setting it opened on and the one it closed on.
struct PauseKeyDialog {
    std::string setting;
};

// The hotkey control's value for a chord, and back. Its modifier flags are
// not RegisterHotKey's, and it marks the navigation keys as extended.
[[nodiscard]] WORD hotkey_control_value(const xrfg::standalone::Hotkey& hotkey) {
    BYTE flags = 0;
    if (hotkey.modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (hotkey.modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (hotkey.modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    switch (hotkey.key) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_DIVIDE: case VK_NUMLOCK:
        flags |= HOTKEYF_EXT;
        break;
    default:
        break;
    }
    return MAKEWORD(static_cast<BYTE>(hotkey.key), flags);
}

[[nodiscard]] xrfg::standalone::Hotkey hotkey_from_control(WORD value) {
    xrfg::standalone::Hotkey hotkey;
    hotkey.key = LOBYTE(value);
    const BYTE flags = HIBYTE(value);
    if (flags & HOTKEYF_SHIFT) hotkey.modifiers |= MOD_SHIFT;
    if (flags & HOTKEYF_CONTROL) hotkey.modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) hotkey.modifiers |= MOD_ALT;
    return hotkey;
}

INT_PTR CALLBACK pause_key_dialog_procedure(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    constexpr int kAvailabilityProbeId = 0x7F01;
    const HWND box = GetDlgItem(dialog, IDC_PAUSE_KEY_HOTKEY);
    const auto show = [&](const std::string& setting) {
        const auto hotkey = xrfg::standalone::parse_hotkey(setting);
        SendMessageW(box, HKM_SETHOTKEY, hotkey ? hotkey_control_value(*hotkey) : 0, 0);
    };
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        const auto* state = reinterpret_cast<const PauseKeyDialog*>(lparam);
        // No rules on the box: any key, with or without modifiers, is the
        // user's to choose. The dialog's text says what a bare key costs.
        show(state->setting);
        SetDlgItemTextW(dialog, IDC_PAUSE_KEY_STATUS,
            (L"Current key binding: " + pause_key_display(state->setting)).c_str());
        SetForegroundWindow(dialog);
        SetFocus(box);
        return FALSE; // the focus is set
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_PAUSE_KEY_DEFAULT:
            show(xrfg::standalone::kDefaultPauseHotkey);
            SetFocus(box);
            return TRUE;
        case IDC_PAUSE_KEY_NONE:
            SendMessageW(box, HKM_SETHOTKEY, 0, 0);
            SetFocus(box);
            return TRUE;
        case IDOK: {
            auto* state = reinterpret_cast<PauseKeyDialog*>(
                GetWindowLongPtrW(dialog, DWLP_USER));
            const WORD value = LOWORD(SendMessageW(box, HKM_GETHOTKEY, 0, 0));
            if (LOBYTE(value) == 0) {
                state->setting = "off";
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            const auto hotkey = hotkey_from_control(value);
            const auto setting = xrfg::standalone::hotkey_setting(hotkey);
            if (!setting) {
                SetDlgItemTextW(dialog, IDC_PAUSE_KEY_STATUS,
                    L"That key cannot be used. Choose a different one.");
                SetFocus(box);
                return TRUE;
            }
            // Asked of the system now, while the dialog can still say so:
            // the tray's own registration is released for as long as this
            // dialog is open, so a refusal is another program's.
            if (!RegisterHotKey(dialog, kAvailabilityProbeId,
                    hotkey.modifiers | MOD_NOREPEAT, hotkey.key)) {
                SetDlgItemTextW(dialog, IDC_PAUSE_KEY_STATUS,
                    L"Another program already uses that combination. "
                    L"Choose a different one.");
                SetFocus(box);
                return TRUE;
            }
            UnregisterHotKey(dialog, kAvailabilityProbeId);
            state->setting = *setting;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    default:
        return FALSE;
    }
}

// Stores a new key and takes it at once if the bridge is armed.
void apply_pause_hotkey(AppState& state, const std::string& setting) {
    unregister_pause_hotkey(state);
    state.settings.pause_hotkey = setting;
    save_settings(state);
    if (state.armed) register_pause_hotkey(state);
    log_lifecycle(state.local_directory, L"pause-hotkey-changed");
}

void change_pause_hotkey(AppState& state) {
    // Released while the dialog is open, or pressing the current chord in
    // the box would pause the game instead of reaching the box.
    unregister_pause_hotkey(state);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&controls);
    PauseKeyDialog dialog{state.settings.pause_hotkey};
    const INT_PTR answer = DialogBoxParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PAUSE_KEY), state.window,
        pause_key_dialog_procedure, reinterpret_cast<LPARAM>(&dialog));
    if (answer == IDOK) {
        apply_pause_hotkey(state, dialog.setting);
    } else if (state.armed) {
        register_pause_hotkey(state);
    }
}

// Menu building. Every entry is text, optionally followed by a tab and a
// right-aligned annotation: the benchmark's estimate for that choice.
void append_entry(HMENU menu, UINT id, const std::wstring& text, bool checked = false,
                  bool enabled = true, bool radio = false, const std::wstring& annotation = {}) {
    std::wstring label = annotation.empty() ? text : text + L"\t" + annotation;
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    item.fType = MFT_STRING | (radio ? MFT_RADIOCHECK : 0U);
    item.fState = (checked ? MFS_CHECKED : MFS_UNCHECKED) | (enabled ? MFS_ENABLED : MFS_DISABLED);
    item.wID = id;
    item.dwTypeData = label.data();
    InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
}

// A line that informs and does nothing: a heading or the bridge's state.
void append_note(HMENU menu, const std::wstring& text, bool bold = false) {
    std::wstring label = text;
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
    item.fType = MFT_STRING;
    item.fState = MFS_DISABLED | (bold ? MFS_DEFAULT : 0U);
    item.dwTypeData = label.data();
    InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
}

void append_submenu(HMENU menu, HMENU submenu, const std::wstring& text, bool enabled = true) {
    std::wstring label = text;
    MENUITEMINFOW item{};
    item.cbSize = sizeof(item);
    item.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_STATE;
    item.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
    item.hSubMenu = submenu;
    item.dwTypeData = label.data();
    InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
}

void append_separator(HMENU menu) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

// The menu, in the order a user decides things: what the bridge is doing,
// arming and pausing, which frame-generation method and what it costs, its
// quality and performance options, then the display, diagnostics and the
// rarely needed. Every choice that has a benchmark result says what it costs
// on this PC and the best it can do.
void show_context_menu(AppState& state) {
    namespace sl = xrfg::standalone;
    namespace bm = xrfg::benchmark;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    const auto& settings = state.settings;
    const sl::Method method = sl::current_method(settings);
    const bool native = method == sl::Method::native_dlss;
    const bool triple = settings.triple_frame_gen;
    const sl::OfxrMode mode = sl::ofxr_mode(settings);
    // Extrapolation runs FidelityFX flow whatever is chosen.
    const bool forced_fidelity_fx = !native && sl::mode_forces_fidelity_fx(mode);
    const bool nvidia_flow = !native && method != sl::Method::fidelity_fx && !forced_fidelity_fx;
    const bool extrapolating = !native && mode == sl::OfxrMode::extrapolate;
    // Estimates only from results measured on this PC's graphics card.
    const bm::Results* results =
        state.benchmark.results_for_this_gpu() ? state.benchmark.results() : nullptr;
    const double refresh = state.benchmark.refresh_hz();
    const auto method_note = [&](sl::Method entry, const sl::CostQuery& query = {}) {
        if (results == nullptr) return std::wstring();
        return sl::menu_annotation(sl::method_cost(*results, settings, entry, query), refresh,
                                   query.triple.value_or(triple));
    };
    // An NVIDIA engine is not what runs under extrapolation.
    const auto engine_note = [&](sl::Method entry) {
        return forced_fidelity_fx ? std::wstring(L"FidelityFX runs in this OFXR mode")
                                  : method_note(entry);
    };
    const auto mode_note = [&](sl::OfxrMode entry) {
        if (results == nullptr) return std::wstring();
        return sl::menu_annotation(sl::ofxr_mode_cost(*results, settings, entry), refresh, triple);
    };

    // What the bridge is doing.
    wchar_t title[96]{};
    swprintf_s(title, L"OFXR Bridge V%03u: %s", kImplementationVersion,
               state.paused ? L"paused" : state.armed ? L"armed" : L"not armed");
    append_note(menu, title, true);
    append_note(menu, L"Frame generation: " + sl::active_method_summary(settings));
    if (!native) append_note(menu, L"OFXR is " + sl::ofxr_mode_name(mode));
    append_separator(menu);
    AppendMenuW(
        menu,
        MF_STRING | (state.armed ? MF_CHECKED : MF_UNCHECKED),
        toggle_arm,
        state.armed
            ? L"Disarm bridge"
            : L"Arm bridge until manual disarm");
    // The key is shown beside the entry it presses.
    const std::wstring key = xrfg::standalone::parse_hotkey(settings.pause_hotkey)
        ? pause_key_display(settings.pause_hotkey) : std::wstring();
    append_entry(menu, toggle_pause,
                 state.paused ? L"Resume frame generation" : L"Pause frame generation", false,
                 state.armed && state.pause_signal, false, key);
    // Must outlive the menu, which only borrows it.
    HBITMAP pause_bitmap = state.paused ? create_pause_bitmap() : nullptr;
    if (pause_bitmap) {
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_BITMAP;
        item.hbmpItem = pause_bitmap;
        SetMenuItemInfoW(menu, toggle_pause, FALSE, &item);
    }
    append_separator(menu);

    // Frame generation method: one list, then how OFXR makes frames, then
    // how many frames.
    if (HMENU methods = CreatePopupMenu()) {
        if (results != nullptr) {
            append_note(methods, L"On this PC at " + std::to_wstring(results->eye_width) + L" \u00D7 " +
                                     std::to_wstring(results->eye_height) + L" per eye, " +
                                     std::to_wstring(static_cast<int>(std::lround(refresh))) +
                                     L" Hz: GPU time per generated frame, best speed-up");
        } else if (state.benchmark.results() != nullptr) {
            append_note(methods, L"The benchmark results are for another graphics card: run Benchmark this PC again");
        } else {
            append_note(methods, L"Run Benchmark this PC to see what each method costs here");
        }
        append_separator(methods);
        append_note(methods, L"OFXR from optical flow (games without DLSS vectors)");
        append_entry(methods, method_fidelity_fx, L"FidelityFX optical flow (any graphics card)",
                     method == sl::Method::fidelity_fx, true, true, method_note(sl::Method::fidelity_fx));
        append_entry(methods, method_nvidia_fast, L"NVIDIA optical flow, fast (testing)",
                     method == sl::Method::nvidia_fast, true, true, engine_note(sl::Method::nvidia_fast));
        append_entry(methods, method_nvidia_medium, L"NVIDIA optical flow, medium (default)",
                     method == sl::Method::nvidia_medium, true, true, engine_note(sl::Method::nvidia_medium));
        append_entry(methods, method_nvidia_slow, L"NVIDIA optical flow, slow (best flow quality)",
                     method == sl::Method::nvidia_slow, true, true, engine_note(sl::Method::nvidia_slow));
#ifdef XRFG_NATIVE_DLSSG
        append_separator(methods);
        append_note(methods, L"Games with DLSS: NVIDIA's own frame generation");
        append_entry(methods, method_native_dlss, L"NVIDIA DLSS Frame Generation (experimental)",
                     native, true, true, method_note(sl::Method::native_dlss));
#endif
        // How OFXR makes frames. The costs here are in games with DLSS
        // vectors; the list above shows what runs in the others.
        append_separator(methods);
        append_note(methods, native ? L"How OFXR makes frames (not with DLSS Frame Generation)"
                                    : L"How OFXR makes frames (cost in games with DLSS vectors)");
        append_entry(methods, ofxr_mode_interpolate,
                     L"Interpolate, from the game's DLSS vectors where it has them (cheapest)",
                     mode == sl::OfxrMode::interpolate, !native, true,
                     mode_note(sl::OfxrMode::interpolate));
        append_entry(methods, ofxr_mode_hybrid,
                     L"Interpolate, DLSS vectors + FidelityFX flow (best quality, default)",
                     mode == sl::OfxrMode::hybrid, !native, true, mode_note(sl::OfxrMode::hybrid));
        const std::wstring extrapolation = mode_note(sl::OfxrMode::extrapolate);
        append_entry(methods, ofxr_mode_extrapolate,
                     L"Extrapolate, SpaceWarp-style (lowest latency)",
                     mode == sl::OfxrMode::extrapolate, !native, true,
                     extrapolation.find(L" ms") == std::wstring::npos
                         ? extrapolation : extrapolation + L" \u00B7 no added latency");
        append_separator(methods);
        append_note(methods, L"Frames shown per game frame");
        append_entry(methods, frames_2x, L"2X: one generated frame per game frame", !triple, true, true,
                     method_note(method, {.triple = false}));
        append_entry(methods, frames_3x, L"3X: two generated frames per game frame (switches live)",
                     triple, true, true, method_note(method, {.triple = true}));
        append_submenu(menu, methods, L"Frame generation method");
    }

    // Quality and performance: the chosen method's resolution and checks,
    // and the pipeline's latency trade.
    if (HMENU quality = CreatePopupMenu()) {
        const sl::Method flow_method = native ? sl::Method::fidelity_fx : method;
        if (HMENU scales = CreatePopupMenu()) {
            const struct {
                UINT command;
                xrfg::standalone::NvidiaInputScale scale;
                int percent;
                const wchar_t* text;
            } entries[]{
                {nvidia_scale_full, xrfg::standalone::NvidiaInputScale::full, 100, L"100% (finest flow)"},
                {nvidia_scale_three_quarter, xrfg::standalone::NvidiaInputScale::three_quarter, 75, L"75%"},
                {nvidia_scale_half, xrfg::standalone::NvidiaInputScale::half, 50, L"50% (default)"},
            };
            for (const auto& entry : entries) {
                append_entry(scales, entry.command, entry.text, settings.nvidia_input_scale == entry.scale,
                             true, true, method_note(flow_method, {.input_scale = entry.percent}));
            }
            append_submenu(quality, scales, L"Optical flow resolution", !native);
        }
        std::wstring both_ways;
        if (results != nullptr && nvidia_flow) {
            const auto with = sl::method_cost(*results, settings, method, {.bidirectional = true});
            const auto without = sl::method_cost(*results, settings, method, {.bidirectional = false});
            if (with.status == bm::CaseStatus::ok && without.status == bm::CaseStatus::ok) {
                both_ways = L"+" + bm::format_ms(std::max(with.cost_ms - without.cost_ms, 0.0));
            }
        }
        append_entry(quality, toggle_nvidia_bidirectional,
                     L"NVIDIA optical flow both ways (cleaner edges)", settings.nvidia_bidirectional,
                     nvidia_flow, false, both_ways);
#ifdef XRFG_NATIVE_DLSSG
        // Below 100%, NVIDIA generates at a lower resolution and the bridge
        // puts back the real frame's detail wherever it can follow the
        // game's motion. On recorded Galactic Racer frames 67% and 50% had
        // less error than 100% and were sharper, so 67% is the default; a
        // sharp synthetic scene loses detail at 50%. dlssg_resolution in the
        // layer's INI takes 25 to 100.
        if (HMENU native_scales = CreatePopupMenu()) {
            const struct { UINT command; int percent; const wchar_t* text; } entries[]{
                {dlssg_scale_full, 100, L"100% (most GPU time)"},
                {dlssg_scale_two_thirds, 67, L"67% (default)"},
                {dlssg_scale_half, 50, L"50% (fastest)"},
            };
            for (const auto& entry : entries) {
                append_entry(native_scales, entry.command, entry.text,
                             settings.native_scale == entry.percent, true, true,
                             method_note(sl::Method::native_dlss, {.native_scale = entry.percent}));
            }
            append_submenu(quality, native_scales, L"DLSS Frame Generation resolution", native);
        }
#endif
        append_separator(quality);
        // 3X needs what "Prefer FPS over latency" sets up, so it holds that
        // on; extrapolation is there for latency, and the layer turns it off.
        AppendMenuW(
            quality,
            MF_STRING |
                (!extrapolating && (settings.deep_pipeline || settings.triple_frame_gen)
                     ? MF_CHECKED : MF_UNCHECKED) |
                (settings.triple_frame_gen || extrapolating ? MF_GRAYED : MF_ENABLED),
            toggle_deep_pipeline,
            extrapolating ? L"Prefer FPS over latency (off while extrapolating)"
            : settings.triple_frame_gen
                ? L"Prefer FPS over latency (on with 3X Frame Gen)"
                : L"Prefer FPS over latency (adds a frame of latency)");
        append_submenu(menu, quality, L"Quality and performance");
    }
    append_entry(menu, show_benchmark, L"Benchmark this PC\u2026", false, true, false,
                 state.benchmark.menu_hint());
    append_separator(menu);

    HMENU overlay_menu = CreatePopupMenu();
    if (overlay_menu) {
        const struct { UINT command; xrfg::FpsOverlayPosition position; const wchar_t* text; } entries[]{
            {overlay_upper_left, xrfg::FpsOverlayPosition::upper_left, L"Upper left"},
            {overlay_upper_right, xrfg::FpsOverlayPosition::upper_right, L"Upper right"},
            {overlay_lower_left, xrfg::FpsOverlayPosition::lower_left, L"Lower left"},
            {overlay_lower_right, xrfg::FpsOverlayPosition::lower_right, L"Lower right"},
            {overlay_off, xrfg::FpsOverlayPosition::off, L"Off"}};
        for (const auto& entry : entries) {
            if (entry.position == xrfg::FpsOverlayPosition::off) AppendMenuW(overlay_menu, MF_SEPARATOR, 0, nullptr);
            append_entry(overlay_menu, entry.command, entry.text,
                         settings.overlay_position == entry.position, true, true);
        }
        // The status panel: what is asked against what runs, why they
        // differ, and the frame rates. It stands in for the counter while it
        // shows.
        append_separator(overlay_menu);
        append_note(overlay_menu, L"Status panel in the headset");
        const struct { UINT command; xrfg::StatusPanelMode mode; const wchar_t* text; } panels[]{
            {panel_gesture, xrfg::StatusPanelMode::gesture,
             L"On a controller turned upside down (default)"},
            {panel_always, xrfg::StatusPanelMode::always, L"Always, low in the view"},
            {panel_off, xrfg::StatusPanelMode::off, L"Off"}};
        for (const auto& entry : panels) {
            append_entry(overlay_menu, entry.command, entry.text,
                         settings.status_panel == entry.mode, true, true);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(overlay_menu), L"FPS overlay");
    }
    if (HMENU diagnostics = CreatePopupMenu()) {
        AppendMenuW(diagnostics, MF_STRING | (settings.diagnostics ? MF_CHECKED : MF_UNCHECKED),
                    toggle_diagnostics, L"Bridge flight recorder (switches live)");
        AppendMenuW(diagnostics, MF_STRING, open_logs, L"Open bridge logs");
        append_submenu(menu, diagnostics, L"Diagnostics");
    }
    if (HMENU advanced = CreatePopupMenu()) {
        AppendMenuW(advanced, MF_STRING | (settings.single_swapchain_rings ? MF_CHECKED : MF_UNCHECKED),
                    toggle_lower_vram, L"Lower VRAM (may cause stuttering)");
        // Opens the dialog that changes it. While armed, a key the system
        // would not give the tray says so here, since it will not work.
        std::wstring binding_label =
            L"Pause key: " + pause_key_display(settings.pause_hotkey) + L"\u2026";
        if (state.armed && state.pause_signal && !state.pause_hotkey_registered &&
            xrfg::standalone::parse_hotkey(settings.pause_hotkey)) {
            binding_label += L" (used by another program)";
        }
        AppendMenuW(advanced, MF_STRING, change_pause_key, binding_label.c_str());
        append_submenu(menu, advanced, L"Advanced");
    }
    append_separator(menu);
    HMENU donate_menu = CreatePopupMenu();
    if (donate_menu) {
        AppendMenuW(donate_menu, MF_STRING, donate_creator,
            L"tig3rmast3r, creator of OFXR");
        AppendMenuW(donate_menu, MF_STRING, donate_maintainer,
            L"Djules, maintainer of 0.2.x");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(donate_menu), L"Donate");
    }
    AppendMenuW(menu, MF_STRING, show_about, L"About");
    AppendMenuW(menu, MF_STRING, exit_application, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(state.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, state.window, nullptr);
    PostMessageW(state.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (pause_bitmap) DeleteObject(pause_bitmap);
}

void handle_command(AppState& state, UINT command) {
    switch (command) {
    case toggle_arm:
        if (state.armed) {
            std::wstring error;
            if (!disarm_bridge(state, &error)) {
                show_error(state.window, error);
            }
        } else {
            std::wstring error;
            if (!arm_bridge(state, &error)) {
                show_error(state.window, error);
            }
        }
        break;
    case toggle_pause:
        if (!state.armed || !state.pause_signal) break;
        if (state.paused ? ResetEvent(state.pause_signal)
                         : SetEvent(state.pause_signal)) {
            state.paused = !state.paused;
            log_lifecycle(state.local_directory, state.paused ? L"pause" : L"resume");
            refresh_tray_icon(state);
            if (state.paused) {
                show_balloon(
                    state,
                    L"Frame generation paused",
                    L"Running games still go through the bridge. To rule OFXR out, disarm and restart the game.");
            } else {
                show_balloon(
                    state,
                    L"Frame generation resumed",
                    L"Running games generate frames again.");
            }
        } else {
            show_error(
                state.window, last_error_message(L"Switching the frame generation pause"));
        }
        break;
    case change_pause_key:
        change_pause_hotkey(state);
        break;
    case generation_ofxr:
    case generation_dlss:
        state.settings.frame_generation = command == generation_dlss
            ? xrfg::standalone::FrameGeneration::native_dlss : xrfg::standalone::FrameGeneration::ofxr;
        update_runtime_options(state, L"The frame-generation algorithm will be used by the next OpenXR session.");
        break;
    case dlssg_scale_full:
    case dlssg_scale_two_thirds:
    case dlssg_scale_half:
        state.settings.native_scale = command == dlssg_scale_full ? 100
            : command == dlssg_scale_two_thirds ? 67 : 50;
        update_runtime_options(state, L"DLSS Frame Generation will use this resolution the next time the game starts.");
        break;
    case backend_fidelity_fx:
        state.settings.backend = xrfg::standalone::FlowBackend::fidelity_fx;
        update_runtime_options(state);
        break;
    case backend_nvidia_fast:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::fast;
        update_runtime_options(state);
        break;
    case backend_nvidia_slow:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::slow;
        update_runtime_options(state);
        break;
    case backend_nvidia_medium:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::medium;
        update_runtime_options(state);
        break;
    case toggle_nvidia_bidirectional:
        state.settings.nvidia_bidirectional =
            !state.settings.nvidia_bidirectional;
        update_runtime_options(state);
        break;
    case nvidia_scale_full:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::full;
        update_runtime_options(state);
        break;
    case nvidia_scale_three_quarter:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::three_quarter;
        update_runtime_options(state);
        break;
    case nvidia_scale_half:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::half;
        update_runtime_options(state);
        break;
    case toggle_deep_pipeline:
        if (state.settings.triple_frame_gen) {
            break;
        }
        state.settings.deep_pipeline = !state.settings.deep_pipeline;
        update_runtime_options(state);
        // A menu item cannot carry a tooltip, so the explanation goes in
        // the notification, armed or not.
        show_balloon(
            state,
            state.settings.deep_pipeline
                ? L"Prefer FPS over latency: on"
                : L"Prefer FPS over latency: off",
            L"Helps games that only just reach half your headset's refresh "
            L"rate hold full FPS more steadily. Adds one frame of latency "
            L"(about 11 ms at 90 Hz). Leave off if the game already runs "
            L"comfortably. Takes effect the next time the game starts.");
        break;
    case toggle_lower_vram:
        state.settings.single_swapchain_rings = !state.settings.single_swapchain_rings;
        update_runtime_options(state);
        show_balloon(
            state,
            state.settings.single_swapchain_rings
                ? L"Lower VRAM: on"
                : L"Lower VRAM: off",
            L"On, OFXR keeps one working image per output instead of two and "
            L"uses about half a gigabyte less VRAM at high resolutions. On "
            L"some graphics cards that showed as stutter; turn it off if you "
            L"see any and have VRAM to spare. Takes effect the next time the "
            L"game starts.");
        break;
    case toggle_triple_frame_gen:
        state.settings.triple_frame_gen = !state.settings.triple_frame_gen;
        if (state.settings.triple_frame_gen) {
            // 3X runs on what this sets up; it stays on afterwards.
            state.settings.deep_pipeline = true;
        }
        update_runtime_options(state);
        if (state.armed && xrfg::implicit_layer::running_session_needs_restart_for_3x()) {
            // A notification is easy to miss - Windows holds them back
            // while a game runs - and this one says the click did nothing,
            // so it has to be seen.
            MessageBoxW(
                state.window,
                L"Restart your game to apply this change.\n\n"
                L"The game that is running was started with \"Prefer FPS over "
                L"latency\" off, so 3X Frame Gen cannot be switched while it "
                L"runs. The setting is saved: the game will use it the next "
                L"time it starts.",
                L"OFXR Bridge - game restart needed",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        } else {
            show_balloon(
                state,
                state.settings.triple_frame_gen
                    ? L"3X Frame Gen: on"
                    : L"3X Frame Gen: off",
                state.settings.triple_frame_gen
                    ? L"Two generated frames for every frame the game "
                      L"renders: the game runs at a third of your headset's "
                      L"refresh rate (30 FPS at 90 Hz). Switches in a running "
                      L"game within a moment."
                    : L"Back to one generated frame per game frame. Switches "
                      L"in a running game within a moment.");
        }
        break;
    case overlay_off:
    case overlay_upper_left:
    case overlay_upper_right:
    case overlay_lower_left:
    case overlay_lower_right:
        state.settings.overlay_position = command == overlay_off ? xrfg::FpsOverlayPosition::off
            : command == overlay_upper_left ? xrfg::FpsOverlayPosition::upper_left
            : command == overlay_lower_left ? xrfg::FpsOverlayPosition::lower_left
            : command == overlay_lower_right ? xrfg::FpsOverlayPosition::lower_right
            : xrfg::FpsOverlayPosition::upper_right;
        update_runtime_options(
            state, L"The FPS overlay position updates in running applications.");
        break;
    case panel_gesture:
    case panel_always:
    case panel_off:
        state.settings.status_panel = command == panel_gesture ? xrfg::StatusPanelMode::gesture
            : command == panel_always ? xrfg::StatusPanelMode::always
                                      : xrfg::StatusPanelMode::off;
        // The gesture needs the game's input set up with the layer's grip
        // action, which happens as a game starts; the other two are followed
        // live.
        update_runtime_options(
            state,
            state.settings.status_panel == xrfg::StatusPanelMode::gesture
                ? L"Turn a controller upside down in a game to see what frame "
                  L"generation is doing. A game started while the panel was "
                  L"always on or off needs a restart for the gesture."
            : state.settings.status_panel == xrfg::StatusPanelMode::always
                ? L"The status panel shows low in the view, in running games too."
                : L"The status panel is off, in running games too.");
        break;
    case toggle_diagnostics:
        state.settings.diagnostics = !state.settings.diagnostics;
        update_runtime_options(
            state,
            state.settings.diagnostics
                ? L"Recording starts within a moment, in running games too, "
                  L"with one hitch. The log begins with the session's "
                  L"start-up records."
                : L"Recording stops within a moment, with one hitch, and the "
                  L"log file is closed.");
        break;
    case open_logs: {
        const auto directory = runtime_directory(state.local_directory);
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        ShellExecuteW(
            state.window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case donate_creator:
    case donate_maintainer:
        ShellExecuteW(
            state.window, L"open",
            command == donate_creator ? kDonateCreatorUrl : kDonateMaintainerUrl,
            nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case show_about: {
        wchar_t version_label[64]{};
        swprintf_s(version_label, L"OFXR Bridge V%03u", kImplementationVersion);
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = state.window;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                         TDF_ENABLE_HYPERLINKS |
                         TDF_SIZE_TO_CONTENT |
                         TDF_USE_HICON_MAIN;
        dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        dialog.pszWindowTitle = kApplicationName;
        dialog.pszMainInstruction = version_label;
        dialog.pszContent =
            L"Created by tig3rmast3r, the original author of OFXR.\r\n"
            L"Support him: <a href=\"https://ko-fi.com/tig3rmast3r\">"
            L"ko-fi.com/tig3rmast3r</a>\r\n\r\n"
            L"0.2.X version maintained by Djules.\r\n"
            L"Support him: <a href=\"https://ko-fi.com/djules\">"
            L"ko-fi.com/djules</a>\r\n\r\n"
            L"Licensed under LGPL-3.0-or-later.\r\n"
            L"<a href=\"https://github.com/djules75/OFXR-Bridge\">"
            L"github.com/djules75/OFXR-Bridge</a>";
        dialog.hMainIcon = state.disarmed_icon;
        dialog.pfCallback = [](
            HWND window,
            UINT notification,
            WPARAM,
            LPARAM parameter,
            LONG_PTR) -> HRESULT {
                if (notification == TDN_HYPERLINK_CLICKED) {
                    ShellExecuteW(
                        window,
                        L"open",
                        reinterpret_cast<LPCWSTR>(parameter),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
                return S_OK;
            };
        if (FAILED(TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr))) {
            const std::wstring message = std::wstring(version_label) +
                L"\r\n\r\nCreated by tig3rmast3r, the original author of OFXR.\r\n"
                L"Support him: https://ko-fi.com/tig3rmast3r\r\n\r\n"
                L"0.2.X version maintained by Djules.\r\n"
                L"Support him: https://ko-fi.com/djules\r\n\r\n"
                L"License: LGPL-3.0-or-later\r\n"
                L"https://github.com/djules75/OFXR-Bridge";
            MessageBoxW(
                state.window,
                message.c_str(),
                kApplicationName,
                MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case method_fidelity_fx:
    case method_nvidia_fast:
    case method_nvidia_medium:
    case method_nvidia_slow:
    case method_native_dlss: {
        namespace sl = xrfg::standalone;
        const auto previous = state.settings.frame_generation;
        sl::apply_method(state.settings,
            command == method_fidelity_fx ? sl::Method::fidelity_fx
            : command == method_nvidia_fast ? sl::Method::nvidia_fast
            : command == method_nvidia_slow ? sl::Method::nvidia_slow
            : command == method_native_dlss ? sl::Method::native_dlss
            : sl::Method::nvidia_medium);
        // The messages generation_* and backend_* give.
        update_runtime_options(state, previous != state.settings.frame_generation
            ? L"The frame-generation algorithm will be used by the next OpenXR session."
            : nullptr);
        break;
    }
    case ofxr_mode_interpolate:
    case ofxr_mode_hybrid:
    case ofxr_mode_extrapolate: {
        namespace sl = xrfg::standalone;
        const auto mode = command == ofxr_mode_hybrid ? sl::OfxrMode::hybrid
            : command == ofxr_mode_extrapolate ? sl::OfxrMode::extrapolate
            : sl::OfxrMode::interpolate;
        sl::apply_ofxr_mode(state.settings, mode);
        update_runtime_options(state);
        // A menu item cannot carry a tooltip, so the trade goes in the
        // notification, armed or not, as for "Prefer FPS over latency".
        show_balloon(
            state,
            mode == sl::OfxrMode::extrapolate ? L"OFXR: extrapolate"
            : mode == sl::OfxrMode::hybrid ? L"OFXR: vectors + FidelityFX flow"
                                           : L"OFXR: interpolate",
            mode == sl::OfxrMode::extrapolate
                ? L"Each real frame is shown at once and the next predicted from it: a display "
                  L"frame less latency, but predictions err more. Uses FidelityFX flow without "
                  L"DLSS vectors; Prefer FPS over latency is off. From the next game start."
            : mode == sl::OfxrMode::hybrid
                ? L"In games with DLSS, FidelityFX flow runs beside the game's vectors and each "
                  L"pixel takes the better: the least error measured, for about 1 ms more. "
                  L"Games without DLSS keep the optical flow chosen above. From the next game "
                  L"start."
                : L"Frames are generated between two real frames, from the game's DLSS vectors "
                  L"where it has them and the chosen optical flow where not. From the next game "
                  L"start.");
        break;
    }
    case frames_2x:
    case frames_3x:
        if (state.settings.triple_frame_gen != (command == frames_3x)) {
            handle_command(state, toggle_triple_frame_gen);
        }
        break;
    case show_benchmark:
        state.benchmark.show(state.settings);
        break;
    case exit_application:
        SendMessageW(state.window, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == state->taskbar_created_message) {
        static_cast<void>(add_tray_icon(*state));
        return 0;
    }
    switch (message) {
    case kTrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_context_menu(*state);
        } else if (lparam == WM_LBUTTONDBLCLK) {
            handle_command(*state, toggle_arm);
        }
        return 0;
    case WM_COMMAND:
        handle_command(*state, LOWORD(wparam));
        return 0;
    case WM_HOTKEY:
        if (wparam == kPauseHotkeyId) handle_command(*state, toggle_pause);
        return 0;
    case kBenchmarkProgressMessage:
        state->benchmark.on_progress();
        return 0;
    case kBenchmarkFinishedMessage:
        if (const auto notice = state->benchmark.on_finished()) {
            show_balloon(*state, notice->title, notice->message,
                         notice->error ? NIIF_WARNING : NIIF_INFO);
        }
        return 0;
    case WM_CLOSE: {
        std::wstring error;
        if (!disarm_bridge(*state, &error)) {
            show_error(window, error);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    }
    case WM_QUERYENDSESSION: {
        std::wstring error;
        if (!disarm_bridge(*state, &error)) {
            ShutdownBlockReasonCreate(window, L"OFXR Bridge could not disable its OpenXR registrations.");
            return FALSE;
        }
        ShutdownBlockReasonDestroy(window);
        return TRUE;
    }
    case WM_ENDSESSION:
        if (wparam != FALSE) {
            std::wstring error;
            if (!disarm_bridge(*state, &error))
                log_lifecycle(state->local_directory, L"end-session-cleanup", error);
            DestroyWindow(window);
        } else {
            // A cancelled shutdown remains disarmed; never silently re-arm.
            ShutdownBlockReasonDestroy(window);
        }
        return 0;
    case WM_DESTROY: {
        std::wstring error;
        if (!disarm_bridge(*state, &error))
            log_lifecycle(state->local_directory, L"destroy-cleanup", error);
        // A run in progress ends with the tray.
        state->benchmark.shutdown();
        Shell_NotifyIconW(NIM_DELETE, &state->icon);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

[[nodiscard]] bool watch_registered_arm(
    HANDLE parent, const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory, std::wstring* error,
    xrfg::implicit_layer::RegistryScope scope,
    std::wstring_view registry_subkey = xrfg::implicit_layer::kRegistrySubkey,
    const std::filesystem::path& vulkan_manifest = {}) {
    if (!xrfg::implicit_layer::owned_registration_path(manifest, local_directory)) {
        if (error) *error = L"Cleanup watchdog refused an unowned manifest.";
        return false;
    }
    if (!vulkan_manifest.empty() &&
        !xrfg::implicit_layer::owned_registration_path(
            vulkan_manifest, local_directory,
            xrfg::implicit_layer::kVulkanManifestPrefix)) {
        if (error) *error = L"Cleanup watchdog refused an unowned Vulkan manifest.";
        return false;
    }
    while (xrfg::implicit_layer::manifest_registered(manifest, scope, registry_subkey)) {
        if (parent == nullptr || WaitForSingleObject(parent, kArmPollMilliseconds) != WAIT_TIMEOUT)
            break;
    }
    // Scoped to this arm only: an old helper must never revoke a newer arm.
    bool retired = xrfg::implicit_layer::retire_manifest(
        manifest, scope, error, registry_subkey);
    if (!vulkan_manifest.empty()) {
        std::wstring vulkan_error;
        if (!xrfg::implicit_layer::retire_manifest(
                vulkan_manifest, scope, &vulkan_error,
                xrfg::implicit_layer::kVulkanRegistrySubkey)) {
            retired = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += vulkan_error;
            }
        }
    }
    return retired;
}

[[nodiscard]] std::optional<int> run_cleanup_helper() {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) {
        return EXIT_FAILURE;
    }
    if (argument_count < 2 || _wcsicmp(arguments[1], kCleanupArgument) != 0) {
        LocalFree(arguments);
        return std::nullopt;
    }
    if (argument_count != 5 && argument_count != 6) {
        LocalFree(arguments);
        return EXIT_FAILURE;
    }
    const std::filesystem::path manifest(arguments[2]);
    const std::filesystem::path vulkan_manifest(
        argument_count == 6 ? arguments[5] : L"");
    wchar_t* end = nullptr;
    const unsigned long parsed_pid = std::wcstoul(arguments[3], &end, 10);
    const bool valid_pid = end != arguments[3] && end != nullptr && *end == L'\0' &&
                           parsed_pid > 0 && parsed_pid <= MAXDWORD;
    const std::wstring_view scope_argument(arguments[4]);
    const auto scope = _wcsicmp(scope_argument.data(), L"HKLM") == 0
        ? xrfg::implicit_layer::RegistryScope::local_machine
        : xrfg::implicit_layer::RegistryScope::current_user;
    const bool valid_scope = _wcsicmp(scope_argument.data(), L"HKLM") == 0 ||
                             _wcsicmp(scope_argument.data(), L"HKCU") == 0;
    LocalFree(arguments);
    if (!valid_pid || !valid_scope) {
        return EXIT_FAILURE;
    }

    std::filesystem::path directory;
    try {
        directory = runtime_directory(local_app_data() / L"OFXR Bridge");
    } catch (...) {
        return EXIT_FAILURE;
    }
    if (!xrfg::implicit_layer::owned_manifest_path(manifest, directory)) {
        return EXIT_FAILURE;
    }

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parsed_pid));
    std::wstring error;
    const bool cleaned = watch_registered_arm(parent, manifest,
        local_app_data() / L"OFXR Bridge", &error, scope,
        xrfg::implicit_layer::kRegistrySubkey, vulkan_manifest);
    if (parent != nullptr) {
        CloseHandle(parent);
    }
    log_lifecycle(local_app_data() / L"OFXR Bridge", L"watchdog-cleanup", error);
    return cleaned ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (const auto helper_result = run_cleanup_helper()) {
        return *helper_result;
    }

    HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\OFXRBridgeTray");
    if (single_instance == nullptr) {
        return EXIT_FAILURE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge is already running in the notification area.",
            kApplicationName,
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(single_instance);
        return EXIT_SUCCESS;
    }

    AppState state;
    try {
        state.executable_directory = executable_directory();
        state.local_directory = local_app_data() / L"OFXR Bridge";
        state.settings_path = state.local_directory / L"tray.ini";
        std::wstring cleanup_error;
        if (!cleanup_all_owned_registrations(state, &cleanup_error)) {
            log_lifecycle(state.local_directory, L"startup-cleanup", cleanup_error);
            MessageBoxW(nullptr, cleanup_error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
            CloseHandle(single_instance);
            return EXIT_FAILURE;
        }
        log_lifecycle(state.local_directory, L"startup-cleanup");
        load_settings(state);
    } catch (...) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge could not initialize its local configuration.",
            kApplicationName,
            MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }
    state.taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    state.armed_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_OFXR_ARMED));
    state.disarmed_icon = LoadIconW(
        instance, MAKEINTRESOURCEW(IDI_OFXR_DISARMED));
    if (state.armed_icon == nullptr || state.disarmed_icon == nullptr) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge could not load its notification icons.",
            kApplicationName,
            MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hIcon = state.disarmed_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = state.disarmed_icon;
    if (RegisterClassExW(&window_class) == 0) {
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    const HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kApplicationName,
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr || !add_tray_icon(state)) {
        if (window) DestroyWindow(window);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }
    // The last benchmark's results, for the menu's estimates. The tool sits
    // in the ofxr folder beside nvngx_dlssg.dll, which NGX looks for there.
    state.benchmark.initialize(window, state.local_directory,
        state.executable_directory / L"ofxr" / L"OFXRBenchmark.exe",
        kBenchmarkProgressMessage, kBenchmarkFinishedMessage);

    // Arm straight away: launching the tray is the user asking for the
    // bridge. A failure leaves it disarmed with the reason shown, exactly as
    // the menu item would; the menu can retry.
    {
        std::wstring arm_error;
        if (!arm_bridge(state, &arm_error)) {
            show_error(window, arm_error);
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // The benchmark window is modeless: Tab and Enter work in it here.
        if (state.benchmark.translate(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    std::wstring cleanup_error;
    const bool clean_exit = disarm_bridge(state, &cleanup_error);
    if (!clean_exit) log_lifecycle(state.local_directory, L"message-loop-exit", cleanup_error);
    CloseHandle(single_instance);
    return clean_exit ? static_cast<int>(message.wParam) : EXIT_FAILURE;
}
