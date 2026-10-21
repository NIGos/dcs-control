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
target_compile_options(DcsQvCullPayload PRIVATE ${DCSQV_OPTS} /bigobj)  # main.cpp passed the COFF section limit (C1128)
target_compile_definitions(DcsQvCullPayload PRIVATE ${DCSQV_DEFS})

# Offline checks (frustum decoding, Scene.dll export lookup, timer cache, partition boost, kill switch, suite report).
enable_testing()
add_executable(DcsQvCullTests test/test_main.cpp src/d3d_vtbl.c)
target_compile_options(DcsQvCullTests PRIVATE /W4 /O2 /wd4201 /bigobj $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_compile_definitions(DcsQvCullTests PRIVATE UNICODE _UNICODE NOMINMAX)
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
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/src)
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
