cmake_minimum_required(VERSION 3.20)
project(DcsVrBinaural LANGUAGES CXX)

# Binaural (HRTF) audio for DCS: the interface headers (include/), BinauralCore and its offline harness (task A in
# docs/BINAURAL_AUDIO.md). The audio module in native/dcsqvcull and the pose publisher in native/pupil_shift include
# include/ from here (no copies).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_compile_options(/W4 /WX /permissive- /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Brepro)
add_link_options(/Brepro)

add_library(binaural_headers INTERFACE)
target_include_directories(binaural_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

# The contract: every interface header compiles together (with windows.h), layouts hold, pure helpers behave.
add_executable(binaural_contract_test test/contract_test.cpp)
target_link_libraries(binaural_contract_test PRIVATE binaural_headers)
add_test(NAME binaural_contract COMMAND binaural_contract_test)
set_tests_properties(binaural_contract PROPERTIES PASS_REGULAR_EXPRESSION " 0 failure\\(s\\)")
