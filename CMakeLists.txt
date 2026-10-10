cmake_minimum_required(VERSION 3.24)

project(XRFrameGenerationBridge VERSION 0.439.0 LANGUAGES CXX)

if(MSVC)
    # API layers are injected into hosts that can preload an older game-local
    # MSVCP140.dll. Initialize mutex/CV storage through that loaded runtime
    # instead of relying on the newer toolset's constexpr representation.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

option(XRFG_BUILD_LAYER "Build the Windows OpenXR API layer" ON)
option(XRFG_BUILD_TESTS "Build the core test executable" ON)
option(XRFG_BUILD_STANDALONE "Build the Windows standalone tray launcher" ON)
option(XRFG_NATIVE_DLSSG "Build the experimental native NVIDIA DLSS Frame Generation option" OFF)
set(XRFG_DLSS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/DLSS" CACHE PATH
    "NVIDIA DLSS SDK with include and lib/Windows_x86_64")

set(XRFG_OPENXR_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/OpenXR-SDK"
    CACHE PATH
    "Path to Khronos OpenXR-SDK containing include/openxr/openxr.h")

# OpenVR headers. The layer needs them to read what SteamVR's compositor
# actually scanned out - no OpenXR call reports it. Like the FidelityFX SDK
# the checkout is not committed; nothing from OpenVR is redistributed, since
# openvr_api.dll is loaded from the SteamVR installation at run time.
set(XRFG_OPENVR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/openvr"
    CACHE PATH "Directory containing openvr.h")

# Vulkan headers, vendored like the OpenXR ones: the layer takes every Vulkan
# entry point from the application's own loader at run time and links nothing.
set(XRFG_VULKAN_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/Vulkan-Headers/include"
    CACHE PATH
    "Directory containing vulkan/vulkan.h")

set(XRFG_FIDELITYFX_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/FidelityFX-SDK-v1.1.4/sdk"
    CACHE PATH
    "Path to the pinned AMD FidelityFX SDK v1.1.4 source tree")

set(XRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7"
    CACHE PATH
    "Path to the NVIDIA Optical Flow SDK 5.0.7 root")

add_library(xrfg_core STATIC
    src/core/benchmark_model.cpp
    src/core/fps_overlay_model.cpp
    src/core/image.cpp
    src/core/optical_flow.cpp
    src/core/pose.cpp
    src/core/status_panel_model.cpp)
target_include_directories(xrfg_core PUBLIC include)
target_compile_features(xrfg_core PUBLIC cxx_std_20)

add_executable(xrfg_reference src/reference/main.cpp)
target_link_libraries(xrfg_reference PRIVATE xrfg_core)

if(MSVC)
    foreach(target xrfg_core xrfg_reference)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endforeach()
endif()

if(XRFG_BUILD_TESTS)
    enable_testing()
    add_executable(xrfg_core_tests tests/core_tests.cpp)
    target_link_libraries(xrfg_core_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_core_tests PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_core_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME xrfg_core_tests COMMAND xrfg_core_tests)
    add_executable(xrfg_fps_overlay_tests tests/fps_overlay_tests.cpp)
    target_link_libraries(xrfg_fps_overlay_tests PRIVATE xrfg_core)
    add_test(NAME xrfg_fps_overlay_tests COMMAND xrfg_fps_overlay_tests)
    # The status panel: the flip gesture's hysteresis, its placement on the
    # controller, the asked-against-running text and its pixels. No headset.
    add_executable(xrfg_status_panel_tests tests/status_panel_tests.cpp)
    target_link_libraries(xrfg_status_panel_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_status_panel_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME xrfg_status_panel_tests COMMAND xrfg_status_panel_tests)

    add_executable(xrfg_optical_flow_tests tests/optical_flow_tests.cpp)
    target_link_libraries(xrfg_optical_flow_tests PRIVATE xrfg_core)
    if(MSVC)
        target_compile_options(xrfg_optical_flow_tests PRIVATE /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_optical_flow_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME xrfg_optical_flow_tests COMMAND xrfg_optical_flow_tests)
endif()

if(WIN32 AND XRFG_BUILD_LAYER)
    if(NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_platform_defines.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_loader_negotiation.h" OR
       NOT EXISTS "${XRFG_OPENXR_SDK_ROOT}/include/openxr/openxr_platform.h")
        message(FATAL_ERROR
            "XRFG_OPENXR_SDK_ROOT must point to the required Khronos OpenXR-SDK generated headers")
    endif()

    find_program(XRFG_FXC_EXECUTABLE fxc.exe
        HINTS
            "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64")
    if(NOT XRFG_FXC_EXECUTABLE)
        message(FATAL_ERROR "fxc.exe from the Windows SDK is required for frame-synthesis shaders")
    endif()

    set(XRFG_FIDELITYFX_INCLUDE_DIR
        "${XRFG_FIDELITYFX_SDK_ROOT}/include")
    set(XRFG_FIDELITYFX_LIBRARY_DIR
        "${XRFG_FIDELITYFX_SDK_ROOT}/bin/ffx_sdk")
    set(XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY
        "${XRFG_FIDELITYFX_LIBRARY_DIR}/ffx_opticalflow_x64.lib")
    set(XRFG_FIDELITYFX_DX12_LIBRARY
        "${XRFG_FIDELITYFX_LIBRARY_DIR}/ffx_backend_dx12_x64.lib")
    if(NOT EXISTS "${XRFG_FIDELITYFX_INCLUDE_DIR}/FidelityFX/host/ffx_opticalflow.h" OR
       NOT EXISTS "${XRFG_FIDELITYFX_INCLUDE_DIR}/FidelityFX/host/backends/dx12/ffx_dx12.h" OR
       NOT EXISTS "${XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY}" OR
       NOT EXISTS "${XRFG_FIDELITYFX_DX12_LIBRARY}")
        message(FATAL_ERROR
            "Pinned FidelityFX SDK v1.1.4 headers and Release DX12/Optical Flow libraries are required")
    endif()

    set(XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR
        "${XRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT}/NvOFInterface")
    if(NOT EXISTS "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}/nvOpticalFlowCommon.h" OR
       NOT EXISTS "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}/nvOpticalFlowD3D12.h")
        message(FATAL_ERROR
            "NVIDIA Optical Flow SDK 5.0.7 interface headers are required")
    endif()

    set(XRFG_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
    set(XRFG_PACK_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/pack_flow_input_shader.hpp")
    set(XRFG_FULLSCREEN_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/fullscreen_vertex_shader.hpp")
    set(XRFG_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/game_motion_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_HYBRID_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/hybrid_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_EXTRAPOLATE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_pixel_shader.hpp")
    set(XRFG_EXTRAPOLATE_FLOW_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_flow_pixel_shader.hpp")
    set(XRFG_EXTRAPOLATE_HYBRID_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_hybrid_pixel_shader.hpp")
    set(XRFG_EXTRAPOLATE_MESH_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_mesh_vertex_shader.hpp")
    set(XRFG_EXTRAPOLATE_FLOW_MESH_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_flow_mesh_vertex_shader.hpp")
    set(XRFG_EXTRAPOLATE_MESH_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_mesh_pixel_shader.hpp")
    set(XRFG_EXTRAPOLATE_MESH_FILL_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_mesh_fill_pixel_shader.hpp")
    set(XRFG_EXTRAPOLATE_MESH_BOTH_VERTEX_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_mesh_both_vertex_shader.hpp")
    set(XRFG_EXTRAPOLATE_MESH_HYBRID_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/extrapolate_mesh_hybrid_pixel_shader.hpp")
    set(XRFG_NVIDIA_PACK_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_pack_flow_input_shader.hpp")
    set(XRFG_NVIDIA_PACK_LUMA_INPUT_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_pack_luma_input_shader.hpp")
    set(XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_bidirectional_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_fast_synthesize_midpoint_pixel_shader.hpp")
    set(XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER "${XRFG_GENERATED_SHADER_DIR}/nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader.hpp")
    add_custom_command(
        OUTPUT
            "${XRFG_PACK_INPUT_HEADER}"
            "${XRFG_FULLSCREEN_VERTEX_HEADER}"
            "${XRFG_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_HYBRID_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_EXTRAPOLATE_PIXEL_HEADER}"
            "${XRFG_EXTRAPOLATE_FLOW_PIXEL_HEADER}"
            "${XRFG_EXTRAPOLATE_HYBRID_PIXEL_HEADER}"
            "${XRFG_EXTRAPOLATE_MESH_VERTEX_HEADER}"
            "${XRFG_EXTRAPOLATE_FLOW_MESH_VERTEX_HEADER}"
            "${XRFG_EXTRAPOLATE_MESH_PIXEL_HEADER}"
            "${XRFG_EXTRAPOLATE_MESH_FILL_PIXEL_HEADER}"
            "${XRFG_EXTRAPOLATE_MESH_BOTH_VERTEX_HEADER}"
            "${XRFG_EXTRAPOLATE_MESH_HYBRID_PIXEL_HEADER}"
            "${XRFG_NVIDIA_PACK_INPUT_HEADER}"
            "${XRFG_NVIDIA_PACK_LUMA_INPUT_HEADER}"
            "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER}"
            "${XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${XRFG_GENERATED_SHADER_DIR}"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackFlowInput
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_PACK_INPUT_HEADER}" /Vn g_xrfg_pack_flow_input_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E FullscreenTriangleVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_FULLSCREEN_VERTEX_HEADER}" /Vn g_xrfg_fullscreen_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeGameMotionMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_game_motion_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeHybridMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_HYBRID_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_hybrid_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeExtrapolatedPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_PIXEL_HEADER}" /Vn g_xrfg_extrapolate_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeExtrapolatedFlowPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_FLOW_PIXEL_HEADER}" /Vn g_xrfg_extrapolate_flow_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeExtrapolatedHybridPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_HYBRID_PIXEL_HEADER}" /Vn g_xrfg_extrapolate_hybrid_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E ExtrapolateMeshVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_MESH_VERTEX_HEADER}" /Vn g_xrfg_extrapolate_mesh_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E ExtrapolateFlowMeshVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_FLOW_MESH_VERTEX_HEADER}" /Vn g_xrfg_extrapolate_flow_mesh_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E ExtrapolateMeshPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_MESH_PIXEL_HEADER}" /Vn g_xrfg_extrapolate_mesh_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E ExtrapolateMeshFillPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_MESH_FILL_PIXEL_HEADER}" /Vn g_xrfg_extrapolate_mesh_fill_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T vs_5_1 /E ExtrapolateMeshBothVS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_MESH_BOTH_VERTEX_HEADER}" /Vn g_xrfg_extrapolate_mesh_both_vertex_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E ExtrapolateMeshHybridPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_EXTRAPOLATE_MESH_HYBRID_PIXEL_HEADER}" /Vn g_xrfg_extrapolate_mesh_hybrid_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackNvidiaFlowInput
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_PACK_INPUT_HEADER}" /Vn g_xrfg_nvidia_pack_flow_input_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T cs_5_1 /E PackNvidiaFlowInput
            /D XRFG_NVIDIA_LUMA_INPUT=1
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_PACK_LUMA_INPUT_HEADER}" /Vn g_xrfg_nvidia_pack_luma_input_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaBidirectionalMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaFastMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_fast_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ps_5_1 /E SynthesizeNvidiaFastBidirectionalMidpointPS
            /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect
            /Fh "${XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}" /Vn g_xrfg_nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/frame_synthesis.hlsl"
        VERBATIM)

    add_library(xrfg_d3d12_history STATIC
        src/d3d11/d3d11_d3d12_interop.cpp
        src/d3d11/d3d11_bridge.cpp
        src/vulkan/vulkan_d3d12_interop.cpp
        src/d3d12/d3d12_history.cpp
        src/d3d12/d3d12_frame_synthesizer.cpp
        src/d3d12/d3d12_native_dlssg.cpp
        src/d3d12/frame_dump.cpp
        src/common/dlss_motion_vectors.cpp
        "${XRFG_PACK_INPUT_HEADER}"
        "${XRFG_GAME_MOTION_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_HYBRID_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_EXTRAPOLATE_PIXEL_HEADER}"
        "${XRFG_EXTRAPOLATE_FLOW_PIXEL_HEADER}"
        "${XRFG_EXTRAPOLATE_HYBRID_PIXEL_HEADER}"
        "${XRFG_EXTRAPOLATE_MESH_VERTEX_HEADER}"
        "${XRFG_EXTRAPOLATE_FLOW_MESH_VERTEX_HEADER}"
        "${XRFG_EXTRAPOLATE_MESH_PIXEL_HEADER}"
        "${XRFG_EXTRAPOLATE_MESH_FILL_PIXEL_HEADER}"
        "${XRFG_EXTRAPOLATE_MESH_BOTH_VERTEX_HEADER}"
        "${XRFG_EXTRAPOLATE_MESH_HYBRID_PIXEL_HEADER}"
        "${XRFG_FULLSCREEN_VERTEX_HEADER}"
        "${XRFG_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_PACK_INPUT_HEADER}"
        "${XRFG_NVIDIA_PACK_LUMA_INPUT_HEADER}"
        "${XRFG_NVIDIA_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_FAST_SYNTHESIZE_PIXEL_HEADER}"
        "${XRFG_NVIDIA_FAST_BIDIRECTIONAL_SYNTHESIZE_PIXEL_HEADER}")
    target_include_directories(xrfg_d3d12_history
        PUBLIC include "${XRFG_VULKAN_INCLUDE_DIR}"
        PRIVATE
            "${XRFG_GENERATED_SHADER_DIR}"
            "${XRFG_FIDELITYFX_INCLUDE_DIR}"
            "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}")
    target_compile_features(xrfg_d3d12_history PUBLIC cxx_std_20)
    if(XRFG_NATIVE_DLSSG)
        include(FetchContent)
        set(SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE)
        FetchContent_Declare(ofxr_safetyhook
            GIT_REPOSITORY https://github.com/cursey/safetyhook.git
            GIT_TAG c3f3f306a0f12d1811c0b713ad2ed2a8ddc6cf55)
        FetchContent_MakeAvailable(ofxr_safetyhook)
        set(XRFG_DLSSG_RUNTIME "${XRFG_DLSS_SDK_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssg.dll")
        if(NOT EXISTS "${XRFG_DLSS_SDK_ROOT}/include/nvsdk_ngx_helpers_dlssg.h" OR
           NOT EXISTS "${XRFG_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" OR
           NOT EXISTS "${XRFG_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib" OR
           NOT EXISTS "${XRFG_DLSSG_RUNTIME}")
            message(FATAL_ERROR "XRFG_NATIVE_DLSSG requires a complete NVIDIA DLSS SDK at XRFG_DLSS_SDK_ROOT")
        endif()
        foreach(stage pack seed vertex pixel)
            set(header "${XRFG_GENERATED_SHADER_DIR}/native_dlssg_${stage}_shader.hpp")
            if(stage STREQUAL "pack")
                set(profile cs_5_1)
                set(entry PackNativeDlssG)
            elseif(stage STREQUAL "seed")
                set(profile cs_5_1)
                set(entry SeedNativeDlssG)
            elseif(stage STREQUAL "vertex")
                set(profile vs_5_1)
                set(entry NativeDlssGVS)
            else()
                set(profile ps_5_1)
                set(entry NativeDlssGPS)
            endif()
            add_custom_command(OUTPUT "${header}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${XRFG_GENERATED_SHADER_DIR}"
                COMMAND "${XRFG_FXC_EXECUTABLE}" /nologo /T ${profile} /E ${entry}
                    /O3 /WX /Ges /Qstrip_debug /Qstrip_reflect /Fh "${header}"
                    /Vn g_xrfg_native_dlssg_${stage}_shader "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12/native_dlssg.hlsl"
                DEPENDS src/d3d12/native_dlssg.hlsl VERBATIM)
            target_sources(xrfg_d3d12_history PRIVATE "${header}")
        endforeach()
        target_include_directories(xrfg_d3d12_history PRIVATE "${XRFG_DLSS_SDK_ROOT}/include")
        target_link_libraries(xrfg_d3d12_history PUBLIC
            optimized "${XRFG_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
            debug "${XRFG_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib")
        target_compile_definitions(xrfg_d3d12_history PUBLIC XRFG_NATIVE_DLSSG)
    endif()
    target_link_libraries(xrfg_d3d12_history PUBLIC
        xrfg_core
        d3d11
        d3d12
        dxgi
        "${XRFG_FIDELITYFX_OPTICALFLOW_LIBRARY}"
        "${XRFG_FIDELITYFX_DX12_LIBRARY}")
    target_compile_definitions(xrfg_d3d12_history PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
    # Anything that includes the Vulkan interop header sees the same Vulkan
    # header configuration: no prototypes, since nothing links vulkan-1.
    target_compile_definitions(xrfg_d3d12_history PUBLIC
        VK_NO_PROTOTYPES
        VK_USE_PLATFORM_WIN32_KHR)
    if(MSVC)
        target_compile_options(xrfg_d3d12_history PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_bridge_flight_logger STATIC
        src/layer/bridge_flight_logger.cpp
        include/xrfg/bridge_flight_logger.hpp)
    target_include_directories(xrfg_bridge_flight_logger PUBLIC include)
    target_link_libraries(xrfg_d3d12_history PUBLIC xrfg_bridge_flight_logger)
    target_compile_features(xrfg_bridge_flight_logger PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_bridge_flight_logger PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XRFG_IMPLEMENTATION_VERSION=${PROJECT_VERSION_MINOR})
    if(MSVC)
        target_compile_options(xrfg_bridge_flight_logger PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_library(xrfg_implicit_layer STATIC
        src/common/implicit_layer.cpp
        include/xrfg/implicit_layer.hpp)
    target_include_directories(xrfg_implicit_layer PUBLIC include)
    target_compile_features(xrfg_implicit_layer PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_implicit_layer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
    target_link_libraries(xrfg_implicit_layer PUBLIC advapi32)
    if(MSVC)
        target_compile_options(xrfg_implicit_layer PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    if(NOT EXISTS "${XRFG_OPENVR_INCLUDE_DIR}/openvr.h")
        message(FATAL_ERROR
            "openvr.h is required for the layer: the overlay reports what the "
            "compositor delivered, which no OpenXR call exposes. Place it at "
            "${XRFG_OPENVR_INCLUDE_DIR}/openvr.h - see docs/BUILDING.md.")
    endif()

    add_library(xrfg_fps_overlay STATIC
        src/layer/openxr_fps_overlay.cpp
        src/layer/steamvr_delivery.cpp)
    target_include_directories(xrfg_fps_overlay PUBLIC include "${XRFG_OPENXR_SDK_ROOT}/include")
    target_include_directories(xrfg_fps_overlay PRIVATE "${XRFG_OPENVR_INCLUDE_DIR}")
    target_compile_features(xrfg_fps_overlay PUBLIC cxx_std_20)
    target_compile_definitions(xrfg_fps_overlay PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
        XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 XR_USE_PLATFORM_WIN32)
    target_link_libraries(xrfg_fps_overlay PUBLIC xrfg_core xrfg_bridge_flight_logger d3d11 d3d12 dxgi)
    if(MSVC)
        target_compile_options(xrfg_fps_overlay PRIVATE /W4 /permissive- /EHsc)
    endif()
    if(XRFG_BUILD_TESTS)
        add_executable(xrfg_openxr_fps_overlay_tests tests/openxr_fps_overlay_tests.cpp)
        target_link_libraries(xrfg_openxr_fps_overlay_tests PRIVATE xrfg_fps_overlay)
        add_test(NAME xrfg_openxr_fps_overlay_d3d12 COMMAND xrfg_openxr_fps_overlay_tests)
        add_test(NAME xrfg_openxr_fps_overlay_d3d11 COMMAND xrfg_openxr_fps_overlay_tests d3d11)
    endif()

    add_library(XR_APILAYER_XRFrameBridge_diagnostic SHARED
        src/common/embedded_control.cpp
        src/layer/openxr_layer.cpp
        src/layer/provider_api.cpp)
    target_sources(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE src/layer/ngx_guide_capture.cpp)
    if(XRFG_NATIVE_DLSSG)
        target_include_directories(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE "${XRFG_DLSS_SDK_ROOT}/include")
        target_link_libraries(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE safetyhook)
    endif()
    target_include_directories(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        include
        "${XRFG_OPENXR_SDK_ROOT}/include")
    target_link_libraries(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        xrfg_core
        xrfg_d3d12_history
        xrfg_bridge_flight_logger
        xrfg_fps_overlay
        xrfg_implicit_layer
        psapi)
    target_compile_features(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE cxx_std_20)
    target_compile_definitions(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XR_USE_GRAPHICS_API_D3D11
        XR_USE_GRAPHICS_API_D3D12
        XR_USE_GRAPHICS_API_VULKAN
        XR_USE_PLATFORM_WIN32
        OFXR_PROVIDER_API_EXPORTS)
    if(MSVC)
        target_compile_options(XR_APILAYER_XRFrameBridge_diagnostic PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    add_custom_command(TARGET XR_APILAYER_XRFrameBridge_diagnostic POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/layer/ofxr_bridge.ini"
            "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/ofxr_bridge.ini")
    if(XRFG_NATIVE_DLSSG)
        add_custom_command(TARGET XR_APILAYER_XRFrameBridge_diagnostic POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${XRFG_DLSSG_RUNTIME}"
                "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/nvngx_dlssg.dll")
    endif()

    # The Vulkan implicit layer that serialises queue submissions; see the
    # comment at the top of its source. Links nothing but the CRT.
    add_library(OFXR_vulkan_queue_layer SHARED
        src/vulkan_layer/vulkan_queue_layer.cpp)
    target_include_directories(OFXR_vulkan_queue_layer PRIVATE
        "${XRFG_VULKAN_INCLUDE_DIR}")
    target_compile_features(OFXR_vulkan_queue_layer PRIVATE cxx_std_20)
    target_compile_definitions(OFXR_vulkan_queue_layer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        VK_NO_PROTOTYPES)
    if(MSVC)
        target_compile_options(OFXR_vulkan_queue_layer PRIVATE
            /W4 /permissive- /EHsc)
    endif()

    if(XRFG_BUILD_STANDALONE)
        add_library(xrfg_standalone_core STATIC
            src/tray/benchmark_labels.cpp
            src/tray/standalone_launcher.cpp
            include/xrfg/benchmark_labels.hpp
            include/xrfg/standalone_launcher.hpp)
        target_include_directories(xrfg_standalone_core PUBLIC include)
        target_compile_features(xrfg_standalone_core PUBLIC cxx_std_20)
        target_link_libraries(xrfg_standalone_core PUBLIC xrfg_implicit_layer xrfg_core)
        target_compile_definitions(xrfg_standalone_core PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE)
        if(MSVC)
            target_compile_options(xrfg_standalone_core PRIVATE
                /W4 /permissive- /EHsc)
        endif()

        # "Benchmark this PC": GPU time of every frame-generation method
        # through the synthesizer, at a headset's per-eye size. The tray runs
        # it as a child process, so a driver fault in a case cannot take the
        # tray and its arm with it. It ships in the ofxr folder beside
        # nvngx_dlssg.dll, which NGX looks for in the folder of the module
        # that initialises it.
        add_executable(OFXRBenchmark
            src/benchmark/benchmark_main.cpp
            src/d3d12/fg_benchmark.cpp
            include/xrfg/fg_benchmark.hpp)
        target_include_directories(OFXRBenchmark PRIVATE include)
        target_link_libraries(OFXRBenchmark PRIVATE xrfg_d3d12_history xrfg_core d3d12 dxgi)
        target_compile_features(OFXRBenchmark PRIVATE cxx_std_20)
        target_compile_definitions(OFXRBenchmark PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(MSVC)
            target_compile_options(OFXRBenchmark PRIVATE /W4 /permissive- /EHsc)
        endif()
        if(XRFG_BUILD_TESTS)
            # Every kind of case, small and on WARP so it runs without a GPU:
            # NVIDIA flow and native generation answer "unavailable" there.
            add_test(NAME xrfg_benchmark_tool_warp
                COMMAND OFXRBenchmark --warp --eye 256x256 --warmup 1 --pairs 4
                    --cases ffx_50,ffx_50_3x,vectors,hybrid_50,extrapolate,extrapolate_ffx_50,guide_snapshot,nv_medium_50,native_67
                    --output "${CMAKE_CURRENT_BINARY_DIR}/benchmark-tool-warp.ini")
            set_tests_properties(xrfg_benchmark_tool_warp PROPERTIES TIMEOUT 120
                FAIL_REGULAR_EXPRESSION "result [a-z0-9_]+ failed;error ")
        endif()

        # The tray's "Benchmark this PC" window and the child process behind
        # it. A library of its own because the lifecycle tests compile the
        # tray's main.cpp, which holds one.
        add_library(xrfg_tray_benchmark STATIC
            src/tray/benchmark_window.cpp
            src/tray/benchmark_window.hpp)
        target_link_libraries(xrfg_tray_benchmark PUBLIC
            xrfg_standalone_core comctl32 dxgi gdi32 user32)
        target_compile_features(xrfg_tray_benchmark PUBLIC cxx_std_20)
        target_compile_definitions(xrfg_tray_benchmark PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(MSVC)
            target_compile_options(xrfg_tray_benchmark PRIVATE /W4 /permissive- /EHsc)
        endif()

        enable_language(RC)
        add_executable(OFXRBridgeTray WIN32
            src/tray/main.cpp
            src/tray/resources.rc)
        target_link_libraries(OFXRBridgeTray PRIVATE
            xrfg_standalone_core
            xrfg_tray_benchmark
            comctl32
            gdi32
            ole32
            shell32
            user32)
        target_compile_features(OFXRBridgeTray PRIVATE cxx_std_20)
        if(XRFG_NATIVE_DLSSG)
            target_compile_definitions(OFXRBridgeTray PRIVATE XRFG_NATIVE_DLSSG)
            add_custom_command(TARGET OFXRBridgeTray POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${XRFG_DLSSG_RUNTIME}"
                    "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/nvngx_dlssg.dll"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:OFXRBridgeTray>/licenses"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenses/NVIDIA-DLSS.txt"
                    "$<TARGET_FILE_DIR:OFXRBridgeTray>/licenses/NVIDIA-DLSS.txt")
            foreach(notice SafetyHook-Boost-1.0 Zydis-MIT Zycore-MIT UEVR-API-MIT)
                add_custom_command(TARGET OFXRBridgeTray POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/licenses/${notice}.txt"
                        "$<TARGET_FILE_DIR:OFXRBridgeTray>/licenses/${notice}.txt")
            endforeach()
        endif()
        target_compile_definitions(OFXRBridgeTray PRIVATE
            XRFG_IMPLEMENTATION_VERSION=${PROJECT_VERSION_MINOR}
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE)
        if(MSVC)
            target_compile_options(OFXRBridgeTray PRIVATE
                /W4 /permissive- /EHsc)
        endif()
        add_dependencies(OFXRBridgeTray
            XR_APILAYER_XRFrameBridge_diagnostic)
        add_custom_command(TARGET OFXRBridgeTray POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE_DIR:XR_APILAYER_XRFrameBridge_diagnostic>/ofxr_bridge.ini"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/ofxr_bridge.ini"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:OFXR_vulkan_queue_layer>"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/OFXR_vulkan_queue_layer.dll"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:OFXRBenchmark>"
                "$<TARGET_FILE_DIR:OFXRBridgeTray>/ofxr/OFXRBenchmark.exe")
        add_dependencies(OFXRBridgeTray OFXR_vulkan_queue_layer OFXRBenchmark)

        if(XRFG_BUILD_TESTS)
            add_executable(xrfg_standalone_launcher_tests
                tests/standalone_launcher_tests.cpp)
            target_link_libraries(xrfg_standalone_launcher_tests PRIVATE
                xrfg_standalone_core)
            target_compile_features(xrfg_standalone_launcher_tests PRIVATE
                cxx_std_20)
            if(MSVC)
                target_compile_options(xrfg_standalone_launcher_tests PRIVATE
                    /W4 /permissive- /EHsc)
                target_compile_definitions(xrfg_standalone_launcher_tests PRIVATE
                    NOMINMAX
                    WIN32_LEAN_AND_MEAN
                    UNICODE
                    _UNICODE)
            endif()
            add_test(NAME xrfg_standalone_launcher_tests
                COMMAND xrfg_standalone_launcher_tests)
            # "Benchmark this PC": its catalogue, results file, speed-up
            # arithmetic and the tray's lookups. No GPU.
            add_executable(xrfg_benchmark_model_tests tests/benchmark_model_tests.cpp)
            target_link_libraries(xrfg_benchmark_model_tests PRIVATE xrfg_standalone_core)
            target_compile_features(xrfg_benchmark_model_tests PRIVATE cxx_std_20)
            if(MSVC)
                target_compile_options(xrfg_benchmark_model_tests PRIVATE /W4 /permissive- /EHsc)
                target_compile_definitions(xrfg_benchmark_model_tests PRIVATE
                    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
            endif()
            add_test(NAME xrfg_benchmark_model_tests COMMAND xrfg_benchmark_model_tests)
            add_executable(xrfg_tray_lifecycle_tests tests/tray_lifecycle_tests.cpp)
            target_link_libraries(xrfg_tray_lifecycle_tests PRIVATE
                xrfg_standalone_core xrfg_tray_benchmark comctl32 gdi32 ole32 shell32 user32)
            target_compile_definitions(xrfg_tray_lifecycle_tests PRIVATE
                XRFG_IMPLEMENTATION_VERSION=${PROJECT_VERSION_MINOR}
                NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
            if(MSVC)
                target_compile_options(xrfg_tray_lifecycle_tests PRIVATE /W4 /permissive- /EHsc)
            endif()
            add_test(NAME xrfg_tray_lifecycle_tests COMMAND xrfg_tray_lifecycle_tests)
        endif()
    endif()

    if(XRFG_BUILD_TESTS)
        add_executable(xrfg_bridge_flight_logger_tests
            tests/bridge_flight_logger_tests.cpp)
        target_link_libraries(xrfg_bridge_flight_logger_tests PRIVATE
            xrfg_bridge_flight_logger)
        target_compile_features(xrfg_bridge_flight_logger_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_bridge_flight_logger_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_bridge_flight_logger_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_bridge_flight_logger_tests
            COMMAND xrfg_bridge_flight_logger_tests)

        add_executable(xrfg_layer_smoke tests/layer_smoke.cpp)
        target_include_directories(xrfg_layer_smoke PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_layer_smoke PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_layer_smoke PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_layer_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(
            NAME xrfg_layer_negotiation
            COMMAND xrfg_layer_smoke $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>)

        add_library(xrfg_optiscaler_bootstrap_stub SHARED
            tests/optiscaler_bootstrap_stub.cpp)
        target_include_directories(xrfg_optiscaler_bootstrap_stub PRIVATE
            include
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_optiscaler_bootstrap_stub PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_optiscaler_bootstrap_stub PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_optiscaler_bootstrap_stub PRIVATE
                NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()

        add_executable(xrfg_optiscaler_bootstrap_tests
            tests/optiscaler_bootstrap_tests.cpp)
        target_include_directories(xrfg_optiscaler_bootstrap_tests PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include")
        target_compile_features(xrfg_optiscaler_bootstrap_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_optiscaler_bootstrap_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_optiscaler_bootstrap_tests PRIVATE
                NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        foreach(mode success incompatible failure seh)
            add_test(
                NAME xrfg_optiscaler_bootstrap_${mode}
                COMMAND xrfg_optiscaler_bootstrap_tests
                    $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                    $<TARGET_FILE:xrfg_optiscaler_bootstrap_stub>
                    ${mode})
        endforeach()

        add_executable(xrfg_provider_api_tests tests/provider_api_tests.cpp)
        target_include_directories(xrfg_provider_api_tests PRIVATE include)
        target_compile_features(xrfg_provider_api_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_provider_api_tests PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_provider_api_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_dependencies(xrfg_provider_api_tests XR_APILAYER_XRFrameBridge_diagnostic)
        add_test(
            NAME xrfg_provider_api
            COMMAND xrfg_provider_api_tests $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>)

        add_executable(xrfg_layer_call_chain tests/layer_call_chain.cpp)
        target_include_directories(xrfg_layer_call_chain PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include"
            "${XRFG_VULKAN_INCLUDE_DIR}")
        target_compile_features(xrfg_layer_call_chain PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_layer_call_chain PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_layer_call_chain PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
                XR_USE_GRAPHICS_API_D3D11
                XR_USE_GRAPHICS_API_D3D12
                XR_USE_GRAPHICS_API_VULKAN
                VK_NO_PROTOTYPES
                VK_USE_PLATFORM_WIN32_KHR
                XR_USE_PLATFORM_WIN32)
        endif()
        target_link_libraries(xrfg_layer_call_chain PRIVATE d3d11 d3d12 dxgi)
        foreach(mode inverted-fov d3d11-inverted-fov)
            add_test(NAME xrfg_layer_${mode} COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-${mode}.log" ${mode})
        endforeach()
        # The status panel's flip gesture: the layer's grip action added to
        # the application's bindings, attach and sync, and taken out again
        # wherever the runtime refuses it.
        add_test(NAME xrfg_layer_panel_input COMMAND xrfg_layer_call_chain
            $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
            "${CMAKE_CURRENT_BINARY_DIR}/xrfg-panel-input.log" panel-input)
        # And with the panel off, the input calls are not intercepted at all.
        add_test(
            NAME xrfg_layer_panel_input_off
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_panel_off.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/panel-input-off"
                "-DMODE=panel-input"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_panel_input_off PROPERTIES TIMEOUT 60)
        # The budget scenario is about the private rings falling back from
        # the deeper depth when the runtime refuses a fourth swapchain, so it
        # runs with the rings, which the shipped ini (single-swapchain rings)
        # no longer uses.
        add_test(
            NAME xrfg_layer_swapchain-budget
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_rings_deep.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rings-swapchain-budget"
                "-DMODE=swapchain-budget"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_swapchain-budget PROPERTIES TIMEOUT 60)
        add_dependencies(xrfg_layer_call_chain XR_APILAYER_XRFrameBridge_diagnostic)
        add_executable(xrfg_layer_manual_disarm_tests tests/layer_manual_disarm_tests.cpp)
        target_include_directories(xrfg_layer_manual_disarm_tests PRIVATE
            "${XRFG_OPENXR_SDK_ROOT}/include"
            "${XRFG_VULKAN_INCLUDE_DIR}")
        target_link_libraries(xrfg_layer_manual_disarm_tests PRIVATE xrfg_implicit_layer d3d11 d3d12 dxgi)
        target_compile_definitions(xrfg_layer_manual_disarm_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12
            XR_USE_GRAPHICS_API_VULKAN VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR XR_USE_PLATFORM_WIN32)
        if(MSVC)
            target_compile_options(xrfg_layer_manual_disarm_tests PRIVATE /W4 /permissive- /EHsc)
        endif()
        add_dependencies(xrfg_layer_manual_disarm_tests XR_APILAYER_XRFrameBridge_diagnostic)
        foreach(mode inline d3d11 steamvr flight)
            add_test(NAME xrfg_manual_disarm_${mode} COMMAND xrfg_layer_manual_disarm_tests
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic> ${mode})
            set_tests_properties(xrfg_manual_disarm_${mode} PROPERTIES TIMEOUT 30)
            # The tray's "Pause frame generation": the same stop, then a
            # resume in the same session.
            add_test(NAME xrfg_manual_pause_${mode} COMMAND xrfg_layer_manual_disarm_tests
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic> ${mode} pause)
            set_tests_properties(xrfg_manual_pause_${mode} PROPERTIES TIMEOUT 30)
        endforeach()
        # On Virtual Desktop a single-threaded session takes the presenter at
        # its first generating frames and a split loop stays inline; Pimax
        # OpenXR stays inline; another runtime still goes by the bunched-pair
        # measurement.
        foreach(shape
                "virtual-desktop;split" "virtual-desktop;split;paused"
                "virtual-desktop;single" "virtual-desktop;single;paused"
                "pimax;split" "pimax;single" "other;single")
            string(REPLACE ";" "_" shape_name "${shape}")
            add_test(NAME xrfg_frame_loop_${shape_name} COMMAND xrfg_layer_manual_disarm_tests
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic> frame-loop ${shape})
            set_tests_properties(xrfg_frame_loop_${shape_name} PROPERTIES TIMEOUT 60)
        endforeach()
        # A frame-generation setting changed from the tray while the SteamVR
        # presenter runs with frames queued.
        add_test(NAME xrfg_layer_steamvr_live_switch COMMAND xrfg_layer_manual_disarm_tests
            $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic> steamvr-switch)
        set_tests_properties(xrfg_layer_steamvr_live_switch PROPERTIES TIMEOUT 30)
        # The flight recorder switched on, off and on again in a running
        # session: the session records from before the switch head the file.
        add_test(NAME xrfg_layer_live_recorder COMMAND xrfg_layer_manual_disarm_tests
            $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic> recorder)
        set_tests_properties(xrfg_layer_live_recorder PROPERTIES TIMEOUT 60)
        foreach(marker on off hidden)
            add_test(NAME xrfg_layer_marker_d3d11_${marker} COMMAND xrfg_layer_manual_disarm_tests
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic> d3d11 ${marker})
            set_tests_properties(xrfg_layer_marker_d3d11_${marker} PROPERTIES TIMEOUT 30)
        endforeach()
        add_test(
            NAME xrfg_layer_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-call-chain-test.log")
        add_test(
            NAME xrfg_layer_presenter_swapchain_lifetime
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-presenter-swapchain-lifetime.log"
                flight-destroy-swapchain)
        add_test(
            NAME xrfg_layer_presenter_space_lifetime
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-presenter-space-lifetime.log"
                steamvr-destroy-space)
        # A frame the runtime refuses for its contents costs that frame only:
        # the presenter must not latch it and fail the rest of the session.
        add_test(
            NAME xrfg_layer_presenter_layer_invalid
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-presenter-layer-invalid.log"
                steamvr-layer-invalid)
        # An application that labels its frames with its own display time
        # (MSFS 2024) can only be matched to its wait by elimination. One
        # frame the runtime rejects must not leave a wait pending for the
        # rest of the session.
        add_test(
            NAME xrfg_layer_presenter_own_display_time
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-presenter-own-display-time.log"
                steamvr-own-time)

        add_test(
            NAME xrfg_layer_split_eye_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-split-eye-call-chain-test.log"
                split-eye)

        add_test(
            NAME xrfg_layer_cropped_split_eye_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-cropped-split-eye-call-chain-test.log"
                cropped-split-eye)

        add_test(
            NAME xrfg_layer_double_wide_call_chain
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-double-wide-call-chain-test.log"
                double-wide)

        add_test(
            NAME xrfg_layer_uevr_pipelined_display_time
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-uevr-pipelined-display-time-test.log"
                uevr-pipelined-time)

        add_test(
            NAME xrfg_layer_d3d11_interop
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-interop-test.log"
                d3d11-interop)

        add_test(
            NAME xrfg_layer_d3d11_single_threaded
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-single-threaded-test.log"
                d3d11-single-threaded)

        add_test(
            NAME xrfg_layer_d3d11_double_wide
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-d3d11-double-wide-test.log"
                d3d11-double-wide)

        add_test(
            NAME xrfg_layer_steamvr_inline
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-steamvr-inline-test.log"
                steamvr-inline)

        add_test(
            NAME xrfg_layer_steamvr_presenter_transition
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-steamvr-presenter-test.log"
                steamvr-presenter)

        add_test(
            NAME xrfg_layer_pipelined_presenter
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-pipelined-presenter-test.log"
                flight-simulator)

        add_test(
            NAME xrfg_layer_dcs_pipelined_wait
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-dcs-pipelined-wait-test.log"
                dcs)

        add_test(
            NAME xrfg_layer_dcs_d3d11_inline_only
            COMMAND xrfg_layer_call_chain
                $<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>
                "${CMAKE_CURRENT_BINARY_DIR}/xrfg-layer-dcs-d3d11-inline-only-test.log"
                dcs-d3d11)

        # The same scenarios with the deeper pipeline off ("Prefer FPS over
        # latency" unticked), which the shipped ini turns on: each runs from a
        # scratch directory whose ofxr_bridge.ini turns it off.
        foreach(mode default split-eye cropped-split-eye double-wide
                d3d11-interop steamvr-presenter flight-simulator
                uevr-pipelined-time flight-destroy-swapchain
                steamvr-destroy-space dcs dcs-d3d11)
            add_test(
                NAME xrfg_layer_shallow_${mode}
                COMMAND "${CMAKE_COMMAND}"
                    "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                    "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                    "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_shallow_pipeline.ini"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/shallow-pipeline-${mode}"
                    "-DMODE=${mode}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
            set_tests_properties(xrfg_layer_shallow_${mode} PROPERTIES TIMEOUT 60)
        endforeach()
        # `[ofxr] synthetic_pose=real`: the synthetics go down with the newer
        # real frame's pose, as before the default gave each its own. The
        # scenarios check the pose either way, reading the same ini.
        foreach(mode default split-eye steamvr-presenter d3d11-bridge)
            add_test(
                NAME xrfg_layer_synthetic_pose_real_${mode}
                COMMAND "${CMAKE_COMMAND}"
                    "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                    "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                    "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_synthetic_pose_real.ini"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/synthetic-pose-real-${mode}"
                    "-DMODE=${mode}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
            set_tests_properties(xrfg_layer_synthetic_pose_real_${mode} PROPERTIES TIMEOUT 60)
        endforeach()
        # Extrapolation, as Application SpaceWarp does: the real frame goes
        # down first and the synthetic a display period after it, inline, on
        # the presenter and pipelined.
        foreach(mode default steamvr-presenter flight-simulator uevr-pipelined-time)
            add_test(
                NAME xrfg_layer_extrapolate_${mode}
                COMMAND "${CMAKE_COMMAND}"
                    "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                    "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                    "-DEXTRAPOLATE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_extrapolate.ini"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/extrapolate-${mode}"
                    "-DMODE=${mode}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_extrapolation.cmake")
            set_tests_properties(xrfg_layer_extrapolate_${mode} PROPERTIES TIMEOUT 60)
        endforeach()
        # "3X Frame Gen": two synthetics per application frame. The bridged
        # D3D11 scenario runs it inline and its own assertions hold whatever
        # the multiplier; the SteamVR one runs it on the presenter and is
        # checked for the hand-over order as well as its own assertions.
        add_test(
            NAME xrfg_layer_triple_frame_gen_inline
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DTRIPLE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_triple_frame_gen.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/triple-frame-gen-inline"
                "-DMODE=d3d11-bridge"
                "-DREQUIRE_SCENARIO_PASS=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_triple_frame_gen.cmake")
        add_test(
            NAME xrfg_layer_triple_frame_gen_presenter
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DTRIPLE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_triple_frame_gen.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/triple-frame-gen-presenter"
                "-DMODE=steamvr-presenter"
                "-DEXPECT_PRESENTER=ON"
                "-DREQUIRE_SCENARIO_PASS=ON"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_triple_frame_gen.cmake")
        # The presenter scenarios hold under 3X as they stand: the call chain
        # reads the multiplier from the same ini and expects the virtual
        # period to follow it.
        add_test(
            NAME xrfg_layer_triple_swapchain-budget
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_rings_triple.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rings-triple-swapchain-budget"
                "-DMODE=swapchain-budget"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_triple_swapchain-budget PROPERTIES TIMEOUT 60)
        foreach(mode steamvr-presenter flight-simulator
                flight-destroy-swapchain steamvr-destroy-space)
            add_test(
                NAME xrfg_layer_triple_${mode}
                COMMAND "${CMAKE_COMMAND}"
                    "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                    "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                    "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_triple_frame_gen.ini"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/triple-frame-gen-${mode}"
                    "-DMODE=${mode}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
            set_tests_properties(xrfg_layer_triple_${mode} PROPERTIES TIMEOUT 60)
        endforeach()
        set_tests_properties(
            xrfg_layer_triple_frame_gen_inline
            xrfg_layer_triple_frame_gen_presenter
            PROPERTIES TIMEOUT 60)
        # 3X on the presenter expects every application frame to generate,
        # and with single-swapchain rings a pair reuses both synthetic
        # staging textures of the one before it. On WARP the synthesis runs
        # on the CPU, and a CPU-starved machine - a GPU-less build runner -
        # finishes it late enough that the next pair finds a destination busy
        # and fails open, as it should. A GPU never gets there, so these run
        # everywhere but are excluded by label where the hardware cannot
        # keep up (.github/workflows/build.yml).
        # The two pipelined (flight-simulator) scenarios expect the same and
        # have failed the same way on the runner, only less often.
        set_tests_properties(
            xrfg_layer_triple_frame_gen_presenter
            xrfg_layer_triple_steamvr-presenter
            xrfg_layer_triple_steamvr-destroy-space
            xrfg_layer_triple_flight-simulator
            xrfg_layer_triple_flight-destroy-swapchain
            PROPERTIES LABELS needs_gpu_timing)
        # And through the Vulkan interop, which publishes both synthetics.
        # A Vulkan session never takes a presenter, so this is the inline
        # path; the scenario's own counts are a pair's.
        add_test(
            NAME xrfg_layer_triple_frame_gen_vulkan
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DQUEUE_LAYER_DLL=$<TARGET_FILE:OFXR_vulkan_queue_layer>"
                "-DTRIPLE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_vulkan_triple.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/triple-frame-gen-vulkan"
                "-DMODE=vulkan"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_triple_frame_gen.cmake")
        set_tests_properties(xrfg_layer_triple_frame_gen_vulkan PROPERTIES
            TIMEOUT 60
            SKIP_REGULAR_EXPRESSION "SKIP: no Vulkan implementation on this machine")
        # The Vulkan scenarios need vulkan_bridge on, which their inis say
        # explicitly, and run with the queue layer in the chain, at both
        # depths. A machine without any Vulkan implementation (a GPU-less
        # build runner) cannot run them at all; the call chain says so with
        # one marker and CTest reports the scenario as skipped, not failed.
        # Any other Vulkan setup failure, including the queue layer being
        # refused at instance creation, is still a failure.
        foreach(depth deep shallow)
            add_test(
                NAME xrfg_layer_vulkan_${depth}
                COMMAND "${CMAKE_COMMAND}"
                    "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                    "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                    "-DQUEUE_LAYER_DLL=$<TARGET_FILE:OFXR_vulkan_queue_layer>"
                    "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_vulkan_${depth}.ini"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/vulkan-${depth}"
                    "-DMODE=vulkan"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
            set_tests_properties(xrfg_layer_vulkan_${depth} PROPERTIES
                TIMEOUT 60
                SKIP_REGULAR_EXPRESSION "SKIP: no Vulkan implementation on this machine")
        endforeach()
        # The Vulkan bridge: the same Vulkan application, a fake runtime that
        # lists D3D12 and takes the D3D12 session the layer hands it.
        add_test(
            NAME xrfg_layer_vulkan_bridge
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DQUEUE_LAYER_DLL=$<TARGET_FILE:OFXR_vulkan_queue_layer>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_vulkan_deep.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/vulkan-bridge"
                "-DMODE=vulkan-bridge"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_vulkan_bridge PROPERTIES
            TIMEOUT 60
            SKIP_REGULAR_EXPRESSION "SKIP: no Vulkan implementation on this machine")

        # The D3D11 bridge needs d3d11_bridge=1, which the shipped ini
        # leaves off: the application binds a real D3D11 device and the fake
        # runtime takes the D3D12 session the layer hands it.
        add_test(
            NAME xrfg_layer_d3d11_bridge
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_d3d11_bridge.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/d3d11-bridge"
                "-DMODE=d3d11-bridge"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_d3d11_bridge PROPERTIES TIMEOUT 60)

        # The same bridge with an application that keeps one image acquired
        # ahead and restarts the session with it outstanding: the runtime
        # image has to receive what was painted, every frame.
        add_test(
            NAME xrfg_layer_d3d11_bridge_acquire_ahead
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_d3d11_bridge.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/d3d11-bridge-acquire-ahead"
                "-DMODE=d3d11-bridge-acquire-ahead"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_d3d11_bridge_acquire_ahead PROPERTIES TIMEOUT 60)

        # NVIDIA optical flow cannot initialise on the fake runtime's WARP
        # device, so an ini asking for it exercises the layer's fallback to
        # FidelityFX: the default scenario's exact expectations only hold if
        # generation still runs.
        add_test(
            NAME xrfg_layer_nvidia_fallback
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DPIPELINE_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_nvidia_fallback.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/nvidia-fallback"
                "-DMODE=default"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_call_chain_depth.cmake")
        set_tests_properties(xrfg_layer_nvidia_fallback PROPERTIES TIMEOUT 60)

        add_test(
            NAME xrfg_layer_projection_eyes
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/projection-eyes"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_projection_eyes.cmake")

        add_test(
            NAME xrfg_layer_flight_logging
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/flight-logging-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_flight_logging.cmake")

        add_test(
            NAME xrfg_layer_promise_shown_time
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/promise-shown-time"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_promise_shown_time.cmake")

        add_test(
            NAME xrfg_layer_promise_two_groups
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DSLOW_FRAME_EVERY=4"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/promise-two-groups"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_promise_shown_time.cmake")

        add_test(
            NAME xrfg_layer_promise_shown_time_extrapolate
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DEXTRA_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_extrapolate.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/promise-shown-time-extrapolate"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_promise_shown_time.cmake")

        add_test(
            NAME xrfg_layer_rephase
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rephase"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_rephase.cmake")

        add_test(
            NAME xrfg_layer_cadence_fraction
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cadence-fraction"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_cadence_fraction.cmake")
        # Five seconds behind the call chains' shared lock would leave the
        # others queued past its 30 s wait, so it runs on its own.
        set_tests_properties(xrfg_layer_cadence_fraction PROPERTIES RUN_SERIAL TRUE)
        # Ten seconds each: queued together behind that lock, the third
        # waited past it.
        set_tests_properties(
            xrfg_layer_promise_shown_time
            xrfg_layer_promise_shown_time_extrapolate
            xrfg_layer_promise_two_groups
            xrfg_layer_rephase
            PROPERTIES RESOURCE_LOCK xrfg_layer_long_call_chain)

        add_test(
            NAME xrfg_layer_process_exclusion
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/process-exclusion"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_process_exclusion.cmake")

        add_test(
            NAME xrfg_layer_d3d11_interop_logging
            COMMAND "${CMAKE_COMMAND}"
                "-DLAYER_DLL=$<TARGET_FILE:XR_APILAYER_XRFrameBridge_diagnostic>"
                "-DCALL_CHAIN=$<TARGET_FILE:xrfg_layer_call_chain>"
                "-DENABLED_INI=${CMAKE_CURRENT_SOURCE_DIR}/tests/ofxr_bridge_logging_enabled.ini"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/d3d11-interop-logging-contract"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/layer_d3d11_interop_logging.cmake")

        add_executable(xrfg_d3d11_bridge_tests tests/d3d11_bridge_tests.cpp)
        target_include_directories(xrfg_d3d11_bridge_tests PRIVATE include)
        target_link_libraries(xrfg_d3d11_bridge_tests PRIVATE
            xrfg_d3d12_history
            d3d11
            d3d12
            dxgi)
        target_compile_features(xrfg_d3d11_bridge_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_d3d11_bridge_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_d3d11_bridge_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_d3d11_bridge_tests COMMAND xrfg_d3d11_bridge_tests)

        add_executable(xrfg_d3d12_history_tests tests/d3d12_history_tests.cpp)
        target_include_directories(xrfg_d3d12_history_tests PRIVATE include)
        target_link_libraries(xrfg_d3d12_history_tests PRIVATE
            xrfg_d3d12_history
            d3d12
            dxgi)
        target_compile_features(xrfg_d3d12_history_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_d3d12_history_tests PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_d3d12_history_tests PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_d3d12_history_tests COMMAND xrfg_d3d12_history_tests)
        if(XRFG_NATIVE_DLSSG)
            add_test(NAME xrfg_native_dlssg_tests COMMAND xrfg_d3d12_history_tests)
            set_tests_properties(xrfg_native_dlssg_tests PROPERTIES
                ENVIRONMENT "XRFG_TEST_NATIVE_DLSSG=1" LABELS "needs_gpu_timing" TIMEOUT 180)
        endif()

        add_executable(xrfg_vertical_fov_tests tests/vertical_fov_tests.cpp)
        target_include_directories(xrfg_vertical_fov_tests PRIVATE include)
        target_link_libraries(xrfg_vertical_fov_tests PRIVATE xrfg_d3d12_history d3d12 dxgi)
        target_compile_features(xrfg_vertical_fov_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_vertical_fov_tests PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_vertical_fov_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_vertical_fov_tests COMMAND xrfg_vertical_fov_tests)
        add_executable(xrfg_synthetic_marker_tests tests/synthetic_marker_tests.cpp)
        target_include_directories(xrfg_synthetic_marker_tests PRIVATE include)
        target_link_libraries(xrfg_synthetic_marker_tests PRIVATE xrfg_d3d12_history d3d12 dxgi)
        target_compile_features(xrfg_synthetic_marker_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_synthetic_marker_tests PRIVATE /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_synthetic_marker_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        endif()
        add_test(NAME xrfg_synthetic_marker_tests COMMAND xrfg_synthetic_marker_tests)

        add_executable(xrfg_nvidia_optical_flow_probe
            tests/nvidia_optical_flow_probe.cpp)
        target_include_directories(xrfg_nvidia_optical_flow_probe PRIVATE
            "${XRFG_NVIDIA_OPTICAL_FLOW_INCLUDE_DIR}")
        target_link_libraries(xrfg_nvidia_optical_flow_probe PRIVATE d3d12 dxgi)
        target_compile_features(xrfg_nvidia_optical_flow_probe PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(xrfg_nvidia_optical_flow_probe PRIVATE
                /W4 /permissive- /EHsc)
            target_compile_definitions(xrfg_nvidia_optical_flow_probe PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN)
        endif()
    endif()
endif()

# SteamVR compositor delivery probe. Deliberately outside the layer block: it
# needs no D3D12, no FidelityFX and no layer, only openvr.h, so it stays
# buildable on a checkout that cannot build the layer at all.
#
# external/openvr is not committed, for the same reason the FidelityFX SDK is
# not - see docs/BUILDING.md. Nothing from OpenVR is redistributed: the probe
# loads SteamVR's own openvr_api.dll at runtime.
#
# Not registered as a test. It needs a live SteamVR session with an application
# running, so it is run by hand, like xrfg_nvidia_optical_flow_probe.
if(WIN32 AND EXISTS "${XRFG_OPENVR_INCLUDE_DIR}/openvr.h")
    add_executable(xrfg_steamvr_delivery_probe tests/steamvr_delivery_probe.cpp)
    target_include_directories(xrfg_steamvr_delivery_probe PRIVATE
        "${XRFG_OPENVR_INCLUDE_DIR}")
    target_compile_features(xrfg_steamvr_delivery_probe PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(xrfg_steamvr_delivery_probe PRIVATE
            /W4 /permissive- /EHsc)
        target_compile_definitions(xrfg_steamvr_delivery_probe PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
    endif()
endif()
