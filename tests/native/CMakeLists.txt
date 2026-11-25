cmake_minimum_required(VERSION 3.20)
project(DcsVrNativeSmoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
include_directories("${WORKSPACE}/external/ofxr/external/OpenXR-SDK/include")
add_library(fixture_runtime SHARED fixture_runtime.cpp)
add_executable(loader_smoke loader_smoke.cpp)
target_link_libraries(loader_smoke PRIVATE "${WORKSPACE}/artifacts/native/openxr-loader/src/loader/openxr_loader.lib" dbghelp)
# Pass-through probe layers and a harness that records the real loader's layer order.
add_library(quad_probe SHARED probe_layer.cpp)
target_compile_definitions(quad_probe PRIVATE PROBE_NAME="quad")
add_library(gaze_probe SHARED probe_layer.cpp)
target_compile_definitions(gaze_probe PRIVATE PROBE_NAME="gaze")
# A layer that faults on a null-instance xrGetInstanceProcAddr, as Ultraleap's hand tracking does (GitHub #35).
add_library(nullfault_probe SHARED probe_layer.cpp)
target_compile_definitions(nullfault_probe PRIVATE PROBE_NAME="nullfault" PROBE_FAULT_NULL=1)
# Read-only runtime query used to check whether a runtime exposes eye gaze (for example SteamVR with Sboys 1.3).
add_executable(runtime_gaze_query runtime_gaze_query.cpp)
target_link_libraries(runtime_gaze_query PRIVATE "${WORKSPACE}/artifacts/native/openxr-loader/src/loader/openxr_loader.lib" advapi32)
# Live runtime probe: focus/peripheral FOVs that a native XR_VARJO_quad_views runtime returns (needs a headset).
add_executable(quad_fov_probe quad_fov_probe.cpp)
target_link_libraries(quad_fov_probe PRIVATE "${WORKSPACE}/artifacts/native/openxr-loader/src/loader/openxr_loader.lib" advapi32 d3d11 dxgi)
add_executable(gaze_order gaze_order.cpp)
target_link_libraries(gaze_order PRIVATE "${WORKSPACE}/artifacts/native/openxr-loader/src/loader/openxr_loader.lib" advapi32)
add_executable(quad_focus_policy quad_focus_policy.cpp)
target_compile_features(quad_focus_policy PRIVATE cxx_std_20)
target_include_directories(quad_focus_policy PRIVATE "${WORKSPACE}/external/cheeky/shared")

# Executes the production neural evaluator against user-supplied NVIDIA binaries.
# It creates an offscreen hardware device, never a window or headset session.
option(DCSVR_BUILD_GPU_SMOKE "Build hardware tests requiring the source-built OFXR libraries" OFF)
if(DCSVR_BUILD_GPU_SMOKE)
set(CHEEKY "${WORKSPACE}/external/cheeky")
add_executable(neural_gpu neural_gpu.cpp
    "${CHEEKY}/src/dlss_nr.cpp" "${CHEEKY}/src/dlss_nr_input.cpp"
    "${CHEEKY}/src/dlss_nr_lifetime.cpp" "${CHEEKY}/src/dlss_nr_contract.cpp"
    "${CHEEKY}/src/nr_runtime_module.cpp" "${CHEEKY}/src/graphics_observer.cpp"
    "${CHEEKY}/src/d3d12_output_contract.cpp" "${CHEEKY}/src/d3d12_ngx_dispatch.cpp"
    "${CHEEKY}/src/eye_calibration_d3d12.cpp" "${CHEEKY}/src/gaze_foveation.cpp" "${CHEEKY}/src/gaze_policy.cpp"
    "${CHEEKY}/src/settings.cpp" "${CHEEKY}/src/foveation.cpp"
    "${CHEEKY}/shared/gaze_math.cpp"
    "${CHEEKY}/third_party/reshade/deps/minhook/src/buffer.c"
    "${CHEEKY}/third_party/reshade/deps/minhook/src/hook.c"
    "${CHEEKY}/third_party/reshade/deps/minhook/src/trampoline.c"
    "${CHEEKY}/third_party/reshade/deps/minhook/src/hde/hde64.c")
enable_language(C)
target_compile_features(neural_gpu PRIVATE cxx_std_20)
target_compile_definitions(neural_gpu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(neural_gpu PRIVATE "${CHEEKY}/src" "${CHEEKY}/shared"
    "${CHEEKY}/third_party/reshade/deps/minhook/include" "${CHEEKY}/third_party/openvr/include")
target_link_libraries(neural_gpu PRIVATE d3d11 d3d12 d3dcompiler dxgi dxguid)
target_include_directories(neural_gpu PRIVATE "${WORKSPACE}/external/ofxr/include")
target_link_libraries(neural_gpu PRIVATE
    "${WORKSPACE}/artifacts/native/ofxr-full/xrfg_d3d12_history.lib"
    "${WORKSPACE}/artifacts/native/ofxr-full/xrfg_core.lib"
    "${WORKSPACE}/artifacts/native/ofxr-full/xrfg_bridge_flight_logger.lib"
    "${WORKSPACE}/external/ofxr/external/FidelityFX-SDK-v1.1.4/sdk/bin/ffx_sdk/ffx_opticalflow_x64.lib"
    "${WORKSPACE}/external/ofxr/external/FidelityFX-SDK-v1.1.4/sdk/bin/ffx_sdk/ffx_backend_dx12_x64.lib")
# VRAM of one DX11 DLSS SR feature as DCS creates it (deferred DX11 game feature).
add_executable(dlss11_vram_probe dlss11_vram_probe.cpp)
target_compile_features(dlss11_vram_probe PRIVATE cxx_std_20)
target_compile_definitions(dlss11_vram_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(dlss11_vram_probe PRIVATE d3d11 dxgi)
endif()
