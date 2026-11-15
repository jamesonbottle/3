# ddr5ramtest - DDR5 memory tester. C++20, AVX2.
#
#   ddr5_cpucheck       static library: src/cpu_check.cpp alone, built WITHOUT AVX2 (the run-time AVX2 check; _inject adds a test hook)
#   ddr5engine          static library: everything under src/ except the two front-ends and cpu_check.cpp
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
# MSVC only: treat warnings as errors (off by default; the Windows CI job turns it on through build.bat, /W4 is clean).
option(DDR5_WARNINGS_AS_ERRORS "Build with /WX on MSVC" OFF)
# Version text shown in the run header, `ddr5test_cli --version` and the exe's version resource. Empty = ask git.
set(DDR5_VERSION "" CACHE STRING "Version string (default: git describe --tags --always --dirty, or 'unknown')")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Version: generated header in the build directory (see src/build_version.h.in). Refreshed whenever CMake runs.
set(DDR5_BUILD_NUMBER 0)
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  if(NOT DDR5_VERSION)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE ddr5_git_describe
                    RESULT_VARIABLE ddr5_git_rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(ddr5_git_rc EQUAL 0 AND ddr5_git_describe)
      set(DDR5_VERSION "${ddr5_git_describe}")
    endif()
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE ddr5_git_count
                  RESULT_VARIABLE ddr5_git_rc2 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(ddr5_git_rc2 EQUAL 0 AND ddr5_git_count MATCHES "^[0-9]+$" AND ddr5_git_count LESS 65536)
    set(DDR5_BUILD_NUMBER "${ddr5_git_count}")
  endif()
endif()
if(NOT DDR5_VERSION)
  set(DDR5_VERSION "unknown")
endif()
set(DDR5_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(src/build_version.h.in "${DDR5_GENERATED_DIR}/build_version.h" @ONLY)
message(STATUS "ddr5ramtest version: ${DDR5_VERSION} (build ${DDR5_BUILD_NUMBER})")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# One place for the compiler flags every target shares (engine, front-ends, tests).
add_library(ddr5_flags INTERFACE)
if(DDR5_CHECK_SYNC)
  target_compile_definitions(ddr5_flags INTERFACE DDR5_CHECK_SYNC=1)
endif()
if(MSVC)
  # C4324 (structure padded because of alignas) is the intent of the cache-line aligned counters; C4996 flags fopen /
  # strerror / getenv, whose results this code checks (no _s variants needed).
  target_compile_options(ddr5_flags INTERFACE /O2 /arch:AVX2 /EHsc /utf-8 /W4 /wd4324)
  target_compile_definitions(ddr5_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
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
  src/flush.cpp
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

# The run-time AVX2 check (src/cpu_check.cpp) must run on a CPU that has no AVX2, so it is the one source that is built WITHOUT
# -mavx2 / /arch:AVX2 (the compiler may use AVX2 in any file that has it enabled, this check included): its own small library
# instead of ddr5_flags. Built twice like the engine; the _inject copy adds the DDR5TEST_FAKE_AVX2 test hook.
foreach(lib ddr5_cpucheck ddr5_cpucheck_inject)
  add_library(${lib} STATIC src/cpu_check.cpp)
  target_include_directories(${lib} PUBLIC src)
  if(MSVC)
    target_compile_options(${lib} PRIVATE /O2 /EHsc /utf-8 /W4 /wd4324)
    target_compile_definitions(${lib} PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(DDR5_WARNINGS_AS_ERRORS)
      target_compile_options(${lib} PRIVATE /WX)
    endif()
    target_compile_definitions(${lib} PRIVATE NDEBUG)
  else()
    target_compile_options(${lib} PRIVATE -O2 -Wall -Wextra)
    if(DDR5_SANITIZE)
      target_compile_options(${lib} PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=${DDR5_SANITIZE})
    endif()
  endif()
endforeach()
target_compile_definitions(ddr5_cpucheck_inject PRIVATE DDR5TEST_INJECT)

add_library(ddr5engine STATIC ${ENGINE_SOURCES})
target_include_directories(ddr5engine PUBLIC src "${DDR5_GENERATED_DIR}")
target_link_libraries(ddr5engine PUBLIC ddr5_flags ddr5_cpucheck)

add_library(ddr5engine_inject STATIC ${ENGINE_SOURCES} src/inject.cpp)
target_include_directories(ddr5engine_inject PUBLIC src "${DDR5_GENERATED_DIR}")
target_link_libraries(ddr5engine_inject PUBLIC ddr5_flags ddr5_cpucheck_inject)
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
  target_include_directories(ddr5ramtest PRIVATE "${DDR5_GENERATED_DIR}") # app.rc includes build_version.h
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
