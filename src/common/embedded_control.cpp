#include "xrfg/embedded_control.hpp"
#include "xrfg/implicit_layer.hpp"
#include <windows.h>
#include <map>
#include <mutex>
#include <algorithm>

namespace xrfg::embedded {
namespace {
std::filesystem::path directory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&directory), &module);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    return std::filesystem::path(path).parent_path();
}
const std::filesystem::path& ini() { static auto path = directory() / L"ofxr_bridge.ini"; return path; }
struct Receipt { std::uint64_t revision{}; bool enabled{}; long error{}; };
struct Control {
    std::mutex mutex;
    Settings settings;
    std::uint64_t revision{1}, next_id{1};
    std::map<std::uint64_t, Receipt> sessions;
    Control() {
        const auto options = implicit_layer::read_nvidia_options(directory());
        settings.backend = static_cast<int>(implicit_layer::read_flow_backend(directory()));
        settings.frame_generation = static_cast<int>(implicit_layer::read_frame_generation(directory()));
        settings.preset = static_cast<int>(options.preset);
        settings.scale = static_cast<int>(options.input_scale);
        settings.backward = options.bidirectional;
        settings.enabled = GetPrivateProfileIntW(L"ofxr", L"enabled", 1, ini().c_str()) != 0;
        wchar_t motion[16]{};
        GetPrivateProfileStringW(L"ofxr", L"motion_vectors", L"off", motion, 16, ini().c_str());
        settings.motion_vectors = _wcsicmp(motion, L"dlss") == 0 ? 1 : 0;
    }
};
Control& control() { static Control state; return state; }
bool write(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return WritePrivateProfileStringW(section, key, value, ini().c_str()) != FALSE;
}
constexpr const wchar_t* positions[]{L"off", L"upper_left", L"upper_right", L"lower_left", L"lower_right"};
}
Snapshot snapshot() {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    Snapshot s; s.desired = c.settings; s.revision = c.revision;
    s.sessions = static_cast<unsigned>(c.sessions.size());
    for (auto& [id, r] : c.sessions) {
        if (r.revision != c.revision) ++s.pending;
        else if (r.error) { ++s.errors; s.last_error = r.error; }
        else if (r.enabled) ++s.ready;
        else ++s.bypass;
    }
    return s;
}
bool request(Settings s) {
    if (s.backend < 0 || s.backend > 1 || s.preset < 0 || s.preset > 2 ||
        s.scale < 0 || s.scale > 2 || s.motion_vectors < 0 || s.motion_vectors > 1 ||
        s.frame_generation < 0 || s.frame_generation > 1) return false;
    auto& c = control(); std::scoped_lock lock(c.mutex);
    const auto old = c.settings;
    bool ok = true;
    // Do not rewrite diagnostics, comments, or unrelated configuration.
    if (old.enabled != s.enabled) ok &= write(L"ofxr", L"enabled", s.enabled ? L"1" : L"0");
    if (old.backend != s.backend) ok &= write(L"ofxr", L"backend", s.backend ? L"nvidia" : L"fidelityfx");
    const wchar_t* presets[]{L"slow", L"medium", L"fast"};
    const wchar_t* scales[]{L"100", L"75", L"50"};
    if (old.preset != s.preset) ok &= write(L"ofxr", L"nvidia_preset", presets[s.preset]);
    if (old.scale != s.scale) ok &= write(L"ofxr", L"nvidia_input_scale", scales[s.scale]);
    if (old.backward != s.backward) ok &= write(L"ofxr", L"nvidia_bidirectional", s.backward ? L"1" : L"0");
    if (old.motion_vectors != s.motion_vectors) ok &= write(L"ofxr", L"motion_vectors", s.motion_vectors ? L"dlss" : L"off");
    if (old.frame_generation != s.frame_generation) ok &= write(L"ofxr", L"frame_generation", s.frame_generation ? L"dlss" : L"ofxr");
    if (old != s) { c.settings = s; ++c.revision; }
    return ok;
}
std::uint64_t attach() { auto& c = control(); std::scoped_lock lock(c.mutex); auto id = c.next_id++; c.sessions.emplace(id, Receipt{}); return id; }
void detach(std::uint64_t id) { auto& c = control(); std::scoped_lock lock(c.mutex); c.sessions.erase(id); }
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error) {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    if (auto it = c.sessions.find(id); it != c.sessions.end()) it->second = {revision, enabled, error};
}
bool logging_setting() { return GetPrivateProfileIntW(L"diagnostics", L"logging_enabled", 0, ini().c_str()) != 0; }
bool set_logging_setting(bool enabled) { return write(L"diagnostics", L"logging_enabled", enabled ? L"1" : L"0"); }
int overlay_setting() {
    wchar_t value[32]{}; GetPrivateProfileStringW(L"overlay", L"position", L"upper_right", value, 32, ini().c_str());
    for (int i = 0; i < 5; ++i) if (_wcsicmp(value, positions[i]) == 0) return i;
    return 2;
}
bool set_overlay_setting(int position) { return position >= 0 && position < 5 && write(L"overlay", L"position", positions[position]); }
}

// Private, versioned POD ABI for the combined-module host regression harness.
extern "C" __declspec(dllexport) int OFXR_EmbeddedRequestV1(int enabled, int backend, int preset, int scale, int backward) noexcept {
    try { return xrfg::embedded::request({enabled != 0, backend, preset, scale, backward != 0, 0, 0}) ? 1 : 0; }
    catch (...) { return 0; }
}
