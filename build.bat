@echo off
REM Builds ddr5ramtest.exe (GUI) and ddr5test_cli.exe into build\ with CMake. Run from a "x64 Native Tools Command Prompt for VS".
REM Optional: python tools\gen_suite.py   (only after editing suite.cfg)
setlocal
if not exist build mkdir build
REM Ninja ships with Visual Studio and the GitHub runners; fall back to CMake's default generator if it is missing.
cmake -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release >nul 2>&1 || (rmdir /s /q build\cmake 2>nul & cmake -S . -B build\cmake -DCMAKE_BUILD_TYPE=Release) || exit /b 1
cmake --build build\cmake --config Release --target ddr5ramtest ddr5test_cli || exit /b 1
echo.
echo Built build\ddr5ramtest.exe and build\ddr5test_cli.exe
