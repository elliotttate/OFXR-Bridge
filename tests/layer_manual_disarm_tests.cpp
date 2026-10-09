#define main existing_call_chain_main
#include "layer_call_chain.cpp"
#undef main
#include "xrfg/implicit_layer.hpp"
#include <filesystem>

// Read the runtime-facing D3D11 textures, after publication/retirement, not the
// D3D12 staging output or the independently submitted FPS quad.
std::array<std::size_t, 2> marker_pixels(
    const std::array<ComPtr<ID3D11Texture2D>, 3>& images) {
    std::array<std::size_t, 2> counts{};
    for (const auto& image : images) {
        D3D11_TEXTURE2D_DESC source{}; image->GetDesc(&source);
        auto desc = source;
        desc.ArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> readback;
        if (FAILED(g_d3d11_device->CreateTexture2D(&desc, nullptr, &readback)))
            throw std::runtime_error("create marker readback");
        for (UINT eye = 0; eye < 2; ++eye) {
            g_d3d11_context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0,
                image.Get(), D3D11CalcSubresource(0, eye, source.MipLevels), nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(g_d3d11_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                throw std::runtime_error("map marker readback");
            for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
                const auto* p = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                if (p[0] >= 190 && p[0] <= 192 && p[1] == 0 && p[2] == 255 && p[3] == 255) ++counts[eye];
            }
            g_d3d11_context->Unmap(readback.Get(), 0);
        }
    }
    return counts;
}

// Thrown by a scenario that ends before the Disarm sequence, to reach the
// common teardown.
struct FinishedEarly {};

int main(int argc, char** argv) {
    if (argc < 3 || argc > 6) return 1;
    const std::string mode = argv[2];
    // "pause" as the third argument drives the tray's reversible pause in
    // place of the Disarm, and resumes afterwards.
    const bool pause_test = argc == 4 && std::string(argv[3]) == "pause";
    // "frame-loop <runtime> <single|split> [paused]": that on Virtual Desktop
    // a single-threaded session takes the presenter at its first generating
    // frames and a split loop stays inline, that Pimax OpenXR stays inline
    // whatever its pairs measure, and that another runtime still promotes on
    // bunched pairs.
    //
    const bool frame_loop_test = mode == "frame-loop";
    // "recorder": the flight recorder switched on, off and on again while a
    // session runs.
    const bool recorder_test = mode == "recorder";
    // "steamvr-switch": a frame-generation setting changed from the tray while
    // the SteamVR presenter is running with frames queued.
    const bool switch_test = mode == "steamvr-switch";
    if (frame_loop_test && argc != 5 && argc != 6) return 1;
    const std::string loop_runtime = frame_loop_test ? argv[3] : "";
    const bool split_loop = frame_loop_test && std::string(argv[4]) == "split";
    const bool start_paused = frame_loop_test && argc == 6;
    if (loop_runtime == "virtual-desktop") g_runtime_name_override = "VirtualDesktopXR (Bundled)";
    else if (loop_runtime == "pimax") g_runtime_name_override = "Pimax OpenXR";
    else if (loop_runtime == "other") g_runtime_name_override = "XRFG fake runtime";
    else if (frame_loop_test) return 1;
    const std::string marker_mode =
        argc == 4 && !pause_test && !frame_loop_test ? argv[3] : "";
    if (!marker_mode.empty() && (mode != "d3d11" ||
        (marker_mode != "on" && marker_mode != "off" && marker_mode != "hidden"))) return 1;
    g_flight_simulator_mode = mode == "flight";
    g_steamvr_runtime_mode = g_steamvr_presenter_mode = mode == "steamvr" || switch_test;
    g_d3d11_interop_mode = mode == "d3d11";
    // The interop keeps the private rings; every D3D12 mode takes the
    // single-swapchain rings the shipped ini turns on.
    g_single_rings = mode != "d3d11";
    g_test_application_thread_id = GetCurrentThreadId();
    const auto root = std::filesystem::temp_directory_path() /
        (L"ofxr-disarm-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto manifest = root / L"XR_APILAYER_XRFrameBridge_manual-1-1.json";
    HMODULE module = nullptr;
    HANDLE signal = nullptr;
    HANDLE pause_signal = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    PFN_xrEndSession end_session = nullptr;
    PFN_xrDestroySession destroy_session = nullptr;
    PFN_xrDestroySwapchain destroy_swapchain = nullptr;
    PFN_xrDestroyInstance destroy_instance = nullptr;
    const auto require = [](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    int result = 1;
    try {
        std::filesystem::create_directories(root);
        const auto dll = root / L"OptiScaler.dll";
        std::filesystem::copy_file(argv[1], dll);
        signal = xrfg::implicit_layer::create_arm_signal(manifest);
        require(signal != nullptr, "create isolated arm signal");
        if (pause_test || start_paused) {
            pause_signal = xrfg::implicit_layer::create_pause_signal(manifest);
            require(pause_signal != nullptr, "create isolated pause signal");
        }
        if (start_paused) require(SetEvent(pause_signal) != FALSE, "pause before the session");
        const auto control = xrfg::implicit_layer::arm_signal_name(manifest);
        std::string ascii_control;
        for (const wchar_t c : control) ascii_control.push_back(static_cast<char>(c));
        {
            std::ofstream ini(root / L"ofxr_bridge.ini");
            ini << "[ofxr]\nbackend=fidelityfx\ncontrol_event=" << ascii_control
                << "\n[diagnostics]\nlogging_enabled=" << (marker_mode == "on" || marker_mode == "hidden" ? 1 : 0)
                << "\n[overlay]\nposition=" << (!marker_mode.empty() && marker_mode != "hidden" ? "upper_right" : "off") << '\n';
        }
        require(g_d3d11_interop_mode ? initialize_d3d11() : initialize_d3d12(), "initialize GPU fixture");
        if (!marker_mode.empty()) {
            // Deterministic black images: no uninitialized texture can look like
            // a marker, including private slots the fake runtime never acquires.
            for (const auto* images : {&g_d3d11_application_swapchain_images,
                    &g_d3d11_current_swapchain_images, &g_d3d11_current_swapchain_b_images,
                    &g_d3d11_synthetic_swapchain_images, &g_d3d11_synthetic_swapchain_b_images}) {
                for (const auto& image : *images) {
                    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
                    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv.Texture2DArray.ArraySize = 2;
                    ComPtr<ID3D11RenderTargetView> target;
                    require(SUCCEEDED(g_d3d11_device->CreateRenderTargetView(image.Get(), &rtv, &target)), "initialize marker target");
                    constexpr float black[4]{0, 0, 0, 1};
                    g_d3d11_context->ClearRenderTargetView(target.Get(), black);
                }
            }
            require(wait_for_queue_idle(), "initialize marker pixels");
        }
        module = LoadLibraryW(dll.c_str());
        require(module != nullptr, "load exact candidate DLL");
        const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
            GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
        require(negotiate != nullptr, "negotiation export");
        XrNegotiateLoaderInfo loader{};
        loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader.structSize = sizeof(loader);
        loader.minInterfaceVersion = 1;
        loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader.maxApiVersion = XR_CURRENT_API_VERSION;
        XrNegotiateApiLayerRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        require(XR_SUCCEEDED(negotiate(&loader, kLayerName, &request)), "negotiate");
        XrApiLayerNextInfo next{};
        next.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
        next.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
        next.structSize = sizeof(next);
        strcpy_s(next.layerName, kLayerName);
        next.nextGetInstanceProcAddr = fake_get_instance_proc_addr;
        next.nextCreateApiLayerInstance = fake_create_api_layer_instance;
        XrApiLayerCreateInfo layer{};
        layer.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
        layer.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
        layer.structSize = sizeof(layer);
        layer.nextInfo = &next;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        require(XR_SUCCEEDED(request.createApiLayerInstance(&info, &layer, &instance)), "create instance");
        const auto get = request.getInstanceProcAddr;
        const auto create_session = get_layer_function<PFN_xrCreateSession>(get, "xrCreateSession");
        const auto begin_session = get_layer_function<PFN_xrBeginSession>(get, "xrBeginSession");
        end_session = get_layer_function<PFN_xrEndSession>(get, "xrEndSession");
        destroy_session = get_layer_function<PFN_xrDestroySession>(get, "xrDestroySession");
        destroy_instance = get_layer_function<PFN_xrDestroyInstance>(get, "xrDestroyInstance");
        const auto create_swapchain = get_layer_function<PFN_xrCreateSwapchain>(get, "xrCreateSwapchain");
        destroy_swapchain = get_layer_function<PFN_xrDestroySwapchain>(get, "xrDestroySwapchain");
        const auto enumerate = get_layer_function<PFN_xrEnumerateSwapchainImages>(get, "xrEnumerateSwapchainImages");
        const auto acquire = get_layer_function<PFN_xrAcquireSwapchainImage>(get, "xrAcquireSwapchainImage");
        const auto wait_image = get_layer_function<PFN_xrWaitSwapchainImage>(get, "xrWaitSwapchainImage");
        const auto release_image = get_layer_function<PFN_xrReleaseSwapchainImage>(get, "xrReleaseSwapchainImage");
        const auto wait = get_layer_function<PFN_xrWaitFrame>(get, "xrWaitFrame");
        const auto begin = get_layer_function<PFN_xrBeginFrame>(get, "xrBeginFrame");
        const auto end = get_layer_function<PFN_xrEndFrame>(get, "xrEndFrame");
        require(create_session && begin_session && create_swapchain && enumerate && acquire && wait_image && release_image && wait && begin && end,
            "frame functions");
        XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding12.device = g_device.Get(); binding12.queue = g_queue.Get();
        XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding11.device = g_d3d11_device.Get();
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.systemId = 1;
        session_info.next = g_d3d11_interop_mode ? static_cast<void*>(&binding11) : static_cast<void*>(&binding12);
        require(XR_SUCCEEDED(create_session(instance, &session_info, &session)), "create session");
        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        require(XR_SUCCEEDED(begin_session(session, &begin_info)), "begin session");
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = DXGI_FORMAT_R8G8B8A8_UNORM; sc.sampleCount = 1;
        sc.width = sc.height = 4; sc.faceCount = 1; sc.arraySize = 2; sc.mipCount = 1;
        require(XR_SUCCEEDED(create_swapchain(session, &sc, &swapchain)), "create swapchain");
        std::array<XrSwapchainImageD3D12KHR, 3> images12{};
        std::array<XrSwapchainImageD3D11KHR, 3> images11{};
        for (auto& image : images12) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        for (auto& image : images11) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        uint32_t count{};
        require(XR_SUCCEEDED(enumerate(swapchain, 3, &count, g_d3d11_interop_mode
            ? reinterpret_cast<XrSwapchainImageBaseHeader*>(images11.data())
            : reinterpret_cast<XrSwapchainImageBaseHeader*>(images12.data()))), "enumerate images");
        const auto capture = [&] {
            XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; w.timeout = XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; uint32_t index{};
            require(XR_SUCCEEDED(acquire(swapchain, &a, &index)) && XR_SUCCEEDED(wait_image(swapchain, &w)) &&
                XR_SUCCEEDED(release_image(swapchain, &r)), "capture/release image");
        };
        const auto submit = [&](XrTime time) {
            std::array<XrCompositionLayerProjectionView, 2> views{};
            for (uint32_t i = 0; i < 2; ++i) {
                const auto view = fake_submitted_view_for_time(time, i);
                views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                views[i].pose = view.pose; views[i].fov = view.fov;
                views[i].subImage.swapchain = swapchain;
                views[i].subImage.imageRect.extent = {4, 4}; views[i].subImage.imageArrayIndex = i;
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = g_space; projection.viewCount = 2; projection.views = views.data();
            const auto* base = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo e{XR_TYPE_FRAME_END_INFO};
            e.displayTime = time; e.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            e.layerCount = 1; e.layers = &base;
            // The layer runs its inline second cycle from inside this call;
            // the fake runtime throttles that wait and not the application's.
            g_application_in_end_frame.store(true, std::memory_order_release);
            const XrResult end_result = end(session, &e);
            g_application_in_end_frame.store(false, std::memory_order_release);
            require(XR_SUCCEEDED(end_result), "end frame");
        };
        if (recorder_test) {
            const auto ini_path = root / L"ofxr_bridge.ini";
            const auto logs = [&] {
                std::vector<std::filesystem::path> found;
                for (const auto& entry : std::filesystem::directory_iterator(root)) {
                    if (entry.path().extension() == L".log") found.push_back(entry.path());
                }
                std::sort(found.begin(), found.end());
                return found;
            };
            const auto read = [](const std::filesystem::path& path) {
                std::ifstream stream(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(stream), {});
            };
            const auto frame = [&] {
                XrFrameState state{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(wait(session, nullptr, &state)), "recorder wait");
                require(XR_SUCCEEDED(begin(session, nullptr)), "recorder begin");
                capture();
                submit(state.predictedDisplayTime);
                require(wait_for_queue_idle(), "recorder GPU retirement");
            };
            // The recorder reads its setting four times a second and wants
            // two readings alike, so a switch needs wall time, not frames.
            const auto frames_until = [&](const auto& done, const char* what) {
                for (int i = 0; i < 200; ++i) {
                    frame();
                    if (done()) return;
                    Sleep(20);
                }
                throw std::runtime_error(what);
            };
            const auto set_recorder = [&](bool on) {
                require(WritePrivateProfileStringW(L"diagnostics", L"logging_enabled",
                    on ? L"1" : L"0", ini_path.c_str()) != FALSE, "write the recorder setting");
            };
            for (int i = 0; i < 12; ++i) { frame(); Sleep(20); }
            require(logs().empty(), "no file while the recorder is off");

            set_recorder(true);
            frames_until([&] {
                const auto found = logs();
                return found.size() == 1 &&
                    read(found[0]).find("op=recording result=1") != std::string::npos;
            }, "the recorder starts while the session runs");
            // Long enough for a pair's GPU timing to be read back.
            for (int i = 0; i < 16; ++i) { frame(); Sleep(10); }
            const auto first_path = logs().at(0);
            const std::string first = read(first_path);
            const auto switched_on = first.find("op=recording result=1");
            // What the session is, from before the file existed, ahead of
            // the switch; what it does each frame, only after it.
            require(first.rfind("seq=1 ms=0.0", 0) == 0 &&
                    first.find("op=logger result=") < first.find('\n'),
                "the file opens on the process's first record, at time zero");
            for (const char* kept : {"op=runtime_identity", "op=session_binding",
                     "phase=E op=session_create", "op=swapchain_create",
                     "phase=E op=synthesis_initialize"}) {
                const auto at = first.find(kept);
                require(at != std::string::npos && at < switched_on,
                    "a session record from before the switch is ahead of it");
            }
            const auto first_frame = first.find("op=app_end_frame");
            require(first_frame != std::string::npos && first_frame > switched_on,
                "frames are recorded from the switch on and not before");
            // The synthesizer is rebuilt with its GPU timing at the switch,
            // so the timings are in a log that was not on from the start.
            const auto rebuilt = first.find("op=embedded_configuration", switched_on);
            const auto timed = first.find("op=synthesis_gpu_span", switched_on);
            require(rebuilt != std::string::npos && timed != std::string::npos && timed > rebuilt,
                "GPU timings are recorded after the switch");
            const auto millisecond = [&](std::size_t record) {
                const auto line = first.rfind("seq=", record);
                return std::stod(first.substr(first.find("ms=", line) + 3));
            };
            require(millisecond(switched_on) > millisecond(first.find("phase=E op=session_create")) + 200.0,
                "the session records keep the times they happened at");

            set_recorder(false);
            frames_until([&] {
                return read(first_path).find("op=recording result=0") != std::string::npos;
            }, "the recorder stops while the session runs");
            const auto size_when_stopped = std::filesystem::file_size(first_path);
            for (int i = 0; i < 6; ++i) { frame(); Sleep(20); }
            require(std::filesystem::file_size(first_path) == size_when_stopped,
                "nothing is written after the switch off");

            set_recorder(true);
            frames_until([&] {
                const auto found = logs();
                return found.size() == 2 &&
                    read(found[1]).find("op=recording result=1") != std::string::npos;
            }, "a second start makes a second file");
            const auto found = logs();
            const std::string second = read(found[0] == first_path ? found[1] : found[0]);
            require(second.rfind("seq=1 ms=0.0", 0) == 0 &&
                    second.find("op=runtime_identity") != std::string::npos &&
                    second.find("op=recording result=0") != std::string::npos,
                "the second file carries the session records and the earlier switch");
            require(std::filesystem::file_size(first_path) == size_when_stopped,
                "the first file is left as it was closed");
            std::cout << "recorder: " << first.size() << " bytes in the first file, "
                      << switched_on << " of them from before the switch" << std::endl;
            result = 0;
            throw FinishedEarly{};
        }
        if (frame_loop_test) {
            // The split loop: the wait and the begin on this thread, the
            // submission on another, one after the other - No Man's Sky's
            // shape. The fake runtime does not block a wait, so every inline
            // pair is bunched: the evidence that used to promote any session
            // after thirty of them.
            const auto frame = [&](bool expect_native, const char* what) {
                XrFrameState state{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(wait(session, nullptr, &state)), what);
                if (expect_native)
                    require(state.predictedDisplayPeriod == kFakeDisplayPeriod, what);
                require(XR_SUCCEEDED(begin(session, nullptr)), what);
                capture();
                if (split_loop) {
                    std::exception_ptr failure;
                    std::thread render([&] {
                        try { submit(state.predictedDisplayTime); }
                        catch (...) { failure = std::current_exception(); }
                    });
                    render.join();
                    if (failure) std::rethrow_exception(failure);
                } else {
                    submit(state.predictedDisplayTime);
                }
                require(wait_for_queue_idle(), "frame GPU retirement");
                return state.predictedDisplayPeriod;
            };
            if (start_paused) {
                for (int i = 0; i < 8; ++i) frame(true, "paused frame at the native period");
                require(g_synthetic_release_calls.load() == 0, "nothing generated while paused");
                require(ResetEvent(pause_signal) != FALSE, "resume");
            }
            // Past the thirty pairs the bunched-pair detector needs.
            int presenter_at = -1;
            for (int i = 0; i < 45; ++i) {
                const XrDuration period = frame(false, "generating frame");
                if (presenter_at < 0 && period == kFakeDisplayPeriod * 2) presenter_at = i;
            }
            require(g_synthetic_release_calls.load() > 0, "generation ran");
            std::cout << "frame loop " << loop_runtime << (split_loop ? " split" : " single")
                      << (start_paused ? " paused" : "") << ": presenter at frame "
                      << presenter_at << std::endl;
            const bool decided_by_shape = loop_runtime != "other";
            if (!decided_by_shape && start_paused) return 1; // not a scenario
            if (loop_runtime == "virtual-desktop" && !split_loop)
                // Requested on the first generating frame, started by the next,
                // seen by the application at the wait after that.
                require(presenter_at >= 0 && presenter_at <= 6,
                    "a single-threaded session takes the presenter at its first generating frames on Virtual Desktop");
            else if (decided_by_shape)
                require(presenter_at < 0,
                    "stays inline on this runtime whatever the pairs measure");
            else
                require(presenter_at >= 25,
                    "another runtime still promotes on bunched pairs, and not before");
            result = 0;
            throw FinishedEarly{};
        }
        XrFrameState current{XR_TYPE_FRAME_STATE};
        require(XR_SUCCEEDED(wait(session, nullptr, &current)), "first wait");
        // One more warm-up frame than the promotion needs on its own: the frame
        // that arms generation passes through, so the first pair - and the
        // throttled inline cycle the promotion counts - is one frame later.
        for (int i = 0; i < 9; ++i) {
            require(XR_SUCCEEDED(begin(session, nullptr)), "warmup begin");
            capture();
            XrFrameState next_frame{XR_TYPE_FRAME_STATE};
            if (g_flight_simulator_mode) require(XR_SUCCEEDED(wait(session, nullptr, &next_frame)), "overlapping wait");
            submit(current.predictedDisplayTime + (g_flight_simulator_mode ? kFakeDisplayPeriod : 0));
            require(wait_for_queue_idle(), "warmup GPU retirement");
            if (g_flight_simulator_mode) current = next_frame;
            else require(XR_SUCCEEDED(wait(session, nullptr, &current)), "warmup wait");
        }
        require(g_synthetic_release_calls.load() > 0, "generation active before Disarm");
        if (g_steamvr_presenter_mode || g_flight_simulator_mode)
            require(current.predictedDisplayPeriod == kFakeDisplayPeriod * 2, "presenter active before Disarm");
        if (switch_test) {
            // A new preset rebuilds the synthesizer, whose fence restarts its
            // values. Frames the presenter still holds were made by the old one:
            // flushing their real-frame copies against the new fence once had
            // the application's queue wait for a value it would never reach,
            // and SteamVR's xrEndFrame waited on that queue for ever.
            using Request = int (*)(int, int, int, int, int);
            const auto menu_request =
                reinterpret_cast<Request>(GetProcAddress(module, "OFXR_EmbeddedRequestV1"));
            require(menu_request != nullptr, "embedded menu export");
            const auto synthetic_before = g_synthetic_release_calls.load();
            require(menu_request(1, 0, 2, 1, 0) != 0, "request a preset change while generating");
            for (int i = 0; i < 12; ++i) {
                require(XR_SUCCEEDED(begin(session, nullptr)), "switched begin");
                capture();
                submit(current.predictedDisplayTime);
                require(wait_for_queue_idle(), "switched GPU retirement");
                require(XR_SUCCEEDED(wait(session, nullptr, &current)), "switched wait");
            }
            require(g_synthetic_release_calls.load() > synthetic_before,
                "generation continues after a live setting change");
            std::cout << "steamvr-switch: " << g_synthetic_release_calls.load() - synthetic_before
                      << " synthetic frames after the change" << std::endl;
            result = 0;
            throw FinishedEarly{};
        }
#ifdef XRFG_EMBEDDED_MENU_TEST
        using Request = int (*)(int, int, int, int, int);
        auto menu_request = reinterpret_cast<Request>(GetProcAddress(module, "OFXR_EmbeddedRequestV1"));
        require(menu_request != nullptr, "embedded menu export");
        require(menu_request(0, 0, 1, 2, 0) != 0, "request reversible bypass");
#else
        if (pause_test) require(SetEvent(pause_signal) != FALSE, "signal Pause");
        else require(xrfg::implicit_layer::signal_arm_stop(manifest), "signal Disarm");
#endif
        // Begin/end the already-waited frame first. It may carry the old period,
        // but no new synthetic can be prepared after this stop boundary.
        require(XR_SUCCEEDED(begin(session, nullptr)), "stop boundary begin");
        capture(); submit(current.predictedDisplayTime);
        const auto synthetic_after_stop = g_synthetic_release_calls.load();
        std::size_t records_after_stop{};
        { std::scoped_lock lock(g_end_records_mutex); records_after_stop = g_end_records.size(); }
        for (int i = 0; i < 6; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "disarmed wait");
            require(frame.predictedDisplayPeriod == kFakeDisplayPeriod, "native application period after Disarm");
            require(XR_SUCCEEDED(begin(session, nullptr)), "disarmed begin");
            capture(); submit(frame.predictedDisplayTime);
        }
        require(g_synthetic_release_calls.load() == synthetic_after_stop, "no synthesis after Disarm");
        { std::scoped_lock lock(g_end_records_mutex);
          require(g_end_records.size() >= records_after_stop + 6, "original frames continue");
          for (std::size_t i = records_after_stop; i < g_end_records.size(); ++i)
              require(g_end_records[i].target == SubmittedTarget::original, "only originals after Disarm"); }
        require(wait_for_queue_idle(), "final GPU retirement");
        if (pause_test) {
            require(ResetEvent(pause_signal) != FALSE, "signal Resume");
            int presenter_back_at = -1;
            for (int i = 0; i < 10; ++i) {
                XrFrameState frame{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "resumed wait");
                if (presenter_back_at < 0 && frame.predictedDisplayPeriod == kFakeDisplayPeriod * 2)
                    presenter_back_at = i;
                require(XR_SUCCEEDED(begin(session, nullptr)), "resumed begin");
                capture(); submit(frame.predictedDisplayTime);
                require(wait_for_queue_idle(), "resumed GPU retirement");
            }
            require(g_synthetic_release_calls.load() > synthetic_after_stop,
                "synthesis resumes after the pause in the same XR session");
            // A session that had its presenter when it was paused gets it back
            // at the resume, not when the runtime happens to show the evidence
            // again: on a runtime where that evidence is a transient, it never
            // came back and the session stayed inline.
            std::cout << mode << ": presenter back at resumed wait " << presenter_back_at << std::endl;
            if (g_steamvr_presenter_mode)
                require(presenter_back_at >= 0 && presenter_back_at <= 3,
                    "presenter restored straight after the resume");
            // And a second pause stops it again: the switch is not one-shot.
            require(SetEvent(pause_signal) != FALSE, "signal second Pause");
            {
                XrFrameState frame{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "second pause boundary wait");
                require(XR_SUCCEEDED(begin(session, nullptr)), "second pause boundary begin");
                capture(); submit(frame.predictedDisplayTime);
            }
            const auto synthetic_after_second_pause = g_synthetic_release_calls.load();
            for (int i = 0; i < 4; ++i) {
                XrFrameState frame{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "second pause wait");
                require(frame.predictedDisplayPeriod == kFakeDisplayPeriod,
                    "native application period after the second Pause");
                require(XR_SUCCEEDED(begin(session, nullptr)), "second pause begin");
                capture(); submit(frame.predictedDisplayTime);
            }
            require(g_synthetic_release_calls.load() == synthetic_after_second_pause,
                "no synthesis after the second Pause");
            require(wait_for_queue_idle(), "paused GPU retirement");
        }
#ifdef XRFG_EMBEDDED_MENU_TEST
        // Force context recreation with a different options tuple while keeping
        // FidelityFX so this same test runs on WARP and non-NVIDIA machines.
        require(menu_request(1, 0, 2, 1, 1) != 0, "request re-enable and options change");
        for (int i = 0; i < 10; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "re-enabled wait");
            require(XR_SUCCEEDED(begin(session, nullptr)), "re-enabled begin");
            capture(); submit(frame.predictedDisplayTime);
            require(wait_for_queue_idle(), "re-enabled GPU retirement");
        }
        require(g_synthetic_release_calls.load() > synthetic_after_stop, "synthesis resumes in same XR session");
        require(menu_request(1, 8, 2, 1, 1) == 0, "invalid backend rejected");
        const auto before_failure = g_synthetic_release_calls.load();
        require(menu_request(1, 1, 1, 2, 0) != 0, "request unavailable NVIDIA on WARP");
        for (int i = 0; i < 4; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "failed-config wait");
            require(XR_SUCCEEDED(begin(session, nullptr)), "failed-config begin");
            capture(); submit(frame.predictedDisplayTime);
        }
        // D3D12 fixture is WARP; the D3D11 interop fixture uses the real adapter
        // and may successfully run NVIDIA, so it cannot assert unavailability.
        if (!g_d3d11_interop_mode)
            require(g_synthetic_release_calls.load() == before_failure, "failed backend must bypass");
        require(menu_request(1, 0, 1, 2, 0) != 0, "recover FidelityFX after configuration failure");
        for (int i = 0; i < 10; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "recovered wait");
            require(XR_SUCCEEDED(begin(session, nullptr)), "recovered begin");
            capture(); submit(frame.predictedDisplayTime);
            require(wait_for_queue_idle(), "recovered GPU retirement");
        }
        require(g_synthetic_release_calls.load() > before_failure, "same-session recovery after backend failure");
        require(GetPrivateProfileIntW(L"diagnostics", L"logging_enabled", 42, (root / L"ofxr_bridge.ini").c_str()) == 0,
            "menu preserves diagnostic flag");
#endif
        if (!marker_mode.empty()) {
            const auto synthetic = marker_pixels(g_d3d11_synthetic_swapchain_images);
            for (const auto count : synthetic)
                require((count > 0) == (marker_mode == "on"), "D3D11 published marker/debug/Off gate");
            for (const auto* images : {&g_d3d11_application_swapchain_images, &g_d3d11_current_swapchain_images})
                for (const auto count : marker_pixels(*images)) require(count == 0, "marker contaminated an original");
            std::cout << "D3D11 runtime-facing synthetic marker " << marker_mode << ": "
                      << synthetic[0] << '/' << synthetic[1] << " purple pixels; originals clean\n";
        }
        result = 0;
    } catch (const FinishedEarly&) {
    } catch (const std::exception& error) { std::cerr << mode << ": " << error.what() << '\n'; }
    if (session && end_session) end_session(session);
    if (swapchain && destroy_swapchain) destroy_swapchain(swapchain);
    if (session && destroy_session) destroy_session(session);
    if (instance && destroy_instance) destroy_instance(instance);
    if (module) FreeLibrary(module);
    if (signal) CloseHandle(signal);
    if (pause_signal) CloseHandle(pause_signal);
    std::error_code ignored;
    if (root.is_absolute() && root.filename().wstring().starts_with(L"ofxr-disarm-")) std::filesystem::remove_all(root, ignored);
    if (!result) std::cout << "Live Disarm: " << mode << " passed\n";
    return result;
}
