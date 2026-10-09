#include "benchmark_window.hpp"

#include "resource.h"
#include "xrfg/benchmark_labels.hpp"

#include <windows.h>
#include <commctrl.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <fstream>
#include <sstream>

namespace ofxr_tray {
namespace {

using xrfg::benchmark::CaseKind;
using xrfg::benchmark::CaseResult;
using xrfg::benchmark::CaseSpec;
using xrfg::benchmark::CaseStatus;
using xrfg::benchmark::Results;

constexpr wchar_t kResultsFile[] = L"benchmark.ini";
constexpr wchar_t kRunFile[] = L"benchmark-run.ini";
// The tool prints a line at least every case; a case on a slow GPU at a
// large size takes seconds. Silence this long means it hung.
constexpr ULONGLONG kSilenceMilliseconds = 180'000;
constexpr UINT kCancelExitCode = 0xC0FFEE01;
constexpr UINT kHungExitCode = 0xC0FFEE02;
// Posted to the window itself so a refresh-rate pick is read after the
// combo box has shown it.
constexpr UINT kRefreshPicked = WM_APP + 40;

struct Preset {
    const wchar_t* name;
    UINT width;
    UINT height;
};
// What games render each eye at by default on common headsets, not their
// panels' size.
constexpr std::array<Preset, 5> kPresets{{
    {L"Meta Quest 3 (2064 \u00D7 2208)", 2064, 2208},
    {L"Meta Quest 2 (1832 \u00D7 1920)", 1832, 1920},
    {L"Valve Index (2016 \u00D7 2240)", 2016, 2240},
    {L"2448 \u00D7 2448", 2448, 2448},
    {L"Steam Frame (3004 \u00D7 3004)", 3004, 3004},
}};
constexpr LPARAM kDetectedPreset = 1000;
constexpr LPARAM kCustomPreset = 1001;
constexpr std::array<const wchar_t*, 5> kRefreshRates{L"72", L"80", L"90", L"120", L"144"};

enum Group : int { flow_group = 1, vectors_group, native_group, triple_group };
// The table's rows. Labels are short because the group headers already say
// what kind of method each is; percentages are the method's resolution.
struct RowSpec {
    Group group;
    std::string_view key;
    const wchar_t* label;
};
constexpr std::array<RowSpec, 36> kRows{{
    {flow_group, "ffx_50", L"FidelityFX, 50%"},
    {flow_group, "ffx_75", L"FidelityFX, 75%"},
    {flow_group, "ffx_100", L"FidelityFX, 100%"},
    {flow_group, "nv_fast_50", L"NVIDIA fast, 50%"},
    {flow_group, "nv_fast_75", L"NVIDIA fast, 75%"},
    {flow_group, "nv_fast_100", L"NVIDIA fast, 100%"},
    {flow_group, "nv_medium_50", L"NVIDIA medium, 50% (default)"},
    {flow_group, "nv_medium_75", L"NVIDIA medium, 75%"},
    {flow_group, "nv_medium_100", L"NVIDIA medium, 100%"},
    {flow_group, "nv_slow_50", L"NVIDIA slow, 50%"},
    {flow_group, "nv_slow_75", L"NVIDIA slow, 75%"},
    {flow_group, "nv_slow_100", L"NVIDIA slow, 100%"},
    {flow_group, "nv_fast_50_bidi", L"NVIDIA fast, 50%, both ways"},
    {flow_group, "nv_fast_75_bidi", L"NVIDIA fast, 75%, both ways"},
    {flow_group, "nv_fast_100_bidi", L"NVIDIA fast, 100%, both ways"},
    {flow_group, "nv_medium_50_bidi", L"NVIDIA medium, 50%, both ways"},
    {flow_group, "nv_medium_75_bidi", L"NVIDIA medium, 75%, both ways"},
    {flow_group, "nv_medium_100_bidi", L"NVIDIA medium, 100%, both ways"},
    {flow_group, "nv_slow_50_bidi", L"NVIDIA slow, 50%, both ways"},
    {flow_group, "nv_slow_75_bidi", L"NVIDIA slow, 75%, both ways"},
    {flow_group, "nv_slow_100_bidi", L"NVIDIA slow, 100%, both ways"},
    {flow_group, "extrapolate_ffx_50", L"Extrapolation, FidelityFX, 50%"},
    {flow_group, "extrapolate_ffx_75", L"Extrapolation, FidelityFX, 75%"},
    {flow_group, "extrapolate_ffx_100", L"Extrapolation, FidelityFX, 100%"},
    {vectors_group, "vectors", L"Motion vectors (default)"},
    {vectors_group, "hybrid_50", L"Vectors + FidelityFX flow, 50%"},
    {vectors_group, "hybrid_75", L"Vectors + FidelityFX flow, 75%"},
    {vectors_group, "hybrid_100", L"Vectors + FidelityFX flow, 100%"},
    {vectors_group, "extrapolate", L"Extrapolation, SpaceWarp-style"},
    {native_group, "native_100", L"DLSS Frame Generation, 100%"},
    {native_group, "native_67", L"DLSS Frame Generation, 67% (default)"},
    {native_group, "native_50", L"DLSS Frame Generation, 50%"},
    {triple_group, "ffx_50_3x", L"OFXR, FidelityFX, 50%"},
    {triple_group, "native_100_3x", L"DLSS Frame Generation, 100%"},
    {triple_group, "native_67_3x", L"DLSS Frame Generation, 67%"},
    {triple_group, "native_50_3x", L"DLSS Frame Generation, 50%"},
}};

[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring output(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), size);
    }
    return output;
}

[[nodiscard]] std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(),
                            size, nullptr, nullptr);
    }
    return output;
}

// Up to `limit` bytes of a file another process may be writing.
[[nodiscard]] std::string read_shared(const std::filesystem::path& path, DWORD limit) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text(limit, '\0');
    DWORD read = 0;
    if (!ReadFile(file, text.data(), limit, &read, nullptr)) read = 0;
    CloseHandle(file);
    text.resize(read);
    return text;
}

[[nodiscard]] bool write_atomic(const std::filesystem::path& path, const std::string& text) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream) return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::wstring local_time_text(const FILETIME& time) {
    FILETIME local{};
    SYSTEMTIME parts{};
    if (!FileTimeToLocalFileTime(&time, &local) || !FileTimeToSystemTime(&local, &parts)) return {};
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"%04u-%02u-%02u %02u:%02u", parts.wYear, parts.wMonth,
                  parts.wDay, parts.wHour, parts.wMinute);
    return buffer;
}

[[nodiscard]] std::wstring size_text(std::uint32_t width, std::uint32_t height) {
    return std::to_wstring(width) + L" \u00D7 " + std::to_wstring(height);
}

[[nodiscard]] std::wstring hz_text(double hz) {
    wchar_t buffer[24]{};
    std::swprintf(buffer, std::size(buffer), std::abs(hz - std::round(hz)) < 0.05 ? L"%.0f Hz" : L"%.1f Hz",
                  hz);
    return buffer;
}

[[nodiscard]] std::wstring format(const wchar_t* pattern, double value) {
    wchar_t buffer[48]{};
    std::swprintf(buffer, std::size(buffer), pattern, value);
    return buffer;
}

// The graphics card games run on, as DXGI names it.
[[nodiscard]] std::optional<std::pair<std::string, std::array<std::uint64_t, 3>>> query_adapter() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())))) return std::nullopt;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                       IID_PPV_ARGS(adapter.GetAddressOf())))) {
            return std::nullopt;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        LARGE_INTEGER version{};
        if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) version.QuadPart = 0;
        return std::pair{narrow(description.Description),
                         std::array<std::uint64_t, 3>{description.VendorId, description.DeviceId,
                                                      static_cast<std::uint64_t>(version.QuadPart)}};
    }
}

} // namespace

BenchmarkController::~BenchmarkController() {
    shutdown();
}

void BenchmarkController::initialize(HWND tray_window, std::filesystem::path local_directory,
                                     std::filesystem::path tool, UINT progress_message,
                                     UINT finished_message) {
    tray_window_ = tray_window;
    local_directory_ = std::move(local_directory);
    tool_ = std::move(tool);
    progress_message_ = progress_message;
    finished_message_ = finished_message;
    Results stored = xrfg::benchmark::parse_results(
        read_shared(local_directory_ / kResultsFile, 1U << 20));
    if (stored.format == Results::kFormat && !stored.cases.empty()) results_ = std::move(stored);
    try {
        if (const auto adapter = query_adapter()) {
            AdapterIdentity identity;
            identity.name = adapter->first;
            identity.vendor_id = static_cast<std::uint32_t>(adapter->second[0]);
            identity.device_id = static_cast<std::uint32_t>(adapter->second[1]);
            identity.driver = xrfg::benchmark::driver_version_text(adapter->second[2]);
            adapter_ = identity;
        }
    } catch (...) {
        adapter_.reset();
    }
}

const Results* BenchmarkController::results() const noexcept {
    return results_ ? &*results_ : nullptr;
}

bool BenchmarkController::results_for_this_gpu() const noexcept {
    if (!results_) return false;
    if (!adapter_) return true;
    return results_->gpu == adapter_->name && results_->vendor_id == adapter_->vendor_id &&
           results_->device_id == adapter_->device_id;
}

double BenchmarkController::refresh_hz() const noexcept {
    return results_ ? results_->refresh_hz : 90.0;
}

bool BenchmarkController::running() const noexcept {
    return process_ != nullptr;
}

std::wstring BenchmarkController::menu_hint() const {
    if (running()) {
        return case_count_ ? L"running, " + std::to_wstring(std::min(cases_done_ + 1, case_count_)) +
                                 L" of " + std::to_wstring(case_count_)
                           : L"running";
    }
    if (!results_) return L"not run yet";
    if (!results_for_this_gpu()) return L"results are for another GPU";
    return size_text(results_->eye_width, results_->eye_height) + L" at " + hz_text(results_->refresh_hz);
}

void BenchmarkController::show(const xrfg::standalone::LauncherSettings& settings) {
    settings_ = settings;
    if (dialog_ != nullptr) {
        ShowWindow(dialog_, IsIconic(dialog_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(dialog_);
        return;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),
                                  ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HWND dialog = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BENCHMARK),
                                           nullptr, dialog_procedure, reinterpret_cast<LPARAM>(this));
    if (dialog == nullptr) return;
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
}

void BenchmarkController::settings_changed(const xrfg::standalone::LauncherSettings& settings) {
    settings_ = settings;
    if (dialog_ != nullptr) populate_list();
}

bool BenchmarkController::translate(MSG& message) {
    return dialog_ != nullptr && IsDialogMessageW(dialog_, &message) != FALSE;
}

INT_PTR CALLBACK BenchmarkController::dialog_procedure(HWND dialog, UINT message, WPARAM wparam,
                                                       LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        auto* self = reinterpret_cast<BenchmarkController*>(lparam);
        self->dialog_ = dialog;
        self->initialize_dialog();
        return TRUE;
    }
    auto* self = reinterpret_cast<BenchmarkController*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (self == nullptr) return FALSE;
    return self->handle_dialog(message, wparam, lparam);
}

INT_PTR BenchmarkController::handle_dialog(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const UINT id = LOWORD(wparam);
        const UINT code = HIWORD(wparam);
        if (id == IDC_BENCH_RUN && code == BN_CLICKED) {
            start();
        } else if (id == IDC_BENCH_STOP && code == BN_CLICKED) {
            stop();
        } else if (id == IDCANCEL) {
            DestroyWindow(dialog_);
        } else if (id == IDC_BENCH_PRESET && code == CBN_SELCHANGE) {
            apply_preset();
        } else if ((id == IDC_BENCH_WIDTH || id == IDC_BENCH_HEIGHT) && code == EN_CHANGE) {
            if (!updating_controls_) {
                match_preset();
                update_summary();
            }
        } else if (id == IDC_BENCH_REFRESH && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) {
            PostMessageW(dialog_, kRefreshPicked, 0, 0);
        }
        return TRUE;
    }
    case kRefreshPicked:
        refresh_rate_changed();
        return TRUE;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom != IDC_BENCH_LIST || header->code != NM_CUSTOMDRAW) return FALSE;
        auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
        LRESULT result = CDRF_DODEFAULT;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
            result = CDRF_NOTIFYITEMDRAW;
        } else if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            // lParam bit 0: in use, bold; bit 1: cannot run here, grey.
            if ((draw->nmcd.lItemlParam & 2) != 0) draw->clrText = GetSysColor(COLOR_GRAYTEXT);
            if ((draw->nmcd.lItemlParam & 1) != 0 && bold_font_ != nullptr) {
                SelectObject(draw->nmcd.hdc, bold_font_);
                result = CDRF_NEWFONT;
            } else if ((draw->nmcd.lItemlParam & 2) != 0) {
                result = CDRF_NEWFONT;
            }
        }
        SetWindowLongPtrW(dialog_, DWLP_MSGRESULT, result);
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(dialog_);
        return TRUE;
    case WM_DESTROY:
        if (bold_font_ != nullptr) {
            DeleteObject(bold_font_);
            bold_font_ = nullptr;
        }
        dialog_ = nullptr;
        return TRUE;
    default:
        return FALSE;
    }
}

void BenchmarkController::initialize_dialog() {
    if (HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_OFXR_DISARMED))) {
        SendMessageW(dialog_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(dialog_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    if (HFONT font = reinterpret_cast<HFONT>(SendMessageW(dialog_, WM_GETFONT, 0, 0))) {
        LOGFONTW description{};
        if (GetObjectW(font, sizeof(description), &description) != 0) {
            description.lfWeight = FW_BOLD;
            bold_font_ = CreateFontIndirectW(&description);
        }
    }

    // The table: one row per measured configuration, grouped by what games
    // can use it.
    const HWND list = GetDlgItem(dialog_, IDC_BENCH_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    RECT client{};
    GetClientRect(list, &client);
    const int width = client.right - client.left - GetSystemMetrics(SM_CXVSCROLL);
    const struct {
        const wchar_t* title;
        int percent;
        int format;
    } columns[]{
        {L"Method", 25, LVCFMT_LEFT},      {L"GPU time", 8, LVCFMT_RIGHT},
        {L"Budget", 7, LVCFMT_RIGHT},      {L"Game needs", 9, LVCFMT_RIGHT},
        {L"Speed-up", 10, LVCFMT_RIGHT},   {L"Quality", 12, LVCFMT_LEFT},
        {L"Latency", 7, LVCFMT_RIGHT},     {L"Notes", 22, LVCFMT_LEFT},
    };
    for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[index].title);
        column.cx = width * columns[index].percent / 100;
        column.fmt = columns[index].format;
        column.iSubItem = index;
        ListView_InsertColumn(list, index, &column);
    }
    ListView_EnableGroupView(list, TRUE);
    const struct {
        Group id;
        const wchar_t* header;
    } groups[]{
        {flow_group, L"Games without DLSS vectors: OFXR from optical flow"},
        {vectors_group, L"Games with DLSS vectors: OFXR from the game's motion vectors"},
        {native_group, L"Games with DLSS: NVIDIA DLSS Frame Generation"},
        {triple_group, L"3X Frame Gen: two generated frames per game frame"},
    };
    for (const auto& group : groups) {
        LVGROUP entry{};
        entry.cbSize = sizeof(entry);
        entry.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        entry.pszHeader = const_cast<wchar_t*>(group.header);
        entry.iGroupId = group.id;
        entry.state = LVGS_NORMAL;
        ListView_InsertGroup(list, -1, &entry);
    }

    // Where the headset's size comes from: what the layer last wrote for it,
    // else the newest flight-recorder log, which records the runtime's
    // recommended size and refresh at every session start.
    detected_.reset();
    detected_source_.clear();
    try {
        const std::string ini = read_shared(local_directory_ / L"headset.ini", 64 * 1024);
        if (auto guess = xrfg::benchmark::headset_from_ini(ini)) {
            detected_ = guess;
            detected_source_ = L"from your last VR session";
        } else {
            std::filesystem::path newest;
            FILETIME newest_time{};
            std::error_code error;
            for (const auto& version : std::filesystem::directory_iterator(
                     local_directory_ / L"RuntimeLayer", error)) {
                if (!version.is_directory(error)) continue;
                for (const auto& file : std::filesystem::directory_iterator(version.path(), error)) {
                    const std::wstring name = file.path().filename().wstring();
                    if (name.rfind(L"ofxr-bridge-flight-", 0) != 0 || file.path().extension() != L".log") continue;
                    WIN32_FILE_ATTRIBUTE_DATA attributes{};
                    if (!GetFileAttributesExW(file.path().c_str(), GetFileExInfoStandard, &attributes)) continue;
                    if (newest.empty() || CompareFileTime(&attributes.ftLastWriteTime, &newest_time) > 0) {
                        newest = file.path();
                        newest_time = attributes.ftLastWriteTime;
                    }
                }
            }
            if (!newest.empty()) {
                if (auto logged = xrfg::benchmark::headset_from_flight_log(read_shared(newest, 4U << 20))) {
                    detected_ = logged;
                    detected_source_ = L"from the flight recorder's last session (" +
                                       local_time_text(newest_time) + L")";
                }
            }
        }
    } catch (...) {
        detected_.reset();
    }

    const HWND presets = GetDlgItem(dialog_, IDC_BENCH_PRESET);
    const auto add_preset = [&](const std::wstring& name, LPARAM data) {
        const auto index = SendMessageW(presets, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        SendMessageW(presets, CB_SETITEMDATA, static_cast<WPARAM>(index), data);
    };
    if (detected_) {
        add_preset(L"Your headset (" + size_text(detected_->eye_width, detected_->eye_height) + L")",
                   kDetectedPreset);
    }
    for (std::size_t index = 0; index < kPresets.size(); ++index) {
        add_preset(kPresets[index].name, static_cast<LPARAM>(index));
    }
    add_preset(L"Custom", kCustomPreset);
    const HWND refresh = GetDlgItem(dialog_, IDC_BENCH_REFRESH);
    for (const auto* rate : kRefreshRates) {
        SendMessageW(refresh, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rate));
    }
    SendMessageW(GetDlgItem(dialog_, IDC_BENCH_WIDTH), EM_SETLIMITTEXT, 5, 0);
    SendMessageW(GetDlgItem(dialog_, IDC_BENCH_HEIGHT), EM_SETLIMITTEXT, 5, 0);
    SendMessageW(GetDlgItem(dialog_, IDC_BENCH_PROGRESS), PBM_SETRANGE32, 0, 1);

    // Start from the detected headset, else the last run's size, else a
    // Quest 3; and from the detected refresh, else the stored one.
    std::uint32_t eye_width = 2064, eye_height = 2208;
    double hz = results_ ? results_->refresh_hz : 90.0;
    if (detected_) {
        eye_width = detected_->eye_width;
        eye_height = detected_->eye_height;
        if (detected_->refresh_hz) hz = *detected_->refresh_hz;
    } else if (results_ && results_->eye_width >= 64 && results_->eye_height >= 64) {
        eye_width = results_->eye_width;
        eye_height = results_->eye_height;
    }
    updating_controls_ = true;
    SetDlgItemInt(dialog_, IDC_BENCH_WIDTH, eye_width, FALSE);
    SetDlgItemInt(dialog_, IDC_BENCH_HEIGHT, eye_height, FALSE);
    SetWindowTextW(refresh, format(L"%.0f", hz).c_str());
    updating_controls_ = false;
    match_preset();
    SetDlgItemTextW(dialog_, IDC_BENCH_DETECTED,
        detected_
            ? (L"Detected " + detected_source_ + L": " +
               size_text(detected_->eye_width, detected_->eye_height) + L" per eye" +
               (detected_->refresh_hz ? L" at " + hz_text(*detected_->refresh_hz) : std::wstring()) + L".")
                  .c_str()
            : L"No VR session recorded yet: pick your headset, or type the resolution games render "
              L"each eye at (Diagnostics > Bridge flight recorder records it next time).");
    if (results_ && detected_ && detected_->refresh_hz &&
        std::abs(*detected_->refresh_hz - results_->refresh_hz) > 0.5) {
        refresh_rate_changed();
    }
    populate_list();
    update_summary();
    update_controls();
}

void BenchmarkController::apply_preset() {
    const HWND presets = GetDlgItem(dialog_, IDC_BENCH_PRESET);
    const auto selection = SendMessageW(presets, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return;
    const LPARAM data = SendMessageW(presets, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
    std::uint32_t width = 0, height = 0;
    if (data == kDetectedPreset && detected_) {
        width = detected_->eye_width;
        height = detected_->eye_height;
    } else if (data >= 0 && data < static_cast<LPARAM>(kPresets.size())) {
        width = kPresets[static_cast<std::size_t>(data)].width;
        height = kPresets[static_cast<std::size_t>(data)].height;
    } else {
        SetFocus(GetDlgItem(dialog_, IDC_BENCH_WIDTH));
        return;
    }
    updating_controls_ = true;
    SetDlgItemInt(dialog_, IDC_BENCH_WIDTH, width, FALSE);
    SetDlgItemInt(dialog_, IDC_BENCH_HEIGHT, height, FALSE);
    updating_controls_ = false;
    update_summary();
}

void BenchmarkController::match_preset() {
    const UINT width = GetDlgItemInt(dialog_, IDC_BENCH_WIDTH, nullptr, FALSE);
    const UINT height = GetDlgItemInt(dialog_, IDC_BENCH_HEIGHT, nullptr, FALSE);
    const HWND presets = GetDlgItem(dialog_, IDC_BENCH_PRESET);
    const auto count = SendMessageW(presets, CB_GETCOUNT, 0, 0);
    LRESULT match = CB_ERR, custom = CB_ERR;
    for (LRESULT index = 0; index < count; ++index) {
        const LPARAM data = SendMessageW(presets, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
        if (data == kCustomPreset) custom = index;
        const bool same = data == kDetectedPreset
            ? detected_ && detected_->eye_width == width && detected_->eye_height == height
            : data >= 0 && data < static_cast<LPARAM>(kPresets.size()) &&
                  kPresets[static_cast<std::size_t>(data)].width == width &&
                  kPresets[static_cast<std::size_t>(data)].height == height;
        if (same && match == CB_ERR) match = index;
    }
    SendMessageW(presets, CB_SETCURSEL, static_cast<WPARAM>(match != CB_ERR ? match : custom), 0);
}

std::optional<double> BenchmarkController::entered_refresh() const {
    if (dialog_ == nullptr) return std::nullopt;
    wchar_t text[16]{};
    GetDlgItemTextW(dialog_, IDC_BENCH_REFRESH, text, static_cast<int>(std::size(text)));
    wchar_t* end = nullptr;
    const double hz = std::wcstod(text, &end);
    if (end == text || !(hz >= 30.0 && hz <= 500.0)) return std::nullopt;
    return hz;
}

void BenchmarkController::refresh_rate_changed() {
    const auto hz = entered_refresh();
    if (!hz) return;
    if (results_ && std::abs(results_->refresh_hz - *hz) > 1e-6) {
        results_->refresh_hz = *hz;
        save_results();
    }
    if (live_) live_->refresh_hz = *hz;
    populate_list();
    update_summary();
}

const Results* BenchmarkController::shown_results() const noexcept {
    if (live_) return &*live_;
    return results_ ? &*results_ : nullptr;
}

void BenchmarkController::populate_list() {
    if (dialog_ == nullptr) return;
    namespace sl = xrfg::standalone;
    const HWND list = GetDlgItem(dialog_, IDC_BENCH_LIST);
    const Results* shown = shown_results();
    const double hz = entered_refresh().value_or(shown ? shown->refresh_hz : 90.0);
    const double snapshot_ms = shown ? shown->cost_ms(xrfg::benchmark::kGuideSnapshotKey).value_or(0.0) : 0.0;
    const auto triple_base = shown ? shown->cost_ms(xrfg::benchmark::kOfxrTripleBaseKey) : std::nullopt;
    const auto triple_reference = shown ? shown->cost_ms(xrfg::benchmark::kOfxrTripleReferenceKey) : std::nullopt;
    const sl::Method method = sl::current_method(settings_);
    const sl::OfxrMode mode = sl::ofxr_mode(settings_);
    const bool ofxr = method != sl::Method::native_dlss;
    const int flow_scale = sl::input_scale_percent(settings_.nvidia_input_scale);

    // What runs with the current settings: in a game without DLSS vectors
    // the chosen flow, or FidelityFX under the hybrid, or FidelityFX
    // extrapolation; in a game with them the OFXR mode's own case.
    const auto in_use = [&](const CaseSpec& spec) {
        if ((spec.generated_frames >= 2) != settings_.triple_frame_gen) return false;
        switch (spec.kind) {
        case CaseKind::flow:
            if (!ofxr || mode == sl::OfxrMode::extrapolate || spec.input_scale != flow_scale) return false;
            if (spec.backend == xrfg::benchmark::CaseBackend::fidelity_fx) {
                return method == sl::Method::fidelity_fx || mode == sl::OfxrMode::hybrid;
            }
            return method != sl::Method::fidelity_fx && mode == sl::OfxrMode::interpolate &&
                   spec.bidirectional == settings_.nvidia_bidirectional &&
                   spec.preset == (method == sl::Method::nvidia_fast ? xrfg::benchmark::CasePreset::fast
                                   : method == sl::Method::nvidia_slow ? xrfg::benchmark::CasePreset::slow
                                                                        : xrfg::benchmark::CasePreset::medium);
        case CaseKind::flow_extrapolate:
            return ofxr && mode == sl::OfxrMode::extrapolate && spec.input_scale == flow_scale;
        case CaseKind::vectors:
            return ofxr && mode == sl::OfxrMode::interpolate;
        case CaseKind::hybrid:
            return ofxr && mode == sl::OfxrMode::hybrid && spec.input_scale == flow_scale;
        case CaseKind::extrapolate:
            return ofxr && mode == sl::OfxrMode::extrapolate;
        case CaseKind::native:
            return !ofxr && spec.native_scale == settings_.native_scale;
        default:
            return false;
        }
    };

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    int index = 0;
    const auto add_row = [&](const RowSpec& row) {
        const CaseSpec* spec = xrfg::benchmark::find_case(row.key);
        if (spec == nullptr) return;
        const CaseResult* result = shown ? shown->find(row.key) : nullptr;
        const int frames = spec->generated_frames >= 2 ? 3 : 2;
        std::array<std::wstring, 8> cells;
        cells[0] = row.label;
        std::wstring notes;
        const bool used = in_use(*spec);
        if (used) {
            notes = spec->kind == CaseKind::native ? L"in use"
                : xrfg::benchmark::uses_game_guides(spec->kind) ? L"in use with DLSS vectors"
                                                                : L"in use without DLSS vectors";
        }
        LPARAM flags = used ? 1 : 0;
        if (result != nullptr && result->status == CaseStatus::ok) {
            const double cost = result->median_us / 1000.0 + (xrfg::benchmark::uses_game_guides(spec->kind) ? snapshot_ms : 0.0);
            const auto estimate = xrfg::benchmark::estimate(cost, hz, frames);
            cells[1] = xrfg::benchmark::format_ms(cost);
            cells[2] = format(L"%.1f%%", estimate.budget_share * 100.0);
            if (estimate.fits) {
                cells[3] = format(L"%.0f fps", std::ceil(estimate.min_game_fps - 1e-9));
                cells[4] = format(L"up to +%.0f%%", estimate.max_gain * 100.0);
            } else {
                cells[3] = L"\u2014";
                cells[4] = L"too slow";
            }
        } else if (result != nullptr && result->status == CaseStatus::unavailable) {
            cells[1] = L"not available";
            flags |= 2;
            if (!result->note.empty()) notes += (notes.empty() ? L"" : L"; ") + widen(result->note);
        } else if (result != nullptr && result->status == CaseStatus::failed) {
            cells[1] = L"failed";
            flags |= 2;
            if (!result->note.empty()) notes += (notes.empty() ? L"" : L"; ") + widen(result->note);
        } else if (running() && current_case_ == row.key) {
            cells[1] = L"measuring\u2026";
        }
        if (const auto error = xrfg::benchmark::recorded_error(*spec)) {
            cells[5] = format(L"%.2f ", *error) + xrfg::benchmark::quality_word(*error);
        } else {
            cells[5] = xrfg::benchmark::extrapolates(spec->kind) ? L"lower (predicts)" : L"\u2014";
        }
        const int latency_frames = xrfg::benchmark::added_latency_frames(spec->kind, frames);
        cells[6] = latency_frames == 0 ? L"none" : format(L"+%.1f ms", latency_frames * 1000.0 / hz);
        if (row.key == xrfg::benchmark::kOfxrTripleReferenceKey && triple_base && triple_reference) {
            notes += (notes.empty() ? L"" : L"; ") +
                     (L"other OFXR methods: +" +
                      xrfg::benchmark::format_ms(std::max(*triple_reference - *triple_base, 0.0)) +
                      L" over 2X");
        }
        cells[7] = notes;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
        item.iItem = index++;
        item.iGroupId = row.group;
        item.pszText = cells[0].data();
        item.lParam = flags;
        const int at = ListView_InsertItem(list, &item);
        if (at < 0) return;
        for (int column = 1; column < static_cast<int>(cells.size()); ++column) {
            ListView_SetItemText(list, at, column, cells[static_cast<std::size_t>(column)].data());
        }
    };
    for (const auto& row : kRows) add_row(row);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);

    const double period = 1000.0 / hz;
    std::wstring explanation =
        L"At " + hz_text(hz) + L" with 2X, the game renders every other frame the headset shows, so it has " +
        xrfg::benchmark::format_ms(2 * period) + L" per frame instead of " + xrfg::benchmark::format_ms(period) +
        L"; frame generation's GPU time comes out of that (Budget). Game needs: the frame rate the game "
        L"must reach by itself, without frame generation, for the headset to get every frame. Speed-up: "
        L"the gain for a game at exactly that rate; a faster game gains less (nothing goes past " +
        hz_text(hz) + L") and a slower one cannot hold " + hz_text(hz) + L" with that method. 3X is the "
        L"same with " + xrfg::benchmark::format_ms(3 * period) + L" per game frame. Interpolation shows each "
        L"real frame later (Latency); extrapolation adds no delay but predicts, so it errs more, and uses "
        L"FidelityFX flow where a game has no DLSS vectors. Quality: "
        L"error on 42 recorded Galactic Racer frames where the scene moved, lower is better (a plain blend: "
        L"13.0). Games-with-DLSS rows include " + xrfg::benchmark::format_ms(snapshot_ms) +
        L" per game frame to copy the game's motion vectors and depth. Measured on test frames with nothing "
        L"else running; in a game, which shares the GPU, expect somewhat more.";
    SetDlgItemTextW(dialog_, IDC_BENCH_EXPLAIN, explanation.c_str());
}

void BenchmarkController::update_summary() {
    if (dialog_ == nullptr) return;
    std::wstring text;
    if (running()) {
        text = live_ && !live_->gpu.empty() ? L"Measuring on " + widen(live_->gpu) + L"\u2026"
                                            : L"Starting\u2026";
    } else if (results_) {
        const std::string nvidia = xrfg::benchmark::nvidia_driver_text(results_->vendor_id, results_->driver);
        text = L"Results: " + widen(results_->gpu) + L", driver " +
               widen(nvidia.empty() ? results_->driver : nvidia) + L", " +
               size_text(results_->eye_width, results_->eye_height) + L" per eye, " + widen(results_->date);
        if (!results_->complete) text += L" (incomplete)";
        const UINT width = GetDlgItemInt(dialog_, IDC_BENCH_WIDTH, nullptr, FALSE);
        const UINT height = GetDlgItemInt(dialog_, IDC_BENCH_HEIGHT, nullptr, FALSE);
        if (!results_for_this_gpu()) {
            text += L". Measured on another graphics card: run it again.";
        } else if (adapter_ && adapter_->driver != results_->driver) {
            text += L". The driver has changed since.";
        } else if (width != results_->eye_width || height != results_->eye_height) {
            text += L". Run it again for the resolution above.";
        } else if (results_->drifted()) {
            text += format(L". Other GPU work changed the times by %.0f%% during the run: close it and "
                           L"run again.", (results_->drift - 1.0) * 100.0);
        }
    } else {
        text = L"No results yet: choose your headset and press Run benchmark.";
    }
    SetDlgItemTextW(dialog_, IDC_BENCH_SUMMARY, text.c_str());
}

void BenchmarkController::update_controls() {
    if (dialog_ == nullptr) return;
    const bool busy = running();
    EnableWindow(GetDlgItem(dialog_, IDC_BENCH_RUN), !busy);
    EnableWindow(GetDlgItem(dialog_, IDC_BENCH_STOP), busy);
    for (const int id : {IDC_BENCH_PRESET, IDC_BENCH_WIDTH, IDC_BENCH_HEIGHT}) {
        EnableWindow(GetDlgItem(dialog_, id), !busy);
    }
    const HWND progress = GetDlgItem(dialog_, IDC_BENCH_PROGRESS);
    SendMessageW(progress, PBM_SETRANGE32, 0, static_cast<LPARAM>(std::max<std::size_t>(case_count_, 1)));
    SendMessageW(progress, PBM_SETPOS, busy ? static_cast<WPARAM>(cases_done_) : 0, 0);
    SetDlgItemTextW(dialog_, IDC_BENCH_STATUS, status_.c_str());
}

void BenchmarkController::save_results() {
    if (results_) {
        static_cast<void>(write_atomic(local_directory_ / kResultsFile,
                                       xrfg::benchmark::serialize_results(*results_)));
    }
}

void BenchmarkController::start() {
    if (running() || dialog_ == nullptr) return;
    const UINT width = GetDlgItemInt(dialog_, IDC_BENCH_WIDTH, nullptr, FALSE);
    const UINT height = GetDlgItemInt(dialog_, IDC_BENCH_HEIGHT, nullptr, FALSE);
    const auto hz = entered_refresh();
    if (width < 64 || height < 64 || width > 8192 || height > 8192) {
        status_ = L"Enter a per-eye resolution from 64 to 8192 pixels.";
        update_controls();
        return;
    }
    if (!hz) {
        status_ = L"Enter a refresh rate from 30 to 500 Hz.";
        update_controls();
        return;
    }
    std::error_code missing;
    if (!std::filesystem::is_regular_file(tool_, missing)) {
        status_ = L"OFXRBenchmark.exe is missing from the ofxr folder.";
        update_controls();
        return;
    }
    const auto run_file = local_directory_ / kRunFile;
    std::error_code ignored;
    std::filesystem::create_directories(local_directory_, ignored);
    std::filesystem::remove(run_file, ignored);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        status_ = L"The benchmark could not be started (no pipe).";
        update_controls();
        return;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = xrfg::standalone::quote_windows_argument(tool_.wstring()) + L" --eye " +
                           std::to_wstring(width) + L"x" + std::to_wstring(height) + L" --refresh " +
                           format(L"%.1f", *hz) + L" --output " +
                           xrfg::standalone::quote_windows_argument(run_file.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(tool_.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        tool_.parent_path().c_str(), &startup, &process);
    CloseHandle(write);
    if (!created) {
        CloseHandle(read);
        status_ = L"OFXRBenchmark.exe could not be started.";
        update_controls();
        return;
    }
    // A job that ends the benchmark with the tray, however the tray ends.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_ != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job_, process.hProcess)) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    pipe_ = read;
    hung_ = false;
    cancelled_ = false;
    exit_code_ = 0;
    case_count_ = 0;
    cases_done_ = 0;
    current_case_.clear();
    error_line_.clear();
    {
        std::scoped_lock lock(mutex_);
        lines_.clear();
    }
    Results live;
    live.eye_width = width;
    live.eye_height = height;
    live.refresh_hz = *hz;
    live_ = std::move(live);
    status_ = L"Starting\u2026";
    reader_ = std::thread([this] { reader_loop(); });
    populate_list();
    update_summary();
    update_controls();
}

void BenchmarkController::stop() {
    if (!running()) return;
    cancelled_ = true;
    TerminateProcess(process_, kCancelExitCode);
    status_ = L"Stopping\u2026";
    update_controls();
}

// On its own thread: reads the tool's lines without ever blocking on the
// pipe, so a hung tool is noticed and a stop is immediate.
void BenchmarkController::reader_loop() {
    std::string pending;
    std::array<char, 4096> chunk{};
    ULONGLONG last_output = GetTickCount64();
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) break; // the tool exited
        if (available > 0) {
            DWORD read = 0;
            if (!ReadFile(pipe_, chunk.data(), std::min<DWORD>(available, static_cast<DWORD>(chunk.size())),
                          &read, nullptr) || read == 0) {
                break;
            }
            last_output = GetTickCount64();
            pending.append(chunk.data(), read);
            std::vector<std::string> complete;
            for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
                std::string line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                complete.push_back(std::move(line));
                pending.erase(0, newline + 1);
            }
            if (!complete.empty()) {
                {
                    std::scoped_lock lock(mutex_);
                    for (auto& line : complete) lines_.push_back(std::move(line));
                }
                PostMessageW(tray_window_, progress_message_, 0, 0);
            }
            continue;
        }
        if (WaitForSingleObject(process_, 50) == WAIT_OBJECT_0) continue; // drain, then the pipe breaks
        if (GetTickCount64() - last_output > kSilenceMilliseconds) {
            hung_ = true;
            TerminateProcess(process_, kHungExitCode);
        }
    }
    if (!pending.empty()) {
        std::scoped_lock lock(mutex_);
        lines_.push_back(pending);
    }
    WaitForSingleObject(process_, 10'000);
    DWORD code = 0;
    exit_code_ = GetExitCodeProcess(process_, &code) ? code : 1;
    PostMessageW(tray_window_, progress_message_, 0, 0);
    PostMessageW(tray_window_, finished_message_, 0, 0);
}

void BenchmarkController::handle_line(const std::string& line) {
    std::istringstream words(line);
    std::string verb;
    words >> verb;
    const auto rest = [&] {
        std::string text;
        std::getline(words, text);
        const auto start = text.find_first_not_of(' ');
        return start == std::string::npos ? std::string() : text.substr(start);
    };
    if (verb == "count") {
        words >> case_count_;
    } else if (verb == "begin") {
        std::size_t position = 0;
        words >> position >> current_case_;
        cases_done_ = position > 0 ? position - 1 : 0;
        const CaseSpec* spec = xrfg::benchmark::find_case(current_case_);
        status_ = L"Measuring " + (spec ? widen(spec->name) : widen(current_case_)) + L" (" +
                  std::to_wstring(position) + L" of " + std::to_wstring(case_count_) + L")\u2026";
    } else if (verb == "result") {
        CaseResult result;
        std::string status;
        words >> result.key >> status >> result.median_us;
        result.status = xrfg::benchmark::parse_status(status);
        result.note = rest();
        if (live_) live_->set(std::move(result));
        ++cases_done_;
        // The tool measures its first case once more at the end.
        if (case_count_ > 1 && cases_done_ >= case_count_) {
            status_ = L"Checking that nothing else used the GPU\u2026";
        }
    } else if (verb == "adapter") {
        std::uint32_t vendor = 0, device = 0;
        std::string driver;
        words >> vendor >> device >> driver;
        if (live_) {
            live_->vendor_id = vendor;
            live_->device_id = device;
            live_->driver = driver;
            live_->gpu = rest();
        }
    } else if (verb == "error") {
        error_line_ = rest();
    } else if (verb == "done") {
        status_ = L"Saving the results\u2026";
    }
}

void BenchmarkController::on_progress() {
    std::vector<std::string> lines;
    {
        std::scoped_lock lock(mutex_);
        lines.swap(lines_);
    }
    if (lines.empty()) return;
    bool result_arrived = false;
    for (const auto& line : lines) {
        handle_line(line);
        result_arrived = result_arrived || line.rfind("result ", 0) == 0 || line.rfind("begin ", 0) == 0;
    }
    if (result_arrived) populate_list();
    update_summary();
    update_controls();
}

std::optional<BenchmarkNotice> BenchmarkController::on_finished() {
    if (!running()) return std::nullopt;
    if (reader_.joinable()) reader_.join();
    on_progress();
    const DWORD code = exit_code_;
    const bool cancelled = cancelled_;
    const bool hung = hung_;
    close_run_handles();
    const auto run_file = local_directory_ / kRunFile;
    std::optional<BenchmarkNotice> notice;
    std::error_code ignored;
    if (cancelled) {
        status_ = results_ ? L"Stopped; the earlier results are kept." : L"Stopped.";
    } else {
        Results run = xrfg::benchmark::parse_results(read_shared(run_file, 1U << 20));
        const bool usable = run.format == Results::kFormat && !run.cases.empty();
        if (code == 0 && usable && run.complete) {
            results_ = std::move(run);
            save_results();
            status_ = L"Finished.";
            notice = results_->drifted()
                ? BenchmarkNotice{L"Benchmark finished, but the GPU was busy",
                                  L"Something else used the graphics card during the run, so the times "
                                  L"are uncertain. Close games and other GPU work and run it again.",
                                  true}
                : BenchmarkNotice{L"Benchmark finished",
                                  L"Each method in the OFXR Bridge menu now shows what it costs on this PC.",
                                  false};
        } else {
            wchar_t exit_text[16]{};
            std::swprintf(exit_text, std::size(exit_text), L"0x%08X", static_cast<unsigned>(code));
            std::wstring reason = hung ? L"The graphics card stopped responding"
                : !error_line_.empty() ? widen(error_line_)
                                       : L"OFXRBenchmark stopped unexpectedly (exit code " +
                                             std::wstring(exit_text) + L")";
            while (!reason.empty() && (reason.back() == L'.' || reason.back() == L' ')) reason.pop_back();
            if (usable) {
                // Keep what it measured; the case it was in is what failed.
                if (!run.last_started.empty()) {
                    const CaseResult* last = run.find(run.last_started);
                    if (last == nullptr || last->status == CaseStatus::not_run) {
                        run.set({run.last_started, CaseStatus::failed, 0, 0, 0,
                                 "The benchmark stopped in this case."});
                    }
                }
                run.complete = false;
                results_ = std::move(run);
                save_results();
                reason += L". The results measured before it are kept.";
            } else {
                reason += L".";
            }
            status_ = reason;
            notice = BenchmarkNotice{L"Benchmark stopped early", reason, true};
        }
    }
    std::filesystem::remove(run_file, ignored);
    live_.reset();
    current_case_.clear();
    populate_list();
    update_summary();
    update_controls();
    return notice;
}

void BenchmarkController::close_run_handles() noexcept {
    if (pipe_ != nullptr) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        CloseHandle(job_);
        job_ = nullptr;
    }
}

void BenchmarkController::shutdown() noexcept {
    try {
        if (process_ != nullptr) {
            cancelled_ = true;
            TerminateProcess(process_, kCancelExitCode);
        }
        if (reader_.joinable()) reader_.join();
        close_run_handles();
        live_.reset();
        if (dialog_ != nullptr) DestroyWindow(dialog_);
    } catch (...) {
    }
}

} // namespace ofxr_tray
