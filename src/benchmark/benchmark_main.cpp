// OFXRBenchmark: "Benchmark this PC". The tray runs it as a child process,
// so a driver fault in a case cannot take the tray (and its arm) with it,
// and reads its progress from standard output. It also runs by hand:
//
//   OFXRBenchmark.exe --eye 3004x3004 [--refresh 90] [--output results.ini]
//                     [--cases ffx_50,native_67] [--warmup 8] [--pairs 32]
//                     [--warp] [--list]
//
// Standard output, one line each, flushed as it happens:
//   ofxr-benchmark 1
//   adapter <vendor id> <device id> <driver> <name>
//   count <cases>
//   begin <1-based index> <key>
//   result <key> <ok|unavailable|failed> <median us> <note>
//   drift <ratio>             (the first case again, over its first time)
//   done                      (every case answered; exit code 0)
//   error <message>           (the run stopped; exit code 2)
// The results file is rewritten after every case, complete=0 until the
// last, so a run that dies keeps what it measured and names the case it
// was in.

#include "xrfg/benchmark_model.hpp"
#include "xrfg/fg_benchmark.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::string utf8(std::wstring_view text) {
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

void emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

[[nodiscard]] bool write_atomic(const std::filesystem::path& path, const std::string& text) {
    if (path.empty()) return true;
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] std::string local_date() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth,
                  time.wDay, time.wHour, time.wMinute);
    return buffer;
}

void usage() {
    std::fputs("usage: OFXRBenchmark --eye WIDTHxHEIGHT [--refresh HZ] [--output FILE]\n"
               "                     [--cases KEY,KEY] [--warmup N] [--pairs N] [--warp] [--list]\n",
               stderr);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    xrfg::fg_benchmark::Options options;
    options.module_directory = executable_directory();
    std::filesystem::path output;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        const bool has_value = index + 1 < argc;
        if (argument == L"--list") {
            for (const auto& spec : xrfg::benchmark::standard_cases()) {
                emit(std::string(spec.key) + " " + std::string(spec.name));
            }
            return 0;
        } else if (argument == L"--warp") {
            options.use_warp = true;
        } else if (argument == L"--eye" && has_value) {
            unsigned width = 0, height = 0;
            if (swscanf_s(argv[++index], L"%ux%u", &width, &height) != 2) {
                usage();
                return 1;
            }
            options.eye_width = width;
            options.eye_height = height;
        } else if (argument == L"--refresh" && has_value) {
            options.refresh_hz = std::wcstod(argv[++index], nullptr);
            if (!(options.refresh_hz >= 30.0 && options.refresh_hz <= 500.0)) options.refresh_hz = 90.0;
        } else if (argument == L"--output" && has_value) {
            output = argv[++index];
        } else if (argument == L"--warmup" && has_value) {
            options.warmup_pairs = static_cast<std::uint32_t>(std::wcstoul(argv[++index], nullptr, 10));
        } else if (argument == L"--pairs" && has_value) {
            options.measured_pairs = std::max<std::uint32_t>(
                static_cast<std::uint32_t>(std::wcstoul(argv[++index], nullptr, 10)), 4);
        } else if (argument == L"--cases" && has_value) {
            const std::string list = utf8(argv[++index]);
            std::size_t offset = 0;
            while (offset <= list.size()) {
                const std::size_t comma = list.find(',', offset);
                const std::string key = list.substr(offset, comma == std::string::npos
                                                                ? std::string::npos
                                                                : comma - offset);
                if (!key.empty()) options.keys.push_back(key);
                if (comma == std::string::npos) break;
                offset = comma + 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    xrfg::benchmark::Results results;
    results.eye_width = options.eye_width;
    results.eye_height = options.eye_height;
    results.refresh_hz = options.refresh_hz;
    results.date = local_date();
    emit("ofxr-benchmark 1");

    xrfg::fg_benchmark::Callbacks callbacks;
    callbacks.adapter = [&](const xrfg::fg_benchmark::AdapterInfo& adapter) {
        results.gpu = utf8(adapter.name);
        results.vendor_id = adapter.vendor_id;
        results.device_id = adapter.device_id;
        results.driver = xrfg::benchmark::driver_version_text(adapter.driver_version);
        emit("adapter " + std::to_string(adapter.vendor_id) + " " + std::to_string(adapter.device_id) +
             " " + results.driver + " " + results.gpu);
    };
    callbacks.begin = [&](std::size_t index, std::size_t count, const xrfg::benchmark::CaseSpec& spec) {
        if (index == 0) emit("count " + std::to_string(count));
        results.last_started = std::string(spec.key);
        static_cast<void>(write_atomic(output, xrfg::benchmark::serialize_results(results)));
        emit("begin " + std::to_string(index + 1) + " " + std::string(spec.key));
    };
    callbacks.result = [&](const xrfg::benchmark::CaseResult& result) {
        results.set(result);
        static_cast<void>(write_atomic(output, xrfg::benchmark::serialize_results(results)));
        char median[32]{};
        std::snprintf(median, sizeof(median), "%.1f", result.median_us);
        emit("result " + result.key + " " + std::string(xrfg::benchmark::status_name(result.status)) +
             " " + median + (result.note.empty() ? "" : " " + result.note));
    };

    callbacks.drift = [&](double ratio) {
        results.drift = ratio;
        char text[32]{};
        std::snprintf(text, sizeof(text), "%.3f", ratio);
        emit(std::string("drift ") + text);
    };

    std::wstring error;
    const HRESULT run = xrfg::fg_benchmark::run(options, callbacks, &error);
    if (FAILED(run)) {
        if (!results.cases.empty() || !results.gpu.empty()) {
            static_cast<void>(write_atomic(output, xrfg::benchmark::serialize_results(results)));
        }
        emit("error " + utf8(error));
        return 2;
    }
    results.complete = true;
    results.last_started.clear();
    if (!write_atomic(output, xrfg::benchmark::serialize_results(results))) {
        emit("error The results could not be saved to " + utf8(output.wstring()));
        return 2;
    }
    emit("done");
    return 0;
}
