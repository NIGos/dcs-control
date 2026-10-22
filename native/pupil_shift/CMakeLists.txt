cmake_minimum_required(VERSION 3.20)
project(DcsVrPupilShift LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_compile_options(/W4 /WX /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Brepro)
add_link_options(/Brepro)
# The OpenXR headers come with the Cheeky checkout; -DOPENXR_INCLUDE=... points elsewhere (a separate worktree).
if(NOT OPENXR_INCLUDE)
    set(OPENXR_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../external/cheeky/third_party/openxr/include)
endif()
# OpenXR API layer, loaded by the OpenXR loader from the profile's explicit layer manifest.
add_library(pupil_shift SHARED pupil_shift_layer.cpp)
set_target_properties(pupil_shift PROPERTIES OUTPUT_NAME XR_APILAYER_DCSVR_pupil_shift)
target_include_directories(pupil_shift PRIVATE ${OPENXR_INCLUDE})
add_executable(pupil_shift_tests pupil_shift_tests.cpp)
# End-to-end harness: loads the built layer DLL through the loader negotiation with a fake runtime below it and drives
# it with synthetic gaze traces (pupil_shift_harness.cpp). Writes CSV and SVG traces to harness_out/.
add_executable(pupil_shift_harness pupil_shift_harness.cpp)
target_include_directories(pupil_shift_harness PRIVATE ${OPENXR_INCLUDE})
# A stand-in for the bundled Quad Views' gaze export (DcsVr_QuadViewsGaze), named like its DLL, in its own folder.
add_library(fake_quadviews SHARED fake_quadviews.cpp)
set_target_properties(fake_quadviews PROPERTIES OUTPUT_NAME XR_APILAYER_MBUCCHIA_quad_views_foveated
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_quadviews)
# A stand-in for Pimax's PVR client (LibPVRClient64.dll, getPvrInterface), in its own folder.
add_library(fake_pvr SHARED fake_pvr.cpp)
set_target_properties(fake_pvr PROPERTIES OUTPUT_NAME LibPVRClient64
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake_pvr)
add_dependencies(pupil_shift_harness pupil_shift fake_quadviews fake_pvr)
# The read-only DRS preset filter the layer installs for [DlssPreset] IgnoreDriverPreset (dcsvr_drs_preset_filter.h),
# tested against this PC's real NvAPI DRS (read-only). The probe DLL is named like DLSS ("nvngx_dlss*") and sits next
# to the test, which loads it by name. Without an NVIDIA driver the test exits 2: ctest reports it as skipped.
add_executable(drs_filter_test drs_filter_test.cpp)
add_library(drs_filter_probe SHARED drs_filter_probe.cpp)
set_target_properties(drs_filter_probe PROPERTIES OUTPUT_NAME nvngx_dlss_r122b_probe)
add_dependencies(drs_filter_test drs_filter_probe)
enable_testing()
add_test(NAME pupil_shift COMMAND pupil_shift_tests)
add_test(NAME pupil_shift_harness COMMAND pupil_shift_harness $<TARGET_FILE:pupil_shift> ${CMAKE_CURRENT_BINARY_DIR}/harness_out
    $<TARGET_FILE:fake_quadviews> $<TARGET_FILE:fake_pvr>)
add_test(NAME drs_filter COMMAND drs_filter_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(drs_filter PROPERTIES SKIP_RETURN_CODE 2)
