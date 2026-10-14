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

# Measurements and optimizations; can be replaced while DCS runs.
add_library(DcsQvCullPayload SHARED src/main.cpp src/d3d_vtbl.c)
target_compile_options(DcsQvCullPayload PRIVATE ${DCSQV_OPTS})
target_compile_definitions(DcsQvCullPayload PRIVATE ${DCSQV_DEFS})

# Offline checks (frustum decoding, Scene.dll export lookup, timer cache, partition boost, kill switch, suite report).
enable_testing()
add_executable(DcsQvCullTests test/test_main.cpp src/d3d_vtbl.c)
target_compile_options(DcsQvCullTests PRIVATE /W4 /O2 /wd4201 /bigobj $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_compile_definitions(DcsQvCullTests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(DcsQvCullTests PRIVATE user32 kernel32)
# The profiler check looks for its own module name in the samples.
set_target_properties(DcsQvCullTests PROPERTIES OUTPUT_NAME test_main)
# The shared-memory layout check reads the module's copy at <exe folder>\..\src (the peer's test\ and src\ layout).
set_target_properties(DcsQvCullTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test)
configure_file(src/dcsvr_perception_shm.h ${CMAKE_CURRENT_BINARY_DIR}/src/dcsvr_perception_shm.h COPYONLY)
set(DCSQV_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-run)
file(MAKE_DIRECTORY ${DCSQV_TEST_DIR})
add_test(NAME DcsQvCullTests COMMAND DcsQvCullTests WORKING_DIRECTORY ${DCSQV_TEST_DIR})
set_tests_properties(DcsQvCullTests PROPERTIES PASS_REGULAR_EXPRESSION "\n0 failure\\(s\\)" TIMEOUT 900)  # about 4 min idle; much longer when another game loads the PC

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
