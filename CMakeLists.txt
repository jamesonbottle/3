# ddr5ramtest - DDR5 memory tester. C++20, AVX2.
#
#   ddr5engine          static library: everything under src/ except the two front-ends
#   ddr5engine_inject   the same sources built with -DDDR5TEST_INJECT (fault injection for tools/selftest.sh; the algo_*.cpp call DDR5_INJECT hooks, src/inject.cpp is compiled only here)
#   ddr5test_cli        console front-end (all platforms)
#   ddr5test_cli_inject console front-end linked against the fault-injection engine (self-test only)
#   unit_tests          deterministic unit tests of the engine's pure logic (tools/unit_tests.cpp)
#   ddr5ramtest         Win32 GUI front-end (Windows only)
#
# Executables land in DDR5_OUTPUT_DIR (default <source>/build/) whatever the generator or configuration, which is where build.bat and CI expect them.
cmake_minimum_required(VERSION 3.20)
project(ddr5ramtest CXX)

set(DDR5_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build" CACHE PATH "Where the executables are placed")
# Sanitizers for every target (GCC / Clang): thread, or address,undefined. Set by `SAN=... tools/selftest.sh`; empty = normal build.
set(DDR5_SANITIZE "" CACHE STRING "Value for -fsanitize= (thread | address,undefined); empty for none")
# Debug check of the barrier protocol (every worker must make the same number of Run::sync() calls; see run.h). Always on in
# builds without NDEBUG; tools/selftest.sh turns it on in every leg, release flags included. Off by default.
option(DDR5_CHECK_SYNC "Check that all workers make the same number of sync() calls" OFF)
# MSVC only: treat warnings as errors (off by default; CI turns it on once /W4 is clean).
option(DDR5_WARNINGS_AS_ERRORS "Build with /WX on MSVC" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# One place for the compiler flags every target shares (engine, front-ends, tests).
add_library(ddr5_flags INTERFACE)
if(DDR5_CHECK_SYNC)
  target_compile_definitions(ddr5_flags INTERFACE DDR5_CHECK_SYNC=1)
endif()
if(MSVC)
  target_compile_options(ddr5_flags INTERFACE /O2 /arch:AVX2 /EHsc /utf-8 /W4)
  if(DDR5_WARNINGS_AS_ERRORS)
    target_compile_options(ddr5_flags INTERFACE /WX)
  endif()
  target_compile_definitions(ddr5_flags INTERFACE NDEBUG)
else()
  if(DDR5_SANITIZE)
    # -O1 -g keeps the reports readable and the run fast enough; undefined behaviour must fail the run, not just print.
    target_compile_options(ddr5_flags INTERFACE -O1 -g -fno-omit-frame-pointer -fsanitize=${DDR5_SANITIZE} -mavx2 -Wall -Wextra)
    target_link_options(ddr5_flags INTERFACE -fsanitize=${DDR5_SANITIZE})
    if(DDR5_SANITIZE MATCHES "undefined")
      target_compile_options(ddr5_flags INTERFACE -fno-sanitize-recover=undefined)
      target_link_options(ddr5_flags INTERFACE -fno-sanitize-recover=undefined)
    endif()
  else()
    target_compile_options(ddr5_flags INTERFACE -O2 -mavx2 -Wall -Wextra)
  endif()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(ddr5_flags INTERFACE Threads::Threads)
endif()

set(ENGINE_SOURCES
  src/algo.cpp
  src/algo_chase.cpp
  src/algo_coherency.cpp
  src/algo_copy.cpp
  src/algo_evolve.cpp
  src/algo_fill.cpp
  src/algo_hammer.cpp
  src/algo_march.cpp
  src/algo_pagemix.cpp
  src/algo_probe.cpp
  src/algo_rwstorm.cpp
  src/gen.cpp
  src/log.cpp
  src/order.cpp
  src/params.cpp
  src/run.cpp
  src/seed.cpp
  src/suite.cpp
  src/suite_data.cpp)
if(WIN32)
  list(APPEND ENGINE_SOURCES src/platform_win.cpp)
else()
  list(APPEND ENGINE_SOURCES src/platform_posix.cpp)
endif()

add_library(ddr5engine STATIC ${ENGINE_SOURCES})
target_include_directories(ddr5engine PUBLIC src)
target_link_libraries(ddr5engine PUBLIC ddr5_flags)

add_library(ddr5engine_inject STATIC ${ENGINE_SOURCES} src/inject.cpp)
target_include_directories(ddr5engine_inject PUBLIC src)
target_link_libraries(ddr5engine_inject PUBLIC ddr5_flags)
target_compile_definitions(ddr5engine_inject PRIVATE DDR5TEST_INJECT)

add_executable(ddr5test_cli src/main_cli.cpp)
target_link_libraries(ddr5test_cli PRIVATE ddr5engine)

add_executable(ddr5test_cli_inject src/main_cli.cpp)
target_link_libraries(ddr5test_cli_inject PRIVATE ddr5engine_inject)

add_executable(unit_tests tools/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE ddr5engine)

set(EXES ddr5test_cli ddr5test_cli_inject unit_tests)

if(WIN32)
  enable_language(RC)
  add_executable(ddr5ramtest WIN32 src/main_gui.cpp src/app.rc)
  target_link_libraries(ddr5ramtest PRIVATE ddr5engine)
  if(MSVC)
    target_link_options(ddr5ramtest PRIVATE /SUBSYSTEM:WINDOWS /MANIFEST:NO)   # the manifest is embedded by app.rc
    target_link_libraries(ddr5ramtest PRIVATE comctl32 user32 gdi32 advapi32)
  else()
    target_link_options(ddr5ramtest PRIVATE -mwindows -static)
    target_link_libraries(ddr5ramtest PRIVATE comctl32 gdi32 user32 advapi32)
    target_link_options(ddr5test_cli PRIVATE -static)
  endif()
  list(APPEND EXES ddr5ramtest)
endif()

# Tests: `ctest --output-on-failure` (from the build directory). tools/selftest.sh stays the full gate; it builds its own
# copy of the engine (normal + fault injection) in a temporary directory, so it needs bash and g++ (Linux).
enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
if(NOT WIN32)
  add_test(NAME selftest COMMAND bash "${CMAKE_SOURCE_DIR}/tools/selftest.sh")
  set_tests_properties(selftest PROPERTIES TIMEOUT 1800 LABELS "slow")
endif()

foreach(t IN LISTS EXES)
  set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DDR5_OUTPUT_DIR}")
  foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg} "${DDR5_OUTPUT_DIR}")
  endforeach()
endforeach()
