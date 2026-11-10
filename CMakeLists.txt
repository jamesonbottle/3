# HANDOFF [TODO] CMakeLists.txt - READ FIRST, this is the coordination point for splitting src/engine.h.
#
# Goal: turn the 1372-line header-only src/engine.h into normal C++20 translation units. NO behaviour change.
#  - Pure move: no logic edits, no renames, no reformatting beyond what the move needs. Real fixes go in a separate commit.
#  - Do NOT edit src/engine.h. Every "engine.h N-M" in a HANDOFF block means `git show 73ef2e6:src/engine.h` lines N-M.
#  - The old build (build.bat, tools/selftest.sh, main_*.cpp including engine.h) keeps working until step 5.
#  - Remaining work: grep -rl "HANDOFF \[TODO\]" .     When a file is finished, delete its block.
#  - Syntax-check without a build system:
#        header: echo '#include "X.h"' | g++ -std=c++20 -mavx2 -fsyntax-only -x c++ - -Isrc
#        source: g++ -std=c++20 -mavx2 -pthread -fsyntax-only -Isrc src/X.cpp
# Rules for every file
#  - `static` -> external linkage + declaration in the header. Per-line hot code stays inline in headers:
#    Order::at/pos, Gen::make, IO<>/dispatch, walk, mix64, Run::vtick. Run::parallel is a template, so it lives in run.h.
#  - Globals: g_salt, g_rng (seed.cpp); g_emit, g_console, g_tty (log.cpp). Declared `extern` in the matching header.
#  - Only platform_win.cpp and main_gui.cpp include <windows.h>.
#  - Names stay as they are (plat::, Run, Order, Gen, ...). No new namespaces.
# Order of work (steps depend on the ones before; files inside one step are independent)
#  1  types.h  platform.h  platform_win.cpp  platform_posix.cpp  log.h/.cpp  params.h/.cpp  seed.h/.cpp
#  2  order.h/.cpp  gen.h/.cpp  lineio.h
#  3  run.h/.cpp  algo.h/.cpp
#  4  algo_*.cpp (10 files)  suite.h/.cpp
#  5  this file, main_cli.cpp, main_gui.cpp, tools/unit_tests.cpp, tools/selftest.sh, build.bat, README.md (Build + Layout only); then delete src/engine.h
# Step 5 target: engine sources -> one static lib, built twice (normal + -DDDR5TEST_INJECT, see algo_fill.cpp);
#  exes ddr5ramtest (GUI, Windows only) and ddr5test_cli, plus unit_tests; C++20, AVX2 (-mavx2 / /arch:AVX2), pthread on POSIX.
# Done = tools/selftest.sh passes with today's exact counts (42 lines / 44 bits, ...) and the Windows CI job builds both exes.
cmake_minimum_required(VERSION 3.20)
project(ddr5ramtest CXX)
message(FATAL_ERROR "CMakeLists.txt is a handoff stub: implement step 5 per the HANDOFF block above.")
