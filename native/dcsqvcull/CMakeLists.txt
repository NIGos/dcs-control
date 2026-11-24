cmake_minimum_required(VERSION 3.20)
project(DcsQvCull C CXX)

# DcsQvCull: CPU-side optimizations inside DCS (streaming timer cache, culling partition boost, model allocator
# slabs, texture streaming dedupe, effect constant-buffer skip, plain statistics counter), loaded by
# Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua. Built by scripts/build-dcsqvcull.ps1.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(DCSQV_OPTS /W4 /O2 /wd4201 $<$<COMPILE_LANGUAGE:CXX>:/EHsc /permissive->)
set(DCSQV_DEFS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Stable loader, loaded once by the Lua hook; hot-reloads the payload.
add_library(DcsQvCull SHARED src/loader.cpp)
target_compile_options(DcsQvCull PRIVATE ${DCSQV_OPTS})
target_compile_definitions(DcsQvCull PRIVATE ${DCSQV_DEFS})

# Binaural audio (docs/BINAURAL_AUDIO.md): the interface headers live in native/binaural/include (no copies).
# BinauralCore (the HRTF renderer, native/binaural/src) is built here from its sources as a static library and linked
# into the payload; native/binaural is not add_subdirectory'd, its own tests belong to scripts/build-binaural.ps1, not
# to this ctest. The default HRTF set is embedded as RCDATA HRTF_DEFAULT when native/binaural/data/sadie2_ku100.dcshrtf
# exists; without it the module stays off ("audio: off (no HRTF set: this build has no default HRTF set)").
set(DCSQV_BINAURAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../binaural)
set(DCSQV_BINAURAL_INCLUDE ${DCSQV_BINAURAL_DIR}/include)
set(DCSQV_HRTF_DEFAULT ${DCSQV_BINAURAL_DIR}/data/sadie2_ku100.dcshrtf)
add_library(DcsQvBinauralCore STATIC
  ${DCSQV_BINAURAL_DIR}/src/hrtf_set.cpp
  ${DCSQV_BINAURAL_DIR}/src/renderer.cpp
  ${DCSQV_BINAURAL_DIR}/src/kernels_sse.cpp
  ${DCSQV_BINAURAL_DIR}/src/kernels_avx2.cpp)
target_include_directories(DcsQvBinauralCore PUBLIC ${DCSQV_BINAURAL_INCLUDE})
target_compile_options(DcsQvBinauralCore PRIVATE /W4 /O2 /fp:precise /EHsc /permissive-)
target_compile_definitions(DcsQvBinauralCore PRIVATE ${DCSQV_DEFS})
# kernels_avx2.cpp is the only file built with /arch:AVX2; it runs only when the CPU reports AVX2 and FMA.
set_source_files_properties(${DCSQV_BINAURAL_DIR}/src/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")

# Measurements and optimizations; can be replaced while DCS runs.
set(DCSQV_PAYLOAD_SOURCES src/main.cpp src/d3d_vtbl.c)
if(EXISTS ${DCSQV_HRTF_DEFAULT})
  file(TO_CMAKE_PATH "${DCSQV_HRTF_DEFAULT}" DCSQV_HRTF_DEFAULT_RC)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/payload_hrtf.rc "HRTF_DEFAULT RCDATA \"${DCSQV_HRTF_DEFAULT_RC}\"\n")
  list(APPEND DCSQV_PAYLOAD_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/payload_hrtf.rc)
endif()
add_library(DcsQvCullPayload SHARED ${DCSQV_PAYLOAD_SOURCES})
target_compile_options(DcsQvCullPayload PRIVATE ${DCSQV_OPTS} /bigobj)  # main.cpp passed the COFF section limit (C1128)
target_compile_definitions(DcsQvCullPayload PRIVATE ${DCSQV_DEFS})
target_include_directories(DcsQvCullPayload PRIVATE ${DCSQV_BINAURAL_INCLUDE})
target_link_libraries(DcsQvCullPayload PRIVATE DcsQvBinauralCore)
target_compile_definitions(DcsQvCullPayload PRIVATE DCSQV_HAVE_BINAURAL_CORE=1)

# Offline checks (frustum decoding, Scene.dll export lookup, timer cache, partition boost, kill switch, suite report).
# The audio module's scenarios link a stand-in BinauralCore (test/audio_fake_core.cpp: a core whose output and refusals
# they control) and an allocation counter for the real-time threads (test/audio_rt_alloc.cpp); the end-to-end test
# below links the real one.
enable_testing()
add_executable(DcsQvCullTests test/test_main.cpp src/d3d_vtbl.c test/audio_fake_core.cpp test/audio_rt_alloc.cpp)
target_compile_options(DcsQvCullTests PRIVATE /W4 /O2 /wd4201 /bigobj $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_compile_definitions(DcsQvCullTests PRIVATE UNICODE _UNICODE NOMINMAX DCSQV_HAVE_BINAURAL_CORE=1)
target_include_directories(DcsQvCullTests PRIVATE ${DCSQV_BINAURAL_INCLUDE})
target_link_libraries(DcsQvCullTests PRIVATE user32 kernel32)
# The profiler check looks for its own module name in the samples.
set_target_properties(DcsQvCullTests PROPERTIES OUTPUT_NAME test_main)
# The Lua hook check (r63_lua_hook_test.h) runs ..\lua\DcsQvCull.lua with the stand-in module next to test_main.exe,
# as in the module's own tree: test_main goes to <build>	est, the hook to <build>\lua.
set_target_properties(DcsQvCullTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test)
add_dependencies(DcsQvCullTests DcsQvCullFakePayload)
add_custom_command(TARGET DcsQvCullTests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:DcsQvCullFakePayload> $<TARGET_FILE_DIR:DcsQvCullTests>/qv_fake_payload.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/lua/DcsQvCull.lua ${CMAKE_CURRENT_BINARY_DIR}/lua/DcsQvCull.lua
  # The log-phrase and ini-key checks (R107) read the sources at ..\src next to test_main, as in the module's tree.
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_CURRENT_BINARY_DIR}/src
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src
  # The invariant tests (invariants_test.h) load the app's own DcsQvCull.ini for every Engine option off and on.
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/test/app_ini ${CMAKE_CURRENT_BINARY_DIR}/app_ini)
set(DCSQV_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-run)
file(MAKE_DIRECTORY ${DCSQV_TEST_DIR})
add_test(NAME DcsQvCullTests COMMAND DcsQvCullTests WORKING_DIRECTORY ${DCSQV_TEST_DIR})
set_tests_properties(DcsQvCullTests PROPERTIES PASS_REGULAR_EXPRESSION "\n0 failure\\(s\\)" TIMEOUT 900)  # about 4 min idle; much longer when another game loads the PC

# Binaural audio end to end: the payload's audio module on the fake ed_sound image with the real BinauralCore and the
# core harness's synthetic spherical-head set (native/binaural/test): ITD / ILD trajectory of a circling source, the
# radio centred, no allocation on the audio threads.
add_executable(DcsQvCullAudioE2ETests test/audio_e2e_test.cpp src/d3d_vtbl.c test/audio_rt_alloc.cpp)
target_compile_options(DcsQvCullAudioE2ETests PRIVATE /W4 /O2 /wd4201 /bigobj $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_compile_definitions(DcsQvCullAudioE2ETests PRIVATE UNICODE _UNICODE NOMINMAX DCSQV_HAVE_BINAURAL_CORE=1)
target_include_directories(DcsQvCullAudioE2ETests PRIVATE ${DCSQV_BINAURAL_DIR}/test ${DCSQV_BINAURAL_DIR}/src)
target_link_libraries(DcsQvCullAudioE2ETests PRIVATE DcsQvBinauralCore user32 kernel32)
set_target_properties(DcsQvCullAudioE2ETests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test)
add_test(NAME DcsQvCullAudioE2ETests COMMAND DcsQvCullAudioE2ETests WORKING_DIRECTORY ${DCSQV_TEST_DIR})
set_tests_properties(DcsQvCullAudioE2ETests PROPERTIES PASS_REGULAR_EXPRESSION "\n0 failure\\(s\\)" TIMEOUT 300)

# The detour stub's code and pointer pages: a hot reload must never leave the stub without execute rights.
add_executable(DcsQvCullDetourTests test/detour_test.cpp)
target_compile_options(DcsQvCullDetourTests PRIVATE /O2 /EHa)
target_compile_definitions(DcsQvCullDetourTests PRIVATE NOMINMAX)
add_test(NAME DcsQvCullDetourTests COMMAND DcsQvCullDetourTests WORKING_DIRECTORY ${DCSQV_TEST_DIR})
set_tests_properties(DcsQvCullDetourTests PROPERTIES PASS_REGULAR_EXPRESSION "PASS new layout never faults" TIMEOUT 300)

# Signature scanner and relocation of the hook sites after a DCS patch ([General] SigScan), checked offline.
add_executable(DcsQvCullSigscanTests test/sigscan_test_main.cpp)
target_compile_options(DcsQvCullSigscanTests PRIVATE /W4 /O2 /EHsc)
target_compile_definitions(DcsQvCullSigscanTests PRIVATE UNICODE _UNICODE NOMINMAX)
add_test(NAME DcsQvCullSigscanTests COMMAND DcsQvCullSigscanTests WORKING_DIRECTORY ${DCSQV_TEST_DIR})
set_tests_properties(DcsQvCullSigscanTests PROPERTIES TIMEOUT 300)

# The loader with a stand-in payload next to it: payload swaps, DCS exit during a swap, the previous log kept.
set(DCSQV_LOADER_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/loader-test)
add_library(DcsQvCullFakePayload SHARED test/qv_fake_payload.cpp)
target_compile_options(DcsQvCullFakePayload PRIVATE /W4 /O2 /EHsc)
target_compile_definitions(DcsQvCullFakePayload PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(DcsQvCullFakePayload PROPERTIES OUTPUT_NAME qv_fake_payload RUNTIME_OUTPUT_DIRECTORY ${DCSQV_LOADER_TEST_DIR})
add_executable(DcsQvCullLoaderTests test/loader_test.cpp)
target_compile_options(DcsQvCullLoaderTests PRIVATE /W4 /O2 /EHsc)
target_compile_definitions(DcsQvCullLoaderTests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(DcsQvCullLoaderTests PRIVATE user32 kernel32)
set_target_properties(DcsQvCullLoaderTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DCSQV_LOADER_TEST_DIR})
add_dependencies(DcsQvCullLoaderTests DcsQvCullFakePayload)
add_test(NAME DcsQvCullLoaderTests COMMAND DcsQvCullLoaderTests WORKING_DIRECTORY ${DCSQV_LOADER_TEST_DIR})
set_tests_properties(DcsQvCullLoaderTests PROPERTIES PASS_REGULAR_EXPRESSION "(^|\n)0 failure\\(s\\)" TIMEOUT 300)

# Probe P1 of PER_EYE_PASS_DESIGN.md: a toy app for an NVIDIA GPU (single-pass stereo with a second position: the
# pixel shader's view index, a patched clone against two stock passes). Built, never run by ctest (it needs the GPU);
# run <build>/test/pep_p1_probe.exe by hand (QV_PEP_ARCHIVE=<copy of metashaders2> also offers the cache's clones
# to NvAPI).
add_executable(DcsQvCullPepP1Probe test/pep_p1_probe.cpp)
target_compile_options(DcsQvCullPepP1Probe PRIVATE /W4 /O2 /EHsc)
target_compile_definitions(DcsQvCullPepP1Probe PRIVATE NOMINMAX)
set_target_properties(DcsQvCullPepP1Probe PROPERTIES OUTPUT_NAME pep_p1_probe RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test)
