#include "xrfg/provider_api.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace {

std::filesystem::path g_runtime_ini;
std::optional<std::string> g_runtime_ini_original;

void restore_runtime_ini() {
    std::error_code error;
    if (g_runtime_ini.empty()) {
        return;
    }
    if (g_runtime_ini_original) {
        std::ofstream output(g_runtime_ini, std::ios::binary | std::ios::trunc);
        output.write(g_runtime_ini_original->data(),
                     static_cast<std::streamsize>(g_runtime_ini_original->size()));
    } else {
        std::filesystem::remove(g_runtime_ini, error);
    }
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "provider API test failed: " << message << '\n';
        std::exit(1);
    }
}

template <typename Function>
Function resolve(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    require(address != nullptr, name);
    return reinterpret_cast<Function>(address);
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "layer DLL path argument");
    g_runtime_ini = std::filesystem::path(argv[1]).parent_path() /
                    L"ofxr_bridge.ini";
    if (std::filesystem::exists(g_runtime_ini)) {
        std::ifstream input(g_runtime_ini, std::ios::binary);
        g_runtime_ini_original.emplace(std::istreambuf_iterator<char>(input),
                                       std::istreambuf_iterator<char>());
    }
    std::atexit(restore_runtime_ini);
    const HMODULE module = LoadLibraryA(argv[1]);
    require(module != nullptr, "load layer DLL");

    using GetVersion = std::uint32_t (*)() noexcept;
    using GetControl = int (*)(OFXR_ControlSnapshotV1*) noexcept;
    using RequestControl = int (*)(const OFXR_ControlSettingsV1*) noexcept;
    using GetStatistics = int (*)(OFXR_DlssMotionVectorStatisticsV1*) noexcept;
    using RegisterV2 = int (*)(
        const OFXR_OptiScalerProviderIdentityV2*) noexcept;
    using UnregisterV2 = void (*)() noexcept;
    using GetControlV2 = int (*)(OFXR_ControlSnapshotV2*) noexcept;
    using RequestControlV2 = int (*)(
        const OFXR_ControlSettingsV2*) noexcept;
    using GetStatisticsV2 = int (*)(OFXR_DlssGuideStatisticsV2*) noexcept;
    using PublishGuidesV2 = int (*)(
        const OFXR_DlssGuidePublicationV2*) noexcept;

    const auto get_version = resolve<GetVersion>(
        module, "OFXR_GetProviderApiVersionV1");
    const auto get_control = resolve<GetControl>(
        module, "OFXR_GetControlSnapshotV1");
    const auto request_control = resolve<RequestControl>(
        module, "OFXR_RequestControlV1");
    const auto get_statistics = resolve<GetStatistics>(
        module, "OFXR_GetDlssMotionVectorStatisticsV1");
    const auto get_version_v2 = resolve<GetVersion>(
        module, "OFXR_GetProviderApiVersionV2");
    const auto register_v2 = resolve<RegisterV2>(
        module, "OFXR_RegisterOptiScalerProviderV2");
    const auto unregister_v2 = resolve<UnregisterV2>(
        module, "OFXR_UnregisterOptiScalerProviderV2");
    const auto get_control_v2 = resolve<GetControlV2>(
        module, "OFXR_GetControlSnapshotV2");
    const auto request_control_v2 = resolve<RequestControlV2>(
        module, "OFXR_RequestControlV2");
    const auto get_statistics_v2 = resolve<GetStatisticsV2>(
        module, "OFXR_GetDlssGuideStatisticsV2");
    const auto publish_guides_v2 = resolve<PublishGuidesV2>(
        module, "OFXR_PublishDlssGuidesV2");

    require(get_version() == OFXR_PROVIDER_API_VERSION_V1, "API version");
    require(GetProcAddress(module, "OFXR_OptiScalerProviderV1") == nullptr,
        "provider marker belongs only to the cooperating OptiScaler DLL");

    OFXR_ControlSnapshotV1 snapshot{};
    require(get_control(&snapshot) != 0, "control snapshot");
    require(snapshot.api_version == OFXR_PROVIDER_API_VERSION_V1,
        "control snapshot version");
    snapshot.struct_size = sizeof(snapshot) - 1;
    require(get_control(&snapshot) == 0, "undersized control rejected");

    OFXR_ControlSettingsV1 invalid{};
    invalid.backend = 9;
    require(request_control(&invalid) == 0, "invalid control rejected");

    OFXR_DlssMotionVectorStatisticsV1 statistics{};
    require(get_statistics(&statistics) != 0, "statistics snapshot");
    require(statistics.api_version == OFXR_PROVIDER_API_VERSION_V1,
        "statistics version");

    require(get_version_v2() == OFXR_PROVIDER_API_VERSION_V2,
        "V2 API version");
    OFXR_OptiScalerProviderIdentityV2 identity{};
    identity.capabilities = OFXR_OPTISCALER_CAP_DLSS_GUIDES_V2 |
        OFXR_OPTISCALER_CAP_IN_GAME_MENU_V2;
    auto invalid_identity = identity;
    invalid_identity.magic = OFXR_OPTISCALER_PROVIDER_MAGIC_V1;
    require(register_v2(&invalid_identity) == 0,
        "V2 provider marker rejects wrong magic");
    require(register_v2(&identity) != 0, "V2 provider registration");

    OFXR_ControlSnapshotV2 snapshot_v2{};
    require(get_control_v2(&snapshot_v2) != 0, "V2 control snapshot");
    require(snapshot_v2.api_version == OFXR_PROVIDER_API_VERSION_V2 &&
            snapshot_v2.providers == 1,
        "V2 provider count/version");
    require(snapshot_v2.desired.frame_generation == 0,
        "standalone frame generation remains OFXR");
    auto dlss_settings = snapshot_v2.desired;
    dlss_settings.frame_generation = 1;
    require(request_control_v2(&dlss_settings) != 0, "native DLSS FG option accepted");
    OFXR_ControlSnapshotV2 dlss_snapshot{};
    require(get_control_v2(&dlss_snapshot) != 0 && dlss_snapshot.desired.frame_generation == 1,
        "native DLSS FG option retained");
    require(request_control_v2(&snapshot_v2.desired) != 0, "restore OFXR generation");
    auto unsupported_settings = snapshot_v2.desired;
    unsupported_settings.frame_generation = 2;
    require(request_control_v2(&unsupported_settings) == 0,
        "external frame-generation request rejected");

    OFXR_DlssGuidePublicationV2 legacy_publication{};
    legacy_publication.struct_size = static_cast<std::uint32_t>(
        offsetof(OFXR_DlssGuidePublicationV2, verified_native_device));
    require(publish_guides_v2(&legacy_publication) != 0,
        "legacy V2 guide prefix accepted");
    --legacy_publication.struct_size;
    require(publish_guides_v2(&legacy_publication) == 0,
        "undersized V2 guide prefix rejected");
    OFXR_DlssGuidePublicationV2 extended_publication{};
    require(publish_guides_v2(&extended_publication) != 0,
        "extended V2 guide publication accepted");

    OFXR_DlssGuideStatisticsV2 statistics_v2{};
    require(get_statistics_v2(&statistics_v2) != 0,
        "V2 guide statistics snapshot");
    require(statistics_v2.api_version == OFXR_PROVIDER_API_VERSION_V2,
        "V2 statistics version");
    unregister_v2();
    snapshot_v2 = {};
    require(get_control_v2(&snapshot_v2) != 0 &&
            snapshot_v2.providers == 0,
        "V2 provider unregister");

    FreeLibrary(module);
    std::cout << "provider API test passed\n";
    return 0;
}
