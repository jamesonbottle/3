@echo off
REM Builds ddr5ramtest.exe (GUI) and ddr5test_cli.exe. Run from a "x64 Native Tools Command Prompt for VS".
REM Optional: python tools\gen_suite.py   (only after editing suite.cfg)
setlocal
if not exist build mkdir build
rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl /nologo /O2 /std:c++20 /arch:AVX2 /EHsc /utf-8 /DNDEBUG /Fo:build\ /Fe:build\ddr5ramtest.exe src\main_gui.cpp build\app.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO || exit /b 1
cl /nologo /O2 /std:c++20 /arch:AVX2 /EHsc /utf-8 /DNDEBUG /Fo:build\ /Fe:build\ddr5test_cli.exe src\main_cli.cpp || exit /b 1
echo.
echo Built build\ddr5ramtest.exe and build\ddr5test_cli.exe
