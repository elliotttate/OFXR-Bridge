#pragma once

// "Benchmark this PC" in the tray: runs OFXRBenchmark.exe as a child process
// and reads its progress on a thread of its own, so the tray never waits on
// the GPU; keeps the results in %LOCALAPPDATA%\OFXR Bridge\benchmark.ini;
// and shows them in a modeless window.

#include "xrfg/benchmark_model.hpp"
#include "xrfg/standalone_launcher.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ofxr_tray {

struct BenchmarkNotice {
    std::wstring title;
    std::wstring message;
    bool error{};
};

class BenchmarkController {
public:
    BenchmarkController() = default;
    ~BenchmarkController();
    BenchmarkController(const BenchmarkController&) = delete;
    BenchmarkController& operator=(const BenchmarkController&) = delete;

    // `progress_message` and `finished_message` are posted to `tray_window`
    // from the reader thread; the tray hands them to on_progress and
    // on_finished. Loads the stored results and identifies this PC's GPU.
    void initialize(HWND tray_window, std::filesystem::path local_directory,
                    std::filesystem::path tool, UINT progress_message, UINT finished_message);

    // The stored results, if there are any in this format.
    [[nodiscard]] const xrfg::benchmark::Results* results() const noexcept;
    // Whether they were measured on the graphics card this PC has now.
    [[nodiscard]] bool results_for_this_gpu() const noexcept;
    // The refresh rate the estimates are for: the results', or 90 Hz.
    [[nodiscard]] double refresh_hz() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    // The menu entry's right-hand text: what the results are for, or the
    // run's progress.
    [[nodiscard]] std::wstring menu_hint() const;

    // Opens the window, or brings it forward.
    void show(const xrfg::standalone::LauncherSettings& settings);
    // Marks the methods in use afresh, if the window is open.
    void settings_changed(const xrfg::standalone::LauncherSettings& settings);
    // For the tray's message loop: the window's keyboard navigation.
    [[nodiscard]] bool translate(MSG& message);
    void on_progress();
    // What to tell the user about a finished run, when there is something.
    [[nodiscard]] std::optional<BenchmarkNotice> on_finished();
    // Stops a run and closes the window. Safe to call more than once.
    void shutdown() noexcept;

private:
    struct AdapterIdentity {
        std::string name;
        std::uint32_t vendor_id{};
        std::uint32_t device_id{};
        std::string driver;
    };

    static INT_PTR CALLBACK dialog_procedure(HWND dialog, UINT message, WPARAM wparam,
                                             LPARAM lparam);
    INT_PTR handle_dialog(UINT message, WPARAM wparam, LPARAM lparam);
    void initialize_dialog();
    void apply_preset();
    void match_preset();
    void refresh_rate_changed();
    void populate_list();
    void update_summary();
    void update_controls();
    void start();
    void stop();
    void reader_loop();
    void handle_line(const std::string& line);
    [[nodiscard]] std::optional<double> entered_refresh() const;
    [[nodiscard]] const xrfg::benchmark::Results* shown_results() const noexcept;
    void save_results();
    void close_run_handles() noexcept;

    HWND tray_window_{};
    HWND dialog_{};
    std::filesystem::path local_directory_;
    std::filesystem::path tool_;
    UINT progress_message_{};
    UINT finished_message_{};
    std::optional<xrfg::benchmark::Results> results_;
    std::optional<AdapterIdentity> adapter_;
    xrfg::standalone::LauncherSettings settings_;
    bool updating_controls_{};
    // The headset as the last recorded session or headset.ini tells it.
    std::optional<xrfg::benchmark::HeadsetGuess> detected_;
    std::wstring detected_source_;
    HFONT bold_font_{};

    // The run in progress.
    HANDLE process_{};
    HANDLE job_{};
    HANDLE pipe_{};
    std::thread reader_;
    std::atomic<bool> hung_{};
    std::mutex mutex_;
    std::vector<std::string> lines_;
    DWORD exit_code_{};
    bool cancelled_{};
    std::size_t case_count_{};
    std::size_t cases_done_{};
    std::string current_case_;
    std::string error_line_;
    std::wstring status_;
    // The results as they arrive, shown while the run goes on.
    std::optional<xrfg::benchmark::Results> live_;
};

} // namespace ofxr_tray
