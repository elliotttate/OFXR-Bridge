#pragma once

#include <d3d12.h>

#include <cstdint>

// Versioned process-local ABI used by optional producers such as OptiScaler.
// The OpenXR layer owns all state and GPU snapshot lifetime; producers only
// publish inputs and send UI requests while both modules share one process.
constexpr std::uint32_t OFXR_PROVIDER_API_VERSION_V1 = 1;
constexpr std::uint32_t OFXR_PROVIDER_API_VERSION_V2 = 2;
// ASCII "OFXROPT1". This identity deliberately does not contain a product or
// file version: stock OptiScaler builds can never satisfy this contract by
// merely sharing a filename or changing their version resource.
constexpr std::uint64_t OFXR_OPTISCALER_PROVIDER_MAGIC_V1 =
    0x4F4658524F505431ULL;
// ASCII "OFXROPT2". This permanent marker identifies an OFXR-aware
// OptiScaler independently of its filename and product/file version.
constexpr std::uint64_t OFXR_OPTISCALER_PROVIDER_MAGIC_V2 =
    0x4F4658524F505432ULL;

enum OFXR_OptiScalerProviderCapabilityV1 : std::uint64_t {
    OFXR_OPTISCALER_CAP_DLSS_MOTION_VECTORS_V1 = 1ULL << 0,
    OFXR_OPTISCALER_CAP_IN_GAME_MENU_V1 = 1ULL << 1,
};

struct OFXR_OptiScalerProviderIdentityV1 {
    std::uint32_t struct_size{sizeof(OFXR_OptiScalerProviderIdentityV1)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V1};
    std::uint64_t magic{OFXR_OPTISCALER_PROVIDER_MAGIC_V1};
    std::uint64_t capabilities{};
};

enum OFXR_OptiScalerProviderCapabilityV2 : std::uint64_t {
    OFXR_OPTISCALER_CAP_DLSS_GUIDES_V2 = 1ULL << 0,
    OFXR_OPTISCALER_CAP_IN_GAME_MENU_V2 = 1ULL << 1,
    OFXR_OPTISCALER_CAP_FSR_FRAME_GENERATION_V2 = 1ULL << 2,
};

struct OFXR_OptiScalerProviderIdentityV2 {
    std::uint32_t struct_size{sizeof(OFXR_OptiScalerProviderIdentityV2)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V2};
    std::uint64_t magic{OFXR_OPTISCALER_PROVIDER_MAGIC_V2};
    std::uint64_t capabilities{};
};

struct OFXR_ControlSettingsV1 {
    std::uint32_t struct_size{sizeof(OFXR_ControlSettingsV1)};
    std::int32_t enabled{1};
    std::int32_t backend{};
    std::int32_t preset{1};
    std::int32_t scale{2};
    std::int32_t backward{};
    std::int32_t motion_vectors{};
};

struct OFXR_ControlSnapshotV1 {
    std::uint32_t struct_size{sizeof(OFXR_ControlSnapshotV1)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V1};
    OFXR_ControlSettingsV1 desired{};
    std::uint64_t revision{};
    std::uint32_t sessions{};
    std::uint32_t pending{};
    std::uint32_t bypass{};
    std::uint32_t ready{};
    std::uint32_t errors{};
    std::int32_t last_error{};
};

struct OFXR_ControlSettingsV2 {
    std::uint32_t struct_size{sizeof(OFXR_ControlSettingsV2)};
    std::int32_t enabled{1};
    std::int32_t backend{};
    std::int32_t preset{1};
    std::int32_t scale{2};
    std::int32_t backward{};
    std::int32_t motion_vectors{};
    std::int32_t frame_generation{}; // 0 OFXR, 1 native NVIDIA DLSS FG
};

struct OFXR_ControlSnapshotV2 {
    std::uint32_t struct_size{sizeof(OFXR_ControlSnapshotV2)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V2};
    OFXR_ControlSettingsV2 desired{};
    std::uint64_t revision{};
    std::uint32_t sessions{};
    std::uint32_t pending{};
    std::uint32_t bypass{};
    std::uint32_t ready{};
    std::uint32_t errors{};
    std::int32_t last_error{};
    std::uint32_t providers{};
};

struct OFXR_DlssMotionVectorPublicationV1 {
    std::uint32_t struct_size{sizeof(OFXR_DlssMotionVectorPublicationV1)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V1};
    std::uint64_t stream{};
    ID3D12Resource* output{};
    ID3D12Resource* motion_vectors{};
    ID3D12CommandQueue* producer_queue{};
    ID3D12GraphicsCommandList* producer_command_list{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    std::uint32_t resource_state{
        static_cast<std::uint32_t>(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    std::int32_t jittered{};
    std::int32_t reset{};
};

struct OFXR_DlssMotionVectorStatisticsV1 {
    std::uint32_t struct_size{sizeof(OFXR_DlssMotionVectorStatisticsV1)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V1};
    std::uint64_t published{};
    std::uint64_t invalid_publications{};
    std::uint64_t snapshot_copies{};
    std::uint64_t snapshot_failures{};
    std::uint64_t resolve_calls{};
    std::uint64_t resolve_missing_streams{};
    std::uint64_t resolve_stale_pairs{};
    std::uint64_t resolve_queue_mismatches{};
    std::uint64_t matched{};
    std::uint64_t temporal_rejections{};
    std::uint64_t invalid_rejections{};
    std::uint64_t used{};
    std::uint64_t last_used_publication{};
    std::uint32_t status{};
};

struct OFXR_DlssGuidePublicationV2 {
    std::uint32_t struct_size{sizeof(OFXR_DlssGuidePublicationV2)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V2};
    std::uint64_t stream{};
    ID3D12Resource* output{};
    ID3D12Resource* motion_vectors{};
    ID3D12CommandQueue* producer_queue{};
    ID3D12GraphicsCommandList* producer_command_list{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    std::uint32_t resource_state{
        static_cast<std::uint32_t>(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    std::int32_t jittered{};
    std::int32_t reset{};
    ID3D12Resource* depth{};
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    std::uint32_t depth_resource_state{
        static_cast<std::uint32_t>(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    float frame_time_delta_ms{16.6667F};
    float camera_near{0.1F};
    float camera_far{1000.0F};
    std::int32_t depth_inverted{};
    std::int32_t depth_infinite{};
    // Optional native device already identity-checked by an in-process
    // producer. Appended so older V2 producers and consumers remain ABI-safe
    // through struct_size negotiation.
    ID3D12Device* verified_native_device{};
};

struct OFXR_DlssGuideStatisticsV2 {
    std::uint32_t struct_size{sizeof(OFXR_DlssGuideStatisticsV2)};
    std::uint32_t api_version{OFXR_PROVIDER_API_VERSION_V2};
    std::uint64_t published{};
    std::uint64_t depth_publications{};
    std::uint64_t invalid_publications{};
    std::uint64_t snapshot_copies{};
    std::uint64_t snapshot_failures{};
    std::uint64_t resolve_calls{};
    std::uint64_t resolve_missing_streams{};
    std::uint64_t resolve_stale_pairs{};
    std::uint64_t resolve_queue_mismatches{};
    std::uint64_t matched{};
    std::uint64_t temporal_rejections{};
    std::uint64_t invalid_rejections{};
    std::uint64_t used{};
    std::uint64_t last_used_publication{};
    std::uint32_t status{};
    std::uint64_t fsr_attempts{};
    std::uint64_t fsr_used{};
    std::uint64_t fsr_failures{};
    std::uint32_t fsr_status{};
};

using PFN_OFXR_OptiScalerProviderV1 = int (*)(
    OFXR_OptiScalerProviderIdentityV1* identity) noexcept;
using PFN_OFXR_OptiScalerProviderV2 = int (*)(
    OFXR_OptiScalerProviderIdentityV2* identity) noexcept;

#if defined(OFXR_PROVIDER_API_EXPORTS)
#define OFXR_LAYER_API __declspec(dllexport)
#else
#define OFXR_LAYER_API
#endif

extern "C" {
OFXR_LAYER_API std::uint32_t OFXR_GetProviderApiVersionV1() noexcept;
OFXR_LAYER_API int OFXR_GetControlSnapshotV1(
    OFXR_ControlSnapshotV1* snapshot) noexcept;
OFXR_LAYER_API int OFXR_RequestControlV1(
    const OFXR_ControlSettingsV1* settings) noexcept;
OFXR_LAYER_API int OFXR_GetLoggingSettingV1() noexcept;
OFXR_LAYER_API int OFXR_SetLoggingSettingV1(int enabled) noexcept;
OFXR_LAYER_API int OFXR_GetOverlaySettingV1() noexcept;
OFXR_LAYER_API int OFXR_SetOverlaySettingV1(int position) noexcept;
OFXR_LAYER_API void OFXR_ConfigureDlssMotionVectorsV1(int enabled) noexcept;
OFXR_LAYER_API int OFXR_PublishDlssMotionVectorsV1(
    const OFXR_DlssMotionVectorPublicationV1* publication) noexcept;
OFXR_LAYER_API void OFXR_RetireDlssMotionVectorStreamV1(
    std::uint64_t stream) noexcept;
OFXR_LAYER_API int OFXR_GetDlssMotionVectorStatisticsV1(
    OFXR_DlssMotionVectorStatisticsV1* statistics) noexcept;
OFXR_LAYER_API std::uint32_t OFXR_GetProviderApiVersionV2() noexcept;
OFXR_LAYER_API int OFXR_RegisterOptiScalerProviderV2(
    const OFXR_OptiScalerProviderIdentityV2* identity) noexcept;
OFXR_LAYER_API void OFXR_UnregisterOptiScalerProviderV2() noexcept;
OFXR_LAYER_API int OFXR_GetControlSnapshotV2(
    OFXR_ControlSnapshotV2* snapshot) noexcept;
OFXR_LAYER_API int OFXR_RequestControlV2(
    const OFXR_ControlSettingsV2* settings) noexcept;
OFXR_LAYER_API void OFXR_ConfigureDlssGuidesV2(int enabled) noexcept;
OFXR_LAYER_API int OFXR_PublishDlssGuidesV2(
    const OFXR_DlssGuidePublicationV2* publication) noexcept;
OFXR_LAYER_API void OFXR_RetireDlssGuideStreamV2(
    std::uint64_t stream) noexcept;
OFXR_LAYER_API int OFXR_GetDlssGuideStatisticsV2(
    OFXR_DlssGuideStatisticsV2* statistics) noexcept;
}

#undef OFXR_LAYER_API
