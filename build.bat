@echo off
REM Builds ddr5ramtest.exe (GUI), ddr5test_cli.exe and unit_tests.exe into build\ with CMake. Run from a "x64 Native Tools Command Prompt for VS".
REM Set DDR5_WARNINGS_AS_ERRORS=ON first to build with /WX (the GitHub Actions job does).
setlocal
if not exist build mkdir build
REM Ninja ships with Visual Studio and the GitHub runners; use CMake's default generator only if it is missing. A real
REM configure error is not swallowed: it stops the script with CMake's own message.
set "GEN="
where ninja >nul 2>&1 && set "GEN=-G Ninja"
set "WX="
if /i "%DDR5_WARNINGS_AS_ERRORS%"=="ON" set "WX=-DDDR5_WARNINGS_AS_ERRORS=ON"
cmake -S . -B build\cmake %GEN% -DCMAKE_BUILD_TYPE=Release %WX% || exit /b 1
cmake --build build\cmake --config Release --target ddr5ramtest ddr5test_cli unit_tests || exit /b 1
echo.
echo Built build\ddr5ramtest.exe, build\ddr5test_cli.exe and build\unit_tests.exe
