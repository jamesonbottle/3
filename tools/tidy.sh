#!/usr/bin/env bash
# clang-tidy report over the engine and the console front-end (config: .clang-tidy). Report-only: always exits 0.
# Usage: tools/tidy.sh [extra clang-tidy args]
cd "$(dirname "$0")/.."
cmake -S . -B build/tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null || exit 2
files=$(ls src/*.cpp | grep -v -e main_gui -e platform_win)
# shellcheck disable=SC2086
clang-tidy -p build/tidy --quiet "$@" $files 2>/dev/null
exit 0
